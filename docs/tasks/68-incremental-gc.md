# 68 增量 GC

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v1.0 | ⬜ | [47 GC safepoint 改造](47-gc-safepoint.md)、[64 性能基准套件](64-benchmarks.md) |

## 任务目标

把任务 17/47 交付的 STW 标记-清除 GC 改造为**增量三色标记-清除**收集器，兑现 [08-vm-internals.md](../language/08-vm-internals.md) §5「标记实现为可中断的步骤函数，为增量改造留口」与 §7 性能路线图第 4 项（增量/并发 GC）的既定演进。交付：

- 三色抽象落地：在任务 17 的两色枚举上扩展灰色与「双白」，标记栈语义从「黑色待遍历」改为标准的灰栈；
- Dijkstra 插入写屏障：挂在全部堆对象写边（容器写入、属性写入、upvalue、全局变量、C API 容器操作），保证增量标记期间 mutator 不破坏标记不变量；
- 增量标记主循环：`msGcAlloc` 分配路径上按配速（mark debt）推进 `msGcMarkStep` 步骤函数，世界只在每轮收集的两个短 STW 停顿（标记起点、终止重扫）停住；
- 惰性增量清除：清除阶段同样分步推进，与 mutator 交错，不再出现 O(堆大小) 的单次 STW；
- 停顿测量与回退开关：逐次 STW 停顿经任务 42 的 `msClockMonotonicNs` 计时并汇总统计；CMake 选项 `MS_GC_INCREMENTAL`（默认 `ON`）保留任务 47 的 STW 行为作为回退构建。

完成后，任务 47 语义下的单轮「全堆标记 + 清除」长停顿被消除：STW 停顿上限由「与堆大小成正比」降为「与根集/栈规模成正比」。验收以任务 64 基准套件上的**停顿时间**为主门槛（吞吐仅为护栏）：GC 压力基准下 p99 STW 停顿 ≤ 2 ms、p99.9 ≤ 5 ms；全部既有测试在 `MS_GC_INCREMENTAL=ON/OFF` 两种构建、单线程与多线程配置下回归通过。

本任务不做并发标记（GC 工作全部由 mutator 线程在分配点顺带完成，无独立 GC 线程）；并发标记是 §7 路线图的更后续项。

## 设计依据

- [08-vm-internals.md](../language/08-vm-internals.md)
  - §5 GC：三色标记的演进路径（「增量三色标记（需写屏障）→ 并发标记」）；标记实现为可中断的步骤函数是既定留口；safepoint 检查是读取一个原子标志；触发阈值按存活率自适应（GOGC 思路）。
  - §7 性能路线图第 4 项：增量/并发 GC。
- [47 GC safepoint 改造](47-gc-safepoint.md)（已定稿，本任务在其上扩展）：`struct MsSafePoint`（`flags`/`phase`/`parkedCount`/`threadCount`/线程注册表）、`MsGcPhase` 枚举、`msGcStopTheWorld`/`msGcResumeWorld`/`msSafePointCheck` 握手协议、「停车后不执行字节码/分配/调度操作」不变量、`msGcMarkStaticRoots`/`msGcMarkCoroutineRoots` 的根集两分。本任务复用其两次短停顿的握手机制，并扩展其 `MsGcPhase` 枚举（见「详细设计」§3，为对任务 47 已定稿枚举的显式修订）。
- [17 GC 标记-清除](17-gc-mark-sweep.md)（已定稿）：`struct MsGc`、`msGcAlloc`/`msGcCollect`/`msGcMarkObject`/`msGcMarkValue`、`msGcMarkStep` 步骤函数（预算制、边界即 mutator 插入点）、`msGcSweep` 全链表单遍清除、双函数拆分的根集扫描、阈值自适应公式、「先触发后入链」的分配顺序。本任务复用其全部机制，只改颜色语义与驱动方式。
- [06 对象模型基础](06-object-model.md)：`struct MsObjectHeader{type, tag, markColor, gcNext}`；其 `MsMarkColor`（`MS_MARK_WHITE`/`MS_MARK_BLACK`）与任务 17 的 `MsGcColor`（`MS_GC_WHITE`/`MS_GC_BLACK`）是同一颜色概念的两份命名——本任务在 GC 侧枚举上扩展 `MS_GC_GRAY`/`MS_GC_OTHER_WHITE` 后，头部字段枚举须同步扩展为同一份定义，两份命名合一属实现时的对齐项，以任务 06/17 对齐结果为准。
- [42 平台抽象层](42-platform-layer.md)（已定稿）：`struct MsMutex`、`msMutexLock`/`msMutexUnlock`、`msClockMonotonicNs`（单调纳秒，停顿计时的唯一入口）。
- [10-c-style.md](../language/10-c-style.md)：§1 include guard、§2 格式化、§3 命名、§4 typedef 规则、§6 内存纪律、§9 平台相关代码只允许出现在 `src/platform/`。
- [65 NaN-boxing 值表示](65-nan-boxing.md)（已定稿，编号大于本任务但同属 v1.0 且先于本任务排期）：标记入口已改为值形态 `msGcMarkValue(MsState* L, MsValue v)`——本任务的写屏障同步提供值形态包装，立即数入屏障为空操作；`MS_GC_INCREMENTAL` 回退开关的「双构建回归」做法对齐其 `MS_NAN_BOXING` 的先例。
- 任务 64 性能基准套件（`64-benchmarks.md`）当前尚不存在：本文假定其入口为 `bench/run_benchmarks.py`、结果落盘于 `bench/results/<label>.json`、基线存于 `bench/baselines/`（与任务 65 的假定命名一致），GC 压力基准项假定名 `gc_stress`；实现时以任务 64 文档定名为准。
- 任务 46（M:N 调度器，`struct MsSched`/`struct MsWorker`）文档尚不存在，经任务 47 的假定命名引用，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件与模块划分

