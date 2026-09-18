# 48 标准库：sync

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.3 | ⬜ | [46 M:N 调度器（工作窃取与抢占）](46-scheduler-mn.md) |

## 任务目标

交付 C 内建标准库模块 `sync`（`stdlib/sync/ms_mod_sync.h` / `stdlib/sync/ms_mod_sync.c`），完整实现 `docs/language/07-stdlib.md` §18 列出的全部 5 个名字：`mutex()`（`lock`/`unlock`，支持 `with`）、`rwMutex()`（`rlock`/`runlock`/`lock`/`unlock`）、`waitGroup()`（`add`/`done`/`wait`，协程感知）、`once()`（`do(f)`）与 `setMaxThreads(n)`。

核心约束来自 `docs/language/08-vm-internals.md` §6.2：sync 原语**直接操作协程状态（挂起/唤醒），不依赖 OS 互斥锁**实现脚本级阻塞语义——`mutex.lock()` 争用失败、`waitGroup.wait()` 计数未归零时，挂起的是当前协程，其所在工作线程立即转去运行其他就绪协程，绝不阻塞 OS 线程。同时建立 06-concurrency §5 第 4 条要求的 happens-before 边。完成后，`tests/ms/stdlib/sync/` 下的脚本（testing 模块）可在 `MS_THREADS=1` 与多工作线程配置下验证全部语义；任务 47 的 GC safepoint 使本模块对象在 STW 期间静止可扫。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0：模块分两类，`sync` 为 C 内建模块、置于 `stdlib/` 目录；函数/方法命名小驼峰。
  - §18：sync 模块接口全集（本任务范围的唯一权威来源）。
  - §25 协程感知 API 标注：`sync.waitGroup.wait` 阻塞时挂起协程而非 OS 线程。该清单未列 `mutex.lock` 等，但 08-vm-internals §6.2 与 06-concurrency §5 的示例（`mu.lock()` 用于多协程临界区）蕴含同样语义——本任务将**全部可能阻塞的 sync 方法**一律实现为协程让出点，并在此显式记录这一对 §25 清单的补足。
- `docs/language/06-concurrency.md`
  - §4：`sync.setMaxThreads(n)` 调整调度器工作线程数（默认 CPU 核数，`MS_THREADS` 环境变量可预设）。
  - §5：内存模型第 4 条——`sync` 模块的 `Mutex`/`WaitGroup` 建立各自的 happens-before 边；`with mu { ... }` 语法（`Mutex` 实现 `__enter__`/`__exit__`）。
  - §6：GC STW 期间世界静止，sync 对象的内部状态与等待队列随之静止，无需并发扫描对策。
- `docs/language/08-vm-internals.md`
  - §6.1 `struct MsCoroutine`：`state`、`waiters`、`blockedOn` 字段是协程挂起/唤醒的承载。
  - §6.2：`sync.Mutex` 等原语直接操作协程状态（挂起/唤醒），不依赖 OS 互斥锁（同调度器内）。
