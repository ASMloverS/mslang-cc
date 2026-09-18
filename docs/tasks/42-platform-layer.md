# 42 平台抽象层（线程/原子/socket/时钟/dlopen）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.3 | ⬜ | [02 核心基础设施](02-core-infrastructure.md) |

## 任务目标

交付 `src/platform/` 平台抽象层的完整落地：统一头文件 `src/platform/ms_platform.h` 提供线程创建/加入、互斥与条件变量、原子操作、单调时钟与墙钟、TCP socket、动态库加载六组原语，配 pthread 与 Win32 两套后端实现（`ms_platform_posix.c` / `ms_platform_win32.c`），由 CMake 按平台选择其一编译。完成后：

- 全仓库其余代码只 `#include "platform/ms_platform.h"`，不再出现 `#ifdef _WIN32` 等平台条件编译（10-c-style §9 的硬约束落地为可检查项）；
- 任务 33 落地的最小动态加载（`ms_dynload.*`）与任务 39 落地的最小时钟封装并入本层统一命名，旧文件删除、调用点迁移；
- 任务 43（协程）、46（M:N 调度器）、47（GC safepoint）、48（sync 模块）与 62（net 模块）所需的全部 OS 原语就绪，其中任务 47 文档已假定的接口名（`MsAtomicU32`、`msAtomicLoadAcquire`、`msAtomicStoreRelease`、`msAtomicCompareExchange`、`struct MsMutex`、`msThreadYield`）由本任务正式定义。

本任务自身通过 `tests/c/test_platform.c` 的 C 单元测试独立验证（平台原语无脚本可见语义，脚本级行为由下游任务覆盖）。

## 设计依据

- `docs/language/10-c-style.md`
  - §1 文件组织与 include guard（guard = 项目名 + 相对路径大写蛇形）、自包含头文件。
  - §2 格式化：2 空格缩进、120 列、K&R、星号贴类型、指定初始化器。
  - §3 命名：函数 `msLowerCamelCase`、类型 `MsUpperCamelCase`、常量/枚举值 `MS_UPPER_SNAKE`。
  - §4：C11 标准但 `threads.h` 不可移植，线程/原子走本层；`typedef` 仅限不透明类型、枚举与公开函数指针/纯数据配置结构体，内部结构体一律 `struct MsFoo`。
  - §5 错误处理：无 setjmp；禁止 `errno` 跨层传播，底层错误立即转换。
  - §6 内存纪律：堆分配只经 `msAlloc/msRealloc/msFree`。
  - §9 平台抽象：平台相关代码只允许出现在 `src/platform/`（线程 pthread/Win32、原子、动态库加载、时钟、socket）；其余代码只包含 `ms_platform.h`，不出现 `#ifdef _WIN32`。
- `docs/language/11-project-layout.md`
  - §1：`src/platform/` 的目录位置（线程/原子/时钟/socket/dlopen）；`tests/c/` 用自研 `ms_test.h`。
  - §2：CMake ≥ 3.20，MSVC 2019+ / GCC 10+ / Clang 12+；构建产物只落 `build/`。
  - §4：CI 矩阵 {windows, ubuntu, macos} × {Debug, Release}——本层是跨平台行为的主要被测对象。
- `docs/language/06-concurrency.md`
  - §4 调度器：工作线程数 N 默认 CPU 核数（需 `msThreadCpuCount`）；阻塞在 OS 调用上的操作占用工作线程（需非阻塞 socket + 就绪等待原语）。
  - §5 内存模型：happens-before 边（channel 收发、协程派生、`await`、sync 原语）需要 acquire/release 语义的原子操作作为 C 侧实现底座；mslang 不暴露脚本级原子类型，原子操作止于本层与 VM 内部。
  - §6 GC 协作：STW 握手的全局标志是原子字（经本层抽象）。
