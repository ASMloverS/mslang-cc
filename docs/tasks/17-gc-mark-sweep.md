# 17 GC 标记-清除

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [06 对象模型基础](06-object-model.md)、[09 最小可运行解释器](09-minimal-interpreter.md) |

## 任务目标

交付 mslang 的垃圾回收模块（`src/gc/ms_gc.h` / `src/gc/ms_gc.c`）：单线程 STW 标记-清除收集器，覆盖 [08-vm-internals.md](../language/08-vm-internals.md) §5 规定的首版 GC 全部能力——

- **黑白两色 + 标记栈**的标记算法：对象头 `markColor` 只有白/黑两值，待遍历对象压入显式标记栈，不使用递归；
- **全对象链表**：所有堆对象经对象头 `gcNext` 串成侵入式链表，分配时登记、清除时遍历、关闭时整体释放；
- **根集**：协程调用栈/求值栈、全局命名空间（v0.1 兼任模块注册表）、内建类型表、C API 显式根栈（`msRootPush` 压入的对象）、错误槽与驻留缓存；
- **触发与自适应**：分配字节计数越过阈值即触发（初始 1MB，Go 的 GOGC 思路按存活量自适应调整），支持禁用计数与手动全量收集；
- **无终结器**：不支持 `__del__`，回收即释放；
- **增量留口**：标记实现为按预算推进的步骤函数 `msGcMarkStep`，为任务 68（增量 GC）预留中断点；多线程 STW safepoint 协议不在本任务（任务 47）。

完成后，解释器在任意分配压力下不再泄漏：不可达对象（含循环引用）被回收，存活对象（含协程栈上、根栈上的对象）绝不被误收；任务 18 在其上挂接公开 C API（`msRootPush` / `msRootPop` / `msGCCollect` 等），任务 47 复用本任务的标记-清除主体做 safepoint 改造。本任务自身以 C 单元测试（`tests/c/test_gc.c`）与 ms 脚本压力测试（`tests/ms/gc/`）结合验证。

## 设计依据

- [08-vm-internals.md](../language/08-vm-internals.md)
  - §3 对象模型：`struct MsObjectHeader` 含 `type` / `markColor` / `gcNext` 三字段；所有值均为 `MsObject*` 装箱对象（首版不做 NaN-boxing）；小整数（-256..4095）与短字符串驻留缓存。
  - §4 VM 执行核心：每个协程持有调用栈（`MsCallFrame` 帧数组，帧含 `proto` 与 upvalue 数组）与求值栈。
  - §5 GC：阈值触发（初始 1MB，按存活率自适应，GOGC 思路）；黑白两色 + 标记栈；根集合为协程栈、模块注册表、内建类型表、C API 显式根；清除遍历全对象链表；首版不支持 `__del__`；「标记实现为可中断的步骤函数」为增量改造留口。
- [09-c-api.md](../language/09-c-api.md) §3：显式根栈纪律——任何可能触发分配的两个 API 调用之间存活的局部 `MsObject*` 必须经 `msRootPush` 入根，LIFO 配对 `msRootPop`；函数实参自动是根。
- [10-c-style.md](../language/10-c-style.md)：§1 文件组织与 include guard、§2 格式化（2 空格缩进、120 列、K&R）、§3 命名、§4 typedef 规则（内部结构体不 typedef）、§5 错误处理（OOM 走统一失败路径）、§6 内存纪律（堆分配只经 `msAlloc`/`msRealloc`/`msFree`，C 局部对象遵守根栈纪律）。
- [11-project-layout.md](../language/11-project-layout.md) §1：`src/gc/` 的目录位置；§4：`tests/c/` 用自研 `ms_test.h`、`tests/ms/` 由 `run_tests.py` 驱动。
- 任务 09 已交付：`MsState`（全局命名空间、主协程、错误槽、分配统计）、`msCloseState` 的「遍历全对象链表全部回收」简化策略（本任务将其替换为 `msGcFreeAllObjects`）、临时内建 `assert`/`print`（脚本测试断言手段）。
- 任务 47/68 的既定对接面（其文档已按假定名引用本任务接口，本任务予以定名）：`struct MsGc`、`msGcAlloc`、`msGcCollect`、`msGcMarkRoots`、`msGcMarkObject` / `msGcMarkValue`、`msGcMarkCoroutine`。
- 任务 06（对象模型基础）与任务 08（VM 执行核心）的文档尚不存在，本文对其接口为假定命名：`MsTypeTag` 取值集合、对象头的具体字段名、`msObjTraverse` / `msObjFree` / `msObjSize` 分类型辅助函数、`struct MsCoroutine` / `struct MsCallFrame` 的内部字段、内建类型表与驻留缓存的容器形态，实现时以对应任务文档定名为准。任务 02 的 `msAlloc`/`msRealloc`/`msFree` 与分配统计、`ms_test.h` 同此约定。
- 与任务 18 假定的一处偏差：任务 18 文档把根栈字段（`roots`/`rootCount`/`rootCapacity`）假定在 `MsState` 上并声明以其对应任务文档定名为准；本任务将显式根栈收入 `struct MsGc`（GC 模块自包含、任务 47 改为每线程一根栈时无需改动 `MsState` 布局），任务 18 的公开 `msRootPush`/`msRootPop` 实现为对本文内部函数的薄封装。

