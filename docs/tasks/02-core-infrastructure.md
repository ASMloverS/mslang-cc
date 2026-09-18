# 02 核心基础设施（msAlloc、MsResult、通用宏）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [01 工程骨架与构建系统](01-project-skeleton.md) |

## 任务目标

交付全项目共用的核心基础设施，位于新增的 `src/core/` 目录与公开头文件
`include/mslang/error.h`：

1. **内存封装**（`src/core/ms_memory.h` / `ms_memory.c`）：`msAlloc` /
   `msRealloc` / `msFree` 替代一切直接 `malloc`/`realloc`/`free`，内置
   分配统计（当前字节、峰值字节、存活块数等）与 OOM 统一失败路径，并提供
   测试用失败注入钩子。
2. **错误码枚举**（`include/mslang/error.h`）：`MsResult` 枚举的唯一定义点，
   内部模块与公开 C API 共用。
3. **通用宏**（`src/core/ms_common.h`）：`MS_ASSERT` / `MS_UNREACHABLE` /
   `MS_ARRAY_LEN` / `MS_UNUSED`。
4. **诊断收集器**（`src/core/ms_diag.h` / `ms_diag.c`）：`struct MsDiagList`，
   实现「单文件最多收集 20 条编译错误（含文件/行/列/错误码/消息）」的约定，
   供任务 03（词法分析器）与任务 04（语法分析器）复用。

完成后，任务 03/04/05/06 可以在不碰裸 `malloc`、不重复定义 `MsResult` 的
前提下开发；本任务自身通过 `tests/c/` 下的三个 C 单元测试文件独立验证。

## 设计依据

- `docs/language/10-c-style.md`
  - §1：include guard 规则、头文件自包含、include 顺序与路径形式。
  - §3：命名约定（`ms`/`Ms`/`MS_` 三层前缀）。
  - §4：枚举允许 typedef；内部结构体不 typedef；禁止非 const 可变全局变量。
  - §5：无 setjmp，内部函数用 `MsResult` 报告失败；失败路径早返回。
  - §6：所有堆分配经 `msAlloc/msRealloc/msFree`（带状态统计，OOM 走统一
    失败路径），禁止直接 `malloc`。
  - §8：内部不变量用 `MS_ASSERT`（release 编译为空）；`MS_UNREACHABLE()`
    在 release 下展开为优化提示。
- `docs/language/09-c-api.md` §4：`MsResult` 枚举的取值集合
  `MS_OK` / `MS_ERROR_RUNTIME` / `MS_ERROR_SYNTAX` / `MS_ERROR_OOM`；
  §2 头文件布局中 `error.h` 是错误相关声明的归属头文件。
- `docs/language/08-vm-internals.md` §1：编译错误收集模式——单文件最多
  报告 20 个错误后中止，错误含文件/行/列与错误码。
- `docs/language/11-project-layout.md` §1（仓库结构）、§4（C 单元测试用
  自研 `ms_test.h`）。

需要显式承认的偏差与对齐说明：

- `11-project-layout.md` §1 的目录清单没有 `src/core/`。本任务新增该目录
  存放跨模块基础设施；任务 03 已假定 `"core/ms_diag.h"` 这一包含路径，
  说明该目录是必要的，`11-project-layout.md` 与任务 01 的目录清单需在
  后续同步更新（不在本任务范围内）。
- `03-lexer.md` 假定本任务提供 `struct MsDiagList`、
  `msDiagReport(diags, line, column, code, fmt, ...)` 与容量上限 20，本文
  定名与其保持一致。
- `03-lexer.md` 称 `ms_test.h` 由任务 02 提供，但按 `01-project-skeleton.md`
  的「详细设计」，`ms_test.h` 是任务 01 的交付物；以任务 01 为准，本任务
  的测试直接复用它。
