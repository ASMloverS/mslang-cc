#ifndef MSLANG_SRC_OBJECT_MS_OBJECT_H_
#define MSLANG_SRC_OBJECT_MS_OBJECT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mslang/error.h>

#include "core/ms_common.h"

struct MsString;

// Type tags: order and names match 08-vm-internals section 3. MS_TYPE_COUNT
// is a sentinel, not a tag (20 tags total).
typedef enum {
  MS_TYPE_NIL, MS_TYPE_BOOL, MS_TYPE_INT, MS_TYPE_FLOAT,
  MS_TYPE_STRING, MS_TYPE_BYTES, MS_TYPE_LIST, MS_TYPE_TUPLE,
  MS_TYPE_DICT, MS_TYPE_SET, MS_TYPE_FUNCTION, MS_TYPE_CLASS,
  MS_TYPE_INSTANCE, MS_TYPE_MODULE, MS_TYPE_CHANNEL, MS_TYPE_COROUTINE,
  MS_TYPE_ITERATOR, MS_TYPE_BOUND_METHOD, MS_TYPE_C_FUNCTION,
  MS_TYPE_C_TYPE,
  MS_TYPE_COUNT              // sentinel: number of type tags (20)
} MsTypeTag;

typedef enum {
  MS_MARK_WHITE = 0,         // unmarked; sweep candidate once GC lands (task 17)
  MS_MARK_BLACK = 1          // marked / permanent (singletons, cache, type objects)
} MsMarkColor;

struct MsObjectHeader {
  struct MsType* type;       // the type object; never NULL after construction
  uint8_t tag;               // MsTypeTag, cached for O(1) dispatch
  uint8_t markColor;         // MsMarkColor
  struct MsObject* gcNext;   // next node in the heap's all-objects list
};

struct MsObject {
  struct MsObjectHeader header;
};

struct MsBool {
  struct MsObjectHeader header;
  bool value;
};

struct MsInt {
  struct MsObjectHeader header;
  int64_t value;             // machine-word inline storage; bigint promotion is task 31
};

struct MsFloat {
  struct MsObjectHeader header;
  double value;              // IEEE 754 double
};

// Minimal type object; methods, MRO and instance layout are tasks 15/25.
struct MsType {
  struct MsObjectHeader header;   // tag == MS_TYPE_CLASS; type == &heap->types[MS_TYPE_CLASS]
  struct MsString* name;          // interned type name ("int", "str", ...)
};

#define MS_INT_CACHE_MIN (-256)
#define MS_INT_CACHE_MAX 4095
#define MS_INT_CACHE_SIZE (MS_INT_CACHE_MAX - MS_INT_CACHE_MIN + 1)   // 4352

struct MsHeap {
  struct MsObject* allObjects;            // head of the all-objects list (gcNext chain)
  size_t objectCount;                     // linked (GC-managed) objects; threshold input for task 17
  struct MsObject nilObject;              // the nil singleton
  struct MsBool trueObject;               // the true singleton
  struct MsBool falseObject;              // the false singleton
  struct MsInt intCache[MS_INT_CACHE_SIZE];        // small-int residency, embedded
  struct MsType types[MS_TYPE_COUNT];              // builtin type objects, embedded
  struct MsString** internSlots;          // short-string intern table, msAlloc'd bucket array
  size_t internCap;                       // bucket count, power of two
  size_t internLen;                       // interned string count
};

_Static_assert(MS_TYPE_COUNT == 20, "MsTypeTag must carry exactly 20 type tags");
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(struct MsObjectHeader) == 24,
               "the cached tag must not grow the 24-byte header on 64-bit");
#endif

// MS_ERROR_OOM leaves *heap zeroed.
MsResult msHeapInit(struct MsHeap* heap);

// Frees every linked object, then the intern buckets. NULL or zeroed heap is a no-op.
void msHeapDestroy(struct MsHeap* heap);

// Core allocation: msAlloc's size bytes, initializes the header, links into the
// all-objects list (gcNext), bumps objectCount. Returns NULL on OOM.
// All concrete constructors funnel through here; direct msAlloc of objects is
// forbidden outside this module.
struct MsObject* msObjectAlloc(struct MsHeap* heap, MsTypeTag tag, size_t size);

// Unlinks obj from the all-objects list and frees it. Used by msHeapDestroy and
// by the GC sweep (task 17); NOT part of normal value lifecycle before then.
void msObjectFree(struct MsHeap* heap, struct MsObject* obj);

// Singleton accessors (per-heap; no mutable globals).
struct MsObject* msHeapNil(struct MsHeap* heap);
struct MsObject* msHeapBool(struct MsHeap* heap, bool value);

// Int constructor: values in [MS_INT_CACHE_MIN, MS_INT_CACHE_MAX] return the
// resident cache entry (same pointer every time); others allocate via
// msObjectAlloc. Returns NULL on OOM.
struct MsInt* msIntNew(struct MsHeap* heap, int64_t value);

struct MsFloat* msFloatNew(struct MsHeap* heap, double value);

// Accessors take the boxed base pointer so the VM fast path (task 08) needs
// no downcast. Wrong-tag input is a programming error (MS_ASSERT in debug).
bool msIntIsWord(const struct MsObject* obj);        // machine-word int form; always true in v0.1
int64_t msIntWordValue(const struct MsObject* obj);  // valid only when msIntIsWord holds
double msFloatValue(const struct MsObject* obj);
bool msBoolValue(const struct MsObject* obj);

static inline MsTypeTag msObjectTag(const struct MsObject* obj) {
  return (MsTypeTag)obj->header.tag;
}

struct MsType* msHeapType(struct MsHeap* heap, MsTypeTag tag);     // &heap->types[tag]
const char* msTypeTagName(MsTypeTag tag);                          // static name table

// Value equality per 02-types section 6. Signature fixed by task 05's assumption.
bool msObjectValueEquals(const struct MsObject* a, const struct MsObject* b);

// Hash consistent with msObjectValueEquals: equal values hash equal.
// Hashable scalars only in this task (nil/bool/int/float/string); other tags
// MS_ASSERT in debug (identity hashing for instances is a later task).
uint32_t msObjectHash(const struct MsObject* obj);

// Truthiness per 02-types section 2, scalar subset: nil, false, numeric zero
// and the empty string are falsy. Container emptiness joins in task 16; heap
// objects of other tags are truthy.
bool msObjectIsTruthy(const struct MsObject* obj);

#endif  // MSLANG_SRC_OBJECT_MS_OBJECT_H_
