# 14 闭包与 upvalue

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [13 函数与调用](13-functions-calls.md) |

## 任务目标

交付 mslang 的闭包机制：词法作用域下嵌套函数对外层局部变量的**按引用捕获**（03-syntax §8：无 `nonlocal`，内层可直接写捕获变量）、upvalue 的创建/共享/关闭语义，以及配套的编译器解析扩展与 VM 运行时支持。具体产物：

- `src/vm/ms_upvalue.{h,c}`：upvalue 对象（`struct MsUpvalue`）、每协程 open upvalue 有序链表、捕获（`msUpvalueCapture`）、关闭（`msUpvalueClose`）、引用计数（`msUpvalueRef`/`msUpvalueUnref`）与栈重定位修复（`msUpvalueFixStack`）。
- 编译器扩展（在任务 07 的编译器内落地）：作用域解析阶段的捕获标记（逃逸分析）、嵌套函数的 upvalue 描述符（`struct MsUpvalueDesc`）写入子 Proto、作用域退出点 `MS_OP_CLOSE_UPVALS` 的发射（含 `break`/`continue` 等非顺序出口）。
- VM 扩展（在任务 08 的 `src/vm/ms_vm.c` 落地）：`MS_OP_LOAD_UPVAL` / `MS_OP_STORE_UPVAL` / `MS_OP_CLOSE_UPVALS` 的执行语义，`MS_OP_MAKE_FUNCTION` / `MS_OP_MAKE_LAMBDA` 的 upvalue 装配，`MS_OP_RETURN` / `MS_OP_TAIL_CALL` / 错误恢复帧丢弃 / 协程销毁路径上的隐式关闭。

完成后，以下全部成立：内层函数直接读写外层局部变量；多个闭包共享同一捕获变量；闭包逃逸（外层帧已返回）后捕获变量继续存活（值从栈槽上提到 upvalue 自有存储）；闭包语义对 `func` 声明与 `lambda` 一致。本任务经 `tests/ms/closures/` 下的 ms 脚本验证。

## 设计依据

- `docs/language/03-syntax.md`
  - §4：函数是一等值，支持闭包（词法作用域，upvalue 捕获）；`lambda` 为单表达式函数体。
  - §8：块级作用域；函数内赋值默认为局部变量；闭包对外层局部变量按**引用**捕获，无 `nonlocal`，内层可直接写捕获变量（刻意简化，与 Python 不同）；名字解析顺序：局部 → 闭包外层 → 模块全局 → 内建。
- `docs/language/08-vm-internals.md`
  - §1：嵌套函数是外层 Proto 常量池中的子 Proto；编译器负责作用域解析与闭包转换。
  - §2.2：`MS_OP_LOAD_UPVAL` / `MS_OP_STORE_UPVAL` / `MS_OP_CLOSE_UPVALS` / `MS_OP_MAKE_FUNCTION` / `MS_OP_MAKE_LAMBDA` 指令清单。
  - §4：`MsCallFrame` 含 upvalue 数组字段。
- `docs/language/10-c-style.md`：§1 include guard、§2 格式化、§3 命名、§4 typedef 规则（内部结构体不 typedef）、§5 错误处理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）、§8 断言。
- `docs/language/11-project-layout.md`：`src/vm/` 目录位置；`tests/ms/` 脚本测试约定（任务 ≥ 09 用 ms 脚本，任务 40 之前用内建 `assert` + `print`，由仓库根 `run_tests.py` 驱动）。
- 已定稿的上游任务文档，本文直接引用其定名：
  - [05 字节码格式与 MsProto](05-bytecode-proto.md)：`CLOSE_UPVALS` 操作数约定（`A` = 槽位，关闭槽 ≥ `A` 的 upvalue）；`MAKE_FUNCTION`/`MAKE_LAMBDA` 的 `Bx` = 子 Proto 常量池下标。
  - [06 对象模型基础](06-object-model.md)：对象统一分配入口 `msObjectAlloc`、`MsTypeTag` 20 标签冻结（`_Static_assert` 守序）、`msHeapDestroy` 的全对象链表回收。
  - [08 VM 执行核心](08-vm-core.md)：`struct MsCallFrame`（`proto`/`pc`/`baseIndex`/`upvalues`/`upvalueCount`，upvalue 数组为借用）、`struct MsCoroutine`（`frames`/`stack`/`stackTop` 等）、`msVmPushFrame`、求值栈 `msRealloc` 倍增扩容与重定位纪律、`msVmRun` 错误后丢弃帧并恢复 `frameCount`/`stackTop` 的约定。
  - [13 函数与调用](13-functions-calls.md)：`struct MsFunction`（含 `upvalues`/`upvaluesLen` 字段，本任务填充）、`msNewFunction`、`msFunctionAttachDefaults` 的装配模式、`MAKE_FUNCTION` 的默认值弹栈布局、`TAIL_CALL` 的「回收前补 `CLOSE_UPVALS`」预留挂接点、`MsProto` 将增 `upvalueCount` 的字段名预留。