- 分配统计计数器是文件级 `static` 可变状态，与 10-c-style §4「禁止非 const
  可变全局变量」存在张力。鉴于统计属于进程级诊断数据且 `MsState` 尚不存在
  （任务 06 才引入），本设计将其作为有意识的例外：计数器只经
  `msAlloc/msRealloc/msFree/msMemGetStats/msMemResetStats` 访问，不直接暴露；
  待 `MsState` 落地后可评估把计数器迁入其中。

## 详细设计

### 文件布局

```
include/mslang/error.h     // MsResult 枚举（公开 API，任务 18 扩展错误状态函数）
src/core/ms_common.h/.c    // 通用宏；msResultName 实现
src/core/ms_memory.h/.c    // msAlloc/msRealloc/msFree + 分配统计
src/core/ms_diag.h/.c      // MsDiagList 诊断收集器
```

include guard：`include/mslang/error.h` 用 `MSLANG_INCLUDE_MSLANG_ERROR_H_`；
`src/core/` 下各头文件形如 `MSLANG_SRC_CORE_MS_MEMORY_H_`。所有头文件自包含。
内部源文件以 `"core/ms_memory.h"` 形式包含（`src/` 根相对路径，任务 01 已将
`src/` 加入 include path）。

### MsResult 错误码（include/mslang/error.h）

`MsResult` 同时出现在公开 API 签名（09-c-api §4）与内部模块（lexer/parser/
compiler）中，枚举无法前向声明，因此定义在公开头文件里，内部与外部共用同一
定义点。枚举允许 typedef（10-c-style §4）：

```c
typedef enum {
  MS_OK = 0,          // success
  MS_ERROR_RUNTIME,   // runtime error (exception raised or pending)
  MS_ERROR_SYNTAX,    // compilation failed; diagnostics hold the details
  MS_ERROR_OOM        // allocation failure
} MsResult;

// Static name table for logs and tests ("MS_OK" etc.). Never returns NULL.
const char* msResultName(MsResult result);
```

- 取值集合与 09-c-api §4 完全一致，后续版本只增不改（09-c-api §13 不承诺
  ABI 稳定，但枚举值语义冻结）。
- `msResultName` 是内部辅助：声明放 `src/core/ms_common.h`，实现放
  `src/core/ms_common.c`，不进入公开 API。
- 任务 18 将在此头文件追加 `msErrorOccurred` 等错误状态函数（09-c-api §8），
  本任务只占位枚举。

### 通用宏（src/core/ms_common.h）

```c
#include <assert.h>
#include <stddef.h>

// Number of elements in a fixed-size array. Compile-time only.
#define MS_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// Internal invariant check. Enabled in debug builds, compiled out under NDEBUG.
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
```

- `MS_ASSERT` 只用于内部不变量；公开 API 的参数校验必须返回错误，不依赖
  断言（10-c-style §8）。
- `MS_UNREACHABLE` 的 debug 分支用 `assert(!"...")` 而非 `abort()`，让失败
  走标准断言输出（含文件/行号）。
- 不提供 `MS_MIN/MS_MAX` 等函数式宏；需要时用 `static inline` 函数
  （10-c-style §4「优先 static inline」）。

### 内存封装（src/core/ms_memory.h / ms_memory.c）

#### 公开函数

```c
// Allocates size bytes. Returns NULL on OOM or size == 0 (size == 0 is a
// caller bug; MS_ASSERT in debug). Never aborts. Content is uninitialized.
void* msAlloc(size_t size);

// Resizes ptr to newSize bytes. ptr may be NULL (equivalent to msAlloc).
// On failure returns NULL and leaves the original block untouched.
// newSize must be > 0; use msFree to release.
void* msRealloc(void* ptr, size_t newSize);

// Frees a block from msAlloc/msRealloc. NULL is a no-op.
void msFree(void* ptr);
```

#### 块头与统计

- 每次分配在返回指针前放置一个块头记录请求大小，使 `msFree`/`msRealloc`
  无需调用者传旧大小即可精确记账：

```c
struct MsMemHeader {
  size_t size;            // requested payload size in bytes
  // alignment must preserve max_align_t for the payload
};
```

  块头大小向上对齐到 `max_align_t` 的倍数（C11 `<stddef.h>`），保证负载
  按最大对齐要求返回。