- `docs/language/09-c-api.md` §3（GC 根栈纪律）、§8（C 函数出错置错误槽并返回 `NULL`）、§9（`MsModuleDef`/`msRegisterModule` 与 `mslangInit_<name>` 约定）、§10（`MsTypeDef`/`msDefineType`/`msCInstanceData`——四种同步类型的实现基础）。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则、§5 错误处理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）、§9（OS 原语只经 `src/platform/` 抽象，本模块不得出现平台 `#ifdef`）。
- `docs/language/11-project-layout.md` §1：`stdlib/` 为 C 标准库模块目录；`tests/ms/` 由仓库根 `run_tests.py` 驱动。
- `docs/tasks/README.md` 测试约定：本任务编号 ≥ 40，ms 脚本一律使用 `testing` 模块（`import "testing"` + `testing/assert`，`xxx_test.ms` 命名，测试函数 `test*` 前缀，`testing.run()` 收尾）。
- 任务 42 平台抽象层提供：`struct MsMutex`（`msMutexInit`/`msMutexLock`/`msMutexUnlock`/`msMutexDestroy`）与 `MsAtomicU32`（`msAtomicU32Init`/`msAtomicU32LoadAcquire`/`msAtomicU32StoreRelease`）。**用途限定**：这些 OS 原语只用作 sync 对象内部簿记状态的短时保护锁与 `Once` 快路径原子字，绝不持锁等待脚本级条件——脚本级阻塞一律走协程挂起，这是对 §6.2「不依赖 OS 互斥锁」的落实而非违反（M:N 下多个工作线程会并发触碰同一 sync 对象的等待队列，簿记必须有序；簿记锁永不跨让出点持有）。
- 任务 43 协程提供：`struct MsCoroutine`（含 `nextWaiter` 侵入式等待链字段）、`MS_CORO_SUSPENDED`/`MS_CORO_READY` 状态机、全协程注册表（挂起协程始终是 GC 根）、死锁检测（就绪耗尽且仍有存活协程时报 `RuntimeError`——sync 等待环复用此机制，本任务不另做死锁检测）。
- 任务 29（with 语句）提供 `__enter__`/`__exit__` 协议；任务 40（testing 模块）提供 `testing.run` 与 `testing/assert` 的 `assert.equal`/`assert.isTrue`/`assert.raises` 等。
- 任务 18（C API 基础）与任务 46（M:N 调度器）的设计文档本文撰写时尚不存在。本文假定任务 18 提供 `msDefineType`/`msCInstanceData`/`msRegisterModule`/`msRaiseTypeError`/`msRaiseValueError`/`msRaiseRuntimeError` 与脚本函数调用入口 `msCall`；假定任务 46 提供「与调度器的接口」一节列出的四个原语。上述均为假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件布局

```
stdlib/sync/
├── ms_mod_sync.h     # 模块对外声明：注册入口
└── ms_mod_sync.c     # 四种同步类型（MsTypeDef）、方法实现、MsModuleDef 注册表
```

头文件 include guard `MSLANG_STDLIB_SYNC_MS_MOD_SYNC_H_`，自包含（include `<stdint.h>` 及 C API 头）。全部方法实现与内部结构体为 `ms_mod_sync.c` 文件内 `static`/文件局部，唯一导出符号是注册入口。模块自身不做堆分配：四种实例的附加数据由 `msDefineType` 的 `instanceSize` 机制内嵌于对象（09-c-api §10），等待队列是协程对象间的侵入式链接。

### 2. 与调度器的接口（任务 46 假定原语）

sync 方法在 VM 分派中以普通 `MsCFunction` 形式被调用；需要阻塞时不能自行返回"稍后再试"，而要让当前协程在整个 VM 层面让出。为此假定任务 46 提供（单线程调度器下任务 43 的等价物亦应收敛到同一组名字）：

```c
// Returns the coroutine currently running on this worker thread.
struct MsCoroutine* msSchedCurrent(MsState* L);

// Marks the current coroutine MS_CORO_SUSPENDED and returns the scheduler's
// yield sentinel. A coroutine-aware MsCFunction returns this value as its
// result: the VM abandons the native call without advancing pc and yields
// the worker. When the coroutine is later woken, the native call is
// re-executed from its entry — the same re-execution model as MS_OP_AWAIT
// (task 43), so the re-entered C function must observe its granted state
// and return normally. The caller must have linked the coroutine onto the
// owner's wait queue BEFORE calling this.
MsObject* msSchedSuspendCurrent(MsState* L);

// Transitions co from MS_CORO_SUSPENDED to MS_CORO_READY and enqueues it.
// Safe to call from any worker thread (cross-worker wakeup); also safe when
// co's previous worker has marked it suspended but not yet returned to the
// scheduler loop (task 46 guarantees no double-run in that window).
void msSchedWake(MsState* L, struct MsCoroutine* co);

// Adjusts the worker thread count to n (>= 1). Growing spawns workers;
// shrinking flags excess workers to exit after their current coroutine
// yields (no coroutine is interrupted mid-flight). Overrides the MS_THREADS
// environment default for the remainder of the process.
MsResult msSchedSetMaxThreads(MsState* L, uint32_t n);
```

