# 43 协程与 async/await（单线程协作式）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.3 | ⬜ | [09 最小可运行解释器](09-minimal-interpreter.md)、[23 异常系统](23-exceptions.md) |

## 任务目标

交付 mslang 的协程基础设施与 `async`/`await` 运行语义（单线程协作式版本）：

- 协程对象模块（`src/sched/ms_coroutine.h` / `src/sched/ms_coroutine.c`）：`struct MsCoroutine`（独立调用栈 + 求值栈 + 状态机 READY/RUNNING/SUSPENDED/DEAD + `result` + `waiters`），落实 `docs/language/08-vm-internals.md` §6.1。
- 单线程协作式调度器（`src/sched/ms_sched.h` / `src/sched/ms_sched.c`）：全局 FIFO 就绪队列 + 全协程注册表，无工作线程、无抢占（M:N 调度与抢占属任务 46）。
- 语言语义：调用 `async func` 立即返回协程句柄（类型 `coroutine`）而不执行函数体；`await h` 挂起当前协程直到目标完成并取得返回值；同一句柄可被多个协程 `await`。
- 异常跨协程传播：协程内未捕获异常存入句柄，在 `await` 点（及 `h.result()`）于等待方协程重新抛出，traceback 含目标协程栈。
- 句柄方法：`h.done()`、`h.cancel()`（在目标协程下一让出点注入 `CancelledError`）、`h.result()`（非阻塞，未完成抛 `RuntimeError`）。
- 死锁检测：就绪队列耗尽而仍有未完成协程时，在主协程抛 `RuntimeError`。

完成后，`tests/ms/concurrency/` 下的脚本可以派生、等待、取消协程并验证异常传播；channel（任务 44）、select（任务 45）、M:N 调度（任务 46）在本任务之外。

## 设计依据

- `docs/language/06-concurrency.md`
  - §1：`async func` 定义协程函数，调用不执行函数体、立即返回句柄；`await h` 挂起当前协程，正常结束得返回值、未捕获异常在 `await` 点重抛（traceback 含目标协程栈）；`await` 可用于任意函数，顶层 `await` 等价于同步等待；同一句柄可被多方 `await` 且各得同一结果；句柄方法 `done()` / `cancel()`（注入 `CancelledError`，`RuntimeError` 子类）/ `result()`（未完成抛 `RuntimeError`）。
  - §4：让出点清单（本任务仅有 `await`；channel、select、`time.sleep` 属后续任务）；抢占属任务 46，本任务无抢占。
  - §6：协程的调用栈/求值栈是 GC 根。
- `docs/language/08-vm-internals.md`
  - §2.1：`MsProto.isAsync` 标志。
  - §2.2：并发指令 `MS_OP_SPAWN`（async 调用）与 `MS_OP_AWAIT`。
  - §3：对象模型，`MS_TYPE_COROUTINE` 类型标签。
  - §4：每协程独立调用栈（`MsCallFrame` 数组）与求值栈，堆上分配、可伸缩。
  - §6.1：`struct MsCoroutine` 字段（frames / stack / state / result / waiters / blockedOn）。
- `docs/language/04-exceptions.md`
  - §4：`CancelledError` 是 `RuntimeError` 的子类。
  - §5：协程中未捕获的异常不崩溃解释器，存于协程句柄，`await` 时重新抛出。
  - §6：零成本 try（异常表查表法）——注入 `CancelledError` 复用既有抛出路径，协程内 `try/except/finally` 对其照常生效。
- `docs/language/03-syntax.md` §5：`funcDecl = [ "async" ] "func" ...`（parser 已产出 async 标志，编译器据此置 `MsProto.isAsync`）。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则（内部结构体不 typedef）、§5 错误处理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）。
- `docs/language/11-project-layout.md` §1：`src/sched/`（协程与 M:N 调度器）的目录位置；`tests/ms/` 由 `run_tests.py` 驱动。
- 任务 09 最小可运行解释器提供：`MsState`（全局命名空间、主协程、错误槽、分配统计）、`msEvalFile` 端到端流程、临时内建 `assert`/`print`。任务 23 异常系统提供：异常对象与层级（`RuntimeError`、`CancelledError`、`TypeError`）、异常表驱动的抛出/捕获/栈展开、traceback 捕获。任务 06/07/08/17/23 的文档尚不存在，其接口名（`struct MsCallFrame`、`msVmResumeCoroutine`、`msRaiseException`、GC 根扫描入口等）为本文假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 范围界定