- 新增 `src/gc/ms_gc_incr.h` / `src/gc/ms_gc_incr.c`，include guard `MSLANG_SRC_GC_MS_GC_INCR_H_`；自包含（自行 include `<stdbool.h>` `<stddef.h>` `<stdint.h>` 及 `"gc/ms_gc.h"`、`"gc/ms_safepoint.h"`）。增量驱动状态机、写屏障、配速、停顿统计集中于此模块，前缀 `msGcIncr`。
- 修改 `src/gc/ms_gc.h` / `ms_gc.c`：`MsGcColor` 扩展灰色与双白；`msGcSweep` 拆出分步游标形态 `msGcSweepStep`；`struct MsGc` 增加增量相关字段；`msGcAlloc` 的触发逻辑改走增量状态机。
- 修改 `src/gc/ms_safepoint.c`：`MsGcPhase` 枚举扩展（见 §3）；STW 停顿计时挂在 `msGcStopTheWorld`/`msGcResumeWorld` 配对上。
- 修改对象/容器模块（任务 06/16/32、VM 的 `MS_OP_SET_INDEX`/`MS_OP_SET_ATTR`/`MS_OP_STORE_GLOBAL`/`MS_OP_STORE_UPVAL` 慢路径、C API 容器写入函数）：全部堆对象写边收口处插入写屏障调用。屏障插入点清单见 §4。
- CMake 新增选项 `MS_GC_INCREMENTAL`（v1.0 起默认 `ON`）：`OFF` 时 `ms_gc_incr.c` 的增量驱动退化为任务 47 的 STW 全流程（写屏障调用点为带内联空操作的宏），整条路径与 v0.4 等价，做法对齐任务 65 的 `MS_NAN_BOXING` 先例；不提供运行期开关。

### 2. 三色抽象与颜色枚举扩展

任务 17 的两色方案（白 = 未标记、黑 = 已标记，待遍历对象经标记栈暂存）扩展为标准三色 + 双白：

```c
typedef enum {
  MS_GC_WHITE = 0,        // 当前白：本轮未标记；新生对象在标记/清除期间取此色
  MS_GC_BLACK = 1,        // 已标记且子对象已全部入灰栈
  MS_GC_GRAY = 2,         // 已标记、待遍历（位于灰色栈中）
  MS_GC_OTHER_WHITE = 3   // 另一白：上一轮的白；仅清除阶段把「当前白」判死
} MsGcColor;
```

