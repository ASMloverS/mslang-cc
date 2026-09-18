# 44 channel

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.3 | ⬜ | [43 协程与 async/await](43-coroutines.md)、[23 异常系统](23-exceptions.md) |

## 任务目标

交付 mslang 的 channel 类型与全部通道操作（`src/sched/ms_channel.h` / `src/sched/ms_channel.c`），落实 `docs/language/06-concurrency.md` §2 的完整 API：

- **channel 对象**：`struct MsChannel`（环形缓冲 + 双向等待队列 + 关闭标志），类型标签 `MS_TYPE_CHANNEL`，脚本侧类型名 `channel`；channel 是一等值，可作参数/返回值/字典值传递。
- **两种通道**：`chan()` 无缓冲 rendezvous（send 阻塞到有接收方就绪）、`chan(n)` 容量 n 的缓冲通道。
- **六个方法**：`c.send(v)` / `c.recv()`（阻塞，协程让出点）、`c.trySend(v)` / `c.tryRecv()`（非阻塞）、`c.close()`、`c.len()`。
- **内建函数**：`chan(capacity=0)` 与 `cap(c)` 注册进任务 10 的内建表；任务 10 的 `len` 内建扩展接受 channel。
- **关闭语义**：关闭后 send/trySend 抛 `ChannelClosedError`；recv 在关闭且排空后抛 `ChannelClosedError`；close 唤醒全部阻塞方，由重执行路径各自得出上述结果。
- **`for v in c`**：迭代 channel 直到关闭且排空（`ChannelClosedError` 被循环内部消化），经任务 26 迭代协议的扩展实现。
- **与任务 43 的协作**：阻塞/唤醒复用其协程状态机与就绪队列；`h.cancel()` 可取消阻塞在 channel 上的协程；全阻塞无就绪时由任务 43 的死锁检测报 `RuntimeError`。

完成后 `tests/ms/concurrency/` 下的脚本可跑通 06-concurrency §7 的生产者-消费者示例。明确不做：select（任务 45，届时推广单等待节点设计）、多工作线程下的队列加锁与内存序（任务 46）、`MS_OP_CHAN_*` 指令的编译期生成（见「VM 接线」节的理由）。本任务只交付设计文档；实现与测试代码随实现任务编写。

## 设计依据

- `docs/language/06-concurrency.md`
  - §2：`chan()` / `chan(16)` 两种构造；`send`/`recv`/`trySend`/`tryRecv`/`close`/`len` 方法与 `cap(c)` 内建的签名与逐句注释语义（rendezvous、缓冲区满让出、关闭且排空后 recv 抛 `ChannelClosedError`、try 系列返回值约定）；channel 是一等值；channel 收发是协程让出点；`for v in c` 迭代到关闭且排空、`ChannelClosedError` 被循环内部消化。
  - §4：channel 操作属让出点清单；本任务阶段调度器仍是任务 43 的单线程协作式。
  - §5 内存模型第 1 条：`c.send(v)` 的写入 happens-before 对应 `c.recv()` 返回；单线程下由配对唤醒的因果序平凡成立（见「GC 与内存纪律」节）。
  - §7：生产者-消费者示例——生产者发完后 `c.close()`，消费者 `for v in c` 收集（「发送方负责关闭」约定的规范出处）。
- `docs/language/08-vm-internals.md`
  - §2.2：并发指令清单含 `MS_OP_CHAN_NEW`/`MS_OP_CHAN_SEND`/`MS_OP_CHAN_RECV`/`MS_OP_CHAN_TRY_RECV`；本任务不生成这些指令（理由同任务 43 对 `MS_OP_SPAWN` 的处理，见「VM 接线」节）。
  - §3：`MsTypeTag` 已含 `MS_TYPE_CHANNEL`。
  - §6.1：`struct MsCoroutine` 预留 `blockedOn`（channel/handle this coroutine is blocked on），本任务填充其语义。
  - §6.2：channel 的 `send`/`recv` 阻塞——协程挂入 channel 的等待队列（**双向：send 等待队列 + recv 等待队列**），配对时双方（或单方）转 READY。这是本任务核心算法的规范依据。