本任务是 06-concurrency 的单线程子集：**一个 OS 线程、协作式调度、无抢占**。明确不做：channel（任务 44）、select（任务 45）、多工作线程与工作窃取/抢占（任务 46）、`blockedOn` 字段的 channel 语义（任务 44 填充）、`sync` 模块（任务 48）。调度器代码的组织（`src/sched/`、全协程注册表、协程状态机）按任务 46 的扩展方向预留，避免届时重写。

### 文件与模块划分

- 新增 `src/sched/ms_coroutine.h` / `src/sched/ms_coroutine.c`，include guard `MSLANG_SRC_SCHED_MS_COROUTINE_H_`：协程对象、状态机、生命周期（spawn/finish/唤醒）、句柄方法实现。自包含（include `<stdbool.h>` `<stdint.h>` 及对象模型/异常头）。
- 新增 `src/sched/ms_sched.h` / `src/sched/ms_sched.c`，include guard `MSLANG_SRC_SCHED_MS_SCHED_H_`：调度器结构、就绪队列操作、调度主循环、死锁检测。
- 修改 `src/vm/`：`MsState` 内嵌 `struct MsScheduler`；主协程由裸栈结构重定形为 `MsCoroutine`；调用分派接入 async 分支；新增 `MS_OP_AWAIT` 分派；`MS_OP_RETURN` 展开到底层帧时接协程完成路径；未捕获异常的栈展开终点接 `msCoroFinish`。
- 修改 `src/compiler/`：`async func` 置 `MsProto.isAsync = true`（parser 侧 async 标志既有，仅编译器透传）。
- 修改 `src/gc/ms_gc.c`：根扫描从"主协程一组栈"改为枚举调度器全协程注册表（最小改动；正式多线程化属任务 47）。

### 协程对象与状态机

落实 08-vm-internals §6.1，`state` 由 `int` 细化为枚举：

```c
typedef enum {
  MS_CORO_READY,      // in the scheduler ready queue
  MS_CORO_RUNNING,    // currently executing (exactly one at a time)
  MS_CORO_SUSPENDED,  // parked in another coroutine's waiters queue
  MS_CORO_DEAD        // finished; result/resultIsError are final
} MsCoroState;

struct MsCoroutine {
  MsObjectHeader header;             // MS_TYPE_COROUTINE
  struct MsCallFrame* frames;        // call stack, heap-allocated, growable
  int frameCount;
  int frameCapacity;
  MsObject** stack;                  // evaluation stack, heap-allocated, growable
  int stackTop;
  int stackCapacity;
  MsCoroState state;
  MsObject* result;                  // return value, or the uncaught exception instance
  bool resultIsError;                // true: result holds an uncaught exception
  bool cancelRequested;              // CancelledError pending injection at next resume
  struct MsCoroutine* waiters;       // intrusive LIFO list of coroutines awaiting this one
  struct MsCoroutine* nextWaiter;    // link while this coroutine is parked as a waiter
  struct MsCoroutine* readyNext;     // link while this coroutine is in the ready queue
  struct MsCoroutine* registryNext;  // link in the scheduler's all-coroutines registry
};
```

设计说明：

- `waiters` 用侵入式链表（头插）实现，唤醒时不保证 FIFO 顺序——规范只承诺"都获得同一结果"，未承诺唤醒顺序，避免为尾指针付出额外字段；`blockedOn`（08-vm-internals §6.1）由任务 44 引入，本结构预留注释位。
- `result` 必须配 `resultIsError`：协程可以正常 `return` 一个异常实例，与"未捕获异常"必须区分。
- 状态迁移：`READY → RUNNING`（调度器取出）、`RUNNING → SUSPENDED`（await 挂起）、`SUSPENDED → READY`（目标完成被唤醒）、`RUNNING → DEAD`（return 到底或未捕获异常展开到底）。`DEAD` 为终态；不存在 `SUSPENDED → DEAD`（cancel 不直接杀死，只在恢复运行时注入异常，见下）。

