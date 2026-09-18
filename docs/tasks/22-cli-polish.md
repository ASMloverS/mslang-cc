# 22 CLI 完善（REPL、-e）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [09 最小可运行解释器](09-minimal-interpreter.md) |

## 任务目标

把任务 09 交付的「只能执行脚本文件」的最小 CLI 补全为 [11-project-layout.md](../language/11-project-layout.md) §3 规定的完整命令行界面（`mslang test` 子命令除外，属任务 34）：

```bash
mslang                    # 进入 REPL（无位置参数且 stdin 为 TTY 或管道均可）
mslang script.ms arg1     # 执行脚本，args 经 os.args 暴露（本任务为占位实现，见下）
mslang -e "print(1+1)"    # 执行代码片段
mslang --version          # 打印版本（任务 09 已交付）
mslang --help             # 打印用法
```

REPL 特性（§3 明确要求）：多行输入（花括号未闭合时续行）、历史记录、上次结果绑定 `_`。REPL 必须可被脚本化 stdin 驱动（`mslang < input.txt`），以便纳入 `run_tests.py` 的自动化测试。完成后 v0.1 路线图的「CLI + REPL」一项关闭。

## 设计依据

- [11-project-layout.md](../language/11-project-layout.md)
  - §1：`src/cli/` 为 `mslang` 可执行文件（REPL + 脚本入口）的目录位置；`tests/ms/` 为脚本测试位置。
  - §2：`mslang-cli` 构建目标；脚本测试经 `ctest` 以 CLI 驱动。
  - §3：CLI 行为与 REPL 三项特性（多行输入、历史记录、`_` 绑定）；脚本参数经 `os.args` 暴露。
- [09-c-api.md](../language/09-c-api.md) §4：`msEvalStringAs`（带 chunk 名的源码求值，供 `-e` 与 REPL 使用）、`msGetGlobal` / `msSetGlobal`；§5：`msNewString` / `msNewList`；§6：`msListAppend` / `msDictSet`（占位 args 构造用）；§8：错误取出（`msErrorGet`）。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.2：指令集含 `MS_OP_PRINT_EXPR`（REPL 用），即表达式回显的既定机制。
- [01-lexical.md](../language/01-lexical.md) §6/§7：定界符 token 与分号自动插入规则——REPL 续行判定复用任务 03 的 lexer 而非手写括号扫描。
- [07-stdlib.md](../language/07-stdlib.md) §6：`os.args` 为命令行参数 list 的最终形态。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc/msRealloc/msFree`、平台相关代码只出现在 `src/platform/`）。
- 任务 09 已交付：`src/cli/main.c` 的参数处理骨架、退出码表（`MS_EXIT_*`）、文件预检、`msEvalFile` 驱动与 `tests/ms/` + `run_tests.py` 测试设施；本任务在其上扩展，不重写。
- 假定接口（实现时以对应任务文档定名为准）：任务 07 编译器的编译入口支持 REPL 模式标记（本文假定 `MS_COMPILE_REPL` 选项位）；任务 08 VM 的 `MS_OP_PRINT_EXPR` 语义按本文第 5 节实现；任务 06/16 的 list/dict 构造 API 名按 [09-c-api.md](../language/09-c-api.md) §5/§6。

## 详细设计

### 1. 文件划分

- `src/cli/main.c`（任务 09 已存在）：进程入口、参数解析、`-e`/脚本/REPL 三种模式分派、退出码映射、`os.args` 占位暴露。
- `src/cli/ms_repl.h` / `src/cli/ms_repl.c`（新增）：REPL 引擎——行读取、续行判定、历史记录、求值循环。与终端类型无关，输入输出经 `FILE*` 注入，保证可用管道驱动测试。include guard `MSLANG_SRC_CLI_MS_REPL_H_`，头文件自包含（`<stdio.h>`、`<stdbool.h>`、`<stddef.h>` 及 `<mslang/state.h>`）。

### 2. 命令行语法与参数解析

```
mslang [--version | --help]
mslang -e <snippet> [args...]
mslang <script.ms> [args...]
mslang                       # REPL
```

内部结构体（不 typedef，10-c-style §4）：

```c
typedef enum {
  MS_CLI_MODE_REPL,       // 无位置参数
  MS_CLI_MODE_SCRIPT,     // 执行脚本文件
  MS_CLI_MODE_EVAL,       // -e 代码片段
  MS_CLI_MODE_VERSION,    // --version
  MS_CLI_MODE_HELP        // --help
} MsCliMode;