- 不变量（增量标记期间）：黑色对象绝不直接引用白色对象（强三色不变量，Dijkstra 屏障维护）；灰色对象可引用任意色。
- 双白翻转：每轮收集在终止停顿末尾把 `gc->whiteBit` 翻转（当前白 ↔ 另一白）；清除只回收「翻转前的当前白」（翻转后变为 `MS_GC_OTHER_WHITE`），新生对象一律染翻转后的当前白——新生对象天然跳过本轮清除，无需「新生染黑」的额外规则（Lua 同款双白方案）。
- `msGcMarkObject` 语义调整：白 → 灰并入灰栈（原为白 → 黑）；`msGcMarkStep` 弹出灰对象、遍历子对象后染黑。清除阶段把黑色对象复位为**当前白**（供下一轮），与任务 17 的复位语义同形。
- 单例、int 缓存、类型对象（任务 06 的永久存活对象）保持黑且不链入全对象链表，不受双白翻转影响。

### 3. MsGcPhase 状态机扩展

任务 47 的四值枚举（`IDLE`/`REQUESTED`/`MARKING`/`SWEEPING`）扩展为六值——这是对任务 47 已定稿枚举的显式修订（`MARKING`/`SWEEPING` 的语义由「世界停止」改为「世界运行、增量推进」，任务 47 文档中的相应描述以本文为准）：

```c
typedef enum {
  MS_GC_PHASE_IDLE,         // 无回收
  MS_GC_PHASE_REQUESTED,    // STW 握手中：标记起点停顿（pause #1）
  MS_GC_PHASE_MARKING,      // 增量标记中，世界运行，写屏障生效
  MS_GC_PHASE_TERMINATING,  // STW 握手中：终止重扫停顿（pause #2）
  MS_GC_PHASE_SWEEPING,     // 增量清除中，世界运行
  MS_GC_PHASE_COUNT
} MsGcPhase;
```

状态迁移（每轮收集一次循环）：

```
IDLE --(msGcAlloc 越阈，CAS 竞争出发起者)--> REQUESTED
REQUESTED --(握手完成，起点停顿执行根标记)--> MARKING   [停顿 #1 结束，世界恢复]
MARKING --(灰栈排空被某 mutator 观察到)--> TERMINATING
TERMINATING --(重扫根 + 排空 + 翻转双白)--> SWEEPING    [停顿 #2 结束，世界恢复]
SWEEPING --(分步清除走完全对象链表)--> IDLE            [阈值自适应，统计落账]
```

- `phase` 的并发收敛沿用任务 47：挂在 `MsSafePoint.phase` 原子字上，CAS 保证唯一发起者；竞争失败的线程不停车等待整轮（那是 STW 语义），而是继续 mutator 工作——增量阶段不需要其他线程配合。
- `MS_GC_INCREMENTAL=OFF` 构建下状态机退化为任务 47 原语义：`REQUESTED → MARKING（STW 全量标记）→ SWEEPING（STW 清除）→ IDLE`，枚举值兼容、无行为差异。

### 4. 写屏障（Dijkstra 插入屏障）

**选型**：Dijkstra 插入屏障（写边建立时，若持有方已黑、被引用方为白，则把被引用方染灰）+ 终止停顿重扫无屏障根（协程栈、全局表）。Yuasa 删除屏障（记录被覆盖的旧值）可免去栈重扫，但要求一切覆写点（含求值栈槽、C 局部变量跨分配点）挂屏障，侵入面大且与 09-c-api §3 的根栈纪律重复；Dijkstra 方案的屏障只挂在容器/对象字段写边（数量少、冷路径为主），栈与寄存器窗口这些高频写点走终止重扫——Lua 5.x 验证过的同款取舍。

```c
// Dijkstra insertion barrier. Must be called BEFORE *slot = value on every
// store of a heap-object pointer into another heap object. Fast path: two
// color loads, no lock. Shading (white -> gray, push) runs under
// gc->grayLock (see below). NULL value is a no-op.
void msGcWriteBarrier(MsState* L, struct MsObject* owner, struct MsObject* value);

// Value-form wrapper (task 65 value representation): immediate values are
// a no-op; heap-object payloads delegate to msGcWriteBarrier.
void msGcWriteBarrierValue(MsState* L, struct MsObject* owner, MsValue value);
```

屏障谓词（伪流程）：