### 调度器

```c
struct MsScheduler {
  struct MsCoroutine* readyHead;     // global FIFO ready queue (intrusive list)
  struct MsCoroutine* readyTail;
  struct MsCoroutine* current;       // the RUNNING coroutine (main at top level)
  struct MsCoroutine* main;          // main coroutine, re-homed from task 09's bare stacks
  struct MsCoroutine* registry;      // every live coroutine, for GC root enumeration
  int64_t liveCount;                 // non-DEAD coroutine count (deadlock detection)
};
```

```c
// Initializes the scheduler embedded in L and re-shapes the task-09 main
// coroutine into an MsCoroutine registered as main/current. Called from
// msNewState.
void msSchedInit(MsState* L);

// Releases all coroutine stacks and the registry. Called from msCloseState
// after the object teardown of every MsCoroutine handle.
void msSchedDestroy(MsState* L);

// Enqueues co at the tail of the ready queue. co must be in a state that
// permits scheduling (freshly spawned or woken); MS_ASSERT in debug builds.
void msSchedEnqueue(MsState* L, struct MsCoroutine* co);

// Runs ready coroutines until the main coroutine becomes DEAD, then returns
// MS_OK (or the main coroutine's own unhandled-error result). Also the entry
// used whenever the running coroutine suspends on await. Detects deadlock:
// ready queue empty while liveCount > 0 (excluding a DEAD main) raises
// RuntimeError("deadlock: all coroutines suspended") on the main coroutine.
MsResult msSchedRun(MsState* L);
```

调度主循环：

1. 弹出 `readyHead`（FIFO），置 `RUNNING`、`current`。
2. 恢复前检查 `cancelRequested`：若置位则清标志，在该协程当前 pc 处走任务 23 的抛出路径注入 `CancelledError`（异常表照常匹配，协程内 `try/except` 可捕获、`finally` 照常执行）。
3. 调用任务 08 分派循环的协程化入口（假定名 `msVmResumeCoroutine(L, co)`，返回"让出/完成/错误"三态）。
4. 让出：协程已置 `SUSPENDED` 并挂在目标 `waiters` 上，回步骤 1。
5. 完成/错误：`msCoroFinish` 已置 `DEAD`、存结果、唤醒全部 waiters（逐个置 `READY` 入队尾，`liveCount--`），回步骤 1。
6. 就绪队列空：`main` 为 `DEAD` → 返回；否则 `liveCount > 0` → 死锁，在 `main` 上抛 `RuntimeError` 后返回错误结果（含 `await` 环、永久挂起等一切无法推进的情形）。

主协程即程序入口：`msEvalFile` 在 `main` 上执行顶层 Proto；顶层 `await` 走同一条挂起路径，`msSchedRun` 接管直到 `main` 恢复并完成——这正好实现"顶层 await 等价于同步等待"。

### async 调用的编译与分派

动态语言中调用点无法在编译期确知被调者是否 async（变量可重绑定）。因此：

- 编译器对一切调用照常生成 `MS_OP_CALL` / `MS_OP_CALL_KW`；仅把 `async func` 编译为 `isAsync = true` 的 `MsProto`。
- VM 调用分派检测到被调对象为 `MS_TYPE_FUNCTION` 且 `proto->isAsync` 时，**不压帧执行**，转入 `msCoroSpawn`：创建 `MsCoroutine`（独立调用栈/求值栈，初始容量同主协程默认值），把被调函数与参数布置为其首帧，置 `READY` 入队尾，然后把协程句柄（协程对象本身即脚本层 `coroutine` 类型的值）压入当前协程求值栈作为调用结果。函数体一条指令也不执行（06-concurrency §1）。
- `MS_OP_SPAWN`（08-vm-internals §2.2）作为该分派的内部实现路径保留；本任务编译器不生成它，留给将来编译器可静态判定 `isAsync` 的优化（属任务 46 之后的优化候选）。