- `docs/language/04-exceptions.md` §4：`ChannelClosedError` 直属 `Exception`（**不是** `RuntimeError` 子类）；类型对象已由任务 23 预建（`MS_EXC_CHANNEL_CLOSED_ERROR`），本任务首次产生实际抛出点。
- `docs/language/03-syntax.md` §9：内建清单 `chan(capacity=0)`（带默认值的容量参数）、`cap(c)`（注释明示 cap 只对 channel 有意义）；§8 名字解析顺序使 `chan`/`cap` 可被脚本遮蔽。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）。
- `docs/language/11-project-layout.md` §1：`src/sched/` 为协程与调度设施目录；channel 与调度器强耦合（直接操作协程状态与就绪队列），故归入 `src/sched/` 而非 `src/object/`；`tests/ms/` 由 `run_tests.py` 驱动。
- [任务 43 协程与 async/await](43-coroutines.md)：`struct MsCoroutine` 与 `MsCoroState`（`MS_CORO_SUSPENDED`/`MS_CORO_READY`）、`msSchedEnqueue` 尾入队、`msSchedRun` 主循环与其第 2 步的 `CancelledError` 注入点、`MS_OP_AWAIT` 的「pc 不前进、唤醒重执行」挂起协议（本任务全部阻塞操作复用该协议）、死锁检测（就绪空且仍有存活协程报 `RuntimeError`）。
- [任务 23 异常系统](23-exceptions.md)：`msVmRaiseFmt(L, MS_EXC_CHANNEL_CLOSED_ERROR, ...)` 抛出入口；`MS_EXC_CHANNEL_CLOSED_ERROR` 枚举值已存在。
- [任务 26 for-in 迭代协议](26-iteration-protocol.md)：`MsIterKind`/`struct MsIterator`/`msIterGetIter`/`msIterNext` 与 VM `MS_OP_GET_ITER`/`MS_OP_ITER_NEXT` 分派——本任务对其做显式扩展（新增 `MS_ITER_CHANNEL`）。
- [任务 10 内建函数](10-builtin-functions.md)：`msBuiltinTable` 注册机制与 `msBuiltinCheckArgc`/`msBuiltinCheckType` 参数校验风格——本任务追加 `chan`/`cap` 两项并扩展 `len`。
- 任务 06/08/13/16/32 的文档在本任务写作时部分尚未定稿，其接口名（内建类型方法表机制、`struct MsCallFrame`、方法调用分派入口、`msNewTuple` 等）为本文假定命名，**实现时以对应任务文档定名为准**。

## 详细设计

### 范围界定

本任务在任务 43 的单线程协作式调度器上实现 channel 全部语义：**无锁、无抢占、无 select**。结构上为任务 45/46 预留：select 需要"一个协程同时挂在多个 channel 上"，届时把本任务的嵌入式单等待节点推广为独立堆分配的等待节点链表即可，channel 主体不变；任务 46 的多工作线程需为队列操作补锁与内存序，本任务把队列进出集中于少量 `static` 函数以便届时收口。

### 文件与模块划分

- 新增 `src/sched/ms_channel.h` / `src/sched/ms_channel.c`，include guard `MSLANG_SRC_SCHED_MS_CHANNEL_H_`：channel 对象、双向等待队列、配对唤醒算法、六个方法的实现、for-in 迭代支持。头文件 include `<stdbool.h>` `<stdint.h>` 与 `"sched/ms_coroutine.h"`（任务 43），自包含。
- 修改 `src/sched/ms_coroutine.h`（任务 43 的显式扩展点）：`struct MsCoroutine` 追加 channel 阻塞字段（见下），新增 `MsChanOpState` 枚举。
- 修改 `src/vm/`：`MS_OP_CALL_METHOD` 的 C 函数分派处接 channel 挂起分支；`MS_OP_ITER_NEXT` 分派在 `msIterNext` 之前接 channel 迭代器分支。
- 修改 `src/object/ms_iterator.{h,c}`（任务 26 的显式扩展点）：`MsIterKind` 追加 `MS_ITER_CHANNEL`，`msIterGetIter` 增加 `MS_TYPE_CHANNEL` 分支，`msIterNext` 增加对应同步分支。
- 修改 `src/vm/ms_builtin.c`（任务 10 的显式扩展点）：`msBuiltinTable` 追加 `chan`/`cap`，`len` 增加 channel 分支。
- 修改 `src/gc/ms_gc.c`：channel 的遍历槽（标记缓冲元素）与任务 43 协程遍历的补充（标记新增字段）。

### channel 对象与等待队列

```c
struct MsChannel {
  MsObjectHeader header;             // MS_TYPE_CHANNEL
  MsObject** buffer;                 // ring buffer; NULL when capacity == 0
  int64_t capacity;                  // 0 = unbuffered (rendezvous)
  int64_t head;                      // index of the oldest buffered element
  int64_t count;                     // buffered elements; exposed as c.len()
  bool closed;
  struct MsCoroutine* sendHead;      // FIFO queue of parked senders (intrusive)
  struct MsCoroutine* sendTail;
  struct MsCoroutine* recvHead;      // FIFO queue of parked receivers
  struct MsCoroutine* recvTail;
};
```

