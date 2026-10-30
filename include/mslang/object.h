#ifndef MSLANG_INCLUDE_MSLANG_OBJECT_H_
#define MSLANG_INCLUDE_MSLANG_OBJECT_H_

#include <mslang/state.h>

typedef struct MsObject MsObject;

// Returns obj's internal NUL-terminated buffer when obj is a string. The buffer remains owned by L and must
// not be freed. Invalid input returns NULL and sets a runtime error when L is non-NULL.
const char* msAsCString(MsState* L, MsObject* obj);

#endif  // MSLANG_INCLUDE_MSLANG_OBJECT_H_
