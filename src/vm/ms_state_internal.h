#ifndef MSLANG_SRC_VM_MS_STATE_INTERNAL_H_
#define MSLANG_SRC_VM_MS_STATE_INTERNAL_H_

#include <mslang/state.h>

#include "object/ms_object.h"
#include "object/ms_string.h"
#include "vm/ms_vm.h"

struct MsGlobalEntry {
  struct MsString* name;
  struct MsObject* value;
};

struct MsState {
  struct MsHeap heap;
  struct MsCoroutine main;
  struct MsGlobalEntry* globals;
  int globalsLen;
  int globalsCap;
  struct MsString* error;
  MsResult errorResult;
  int64_t cKeywordCount;
};

struct MsObject* msStateGetGlobal(MsState* L, const struct MsString* name);
MsResult msStateSetGlobal(MsState* L, const struct MsString* name, struct MsObject* value);
const char* msStateErrorMessage(const MsState* L);
MsResult msStateErrorResult(const MsState* L);
void msStateClearError(MsState* L);
void msStateSetOom(MsState* L);
void msStateSetError(MsState* L, MsResult result, const char* message, size_t messageLen);
void msRaiseRuntimeError(MsState* L, const char* fmt, ...);
void msRaiseTypeError(MsState* L, const char* fmt, ...);
void msRaiseValueError(MsState* L, const char* fmt, ...);
void msRaiseAssertionError(MsState* L, const char* fmt, ...);
struct MsObject* msNewNil(MsState* L);
struct MsObject* msNewBool(MsState* L, bool value);
struct MsObject* msNewInt(MsState* L, int64_t value);
struct MsObject* msNewFloat(MsState* L, double value);

#endif  // MSLANG_SRC_VM_MS_STATE_INTERNAL_H_