- 缓冲为定容环形数组，构造时一次性 `msAlloc`（`capacity == 0` 时不分配），随 channel 释放 `msFree`；弹出元素时清槽（写 NULL），不滞留引用。
- 不变量：**两条等待队列至多一条非空**。发送方只在无接收等待且缓冲满（或无缓冲）时挂起；接收方只在缓冲空且无发送等待时挂起。算法各分支维持该不变量（见下）。
- 不变量：**接收等待队列非空 ⟹ 缓冲为空**（有接收方等待时发送方直接交接，不入缓冲）。close 的唤醒正确性依赖此条。

### 协程扩展字段与挂起协议

任务 43 的 `struct MsCoroutine` 追加（任务 43 已在 `waiters` 注释中预留 `blockedOn` 的引入位；本节即 08-vm-internals §6.1 `blockedOn` 的落地）：

```c
typedef enum {
  MS_CHANOP_NONE,        // no completed channel op pending consumption
  MS_CHANOP_SEND_DONE,   // a parked send completed (value taken by the waker)
  MS_CHANOP_RECV_VALUE   // a parked recv completed; pendingValue holds the value
} MsChanOpState;

// fields appended to struct MsCoroutine:
//   MsObject* blockedOn;             // channel this coroutine is parked on; NULL when not parked
//   MsObject* pendingValue;          // send: value in flight; recv: delivered value (NULL = none)
//   MsChanOpState chanOp;            // completion marker consumed on re-execution
//   struct MsCoroutine* chanPrev;    // intrusive doubly-linked wait-queue links
//   struct MsCoroutine* chanNext;    // (doubly-linked to support removal on cancel)
```

挂起协议完全复用任务 43 的 `MS_OP_AWAIT` 模式：**挂起时求值栈原样保留、pc 不越过本指令；唤醒后重执行同一指令**，重执行先消费 `chanOp` 完成标记再走常规路径。值的实际交接由**唤醒方**完成（发送方从接收等待队列取出接收协程，把值写入其 `pendingValue` 并置 `MS_CHANOP_RECV_VALUE`；接收方/close 把发送协程的 `pendingValue` 取走并置 `MS_CHANOP_SEND_DONE`），因此被唤醒方重执行时是幂等的——这是「配对时双方（或单方）转 READY」（08-vm-internals §6.2）的具体化：配对交接中发起方不挂起、只唤醒对方（单方转 READY）；缓冲通道满/空两侧的配对则发送方与接收方先后各自完成。nil 可作为 channel 值传递：`pendingValue == NULL` 表示"无交接"，nil 是装箱单例对象，指针非 NULL，二者不混淆。

### send/recv 核心算法（配对唤醒）

公开入口（VM 方法分派专用，见「VM 接线」节）：

```c
typedef enum {
  MS_CHAN_DONE,      // completed; result available (recv: *out; send: implicitly nil)
  MS_CHAN_SUSPEND,   // co parked on the channel; pc NOT advanced; dispatch must yield
  MS_CHAN_RAISED     // exception raised in co; enter the task-23 unwind path
} MsChanStatus;

// c.send(v): rendezvous or buffer-store semantics per the algorithm below.
MsChanStatus msChanSend(MsState* L, struct MsCoroutine* co, struct MsChannel* ch, MsObject* v);

// c.recv(): on MS_CHAN_DONE, *out holds the received value.
MsChanStatus msChanRecv(MsState* L, struct MsCoroutine* co, struct MsChannel* ch, MsObject** out);
```

`msChanSend` 流程（单线程，无竞态）：

1. 重执行快路径：`co->chanOp == MS_CHANOP_SEND_DONE` → 清标记，返回 `MS_CHAN_DONE`。
2. `ch->closed` → `msVmRaiseFmt(L, MS_EXC_CHANNEL_CLOSED_ERROR, "send on closed channel")`，返回 `MS_CHAN_RAISED`。
3. 接收等待队列非空 → 队首出队一个接收方 r：`r->pendingValue = v`、`r->chanOp = MS_CHANOP_RECV_VALUE`、`r->blockedOn = NULL`、`msSchedEnqueue(L, r)`；返回 `MS_CHAN_DONE`（直接交接，不入缓冲，保持值序）。
4. `capacity > 0 && count < capacity` → 环形压入，返回 `MS_CHAN_DONE`。
5. 否则挂起：`co->pendingValue = v`、`co->chanOp = MS_CHANOP_NONE`、`co->blockedOn = ch`、链入 `sendTail`、置 `MS_CORO_SUSPENDED`，返回 `MS_CHAN_SUSPEND`。