对规范与上游文档的显式处理（实现与评审时以此为据）：

1. **「按引用捕获、无 nonlocal」的实现含义**：捕获变量在内层函数中与普通局部变量地位等同（可任意读写），读写统一经 upvalue 间接寻址；不存在「内层赋值隐式新建局部」的 Python 式陷阱。这条语义由「upvalue 直接指向外层栈槽/关闭存储」自然满足，编译器无需任何特殊判定。
2. **`MsProto` 增补 upvalue 描述符数组**：追加 `upvalueDescs`/`upvalueCount` 两个字段（字段名沿用任务 13 的预留），属对任务 05 结构的扩展，实现时同步任务 05 文档。
3. **`MAKE_FUNCTION` 的 upvalue 装配不经求值栈传递**：任务 05 对该指令的注释「upvalue 自栈弹出」与任务 13 的「先弹 upvalue」预留条款由本任务取代——改为按子 Proto 的 `upvalueDescs` 逐条装配（Lua 式描述符方案）。理由：装配数据全部在 Proto 内静态确定，栈传递会把捕获时机与共享语义复杂化。任务 13 阶段 `upvalueCount` 恒 0，此改动对其行为与测试零回归；实现时回改任务 05/13 文档对应行。
4. **循环变量的捕获语义**：循环变量在一次循环执行中是**单一绑定**，所有迭代中创建的闭包共享同一变量（与 Python 一致，与 Go 1.22+ 的逐迭代绑定不同）；v0.1 不做逐迭代新鲜绑定，文档明示，逐迭代绑定列入路线图候选。块内 `:=` 声明的局部随每次迭代重新进入作用域，其捕获随块退出逐次关闭，各迭代相互独立（见「编译器扩展」）。
5. **`struct MsUpvalue` 不是 `MsObject`**：任务 06 已把 `MsTypeTag` 冻结为规范的 20 个标签（含 `_Static_assert`），为内部对象新增标签会破坏该约定；且同一 upvalue 可被多个函数对象共享（getter/setter 对、`inStack=0` 转发），单一所有者模型不成立。故 `MsUpvalue` 为纯内部堆结构（无 GC 头），经 `msAlloc`/`msFree` 管理，**引用计数**归函数对象的 `upvalues[]` 槽位持有；任务 17（GC）落地时结构不变，GC 只需在标记函数对象/帧时把各 upvalue 持有的值（`*location`）纳入遍历，upvalue 结构体的释放仍走引用计数（函数对象销毁路径逐槽 unref）。值层面的环（函数 → upvalue → 容器 → 函数）由 GC 处理值对象，不经过 upvalue 结构体的引用计数，无环泄漏。
6. **open upvalue 的 `location` 是任务 08「不得缓存栈指针」纪律的唯一例外**：求值栈经 `msRealloc` 扩容会搬移底层数组，open upvalue 持有的栈槽指针随之内悬。修复方案为栈增长路径在成功扩容后立即调用 `msUpvalueFixStack` 平移全部 open `location`（整体偏移相同，链表有序性保持）；该调用是对任务 08 `ms_vm.c` 增长路径的唯一改动点，见「详细设计」。
7. 任务 07（编译器）文档尚不存在：本文对编译器侧接口（作用域栈、符号表条目、`isCaptured` 标记位、发射助手）均为假定命名，**实现时以对应任务文档定名为准**。

