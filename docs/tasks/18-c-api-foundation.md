# 18 C API 基础与嵌入示例

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [09 最小可运行解释器](09-minimal-interpreter.md)、[17 GC 标记-清除](17-gc-mark-sweep.md) |

## 任务目标

把 [09-c-api.md](../language/09-c-api.md) 定义的公开 C API 从规范落到代码：在 `include/mslang/` 下建立 v0.1 所需的头文件族（`mslang.h` / `state.h` / `object.h` / `container.h` / `call.h` / `error.h` / `gc.h` / `version.h`），实现其中的全部 v0.1 函数——状态生命周期与执行（`msNewState` / `msNewStateWithConfig` / `msCloseState` / `msEvalString` / `msEvalFile` / `msEvalStringAs`）、全局变量读写、值构造与类型转换、list/dict/str 容器操作、函数调用与属性访问、错误状态操作，以及显式根栈（`msRootPush` / `msRootPop`）与 GC 控制（`msGCDisable` / `msGCEnable` / `msGCCollect`）。同时交付 `examples/embed_demo.c`：一个完整的嵌入示例，兼作 C API 的集成测试（[11-project-layout.md](../language/11-project-layout.md) §4 第三层）。

完成后，嵌入方只需 `#include <mslang/mslang.h>` 并链接 `mslang` 库即可执行脚本、与脚本互调；`module.h`（扩展模块注册）与 `ctype.h`（C 自定义类型）属 v0.2 范围，由任务 33 落地，不在本任务。

## 设计依据

- [09-c-api.md](../language/09-c-api.md) §1（命名速查）、§2（头文件布局与不透明类型约定）、§3（对象模型与显式根栈纪律）、§4（state.h 接口与 `MsResult` 枚举）、§5（object.h 值构造与转换）、§6（container.h 容器操作）、§7（call.h 调用与属性）、§8（error.h 错误状态约定）、§11（嵌入完整示例）、§12（线程规则）、§13（版本与兼容）。
- [11-project-layout.md](../language/11-project-layout.md) §1（`include/mslang/` 与 `examples/` 位置）、§2（`embed-example` 构建目标、include path 以 `PUBLIC` 暴露）、§4（嵌入测试作为第三层，CI 中编译并运行）。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [08-vm-internals.md](../language/08-vm-internals.md) §1（编译管线与错误收集策略，`msEvalString` 系列复用同一管线）。
- 任务 09 已交付 `msNewState` / `msCloseState` / `msEvalFile` 的最小实现与 `MsState` 的错误槽、全局命名空间、主协程；本任务将其纳入公开头文件族并补齐其余 API。
- 任务 17（GC 标记-清除）提供 GC 本体与根集标记入口，本任务的根栈 API 挂接到其根集扫描上。任务 17 文档尚不存在，本文对其接口（`struct MsGc`、标记入口 `msGcMarkRoots`、阈值触发字段等）为假定命名，实现时以对应任务文档定名为准；同理，任务 02（`msAlloc` 系列、`MsResult`）、任务 06（`struct MsObject` 内部布局、`MsTypeTag` 取值集合）、任务 16（list/dict 内部实现）的接口名若与本文假定不同，以对齐为准。

## 详细设计

### 1. 头文件族布局与包含关系

```
include/mslang/
├── mslang.h        // 伞头文件：按下列顺序包含 v0.1 全部头文件
├── version.h       // 版本宏与 msVersionString（任务 01 已生成，本任务补 msVersionString 实现）
├── state.h         // MsResult、MsConfig、解释器生命周期、执行、全局变量
├── object.h        // MsTypeTag、值构造、类型判断与转换
├── container.h     // list/dict/str 容器操作
├── call.h          // 调用与属性访问
├── error.h         // 错误状态
└── gc.h            // 根栈与 GC 控制
```

