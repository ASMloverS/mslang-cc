# 08 VM 执行核心

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [05 字节码格式与 MsProto](05-bytecode-proto.md)、[06 对象模型基础](06-object-model.md) |

## 任务目标

交付 mslang 栈式字节码 VM 的执行核心（`src/vm/ms_vm.h` / `src/vm/ms_vm.c`）：

- **协程执行上下文**：每个协程独立的调用栈（`struct MsCallFrame` 帧数组）与求值栈（`struct MsObject*` 数组），均堆上分配、按需倍增伸缩，帧含 `proto`、返回地址、求值栈基址、upvalue 数组四要素。
- **switch 分派主循环**：取指-译码-执行的 `msVmRun`；computed goto 分派不在本任务范围（列入 [66 computed goto 分派](66-computed-goto.md)），但分派外壳整体隔离在单一函数内，便于届时按 [08-vm-internals.md](../language/08-vm-internals.md) §2.2 以 CMake 选择分派实现源文件的方式替换。
- **v0.1 核心指令子集语义**：常量/移动、局部变量、全局变量、算术（含机器字快路径内联）、比较、跳转、位置参数调用与返回，共 39 条；v0.1 子集中其余 24 条的归属见「指令实现范围」。
- **链式比较约定**：任务 05 明确留给任务 07/08 约定的 `MS_OP_CMP_CHAIN` 操作数语义，在本文定稿。

完成后，[09 最小可运行解释器](09-minimal-interpreter.md) 可以经 `msCallProto` / `msVmRun` 在主协程上执行模块顶层 `MsProto`。本任务自身经 `tests/c/test_vm.c` 的 C 单元测试独立验证：以任务 05 的 `MsProtoBuilder` 手工构造字节码、任务 06 的对象构造器准备操作数，驱动执行并断言结果。

## 设计依据

- [08-vm-internals.md](../language/08-vm-internals.md)
  - §2.1/§2.2：定长 4 字节指令与两种操作数格式；首版 switch 分派，computed goto 作为编译期可选优化经 CMake 选择分派实现源文件，不在 VM 代码中散布 `#ifdef`。
  - §4（本任务的核心依据）：每个协程拥有独立的调用栈（帧数组）与求值栈，堆上分配、可伸缩；帧为 `proto`、返回地址、求值栈基址、upvalue 数组；算术指令内联快路径——双操作数均为机器字 int / float 时直接计算，否则走类型分派（魔术方法查找）。
  - §5：所有协程的调用栈/求值栈是 GC 根集合的一部分——结构设计上把两栈集中于协程对象，为 [17 GC 标记-清除](17-gc-mark-sweep.md) 的扫描留口。
  - §6.1：`struct MsCoroutine` 的最终形态（含状态、结果、等待队列等调度字段）；本任务只落地其中执行必需的子集，调度字段随 [43 协程与 async/await](43-coroutines.md) 扩展。