## 详细设计

### 方案选型：open upvalue 有序链表（Lua 方案）

候选方案为「open upvalue 链表」（Lua 5.x）与「每帧 upvalue 数组 + 惰性复制」。选择 Lua 方案：

- 捕获变量在外层函数存活期间始终驻留栈槽，读写除一次指针间接外零拷贝；只有外层帧退出（或块作用域结束）时才把值「关闭」到 upvalue 自身的存储。
- 同一槽位被多个闭包捕获时天然共享同一个 `MsUpvalue`，按引用语义自动成立。
- 每协程只需维护一条按栈地址排序的链表；栈式 VM 槽位地址单调可比较，查找/插入为链表顺序操作，捕获次数有限时开销可忽略。

「数组方案」（每帧记录被捕获槽位表、退出时统一复制）不共享 open 期间的写：两个闭包在外层存活期间各持副本，违反按引用语义，需额外间接层修补，故弃用。

### 对象与结构体

```c
// src/vm/ms_upvalue.h — guard MSLANG_SRC_VM_MS_UPVALUE_H_

struct MsUpvalue {
  struct MsObject** location;    // open: points to the captured eval-stack slot;
                                 // closed: points to &closed
  struct MsObject* closed;       // storage the value is copied into on close
  struct MsUpvalue* next;        // per-coroutine open list link (NULL when closed)
  int refcount;                  // MsFunction.upvalues[] slots referencing this entry
};

struct MsUpvalueDesc {           // one entry per variable the proto captures
  uint8_t  inStack;              // 1: capture enclosing frame's local slot `index`
                                 // 0: share enclosing function's upvalue `index`
  uint16_t index;                // local slot index, or parent's upvalue index
};
```

- 不变量：`location` 永远指向「当前值的存放处」。open 状态 `location` 指向求值栈槽位、`closed` 约定置 `NULL`；close 时把 `*location` 拷入 `closed`、再令 `location = &uv->closed`、`next = NULL`。此后读写不必区分两种状态。
- `MsUpvalue` 不是 `MsObject`（设计依据第 5 条）：无 GC 头、不进全对象链表，生命周期由引用计数管理。

对既有结构的扩展：

```c
// struct MsProto 追加（对任务 05 的补充，字段名沿用任务 13 的预留）：
//   struct MsUpvalueDesc* upvalueDescs;   // capture descriptors, msAlloc'd
//   int upvalueCount;                     // == length of upvalueDescs

// struct MsFunction（任务 13 已定）：
//   struct MsUpvalue** upvalues;          // resolved array, length upvaluesLen
//   int upvaluesLen;                      // == proto->upvalueCount after assembly

// struct MsCoroutine（任务 08）追加：
//   struct MsUpvalue* openUpvalues;       // open list head, sorted by location descending
```

- `inStack = 1`：直接捕获外层帧的局部变量，upvalue 的 `location` 指向外层帧的栈槽。
- `inStack = 0`：外层函数自身也是从更外层捕获的（多层嵌套），此时直接共享外层函数对象的 `upvalues[index]`——同一个 `MsUpvalue` 引用计数加一，不新建。这条规则使任意深度嵌套的捕获链收敛为单层间接。
- 描述符由编译器在编译嵌套函数时生成，顺序即函数对象 `upvalues[]` 的下标顺序（`LOAD_UPVAL A` / `STORE_UPVAL A` 的 `A` 即该下标）。
- 帧的 `upvalues`/`upvalueCount` 字段沿用任务 08 的借用约定：压帧时从被调函数对象复制指针与计数，VM 不释放。
- open 链表按 `location`（栈地址）**降序**排列：关闭操作「关闭 ≥ 某槽位的全部 upvalue」即从表头顺序摘取直到 `location < level`，与 Lua 同构。

