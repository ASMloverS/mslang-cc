#ifndef MSLANG_SRC_CORE_MS_COMMON_H_
#define MSLANG_SRC_CORE_MS_COMMON_H_

#include <assert.h>
#include <stddef.h>

#include <mslang/error.h>

// Number of elements in a fixed-size array. Compile-time only.
#define MS_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// Internal invariant check. Enabled in debug builds, compiled out under
// NDEBUG. Public API argument validation must return errors instead.
#ifdef NDEBUG
#define MS_ASSERT(cond) ((void)0)
#else
#define MS_ASSERT(cond) assert(cond)
#endif

// Marks a branch that must never be taken. Debug: assertion failure.
// Release: compiler optimization hint where available.
#ifdef NDEBUG
#if defined(_MSC_VER)
#define MS_UNREACHABLE() __assume(0)
#elif defined(__GNUC__) || defined(__clang__)
#define MS_UNREACHABLE() __builtin_unreachable()
#else
#define MS_UNREACHABLE() ((void)0)
#endif
#else
#define MS_UNREACHABLE() assert(!"unreachable")
#endif

// Silences unused-parameter/variable warnings.
#define MS_UNUSED(x) ((void)(x))

// Static name table for logs and tests ("MS_OK" etc.). Never returns NULL.
const char* msResultName(MsResult result);

#endif  // MSLANG_SRC_CORE_MS_COMMON_H_
