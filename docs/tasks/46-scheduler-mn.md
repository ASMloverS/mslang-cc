# 46 M:N 调度器（工作窃取与抢占）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.3 | ⬜ | [42 平台抽象层（线程/原子/socket/时钟/dlopen）](42-platform-layer.md)、[43 协程与 async/await（单线程协作式）](43-coroutines.md) |

## 任务目标

把任务 43 的单线程协作式调度器升级为 06-concurrency §4 规定的 M:N 调度器：M 个协程映射到 N 个 OS 工作线程（N 默认 CPU 核数，`MS_THREADS` 环境变量或 `sync.setMaxThreads(n)` 调整）。交付：

- **多工作线程接入**（改造 `src/sched/ms_sched.h` / `src/sched/ms_sched.c`）：任务 43 的 `struct MsScheduler` 重构为 `struct MsSched`（全局就绪队列 + 工作线程数组 + 全协程注册表，与任务 47 已假定的定名一致），新增 `struct MsWorker`（每工作线程本地就绪队列与运行现场）；工作线程 0 复用调用线程，`msSchedRun` 的脚本可见语义不变。
- **工作窃取**（08-vm-internals §6.2）：全局 FIFO 就绪队列 + 每工作线程本地环形队列；本地空 → 全局取 → 从随机受害者的本地队列**尾部偷一半**（`msMutexTryLock`，失败即换目标，绝不阻塞）；本地溢出时向全局队列倾泻一半。
- **抢占**（08-vm-internals §6.2）：监视线程每 10ms 置位全局原子抢占标志；VM 在每个回边与函数调用处（任务 08 已标记的位置）单次 acquire 读取该标志，认领成功且当前协程时间片满 10ms 则强制让出，让出的协程回到全局就绪队列尾部。
- **线程耗尽临时扩容**（06-concurrency §4）：工作线程进入阻塞式 OS 调用前经 `msSchedEnterBlocking` 申报；全部工作线程陷入阻塞且仍有就绪工作时临时增派工作线程，压力解除后临时线程自行退休。
- **规模控制**：`MS_THREADS` 环境变量解析与钳制；C 入口 `msSchedSetMaxWorkers`（脚本层 `sync.setMaxThreads` 绑定由任务 48 挂接）。
- **多线程化的正确性地基**：协程状态转换原子化（落实 06-concurrency §5 的 happens-before 边）、死锁检测多线程化、GC 的过渡性互斥措施（正式 safepoint 改造属任务 47）。

完成后，`tests/ms/concurrency/` 下的多线程压力脚本在 `MS_THREADS=1` 与 `MS_THREADS=4` 下结果一致；channel（任务 44）、select（任务 45）、GC safepoint（任务 47）、sync 模块（任务 48）在本任务之外。

## 设计依据

- `docs/language/06-concurrency.md`
  - §4：M 个协程调度到 N 个 OS 工作线程，N 默认 CPU 核数，`MS_THREADS` 环境变量或 `sync.setMaxThreads(n)` 调整；工作窃取；抢占为"VM 在每个回边与函数调用处检查抢占标记，超时（默认 10ms）强制让出"；阻塞在 OS 调用上的操作占用其工作线程，调度器检测到线程耗尽时临时扩容。
  - §5：happens-before 边——协程派生前的写入对协程体可见；协程 `return` happens-before 对其句柄 `await` 的返回；channel 配对边（任务 44 落地，本任务提供其依赖的线程安全入队入口）。
  - §6：safepoint = 让出点 + 函数调用 + 回边；STW 握手的全局标志是原子字（本任务先落抢占位，任务 47 并入 STW 位）。
- `docs/language/08-vm-internals.md`
  - §5：safepoint 检查是读取一个原子标志，开销可忽略。
  - §6.2：全局就绪队列 + 每工作线程本地队列 + 工作窃取（偷本地队列尾部一半）；工作线程每 10ms 设置抢占标志；让出的协程回到就绪队列尾部。
  - §6.3：调度器的工作线程是 VM 内部实现，与嵌入方线程隔离（`MsState` 的外部线程规则不变）。