- 统计结构（内部结构体不 typedef）与访问器：

```c
struct MsMemStats {
  uint64_t allocCount;          // successful msAlloc calls
  uint64_t reallocCount;        // successful msRealloc calls
  uint64_t freeCount;           // msFree calls on non-NULL blocks
  size_t liveBlocks;            // blocks currently alive
  size_t currentBytes;          // payload bytes currently alive
  size_t peakBytes;             // high-water mark of currentBytes
  size_t totalAllocatedBytes;   // cumulative payload bytes ever allocated
};

void msMemGetStats(struct MsMemStats* out);  // copies a snapshot
void msMemResetStats(void);                  // zeroes counters (leaks are NOT freed)
```

- 计数器是 `ms_memory.c` 的文件级 `static` 变量，只经上述函数访问（例外
  理由见「设计依据」）。v0.1 单线程语义下无需原子操作；任务 42（平台抽象
  层）落地后若 GC/调度器引入并发分配，再迁移到平台原子原语。
- `msAlloc(0)`：debug 下 `MS_ASSERT(size > 0)`，返回 `NULL` 且不计入统计。
- 溢出防护：`size + 对齐后块头` 溢出 `SIZE_MAX` 时按 OOM 处理返回 `NULL`。

#### OOM 统一失败路径

- `msAlloc`/`msRealloc` 失败一律返回 `NULL`，绝不 `abort`、绝不打印、绝不
  重试——决策权交给调用者。
- 传播约定（10-c-style §5）：返回 `MsResult` 的函数在最低的
  `MsResult` 边界把 `NULL` 转为 `MS_ERROR_OOM`；返回指针的函数直接上抛
  `NULL`（待 `MsState` 落地后附带设置状态错误位）。失败路径早返回，已获取
  的资源按获取逆序释放。
- 测试用失败注入钩子（供本任务与后续模块的 OOM 路径测试）：

```c
// After n more successful allocations, msAlloc/msRealloc start failing.
// n < 0 disables injection. Test/diagnostic use only.
void msMemSetFailAfter(int64_t n);
```

### 诊断收集器（src/core/ms_diag.h / ms_diag.c）

实现 08-vm-internals §1 的编译错误收集模式：单文件最多 20 条，条目含
文件/行/列/错误码/格式化消息。一个 `MsDiagList` 对应一个编译单元（文件）；
多文件（import，任务 24）每文件一个列表，不在本任务范围内。

```c
#define MS_DIAG_MAX_COUNT 20      // per-file diagnostic cap (08-vm-internals §1)
#define MS_DIAG_MESSAGE_LEN 256   // inline message buffer per entry

struct MsDiag {
  const char* file;                     // == list->chunkName, not owned
  uint32_t line;                        // 1-based; 0 = unknown
  uint32_t column;                      // 1-based, byte count; 0 = unknown
  uint32_t code;                        // numeric part of the Exxx code (E103 -> 103)
  char message[MS_DIAG_MESSAGE_LEN];    // NUL-terminated, truncated if longer
};

struct MsDiagList {
  const char* chunkName;                // file/chunk name, caller-owned, not copied
  struct MsDiag* items;                 // msAlloc'd, capacity MS_DIAG_MAX_COUNT
  size_t count;                         // entries recorded so far
};

// Allocates the fixed-capacity entry array. chunkName must outlive the list
// and is not copied. Returns MS_ERROR_OOM on allocation failure.
MsResult msDiagListInit(struct MsDiagList* list, const char* chunkName);

// Frees the entry array. Does not free chunkName.
void msDiagListDestroy(struct MsDiagList* list);

// Appends one diagnostic with a printf-style formatted message.
// Returns false without recording when the list is full (count ==
// MS_DIAG_MAX_COUNT); callers treat false as "abort compilation".
// Never fails otherwise: the message is vsnprintf'd into the fixed inline
// buffer, so no allocation happens at report time.
bool msDiagReport(struct MsDiagList* list, uint32_t line, uint32_t column,
    uint32_t code, const char* fmt, ...);

bool msDiagListIsFull(const struct MsDiagList* list);
size_t msDiagListCount(const struct MsDiagList* list);

// Returns the index-th entry. index >= count is a programming error
// (MS_ASSERT in debug, NULL in release).
const struct MsDiag* msDiagListAt(const struct MsDiagList* list, size_t index);

// Writes "file:line:column: error E<code>: <message>" into out
// (snprintf semantics: returns the would-be length).
int msDiagFormat(const struct MsDiag* diag, char* out, size_t outSize);

// Prints every entry via msDiagFormat, one per line, for CLI/compiler drivers.
void msDiagListPrint(const struct MsDiagList* list, FILE* out);
```