```c
// Spawns a coroutine for a call to an async function. Lays out the first
// call frame from asyncFn and [argv, argv+argc), enqueues the coroutine as
// READY, and returns the coroutine object (the script-level handle). The
// callee body is not executed. Returns NULL on OOM (error slot set).
struct MsCoroutine* msCoroSpawn(MsState* L, MsObject* asyncFn, int argc, MsObject** argv);
```

### await 语义（MS_OP_AWAIT）

分派循环中的 `MS_OP_AWAIT`（栈顶为句柄 `h`）：

1. 类型检查：`h` 不是 `MS_TYPE_COROUTINE` → 在当前协程抛 `TypeError`。
2. 自等待检查：`h == current` → 抛 `RuntimeError`（必死锁，立即失败优于调度期死锁）。间接等待环（A await B、B await A）由调度器死锁检测统一报 `RuntimeError`。
3. 快路径：`h` 已 `DEAD` → 弹栈，`resultIsError == false` 则压入 `result` 继续；为 `true` 则在当前协程重抛该异常实例（走任务 23 抛出路径，traceback 追加当前协程的等待帧）。
4. 慢路径：当前协程置 `SUSPENDED`、`nextWaiter` 头插入 `h->waiters`，**pc 不越过本指令**，分派循环向 `msSchedRun` 返回"让出"。
5. 恢复语义：协程被唤醒重新运行时 pc 仍指向同一条 `MS_OP_AWAIT`，重执行后必走第 3 步快路径——挂起/恢复无需保存额外现场，求值栈上的句柄原样保留至重执行时弹出。

`await` 不限于 async 函数内（06-concurrency §1）：普通函数与顶层都走同一路径；当 `current == main` 时挂起主协程，`msSchedRun` 继续驱动其余协程直到目标完成唤醒 `main`。

### 协程完成与异常跨协程传播

```c
// Terminal path of a coroutine: stores the outcome, marks it DEAD, wakes
// every waiter (READY, tail of ready queue), and unregisters liveness.
// Invoked from two places only: MS_OP_RETURN unwinding past the base frame
// (isError = false, result = return value), and the uncaught-exception
// unwinder of task 23 reaching the base frame (isError = true, result = the
// exception instance with the target coroutine's traceback already attached).
void msCoroFinish(MsState* L, struct MsCoroutine* co, MsObject* result, bool isError);
```

传播语义（04-exceptions §5、06-concurrency §1）：

- 协程内未捕获异常**不崩溃解释器**：任务 23 的栈展开到达该协程底层帧时转入 `msCoroFinish(isError = true)`；异常实例上的 traceback 捕获于展开起点，已含目标协程栈。
- 每个被唤醒的等待方在其 `await` 点重抛同一异常实例；等待方不捕获则继续向其等待方传播——跨协程链式传播由"完成即唤醒、唤醒即重抛"自然组成，无需额外机制。
- 多个等待方重抛的是**同一异常实例**（规范承诺"都获得同一结果"，异常亦同）；重抛只追加等待方自身帧到 traceback，不重置既有内容（对齐 04-exceptions §2 的 `raise e` 语义）。

### 句柄方法

`coroutine` 为内建类型，三个方法以 `MsCFunction` 挂到其类型方法表（内建类型方法表机制假定自任务 06/10，定名以实现时对齐为准）：

```c
// h.done() -> bool: true iff the coroutine is MS_CORO_DEAD.
static MsObject* msCoroMethodDone(MsState* L, int64_t argc, MsObject** argv);

// h.result(): non-blocking. Raises RuntimeError if the coroutine is not
// DEAD. If it finished with an uncaught exception, rethrows that instance
// (same as await); otherwise returns the stored return value.
static MsObject* msCoroMethodResult(MsState* L, int64_t argc, MsObject** argv);

// h.cancel() -> bool: false if already DEAD; otherwise sets cancelRequested
// and returns true. The CancelledError is injected the next time the target
// coroutine is resumed (its next scheduling turn), reusing the normal raise
// path so try/except/finally inside the target apply.
static MsObject* msCoroMethodCancel(MsState* L, int64_t argc, MsObject** argv);
```