```c
if (gc->phase == MS_GC_PHASE_MARKING &&   // 原子字普通读即可，见下
    owner->header.markColor == MS_GC_BLACK &&
    (value->header.markColor == MS_GC_WHITE ||
     value->header.markColor == MS_GC_OTHER_WHITE)) {
  shadeGray(gc, value);                   // grayLock 保护：染灰 + 压栈
}
```

- 屏障插入点清单（实现时逐一审计，编译期无法强制，靠本节清单 + 评审 + 测试方案的不变量测试兜底）：list/dict/set/tuple 元素写入与扩容搬移（任务 16/32）、实例与类属性写入（`MS_OP_SET_ATTR` 慢路径）、全局/模块变量表写入（`MS_OP_STORE_GLOBAL`）、upvalue 写入与关闭（任务 14）、协程对象字段（`result`/`waiters` 在协程对象被染黑后的写入）、C API 容器操作与 C 扩展类型的对象字段存储（任务 33 的 setter 约定处）。求值栈、调用帧局部槽、寄存器窗口、C 局部变量**不挂屏障**，由终止停顿重扫覆盖。
- 并发互斥：增量标记由多个 mutator 线程各自在分配点推进，共享灰栈的「染灰 + 压栈」与步骤函数的「弹栈 + 遍历」都经 `gc->grayLock`（任务 42 的 `struct MsMutex`）互斥；颜色字段的读写在持锁或单线程语义下安全（屏障谓词的快路径两读允许竞态——漏读导致的重复染灰无害，染灰后的 release 压栈与读取方的 acquire 弹栈配对）。此处不加平台原子，颜色字节沿用普通 `uint8_t`，竞态窗口的正确性论证（最坏情形是重复标记，绝不漏标）写入实现注释。
- `phase` 读取：屏障热路径对 `MsSafePoint.phase` 做普通（非原子序）读——漏过一次「进入 MARKING」的后果是该线程在下一次 safepoint 检查前不执行屏障，而进入 MARKING 必然先经 STW 起点停顿（世界静止、全体线程在 safepoint），停顿恢复时的 release/acquire 配对保证此后所有线程看到新相位，故普通读安全；该论证写入实现注释。

### 5. 增量标记主流程

复用任务 17 的 `msGcMarkStep` 步骤函数（预算制），驱动逻辑收在 `ms_gc_incr.c`：

```c
// Trigger-side entry called from msGcAlloc when allocBytes crosses the
// threshold in phase IDLE. Runs pause #1 via msGcStopTheWorld: gray the
// static roots (msGcMarkStaticRoots) and push every scheduler coroutine
// OBJECT (not its stack) onto the gray stack — stacks are traversed when
// the coroutine object is popped, keeping the pause proportional to the
// root count, not to total stack slots. Then msGcResumeWorld -> MARKING.
MsResult msGcIncrStartCycle(MsState* L);

// Mutator-side mark driver, called from msGcAlloc on every allocation while
// phase == MARKING. Adds sizeBytes to the mark debt and, when the debt
// reaches MS_GC_INCR_STEP_BYTES, runs msGcMarkStep with budget
// MS_GC_INCR_STEP_BUDGET. When the step reports the gray stack drained,
// enters pause #2 (see msGcIncrTerminate).
MsResult msGcIncrMarkStep(MsState* L, size_t sizeBytes);

// Pause #2: msGcStopTheWorld; rescan the barrier-free roots — every
// coroutine's frames/eval stack (msGcMarkCoroutineRoots) and the static
// roots (msGcMarkStaticRoots) — then drain the gray stack to empty, flip
// the white bit, move the sweep cursor to the all-objects head, flip phase
// to SWEEPING, msGcResumeWorld. Pause length is proportional to root slots
// plus residual gray work, not to heap size.
MsResult msGcIncrTerminate(MsState* L);
```

常量：

```c
#define MS_GC_INCR_STEP_BYTES   (16u * 1024u)   // 配速：每分配 16KB 推进一步
#define MS_GC_INCR_STEP_BUDGET  128             // 每步最多遍历对象数（任务 17 预算语义）
#define MS_GC_PAUSE_GOAL_MS     2               // STW 停顿设计目标（构建期可调，非硬保证）
```