### 公开接口（src/vm/ms_upvalue.h）

```c
// Finds or creates an open upvalue over eval-stack slot `slot` of coroutine
// co. Reuses the existing entry when one already points at `slot`, so all
// closures capturing the same variable share one MsUpvalue. The result
// carries one reference owned by the caller's upvalues[] slot. Returns NULL
// on OOM (caller maps to MS_ERROR_OOM). L is reserved for the task 17 GC
// allocation context.
struct MsUpvalue* msUpvalueCapture(MsState* L, struct MsCoroutine* co, struct MsObject** slot);

// Closes every open upvalue of co whose location >= level: copies the value
// into the upvalue's own storage, redirects location, unlinks from the open
// list. Called on block-scope exit (CLOSE_UPVALS), frame return, frame
// discard on error, and coroutine teardown. A no-op when nothing qualifies.
void msUpvalueClose(struct MsCoroutine* co, struct MsObject** level);

// Takes one reference on uv and returns it (inStack=0 re-capture shares the
// parent's upvalue).
struct MsUpvalue* msUpvalueRef(struct MsUpvalue* uv);

// Drops one reference; frees the struct with msFree at zero. The upvalue must
// be closed by then (MS_ASSERT in debug builds).
void msUpvalueUnref(struct MsUpvalue* uv);

// Repairs open upvalue locations after the eval stack is relocated by
// msRealloc: every location shifts by (co->stack - oldStack), order preserved.
// Called from the stack growth path in ms_vm.c right after a successful
// reallocation.
void msUpvalueFixStack(struct MsCoroutine* co, struct MsObject** oldStack);
```

实现要点：

- `msUpvalueCapture` 沿 open 链表按降序查找 `slot`：命中则 `refcount` 加一并返回；越过（遇到 `location < slot`）则在当前位置插入新节点（`refcount = 1`）。新节点经 `msAlloc` 分配，OOM 返回 `NULL`。
- `msUpvalueClose` 从表头循环：`uv->closed = *uv->location`、`uv->location = &uv->closed`、摘下表头。`level` 取「将被销毁的第一个槽位」的地址。幂等：无合格节点时为空操作。
- `msUpvalueUnref` 归零时 `MS_ASSERT(uv->location == &uv->closed)`（open upvalue 必被 ≥1 个存活帧的函数引用，不会在 open 状态归零），随后 `msFree`。
- `msUpvalueFixStack` 全表遍历，逐节点 `uv->location = co->stack + (uv->location - oldStack)`；调用点保证链表中只有 open 节点（closed 节点已摘链）。
- 本模块在任务 17 之前不触发任何 GC；`closed` 指向的值对象归对象模型管（任务 17 将其纳入标记遍历，见「生命周期」）。

另在任务 13 的 `src/vm/ms_function.{h,c}` 追加一个装配助手（镜像 `msFunctionAttachDefaults` 的既有模式）：

```c
// Hands ownership of the assembled upvalue array to fn (task 14). Called
// only by the MAKE_FUNCTION/MAKE_LAMBDA handlers.
void msFunctionAttachUpvalues(struct MsFunction* fn, struct MsUpvalue** upvalues, int upvaluesLen);
```

### 指令语义（VM 侧扩展）

记号：当前帧 `frame`（`co->frames` 栈顶），槽位地址 `slotAddr(i) = co->stack + frame->baseIndex + i`。

| 指令 | 语义 |
|---|---|
| `LOAD_UPVAL A` | 压栈 `*frame->upvalues[A]->location` |
| `STORE_UPVAL A` | 弹栈写入 `*frame->upvalues[A]->location` |
| `CLOSE_UPVALS A` | `msUpvalueClose(co, slotAddr(A))`——关闭本帧槽位 ≥ A 的全部 open upvalue |
| `MAKE_FUNCTION Bx` / `MAKE_LAMBDA Bx` | 先按任务 13 弹出 `defaultCount` 个默认值；再按子 Proto 的 `upvalueDescs` 逐条装配：`inStack=1` → `msUpvalueCapture(L, co, slotAddr(index))`；`inStack=0` → `msUpvalueRef(frame->upvalues[index])`（共享）。装配数组经 `msFunctionAttachUpvalues` 写入新函数对象，函数对象压栈 |
| `RETURN` | 帧拆除前先 `msUpvalueClose(co, slotAddr(0))`，保证本帧所有捕获变量安全上提；随后走任务 13 既定的返回流程 |
| `TAIL_CALL` | 复用当前帧前先 `msUpvalueClose(co, slotAddr(0))`（任务 13 预留的挂接点），再按任务 13 搬移绑定结果 |