## 详细设计

### 1. 文件与模块划分

- 头文件 `src/gc/ms_gc.h`，include guard `MSLANG_SRC_GC_MS_GC_H_`，自包含（自行 include `<stdbool.h>` `<stddef.h>` `<stdint.h>` 及对象模型、状态内部头）。模块前缀 `msGc`。
- 实现文件 `src/gc/ms_gc.c`；标记/清除的内部辅助一律文件内 `static`。
- GC 状态集中于 `struct MsGc`，作为字段嵌入 `struct MsState`（任务 09 的内部头中新增 `struct MsGc gc;`）；不引入全局变量（10-c-style §4）。
- 职责边界：GC 模块**不认识任何具体对象类型的内部布局**。子对象遍历、类型相关释放、对象尺寸三个操作由对象模块（任务 06/16）以分类型函数提供，GC 只调用：

```c
// Provided by the object module (tasks 06/16); assumed names.
void   msObjTraverse(struct MsGc* gc, MsObject* obj);   // msGcMarkObject on every child
void   msObjFree(MsState* L, MsObject* obj);            // frees payload and the object itself
size_t msObjSize(const MsObject* obj);                  // accounted bytes of the object
```

### 2. 颜色、常量与统计

两色方案：`MS_GC_WHITE`（未标记/待回收）与 `MS_GC_BLACK`（已标记）。标记栈中的对象均为黑色、尚未遍历子对象；出栈遍历后仍为黑色，由清除阶段统一复位为白色。不引入灰色——灰色是任务 68 增量三色标记的事，届时在同一枚举上扩展。

```c
typedef enum {
  MS_GC_WHITE = 0,
  MS_GC_BLACK = 1
} MsGcColor;

#define MS_GC_DEFAULT_THRESHOLD_BYTES (1024u * 1024u)  // 1 MiB，08-vm-internals §5
#define MS_GC_MIN_THRESHOLD_BYTES     MS_GC_DEFAULT_THRESHOLD_BYTES
#define MS_GC_GROW_PERCENT            100              // GOGC 语义：阈值 = 存活量 × (1 + 100%)
#define MS_GC_MARK_STEP_BUDGET        256              // msGcCollect 驱动步骤函数的默认预算（对象数）

struct MsGcStats {
  size_t   objectCount;      // 全对象链表当前长度
  size_t   allocBytes;       // 距上次收集的累计登记字节数
  size_t   thresholdBytes;   // 当前触发阈值
  size_t   lastLiveBytes;    // 上次清除后测得的存活字节数
  uint64_t collectCount;     // 累计完成的完整收集轮数
};
```

### 3. struct MsGc

