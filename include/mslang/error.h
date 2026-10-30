#ifndef MSLANG_INCLUDE_MSLANG_ERROR_H_
#define MSLANG_INCLUDE_MSLANG_ERROR_H_

// Result codes shared by the public C API and internal modules. The value
// set matches 09-c-api.md section 4 and is frozen: extend only by appending.
typedef enum {
  MS_OK = 0,          // success
  MS_ERROR_RUNTIME,   // runtime error (exception raised or pending)
  MS_ERROR_SYNTAX,    // compilation failed; diagnostics hold the details
  MS_ERROR_OOM        // allocation failure
} MsResult;

typedef struct MsObject MsObject;
typedef struct MsState MsState;

// Returns the current error object and clears L's error slot. The object remains owned by L; NULL means no
// error was pending. L itself may be NULL, in which case the function returns NULL.
MsObject* msErrorGet(MsState* L);

#endif  // MSLANG_INCLUDE_MSLANG_ERROR_H_
