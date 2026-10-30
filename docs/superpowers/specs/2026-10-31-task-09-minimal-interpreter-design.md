# 任务 09 最小可运行解释器实现设计

## 目标与范围

依据 `docs/tasks/09-minimal-interpreter.md`，把现有 Lexer、Parser、Compiler 和 VM 接通为可独立
运行的 `mslang` 命令行解释器。交付 `msEvalFile`、临时内建 `print`/`assert`、脚本与版本两种 CLI
用法、脚本测试驱动和 CTest 集成。

本任务只验证顶层语句、全局声明、算术表达式、`if`/`for` 和位置参数调用。不提前实现完整内建
体系、REPL、`-e`、脚本参数、`mslang test`、异常对象、模块系统或 GC。

## 方案选择

### 采用：薄层端到端集成

`msEvalFile` 作为公开求值入口，复用任务 01–08 的模块和 `MsState` 生命周期。内建、求值管线、CLI
和测试驱动各自保持单一职责，改动最小且符合后续嵌入场景。

### 未采用：提前抽象通用求值管线

同时实现 `msEvalStringAs` 可减少未来 REPL 的少量重复，但会扩大公开 API 和测试范围。本任务只
保留文件求值入口，待任务 22 确有调用方时再抽象源码求值。

### 未采用：CLI 内直接编译执行

把管线写入 `main.c` 虽能减少一个内部入口，却会绕过公开 C API、耦合进程逻辑与语言运行时，也不
利于错误和资源生命周期测试，因此不采用。

## 文件与模块边界

- `include/mslang/state.h`：增加公开 `msEvalFile` 声明，继续隐藏 `MsState` 布局。
- `include/mslang/error.h` / `include/mslang/object.h`：提供最小错误读取和错误字符串转换接口，
  使 CLI 无需包含 `src/vm/ms_state_internal.h`。
- `src/vm/ms_state.c` / `ms_state_internal.h`：复用现有状态、全局表和错误槽；补充求值所需的
  内部错误写入能力，不建立第二套状态或错误机制。
- `src/vm/ms_eval.c`：实现文件读取和源码到顶层 Proto 的编译、执行、资源回收管线。
- `src/vm/ms_builtin.h` / `ms_builtin.c`：实现并注册最小 `print` 和 `assert`。
- `src/cli/main.c`：参数校验、文件预检、状态生命周期、stderr 报告及进程退出码映射。
- `run_tests.py`：递归发现脚本，执行 CLI，检查 `.exit` 与 `.out` 同伴文件并汇总差异。
- `tests/ms/smoke/`：正向冒烟脚本与负向退出码脚本。
- `CMakeLists.txt`：注册新库源文件，并把脚本测试驱动加入 CTest。

## 求值管线与资源所有权

`msEvalFile(L, path)` 校验公开参数并清除上一次错误，然后依次执行：

1. 以二进制模式读取完整文件，显式追加 NUL；空文件和内含 NUL 的源码按既有 Lexer 长度语义处理。
2. 用路径初始化固定容量为 20 的 `MsDiagList`，再初始化借用源码缓冲区的 Lexer 和 AST arena。
3. 由 Parser 生成 `MS_AST_SOURCE_FILE`，再由 Compiler 生成模块顶层 `MsProto`。
4. 在主协程上以零参数调用 `msCallProto`，成功后丢弃顶层 nil 返回值并返回 `MS_OK`。
5. 任一步失败都保留对应 `MsResult`，按获取逆序释放 Proto、Compiler、arena、Lexer、诊断列表和
   源码缓冲区。

源码和 AST 只存活到编译结束；Proto 由求值入口持有至 VM 返回，随后由 `msProtoFree` 递归释放。
Proto 中的字符串和数字常量由 `MsState.heap` 持有。当前尚无 GC，因此不新增根栈或状态字段；
主协程在 `msCallProto` 返回后不再引用顶层 Proto。

CLI 的文件预检负责把不存在或不可读映射为 `MS_EXIT_IO`。直接调用公开 `msEvalFile` 时，同类读取
失败转为带 OS 消息的 `MS_ERROR_RUNTIME`，因为 `MsResult` 没有 I/O 枚举。

## 顶层 Proto 约定

复用编译器已有模块上下文，确认而非重写以下行为：

- 模块名为 `<main>`，源文件名为传入路径，参数数为 0，异步和可变参数标志均为 false。
- 顶层 `:=` 生成 `MS_OP_STORE_GLOBAL`，不占局部窗口。
- 顶层末尾隐式生成 `LOAD_NIL; RETURN`。
- 顶层 `if`、三部分 `for`、声明、赋值和算术表达式沿用任务 07/08 已实现的指令与 VM 语义。

若冒烟测试暴露上述既有路径的集成缺陷，只做任务 09 验收所需的最小修正，并补充相应回归覆盖；
不借此实现后续任务语法。

## 最小内建

`msNewState` 在主协程初始化后调用最小注册函数。每个内建先由 heap 创建名称字符串和
`MS_TYPE_C_FUNCTION` 对象，再通过 `msStateSetGlobal` 写入全局表。任一步 OOM 都写入错误槽；
`msNewState` 清理部分初始化的状态并返回 `NULL`。既有 `msCloseState` 已能回收这些对象和部分全局表。