`msChanRecv` 基于共享的内部步骤函数（for-in 迭代复用同一函数，仅结果映射不同）：

```c
typedef enum {
  MS_CHAN_STEP_VALUE,    // *out holds a received value
  MS_CHAN_STEP_CLOSED,   // channel closed and drained
  MS_CHAN_STEP_PARKED    // co parked on the recv queue
} MsChanStep;

static MsChanStep msChanRecvStep(MsState* L, struct MsCoroutine* co, struct MsChannel* ch, MsObject** out);
```

`msChanRecvStep` 流程：

1. 重执行快路径：`co->chanOp == MS_CHANOP_RECV_VALUE` → `*out = co->pendingValue`，清标记与 `pendingValue`，返回 `MS_CHAN_STEP_VALUE`。
2. 缓冲非空 → `*out` = 环形弹出（清槽）；随后若发送等待队列非空：队首发送方 s 出队，把 `s->pendingValue` 补入缓冲（保持值序：缓冲内元素均旧于任何阻塞发送方的值），`s->pendingValue = NULL`、`s->chanOp = MS_CHANOP_SEND_DONE`、`s->blockedOn = NULL`、唤醒 s；返回 `MS_CHAN_STEP_VALUE`。
3. 发送等待队列非空（必为无缓冲 rendezvous）→ 队首发送方 s 出队，`*out = s->pendingValue`，同上加 `SEND_DONE` 并唤醒；返回 `MS_CHAN_STEP_VALUE`（接收方不挂起，单方唤醒）。
4. `ch->closed` → 返回 `MS_CHAN_STEP_CLOSED`（此刻缓冲必空：第 2 步未命中）。
5. 否则挂起：`co->blockedOn = ch`、`co->chanOp = MS_CHANOP_NONE`、链入 `recvTail`、置 `MS_CORO_SUSPENDED`，返回 `MS_CHAN_STEP_PARKED`。

`msChanRecv` 的映射：`VALUE` → `MS_CHAN_DONE`；`PARKED` → `MS_CHAN_SUSPEND`；`CLOSED` → 抛 `ChannelClosedError`（"recv on closed channel"），`MS_CHAN_RAISED`。这正落实「关闭**且排空**后 recv 才抛」：关闭后缓冲内残值仍被正常取走。

### try 操作（非阻塞）

`trySend`/`tryRecv` 是普通 `MsCFunction`（不挂起，无重执行状态）：

- `c.trySend(v)`：closed → 抛 `ChannelClosedError`（规范只规定"缓冲满返回 false"，关闭情形规范未言明，本任务裁定为与 send 一致地抛出，见「约定与歧义裁定」）；否则按 `msChanSend` 第 3/4 步尝试交接或入缓冲，成功返回 `true`；不可立即完成返回 `false`，**不挂起**。
- `c.tryRecv()`：按 `msChanRecvStep` 第 2/3 步取值，取到返回 `(v, true)`；取不到（含关闭且排空）返回 `(nil, false)`，**不抛 `ChannelClosedError`**（非阻塞接口以 `ok` 表达空/关闭）。返回值为 2 元素 tuple（任务 32 已落地，定容二元组用语义更贴切的 tuple；`v, ok := c.tryRecv()` 经既有解包协议工作）。

### close 语义（唤醒 + 重执行）

`c.close()`：

1. 已关闭 → 抛 `ChannelClosedError`（"close of closed channel"；规范未言明，裁定为与 Go 的 panic 相对应的脚本异常，见「约定与歧义裁定」）。
2. 置 `closed = true`。
3. 唤醒**全部**阻塞发送方：逐个出队，`pendingValue = NULL`（丢弃未交接的值）、`chanOp = MS_CHANOP_NONE`、`blockedOn = NULL`、入就绪队列尾。其重执行 send 时在第 2 步撞上 `closed` → 在各自 send 调用点抛 `ChannelClosedError`。
4. 唤醒**全部**阻塞接收方：逐个出队、清 `blockedOn`、入就绪队列尾。由「接收等待 ⟹ 缓冲空」不变量，其重执行必然落入第 4 步：方法形式抛 `ChannelClosedError`，for-in 形式转为迭代耗尽（见下）。