设计要点：

- 条目数组在 `msDiagListInit` 一次性 `msAlloc`（20 × sizeof(struct MsDiag)，
  约 6 KB），之后不再增长——容量硬上限即收集上限，消除报告路径上的一切
  分配与失败分支。
- 消息定长内联：超长消息截断（`vsnprintf` 保证 NUL 结尾），截断可接受，
  编译错误消息应简洁。
- 错误码存数值部分（如 E103 存 103），输出时格式化为 `E%03u`；错误码与
  语义的对应表归各模块（lexer 的 E101–E112 等），core 不维护码表。
- `chunkName` 与每个条目的 `file` 字段都不复制（与任务 03 的 lexer 对
  `source`/`chunkName` 的所有权约定一致）；条目 `file` 在报告时从
  `list->chunkName` 填入，使单条 `MsDiag` 可脱离列表独立格式化。
- `ms_diag.h` 自行包含 `<stdbool.h>`、`<stdint.h>`、`<stddef.h>` 与
  `<stdio.h>`（`msDiagListPrint` 的 `FILE*`），保持头文件自包含。

## 实现步骤

1. 新建 `src/core/` 目录；编写 `include/mslang/error.h`（`MsResult` 枚举）。
   验证：编译通过。
2. 编写 `src/core/ms_common.h` 全部宏与 `src/core/ms_common.c`
   （`msResultName` 名称表）。验证：编译期 `MS_ARRAY_LEN` 求值正确；
   `MS_ASSERT(true)` 无副作用；`MS_UNREACHABLE` 在 Debug/Release 两种配置下
   都编译通过；`tests/c/test_common.c` 断言名称表与枚举一一对应。
3. 实现 `ms_memory.h/.c`：块头、`msAlloc/msRealloc/msFree`、统计计数、
   `msMemGetStats/msMemResetStats`、`msMemSetFailAfter`。验证：
   `tests/c/test_memory.c` 全绿（见「测试方案」），ASAN 配置下无越界。
4. 实现 `ms_diag.h/.c`：初始化/销毁/报告/格式化/打印。验证：
   `tests/c/test_diag.c` 全绿。
5. 把 `src/core/*.c` 加入 `mslang` 库 target，三个测试文件按任务 01 的
   「每模块独立可执行 + ctest 注册」约定接入构建。验证：
   `ctest --test-dir build` 全绿，构建产物只在 `build/`。

## 测试方案

本任务早于最小可运行解释器（任务 09），按约定用 C 单元测试
（`tests/c/`，任务 01 的 `ms_test.h`，`MS_TEST`/`MS_ASSERT_EQ` 宏）。
本任务只交付设计文档，测试代码随实现编写；测试文件清单与覆盖点如下。

`tests/c/test_common.c`（宏与 MsResult）：

- `MS_ARRAY_LEN` 对不同长度定长数组的编译期求值结果。
- `MS_ASSERT(true)` 正常通过；`MS_UNUSED` 消除未使用警告（编译期验证）。
- `msResultName` 对全部四个枚举值返回对应字符串，无空缺。

`tests/c/test_memory.c`（内存封装）：

