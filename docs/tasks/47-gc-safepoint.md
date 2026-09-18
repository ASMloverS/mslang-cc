# 47 GC safepoint 改造

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.3 | ⬜ | [17 GC 标记-清除](17-gc-mark-sweep.md)、[46 M:N 调度器（工作窃取与抢占）](46-scheduler-mn.md) |

## 任务目标

把任务 17 的单线程 STW 标记-清除 GC 改造为可在 M:N 调度器（任务 46）下安全运行的多线程 STW GC。交付：

- safepoint 协议模块（`src/gc/ms_safepoint.h` / `src/gc/ms_safepoint.c`）：全局原子标志 + 线程注册表 + 停车（park）/恢复握手，实现 08-vm-internals §5 的"GC 线程设置全局标记 → 各工作线程在最近 safepoint 自旋等待 → 标记清除 → 恢复"。
- VM 检查点接入：函数调用、回边、调度器让出点三类 safepoint 与任务 46 的抢占检查合并为单次原子字读取，开销可忽略。
- 根集扩展：从"当前协程栈"扩展为"调度器内全部协程（运行中/就绪/阻塞在 channel 或 await 上）的调用栈、求值栈与 upvalue"，以及各注册线程的 C API 显式根。
- GC 与调度器的互斥规则：STW 期间所有调度活动（取协程、工作窃取、唤醒配对）冻结，且不因锁持有顺序产生死锁。

完成后，多工作线程下的并发分配压力（N 个协程经 channel 交换新分配对象）可触发任意次 GC 而不出现悬垂引用、重复释放或死锁；单线程行为与任务 17 完全一致（回归不破）。

## 设计依据

- `docs/language/08-vm-internals.md`
  - §5 GC：阈值触发（按存活率自适应）；黑白两色 + 标记栈；根集合含"所有协程的调用栈/求值栈、模块注册表、内建类型表、C API 显式根（`msRootPush`）"；STW 协作协议；safepoint 检查是读取一个原子标志（经 `src/platform/` 原子抽象）；"标记实现为可中断的步骤函数"为增量改造留口。
  - §6.1 `struct MsCoroutine`：协程持有 `frames`（调用栈）、`stack`（求值栈）、`result`、`waiters`、`blockedOn`。
  - §6.2 调度器：全局就绪队列 + 每工作线程本地队列 + 工作窃取；抢占为"工作线程每 10ms 设置抢占标志，VM 在回边/调用指令检查并让出"。
- `docs/language/06-concurrency.md`
  - §4：让出点清单（channel 操作、`await`、`select` 阻塞、`time.sleep`、协程感知 IO）；抢占检查位于回边与函数调用。
  - §6 与 GC 的协作：STW 时所有工作线程在最近 safepoint 停住（safepoint = 让出点 + 函数调用 + 回边）；协程的 VM 栈、寄存器窗口、句柄表都是 GC 根。
- `docs/language/09-c-api.md`：§4 `msRootPush`/`msRootPop` 显式根协议；08-vm-internals §6.3 的线程规则（同一时刻一个外部线程操作一个 `MsState`）保证外部线程的 C API 根栈本身无并发写。
- `docs/language/10-c-style.md`：§2 格式化（2 空格、120 列、K&R）、§3 命名、§4 typedef 规则、§6 内存纪律、§9 平台相关代码只允许出现在 `src/platform/`（原子操作不得散落于 GC 代码）。
- `docs/language/11-project-layout.md`：`src/gc/`、`src/sched/`、`src/platform/` 的目录位置；`tests/c/` 用 `ms_test.h`、`tests/ms/` 由 `run_tests.py` 驱动。
- 任务 17 GC 标记-清除提供：`struct MsGc`（全对象链表、标记栈、阈值与自适应统计）、`msGcCollect` 单线程流程、`msGcMarkObject`/`msGcMarkValue`。任务 42 平台抽象层提供原子与互斥原语（假定名 `msAtomicU32`、`msAtomicLoadAcquire`/`msAtomicStoreRelease`、`msAtomicCompareExchange`、`struct MsMutex`、`msThreadYield`）。任务 46 M:N 调度器提供：`struct MsSched`（全局就绪队列、工作线程数组、协程注册表）、`struct MsWorker`、抢占标志与回边/调用检查点。上述任务的文档尚不存在，接口名为本文假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 文件与模块划分

