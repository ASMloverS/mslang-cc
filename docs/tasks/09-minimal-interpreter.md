# 09 最小可运行解释器（里程碑）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [07 编译器](07-compiler.md)、[08 VM 执行核心](08-vm-core.md) |

## 任务目标

将任务 01–08 产出的全部部件端到端接通，得到一个可独立运行的 `mslang` 可执行文件：

```bash
mslang script.ms        # 编译并顺序执行脚本顶层语句
```

本里程碑要求跑通的能力下限：

- `print("hello")` —— 顶层调用临时内建函数并输出到 stdout；
- 顶层变量声明与算术表达式求值：`x := 1 + 2 * 3`、`print(x)` 输出 `7`；
- 顶层控制流（`if` / `for`）驱动的语句执行与打印。

本任务是第一个引入 ms 脚本测试的任务：从本任务起，后续任务一律改用 `tests/ms/` 下的 ms 脚本做语言级验证（C 单元测试仍保留给各内部模块）。完整的内建函数体系、REPL、`-e` 代码片段执行与 `mslang test` 子命令不在本任务范围（见 [11-project-layout.md](../language/11-project-layout.md) §3 与路线图 v0.1）。

## 设计依据

- [00-overview.md](../language/00-overview.md) §1（独立解释器定位）、§3（顶层语句即程序，无入口函数）。
- [08-vm-internals.md](../language/08-vm-internals.md) §1（编译管线：源码 → Lexer → Parser → AST → Compiler → MsProto → VM）、§2.1（`MsProto` 布局，模块顶层编译为一个 Proto）、§4（VM 执行核心：调用栈/求值栈）。
- [09-c-api.md](../language/09-c-api.md) §4（state.h：`msNewState` / `msCloseState` / `msEvalFile` / `MsResult`）、§8（错误处理约定）、§11（嵌入示例的错误报告方式）。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、include guard、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [11-project-layout.md](../language/11-project-layout.md) §1（`src/cli/` 与 `tests/ms/` 位置）、§2（`mslang-cli` 构建目标）、§3（CLI 行为）、§4（三层测试策略）。

## 详细设计

### 1. 端到端数据流

```
script.ms 文件
  → CLI 读入路径，调用 msEvalFile(L, path)
  → msEvalFile 内部串联已有模块：
      msLexerInit / msLexerNext        (任务 03)
      → msParserRun → AST              (任务 04)
      → msCompilerCompile → MsProto    (任务 07，模块顶层 Proto)
      → msVmRun / msCallProto          (任务 08，主协程上执行顶层 Proto)
  → 顶层语句副作用：临时内建 print 写 stdout
  → MsResult 返回 CLI，CLI 映射为进程退出码
```

各环节的错误统一沿 `MsResult` 向上传播，错误详情（含文件/行/列）挂到 `MsState` 的错误槽，由 CLI 取出打印到 stderr（对齐 [09-c-api.md](../language/09-c-api.md) §11 的 `msErrorGet` 用法；错误槽/报告机制在异常系统落地前以最简形式实现，见任务 02 基础设施）。编译阶段沿用「单文件最多报告 20 个错误后中止」的收集策略（[08-vm-internals.md](../language/08-vm-internals.md) §1）。

### 2. MsState 的创建/销毁与最小职责

接口沿用 [09-c-api.md](../language/09-c-api.md) §4，本任务不要求实现 `msNewStateWithConfig`（可留到配置项出现时再补）：

```c
// include/mslang/state.h
#ifndef MSLANG_INCLUDE_MSLANG_STATE_H_
#define MSLANG_INCLUDE_MSLANG_STATE_H_

MsState*  msNewState(void);
void      msCloseState(MsState* L);
MsResult  msEvalFile(MsState* L, const char* path);

#endif  // MSLANG_INCLUDE_MSLANG_STATE_H_
```

本任务中 `MsState` 的最小职责（其余字段随 GC、调度器任务逐步填充）：

- **全局命名空间**：一张 `名字 → MsObject*` 的表，承载临时内建（`print`、`assert`）与脚本顶层 `:=` 声明的全局变量；`MS_OP_LOAD_GLOBAL` / `MS_OP_STORE_GLOBAL` 在此表上读写。
- **主协程**：持有一个主协程（调用栈 + 求值栈，任务 08 产物），`msEvalFile` 在其上执行顶层 Proto；本任务不创建额外线程与调度器。
- **错误槽**：保存最近一次失败的消息对象（含文件/行/列），供 CLI 经 `msErrorGet` 取出。
- **分配统计**：所有堆内存经 `msAlloc`/`msRealloc`/`msFree`（任务 02），`MsState` 记录分配计数，为 GC 阈值触发留口。