- `docs/language/10-c-style.md`：§2 格式化、§3 命名、§4 typedef 规则（`struct MsSched`/`struct MsWorker` 不 typedef）、§6 内存纪律、§9 平台相关代码只出现在 `src/platform/`（调度器代码零 `#ifdef`）。
- `docs/language/11-project-layout.md`：`src/sched/` 的目录位置；`tests/ms/` 由 `run_tests.py` 驱动、`tests/c/` 用 `ms_test.h`。
- 任务 42（已定名，直接引用）：`struct MsThread`/`msThreadCreate`/`msThreadJoin`、`struct MsMutex`/`msMutexTryLock`、`struct MsCond`、`MsAtomicU32`/`MsAtomicI32` 及 `msAtomicLoadAcquire`/`msAtomicStoreRelease`/`msAtomicCompareExchange`/`msAtomicU32FetchOr`/`msAtomicI32FetchAdd`、`msThreadCpuCount`、`msThreadSleepMs`、`msClockMonotonicNs`。`msMutexTryLock` 的注释已明确"for work-stealing paths (task 46)"，本任务即其消费方。
- 任务 43（已定名，直接引用并在本节注明演进）：`struct MsCoroutine`（状态机 READY/RUNNING/SUSPENDED/DEAD、`result`/`waiters`/`readyNext`/`registryNext`）、`msCoroSpawn`/`msCoroFinish`、`msSchedInit`/`msSchedDestroy`/`msSchedEnqueue`/`msSchedRun`、死锁检测语义。本任务将其 `struct MsScheduler` 重构为 `struct MsSched`——任务 47 的文档已按 `struct MsSched`/`struct MsWorker`/抢占标志假定本任务的定名，本文即其定稿处。
- 任务 44/45（channel、select）文档尚不存在：本任务要求其"协程从阻塞转 READY"统一经过 `msSchedEnqueueGlobal` 以获得线程安全与 happens-before 排序，其阻塞/唤醒内部接口名为假定命名，实现时以对应任务文档定名为准。任务 48（sync 模块）文档尚不存在：`sync.setMaxThreads` 的脚本绑定假定为其对本任务 `msSchedSetMaxWorkers` 的薄封装，实现时以其定名为准。

## 详细设计

### 范围界定与单线程等价承诺

- 本任务不改变任何脚本可见语义：`MS_THREADS=1` 时调度行为必须与任务 43 完全一致（既有测试即回归判据），抢占是 06-concurrency §4 的语义组成部分，在单工作线程下同样生效。
- 明确不做：channel/select 的内部线程安全改造（任务 44/45，本任务只提供线程安全的唤醒入队入口）、STW safepoint 协议（任务 47，本任务只做过渡性 GC 互斥并预留标志字与设计形状）、`sync` 模块脚本绑定（任务 48）、协程感知 IO 的 netpoller（任务 62 方向）。
- `MsState` 内嵌字段由 `struct MsScheduler sched` 改为 `struct MsSched sched`；`msSchedInit`/`msSchedDestroy`/`msSchedEnqueue`/`msSchedRun` 四个任务 43 入口名保留（签名演进见下）。

### 常量与标志

```c
#define MS_SCHED_LOCAL_QUEUE_CAP 256                          // per-worker ring capacity (power of two)
#define MS_SCHED_PREEMPT_SLICE_NS (10ull * 1000ull * 1000ull) // 10ms time slice (06-concurrency §4)
#define MS_SCHED_MONITOR_TICK_MS 10u                          // monitor period = one preemption slice
#define MS_SCHED_HARD_MAX_WORKERS 1024                        // clamp for MS_THREADS / setMaxWorkers

#define MS_SCHED_PREEMPT_REQUEST 0x1u  // bit in MsSched.preemptFlags; task 47 ORs in its STOP_WORLD bit
```

### struct MsWorker 与 struct MsSched

任务 47 已定名引用这两个类型，此处定稿：