`ms_mod_sync.c` 内部以两个 `static` 助手收敛等待队列操作（复用 `MsCoroutine.nextWaiter` 链接，FIFO）：

```c
// Appends co to the intrusive FIFO queue [head, tail).
static void syncWaitPush(struct MsCoroutine** head, struct MsCoroutine** tail, struct MsCoroutine* co);

// Pops the head, or NULL when empty.
static struct MsCoroutine* syncWaitPop(struct MsCoroutine** head, struct MsCoroutine** tail);
```

挂起协议（所有阻塞方法共用，顺序不可交换）：

1. `msMutexLock(&guard)`；
2. 复查条件（快路径可能在加锁前已失败）——条件满足则消费授权（见各类型算法）并解锁返回；
3. 条件不满足：`syncWaitPush` 把 `msSchedCurrent(L)` 挂上等待队列；
4. `msMutexUnlock(&guard)`；
5. 立即 `return msSchedSuspendCurrent(L);`。

步骤 3–5 之间无其他语句。唤醒方（`unlock`/`done` 等）同样持 `guard` 完成"出队 + 状态移交 + `msSchedWake`"，因此"先入队后挂起"与"先移交后唤醒"在簿记锁下互为原子，不存在丢失唤醒；任务 46 保证唤醒一个刚被标记挂起、其工作线程尚未退出的协程是安全的（接口注释已载明）。

### 3. 类型注册与实例数据

四种类型经 `msDefineType` 注册（`name` 分别为 `"Mutex"`/`"RWMutex"`/`"WaitGroup"`/`"Once"`），实例附加数据经 `msCInstanceData` 取得。内部结构体不 typedef：

```c
struct MsSyncMutex {
  struct MsMutex guard;          // bookkeeping lock, never held across a yield
  bool locked;
  bool grantPending;             // a handoff grant is outstanding (see §4)
  struct MsCoroutine* owner;     // last acquirer; reentrancy check + diagnostics
  struct MsCoroutine* waitHead;
  struct MsCoroutine* waitTail;
};

struct MsSyncRWMutex {
  struct MsMutex guard;
  int64_t readerCount;           // active readers, including unconsumed reader grants
  bool writerActive;
  bool writerGrant;              // a writer handoff grant is outstanding
  struct MsCoroutine* writerOwner;
  struct MsCoroutine* readerHead;
  struct MsCoroutine* readerTail;
  struct MsCoroutine* writerHead;
  struct MsCoroutine* writerTail;
};

struct MsSyncWaitGroup {
  struct MsMutex guard;
  int64_t count;
  struct MsCoroutine* waitHead;
  struct MsCoroutine* waitTail;
};

struct MsSyncOnce {
  MsAtomicU32 done;              // fast path: acquire load, no guard
  struct MsMutex guard;
  bool inProgress;
  struct MsCoroutine* executor;  // coroutine currently running f, or NULL
  struct MsCoroutine* waitHead;
  struct MsCoroutine* waitTail;
};
```

- `finalize` 回调只做 `msMutexDestroy(&guard)`（平台层簿记锁，零堆分配）；不访问其他脚本对象，遵守 09-c-api §10 的终结器约束。
- 等待队列链接指向 `MsCoroutine`（GC 对象）但不构成 GC 边：挂起的协程始终在调度器全协程注册表中（任务 43/46），经注册表根可达；sync 实例故无需 `mark` 回调。此不变量在评审清单中固定列出。
- `MsTypeDef.methods` 方法表（普通 `MsCFunction`，接收者经任务 18 的 C 方法绑定机制取得）：
  - Mutex：`lock`、`unlock`、`__enter__`、`__exit__`；
  - RWMutex：`rlock`、`runlock`、`lock`、`unlock`、`__enter__`、`__exit__`（`__enter__` 取**写锁**，对齐"with 保护临界区"的直觉；规范只要求 Mutex 支持 with，此为对规范空白的补足决策，已在测试锁定）；
  - WaitGroup：`add`、`done`、`wait`；
  - Once：`do`。

方法统一骨架（以 Mutex.lock 为例，仅示意分派结构，非完整实现）：

