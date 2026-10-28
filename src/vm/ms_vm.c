#include "vm/ms_vm.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "object/ms_operator.h"
#include "object/ms_string.h"
#include "vm/ms_state_internal.h"

static MsResult msVmRuntimeError(MsState* L, const char* message) {
  msRaiseRuntimeError(L, "%s", message);
  return msStateErrorResult(L) == MS_ERROR_OOM ? MS_ERROR_OOM : MS_ERROR_RUNTIME;
}

static MsResult msVmOom(MsState* L) {
  msRaiseRuntimeError(L, "out of memory");
  L->errorResult = MS_ERROR_OOM;
  return MS_ERROR_OOM;
}

static MsResult msVmErrorAt(MsState* L, const struct MsCallFrame* frame, const char* message) {
  const char* sourceFile = "<unknown>";
  if (frame->proto != NULL && frame->proto->sourceFile != NULL) {
    sourceFile = msStringData(frame->proto->sourceFile);
  }
  uint32_t line = 0;
  if (frame->proto != NULL) {
    line = msProtoLineAt(frame->proto, frame->pc - 1);
  }
  msRaiseRuntimeError(L, "%s:%u: %s", sourceFile, (unsigned)line, message);
  return msStateErrorResult(L) == MS_ERROR_OOM ? MS_ERROR_OOM : MS_ERROR_RUNTIME;
}

static MsResult msVmPropagatedErrorAt(MsState* L, const struct MsCallFrame* frame) {
  if (msStateErrorResult(L) == MS_ERROR_OOM) {
    return MS_ERROR_OOM;
  }
  const char* message = msStateErrorMessage(L);
  return msVmErrorAt(L, frame, message != NULL ? message : "operation failed");
}

static struct MsObject* msVmConstAt(MsState* L, const struct MsCallFrame* frame,
    int index, MsResult* result) {
  if (index < 0 || index >= frame->proto->constsLen || frame->proto->consts == NULL
      || frame->proto->consts[index] == NULL) {
    *result = msVmErrorAt(L, frame, "constant index out of bounds");
    return NULL;
  }
  return frame->proto->consts[index];
}

static MsResult msVmUndefinedGlobalAt(MsState* L, const struct MsCallFrame* frame,
    const struct MsString* name) {
  const char* sourceFile = "<unknown>";
  if (frame->proto != NULL && frame->proto->sourceFile != NULL) {
    sourceFile = msStringData(frame->proto->sourceFile);
  }
  uint32_t line = 0;
  if (frame->proto != NULL) {
    line = msProtoLineAt(frame->proto, frame->pc - 1);
  }
  msRaiseRuntimeError(L, "%s:%u: undefined name '%s'", sourceFile, (unsigned)line, msStringData(name));
  return msStateErrorResult(L) == MS_ERROR_OOM ? MS_ERROR_OOM : MS_ERROR_RUNTIME;
}

static void msVmJumpBy(struct MsCallFrame* frame, int32_t offset) {
  int64_t target = (int64_t)frame->pc + offset;
  frame->pc = target < 0 ? -1 : target > INT_MAX ? INT_MAX : (int)target;
}

static MsResult msVmEnsureStack(MsState* L, struct MsCoroutine* co, int needed) {
  if (needed < 0 || needed > MS_VM_STACK_MAX_CAP) {
    return msVmRuntimeError(L, "stack overflow");
  }
  if (needed <= co->stackCap) {
    return MS_OK;
  }

  int newCap = co->stackCap == 0 ? MS_VM_STACK_INIT_CAP : co->stackCap;
  while (newCap < needed) {
    if (newCap > MS_VM_STACK_MAX_CAP / 2) {
      newCap = MS_VM_STACK_MAX_CAP;
    } else {
      newCap *= 2;
    }
  }
  if ((size_t)newCap > SIZE_MAX / sizeof(*co->stack)) {
    return msVmOom(L);
  }
  struct MsObject** stack = msRealloc(co->stack, (size_t)newCap * sizeof(*stack));
  if (stack == NULL) {
    return msVmOom(L);
  }
  co->stack = stack;
  co->stackCap = newCap;
  return MS_OK;
}