- 新增 `src/gc/ms_safepoint.h` / `src/gc/ms_safepoint.c`，include guard `MSLANG_SRC_GC_MS_SAFEPOINT_H_`；自包含（include `<stdbool.h>` `<stdint.h>` 及平台层与调度器头）。STW 协议集中于此文件，VM 分派循环与调度器只调用两个入口（`msSafePointCheck`、`msSafePointInitThread`），不感知协议细节。
- 修改 `src/gc/ms_gc.c`：根集扫描函数拆分为"静态根"（模块注册表、内建类型表、驻留缓存）与"协程根"（经 `MsSched` 枚举），后者在 STW 状态下遍历调度器；`msGcAlloc` 的阈值触发改为经 `msGcStopTheWorld`/`msGcResumeWorld` 包裹的 `msGcCollectSTW`。
- 修改 `src/sched/ms_sched.c`：工作线程主循环取协程前调用 `msSafePointCheck`；协程创建时加入全局协程注册表（`MsSched` 内既有或新增的全部协程侵入式链表）。
- 修改 `src/vm/` 分派循环：`MS_OP_CALL*` 与回边跳转处的抢占检查替换为统一的 `msSafePointCheck`。
- 模块不持有堆内存：线程注册节点由调用方（工作线程控制块 `MsWorker`、外部线程栈上变量）提供存储，注册表只做侵入式链接。

### safepoint 标志字与线程状态

任务 46 的抢占标志与本任务的 STW 标志合并为一个全局原子字，VM 热路径只读一次（08-vm-internals §5："开销可忽略"）：

```c
typedef enum {
  MS_SP_FLAG_NONE       = 0,
  MS_SP_FLAG_PREEMPT    = 1u << 0,   // 任务 46 的抢占请求
  MS_SP_FLAG_STOP_WORLD = 1u << 1    // GC 请求所有线程停车
} MsSafePointFlags;

typedef enum {
  MS_GC_PHASE_IDLE,        // 无回收
  MS_GC_PHASE_REQUESTED,   // STOP_WORLD 已置位，等待线程停车
  MS_GC_PHASE_MARKING,     // 世界已停，标记中
  MS_GC_PHASE_SWEEPING,    // 清除中
  MS_GC_PHASE_COUNT
} MsGcPhase;
```

注册到 GC 的执行线程（每个调度器工作线程一个；外部嵌入线程 attach 时一个）：

```c
typedef enum {
  MS_VT_WORKER,     // 调度器工作线程
  MS_VT_EXTERNAL    // 经 C API attach 的外部线程
} MsVmThreadKind;

struct MsVmThread {
  struct MsVmThread* next;      // 注册表侵入式链表，registryLock 保护
  MsVmThreadKind kind;
  MsAtomicU32 parked;           // 1 = 已在 safepoint 停住（含空闲/外部区）
  uint32_t preemptTick;         // 任务 46 的抢占计数，原样保留
  struct MsCoroutine* current;  // 正在运行的协程（停车时必须为 NULL 或已静止）
  struct MsRootSlot* cApiRoots; // 该线程的 msRootPush 栈（C API 显式根）
  void* owner;                  // 回指 MsWorker / 外部控制块
};

struct MsSafePoint {
  MsAtomicU32 flags;            // MsSafePointFlags 位掩码，全局唯一检查点
  MsAtomicI32 parkedCount;      // 已停车线程数
  MsAtomicI32 threadCount;      // 已注册线程数
  MsAtomicU32 phase;            // MsGcPhase，CAS 保证唯一 GC 发起者
  struct MsMutex registryLock;  // 仅保护 threads 链表增删
  struct MsVmThread* threads;
  struct MsGc* gc;              // 回指，STOP 解除后的收尾统计用
};
```