`msNewState` 流程：分配并零初始化 `MsState` → 初始化全局命名空间 → 挂接临时内建（见第 4 节）→ 初始化主协程。`msCloseState` 按获取的逆序释放：销毁协程 → 释放全局命名空间与全部存活对象（本任务允许采用「关闭时遍历全对象链表全部回收」的简化策略，正式 GC 在后续任务落地）→ 释放 `MsState` 本体。

### 3. 模块顶层编译为 MsProto 的约定

- 整个脚本文件的顶层语句编译为**一个** `MsProto`（对齐 [08-vm-internals.md](../language/08-vm-internals.md) §1「每个函数（含模块顶层）编译为一个 MsProto」）。
- 顶层 Proto 的字段约定：`paramCount = 0`、`isAsync = false`、`hasVarArgs = false`、`hasKwArgs = false`；`name` 为 `"<main>"`，`sourceFile` 为脚本路径；指令流末尾保证以 `MS_OP_RETURN`（返回 nil）结束，由编译器在生成时补齐。
- 顶层 `:=` 声明的变量一律走全局命名空间（`MS_OP_STORE_GLOBAL`），不占用顶层 Proto 的 `localCount` 窗口；本任务无嵌套函数时的 `localCount` 可为 0。
- 执行约定：VM 把顶层 Proto 当作一次零参函数调用，在主协程上压帧执行；`MS_OP_RETURN` 弹出帧后执行结束。编译产物 MsProto 由 `MsState` 持有引用，防止执行期间被回收。

### 4. 临时内建 print 的挂接方式

完整内建函数体系在任务 10 实现；本任务只挂接 `print` 与 `assert` 两个临时内建，机制即为正式机制的最小样例：

- 以 `MS_TYPE_C_FUNCTION` 类型的 C 函数对象实现（签名对齐 [09-c-api.md](../language/09-c-api.md) §9 的 `MsCFunction`）：

```c
// src/vm/ms_builtin.c（任务 10 将扩展为完整内建模块）
static MsObject* msBuiltinPrint(MsState* L, int64_t argc, MsObject** argv);
static MsObject* msBuiltinAssert(MsState* L, int64_t argc, MsObject** argv);

void msBuiltinRegisterMinimal(MsState* L);  // 由 msNewState 调用，写入全局命名空间
```

- `print` 语义：将各参数按默认字符串表示以单个空格连接写入 stdout，末尾换行；返回 nil。
- `assert` 语义：首参数按真值规则判断，为假则以错误消息（第二参数可选）失败，返回 `NULL` 并置错误槽，VM 将其转为执行失败（本任务尚无异常对象，`MsResult = MS_ERROR_RUNTIME`）；为真返回 nil。
- 挂接时机：`msNewState` 内调用 `msBuiltinRegisterMinimal`，将两个 C 函数对象以 `"print"` / `"assert"` 为名写入全局命名空间，脚本侧经 `MS_OP_LOAD_GLOBAL` 解析到。

### 5. CLI 入口（src/cli/main.c）

`mslang-cli` 可执行目标（[11-project-layout.md](../language/11-project-layout.md) §2）的唯一源文件，本任务只支持两种用法：

```bash
mslang script.ms        # 执行脚本
mslang --version        # 打印版本（MS_VERSION_MAJOR/MINOR/PATCH）
```

参数处理流程：

1. `argc` 校验：无参数或多于一个位置参数 → 打印用法到 stderr，退出 `MS_EXIT_USAGE`。REPL、`-e`、`test` 子命令暂不实现，遇到时同样按用法错误处理并注明「尚未支持」。
2. 文件预检：以 `fopen(path, "rb")` 探测脚本存在且可读，失败打印 OS 原因到 stderr，退出 `MS_EXIT_IO`（文件 I/O 属 CLI 职责，不占用 `MsResult` 枚举）。
3. `MsState* L = msNewState()`；失败（OOM）退出 `MS_EXIT_OOM`。
4. `MsResult result = msEvalFile(L, path)`；非 `MS_OK` 时经 `msErrorGet` 取错误消息打印 stderr。
5. `msCloseState(L)`，按下表映射退出码。

退出码约定（`src/cli/main.c` 内定义，对齐 [09-c-api.md](../language/09-c-api.md) §4 的 `MsResult`；OS/用法类退出码借用 BSD sysexits 取值）：

| 退出码 | 常量 | 对应 MsResult | 含义 |
|---|---|---|---|
| 0 | `MS_EXIT_OK` | `MS_OK` | 成功 |
| 1 | `MS_EXIT_RUNTIME` | `MS_ERROR_RUNTIME` | 运行时错误（含未捕获异常、`assert` 失败） |
| 2 | `MS_EXIT_SYNTAX` | `MS_ERROR_SYNTAX` | 词法/语法/编译错误 |
| 3 | `MS_EXIT_OOM` | `MS_ERROR_OOM` | 内存不足（含 `msNewState` 失败） |
| 64 | `MS_EXIT_USAGE` | —（CLI 层） | 用法错误 |
| 74 | `MS_EXIT_IO` | —（CLI 层） | 脚本文件不存在/不可读等 OS 错误 |

