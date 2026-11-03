#include "object/ms_list.h"

#include "core/ms_memory.h"
#include "vm/ms_state_internal.h"

struct MsObject* msNewList(MsState* L, int64_t capacity) {
  if (capacity < 0) {
    msRaiseValueError(L, "list capacity must be non-negative");
    return NULL;
  }
  if ((uint64_t)capacity > SIZE_MAX / sizeof(struct MsObject*)) {
    msStateSetOom(L);
    return NULL;
  }
  struct MsList* list = (struct MsList*)msObjectAlloc(&L->heap, MS_TYPE_LIST, sizeof(struct MsList));
  if (list == NULL) {
    msStateSetOom(L);
    return NULL;
  }
  list->items = NULL;
  list->len = 0;
  list->capacity = 0;
  if (capacity > 0) {
    list->items = (struct MsObject**)msAlloc((size_t)capacity * sizeof(*list->items));
    if (list->items == NULL) {
      msObjectFree(&L->heap, (struct MsObject*)list);
      msStateSetOom(L);
      return NULL;
    }
    list->capacity = capacity;
  }
  return (struct MsObject*)list;
}

MsResult msListAppend(MsState* L, struct MsList* list, struct MsObject* value) {
  MS_ASSERT(list != NULL);
  MS_ASSERT(value != NULL);
  if (list->len == list->capacity) {
    if (list->capacity > INT64_MAX / 2) {
      msStateSetOom(L);
      return MS_ERROR_OOM;
    }
    int64_t newCapacity = list->capacity == 0 ? 4 : list->capacity * 2;
    if ((uint64_t)newCapacity > SIZE_MAX / sizeof(*list->items)) {
      msStateSetOom(L);
      return MS_ERROR_OOM;
    }
    struct MsObject** items = (struct MsObject**)msRealloc(list->items,
        (size_t)newCapacity * sizeof(*items));
    if (items == NULL) {
      msStateSetOom(L);
      return MS_ERROR_OOM;
    }
    list->items = items;
    list->capacity = newCapacity;
  }
  list->items[list->len++] = value;
  return MS_OK;
}

void msListReleaseStorage(struct MsList* list) {
  MS_ASSERT(list != NULL);
  msFree(list->items);
  list->items = NULL;
  list->len = 0;
  list->capacity = 0;
}