### 公开函数

```c
// Initializes the safepoint world for gc. No threads are registered yet;
// msSafePointInit must complete before the scheduler starts its workers.
void msSafePointInit(struct MsSafePoint* sp, struct MsGc* gc);

// Releases the registry. All threads must be detached first (MS_ASSERT).
void msSafePointDestroy(struct MsSafePoint* sp);

// Registers *thread (caller-provided storage, must outlive the detach).
// A thread registers with parked = 1: it is born "parked" and must call
// msSafePointResume before executing any VM bytecode or allocation.
void msSafePointAttachThread(struct MsSafePoint* sp, struct MsVmThread* thread,
    MsVmThreadKind kind, void* owner);

// Parks the thread if the world is stopped, then removes it from the
// registry. After return the thread must never touch the VM again.
void msSafePointDetachThread(struct MsSafePoint* sp, struct MsVmThread* thread);

// The single VM hot-path entry. Called at every safepoint: function call,
// loop back-edge, and scheduler yield point (channel ops, await, select,
// sleep). Fast path is one acquire load of sp->flags; when zero it returns
// immediately. Handles preemption (delegates to the task-46 yield) and
// STW parking.
void msSafePointCheck(struct MsSafePoint* sp, struct MsVmThread* thread);

// Marks the thread as running VM code again after it was born/idle
// (parked = 1). Blocks while STOP_WORLD is set.
void msSafePointResume(struct MsSafePoint* sp, struct MsVmThread* thread);

// Requests STW and blocks until every registered thread except *self* has
// parked; *self* is marked parked for the duration (its own coroutine and
// stacks are stationary while it runs GC). Only one thread can be the GC
// leader; concurrent triggers lose the phase CAS and fall through to
// msSafePointPark instead.
void msGcStopTheWorld(struct MsSafePoint* sp, struct MsVmThread* self);

// Clears STOP_WORLD (release) and flips the phase back to IDLE. Parked
// threads observe the clear and resume on their own; no drain wait.
void msGcResumeWorld(struct MsSafePoint* sp, struct MsVmThread* self);

// Parks the calling thread until STOP_WORLD clears. Spin phase: bounded
// pause loop (msAtomicLoadAcquire + msThreadYield after N iterations);
// no condvar — parking latency is bounded by safepoint density, and a
// condvar would add a fourth lock to the GC/scheduler ordering.
void msSafePointPark(struct MsSafePoint* sp, struct MsVmThread* thread);
```

### STW 握手协议

1. 发起：分配路径 `msGcAlloc` 越过阈值后调用 `msGcCollectSTW`。线程先把自身 `parked` 置 1（release），然后对 `phase` 做 `IDLE → REQUESTED` 的 CAS：失败说明已有 GC 在进行，直接走 `msSafePointPark`（停车等本轮结束，回来重试分配）。
2. 请求：发起者把 `MS_SP_FLAG_STOP_WORLD` 原子或入 `flags`（release），然后自旋等待 `parkedCount == threadCount`。所有工作线程在下一次 `msSafePointCheck`（调用/回边/让出点，或工作线程主循环取协程前）读到标志，置自身 `parked = 1` 并递增 `parkedCount` 后自旋于标志清除。
3. 不变量：**线程停车后不执行任何 VM 字节码、不分配、不触碰调度队列**；因此握手完成时，全部协程的栈与调度器全部队列处于静止状态，GC 可以无锁遍历。
4. 标记与清除：发起者把 `phase` 推进到 `MARKING`/`SWEEPING`，调用改造后的根扫描（见下）与任务 17 既有的标记-清除主体。标记保持步骤函数形态（08-vm-internals §5 的增量改造留口），本任务不引入写屏障。
5. 恢复：`phase → IDLE`，清除 `MS_SP_FLAG_STOP_WORLD`（release）。停车线程在下一次自旋迭代中观察到清除，`parked = 0`、递减 `parkedCount`，回到各自的检查点继续。无需等待全部线程离开停车循环：二次 GC 发起时 `parkedCount` 语义不变（仍在停车循环里的线程保持 `parked = 1`，只会再次停车），正确性不受影响。

