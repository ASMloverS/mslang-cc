#ifndef MSLANG_SRC_CORE_MS_MEMORY_H_
#define MSLANG_SRC_CORE_MS_MEMORY_H_

#include <stddef.h>
#include <stdint.h>

// MSVC C mode (-std:c11) does not provide max_align_t in <stddef.h>; define
// an equivalent whose alignment matches the strictest fundamental type.
#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)
typedef union {
  long long msAlignLl;
  long double msAlignLd;
  double msAlignD;
  void* msAlignP;
  void (*msAlignFp)(void);
} max_align_t;
#endif

// Allocates size bytes. Returns NULL on OOM or size == 0 (size == 0 is a
// caller bug; MS_ASSERT in debug). Never aborts. Content is uninitialized.
void* msAlloc(size_t size);

// Resizes ptr to newSize bytes. ptr may be NULL (equivalent to msAlloc).
// On failure returns NULL and leaves the original block untouched.
// newSize must be > 0; use msFree to release.
void* msRealloc(void* ptr, size_t newSize);

// Frees a block from msAlloc/msRealloc. NULL is a no-op.
void msFree(void* ptr);

// Allocation statistics. totalAllocatedBytes counts payload bytes ever
// allocated, including the growth delta of growing msRealloc calls.
struct MsMemStats {
  uint64_t allocCount;
  uint64_t reallocCount;
  uint64_t freeCount;
  size_t liveBlocks;
  size_t currentBytes;
  size_t peakBytes;
  size_t totalAllocatedBytes;
};

// Copies a snapshot of the counters into out.
void msMemGetStats(struct MsMemStats* out);

// Zeroes the counters. Does NOT free live blocks; use only for test
// isolation, never to hide leaks.
void msMemResetStats(void);

// After n more successful allocations, msAlloc/msRealloc start failing.
// n < 0 disables injection. Test/diagnostic use only.
void msMemSetFailAfter(int64_t n);

#endif  // MSLANG_SRC_CORE_MS_MEMORY_H_
