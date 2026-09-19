# Task 01 工程骨架与构建系统 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 搭建可构建、可测试的空仓库骨架：CMake 构建系统、目录结构、自研 C 测试框架 `ms_test.h`、只支持 `--version` 的 CLI 空壳、`run_tests.py` 雏形。

**Architecture:** 单一根 `CMakeLists.txt` 管理全部 target（`mslang` 库、`mslang-cli`、`embed-example`、按模块独立的 C 测试可执行文件）。版本宏经 `configure_file` 从 `PROJECT_VERSION_*` 生成到 `build/include/mslang/version.h`。测试框架为单头文件显式登记表，无自动注册。

**Tech Stack:** C11（MSVC 2022 / GCC 10+ / Clang 12+）、CMake ≥ 3.20、Python ≥ 3.10（纯标准库）。

**Spec:** `docs/tasks/01-project-skeleton.md`（设计依据：`docs/language/11-project-layout.md` §1/§2/§4、`docs/language/10-c-style.md`、`AGENTS.md`）。

**Environment (verified on this machine):** Windows，Ninja 1.12.1、MSVC cl 14.38、CMake 3.29.5、Python 3.14.3 均在 PATH。构建命令统一使用 `-G Ninja`。

**Global rules (from AGENTS.md + 10-c-style.md):**
- 所有文本文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- 构建产物只允许出现在 `build/`。
- C 代码：2 空格缩进、120 列、K&R 大括号、指针星号贴类型、include guard（不用 `#pragma once`）、注释只用 `//` 且用英文、单语句块也带大括号。
- Commit message 格式：`<gitmoji> <type>(<scope>): <message>`（gitmoji 取自 AGENTS.md 清单）。

---

### Task 1: 目录结构与 `.gitkeep`

**Files:**
- Create: `src/lexer/.gitkeep`、`src/parser/.gitkeep`、`src/compiler/.gitkeep`、`src/vm/.gitkeep`、`src/object/.gitkeep`、`src/gc/.gitkeep`、`src/sched/.gitkeep`、`src/platform/.gitkeep`、`stdlib/.gitkeep`、`lib/.gitkeep`、`tests/ms/.gitkeep`、`tests/fixtures/.gitkeep`

（`include/mslang/`、`src/cli/`、`tests/c/`、`examples/` 在后续任务中由真实文件占位，不需要 `.gitkeep`。）

- [ ] **Step 1: 创建目录与占位文件**

```bash
mkdir -p include/mslang \
  src/{lexer,parser,compiler,vm,object,gc,sched,platform,cli} \
  stdlib lib tests/{c,ms,fixtures} examples
touch src/lexer/.gitkeep src/parser/.gitkeep src/compiler/.gitkeep \
  src/vm/.gitkeep src/object/.gitkeep src/gc/.gitkeep \
  src/sched/.gitkeep src/platform/.gitkeep \
  stdlib/.gitkeep lib/.gitkeep tests/ms/.gitkeep tests/fixtures/.gitkeep
```

- [ ] **Step 2: 验证目录结构**

Run: `git status --short`
Expected: 列出上述 12 个 `.gitkeep` 为 untracked；`.gitignore` 已含 `build/`（已确认，无需修改）。

- [ ] **Step 3: Commit**

```bash
git add .
git commit -m "🔧 chore(repo): add source directory skeleton"
```

---

### Task 2: CMake 骨架 + 版本头 + `mslang` 占位库

**Files:**
- Create: `CMakeLists.txt`
- Create: `include/mslang/version.h.in`
- Create: `src/ms_version.c`

- [ ] **Step 1: 编写 `include/mslang/version.h.in`**

```c
#ifndef MSLANG_INCLUDE_MSLANG_VERSION_H_
#define MSLANG_INCLUDE_MSLANG_VERSION_H_

#define MS_VERSION_MAJOR @PROJECT_VERSION_MAJOR@
#define MS_VERSION_MINOR @PROJECT_VERSION_MINOR@
#define MS_VERSION_PATCH @PROJECT_VERSION_PATCH@

// Returns the mslang version string, e.g. "0.1.0". The returned pointer is
// valid for the lifetime of the program and must not be freed.
const char* msVersionString(void);

#endif  // MSLANG_INCLUDE_MSLANG_VERSION_H_
```

- [ ] **Step 2: 编写 `src/ms_version.c`**

