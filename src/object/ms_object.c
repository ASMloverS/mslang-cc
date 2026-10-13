#include "object/ms_object.h"

#include <string.h>

#include "core/ms_memory.h"
#include "object/ms_string.h"

static const char* const msTypeTagNames[MS_TYPE_COUNT] = {
  "MS_TYPE_NIL", "MS_TYPE_BOOL", "MS_TYPE_INT", "MS_TYPE_FLOAT",
  "MS_TYPE_STRING", "MS_TYPE_BYTES", "MS_TYPE_LIST", "MS_TYPE_TUPLE",
  "MS_TYPE_DICT", "MS_TYPE_SET", "MS_TYPE_FUNCTION", "MS_TYPE_CLASS",
  "MS_TYPE_INSTANCE", "MS_TYPE_MODULE", "MS_TYPE_CHANNEL", "MS_TYPE_COROUTINE",
  "MS_TYPE_ITERATOR", "MS_TYPE_BOUND_METHOD", "MS_TYPE_C_FUNCTION",
  "MS_TYPE_C_TYPE"
};

// Script-visible names per 02-types section 1. Used by msHeapInit.
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
  MS_ASSERT(*link != NULL);  // freeing an unlinked object is a programming error
  if (*link == NULL) {
    return;                  // release guard
  }
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

struct MsType* msHeapType(struct MsHeap* heap, MsTypeTag tag) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(tag >= 0 && tag < MS_TYPE_COUNT);
  return &heap->types[tag];
}

MsResult msHeapInit(struct MsHeap* heap) {
  MS_ASSERT(heap != NULL);
  memset(heap, 0, sizeof(*heap));
  // Keep in sync with msTestHeapBootstrap in tests/c/test_string.c.
  heap->internSlots = (struct MsString**)msAlloc(MS_STRING_INTERN_INITIAL_CAP
                                                 * sizeof(*heap->internSlots));
  if (heap->internSlots == NULL) {
    return MS_ERROR_OOM;
  }
  heap->internCap = MS_STRING_INTERN_INITIAL_CAP;
  memset(heap->internSlots, 0, heap->internCap * sizeof(*heap->internSlots));
  // Type objects: permanent (black), unlinked, self-referential type pointer.
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    heap->types[i].header.type = &heap->types[MS_TYPE_CLASS];
    heap->types[i].header.tag = MS_TYPE_CLASS;
    heap->types[i].header.markColor = MS_MARK_BLACK;
    heap->types[i].header.gcNext = NULL;
    heap->types[i].name = NULL;
  }
  // Type names come from the intern table; on OOM roll back in reverse
  // acquisition order via msHeapDestroy (which also re-zeroes *heap).
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    heap->types[i].name = msStringFromCStr(heap, msTypeScriptNames[i]);
    if (heap->types[i].name == NULL) {
      msHeapDestroy(heap);
      return MS_ERROR_OOM;
    }
  }
  heap->nilObject.header.type = &heap->types[MS_TYPE_NIL];
  heap->nilObject.header.tag = MS_TYPE_NIL;
  heap->nilObject.header.markColor = MS_MARK_BLACK;
  heap->nilObject.header.gcNext = NULL;
  heap->trueObject.header.type = &heap->types[MS_TYPE_BOOL];
  heap->trueObject.header.tag = MS_TYPE_BOOL;
  heap->trueObject.header.markColor = MS_MARK_BLACK;
  heap->trueObject.header.gcNext = NULL;
  heap->trueObject.value = true;
  heap->falseObject.header.type = &heap->types[MS_TYPE_BOOL];
  heap->falseObject.header.tag = MS_TYPE_BOOL;
  heap->falseObject.header.markColor = MS_MARK_BLACK;
  heap->falseObject.header.gcNext = NULL;
  heap->falseObject.value = false;
  for (size_t i = 0; i < MS_INT_CACHE_SIZE; ++i) {
    heap->intCache[i].header.type = &heap->types[MS_TYPE_INT];
    heap->intCache[i].header.tag = MS_TYPE_INT;
    heap->intCache[i].header.markColor = MS_MARK_BLACK;
    heap->intCache[i].header.gcNext = NULL;
    heap->intCache[i].value = (int64_t)MS_INT_CACHE_MIN + (int64_t)i;
  }
  return MS_OK;
}

struct MsObject* msHeapNil(struct MsHeap* heap) {
  MS_ASSERT(heap != NULL);
  return &heap->nilObject;
}

struct MsObject* msHeapBool(struct MsHeap* heap, bool value) {
  MS_ASSERT(heap != NULL);
  return value ? (struct MsObject*)&heap->trueObject : (struct MsObject*)&heap->falseObject;
}

struct MsInt* msIntNew(struct MsHeap* heap, int64_t value) {
  MS_ASSERT(heap != NULL);
  // Residency is an implementation detail (02-types section 6); the cache-hit
  // decision lives here and nowhere else.
  if (value >= MS_INT_CACHE_MIN && value <= MS_INT_CACHE_MAX) {
    return &heap->intCache[value - MS_INT_CACHE_MIN];
  }
  struct MsInt* obj = (struct MsInt*)msObjectAlloc(heap, MS_TYPE_INT, sizeof(struct MsInt));
  if (obj == NULL) {
    return NULL;
  }
  obj->value = value;
  return obj;
}

struct MsFloat* msFloatNew(struct MsHeap* heap, double value) {
  MS_ASSERT(heap != NULL);
  struct MsFloat* obj =
      (struct MsFloat*)msObjectAlloc(heap, MS_TYPE_FLOAT, sizeof(struct MsFloat));
  if (obj == NULL) {
    return NULL;
  }
  obj->value = value;
  return obj;
}

bool msIntIsWord(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL && obj->header.tag == MS_TYPE_INT);
  MS_UNUSED(obj);                // Release (NDEBUG): the assert compiles out
  return true;                   // v0.1 ints are always machine-word; task 31 changes this
}

int64_t msIntWordValue(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL && obj->header.tag == MS_TYPE_INT);
  MS_ASSERT(msIntIsWord(obj));
  return ((const struct MsInt*)obj)->value;
}

double msFloatValue(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL && obj->header.tag == MS_TYPE_FLOAT);
  return ((const struct MsFloat*)obj)->value;
}

bool msBoolValue(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL && obj->header.tag == MS_TYPE_BOOL);
  return ((const struct MsBool*)obj)->value;
}