```c
typedef enum {
  MS_WORKER_RUNNING,     // executing a coroutine
  MS_WORKER_IDLE,        // parked on the work condition variable
  MS_WORKER_BLOCKED_OS,  // inside a declared blocking OS call (temporary-growth accounting)
  MS_WORKER_STOPPING     // shutdown observed, unwinding
} MsWorkerState;

struct MsWorker {
  struct MsThread thread;                                // unused for worker 0 (the msSchedRun caller thread)
  int index;
  bool isTemporary;                                      // spawned by thread-exhaustion growth; retires when idle
  struct MsSched* sched;                                 // back-pointer
  struct MsMutex localLock;                              // guards the local ring below
  struct MsCoroutine* localQueue[MS_SCHED_LOCAL_QUEUE_CAP];  // ring buffer, monotonic head/tail indices
  uint32_t localHead;                                    // pop side (FIFO); owner pops here
  uint32_t localTail;                                    // push side; thieves steal from here (tail half)
  MsAtomicI32 state;                                     // MsWorkerState; written by owner, read by monitor/others
  struct MsCoroutine* current;                           // coroutine this worker is running (owner-owned)
  uint64_t sliceStartNs;                                 // monotonic ns when current resumed (owner-only, no race)
  uint32_t preemptTick;                                  // preemptions performed; diagnostics/tests (kept by task 47)
};

struct MsSched {
  struct MsMutex globalLock;                             // guards global queue, registry, waiters lists, liveCount
  struct MsCond workCond;                                // signalled on global/local enqueue; broadcast on shutdown
  struct MsCoroutine* globalHead;                        // global FIFO ready queue (intrusive readyNext)
  struct MsCoroutine* globalTail;
  struct MsCoroutine* registry;                          // all live coroutines (task 43), now under globalLock
  struct MsCoroutine* main;                              // main coroutine (task 43 semantics unchanged)
  int64_t liveCount;                                     // non-DEAD coroutines; deadlock input, under globalLock
  struct MsWorker* workers;                              // msAlloc'd array, capacity MS_SCHED_HARD_MAX_WORKERS
  int workerCount;                                       // current workers, including temporary ones
  int maxWorkers;                                        // target N: MS_THREADS / cpuCount / setMaxWorkers
  MsAtomicI32 activeCount;                               // workers not IDLE (RUNNING or BLOCKED_OS)
  MsAtomicI32 shutdown;                                  // 1 = all workers and the monitor exit
  MsAtomicU32 preemptFlags;                              // MS_SCHED_PREEMPT_REQUEST; the single global check word
  struct MsMutex gcAllocLock;                            // interim allocation/GC mutex, removed by task 47
  struct MsThread monitor;                               // preemption monitor thread
  bool monitorStarted;
  bool gcSuspended;                                      // auto-GC disabled for the multithreaded run (interim)
};
```

设计说明：

- **单全局检查字**：抢占标志放在 `MsSched.preemptFlags` 一个原子字上而非每线程一个——任务 47 正是把它的 `MS_SP_FLAG_STOP_WORLD` 位并入同一个字（"VM 热路径只读一次"），本任务的形状使其成为纯增量改动。
- **本地队列用互斥锁保护的环形缓冲**而非无锁 deque：窃取方用 `msMutexTryLock`（任务 42 为此而设），失败即跳过，等待方永不在持锁上阻塞；正确性优先于无锁技巧，与 08-vm-internals §6.2 的语义（偷尾部一半）无冲突。
- **`sliceStartNs` 只由属主工作线程读写**：监视线程不读它（见「抢占」），跨线程撕裂读问题在设计上不存在。
- 任务 43 的 `readyHead`/`readyTail`/`current` 字段消失：`current` 下沉为每工作线程 `MsWorker.current`，就绪队列拆为全局 + 每线程本地两级。

### 公开函数

