# 45 select 多路复用

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.3 | ⬜ | [44 channel](44-channel.md) |

## 任务目标

交付 `select` 语句的编译与运行支持（`docs/language/06-concurrency.md` §3），补全 v0.3 单线程协作式并发模型（任务 43/44）的最后一块语言级语义：

- 编译器（`src/compiler/`）：把任务 04 已产出的 `MS_AST_SELECT` / `MS_AST_SELECT_CASE` 节点编译为 `MS_OP_SELECT_BEGIN` / `MS_OP_SELECT_ADD_RECV` / `MS_OP_SELECT_ADD_SEND` / `MS_OP_SELECT_EXEC` 四条并发指令（08-vm-internals §2.2 已定名），含 case 体地址回填与 recv 绑定的存取代码。
- 运行时（新增 `src/sched/ms_select.h` / `src/sched/ms_select.c`）：select 帧（`struct MsSelectState`）、就绪轮询（伪随机序）、`default` 分支、无 `default` 且全部阻塞时把当前协程同时挂入多个 channel 等待队列、任一 case 就绪时的赢家认领与唤醒、空 `select {}` 永久阻塞。
- 与任务 44 channel 的协作契约：channel 的配对/关闭路径能识别并认领 select 等待者；select 的轮询/挂起复用 channel 的非阻塞收发原语与等待队列。
- GC 协作：select 帧内的 send 值与已交付 recv 值在挂起期间是 GC 根。

完成后，`tests/ms/concurrency/` 下的脚本可以使用 `select`/`case`/`default` 在多个 channel 间多路复用。M:N 化后的并发正确性由任务 46 复核（本任务的认领协议按多线程安全设计，v0.3 单线程下天然成立）。

## 设计依据

- `docs/language/06-concurrency.md`
  - §3：`selectStmt`/`selectCase`/`recvStmt`/`sendStmt` 的 EBNF；多 case 同时就绪时**伪随机**选一个（不保证顺序公平，避免饥饿由实现保证）；无 `default` 且全部阻塞时协程挂起直到任一 case 就绪；`select {}`（空 select）永久挂起当前协程；等待协程句柄用 `await`，不进入 `select` 的 case（首版决策）。
  - §4：`select` 阻塞是协程让出点。
  - §2：channel 的 rendezvous/缓冲语义与 `ChannelClosedError`（recv 已关闭且排空的 channel 抛出、send 已关闭的抛出）是 select 各 case 的就绪/失败判定底座。
  - §6：协程的栈与句柄是 GC 根——select 帧随协程枚举。
- `docs/language/08-vm-internals.md`
  - §2.2：并发指令 `MS_OP_SELECT_BEGIN` / `MS_OP_SELECT_ADD_RECV` / `MS_OP_SELECT_ADD_SEND` / `MS_OP_SELECT_EXEC`（本任务直接采用这四个已定名指令）；指令定长 4 字节 `opcode(8) | A(8) | Bx(16)`。
  - §6.1：`MsCoroutine.blockedOn` 记录协程阻塞所在（本任务在 select 挂起时指向 select 帧）。
  - §6.2：channel 的 send/recv 等待队列与配对唤醒——select 挂起复用同一队列结构。