```c
static MsObject* syncMutexLock(MsState* L, int64_t argc, MsObject** argv) {
  struct MsSyncMutex* m = msCInstanceData(argv[0]);   // receiver binding per task 18
  struct MsCoroutine* self = msSchedCurrent(L);
  msMutexLock(&m->guard);
  // consume a handoff grant / take the free lock / detect reentrancy /
  // or enqueue + return msSchedSuspendCurrent(L) after unlocking the guard
  ...
}
```

### 4. Mutex 算法

状态移交采用**授权（grant）交接**：解锁方不释放锁态，直接把持有权移交给被唤醒者，消除"唤醒后重新争抢"的惊群与第三方插队窗口。

- `lock()`（持 `guard` 判定）：
  1. `grantPending && ...`：授权是可互换的——凡在授权未消费期间进入 `lock()` 的协程均可消费之：`grantPending = false`、`owner = self`，解锁返回（被授权者若被插队，重执行时走下面的普通路径重新入队，正确性不变，仅牺牲严格 FIFO——规范未承诺唤醒顺序，与任务 43 的 LIFO waiters 决策一致）；
  2. `!locked`：`locked = true`、`owner = self`，解锁返回；
  3. `locked && owner == self`：`RuntimeError("sync.mutex: re-entrant lock")`（规范未定义重入；Go 语义为死锁，本任务选择立即报错——立即失败优于调度期死锁，与任务 43 对自 `await` 的决策同构）；
  4. 其余：入队、解锁、`msSchedSuspendCurrent`。
- `unlock()`：`!locked` → `RuntimeError("sync.mutex: unlock of unlocked mutex")`；否则弹一个等待者：有则 `grantPending = true`、`owner = NULL`（消费时重写）、`msSchedWake`；无则 `locked = false`、`owner = NULL`。跨协程 `unlock` 允许（对齐 Go，`owner` 仅服务重入检测与诊断）。
- happens-before：`unlock` 的簿记写（release 于 `guard` 解锁）先行于后续 `lock` 的授权消费/空锁获取（acquire 于 `guard` 加锁），临界区内的脚本写沿同一 `guard` 锁链传递。
- `with` 支持：`__enter__` = `lock()` 并返回实例自身；`__exit__` = `unlock()`，返回 `false`（不抑制异常，with 协议细节以任务 29 为准）。

### 5. RWMutex 算法

写者优先（避免写者饥饿），授权交接与 Mutex 同构：

- `rlock()`：存在未消费写授权（`writerGrant`）、写者在持（`writerActive`）或有排队写者（`writerHead != NULL`）→ 入读者队列挂起；否则 `readerCount++` 返回。重执行的读者与读者授权同样可互换：`unlock` 唤醒读者前已把名额计入 `readerCount`（见下），被插队的唤醒读者重执行时走普通快路径，记账自然守恒。
- `runlock()`：`readerCount <= 0` → `RuntimeError`；否则 `readerCount--`；归 0 且有排队写者 → 弹一个写者，`writerActive = true`、`writerGrant = true`、`msSchedWake`。
- `lock()`（写）：消费 `writerGrant`（`writerOwner = self`）；或 `!writerActive && readerCount == 0` → `writerActive = true`、`writerOwner = self`；`writerOwner == self` → `RuntimeError`（重入写锁）；否则入写者队列挂起。
- `unlock()`（写）：`!writerActive` → `RuntimeError`；`writerActive = false`、`writerOwner = NULL`；优先弹一个写者移交（同 `runlock` 的交接）；无写者则唤醒**全部**排队读者：出队计数 `k`，`readerCount += k`（名额预记账），逐个 `msSchedWake`。
- 同一写者持锁期间调 `rlock()`（或反之）会自死锁，由调度器死锁检测统一报 `RuntimeError`，本类型不做属主组合检测。

### 6. WaitGroup 算法（协程感知）