设计决策（对规范空白的补足，已在文中各处置明）：

- `cancel()` 返回 `bool` 表示是否成功标记（规范只描述注入行为，未给返回值）；对 `DEAD` 协程调用为 no-op 返回 `false`。
- "下一个让出点注入"在单线程协作式下的落地：目标协程被 `cancel()` 时必然不处于运行态（调用者才是 `current`），故注入点统一为"下次被调度恢复时"，由 `msSchedRun` 步骤 2 实施；这与 06-concurrency §1 的语义等价（目标感知取消的最早时机本就是其下一次获得执行权）。
- 目标协程未捕获 `CancelledError` → 正常走 `msCoroFinish(isError = true)`，其等待方在 `await` 点收到 `CancelledError` 重抛。
- `result()` 对异常完成的协程重抛该异常（规范只规定"未完成抛 RuntimeError"；此选择与 `await` 语义保持一致，避免同一终态两种取结果语义）。

### GC 协作

- `MsScheduler.registry` 是全部存活协程的侵入式链表：spawn 时登记，`DEAD` 协程的句柄对象被 GC 回收时才摘除（`DEAD` 不等于可回收——句柄可能仍被脚本持有查 `result()`）。
- `src/gc/ms_gc.c` 根扫描最小改动：从"主协程一组栈"改为遍历 `registry`，对每条协程标记其调用栈（`proto`、upvalue）、求值栈、`result`、`waiters` 链。多线程 safepoint 化属任务 47，本任务单线程无此需求。

## 实现步骤

1. 建 `src/sched/ms_coroutine.h` / `ms_coroutine.c` 骨架：`MsCoroState`、`struct MsCoroutine`、协程对象的分配/销毁（栈数组经 `msAlloc`/`msRealloc`/`msFree`）、类型对象注册（`MS_TYPE_COROUTINE`）。验证：既有测试全部通过（尚无脚本可触达路径）。
2. 建 `src/sched/ms_sched.h` / `ms_sched.c`：`struct MsScheduler` 内嵌 `MsState`、`msSchedInit`/`msSchedDestroy`、主协程重定形、`msSchedEnqueue`、全协程注册表。验证：任务 09 起的全部既有脚本测试回归通过（单协程行为不变）。
3. GC 根扫描改为枚举 `registry`（含 `result`/`waiters`）。验证：既有 GC 相关测试回归通过。
4. 编译器透传 `async func` → `MsProto.isAsync`；调用分派接 `msCoroSpawn`。验证：ms 脚本中调用 async 函数返回句柄、`type(h) == coroutine`、函数体未执行（副作用计数为零）。
5. 实现 `msSchedRun` 主循环与 `MS_OP_AWAIT` 慢路径（挂起/pc 不前进/唤醒重执行）及快路径。验证：spawn 顺序、FIFO 调度顺序、`await` 取返回值、多等待方、普通函数与顶层 `await`。
6. `MS_OP_RETURN` 到底与任务 23 展开到底两条路径接 `msCoroFinish`。验证：正常 return 值传递、协程内未捕获异常在 `await` 点重抛且类型/traceback 正确、链式传播。
7. 死锁检测（含自 await 立即报错）。验证：互相 await 的两个协程使进程以运行时错误退出。
8. 句柄方法 `done`/`result`/`cancel` 与 `CancelledError` 注入。验证：各方法的全部分支（见测试方案）。
9. 全平台 Debug/Release 构建，Debug（ASAN）下跑全部测试，配合任务 02 分配统计确认无泄漏（含异常完成与被取消协程的栈释放）。

## 测试方案

本任务晚于任务 40（testing 模块），ms 脚本一律使用 `testing` 模块，由仓库根 `run_tests.py` 驱动 mslang CLI 执行。测试文件清单（本任务只交付本设计文档，测试代码随实现任务编写）：