- `docs/language/03-syntax.md` §3：`selectStmt` 属 statement；其产生式以 06-concurrency §3 为准（任务 04 已落实解析）。
- 任务 04 语法分析器与 AST 提供：`MS_AST_SELECT`（case 为 `MS_AST_SELECT_CASE` 列表）节点，case 区分 recv（可带 `identList` `:=`/`=` 绑定）/ send（带实参）/ default；select 形状（default 至多一个、case 仅允许 recv/send 语句）已在 parser 校验，编译器不再重复检查。
- 任务 43 协程与调度器提供：`struct MsCoroutine`（`state`/`waiters`/`nextWaiter`/`blockedOn` 预留位）、`struct MsScheduler`（就绪队列、`current`、死锁检测）、`msSchedEnqueue`、让出-恢复机制（挂起时 pc 不前进、唤醒后重执行同一条指令）。本任务的 select 挂起/恢复沿用该模式。任务 43 的取消语义（`cancelRequested` 在下次被调度恢复时注入 `CancelledError`）同样适用于 select 阻塞中的协程。
- 任务 44 channel 提供：`struct MsChannel`、send/recv 双向等待队列、配对唤醒、关闭传播、非阻塞收发内部原语。任务 44 文档尚不存在，其结构体与函数名（`struct MsChanWaiter`、`msChannelTryRecv`、`msChannelTrySend`、等待队列出入队函数等）为本文假定命名，实现时以对应任务文档定名为准；本文与其的接口契约以「与任务 44 的协作契约」一节的语义描述为准。
- 任务 42 平台抽象层提供：`MsAtomicI32` / `msAtomicLoadAcquire` / `msAtomicStoreRelease` / `msAtomicCompareExchange`（赢家认领原子字，为任务 46 多线程化预留）、`msClockMonotonicNs`（伪随机种子）。
- 任务 07 编译器提供：语句/表达式编译框架、跳转回填辅助、赋值声明代码生成（任务 11 的 `:=`/`=`/多目标解包语义）。任务 07 文档尚不存在，其内部函数名（回填、表达式丢弃等）为假定命名，实现时以对应任务文档定名为准。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则（内部结构体不 typedef）、§5 错误处理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）。
- `docs/language/11-project-layout.md` §1：`src/sched/` 目录位置；`tests/ms/` 由仓库根 `run_tests.py` 驱动。

## 详细设计

### 范围界定

本任务仍处单线程协作式调度（任务 43/44 同）：任何时刻只有一个协程运行，"轮询—挂起"之间不会被抢占，等待队列无并发写。但赢家认领协议（原子 CAS + 撤销其余登记）按任务 46 的多线程形态设计，届时无需重写 select 逻辑。明确不做：`select` case 中的协程句柄等待（规范首版决策：句柄用 `await`）、超时 case（用 `default` + `time.sleep` 轮询或后续标准库设施表达）、netpoller 集成（任务 46/62）。

### 对规范空白的补足（语义决策）

规范（06-concurrency §3）只给出语法与三条语义要点，下列细节为本任务的明确决策，实现与测试以本节为准：

1. **求值次序（对齐 Go）**：进入 `select` 时，各 case 的 channel 表达式与 send 语句的值表达式按源码顺序**恰好各求值一次**（在检查就绪性之前）；case 体的求值只发生在被选中之后。因此 `case c.send(f()):` 中 `f()` 即使该 case 最终未选中也已执行——这是 Go 语义，文档与测试显式承认。
2. **伪随机选择**：轮询顺序为 case 索引的均匀随机排列（Fisher–Yates，调度器内 xorshift PRNG，种子取自 `msClockMonotonicNs` 异或 `MsState` 地址）；首个就绪 case 即被选中。每次 `select` 执行独立洗牌——同一批就绪 case 在循环中反复 select 时各 case 被选概率均等，"避免饥饿"由此满足；不提供顺序公平承诺。
3. **default 语义**：存在 `default` 时 select 绝不挂起——轮询（第 2 点的随机序）无一就绪则执行 `default`。有 case 就绪时 `default` 永不执行。
4. **空 `select {}`**：零 case 且无 `default`，协程永久挂起（不注册任何等待节点）。若此后就绪队列耗尽且仍有存活协程，任务 43 的死锁检测在主协程抛 `RuntimeError`——单协程程序中的空 select 因此以死锁错误终止，多协程程序中则安静挂起。
5. **已关闭 channel**：recv case 的 channel 已关闭且排空时视为**就绪**，选中该 case 在执行处抛 `ChannelClosedError`（与直接 `recv()` 语义一致，不静默跳过、不返回零值）；send case 的 channel 已关闭时同样视为就绪，选中即抛 `ChannelClosedError`。挂起中的 select 若其等待的 channel 被关闭，以"关闭结果"唤醒，恢复后按同一规则抛出。若随机序上先于已关闭 case 命中了正常就绪 case，则正常 case 胜出（多 case 就绪伪随机的自然推论）。
6. **recv 绑定**：`case v := c.recv():` / `case v = c.recv():` 的绑定复用任务 11 的声明/赋值代码生成；`identList` 多目标（如 `case a, b := c.recv():`）按多重赋值/解包规则处理单值（recv 只产单值，多目标将走解包语义并在非可迭代时抛 `TypeError`），v0.3 不为其提供 Go 式 `v, ok` 特例。无绑定的 `case c.recv():` 丢弃接收值。
7. **类型检查**：case 的 channel 表达式求值结果不是 `MS_TYPE_CHANNEL` 时在 select 语句处抛 `TypeError`（无论该 case 最终是否会被选中，因为求值发生在就绪判定之前）。
8. **与 `cancel()` 的交互**：沿用任务 43 语义——取消不强制唤醒阻塞中的协程，`CancelledError` 在协程下次被调度恢复时注入。若 select 已被某 case 就绪唤醒而协程尚未运行，恢复时先检查取消标志：取消胜出则注入 `CancelledError`，已完成的 channel 传递不回滚（与任务 43 对 `await` 的处理一致）。
9. **挂起时的 safepoint**：select 挂起是让出点（06-concurrency §4），挂起前经任务 47 的 `msSafePointCheck`；本任务按任务 43 的既定让出路径放置调用点，多线程停车协议本身属任务 47。