```c
struct MsGc {
  MsObject*  allObjects;         // 全对象侵入式链表头（经 MsObjectHeader.gcNext）
  MsObject** markStack;          // 待遍历的黑色对象栈，动态数组
  size_t     markStackCount;
  size_t     markStackCapacity;
  MsObject** roots;              // 显式根栈（C API msRootPush 与 VM 内部共用），动态数组
  size_t     rootCount;
  size_t     rootCapacity;
  size_t     allocBytes;         // 触发计数：上次收集以来登记的字节数
  size_t     thresholdBytes;     // 越过即触发收集
  size_t   lastLiveBytes;        // 上次清除后的存活量，自适应依据
  int        disableCount;       // > 0 时抑制阈值触发的自动收集（可嵌套）
  uint64_t   collectCount;
};
```

`MsState` 内嵌该结构；`msNewState` 调 `msGcInit`，`msCloseState` 调 `msGcFreeAllObjects` 后调 `msGcDestroy`。

### 4. 模块接口

```c
// Initializes gc to the empty state with the default threshold.
void msGcInit(struct MsGc* gc);

// Releases the mark stack and root stack arrays. All heap objects must
// already be freed via msGcFreeAllObjects (MS_ASSERT allObjects == NULL).
void msGcDestroy(struct MsGc* gc);

// Frees every object on the all-objects list without marking. Used by
// msCloseState; replaces task 09's ad-hoc shutdown walk.
void msGcFreeAllObjects(MsState* L);

// Allocation hook called by every object constructor (task 06's msObjNew
// path) AFTER the raw msAlloc and BEFORE linking any reference to the
// newborn: when allocBytes has reached the threshold (and GC is not
// disabled) runs a full collection first — the newborn is not yet on the
// list, so it is trivially safe — then links obj into the all-objects
// list as MS_GC_WHITE and accounts sizeBytes.
// Returns MS_ERROR_OOM only if the triggered collection itself fails.
MsResult msGcAlloc(MsState* L, MsObject* obj, size_t sizeBytes);

// Accounts bytes of auxiliary buffers owned by objects (list/dict growth)
// so the trigger sees real memory pressure. Never triggers by itself;
// the next msGcAlloc observes the raised allocBytes.
void msGcAccountAlloc(struct MsGc* gc, size_t bytes);

// One full STW collection: mark roots, drain the mark stack via
// msGcMarkStep, sweep, adapt the threshold. Unconditional — disableCount
// only suppresses the trigger in msGcAlloc. Single-threaded v0.1: runs
// synchronously at the allocation point, no safepoint protocol (task 47).
MsResult msGcCollect(MsState* L);

// Pushes every root onto the mark stack (see §5).
MsResult msGcMarkRoots(MsState* L);

// Marks one coroutine's frames (proto, upvalues) and evaluation stack
// slots. v0.1 has only the main coroutine; the scheduler-wide
// enumeration is task 47's msGcMarkCoroutineRoots on top of this one.
MsResult msGcMarkCoroutine(MsState* L, struct MsCoroutine* co);

// Core marking primitive: NULL or non-white obj is a no-op; otherwise
// colors it black and pushes it onto the mark stack (msRealloc growth,
// MS_ERROR_OOM on failure).
MsResult msGcMarkObject(struct MsGc* gc, MsObject* obj);

// v0.1 values are all boxed MsObject*, so marking a value is marking an
// object; the wrapper exists so call sites survive the task-65 value
// representation change and matches the name task 47 already cites.
static inline MsResult msGcMarkValue(struct MsGc* gc, MsObject* value) {
  return msGcMarkObject(gc, value);
}

// Interruptible mark step: pops and traverses at most budget objects
// from the mark stack; sets *done when the stack drains. msGcCollect
// loops on it with MS_GC_MARK_STEP_BUDGET — the loop boundary is the
// seam where task 68 later interleaves the mutator (with write
// barriers). Never fails when budget > 0 progress is possible;
// MS_ERROR_OOM propagates from child marking.
MsResult msGcMarkStep(MsState* L, size_t budget, bool* done);

// Walks the all-objects list: frees white objects via msObjFree,
// resets black objects to white for the next cycle, measures
// lastLiveBytes, and adapts thresholdBytes (see §7). Cannot fail.
void msGcSweep(MsState* L);

// Explicit root stack, strict LIFO. NULL pushes are allowed as
// placeholders and skipped by marking. Pop on an empty stack is a
// programming error (MS_ASSERT). Backed by gc->roots (msRealloc growth,
// initial capacity 8); growth failure is OOM — the object would lose
// protection, so it takes the unified OOM failure path.
MsResult msGcRootPush(MsState* L, MsObject* obj);
void     msGcRootPop(MsState* L);

// Disable counter (nestable). Re-enabling to zero with allocBytes over
// the threshold runs one collection immediately.
void msGcDisable(MsState* L);
void msGcEnable(MsState* L);

// Diagnostics snapshot for tests and future tooling.
void msGcGetStats(const MsState* L, struct MsGcStats* out);
```