- `docs/language/08-vm-internals.md` §5/§6：safepoint 检查是读取一个原子标志、开销可忽略；工作线程空闲时等待条件变量。
- `docs/language/07-stdlib.md`：`os.platform`（§os）需要平台名字符串；§9 `time.now`/`time.monotonic` 的 C 侧底座；§18 `sync.Mutex` 等为协程级原语，不直接映射 OS 锁，本层只服务 VM/调度器内部。
- 任务 02 核心基础设施提供：`MsResult`（`MS_OK`/`MS_ERROR_RUNTIME`/`MS_ERROR_OOM`）、`MS_ASSERT`、`msAlloc/msRealloc/msFree`、`ms_test.h`。任务 02 文档尚不存在，接口名为假定命名，实现时以对应任务文档定名为准。
- 任务 33 C 扩展已落地 `src/platform/ms_dynload.{c,h}` 三函数最小面（`msDynloadOpen/msDynloadSym/msDynloadError`），其文档明确"任务 42 落地时并入平台层统一命名"；动态库扩展名（`dll`/`so`/`dylib`）的选择逻辑也随本任务移入平台层。
- 任务 39（time 标准库）为 `time.now`/`time.monotonic` 落地了最小时钟封装（过渡实现，位置与命名以其实际代码为准，任务 39 文档尚不存在）；本任务将其并入 `msClock*` 并删除旧封装。任务 34 的测试计时尚用 `clock()` 或平台 API 的 `static` 封装过渡，本任务落地后统一改走 `msClockMonotonicNs`。

## 详细设计

### 文件与后端组织

- 头文件 `src/platform/ms_platform.h`，include guard `MSLANG_SRC_PLATFORM_MS_PLATFORM_H_`，自包含（`<stdbool.h>` `<stddef.h>` `<stdint.h>` 及任务 02 的 `"core/ms_result.h"`）。头文件内**无任何平台条件编译**：两套后端实现同一组声明。
- 后端实现 `src/platform/ms_platform_posix.c`（pthread + `<dlfcn.h>` + BSD sockets + `clock_gettime` + `__atomic` 内建）与 `src/platform/ms_platform_win32.c`（Win32 线程 + Winsock2 + `QueryPerformanceCounter` + `LoadLibrary` + `Interlocked*` 内建）。CMake 按平台二选一：

```cmake
if (WIN32)
  set(MSLANG_PLATFORM_SRC src/platform/ms_platform_win32.c)  # links ws2_32
else ()
  set(MSLANG_PLATFORM_SRC src/platform/ms_platform_posix.c)  # links Threads::Threads, ${CMAKE_DL_LIBS}
endif ()
```

- 后端文件内部允许平台宏（这是 §9 划定的唯一合法位置）；POSIX 后端内部的系统差异（如 macOS 无 `pthread_condattr_setclock` 旧版本）用 `#ifdef __APPLE__` 等在该文件内收敛，不外溢。
- 删除任务 33 的 `src/platform/ms_dynload.{c,h}`，调用点（`src/module/ms_native_module.c`）迁移到 `msDl*`；删除任务 39 的最小时钟封装，`stdlib/time` 与任务 34 的计时迁移到 `msClock*`。

### 通用约定

- **零堆分配**：本层全部对象（线程/互斥/条件变量/socket）由调用者提供存储（不透明字节区，见下），函数自身不做堆分配，与 §6 内存纪律天然兼容。
- **不透明存储**：可嵌入对象用定长字节区 + 对齐联合承载，后端用 `_Static_assert` 校验原生类型放得下：

```c
#define MS_THREAD_STORAGE_SIZE 16   // pthread_t / HANDLE
#define MS_MUTEX_STORAGE_SIZE 64    // pthread_mutex_t (<=56) / CRITICAL_SECTION (40)
#define MS_COND_STORAGE_SIZE 64     // pthread_cond_t (<=48) / CONDITION_VARIABLE (8)

struct MsThread {
  union { max_align_t align; unsigned char opaque[MS_THREAD_STORAGE_SIZE]; } storage;
};
struct MsMutex {
  union { max_align_t align; unsigned char opaque[MS_MUTEX_STORAGE_SIZE]; } storage;
};
struct MsCond {
  union { max_align_t align; unsigned char opaque[MS_COND_STORAGE_SIZE]; } storage;
};
```

