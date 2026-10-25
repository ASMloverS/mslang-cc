#ifndef MSLANG_SRC_VM_MS_FUNCTION_H_
#define MSLANG_SRC_VM_MS_FUNCTION_H_

#include <stdint.h>

#include <mslang/state.h>

#include "object/ms_object.h"

typedef struct MsObject MsObject;
typedef MsObject* (*MsCFunction)(MsState* L, int64_t argc, MsObject** argv);

struct MsFunction {
  struct MsObjectHeader header;
  struct MsProto* proto;
  struct MsUpvalue** upvalues;
  int upvalueCount;
};

struct MsCFunctionObject {
  struct MsObjectHeader header;
  struct MsString* name;
  MsCFunction func;
};

struct MsObject* msNewFunction(MsState* L, struct MsProto* proto);
struct MsObject* msNewCFunction(MsState* L, struct MsString* name, MsCFunction func);
struct MsProto* msFunctionProto(const struct MsObject* fn);
MsCFunction msAsCFunction(const struct MsObject* fn);

#endif  // MSLANG_SRC_VM_MS_FUNCTION_H_