close 自身不直接递送任何结果/异常，只负责唤醒——所有终态由重执行路径统一得出，与任务 43 的 AWAIT 协议同源，避免在唤醒处复制接收逻辑的特判。close 返回 nil。

### VM 接线：方法分派与挂起

`c.send(v)`/`c.recv()` 在脚本层是方法调用，编译产物是普通 `MS_OP_CALL_METHOD`（动态语言无法静态判定接收者是 channel，故 **`MS_OP_CHAN_*` 指令不由编译器生成**，与任务 43 对 `MS_OP_SPAWN` 的保留处理一致；这些指令留作将来可静态判定时的优化路径）。挂起能力经分派特判获得：

```c
typedef enum {
  MS_CHAN_METHOD_REGULAR,   // trySend/tryRecv/close/len: ordinary MsCFunction call
  MS_CHAN_METHOD_SEND,
  MS_CHAN_METHOD_RECV
} MsChanMethodKind;

// Classifies a method object resolved from the channel type's method table
// (compares the wrapped C function pointer against the two suspending
// natives). Used by the VM method-call dispatch.
MsChanMethodKind msChanClassifyMethod(MsObject* method);
```

VM 的 `MS_OP_CALL_METHOD` 在解析出方法对象后、进入常规 C 调用前：接收者类型标签为 `MS_TYPE_CHANNEL` 且 `msChanClassifyMethod` 为 SEND/RECV 时——先校验参数个数（send 恰 1 参、recv 恰 0 参，不符抛 `TypeError`），再调 `msChanSend`/`msChanRecv`：

- `MS_CHAN_DONE`：弹出接收者与参数，压入结果（send 压 nil、recv 压 `*out`），pc 前进，继续分派。
- `MS_CHAN_SUSPEND`：**求值栈与 pc 原样不动**，分派循环向 `msSchedRun` 返回"让出"（任务 43 既有机制）。
- `MS_CHAN_RAISED`：进入任务 23 的栈展开路径（traceback 指向 send/recv 调用点）。

`trySend`/`tryRecv`/`close`/`len` 四个方法不挂起，走任务 43 同款"内建类型方法表挂 `MsCFunction`"的常规路径。channel 类型对象（名 `"channel"`）随 `msChanInitType` 在解释器初始化时注册（同任务 43 注册 `coroutine` 类型的机制），六个方法一次性挂入其方法表。

### for-in 迭代（`for v in c`）

对任务 26 的显式扩展：

1. `MsIterKind` 追加 `MS_ITER_CHANNEL`；`msIterGetIter` 对 `MS_TYPE_CHANNEL` 经 `msIterNew(L, MS_ITER_CHANNEL, ch)` 产迭代器（`source` 持有 channel，GC 遍历复用任务 26 既有挂接）。迭代器一次性、逐值消费 channel——两个 for 循环同时迭代同一 channel 时互相竞争取值，语义自明。
2. VM 的 `MS_OP_ITER_NEXT` 在调用 `msIterNext` 之前检查迭代器 kind：`MS_ITER_CHANNEL` 时改调

```c
typedef enum {
  MS_CHAN_ITER_VALUE,      // *out holds the next value
  MS_CHAN_ITER_EXHAUSTED,  // closed and drained: clean loop exit
  MS_CHAN_ITER_PARKED      // co parked on the recv queue; re-executes on wake
} MsChanIterStatus;

MsChanIterStatus msChanIterNext(MsState* L, struct MsCoroutine* co, struct MsChannel* ch, MsObject** out);
```

   其内部即 `msChanRecvStep`：`VALUE` → 压值继续；`CLOSED` → `EXHAUSTED`，VM 弹出迭代器并按 sAx 跳转出循环——**这就是「`ChannelClosedError` 被循环内部消化」的落点**，迭代耗尽不物化异常对象（与任务 26 原生迭代器零异常开销的热路径约定一致）；`PARKED` → pc 不前进、让出，唤醒重执行（先消费 `MS_CHANOP_RECV_VALUE`，否则重估得值/耗尽）。
3. `msIterNext` 的 `MS_ITER_CHANNEL` 分支仅供 `next()` 内建（C 函数无法挂起）：立即可取值则取值；关闭且排空则 `hasValue = false`（由 `next` 物化 `StopIteration`）；**会阻塞则抛 `RuntimeError`**（"next() on channel iterator would block"）——裁定见下节。for 循环不经此路径。

### 内建函数 `chan`/`cap` 与 `len` 扩展