### 文件与模块划分

- 新增 `src/sched/ms_select.h` / `src/sched/ms_select.c`，include guard `MSLANG_SRC_SCHED_MS_SELECT_H_`：select 帧、四条指令的运行时实现（轮询/挂起/认领/唤醒）、伪随机。自包含（`<stdbool.h>` `<stdint.h>` 及任务 02/42/43 的头文件）；channel 结构经任务 44 头文件前向声明。
- 修改 `src/sched/ms_coroutine.h`：`struct MsCoroutine` 增加 `struct MsSelectState* selectState`（懒分配、协程销毁时释放）字段。
- 修改 `src/sched/ms_sched.h` / `ms_sched.c`：`struct MsScheduler` 增加 `uint64_t selectRngState`（xorshift 状态，永不为 0），`msSchedInit` 播种（`msClockMonotonicNs() ^ (uintptr_t)L`，结果为 0 时取固定非零常数）。
- 修改 `src/compiler/`（任务 07 模块）：`MS_AST_SELECT` 的代码生成。
- 修改 `src/vm/`（任务 08 模块）：分派循环接入四条 `MS_OP_SELECT_*` 指令；`MS_OP_SELECT_EXEC` 的挂起路径复用任务 43 的让出返回约定。
- 修改 `src/gc/ms_gc.c`：根枚举在任务 43 的协程遍历基础上补标记 select 帧内容。
- 修改任务 44 的 channel 模块（本任务范围内的增量）：等待队列节点扩展 select 回指；配对/关闭路径认领 select 等待者。接口契约见下节，落地代码属任务 44 文件的追加而非重写。

### 与任务 44 的协作契约

假定任务 44 的等待队列为侵入式单向链表（与任务 43 的 `waiters` 同体例），节点为 `struct MsChanWaiter`。本任务对其的最小扩展（假定命名，以任务 44 定名为准）：

```c
struct MsChanWaiter {
  struct MsCoroutine* co;              // parked coroutine
  struct MsChanWaiter* next;           // intrusive queue link
  MsObject* value;                     // parked sender's value (send queue only)
  struct MsSelectState* select;        // NULL for plain send/recv; set for select waiters
  int selectCaseIndex;                 // case slot within select->cases
};
```

契约规则：