- [02-types.md](../language/02-types.md) §2（真值规则）、§3.3（混合运算：`int op float` → `float`、`int / int` → `float`、`int div int` → `int` 向下取整、无 float→int 隐式转换）、§6（`==` 按值、`is` 按身份）。
- [09-c-api.md](../language/09-c-api.md) §4（`MsResult`、state.h 执行入口约定）、§8（错误处理约定：出错置错误槽）、§9（`MsCFunction` 签名 `MsObject* (*)(MsState* L, int64_t argc, MsObject** argv)`）。
- [10-c-style.md](../language/10-c-style.md)：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则（内部结构体不 typedef）、§5 错误处理与资源管理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）、§8 断言（内部不变量用 `MS_ASSERT`）。
- [11-project-layout.md](../language/11-project-layout.md)：`src/vm/` 目录位置；`tests/c/` 用自研 `ms_test.h`（任务 01 提供）做模块单元测试。
- 任务 05 提供：`struct MsProto`（含 `paramCount`/`localCount`/`stackSize`/`lines`）、`MsOpCode` 全集与 v0.1 子集、`msOpDecodeOp/A/Bx/SAx` 译码内联函数、`msProtoLineAt` 行号查询。其文档已定名，本文直接引用。
- 任务 02（文档尚未落定）提供：`msAlloc/msRealloc/msFree`、`MS_ASSERT`、分配统计接口（用于泄漏断言）。本文假定其接口名；若任务 02 文档定名不同，以实现时对齐为准。
- 任务 06（对象模型基础，文档尚未落定）按 [08-vm-internals.md](../language/08-vm-internals.md) §3 与 [09-c-api.md](../language/09-c-api.md) §5 假定提供以下接口，**实现时以任务 06 文档定名为准**：
  - 类型判定与取值：`MsTypeTag msTypeOf(const struct MsObject* obj)`；
    `bool msIntIsWord(const struct MsObject* obj)`（机器字直存表示）；
    `int64_t msIntWordValue(const struct MsObject* obj)`、`double msFloatValue(const struct MsObject* obj)`；
  - 构造：`struct MsObject* msNewInt(MsState* L, int64_t v)` / `msNewFloat` / `msNewBool` / `msNewNil`（09-c-api §5 已定名）；
  - 慢路径入口：`struct MsObject* msObjectBinaryOp(MsState* L, MsOpCode op, struct MsObject* lhs, struct MsObject* rhs)` 与 `msObjectUnaryOp`（内建类型组合的运算分派，魔术方法查找随 [15 class 基础](15-class-basics.md) / [25 class 继承与魔术方法](25-class-inheritance.md) 在该接口内部接入，VM 侧不变）；
  - 值相等：`bool msObjectValueEquals(const struct MsObject* a, const struct MsObject* b)`（沿用任务 05 的同名假定）；
  - 函数对象访问：`struct MsProto* msFunctionProto(const struct MsObject* fn)`、`MsCFunction msAsCFunction(const struct MsObject* fn)`；
  - 全局命名空间：`struct MsObject* msStateGetGlobal(MsState* L, const struct MsString* name)`（未定义返回 `NULL`）、`MsResult msStateSetGlobal(MsState* L, const struct MsString* name, struct MsObject* value)`；
  - 最小 `MsState` 的创建/销毁（`msNewState` / `msCloseState`，供 C 单元测试构造环境；任务 06 负责对象模型所需的最小形态，[09 最小可运行解释器](09-minimal-interpreter.md) 再扩展全局命名空间挂接与错误槽）。
  - 错误报告沿用 [09-c-api.md](../language/09-c-api.md) §8 已定名的 `void msRaiseRuntimeError(MsState* L, const char* fmt, ...)`。

对规范的三处显式处理（实现与评审时以此为据）：

1. **帧的求值栈基址用偏移而非指针**。求值栈可伸缩意味着 `msRealloc` 会搬移底层数组，指针基址在增长后全部失效；`baseIndex`（int 下标）对重定位天然免疫，热点循环缓存的窗口指针在每次增长后重新推导（见「求值栈增长与重定位」）。§4「求值栈基址」的语义不变。
2. **机器字 int 溢出的去向**。§3 规定溢出自动提升为堆上大整数，但大整数属 [31 任意精度整数](31-bigint.md)（v0.2）。本任务的快路径只做**不溢出**的机器字运算（检出溢出即转慢路径 `msObjectBinaryOp`）；慢路径在任务 31 前对溢出的处理（报运行时错误或以截断语义拒绝）以任务 06 文档为准。
3. **`MS_OP_CMP_CHAIN` 操作数约定**。任务 05 明确由任务 07/08 约定，本文在「链式比较约定」定稿，任务 07 按此生成代码。

## 详细设计

### 文件与模块边界

- `src/vm/ms_vm.h`（guard `MSLANG_SRC_VM_MS_VM_H_`）：`struct MsCallFrame`、`struct MsCoroutine`、容量常量与公开函数声明；自包含，include `<stdbool.h>`、`<stdint.h>`、任务 02 的 `"core/ms_result.h"`（假定名）与 `"vm/ms_proto.h"`；前向声明 `struct MsObject`、`struct MsString`、`struct MsUpvalue`、`MsState`。
- `src/vm/ms_vm.c`：分派主循环与全部栈/帧管理实现；辅助函数一律文件内 `static`。
- 模块的堆分配仅四处：协程的帧数组、求值栈数组及其增长，全部经 `msAlloc/msRealloc/msFree`，所有者为协程对象（`msCoroutineDestroy` 释放）。帧的 `upvalues` 指针是**借用**（指向函数对象持有的 upvalue 数组），VM 不释放。
- 协程对象本体（`struct MsCoroutine`）v0.1 由调用方经 `msCoroutineInit` / `msCoroutineDestroy` 管理（可内嵌于 `MsState` 的主协程字段）；GC 对象化（挂 `MsObjectHeader`、加调度字段）属任务 43，本文结构为其预留兼容形态。