static MsResult msVmEnsureFrames(MsState* L, struct MsCoroutine* co, int needed) {
  if (needed < 0 || needed > MS_VM_FRAMES_MAX) {
    return msVmRuntimeError(L, "call stack overflow");
  }
  if (needed <= co->framesCap) {
    return MS_OK;
  }

  int newCap = co->framesCap == 0 ? MS_VM_FRAMES_INIT_CAP : co->framesCap;
  while (newCap < needed) {
    if (newCap > MS_VM_FRAMES_MAX / 2) {
      newCap = MS_VM_FRAMES_MAX;
    } else {
      newCap *= 2;
    }
  }
  if ((size_t)newCap > SIZE_MAX / sizeof(*co->frames)) {
    return msVmOom(L);
  }
  struct MsCallFrame* frames = msRealloc(co->frames, (size_t)newCap * sizeof(*frames));
  if (frames == NULL) {
    return msVmOom(L);
  }
  co->frames = frames;
  co->framesCap = newCap;
  return MS_OK;
}

static MsResult msVmPushFrameAt(MsState* L, struct MsCoroutine* co, struct MsProto* proto,
    int argc, struct MsObject** argv, bool argsInPlace, int base, struct MsUpvalue** upvalues) {
  if (proto == NULL || argc < 0 || (argc > 0 && argv == NULL)) {
    return msVmRuntimeError(L, "invalid frame arguments");
  }
  if (proto->paramCount < 0 || argc != proto->paramCount) {
    return msVmRuntimeError(L, "argument count mismatch");
  }
  if (proto->hasVarArgs || proto->hasKwArgs) {
    return msVmRuntimeError(L, "variadic calls not supported");
  }
  if (proto->localCount < proto->paramCount || proto->stackSize < 0 || base < 1) {
    return msVmRuntimeError(L, "invalid frame layout");
  }

  int64_t windowEnd = (int64_t)base + proto->localCount + proto->stackSize;
  if (windowEnd > MS_VM_STACK_MAX_CAP || windowEnd > INT_MAX) {
    return msVmRuntimeError(L, "stack overflow");
  }
  if (co->frameCount < 0 || co->frameCount >= MS_VM_FRAMES_MAX) {
    return msVmRuntimeError(L, "call stack overflow");
  }
  int64_t argsEnd = (int64_t)base + argc;
  if (argsInPlace && co->stackTop < argsEnd) {
    return msVmRuntimeError(L, "invalid in-place arguments");
  }

  int argvIndex = -1;
  if (!argsInPlace && argc > 0 && co->stack != NULL) {
    uintptr_t stackAddress = (uintptr_t)(void*)co->stack;
    uintptr_t argvAddress = (uintptr_t)(void*)argv;
    size_t stackBytes = (size_t)co->stackTop * sizeof(*co->stack);
    if (argvAddress >= stackAddress && argvAddress - stackAddress < stackBytes) {
      size_t byteOffset = argvAddress - stackAddress;
      size_t index = byteOffset / sizeof(*co->stack);
      if (byteOffset % sizeof(*co->stack) == 0 && (int64_t)index + argc <= co->stackTop) {
        argvIndex = (int)index;
      }
    }
  }

  MsResult result = msVmEnsureStack(L, co, (int)windowEnd);
  if (result != MS_OK) {
    return result;
  }
  result = msVmEnsureFrames(L, co, co->frameCount + 1);
  if (result != MS_OK) {
    return result;
  }

  if (!argsInPlace) {
    co->stack[base - 1] = msNewNil(L);
    if (argc > 0) {
      if (argvIndex >= 0) {
        argv = co->stack + argvIndex;
      }
      memcpy(co->stack + base, argv, (size_t)argc * sizeof(*argv));
    }
  }
  for (int i = argc; i < proto->localCount; ++i) {
    co->stack[base + i] = msNewNil(L);
  }
  co->stackTop = base + proto->localCount;

  struct MsCallFrame* frame = &co->frames[co->frameCount++];
  frame->proto = proto;
  frame->pc = 0;
  frame->baseIndex = base;
  frame->upvalues = upvalues;
  frame->upvalueCount = 0;
  return MS_OK;
}

void msCoroutineInit(struct MsCoroutine* co) {
  memset(co, 0, sizeof(*co));
}

void msCoroutineDestroy(struct MsCoroutine* co) {
  msFree(co->frames);
  msFree(co->stack);
  memset(co, 0, sizeof(*co));
}

MsResult msVmPushFrame(MsState* L, struct MsCoroutine* co, struct MsProto* proto,
    int argc, struct MsObject** argv) {
  if (co->stackTop < 0 || co->stackTop == INT_MAX) {
    return msVmRuntimeError(L, "stack overflow");
  }
  return msVmPushFrameAt(L, co, proto, argc, argv, false, co->stackTop + 1, NULL);
}

static bool msVmCanPush(const struct MsCoroutine* co, const struct MsCallFrame* frame, int windowEnd) {
  return co->stackTop >= frame->baseIndex + frame->proto->localCount && co->stackTop < windowEnd;
}

