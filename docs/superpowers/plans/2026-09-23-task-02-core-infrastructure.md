# Task 02 核心基础设施 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付全项目共用核心基础设施：`MsResult` 错误码（`include/mslang/error.h`）、通用宏（`src/core/ms_common.h/.c`）、内存封装 msAlloc/msRealloc/msFree + 统计 + 失败注入（`src/core/ms_memory.h/.c`）、诊断收集器 MsDiagList（`src/core/ms_diag.h/.c`），全部由 C 单元测试验证。

**Architecture:** 在任务 01 的骨架上新增 `src/core/` 目录（4 个模块加入 `mslang` 库 target），测试沿用任务 01 的 `mslang_add_c_test` 每模块独立可执行约定。内存封装用「对齐块头记录请求大小」实现免旧大小的精确记账；诊断收集器固定容量 20 条、消息定长内联，报告路径零分配。

**Tech Stack:** C11（MSVC 2022 / Ninja / CMake 3.29）、任务 01 的 `tests/c/ms_test.h`。

**Spec:** `docs/tasks/02-core-infrastructure.md`（接口签名与语义以该文档为准；本计划给出与其一致的完整实现代码）。

**Environment:** Windows，Git Bash，构建命令 `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build`，测试 `ctest --test-dir build --output-on-failure`。

**Global rules:** UTF-8 无 BOM、LF、无行尾空白；产物只在 `build/`；2 空格缩进、120 列、K&R、指针星号贴类型、include guard、仅 `//` 英文注释；commit 格式 `<gitmoji> <type>(<scope>): <message>`。

**已知偏差（spec 已批准）：** `src/core/` 不在 11-project-layout §1 目录清单中，spec 明确「后续同步更新，不在本任务范围内」；内存统计的文件级 static 可变状态是 spec 明文批准的例外。

---

### Task 1: `MsResult` 错误码 + 通用宏 + `msResultName`

**Files:**
- Create: `include/mslang/error.h`
- Create: `src/core/ms_common.h`
- Create: `src/core/ms_common.c`
- Create: `tests/c/test_common.c`
- Modify: `CMakeLists.txt`（库源文件、`mslang_add_c_test` 增加 src/ include、注册 test_common）

- [ ] **Step 1: 编写 `include/mslang/error.h`**

```c
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

#endif  // MSLANG_INCLUDE_MSLANG_ERROR_H_
```

- [ ] **Step 2: 编写 `src/core/ms_common.h`**

```c
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
```

- [ ] **Step 3: 编写 `src/core/ms_common.c`**

```c
#include "core/ms_common.h"

const char* msResultName(MsResult result) {
  switch (result) {
    case MS_OK:
      return "MS_OK";
    case MS_ERROR_RUNTIME:
      return "MS_ERROR_RUNTIME";
    case MS_ERROR_SYNTAX:
      return "MS_ERROR_SYNTAX";
    case MS_ERROR_OOM:
      return "MS_ERROR_OOM";
    default:
      return "MS_RESULT_UNKNOWN";
  }
}
```

- [ ] **Step 4: 编写 `tests/c/test_common.c`**