```c
#include <mslang/version.h>

#define MS_STRINGIFY_IMPL(x) #x
#define MS_STRINGIFY(x) MS_STRINGIFY_IMPL(x)

const char* msVersionString(void) {
  return MS_STRINGIFY(MS_VERSION_MAJOR) "." MS_STRINGIFY(MS_VERSION_MINOR) "." MS_STRINGIFY(MS_VERSION_PATCH);
}
```

- [ ] **Step 3: 编写根 `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(mslang VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MSLANG_BUILD_TESTS "Build tests" ON)
option(MSLANG_BUILD_SHARED "Build shared library" OFF)
option(MSLANG_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MSLANG_STRICT_WARNINGS "Treat warnings as errors" ON)

if(MSLANG_BUILD_SHARED)
  set(MSLANG_LIBRARY_TYPE SHARED)
else()
  set(MSLANG_LIBRARY_TYPE STATIC)
endif()

configure_file(
    "${PROJECT_SOURCE_DIR}/include/mslang/version.h.in"
    "${PROJECT_BINARY_DIR}/include/mslang/version.h"
    @ONLY)

function(mslang_apply_warnings target)
  if(MSLANG_STRICT_WARNINGS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /WX)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
endfunction()

function(mslang_apply_asan target)
  if(MSLANG_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endfunction()

add_library(mslang ${MSLANG_LIBRARY_TYPE} src/ms_version.c)
target_include_directories(mslang
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_BINARY_DIR}/include"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
mslang_apply_warnings(mslang)
mslang_apply_asan(mslang)

add_executable(embed-example examples/embed_demo.c)
target_link_libraries(embed-example PRIVATE mslang)
mslang_apply_warnings(embed-example)
mslang_apply_asan(embed-example)

if(MSLANG_BUILD_TESTS)
  enable_testing()
  function(mslang_add_c_test name)
    add_executable(${name} "tests/c/${name}.c")
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests/c")
    target_link_libraries(${name} PRIVATE mslang)
    mslang_apply_warnings(${name})
    mslang_apply_asan(${name})
    add_test(NAME ${name} COMMAND ${name})
  endfunction()
endif()
```

说明：`mslang-cli` 与测试注册在 Task 3/4 追加；`embed-example` 的源文件在 Task 5 创建——本步先注释掉或暂不写 `embed-example` 段。**按顺序执行：本步 CMakeLists 只写到 `add_library(mslang ...)` 段为止**，`embed-example` 与测试段在对应 Task 追加。

- [ ] **Step 4: 配置并构建（验证通过）**

Run: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
Expected: 配置成功，`mslang` 静态库编译无警告（`MSLANG_STRICT_WARNINGS` 默认 ON）；`build/include/mslang/version.h` 生成且含 `#define MS_VERSION_MINOR 1`。

验证生成头：Run: `grep "MS_VERSION_MINOR" build/include/mslang/version.h` → Expected: `#define MS_VERSION_MINOR 1`

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/mslang/version.h.in src/ms_version.c
git commit -m "🔧 chore(build): add CMake skeleton and version header"
```

---

### Task 3: CLI 空壳 `mslang --version`

**Files:**
- Create: `src/cli/main.c`
- Modify: `CMakeLists.txt`（在 `add_library` 段后追加 `mslang-cli` target）

- [ ] **Step 1: 确认失败前提（当前无可执行文件）**

Run: `ls build/mslang.exe 2>/dev/null || echo "not built yet"`
Expected: `not built yet`

- [ ] **Step 2: 编写 `src/cli/main.c`**

```c
#include <stdio.h>
#include <string.h>

#include <mslang/version.h>