- `add(n)`：`n` 必须是 int（否则 `TypeError`）。持 `guard`：`count += n`；`count < 0` → `RuntimeError("sync.waitGroup: negative counter")`（对齐 Go panic 的违例语义）；`count` 归 0 且有等待者 → 出队全部并逐个 `msSchedWake`。正 `n` 与等待中协程并发的复用竞态按 Go 约定属调用方误用，本实现自然语义为：等待者在下一次归 0 时才被唤醒。
- `done()`：等价 `add(-1)`。
- `wait()`：持 `guard` 复查 `count == 0` 则直接返回；否则入队挂起。重执行时 `count == 0` 必成立（唤醒条件即归 0），直接返回——条件幂等，无需授权字段。
- 协程感知（07-stdlib §25）：`wait` 挂起期间其工作线程可运行其他协程；测试以"等待期间另一协程持续推进"断言此性质，而非依赖时序。
- happens-before：使计数归 0 的 `done()`（release 于 `guard`）先行于任一 `wait()` 返回（acquire 于 `guard`）。

### 7. Once 算法

- `do(f)`：`f` 必须可调用（否则 `TypeError`）。
  1. 快路径：`msAtomicU32LoadAcquire(&done) == 1` → 返回 `nil`，不触 `guard`（acquire 配对执行者的 release，保证 `f` 内的写入对后续 `do` 调用方可见）；
  2. 慢路径持 `guard`：`done == 1` → 返回 `nil`；`inProgress && executor == self` → `RuntimeError("sync.once: recursive do")`（立即失败优于死锁）；`inProgress` → 入队挂起（重执行时 `done == 1`，返回 `nil`，**不再调用自己的 `f`**）；
  3. 否则成为执行者：`inProgress = true`、`executor = self`，**解 `guard`** 后经 C API（假定名 `msCall`）以 0 参调用 `f`——执行期间本协程可正常让出（channel、await 等），`inProgress` 持续为真；
  4. `f` 返回后持 `guard`：`msAtomicU32StoreRelease(&done, 1)`、`inProgress = false`、`executor = NULL`，唤醒全部等待者；
  5. `f` 抛异常：**仍标记 `done = 1`**（对齐 Go：`Once` 认为 `f` 已执行，即使其 panic），照常唤醒等待者（它们见 `done` 返回 `nil`，不感知异常），异常经 `msCall` 的错误槽向本调用方传播（返回 `NULL`）。规范未规定异常语义，此为补足决策。
- `do` 返回值恒为 `nil`（`f` 的返回值丢弃；规范未规定，此为补足决策）。

### 8. setMaxThreads

- `sync.setMaxThreads(n)`：`n` 必须是 int（否则 `TypeError`）且 `>= 1`（否则 `ValueError`）；转 `uint32_t` 后调 `msSchedSetMaxThreads(L, (uint32_t)n)`，失败映射 `msRaiseRuntimeError`；返回 `nil`（规范未规定返回值，此为补足决策）。
- 语义边界（任务 46 职责，本模块仅透传）：扩容立即生效；缩容不中断在飞协程，多余工作线程在当前协程让出后退出；进程启动时的默认值优先级为 `setMaxThreads` 调用 > `MS_THREADS` 环境变量 > CPU 核数（06-concurrency §4）。

### 9. 模块注册

与既有 C 标准库模块同一模式（任务 19/20 建立）：

```c
static const MsMethodDef syncMethods[] = {
  {"mutex", syncNewMutex, "mutex() -> Mutex"},
  {"rwMutex", syncNewRWMutex, "rwMutex() -> RWMutex"},
  {"waitGroup", syncNewWaitGroup, "waitGroup() -> WaitGroup"},
  {"once", syncNewOnce, "once() -> Once"},
  {"setMaxThreads", syncSetMaxThreads, "setMaxThreads(n)"},
  {NULL, NULL, NULL},
};

static const MsModuleDef syncModuleDef = {
  "sync", "coroutine-level synchronization primitives", syncMethods,
};

// Follows the 09-c-api §9 dynamic-loading naming convention.
const MsModuleDef* mslangInit_sync(void);

// Called by the stdlib registrar during msNewState: registers the four
// types (msDefineType) and the module (msRegisterModule).
MsResult msSyncRegister(MsState* L);
```

四个构造器无参数（参数个数不符报 `TypeError`），经类型对象分配实例并初始化簿记字段（`msMutexInit`/`msAtomicU32Init`）；簿记锁初始化失败（`MS_ERROR_*`）映射为运行时错误。