### GC 与调度器的互斥

- 检查点位置（06-concurrency §6：safepoint = 让出点 + 函数调用 + 回边）：
  - VM 分派循环：`MS_OP_CALL`/`MS_OP_CALL_KW`/`MS_OP_CALL_METHOD`/`MS_OP_TAIL_CALL` 与所有向后跳转指令的回边处调用 `msSafePointCheck`——复用任务 46 已放置的抢占检查点，两处标志一次读取；
  - 让出点：channel `send`/`recv` 阻塞、`await` 挂起、`select` 阻塞、`time.sleep` 挂起协程前，工作线程先经 `msSafePointCheck`；挂起后该工作线程取下一个协程前再查一次；
  - 空闲工作线程（就绪队列为空、等待条件变量）在阻塞前置 `parked = 1`，被唤醒后走 `msSafePointResume`——即"空闲即停车"不变量。
- 锁序规则（防死锁）：`msSafePointCheck` 只允许在工作线程**未持有任何调度器锁**（全局队列锁、本地队列锁、channel 内部锁）的位置调用；工作线程持锁路径（窃取、入队、channel 配对唤醒）内不得分配 VM 对象、不得触发检查点。GC 发起者等待握手时同样不持有任何调度器锁，因此不存在"GC 等线程停车、线程等 GC 持锁释放"的环。
- 唤醒配对与窃取在 STW 期间天然冻结：这些动作只由工作线程执行，而工作线程不停车就不会离开检查点；停车后世界静止，GC 遍历队列无并发写。
- 外部线程（`MS_VT_EXTERNAL`）：按 08-vm-internals §6.3，同一时刻只有一个外部线程操作一个 `MsState`；其进入/离开 C API 边界（`ms_*` 调用入口/出口）即 `msSafePointResume`/置 `parked = 1` 的对。处于"外部区"（在 C API 之外、不触碰 VM）的线程视为已停车；它在 STW 期间重新进入 C API 时会被 `msSafePointResume` 挡住。其 `cApiRoots` 栈作为根扫描，与协程根并列。

### 根集扩展

任务 17 的根扫描拆分为静态根与协程根两部分，均在 STW 状态下、由 GC 发起者单线程执行（无锁）：

```c
// Marks the roots that exist regardless of concurrency: module registry,
// builtin type table, interned string/small-int caches, and the C API
// explicit-root stacks of every registered thread (sp->threads walk).
void msGcMarkStaticRoots(struct MsGc* gc, struct MsSafePoint* sp);

// Marks every coroutine known to the scheduler: for each coroutine in the
// scheduler-wide registry list, mark the coroutine object, then walk its
// call frames (proto, upvalues) and evaluation stack, its result, and its
// waiters queue (08-vm-internals §6.1). Coroutines blocked on channels or
// await are reached through the same registry — channel wait queues are
// never traversed for GC purposes.
void msGcMarkCoroutineRoots(struct MsGc* gc, struct MsSched* sched);
```

设计决策：