### 协程执行上下文：调用栈与求值栈

```c
#define MS_VM_STACK_INIT_CAP 64             // eval stack initial slots
#define MS_VM_STACK_MAX_CAP (1 << 20)       // hard cap; growth beyond is a runtime error
#define MS_VM_FRAMES_INIT_CAP 8             // call stack initial frames
#define MS_VM_FRAMES_MAX 1000               // recursion depth guard

struct MsCallFrame {
  struct MsProto* proto;          // function being executed (borrowed)
  int pc;                         // next instruction index in proto->code;
                                  // saved by CALL as this frame's return address
  int baseIndex;                  // eval-stack index of slot 0 of this frame's register window
  struct MsUpvalue** upvalues;    // borrowed from the MsFunction; NULL/0 until task 14
  int upvalueCount;
};

struct MsCoroutine {
  struct MsCallFrame* frames;     // call stack, msAlloc'd, growable
  int frameCount;
  int framesCap;
  struct MsObject** stack;        // evaluation stack, msAlloc'd, growable
  int stackTop;                   // index one past the topmost value
  int stackCap;
};
```

帧窗口布局（单块求值栈上所有帧共享地址空间）：

```
stack:  ... | callable | arg0 ... arg_{paramCount-1} | nil-filled locals ... | scratch ...
                       ^baseIndex                    window = localCount slots
                       window 上界 = baseIndex + localCount + proto->stackSize
```

- 前 `paramCount` 槽由调用方以实参初始化；其余局部槽在压帧时填充 nil（`LOAD_LOCAL` 读到未赋值槽不产生未定义值）。
- `baseIndex + localCount` 起是该帧的求值暂存区，深度由编译期算出的 `proto->stackSize` 封顶；压帧时一次性确保整块窗口容量，帧内指令的 push/pop 因此不再做容量检查（仅 debug 构建以 `MS_ASSERT` 守 `stackSize` 不变量）。

### 求值栈增长与重定位

- `ensureStackCapacity(co, needed)`（static）：`stackTop + needed > stackCap` 时倍增扩容（首分配 `MS_VM_STACK_INIT_CAP`），超过 `MS_VM_STACK_MAX_CAP` 置运行时错误「stack overflow」并返回 `MS_ERROR_RUNTIME`。
- 扩容经 `msRealloc`，数组可能搬移。**纪律**：任何缓存的 `MsObject**` 窗口/栈顶指针不得跨越可能触发扩容的调用（压帧、`msObjectBinaryOp` 等分配点）持有；分派主循环在每次帧切换与每次扩容后从 `co->stack + frame->baseIndex` 重新推导。`baseIndex` 为偏移，无需修复帧数组。
- 帧数组同样倍增扩容，达到 `MS_VM_FRAMES_MAX` 置运行时错误「call stack overflow」（`RecursionError` 的 v0.1 占位，异常对象化属 [23 异常系统](23-exceptions.md)）。

### 公开接口

```c
// Initializes an execution context (empty stacks, no allocation failure
// possible: arrays are allocated lazily on first use).
void msCoroutineInit(struct MsCoroutine* co);

// Releases both stacks. Values remaining on the eval stack are NOT freed
// (their ownership belongs to the object model / GC).
void msCoroutineDestroy(struct MsCoroutine* co);

// Pushes a frame for proto without running it. argc must equal
// proto->paramCount (varargs/kwargs conventions land in task 13); argv may be
// NULL when argc == 0. Fails with MS_ERROR_OOM or MS_ERROR_RUNTIME (stack
// limits). Exposed for tests and for task 09/13; the dispatch loop itself
// pushes frames through the same internal path.
MsResult msVmPushFrame(MsState* L, struct MsCoroutine* co, struct MsProto* proto,
    int argc, struct MsObject** argv);

// Runs the dispatch loop on co until frameCount drops to stopAt (the RETURN
// of the frame at that depth pushes its result and ends the run). On
// MS_ERROR_RUNTIME / MS_ERROR_OOM the error is recorded on L's error slot
// (via msRaiseRuntimeError) and frames above stopAt are discarded, with
// stackTop restored to the entry value.
MsResult msVmRun(MsState* L, struct MsCoroutine* co, int stopAt);

// Convenience composition: pushes a frame for proto and runs to completion,
// handing the return value to *out (caller does not own it beyond the next
// allocation; root discipline per 09-c-api §3 applies once GC lands).
MsResult msCallProto(MsState* L, struct MsCoroutine* co, struct MsProto* proto,
    int argc, struct MsObject** argv, struct MsObject** out);
```

