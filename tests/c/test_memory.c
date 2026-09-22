#include "core/ms_memory.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ms_test.h"

// Every test resets the counters for isolation and asserts liveBlocks == 0
// && currentBytes == 0 at the end, so a leak fails the suite.

static void assertNoLeak(void) {
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  MS_ASSERT_EQ(0, stats.currentBytes);
}

MS_TEST(Memory, AllocWritableAndAccounted) {
  msMemResetStats();
  char* p = (char*)msAlloc(64);
  MS_ASSERT_TRUE(p != NULL);
  memset(p, 0xAB, 64);
  MS_ASSERT_EQ(0xAB, (unsigned char)p[63]);

  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(1, stats.allocCount);
  MS_ASSERT_EQ(1, stats.liveBlocks);
  MS_ASSERT_EQ(64, stats.currentBytes);
  MS_ASSERT_EQ(64, stats.peakBytes);
  MS_ASSERT_EQ(64, stats.totalAllocatedBytes);

  msFree(p);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(1, stats.freeCount);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  MS_ASSERT_EQ(0, stats.currentBytes);
  assertNoLeak();
}

MS_TEST(Memory, AllocAlignsToMaxAlign) {
  msMemResetStats();
  void* p = msAlloc(1);
  MS_ASSERT_TRUE(p != NULL);
  MS_ASSERT_EQ(0, (uintptr_t)p % alignof(max_align_t));
  msFree(p);
  assertNoLeak();
}

MS_TEST(Memory, AllocationsDoNotOverlap) {
  msMemResetStats();
  char* a = (char*)msAlloc(16);
  char* b = (char*)msAlloc(16);
  MS_ASSERT_TRUE(a != NULL && b != NULL);
  memset(a, 0x11, 16);
  memset(b, 0x22, 16);
  MS_ASSERT_TRUE(a != b);
  MS_ASSERT_EQ(0x11, (unsigned char)a[0]);
  MS_ASSERT_EQ(0x11, (unsigned char)a[15]);
  MS_ASSERT_EQ(0x22, (unsigned char)b[0]);
  msFree(a);
  msFree(b);
  assertNoLeak();
}

MS_TEST(Memory, PeakTracksHighWaterMark) {
  msMemResetStats();
  void* a = msAlloc(100);
  void* b = msAlloc(50);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(150, stats.currentBytes);
  MS_ASSERT_EQ(150, stats.peakBytes);
  MS_ASSERT_EQ(150, stats.totalAllocatedBytes);
  msFree(a);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(50, stats.currentBytes);
  MS_ASSERT_EQ(150, stats.peakBytes);
  msFree(b);
  assertNoLeak();
}

MS_TEST(Memory, ReallocPreservesDataAndAccountsDelta) {
  msMemResetStats();
  char* p = (char*)msAlloc(16);
  MS_ASSERT_TRUE(p != NULL);
  for (int i = 0; i < 16; ++i) {
    p[i] = (char)i;
  }
  char* grown = (char*)msRealloc(p, 64);
  MS_ASSERT_TRUE(grown != NULL);
  bool preserved = true;
  for (int i = 0; i < 16; ++i) {
    if (grown[i] != (char)i) {
      preserved = false;
    }
  }
  MS_ASSERT_TRUE(preserved);

  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(1, stats.reallocCount);
  MS_ASSERT_EQ(1, stats.liveBlocks);
  MS_ASSERT_EQ(64, stats.currentBytes);
  MS_ASSERT_EQ(64, stats.peakBytes);
  MS_ASSERT_EQ(64, stats.totalAllocatedBytes);  // 16 alloc + 48 growth

  char* shrunk = (char*)msRealloc(grown, 8);
  MS_ASSERT_TRUE(shrunk != NULL);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(8, stats.currentBytes);
  MS_ASSERT_EQ(64, stats.peakBytes);
  MS_ASSERT_EQ(64, stats.totalAllocatedBytes);  // shrink keeps cumulative
  msFree(shrunk);
  assertNoLeak();
}

