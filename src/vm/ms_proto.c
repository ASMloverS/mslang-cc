#include "vm/ms_proto.h"

#include <string.h>

#include "core/ms_memory.h"

void msProtoBuilderInit(struct MsProtoBuilder* b) {
  memset(b, 0, sizeof(*b));
}

void msProtoBuilderDestroy(struct MsProtoBuilder* b) {
  msFree(b->proto.code);
  msFree(b->proto.consts);
  msFree(b->proto.tryBlocks);
  msFree(b->proto.lines);
  msProtoBuilderInit(b);
}