```c
// Task-43 signature kept. Additionally resolves the worker target N:
// MS_THREADS env (positive decimal, clamped to [1, MS_SCHED_HARD_MAX_WORKERS],
// invalid values ignored) or msThreadCpuCount() when absent.
void msSchedInit(MsState* L);

// Releases the workers array and the registry. Workers and the monitor are
// always joined by msSchedRun before it returns; destroy asserts none run.
void msSchedDestroy(MsState* L);

// Thread-safe enqueue as READY. When w != NULL (a running worker spawning or
// waking), pushes at w's local tail, spilling half to the global queue on
// overflow; otherwise (external/C API thread) appends to the global tail.
// Signals workCond when any worker is idle.
void msSchedEnqueue(MsState* L, struct MsWorker* w, struct MsCoroutine* co);

// Appends co at the global queue tail and signals workCond. Used for paths
// the spec pins to the queue tail: preemption yields (08-vm-internals §6.2),
// awaiter wakeups from msCoroFinish, and task 44/45 channel/select wakeups.
void msSchedEnqueueGlobal(MsState* L, struct MsCoroutine* co);

// Starts workers[1..N-1] plus the monitor thread, then runs the worker loop
// as workers[0] on the calling thread until the main coroutine is DEAD
// (task-43 result semantics: MS_OK or the main coroutine's error). Shuts
// down, joins every thread, and re-enables the interim GC suspension.
MsResult msSchedRun(MsState* L);

// Dispatch-loop checkpoint at back-edges and calls: one acquire load of
// sched->preemptFlags (zero = fast path out). On a claimed slice timeout,
// requeues the current coroutine at the global tail, bumps w->preemptTick,
// and returns true — the dispatch loop returns to the worker loop.
bool msSchedPreemptCheck(MsState* L, struct MsWorker* w);

// Blocking-OS-call bracketing for coroutine-unaware APIs (os/io file calls,
// C extensions). Enter may spawn a temporary worker when every worker is
// about to be blocked while ready work exists (thread-exhaustion growth).
void msSchedEnterBlocking(MsState* L, struct MsWorker* w);
void msSchedLeaveBlocking(MsState* L, struct MsWorker* w);

// Runtime adjustment of the target worker count (clamped). Raising spawns
// workers lazily at the next enqueue; lowering lets surplus workers retire
// at their next idle point — a running worker is never killed. The script
// binding sync.setMaxThreads (task 48) is a thin wrapper of this entry.
void msSchedSetMaxWorkers(MsState* L, int n);
```

任务 43 既有接口的签名演进：`msSchedEnqueue` 增加 `w` 参数（调用点都在分派/唤醒路径，可取得当前 worker；外部线程传 `NULL`）；任务 43 假定的 `msVmResumeCoroutine` 调整为 `msVmResumeCoroutine(MsState* L, struct MsWorker* w, struct MsCoroutine* co)`（worker 随执行线程走，不回读共享 `MsState` 字段）；`msCoroSpawn`/`msCoroFinish` 名字与语义不变，内部改走线程安全路径。

### 协程对象的原子化与 happens-before 落地

任务 43 的 `struct MsCoroutine` 两个字段改为原子（其余字段的并发访问经锁保护，不动）：

- `state` → `MsAtomicI32`：所有迁移以 `msAtomicStoreRelease` 写入、以 `msAtomicLoadAcquire` 读取。
- `cancelRequested` → `MsAtomicU32`：`h.cancel()` 可由任意工作线程发起。

06-concurrency §5 各边在 C 侧的承载（脚本层无原子语义，原子止于 VM 内部）：

- **派生边**：`msCoroSpawn` 完整初始化协程后，经 `msSchedEnqueue` 入队——入队/出队都持锁（本地 `localLock` 或 `globalLock`），unlock/lock 配对给出释放/获取排序，派生点之前的写入对新协程可见。
- **完成边**：`msCoroFinish` 先写 `result`/`resultIsError`，再 release-store `state = DEAD`；`await` 快路径与 `done()`/`result()` acquire-read `state`，读到 DEAD 即保证 `result` 可见——"return happens-before await 返回"由此落地。
- **唤醒路径**：`waiters` 链表的头插（await 慢路径）与摘取（`msCoroFinish` 唤醒）都在 `globalLock` 下进行；被唤醒者经 `msSchedEnqueueGlobal` 入队，同享锁排序。任务 44 的 channel 配对唤醒复用 `msSchedEnqueueGlobal`，不再自造队列写路径。