struct MsCliOptions {
  MsCliMode mode;
  const char* scriptPath;   // SCRIPT 模式：脚本路径；EVAL 模式：片段文本
  int argsIndex;            // argv 中脚本参数区起点（os.args[1..] 来源）
  int argsCount;
};

// Parses argv into out. Returns MS_EXIT_OK to continue execution, or an
// exit code to terminate with immediately (usage errors: MS_EXIT_USAGE;
// --version/--help have already printed and yield MS_EXIT_OK via their
// mode, handled by the caller before evaluation).
int msCliParseArgs(int argc, char** argv, struct MsCliOptions* out);
```

规则：

- 第一个非选项参数是脚本路径；其后全部参数（含以 `-` 开头的）原样进入脚本参数区，不再做选项解析（与 Python 一致）。
- `-e` 必须跟恰好一个片段操作数，缺失报用法错误（`MS_EXIT_USAGE`）。`-e` 与脚本路径互斥，同时出现报用法错误。
- 未知选项报用法错误并打印用法到 stderr。`mslang test ...` 暂不支持：识别到 `test` 子命令位置参数时给出「尚未支持，见任务 34」提示并以 `MS_EXIT_USAGE` 退出，避免被当作脚本文件报出误导性的 I/O 错误。
- 退出码沿用任务 09 的 `MS_EXIT_*` 表，不新增常量；REPL 正常结束（EOF）一律 `MS_EXIT_OK`。

### 3. `-e` 代码片段执行

- 直接调用 `msEvalStringAs(L, snippet, "<command-line>")`（09-c-api §4），不做表达式回显（与 Python `-c` 一致；回显是 REPL 专属行为）。
- `MsResult` 按任务 09 的表映射退出码；错误经 `msErrorGet` 取消息写 stderr，chunk 名 `<command-line>` 出现在错误位置信息中。
- 片段后面的参数照常进入 `os.args`（见第 7 节）。

### 4. REPL 引擎

```c
struct MsReplConfig {
  const char* prompt;       // 主提示符，">>> "
  const char* contPrompt;   // 续行提示符，"... "
  size_t historyMax;        // 历史条数上限，0 表示不记录
  const char* historyPath;  // 历史文件路径；NULL 表示不持久化
};

struct MsRepl {
  MsState* L;               // 解释器状态，调用者持有，REPL 不拥有
  struct MsReplConfig config;
  FILE* in;                 // 输入流（可以是管道）
  FILE* out;                // 回显与表达式结果输出流
  bool interactive;         // in 与 out 均为 TTY 时为 true
  char** history;           // msAlloc 动态数组，元素各为 msAlloc 的 NUL 结尾串
  size_t historyCount;
  size_t historyCap;
  char* buffer;             // 当前多行输入的累积缓冲区（msRealloc 增长）
  size_t bufferLen;
  size_t bufferCap;
};

void msReplInit(struct MsRepl* repl, MsState* L, FILE* in, FILE* out,
    const struct MsReplConfig* config);
void msReplDestroy(struct MsRepl* repl);