公开 C API（`msRootPush`/`msRootPop`/`msGCDisable`/`msGCEnable`/`msGCCollect`，任务 18）是上述内部函数的薄封装，签名与语义以 [09-c-api.md](../language/09-c-api.md) §3 为准，本任务不导出公共头。

### 5. 根集扫描（msGcMarkRoots）

按 [08-vm-internals.md](../language/08-vm-internals.md) §5 的根集合，v0.1 落为五类，顺序无关：

1. **协程栈**：`msGcMarkCoroutine` 遍历主协程——调用栈每一帧标记 `proto` 与 upvalue 数组中的对象，再标记求值栈 `[栈底, 栈顶)` 区间的每个槽位。v0.1 只有主协程（任务 09），但接口按单协程参数化，任务 47 在其上做全协程枚举。
2. **全局命名空间 / 模块注册表**：v0.1 模块系统（任务 24）未落地，任务 09 的全局命名空间表兼任模块注册表——表内全部字符串键与值对象逐条标记；任务 24 引入独立模块注册表后并入本入口，不另开扫描函数。
3. **内建类型表**：任务 06 挂在内建类型表中的类型对象逐条标记（类型对象若是静态存储则天然存活，标记为无操作）。
4. **C API 显式根栈**：`gc->roots[0, rootCount)` 逐条标记，跳过 `NULL` 占位。
5. **MsState 内部杂根**：错误槽当前错误对象、小整数驻留缓存（-256..4095）与驻留字符串表逐条标记（与任务 47 的「驻留缓存作为静态根」约定一致）；nil/true/false 单例如为堆对象同法处理。

### 6. 标记主流程（黑白两色 + 标记栈）

1. `msGcCollect` 入口先调 `msGcMarkRoots`，把全部根压入标记栈（根对象染黑）。
2. 循环调用 `msGcMarkStep(L, MS_GC_MARK_STEP_BUDGET, &done)` 直至 `done`：每步从标记栈弹出至多 `budget` 个对象，逐个调 `msObjTraverse`——后者对对象的每个子对象调 `msGcMarkObject`（白→黑并压栈）。字符串、int、float 等叶子类型的 `msObjTraverse` 为空操作。
3. 标记栈排空即标记完成。**两色不变量**：黑色 = 已标记（无论子对象是否已遍历，未遍历者必然仍在标记栈中）；白色 = 尚未从任何根到达。标记栈排空时不存在「黑色未遍历」的对象，因此清除可以只凭颜色判定生死。
4. 循环引用天然可回收：不可达的环上所有对象保持白色，清除阶段一并释放——这是标记-清除相对引用计数的本任务级收益，不引入额外机制。
5. 步骤函数边界即任务 68 的中断点：v0.1 在 STW 内一口气排空，任务 68 引入写屏障后允许在两次 `msGcMarkStep` 之间恢复 mutator。本任务不为步骤函数做除此以外的任何增量准备（不预留写屏障、不预留灰色）。

### 7. 清除与自适应阈值

`msGcSweep` 单遍遍历全对象链表（伪流程）：

```c
MsObject** p = &gc->allObjects;
size_t liveBytes = 0;
while (*p != NULL) {
  MsObject* obj = *p;
  if (obj->header.markColor == MS_GC_WHITE) {
    *p = obj->header.gcNext;   // 摘链
    msObjFree(L, obj);         // 分类型释放载荷与对象本体（msFree）
  } else {
    obj->header.markColor = MS_GC_WHITE;  // 复位，供下一轮
    liveBytes += msObjSize(obj);
    p = &obj->header.gcNext;
  }
}
gc->lastLiveBytes = liveBytes;
gc->allocBytes = 0;
gc->collectCount++;
```