### 工作线程主循环与工作窃取

`msSchedRun` 把调用线程登记为 `workers[0]`（不为其 `msThreadCreate`），创建 `workers[1..N-1]` 与监视线程。每个工作线程的主循环：

1. **本地取**：持 `localLock` 从 `localHead` 弹一个协程（FIFO）。
2. **全局取**：本地空则持 `globalLock` 从 `globalHead` 弹一个。
3. **窃取**：仍空则工作窃取（见下）。
4. **空闲驻车**：三者皆空 → `activeCount` 减一；若自己是最后一个活动工作者（fetch_add 返回 1）且 `liveCount > 0` 且 `main` 未 DEAD → 判定死锁（见「死锁检测」）；随后在 `workCond` 上等待（谓词：全局队列非空 或 shutdown），被唤醒后 `activeCount` 加一并回步骤 1（本地可能已被其他线程的入队填满，但唤醒来源是"有工作到达某处"，经窃取总能取得）。
5. **运行**：置 `MS_WORKER_RUNNING`、`current = co`、`sliceStartNs = msClockMonotonicNs()`，调 `msVmResumeCoroutine`；返回后按三态处理——READY（让出/被抢占，已自入全局队尾）回步骤 1；SUSPENDED（挂在某个等待队列上）回步骤 1；DEAD（`msCoroFinish` 已完成结果存贮与 waiters 唤醒）回步骤 1。`main` 为 DEAD 时 worker 0 置 `shutdown`、broadcast `workCond`，退出循环并 join 全部线程。

窃取算法（偷受害者本地队列尾部一半）：

1. 以每工作线程私有 xorshift 状态选随机起点，遍历 `workers[0..workerCount)`（跳过自身；`workerCount` 只在持 `globalLock` 的增删点变化，遍历时持 `globalLock` 读快照后释放）。
2. 对受害者 `v`：`msMutexTryLock(&v->localLock)`，失败即换下一个（绝不阻塞——这是 trylock 的存在理由）。
3. 成功则计算 `n = (v->localTail - v->localHead)`，`take = (n + 1) / 2`（非空至少偷 1）；从 `v->localTail` 侧（最新推入的一半）摘 `take` 个，持自身 `localLock` 推入自身尾部，解锁双方。
4. 偷到即从自身本地队列头部弹一个运行；全部受害者试完仍空则进入空闲驻车。

本地队列溢出：`msSchedEnqueue` 本地推入时若环满（`localTail - localHead == MS_SCHED_LOCAL_QUEUE_CAP`），先把本地较旧的一半移入全局队列尾部（持 `globalLock`），再推入新协程——本地队列容量因此恒定有界，全局队列承担溢出与跨线程均衡。

### 抢占

两条规范要求的合成（06 §4"检查抢占标记，超时 10ms 强制让出"；08 §6.2"工作线程每 10ms 设置抢占标志"）：

- **监视线程**：唯一一个额外线程，循环 `msThreadSleepMs(MS_SCHED_MONITOR_TICK_MS)` 后 `msAtomicU32FetchOr(&preemptFlags, MS_SCHED_PREEMPT_REQUEST)`；观察到 `shutdown` 即退出。它只做周期性置位，不读各 worker 的 `sliceStartNs`——标志的精确解释留给认领方。
- **检查点**：任务 08 在分派循环的回边与 `MS_OP_CALL*` 处预留的注释位置，接入 `msSchedPreemptCheck`。快路径是对 `preemptFlags` 的单次 acquire 读：为零立即返回（08-vm-internals §5"开销可忽略"）。
- **认领**：读到置位后以 `msAtomicCompareExchange` 把该位清零（CAS 保证同一 tick 只有一个认领者）；认领成功者用**本线程自有**的 `sliceStartNs` 判定：时间片已满 `MS_SCHED_PREEMPT_SLICE_NS` → 当前协程置 READY、`msSchedEnqueueGlobal` 入全局队尾、`preemptTick++`，分派循环返回让出三态；不足 10ms 则丢弃本次认领（不补偿置位，下一个 10ms tick 会再置位，最坏粒度 20ms，可接受且不引入跨线程读）。
- 单工作线程下抢占同样生效——这正是纯计算长循环在 `MS_THREADS=1` 下不饿死其他协程的保证（回归判据之一）。