要点：

- 读写语义完全由 `location` 间接给出，VM 分支不区分 open/closed。
- `MAKE_FUNCTION` 的装配发生在创建闭包的运行时，外层帧即执行该指令的当前帧（嵌套函数的 `func`/`lambda` 字面量在外层函数体内求值）。装配中途 OOM：已捕获/已引用的条目逐条 unref、释放装配数组，按运行时错误传播（任务 13 的错误路径）。
- upvalue 不再经求值栈传递（设计依据第 3 条）：`MAKE_FUNCTION` 的弹栈数量回到任务 13 的 `defaultCount` 一个。
- 顶层模块 Proto 无外层函数，`upvalueCount` 恒为 0；模块顶层变量是全局命名空间成员（任务 09/11 约定），不参与 upvalue 机制。
- `inStack=0` 分支不做 `msUpvalueCapture`：外层函数对象的 upvalue 可能已关闭（外层帧已返回），其 `location` 指向 `closed` 存储，直接共享指针即可。
- **帧丢弃路径**（任务 08 约定：`msVmRun` 出错后丢弃 `stopAt` 以上的帧）：逐帧先 `msUpvalueClose(co, co->stack + f->baseIndex)` 再丢弃，保证错误恢复后无悬挂 `location`。
- **协程销毁**（`msCoroutineDestroy`）：对全部存活帧逐帧执行同上关闭，再释放栈数组。
- **压帧挂接**：`msVmPushFrame` 与调用路径（任务 08/13）压入 `MS_TYPE_FUNCTION` 帧时，把函数对象的 `upvalues`/`upvaluesLen` 复制进帧的借用字段（任务 08 目前恒 `NULL/0`，本任务接通）。
- **栈增长挂接**：任务 08 的扩容路径在 `msRealloc` 成功后调用 `msUpvalueFixStack(co, oldStack)`（设计依据第 6 条）。

### 编译器扩展（闭包转换与逃逸分析）

在任务 07 的作用域解析基础上做三处扩展（接口名为假定，实现时对齐）：

1. **逃逸分析（编译期、纯词法）**。解析嵌套函数体内的标识符时按「局部 → 闭包外层 → 全局 → 内建」顺序解析；当名字命中**外层某一函数作用域的局部变量**（含外层函数的参数，参数同样是栈槽）时：
   - 在该局部变量的符号表条目上置 `isCaptured = true`。这就是本语言的全部「逃逸分析」：词法可达即逃逸，无数据流分析，保守但精确——只有被嵌套函数引用的局部才付出 upvalue 代价；
   - 沿嵌套链逐层传递：若该变量对中间层函数不是局部，则在中间层 Proto 记一条 `inStack=0` 的转发描述符并继续向外解析，直到命中定义层（记 `inStack=1`）。每层函数 Proto 得到的描述符序列即 `upvalueDescs`；
   - 每一层的 upvalue 下标按首次引用顺序分配、按变量去重（同一外层变量多次引用只占一个下标）；
   - 内层函数体内对被捕获名字的读生成 `LOAD_UPVAL`、写（`=`/复合赋值）生成 `STORE_UPVAL`。任务 13 的「嵌套函数引用外层局部报编译错误」检查在本任务解除。
