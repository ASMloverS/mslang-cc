#ifndef MSLANG_SRC_OBJECT_MS_STRING_H_
#define MSLANG_SRC_OBJECT_MS_STRING_H_

#include "object/ms_object.h"

#define MS_STRING_SHORT_MAX_LEN 32   // residency threshold in bytes
#define MS_STRING_INTERN_INITIAL_CAP 64   // bucket count at heap init; shared with msHeapInit

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC C mode (-std:c11) does not implement C11 flexible array members and
// warns C4200; the layout and semantics used here are the standard ones.
#pragma warning(push)
#pragma warning(disable: 4200)
#endif

struct MsString {
  struct MsObjectHeader header;
  uint32_t hash;             // cached FNV-1a-32 over the bytes, computed once at construction
  size_t byteLen;            // UTF-8 byte length; code-point count is NOT stored (O(n) indexing)
  char data[];               // byteLen + 1 bytes, NUL-terminated for C interop
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

// Constructors. Content of length <= MS_STRING_SHORT_MAX_LEN is interned: equal
// content returns the same object (same pointer). Longer strings are always
// freshly allocated. Return NULL on OOM.
struct MsString* msStringNew(struct MsHeap* heap, const char* data, size_t len);
struct MsString* msStringFromCStr(struct MsHeap* heap, const char* cstr);

static inline const char* msStringData(const struct MsString* s) {
  return s->data;
}

static inline size_t msStringLen(const struct MsString* s) {
  return s->byteLen;
}

static inline uint32_t msStringHash(const struct MsString* s) {
  return s->hash;
}

// Byte-wise content equality.
bool msStringEquals(const struct MsString* a, const struct MsString* b);

#endif  // MSLANG_SRC_OBJECT_MS_STRING_H_