- 每个头文件自包含（自行 include `<stdbool.h>` `<stddef.h>` `<stdint.h>` 等），include guard 形如 `MSLANG_INCLUDE_MSLANG_<文件>_H_`（沿用任务 09 在 `state.h` 上确立的形式）。
- 包含顺序：`mslang.h` 按 `version.h → state.h → object.h → container.h → call.h → error.h → gc.h` 依序包含；各头文件之间允许直接互相 include（如 `object.h` include `state.h` 以获得 `MsState`），靠 guard 防重复。
- 导出符号：公开头文件中的声明一律加 `MS_API` 宏（`version.h` 中定义，共享库构建时展开为 `__declspec(dllexport/dllimport)` 或 `__attribute__((visibility("default")))`，静态构建为空），保证 `MSLANG_BUILD_SHARED=ON` 时符号可见性正确。
- 实现位置：公开函数的实现按职责落在 `src/api/ms_state.c` / `ms_object.c` / `ms_container.c` / `ms_call.c` / `ms_error.c` / `ms_gc.c`（新增 `src/api/` 目录聚合 API 适配层），内部调用任务 06/08/16/17 的模块接口；`MsState` 与 `MsObject` 的完整定义只出现在 `src/` 内部头文件中。

### 2. 不透明类型约定

- 公开头文件中只出现前向声明：

```c
// include/mslang/state.h
typedef struct MsState MsState;

// include/mslang/object.h
typedef struct MsObject MsObject;
```

  说明：[10-c-style.md](../language/10-c-style.md) §4「内部结构体不 typedef」针对 `src/` 内部结构体；公开不透明类型必须以 typedef 前向声明暴露句柄名，是该规则的显式例外（09-c-api §2 要求 `MsState`/`MsObject` 均为不透明类型）。`src/` 内部的完整定义保持 `struct MsState` / `struct MsObject` 不 typedef。
- 公开 API 的签名只使用 C 基本类型（`bool`、`int64_t`、`double`、`size_t`、`const char*`）与不透明指针，不暴露任何结构体布局；唯二例外是纯输入型 POD：`MsConfig`（见下）与 v0.2 的 `MsModuleDef`/`MsMethodDef`/`MsTypeDef`（任务 33）。
- `NULL` 语义：所有返回 `MsObject*` 的 API 失败时返回 `NULL` 且设置错误状态（09-c-api §8）；返回 `MsResult` 的 API 用枚举区分失败类别。

### 3. version.h

任务 01 已生成 `MS_VERSION_MAJOR/MINOR/PATCH` 宏（当前 `0.1.0`）；本任务补齐运行期接口：

```c
// include/mslang/version.h
#define MS_VERSION_MAJOR 0
#define MS_VERSION_MINOR 1
#define MS_VERSION_PATCH 0

// Returns the runtime version string, e.g. "0.1.0". Static storage.
MS_API const char* msVersionString(void);
```

`msVersionString` 实现于 `src/api/ms_state.c`，返回编译期拼好的字符串常量，供嵌入方做编译期（宏）与运行期（函数）一致性检查（09-c-api §13）。

### 4. state.h：状态、配置与执行

```c
// include/mslang/state.h
typedef enum {
  MS_OK = 0,
  MS_ERROR_RUNTIME,
  MS_ERROR_SYNTAX,
  MS_ERROR_OOM
} MsResult;

typedef struct {
  int64_t gcThresholdBytes;  // GC 触发阈值（字节）；<= 0 表示用内部默认值
} MsConfig;

MS_API MsState*  msNewState(void);
// NULL config is equivalent to msNewState(). Fields <= 0 fall back to defaults.
MS_API MsState*  msNewStateWithConfig(const MsConfig* config);
MS_API void      msCloseState(MsState* L);

MS_API MsResult  msEvalString(MsState* L, const char* source);
MS_API MsResult  msEvalFile(MsState* L, const char* path);
MS_API MsResult  msEvalStringAs(MsState* L, const char* source, const char* chunkName);

// Returns the global or nil when absent; the result follows the usual
// ownership rule (root it or consume it before the next allocation).
MS_API MsObject* msGetGlobal(MsState* L, const char* name);
MS_API void      msSetGlobal(MsState* L, const char* name, MsObject* value);
```

要点：