- 配速（mark debt）：`gc->markDebt += sizeBytes`（含 `msGcAccountAlloc` 登记的容器辅助缓冲）；债务累计到 `MS_GC_INCR_STEP_BYTES` 才推进一步并把债务清零——避免逐次小分配都走步骤函数的分摊开销。债务不清零而越积越多（分配率高于标记率）时下一轮分配继续推进，天然形成 mutator assist；不做「债务上限内强制加班」的复杂调度，吞吐护栏由验收门槛兜底。
- 非分配线程不阻塞进度：纯计算协程不推进标记，但任何分配中的线程都会推进；灰栈排空由「正在分配的线程」观察到即可进入终止停顿，正确性不依赖全部线程参与。
- 手动 `msGcCollect`（任务 17/18 的诊断入口）：语义改为「若处于 IDLE 则跑一整轮增量收集的同步版（起点停顿 → 循环推进至排空 → 终止停顿 → 清除走完整链表）」，供测试与 C API 获得确定性的完整回收点；`disableCount` 语义与任务 17 一致（抑制自动触发，不抑制手动）。

### 6. 增量清除（惰性分步）

任务 17 的 `msGcSweep` 拆为游标式分步函数，SWEEPING 阶段由 `msGcAlloc` 顺带推进：

```c
// Incremental sweep: walks at most budget nodes of the all-objects list
// from gc->sweepCursor. Frees MS_GC_OTHER_WHITE objects via msObjFree,
// recolors black objects to the current white, and accumulates live
// bytes. Sets *done when the walk reaches the list end; the caller then
// adapts the threshold (task 17's formula) and flips phase to IDLE.
// Never fails.
void msGcSweepStep(MsState* L, size_t budget, bool* done);
```

- `struct MsGc` 新增 `struct MsObject** sweepCursor`：指向下一个待检查节点的链接位置（与任务 17 清除循环的 `p` 同形）。
- SWEEPING 阶段每步预算 `MS_GC_INCR_STEP_BUDGET`，推进时机与标记步相同（分配点、按债务）；清除未完成时新分配照常——新生对象染当前白、链表插入在游标之前（头插），游标走过的区域不会再遇新对象，语义自洽。
- 一轮收集结束时（`done`）才做任务 17 §7 的阈值自适应与 `collectCount++`；`lastLiveBytes` 由各清除步累计。
- 连续两轮：SWEEPING 未完成时阈值再次越过（高分配压力）不提前开新轮——`msGcAlloc` 在 SWEEPING 相位下只推进清除步，触发判定仅 `IDLE` 有效；极端分配压力下内存增长由阈值自适应在下一轮收敛，与 Go 的「一轮未结束不开新轮」同策略。

### 7. struct MsGc 扩展字段

在任务 17 的 `struct MsGc` 尾部追加（既有字段不动）：

```c
struct MsGc {
  // ... 任务 17 既有字段（allObjects、markStack、roots、阈值与统计）...
  struct MsMutex  grayLock;      // 灰栈（markStack 复用）与染灰的互斥
  struct MsObject** sweepCursor; // 增量清除游标
  size_t          markDebt;      // 配速债务（字节）
  uint8_t         currentWhite;  // MS_GC_WHITE 或 MS_GC_OTHER_WHITE
  bool            incrActive;    // 编译开关 ON 时恒 true，集中判定用
  // 停顿统计（见 §8）
  uint64_t        pauseCount;
  uint64_t        totalPauseNs;
  uint64_t        maxPauseNs;
  uint64_t        lastPauseNs;
};
```

任务 17 的 `markStack` 字段复用为灰栈（语义改名注释说明，不改字段名以保持任务 47 代码兼容）；任务 17 的 `MS_GC_MARK_STEP_BUDGET` 保留为 STW 回退路径与手动收集的驱动预算。

### 8. 停顿测量与统计

- 计时点：`msGcStopTheWorld` 返回后（握手完成）记 `t0 = msClockMonotonicNs()`，`msGcResumeWorld` 前记 `t1`，差值累入 `pauseCount`/`totalPauseNs`/`maxPauseNs`/`lastPauseNs`。两次短停顿（起点/终止）分别计次——统计口径是「单次 STW 停顿」，与验收门槛同口径。
- 诊断接口：

```c
struct MsGcPauseStats {
  uint64_t pauseCount;
  uint64_t totalPauseNs;
  uint64_t maxPauseNs;
  uint64_t lastPauseNs;
};

// Snapshot of the incremental-GC pause counters (subset of msGcGetStats'
// struct; kept separate so task 17's MsGcStats stays unchanged).
void msGcGetPauseStats(const MsState* L, struct MsGcPauseStats* out);
```

