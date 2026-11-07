#include "vm/ms_vm.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "object/ms_dict.h"
#include "object/ms_list.h"
#include "object/ms_operator.h"
#include "object/ms_string.h"
#include "vm/ms_function.h"
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

static bool msVmMessageHasLocation(const char* message) {
  const char* locationEnd = strstr(message, ": ");
  if (locationEnd == NULL) {
    return false;
  }
  const char* lineStart = locationEnd;
  while (lineStart > message && lineStart[-1] >= '0' && lineStart[-1] <= '9') {
    --lineStart;
  }
  return lineStart < locationEnd && lineStart > message + 1 && lineStart[-1] == ':';
}

static MsResult msVmWrapErrorAt(MsState* L, const struct MsCallFrame* frame) {
  if (msStateErrorResult(L) == MS_ERROR_OOM) {
    return MS_ERROR_OOM;
  }
  const char* message = msStateErrorMessage(L);
  if (message == NULL) {
    message = "operation failed";
  }
  if (msVmMessageHasLocation(message)) {
    return MS_ERROR_RUNTIME;
  }
  return msVmErrorAt(L, frame, message);
}

static MsResult msVmPropagatedErrorAt(MsState* L, const struct MsCallFrame* frame) {
  return msVmWrapErrorAt(L, frame);
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
    int argc, struct MsObject** argv, bool argsInPlace, int base, struct MsUpvalue** upvalues,
    int upvalueCount) {
  if (proto == NULL || argc < 0 || (argc > 0 && argv == NULL && !argsInPlace)) {
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
  frame->upvalueCount = upvalueCount;
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
  return msVmPushFrameAt(L, co, proto, argc, argv, false, co->stackTop + 1, NULL, 0);
}

static bool msVmCanPush(const struct MsCoroutine* co, const struct MsCallFrame* frame, int windowEnd) {
  return co->stackTop >= frame->baseIndex + frame->proto->localCount && co->stackTop < windowEnd;
}

static bool msVmCanPop(const struct MsCoroutine* co, const struct MsCallFrame* frame) {
  return co->stackTop > frame->baseIndex + frame->proto->localCount;
}

static bool msVmCheckedMul(int64_t lhs, int64_t rhs, int64_t* out) {
  if (lhs == 0 || rhs == 0) {
    *out = 0;
    return true;
  }
  if (lhs == -1) {
    if (rhs == INT64_MIN) {
      return false;
    }
    *out = -rhs;
    return true;
  }
  if (rhs == -1) {
    if (lhs == INT64_MIN) {
      return false;
    }
    *out = -lhs;
    return true;
  }
  if ((lhs > 0 && rhs > 0 && lhs > INT64_MAX / rhs)
      || (lhs > 0 && rhs < 0 && rhs < INT64_MIN / lhs)
      || (lhs < 0 && rhs > 0 && lhs < INT64_MIN / rhs)
      || (lhs < 0 && rhs < 0 && lhs < INT64_MAX / rhs)) {
    return false;
  }
  *out = lhs * rhs;
  return true;
}

static bool msVmIsNumeric(const struct MsObject* value) {
  return msObjectTag(value) == MS_TYPE_FLOAT
      || (msObjectTag(value) == MS_TYPE_INT && msIntIsWord(value));
}

static double msVmNumericValue(const struct MsObject* value) {
  return msObjectTag(value) == MS_TYPE_INT
      ? (double)msIntWordValue(value)
      : msFloatValue(value);
}

static struct MsObject* msVmBinaryOp(MsState* L, MsOpCode op, struct MsObject* lhs, struct MsObject* rhs) {
  if (msObjectTag(lhs) == MS_TYPE_INT && msObjectTag(rhs) == MS_TYPE_INT
      && msIntIsWord(lhs) && msIntIsWord(rhs)) {
    int64_t lhsValue = msIntWordValue(lhs);
    int64_t rhsValue = msIntWordValue(rhs);
    int64_t result;
    switch (op) {
      case MS_OP_ADD:
        if ((rhsValue > 0 && lhsValue > INT64_MAX - rhsValue)
            || (rhsValue < 0 && lhsValue < INT64_MIN - rhsValue)) {
          break;
        }
        return msNewInt(L, lhsValue + rhsValue);
      case MS_OP_SUB:
        if ((rhsValue > 0 && lhsValue < INT64_MIN + rhsValue)
            || (rhsValue < 0 && lhsValue > INT64_MAX + rhsValue)) {
          break;
        }
        return msNewInt(L, lhsValue - rhsValue);
      case MS_OP_MUL:
        if (!msVmCheckedMul(lhsValue, rhsValue, &result)) {
          break;
        }
        return msNewInt(L, result);
      case MS_OP_DIV:
        if (rhsValue == 0) {
          break;
        }
        return msNewFloat(L, (double)lhsValue / (double)rhsValue);
      case MS_OP_FLOORDIV:
      case MS_OP_MOD:
        if (rhsValue == 0 || (lhsValue == INT64_MIN && rhsValue == -1)) {
          break;
        }
        result = op == MS_OP_FLOORDIV ? lhsValue / rhsValue : lhsValue % rhsValue;
        int64_t remainder = lhsValue % rhsValue;
        if (remainder != 0 && ((lhsValue < 0) != (rhsValue < 0))) {
          result += op == MS_OP_FLOORDIV ? -1 : rhsValue;
        }
        return msNewInt(L, result);
      case MS_OP_POW:
        if (rhsValue < 0) {
          return msNewFloat(L, pow((double)lhsValue, (double)rhsValue));
        }
        result = 1;
        int64_t base = lhsValue;
        uint64_t exponent = (uint64_t)rhsValue;
        while (exponent != 0) {
          if ((exponent & 1) != 0 && !msVmCheckedMul(result, base, &result)) {
            return msObjectBinaryOp(L, op, lhs, rhs);
          }
          exponent >>= 1;
          if (exponent != 0 && !msVmCheckedMul(base, base, &base)) {
            return msObjectBinaryOp(L, op, lhs, rhs);
          }
        }
        return msNewInt(L, result);
      case MS_OP_BITAND:
        return msNewInt(L, lhsValue & rhsValue);
      case MS_OP_BITOR:
        return msNewInt(L, lhsValue | rhsValue);
      case MS_OP_BITXOR:
        return msNewInt(L, lhsValue ^ rhsValue);
      case MS_OP_SHL:
      case MS_OP_SHR:
        if (rhsValue < 0 || rhsValue >= 64) {
          msRaiseRuntimeError(L, "invalid shift count");
          return NULL;
        }
        if (op == MS_OP_SHR) {
          unsigned int count = (unsigned int)rhsValue;
          result = lhsValue >= 0
              ? (int64_t)((uint64_t)lhsValue >> count)
              : -1 - (int64_t)((~(uint64_t)lhsValue) >> count);
          return msNewInt(L, result);
        }
        if (rhsValue == 63) {
          if (lhsValue == 0) {
            return msNewInt(L, 0);
          }
          if (lhsValue == -1) {
            return msNewInt(L, INT64_MIN);
          }
          break;
        }
        int64_t factor = (int64_t)(UINT64_C(1) << (unsigned int)rhsValue);
        if (!msVmCheckedMul(lhsValue, factor, &result)) {
          break;
        }
        return msNewInt(L, result);
      default:
        return msObjectBinaryOp(L, op, lhs, rhs);
    }
    return msObjectBinaryOp(L, op, lhs, rhs);
  }

  if (msVmIsNumeric(lhs) && msVmIsNumeric(rhs)
      && (op == MS_OP_ADD || op == MS_OP_SUB || op == MS_OP_MUL || op == MS_OP_DIV || op == MS_OP_POW)) {
    double lhsValue = msVmNumericValue(lhs);
    double rhsValue = msVmNumericValue(rhs);
    if (op == MS_OP_DIV && rhsValue == 0.0) {
      msRaiseRuntimeError(L, "division by zero");
      return NULL;
    }
    double result;
    switch (op) {
      case MS_OP_ADD:
        result = lhsValue + rhsValue;
        break;
      case MS_OP_SUB:
        result = lhsValue - rhsValue;
        break;
      case MS_OP_MUL:
        result = lhsValue * rhsValue;
        break;
      case MS_OP_DIV:
        result = lhsValue / rhsValue;
        break;
      case MS_OP_POW:
        result = pow(lhsValue, rhsValue);
        break;
      default:
        return msObjectBinaryOp(L, op, lhs, rhs);
    }
    return msNewFloat(L, result);
  }
  return msObjectBinaryOp(L, op, lhs, rhs);
}

static struct MsObject* msVmUnaryOp(MsState* L, MsOpCode op, struct MsObject* operand) {
  if (op == MS_OP_NOT) {
    return msNewBool(L, !msObjectIsTruthy(operand));
  }
  if (msObjectTag(operand) == MS_TYPE_INT && msIntIsWord(operand)) {
    int64_t value = msIntWordValue(operand);
    if (op == MS_OP_NEG && value != INT64_MIN) {
      return msNewInt(L, -value);
    }
    if (op == MS_OP_INVERT) {
      return msNewInt(L, ~value);
    }
  }
  if (msObjectTag(operand) == MS_TYPE_FLOAT && op == MS_OP_NEG) {
    return msNewFloat(L, -msFloatValue(operand));
  }
  return msObjectUnaryOp(L, op, operand);
}

static bool msVmComparisonFromOrder(MsOpCode op, int order) {
  switch (op) {
    case MS_OP_EQ:
      return order == 0;
    case MS_OP_NE:
      return order != 0;
    case MS_OP_LT:
      return order == -1;
    case MS_OP_LE:
      return order == -1 || order == 0;
    case MS_OP_GT:
      return order == 1;
    case MS_OP_GE:
      return order == 1 || order == 0;
    default:
      return false;
  }
}

static bool msVmCompareNumeric(MsOpCode op, const struct MsObject* lhs, const struct MsObject* rhs) {
  int order = MS_OBJECT_COMPARE_UNORDERED;
  if (!msObjectCompare(lhs, rhs, &order)) {
    return false;
  }
  return msVmComparisonFromOrder(op, order);
}

static MsResult msVmCompare(MsState* L, struct MsCoroutine* co, const struct MsCallFrame* frame,
    MsOpCode op, bool* out) {
  if (co->stackTop < frame->baseIndex + frame->proto->localCount + 2) {
    return msVmErrorAt(L, frame, "evaluation stack underflow");
  }
  struct MsObject* rhs = co->stack[--co->stackTop];
  struct MsObject* lhs = co->stack[--co->stackTop];
  if (op == MS_OP_IS) {
    *out = lhs == rhs;
    return MS_OK;
  }
  if (op >= MS_OP_EQ && op <= MS_OP_GE && msVmIsNumeric(lhs) && msVmIsNumeric(rhs)) {
    *out = msVmCompareNumeric(op, lhs, rhs);
    return MS_OK;
  }
  if (op == MS_OP_EQ || op == MS_OP_NE) {
    bool equal = lhs == rhs || msObjectValueEquals(lhs, rhs);
    *out = op == MS_OP_EQ ? equal : !equal;
    return MS_OK;
  }

  struct MsObject* value = msObjectBinaryOp(L, op, lhs, rhs);
  if (value == NULL) {
    return msVmPropagatedErrorAt(L, frame);
  }
  *out = msBoolValue(value);
  return MS_OK;
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
      case MS_OP_SUB:
      case MS_OP_MUL:
      case MS_OP_DIV:
      case MS_OP_FLOORDIV:
      case MS_OP_MOD:
      case MS_OP_POW:
      case MS_OP_BITAND:
      case MS_OP_BITOR:
      case MS_OP_BITXOR:
      case MS_OP_SHL:
      case MS_OP_SHR: {
        if (co->stackTop < frame->baseIndex + frame->proto->localCount + 2) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        struct MsObject* rhs = co->stack[--co->stackTop];
        struct MsObject* lhs = co->stack[--co->stackTop];
        struct MsObject* value = msVmBinaryOp(L, msOpDecodeOp(instr), lhs, rhs);
        if (value == NULL) {
          result = msVmPropagatedErrorAt(L, frame);
          break;
        }
        co->stack[co->stackTop++] = value;
        break;
      }
      case MS_OP_NEG:
      case MS_OP_INVERT:
      case MS_OP_NOT: {
        if (!msVmCanPop(co, frame)) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        struct MsObject* operand = co->stack[--co->stackTop];
        struct MsObject* value = msVmUnaryOp(L, msOpDecodeOp(instr), operand);
        if (value == NULL) {
          result = msVmPropagatedErrorAt(L, frame);
          break;
        }
        co->stack[co->stackTop++] = value;
        break;
      }
      case MS_OP_EQ:
      case MS_OP_NE:
      case MS_OP_LT:
      case MS_OP_LE:
      case MS_OP_GT:
      case MS_OP_GE:
      case MS_OP_IS:
      case MS_OP_IN: {
        bool compared = false;
        result = msVmCompare(L, co, frame, msOpDecodeOp(instr), &compared);
        if (result == MS_OK) {
          co->stack[co->stackTop++] = msNewBool(L, compared);
        }
        break;
      }
      case MS_OP_CMP_CHAIN: {
        MsOpCode op = (MsOpCode)msOpDecodeA(instr);
        if (op < MS_OP_EQ || op > MS_OP_IN) {
          result = msVmErrorAt(L, frame, "invalid comparison chain opcode");
          break;
        }
        if (co->stackTop < frame->baseIndex + frame->proto->localCount + 2) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        struct MsObject* right = co->stack[co->stackTop - 1];
        bool compared = false;
        result = msVmCompare(L, co, frame, op, &compared);
        if (result != MS_OK) {
          break;
        }
        co->stack[co->stackTop++] = compared ? right : msNewBool(L, false);
        if (!compared) {
          frame->pc += msOpDecodeBx(instr);
        }
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
      case MS_OP_BUILD_LIST:
      case MS_OP_BUILD_DICT: {
        bool isDict = msOpDecodeOp(instr) == MS_OP_BUILD_DICT;
        int items = msOpDecodeA(instr);
        int count = isDict ? items * 2 : items;
        int base = co->stackTop - count;
        if (base < frame->baseIndex + frame->proto->localCount) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        if (count == 0 && !msVmCanPush(co, frame, windowEnd)) {
          result = msVmErrorAt(L, frame, "evaluation stack overflow");
          break;
        }
        struct MsObject* container = isDict ? msNewDict(L) : msNewList(L, items);
        if (container == NULL) {
          result = msVmPropagatedErrorAt(L, frame);
          break;
        }
        for (int i = 0; i < count; i += isDict ? 2 : 1) {
          if (isDict) {
            result = msDictSet(L, (struct MsDict*)container, co->stack[base + i], co->stack[base + i + 1]);
          } else {
            result = msListAppend(L, (struct MsList*)container, co->stack[base + i]);
          }
          if (result != MS_OK) {
            result = msVmPropagatedErrorAt(L, frame);
            break;
          }
        }
        if (result != MS_OK) {
          break;
        }
        co->stackTop = base;
        co->stack[co->stackTop++] = container;
        break;
      }
      case MS_OP_UNPACK: {
        int count = msOpDecodeA(instr);
        int star = msOpDecodeBx(instr);
        if (!msVmCanPop(co, frame)) {
          result = msVmErrorAt(L, frame, "evaluation stack underflow");
          break;
        }
        struct MsObject* source = co->stack[co->stackTop - 1];
        if (msObjectTag(source) != MS_TYPE_LIST) {
          const struct MsString* typeName = msHeapType(&L->heap, msObjectTag(source))->name;
          msRaiseTypeError(L, "cannot unpack non-list '%s'", msStringData(typeName));
          result = msVmPropagatedErrorAt(L, frame);
          break;
        }
        struct MsList* list = (struct MsList*)source;
        int64_t length = msListLen(list);
        bool valid = star == MS_UNPACK_NO_STAR ? length == count : length >= count - 1;
        if (!valid) {
          if (star == MS_UNPACK_NO_STAR) {
            msRaiseValueError(L, "expected %d values to unpack, got %lld", count, (long long)length);
          } else {
            msRaiseValueError(L, "expected at least %d values to unpack, got %lld",
                count - 1, (long long)length);
          }
          result = msVmPropagatedErrorAt(L, frame);
          break;
        }
        if (count < 1 || (star != MS_UNPACK_NO_STAR && (star < 0 || star >= count))
            || co->stackTop - 1 + count >= windowEnd) {
          result = msVmErrorAt(L, frame, "invalid unpack instruction");
          break;
        }
        int sourceIndex = co->stackTop - 1;
        int64_t middleLen = star == MS_UNPACK_NO_STAR ? 0 : length - (count - 1);
        for (int i = 0; i < count; ++i) {
          struct MsObject* value = NULL;
          if (i == star) {
            value = msNewList(L, middleLen);
            if (value == NULL) {
              result = msVmPropagatedErrorAt(L, frame);
              break;
            }
            co->stack[sourceIndex + 1 + i] = value;
            co->stackTop = sourceIndex + i + 2;
            for (int64_t j = 0; j < middleLen; ++j) {
              result = msListAppend(L, (struct MsList*)value, list->items[star + j]);
              if (result != MS_OK) {
                result = msVmPropagatedErrorAt(L, frame);
                break;
              }
            }
          } else {
            int64_t index = star != MS_UNPACK_NO_STAR && i > star
                ? length - (count - i)
                : i;
            value = list->items[index];
            co->stack[sourceIndex + 1 + i] = value;
            co->stackTop = sourceIndex + i + 2;
          }
          if (result != MS_OK) {
            break;
          }
        }
        if (result != MS_OK) {
          break;
        }
        co->stack[sourceIndex] = co->stack[sourceIndex + 1];
        for (int i = 1; i < count; ++i) {
          co->stack[sourceIndex + i] = co->stack[sourceIndex + i + 1];
        }
        co->stackTop = sourceIndex + count;
        break;
      }
      case MS_OP_CALL: {
        // safepoint: scheduler preemption and GC handshake.
        int argc = msOpDecodeA(instr);
        int base = co->stackTop - argc;
        if (base < 1) {
          result = msVmErrorAt(L, frame, "invalid call stack");
          break;
        }
        struct MsObject* callable = co->stack[base - 1];
        if (msObjectTag(callable) == MS_TYPE_FUNCTION) {
          struct MsFunction* function = (struct MsFunction*)callable;
          struct MsProto* proto = function->proto;
          if (proto->isAsync) {
            result = msVmErrorAt(L, frame, "async call requires the scheduler");
            break;
          }
          result = msVmPushFrameAt(L, co, proto, argc, NULL, true, base,
              function->upvalues, function->upvalueCount);
          if (result == MS_OK) {
            frame = &co->frames[co->frameCount - 1];
          }
          break;
        }
        if (msObjectTag(callable) == MS_TYPE_C_FUNCTION) {
          MsCFunction function = msAsCFunction(callable);
          struct MsObject* value = function(L, argc, &co->stack[base]);
          frame = &co->frames[co->frameCount - 1];
          if (value == NULL) {
            result = msVmPropagatedErrorAt(L, frame);
            break;
          }
          co->stackTop = base - 1;
          co->stack[co->stackTop++] = value;
          break;
        }
        result = msVmErrorAt(L, frame, "object is not callable");
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
      if (result == MS_ERROR_RUNTIME) {
        result = msVmWrapErrorAt(L, frame);
      }
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