经任务 10 的 `msBuiltinTable` 追加（参数校验复用 `msBuiltinCheckArgc`/`msBuiltinCheckType` 风格）：

- `chan()` / `chan(capacity)`：0 参 → 无缓冲（capacity 0）；1 参须为 int（bool 拒绝，`TypeError`）；负值抛 `ValueError`；kwarg 形式 `chan(capacity=16)` 按任务 10 §4 假定的 kwarg 尾部约定解析（以任务 13 定稿为准）。返回新 channel 对象。
- `cap(c)`：channel → `capacity`（无缓冲为 0）；其余类型抛 `TypeError`（03-syntax §9 明示 cap 只对 channel 有意义）。
- `len(c)`：任务 10 的 `len` 分派追加 `MS_TYPE_CHANNEL` 分支 → `count`，与 `c.len()` 方法同值（03-syntax §9 将 `len(x)` 泛型列出而 06-concurrency §2 只给出 `c.len()` 方法，两口径并存属裁定，见下节）。

### 与 cancel 的协作

任务 43 的 `h.cancel()` 注入点为"目标协程下次被调度恢复时"；阻塞在 channel 上的协程若无人唤醒将永远等不到恢复点，取消会退化为死锁。故本任务扩展 `msCoroMethodCancel`（任务 43 的挂接点）：目标 `blockedOn != NULL` 时，先调

```c
// Removes co from the wait queue of the channel it is parked on (if any),
// clearing blockedOn/pendingValue/chanOp. No-op when co is not parked.
void msChanUnlink(struct MsCoroutine* co);
```

将其从 channel 等待队列摘除（等待队列取双向链表正为此处 O(1) 摘除），随后照常置 `cancelRequested` 并入就绪队列——协程下次恢复时由 `msSchedRun` 第 2 步在其 channel 操作调用点注入 `CancelledError`，协程内 `try/except/finally` 照常生效；未捕获则经 `msCoroFinish(isError = true)` 传播给等待方。

### GC 与内存纪律

- channel 遍历槽（任务 17 的逐类型遍历挂接，假定名）：按环形序标记 `buffer[head .. head+count)`；等待队列中的协程不由此标记（它们经调度器 `registry` 是根）。弹出即清槽，不留过期引用。
- 任务 43 的协程遍历**必须补标** `blockedOn` 与 `pendingValue`：否则阻塞中的协程所持的 channel 与在途值会被误回收。
- 全部堆分配经 `msAlloc/msRealloc/msFree`；channel 对象与环形数组的所有权归 GC，无额外所有者。
- 内存模型：单线程下 `send` 写入 happens-before `recv` 返回由"交接先于唤醒"的程序序平凡成立；任务 46 引入工作线程时，等待队列与环形缓冲的读写集中于本模块的 `static` 入队/出队/交接函数，届时在统一处补锁与内存序。

### 约定与歧义裁定

规范未言明、本任务裁定如下（各处在上文已就地标注，此处汇总）：

- **发送方负责关闭**：channel 不记录属主；约定由发送方（生产者）发完后 `close()` 作为"不再有新值"的信号（06-concurrency §7 的范例模式），接收方永不 close；`for v in c` 消费者的终止依赖该约定。违反约定（关闭后 send）由 `ChannelClosedError` 强制暴露，不是静默错误。
- 重复 `close` 抛 `ChannelClosedError`（对应 Go 的 panic；保持"对已关闭 channel 的主动操作均报错"的一致性）。
- `trySend` 作用于已关闭 channel 抛 `ChannelClosedError`（send 家族语义一致；`false` 仅表达"此刻无法成交"）。
- `tryRecv` 返回 2 元素 **tuple** `(v, ok)`；关闭且排空返回 `(nil, false)` 而不抛异常。
- `len(c)` 内建接受 channel（与 `c.len()` 同值）；`cap` 仅接受 channel。
- `next()` 内建作用于 channel 迭代器且会阻塞时抛 `RuntimeError`（C 函数无法挂起；for-in 不受影响，走 VM 分派的挂起路径）。
- 唤醒顺序：等待队列 FIFO（channel 的公平性直觉；规范未承诺，此处为确定性与可测试性选型，与任务 43"waiters 不保证顺序"的协程句柄语义互不冲突）。

## 实现步骤

