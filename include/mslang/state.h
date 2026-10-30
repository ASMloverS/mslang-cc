#ifndef MSLANG_INCLUDE_MSLANG_STATE_H_
#define MSLANG_INCLUDE_MSLANG_STATE_H_

#include <mslang/error.h>

typedef struct MsState MsState;

// Creates an empty runtime state. Returns NULL when allocation fails.
MsState* msNewState(void);

// Releases a runtime state and every object owned by its heap. L may be NULL.
void msCloseState(MsState* L);

// Compiles and executes path on L's main coroutine. path must remain valid for the call. Returns MS_OK or the
// first syntax, runtime, or allocation error; error details remain in L until retrieved or cleared.
MsResult msEvalFile(MsState* L, const char* path);

#endif  // MSLANG_INCLUDE_MSLANG_STATE_H_