- 调度器维护**全协程注册表**（`MsSched` 内侵入式链表，协程创建时登记、终态回收时摘除）：就绪的、阻塞的、运行中的协程都在其中，GC 只遍历这一条链，无需理解全局队列/本地队列/channel 等待队列的拓扑。单协程的栈扫描逻辑沿用任务 17 的 `msGcMarkCoroutine`，本任务只做枚举。
- `waiters`（await 本协程的其他协程队列）经协程对象的常规标记边覆盖，不单独处理。
- 小整数/驻留字符串缓存作为静态根标记（与任务 17 一致）。
- C API 显式根从"全局一根栈"改为**每注册线程一根栈**（`MsVmThread.cApiRoots`）：STW 后世界静止，`msGcMarkStaticRoots` 遍历注册表逐栈标记；运行期各线程只写自己的栈，无需加锁（09-c-api 的 LIFO 约束不变）。

### 阈值触发的并发收敛

- 多线程同时越过阈值时由 `phase` 的 CAS 收敛为单一发起者；其余线程经 `msSafePointPark` 等待本轮结束，回来后重试分配（此时阈值已按存活率自适应更新，通常不再触发）。
- 分配路径在 `msGcAlloc` 内先做一次 `msSafePointCheck` 快路径（与回边检查同一标志字），保证"正在分配的线程必然先到达 safepoint"，消除"GC 等一个永远不分心的分配者"的活锁。
- OOM 语义不变：`msGcCollectSTW` 后仍不足则返回 `MS_ERROR_OOM`（任务 17 既有行为）。

## 实现步骤

1. 建 `src/gc/ms_safepoint.h` / `ms_safepoint.c` 骨架：`MsSafePointFlags`、`MsGcPhase`、`struct MsVmThread`、`struct MsSafePoint`、`msSafePointInit`/`msSafePointDestroy`。验证：C 单元测试断言初始化后 `flags == 0`、`threadCount == 0`。
2. 线程注册表：`msSafePointAttachThread`/`msSafePointDetachThread`（registryLock 保护、`parked = 1` 出生语义、`threadCount` 维护）。验证：多个假线程 attach/detach 后计数与链表正确；未 detach 先 destroy 触发 `MS_ASSERT`。
3. 停车原语 `msSafePointPark` 与 `msSafePointResume`：自旋 + `msThreadYield` 退避、acquire/release 配对。验证：单线程设置/清除标志，观察停车线程进出。
4. 握手协议 `msGcStopTheWorld`/`msGcResumeWorld` + `phase` CAS 收敛。验证：N 个假线程各自循环"模拟工作 → `msSafePointCheck`"，主线程反复 STW/恢复，断言握手完成时 `parkedCount == threadCount`、无死锁（超时看护）。
5. VM 检查点接入：分派循环的调用/回边处替换任务 46 的抢占检查为 `msSafePointCheck`（抢占分支内部委托任务 46 的让出）；确认单线程（`MS_THREADS=1`）行为与任务 17 完全一致的回归。
6. 调度器接入：工作线程主循环取协程前检查点、让出点检查点、空闲即停车不变量、协程全量注册表（创建登记/终态摘除）。验证：ms 脚本多协程 channel 通信结果正确。
7. GC 根集改造：`msGcMarkStaticRoots`（含每线程 C API 根栈迁移）+ `msGcMarkCoroutineRoots`（全协程注册表枚举）；`msGcCollect` 包装为 `msGcCollectSTW`；分配路径的阈值触发与 CAS 收敛。验证：C 单元测试构造"仅被阻塞协程引用"的对象，STW 回收后仍存活；仅被垃圾 channel 引用的对象被回收。
8. 并发压力加固：C 层 N 工作线程随机分配/释放长跑（数十万次 GC），ms 层并发分配压力脚本；配合 ASAN 与任务 02 的分配统计确认无悬垂、无泄漏、无重复释放。

## 测试方案

本任务晚于任务 40（testing 模块），ms 脚本一律使用 `testing` 模块（`import "testing"` + `testing/assert`），由仓库根 `run_tests.py` 驱动 mslang CLI 执行；同时保留一组 C 单元测试覆盖协议本身。ms 压力脚本需在 `MS_THREADS=4`（或更大）下运行才有意义，`run_tests.py` 层以环境变量控制；同样脚本在 `MS_THREADS=1` 下须全部通过（回归）。