- 基准对接（假定命名，以任务 64 文档为准）：环境变量 `MS_GC_PAUSE_STATS=1` 时进程退出前向 stderr 打印一行机器可解析汇总（`gc_pause: count=N total_ns=N max_ns=N`），任务 64 的 `bench/run_benchmarks.py` 解析该行进报告；分位数（p99/p99.9）由基准驱动在多次运行/多次停顿样本上计算，C 侧只提供原始计数与逐次日志。逐次明细日志用 `MS_GC_PAUSE_LOG=1`（每停顿一行），仅调试用途。

### 9. 与 safepoint/调度器的整合规则

- 增量推进点在 `msGcAlloc` 内（任务 47 已保证分配路径先于 safepoint 检查），不新增 safepoint 类别；VM 分派循环与调度器对增量 GC 完全无感知——世界运行期间的标记/清除步由分配者顺带完成，不需要其他线程停车。
- 两次 STW 停顿复用任务 47 的 `msGcStopTheWorld`/`msGcResumeWorld` 握手与「停车后不执行字节码/分配/调度操作」不变量；锁序规则不变（发起者等待握手时不持有任何调度器锁）。
- 起点停顿的工作量（静态根 + 协程对象入灰栈）与终止停顿的工作量（根重扫 + 排空灰栈）都在 STW 内、由发起者单线程执行；排空灰栈在停顿内不设预算上限——终止阶段的排空残余量由配速保证有界（债务机制使灰栈接近排空才被观察到排空），长尾风险由验收门槛的 max ≤ 10 ms 项监控。
- 空闲工作线程「空闲即停车」（任务 47）天然兼容：增量阶段无全局动作，空闲线程无需唤醒参与 GC。
- 外部线程（`MS_VT_EXTERNAL`）：其 C API 调用若触发分配，同样推进增量步；其 C API 根栈在两次 STW 停顿内经 `msGcMarkStaticRoots` 扫描，与任务 47 一致。

### 10. 回退开关与构建矩阵

- CMake 选项 `MS_GC_INCREMENTAL`（v1.0 起默认 `ON`）：`OFF` 时 `msGcIncr*` 三个驱动函数编译为任务 47 的 STW 全流程调用、`msGcWriteBarrier` 为内联空操作、`MsGcPhase` 的 `TERMINATING` 不出现——与任务 65 的 `MS_NAN_BOXING` 双构建先例一致，CI 对两种构建各跑完整测试与基准。
- 构建产物只落在 `build/`（AGENTS.md 与 10-c-style 约束不变）。

## 实现步骤

1. 颜色扩展：`MsGcColor` 加 `MS_GC_GRAY`/`MS_GC_OTHER_WHITE`，`struct MsGc` 加 `currentWhite` 与双白翻转辅助（文件内 `static`）；任务 06 头部枚举与任务 17 GC 枚举的对齐合一同步落地。验证：C 单元测试断言四色枚举值、翻转辅助的真值表；任务 17 既有 GC 测试在全量 STW（`MS_GC_INCREMENTAL=OFF` 等价路径）下保持绿色。
2. `MsGcPhase` 扩展与状态机骨架：`TERMINATING` 相位、迁移函数（文件内 `static`）、CAS 收敛沿用任务 47；`msGcAlloc` 的触发判定改为按相位分派（IDLE 才触发）。验证：C 单元测试驱动相位迁移序列，非法迁移触发 `MS_ASSERT`。
3. 灰栈互斥与 `msGcMarkObject` 语义切换（白→灰压栈、弹栈遍历后染黑），`msGcMarkStep` 预算语义不变。验证：任务 17 的标记测试改写为三色断言后通过；多线程压力（N 线程并发 `msGcMarkObject`）下无重复释放/漏标（ASAN + 分配统计）。
4. 写屏障：`msGcWriteBarrier`/`msGcWriteBarrierValue` 与 §4 清单的全部插入点接入；`MS_GC_INCREMENTAL=OFF` 下为内联空操作。验证：C 单元测试构造「黑持有方 + 白值」场景断言染灰；屏障缺失探测测试（临时注释单点屏障，增量不变量检查器报错，测后恢复——以构建开关而非提交代码差异实现）。
5. 增量标记驱动：`msGcIncrStartCycle`（起点停顿）+ `msGcIncrMarkStep`（债务配速）+ `msGcIncrTerminate`（终止停顿、根重扫、双白翻转）。验证：C 单元测试以受控分配序列走完整轮，断言两次停顿的相位序列与标记完整性；ms 层 GC 压力脚本（任务 17/47 既有）在 `ON` 构建下通过。
6. 增量清除：`msGcSweepStep` 游标式分步 + `struct MsGc` 扩展字段 + SWEEPING 相位的分配点推进。验证：C 单元测试断言分步预算上界、游标推进、清除后 `lastLiveBytes` 与阈值自适应与任务 17 公式一致；新生对象（清除期间分配）存活。
7. 停顿统计与诊断：`msGcGetPauseStats`、停顿计时挂接、`MS_GC_PAUSE_STATS`/`MS_GC_PAUSE_LOG` 环境变量。验证：C 单元测试断言计数单调；手动跑 GC 压力脚本观察 stderr 汇总行格式。
8. 全量回归与基准：两种构建（`ON`/`OFF`）× 单线程/多线程全测试套件；任务 64 基准套件跑 GC 压力基准并落盘停顿报告，按「验收标准」门槛核对，未达标项做剖析调优（重点：屏障快路径、配速常量、终止排空残余量）。