各后端文件开头：`static_assert(sizeof(pthread_mutex_t) <= MS_MUTEX_STORAGE_SIZE, ...)`（Win32 侧同理），尺寸不足即编译失败而非运行期损坏。

- **错误处理**：线程/互斥/条件变量失败返回 `MsResult`（`MS_ERROR_RUNTIME`；POSIX 初始化失败如 `ENOMEM` 映射 `MS_ERROR_OOM`）；socket 用专用状态枚举 + 线程本地错误查询（见下）；`errno`/`WSAGetLastError` 不出本层（§5）。
- **线程本地错误缓冲**：`msDlError`/`msSocketLastErrorMessage` 返回线程本地静态缓冲（`_Thread_local` / Win32 `__declspec(thread)`，在后端文件内选择）。这是对 10-c-style §4「禁止非 const 可变全局变量」的受控例外：平台层位于 `MsState` 之下、无状态对象可挂，仅限这两处与 Winsock 引用计数，评审清单固定包含此项。
- **析构纪律**：所有 `*Destroy`/`*Close`/`*Join` 在对象仍处于使用状态时调用属编程错误（debug 构建 `MS_ASSERT`）；每对创建/销毁的所有者必须是调用方明确的一段代码。

### 平台信息

```c
// Returns the platform name: "windows", "linux", "darwin", or another
// lowercase uname-style name. Backs os.platform (07-stdlib).
const char* msPlatformName(void);

// Returns the shared-library file extension without dot:
// "dll" (Windows), "so" (Linux), "dylib" (macOS). Used by the module
// loader's native-library candidates (task 33 flow).
const char* msPlatformSharedLibExt(void);
```

### 原子操作

原子字为不透明 typedef（允许 typedef 的不透明类型，§4），字段只能经下列函数访问：

```c
typedef struct { volatile uint32_t value; } MsAtomicU32;
typedef struct { volatile int32_t value; } MsAtomicI32;

void msAtomicU32Init(MsAtomicU32* a, uint32_t v);          // plain init before sharing
uint32_t msAtomicU32LoadAcquire(const MsAtomicU32* a);
void msAtomicU32StoreRelease(MsAtomicU32* a, uint32_t v);
bool msAtomicU32CompareExchange(MsAtomicU32* a, uint32_t* expected, uint32_t desired);
uint32_t msAtomicU32FetchOr(MsAtomicU32* a, uint32_t mask);  // returns old value

int32_t msAtomicI32LoadAcquire(const MsAtomicI32* a);
void msAtomicI32StoreRelease(MsAtomicI32* a, int32_t v);
bool msAtomicI32CompareExchange(MsAtomicI32* a, int32_t* expected, int32_t desired);
int32_t msAtomicI32FetchAdd(MsAtomicI32* a, int32_t delta);  // returns old value; sub = negative delta
```

- 语义对齐 C11 内存模型：load = acquire、store = release、RMW（CAS/fetch_or/fetch_add）= acq_rel；CAS 为强语义，失败时把当前值写入 `*expected`（失败路径 acquire）。这组语义是 06-concurrency §5 happens-before 边在 C 侧的承载：channel 配对、协程派生、safepoint 标志均只需 acquire/release。
- 任务 47 假定的无后缀名以 `_Generic` 宏分派到上述类型化函数（类型分派是函数无法替代的场景，§4 允许）：

```c
#define msAtomicLoadAcquire(pa) \
  _Generic((pa), MsAtomicU32*: msAtomicU32LoadAcquire, const MsAtomicU32*: msAtomicU32LoadAcquire, \
      MsAtomicI32*: msAtomicI32LoadAcquire, const MsAtomicI32*: msAtomicI32LoadAcquire)(pa)
#define msAtomicStoreRelease(pa, v) \
  _Generic((pa), MsAtomicU32*: msAtomicU32StoreRelease, MsAtomicI32*: msAtomicI32StoreRelease)((pa), (v))
#define msAtomicCompareExchange(pa, expected, desired) \
  _Generic((pa), MsAtomicU32*: msAtomicU32CompareExchange, MsAtomicI32*: msAtomicI32CompareExchange) \
      ((pa), (expected), (desired))
```