- 任一 channel 操作（send/recv/close）使某等待队列头部可配对时，若队首节点 `select == NULL` 走任务 44 原路径；否则**不得直接唤醒**，改为调用 `msSelectTryClaim(L, node->select, node->selectCaseIndex, value, outcome)`：认领成功则由 select 侧负责摘除该 select 在**所有** channel 上登记的节点并唤醒协程；认领失败（该 select 已被别的 case 抢先）则 channel 侧把本节点当作已失效摘除，继续看队列下一节点。
- channel 关闭时遍历两条等待队列，对 select 节点以"关闭结果"（`MS_SELECT_OUTCOME_CLOSED`）认领；认领成功的 select 恢复后按「语义决策 5」抛出 `ChannelClosedError`。
- select 挂起时，recv case 节点入对应 channel 的 **recv 等待队列**、send case 节点入 **send 等待队列**（`value` 字段携带待发送值），多个 case 允许指向同一 channel（各占一个节点，独立参与认领）。
- 上述认领函数的返回值只表达"本次认领是否赢家"；被抢跑的节点何时摘除由认领方统一处理（见「挂起与唤醒」）。

### 数据结构

```c
typedef enum {
  MS_SELECT_CASE_RECV,
  MS_SELECT_CASE_SEND
} MsSelectCaseKind;

typedef enum {
  MS_SELECT_OUTCOME_VALUE,    // recv case: a value was delivered
  MS_SELECT_OUTCOME_SENT,     // send case: the value was taken by a receiver
  MS_SELECT_OUTCOME_CLOSED    // the channel was closed (raise ChannelClosedError on resume)
} MsSelectOutcome;

struct MsSelectCase {
  struct MsChannel* channel;
  MsSelectCaseKind kind;
  MsObject* sendValue;         // SEND only; owned by the frame while parked
  uint32_t bodyPc;             // absolute pc of the case body in the current proto
  struct MsChanWaiter waiter;  // channel queue node, valid only while parked
};

struct MsSelectState {
  struct MsSelectCase* cases;  // growable array, msRealloc'd
  int caseCount;
  int caseCapacity;
  bool hasDefault;
  uint32_t defaultPc;          // valid only when hasDefault
  MsAtomicI32 winner;          // -1 = undecided; otherwise the claimed case index
  MsObject* deliveredValue;    // recv transfer at claim time (VALUE outcome)
  MsSelectOutcome outcome;     // valid once winner >= 0
  bool parked;                 // true between park and wake
};
```

- `struct MsSelectState` 每协程至多一个活跃实例（`co->selectState`，首次执行 `MS_OP_SELECT_BEGIN` 时经 `msAlloc` 懒分配，`msSelectStateDestroy` 随 `msCoroDestroy` 释放）；`cases` 数组按需 `msRealloc` 扩容并跨 select 语句复用，避免每次 select 都分配。同一协程顺序执行多个嵌套/循环 select 时，`BEGIN` 复用同一帧（上一帧已随上一次 `EXEC` 完成而失效）。
- `winner` 用 `MsAtomicI32`（任务 42）实现"最多一个赢家"：认领是 `expected = -1` 的 CAS；v0.3 单线程下恒成功一次，任务 46 多线程下由 CAS 裁决唯一赢家。`outcome`/`deliveredValue` 在 CAS 成功**之前**写入，经 CAS 的 acq_rel 语义与恢复路径的 acquire 读配对（06-concurrency §5 的 happens-before 在 C 侧的落点）。
- `winner == -1` 不是合法 case 索引，天然作"未定"哨兵。

### 编译方案

编译器对 `MS_AST_SELECT` 生成如下布局（`case k` 指第 k 个 case，pc 为绝对地址、编译期回填）：

```
  MS_OP_SELECT_BEGIN      Bx = caseCount(不含 default)
  ; ── 每个 case 按源码顺序 ──
  <channel 表达式>                          ; 求值一次，栈顶为 channel
  [<send 值表达式>]                         ; 仅 send case
  MS_OP_SELECT_ADD_RECV   Bx = bodyPc_k     ; 弹 channel，登记 case k
  ; 或 MS_OP_SELECT_ADD_SEND Bx = bodyPc_k  ; 弹 value、channel，登记 case k
  ; ── 全部 case 登记完毕 ──
  MS_OP_SELECT_EXEC       A = hasDefault, Bx = defaultPc
  ; EXEC 不返回顺序下一条：选中后直接跳到某 case 的 bodyPc
bodyPc_0:
  [recv case 有绑定时: 任务 11 的存取代码; 无绑定时: 丢弃栈顶接收值]
  <case 0 体>
  MS_OP_JUMP endPc
  ...
defaultPc:
  <default 体>
  MS_OP_JUMP endPc
endPc:
```

