#include "core/ms_diag.h"

#include <stdarg.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

MsResult msDiagListInit(struct MsDiagList* list, const char* chunkName) {
  MS_ASSERT(list != NULL);
  MS_ASSERT(chunkName != NULL);
  list->chunkName = chunkName;
  list->items = (struct MsDiag*)msAlloc(MS_DIAG_MAX_COUNT * sizeof(struct MsDiag));
  if (list->items == NULL) {
    return MS_ERROR_OOM;
  }
  list->count = 0;
  return MS_OK;
}

void msDiagListDestroy(struct MsDiagList* list) {
  if (list == NULL) {
    return;
  }
  msFree(list->items);
  list->items = NULL;
  list->count = 0;
  list->chunkName = NULL;
}

bool msDiagReport(struct MsDiagList* list, uint32_t line, uint32_t column, uint32_t code, const char* fmt, ...) {
  MS_ASSERT(list != NULL);
  MS_ASSERT(fmt != NULL);
  if (list->count >= MS_DIAG_MAX_COUNT) {
    return false;
  }
  struct MsDiag* diag = &list->items[list->count];
  diag->file = list->chunkName;
  diag->line = line;
  diag->column = column;
  diag->code = code;
  va_list args;
  va_start(args, fmt);
  vsnprintf(diag->message, sizeof(diag->message), fmt, args);
  va_end(args);
  ++list->count;
  return true;
}

bool msDiagListIsFull(const struct MsDiagList* list) {
  MS_ASSERT(list != NULL);
  return list->count >= MS_DIAG_MAX_COUNT;
}

size_t msDiagListCount(const struct MsDiagList* list) {
  MS_ASSERT(list != NULL);
  return list->count;
}

const struct MsDiag* msDiagListAt(const struct MsDiagList* list, size_t index) {
  MS_ASSERT(list != NULL);
  MS_ASSERT(index < list->count);
  if (index >= list->count) {
    return NULL;
  }
  return &list->items[index];
}

int msDiagFormat(const struct MsDiag* diag, char* out, size_t outSize) {
  MS_ASSERT(diag != NULL);
  return snprintf(out, outSize, "%s:%u:%u: error E%03u: %s", diag->file,
      (unsigned)diag->line, (unsigned)diag->column, (unsigned)diag->code, diag->message);
}

void msDiagListPrint(const struct MsDiagList* list, FILE* out) {
  MS_ASSERT(list != NULL);
  MS_ASSERT(out != NULL);
  char line[MS_DIAG_MESSAGE_LEN + 128];
  for (size_t i = 0; i < list->count; ++i) {
    msDiagFormat(&list->items[i], line, sizeof(line));
    fprintf(out, "%s\n", line);
  }
}