### switch 分派主循环

`msVmRun` 的结构（伪代码，非实现）：

```
loop:
  instr = frame->proto->code[frame->pc++]
  switch (msOpDecodeOp(instr)) {
    case MS_OP_LOAD_CONST: ... break;
    ...每个已支持指令一个 case...
    default:  // 未实现指令（含 v0.1 之外的全部枚举值）
      MS_ASSERT(0);  // debug：编译器不应生成
      report "opcode not implemented"; return MS_ERROR_RUNTIME;
  }
```

- 译码一律用任务 05 的 `msOpDecode*` 内联函数；格式（ABC/sAx）以 `msOpFormatOf` 元数据为准，不在 case 体内另作假设。
- 遵守 [10-c-style.md](../language/10-c-style.md) §2：每个 `case` 以 `break`/`return` 收尾或标注 `// fallthrough`；`default` 兜底未实现指令（见上）。
- **computed goto 预留**：分派外壳（取指、译码、switch）集中在 `msVmRun` 一个函数内，指令语义以本文档为唯一事实源；任务 66 届时按 [08-vm-internals.md](../language/08-vm-internals.md) §2.2 以 CMake 选择分派实现源文件替换外壳，本任务不在代码中引入任何 `#ifdef`。
- **safepoint/抢占预留**：回边（负偏移跳转）与 `CALL` 两处是 §5/§6.2 规定的 safepoint 与抢占检查位置，本任务以注释标记、不放检查代码（任务 [46](46-scheduler-mn.md)/[47](47-gc-safepoint.md) 接入）。

### 算术指令的机器字快路径

每条算术 case 的快路径**直接内联在 case 体内**（或以 `static inline` 助手承载，不经过外部函数调用层），命中条件与行为：

- **双机器字 int**（`msIntIsWord` 均真）：取 `int64_t` 直接计算，**先检出溢出**：
  - `ADD`/`SUB`/`NEG`：边界比较（如 `b > 0 && a > INT64_MAX - b`）；`NEG(INT64_MIN)` 视为溢出。
  - `MUL`：以无符号乘法配合回除验证或边界预检（严禁有符号溢出 UB）；`INT64_MIN * -1` 视为溢出。
  - `FLOORDIV`/`MOD`：Python 语义——商向 -inf 取整、余数取除数符号（`(a % b != 0) && ((a < 0) != (b < 0))` 时调整）；除数为零报运行时错误（ZeroDivisionError 占位）；`INT64_MIN / -1` 视为溢出。
  - `DIV`：`int / int` → `float`（02-types §3.3），不溢出的情形恒转 double 计算；除数为零报运行时错误。
  - `POW`：仅非负指数且每步乘法不溢出走快路径；负指数转 float 计算（`a ** -b == 1.0 / (a ** b)`）；其余转慢路径。
  - 位运算（`BITAND/BITOR/BITXOR/SHL/SHR/INVERT`）：仅 int 参与；移位计数为负报运行时错误，≥ 64 按 C 语义未定义故约定为运行时错误（与 Go 的可变移位不同，mslang 语义以 [03-syntax.md](../language/03-syntax.md) 定稿为准，实现时对齐全文）。
  - 命中溢出或任一为堆上大整数表示（`msIntIsWord` 为假）：**不计算**，整体移交慢路径 `msObjectBinaryOp`（任务 31 前其溢出处理以任务 06 为准，见「设计依据」第 2 条）。