### 线程耗尽临时扩容

- 阻塞式 OS 调用（os/io 模块中未协程化的文件操作、未封装的 C 扩展）由调用方在阻塞前调 `msSchedEnterBlocking`、返回后调 `msSchedLeaveBlocking` 申报（各模块的具体挂接点属 io/os 与 C 扩展任务，本任务提供原语）。
- `msSchedEnterBlocking`：`state → MS_WORKER_BLOCKED_OS`；若此时 `activeCount` 中扣除 BLOCKED_OS 后为 0（自己已是最后一个能跑的工作线程）且就绪队列非空，则在 `workerCount < MS_SCHED_HARD_MAX_WORKERS` 时增派一个 `isTemporary = true` 的工作线程。
- 退休：临时工作线程在空闲驻车前检查 `workerCount > maxWorkers`，成立则直接退出（自我退休），否则按常规驻车。永久工作线程只受 `msSchedSetMaxWorkers` 调低影响，同样在空闲点退休，绝不杀运行中的线程。
- 扩容与退休都在 `globalLock` 下修改 `workerCount`/数组；窃取遍历按快照进行，退役者的本地队列在退出前倾泻回全局队列。

### 死锁检测的多线程化

任务 43 的"就绪队列空且仍有存活协程"在多线程下不再成立（空只是瞬态）。新判据：**`activeCount` 降为 0**（无任何工作线程在运行或阻塞于已申报的 OS 调用）且 `liveCount > 0` 且 `main` 未 DEAD——此时再无任何执行体能产生新工作，等价于任务 43 的静态死锁。处置相同：在 `main` 上注入 `RuntimeError`（复用取消注入路径），`msSchedEnqueueGlobal` 唤醒 main 使其以运行时错误终结，随后进入正常 shutdown。定时器驻留协程（`time.sleep` 协程化属后续任务，假定入口 `msSchedParkTimer`，以其任务文档定名为准）届时计入"潜在唤醒源"，从死锁判据中排除；本任务阶段尚无该唤醒源。

### GC 过渡措施（任务 47 前的显式临时方案）

多线程下任务 17 的 `msGcAlloc` 有两个硬问题：全对象链表与统计的并发写，以及"在分配点同步收集"会遍历其他线程正在变动的协程栈。任务 47 的 safepoint 协议才是正解；本任务落地明确的过渡措施（文档承认、任务 47 移除）：

- `msGcAlloc` 的整个函数体在多工作线程运行期间经 `MsSched.gcAllocLock` 互斥（含阈值判定与可能的收集）；驻留缓存等全局可变表的写入同样在该锁内。
- `msSchedRun` 在工作线程数大于 1 时先 `msGcDisable`（抑制自动触发，分配只登记）并置 `gcSuspended`，join 全部线程后 `msGcEnable`（恢复单线程语义，越阈即补一次收集）。即：**多线程运行期间不做收集**，内存随分配增长；本任务的测试据此控制分配规模。多线程压力下的长期运行内存安全由任务 47 保证，两任务之间该限制为已知且文档化的过渡状态。

### 共享错误槽与脚本可观测输出

- 任务 23 的异常以对象形式沿栈展开传播、终态存入协程句柄，不落地共享错误槽；`MsState` 错误槽的写权限定为主协程路径与 C API 边界（08-vm-internals §6.3 的外部线程规则不变），工作线程运行协程期间不写共享错误槽。
- `print` 等输出在多线程下交错属预期（规范未承诺输出顺序）；测试脚本不在断言路径上依赖并发输出的字节序。

## 实现步骤