要点：

- `bodyPc` 为 `Bx` 寻址（16 位绝对 pc），与 `MS_OP_SETUP_TRY` 等 Bx 控制指令同限：单 proto 指令数 ≤ 65535，超出由编译器报"函数过大"（复用任务 07 的既有诊断，不新增错误码）。
- `ADD_RECV`/`ADD_SEND` 在运行时只登记、不就绪判定：求值副作用（含 send 值表达式）在此全部完成，落实「语义决策 1」；channel 操作数非 `MS_TYPE_CHANNEL` 时在 ADD 处抛 `TypeError`（决策 7），行号归属 select 语句首行。
- `EXEC` 的恢复路径对 recv case 把交付值压栈（见下），因此 recv case 体的开头统一面对"栈顶 = 接收值"的布局：有绑定则生成任务 11 的声明/赋值存取码（`v :=` 新局部槽位、`v =` 既有目标、多目标解包），无绑定则生成任务 07 的表达式丢弃码（假定 `MS_OP_POP`，定名以任务 07 为准）。send case 与 default 的体入口栈布局与 `EXEC` 前一致（无额外压栈）。
- case 体与 default 体是普通 block，`break`/`continue`/`return` 等语境计数器规则不变（`select` 不是循环，`break` 穿透由 parser 的 `loopDepth` 既有机制保证）。
- 窥孔/死跳转消除（08-vm-internals §1）不跨 `EXEC` 与其 case 体合并——`EXEC` 是多路跳转源，回填点列表化管理。

### 执行算法（MS_OP_SELECT_EXEC）

```c
typedef enum {
  MS_SELECT_EXEC_JUMP,     // *outJumpPc set; recv winner also pushed deliveredValue
  MS_SELECT_EXEC_SUSPEND,  // coroutine parked; dispatch loop yields (task 43 contract)
  MS_SELECT_EXEC_ERROR     // exception raised in the current coroutine (task 23 path)
} MsSelectExecResult;

// Runs one MS_OP_SELECT_EXEC for co. defaultPc is valid iff the frame's
// hasDefault is set. On JUMP, *outJumpPc receives the body pc to dispatch to.
MsSelectExecResult msSelectExec(MsState* L, struct MsCoroutine* co, uint32_t* outJumpPc);
```

三阶段：

1. **恢复判定**：`winner >= 0` 说明本次是唤醒后重执行（pc 未越过 `EXEC`，任务 43 约定）。取 `cases[winner]`：按 `outcome` 处理——`VALUE`：压 `deliveredValue` 入求值栈、跳 `bodyPc`；`SENT`：直接跳 `bodyPc`（值已在认领时被接收方取走）；`CLOSED`：在当前协程抛 `ChannelClosedError`（任务 23 路径）。随后复位帧（`winner = -1`、`parked = false`、清空交付槽），返回 `JUMP`/`ERROR`。
2. **轮询**（首次执行）：`winner == -1` 时，对 `cases[0..caseCount)` 的索引做 Fisher–Yates 洗牌（调度器 `selectRngState` 的 xorshift64\*），按随机序逐 case 尝试任务 44 的非阻塞原语（假定 `msChannelTryRecv` / `msChannelTrySend`）：
   - recv 成功取到值 / send 成功交付：记为该 case 的就绪结果（不置 `winner`，直接转阶段 1 的同等处理：压值/跳体）；channel 已关闭且排空（recv）或已关闭（send）按「语义决策 5」视为就绪，抛 `ChannelClosedError`。
   - 全部不就绪：`hasDefault` → 跳 `defaultPc`；否则转阶段 3。
   - `caseCount == 0 && !hasDefault`：跳过轮询直接转阶段 3，挂起时登记零个节点（决策 4）。