阈值自适应（GOGC 思路，08-vm-internals §5「按存活率自适应」的具体化）：

```c
size_t target = gc->lastLiveBytes * (100 + MS_GC_GROW_PERCENT) / 100;
gc->thresholdBytes = target < MS_GC_MIN_THRESHOLD_BYTES ? MS_GC_MIN_THRESHOLD_BYTES : target;
```

- 存活率高（垃圾少）→ `lastLiveBytes` 大 → 阈值抬高，减少徒劳的全堆扫描；存活率低 → 阈值回落到下限 1MB，及时释放。
- 存活量为零时阈值回到默认值，避免空堆里阈值萎缩为 0 导致逐次分配触发。
- `msGcAccountAlloc` 登记的容器辅助缓冲计入 `allocBytes`（触发端看到真实内存压力），但不计入 `lastLiveBytes`（存活量以对象尺寸为准）；该近似在文档显式承认，精确化留待后续性能任务。

### 8. 触发路径与 OOM 交互

- 触发点只有一处：`msGcAlloc`。所有对象构造（任务 06 的 `msObjNew` 路径）必须经它登记，禁止绕开；VM 与编译器持有的 C 局部对象在跨越分配点时遵守根栈纪律（09-c-api §3，10-c-style §6 评审清单项）。
- `msGcAlloc` 先判触发、后入链：收集发生在新生对象入链之前，新生对象不在全对象链表也不在任何根上，收集对它无影响——这是「先收集后入链」顺序的存在理由，反之则要求新生对象默认染黑，徒增概念。
- `disableCount > 0` 时 `msGcAlloc` 只登记不触发；`msGcEnable` 减到 0 且 `allocBytes >= thresholdBytes` 时立即补一次 `msGcCollect`。
- OOM：标记栈与根栈扩容失败、或底层 `msAlloc` 失败，统一走 `MS_ERROR_OOM` 失败路径（10-c-style §5）；收集本身不分配除标记栈外的内存，清除绝不失败。
- GC **不移动对象**（非压缩式），`MsObject*` 指针跨收集稳定——这是 C API 句柄语义（09-c-api §3）的前提，文中所有设计不得引入移动。

### 9. 无终结器与关闭路径

- v0.1 不支持 `__del__`（08-vm-internals §5：「GC 语言终结器的坑不值得踩」）；`msObjFree` 只做确定性释放，回收顺序对脚本不可观测。C 扩展类型的 `finalize` 资源钩子（09-c-api §10）属任务 33 范围，与本任务的正交性届时评审（回收顺序未定义、`finalize` 内不得触碰其他脚本对象）。
- `msGcFreeAllObjects`：不标记、直接遍历全对象链表逐个 `msObjFree`，供 `msCloseState` 使用，替换任务 09 的临时实现；随后 `msGcDestroy` 释放标记栈与根栈数组。关闭后任务 02 的分配统计必须归零（含 GC 模块自身的两个动态数组）。

### 10. v0.1 的 STW 语义

v0.1 只有单线程（任务 09 的主协程、无调度器），收集在分配点同步发生，天然满足 STW：收集期间不存在其他执行线程，「标记/清除期间不发生任何对象分配」由单线程调用栈结构保证。08-vm-internals §5 描述的「全局标记 → 各工作线程 safepoint 停车」协议是任务 47 的改造内容，本任务只保证根集拆分（静态根 vs 协程根的扫描已分函数）与单协程标记的参数化，不提前实现任何线程设施。

## 实现步骤