- 后端映射：POSIX 用 GCC/Clang `__atomic_load_n`/`__atomic_store_n`/`__atomic_compare_exchange_n`/`__atomic_fetch_or`/`__atomic_fetch_add`（显式内存序参数）；Win32 用 `InterlockedCompareExchange`/`InterlockedOr`/`InterlockedExchangeAdd`（全屏障，满足 acq_rel 上界），acquire load 用 `volatile` 读 + 编译器屏障，release store 用 `InterlockedExchange`。32 位对齐由 `_Static_assert(sizeof(MsAtomicU32) == sizeof(uint32_t))` 与 typedef 布局保证。

### 线程

```c
typedef void (*MsThreadFn)(void* arg);  // allowed function-pointer typedef (10-c-style §4)

// Starts a thread running fn(arg), storing the handle in caller-provided
// *thread. Returns MS_ERROR_RUNTIME on OS failure. The thread is joinable;
// detached threads do not exist in this API.
MsResult msThreadCreate(struct MsThread* thread, MsThreadFn fn, void* arg);

// Blocks until the thread exits and releases its OS handle. Joining twice
// or never joining is a programming error (MS_ASSERT in debug builds).
void msThreadJoin(struct MsThread* thread);

// Yields the current timeslice (sched_yield / SwitchToThread). Used by
// bounded spin waits (safepoint park, task 47).
void msThreadYield(void);

// Blocks the current thread for at least ms milliseconds.
void msThreadSleepMs(uint32_t ms);

// Returns the number of online logical CPUs (>= 1); backs the scheduler's
// default worker count (06-concurrency §4, MS_THREADS fallback).
uint32_t msThreadCpuCount(void);

// Returns a process-unique id of the calling thread, for diagnostics/tests.
uint64_t msThreadCurrentId(void);
```

后端：POSIX 直接 `pthread_create/pthread_join`；Win32 用 `_beginthreadex`（而非 `CreateThread`，保证 CRT 每线程状态安全）+ `WaitForSingleObject` + `CloseHandle`。线程函数返回值不设（结果经 `arg` 回传），保持两后端语义一致的最小交集。

### 互斥与条件变量

非递归互斥锁 + 条件变量。协程级 `sync.Mutex` 不经过这里（08-vm-internals §6.2：直接操作协程状态）；本组原语服务调度器队列、GC 注册表等 VM 内部共享结构。

```c
MsResult msMutexInit(struct MsMutex* mutex);
void msMutexDestroy(struct MsMutex* mutex);
void msMutexLock(struct MsMutex* mutex);
bool msMutexTryLock(struct MsMutex* mutex);   // for work-stealing paths (task 46)
void msMutexUnlock(struct MsMutex* mutex);

MsResult msCondInit(struct MsCond* cond);
void msCondDestroy(struct MsCond* cond);
void msCondWait(struct MsCond* cond, struct MsMutex* mutex);

// Returns false on timeout, true when signalled. Spurious wakeups are
// possible; callers must re-check the predicate.
bool msCondWaitTimeout(struct MsCond* cond, struct MsMutex* mutex, uint32_t timeoutMs);

void msCondSignal(struct MsCond* cond);
void msCondBroadcast(struct MsCond* cond);
```

后端：POSIX `pthread_mutex_*`/`pthread_cond_*`，超时等待用 `pthread_condattr_setclock(CLOCK_MONOTONIC)` 配 `pthread_cond_timedwait`（不受墙钟调整影响）；Win32 `CRITICAL_SECTION`（`TryEnterCriticalSection` 对应 trylock）+ `CONDITION_VARIABLE`（`SleepConditionVariableCS`）。MSVC 不实现 C11 `threads.h`，这正是本层存在的原因（10-c-style §4）。

