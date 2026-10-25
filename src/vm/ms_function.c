#include "vm/ms_function.h"

#include "vm/ms_state_internal.h"

struct MsObject* msNewFunction(MsState* L, struct MsProto* proto) {
  if (proto == NULL) {
    return NULL;
  }
  struct MsFunction* function = (struct MsFunction*)msObjectAlloc(&L->heap, MS_TYPE_FUNCTION,
      sizeof(struct MsFunction));
  if (function == NULL) {
    L->error = NULL;
    L->errorResult = MS_ERROR_OOM;
    return NULL;
  }
  function->proto = proto;
  function->upvalues = NULL;
  function->upvalueCount = 0;
  return (struct MsObject*)function;
}

struct MsObject* msNewCFunction(MsState* L, struct MsString* name, MsCFunction func) {
  if (func == NULL) {
    return NULL;
  }
  struct MsCFunctionObject* function = (struct MsCFunctionObject*)msObjectAlloc(&L->heap,
      MS_TYPE_C_FUNCTION, sizeof(struct MsCFunctionObject));
  if (function == NULL) {
    L->error = NULL;
    L->errorResult = MS_ERROR_OOM;
    return NULL;
  }
  function->name = name;
  function->func = func;
  return (struct MsObject*)function;
}

struct MsProto* msFunctionProto(const struct MsObject* fn) {
  if (fn == NULL || msObjectTag(fn) != MS_TYPE_FUNCTION) {
    return NULL;
  }
  return ((const struct MsFunction*)fn)->proto;
}

MsCFunction msAsCFunction(const struct MsObject* fn) {
  if (fn == NULL || msObjectTag(fn) != MS_TYPE_C_FUNCTION) {
    return NULL;
  }
  return ((const struct MsCFunctionObject*)fn)->func;
}