### 10. 错误语义汇总

| 情形 | 异常 |
|---|---|
| 方法/构造器参数个数或类型不符（含 `once.do` 的 `f` 不可调用、`add` 非 int） | `TypeError` |
| `setMaxThreads(0)` 或负数 | `ValueError` |
| Mutex 重入加锁；`unlock` 未持有的 Mutex | `RuntimeError` |
| RWMutex 重入写锁；`runlock` 时 `readerCount == 0`；写 `unlock` 时无写者在持 | `RuntimeError` |
| `add` 使 WaitGroup 计数为负 | `RuntimeError` |
| `once.do` 递归调用 | `RuntimeError` |
| sync 等待构成的协程等待环（互锁、Once/RWMutex 组合自锁） | 调度器死锁检测报 `RuntimeError`（任务 43/46 机制，非本模块职责） |

## 实现步骤

1. 建 `stdlib/sync/ms_mod_sync.{c,h}` 骨架：guard、`mslangInit_sync`、`msSyncRegister`、四个 `MsTypeDef`（方法表先置空）、五个模块函数桩（构造器返回实例、`setMaxThreads` 透传校验）。验证：脚本 `import "sync"` 后 `type(sync.mutex())` 为 `Mutex`、四类型实例可创建可回收。
2. 对齐任务 46 的调度器接口定名，落实 §2 的两个 `static` 队列助手与挂起协议五步法。验证：单协程脚本回归全部通过（尚无可触达的挂起路径）。
3. WaitGroup 全量（`add`/`done`/`wait`、负计数报错、归 0 唤醒全部）。验证：`waitgroup_test.ms` 通过——这是挂起协议的首个端到端检验。
4. Mutex 全量（授权交接、重入报错、`with` 方法）。验证：`mutex_test.ms` 与 `mutex_with_test.ms` 通过。
5. RWMutex 全量（写者优先、读者批量唤醒预记账、两类重入/失衡报错）。验证：`rwmutex_test.ms` 通过。
6. Once 全量（原子快路径、执行者/等待者分派、异常仍标记 done、递归检测）。验证：`once_test.ms` 通过。
7. `setMaxThreads` 与 `MS_THREADS` 优先级联调。验证：`setmaxthreads_test.ms` 在两种环境配置下通过。
8. 全量加固：`tests/ms/stdlib/sync/` 在 `MS_THREADS=1` 与 `MS_THREADS=4` 下各跑一轮；任务 43–47 既有测试回归；Debug（ASAN/LSAN）构建确认无泄漏（含挂起中协程被唤醒/被取消两条路径的实例回收）；grep 确认 `stdlib/sync/` 内无平台条件编译。

## 测试方案

本任务晚于任务 40，ms 脚本一律使用 `testing` 模块（`import "testing"`、`import "testing/assert"`，测试函数 `test*` 前缀，`testing.run()` 收尾），由仓库根 `run_tests.py` 驱动 mslang CLI 执行。全部脚本须在 `MS_THREADS=1` 与 `MS_THREADS=4` 两种配置下通过（`run_tests.py` 以环境变量控制两轮运行）；并发断言一律用 WaitGroup/channel 协调而非 `time.sleep`，涉时序的断言只断言结果正确性与最终状态，不断言交错顺序。测试文件清单（本任务只交付本设计文档，测试代码随实现编写）：

