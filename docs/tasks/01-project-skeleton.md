# 01 工程骨架与构建系统

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ✅ | 无 |

## 任务目标

搭建可构建、可测试、可持续集成的空仓库骨架：CMake 构建系统、`src/` 目录结构、
自研 C 测试框架 `ms_test.h`、只支持 `--version` 的 CLI 空壳、Python 测试驱动
脚本 `run_tests.py` 的雏形。本任务不实现任何语言功能，但后续所有任务都建立在
这套骨架之上。

## 设计依据

- `docs/language/11-project-layout.md` §1（仓库结构）、§2（CMake 构建）、
  §4（测试策略）
- `docs/language/10-c-style.md` §1（源文件组织）、§9（平台抽象）
- `docs/language/09-c-api.md` §1（版本宏 `MS_VERSION_MAJOR/MINOR/PATCH`）
- `AGENTS.md`（构建产物只能在 `build/`、UTF-8 无 BOM、LF 行尾；Python 遵循
  PEP 8 且全量类型注解、禁止 `Optional`/`Union`）

## 详细设计

### 目录结构

按 `11-project-layout.md` §1 创建全部目录（空目录以 `.gitkeep` 占位）：

```
include/mslang/    src/{lexer,parser,compiler,vm,object,gc,sched,platform,cli}
stdlib/            lib/            tests/{c,ms,fixtures}    examples/
```

### CMake 构建

- 根 `CMakeLists.txt`：`cmake_minimum_required(VERSION 3.20)`，
  `project(mslang VERSION 0.1.0 LANGUAGES C)`，`C_STANDARD 11` 且
  `C_STANDARD_REQUIRED ON`、`C_EXTENSIONS OFF`。
- 四个构建选项，默认值与语义同 `11-project-layout.md` §2：
  `MSLANG_BUILD_TESTS`(ON)、`MSLANG_BUILD_SHARED`(OFF)、
  `MSLANG_ENABLE_ASAN`(OFF)、`MSLANG_STRICT_WARNINGS`(ON)。
- Target 布局（本任务只建空壳，后续任务往里填源文件）：

  | Target | 类型 | 内容 |
  |---|---|---|
  | `mslang` | 库（STATIC/SHARED 随 `MSLANG_BUILD_SHARED`） | 暂只含占位源文件 |
  | `mslang-cli` | 可执行文件，链接 `mslang` | `src/cli/main.c` |
  | `mslang-tests` | 可执行文件（`MSLANG_BUILD_TESTS=ON` 时） | `tests/c/` 全部源文件 |
  | `embed-example` | 可执行文件 | `examples/embed_demo.c` 占位 |

- include path：`include/` 以 `PUBLIC` 暴露（尖括号 `<mslang/...>`），`src/`
  以 `PRIVATE` 加入（引号 `"lexer/ms_lexer.h"` 形式）。
- `MSLANG_STRICT_WARNINGS=ON` 时开启高警告级别并视为错误：MSVC `/W4 /WX`，
  GCC/Clang `-Wall -Wextra -Wpedantic -Werror`。
- `MSLANG_ENABLE_ASAN=ON` 且非 MSVC 时为 `mslang` 及各可执行 target 追加
  `-fsanitize=address`（编译与链接）。
- `enable_testing()` + `add_test(NAME c-unit COMMAND mslang-tests)`，脚本测试的
  `add_test` 由任务 09 接入。
- 版本宏：从 `PROJECT_VERSION_*` 生成 `include/mslang/version.h`
  （`MS_VERSION_MAJOR/MINOR/MINOR/PATCH` 中的 MAJOR/MINOR/PATCH 与
  `msVersionString()` 声明），用 `configure_file` 或直接手写，随项目版本更新。

### `ms_test.h` 自研测试框架（约 100 行，单头文件）

不提供跨翻译单元的隐式自动注册（C 无可移植的构造器机制），采用显式登记表：

