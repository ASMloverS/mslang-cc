#include "object/ms_object.h"

#include <string.h>

#include "core/ms_memory.h"
#include "ms_test.h"

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
  MS_ASSERT_TRUE(d != NULL);                     // list: d -> c -> b -> a
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
}

MS_TEST(object, heapDestroyNullAndZeroedSafe) {
  msHeapDestroy(NULL);
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  msHeapDestroy(&heap);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
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

static const MsTestCase msTestCases[] = {
  {"object.typeTagCountIs20", testobjecttypeTagCountIs20},
  {"object.typeTagNamesCompleteDistinctOrdered", testobjecttypeTagNamesCompleteDistinctOrdered},
  {"object.allocLinksIntoAllObjectsList", testobjectallocLinksIntoAllObjectsList},
  {"object.freeUnlinksHeadMidTail", testobjectfreeUnlinksHeadMidTail},
  {"object.allocOomReturnsNull", testobjectallocOomReturnsNull},
  {"object.heapDestroyNullAndZeroedSafe", testobjectheapDestroyNullAndZeroedSafe},
  {"object.allocMinimumSizeIsBaseObject", testobjectallocMinimumSizeIsBaseObject},
};

MS_TEST_MAIN(msTestCases)