```c
#include "core/ms_common.h"

#include <string.h>

#include "ms_test.h"

MS_TEST(Common, ResultValuesMatchCApiSpec) {
  MS_ASSERT_EQ(0, MS_OK);
  MS_ASSERT_EQ(1, MS_ERROR_RUNTIME);
  MS_ASSERT_EQ(2, MS_ERROR_SYNTAX);
  MS_ASSERT_EQ(3, MS_ERROR_OOM);
}

MS_TEST(Common, ArrayLen) {
  int one[1] = {0};
  int five[5] = {0};
  int twenty[20] = {0};
  MS_ASSERT_EQ(1, MS_ARRAY_LEN(one));
  MS_ASSERT_EQ(5, MS_ARRAY_LEN(five));
  MS_ASSERT_EQ(20, MS_ARRAY_LEN(twenty));
}

MS_TEST(Common, AssertTruePasses) {
  MS_ASSERT(true);
  MS_ASSERT_TRUE(true);
}

static int unusedProbe(int unusedParam) {
  MS_UNUSED(unusedParam);
  return 42;
}

MS_TEST(Common, UnusedSilencesWarning) {
  MS_ASSERT_EQ(42, unusedProbe(7));
}

static void unreachableProbe(int value) {
  switch (value) {
    case 0:
      break;
    default:
      MS_UNREACHABLE();
  }
}

MS_TEST(Common, UnreachableCompilesAndPassesOnHandledPath) {
  unreachableProbe(0);
  MS_ASSERT_TRUE(true);
}

MS_TEST(Common, ResultNameCoversAllValues) {
  MS_ASSERT_TRUE(strcmp(msResultName(MS_OK), "MS_OK") == 0);
  MS_ASSERT_TRUE(strcmp(msResultName(MS_ERROR_RUNTIME), "MS_ERROR_RUNTIME") == 0);
  MS_ASSERT_TRUE(strcmp(msResultName(MS_ERROR_SYNTAX), "MS_ERROR_SYNTAX") == 0);
  MS_ASSERT_TRUE(strcmp(msResultName(MS_ERROR_OOM), "MS_ERROR_OOM") == 0);
}

MS_TEST(Common, ResultNameNeverReturnsNull) {
  MS_ASSERT_TRUE(msResultName((MsResult)999) != NULL);
}

static const MsTestCase msTestCases[] = {
    {"Common.ResultValuesMatchCApiSpec", testCommonResultValuesMatchCApiSpec},
    {"Common.ArrayLen", testCommonArrayLen},
    {"Common.AssertTruePasses", testCommonAssertTruePasses},
    {"Common.UnusedSilencesWarning", testCommonUnusedSilencesWarning},
    {"Common.UnreachableCompilesAndPassesOnHandledPath", testCommonUnreachableCompilesAndPassesOnHandledPath},
    {"Common.ResultNameCoversAllValues", testCommonResultNameCoversAllValues},
    {"Common.ResultNameNeverReturnsNull", testCommonResultNameNeverReturnsNull},
};

MS_TEST_MAIN(msTestCases)
```

- [ ] **Step 5: 修改 `CMakeLists.txt`**

把库 target 的源文件行改为：

```cmake
add_library(mslang ${MSLANG_LIBRARY_TYPE}
    src/ms_version.c
    src/core/ms_common.c)
```

在 `mslang_add_c_test` 函数体内、`target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests/c")` 一行后追加 src/ include（测试以 `"core/ms_common.h"` 形式包含内部头）：

```cmake
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
```

在 `mslang_add_c_test(test_ms_test)` 一行后追加：

```cmake
  mslang_add_c_test(test_common)
```

- [ ] **Step 6: 构建并运行 ctest（Debug 与 Release 两种配置都要验证 `MS_ASSERT`/`MS_UNREACHABLE` 两条分支都能编译）**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: 零警告；`100% tests passed, 0 tests failed out of 2`。

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

Expected: Release 同样零警告全绿；最后切回 Debug 保持后续任务一致。

- [ ] **Step 7: Commit**

```bash
git add include/mslang/error.h src/core/ms_common.h src/core/ms_common.c tests/c/test_common.c CMakeLists.txt
git commit -m "✨ feat(core): add MsResult error codes and common macros"
```

---

### Task 2: 内存封装 `ms_memory`

**Files:**
- Create: `src/core/ms_memory.h`
- Create: `src/core/ms_memory.c`
- Create: `tests/c/test_memory.c`
- Modify: `CMakeLists.txt`（库加 `src/core/ms_memory.c`，注册 test_memory）

- [ ] **Step 1: 编写 `src/core/ms_memory.h`**