1. 建 `src/sched/ms_channel.h` / `ms_channel.c` 骨架：`struct MsChannel`、`msChanNew`（环形数组分配）、channel 类型对象注册（名 `"channel"`，空方法表）、GC 遍历槽。验证：既有全部测试无回归。
2. 内建 `chan`/`cap` 注册与 `len` 扩展，参数校验（负容量 `ValueError`、非 int `TypeError`、`cap(42)` `TypeError`）。验证：脚本断言 `type(c) == channel`、`cap`/`len`/`c.len()` 初值与各错误路径。
3. 四个不挂起方法与等待队列骨架：`trySend`/`tryRecv`/`close`/`len` 以 `MsCFunction` 挂方法表；双向等待队列的入队/出队/`msChanUnlink`；交接与缓冲补位逻辑（此时尚无挂起方可达，仅用单协程脚本覆盖 try/close 分支）。验证：单协程下 try 系列全部分支、缓冲 FIFO、close 后 try 行为。
4. 协程扩展字段与挂起路径：`MsChanOpState` 与五个新字段、`msChanSend`/`msChanRecv`/`msChanRecvStep` 全流程、`msChanClassifyMethod`、VM `MS_OP_CALL_METHOD` 特判（DONE/SUSPEND/RAISED 三态）。验证：rendezvous 交接轨迹、缓冲满挂起-取走唤醒、唤醒重执行幂等（值不重复交接）。
5. close 的唤醒全量路径：关闭撞醒阻塞发送方/接收方并在各自恢复点得出 `ChannelClosedError`/迭代耗尽。验证：关闭时各类阻塞方的恢复行为、缓冲残值排空后再抛。
6. cancel 协作：`msCoroMethodCancel` 接 `msChanUnlink`。验证：取消阻塞中的发送方/接收方，`CancelledError` 在其操作点注入且 channel 后续收发正常。
7. for-in 迭代：`MS_ITER_CHANNEL` 扩展（任务 26 枚举与 `msIterGetIter`）、VM `MS_OP_ITER_NEXT` 分支、`msChanIterNext`、`msIterNext` 的同步分支（`next()` 会阻塞抛 `RuntimeError`）。验证：`for v in c` 到关闭、break 后 channel 可用、`next()` 三分支。
8. GC 补标（channel 缓冲、协程 `blockedOn`/`pendingValue`）与全量复查：任务 43 及之前全部脚本回归；Debug（ASAN）跑全部本任务测试；任务 02 分配统计在 `msCloseState` 后归零（含挂起中被取消/被关闭唤醒的协程路径）。

## 测试方案

本任务晚于任务 40（testing 模块），ms 脚本一律使用 `testing` 模块，由仓库根 `run_tests.py` 驱动 mslang CLI 执行。无法以脚本内断言捕获的致命情形（死锁）沿用 `<name>.exit` 同伴文件约定声明预期退出码 1。测试目录 `tests/ms/concurrency/`（与任务 43 同目录；本任务只交付本设计文档，脚本随实现编写）：

- `chan_create_test.ms`：`chan()` 的 `cap == 0`、`chan(16)` 的 `cap == 16`；`type(c) == channel`；`c.len()` 与 `len(c)` 初始为 0 且同值；channel 作一等值传递（函数参数/返回值/字典值）；`chan(-1)` 抛 `ValueError`、`chan("x")` 抛 `TypeError`、`chan(true)` 抛 `TypeError`、`cap(42)` 抛 `TypeError`。
- `chan_unbuffered_test.ms`：rendezvous 语义——async 发送方在 `send` 前后打点，main 延时 `recv`，断言发送方在接收就绪前未完成（执行轨迹序）；值原样传递（含 nil、list 等对象，`is` 同一性）；多轮收发顺序。
- `chan_buffered_test.ms`：容量内 `send` 不挂起立即完成（无其他协程参与）；缓冲满后 `send` 让出（轨迹断言）；FIFO 顺序；取走一个元素唤醒一个阻塞发送方且其值补入队尾、总值序保持；`len` 随收发变化。
- `chan_try_test.ms`：`tryRecv` 空缓冲 → `(nil, false)`；`trySend` 满缓冲/无接收方无缓冲 → `false`；有阻塞接收方时 `trySend` 立即成交返回 `true` 且对方被唤醒取值；有阻塞发送方时 `tryRecv` 取到其值并唤醒对方；关闭后 `trySend` 抛 `ChannelClosedError`、关闭且排空后 `tryRecv` 返回 `(nil, false)`；返回值可经 `v, ok := c.tryRecv()` 解包。
- `chan_close_test.ms`：关闭后 recv 先排空缓冲残值再抛 `ChannelClosedError`（类型经 `except ChannelClosedError` 断言，且其不是 `RuntimeError` 子类——`except RuntimeError` 不命中）；关闭后 `send` 抛；重复 `close` 抛；关闭撞醒阻塞发送方并在其 send 点抛 `ChannelClosedError`；关闭撞醒阻塞接收方并在其 recv 点抛；`close()` 返回 nil。
- `chan_for_test.ms`：`for v in c` 迭代到关闭且排空（收集序列断言）；迭代中 `break` 后 channel 仍可正常收发；已关闭的空 channel 零次迭代；循环体内 `raise StopIteration` 不被误消化（承任务 26 不变量）；`next()` 作用于 channel 迭代器：有值取值、关闭且排空抛 `StopIteration`、会阻塞抛 `RuntimeError`。
- `chan_producer_consumer_test.ms`：06-concurrency §7 完整示例（`chan(8)` 的 jobs、`chan(1)` 的 results、`for v in c` 求和、生产者发完 `close`），断言 `results.recv()` 为 328350。
- `chan_cancel_test.ms`：取消阻塞在 `send` 上的协程——其恢复点收到 `CancelledError`、可被协程内 `try/except` 捕获、`finally` 执行；取消阻塞在 `recv` 上的协程同理；取消后该 channel 由其他协程继续收发正常（等待队列清洁）；未捕获时 `await` 句柄重抛 `CancelledError`。
- `chan_deadlock_test.ms`（负向用例，配 `chan_deadlock_test.exit` 声明退出码 1）：main 对空 channel `recv` 且无其他协程 → 任务 43 死锁检测报 `RuntimeError`；两协程互等对方 channel 发送的变体（可拆第二脚本配各自 `.exit`）。
- 回归：任务 09–43 的全部既有脚本测试不变通过（重点：任务 43 的协程测试与本任务改动后的 GC 路径）。