```c
typedef void (*MsTestFunc)(void);

typedef struct {
  const char* name;
  MsTestFunc func;
} MsTestCase;

#define MS_TEST(suite, name) ...
#define MS_ASSERT_EQ(expected, actual) ...
#define MS_ASSERT_TRUE(cond) ...
#define MS_TEST_MAIN(cases) ...
```

- `MS_TEST(suite, name)` 定义测试函数 `test<Suite><Name>`（命名遵循
  `10-c-style.md` §3 测试函数规则）。
- 断言失败时打印文件、行号、期望值与实际值，并将失败计入全局计数，不中断
  后续测试。
- `MS_TEST_MAIN(cases)` 生成 `main`：遍历用例表逐个执行，打印
  `PASS/FAIL` 与汇总，任一失败则返回非零退出码。
- 每个被测模块一个 `tests/c/test_<module>.c`，自带用例表与 `MS_TEST_MAIN`，
  各自链接为独立可执行文件注册到 ctest（优于单一 `mslang-tests` 大目标，
  与 `11-project-layout.md` §4"各模块独立测试"一致；`mslang-tests` 作为
  聚合目标可选保留）。

### CLI 空壳（`src/cli/main.c`）

- 仅支持 `mslang --version`：打印 `mslang 0.1.0` 并返回 0。
- 其他参数打印用法说明并返回退出码 2（参数错误）。退出码全集在任务 09 定义。

### `run_tests.py`（仓库根，测试统一驱动）

- 纯标准库实现（`pathlib`、`subprocess`、`sys`），无第三方依赖。
- 职责：递归发现 `tests/ms/**/*.ms`，依次调用 `mslang <script>`，按退出码
  判定通过/失败，可选比对期望输出，最终打印汇总并以非零退出码反映失败。
- CLI 形态：`python run_tests.py [--mslang <解释器路径>] [--filter <glob>]`，
  解释器默认取 `build/` 下构建产物。
- 本任务只交付骨架：发现逻辑、子进程执行与汇总报告；`tests/ms/` 为空时
  打印 "no tests found" 并退出 0。
- 代码遵循 PEP 8，所有函数带完整类型注解（`X | Y` 语法）。

## 实现步骤

1. 创建目录结构与 `.gitkeep` 占位。
2. 编写根 `CMakeLists.txt`：项目声明、C11、四个选项、`mslang` 占位库。
3. 编写 `include/mslang/version.h` 与 `src/cli/main.c`，接入 `mslang-cli`。
4. 编写 `tests/c/ms_test.h` 与一个自测文件 `tests/c/test_ms_test.c`
   （验证宏展开与断言计数），注册进 ctest。
5. 编写 `run_tests.py` 骨架。
6. 在本机完成 Debug 配置、构建、运行 `ctest`，确认全绿。

## 测试方案

- C 单元测试 `tests/c/test_ms_test.c`：`MS_TEST` 宏正确命名与登记；
  `MS_ASSERT_EQ` 相等/不等两路径；`MS_TEST_MAIN` 全通过时退出码 0。
- 构建冒烟（手工/CI）：`cmake -B build -DCMAKE_BUILD_TYPE=Debug` 与
  `cmake --build build` 在三个平台成功；`mslang --version` 输出符合格式。
- `run_tests.py`：在空 `tests/ms/` 下退出 0 且打印 "no tests found"。

## 验收标准

- [x] `cmake -B build` 与 `cmake --build build` 在 Windows/Linux/macOS 均成功（本机 Windows 验证；Linux/macOS 依赖 CI）。
- [x] `ctest --test-dir build` 全绿。
- [x] 开启 `MSLANG_STRICT_WARNINGS` 后无任何警告。
- [x] `mslang --version` 输出 `mslang 0.1.0`。
- [x] `python run_tests.py` 在无 ms 测试时退出 0。
- [x] 构建产物全部位于 `build/` 内，仓库其他位置无产物。
- [x] 所有文本文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