1. 建 `src/gc/ms_gc.h` / `src/gc/ms_gc.c` 骨架：`MsGcColor` 枚举、阈值常量、`struct MsGc` / `struct MsGcStats`、`msGcInit` / `msGcDestroy`；`struct MsState` 嵌入 `struct MsGc gc` 并在 `msNewState` / `msCloseState` 挂接。验证：编译通过；C 单元测试断言初始化后链表为空、阈值为 1MB、计数为零。
2. 若任务 06/16 尚未提供，在对象模块补齐 `msObjTraverse` / `msObjFree` / `msObjSize` 三个分类型入口（switch on `MsTypeTag`）。验证：C 测试对每种已存在类型构造对象，遍历回调计数与子对象集合一致。
3. 实现 `msGcAlloc`（先触发后入链）与 `msGcAccountAlloc`：全对象链表登记、字节统计、对象构造路径统一改走 `msGcAlloc`；本轮暂以「阈值永不越过」运行（`msGcCollect` 尚为空壳）。验证：C 测试分配 N 个对象后链表长度与 `allocBytes` 正确；任务 09 全部既有测试保持绿色。
4. 实现标记：`msGcMarkObject` / `msGcMarkValue`、标记栈动态数组（`msRealloc` 倍增、OOM 路径）、`msGcMarkStep` 步骤函数。验证：C 测试手工构造对象图，以 `budget = 1` 逐步排空，断言每步处理数与最终 `done`。
5. 实现根集扫描：`msGcMarkCoroutine`（帧 proto/upvalue + 求值栈区间）与 `msGcMarkRoots` 五类根（全局命名空间、内建类型表、显式根栈、错误槽、驻留缓存）。验证：C 测试构造「仅被根引用」与「无引用」两类对象，标记后断言颜色。
6. 实现 `msGcSweep` 与 `msGcCollect` 全流程（标记 → 排空 → 清除 → 阈值自适应）。验证：C 测试中断言垃圾对象（含循环引用）被回收、存活对象内容完好、`lastLiveBytes` 与阈值调整符合 §7 公式。
7. 实现触发与禁用：`msGcAlloc` 的阈值判定、`msGcDisable` / `msGcEnable` 计数与补触发。验证：C 测试把阈值调小后分配自动触发；disable 期间越阈不触发，enable 归零时补触发。
8. 实现显式根栈 `msGcRootPush` / `msGcRootPop`（动态数组、LIFO、NULL 占位、空弹断言、扩容 OOM 路径）。验证：C 测试入根对象跨 `msGcCollect` 存活；乱序配对在 Debug 下触发 `MS_ASSERT`。
9. `msCloseState` 改走 `msGcFreeAllObjects` + `msGcDestroy`，实现 `msGcGetStats`。验证：`msCloseState` 后任务 02 分配统计归零（含 GC 自身数组）。
10. 接入 ms 脚本压力测试与 `run_tests.py`（见测试方案），全平台（Win/Linux/macOS）× Debug/Release 构建，Debug（ASAN / `/RTC`）下无内存错误与泄漏报告。

## 测试方案

本任务晚于任务 09：语言级测试一律用 ms 脚本（`tests/ms/`，`testing` 模块任务 40 才存在，故用内建 `assert` + `print`）；GC 内部机制（颜色、链表、阈值、步骤函数）脚本不可观测，用 C 单元测试覆盖（[11-project-layout.md](../language/11-project-layout.md) §4 第一层，`ms_test.h` / `MS_TEST` / `MS_ASSERT_EQ`）。两层结合：C 层证明算法正确，脚本层证明真实分配压力下解释器行为正确。本任务只交付设计文档，测试实体随实现步骤编写。

### 1. C 单元测试（tests/c/test_gc.c）

- 生命周期：`msGcInit` 后字段默认；空状态 `msGcCollect` 是无操作；`msGcDestroy` 前未清空链表触发 `MS_ASSERT`。
- 登记与统计：分配 N 个对象后 `objectCount == N`、`allocBytes` 累计正确；`msGcAccountAlloc` 抬升 `allocBytes` 但不触发。
- 标记：手工图（list 套 list、字符串叶）逐层染黑；`budget = 1` 的步骤函数每步至多处理一个对象；重复标记同一对象是幂等无操作；标记 `NULL` 安全。
- 根集：仅被全局变量、协程求值栈槽、根栈、错误槽、驻留缓存引用的对象在收集后全部存活；无任何引用的对象被回收。
- 循环垃圾：两个 list 互相持有、对外无引用，收集后两者皆释放（任务 02 分配统计前后对比）。
- 阈值触发与自适应：阈值调小后分配自动触发一次收集（`collectCount` +1）；高存活量场景收集后 `thresholdBytes == lastLiveBytes * 2`；空堆收集后阈值回落 1MB。
- 禁用计数：嵌套 disable/enable；disable 期间越阈不触发；enable 归零且越阈时立即补一次；`msGcCollect` 手动调用不受禁用抑制。
- 根栈：LIFO 次序、NULL 占位跳过、容量倍增、空弹触发 `MS_ASSERT`（Debug）。
- 关闭路径：`msGcFreeAllObjects` 后分配统计归零。