测试文件清单（本任务只交付本设计文档，测试代码随实现任务编写）：

- `tests/c/test_gc_safepoint.c`（自研 `ms_test.h`，`MS_TEST`/`MS_ASSERT_EQ`）：
  - 注册表：attach/detach 计数、出生即停车语义、重复 attach 断言；
  - 握手：N 个假工作线程 + 反复 STW/恢复（≥1000 轮），每轮断言握手完成条件与停车后世界静止（线程不再推进模拟 pc）；
  - CAS 收敛：多线程同时触发，断言每轮恰好一个发起者、其余线程停车等待；
  - 空闲即停车：线程在"空闲阻塞"状态下参与握手；
  - 根集：构造运行中/就绪/阻塞三类协程分别持有的对象，STW 后断言存活；断言不可达 channel 及其等待值被回收。
- `tests/ms/concurrency/gc_stress_alloc_test.ms`：8+ 个协程经 channel 交换大量新分配对象（字符串、list、dict），主协程汇总校验结果与总数；全程触发多次 GC（分配量远超 1MB 初始阈值）。
- `tests/ms/concurrency/gc_coroutine_roots_test.ms`：协程在 channel 上阻塞时其局部变量构造的对象，经后续分配压力触发 GC 后，唤醒协程并断言对象内容完好；`await` 挂起协程的栈上对象同理。
- `tests/ms/concurrency/gc_preempt_mix_test.ms`：纯计算长循环（走回边检查点与任务 46 抢占）与分配型协程混合运行，断言 GC 期间不发生饿死、最终结果正确。
- 回归：任务 17 与任务 43–46 的全部既有测试在单线程与多线程配置下均须通过。

## 验收标准

- [ ] `src/gc/ms_safepoint.h` / `ms_safepoint.c` 存在，guard 为 `MSLANG_SRC_GC_MS_SAFEPOINT_H_`，头文件自包含；代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsVmThread`/`struct MsSafePoint` 不 typedef）；原子操作只经 `src/platform/` 抽象，GC 代码无平台 `#ifdef`。
- [ ] VM 检查点只读一个全局原子字（`MS_SP_FLAG_PREEMPT | MS_SP_FLAG_STOP_WORLD`），位于函数调用、回边与全部让出点（channel 操作、`await`、`select`、`time.sleep`、工作线程取协程前），快路径零标志时立即返回。
- [ ] STW 握手满足：握手完成时 `parkedCount == threadCount`，停车线程不执行字节码/分配/调度操作；`phase` CAS 保证唯一发起者；空闲工作线程以"空闲即停车"参与协议；外部线程经 attach/`msSafePointResume` 纳入协议。
- [ ] 锁序规则落实：`msSafePointCheck` 与 GC 发起等待均不持有调度器/channel 锁，持锁路径内无检查点与分配；压力测试下无死锁。
- [ ] 根集覆盖静态根（模块注册表、内建类型表、驻留缓存、各注册线程的 C API 根栈）与全协程根（经调度器全协程注册表枚举，含就绪/阻塞/运行中协程的调用栈、求值栈、upvalue、`result`）；channel 等待队列不作为遍历入口。
- [ ] `msGcCollectSTW` 后阈值自适应更新，并发触发经 CAS 收敛，OOM 语义与任务 17 一致；单线程配置行为与任务 17 完全回归。
- [ ] 「测试方案」全部 C 单元测试与 ms 脚本在 `MS_THREADS=1` 与 `MS_THREADS≥4` 下通过；Debug + ASAN 构建无悬垂引用、泄漏或重复释放；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 17/42/46 的接口假定（`struct MsGc`、`msAtomic*`、`struct MsSched`/`struct MsWorker` 等）在实现时已对齐。