3. **挂起**：置 `parked = true`，逐 case 把 `waiter` 节点（`select` 回指 + `selectCaseIndex`）入对应 channel 的对应等待队列；协程置 `MS_CORO_SUSPENDED`、`blockedOn` 指向本帧；**pc 不越过 `EXEC`**，分派循环按任务 43 约定向 `msSchedRun` 返回让出。单线程下入队过程无人能插入认领；任务 46 的持锁挂起路径需在挂起完成后复查一次 `winner`（入队期间被认领则立即摘节点转为恢复路径），该复查点本任务以注释与断言标出、由任务 46 激活。

认领与唤醒（channel 侧回调，select 模块实现）：

```c
// Claims the select on behalf of one of its parked cases. Exactly one
// caller wins (CAS on winner). On success: stores outcome/value, unlinks
// every registered waiter node from its channel queue, marks the coroutine
// READY and enqueues it (msSchedEnqueue). Returns false when the select was
// already claimed; the caller then treats its own node as stale.
bool msSelectTryClaim(MsState* L, struct MsSelectState* select, int caseIndex,
    MsObject* value, MsSelectOutcome outcome);
```

- 撤销其余登记：赢家遍历 `cases[0..caseCount)`，把除获胜节点外的 `waiter` 从各自 channel 等待队列摘除（单向链表摘除 O(队长)，队长受程序行为限制，可接受；多 channel 场景节点总数 = case 数）。**认领方负责摘除包括本节点在内的全部节点**——channel 侧在 `msSelectTryClaim` 返回成功后不再触碰该 select 的任何节点。
- 唤醒顺序：摘除全部节点后置 `SUSPENDED → READY` 入就绪队列尾（任务 43 `msSchedEnqueue`）；协程恢复后重执行 `EXEC` 走阶段 1。
- 同一 channel 被同帧多个 case 登记（如 `case c.recv():` 与 `case c.send(x):` 并存）时两个节点独立入队，任一被配对都能认领，无特例。

### 伪随机

```c
// xorshift64* step over the scheduler's selectRngState (never zero).
static uint64_t msSelectRandomNext(struct MsScheduler* sched);

// In-place Fisher-Yates shuffle of order[0..n).
static void msSelectShuffle(struct MsScheduler* sched, int* order, int n);
```

洗牌用的索引暂存数组随 `struct MsSelectState` 的扩容一并维护（与 `cases` 同容量），不在热路径上分配。PRNG 不追求密码学强度，只保证均匀性与低开销；不提供脚本可见的种子接口（与 06-concurrency §3"伪随机"措辞一致），测试对分布只做宽松统计断言。

### GC 协作

`src/gc/ms_gc.c` 的协程根枚举（任务 43 落地）补充：协程的 `selectState` 非空时，标记 `deliveredValue` 与全部 `cases[i]`（`channel`、`sendValue`）。挂起期间 send 值仅被 select 帧持有（求值栈已在 `ADD_SEND` 弹出），必须经此路径存活；`blockedOn` 指针本身只作诊断用途、不构成额外根（帧经 `co->selectState` 可达）。channel 等待队列不作为遍历入口（与任务 47 的根集约定一致）。

### 错误处理

- `TypeError`：ADD 时 channel 操作数类型错误（决策 7）；多目标绑定对单值解包失败（决策 6，任务 11 既有路径）。
- `ChannelClosedError`：轮询或恢复命中已关闭 channel（决策 5）；挂起期间 channel 关闭经 `MS_SELECT_OUTCOME_CLOSED` 唤醒后抛出。
- OOM：`selectState` 懒分配 / `cases` 扩容失败经 `msAlloc`/`msRealloc` 的统一失败路径返回 `MS_ERROR_OOM`，协程未置 `SUSPENDED`、无半登记状态（扩容先于任何队列操作）。

## 实现步骤

