# 34 mslang test 子命令

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [22 CLI 完善（REPL、-e）](22-cli-polish.md)、[24 模块系统与 import](24-modules-import.md) |

## 任务目标

为 `mslang` 可执行文件交付 `test` 子命令（`src/cli/ms_test_cmd.h` / `src/cli/ms_test_cmd.c`）：按 Go 风格的路径模式发现 `*_test.ms` 测试文件，在独立的解释器状态中逐文件执行其中的 `test*` 测试函数，隔离单文件失败，输出逐文件与汇总报告，并以退出码向外部驱动（CI、`run_tests.py`）传递结果。完成后：

```bash
mslang test ./...                # 递归发现当前目录下全部 *_test.ms 并运行
mslang test ./tests/ms/...       # 指定子树递归
mslang test ./tests/ms           # 仅该目录（不递归）
mslang test foo_test.ms          # 直接指定文件
mslang test -v ./...             # 逐测试函数打印
mslang test -run math ./...      # 只运行名字含 "math" 的测试函数
```

本任务把测试执行器做成 CLI 内部的独立组件，供任务 40（testing 标准库模块）复用同一套「发现 + 逐函数执行 + 报告」语义；`testing` 模块落地后，两者对同一测试文件必须给出一致的判定结果。

## 设计依据

- [11-project-layout.md](../language/11-project-layout.md)
  - §1：`src/cli/` 为 CLI 代码位置；`tests/ms/` 为脚本测试位置；`tests/fixtures/` 为测试数据位置。
  - §3：CLI 行为清单，含 `mslang test ./...`（运行测试，发现 `*_test.ms`）。
  - §4：三层测试策略；`ctest` 以 CLI 驱动 `tests/ms/`。
- [07-stdlib.md](../language/07-stdlib.md) §19（testing 约定）：测试文件命名 `xxx_test.ms`；测试函数名以 `test` 开头；`testing.run()` 发现并运行当前模块的 `test*` 函数；CLI 支持 `mslang test ./...` 并汇总报告。
- [08-vm-internals.md](../language/08-vm-internals.md) §1：编译错误经诊断收集器汇总（单文件上限 20 条），测试文件的编译失败按此模型归类为「编译错误」而非「测试失败」。
- [09-c-api.md](../language/09-c-api.md) §4：`msNewState` / `msCloseState` / `msEvalFile` / `MsResult`；§8/§11：错误槽与 `msErrorGet` 的错误取出方式。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、include guard、堆分配只经 `msAlloc`/`msRealloc`/`msFree`、内部结构体不 typedef）。
- [README.md](README.md)「测试约定」：任务 09 起用 ms 脚本测试；`testing` 模块（任务 40）完成前脚本用内建 `assert` + `print`；根目录 `run_tests.py` 是外部包装器，不替代 `mslang test`。
- 任务 22（CLI 完善）提供 argv 解析与子命令分派骨架（本文假定入口形如 `msCliMain(argc, argv)`，本任务在其中注册 `test` 子命令）；任务 24（模块系统与 import）提供模块全局命名空间的枚举能力（测试函数经模块全局表发现，本文假定接口形如 `msModuleGlobals` / 模块对象的全局表遍历）。这两个任务的文档尚未定稿，接口名为本文假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 文件与头文件骨架

- 头文件 `src/cli/ms_test_cmd.h`，include guard `MSLANG_SRC_CLI_MS_TEST_CMD_H_`，自包含（自行 include `<stdbool.h>` `<stddef.h>`）。
- 实现文件 `src/cli/ms_test_cmd.c`；路径扫描、排序等辅助函数一律文件内 `static`。
- 模块持有 `struct MsTestReport` 结果树（经 `msAlloc` 分配），由 `msTestReportFree` 统一释放，所有权单一。
- CLI 主分派（任务 22）识别 `argv[1] == "test"` 后调用 `msTestMain(argc - 1, argv + 1)`，其返回值即进程退出码。

### 数据结构

内部结构体不 typedef（10-c-style §4），字段 lowerCamelCase：

