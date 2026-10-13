#include "object/ms_object.h"

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
};

MS_TEST_MAIN(msTestCases)
