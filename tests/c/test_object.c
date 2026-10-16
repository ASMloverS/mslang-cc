#include "object/ms_object.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "core/ms_memory.h"
#include "ms_test.h"
#include "object/ms_string.h"

MS_TEST(object, typeTagCountIs20) {
  MS_ASSERT_EQ(20, MS_TYPE_COUNT);
}

MS_TEST(object, typeTagNamesCompleteDistinctOrdered) {
  static const char* const expected[MS_TYPE_COUNT] = {
    "MS_TYPE_NIL", "MS_TYPE_BOOL", "MS_TYPE_INT", "MS_TYPE_FLOAT",
    "MS_TYPE_STRING", "MS_TYPE_BYTES", "MS_TYPE_LIST", "MS_TYPE_TUPLE",
    "MS_TYPE_DICT", "MS_TYPE_SET", "MS_TYPE_FUNCTION", "MS_TYPE_CLASS",
    "MS_TYPE_INSTANCE", "MS_TYPE_MODULE", "MS_TYPE_CHANNEL", "MS_TYPE_COROUTINE",
    "MS_TYPE_ITERATOR", "MS_TYPE_BOUND_METHOD", "MS_TYPE_C_FUNCTION",
    "MS_TYPE_C_TYPE"
  };
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    const char* name = msTypeTagName((MsTypeTag)i);
    MS_ASSERT_TRUE(name != NULL);
    MS_ASSERT_TRUE(strlen(name) > 0);
    MS_ASSERT_TRUE(strcmp(name, expected[i]) == 0);
    for (int j = i + 1; j < (int)MS_TYPE_COUNT; ++j) {
      MS_ASSERT_TRUE(strcmp(name, msTypeTagName((MsTypeTag)j)) != 0);
    }
  }
  MS_ASSERT_TRUE(strcmp(msTypeTagName((MsTypeTag)MS_TYPE_COUNT), "MS_TYPE_INVALID") == 0);
  MS_ASSERT_TRUE(strcmp(msTypeTagName((MsTypeTag)-1), "MS_TYPE_INVALID") == 0);
}

MS_TEST(object, allocLinksIntoAllObjectsList) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  struct MsObject* a = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  struct MsObject* b = msObjectAlloc(&heap, MS_TYPE_FLOAT, sizeof(struct MsFloat));
  MS_ASSERT_TRUE(a != NULL && b != NULL);
  MS_ASSERT_TRUE(heap.allObjects == b);          // LIFO head insertion
  MS_ASSERT_TRUE(b->header.gcNext == a);
  MS_ASSERT_TRUE(a->header.gcNext == NULL);
  MS_ASSERT_EQ(2, (long long)heap.objectCount);
  MS_ASSERT_TRUE(a->header.type == &heap.types[MS_TYPE_INT]);
  MS_ASSERT_EQ(MS_TYPE_INT, a->header.tag);
  MS_ASSERT_EQ(MS_MARK_WHITE, a->header.markColor);
  MS_ASSERT_TRUE(b->header.type == &heap.types[MS_TYPE_FLOAT]);
  MS_ASSERT_EQ(MS_TYPE_FLOAT, b->header.tag);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, freeUnlinksHeadMidTail) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  struct MsObject* a = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  struct MsObject* b = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  struct MsObject* c = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  struct MsObject* d = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  MS_ASSERT_TRUE(a != NULL && b != NULL && c != NULL && d != NULL);  // list: d -> c -> b -> a
  MS_ASSERT_EQ(4, (long long)heap.objectCount);
  msObjectFree(&heap, d);                        // head
  MS_ASSERT_TRUE(heap.allObjects == c);
  MS_ASSERT_EQ(3, (long long)heap.objectCount);
  msObjectFree(&heap, b);                        // middle
  MS_ASSERT_TRUE(c->header.gcNext == a);
  MS_ASSERT_EQ(2, (long long)heap.objectCount);
  msObjectFree(&heap, a);                        // tail
  MS_ASSERT_TRUE(c->header.gcNext == NULL);
  MS_ASSERT_EQ(1, (long long)heap.objectCount);
  msObjectFree(&heap, c);                        // only node
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, allocOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  msMemSetFailAfter(0);
  struct MsObject* obj = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(obj == NULL);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, heapDestroyNullAndZeroedSafe) {
  msMemResetStats();
  msHeapDestroy(NULL);
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  msHeapDestroy(&heap);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  struct MsObject* obj = msObjectAlloc(&heap, MS_TYPE_NIL, sizeof(struct MsObject));
  MS_ASSERT_TRUE(obj != NULL);
  msHeapDestroy(&heap);
  msHeapDestroy(&heap);                          // double destroy is a no-op
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, allocMinimumSizeIsBaseObject) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  struct MsObject* obj = msObjectAlloc(&heap, MS_TYPE_NIL, sizeof(struct MsObject));
  MS_ASSERT_TRUE(obj != NULL);
  MS_ASSERT_EQ(1, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, freeUnlinkedObjectIsIgnored) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  struct MsObject* a = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  MS_ASSERT_TRUE(a != NULL);
  struct MsObject stranger;
  memset(&stranger, 0, sizeof(stranger));        // never linked