### 2. ms 脚本测试（tests/ms/gc/，run_tests.py 驱动）

脚本无法强制 GC 也无法观测对象存亡，测试策略是**制造远超 1MB 初始阈值的分配压力并断言结果正确**——若存活对象被误收，断言即失败或进程崩溃：

- `alloc_pressure.ms`：大循环构造临时字符串与 list（累计分配量远超阈值，保证触发多次收集），维护校验和，末尾 `assert` 校验和并 `print("gc alloc pressure ok")`。
- `live_set.ms`：在全局 list 中累积 N 个对象的同时制造等量垃圾，循环结束后逐一 `assert` 存活对象内容——验证全局命名空间根与求值栈根的扫描正确性。
- `cycles.ms`：构造互相引用的 list 环后解除引用，再跑分配压力，断言后续计算结果正确——覆盖循环垃圾回收路径的脚本侧行为。
- `call_frames.ms`：递归函数每层持有大对象实参（实参自动是根）并叠加分配压力，断言递归返回结果——验证调用栈帧（proto/upvalue/求值栈）根的扫描。

负向验证在 C 层完成（脚本层无可观测失败通道）：Debug + ASAN（MSVC 用 `/RTC`）下运行全部脚本，无悬垂引用、无重复释放、无泄漏。

### 3. 回归

任务 09–16 的全部既有测试（`tests/ms/` 与 `tests/c/`）在本任务落地后必须保持绿色——GC 接入不得改变任何语言级可观测行为。

## 验收标准

- [ ] `src/gc/ms_gc.h` / `ms_gc.c` 存在，guard 为 `MSLANG_SRC_GC_MS_GC_H_`，头文件自包含；代码风格通过 [10-c-style.md](../language/10-c-style.md) 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsGc` / `struct MsGcStats` 不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] 对象头 `markColor`（`MS_GC_WHITE` / `MS_GC_BLACK` 两色）与 `gcNext` 全对象链表按本文 §2/§3 使用；所有对象构造统一经 `msGcAlloc` 登记，无旁路。
- [ ] 标记为「两色 + 显式标记栈」非递归实现；`msGcMarkStep` 按预算推进、`msGcCollect` 以其循环驱动，步骤边界清晰可供任务 68 插入 mutator。
- [ ] 根集覆盖本文 §5 全部五类：协程调用栈/求值栈、全局命名空间（兼任模块注册表）、内建类型表、显式根栈、错误槽与驻留缓存；循环引用垃圾可回收。
- [ ] 触发为分配计数越阈（初始 1MB），收集后按 §7 公式依存活量自适应（下限 1MB）；`msGcDisable` / `msGcEnable` 计数语义正确，`msGcCollect` 不受禁用抑制。
- [ ] 无终结器：`msObjFree` 确定性释放，脚本无可观测回收顺序；GC 不移动对象。
- [ ] `msGcRootPush` / `msGcRootPop` 严格 LIFO、允许 NULL 占位、扩容失败走统一 OOM 路径；与任务 18 的公开 `msRootPush` / `msRootPop` 语义对接。
- [ ] `msCloseState` 经 `msGcFreeAllObjects` + `msGcDestroy` 释放全部对象与 GC 自身数组，任务 02 分配统计归零。
- [ ] `tests/c/test_gc.c` 覆盖「测试方案」第 1 节全部清单项，`tests/ms/gc/` 四个脚本经 `python run_tests.py` 通过，任务 09–16 既有测试全数回归通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug（ASAN / `/RTC`）下全部 GC 测试无内存错误与泄漏；无 TBD/TODO 占位；与任务 06/08 的接口假定（`msObjTraverse` / `msObjFree` / `msObjSize`、`struct MsCoroutine` 字段等）在实现时已对齐。