```c
#ifndef MSLANG_SRC_CORE_MS_MEMORY_H_
#define MSLANG_SRC_CORE_MS_MEMORY_H_

#include <stddef.h>
#include <stdint.h>

// Allocates size bytes. Returns NULL on OOM or size == 0 (size == 0 is a
// caller bug; MS_ASSERT in debug). Never aborts. Content is uninitialized.
void* msAlloc(size_t size);

// Resizes ptr to newSize bytes. ptr may be NULL (equivalent to msAlloc).
// On failure returns NULL and leaves the original block untouched.
// newSize must be > 0; use msFree to release.
void* msRealloc(void* ptr, size_t newSize);

// Frees a block from msAlloc/msRealloc. NULL is a no-op.
void msFree(void* ptr);

// Allocation statistics. totalAllocatedBytes counts payload bytes ever
// allocated, including the growth delta of growing msRealloc calls.
struct MsMemStats {
  uint64_t allocCount;
  uint64_t reallocCount;
  uint64_t freeCount;
  size_t liveBlocks;
  size_t currentBytes;
  size_t peakBytes;
  size_t totalAllocatedBytes;
};

// Copies a snapshot of the counters into out.
void msMemGetStats(struct MsMemStats* out);

// Zeroes the counters. Does NOT free live blocks; use only for test
// isolation, never to hide leaks.
void msMemResetStats(void);

// After n more successful allocations, msAlloc/msRealloc start failing.
// n < 0 disables injection. Test/diagnostic use only.
void msMemSetFailAfter(int64_t n);

#endif  // MSLANG_SRC_CORE_MS_MEMORY_H_
```

- [ ] **Step 2: 编写 `src/core/ms_memory.c`**

```c
#include "core/ms_memory.h"

#include <stdalign.h>
#include <stdlib.h>

#include "core/ms_common.h"

struct MsMemHeader {
  size_t size;  // requested payload size in bytes
};

// Header size rounded up so the payload stays aligned to max_align_t.
#define MS_MEM_HEADER_SIZE \
  ((sizeof(struct MsMemHeader) + alignof(max_align_t) - 1) / alignof(max_align_t) * alignof(max_align_t))

// Process-level diagnostic counters, accessed only through the functions in
// this file (sanctioned exception to the no-mutable-globals rule; see task
// 02 design notes). Single-threaded semantics for v0.1.
static struct MsMemStats msMemStats;
static int64_t msMemFailAfter = -1;

// Returns true when the allocation must fail due to fault injection.
static bool consumeFailAfter(void) {
  if (msMemFailAfter < 0) {
    return false;
  }
  if (msMemFailAfter == 0) {
    return true;
  }
  --msMemFailAfter;
  return false;
}

static void accountAlloc(size_t size) {
  ++msMemStats.allocCount;
  ++msMemStats.liveBlocks;
  msMemStats.currentBytes += size;
  if (msMemStats.currentBytes > msMemStats.peakBytes) {
    msMemStats.peakBytes = msMemStats.currentBytes;
  }
  msMemStats.totalAllocatedBytes += size;
}

void* msAlloc(size_t size) {
  MS_ASSERT(size > 0);
  if (size == 0 || size > SIZE_MAX - MS_MEM_HEADER_SIZE) {
    return NULL;
  }
  if (consumeFailAfter()) {
    return NULL;
  }
  struct MsMemHeader* header = (struct MsMemHeader*)malloc(MS_MEM_HEADER_SIZE + size);
  if (header == NULL) {
    return NULL;
  }
  header->size = size;
  accountAlloc(size);
  return (void*)((char*)header + MS_MEM_HEADER_SIZE);
}

void* msRealloc(void* ptr, size_t newSize) {
  if (ptr == NULL) {
    return msAlloc(newSize);
  }
  MS_ASSERT(newSize > 0);
  if (newSize == 0 || newSize > SIZE_MAX - MS_MEM_HEADER_SIZE) {
    return NULL;
  }
  struct MsMemHeader* oldHeader = (struct MsMemHeader*)((char*)ptr - MS_MEM_HEADER_SIZE);
  size_t oldSize = oldHeader->size;
  if (consumeFailAfter()) {
    return NULL;
  }
  struct MsMemHeader* newHeader = (struct MsMemHeader*)realloc(oldHeader, MS_MEM_HEADER_SIZE + newSize);
  if (newHeader == NULL) {
    return NULL;
  }
  newHeader->size = newSize;
  ++msMemStats.reallocCount;
  msMemStats.currentBytes += newSize - oldSize;  // unsigned wraparound yields the exact delta
  if (msMemStats.currentBytes > msMemStats.peakBytes) {
    msMemStats.peakBytes = msMemStats.currentBytes;
  }
  if (newSize > oldSize) {
    msMemStats.totalAllocatedBytes += newSize - oldSize;
  }
  return (void*)((char*)newHeader + MS_MEM_HEADER_SIZE);
}

void msFree(void* ptr) {
  if (ptr == NULL) {
    return;
  }
  struct MsMemHeader* header = (struct MsMemHeader*)((char*)ptr - MS_MEM_HEADER_SIZE);
  ++msMemStats.freeCount;
  --msMemStats.liveBlocks;
  msMemStats.currentBytes -= header->size;
  free(header);
}

void msMemGetStats(struct MsMemStats* out) {
  MS_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  *out = msMemStats;
}

void msMemResetStats(void) {
  msMemStats = (struct MsMemStats){0};
}

void msMemSetFailAfter(int64_t n) {
  msMemFailAfter = n;
}
```

