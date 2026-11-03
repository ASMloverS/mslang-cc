#include "object/ms_dict.h"

#include "core/ms_memory.h"
#include "vm/ms_state_internal.h"

struct MsObject* msNewDict(MsState* L) {
  struct MsDict* dict = (struct MsDict*)msObjectAlloc(&L->heap, MS_TYPE_DICT, sizeof(struct MsDict));
  if (dict == NULL) {
    msStateSetOom(L);
    return NULL;
  }
  dict->entries = NULL;
  dict->len = 0;
  dict->capacity = 0;
  return (struct MsObject*)dict;
}

MsResult msDictSet(MsState* L, struct MsDict* dict, struct MsObject* key, struct MsObject* value) {
  MS_ASSERT(dict != NULL);
  MS_ASSERT(key != NULL);
  MS_ASSERT(value != NULL);
  for (int64_t i = 0; i < dict->len; ++i) {
    if (msObjectValueEquals(dict->entries[i].key, key)) {
      dict->entries[i].value = value;
      return MS_OK;
    }
  }
  if (dict->len == dict->capacity) {
    if (dict->capacity > INT64_MAX / 2) {
      msStateSetOom(L);
      return MS_ERROR_OOM;
    }
    int64_t newCapacity = dict->capacity == 0 ? 4 : dict->capacity * 2;
    if ((uint64_t)newCapacity > SIZE_MAX / sizeof(*dict->entries)) {
      msStateSetOom(L);
      return MS_ERROR_OOM;
    }
    struct MsDictEntry* entries = (struct MsDictEntry*)msRealloc(dict->entries,
        (size_t)newCapacity * sizeof(*entries));
    if (entries == NULL) {
      msStateSetOom(L);
      return MS_ERROR_OOM;
    }
    dict->entries = entries;
    dict->capacity = newCapacity;
  }
  dict->entries[dict->len].key = key;
  dict->entries[dict->len].value = value;
  ++dict->len;
  return MS_OK;
}

bool msDictNext(const struct MsDict* dict, int64_t* cursor, struct MsObject** key, struct MsObject** value) {
  MS_ASSERT(dict != NULL);
  MS_ASSERT(cursor != NULL);
  MS_ASSERT(key != NULL);
  MS_ASSERT(value != NULL);
  if (*cursor < 0 || *cursor >= dict->len) {
    return false;
  }
  const struct MsDictEntry* entry = &dict->entries[*cursor];
  ++*cursor;
  *key = entry->key;
  *value = entry->value;
  return true;
}

void msDictReleaseStorage(struct MsDict* dict) {
  MS_ASSERT(dict != NULL);
  msFree(dict->entries);
  dict->entries = NULL;
  dict->len = 0;
  dict->capacity = 0;
}