```c
typedef enum {
  MS_TESTFILE_PASS,   // 全部测试通过（含"无 test* 函数但顶层执行成功"）
  MS_TESTFILE_FAIL,   // 至少一个测试函数失败（断言/异常）
  MS_TESTFILE_ERROR   // 文件级错误：IO 失败、编译错误、顶层求值失败
} MsTestFileStatus;

struct MsTestOptions {
  bool verbose;           // -v：逐测试函数打印
  const char* runFilter;  // -run <子串>，NULL 表示不过滤；匹配测试函数名
};

struct MsTestFailure {
  char* testName;         // 测试函数名，"<top-level>" 表示顶层执行失败；堆分配
  char* message;          // 错误消息（含文件/行/列或异常信息）；堆分配
};

struct MsTestFileResult {
  char* path;             // 相对调用 cwd 的路径，按报告原样打印；堆分配
  MsTestFileStatus status;
  int passed;             // 通过的测试函数数
  int failed;             // 失败的测试函数数
  double seconds;         // 该文件总耗时（编译 + 顶层 + 全部测试函数）
  struct MsTestFailure* failures;  // 动态数组
  size_t failureCount;
};

struct MsTestReport {
  struct MsTestFileResult* files;  // 动态数组，按路径字典序
  size_t fileCount;
  int totalPassed;        // 全部文件 passed 之和
  int totalFailed;        // 全部文件 failed 之和
  int errorFiles;         // MS_TESTFILE_ERROR 的文件数
  double seconds;         // 总耗时
};
```

### 公开函数

```c
// Parses test-subcommand arguments (patterns, -v, -run) and runs the whole
// pipeline: discover -> execute -> report. Returns the process exit code
// (see "退出码" below); never returns MS_* — CLI 边界处完成退出码映射。
int msTestMain(int argc, char** argv);

// Expands one pattern into a sorted list of test file paths.
// Pattern forms: "dir/..." (recursive), "dir" (non-recursive), "file.ms".
// outPaths/outCount receive an msAlloc'd array of msAlloc'd strings;
// caller frees each element and the array with msFree.
// Returns MS_ERROR_IO when the base path does not exist.
MsResult msTestDiscover(const char* pattern, char*** outPaths, size_t* outCount);

// Runs one test file in a fresh MsState and fills out (fields owned by the
// report; freed by msTestReportFree). Out-of-memory propagates as
// MS_ERROR_OOM; all other failures are recorded in out, not returned.
MsResult msTestRunFile(const char* path, const struct MsTestOptions* opts, struct MsTestFileResult* out);

// Prints per-file lines and the summary line to stdout; failure details to
// stdout (对齐 Go test 的输出习惯) — stderr 仅保留给 runner 自身的用法错误。
void msTestReportPrint(const struct MsTestReport* report, const struct MsTestOptions* opts);

// Frees the whole report tree.
void msTestReportFree(struct MsTestReport* report);
```

### 测试发现

`msTestDiscover` 的路径模式规则（对齐 Go 的 `...` 语义）：

