#ifndef MSLANG_SRC_OBJECT_MS_DICT_H_
#define MSLANG_SRC_OBJECT_MS_DICT_H_

#include <stdbool.h>
#include <stdint.h>

#include <mslang/state.h>

#include "object/ms_object.h"

struct MsDictEntry {
  struct MsObject* key;
  struct MsObject* value;
};

struct MsDict {
  struct MsObjectHeader header;
  struct MsDictEntry* entries;
  int64_t len;
  int64_t capacity;
};

struct MsObject* msNewDict(MsState* L);
MsResult msDictSet(MsState* L, struct MsDict* dict, struct MsObject* key, struct MsObject* value);
bool msDictNext(const struct MsDict* dict, int64_t* cursor, struct MsObject** key, struct MsObject** value);
void msDictReleaseStorage(struct MsDict* dict);

static inline int64_t msDictLen(const struct MsDict* dict) {
  return dict->len;
}

#endif  // MSLANG_SRC_OBJECT_MS_DICT_H_