1. 建 `src/sched/ms_select.h` / `ms_select.c` 骨架：`MsSelectCaseKind`/`MsSelectOutcome`/`MsSelectExecResult` 枚举、`struct MsSelectCase`/`struct MsSelectState`、`msSelectStateDestroy`；`struct MsCoroutine` 增加 `selectState` 字段并接入协程销毁路径。验证：全量既有测试回归通过（尚无脚本可触达路径）。
2. 调度器 PRNG：`struct MsScheduler` 增加 `selectRngState`，`msSchedInit` 播种；`msSelectRandomNext`/`msSelectShuffle`。验证：同一进程多次洗牌产出不同排列、排列是合法置换（开发期断言）。
3. 编译器：`MS_AST_SELECT` 代码生成（BEGIN/ADD/EXEC/回填/绑定存取），行号归属。验证：ms 脚本单 case recv 就绪场景跑通（channel 由任务 44 提供），`default` 立即执行场景跑通。
4. VM 分派：`MS_OP_SELECT_BEGIN`（懒分配/复位帧）、`ADD_RECV`/`ADD_SEND`（弹栈登记、类型检查）、`EXEC` 的恢复判定与轮询阶段（不含挂起）。验证：双 case 其一就绪、多 case 同时就绪只执行一个、`default` 在无就绪时执行、`select { default: }`、send 值表达式在 select 入口恰好求值一次（副作用计数）。
5. 挂起与唤醒：`EXEC` 阶段 3 的多队列登记、`msSelectTryClaim`（CAS、撤销其余登记、入队唤醒）、channel 侧认领钩子与关闭唤醒。验证：无 `default` 且全部阻塞时协程挂起、对端 send/recv 唤醒后从 select 处继续、两个 channel 上各挂一个 case 时任一就绪即唤醒且另一登记被撤销（以唤醒后再操作另一 channel 的行为间接断言）、挂起期间 channel 关闭抛 `ChannelClosedError`。
6. 边界语义：空 `select {}` 永久挂起与死锁检测联动；非 channel 操作数 `TypeError`；已关闭 channel 的就绪-抛出语义；recv 绑定（`:=`/`=`/无绑定丢弃）；取消标志与被唤醒协程的交互（决策 8）。验证：对应 ms 脚本与负向用例。
7. GC 根：协程枚举补标 select 帧。验证：挂起中的 select 持有的 send 值/交付值经 GC 后完好（配合任务 17 的分配统计与任务 43 的根扫描）。
8. 全平台 Debug/Release 构建，Debug（ASAN）下跑全部测试，配合任务 02 分配统计确认无泄漏（含挂起-唤醒路径的节点摘除无遗漏）。

## 测试方案

本任务晚于任务 40（testing 模块），ms 脚本一律使用 `testing` 模块，由仓库根 `run_tests.py` 驱动 mslang CLI 执行。测试文件清单（本任务只交付本设计文档，测试代码随实现任务编写），目录沿用任务 43 的 `tests/ms/concurrency/`：