- **int/float 混合与双 float**：按 §3.3 提升为 double 计算（`DIV` 亦同），结果 `msNewFloat`；无 float→int 隐式转换，`FLOORDIV`/`MOD`/位运算遇 float 走慢路径或报类型错误（以慢路径为准）。
- **其余类型组合**：移交慢路径（字符串拼接 `"a" + "b"`、容器运算、未来的魔术方法），慢路径返回 `NULL` 表示类型错误（错误已入错误槽），VM 原样传播 `MS_ERROR_RUNTIME`。
- 一元指令：`NEG` 同上加法规则；`INVERT` 仅机器字 int 快路径；`NOT` 按真值规则（见下）弹一压一 bool，无慢路径。

### 比较、真值与链式比较约定

- **数值比较**（`EQ/NE/LT/LE/GT/GE`）：双机器字 int 直接比较；int/float 混合提升 double 比较（`nan != nan` 自然成立）；其余类型 `EQ`/`NE` 先走指针相等捷径、再走 `msObjectValueEquals`；`LT` 等序比较对非数值类型移交慢路径（字符串按内容、未来 `__lt__`），不支持则类型错误。
- **`IS`**：身份比较——指针相等；nil/true/false 为驻留单例（任务 06 保证），小整数驻留属实现细节，脚本不得依赖（02-types §6），VM 不做额外处理。
- **真值判定**（条件跳转、`NOT`）按 02-types §2：`nil`、`false`、数值零、空字符串/字节串、空容器为假；容器长度经任务 06 的长度接口查询（假定 `msLen` 形态，以任务 06 为准）；`__bool__`/`__len__` 魔术方法回落随任务 25 接入真值入口，VM 侧以单一 `static bool vmIsTruthy(...)` 承载，届时只改一处。
- **条件跳转**：`JUMP_IF_FALSE/JUMP_IF_TRUE/JUMP_IF_NIL` 弹栈顶判定，`JUMP` 无条件；目标 = `pc + 1 + sAx`（任务 05 约定，本任务的 `pc` 在取指后已自增，故目标即 `pc + sAx`）。
- **链式比较约定（定稿，任务 07 按此生成）**：`CMP_CHAIN` 为 ABC 格式，`A` = 本段比较的操作码（`MS_OP_EQ`..`MS_OP_GE`，值域 8 位内），`Bx` = 失败时相对下一条指令的**无符号前向**跳转偏移。语义：弹 `right`、`left`；若 `compare(left, right, A)` 为真，压回 `right`（作为下一段的左操作数）并顺序执行；为假则压入 `false` 并跳转 `pc + Bx`（链尾）。`a < b < c` 的生成形态：

```
eval a
eval b
CMP_CHAIN A=MS_OP_LT Bx=+k   // 栈: [b]（真，继续）或 [false] + 跳转链尾（假）
eval c
LT                           // 栈: [result]
Lend:                        // 链尾：Bx 指向这里
```

语义与 Python 一致：链式短路，失败段之后的操作数不求值；n 段比较 = n-1 条 `CMP_CHAIN` + 1 条普通比较指令。

### 全局变量指令

`LOAD_GLOBAL Bx` / `STORE_GLOBAL Bx`：`Bx` 为名字符串的常量池下标（任务 05 约定），经假定接口 `msStateGetGlobal/msStateSetGlobal` 读写 `MsState` 的全局命名空间。`LOAD_GLOBAL` 未命中报运行时错误「undefined name '<name>'」；名字对象从常量池借用，VM 不持有。

### 调用与返回

- `CALL A`（ABC，`A` = 位置参数个数）：栈形 `[... callable arg0 ... arg_{A-1}]`。
  - **脚本函数**（`MS_TYPE_FUNCTION`）：`argc == proto->paramCount` 校验（不等报运行时错误；`hasVarArgs`/`hasKwArgs` 约定由 [13 函数与调用](13-functions-calls.md) 定稿，本任务遇两标志位为真的 Proto 报「not supported」）；`baseIndex = stackTop - A`；确保窗口容量；nil 填充剩余局部槽；保存当前帧 `pc`（返回地址），压入新帧 `{proto, pc=0, baseIndex, upvalues=NULL, upvalueCount=0}`（upvalue 装载随 [14 闭包与 upvalue](14-closures.md)）。
  - **C 函数**（`MS_TYPE_C_FUNCTION`）：以 `argv = &stack[baseIndex]` 直接调用 `MsCFunction`；返回 `NULL` 即错误已入槽，传播 `MS_ERROR_RUNTIME`；否则栈截断到 `baseIndex - 1`（弹掉 callable 与实参），压入返回值，不压帧。
  - 其余类型报运行时错误「object is not callable」。