## 测试方案

本任务晚于任务 40：ms 脚本一律用 `testing` 模块（`import "testing"` + `testing/assert`），由仓库根 `run_tests.py` 驱动；GC 内部不变量（颜色、相位、屏障、配速）脚本不可观测，用 C 单元测试覆盖（`ms_test.h`/`MS_TEST`/`MS_ASSERT_EQ`）。停顿测量经任务 64 基准套件（假定入口 `bench/run_benchmarks.py` + `MS_GC_PAUSE_STATS` 解析）完成。本任务只交付设计文档，测试实体随实现步骤编写。

### 1. C 单元测试（tests/c/test_gc_incremental.c）

- 颜色与双白：四色枚举值；`currentWhite` 翻转后新生对象取当前白、`MS_GC_OTHER_WHITE` 对象在清除中被回收；单例/int 缓存/类型对象不受翻转影响。
- 相位状态机：完整迁移序列 `IDLE→REQUESTED→MARKING→TERMINATING→SWEEPING→IDLE`；非法迁移 `MS_ASSERT`；多线程并发触发经 CAS 收敛为单发起者、其余线程不停车。
- 写屏障：黑持有方写白值 → 值染灰入栈；白持有方/灰持有方/非 MARKING 相位下屏障为空操作；`NULL` 值安全；值形态包装对立即数为空操作（`ON` 且任务 65 落地后）。
- 强三色不变量：受控增量轮（小预算步进 + 交错 mutator 写入）全程断言「无黑色对象直接引用白色对象」——以调试构建的不变量检查器（遍历全对象链表 + `msObjTraverse` 回调校验）在每一步后执行。
- 配速：分配字节累计达 `MS_GC_INCR_STEP_BYTES` 才推进一步；高分配率下债务累积与推进次数符合预期；灰栈排空后进入终止相位。
- 增量清除：`msGcSweepStep` 每步处理数 ≤ 预算；清除期间头插的新生对象不被本轮回收；`done` 后阈值按任务 17 公式自适应、`collectCount` +1。
- 停顿统计：受控轮次后 `pauseCount`/`totalPauseNs`/`maxPauseNs` 单调且自洽（max ≥ total/count）。
- 回退构建：`MS_GC_INCREMENTAL=OFF` 下行为与任务 47 逐点一致（相位无 `TERMINATING`、屏障空操作、单轮 STW 完成标记+清除）。

### 2. ms 脚本测试（tests/ms/gc/，testing 模块）