- `MsConfig` 是公开 POD（嵌入方按值填充），v0.1 只含 `gcThresholdBytes` 一个字段，对接任务 17 的 GC 阈值；`NULL` 或字段 `<= 0` 一律取内部默认值。后续版本新增字段时追加到尾部并在文档中记录（首版不承诺 ABI 稳定，09-c-api §13）。
- `msEvalString` 等价于 `msEvalStringAs(L, source, "<string>")`；`chunkName` 写入诊断与错误槽（09-c-api §4）。
- 三个 `msEval*` 复用任务 09 已接通的编译管线：源码 → lexer → parser → compiler → 顶层 `MsProto` → 主协程执行；`msEvalFile` 增加文件读取（读取失败返回 `MS_ERROR_RUNTIME` 并在错误槽写入 OS 原因——文件 I/O 的 `MS_EXIT_IO` 退出码仍是 CLI 层职责，不进 `MsResult` 枚举，与任务 09 一致）。
- `MsState` 内部结构在本任务扩展（`src/vm/ms_state_internal.h`，不 typedef）：在任务 09 的全局命名空间、主协程、错误槽、分配统计之上，新增**根栈字段**（见第 8 节）与 **GC 控制字段**（`gcDisabled` 计数、`gcThresholdBytes`）。
- 线程规则按 09-c-api §12：一个 `MsState` 同一时刻只能被一个 OS 线程操作；v0.1 无线程原语，规则仅以文档与注释形式声明，不做运行期检查。

### 5. object.h：值构造与转换

```c
// include/mslang/object.h
typedef enum {
  MS_TYPE_NIL,
  MS_TYPE_BOOL,
  MS_TYPE_INT,
  MS_TYPE_FLOAT,
  MS_TYPE_STR,
  MS_TYPE_LIST,
  MS_TYPE_DICT,
  MS_TYPE_FUNCTION,     // 脚本函数（MsProto 包装）
  MS_TYPE_C_FUNCTION,   // C 函数对象
  MS_TYPE_CLASS,
  MS_TYPE_INSTANCE
} MsTypeTag;

MS_API MsObject*   msNewNil(MsState* L);
MS_API MsObject*   msNewBool(MsState* L, bool v);
MS_API MsObject*   msNewInt(MsState* L, int64_t v);
MS_API MsObject*   msNewFloat(MsState* L, double v);
MS_API MsObject*   msNewString(MsState* L, const char* utf8);
MS_API MsObject*   msNewStringN(MsState* L, const char* data, size_t len);
MS_API MsObject*   msNewList(MsState* L, int64_t capacity);
MS_API MsObject*   msNewDict(MsState* L);

MS_API MsTypeTag   msTypeOf(MsObject* obj);
MS_API bool        msIsNil(MsObject* obj);
MS_API bool        msAsBool(MsState* L, MsObject* obj);
// Raises TypeError if not convertible.
MS_API int64_t     msAsInt(MsState* L, MsObject* obj);
MS_API double      msAsFloat(MsState* L, MsObject* obj);
// Internal buffer; valid until the next allocation.
MS_API const char* msAsCString(MsState* L, MsObject* obj);
MS_API size_t      msStringLen(MsObject* strObj);
```

范围界定（09-c-api §5 的子集与延后）：

- 本任务**声明并实现**以上函数；`msNewIntFromString`（依赖大整数，任务 31）、`msNewBytes` / `msNewTuple`（任务 32）、`msIsInstance`（依赖继承语义定稿，任务 25）**不在本任务声明**，随对应任务加入头文件，避免「已声明无实现」的悬空 API。
- `MsTypeTag` 取值集合按 v0.1 已存在的类型给出（任务 06/13/15/16 产物）；`MS_TYPE_BYTES` / `MS_TYPE_TUPLE` / `MS_TYPE_SET` 等随任务 32 追加到枚举尾部。
- `msTypeOf` / `msIsNil` / `msStringLen` 不取 `MsState*`：纯查询、不分配、不失败（09-c-api §5 签名如此）。
- `msNewNil` / `msNewBool` 返回单例（nil/true/false 各一，任务 06 的不变对象策略），不触发 GC 分配；其余 `msNew*` 是分配点，返回值遵守所有权语义：要么入根，要么在下一次分配前消费（09-c-api §3）。
- 转换函数的失败约定：`msAsInt` / `msAsFloat` / `msAsCString` 不可转换时设置 `TypeError` 错误状态并返回 `0` / `0.0` / `NULL`，调用方用 `msErrorOccurred` 判定；`msAsBool` 按语言真值规则求值，不失败。
- `msAsCString` 返回对象内部 UTF-8 缓冲（保证 NUL 结尾），有效期到下一次分配为止，调用方不得 `msFree`；`msStringLen` 返回字节长度（可与内嵌 NUL 共存，配合 `msNewStringN` 构造的串）。