#ifdef NDEBUG
  msObjectFree(&heap, &stranger);                // must not touch list, count, or the stranger
#endif
  MS_ASSERT_TRUE(heap.allObjects == a);
  MS_ASSERT_EQ(1, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, heapInitDestroyNoLeaks) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
  MS_ASSERT_TRUE(heap.allObjects == NULL);       // destroy re-zeroes
  MS_ASSERT_TRUE(heap.internSlots == NULL);
}

MS_TEST(object, heapInitOomLeavesHeapZeroed) {
  // Failure on the intern bucket allocation.
  msMemResetStats();
  msMemSetFailAfter(0);
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_ERROR_OOM, msHeapInit(&heap));
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_TRUE(heap.internSlots == NULL);
  MS_ASSERT_EQ(0, (long long)heap.internCap);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
  // Failure on the first type-name string: buckets freed by rollback.
  msMemResetStats();
  msMemSetFailAfter(1);
  MS_ASSERT_EQ(MS_ERROR_OOM, msHeapInit(&heap));
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_TRUE(heap.internSlots == NULL);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
  // Mid-loop rollback: buckets + 9 names succeed, the 10th name fails; the
  // partially populated name set must be freed and *heap re-zeroed.
  msMemResetStats();
  msMemSetFailAfter(10);
  MS_ASSERT_EQ(MS_ERROR_OOM, msHeapInit(&heap));
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_TRUE(heap.internSlots == NULL);
  MS_ASSERT_EQ(0, (long long)heap.internCap);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, typeObjectsCompleteAndSelfReferential) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    struct MsType* t = msHeapType(&heap, (MsTypeTag)i);
    MS_ASSERT_TRUE(t == &heap.types[i]);
    MS_ASSERT_EQ(MS_TYPE_CLASS, t->header.tag);
    MS_ASSERT_TRUE(t->header.type == &heap.types[MS_TYPE_CLASS]);
    MS_ASSERT_EQ(MS_MARK_BLACK, t->header.markColor);
    MS_ASSERT_TRUE(t->header.gcNext == NULL);    // not on the all-objects list
    MS_ASSERT_TRUE(t->name != NULL);
    MS_ASSERT_EQ(MS_TYPE_STRING, t->name->header.tag);
    for (int j = i + 1; j < (int)MS_TYPE_COUNT; ++j) {
      MS_ASSERT_TRUE(t != &heap.types[j]);       // 20 distinct type objects
    }
  }
  // Name spot checks per 02-types section 1.
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_NIL].name), "NilType") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_BOOL].name), "bool") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_INT].name), "int") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_FLOAT].name), "float") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_STRING].name), "str") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_BYTES].name), "bytes") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_LIST].name), "list") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_TUPLE].name), "tuple") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_DICT].name), "dict") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_SET].name), "set") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_FUNCTION].name), "function") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_CLASS].name), "type") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_INSTANCE].name), "instance") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_MODULE].name), "module") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_CHANNEL].name), "chan") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_COROUTINE].name), "coroutine") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_ITERATOR].name), "iterator") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_BOUND_METHOD].name), "bound method") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_C_FUNCTION].name), "cfunction") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_C_TYPE].name), "ctype") == 0);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, permanentObjectsNotLinked) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  // Exactly the 20 interned type names are linked heap objects after init.
  MS_ASSERT_EQ(20, (long long)heap.objectCount);
  MS_ASSERT_EQ(20, (long long)heap.internLen);
  // Singletons and cache entries: black, unlinked, do not change objectCount.
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.nilObject.header.markColor);
  MS_ASSERT_TRUE(heap.nilObject.header.gcNext == NULL);
  MS_ASSERT_EQ(MS_TYPE_NIL, heap.nilObject.header.tag);
  MS_ASSERT_TRUE(heap.nilObject.header.type == &heap.types[MS_TYPE_NIL]);
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.trueObject.header.markColor);
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.falseObject.header.markColor);
  MS_ASSERT_TRUE(heap.trueObject.header.gcNext == NULL);
  MS_ASSERT_TRUE(heap.trueObject.value);
  MS_ASSERT_TRUE(!heap.falseObject.value);
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.intCache[0].header.markColor);
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.intCache[MS_INT_CACHE_SIZE - 1].header.markColor);
  MS_ASSERT_EQ(-256, (long long)heap.intCache[0].value);
  MS_ASSERT_EQ(4095, (long long)heap.intCache[MS_INT_CACHE_SIZE - 1].value);
  MS_ASSERT_TRUE(heap.intCache[0].header.gcNext == NULL);
  MS_ASSERT_TRUE(heap.intCache[0].header.type == &heap.types[MS_TYPE_INT]);
  MS_ASSERT_EQ(20, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

static uint64_t msTestDoubleBits(double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

MS_TEST(object, singletonAccessorsIdentity) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  MS_ASSERT_TRUE(msHeapNil(&heap) == msHeapNil(&heap));
  MS_ASSERT_TRUE(msHeapNil(&heap) == &heap.nilObject);
  MS_ASSERT_TRUE(msHeapBool(&heap, true) == msHeapBool(&heap, true));
  MS_ASSERT_TRUE(msHeapBool(&heap, false) == msHeapBool(&heap, false));
  MS_ASSERT_TRUE(msHeapBool(&heap, true) != msHeapBool(&heap, false));
  MS_ASSERT_TRUE(msHeapBool(&heap, true) != msHeapNil(&heap));
  size_t countBefore = heap.objectCount;
  msHeapNil(&heap);
  msHeapBool(&heap, true);
  MS_ASSERT_EQ((long long)countBefore, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, intCacheIdentityAtBoundaries) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  size_t countBefore = heap.objectCount;
  int64_t cached[] = {-256, 0, 4095};
  for (size_t i = 0; i < sizeof(cached) / sizeof(cached[0]); ++i) {
    struct MsInt* a = msIntNew(&heap, cached[i]);
    struct MsInt* b = msIntNew(&heap, cached[i]);
    MS_ASSERT_TRUE(a == b);                      // resident: same pointer
    MS_ASSERT_EQ(cached[i], (long long)a->value);
    MS_ASSERT_EQ(MS_TYPE_INT, a->header.tag);
    MS_ASSERT_EQ(MS_MARK_BLACK, a->header.markColor);
  }
  MS_ASSERT_EQ((long long)countBefore, (long long)heap.objectCount);  // cache hits never allocate
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, intUncachedAllocates) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  int64_t uncached[] = {-257, 4096, INT64_MIN, INT64_MAX};
  size_t countBefore = heap.objectCount;
  for (size_t i = 0; i < sizeof(uncached) / sizeof(uncached[0]); ++i) {
    struct MsInt* a = msIntNew(&heap, uncached[i]);
    struct MsInt* b = msIntNew(&heap, uncached[i]);
    MS_ASSERT_TRUE(a != NULL && b != NULL);
    MS_ASSERT_TRUE(a != b);                      // fresh allocation each time
    MS_ASSERT_EQ(uncached[i], (long long)a->value);
    MS_ASSERT_EQ(MS_MARK_WHITE, a->header.markColor);
  }
  MS_ASSERT_EQ((long long)(countBefore + 8), (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, intAccessorsRoundtrip) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsInt* cached = msIntNew(&heap, 42);
  struct MsInt* fresh = msIntNew(&heap, 5000);
  MS_ASSERT_TRUE(fresh != NULL);
  MS_ASSERT_TRUE(msIntIsWord((struct MsObject*)cached));
  MS_ASSERT_TRUE(msIntIsWord((struct MsObject*)fresh));
  MS_ASSERT_EQ(42, (long long)msIntWordValue((struct MsObject*)cached));
  MS_ASSERT_EQ(5000, (long long)msIntWordValue((struct MsObject*)fresh));
  MS_ASSERT_EQ(MS_TYPE_INT, msObjectTag((struct MsObject*)cached));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, intNewOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  size_t countBefore = heap.objectCount;
  msMemSetFailAfter(0);
  struct MsInt* obj = msIntNew(&heap, 5000);
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(obj == NULL);
  MS_ASSERT_EQ((long long)countBefore, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, floatRoundtripPreservesBits) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  double values[] = {0.0, -0.0, 3.14, -2.5e300, INFINITY, -INFINITY, NAN};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    struct MsFloat* f = msFloatNew(&heap, values[i]);
    MS_ASSERT_TRUE(f != NULL);
    MS_ASSERT_EQ(MS_TYPE_FLOAT, f->header.tag);
    MS_ASSERT_TRUE(f->header.type == &heap.types[MS_TYPE_FLOAT]);
    MS_ASSERT_EQ((long long)msTestDoubleBits(values[i]),
                 (long long)msTestDoubleBits(msFloatValue((struct MsObject*)f)));
  }
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, boolValueAccessor) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  MS_ASSERT_TRUE(msBoolValue(msHeapBool(&heap, true)));
  MS_ASSERT_TRUE(!msBoolValue(msHeapBool(&heap, false)));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, floatNewOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  size_t countBefore = heap.objectCount;
  msMemSetFailAfter(0);
  struct MsFloat* obj = msFloatNew(&heap, 1.5);
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(obj == NULL);
  MS_ASSERT_EQ((long long)countBefore, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, valueEqualsScalars) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* nilA = msHeapNil(&heap);
  MS_ASSERT_TRUE(msObjectValueEquals(nilA, nilA));
  MS_ASSERT_TRUE(msObjectValueEquals(msHeapBool(&heap, true), msHeapBool(&heap, true)));
  MS_ASSERT_TRUE(!msObjectValueEquals(msHeapBool(&heap, true), msHeapBool(&heap, false)));
  struct MsInt* i42a = msIntNew(&heap, 5000);    // uncached: distinct objects, equal value
  struct MsInt* i42b = msIntNew(&heap, 5000);
  struct MsInt* i43 = msIntNew(&heap, 5001);
  struct MsFloat* f42 = msFloatNew(&heap, 5000.0);
  MS_ASSERT_TRUE(i42a != i42b);
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)i42a, (struct MsObject*)i42b));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)i42a, (struct MsObject*)i43));
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)i42a, (struct MsObject*)f42));
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)f42, (struct MsObject*)i42a));
  struct MsFloat* fHalf = msFloatNew(&heap, 5000.5);
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)i42a, (struct MsObject*)fHalf));
  // bool is not part of the numeric cross-tag rule (02-types section 6).
  MS_ASSERT_TRUE(!msObjectValueEquals(msHeapBool(&heap, true), (struct MsObject*)msIntNew(&heap, 1)));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)msIntNew(&heap, 1), msHeapBool(&heap, true)));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, valueEqualsNaNAndIdentity) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsFloat* nanA = msFloatNew(&heap, NAN);
  struct MsFloat* nanB = msFloatNew(&heap, NAN);
  MS_ASSERT_TRUE(nanA != nanB);
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)nanA, (struct MsObject*)nanB));
  // NaN is unequal even to itself (02-types section 3.2); the identity fast
  // path must not override that, but still works for ordinary floats.
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)nanA, (struct MsObject*)nanA));
  struct MsFloat* ordinary = msFloatNew(&heap, 1.5);
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)ordinary, (struct MsObject*)ordinary));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, valueEqualsStringAndCrossTag) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  char buf[41];
  memset(buf, 'z', 40);
  buf[40] = '\0';
  struct MsString* s1 = msStringNew(&heap, buf, 40);   // long: not interned
  struct MsString* s2 = msStringNew(&heap, buf, 40);
  MS_ASSERT_TRUE(s1 != s2);
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)s1, (struct MsObject*)s2));
  struct MsString* other = msStringFromCStr(&heap, "short-str");
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)s1, (struct MsObject*)other));
  struct MsInt* i1 = msIntNew(&heap, 1);
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)i1, (struct MsObject*)other));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)other, (struct MsObject*)i1));
  MS_ASSERT_TRUE(!msObjectValueEquals(NULL, (struct MsObject*)i1));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)i1, NULL));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, hashConsistentWithEquals) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsInt* i42 = msIntNew(&heap, 5000);
  struct MsFloat* f42 = msFloatNew(&heap, 5000.0);
  struct MsFloat* fNeg7 = msFloatNew(&heap, -7.0);
  struct MsInt* iNeg7 = msIntNew(&heap, -7);
  // Equal values must hash equal (dict keys, task 16).
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)i42),
               (long long)msObjectHash((struct MsObject*)f42));
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)iNeg7),
               (long long)msObjectHash((struct MsObject*)fNeg7));
  char buf[41];
  memset(buf, 'q', 40);
  buf[40] = '\0';
  struct MsString* s1 = msStringNew(&heap, buf, 40);
  struct MsString* s2 = msStringNew(&heap, buf, 40);
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)s1),
               (long long)msObjectHash((struct MsObject*)s2));
  MS_ASSERT_EQ((long long)msObjectHash(msHeapBool(&heap, true)),
               (long long)msObjectHash(msHeapBool(&heap, true)));
  MS_ASSERT_EQ((long long)msObjectHash(msHeapNil(&heap)),
               (long long)msObjectHash(msHeapNil(&heap)));
  // Non-integral floats hash by bits; determinism check only.
  struct MsFloat* fHalfA = msFloatNew(&heap, 0.5);
  struct MsFloat* fHalfB = msFloatNew(&heap, 0.5);
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)fHalfA),
               (long long)msObjectHash((struct MsObject*)fHalfB));
  // -0.0 == 0 and the integral-float hash path unifies their bit patterns.
  struct MsInt* iZero = msIntNew(&heap, 0);
  struct MsFloat* fNegZero2 = msFloatNew(&heap, -0.0);
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)iZero, (struct MsObject*)fNegZero2));
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)iZero),
               (long long)msObjectHash((struct MsObject*)fNegZero2));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, valueEqualsLargeIntPrecision) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  // 2^53+1 rounds to 2^53 as double: must be UNEQUAL (exact int64 compare),
  // or equals/hash would diverge and dict numeric keys (task 16) would break.
  struct MsInt* big = msIntNew(&heap, 9007199254740993LL);        // 2^53 + 1
  struct MsFloat* bigF = msFloatNew(&heap, 9007199254740992.0);   // 2^53
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)big, (struct MsObject*)bigF));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)bigF, (struct MsObject*)big));
  // Exactly representable large pair: equal, and hashes must agree.
  struct MsInt* e60 = msIntNew(&heap, 1152921504606846976LL);     // 2^60
  struct MsFloat* e60F = msFloatNew(&heap, 1152921504606846976.0);
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)e60, (struct MsObject*)e60F));
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)e60),
               (long long)msObjectHash((struct MsObject*)e60F));
  // Integral float outside int64 range never equals any int.
  struct MsFloat* huge = msFloatNew(&heap, 9223372036854775808.0); // 2^63
  struct MsInt* iMax = msIntNew(&heap, INT64_MAX);
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)huge, (struct MsObject*)iMax));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)iMax, (struct MsObject*)huge));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, truthinessTable) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  MS_ASSERT_TRUE(!msObjectIsTruthy(msHeapNil(&heap)));
  MS_ASSERT_TRUE(!msObjectIsTruthy(msHeapBool(&heap, false)));
  MS_ASSERT_TRUE(msObjectIsTruthy(msHeapBool(&heap, true)));
  MS_ASSERT_TRUE(!msObjectIsTruthy((struct MsObject*)msIntNew(&heap, 0)));
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)msIntNew(&heap, 1)));
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)msIntNew(&heap, -1)));
  struct MsFloat* fZero = msFloatNew(&heap, 0.0);
  struct MsFloat* fNegZero = msFloatNew(&heap, -0.0);
  struct MsFloat* fHalf = msFloatNew(&heap, 0.5);
  MS_ASSERT_TRUE(!msObjectIsTruthy((struct MsObject*)fZero));
  MS_ASSERT_TRUE(!msObjectIsTruthy((struct MsObject*)fNegZero));
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)fHalf));
  MS_ASSERT_TRUE(!msObjectIsTruthy((struct MsObject*)msStringFromCStr(&heap, "")));
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)msStringFromCStr(&heap, "x")));
  // Heap objects of other tags are truthy (type object as the specimen).
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)msHeapType(&heap, MS_TYPE_CLASS)));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