`print` 接受任意个数的位置参数，以单个空格分隔并追加换行，返回 nil。本任务支持 nil、bool、int、
float 和 string 的默认标量表示；字符串直接输出其 UTF-8 字节。其他类型使用稳定的类型名占位，
不提前实现实例 `__str__` 协议或任务 10 的关键字参数。

`assert` 接受一个必选条件和一个可选消息。条件使用 `msObjectIsTruthy`；为真返回 nil，为假返回
`NULL` 并写 `MS_ERROR_RUNTIME`。未提供消息时使用 `assertion failed`，提供时输出其默认字符串表示。
参数个数不为 1 或 2 时报告运行时错误。VM 现有 C 函数调用路径负责补充源码位置并传播失败。

## 错误读取与诊断

错误槽继续保存 `MsString*` 和 `MsResult`。增加与 C API 规范一致的最小接口：

```c
MsObject* msErrorGet(MsState* L);
const char* msAsCString(MsState* L, MsObject* obj);
```

`msErrorGet` 返回当前错误对象并清除错误槽；对象仍由 heap 持有，调用方不得释放。`msAsCString`
在本任务只接受字符串对象，返回其内部 NUL 终止缓冲区；错误对象保证满足该约束。公开参数无效时
返回 `NULL` 并设置运行时错误，不暴露结构体布局。

词法、语法和编译失败时，求值入口用 `msDiagFormat` 将全部诊断按原顺序合并为换行分隔的错误字符串，
结果码为 `MS_ERROR_SYNTAX`；诊断收集或格式化所需内存失败时返回 `MS_ERROR_OOM`。运行时错误沿用
VM 已写入的位置和消息。CLI 先保存 `msEvalFile` 的结果码，再取错误并输出到 stderr，避免清除错误槽
后丢失退出码分类。

## CLI 行为与退出码

唯一支持的成功形式为 `mslang script.ms` 和 `mslang --version`。版本输出沿用
`mslang <major>.<minor>.<patch>`。无参数、多余参数、`-e`、`test` 和其他选项均输出用法到 stderr；
对已知但未实现的选项附加「尚未支持」说明。

CLI 在创建状态前用 `fopen(path, "rb")` 预检并立即关闭文件。状态创建失败固定映射为 OOM；求值结束
后关闭状态，再返回映射后的退出码：

- `MS_OK` → 0；`MS_ERROR_RUNTIME` → 1；`MS_ERROR_SYNTAX` → 2；`MS_ERROR_OOM` → 3。
- 用法错误 → 64；文件预检失败 → 74。
- 所有错误消息写 stderr，脚本标准输出不被 CLI 改写。

## 脚本测试设施

保留 `run_tests.py` 的 `--mslang` 和 `--filter`，增加 `MSLANG_BIN` 环境变量，优先级为命令行参数、
环境变量、`build/` 默认候选。所有函数保留完整类型标注，子进程不依赖 shell。

每个 `tests/ms/**/*.ms` 的期望退出码默认为 0，可由 `<name>.exit` 中的单个整数覆盖；存在
`<name>.out` 时，按字节比较完整 stdout。失败报告包含相对脚本名、预期/实际退出码、stdout 差异及
stderr，任一失败使驱动返回 1。CTest 通过 `$<TARGET_FILE:mslang-cli>` 显式传入当前配置的解释器，
避免多配置生成器误选其他构建产物。

初始脚本包括：

- `hello.ms` / `hello.out`：输出 `hello`。
- `arithmetic.ms` / `arithmetic.out`：断言优先级、全局变量、除法和取模，输出 `arithmetic ok`。
- `control_flow.ms` / `control_flow.out`：用 `if` 和三部分 `for` 累加并断言，输出
  `control flow ok`。
- `syntax_error.ms` / `syntax_error.exit`：预期退出码 2。
- `assert_failure.ms` / `assert_failure.exit`：预期退出码 1。

另外直接调用 CLI 验证不存在文件、用法错误和 `--version`。CTest 脚本层覆盖所有 `.ms` 用例，既有
C 单元测试继续负责内部模块回归。

## 实施与验证

先创建脚本及驱动测试并观察当前 CLI 的预期失败，再按内建注册、求值管线、CLI、CTest 的顺序小步
实现，每步运行对应测试。最终验证：

1. `python run_tests.py` 全部通过，并逐项检查 CLI 退出码、stdout/stderr。
2. 严格警告的 Debug 构建和完整 `ctest --test-dir build -C Debug --output-on-failure`。
3. Release 构建和完整 `ctest --test-dir build -C Release --output-on-failure`。
4. 关闭状态后的既有分配统计断言保持通过，`git diff --check` 无格式错误，产物仅位于 `build/`。

当前环境只能直接验证 Windows；Linux/macOS 的构建与运行不能声称已实测。全部实际可执行验收通过
后，再勾选任务 09 对应项并更新任务索引状态，未实测的平台项目保持明确说明。