1. 结构重构：`struct MsScheduler` → `struct MsSched`（先固定 `workerCount == 1`、本地队列直落全局的空 worker 形态），协程 `state`/`cancelRequested` 原子化与全调用点收敛，`msSchedEnqueue`/`msVmResumeCoroutine` 签名演进。验证：任务 43 起的全部既有脚本测试在 `MS_THREADS=1` 下逐字节回归通过。
2. 全局队列、注册表、`waiters` 链表的锁保护与 `workCond` 信号路径；`msSchedEnqueueGlobal` 落地，`msCoroFinish` 唤醒改走全局队尾。验证：单工作线程行为仍不变（回归），分配统计无泄漏。
3. 工作线程接入：`MS_THREADS` 解析与钳制、`workers[1..N-1]` 与监视线程的创建/join、worker 0 复用调用线程、工作线程主循环骨架（本地 → 全局 → 驻车，窃取先为空转）、shutdown 广播。验证：简单 spawn/await 脚本在 `MS_THREADS=4` 下结果正确。
4. 本地环形队列 + 溢出倾泻 + 偷尾部一半（trylock、随机起点、快照遍历）。验证：`tests/c/test_sched.c` 断言环不变量、倾泻数量、窃取数量与"绝不阻塞"路径；ms 压力脚本结果正确。
5. 抢占：监视线程置位、`msSchedPreemptCheck` 接入回边/调用检查点、让出回全局队尾、`preemptTick` 计数。验证：抢占脚本在 `MS_THREADS=1` 下协程交替推进（见测试方案）。
6. 死锁检测多线程化（`activeCount` 判据 + main 注入 `RuntimeError`）。验证：任务 43 的互相 await 死锁用例在 `MS_THREADS=4` 下仍以运行时错误退出。
7. 阻塞申报与临时扩容/退休（`msSchedEnterBlocking`/`msSchedLeaveBlocking`、上限钳制）。验证：`tests/c/test_sched.c` 以模拟阻塞驱动计数，断言扩容触发与硬上限。
8. GC 过渡措施：`gcAllocLock` 包裹 `msGcAlloc` 与驻留缓存写入、`msSchedRun` 的 disable/enable 配对。验证：多线程分配压力脚本在 Debug + ASAN/TSan 下无数据竞争、无悬垂引用。
9. `msSchedSetMaxWorkers`（钳制、惰性增员、空闲退休）+ 卫生收尾：全平台 Debug/Release 构建、任务 43 全套测试在 `MS_THREADS=1` 与 `MS_THREADS=4` 双配置回归、构建产物只落 `build/`。验证：CI 矩阵全绿。

## 测试方案

本任务晚于任务 40（testing 模块），ms 脚本一律使用 `testing` 模块，由仓库根 `run_tests.py` 驱动 mslang CLI 执行；多线程相关脚本须在 `MS_THREADS=1` 与 `MS_THREADS=4` 双配置下各跑一遍且结果一致（同任务 47 的约定，由 `run_tests.py` 层以环境变量控制）。调度器内部算法（环形队列、窃取、扩容计数）脚本不可观测，沿用任务 42/47 的先例补一组 C 单元测试。测试文件清单（本任务只交付本设计文档，测试代码随实现任务编写）：

