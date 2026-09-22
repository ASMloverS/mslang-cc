#ifndef MSLANG_SRC_CORE_MS_DIAG_H_
#define MSLANG_SRC_CORE_MS_DIAG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <mslang/error.h>

#define MS_DIAG_MAX_COUNT 20    // per-file diagnostic cap (08-vm-internals section 1)
#define MS_DIAG_MESSAGE_LEN 256 // inline message buffer per entry

struct MsDiag {
  const char* file;                  // == list->chunkName, not owned
  uint32_t line;                     // 1-based; 0 = unknown
  uint32_t column;                   // 1-based, byte count; 0 = unknown
  uint32_t code;                     // numeric part of the Exxx code (E103 -> 103)
  char message[MS_DIAG_MESSAGE_LEN]; // NUL-terminated, truncated if longer
};

// Diagnostics for one compilation unit (file). The entry array is allocated
// once at init with the fixed cap, so reporting never allocates.
struct MsDiagList {
  const char* chunkName; // file/chunk name, caller-owned, not copied
  struct MsDiag* items;  // msAlloc'd, capacity MS_DIAG_MAX_COUNT
  size_t count;          // entries recorded so far
};

// Allocates the fixed-capacity entry array. chunkName must outlive the list
// and is not copied. Returns MS_ERROR_OOM on allocation failure.
MsResult msDiagListInit(struct MsDiagList* list, const char* chunkName);

// Frees the entry array. Does not free chunkName.
void msDiagListDestroy(struct MsDiagList* list);

// Appends one diagnostic with a printf-style formatted message. Returns
// false without recording when the list is full (count == MS_DIAG_MAX_COUNT);
// callers treat false as "abort compilation". Never fails otherwise.
bool msDiagReport(struct MsDiagList* list, uint32_t line, uint32_t column, uint32_t code, const char* fmt, ...);

bool msDiagListIsFull(const struct MsDiagList* list);

size_t msDiagListCount(const struct MsDiagList* list);

// Returns the index-th entry. index >= count is a programming error
// (MS_ASSERT in debug, NULL in release).
const struct MsDiag* msDiagListAt(const struct MsDiagList* list, size_t index);

// Writes "file:line:column: error E<code>: <message>" into out (snprintf
// semantics: returns the would-be length).
int msDiagFormat(const struct MsDiag* diag, char* out, size_t outSize);

// Prints every entry via msDiagFormat, one per line.
void msDiagListPrint(const struct MsDiagList* list, FILE* out);

#endif  // MSLANG_SRC_CORE_MS_DIAG_H_