## 验收标准

- [ ] `src/sched/ms_channel.h` / `ms_channel.c` 存在，guard 为 `MSLANG_SRC_SCHED_MS_CHANNEL_H_`，头文件自包含；代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsChannel` 不 typedef、堆分配只经 `msAlloc/msRealloc/msFree`）。
- [ ] `chan()`/`chan(n)` 构造无缓冲 rendezvous 与缓冲 channel；`chan` 参数校验（非 int `TypeError`、负值 `ValueError`）；`cap(c)` 返回容量（无缓冲为 0）且拒绝非 channel；`len(c)` 与 `c.len()` 返回当前缓冲元素数且同值。
- [ ] rendezvous 与缓冲通道的阻塞/配对唤醒语义与本文算法一致：双向等待队列、直接交接优先于入缓冲、缓冲满/空时挂起、唤醒后重执行幂等（值不重复交接、不丢失）、值序 FIFO。
- [ ] 关闭语义完整：关闭后 `send`/`trySend` 抛 `ChannelClosedError`；recv 排空残值后才抛；重复 close 抛；close 唤醒全部阻塞方且各方在自身调用点得出结果；`tryRecv` 关闭且排空返回 `(nil, false)`。
- [ ] `ChannelClosedError` 经任务 23 抛出路径产生（`MS_EXC_CHANNEL_CLOSED_ERROR`），traceback 指向 send/recv 调用点；其层级为 `Exception` 直属。
- [ ] `for v in c` 迭代到关闭且排空，耗尽不物化异常对象；迭代器可挂起当前协程；`next()` 的 channel 迭代器三分支（值/StopIteration/会阻塞 RuntimeError）正确。
- [ ] `h.cancel()` 可取消阻塞在 channel 上的协程：`CancelledError` 在其操作点注入、等待队列摘除干净、channel 后续可用；channel 全阻塞且就绪空时由任务 43 死锁检测报 `RuntimeError`。
- [ ] GC 覆盖 channel 缓冲元素与协程 `blockedOn`/`pendingValue`；长链收发+GC 后对象完好；挂起路径（含被取消、被 close 唤醒）无泄漏，`msCloseState` 后分配统计归零。
- [ ] `MS_OP_CHAN_*` 指令未被编译器生成（保留）；channel 方法经 `MS_OP_CALL_METHOD` 分派特判挂起，channel 可被子句遮蔽/作为一等值传递。
- [ ] 「测试方案」全部 ms 脚本通过（含 `.exit` 负向用例）；任务 09–43 既有测试回归通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 06/08/13/16/26/32 的接口假定（内建类型方法表、方法调用分派入口、`MsIterKind` 扩展点、`msNewTuple` 等）在实现时已对齐；「约定与歧义裁定」一节的选择已在文中各处置明。