- `tests/ms/concurrency/mn_parallel_sum_test.ms`：K 个 async 协程各自计算部分和，写入**预分配 list 的互不相交槽位**（测试纪律：并发写只落预分配容器的不相交槽，规避容器增长路径的内部并发），主协程 `await` 全部句柄后校验总和；`MS_THREADS=1` 与 `4` 结果一致。
- `tests/ms/concurrency/mn_spawn_stress_test.ms`：派生一万个轻量协程（各返回小计算结果），乱序完成，主协程以校验和汇总（不依赖完成顺序）；覆盖 spawn→本地队列→窃取的全路径压力。
- `tests/ms/concurrency/mn_preempt_test.ms`：一个纯计算长循环协程（数百 ms，无任何让出点）+ 一个每轮迭代主动 `await` 已就绪句柄的记录协程；断言记录协程在计算协程存活期间持续推进——`MS_THREADS=1` 下无抢占则必然失败，是抢占生效的直接判据。
- `tests/ms/concurrency/mn_fairness_test.ms`：N 个纯计算协程各自累计推进计数，断言全部计数均大于零且量级相近（让出回全局队尾的轮转公平性）。
- `tests/ms/concurrency/mn_await_stress_test.ms`：多等待方/等待链/异常跨协程传播在多线程下复跑（任务 43 场景的并发版）：三方 `await` 同一句柄得同一结果、未捕获异常在 `await` 点重抛且类型/traceback 正确、`cancel()` 注入 `CancelledError`。
- `tests/ms/concurrency/mn_deadlock_test.ms`（负向用例，配 `.exit` 声明预期退出码 1）：多线程下两协程互相 `await`，进程以 `RuntimeError` 死锁退出。
- `tests/c/test_sched.c`（`ms_test.h`，`MS_TEST`/`MS_ASSERT_EQ`，挂入 `mslang-tests`）：环形队列推/弹/溢出倾泻数量；窃取恰取尾部一半（含奇数向上取整、受害者空队列、`msMutexTryLock` 失败跳过）；`MS_THREADS` 解析（缺省回退核数、非法值忽略、钳制到硬上限）；模拟全阻塞触发临时扩容与 `isTemporary` 退休；`msSchedSetMaxWorkers` 的钳制。
- 回归：任务 09–45 的全部既有脚本测试在 `MS_THREADS=1` 与 `MS_THREADS=4` 下均通过（`MS_THREADS=1` 即任务 43 语义等价判据）；`tests/c/` 既有套件全绿。

## 验收标准

- [ ] `src/sched/ms_sched.h` / `ms_sched.c` 落地本文全部结构（guard 保持 `MSLANG_SRC_SCHED_MS_SCHED_H_`，头文件自包含）；代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsSched`/`struct MsWorker` 不 typedef、堆分配只经 `msAlloc/msRealloc/msFree`）；调度器代码无平台 `#ifdef`，线程/原子/时钟全部经任务 42 接口。
- [ ] 工作线程数默认 `msThreadCpuCount()`，`MS_THREADS` 环境变量可覆盖（正整数、非法值忽略、钳制硬上限）；`msSchedSetMaxWorkers` 语义与本文一致，为任务 48 的 `sync.setMaxThreads` 绑定留好 C 入口。
- [ ] 就绪结构为全局 FIFO 队列 + 每工作线程本地环形队列；窃取从随机受害者本地队列尾部偷一半（trylock，绝不阻塞）；本地溢出向全局倾泻一半；让出/被抢占/被唤醒的协程回全局队尾。
- [ ] 抢占实现"监视线程每 10ms 置位全局原子标志 + 回边/函数调用检查点单次 acquire 读取 + 认领且时间片满 10ms 强制让出"；`MS_THREADS=1` 下纯计算长循环不饿死其他协程（`mn_preempt_test.ms` 为判据）。
- [ ] happens-before 落地：协程 `state`/`cancelRequested` 原子化；派生、完成-唤醒两路径的 release/acquire 与锁排序与本文一致；`waiters` 操作全部在锁内。
- [ ] 死锁检测以 `activeCount == 0 && liveCount > 0` 为判据，多线程下互相 `await` 仍报 `RuntimeError`；`MS_THREADS=1` 下与任务 43 行为完全一致。
- [ ] `msSchedEnterBlocking`/`msSchedLeaveBlocking` 申报路径、线程耗尽临时扩容（硬上限）与临时线程退休实现并经 C 单元测试验证。
- [ ] GC 过渡措施就位：多线程运行期间 `msGcAlloc` 经 `gcAllocLock` 互斥且自动收集暂停，join 后恢复；多线程分配压力脚本在 ASAN/TSan 下无竞争与悬垂。
- [ ] 「测试方案」全部 ms 脚本在 `MS_THREADS=1` 与 `MS_THREADS=4` 双配置下通过，`tests/c/test_sched.c` 全绿，任务 09–45 既有测试双配置回归通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 44/45/48 的接口假定（`msSchedEnqueueGlobal` 复用、`sync.setMaxThreads` 绑定、`msSchedParkTimer`）在实现时已按对应任务文档对齐。