static void printUsage(void) {
  fprintf(stderr, "usage: mslang [--version]\n");
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("mslang %s\n", msVersionString());
    return 0;
  }
  printUsage();
  return 2;
}
```

- [ ] **Step 3: 在 `CMakeLists.txt` 的 `mslang_apply_asan(mslang)` 行之后追加**

```cmake
add_executable(mslang-cli src/cli/main.c)
target_link_libraries(mslang-cli PRIVATE mslang)
set_target_properties(mslang-cli PROPERTIES OUTPUT_NAME mslang)
mslang_apply_warnings(mslang-cli)
mslang_apply_asan(mslang-cli)
```

- [ ] **Step 4: 重新配置、构建、验证**

Run: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
Expected: 构建成功无警告，产出 `build/mslang.exe`。

Run: `./build/mslang.exe --version`
Expected 输出: `mslang 0.1.0`；退出码 0（`echo $?` → `0`）。

Run: `./build/mslang.exe --bogus; echo "exit=$?"`
Expected: stderr 打印 `usage: mslang [--version]`，`exit=2`。

- [ ] **Step 5: Commit**

```bash
git add src/cli/main.c CMakeLists.txt
git commit -m "✨ feat(cli): add mslang --version shell"
```

---

### Task 4: `ms_test.h` 测试框架 + 自测

**Files:**
- Create: `tests/c/ms_test.h`
- Create: `tests/c/test_ms_test.c`
- Modify: `CMakeLists.txt`（在 `mslang_add_c_test` 函数定义后追加一行注册）

- [ ] **Step 1: 编写 `tests/c/ms_test.h`**

```c
#ifndef MSLANG_TESTS_C_MS_TEST_H_
#define MSLANG_TESTS_C_MS_TEST_H_

#include <stddef.h>
#include <stdio.h>

typedef void (*MsTestFunc)(void);

typedef struct {
  const char* name;
  MsTestFunc func;
} MsTestCase;

static int msTestFailureCount = 0;
static const char* msTestCurrentName = "<unknown>";

#define MS_TEST(suite, name) static void test##suite##name(void)