### 6. container.h：容器操作

```c
// include/mslang/container.h
MS_API int64_t   msLen(MsState* L, MsObject* container);

MS_API MsObject* msListGet(MsState* L, MsObject* list, int64_t index);
MS_API void      msListSet(MsState* L, MsObject* list, int64_t index, MsObject* v);
MS_API void      msListAppend(MsState* L, MsObject* list, MsObject* v);

// Returns nil when the key is missing.
MS_API MsObject* msDictGet(MsState* L, MsObject* dict, MsObject* key);
MS_API void      msDictSet(MsState* L, MsObject* dict, MsObject* key, MsObject* v);
MS_API bool      msDictContains(MsState* L, MsObject* dict, MsObject* key);
MS_API void      msDictDelete(MsState* L, MsObject* dict, MsObject* key);

MS_API MsObject* msStrConcat(MsState* L, MsObject* a, MsObject* b);
```

- 语义与脚本侧运算符完全对齐（任务 16 的容器实现）：下标越界置 `IndexError`；`msDictGet` 键缺失返回 nil（09-c-api §6），与出错（返回 `NULL`）区分；`msListAppend` / `msDictSet` 是分配点，调用前持有的局部对象必须先入根。
- 负下标：`msListGet` / `msListSet` 支持 `-1` 表末元素的脚本语义（与任务 16 的下标实现一致；若任务 16 定稿不支持负下标，以对齐为准）。
- 参数类型错误（如对非容器调 `msListGet`）置 `TypeError` 并返回 `NULL` / 提前返回。
- `msStrConcat` 等价于脚本 `a + b` 的字符串拼接，两参数均须为 str，否则置 `TypeError`。

### 7. call.h：调用与属性

```c
// include/mslang/call.h
MS_API MsObject* msCallObject(MsState* L, MsObject* callable, int64_t argc, MsObject** argv);
MS_API MsObject* msCallMethod(MsState* L, MsObject* obj, const char* method, int64_t argc, MsObject** argv);

MS_API MsObject* msGetAttr(MsState* L, MsObject* obj, const char* name);
MS_API void      msSetAttr(MsState* L, MsObject* obj, const char* name, MsObject* v);
MS_API bool      msHasAttr(MsState* L, MsObject* obj, const char* name);
```

- `msCallObject` 接受 `MS_TYPE_FUNCTION`（脚本函数，经任务 08 的调用约定在主协程上压帧执行）与 `MS_TYPE_C_FUNCTION`（直接 C 调用）两种可调用对象；不可调用置 `TypeError` 返回 `NULL`。argv 为空参数列表时允许 `NULL`。
- 调用约定保证：**argv 中的实参在调用期间自动是根**（09-c-api §3），返回值在下一次分配前有效；`msCallObject` 本身是分配点，调用方持有的其他局部对象须先入根。
- `msCallMethod` 等价于 `msGetAttr` + 绑定 self + `msCallObject`；属性缺失或不可调用置 `AttributeError`/`TypeError` 返回 `NULL`（v0.1 尚无异常类型体系，错误对象均为字符串消息，见第 8 节，错误类别名仅为消息前缀约定）。
- `msGetAttr` / `msSetAttr` / `msHasAttr` 作用于 class 实例（任务 15）；对无属性模型的类型（int、str 等）调用 `msGetAttr` 置 `AttributeError` 返回 `NULL`，`msHasAttr` 返回 `false` 且不置错误。

### 8. error.h 与错误对象的 v0.1 形态

```c
// include/mslang/error.h
MS_API bool      msErrorOccurred(MsState* L);
// Takes and clears the current exception object.
MS_API MsObject* msErrorGet(MsState* L);
MS_API void      msErrorClear(MsState* L);
MS_API void      msRaise(MsState* L, MsObject* excObj);
MS_API void      msRaiseTypeError(MsState* L, const char* fmt, ...);
MS_API void      msRaiseValueError(MsState* L, const char* fmt, ...);
MS_API void      msRaiseRuntimeError(MsState* L, const char* fmt, ...);
MS_API void      msRaiseOSError(MsState* L, int sysErrno, const char* fmt, ...);
```

