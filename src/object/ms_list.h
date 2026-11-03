#ifndef MSLANG_SRC_OBJECT_MS_LIST_H_
#define MSLANG_SRC_OBJECT_MS_LIST_H_

#include <stdint.h>

#include <mslang/state.h>

#include "object/ms_object.h"

struct MsList {
  struct MsObjectHeader header;
  struct MsObject** items;
  int64_t len;
  int64_t capacity;
};

struct MsObject* msNewList(MsState* L, int64_t capacity);
MsResult msListAppend(MsState* L, struct MsList* list, struct MsObject* value);
void msListReleaseStorage(struct MsList* list);

static inline int64_t msListLen(const struct MsList* list) {
  return list->len;
}

#endif  // MSLANG_SRC_OBJECT_MS_LIST_H_