注意：`#include <stdbool.h>` 需要加在 include 列表中（`consumeFailAfter` 返回 bool）——完整 include 块为：

```c
#include "core/ms_memory.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdlib.h>

#include "core/ms_common.h"
```

- [ ] **Step 3: 编写 `tests/c/test_memory.c`**

```c
#include "core/ms_memory.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ms_test.h"

// Every test resets the counters for isolation and asserts liveBlocks == 0
// && currentBytes == 0 at the end, so a leak fails the suite.

static void assertNoLeak(void) {
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  MS_ASSERT_EQ(0, stats.currentBytes);
}

MS_TEST(Memory, AllocWritableAndAccounted) {
  msMemResetStats();
  char* p = (char*)msAlloc(64);
  MS_ASSERT_TRUE(p != NULL);
  memset(p, 0xAB, 64);
  MS_ASSERT_EQ(0xAB, (unsigned char)p[63]);

  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(1, stats.allocCount);
  MS_ASSERT_EQ(1, stats.liveBlocks);
  MS_ASSERT_EQ(64, stats.currentBytes);
  MS_ASSERT_EQ(64, stats.peakBytes);
  MS_ASSERT_EQ(64, stats.totalAllocatedBytes);

  msFree(p);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(1, stats.freeCount);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  MS_ASSERT_EQ(0, stats.currentBytes);
  assertNoLeak();
}

MS_TEST(Memory, AllocAlignsToMaxAlign) {
  msMemResetStats();
  void* p = msAlloc(1);
  MS_ASSERT_TRUE(p != NULL);
  MS_ASSERT_EQ(0, (uintptr_t)p % alignof(max_align_t));
  msFree(p);
  assertNoLeak();
}

MS_TEST(Memory, AllocationsDoNotOverlap) {
  msMemResetStats();
  char* a = (char*)msAlloc(16);
  char* b = (char*)msAlloc(16);
  MS_ASSERT_TRUE(a != NULL && b != NULL);
  memset(a, 0x11, 16);
  memset(b, 0x22, 16);
  MS_ASSERT_TRUE(a != b);
  MS_ASSERT_EQ(0x11, (unsigned char)a[0]);
  MS_ASSERT_EQ(0x11, (unsigned char)a[15]);
  MS_ASSERT_EQ(0x22, (unsigned char)b[0]);
  msFree(a);
  msFree(b);
  assertNoLeak();
}

MS_TEST(Memory, PeakTracksHighWaterMark) {
  msMemResetStats();
  void* a = msAlloc(100);
  void* b = msAlloc(50);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(150, stats.currentBytes);
  MS_ASSERT_EQ(150, stats.peakBytes);
  MS_ASSERT_EQ(150, stats.totalAllocatedBytes);
  msFree(a);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(50, stats.currentBytes);
  MS_ASSERT_EQ(150, stats.peakBytes);
  msFree(b);
  assertNoLeak();
}

MS_TEST(Memory, ReallocPreservesDataAndAccountsDelta) {
  msMemResetStats();
  char* p = (char*)msAlloc(16);
  MS_ASSERT_TRUE(p != NULL);
  for (int i = 0; i < 16; ++i) {
    p[i] = (char)i;
  }
  char* grown = (char*)msRealloc(p, 64);
  MS_ASSERT_TRUE(grown != NULL);
  bool preserved = true;
  for (int i = 0; i < 16; ++i) {
    if (grown[i] != (char)i) {
      preserved = false;
    }
  }
  MS_ASSERT_TRUE(preserved);

  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(1, stats.reallocCount);
  MS_ASSERT_EQ(1, stats.liveBlocks);
  MS_ASSERT_EQ(64, stats.currentBytes);
  MS_ASSERT_EQ(64, stats.peakBytes);
  MS_ASSERT_EQ(64, stats.totalAllocatedBytes);  // 16 alloc + 48 growth

  char* shrunk = (char*)msRealloc(grown, 8);
  MS_ASSERT_TRUE(shrunk != NULL);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(8, stats.currentBytes);
  MS_ASSERT_EQ(64, stats.peakBytes);
  MS_ASSERT_EQ(64, stats.totalAllocatedBytes);  // shrink keeps cumulative
  msFree(shrunk);
  assertNoLeak();
}

MS_TEST(Memory, ReallocNullActsAsAlloc) {
  msMemResetStats();
  void* p = msRealloc(NULL, 32);
  MS_ASSERT_TRUE(p != NULL);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(1, stats.allocCount);
  MS_ASSERT_EQ(0, stats.reallocCount);
  MS_ASSERT_EQ(32, stats.currentBytes);
  msFree(p);
  assertNoLeak();
}

MS_TEST(Memory, FreeNullIsNoOp) {
  msMemResetStats();
  msFree(NULL);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.freeCount);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  assertNoLeak();
}

MS_TEST(Memory, FailAfterZeroFailsImmediately) {
  msMemResetStats();
  msMemSetFailAfter(0);
  MS_ASSERT_TRUE(msAlloc(8) == NULL);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.allocCount);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  MS_ASSERT_EQ(0, stats.currentBytes);
  msMemSetFailAfter(-1);
  void* p = msAlloc(8);
  MS_ASSERT_TRUE(p != NULL);
  msFree(p);
  assertNoLeak();
}

MS_TEST(Memory, FailAfterCountsDownSuccesses) {
  msMemResetStats();
  msMemSetFailAfter(2);
  void* a = msAlloc(8);
  void* b = msAlloc(8);
  void* c = msAlloc(8);
  MS_ASSERT_TRUE(a != NULL);
  MS_ASSERT_TRUE(b != NULL);
  MS_ASSERT_TRUE(c == NULL);
  msMemSetFailAfter(-1);
  msFree(a);
  msFree(b);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(2, stats.allocCount);
  assertNoLeak();
}

MS_TEST(Memory, ReallocFailureKeepsOriginalBlock) {
  msMemResetStats();
  char* p = (char*)msAlloc(32);
  MS_ASSERT_TRUE(p != NULL);
  p[0] = 'x';
  msMemSetFailAfter(0);
  MS_ASSERT_TRUE(msRealloc(p, 64) == NULL);
  msMemSetFailAfter(-1);
  MS_ASSERT_EQ('x', p[0]);  // original block untouched
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.reallocCount);
  MS_ASSERT_EQ(1, stats.liveBlocks);
  MS_ASSERT_EQ(32, stats.currentBytes);
  msFree(p);
  assertNoLeak();
}

MS_TEST(Memory, AllocOverflowFailsCleanly) {
  msMemResetStats();
  MS_ASSERT_TRUE(msAlloc(SIZE_MAX) == NULL);
  MS_ASSERT_TRUE(msAlloc(SIZE_MAX - 4) == NULL);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.allocCount);
  assertNoLeak();
}

static const MsTestCase msTestCases[] = {
    {"Memory.AllocWritableAndAccounted", testMemoryAllocWritableAndAccounted},
    {"Memory.AllocAlignsToMaxAlign", testMemoryAllocAlignsToMaxAlign},
    {"Memory.AllocationsDoNotOverlap", testMemoryAllocationsDoNotOverlap},
    {"Memory.PeakTracksHighWaterMark", testMemoryPeakTracksHighWaterMark},
    {"Memory.ReallocPreservesDataAndAccountsDelta", testMemoryReallocPreservesDataAndAccountsDelta},
    {"Memory.ReallocNullActsAsAlloc", testMemoryReallocNullActsAsAlloc},
    {"Memory.FreeNullIsNoOp", testMemoryFreeNullIsNoOp},
    {"Memory.FailAfterZeroFailsImmediately", testMemoryFailAfterZeroFailsImmediately},
    {"Memory.FailAfterCountsDownSuccesses", testMemoryFailAfterCountsDownSuccesses},
    {"Memory.ReallocFailureKeepsOriginalBlock", testMemoryReallocFailureKeepsOriginalBlock},
    {"Memory.AllocOverflowFailsCleanly", testMemoryAllocOverflowFailsCleanly},
};

MS_TEST_MAIN(msTestCases)
```