#define MS_ASSERT_TRUE(condition) \
  do { \
    if (!(condition)) { \
      ++msTestFailureCount; \
      printf("FAIL %s:%d: %s: assertion failed: %s\n", __FILE__, __LINE__, \
             msTestCurrentName, #condition); \
    } \
  } while (0)

#define MS_ASSERT_EQ(expected, actual) \
  do { \
    long long msExpected_ = (long long)(expected); \
    long long msActual_ = (long long)(actual); \
    if (msExpected_ != msActual_) { \
      ++msTestFailureCount; \
      printf("FAIL %s:%d: %s: expected %lld, actual %lld\n", __FILE__, __LINE__, \
             msTestCurrentName, msExpected_, msActual_); \
    } \
  } while (0)

#define MS_TEST_MAIN(cases) \
  int main(void) { \
    size_t msTotal_ = sizeof(cases) / sizeof((cases)[0]); \
    size_t msFailed_ = 0; \
    for (size_t msI_ = 0; msI_ < msTotal_; ++msI_) { \
      msTestCurrentName = (cases)[msI_].name; \
      int msFailuresBefore_ = msTestFailureCount; \
      (cases)[msI_].func(); \
      if (msTestFailureCount == msFailuresBefore_) { \
        printf("PASS %s\n", (cases)[msI_].name); \
      } else { \
        ++msFailed_; \
        printf("FAIL %s\n", (cases)[msI_].name); \
      } \
    } \
    printf("%zu of %zu tests passed\n", msTotal_ - msFailed_, msTotal_); \
    return msFailed_ == 0 ? 0 : 1; \
  }

#endif  // MSLANG_TESTS_C_MS_TEST_H_
```

- [ ] **Step 2: 编写 `tests/c/test_ms_test.c`（自测，先于注册进 CMake）**

```c
#include "ms_test.h"

MS_TEST(MsTest, AssertEqPassesOnEquality) {
  MS_ASSERT_EQ(42, 40 + 2);
}

MS_TEST(MsTest, AssertTruePassesOnTrue) {
  MS_ASSERT_TRUE(1 < 2);
}

// Intentionally triggers a failed assertion to verify the failure counter
// increments and execution continues, then restores the counter so the
// suite still passes.
MS_TEST(MsTest, AssertEqCountsFailureWithoutAborting) {
  int failuresBefore = msTestFailureCount;
  MS_ASSERT_EQ(1, 2);
  MS_ASSERT_TRUE(msTestFailureCount == failuresBefore + 1);
  msTestFailureCount = failuresBefore;
}

MS_TEST(MsTest, AssertTrueCountsFailureWithoutAborting) {
  int failuresBefore = msTestFailureCount;
  MS_ASSERT_TRUE(1 > 2);
  MS_ASSERT_TRUE(msTestFailureCount == failuresBefore + 1);
  msTestFailureCount = failuresBefore;
}

static const MsTestCase kCases[] = {
    {"MsTest.AssertEqPassesOnEquality", testMsTestAssertEqPassesOnEquality},
    {"MsTest.AssertTruePassesOnTrue", testMsTestAssertTruePassesOnTrue},
    {"MsTest.AssertEqCountsFailureWithoutAborting", testMsTestAssertEqCountsFailureWithoutAborting},
    {"MsTest.AssertTrueCountsFailureWithoutAborting", testMsTestAssertTrueCountsFailureWithoutAborting},
};

MS_TEST_MAIN(kCases)
```

- [ ] **Step 3: 在 `CMakeLists.txt` 的 `mslang_add_c_test` 函数定义结束后追加**

```cmake
  mslang_add_c_test(test_ms_test)
```

（追加在 `endif()` 之前、`endfunction()` 之后，保持缩进 2 空格。）

- [ ] **Step 4: 构建并运行 ctest（验证通过）**

Run: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 构建无警告；`test_ms_test` PASS；`100% tests passed, 0 tests failed out of 1`；`4 of 4 tests passed`（输出中两条 intentional FAIL 行属预期，最终退出码 0）。

- [ ] **Step 5: Commit**

```bash
git add tests/c/ms_test.h tests/c/test_ms_test.c CMakeLists.txt
git commit -m "✅ test(c): add ms_test.h framework with self-test"
```

---

### Task 5: `embed-example` 占位

**Files:**
- Create: `examples/embed_demo.c`
- Modify: `CMakeLists.txt`（确认 Task 2 模板中的 `embed-example` 段已存在；若 Task 2 按说明未写入，则在 `mslang-cli` 段后追加）

- [ ] **Step 1: 编写 `examples/embed_demo.c`**

```c
#include <stdio.h>

#include <mslang/version.h>

int main(void) {
  printf("embed-example: mslang %s\n", msVersionString());
  return 0;
}
```

- [ ] **Step 2: 确认 `CMakeLists.txt` 含 `embed-example` 段（Task 2 Step 3 的完整模板已包含；若当时省略则现在追加）**

```cmake
add_executable(embed-example examples/embed_demo.c)
target_link_libraries(embed-example PRIVATE mslang)
mslang_apply_warnings(embed-example)
mslang_apply_asan(embed-example)
```

- [ ] **Step 3: 构建并运行**

Run: `cmake --build build && ./build/embed-example.exe`
Expected: 构建无警告；输出 `embed-example: mslang 0.1.0`；退出码 0。

- [ ] **Step 4: Commit**

```bash
git add examples/embed_demo.c CMakeLists.txt
git commit -m "✅ test(examples): add embed-example placeholder"
```

---

### Task 6: `run_tests.py` 骨架

**Files:**
- Create: `run_tests.py`

- [ ] **Step 1: 编写 `run_tests.py`**

```python
"""Unified test driver: discovers tests/ms/**/*.ms and runs them via mslang."""

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
TESTS_MS_DIR = REPO_ROOT / "tests" / "ms"


def find_mslang_binary(explicit: Path | None) -> Path | None:
    """Resolves the mslang CLI binary, honoring --mslang when given."""
    if explicit is not None:
        return explicit if explicit.is_file() else None
    candidates: list[Path] = []
    for subdir in ("", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
        for name in ("mslang", "mslang.exe"):
            candidates.append(REPO_ROOT / "build" / subdir / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def discover_tests(pattern: str) -> list[Path]:
    """Returns sorted tests/ms/**/*.ms paths matching the glob filter."""
    if not TESTS_MS_DIR.is_dir():
        return []
    return sorted(
        path for path in TESTS_MS_DIR.rglob("*.ms") if path.match(pattern)
    )


def run_test(mslang: Path, script: Path) -> bool:
    """Runs one script; passes when the interpreter exits with code 0."""
    result = subprocess.run(
        [str(mslang), str(script)],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0


def main(argv: list[str] | None = None) -> int:
    """Entry point; returns the process exit code."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mslang",
        type=Path,
        default=None,
        help="path to the mslang interpreter binary",
    )
    parser.add_argument(
        "--filter",
        default="*.ms",
        help="glob filter for test scripts (default: *.ms)",
    )
    args = parser.parse_args(argv)

    tests = discover_tests(args.filter)
    if not tests:
        print("no tests found")
        return 0

    mslang = find_mslang_binary(args.mslang)
    if mslang is None:
        print(
            "error: mslang binary not found; build first or pass --mslang",
            file=sys.stderr,
        )
        return 1

    failed: list[Path] = []
    for script in tests:
        if run_test(mslang, script):
            print(f"PASS {script.relative_to(REPO_ROOT)}")
        else:
            failed.append(script)
            print(f"FAIL {script.relative_to(REPO_ROOT)}")

    print(f"{len(tests) - len(failed)} of {len(tests)} tests passed")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: 验证空测试目录行为**

Run: `python run_tests.py; echo "exit=$?"`
Expected: 输出 `no tests found`，`exit=0`。

- [ ] **Step 3: 验证参数解析**

Run: `python run_tests.py --help`
Expected: 打印 `--mslang` 与 `--filter` 用法，退出码 0。

- [ ] **Step 4: Commit**

```bash
git add run_tests.py
git commit -m "🔧 chore(tests): add run_tests.py driver skeleton"
```

---

### Task 7: 全量验收 + 状态标记

**Files:**
- Modify: `docs/tasks/README.md`（任务 01 状态 `⬜` → `✅`）
- Modify: `docs/tasks/01-project-skeleton.md`（状态表 `⬜` → `✅`，勾选验收标准复选框）

- [ ] **Step 1: 干净重建（模拟 CI）**

Run: `rm -rf build && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
Expected: 从零配置+构建成功，零警告。

- [ ] **Step 2: ctest 全绿**

Run: `ctest --test-dir build --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 1`。

- [ ] **Step 3: CLI 验收**

Run: `./build/mslang.exe --version` → Expected: `mslang 0.1.0`
Run: `./build/embed-example.exe` → Expected: `embed-example: mslang 0.1.0`

- [ ] **Step 4: run_tests.py 验收**

Run: `python run_tests.py; echo "exit=$?"`
Expected: `no tests found`，`exit=0`。

- [ ] **Step 5: ASAN 选项在 MSVC 下为安全 no-op**

Run: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMSLANG_ENABLE_ASAN=ON && cmake --build build`
Expected: 配置与构建成功（MSVC 下不加 `-fsanitize=address`）。

- [ ] **Step 6: 产物与文件格式检查**

Run: `git status --short`
Expected: 无 `build/` 外的产物文件（`build/` 被 gitignore）。

Run: `git ls-files | xargs grep -lP '\r' 2>/dev/null; echo "crlf-check-done"`
Expected: 无输出文件（除 `crlf-check-done`），即全部为 LF。

Run: `git ls-files | xargs grep -lP '^\xEF\xBB\xBF' 2>/dev/null; echo "bom-check-done"`
Expected: 无 BOM 文件。

- [ ] **Step 7: 更新任务状态标记并勾选验收标准**

`docs/tasks/README.md`：`| ⬜ | 01 |` → `| ✅ | 01 |`。
`docs/tasks/01-project-skeleton.md`：状态表 `⬜` → `✅`，验收标准 `- [ ]` 全部改为 `- [x]`（Windows 本地已验证的条目；Linux/macOS 条目按实标注为通过 CMake 跨平台语法保证，或在条目后注明仅本机 Windows 验证——勾选前逐条对照 Step 1–6 的实际结果）。

- [ ] **Step 8: Commit**

```bash
git add docs/tasks/README.md docs/tasks/01-project-skeleton.md
git commit -m "📝 docs(tasks): mark task 01 complete"
```

---

## Self-Review 记录

- **Spec coverage：** 目录结构 → Task 1；CMake 选项/target/警告/ASAN/版本宏 → Task 2/3/5；`ms_test.h` 三个宏 + 显式登记表 + 独立测试可执行文件注册 ctest → Task 4；CLI 空壳与退出码 2 → Task 3；`run_tests.py` 骨架（发现/执行/汇总/空目录退出 0/PEP 8 + `X | Y` 注解）→ Task 6；验收标准逐条 → Task 7。
- **偏差说明（spec 允许）：** 不建 `mslang-tests` 聚合大目标——任务文档明确"各模块独立可执行文件优于单一大目标，`mslang-tests` 可选保留"，故省略（YAGNI）；版本头采用 `configure_file` 路线（spec 允许二选一）。
- **Placeholder scan：** 无 TBD/TODO；所有步骤含完整代码与命令。
- **Type consistency：** `MS_TEST`/`MS_ASSERT_EQ`/`MS_ASSERT_TRUE`/`MS_TEST_MAIN`/`msTestFailureCount`/`msTestCurrentName`/`kCases` 在 ms_test.h 与 test_ms_test.c 间一致；`msVersionString()` 在 version.h.in、ms_version.c、main.c、embed_demo.c 间一致。