## 实现步骤

1. 在 `include/mslang/state.h` 落定 `msNewState` / `msCloseState` / `msEvalFile` 声明，并在 `src/` 内部头文件中给出本任务所需的 `MsState` 最小结构（全局命名空间、主协程、错误槽、分配统计）。
2. 实现 `src/vm/ms_builtin.c`：`msBuiltinPrint` / `msBuiltinAssert` 两个 `MsCFunction` 与 `msBuiltinRegisterMinimal`。
3. 实现 `msNewState` / `msCloseState`：初始化与逆序销毁，调用 `msBuiltinRegisterMinimal` 挂接临时内建。
4. 实现 `msEvalFile`：读取文件内容 → lexer → parser → compiler → 顶层 MsProto → 主协程执行 → 返回 `MsResult`；每步失败早返回并填充错误槽（含文件/行/列）。
5. 在编译器中确认顶层 Proto 约定（第 3 节）：`name = "<main>"`、末尾补 `MS_OP_RETURN`、顶层 `:=` 走全局命名空间。
6. 实现 `src/cli/main.c`：参数处理、文件预检、退出码映射（第 5 节）；接入 CMake `mslang-cli` 目标并链接 `mslang` 库。
7. 建立 `tests/ms/` 与 `run_tests.py` 的脚本测试设施（见测试方案），跑通冒烟脚本。
8. 全平台（Win/Linux/macOS）× Debug/Release 本地构建验证，Debug 下无 ASAN 报告。

## 测试方案

本任务是第一个使用 ms 脚本测试的任务，建立两层设施（[11-project-layout.md](../language/11-project-layout.md) §4 的脚本测试层；`testing` 标准库模块在 v0.2 才存在，本阶段用内建 `assert` 自断言替代）：

### 1. 冒烟脚本（tests/ms/smoke/）

每个脚本自含断言，成功路径必须以 `print` 输出约定行并以退出码 0 结束：

- `hello.ms`：`print("hello")` —— 验证顶层语句、字符串字面量、临时内建挂接与 stdout 输出。
- `arithmetic.ms`：顶层 `:=` 声明 + 算术/比较表达式，如 `x := 1 + 2 * 3`、`assert(x == 7)`、`assert(10 / 4 == 2.5)`、`assert(7 % 3 == 1)`，末尾 `print("arithmetic ok")` —— 验证常量池、算术指令快路径与全局命名空间。
- `control_flow.ms`：`if` 分支与 `for` 循环驱动累加并断言结果（如 `sum := 0; for i in 1..10 { sum += i }; assert(sum == 55)`，具体语法以任务 03/04 已定稿的词法语法为准），末尾 `print("control flow ok")`。

负向用例（由 `run_tests.py` 按预期退出码驱动）：一个含语法错误的脚本预期退出码 2，一个 `assert(false)` 的脚本预期退出码 1。

### 2. 仓库根目录 run_tests.py

职责（遵守仓库 Python 规范：PEP 8、全量类型标注、`X | Y` 联合语法）：

1. 递归发现 `tests/ms/**/*.ms`；带同伴文件 `<name>.out` 的脚本额外比对 stdout 全文，负向用例以同伴文件 `<name>.exit` 声明预期退出码。
2. 依次以子进程调用 `mslang` CLI（路径由命令行参数或 `MSLANG_BIN` 环境变量指定，默认取 `build/` 下的构建产物），检查退出码与（可选的）stdout。
3. 汇总通过/失败数，逐条列出失败用例的名称、预期/实际退出码与输出差异；任一失败时进程以非零退出。
4. 供 `ctest` 包装调用，使脚本测试并入统一测试入口（[11-project-layout.md](../language/11-project-layout.md) §2 的 `ctest --test-dir build`）。

本文档只描述测试方案；脚本与 `run_tests.py` 的实体文件在实现步骤第 7 步创建。

## 验收标准

- [ ] `mslang tests/ms/smoke/hello.ms` 输出 `hello` 且退出码为 0。
- [ ] `mslang tests/ms/smoke/arithmetic.ms` 断言全部通过，输出 `arithmetic ok`，退出码为 0。
- [ ] `mslang tests/ms/smoke/control_flow.ms` 断言全部通过，输出 `control flow ok`，退出码为 0。
- [ ] 语法错误脚本退出码为 2，`assert(false)` 脚本退出码为 1，不存在的文件退出码为 74，用法错误退出码为 64，且错误信息均输出到 stderr。
- [ ] `python run_tests.py` 发现全部 `tests/ms/**/*.ms` 并全数通过，进程退出码为 0。
- [ ] `ctest --test-dir build` 包含并跑通脚本测试层。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告（`msCloseState` 后无残余分配计数）。
- [ ] `mslang --version` 打印 `MS_VERSION_MAJOR.MINOR.PATCH` 且退出码为 0。