MS_TEST(Memory, ReallocNullActsAsAlloc) {
  msMemResetStats();
  void* p = msRealloc(NULL, 32);
  MS_ASSERT_TRUE(p != NULL);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(1, stats.allocCount);
  MS_ASSERT_EQ(0, stats.reallocCount);
  MS_ASSERT_EQ(32, stats.currentBytes);
  msFree(p);
  assertNoLeak();
}

MS_TEST(Memory, FreeNullIsNoOp) {
  msMemResetStats();
  msFree(NULL);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.freeCount);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  assertNoLeak();
}

MS_TEST(Memory, FailAfterZeroFailsImmediately) {
  msMemResetStats();
  msMemSetFailAfter(0);
  MS_ASSERT_TRUE(msAlloc(8) == NULL);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.allocCount);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  MS_ASSERT_EQ(0, stats.currentBytes);
  msMemSetFailAfter(-1);
  void* p = msAlloc(8);
  MS_ASSERT_TRUE(p != NULL);
  msFree(p);
  assertNoLeak();
}

MS_TEST(Memory, FailAfterCountsDownSuccesses) {
  msMemResetStats();
  msMemSetFailAfter(2);
  void* a = msAlloc(8);
  void* b = msAlloc(8);
  void* c = msAlloc(8);
  MS_ASSERT_TRUE(a != NULL);
  MS_ASSERT_TRUE(b != NULL);
  MS_ASSERT_TRUE(c == NULL);
  msMemSetFailAfter(-1);
  msFree(a);
  msFree(b);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(2, stats.allocCount);
  assertNoLeak();
}

MS_TEST(Memory, ReallocFailureKeepsOriginalBlock) {
  msMemResetStats();
  char* p = (char*)msAlloc(32);
  MS_ASSERT_TRUE(p != NULL);
  p[0] = 'x';
  msMemSetFailAfter(0);
  MS_ASSERT_TRUE(msRealloc(p, 64) == NULL);
  msMemSetFailAfter(-1);
  MS_ASSERT_EQ('x', p[0]);  // original block untouched
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.reallocCount);
  MS_ASSERT_EQ(1, stats.liveBlocks);
  MS_ASSERT_EQ(32, stats.currentBytes);
  msFree(p);
  assertNoLeak();
}

MS_TEST(Memory, AllocOverflowFailsCleanly) {
  msMemResetStats();
  MS_ASSERT_TRUE(msAlloc(SIZE_MAX) == NULL);
  MS_ASSERT_TRUE(msAlloc(SIZE_MAX - 4) == NULL);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.allocCount);
  assertNoLeak();
}

static const MsTestCase msTestCases[] = {
    {"Memory.AllocWritableAndAccounted", testMemoryAllocWritableAndAccounted},
    {"Memory.AllocAlignsToMaxAlign", testMemoryAllocAlignsToMaxAlign},
    {"Memory.AllocationsDoNotOverlap", testMemoryAllocationsDoNotOverlap},
    {"Memory.PeakTracksHighWaterMark", testMemoryPeakTracksHighWaterMark},
    {"Memory.ReallocPreservesDataAndAccountsDelta", testMemoryReallocPreservesDataAndAccountsDelta},
    {"Memory.ReallocNullActsAsAlloc", testMemoryReallocNullActsAsAlloc},
    {"Memory.FreeNullIsNoOp", testMemoryFreeNullIsNoOp},
    {"Memory.FailAfterZeroFailsImmediately", testMemoryFailAfterZeroFailsImmediately},
    {"Memory.FailAfterCountsDownSuccesses", testMemoryFailAfterCountsDownSuccesses},
    {"Memory.ReallocFailureKeepsOriginalBlock", testMemoryReallocFailureKeepsOriginalBlock},
    {"Memory.AllocOverflowFailsCleanly", testMemoryAllocOverflowFailsCleanly},
};

MS_TEST_MAIN(msTestCases)