- `tests/ms/concurrency/spawn_basic_test.ms`：调用 async 函数立即返回（函数体内副作用计数在调用后仍为 0）、句柄类型为 `coroutine`、`await` 后函数体恰好执行一次并取得 `return` 值；无 `return` 的 async 函数 `await` 得 nil。
- `tests/ms/concurrency/sched_order_test.ms`：多个协程经共享 list 记录执行轨迹，断言 spawn 按 FIFO 入队、`await` 挂起后运行下一就绪协程、唤醒后从挂起点继续（局部变量现场完整）。
- `tests/ms/concurrency/await_sites_test.ms`：普通（非 async）函数内的 `await`；顶层 `await` 等价同步等待（顶层语句顺序断言）；`await` 非句柄值抛 `TypeError`。
- `tests/ms/concurrency/multi_waiter_test.ms`：三个协程 `await` 同一句柄，全部得到同一结果对象（含 `is` 同一性断言）；句柄完成后再 `await` 立即返回。
- `tests/ms/concurrency/exception_propagate_test.ms`：协程内未捕获异常（`ValueError`）在 `await` 点重抛、类型与消息保持、可被等待方 `try/except` 捕获、traceback 含目标协程函数名；`a await b`、`b await c`、`c` 抛异常的三级链式传播；协程正常 `return` 一个异常实例时 `await` 返回值本身而非抛出（`resultIsError` 区分）。
- `tests/ms/concurrency/handle_methods_test.ms`：`done()` 在完成前后分别为 `false`/`true`；`result()` 未完成时抛 `RuntimeError`、正常完成后返回值、异常完成后重抛该异常；对已 `DEAD` 句柄反复 `result()`/`await` 结果一致。
- `tests/ms/concurrency/cancel_test.ms`：对已 spawn 未完成的协程 `cancel()` 返回 `true`，目标协程内 `try/except CancelledError` 捕获到注入；未捕获时 `await` 该句柄重抛 `CancelledError`（且其是 `RuntimeError` 子类）；目标 `finally` 在取消路径上照常执行；对 `DEAD` 协程 `cancel()` 返回 `false`。
- `tests/ms/concurrency/deadlock_test.ms`（负向用例，配 `<name>.exit` 声明预期退出码 1）：两个协程互相 `await`；以及协程 `await` 自身立即抛 `RuntimeError`（后者可放入正向脚本断言）。
- 回归：任务 09–41 的全部既有脚本测试（单协程行为）不变通过。

## 验收标准

- [ ] `src/sched/ms_coroutine.{h,c}`（guard `MSLANG_SRC_SCHED_MS_COROUTINE_H_`）与 `src/sched/ms_sched.{h,c}`（guard `MSLANG_SRC_SCHED_MS_SCHED_H_`）存在，头文件自包含；代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsCoroutine`/`struct MsScheduler` 不 typedef）；堆分配只经 `msAlloc/msRealloc/msFree`。
- [ ] 调用 `async func` 不执行函数体、立即返回 `coroutine` 句柄；函数体在调度器运行到该协程时才执行。
- [ ] `await` 语义完整：挂起/恢复（求值栈与局部现场不变）、取返回值、多等待方同一结果、任意函数与顶层可用、非句柄抛 `TypeError`。
- [ ] 未捕获异常存于句柄并在 `await` 点重抛（traceback 含目标协程栈），跨协程链式传播正确；正常返回异常实例不被误判为失败。
- [ ] `done()`/`result()`/`cancel()` 行为与本文「句柄方法」一致，`CancelledError` 在目标协程下次恢复时注入且 `try/except/finally` 照常生效。
- [ ] 单线程 FIFO 就绪队列 + 全协程注册表实现；就绪耗尽且仍有存活协程时报 `RuntimeError` 死锁；自 `await` 立即报错。
- [ ] GC 根覆盖全部协程的调用栈/求值栈/`result`/`waiters`；长等待链上的对象在 GC 后完好。
- [ ] 「测试方案」全部 ms 脚本通过；任务 09–41 既有测试回归通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 06/07/08/17/23 的接口假定（`MsCallFrame`、`msVmResumeCoroutine`、`msRaiseException`、内建类型方法表、GC 根扫描入口等）在实现时已对齐。