static const MsTestCase msTestCases[] = {
  {"object.typeTagCountIs20", testobjecttypeTagCountIs20},
  {"object.typeTagNamesCompleteDistinctOrdered", testobjecttypeTagNamesCompleteDistinctOrdered},
  {"object.allocLinksIntoAllObjectsList", testobjectallocLinksIntoAllObjectsList},
  {"object.freeUnlinksHeadMidTail", testobjectfreeUnlinksHeadMidTail},
  {"object.freeUnlinkedObjectIsIgnored", testobjectfreeUnlinkedObjectIsIgnored},
  {"object.allocOomReturnsNull", testobjectallocOomReturnsNull},
  {"object.heapDestroyNullAndZeroedSafe", testobjectheapDestroyNullAndZeroedSafe},
  {"object.allocMinimumSizeIsBaseObject", testobjectallocMinimumSizeIsBaseObject},
  {"object.heapInitDestroyNoLeaks", testobjectheapInitDestroyNoLeaks},
  {"object.heapInitOomLeavesHeapZeroed", testobjectheapInitOomLeavesHeapZeroed},
  {"object.typeObjectsCompleteAndSelfReferential", testobjecttypeObjectsCompleteAndSelfReferential},
  {"object.permanentObjectsNotLinked", testobjectpermanentObjectsNotLinked},
  {"object.singletonAccessorsIdentity", testobjectsingletonAccessorsIdentity},
  {"object.intCacheIdentityAtBoundaries", testobjectintCacheIdentityAtBoundaries},
  {"object.intUncachedAllocates", testobjectintUncachedAllocates},
  {"object.intAccessorsRoundtrip", testobjectintAccessorsRoundtrip},
  {"object.intNewOomReturnsNull", testobjectintNewOomReturnsNull},
  {"object.floatRoundtripPreservesBits", testobjectfloatRoundtripPreservesBits},
  {"object.boolValueAccessor", testobjectboolValueAccessor},
  {"object.floatNewOomReturnsNull", testobjectfloatNewOomReturnsNull},
  {"object.valueEqualsScalars", testobjectvalueEqualsScalars},
  {"object.valueEqualsNaNAndIdentity", testobjectvalueEqualsNaNAndIdentity},
  {"object.valueEqualsStringAndCrossTag", testobjectvalueEqualsStringAndCrossTag},
  {"object.hashConsistentWithEquals", testobjecthashConsistentWithEquals},
  {"object.valueEqualsLargeIntPrecision", testobjectvalueEqualsLargeIntPrecision},
  {"object.truthinessTable", testobjecttruthinessTable},
};

MS_TEST_MAIN(msTestCases)