static bool msVmCanPop(const struct MsCoroutine* co, const struct MsCallFrame* frame) {
  return co->stackTop > frame->baseIndex + frame->proto->localCount;
}

MsResult msVmRun(MsState* L, struct MsCoroutine* co, int stopAt) {
  int entryFrames = co->frameCount;
  int entryTop = co->stackTop;
  if (stopAt < 0 || stopAt > entryFrames) {
    return msVmRuntimeError(L, "invalid stop depth");
  }
  if (stopAt == entryFrames) {
    return MS_OK;
  }

  while (co->frameCount > stopAt) {
    struct MsCallFrame* frame = &co->frames[co->frameCount - 1];
    if (frame->proto == NULL || frame->proto->code == NULL
        || frame->pc < 0 || frame->pc >= frame->proto->codeLen) {
      MsResult result = msVmErrorAt(L, frame, "instruction pointer out of bounds");
      co->frameCount = stopAt;
      co->stackTop = entryTop;
      return result;
    }

    uint32_t instr = frame->proto->code[frame->pc++];
    int windowEnd = frame->baseIndex + frame->proto->localCount + frame->proto->stackSize;
    MsResult result = MS_OK;
    switch ((int)msOpDecodeOp(instr)) {
      case MS_OP_NOP:
        break;
      case MS_OP_LOAD_CONST: {
        struct MsObject* constant = msVmConstAt(L, frame, msOpDecodeBx(instr), &result);
        if (constant == NULL) {
          break;
        }
        if (!msVmCanPush(co, frame, windowEnd)) {
          result = msVmErrorAt(L, frame, "evaluation stack overflow");
          break;
        }
        co->stack[co->stackTop++] = constant;
        break;
      }
      case MS_OP_LOAD_NIL:
      case MS_OP_LOAD_TRUE:
      case MS_OP_LOAD_FALSE:
        if (!msVmCanPush(co, frame, windowEnd)) {
          result = msVmErrorAt(L, frame, "evaluation stack overflow");
          break;
        }
        if (msOpDecodeOp(instr) == MS_OP_LOAD_NIL) {
          co->stack[co->stackTop++] = msNewNil(L);
        } else {
          co->stack[co->stackTop++] = msNewBool(L, msOpDecodeOp(instr) == MS_OP_LOAD_TRUE);
        }
        break;
      case MS_OP_MOVE: {
        int destination = msOpDecodeA(instr);
        int source = msOpDecodeBx(instr);
        if (destination >= frame->proto->localCount || source >= frame->proto->localCount) {
          result = msVmErrorAt(L, frame, "local index out of bounds");
          break;
        }
        co->stack[frame->baseIndex + destination] = co->stack[frame->baseIndex + source];
        break;
      }
      case MS_OP_LOAD_LOCAL: {
        int slot = msOpDecodeA(instr);
        if (slot >= frame->proto->localCount) {
          result = msVmErrorAt(L, frame, "local index out of bounds");
          break;
        }
        if (!msVmCanPush(co, frame, windowEnd)) {
          result = msVmErrorAt(L, frame, "evaluation stack overflow");
          break;
        }
        co->stack[co->stackTop++] = co->stack[frame->baseIndex + slot];
        break;
      }
      case MS_OP_STORE_LOCAL: {
        int slot = msOpDecodeA(instr);
        if (slot >= frame->proto->localCount) {
          result = msVmErrorAt(L, frame, "local index out of bounds");
          break;
        }
        if (!msVmCanPop(co, frame)) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        co->stack[frame->baseIndex + slot] = co->stack[--co->stackTop];
        break;
      }
      case MS_OP_LOAD_GLOBAL: {
        struct MsObject* nameObject = msVmConstAt(L, frame, msOpDecodeBx(instr), &result);
        if (nameObject == NULL) {
          break;
        }
        if (msObjectTag(nameObject) != MS_TYPE_STRING) {
          result = msVmErrorAt(L, frame, "invalid global name");
          break;
        }
        if (!msVmCanPush(co, frame, windowEnd)) {
          result = msVmErrorAt(L, frame, "evaluation stack overflow");
          break;
        }
        const struct MsString* name = (const struct MsString*)nameObject;
        struct MsObject* value = msStateGetGlobal(L, name);
        if (value == NULL) {
          result = msVmUndefinedGlobalAt(L, frame, name);
          break;
        }
        co->stack[co->stackTop++] = value;
        break;
      }
      case MS_OP_STORE_GLOBAL: {
        struct MsObject* nameObject = msVmConstAt(L, frame, msOpDecodeBx(instr), &result);
        if (nameObject == NULL) {
          break;
        }
        if (msObjectTag(nameObject) != MS_TYPE_STRING) {
          result = msVmErrorAt(L, frame, "invalid global name");
          break;
        }
        if (!msVmCanPop(co, frame)) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        struct MsObject* value = co->stack[--co->stackTop];
        result = msStateSetGlobal(L, (const struct MsString*)nameObject, value);
        break;
      }
      case MS_OP_ADD:
      case MS_OP_LT: {
        if (co->stackTop < frame->baseIndex + frame->proto->localCount + 2) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        struct MsObject* rhs = co->stack[--co->stackTop];
        struct MsObject* lhs = co->stack[--co->stackTop];
        MsOpCode op = msOpDecodeOp(instr);
        struct MsObject* value = NULL;
        bool fastPath = false;
        if (msObjectTag(lhs) == MS_TYPE_INT && msObjectTag(rhs) == MS_TYPE_INT
            && msIntIsWord(lhs) && msIntIsWord(rhs)) {
          int64_t lhsValue = msIntWordValue(lhs);
          int64_t rhsValue = msIntWordValue(rhs);
          if (op == MS_OP_LT) {
            value = msNewBool(L, lhsValue < rhsValue);
            fastPath = true;
          } else {
            bool overflows = (rhsValue > 0 && lhsValue > INT64_MAX - rhsValue)
                || (rhsValue < 0 && lhsValue < INT64_MIN - rhsValue);
            if (!overflows) {
              value = msNewInt(L, lhsValue + rhsValue);
              fastPath = true;
            }
          }
        }
        if (!fastPath) {
          value = msObjectBinaryOp(L, op, lhs, rhs);
        }
        if (value == NULL) {
          result = msVmPropagatedErrorAt(L, frame);
          break;
        }
        co->stack[co->stackTop++] = value;
        break;
      }
      case MS_OP_JUMP: {
        int32_t offset = msOpDecodeSAx(instr);
        if (offset < 0) {
          // safepoint: scheduler preemption and GC handshake.
        }
        msVmJumpBy(frame, offset);
        break;
      }
      case MS_OP_JUMP_IF_FALSE:
      case MS_OP_JUMP_IF_TRUE:
      case MS_OP_JUMP_IF_NIL: {
        if (!msVmCanPop(co, frame)) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        struct MsObject* condition = co->stack[--co->stackTop];
        bool truthy = msObjectIsTruthy(condition);
        MsOpCode op = msOpDecodeOp(instr);
        bool shouldJump = op == MS_OP_JUMP_IF_NIL
            ? msObjectTag(condition) == MS_TYPE_NIL
            : truthy == (op == MS_OP_JUMP_IF_TRUE);
        if (shouldJump) {
          int32_t offset = msOpDecodeSAx(instr);
          if (offset < 0) {
            // safepoint: scheduler preemption and GC handshake.
          }
          msVmJumpBy(frame, offset);
        }
        break;
      }
      case MS_OP_RETURN: {
        if (!msVmCanPop(co, frame)) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        struct MsObject* returnValue = co->stack[--co->stackTop];
        int returnSlot = frame->baseIndex - 1;
        co->stackTop = returnSlot;
        --co->frameCount;
        co->stack[co->stackTop++] = returnValue;
        if (co->frameCount == stopAt) {
          return MS_OK;
        }
        break;
      }
      default:
        MS_ASSERT(!"opcode not implemented");
        result = msVmErrorAt(L, frame, "opcode not implemented");
        break;
    }
    if (result != MS_OK) {
      co->frameCount = stopAt;
      co->stackTop = entryTop;
      return result;
    }
  }
  return MS_OK;
}

MsResult msCallProto(MsState* L, struct MsCoroutine* co, struct MsProto* proto,
    int argc, struct MsObject** argv, struct MsObject** out) {
  if (out == NULL) {
    return msVmRuntimeError(L, "null return slot");
  }
  *out = NULL;
  int entryFrames = co->frameCount;
  int entryTop = co->stackTop;
  MsResult result = msVmPushFrame(L, co, proto, argc, argv);
  if (result != MS_OK) {
    co->frameCount = entryFrames;
    co->stackTop = entryTop;
    return result;
  }

  result = msVmRun(L, co, entryFrames);
  if (result != MS_OK) {
    co->frameCount = entryFrames;
    co->stackTop = entryTop;
    return result;
  }
  MS_ASSERT(co->stackTop == entryTop + 1);
  *out = co->stack[entryTop];
  return MS_OK;
}
