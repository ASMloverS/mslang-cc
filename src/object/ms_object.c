#include "object/ms_object.h"

#include <string.h>

#include "core/ms_memory.h"
#include "object/ms_dict.h"
#include "object/ms_list.h"
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
  if (tag == MS_TYPE_PROTO) {
    return "MS_TYPE_PROTO";
  }
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

static void msObjectReleaseStorage(struct MsObject* obj) {
  switch (obj->header.tag) {
    case MS_TYPE_LIST:
      msListReleaseStorage((struct MsList*)obj);
      break;
    case MS_TYPE_DICT:
      msDictReleaseStorage((struct MsDict*)obj);
      break;
    default:
      break;
  }
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
  if (obj->header.tag == MS_TYPE_STRING) {
    msStringInternRemove(heap, (const struct MsString*)obj);
  }
  msObjectReleaseStorage(obj);
  msFree(obj);
}

void msHeapDestroy(struct MsHeap* heap) {
  if (heap == NULL) {
    return;
  }
  struct MsObject* obj = heap->allObjects;
  while (obj != NULL) {
    struct MsObject* next = obj->header.gcNext;
    msObjectReleaseStorage(obj);
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

// splitmix64 finalizer: well-mixed 64 -> 32 bit reduction.
static uint32_t msHashMix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return (uint32_t)x;
}

bool msObjectValueEquals(const struct MsObject* a, const struct MsObject* b) {
  if (a == b) {
    // Identity fast path (interning residency) -- but NaN is unequal even to
    // itself (02-types section 3.2), so floats take the value compare here.
    if (a != NULL && a->header.tag == MS_TYPE_FLOAT) {
      double v = ((const struct MsFloat*)a)->value;
      return v == v;
    }
    return true;
  }
  if (a == NULL || b == NULL) {
    return false;
  }
  if (a->header.tag != b->header.tag) {
    // Cross-tag numeric compare (02-types section 6). (double)X is always an
    // integral double, so equality can only hold when the float side is
    // integral-valued and in int64 range; compare exactly as int64 so that
    // msObjectValueEquals and msObjectHash agree by construction (e.g. int
    // 2^53+1 must NOT equal float 2^53, or their hashes would diverge).
    // Bigint promotion (task 31) must keep this discipline.
    const struct MsInt* intSide;
    const struct MsFloat* floatSide;
    if (a->header.tag == MS_TYPE_INT && b->header.tag == MS_TYPE_FLOAT) {
      intSide = (const struct MsInt*)a;
      floatSide = (const struct MsFloat*)b;
    } else if (a->header.tag == MS_TYPE_FLOAT && b->header.tag == MS_TYPE_INT) {
      intSide = (const struct MsInt*)b;
      floatSide = (const struct MsFloat*)a;
    } else {
      return false;
    }
    double dv = floatSide->value;
    if (dv >= -9223372036854775808.0 && dv < 9223372036854775808.0) {
      int64_t fiv = (int64_t)dv;
      if ((double)fiv == dv) {
        return fiv == intSide->value;
      }
    }
    return false;
  }
  switch (a->header.tag) {
    case MS_TYPE_NIL:
      return true;
    case MS_TYPE_BOOL:
      return ((const struct MsBool*)a)->value == ((const struct MsBool*)b)->value;
    case MS_TYPE_INT:
      return ((const struct MsInt*)a)->value == ((const struct MsInt*)b)->value;
    case MS_TYPE_FLOAT:
      return ((const struct MsFloat*)a)->value == ((const struct MsFloat*)b)->value;
    case MS_TYPE_STRING:
      return msStringEquals((const struct MsString*)a, (const struct MsString*)b);
    case MS_TYPE_LIST: {
      const struct MsList* lhs = (const struct MsList*)a;
      const struct MsList* rhs = (const struct MsList*)b;
      if (lhs->len != rhs->len) {
        return false;
      }
      for (int64_t i = 0; i < lhs->len; ++i) {
        if (!msObjectValueEquals(lhs->items[i], rhs->items[i])) {
          return false;
        }
      }
      return true;
    }
    default:
      // Value semantics for remaining containers/instances land in tasks 16/25
      // inside this function; callers see no change.
      return false;
  }
}

uint32_t msObjectHash(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL);
  switch (obj->header.tag) {
    case MS_TYPE_NIL:
      return 0x27d4eb2fu;        // fixed constant; any stable value works
    case MS_TYPE_BOOL:
      return ((const struct MsBool*)obj)->value ? 0x9e3779b9u : 0x85ebca6bu;
    case MS_TYPE_INT:
      return msHashMix64((uint64_t)((const struct MsInt*)obj)->value);
    case MS_TYPE_FLOAT: {
      double v = ((const struct MsFloat*)obj)->value;
      // Integral doubles hash as ints so hash(42) == hash(42.0); required for
      // numeric dict keys (task 16). Bounds guard the int64 conversion.
      if (v >= -9223372036854775808.0 && v < 9223372036854775808.0) {
        int64_t iv = (int64_t)v;
        if ((double)iv == v) {
          return msHashMix64((uint64_t)iv);
        }
      }
      uint64_t bits;
      memcpy(&bits, &v, sizeof(bits));
      return msHashMix64(bits);
    }
    case MS_TYPE_STRING:
      return ((const struct MsString*)obj)->hash;
    default:
      // Identity hashing for instances/containers is a later task.
      MS_ASSERT(!"msObjectHash: tag not hashable in this task");
      return 0;
  }
}

bool msObjectIsTruthy(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL);
  switch (obj->header.tag) {
    case MS_TYPE_NIL:
      return false;
    case MS_TYPE_BOOL:
      return ((const struct MsBool*)obj)->value;
    case MS_TYPE_INT:
      return ((const struct MsInt*)obj)->value != 0;
    case MS_TYPE_FLOAT:
      return ((const struct MsFloat*)obj)->value != 0.0;
    case MS_TYPE_STRING:
      return ((const struct MsString*)obj)->byteLen != 0;
    case MS_TYPE_LIST:
      return msListLen((const struct MsList*)obj) != 0;
    case MS_TYPE_DICT:
      return msDictLen((const struct MsDict*)obj) != 0;
    default:
      return true;
  }
}