- `RETURN`：弹栈顶为返回值；栈截断到 `baseIndex - 1`；弹帧。弹后 `frameCount == stopAt`：压入返回值，`msVmRun` 返回 `MS_OK`；否则恢复调用者帧（其 `pc` 已指向返回地址）继续执行，返回值压入其暂存区。
- 模块顶层 Proto 即一次零参调用（[09 最小可运行解释器](09-minimal-interpreter.md) §3）：`msCallProto(L, co, mainProto, 0, NULL, &result)`。

### 指令实现范围

本任务实现 v0.1 子集（63 条）中的 **39 条**：常量/移动 5（`LOAD_CONST`、`LOAD_NIL`、`LOAD_TRUE`、`LOAD_FALSE`、`MOVE`）、局部 2（`LOAD_LOCAL`、`STORE_LOCAL`）、全局 2、算术 15、比较 8（含 `CMP_CHAIN`、`IS`）、跳转 4、`CALL`、`RETURN`、`NOP`。

其余 24 条 v0.1 指令的归属（本任务执行到它们时命中 `default`：debug 构建 `MS_ASSERT` 失败，release 报「opcode not implemented」运行时错误）：

| 指令 | 归属任务 |
|---|---|
| `LOAD_UPVAL` `STORE_UPVAL` `CLOSE_UPVALS` `MAKE_FUNCTION` `MAKE_LAMBDA` | [14 闭包与 upvalue](14-closures.md)（本任务已承载帧的 upvalue 数组字段与空值约定） |
| `MAKE_CLASS` `GET_ATTR` `SET_ATTR` `DEL_ATTR` `LOAD_METHOD` `CALL_METHOD` | [15 class 基础](15-class-basics.md) |
| `CALL_KW` `TAIL_CALL` | [13 函数与调用](13-functions-calls.md)（调用约定定稿处） |
| `BUILD_LIST` `BUILD_DICT` `INDEX` `SET_INDEX` `DEL_INDEX` `APPEND` `IN` | [16 容器 list 与 dict](16-containers-list-dict.md) |
| `GET_ITER` `ITER_NEXT` `UNPACK` | [26 for-in 迭代协议](26-iteration-protocol.md)；协调点：若 [09 最小可运行解释器](09-minimal-interpreter.md) 的 for 循环下限早于任务 26，其实现上提至任务 09/16 侧完成，本任务的栈/帧约定不变 |
| `PRINT_EXPR` | [22 CLI 完善](22-cli-polish.md)（REPL） |
| `IMPORT` `IMPORT_FROM` | [24 模块系统与 import](24-modules-import.md)（v0.2） |
| `BUILD_TUPLE` `BUILD_SET` `SLICE` | [30 切片与下标完善](30-slicing.md)、[32 bytes、tuple 与 set](32-bytes-tuple-set.md)（v0.2） |
| `SETUP_TRY` `POP_TRY` `RAISE` `RERAISE` | [23 异常系统](23-exceptions.md)（v0.2） |
| 并发类 10 条 | [43](43-coroutines.md)/[44](44-channel.md)/[45](45-select.md)（v0.3） |

### 运行时错误与诊断

- 所有运行时错误经 `msRaiseRuntimeError(L, fmt, ...)` 记录后返回 `MS_ERROR_RUNTIME`；消息携带位置：源文件取 `frame->proto->sourceFile`，行号取 `msProtoLineAt(frame->proto, frame->pc - 1)`（出错指令的 pc）。
- 无异常对象的 v0.1：VM 不做栈内展开搜索，直接中止 `msVmRun`；`msVmRun`/`msCallProto` 负责把协程恢复到进入时的 `frameCount`/`stackTop`（丢弃中间帧），保证同一协程可再次执行（REPL 场景）。
- OOM（栈/帧扩容失败）返回 `MS_ERROR_OOM`，同样恢复协程状态。

### 与 GC / 调度的前向兼容