- `tests/ms/stdlib/sync/mutex_test.ms`：N=8 协程 × K=1000 次 `lock`/`unlock` 保护的自增，总计 == N×K；空 Mutex `lock` 立即返回；`unlock` 未持有 Mutex 抛 `RuntimeError`；同协程重入 `lock` 抛 `RuntimeError`；构造器带参数抛 `TypeError`。
- `tests/ms/stdlib/sync/mutex_with_test.ms`：`with mu { ... }` 进入即持锁、块内写共享计数正确；块内抛异常时 `__exit__` 仍解锁且异常向外传播（后续协程可再获取）；`__enter__` 返回实例自身。
- `tests/ms/stdlib/sync/rwmutex_test.ms`：多读者并发持读锁（读者计数峰值 > 1，经临界区内记录观测）；写锁与读锁互斥（写者临界区内读者计数为 0）；写者优先（写者排队期间新读者不插队，以共享日志序列断言读写分组）；`runlock` 失衡、`unlock` 无写者、重入写锁均抛 `RuntimeError`。
- `tests/ms/stdlib/sync/waitgroup_test.ms`：`add(N)` 后 spawn N 个协程各 `done()`，`wait()` 返回后结果列表恰好 N 项；`wait()` 挂起期间其他协程持续推进（协程感知断言）；计数未归零前 `wait()` 不返回（经 `done()` 方记录顺序）；计数为 0 时 `wait()` 立即返回；`add` 致负抛 `RuntimeError`；`add("1")` 抛 `TypeError`。
- `tests/ms/stdlib/sync/once_test.ms`：M 个协程并发 `do(f)`，副作用计数恰好 1 且全部调用方正常返回；`done` 后 `do(g)` 不执行 `g`；`f` 抛异常时本调用方收到该异常、后续 `do` 不再执行 `f` 且不抛（`assert.raises` + 副作用计数断言）；`do` 递归调用抛 `RuntimeError`；`do(42)` 抛 `TypeError`。
- `tests/ms/stdlib/sync/setmaxthreads_test.ms`：`setMaxThreads(1)` 与 `setMaxThreads(4)` 后并发计算结果仍正确（与调度线程数无关性）；`setMaxThreads(0)`/负数抛 `ValueError`；非 int 抛 `TypeError`。
- `tests/ms/stdlib/sync/deadlock_test.ms`（负向用例，配 `deadlock_test.exit` 声明预期退出码 1）：两协程交叉锁定对方持有的 Mutex，调度器死锁检测以 `RuntimeError` 终止进程。
- 回归：任务 43–47 的全部既有并发测试在两种 `MS_THREADS` 配置下不变通过。

## 验收标准

- [ ] `stdlib/sync/ms_mod_sync.{c,h}` 存在，guard 为 `MSLANG_STDLIB_SYNC_MS_MOD_SYNC_H_`，头文件自包含；代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsSyncMutex` 等内部结构体不 typedef）；模块内无平台条件编译，OS 原语只经 `src/platform/ms_platform.h`。
- [ ] 07-stdlib §18 的 5 个名字全部实现；`Mutex` 支持 `with`；`waitGroup.wait` 及全部可能阻塞的 sync 方法以协程挂起实现让出，不阻塞 OS 线程（08-vm-internals §6.2）；簿记锁永不跨让出点持有。
- [ ] 四种类型经 `msDefineType` 注册，实例数据内嵌、零额外堆分配；等待队列为协程侵入式 FIFO 链；`finalize` 只销毁簿记锁；挂起协程经调度器注册表始终是 GC 根，sync 实例无 `mark` 回调的不变量成立。
- [ ] 挂起协议（入队 → 解簿记锁 → `msSchedSuspendCurrent` → 唤醒后重执行消费授权/复查幂等条件）在四个类型中一致落实，多工作线程下无丢失唤醒、无双重运行。
- [ ] 错误语义与「详细设计」第 10 节表格逐条一致；`once.do` 在 `f` 抛异常时仍标记完成且异常向本调用方传播；等待环由调度器死锁检测报 `RuntimeError`。
- [ ] `setMaxThreads` 透传任务 46 的调度器接口，参数校验（int、≥ 1）与返回值（`nil`）符合设计；优先级 `setMaxThreads` > `MS_THREADS` > CPU 核数成立。
- [ ] 「测试方案」全部 ms 脚本在 `MS_THREADS=1` 与 `MS_THREADS=4` 下通过；任务 43–47 既有测试回归通过；Debug + ASAN/LSAN 无泄漏；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 18/29/40/46 的接口假定（`msDefineType`/`msCall`、with 协议、testing 模块、`msSchedCurrent`/`msSchedSuspendCurrent`/`msSchedWake`/`msSchedSetMaxThreads`）在实现时已按对应任务文档定名对齐。
