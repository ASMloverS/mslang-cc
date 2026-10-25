#ifndef MSLANG_INCLUDE_MSLANG_STATE_H_
#define MSLANG_INCLUDE_MSLANG_STATE_H_

typedef struct MsState MsState;

// Creates an empty runtime state. Returns NULL when allocation fails.
MsState* msNewState(void);

// Releases a runtime state and every object owned by its heap. L may be NULL.
void msCloseState(MsState* L);

#endif  // MSLANG_INCLUDE_MSLANG_STATE_H_