### 时钟

```c
// Monotonic clock, nanoseconds from an unspecified epoch; never moves
// backward. Backs time.monotonic, scheduler timers, and test timing.
uint64_t msClockMonotonicNs(void);

// Wall clock, nanoseconds since the Unix epoch (may jump). Backs time.now.
int64_t msClockUnixEpochNs(void);
```

- 后端：POSIX `clock_gettime(CLOCK_MONOTONIC / CLOCK_REALTIME)`；Win32 `QueryPerformanceCounter` + `QueryPerformanceFrequency`（先算 64 位乘法溢出安全的换算：`ticks / freq` 与 `ticks % freq` 分治）与 `GetSystemTimePreciseAsFileTime`（旧系统回退 `GetSystemTimeAsFileTime`）。
- **与任务 39 的合并**：任务 39 为最
  小化落地 `time.now`/`time.monotonic` 引入的临时时钟封装（其文档尚不存在，命名以实际代码为准）整体删除，stdlib/time 改调上述两函数；任务 34 的测试计时（`clock()` 或平台 API 的 `static` 封装）一并改走 `msClockMonotonicNs`。合并后全仓库除 `ms_platform_*.c` 外不存在第二处时钟平台调用。

### socket

面向任务 46（调度器协程感知 IO）与任务 62（net 模块）的 TCP 最小面：只覆盖 IPv4/IPv6 字节流，UDP 与 Unix domain socket 不在 v0.3 范围。

```c
struct MsSocket {
  intptr_t handle;   // POSIX fd or Win32 SOCKET; -1 = invalid
};

typedef enum {
  MS_SOCKET_STATUS_OK,
  MS_SOCKET_STATUS_ERROR,        // details via msSocketLastError/Message
  MS_SOCKET_STATUS_WOULD_BLOCK   // non-blocking op cannot complete now
} MsSocketStatus;

typedef enum {
  MS_SOCKET_POLL_READABLE = 1u << 0,
  MS_SOCKET_POLL_WRITABLE = 1u << 1
} MsSocketPollEvent;

// Winsock lifecycle; no-ops on POSIX. Internally refcounted (via MsAtomicI32)
// so MsState init/teardown may call them freely.
void msSocketGlobalInit(void);
void msSocketGlobalCleanup(void);

MsSocketStatus msSocketTcpOpen(struct MsSocket* outSock);          // AF_INET/AF_INET6 decided at bind/connect
MsSocketStatus msSocketSetNonBlocking(struct MsSocket* sock, bool nonBlocking);
MsSocketStatus msSocketBind(struct MsSocket* sock, const char* host, uint16_t port);  // host NULL = wildcard
MsSocketStatus msSocketListen(struct MsSocket* sock, int backlog);
MsSocketStatus msSocketAccept(struct MsSocket* listenSock, struct MsSocket* outSock);
MsSocketStatus msSocketConnect(struct MsSocket* sock, const char* host, uint16_t port);
MsSocketStatus msSocketSend(struct MsSocket* sock, const void* data, size_t len, size_t* outSent);
MsSocketStatus msSocketRecv(struct MsSocket* sock, void* buf, size_t len, size_t* outRead);

// Single-socket readiness wait (poll / WSAPoll). timeoutMs < 0 waits
// forever. *outReady receives the MS_SOCKET_POLL_* mask actually ready.
// The scheduler's multi-fd netpoller (task 46) may build on or extend this.
MsSocketStatus msSocketPoll(struct MsSocket* sock, uint32_t events, int32_t timeoutMs, uint32_t* outReady);

// Retrieves the local address; hostBuf may be NULL when only the port is
// wanted. Used by tests with bind(port 0).
MsSocketStatus msSocketLocalAddr(const struct MsSocket* sock, char* hostBuf, size_t hostBufLen,
    uint16_t* outPort);

void msSocketClose(struct MsSocket* sock);   // sets handle to -1
int msSocketLastError(void);                 // errno / WSAGetLastError snapshot
const char* msSocketLastErrorMessage(void);  // thread-local buffer
```