2. **捕获槽位的关闭点发射**。维护当前函数内「已捕获槽位」集合，在以下位置发射 `CLOSE_UPVALS A`（`A` = 将被销毁的最低捕获槽位）：
   - 退出含捕获变量的块作用域时（`if`/`for`/`while`/裸块的 `}` 处）；这使块内 `:=` 局部在每次迭代末尾被关闭，各迭代的闭包捕获相互独立（设计依据第 4 条）；
   - `break`/`continue` 等跳出含捕获变量的块之前（逐层退栈路径上逐块发射）；
   - 函数返回前统一由 `RETURN`/`TAIL_CALL` 的隐式关闭兜底（覆盖参数与最外层块槽位），显式发射只针对块作用域提前退出的情形；
   - 无捕获变量的块不发射任何额外指令——非逃逸路径零开销。
3. **子 Proto 元数据写入**。编译 `func`/`lambda` 声明时把描述符数组挂到子 Proto 的 `upvalueDescs`/`upvalueCount`，子 Proto 照常入常量池并发射 `MAKE_FUNCTION`/`MAKE_LAMBDA`（`Bx` = 常量池下标，沿用任务 05/13）。

名字解析的优先级不变（03-syntax §8）：局部命中后不查 upvalue；upvalue 命中后不查全局；`global` 声明的名字直接走全局段，不产生捕获。

### 生命周期与内存纪律

- open `MsUpvalue`：由 open 链表链接（非 owning），同时被 ≥1 个函数对象的 `upvalues[]` 槽位引用（owning，计入 `refcount`）。
- close 后：链表引用移除，函数对象的槽位引用成为唯一存续依据；函数对象销毁路径（v0.1 为任务 09 的 `msCloseState` 全量回收，任务 17 起为 GC sweep）遍历 `upvalues[]` 逐槽 `msUpvalueUnref`，归零即 `msFree` 结构体本体。
- 释放顺序保证：`msCloseState` 按任务 09 的逆序销毁——先销毁协程（`msCoroutineDestroy` 关闭全部 open upvalue，链表清空），再回收堆对象（函数对象 unref，此时 `MS_ASSERT` 已关闭成立）。
- 值层面：open 期间值即栈槽内容，随栈扫描被覆盖；close 后值存于 `closed`，任务 17 标记函数对象/帧的 upvalue 数组时对每个 `MsUpvalue` 标记 `*location`。upvalue 结构体本身永远不参与 GC（设计依据第 5 条）。
- 协程销毁与 `msVmRun` 错误恢复路径的兜底关闭（见「指令语义」）保证任何退出路径都不留悬挂 `location` 指向已销毁栈区。

## 实现步骤

1. 建 `src/vm/ms_upvalue.h` / `ms_upvalue.c` 骨架：`struct MsUpvalue`、`struct MsUpvalueDesc`、五个公开函数签名；`MsCoroutine` 增加 `openUpvalues` 字段（`msCoroutineInit` 零初始化随之覆盖）；`MsProto` 追加 `upvalueDescs`/`upvalueCount` 并接通构建器与释放路径；`ms_function` 模块增加 `msFunctionAttachUpvalues`。验证：头文件自包含编译通过；任务 05/08/13 既有测试无回归。
2. 实现 `msUpvalueCapture` / `msUpvalueRef` / `msUpvalueUnref`：降序链表查找/插入、同槽位复用、引用计数归零释放、OOM 路径。验证（随第 5 步脚本观察行为）：同一槽位两次捕获返回同一指针。
3. 实现 `msUpvalueClose` / `msUpvalueFixStack`：拷贝、`location` 重定向、链表摘除、空操作幂等、扩容平移；在任务 08 的栈增长路径挂接 `msUpvalueFixStack`。验证：深表达式触发栈扩容后闭包读写仍正确（脚本层构造，见测试方案 `stack_growth.ms`）。
4. 编译器扩展：作用域解析的 `isCaptured` 标记与逐层描述符传递、`upvalueDescs`/`upvalueCount` 写入子 Proto、被捕获名字的 `LOAD_UPVAL`/`STORE_UPVAL` 生成；解除任务 13 的外层局部引用拒绝。验证：含嵌套函数源码的编译产物描述符内容（Proto 转储扩展打印）；任务 13 的 `err_outer_local.ms` 负例转为正例。
5. VM 指令落地：`LOAD_UPVAL`/`STORE_UPVAL`/`CLOSE_UPVALS` 三个 case、`MAKE_FUNCTION`/`MAKE_LAMBDA` 装配分支（含 OOM 中途回收）、压帧挂接 upvalue 数组、`RETURN`/`TAIL_CALL` 隐式关闭、帧丢弃与协程销毁的兜底关闭。验证：`tests/ms/closures/` 基础脚本（read/write/counter）通过。
6. 关闭点发射：块退出、`break`/`continue` 路径的 `CLOSE_UPVALS`。验证：`loop_capture.ms`、`block_scope.ms`、`break_continue.ms` 通过；无捕获函数的指令流与任务 13 产物逐条相同（零回归）。
7. 边界与错误路径：多层嵌套转发（`inStack=0`）、错误恢复帧丢弃的关闭、`global` 与捕获名的区分。验证：`nested_chain.ms`、`err_capture_abort.ms`、`shadowing.ms` 通过；`msCloseState` 后分配统计归零。
8. 全量回归：`python run_tests.py` 全绿、`ctest --test-dir build` 通过、Debug 构建（ASAN / `/RTC`）无内存错误。