// Runs the read-eval-print loop until EOF on in. Returns a process exit
// code: MS_EXIT_OK on EOF; MS_EXIT_OOM if any allocation fails fatally.
// Per-input errors (syntax/runtime) are reported to stderr and the loop
// continues.
int msReplRun(struct MsRepl* repl);
```

主循环：

1. `interactive` 为真时向 `out` 打印提示符（首行 `prompt`，续行 `contPrompt`）并 flush；非 TTY（脚本化 stdin）一律不打印提示符，保证 stdout 可与 `.out` 同伴文件精确比对。
2. 读入一行（`fgets` 到累积缓冲区追加）。EOF 发生在空缓冲区时退出循环；发生在非空缓冲区时按「输入完整」处理一次后退出。
3. 每读入一行后做完整性判定（第 5 节）：不完整则回到第 1 步打续行提示符继续读；完整则求值。
4. 求值（第 6 节）：编译失败（`MS_ERROR_SYNTAX`）或运行失败（`MS_ERROR_RUNTIME`）均经 `msErrorGet` 打印到 stderr 并继续循环，REPL 不因单次输入错误退出。
5. 非空且非纯空白的完整输入在求值前加入历史（无论求值成败）。

行编辑说明：v0.1 的行读取基于 `fgets` 行缓冲，不提供光标编辑与方向键回调——原始模式终端输入属平台相关代码，依 [10-c-style.md](../language/10-c-style.md) §9 只能放在 `src/platform/`，而平台抽象层是任务 42（v0.3）。本任务交付的「历史记录」为：会话内历史容器 + 历史文件持久化 + 可供查询的内部 API（`ms_repl.c` 内）；方向键回调在任务 42 落地后接入同一历史容器。此与 §3「历史记录」特性的差距在此显式承认。

### 5. 多行输入完整性判定

规范要求「花括号未闭合时续行」。实现不复用手写括号计数，而是复用任务 03 的 lexer 对累积缓冲区做一次完整扫描，统计定界符配对深度：

```c
// Returns true when the accumulated input has no unclosed
// '(' '[' '{' and no lexer-level error that implies incompleteness.
static bool msReplInputComplete(const char* source, size_t len);
```

算法：

1. 用一次性 `struct MsDiagList` 初始化 lexer 扫描整个缓冲区，逐 token 维护深度计数（三个定界符合计一个深度即可：任一开符号 +1，对应闭符号 -1，不校验配对种类——种类错配是语法错误，交给编译器报告）。
2. 扫描结束（EOF token）后深度 > 0 → 不完整（续行）；深度 ≤ 0 → 完整。
3. 出现词法诊断（如未闭合字符串 E103、非法字符）→ 视为完整，让错误在求值阶段以编译错误形式报告，避免 REPL 卡死在永远「不完整」的输入上。
4. 闭符号多于开符号导致深度为负时立即视为完整（同理由 3）。

仅定界符驱动续行：行尾是二元运算符、`,`、`.` 等情形（01-lexical §7 的分号插入续行规则）在 REPL 中不触发续行，按完整输入编译并报语法错误。这与 §3 的「花括号未闭合时续行」字面要求一致，属有意取舍（Python REPL 同此行为），在此显式记录。

### 6. 表达式回显与 `_` 绑定

机制使用 [08-vm-internals.md](../language/08-vm-internals.md) §2.2 既定的 `MS_OP_PRINT_EXPR`：

- REPL 求值入口 `static MsResult msReplEval(struct MsRepl* repl, const char* source, size_t len)` 以 REPL 模式编译累积输入：调用任务 07 的编译入口并置 `MS_COMPILE_REPL` 选项位（假定名，实现时以任务 07 文档为准）。该模式下，顶层**最后一个表达式语句**编译为「求值 + `MS_OP_PRINT_EXPR`」而非「求值 + 弹栈丢弃」；其余语句不变。
- `MS_OP_PRINT_EXPR` 的 VM 语义（任务 08 实现时对齐本文）：弹出一个值；若值非 nil，将其默认字符串表示（v0.1 与 `print` 的 str 表示相同，repr/str 未分化）加换行写入 stdout；随后将该值（含 nil 时不写 `_`，见下）存入全局命名空间的 `_`。nil 值既不打印也不改写 `_`（与 CPython displayhook 对齐）。
- `_` 是普通全局变量：脚本可读写，REPL 输入里的语句显式给 `_` 赋值时以脚本为准，下一次表达式回显再覆盖。
- `-e` 与脚本模式不置 `MS_COMPILE_REPL`，无回显、不写 `_`。

### 7. 脚本参数暴露（`os.args` 占位）

os 标准库模块在任务 36（v0.2），且其依赖的模块系统在任务 24。本任务以最小占位方式提前暴露参数：

```c
// src/cli/main.c（static）
// Builds the placeholder `os` global: a dict {"args": [str, ...]}.
// args[0] is the script path ("-e" for snippet mode, "" for REPL);
// the remaining elements are the CLI arguments after it.
static MsResult msCliExposeArgs(MsState* L, const struct MsCliOptions* opts, int argc, char** argv);
```

- 用 `msNewList` / `msNewString` / `msListAppend` 构造 list，用 `msNewDict` / `msDictSet` 包一层 dict，经 `msSetGlobal(L, "os", dict)` 写入全局命名空间；在 `msNewState` 之后、求值之前调用。C 局部对象遵守根栈纪律（09-c-api §3）。
- 三种模式的 `args[0]`：脚本模式为脚本路径原样；`-e` 模式为字符串 `"-e"`；REPL 为空串 `""`（对齐 Python `sys.argv` 约定）。
- **占位期间脚本侧访问路径是 `os["args"]`（dict 下标），不是 `os.args`**：dict 不定义属性访问语法。本文与对应测试均以 `os["args"]` 为准。任务 36 交付真正的 `os` 模块后，占位全局被移除，`os.args` 成为正式访问路径，届时需同步修改本任务留下的 `tests/ms/cli/*args*.ms` 用例。此切换点在此显式注明。

### 8. `run_tests.py` 同伴文件扩展

为驱动 REPL 与 `-e` 测试，`run_tests.py`（任务 09 建立，本任务扩展，Python 代码继续遵守 PEP 8 + 全量类型标注 + `X | Y` 联合语法）在既有 `.out` / `.exit` 同伴文件之外新增三种：

| 同伴文件 | 含义 |
|---|---|
| `<name>.stdin` | 存在时不传脚本路径，改为把文件内容喂给 `mslang` 的 stdin（REPL 模式） |
| `<name>.cliargs` | 存在时其内容按行（shlex 切分）完整构成 `mslang` 之后的 argv，脚本路径不再自动附加（用于 `-e`、带参脚本） |
| `<name>.env` | `KEY=VALUE` 行，注入子进程环境（如 `MSLANG_HISTORY` 指向临时历史文件） |

三种可组合；`.out`（比对 stdout 全文）与 `.exit`（断言退出码）语义不变。stderr 不比对（错误消息格式属实现细节），负向用例以退出码断言。

## 实现步骤

1. 扩展 `run_tests.py`：新增 `.stdin` / `.cliargs` / `.env` 同伴文件支持。验证：手工构造一个 `.stdin` 用例（空输入即 EOF），`mslang` 无参启动后读到 EOF 以退出码 0 结束，驱动器判通过。
2. 重构 `src/cli/main.c` 参数解析为 `msCliParseArgs`（第 2 节）：`-e`、`--help`、`test` 占位提示、脚本参数区截断。验证：`mslang -e "print(1+1)"` 输出 `2` 退出码 0；`mslang -e` 缺操作数退出码 64；任务 09 的全部脚本/退出码用例不回归。
3. 实现 `msCliExposeArgs` 占位（第 7 节）。验证：带参脚本打印 `os["args"]` 与 `.out` 比对一致，三种模式 `args[0]` 正确。
4. 建 `src/cli/ms_repl.{h,c}`：`struct MsRepl`、累积缓冲区管理（`msAlloc/msRealloc/msFree`）、`fgets` 行读取、TTY 判定与提示符门控（第 4 节）。验证：单行输入的 `.stdin` 用例通过（如 `print("hi")` 一行 + EOF）。
5. 实现 `msReplInputComplete`（第 5 节，lexer 复用）。验证：未闭合 `{`/`[`/`(` 的多行用例、未闭合字符串立即报错不续行、深度为负立即报错。
6. 接入 REPL 模式编译与 `MS_OP_PRINT_EXPR`（第 6 节，与任务 07/08 实现对齐假定接口）。验证：`1 + 1` 回显 `2`；`print(1)`（返回 nil）不回显；`_` 绑定与 nil 不改写 `_` 的用例。
7. 历史记录：会话内容器（去重连续重复、超上限截断）+ 历史文件加载/保存（默认 `$HOME/.mslang_history`，`HOME` 缺失时退回 `USERPROFILE`；环境变量 `MSLANG_HISTORY` 覆盖，置空串则禁用）。验证：`.env` 注入临时历史路径的两用例：首次运行写文件、二次运行加载。
8. `main.c` 接线 REPL 模式：无位置参数时构造 `MsReplConfig` 并 `msReplRun`，退出码透传。验证：任务 09 冒烟用例 + 本任务全部新增用例经 `python run_tests.py` 与 `ctest --test-dir build` 通过。
9. 三平台（Win/Linux/macOS）× Debug/Release 构建验证；Debug（ASAN / `/RTC`）下无内存错误与泄漏（REPL 历史与缓冲区在 `msReplDestroy` 全数释放）。

## 测试方案

本任务晚于任务 09，一律用 ms 脚本测试（`tests/ms/`，任务 40 之前用内建 `assert` + `print` 断言）；REPL 与 `-e` 用例经上节扩展的同伴文件由 `run_tests.py` 驱动。本文档只描述测试方案，实体文件随实现编写。所有用例位于 `tests/ms/cli/`。

| 用例 | 同伴文件 | 覆盖点 |
|---|---|---|
| `repl_smoke.ms` | `.stdin` `.out` | 无参进入 REPL；单行表达式回显（`1 + 1` → `2`）；`print` 正常输出；EOF 退出码 0；非 TTY 无提示符污染 stdout |
| `repl_multiline.ms` | `.stdin` `.out` | 未闭合 `{`（dict 字面量）、`[`（list）、`(`（调用参数）跨行续读后正确求值；嵌套定界符；闭合后回显 |
| `repl_multiline_abort.ms` | `.stdin` `.out` `.exit`(0) | 未闭合字符串不触发续行、按完整输入报语法错误（stderr 不比对）；深度为负（多余 `}`）同理；REPL 报错后继续接受后续输入 |
| `repl_underscore.ms` | `.stdin` `.out` | `1 + 2` 后 `_` 为 `3`；`_ * 10` → `30`；返回 nil 的语句（`print` 调用）不回显且不改写 `_`；脚本显式写 `_` 后被下次回显覆盖 |
| `repl_error_recovery.ms` | `.stdin` `.out` `.exit`(0) | 语法错误行、运行时错误行（如 `1 / 0` 若已定语义，或 `assert(false)`）后 REPL 继续；后续表达式回显正常 |
| `repl_history.ms` | `.stdin` `.env` | `MSLANG_HISTORY` 指向临时文件：会话内输入写入历史文件；驱动器运行两次，第二次进程启动加载既有历史（经历史文件内容断言，由驱动器比对文件存在且非空） |
| `eval_basic.ms` | `.cliargs`(`-e` + 片段) `.out` | `-e "print(1+1)"` 输出 `2`；片段中无回显（`1+1` 片段无输出） |
| `eval_error.ms` | `.cliargs` `.exit` | 语法错误片段退出码 2；`assert(false)` 片段退出码 1；缺操作数退出码 64 |
| `script_args.ms` | `.cliargs`(路径 + `a` `b c`) `.out` | 脚本打印 `os["args"]` 长度与逐元素内容：含路径为 `args[0]`、含空格参数原样保留 |
| `eval_args.ms` | `.cliargs`(`-e` 片段 + `x` `y`) `.out` | `-e` 模式 `os["args"][0] == "-e"`，尾随参数就位 |
| `repl_args.ms` | `.stdin` `.out` | REPL 模式 `os["args"][0] == ""` |
| `test_command_stub.ms` | `.cliargs`(`test` `./...`) `.exit`(64) | `mslang test` 给出「尚未支持」提示并以用法错误退出 |

覆盖点汇总：三种模式分派、`-e` 全分支、REPL 回显/续行/错误恢复/`_`/历史、args 三模式、退出码、非 TTY 行为。`--version` / `--help` 已由任务 09 覆盖或在 `eval_error` 同批断言（退出码 0 + 输出非空，由驱动器 `.out` 比对）。

## 验收标准

- [ ] `mslang`（无参数）进入 REPL：脚本化 stdin 驱动下表达式回显、`print` 输出、EOF 以退出码 0 结束，且 stdout 无提示符污染。
- [ ] REPL 多行输入：`{`/`[`/`(` 未闭合时以 `... ` 续行（交互模式），闭合后整体求值；未闭合字符串与多余闭符号不续行、报编译错误后 REPL 继续。
- [ ] 上次结果绑定 `_`：非 nil 表达式结果打印并写入全局 `_`；nil 不打印、不改写 `_`。
- [ ] 历史记录：会话内历史去重与上限截断；历史文件在退出时保存、启动时加载；`MSLANG_HISTORY` 环境变量覆盖路径、置空禁用。
- [ ] `mslang -e "print(1+1)"` 输出 `2`、退出码 0；`-e` 无回显；语法/运行时错误分别退出码 2/1；缺操作数与未知选项退出码 64；`mslang test` 提示尚未支持并退出码 64。
- [ ] 脚本参数经占位 `os` 全局暴露：`os["args"][0]` 为脚本路径 / `"-e"` / `""`，尾随参数原样（含空格）；文档与测试均已注明任务 36 完成后切换到正式 `os.args`。
- [ ] `src/cli/ms_repl.h` guard 为 `MSLANG_SRC_CLI_MS_REPL_H_`，头文件自包含；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc/msRealloc/msFree`）。
- [ ] `run_tests.py` 支持 `.stdin` / `.cliargs` / `.env` 同伴文件且遵守仓库 Python 规范；`python run_tests.py` 与 `ctest --test-dir build` 全数通过，任务 09 既有用例无回归。
- [ ] 三平台 Debug/Release 构建通过，Debug（ASAN / `/RTC`）无内存错误与泄漏；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 07/08 的假定接口（`MS_COMPILE_REPL`、`MS_OP_PRINT_EXPR` 语义）在实现时已对齐对应任务文档。