- `incr_alloc_pressure_test.ms`：大循环构造临时字符串/list/dict（累计分配量远超阈值，保证多轮增量收集），维护校验和并断言——覆盖增量标记+清除下的存活判定。
- `incr_live_mutation_test.ms`：构造大型长期存活对象图（list 套 dict 套字符串），在持续分配压力下反复改写容器内部引用（删边/加边），末尾逐项断言存活对象内容——压力化覆盖写屏障正确性（脚本无法观测相位，靠分配压力保证多轮收集与交错）。
- `incr_coroutine_roots_test.ms`：多协程在 channel 上阻塞/唤醒期间叠加分配压力，断言阻塞协程局部对象的最终内容——覆盖终止停顿的协程栈重扫。
- 并发配置：全部 GC 脚本在 `MS_THREADS=1` 与 `MS_THREADS≥4` 下各跑一遍（同任务 47 的环境变量约定）。

### 3. 停顿测量（任务 64 基准套件对接，假定命名）

- 基准项：任务 64 套件中的 GC 压力基准（假定名 `gc_stress`，大存活堆 + 高分配率）与混合负载基准各跑 ≥ 5 轮；`MS_GC_PAUSE_STATS=1` 收集每轮停顿汇总，驱动侧计算 p99/p99.9/max。
- 对照：同一基准在 `MS_GC_INCREMENTAL=OFF`（任务 47 STW 行为）下跑同轮数，产出「STW vs 增量」停顿对比表，随 `bench/results/v1.0-incr-gc.json` 落盘存档。
- 吞吐护栏：基准墙钟时间相对任务 65 基线（`bench/baselines/v1.0-nanbox.json`）的劣化 ≤ 10%——护栏项，非主门槛。

### 4. 回归

- 全部既有测试（`tests/c/` 与 `tests/ms/`，任务 09–67 的全部套件）在 `MS_GC_INCREMENTAL=ON`/`OFF` × `MS_THREADS=1`/`≥4` 四种组合下通过。
- Debug + ASAN（MSVC 用 `/RTC`）下全部 GC 测试无悬垂引用、无重复释放、无泄漏（任务 02 分配统计归零）。

## 验收标准

- [ ] `src/gc/ms_gc_incr.h` / `ms_gc_incr.c` 存在，guard 为 `MSLANG_SRC_GC_MS_GC_INCR_H_`，头文件自包含；代码风格通过 [10-c-style.md](../language/10-c-style.md) 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsGc` 扩展字段不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`、原子与互斥只经 `src/platform/` 抽象）。
- [ ] 三色 + 双白落地：`MS_GC_GRAY`/`MS_GC_OTHER_WHITE` 加入颜色枚举并与任务 06 头部枚举合一；强三色不变量在调试构建的不变量检查器下全程成立；双白翻转后新生对象天然跳过本轮清除。
- [ ] Dijkstra 插入屏障覆盖「详细设计」§4 清单的全部堆对象写边；求值栈/帧槽/C 局部不挂屏障、由终止停顿重扫覆盖；屏障快路径无锁。
- [ ] `MsGcPhase` 六值状态机按 §3 迁移，并发触发经 CAS 收敛；两次 STW 停顿复用任务 47 握手且满足其不变量与锁序规则；起点停顿工作量正比于根集规模、终止停顿正比于根槽数 + 残余灰栈，均与堆大小脱钩。
- [ ] 配速（mark debt）与增量清除分步按 §5/§6 实现；SWEEPING 未完成不开新轮；`msGcCollect` 手动语义与 `disableCount` 语义同任务 17；阈值自适应公式不变。
- [ ] 停顿统计（`msGcGetPauseStats`）与 `MS_GC_PAUSE_STATS`/`MS_GC_PAUSE_LOG` 诊断输出可用，口径为「单次 STW 停顿」。
- [ ] 停顿门槛达标（任务 64 基准、参考负载、≥ 5 轮）：p99 STW 停顿 ≤ 2 ms、p99.9 ≤ 5 ms、max ≤ 10 ms；相对 `MS_GC_INCREMENTAL=OFF` 的 STW 基线停顿显著下降且对比表落盘；吞吐劣化 ≤ 10%（护栏）。
- [ ] `MS_GC_INCREMENTAL=ON`/`OFF` × `MS_THREADS=1`/`≥4` 四种组合下全部既有测试与「测试方案」新增测试通过；Debug + ASAN（或 `/RTC`）无内存错误与泄漏；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 06/17/46/47/64/65 的接口对齐项（颜色枚举合一、`MsValue` 形态屏障、基准入口与基线文件名等）在实现时已按对应任务文档定名落实。