- 调用栈与求值栈集中在 `struct MsCoroutine`，任务 17 的根扫描只需遍历 `frames`（proto、upvalues）与 `stack[0..stackTop)`；本任务不引入扫描接口，但保证无旁路副本（值只存在于栈上、常量池中或由对象模型持有）。
- C 局部 `MsObject*` 跨分配点的根纪律（[09-c-api.md](../language/09-c-api.md) §3）自任务 17 起强制；本任务代码在可能分配的调用之间不缓存裸 `MsObject*`（慢路径返回值立即压栈）。
- 协程的调度字段（状态、结果、等待队列）与多协程管理属任务 43/46；本任务的 `MsCoroutine` 为其预留结构演进空间，公开 API 不暴露内部布局以外的承诺。

## 实现步骤

1. 建 `src/vm/ms_vm.h` / `ms_vm.c` 骨架：容量常量、`struct MsCallFrame`、`struct MsCoroutine`、`msCoroutineInit`/`msCoroutineDestroy`（惰性分配）。验证：init 后全零状态；destroy 空协程后任务 02 分配统计归零。
2. 求值栈与帧数组管理（static）：倍增扩容、两级硬上限（`MS_VM_STACK_MAX_CAP`、`MS_VM_FRAMES_MAX`）、重定位纪律。验证：经 `msVmPushFrame` 连续压帧触发两类增长，容量与内容保持正确；越限报对应运行时错误。
3. `msVmPushFrame` 完整语义：实参拷贝、nil 填充局部槽、窗口容量一次性确保、`argc != paramCount` 与标志位为真的拒绝路径。验证：窗口布局逐槽断言。
4. 分派主循环骨架 + 常量/移动/局部/全局指令 + `NOP` + `default` 未实现指令路径；`msVmRun`/`msCallProto` 的 stopAt 与错误恢复语义。验证：手工构造 Proto 取常量返回；`LOAD_GLOBAL` 未命中报错；执行 `SETUP_TRY` 等未实现指令返回 `MS_ERROR_RUNTIME`。
5. 跳转与真值：四条跳转（含回边）、`vmIsTruthy` 全类型表。验证：手工构造循环 Proto（如累加 1..100）结果正确。
6. 算术快路径：双机器字 int 全操作（含 `FLOORDIV`/`MOD` 的 Python 取整语义）、溢出检出转慢路径、int/float 混合、`POW` 规则、位运算与移位错误；一元三指令。验证：逐操作的正例/边界/错误用例。
7. 比较与 `CMP_CHAIN`：数值比较、值相等捷径、身份比较、链式比较的真/假两条路径与短路。验证：含 `a < b < c` 全真/中途失败/首段失败三组 Proto。
8. `CALL`/`RETURN`：脚本函数调用与返回、C 函数直调、参数个数校验、嵌套与递归（深度上限）、返回值落栈位置。验证：递归 fib Proto（经全局命名空间自引用）与测试用 `MsCFunction`。
9. 接入 CMake `mslang` 库目标与 `mslang-tests`，`ctest` 全绿；`MSLANG_STRICT_WARNINGS` 无警告；Win/Linux/macOS Debug/Release 构建通过。

## 测试方案

本任务早于最小可运行解释器（任务 09），只能用 C 单元测试。测试文件 `tests/c/test_vm.c`（本任务只交付本设计文档，测试代码随实现任务编写），使用任务 01 的 `ms_test.h`（`MS_TEST`/`MS_ASSERT_EQ`）。统一测试手法：以任务 05 的 `MsProtoBuilder` 手工构造字节码（元数据字段直接写 `b->proto`），以任务 06 的对象构造器准备常量与全局名，`msCallProto` 驱动执行后断言返回值与错误槽；需要局部槽/深栈的用例相应设置 `localCount`/`stackSize`。每个用例结束后 `msCoroutineDestroy` + `msCloseState`，断言任务 02 分配统计归零（无泄漏）。

覆盖清单：