- v0.1 异常系统（任务 23）未落地，错误对象是**字符串对象**：`msRaise*` 把格式化的 `"TypeError: <message>"` 形式消息包装为 str 存入错误槽；`msErrorGet` 取出后可用 `msAsCString` 打印（与 09-c-api §11 示例的用法兼容）。任务 23 落地异常类后错误对象替换为异常实例，签名不变。
- 不变式：错误槽非空期间，任何可能执行的 API 在入口检查（调试构建 `MS_ASSERT(!msErrorOccurred(L))` 提示调用方先处理错误）；`msEval*` 与 `msCallObject` 失败时保证错误槽非空。
- `msRaiseOSError` 的 `sysErrno` 保留 `int`（直接对应 C 库 `errno` 约定），是「对外接口用定宽类型」规则的显式例外（09-c-api §8）。
- 错误槽本身持有的对象在 GC 扫描时作为根（属 `MsState` 内部根，不占用户根栈）。

### 9. gc.h 与根栈纪律实现

```c
// include/mslang/gc.h
// Pushes obj onto the explicit root stack; obj stays reachable across GC.
MS_API void msRootPush(MsState* L, MsObject* obj);
// Pops the most recently pushed root, in strict LIFO order.
MS_API void msRootPop(MsState* L);

MS_API void msGCDisable(MsState* L);
MS_API void msGCEnable(MsState* L);
MS_API void msGCCollect(MsState* L);
```

根栈实现（`src/api/ms_gc.c` + `MsState` 内部字段）：

```c
// struct MsState 内部新增（src/vm/ms_state_internal.h，不 typedef）：
//   MsObject** roots;        // 根栈，动态数组
//   size_t     rootCount;
//   size_t     rootCapacity;
```

- `msRootPush`：`rootCount == rootCapacity` 时经 `msRealloc` 倍增扩容（初始容量 8）；扩容失败属 OOM——置错误槽并经 `MS_ASSERT` 在调试构建中止（此时被压对象失去保护，只能快速失败，无法安全继续）。允许压入 `NULL`（作为占位，与 LIFO 配对简化嵌套代码），标记阶段跳过。
- `msRootPop`：严格 LIFO，`rootCount == 0` 时 `MS_ASSERT` 失败（配对错乱是嵌入方编程错误）。
- GC 挂接：任务 17 的标记阶段把根栈 `[0, rootCount)` 整体作为根集扫描（与全局命名空间、主协程求值栈、错误槽并列）；本任务只负责把根栈数据交给任务 17 的根标记入口，不改动 GC 算法。
- `msGCDisable` / `msGCEnable`：内部为计数器（允许嵌套配对），计数大于 0 时抑制阈值触发的自动 GC；`msGCEnable` 减到 0 时若已超阈值则立即触发一次收集。`msGCCollect` 无条件执行一次完整标记-清除（不受 disable 计数抑制）。
- 纪律文档化：在 `gc.h` 头注释中写明 09-c-api §3 的根栈规则（两个可能分配的 API 调用之间存活的局部 `MsObject*` 必须入根；实参自动是根；返回值在下一次分配前有效），`embed_demo.c` 作为示范范本。

### 10. embed_demo.c 嵌入示例（兼集成测试）

`examples/embed_demo.c` 在 09-c-api §11 示例基础上扩展为可自动判定的集成测试，流程：

1. `msVersionString()` 打印并与 `MS_VERSION_MAJOR/MINOR/PATCH` 拼出的编译期字符串比对，不一致退出码 1。
2. `msNewState()` 建状态；`msEvalStringAs` 执行内嵌脚本源码，定义 `fib(n)` 递归函数与全局 `greeting`（覆盖 eval、函数定义与调用、全局命名空间）。
3. `msGetGlobal(L, "fib")` → `msRootPush` → `msNewInt(L, 30)` → `msRootPush` → `msCallObject(L, fib, 1, &arg)` → 两次 `msRootPop`，断言 `msAsInt(L, result) == 832040`，打印 `fib(30) = 832040`（覆盖根栈纪律、调用、转换）。
4. `msGetGlobal(L, "greeting")` + `msAsCString` 打印（覆盖字符串往返）。
5. 容器往返：C 侧 `msNewList` + `msListAppend` 构造列表，`msSetGlobal(L, "xs", list)` 后 `msEvalString(L, "assert(xs[0] == 1)")`（覆盖 C→脚本数据传递与 `msSetGlobal`）。
6. 错误路径：`msCallObject` 调用一个非函数全局（如 `msGetGlobal(L, "greeting")`），断言返回 `NULL` 且 `msErrorOccurred`，用 `msErrorGet` + `msAsCString` 打印错误行，`msErrorClear` 复位（覆盖错误约定）。
7. `msGCCollect(L)` 强制一次收集后再次调用 `fib(10)` 断言结果，验证根栈保护下的对象在 GC 后存活（覆盖根栈与 GC 挂接）。
8. `msCloseState(L)`，打印 `embed demo ok`，退出码 0。