语义要点：

- 非阻塞 socket 上 `connect` 进行中、`send`/`recv`/`accept` 无数据时返回 `MS_SOCKET_STATUS_WOULD_BLOCK`（POSIX `EAGAIN`/`EINPROGRESS`、Win32 `WSAEWOULDBLOCK`/`WSAEINPROGRESS` 归一）；`connect` 的完成与否经 `msSocketPoll(..., MS_SOCKET_POLL_WRITABLE, ...)` 判定。
- `recv` 对端有序关闭返回 `MS_SOCKET_STATUS_OK` 且 `*outRead == 0`，由调用方解释 EOF。
- 地址解析（`getaddrinfo`/`GetAddrInfoW`）在 `bind`/`connect` 内部完成，DNS 阻塞语义在此显式承认（协程感知 DNS 列入任务 62 的考虑范围）。
- 句柄创建后默认阻塞模式；是否设非阻塞由调用方（net 模块）决定，本层不预设。

### 动态库加载

并入任务 33 的 `ms_dynload.*` 三函数并更名、补一个关闭函数：

```c
void* msDlOpen(const char* path);                     // NULL on failure
void* msDlSym(void* handle, const char* symbol);      // NULL on failure
const char* msDlError(void);                          // thread-local buffer, last error text
void msDlClose(void* handle);
```

后端：POSIX `dlopen(RTLD_NOW | RTLD_LOCAL)`/`dlsym`/`dlerror`/`dlclose`；Win32 `LoadLibraryA`/`GetProcAddress`/`FormatMessageA` 进线程本地缓冲/`FreeLibrary`。`msDlClose` 为新增：任务 33 的模块加载路径仍不卸载已注册扩展（v0.2 决策不变），但测试与一次性探测场景需要显式关闭。迁移内容：删除 `src/platform/ms_dynload.{c,h}`，`src/module/ms_native_module.c` 三处调用更名，扩展名选择（`dll`/`so`/`dylib`）改经 `msPlatformSharedLibExt`，任务 33 的全部测试保持通过。

## 实现步骤

1. 建 `src/platform/ms_platform.h` 全量声明骨架（guard `MSLANG_SRC_PLATFORM_MS_PLATFORM_H_`、不透明存储联合体、全部六组接口签名与英文文档注释）+ CMake 后端源文件选择（`WIN32` 分支）+ 两后端的 `msPlatformName`/`msPlatformSharedLibExt`。验证：空后端桩在 Linux/Windows 均编译链接通过；单元测试断言两函数返回值属于合法集合。
2. 时钟：两后端实现 `msClockMonotonicNs`/`msClockUnixEpochNs`。验证：单调性测试（连续 10⁵ 次调用非递减）、`msThreadSleepMs(20)` 后流逝 ≥ 15ms（容忍调度抖动）、Unix 钟与 `time(NULL)` 差 < 5s。
3. 原子操作：typedef、八个类型化函数、三个 `_Generic` 分派宏、两后端映射。验证：单线程语义（load/store/CAS 成败/fetch_or/fetch_add 返回值）；多线程 CAS 唯一胜者、fetch_add 总和精确。
4. 线程：`msThreadCreate`/`msThreadJoin`/`msThreadYield`/`msThreadSleepMs`/`msThreadCpuCount`/`msThreadCurrentId` 两后端。验证：N=8 线程各自原子递增后 join，总计数精确；各线程 id 互异且与主线程不同；`msThreadCpuCount() >= 1`。
5. 互斥与条件变量：两后端 + `msMutexTryLock` + `msCondWaitTimeout`。验证：N 线程 × M 次临界区内递增，结果 == N×M；持锁期间 trylock 失败；signal 唤醒单等待者、broadcast 唤醒全部；超时等待在 ~timeout 后返回 false（容忍上限放宽）。
6. dlopen 并入：实现 `msDl*` 四函数，删除 `ms_dynload.{c,h}`，迁移 `ms_native_module.c` 调用点与扩展名查询。验证：任务 33 全部测试通过；夹具库 `ctestext` 可打开、`mslangInit_ctestext` 可解析、坏路径返回 NULL 且 `msDlError` 非空、`msDlClose` 后符号不可再用。
7. 时钟并入：删除任务 39 的最小时钟封装，stdlib/time 与任务 34 的计时改调 `msClock*`。验证：任务 39 与任务 34 的既有测试全部通过；grep 确认平台时钟调用只剩 `ms_platform_*.c` 两处。
8. socket：WSA 生命周期（引用计数）、TCP 全套、非阻塞归一、`msSocketPoll`、`msSocketLocalAddr`。验证：loopback echo 集成测试（见「测试方案」）；非阻塞 `recv` 空读返回 `WOULD_BLOCK`；poll 超时与就绪两路。
9. 卫生收尾：grep 断言 `src/platform/` 之外无 `#ifdef _WIN32`/`#if defined(_WIN32)` 等平台条件编译；Debug + ASAN/LSAN 构建跑全量 C 测试（线程用例可叠加 TSan 构建）；CI 三系统 × Debug/Release 全绿。验证：检查脚本/CI 步骤输出为空差异。