- 生命周期：init 全零；空协程 destroy；`msCallProto` 空 Proto（仅 `RETURN`）返回 nil。
- 求值栈：构造 `stackSize` 超过初始容量 64 的深表达式 Proto，结果正确（验证增长与 `baseIndex` 重定位纪律）；构造超过 `MS_VM_STACK_MAX_CAP` 需求的压帧报「stack overflow」。
- 调用栈：嵌套调用超过 `MS_VM_FRAMES_INIT_CAP` 触发帧数组增长后结果正确；递归达到 `MS_VM_FRAMES_MAX` 报「call stack overflow」且错误后协程恢复到进入深度、可再次成功执行。
- 帧窗口：多参数 + 多局部 Proto，断言实参入槽、未赋值局部槽读为 nil、`MOVE`/`LOAD_LOCAL`/`STORE_LOCAL` 逐槽语义。
- 常量/全局：`LOAD_CONST` 各常量池下标；`LOAD_NIL/TRUE/FALSE`；`STORE_GLOBAL` 后 `LOAD_GLOBAL` 往返；`LOAD_GLOBAL` 未命中报「undefined name」。
- 跳转与真值：`JUMP` 前向/回边；条件跳转对真值表逐项（nil/false/0/0.0/""/非空值）；回边循环累加 Proto。
- 算术：`ADD/SUB/MUL/DIV/FLOORDIV/MOD/POW/NEG` 的机器字 int 正例与边界（`INT64_MAX/INT64_MIN` 邻域）；`-7 div 2 == -4`、`-7 % 2 == 1`、`7 % -3 == -2` 等符号语义；`int / int` 得 float（`10 / 4 == 2.5`）；int/float 混合提升；溢出用例转慢路径的行为与任务 06 文档对齐后断言；除零、模零、负移位、≥64 移位、float 位运算各错误路径。
- 比较：数值六运算真/假；混合比较；`EQ` 指针捷径与值相等（同值不同指针对象）；`IS` 对 nil/true/false 单例；`CMP_CHAIN` 三段链全真、第二段失败、首段失败（验证短路不求值后续操作数——以一个会报错的表达式作后续操作数观察其未执行）。
- 调用：脚本函数多层调用与返回值落位；C 函数（测试内注册一个 `MsCFunction`）直调成功与其返回 `NULL` 的错误传播；`argc` 不匹配报错；不可调用对象报错。
- 错误诊断：人为触发运行时错误，断言错误槽消息含源文件名与正确行号（经 `lines` 表多行号段验证）。
- 未实现指令：对「指令实现范围」表中每类抽一条（如 `MAKE_FUNCTION`、`BUILD_LIST`、`GET_ITER`），断言 debug 构建外的 release 路径返回 `MS_ERROR_RUNTIME` 且协程状态已恢复。

## 验收标准

- [ ] `src/vm/ms_vm.h` / `ms_vm.c` 存在，guard 为 `MSLANG_SRC_VM_MS_VM_H_`，头文件自包含，风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsCallFrame`/`struct MsCoroutine` 不 typedef）。
- [ ] 每个协程独立的调用栈（帧数组）与求值栈，均堆上分配、倍增伸缩；`MsCallFrame` 含 proto/返回地址（`pc`）/求值栈基址（`baseIndex` 偏移）/upvalue 数组四要素；求值栈基址以偏移表示，扩容重定位后结果正确。
- [ ] 分派为单一 `msVmRun` 函数内的 switch；无 computed goto、无 `#ifdef` 分派分支；`default` 覆盖全部未实现指令；回边与 `CALL` 处留有 safepoint 注释标记。
- [ ] 本任务实现的 39 条指令语义与本文一致；算术快路径满足：双机器字 int 直接计算、溢出检出转慢路径、无符号运算规避 UB、`FLOORDIV`/`MOD` 为 Python 取整语义、`int / int` 得 float。
- [ ] `CMP_CHAIN` 操作数约定（A = 比较操作码、Bx = 失败前向偏移、真压回右操作数、假压 false 跳链尾）按本文定稿并与任务 07 对齐。
- [ ] `CALL`/`RETURN` 的帧压弹、实参布局、nil 填充、C 函数直调、深度上限与错误恢复语义如文所述；`msVmRun` 错误后协程恢复到进入深度。
- [ ] 运行时错误经错误槽记录且含源文件与行号（`msProtoLineAt`）；全部堆分配经 `msAlloc/msRealloc/msFree`，任务 02 分配统计显示无泄漏。
- [ ] `tests/c/test_vm.c` 覆盖「测试方案」全部清单项并全部通过；`ctest` 全绿；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 02/06 的接口假定（`msAlloc`、`MS_ASSERT`、`msIntIsWord`、`msObjectBinaryOp`、`msStateGetGlobal`、`msRaiseRuntimeError` 等）在实现时已按对应任务文档对齐；「指令实现范围」表的归属与 [README.md](README.md) 依赖图无冲突。