约定：每一步失败打印 `embed demo FAIL: <步骤名>` 到 stderr 并以非零退出；成功路径的 stdout 全文固定，供 ctest 以 `PASS_REGULAR_EXPRESSION` 比对末行。可选位置参数 `[script.ms]`：提供时改用 `msEvalFile` 加载该脚本替换第 2 步的内嵌源码（覆盖 `msEvalFile`），ctest 以 `${CMAKE_SOURCE_DIR}/tests/fixtures/embed_fib.ms` 传参再跑一遍。构建接入 `embed-example` target（任务 01 已占位）并 `add_test(NAME embed-demo ...)` 注册两次运行（内嵌版与文件版）。

## 实现步骤

1. 建 `include/mslang/` 头文件族骨架：`version.h`（补 `msVersionString` 声明）、`state.h`、`object.h`、`container.h`、`call.h`、`error.h`、`gc.h`、伞头 `mslang.h`，定义 `MS_API` 宏与 `MsResult` / `MsTypeTag` / `MsConfig`。验证：一个只 `#include <mslang/mslang.h>` 的空 `main` 在静态与共享两种构建下编译链接通过。
2. 扩展 `struct MsState` 内部结构：新增根栈字段与 GC 控制字段，`msNewState` / `msCloseState` 负责其初始化与释放（`msFree` 根栈数组）。验证：任务 09 的既有脚本测试全部保持绿色。
3. 实现 `src/api/ms_state.c`：`msNewStateWithConfig`（`gcThresholdBytes` 写入 GC 阈值）、`msEvalString` / `msEvalStringAs`（复用编译管线）、`msGetGlobal` / `msSetGlobal`、`msVersionString`。验证：embed_demo 第 1、2 步可通过。
4. 实现 `src/api/ms_gc.c`：根栈 push/pop（动态数组、扩容、LIFO 断言）、对接任务 17 根标记入口、`msGCDisable` / `msGCEnable` / `msGCCollect`。验证：embed_demo 第 7 步（GC 后再调用）通过；Debug 下故意不配对的 push/pop 触发断言。
5. 实现 `src/api/ms_error.c`：错误槽操作与 `msRaise*` 系列（格式化经 `msAlloc` 缓冲，定长上限截断）。验证：embed_demo 第 6 步错误路径。
6. 实现 `src/api/ms_object.c`：v0.1 子集的构造与转换函数。验证：embed_demo 第 3、4 步。
7. 实现 `src/api/ms_container.c` 与 `src/api/ms_call.c`：容器操作、`msCallObject` / `msCallMethod` / 属性三函数。验证：embed_demo 第 3、5 步。
8. 编写 `examples/embed_demo.c` 全流程，接入 CMake `embed-example` target 与两条 `add_test`。验证：`ctest --test-dir build` 中 `embed-demo` 两个用例通过。
9. 编写 `tests/ms/c_api/` 脚本测试与 `tests/fixtures/embed_fib.ms`（见测试方案）。验证：`python run_tests.py` 全绿。
10. 全平台（Win/Linux/macOS）× Debug/Release 构建验证，Debug（ASAN / `/RTC`）无内存错误与泄漏报告（`msCloseState` 后分配计数归零，含根栈数组）。

## 测试方案

本任务在任务 09 之后，语言级测试一律用 ms 脚本（`tests/ms/`，内建 `assert` + `print`，`testing` 模块任务 40 才存在）；C API 本身由 `examples/embed_demo.c` 集成测试覆盖（[11-project-layout.md](../language/11-project-layout.md) §4 第三层）。本任务只交付设计文档，测试实体随实现步骤编写。

### 1. 嵌入集成测试（examples/embed_demo.c，ctest 驱动）

