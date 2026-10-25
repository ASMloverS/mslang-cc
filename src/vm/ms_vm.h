#ifndef MSLANG_SRC_VM_MS_VM_H_
#define MSLANG_SRC_VM_MS_VM_H_

#include <mslang/error.h>

#include "vm/ms_proto.h"

typedef struct MsState MsState;

#define MS_VM_STACK_INIT_CAP 64
#define MS_VM_STACK_MAX_CAP (1 << 20)
#define MS_VM_FRAMES_INIT_CAP 8
#define MS_VM_FRAMES_MAX 1000

struct MsUpvalue;

struct MsCallFrame {
  struct MsProto* proto;
  int pc;
  int baseIndex;
  struct MsUpvalue** upvalues;
  int upvalueCount;
};

struct MsCoroutine {
  struct MsCallFrame* frames;
  int frameCount;
  int framesCap;
  struct MsObject** stack;
  int stackTop;
  int stackCap;
};

void msCoroutineInit(struct MsCoroutine* co);
void msCoroutineDestroy(struct MsCoroutine* co);
MsResult msVmPushFrame(MsState* L, struct MsCoroutine* co, struct MsProto* proto,
    int argc, struct MsObject** argv);
MsResult msVmRun(MsState* L, struct MsCoroutine* co, int stopAt);
MsResult msCallProto(MsState* L, struct MsCoroutine* co, struct MsProto* proto,
    int argc, struct MsObject** argv, struct MsObject** out);

#endif  // MSLANG_SRC_VM_MS_VM_H_