## 测试方案

本任务晚于最小可运行解释器（任务 09），一律用 ms 脚本测试，由仓库根 `run_tests.py` 驱动 mslang CLI；`testing` 模块（任务 40）之前用内建 `assert` + `print` 自断言。正向脚本成功路径以 `print("<name> ok")` 收尾、退出码 0；负向用例以 `<name>.exit` 同伴文件声明预期退出码（设施约定见任务 09）。注意容器（list/dict）属任务 16、晚于本任务，测试脚本**不得使用** list/dict 字面量与方法，需要保存多个闭包的场景用具名变量逐个持有。测试目录 `tests/ms/closures/`（本任务只交付本设计文档，脚本随实现编写）：

- `read_capture.ms`：内层函数只读捕获外层局部；多次调用读到外层变量的当前值（先读后改再读）。
- `write_capture.ms`：内层函数**写**捕获变量（无 `nonlocal` 声明），外层随后读到新值——验证按引用捕获语义。
- `counter.ms`：经典计数器工厂 `func makeCounter() { n := 0; return func() { n += 1; return n } }`；两个实例互不影响（各自的 `n`），同一实例递增连续——验证每次调用产生独立的捕获环境。
- `shared_capture.ms`：同一外层函数返回两个闭包（getter/setter 对），共享同一捕获变量——验证 `msUpvalueCapture` 的同槽位复用。
- `escape.ms`：外层函数返回闭包后自身帧已销毁，闭包仍读到正确值；含尾调用形态（`return makeInner()`）下外层捕获变量在 `TAIL_CALL` 帧复用前正确关闭——验证 close 时值上提。
- `loop_capture.ms`：三层式 `for` 循环体内创建闭包、用具名变量逐迭代持有（如 `if i == 0 { f0 = ... } else { f1 = ... }`），循环结束后逐个调用：所有闭包共享循环变量的最终值（设计依据第 4 条的既定语义）；循环体内块级局部（迭代内 `:=` 新变量）被捕获的情形逐次独立——验证块退出关闭点。
- `block_scope.ms`：`if`/裸块内声明的局部被闭包捕获，块结束后调用闭包值正确——验证块作用域退出的 `CLOSE_UPVALS`。
- `break_continue.ms`：含捕获变量的循环中 `break`/`continue` 跳转，跳转路径上的关闭语义正确、循环外调用已捕获闭包不崩溃且值正确。
- `nested_chain.ms`：三层嵌套 `func a() { x := 1; return func() { return func() { x += 1; return x } } }`，最内层经两级转发读写 `x`——验证 `inStack=0` 描述符链。
- `lambda_capture.ms`：`lambda` 捕获外层变量（读与写）与 `func` 行为一致。
- `shadowing.ms`：内层同名局部遮蔽捕获变量；捕获变量与全局同名时，无 `global` 声明的赋值走捕获、有 `global` 声明的写全局——验证解析顺序局部 → 外层 → 全局。
- `stack_growth.ms`：被捕获的槽位位于求值栈深处（外层函数先声明大量局部再捕获其一），闭包创建后触发深层表达式求值迫使栈扩容，扩容前后闭包读写一致——验证 `msUpvalueFixStack`。
- `err_capture_abort.ms`（配 `err_capture_abort.exit`，内容 `1`）：创建存活闭包后触发运行时错误（如对 nil 做算术），覆盖错误恢复的帧丢弃关闭路径；进程以退出码 1 结束且 Debug 构建无内存错误（ASAN 层断言，脚本无法自证的部分由验收标准兜底）。