1. `xxx_test.ms` 文件路径：直接收录（即便文件名不符约定也收录，由用户负责）。
2. 目录路径：收录该目录**直接**包含的 `*_test.ms`，不递归。
3. 以 `/...`（或 `...\` 归一后）结尾的模式：剥去后缀得基目录，递归收录基目录下全部 `*_test.ms`。`./...` 的基目录是 `.`。
4. 无参数调用 `mslang test`：等价于 `./...`。
5. 递归遍历时跳过：隐藏目录（名字以 `.` 开头，含 `.git`）、`build/`（AGENTS.md 规定的构建产物目录）。
6. 收录结果按路径字典序排序，保证输出稳定、可复现。
7. 基路径不存在：`msTestMain` 打印用法级错误到 stderr，退出码 2。模式合法但匹配结果为空：打印 `no test files matched: <pattern>`，不计失败（与 Go 的 "no test files" 行为一致）。

目录遍历使用 C11 能力受限，v0.2 用 `_findfirst/_findnext`（MSVC）与 `opendir/readdir`（POSIX）两组 `static` 平台分支实现，集中在一个 `msTestScanDir` 静态函数内，不引入任务 42（平台抽象层）的依赖——任务 42 落地后可替换为其目录 API。

### 单文件执行与失败隔离

`msTestRunFile` 流程：

1. 记录起始时间（单调时钟，`time.monotonic` 的 C 侧对应物；任务 39 未落地前用 `clock()` 或平台 API 的 `static` 封装）。
2. 创建全新 `MsState`（`msNewState`）：每个测试文件一份全新解释器状态，全局命名空间互不可见，这是文件级隔离的根基。
3. `msEvalFile(L, path)` 编译并执行顶层：
   - 编译错误（诊断非空）→ `MS_TESTFILE_ERROR`，消息取诊断首条（含文件/行/列）；
   - 顶层运行时错误（含顶层 `assert` 失败、`raise`）→ `MS_TESTFILE_ERROR`，`testName` 记为 `<top-level>`，消息经 `msErrorGet` 取出；
   - 文件级错误即终止本文件执行，跳转步骤 6。
4. 顶层成功后，遍历模块全局命名空间（任务 24 的模块对象/全局表），收集满足全部条件的条目：名字以 `test` 开头、值为函数、必选参数数为 0。按名字字典序排序后逐个调用。`-run` 过滤在此生效：名字不含过滤子串的函数跳过（不计 passed/failed）。
5. 每个测试函数调用包一层 C API 保护（任务 23 异常系统的捕获能力，`msCallProtected` 一类接口，定名以任务文档为准）：
   - 正常返回 → `passed++`，`-v` 时打印 `--- PASS: testAdd (0.001s)`；
   - 断言失败或抛出未捕获异常 → `failed++`，失败消息（异常类型 + 消息 + 位置）记入 `failures`，**继续**执行本文件后续测试函数。
6. 销毁 `MsState`（`msCloseState`），记录耗时，填 `status`：`failed > 0` → `MS_TESTFILE_FAIL`；步骤 3 出错 → `MS_TESTFILE_ERROR`；否则 `MS_TESTFILE_PASS`。

隔离边界：隔离建立在「每文件一个 `MsState` + 每函数一次受保护调用」上。单文件失败绝不影响后续文件；但进程级崩溃（解释器 bug 导致段错误）无法在本进程内隔离——更强的每文件子进程隔离由根目录 `run_tests.py` 提供（见下「与 run_tests.py 的关系」）。

测试函数与 `testing` 模块的关系：本任务早于任务 40，测试函数体内只能用内建 `assert` 与 `print`。任务 40 落地后，`testing/assert` 的失败同样以异常形式冒泡到步骤 5 的受保护调用，本执行器无需改动即可兼容——这是「任务 40 依赖任务 34」而非反向依赖的原因。

### 报告格式

逐文件一行（stdout）：

```
ok      tests/ms/math_test.ms     0.012s  (3 tests)
FAIL    tests/ms/bad_test.ms      0.020s  (1/3 failed)
ERROR   tests/ms/broken_test.ms   compile error: tests/ms/broken_test.ms:7:3: ...
```

- 无 `-v` 时，失败详情（每个失败函数的消息）紧随其文件行之后缩进打印；`-v` 时另加 `--- PASS/FAIL: <name> (<秒>s)` 逐函数行。
- 汇总行（stdout，最后一行）：

```
3 files, 5 passed, 1 failed, 1 error, 0.045s
```

字段顺序固定：文件数、通过测试数、失败测试数、错误文件数、总秒数（三位小数）。

### 退出码

| 退出码 | 含义 |
|---|---|
| 0 | 全部文件通过（含匹配为空、含无 `test*` 函数的文件） |
| 1 | 存在 `MS_TESTFILE_FAIL` 或 `MS_TESTFILE_ERROR` 文件 |
| 2 | 用法错误：未知选项、基路径不存在、模式非法 |

`msTestMain` 是唯一产生退出码的位置；OOM 归为退出码 2 并打印 `out of memory` 到 stderr（测试基础设施故障与测试失败区分）。

### 与 run_tests.py 的关系

两者并存，职责不同（[README.md](README.md)「测试约定」）：

- `mslang test`（本任务）：规范的语言内测试入口，进程内多文件执行，速度快，供开发者日常使用；`ctest` 经它驱动 `tests/ms/`。
- `run_tests.py`（仓库根）：外部统一驱动，逐文件派生子进程调用 `mslang`，按退出码与输出判定。它提供更强的进程级隔离（解释器崩溃也能记为该文件失败并继续），且自身不依赖被测解释器的 test 子命令是否完好——解释器回归不会同时打坏「被测对象」与「测试驱动」。

约定：两者对同一测试文件的判定契约一致——退出码 0 = 通过、非 0 = 失败，失败详情含文件与位置。`run_tests.py` 保持逐文件直调 `mslang <file>` 的默认模式，不改为委托 `mslang test`，以维持隔离强度；可在将来增设 `--fast` 模式委托本命令，属包装器演进，不在本任务范围。

## 实现步骤

1. 建 `src/cli/ms_test_cmd.h` / `ms_test_cmd.c` 骨架：全量结构体与枚举、`msTestReportFree`。在任务 22 的 CLI 分派中注册 `test` 子命令，空实现先返回退出码 2 并打印用法。验证：`mslang test --bad-option` 退出码 2、stderr 有用法说明；`mslang test`（空目录下）打印 `no test files matched`、退出码 0。
2. 实现 `msTestDiscover`：三种模式、排序、隐藏目录与 `build/` 跳过、基路径不存在报错；`msTestScanDir` 的 MSVC/POSIX 双分支。验证：fixture 目录树上逐模式比对收录清单。
3. 实现 `msTestRunFile` 的文件级路径：`msNewState` / `msEvalFile` / 编译错误与顶层运行时错误归类 / `msCloseState` / 计时。验证：通过文件、语法错误文件、顶层抛错文件三种 fixture 的 `status` 与消息。
4. 实现测试函数收集与执行：全局表遍历、`test*` + 零必选参过滤、名字排序、`-run` 过滤、受保护调用、失败收集后继续。验证：混合通过/失败 fixture 的 passed/failed 计数与失败消息；`-run` 子串筛选结果。
5. 实现 `msTestReportPrint` 与 `msTestMain` 的完整流程串接、退出码映射。验证：逐文件行、`-v` 行、失败详情缩进、汇总行、三种退出码。
6. 建 fixtures（见「测试方案」）并接入 `ctest`（CMake `add_test` 驱动 `mslang test` 各模式 + `PASS_REGULAR_EXPRESSION` / `WILL_FAIL` 断言退出码与输出）。验证：`ctest --test-dir build` 全绿；`mslang test ./tests/ms/...` 对全量脚本测试自举冒烟通过。

## 测试方案

本任务验证对象是 CLI 子命令本身：判定依赖「以特定参数调用 `mslang` 二进制并断言其退出码与输出」，而 `os.exec` 属任务 36（晚于本任务），ms 脚本无法在进程内派生子进程。因此本任务的测试分两层，属对「任务 09 起一律 ms 脚本测试」约定的有依据例外：

1. **fixture + ctest 层**（主验收手段）：在 `tests/fixtures/cli-test/` 下构造样例测试文件树，由 CMake `add_test` 直接驱动 `mslang test` 并断言退出码与输出（`PASS_REGULAR_EXPRESSION` / `WILL_FAIL`）。fixture 全部用内建 `assert` + `print`（任务 40 之前的约定）。
2. **自举冒烟层**：`mslang test ./tests/ms/...` 必须能运行仓库全量脚本测试套件并全绿——用测试命令跑真实测试套件，即「测试的测试」。

fixture 清单（`tests/fixtures/cli-test/`，本任务只交付设计文档，文件随实现编写）：

- `pass/sample_test.ms`：两个 `test*` 函数全通过；一个非 `test` 前缀函数与一个带参 `test` 前缀函数（必须被跳过，不计数）。
- `fail/mixed_test.ms`：一个通过、一个 `assert` 失败、失败函数之后的另一个通过函数（验证失败后继续执行）。
- `error/compile_error_test.ms`：语法错误；`error/top_level_test.ms`：顶层运行时错误（如除零）。两者必须归为 ERROR 而非 FAIL。
- `nested/deep/deeper_test.ms`：深层目录文件，验证 `/...` 递归收录、`nested/`（不带 `...`）不收录深层文件。
- `hidden/.dot/skip_test.ms` 与 `build/skip_test.ms`：必须不被递归收录。
- `empty/`：空目录，验证「匹配为空」行为（提示 + 退出码 0）。
- `helper.ms`（非 `_test.ms` 后缀）：放在含测试文件的目录中，必须不被收录。

覆盖点：

- 发现：`./...` 递归、目录非递归、直指定文件、无参数等价 `./...`、排序稳定性、隐藏目录与 `build/` 跳过、基路径不存在（退出码 2）、匹配为空（退出码 0 + 提示）。
- 执行与隔离：全过文件 `ok`；混合文件 `FAIL` 且失败消息含函数名与位置、后续函数照常执行；编译错误与顶层错误归为 `ERROR`；一个失败/错误文件不影响其后的文件（在排序中置于前位验证）；每文件独立全局状态（`fail/` 与 `pass/` 文件声明同名全局变量互不污染）。
- 过滤与计数：`-run` 子串只运行匹配函数且不计入 passed/failed；无 `test*` 函数的文件记 `ok` 且计数为 0。
- 报告：`ok`/`FAIL`/`ERROR` 行格式、`-v` 的逐函数行、失败详情缩进、汇总行四字段与秒数格式。
- 退出码：全绿 0、有 FAIL 1、有 ERROR 1、用法错误 2。
- 自举冒烟：`mslang test ./tests/ms/...` 全量通过且退出码 0；与 `run_tests.py` 逐文件驱动的判定结果一致。

## 验收标准

- [ ] `src/cli/ms_test_cmd.h` / `ms_test_cmd.c` 存在，guard 为 `MSLANG_SRC_CLI_MS_TEST_CMD_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] 任务 22 的 CLI 分派识别 `test` 子命令并转入 `msTestMain`，其返回值即进程退出码；`mslang test --help` 或用法错误打印说明到 stderr 且退出码 2。
- [ ] `msTestDiscover` 实现文件/目录/`/...` 三种模式与无参默认 `./...`，递归时跳过隐藏目录与 `build/`，结果按路径字典序排序；基路径不存在退出码 2，匹配为空打印提示且退出码 0。
- [ ] `msTestRunFile` 每文件创建独立 `MsState`：文件间全局状态互不可见；编译错误与顶层运行时错误归为 ERROR，`test*` 函数失败归为 FAIL 且不中断本文件后续函数；单文件失败/错误不影响其余文件。
- [ ] 测试函数收集规则正确：名字 `test` 前缀、值为函数、必选参数数为 0，按名字排序执行；`-run` 子串过滤生效且被跳过函数不计数。
- [ ] 报告格式与「详细设计」一致：逐文件 `ok`/`FAIL`/`ERROR` 行、`-v` 逐函数行、失败详情、汇总行；退出码 0/1/2 映射正确。
- [ ] `tests/fixtures/cli-test/` 的 fixture 树覆盖「测试方案」清单，并经 `ctest` 断言退出码与输出全部通过；构建产物只落在 `build/`。
- [ ] 自举冒烟：`mslang test ./tests/ms/...` 对全量脚本测试套件退出码 0，判定结果与 `run_tests.py` 一致。
- [ ] 无 TBD/TODO 占位；与任务 22/24 的接口假定（`msCliMain` 分派、模块全局表遍历、受保护调用）在实现时已对齐。