覆盖点见「详细设计」第 10 节的八步流程：`msVersionString` 一致性、状态生命周期、`msEvalStringAs` / `msEvalFile`、全局变量读写、根栈 push/pop 纪律、`msCallObject`、int/str 转换、C→脚本容器数据传递、错误状态全路径（`msErrorOccurred` / `msErrorGet` / `msAsCString` / `msErrorClear`）、显式 `msGCCollect` 后对象存活、`msCloseState` 无泄漏。

ctest 注册两个用例：

- `embed-demo`：无参运行（内嵌脚本源码）。
- `embed-demo-file`：传 `tests/fixtures/embed_fib.ms` 运行（`msEvalFile` 路径），脚本内容与内嵌源码相同，断言两侧行为一致。

### 2. ms 脚本测试（tests/ms/c_api/，run_tests.py 驱动）

- `fib.ms`：与 `tests/fixtures/embed_fib.ms` 相同的 `fib` 定义，`assert(fib(30) == 832040)`、`assert(fib(10) == 55)`，末尾 `print("c api fib ok")`——保证 embed_demo 调用的脚本逻辑在 CLI 下语义一致（同一函数体两侧跑通）。
- `globals.ms`：顶层 `:=` 声明若干全局变量与函数，断言全局命名空间的读写在脚本侧的行为（`assert` 读取顶层全局、函数内 `global` 声明写回后顶层可见），对应 `msGetGlobal` / `msSetGlobal` 操作的同一张表的脚本侧语义。

### 3. 负向与内存验证

- 根栈错配：Debug 构建下，embed_demo 编译期开关（`#ifdef` 调试段，不进 CI 主路径）验证 `msRootPop` 空弹触发 `MS_ASSERT`。
- ASAN / `/RTC`：`embed-demo` 两个用例在 Debug 配置下无内存错误；`msCloseState` 后经任务 02 分配统计确认零残余分配（含根栈数组与错误槽对象）。

## 验收标准

- [ ] `include/mslang/` 下八个头文件（`mslang.h` / `state.h` / `object.h` / `container.h` / `call.h` / `error.h` / `gc.h` / `version.h`）存在，guard 形如 `MSLANG_INCLUDE_MSLANG_<文件>_H_`，各自自包含；伞头 `#include <mslang/mslang.h>` 一次引入全部 v0.1 API。
- [ ] `MsState` / `MsObject` 在公开头中仅 typedef 前向声明，公开签名不含任何内部结构体布局；代码风格通过 [10-c-style.md](../language/10-c-style.md) 检查（2 空格缩进、120 列、K&R、星号贴类型、命名约定、`src/` 内部结构体不 typedef）。
- [ ] `MsResult` / `MsTypeTag` / `MsConfig` / `MS_VERSION_*` 与 `msVersionString` 落地；`msNewStateWithConfig` 的 `gcThresholdBytes` 实际作用于 GC 阈值。
- [ ] `msEvalString` / `msEvalStringAs` / `msEvalFile` / `msGetGlobal` / `msSetGlobal` 与「详细设计」第 4 节语义一致；object.h / container.h / call.h / error.h 中本任务声明的函数全部实现，失败路径遵守「返回 `NULL` + 置错误槽」约定。
- [ ] 根栈 `msRootPush` / `msRootPop` 严格 LIFO 并作为 GC 根集被任务 17 扫描；`msGCDisable` / `msGCEnable` 计数嵌套正确，`msGCCollect` 无条件全量收集；强制 GC 后受根保护的对象存活。
- [ ] `examples/embed_demo.c` 覆盖「详细设计」第 10 节全部步骤，`ctest --test-dir build` 中 `embed-demo` 与 `embed-demo-file` 两用例通过，成功输出以 `embed demo ok` 结尾。
- [ ] `tests/ms/c_api/fib.ms` 与 `tests/ms/c_api/globals.ms` 经 `python run_tests.py` 全数通过。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）下 embed 用例无内存错误，`msCloseState` 后分配计数归零；构建产物只落在 `build/`。
- [ ] 头文件族不包含 `module.h` / `ctype.h`（任务 33 范围），不声明 `msNewBytes` / `msNewTuple` / `msNewIntFromString` / `msIsInstance` 等延后 API；无 TBD/TODO 占位。