- `tests/ms/concurrency/select_basic_test.ms`：单 recv case 从有缓冲 channel 取到值并绑定（`case v := c.recv():`）；`v =` 赋值既有变量；无绑定 recv 丢弃值不报错；单 send case 向有接收方的无缓冲 channel 交付后执行体。
- `tests/ms/concurrency/select_ready_order_test.ms`：两个 case 同时就绪时恰执行其一、另一 case 无副作用；随机分布宽松断言——两就绪 case 循环 select 一万次，各自被选次数均在 30%–70% 区间（统计容差，不依赖种子）。
- `tests/ms/concurrency/select_eval_order_test.ms`：各 case 的 channel 表达式与 send 值表达式按源码序恰好求值一次（共享 list 记录求值轨迹）；未被选中 case 的 send 值表达式也已求值（决策 1）。
- `tests/ms/concurrency/select_default_test.ms`：无就绪时 `default` 执行且不挂起（前后语句顺序断言）；有 case 就绪时 `default` 不执行；`select { default: }`（零 case）直接走 default；带 default 的 select 在空 channel 上不阻塞（后续语句立即执行）。
- `tests/ms/concurrency/select_block_wakeup_test.ms`：无 default 时 select 挂起当前协程、另一协程 send 后唤醒并继续；双 channel 各挂一个 case，对任一 channel 操作即唤醒，另一 channel 上的登记被撤销（唤醒后另一 channel 的后续配对与计数行为断言）；send case 挂起后被接收方取走值（经 channel 对端断言值内容）。
- `tests/ms/concurrency/select_closed_test.ms`：recv case 的 channel 已关闭且排空时选中即抛 `ChannelClosedError`（try/except 断言）；send case 对已关闭 channel 抛 `ChannelClosedError`；挂起中的 select 因 channel 关闭被唤醒并抛 `ChannelClosedError`；关闭与正常就绪并存时行为符合伪随机就绪语义（只断言不崩溃且两种结局之一，不做精确分布断言）。
- `tests/ms/concurrency/select_loop_test.ms`：`for` 循环内 select 多路消费两个生产者的消息直到双双关闭（以 try/except 消化 `ChannelClosedError` 计数退出），断言全部消息被消费且无重复——覆盖循环中反复挂起/唤醒与帧复用。
- `tests/ms/concurrency/select_errors_test.ms`（负向用例拆分为独立脚本，配 `<name>.exit` 声明预期退出码）：case 的 channel 表达式求值为非 channel 抛 `TypeError`；空 `select {}` 单协程程序触发死锁检测抛 `RuntimeError`；多目标绑定对单值解包失败抛 `TypeError`（决策 6 的解包语义）。
- 回归：任务 09–44 的全部既有脚本测试（尤其任务 43 的协程与任务 44 的 channel 测试）不变通过。

## 验收标准

- [ ] `src/sched/ms_select.h` / `ms_select.c` 存在，guard 为 `MSLANG_SRC_SCHED_MS_SELECT_H_`，头文件自包含；代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsSelectState`/`struct MsSelectCase` 不 typedef）；堆分配只经 `msAlloc/msRealloc/msFree`。
- [ ] 编译器把 `MS_AST_SELECT` 编译为 `MS_OP_SELECT_BEGIN` / `ADD_RECV` / `ADD_SEND` / `EXEC` 四条指令，case 体地址回填正确，recv 绑定复用任务 11 存取语义，行号归属 select 语句。
- [ ] 求值次序落实：channel 表达式与 send 值表达式在进入 select 时按源码序恰好各求值一次；非 channel 操作数抛 `TypeError`。
- [ ] 伪随机选择落实：多 case 同时就绪时按均匀随机排列选首个就绪者；循环 select 下两就绪 case 的选择分布落在统计容差内。
- [ ] `default` 语义落实：无就绪时执行且不挂起；有就绪时永不执行。
- [ ] 挂起/唤醒机制落实：无 `default` 且全部阻塞时协程同时挂入全部相关 channel 的等待队列；任一 case 就绪经 `msSelectTryClaim` 唯一认领、撤销其余登记、唤醒后从 select 语句继续且现场完整；挂起期间 channel 关闭以 `ChannelClosedError` 唤醒。
- [ ] 空 `select {}` 永久挂起当前协程，与任务 43 死锁检测联动正确（单协程报 `RuntimeError`，多协程安静挂起）。
- [ ] 已关闭 channel 的 recv/send case 按「语义决策 5」的就绪-抛出语义工作。
- [ ] GC 根覆盖挂起 select 帧的 send 值与交付值；`msSelectTryClaim` 的认领经 `MsAtomicI32` CAS，恢复路径与认领路径的写读按 acquire/release 配对。
- [ ] 「测试方案」全部 ms 脚本通过；任务 09–44 既有测试回归通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 07（回填/丢弃码生成）、任务 44（`struct MsChannel`、`struct MsChanWaiter`、非阻塞收发与等待队列函数）的接口假定在实现时已按对应任务文档定名对齐。