注意：`msAlloc(0)` 在 Debug 下触发 `MS_ASSERT` 终止进程，spec 排除死亡测试，故不测该路径。`test_memory.c` 需要 `<stdbool.h>`（`bool preserved`）——include 块补上：

```c
#include "core/ms_memory.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ms_test.h"
```

- [ ] **Step 4: 修改 `CMakeLists.txt`**

库源文件列表追加 `src/core/ms_diag.c` 之外的 memory 源：

```cmake
add_library(mslang ${MSLANG_LIBRARY_TYPE}
    src/ms_version.c
    src/core/ms_common.c
    src/core/ms_memory.c)
```

注册测试（`mslang_add_c_test(test_common)` 之后）：

```cmake
  mslang_add_c_test(test_memory)
```

- [ ] **Step 5: 构建 + ctest**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: 零警告；`100% tests passed, 0 tests failed out of 3`。

- [ ] **Step 6: Commit**

```bash
git add src/core/ms_memory.h src/core/ms_memory.c tests/c/test_memory.c CMakeLists.txt
git commit -m "✨ feat(core): add msAlloc/msRealloc/msFree with stats and fault injection"
```

---

### Task 3: 诊断收集器 `ms_diag`

**Files:**
- Create: `src/core/ms_diag.h`
- Create: `src/core/ms_diag.c`
- Create: `tests/c/test_diag.c`
- Modify: `CMakeLists.txt`（库加 `src/core/ms_diag.c`，注册 test_diag）

