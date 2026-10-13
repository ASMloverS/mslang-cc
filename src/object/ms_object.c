#include "object/ms_object.h"

#include <string.h>

#include "core/ms_memory.h"

static const char* const msTypeTagNames[MS_TYPE_COUNT] = {
  "MS_TYPE_NIL", "MS_TYPE_BOOL", "MS_TYPE_INT", "MS_TYPE_FLOAT",
  "MS_TYPE_STRING", "MS_TYPE_BYTES", "MS_TYPE_LIST", "MS_TYPE_TUPLE",
  "MS_TYPE_DICT", "MS_TYPE_SET", "MS_TYPE_FUNCTION", "MS_TYPE_CLASS",
  "MS_TYPE_INSTANCE", "MS_TYPE_MODULE", "MS_TYPE_CHANNEL", "MS_TYPE_COROUTINE",
  "MS_TYPE_ITERATOR", "MS_TYPE_BOUND_METHOD", "MS_TYPE_C_FUNCTION",
  "MS_TYPE_C_TYPE"
};

// Script-visible names per 02-types section 1. Used by msHeapInit (task: heap init).
static const char* const msTypeScriptNames[MS_TYPE_COUNT] = {
  "NilType", "bool", "int", "float", "str", "bytes", "list", "tuple",
  "dict", "set", "function", "type", "instance", "module", "chan",
  "coroutine", "iterator", "bound method", "cfunction", "ctype"
};

const char* msTypeTagName(MsTypeTag tag) {
  if (tag < 0 || tag >= MS_TYPE_COUNT) {
    return "MS_TYPE_INVALID";
  }
  return msTypeTagNames[tag];
}

struct MsObject* msObjectAlloc(struct MsHeap* heap, MsTypeTag tag, size_t size) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(tag >= 0 && tag < MS_TYPE_COUNT);
  MS_ASSERT(size >= sizeof(struct MsObject));
  struct MsObject* obj = (struct MsObject*)msAlloc(size);
  if (obj == NULL) {
    return NULL;
  }
  obj->header.type = &heap->types[tag];
  obj->header.tag = (uint8_t)tag;
  obj->header.markColor = MS_MARK_WHITE;
  obj->header.gcNext = heap->allObjects;
  heap->allObjects = obj;
  ++heap->objectCount;
  return obj;
}

void msObjectFree(struct MsHeap* heap, struct MsObject* obj) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(obj != NULL);
  struct MsObject** link = &heap->allObjects;
  while (*link != NULL && *link != obj) {
    link = &(*link)->header.gcNext;
  }
  // Only linked objects may be freed; MS_ASSERT guards debug, this guards release.
  if (*link == NULL) {
    return;
  }
  MS_ASSERT(*link == obj);
  *link = obj->header.gcNext;
  --heap->objectCount;
  msFree(obj);
}

void msHeapDestroy(struct MsHeap* heap) {
  if (heap == NULL) {
    return;
  }
  struct MsObject* obj = heap->allObjects;
  while (obj != NULL) {
    struct MsObject* next = obj->header.gcNext;
    msFree(obj);
    obj = next;
  }
  msFree(heap->internSlots);
  memset(heap, 0, sizeof(*heap));
}
