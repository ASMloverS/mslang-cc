#include "object/ms_string.h"

#include <stdio.h>
#include <string.h>

#include "core/ms_memory.h"
#include "ms_test.h"

// Bootstraps just the intern table (same logic as msHeapInit); the object
// model tasks land heap init in Task 4, and these tests must not depend on it.
static void msTestHeapBootstrap(struct MsHeap* heap) {
  memset(heap, 0, sizeof(*heap));
  heap->internCap = MS_STRING_INTERN_INITIAL_CAP;
  heap->internSlots =
      (struct MsString**)msAlloc(heap->internCap * sizeof(*heap->internSlots));
  MS_ASSERT_TRUE(heap->internSlots != NULL);
  memset(heap->internSlots, 0, heap->internCap * sizeof(*heap->internSlots));
}

static void msTestAssertNoLeaks(void) {
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(string, newRoundtrip) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* s = msStringNew(&heap, "hello", 5);
  MS_ASSERT_TRUE(s != NULL);
  MS_ASSERT_EQ(5, (long long)msStringLen(s));
  MS_ASSERT_TRUE(strcmp(msStringData(s), "hello") == 0);
  MS_ASSERT_EQ('\0', msStringData(s)[5]);
  MS_ASSERT_EQ(MS_TYPE_STRING, s->header.tag);
  MS_ASSERT_TRUE(s->header.type == &heap.types[MS_TYPE_STRING]);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, fromCStrMatchesNew) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* a = msStringFromCStr(&heap, "mslang");
  struct MsString* b = msStringNew(&heap, "mslang", 6);
  MS_ASSERT_TRUE(a == b);                        // same content, same interned pointer
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, emptyStringInterned) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* a = msStringNew(&heap, NULL, 0);   // NULL data + 0 len == empty
  struct MsString* b = msStringFromCStr(&heap, "");
  MS_ASSERT_TRUE(a == b);
  MS_ASSERT_EQ(0, (long long)msStringLen(a));
  MS_ASSERT_EQ('\0', msStringData(a)[0]);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, fnv1aGoldenVectors) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* hello = msStringFromCStr(&heap, "hello");
  struct MsString* empty = msStringFromCStr(&heap, "");
  MS_ASSERT_EQ(0x4f9f2cab, (long long)msStringHash(hello));   // FNV-1a-32("hello")
  MS_ASSERT_EQ(2166136261, (long long)msStringHash(empty));   // offset basis
  struct MsString* hello2 = msStringNew(&heap, "hello", 5);
  MS_ASSERT_EQ((long long)msStringHash(hello), (long long)msStringHash(hello2));
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, shortStringsInterned) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* a = msStringNew(&heap, "short", 5);
  struct MsString* b = msStringNew(&heap, "short", 5);
  MS_ASSERT_TRUE(a == b);
  // Multibyte UTF-8 content ("e" + combining sequences are fine raw bytes here).
  struct MsString* u1 = msStringNew(&heap, "\xc3\xa9\xc3\xa8", 4);
  struct MsString* u2 = msStringNew(&heap, "\xc3\xa9\xc3\xa8", 4);
  MS_ASSERT_TRUE(u1 == u2);
  MS_ASSERT_EQ(4, (long long)msStringLen(u1));
  // Embedded NUL is content, addressed by length.
  struct MsString* n1 = msStringNew(&heap, "a\0b", 3);
  struct MsString* n2 = msStringNew(&heap, "a\0b", 3);
  MS_ASSERT_TRUE(n1 == n2);
  MS_ASSERT_EQ(3, (long long)msStringLen(n1));
  MS_ASSERT_EQ('\0', msStringData(n1)[1]);
  MS_ASSERT_TRUE(!msStringEquals(n1, msStringFromCStr(&heap, "a")));
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, longStringsNotInterned) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  char buf33[34];
  memset(buf33, 'x', 33);
  buf33[33] = '\0';
  struct MsString* a = msStringNew(&heap, buf33, 33);
  struct MsString* b = msStringNew(&heap, buf33, 33);
  MS_ASSERT_TRUE(a != b);                        // 33 > 32: fresh allocation each time
  MS_ASSERT_TRUE(msStringEquals(a, b));
  MS_ASSERT_EQ(2, (long long)heap.objectCount);
  MS_ASSERT_EQ(0, (long long)heap.internLen);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, shortMaxBoundary) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  char buf32[33];
  memset(buf32, 'y', 32);
  buf32[32] = '\0';
  struct MsString* a = msStringNew(&heap, buf32, 32);
  struct MsString* b = msStringNew(&heap, buf32, 32);
  MS_ASSERT_TRUE(a == b);                        // exactly 32: still interned
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, prefixNotEqual) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* ab = msStringFromCStr(&heap, "ab");
  struct MsString* abc = msStringFromCStr(&heap, "abc");
  MS_ASSERT_TRUE(!msStringEquals(ab, abc));
  MS_ASSERT_TRUE(!msStringEquals(abc, ab));
  MS_ASSERT_TRUE(msStringEquals(ab, ab));
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, internGrowthKeepsHits) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* ptrs[200];
  char buf[16];
  for (int i = 0; i < 200; ++i) {
    snprintf(buf, sizeof(buf), "k%d", i);
    ptrs[i] = msStringFromCStr(&heap, buf);
    MS_ASSERT_TRUE(ptrs[i] != NULL);
  }
  // Load factor 0.75 with doubling: 64 -> 128 -> 256 -> 512 for 200 entries.
  MS_ASSERT_EQ(200, (long long)heap.internLen);
  MS_ASSERT_EQ(512, (long long)heap.internCap);
  for (int i = 0; i < 200; ++i) {
    snprintf(buf, sizeof(buf), "k%d", i);
    MS_ASSERT_TRUE(msStringFromCStr(&heap, buf) == ptrs[i]);   // still hits after rehash
  }
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, newOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  msMemSetFailAfter(0);
  struct MsString* s = msStringFromCStr(&heap, "oom-target");
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(s == NULL);
  MS_ASSERT_EQ(0, (long long)heap.internLen);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, internGrowOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  char buf[16];
  for (int i = 0; i < 48; ++i) {                 // fill to the 0.75 load edge of 64 buckets
    snprintf(buf, sizeof(buf), "s%d", i);
    MS_ASSERT_TRUE(msStringFromCStr(&heap, buf) != NULL);
  }
  MS_ASSERT_EQ(64, (long long)heap.internCap);
  msMemSetFailAfter(0);                          // the rehash bucket allocation fails
  struct MsString* s = msStringFromCStr(&heap, "s48");
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(s == NULL);
  MS_ASSERT_EQ(48, (long long)heap.internLen);
  MS_ASSERT_EQ(64, (long long)heap.internCap);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

static const MsTestCase msTestCases[] = {
  {"string.newRoundtrip", teststringnewRoundtrip},
  {"string.fromCStrMatchesNew", teststringfromCStrMatchesNew},
  {"string.emptyStringInterned", teststringemptyStringInterned},
  {"string.fnv1aGoldenVectors", teststringfnv1aGoldenVectors},
  {"string.shortStringsInterned", teststringshortStringsInterned},
  {"string.longStringsNotInterned", teststringlongStringsNotInterned},
  {"string.shortMaxBoundary", teststringshortMaxBoundary},
  {"string.prefixNotEqual", teststringprefixNotEqual},
  {"string.internGrowthKeepsHits", teststringinternGrowthKeepsHits},
  {"string.newOomReturnsNull", teststringnewOomReturnsNull},
  {"string.internGrowOomReturnsNull", teststringinternGrowOomReturnsNull},
};

MS_TEST_MAIN(msTestCases)