- [ ] **Step 1: 编写 `src/core/ms_diag.h`**

```c
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
```

- [ ] **Step 2: 编写 `src/core/ms_diag.c`**

```c
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
```

- [ ] **Step 3: 编写 `tests/c/test_diag.c`**

```c
#include "core/ms_diag.h"

#include <stdio.h>
#include <string.h>

#include "core/ms_memory.h"
#include "ms_test.h"

static void assertNoLeak(void) {
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  MS_ASSERT_EQ(0, stats.currentBytes);
}

MS_TEST(Diag, InitAndDestroy) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  MS_ASSERT_EQ(0, msDiagListCount(&list));
  MS_ASSERT_TRUE(!msDiagListIsFull(&list));
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, InitFailureReturnsOom) {
  msMemResetStats();
  msMemSetFailAfter(0);
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_ERROR_OOM, msDiagListInit(&list, "test.ms"));
  msMemSetFailAfter(-1);
  assertNoLeak();
}

MS_TEST(Diag, ReportRecordsFields) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  MS_ASSERT_TRUE(msDiagReport(&list, 10, 5, 103, "unexpected '%c'", 'x'));
  MS_ASSERT_EQ(1, msDiagListCount(&list));
  const struct MsDiag* diag = msDiagListAt(&list, 0);
  MS_ASSERT_TRUE(diag != NULL);
  MS_ASSERT_TRUE(diag->file == list.chunkName);
  MS_ASSERT_EQ(10, diag->line);
  MS_ASSERT_EQ(5, diag->column);
  MS_ASSERT_EQ(103, diag->code);
  MS_ASSERT_TRUE(strcmp(diag->message, "unexpected 'x'") == 0);
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, LongMessageIsTruncated) {
  msMemResetStats();
  char longMessage[300];
  memset(longMessage, 'a', sizeof(longMessage) - 1);
  longMessage[sizeof(longMessage) - 1] = '\0';
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  MS_ASSERT_TRUE(msDiagReport(&list, 1, 1, 1, "%s", longMessage));
  const struct MsDiag* diag = msDiagListAt(&list, 0);
  MS_ASSERT_EQ(MS_DIAG_MESSAGE_LEN - 1, strlen(diag->message));
  MS_ASSERT_EQ('\0', diag->message[MS_DIAG_MESSAGE_LEN - 1]);
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, CapacityCapStopsAtTwenty) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  int accepted = 0;
  for (uint32_t i = 0; i < 25; ++i) {
    if (msDiagReport(&list, i + 1, 1, 100, "error %u", (unsigned)i)) {
      ++accepted;
    }
  }
  MS_ASSERT_EQ(MS_DIAG_MAX_COUNT, accepted);
  MS_ASSERT_EQ(MS_DIAG_MAX_COUNT, msDiagListCount(&list));
  MS_ASSERT_TRUE(msDiagListIsFull(&list));
  MS_ASSERT_TRUE(!msDiagReport(&list, 99, 1, 100, "too late"));
  MS_ASSERT_EQ(MS_DIAG_MAX_COUNT, msDiagListCount(&list));
  const struct MsDiag* first = msDiagListAt(&list, 0);
  MS_ASSERT_EQ(1, first->line);
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, FormatProducesExpectedText) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  MS_ASSERT_TRUE(msDiagReport(&list, 1, 2, 103, "bad token"));
  char buffer[512];
  int written = msDiagFormat(msDiagListAt(&list, 0), buffer, sizeof(buffer));
  MS_ASSERT_TRUE(strcmp(buffer, "test.ms:1:2: error E103: bad token") == 0);
  MS_ASSERT_EQ(strlen(buffer), written);
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, PrintWritesOneLinePerEntry) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  msDiagReport(&list, 1, 1, 101, "first");
  msDiagReport(&list, 2, 3, 102, "second");
  // tmpfile() may fail in restricted environments; formatting itself is
  // covered by Diag.FormatProducesExpectedText, so a NULL FILE skips the
  // content check instead of failing.
  FILE* out = tmpfile();
  if (out != NULL) {
    msDiagListPrint(&list, out);
    rewind(out);
    char line[512];
    MS_ASSERT_TRUE(fgets(line, sizeof(line), out) != NULL);
    MS_ASSERT_TRUE(strcmp(line, "test.ms:1:1: error E101: first\n") == 0);
    MS_ASSERT_TRUE(fgets(line, sizeof(line), out) != NULL);
    MS_ASSERT_TRUE(strcmp(line, "test.ms:2:3: error E102: second\n") == 0);
    fclose(out);
  }
  msDiagListDestroy(&list);
  assertNoLeak();
}

static const MsTestCase msTestCases[] = {
    {"Diag.InitAndDestroy", testDiagInitAndDestroy},
    {"Diag.InitFailureReturnsOom", testDiagInitFailureReturnsOom},
    {"Diag.ReportRecordsFields", testDiagReportRecordsFields},
    {"Diag.LongMessageIsTruncated", testDiagLongMessageIsTruncated},
    {"Diag.CapacityCapStopsAtTwenty", testDiagCapacityCapStopsAtTwenty},
    {"Diag.FormatProducesExpectedText", testDiagFormatProducesExpectedText},
    {"Diag.PrintWritesOneLinePerEntry", testDiagPrintWritesOneLinePerEntry},
};

MS_TEST_MAIN(msTestCases)
```

