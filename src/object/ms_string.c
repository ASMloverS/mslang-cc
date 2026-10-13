#include "object/ms_string.h"

#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

// FNV-1a, 32-bit: offset basis 2166136261, prime 16777619.
static uint32_t msStringFnv1a(const char* data, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= (uint8_t)data[i];
    hash *= 16777619u;
  }
  return hash;
}

// Open-addressing intern table: linear probing over the cached hash, load
// factor 0.75 with doubling rehash. Buckets hold MsString*; NULL means empty.
static struct MsString* msStringInternFind(const struct MsHeap* heap, const char* data,
                                           size_t len, uint32_t hash) {
  MS_ASSERT(heap->internSlots != NULL);
  MS_ASSERT(heap->internCap > 0);
  size_t mask = heap->internCap - 1;
  size_t idx = hash & mask;
  while (heap->internSlots[idx] != NULL) {
    struct MsString* s = heap->internSlots[idx];
    if (s->hash == hash && s->byteLen == len
        && (len == 0 || memcmp(s->data, data, len) == 0)) {
      return s;
    }
    idx = (idx + 1) & mask;
  }
  return NULL;
}

// Inserts an already-allocated string; caller guarantees a free slot exists.
static void msStringInternInsert(struct MsHeap* heap, struct MsString* s) {
  size_t mask = heap->internCap - 1;
  size_t idx = s->hash & mask;
  while (heap->internSlots[idx] != NULL) {
    idx = (idx + 1) & mask;
  }
  heap->internSlots[idx] = s;
  ++heap->internLen;
}

static bool msStringInternGrow(struct MsHeap* heap) {
  size_t newCap = heap->internCap * 2;
  struct MsString** newSlots = (struct MsString**)msAlloc(newCap * sizeof(*newSlots));
  if (newSlots == NULL) {
    return false;
  }
  memset(newSlots, 0, newCap * sizeof(*newSlots));
  for (size_t i = 0; i < heap->internCap; ++i) {
    struct MsString* s = heap->internSlots[i];
    if (s == NULL) {
      continue;
    }
    size_t idx = s->hash & (newCap - 1);
    while (newSlots[idx] != NULL) {
      idx = (idx + 1) & (newCap - 1);
    }
    newSlots[idx] = s;
  }
  msFree(heap->internSlots);
  heap->internSlots = newSlots;
  heap->internCap = newCap;
  return true;
}

static struct MsString* msStringAlloc(struct MsHeap* heap, const char* data, size_t len,
                                      uint32_t hash) {
  struct MsString* s = (struct MsString*)msObjectAlloc(heap, MS_TYPE_STRING,
                                                       sizeof(struct MsString) + len + 1);
  if (s == NULL) {
    return NULL;
  }
  s->hash = hash;
  s->byteLen = len;
  if (len > 0) {
    memcpy(s->data, data, len);
  }
  s->data[len] = '\0';
  return s;
}

struct MsString* msStringNew(struct MsHeap* heap, const char* data, size_t len) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(data != NULL || len == 0);
  uint32_t hash = msStringFnv1a(data, len);
  if (len > MS_STRING_SHORT_MAX_LEN) {
    return msStringAlloc(heap, data, len, hash);
  }
  // Lookup hashes the input bytes in place: no allocate-then-discard on a hit.
  struct MsString* found = msStringInternFind(heap, data, len, hash);
  if (found != NULL) {
    return found;
  }
  if ((heap->internLen + 1) * 4 > heap->internCap * 3 && !msStringInternGrow(heap)) {
    return NULL;
  }
  struct MsString* s = msStringAlloc(heap, data, len, hash);
  if (s == NULL) {
    return NULL;
  }
  // Interned strings join the all-objects list like any other object; their
  // survival under GC (table as root-set member) is task 17's call.
  msStringInternInsert(heap, s);
  return s;
}

struct MsString* msStringFromCStr(struct MsHeap* heap, const char* cstr) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(cstr != NULL);
  return msStringNew(heap, cstr, strlen(cstr));
}

bool msStringEquals(const struct MsString* a, const struct MsString* b) {
  MS_ASSERT(a != NULL);
  MS_ASSERT(b != NULL);
  if (a == b) {
    return true;
  }
  if (a->byteLen != b->byteLen) {
    return false;
  }
  return a->byteLen == 0 || memcmp(a->data, b->data, a->byteLen) == 0;
}