另有任务 13 的存量负例 `tests/ms/functions/err_outer_local.ms`（嵌套函数引用外层局部报编译错误）：本任务落地后该用例转为正例，实现时移除该脚本并将其场景并入 `write_capture.ms`。不新增 C 测试文件（README 约定任务 ≥ 09 一律 ms 脚本）；open 链表有序性、同槽位复用、「unref 归零必已关闭」等内部不变量以 `MS_ASSERT` 落在实现中，由 Debug 构建下的全量脚本运行覆盖。

## 验收标准

- [ ] `src/vm/ms_upvalue.{h,c}` 存在，guard 为 `MSLANG_SRC_VM_MS_UPVALUE_H_`，头文件自包含，风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsUpvalue`/`struct MsUpvalueDesc` 不 typedef、C 注释英文且仅 `//`）。
- [ ] open upvalue 链表按栈地址降序维护；同槽位捕获复用同一 `MsUpvalue`（引用计数加一）；`msUpvalueClose` 正确拷贝值、重定向 `location`、摘除链表且幂等；`msUpvalueUnref` 归零释放且 debug 下断言已关闭。
- [ ] `struct MsProto` 增加 `upvalueDescs`/`upvalueCount`（任务 05 扩展已同步）；`inStack=1` 捕获外层栈槽、`inStack=0` 共享外层 upvalue 指针（`msUpvalueRef`）的装配语义与本文一致；upvalue 不经求值栈传递，任务 05/13 文档的对应注释已回改。
- [ ] `LOAD_UPVAL`/`STORE_UPVAL`/`CLOSE_UPVALS` 语义落实；`RETURN`/`TAIL_CALL`、`msVmRun` 错误恢复帧丢弃、`msCoroutineDestroy` 四条路径均隐式关闭本帧 upvalue；压帧时函数对象的 upvalue 数组正确挂接到帧；栈扩容后 open upvalue 经 `msUpvalueFixStack` 修复（`stack_growth.ms` 通过）。
- [ ] 编译器逃逸标记为纯词法分析：仅被嵌套函数引用的局部置 `isCaptured`；含捕获变量的块退出点（含 `break`/`continue` 路径）发射 `CLOSE_UPVALS`；无捕获函数的指令流与任务 13 完全一致；任务 13 的 `err_outer_local.ms` 负例已移除并转为正例覆盖。
- [ ] 按引用捕获语义成立：内层直接写捕获变量无需 `nonlocal`，外层可见；多闭包共享；逃逸后值存活；循环变量单次循环单一绑定、块内 `:=` 逐迭代独立的语义与设计依据第 4 条一致。
- [ ] `tests/ms/closures/` 全部脚本经 `python run_tests.py` 通过（正向退出码 0、`err_capture_abort.ms` 以退出码 1 失败），覆盖「测试方案」清单各项；脚本不使用 list/dict（任务 16 之前）；`ctest --test-dir build` 全绿。
- [ ] 全部堆分配经 `msAlloc/msRealloc/msFree`；`MsUpvalue` 为引用计数的纯内部结构（无 GC 头）；Debug 构建（ASAN / `/RTC`）无内存错误，`msCloseState` 后分配统计归零；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 07 编译器接口的假定命名在实现时已按对应任务文档对齐；任务 17 的接入点（标记 upvalue 持有的值、函数 sweep 路径逐槽 unref）在文中显式标注。