## 测试方案

本任务按任务要求使用 C 单元测试：平台原语无脚本可见语义，`ms_test.h`（任务 02，`MS_TEST`/`MS_ASSERT_EQ`）直接驱动即可；脚本级并发行为属任务 43–48 的测试范围。测试文件 `tests/c/test_platform.c`（本任务只交付本设计文档，测试代码随实现任务编写），挂入 `mslang-tests` 目标经 ctest 运行。dlopen 用例复用任务 33 的 `tests/fixtures/ctestext` 共享库夹具。涉及时间的断言一律用容忍区间（下界略小于名义值、上界放宽一个数量级），避免 CI 抖动误判。

覆盖清单：

- 平台信息：`msPlatformName` ∈ {"windows","linux","darwin",...} 非空小写串；`msPlatformSharedLibExt` 与当前平台构建产物后缀一致。
- 时钟：单调钟 10⁵ 次连续调用非递减；两次调用间 `msThreadSleepMs(20)`，流逝 ∈ [15ms, 2s)；墙钟与 `time(NULL)` 差 < 5s；单调钟减差不因调用次数漂移（粗测）。
- 原子：单线程 load/store/init/CAS 成败两路（失败时 `expected` 被刷新）/fetch_or 位累积/fetch_add 返回旧值；多线程：N 线程对同一 `MsAtomicI32` 各 fetch_add 一万次，总和精确；N 线程对同一 `MsAtomicU32` 从 0 CAS 到 1，恰好一个胜者；release-store/acquire-load 配对传递非原子数据（先写普通变量、release 置标志、另一线程 acquire 读到标志后断言普通变量可见）。
- 线程：create/join 基本路径；线程函数经 `arg` 回传结果；8 线程并发原子计数；`msThreadCurrentId` 互异性；`msThreadCpuCount >= 1`；join 后 `MsThread` 存储可复用于新线程。
- 互斥/条件变量：锁保护计数器 N×M 精确；持锁时 `msMutexTryLock` 返回 false、释放后成功；生产者-消费者模型下 `msCondSignal` 唤醒恰一名等待者；`msCondBroadcast` 唤醒全部；`msCondWaitTimeout(50)` 无信号时返回 false 且耗时下界达标；虚假唤醒安全性（谓词重查模型下结果仍正确）。
- dlopen：打开 `ctestext` 夹具库成功；解析 `mslangInit_ctestext` 非空、解析不存在符号返回 NULL；打开不存在路径返回 NULL 且 `msDlError` 非空；`msDlClose` 正常返回。回归：任务 33 的 `test_c_extension.c` 与 `tests/ms/c_extension/` 全部通过。
- socket：loopback（127.0.0.1）全双工 echo——服务端线程 `bind(port 0)` → `msSocketLocalAddr` 取实际端口 → `listen`/`accept`，客户端 `connect`/`send`/`recv` 逐字节校验回显；非阻塞模式：`recv` 无数据返回 `WOULD_BLOCK`，对端发送后 `msSocketPoll(READABLE, 1000)` 返回就绪且可读；`poll(timeout=50)` 无事件时超时返回（`outReady == 0`）；非法端口/不可达地址的 `connect` 返回 `ERROR` 且 `msSocketLastErrorMessage` 非空；`msSocketGlobalInit/Cleanup` 嵌套调用安全。
- 卫生：CI 步骤 grep 全仓库（排除 `src/platform/` 与文档）无 `#ifdef _WIN32`、`#ifdef __APPLE__`、`#ifdef __linux__`；构建产物只在 `build/`。

