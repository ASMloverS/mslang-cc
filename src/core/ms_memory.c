#include "core/ms_memory.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdlib.h>

#include "core/ms_common.h"

struct MsMemHeader {
  size_t size;  // requested payload size in bytes
};

// Header size rounded up so the payload stays aligned to max_align_t.
#define MS_MEM_HEADER_SIZE \
  ((sizeof(struct MsMemHeader) + alignof(max_align_t) - 1) / alignof(max_align_t) * alignof(max_align_t))

// Process-level diagnostic counters, accessed only through the functions in
// this file (sanctioned exception to the no-mutable-globals rule; see task
// 02 design notes). Single-threaded semantics for v0.1.
static struct MsMemStats msMemStats;
static int64_t msMemFailAfter = -1;

// Returns true when the allocation must fail due to fault injection.
static bool consumeFailAfter(void) {
  if (msMemFailAfter < 0) {
    return false;
  }
  if (msMemFailAfter == 0) {
    return true;
  }
  --msMemFailAfter;
  return false;
}

static void accountAlloc(size_t size) {
  ++msMemStats.allocCount;
  ++msMemStats.liveBlocks;
  msMemStats.currentBytes += size;
  if (msMemStats.currentBytes > msMemStats.peakBytes) {
    msMemStats.peakBytes = msMemStats.currentBytes;
  }
  msMemStats.totalAllocatedBytes += size;
}

void* msAlloc(size_t size) {
  MS_ASSERT(size > 0);
  if (size == 0 || size > SIZE_MAX - MS_MEM_HEADER_SIZE) {
    return NULL;
  }
  if (consumeFailAfter()) {
    return NULL;
  }
  struct MsMemHeader* header = (struct MsMemHeader*)malloc(MS_MEM_HEADER_SIZE + size);
  if (header == NULL) {
    return NULL;
  }
  header->size = size;
  accountAlloc(size);
  return (void*)((char*)header + MS_MEM_HEADER_SIZE);
}

void* msRealloc(void* ptr, size_t newSize) {
  if (ptr == NULL) {
    return msAlloc(newSize);
  }
  MS_ASSERT(newSize > 0);
  if (newSize == 0 || newSize > SIZE_MAX - MS_MEM_HEADER_SIZE) {
    return NULL;
  }
  struct MsMemHeader* oldHeader = (struct MsMemHeader*)((char*)ptr - MS_MEM_HEADER_SIZE);
  size_t oldSize = oldHeader->size;
  if (consumeFailAfter()) {
    return NULL;
  }
  struct MsMemHeader* newHeader = (struct MsMemHeader*)realloc(oldHeader, MS_MEM_HEADER_SIZE + newSize);
  if (newHeader == NULL) {
    return NULL;
  }
  newHeader->size = newSize;
  ++msMemStats.reallocCount;
  msMemStats.currentBytes += newSize - oldSize;  // unsigned wraparound yields the exact delta
  if (msMemStats.currentBytes > msMemStats.peakBytes) {
    msMemStats.peakBytes = msMemStats.currentBytes;
  }
  if (newSize > oldSize) {
    msMemStats.totalAllocatedBytes += newSize - oldSize;
  }
  return (void*)((char*)newHeader + MS_MEM_HEADER_SIZE);
}

void msFree(void* ptr) {
  if (ptr == NULL) {
    return;
  }
  struct MsMemHeader* header = (struct MsMemHeader*)((char*)ptr - MS_MEM_HEADER_SIZE);
  ++msMemStats.freeCount;
  --msMemStats.liveBlocks;
  msMemStats.currentBytes -= header->size;
  free(header);
}

void msMemGetStats(struct MsMemStats* out) {
  MS_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  *out = msMemStats;
}

void msMemResetStats(void) {
  msMemStats = (struct MsMemStats){0};
}

void msMemSetFailAfter(int64_t n) {
  msMemFailAfter = n;
}