- `msAlloc` 返回指针可读写全部请求字节；多次分配指针互不重叠。
- 统计记账：N 次分配后 `allocCount`、`liveBlocks`、`currentBytes`、
  `peakBytes`、`totalAllocatedBytes` 精确匹配；`msFree` 后对应递减。
- `msRealloc`：扩容后原数据保留、统计按增量记账；`ptr == NULL` 时等价
  `msAlloc`；`msFree(NULL)` 为无操作且不影响统计。
- 失败注入：`msMemSetFailAfter(0)` 后 `msAlloc`/`msRealloc` 返回 `NULL`，
  `msRealloc` 失败时原块仍有效且可正常 `msFree`；注入期间统计不产生虚假
  计数；`msMemSetFailAfter(-1)` 恢复正常。
- 泄漏检查：每个用例结束时断言 `liveBlocks == 0 && currentBytes == 0`
  （`msMemResetStats` 只用于隔离用例，不用于掩盖泄漏）。
- 释放后块的负载区对齐满足 `max_align_t`（指针值按 `alignof(max_align_t)`
  断言）。

`tests/c/test_diag.c`（诊断收集器）：

- 初始化后 `count == 0`、`IsFull == false`；销毁后配合内存统计断言无泄漏。
- `msDiagReport` 逐条记录，条目的 `file` 等于初始化时的 `chunkName`，
  行/列/错误码原样保存，`fmt` 变参正确展开。
- 消息超长（> 255 字节）截断且 NUL 结尾。
- 容量上限：报告 25 条，仅前 20 条入库，第 21 条起 `msDiagReport` 返回
  `false` 且 `count` 停在 20、`msDiagListIsFull` 为 `true`。
- `msDiagListAt` 越界在 debug 下断言（编译期/走查验证，不做死亡测试）；
  合法下标返回正确条目。
- `msDiagFormat` 输出格式为 `file:line:column: error E103: msg`；
  `msDiagListPrint` 每条目一行（写入 `tmpfile()` 比对，或仅验证调用不
  崩溃并人工抽查）。

## 验收标准

- [ ] `include/mslang/error.h`、`src/core/ms_common.{h,c}`、
  `src/core/ms_memory.{h,c}`、`src/core/ms_diag.{h,c}` 存在；guard 形如
  `MSLANG_INCLUDE_MSLANG_ERROR_H_` / `MSLANG_SRC_CORE_MS_<文件>_H_`，
  头文件全部自包含。
- [ ] 代码风格符合 10-c-style：2 空格缩进、120 列、K&R 括号、指针星号贴
  类型、函数 `msLowerCamelCase`、类型 `MsUpperCamelCase`、常量与枚举值
  `MS_UPPER_SNAKE`、`struct MsDiag`/`struct MsDiagList` 不 typedef。
- [ ] `MsResult` 取值与 09-c-api §4 一致且全项目唯一定义点；后续模块
  不重复定义。
- [ ] 全项目（含后续模块提交时评审）无直接 `malloc/realloc/free` 调用；
  `msAlloc/msRealloc/msFree` 失败返回 `NULL`，调用侧统一转为
  `MS_ERROR_OOM` 或上抛 `NULL`。
- [ ] 分配统计（allocCount/freeCount/liveBlocks/currentBytes/peakBytes/
  totalAllocatedBytes）记账精确；`msMemSetFailAfter` 失败注入可用。
- [ ] `struct MsDiagList` 与 `msDiagReport` 签名同任务 03 的假定一致，
  容量上限 20（`MS_DIAG_MAX_COUNT`），条目含文件/行/列/错误码/消息。
- [ ] `tests/c/test_common.c`、`tests/c/test_memory.c`、
  `tests/c/test_diag.c` 覆盖「测试方案」全部清单项并全部通过
  （`ctest --test-dir build` 全绿，开启 `MSLANG_STRICT_WARNINGS` 无警告）。
- [ ] 构建产物只落在 `build/`；所有文本文件 UTF-8 无 BOM、LF 行尾、
  无行尾空白。
- [ ] 无 TBD/TODO 占位；与任务 03/04 的接口假定在实现时已对齐。