## 验收标准

- [ ] `src/platform/ms_platform.h` 存在，guard 为 `MSLANG_SRC_PLATFORM_MS_PLATFORM_H_`，头文件自包含且不含任何平台条件编译；`ms_platform_posix.c`/`ms_platform_win32.c` 两套后端实现同一接口，CMake 按 `WIN32` 二选一（POSIX 链接 `Threads::Threads` 与 `${CMAKE_DL_LIBS}`，Win32 链接 `ws2_32`）。
- [ ] 代码风格通过 10-c-style 检查：2 空格缩进、120 列、K&R、星号贴类型；`struct MsThread`/`struct MsMutex`/`struct MsCond`/`struct MsSocket` 不 typedef；`MsAtomicU32`/`MsAtomicI32`/`MsSocketStatus`/`MsThreadFn` 的 typedef 属 §4 允许类别；本层零堆分配（不调用 `msAlloc`）。
- [ ] `src/platform/` 之外全仓库无 `#ifdef _WIN32` 等平台条件编译（grep 可验证），其余代码只经 `ms_platform.h` 使用平台能力。
- [ ] 原子接口提供 `MsAtomicU32`/`MsAtomicI32` 及 load-acquire/store-release/CAS/fetch-or/fetch-add，`_Generic` 宏 `msAtomicLoadAcquire`/`msAtomicStoreRelease`/`msAtomicCompareExchange` 与任务 47 的假定命名一致；acquire/release 语义在 POSIX（`__atomic` 显式内存序）与 Win32（`Interlocked*`）两后端下均成立，多线程测试验证。
- [ ] 线程接口（create/join/yield/sleepMs/cpuCount/currentId）两后端可用；互斥非递归、含 trylock；条件变量支持 signal/broadcast/超时等待；不透明存储尺寸有 `_Static_assert` 兜底。
- [ ] 时钟并入完成：`msClockMonotonicNs`（单调、纳秒）与 `msClockUnixEpochNs`（墙钟）为全仓库唯一时钟平台入口；任务 39 的最小时钟封装与任务 34 的临时计时已迁移，两者既有测试全部通过。
- [ ] dlopen 并入完成：`ms_dynload.{c,h}` 删除，`msDlOpen/msDlSym/msDlError/msDlClose` 就位，`src/module/ms_native_module.c` 与扩展名查询（`msPlatformSharedLibExt`）迁移完毕，任务 33 全部测试通过。
- [ ] socket 接口覆盖 TCP open/bind/listen/accept/connect/send/recv/close、非阻塞归一（`MS_SOCKET_STATUS_WOULD_BLOCK`）、`msSocketPoll`、`msSocketLocalAddr` 与 Winsock 生命周期；loopback echo 与非阻塞/poll 用例在 Windows/Linux/macOS 均通过。
- [ ] `tests/c/test_platform.c` 覆盖「测试方案」全部清单项，在 CI 三系统 × Debug/Release 下全部通过；Debug + ASAN/LSAN 无泄漏与越界；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 02（`MsResult`/`MS_ASSERT`/`ms_test.h`）与任务 39（最小时钟封装命名）的接口假定在实现时已按对应任务文档或实际代码对齐。
