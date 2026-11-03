#include "vm/ms_state_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mslang/object.h>

#include "core/ms_memory.h"
#include "vm/ms_builtin.h"

void msStateSetOom(MsState* L) {
  L->error = NULL;
  L->errorResult = MS_ERROR_OOM;
}

void msStateSetError(MsState* L, MsResult result, const char* message, size_t messageLen) {
  struct MsString* error = msStringNew(&L->heap, message, messageLen);
  if (error == NULL) {
    msStateSetOom(L);
    return;
  }
  L->error = error;
  L->errorResult = result;
}

static void msStateRaiseV(MsState* L, const char* prefix, const char* fmt, va_list args) {
  va_list measureArgs;
  va_copy(measureArgs, args);
  int bodyLen = vsnprintf(NULL, 0, fmt, measureArgs);
  va_end(measureArgs);
  if (bodyLen < 0) {
    msStateSetOom(L);
    return;
  }

  char* body = msAlloc((size_t)bodyLen + 1);
  if (body == NULL) {
    msStateSetOom(L);
    return;
  }
  va_list formatArgs;
  va_copy(formatArgs, args);
  int written = vsnprintf(body, (size_t)bodyLen + 1, fmt, formatArgs);
  va_end(formatArgs);
  if (written != bodyLen) {
    msFree(body);
    msStateSetOom(L);
    return;
  }

  size_t prefixLen = strlen(prefix);
  char* message = msAlloc(prefixLen + (size_t)bodyLen + 1);
  if (message == NULL) {
    msFree(body);
    msStateSetOom(L);
    return;
  }
  memcpy(message, prefix, prefixLen);
  memcpy(message + prefixLen, body, (size_t)bodyLen + 1);
  msStateSetError(L, MS_ERROR_RUNTIME, message, prefixLen + (size_t)bodyLen);
  msFree(message);
  msFree(body);
}

MsState* msNewState(void) {
  MsState* L = msAlloc(sizeof(*L));
  if (L == NULL) {
    return NULL;
  }
  memset(L, 0, sizeof(*L));
  if (msHeapInit(&L->heap) != MS_OK) {
    msFree(L);
    return NULL;
  }
  msCoroutineInit(&L->main);
  msBuiltinRegisterAll(L);
  if (L->errorResult != MS_OK) {
    msCloseState(L);
    return NULL;
  }
  return L;
}

void msCloseState(MsState* L) {
  if (L == NULL) {
    return;
  }
  msCoroutineDestroy(&L->main);
  msFree(L->globals);
  msHeapDestroy(&L->heap);
  msFree(L);
}

struct MsObject* msStateGetGlobal(MsState* L, const struct MsString* name) {
  for (int i = 0; i < L->globalsLen; ++i) {
    if (msStringEquals(L->globals[i].name, name)) {
      return L->globals[i].value;
    }
  }
  return NULL;
}

MsResult msStateSetGlobal(MsState* L, const struct MsString* name, struct MsObject* value) {
  for (int i = 0; i < L->globalsLen; ++i) {
    if (msStringEquals(L->globals[i].name, name)) {
      L->globals[i].value = value;
      return MS_OK;
    }
  }

  if (L->globalsLen == L->globalsCap) {
    int newCap = 8;
    if (L->globalsCap != 0) {
      if (L->globalsCap > INT_MAX / 2) {
        msStateSetOom(L);
        return MS_ERROR_OOM;
      }
      newCap = L->globalsCap * 2;
    }
    if ((size_t)newCap > SIZE_MAX / sizeof(*L->globals)) {
      msStateSetOom(L);
      return MS_ERROR_OOM;
    }
    struct MsGlobalEntry* entries = msRealloc(L->globals, (size_t)newCap * sizeof(*entries));
    if (entries == NULL) {
      msStateSetOom(L);
      return MS_ERROR_OOM;
    }
    L->globals = entries;
    L->globalsCap = newCap;
  }
  L->globals[L->globalsLen].name = (struct MsString*)name;
  L->globals[L->globalsLen].value = value;
  ++L->globalsLen;
  return MS_OK;
}

const char* msStateErrorMessage(const MsState* L) {
  return L->error == NULL ? NULL : msStringData(L->error);
}

MsResult msStateErrorResult(const MsState* L) {
  return L->errorResult;
}

void msStateClearError(MsState* L) {
  L->error = NULL;
  L->errorResult = MS_OK;
}

MsObject* msErrorGet(MsState* L) {
  if (L == NULL) {
    return NULL;
  }
  struct MsString* error = L->error;
  msStateClearError(L);
  return (struct MsObject*)error;
}

const char* msAsCString(MsState* L, MsObject* obj) {
  if (L == NULL || obj == NULL || msObjectTag(obj) != MS_TYPE_STRING) {
    if (L != NULL) {
      msRaiseTypeError(L, "msAsCString() requires a string");
    }
    return NULL;
  }
  return msStringData((struct MsString*)obj);
}

void msRaiseRuntimeError(MsState* L, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  msStateRaiseV(L, "", fmt, args);
  va_end(args);
}

void msRaiseTypeError(MsState* L, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  msStateRaiseV(L, "TypeError: ", fmt, args);
  va_end(args);
}

void msRaiseValueError(MsState* L, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  msStateRaiseV(L, "ValueError: ", fmt, args);
  va_end(args);
}

void msRaiseAssertionError(MsState* L, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  msStateRaiseV(L, "AssertionError: ", fmt, args);
  va_end(args);
}

struct MsObject* msNewNil(MsState* L) {
  return msHeapNil(&L->heap);
}

struct MsObject* msNewBool(MsState* L, bool value) {
  return msHeapBool(&L->heap, value);
}

struct MsObject* msNewInt(MsState* L, int64_t value) {
  struct MsObject* object = (struct MsObject*)msIntNew(&L->heap, value);
  if (object == NULL) {
    msStateSetOom(L);
  }
  return object;
}

struct MsObject* msNewFloat(MsState* L, double value) {
  struct MsObject* object = (struct MsObject*)msFloatNew(&L->heap, value);
  if (object == NULL) {
    msStateSetOom(L);
  }
  return object;
}