- [ ] **Step 4: 修改 `CMakeLists.txt`**

库源文件列表追加 `src/core/ms_diag.c`；注册 `mslang_add_c_test(test_diag)`。

- [ ] **Step 5: 构建 + ctest**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: 零警告；`100% tests passed, 0 tests failed out of 4`。

- [ ] **Step 6: Commit**

```bash
git add src/core/ms_diag.h src/core/ms_diag.c tests/c/test_diag.c CMakeLists.txt
git commit -m "✨ feat(core): add MsDiagList diagnostic collector"
```

---

### Task 4: 全量验收 + 状态标记

**Files:**
- Modify: `docs/tasks/README.md`（任务 02 行 `⬜` → `✅`）
- Modify: `docs/tasks/02-core-infrastructure.md`（状态 `⬜` → `✅`，勾选验收标准）

- [ ] **Step 1: 干净重建 + 全测试（Debug）**

```bash
rm -rf build && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: 零警告；4/4 测试通过。

- [ ] **Step 2: Release 配置验证（MS_ASSERT/MS_UNREACHABLE 的 NDEBUG 分支）**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

Expected: 零警告全绿，切回 Debug。

- [ ] **Step 3: CLI 回归**

`./build/mslang.exe --version` → `mslang 0.1.0`；`./build/embed-example.exe` → 退出 0；`python run_tests.py` → `no tests found` 退出 0。

- [ ] **Step 4: 无裸 malloc 检查**

```bash
grep -rn --include='*.c' --include='*.h' -E '\b(malloc|realloc|free)\s*\(' src/ include/ | grep -v ms_memory.c
```

Expected: 无输出（仅 `src/core/ms_memory.c` 内部允许调用 CRT malloc/realloc/free）。

- [ ] **Step 5: 产物与格式检查**

```bash
git status --short
git ls-files | xargs grep -lP '\r' 2>/dev/null; echo crlf-ok
git ls-files | xargs grep -lP '^\xEF\xBB\xBF' 2>/dev/null; echo bom-ok
git ls-files | xargs grep -lP ' +$' 2>/dev/null; echo trailing-ws-ok
```

Expected: 无 build/ 外产物；三个检查列表为空。

- [ ] **Step 6: 更新状态标记**

`docs/tasks/README.md`：`| ⬜ | 02 |` → `| ✅ | 02 |`。
`docs/tasks/02-core-infrastructure.md`：状态表 `⬜` → `✅`；验收标准逐条对照本任务实际结果后 `- [ ]` → `- [x]`（「后续模块不重复定义 MsResult」等面向未来的条目按当前状态勾选：全仓库仅 error.h 定义，可 grep 验证）。

- [ ] **Step 7: Commit**

```bash
git add docs/tasks/README.md docs/tasks/02-core-infrastructure.md
git commit -m "📝 docs(tasks): mark task 02 complete"
```

---

## Self-Review 记录

- **Spec coverage：** error.h/MsResult → Task 1；通用宏 + msResultName → Task 1；内存封装（块头/统计/失败注入/溢出防护/OOM 路径）→ Task 2；MsDiagList（20 条上限/定长消息/格式化/打印）→ Task 3；测试方案清单逐项 → 三个测试文件覆盖（`msDiagListAt` 越界为 debug 断言，按 spec 不做死亡测试；`msAlloc(0)` 同理排除）；验收标准 → Task 4。
- **Placeholder scan：** 无 TBD/TODO，全部步骤含完整代码与命令。
- **Type consistency：** `MsMemStats` 字段名、`MS_DIAG_MAX_COUNT`/`MS_DIAG_MESSAGE_LEN`、`msDiagReport/msDiagListInit/...` 签名、`msMemSetFailAfter/msMemGetStats/msMemResetStats` 在头文件、实现、测试三处一致；`assertNoLeak` 辅助函数在 test_memory.c 与 test_diag.c 各自文件内定义（static，无跨文件依赖）。
- **已知风险：** MSVC `/W4` 下 `test_common.c` 的局部数组仅被 `sizeof` 使用——`/WX` 会在构建时验证；若触发警告，实现者须报告并按最小改动修复。
