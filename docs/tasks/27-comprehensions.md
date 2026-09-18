# 27 推导式

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [16 容器 list 与 dict](16-containers-list-dict.md)、[26 for-in 迭代协议](26-iteration-protocol.md) |

## 任务目标

交付 mslang 三种推导式（[03-syntax.md](../language/03-syntax.md) §6.2）的编译期支持：list 推导式 `[x * x for x in xs]`、dict 推导式 `{k: v * 2 for k, v in m}`、set 推导式 `{x % 10 for x in xs}`，含多重 `for` 级联（`[x + y for x in a for y in b]`）、`if` 条件过滤、解包目标（`for k, v in m`）与任意深度嵌套（推导式可作为元素表达式或内层 iterable 再出现）。

语法与 AST 节点（`MS_AST_LIST_COMP` / `MS_AST_DICT_COMP` / `MS_AST_SET_COMP`）已由任务 04 落地；本任务只扩展编译器与 VM：选定就地展开的字节码生成策略、为 dict/set 推导式补充构建指令、落实推导式的隐式作用域与循环变量绑定语义。set 类型本体属任务 32，本任务为其推导式接通编译管线并以占位指令收尾，任务 32 落地后仅需替换占位语义。完成后通过 `tests/ms/comprehensions/` 的 ms 脚本验证。

生成器表达式（惰性版本）不在本任务范围，03-syntax §6.2 已列入路线图。

## 设计依据

- [03-syntax.md](../language/03-syntax.md)
  - §6.2：三种推导式形态、多重 `for` 级联、`if` 过滤；生成器表达式列入路线图（不支持）。
  - §3.1：`for-in` 目标列表支持解包（`for k, v in m`，dict 迭代产出键值对）。
  - §8：块级作用域；闭包按引用捕获外层局部变量；名字解析顺序 局部 → 闭包外层 → 模块全局 → 内建。
- [08-vm-internals.md](../language/08-vm-internals.md)
  - §1：编译管线定位（Compiler 遍历 AST 生成 MsProto；无独立优化 pass）。
  - §2.2：迭代指令 `MS_OP_GET_ITER` / `MS_OP_ITER_NEXT`（失败时跳转，避免 StopIteration 异常开销）/ `MS_OP_UNPACK`；容器指令 `MS_OP_BUILD_LIST` / `MS_OP_BUILD_DICT` / `MS_OP_BUILD_SET` / `MS_OP_APPEND`；闭包指令 `MS_OP_CLOSE_UPVALS`。指令集目标总条数 80 条以内。
- [任务 04 语法分析器与 AST](04-parser-ast.md)：推导式节点结构——`comprehension` 联合分支 `{ clauses, elem, key }`，`clauses` 为 `for targets in expr [if cond]` 子句列表；dict 推导式用 `key` + `elem`，list/set 推导式 `key == NULL`。
- [任务 16 容器 list 与 dict](16-containers-list-dict.md)：`MS_OP_BUILD_LIST` / `MS_OP_BUILD_DICT` / `MS_OP_APPEND` 已接通；`msListAppend` / `msDictSet` 语义（dict 重复键原位替换、不改变插入位置）——dict 推导式的重复键行为直接继承此语义；`msDictNext` 插入序迭代约定。
- [任务 26 for-in 迭代协议](26-iteration-protocol.md)：该任务文档尚未定稿。本文假定其指令约定为——`MS_OP_GET_ITER`：弹出栈顶对象、压入其迭代器（不可迭代则置运行时错误）；`MS_OP_ITER_NEXT sAx`：迭代器位于栈顶，成功时压入下一个值并顺序执行，耗尽时弹出迭代器并跳转 sAx；`MS_OP_UNPACK A`：弹出栈顶可迭代值，展开为 A 个值压栈（供多目标绑定），长度不符报运行时错误。**以上指令名与栈约定为假定命名，实现时以任务 26 文档定名为准。**
- 任务 07（编译器）文档尚未定稿。本文引用的编译器内部接口（`struct MsCompiler`、`msCompilerPushScope` / `msCompilerPopScope` / `msCompilerDeclareLocal` / `msCompilerEmit*` 等）均为假定命名，实现时以对应任务文档定名为准。
- [10-c-style.md](../language/10-c-style.md)：全部 C 代码遵循其规范（2 空格缩进、120 列行宽、K&R、指针星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。

## 详细设计

### 1. 编译策略决策：就地展开，而非隐式函数

候选方案二选一：

- **隐式函数（CPython 式）**：把推导式编译为外层 proto 常量池中的匿名子 proto，最外层 iterable 作为参数传入，运行时 `MS_OP_MAKE_FUNCTION` + `MS_OP_CALL` 执行；内层循环变量对外层可见性经 upvalue 捕获解决。
- **就地展开（本任务选定）**：编译器把推导式整体脱糖为当前 proto 内的一段内联循环字节码，循环变量经编译器作用域栈压入一个新的隐式块作用域，推导式结束后弹栈。

选定就地展开的理由：

1. **作用域规则契合**。03-syntax §8 只有「块级作用域 + 闭包按引用捕获」两级机制，没有 Python 的「推导式自带函数作用域」规则。推导式循环变量不外泄的语义，用编译期压入/弹出块作用域即可完整表达，无需函数边界。
2. **嵌套推导式零额外机制**。就地展开下，内层推导式的 iterable 引用外层循环变量就是普通的局部变量读取（`MS_OP_LOAD_LOCAL`）；隐式函数方案则要求外层循环变量按 cell 捕获、跨 proto 传递，复杂度全部落在闭包通路上。
3. **无运行时开销**。省去每条推导式一次的 MsProto 分配、函数对象创建与调用帧进出；推导式在脚本中是高频构造。
4. **编译器架构一致**。任务 07 的编译器本就维护块作用域栈处理 `{}` 块，推导式复用同一机制，不引入「表达式中合成函数」的新代码路径。

代价与对策（就地展开的两个已知陷阱，均在本设计中显式处理）：

- **闭包捕获循环变量**：就地展开下循环变量是外层函数的同一局部槽位，每轮迭代复用。若推导式元素表达式产生闭包（如 `[(lambda: x) for x in xs]`），不处理则所有闭包共享最终值。对策见第 7 节：迭代回边处对被捕获的循环变量发射 `MS_OP_CLOSE_UPVALS`，使每轮迭代产生新绑定，与 Python 语义一致。
- **栈深统计**：循环迭代器与结果容器常驻求值栈，编译器须把推导式展开段纳入 `MsProto.stackSize` 的最大栈深计算（见第 4 节的栈布局不变量）。

`return`/`break`/`continue` 不可能出现在推导式内部：推导式的子结构全部是表达式（任务 04 的 AST 只挂表达式节点），语句级语境检查（parser 的 `loopDepth`/`funcDepth`）天然不适用，无需额外限制。推导式合成循环不允许被外层 `break`/`continue` 误命中——编译器在进入推导式时保存并复位循环上下文标签（假定接口 `msCompilerLoopContext`），退出时恢复，保证 `[x for x in xs]` 出现在循环体内时，外层循环的 `break` 仍指向外层循环。

### 2. 编译器内部接口（假定命名）

全部函数为 `src/compiler/` 内文件级 `static`：

```c
// Dispatches on MS_AST_LIST_COMP / MS_AST_DICT_COMP / MS_AST_SET_COMP.
// Leaves the built container on the eval stack. Errors are recorded in the
// shared diagnostic list; code is a compile-time error code (E3xx range,
// assigned with the task 07 compiler table).
static void compileComprehension(struct MsCompiler* c, const struct MsAst* node);

// Emits one "for targets in iterable [if cond]" level. clauseIx is the index
// into node->as.comprehension.clauses; recursion over clauseIx implements the
// cascading of multiple for clauses. endLabel is the shared patch target for
// loop exit; continueLabel is the per-level patch target for if-filter skips.
static void compileComprehensionLevel(struct MsCompiler* c, const struct MsAst* node,
    size_t clauseIx, int endLabel);

// Emits the per-level body: element (and key) evaluation plus the
// container-add instruction for the innermost level.
static void compileComprehensionEmit(struct MsCompiler* c, const struct MsAst* node);
```

作用域栈操作复用任务 07 的既有接口（假定命名）：`msCompilerPushScope(c)` / `msCompilerPopScope(c)` / `msCompilerDeclareLocal(c, name, nameLen)` / `msCompilerEmitJump*`。作用域弹栈时若该作用域内有被捕获（escape）的局部，任务 07 的既有逻辑负责发射 `MS_OP_CLOSE_UPVALS`；本任务第 7 节的迭代级关闭在此基础上追加一条显式发射点。

### 3. 指令扩展：`MS_OP_DICT_ADD` 与 `MS_OP_SET_ADD`

`MS_OP_APPEND`（任务 16 已接通 list 语义）不足以覆盖 dict/set 推导式，本任务为指令集新增两条容器构建指令（与 08-vm-internals §2.2 的 80 条上限相容，扩充后仍远低于上限）：

| 指令 | 操作数 | 栈行为 | 语义 |
|---|---|---|---|
| `MS_OP_DICT_ADD` | A = 目标 dict 距栈顶的槽位深度（弹出操作数后计，1 起） | 先弹 value、再弹 key | 对目标 dict 执行 `msDictSet` 语义：键不可哈希报运行时错误；重复键原位替换、不改变插入位置（继承任务 16） |
| `MS_OP_SET_ADD` | A = 目标 set 距栈顶的槽位深度（同口径） | 弹出 value | 对目标 set 执行插入去重语义；**任务 32 落地前为占位**（见下） |

`MS_OP_APPEND` 的 A 操作数口径与上表统一：A = 目标 list 距栈顶的槽位深度（弹出待追加值后计，1 起）。任务 16 接通该指令时尚无推导式消费方，若其实现的栈口径与此不同，以本文口径为准对齐（两处同一指令，不允许两套语义）。

set 占位策略：`MS_OP_BUILD_SET` 与 `MS_OP_SET_ADD` 的 VM 分支在任务 32 之前一律置运行时错误 "set type not implemented (task 32)"；编译器对 `MS_AST_SET_COMP` 正常生成完整字节码（含 `MS_OP_GET_ITER` / 循环 / `MS_OP_SET_ADD`），**不**在编译期拒绝——这样任务 32 只需实现 set 对象与两条指令的真实分支，编译器零改动。此占位在任务 32 验收时删除。

### 4. list 推导式的字节码模板

以 `[elem for t in iter0 if cond]`（单层、带过滤）为例。栈布局不变量：进入推导式后，求值栈上常驻 `[it0, result]`（迭代器在下、结果容器在上之前——见下），编译期可静态算出每个 `MS_OP_*_ADD` 的 A。

```
  ; ---- 外层（推导式隐式作用域之外）----
  <iter0 表达式求值>                  ; 栈: [iterable]
  MS_OP_GET_ITER                     ; 栈: [it0]
  ; ---- 压入推导式隐式块作用域 ----
  MS_OP_BUILD_LIST 0                 ; 栈: [it0, result]
L0:                                  ; 循环头（任务 26 约定：迭代器须在栈顶）
  ; 交换使迭代器置顶由 ITER_NEXT 的既定约定决定；
  ; 若任务 26 约定 ITER_NEXT 以 A 操作数定位迭代器，则此处无需调整。
  MS_OP_ITER_NEXT -> Lend            ; 成功: [it0, result, v]；耗尽: 弹 it0，跳 Lend
  ; ---- 绑定目标 ----
  MS_OP_STORE_LOCAL t                ; 单目标；多目标先 MS_OP_UNPACK n 再逐个 STORE_LOCAL
  ; ---- if 过滤（可选）----
  <cond 表达式求值>                  ; 栈: [it0, result, c]
  MS_OP_JUMP_IF_FALSE -> Lnext       ; 假: 跳过元素发射
  ; ---- 元素发射 ----
  <elem 表达式求值>                  ; 栈: [it0, result, e]
  MS_OP_APPEND A=1                   ; 弹 e，追加到 result（弹出后 result 距栈顶 1 槽）
Lnext:                               ; if 为假的汇合点
  ; ---- 迭代级 upvalue 关闭（第 7 节）----
  [MS_OP_CLOSE_UPVALS t]             ; 仅当 t 被闭包捕获
  MS_OP_JUMP -> L0
Lend:                                ; 栈: [result]（迭代器已被 ITER-NEXT 弹走）
  ; ---- 弹出推导式隐式块作用域 ----
```

要点：

- **最外层 iterable 在推导式作用域之外求值**（Python 语义）：`[x for x in [x]]` 中外层 `x` 可见。实现上即 `compileComprehension` 先编译 `clauses[0]` 的 iterable 表达式，再压作用域。
- 结果容器在循环开始**之前**构建（`MS_OP_BUILD_LIST 0`），空迭代自然得到空容器，无需特判。
- `if` 过滤为纯跳转：条件为假跳到 `Lnext`（回边），不产生额外指令；每个 `for` 子句至多一个 `if`（任务 04 的 AST 形态）。
- 迭代级 `MS_OP_CLOSE_UPVALS` 放在回边之前、`if` 汇合点之后，保证被过滤的迭代同样关闭（同一迭代内产生的闭包一致看待）。
- 正常结束路径上迭代器由 `MS_OP_ITER_NEXT` 耗尽分支弹出（假定约定，以任务 26 定名为准）；`result` 作为表达式值留栈。

### 5. dict 与 set 推导式模板

dict 推导式 `{k: v * 2 for k, v in m}` 与 list 模板同构，差异仅在结果构建与元素发射：

```
  MS_OP_BUILD_DICT 0                 ; 结果容器
  ...
  ; ---- 绑定目标：MS_OP_UNPACK 2 展开键值对，逐个 STORE_LOCAL ----
  ; ---- if 过滤（同上）----
  <key 表达式求值>                   ; 栈: [it0, result, k]
  <elem 表达式求值>                  ; 栈: [it0, result, k, v]
  MS_OP_DICT_ADD A=1                 ; 弹 v、弹 k，写入 result
```

- 重复键语义直接继承 `msDictSet`：后者覆盖值、不改变首次插入位置（任务 16），与 Python dict 推导式一致。
- 键不可哈希（如 list 键）在 `MS_OP_DICT_ADD` 的 VM 分支报运行时错误，错误传播链路与任务 16 的 `m[k] = v` 相同。

set 推导式：`MS_OP_BUILD_SET 0` 构建结果、元素发射用 `MS_OP_SET_ADD A=1`；任务 32 落地前两条指令为占位运行时错误（第 3 节）。去重语义（相等即去重、保持首插序）由任务 32 定义，本文不预定。

### 6. 多重 `for` 级联

`[x + y for x in a for y in b]` 编译为嵌套循环：`compileComprehensionLevel` 按 `clauseIx` 递归——第 i 级先求值该级 iterable 并 `MS_OP_GET_ITER`，再发射本级循环头；循环体若还有下一级则递归进入（**内层 iterable 在每轮外层迭代中重新求值、重新取迭代器**，与 Python 一致），最后一级循环体才是元素发射。栈布局逐级加深：

```
  <a 求值> MS_OP_GET_ITER            ; [it0]
  MS_OP_BUILD_LIST 0                 ; [it0, result]
L0: MS_OP_ITER_NEXT -> Lend0         ; [it0, result, x] → 绑定 x
  <b 求值> MS_OP_GET_ITER            ; [it0, result, it1]
L1: MS_OP_ITER_NEXT -> Lend1         ; [it0, result, it1, y] → 绑定 y
  <x + y> MS_OP_APPEND A=2           ; 弹出 e 后 result 距栈顶 2 槽（it1 在其上）
  [CLOSE_UPVALS y] MS_OP_JUMP -> L1
Lend1:                               ; it1 已弹 → [it0, result]
  [CLOSE_UPVALS x] MS_OP_JUMP -> L0
Lend0:                               ; [result]
```

- 每级 `for` 子句的 `if` 过滤只影响该级：第 i 级条件为假跳到第 i 级的回边（`Lnext_i`），即「内层整轮跳过」由外层条件自然实现。
- 各级的 `MS_OP_APPEND`/`MS_OP_DICT_ADD` 的 A 操作数由编译器按当前存活的内层迭代器个数静态计算（A = 1 + 该指令点上方迭代器数），无运行时调整指令。
- 作用域上，多重 `for` 级联共享**同一个**推导式隐式作用域（所有循环变量同级声明，后声明的遮蔽先声明的同名变量——`[x for x in a for x in b]` 内层 `x` 遮蔽外层，作用域弹栈时一并结束）；嵌套推导式（下节）才引入新作用域。

### 7. 嵌套推导式与作用域

- 推导式可出现在任意表达式位置：元素表达式（`[[y * y for y in row] for row in grid]`）、内层 iterable（`[y for row in grid for y in [v for v in row]]`）、条件表达式、dict 的 key/value 等。编译上无特殊路径——`compileComprehension` 是表达式编译的常规一级，递归命中。
- 每条推导式压入自己的隐式块作用域；嵌套时形成作用域栈上的父子关系。就地展开下内层推导式引用外层循环变量是普通局部读取（`grid`/`row`/`y` 都在同一 proto 的局部窗口内），无闭包机制介入。
- 循环变量**不外泄**：推导式作用域弹栈后，外层同名变量不受影响，`x := 100; ys := [x * 2 for x in [1, 2]]; x` 仍为 100。若外层无同名变量，推导式后引用 `x` 报未定义名字错误（任务 07/11 的常规名字解析路径）。
- 循环变量的迭代级绑定：若循环变量被其迭代内的 lambda/闭包捕获（编译器作用域解析阶段已标记 escape），在回边前发射 `MS_OP_CLOSE_UPVALS`，使下一轮迭代的重新绑定产生新单元——`fs := [(lambda: x) for x in [1, 2, 3]]` 中 `fs[0]()` / `fs[1]()` / `fs[2]()` 分别为 1/2/3，与 Python 一致。未被捕获时不发射，无开销。
- 推导式出现在模块顶层与函数体内走同一代码路径（顶层即主 proto 的局部窗口）。

### 8. 错误与边界语义

- iterable 不可迭代：`MS_OP_GET_ITER` 按任务 26 约定置运行时错误（"object is not iterable" 类），VM 中止、CLI 退出码 1（任务 09 约定；异常系统任务 23 落地后转为异常对象）。
- 解包目标长度不符：`MS_OP_UNPACK` 报运行时错误（任务 26 语义），推导式不另做检查。
- 推导式内部的表达式错误（如 elem 求值中的除零、缺键）沿既有运行时错误路径传播，循环不做清理特判——求值栈残项随帧销毁，与普通循环体内出错一致。
- OOM：结果容器扩容失败经任务 16 的统一 OOM 路径；编译期 arena 分配失败返回 `MS_ERROR_OOM`（任务 07 既有约定）。
- 诊断：本任务不新增语法错误码（推导式语法检查属任务 04）；编译期可能出现的语义诊断（如作用域解析失败）复用任务 07 的错误码段。

## 实现步骤

1. 编译器接入 `MS_AST_LIST_COMP` 分派：实现 `compileComprehension` / `compileComprehensionLevel` 骨架——最外层 iterable 作用域外求值、`MS_OP_GET_ITER`、压隐式作用域、`MS_OP_BUILD_LIST 0`、单层循环（无 `if`）、`MS_OP_APPEND`（A 口径对齐第 3 节）、作用域弹栈与栈深统计。验证：ms 脚本 `[x * x for x in [1, 2, 3]]` 断言结果为 `[1, 4, 9]`。
2. `if` 条件过滤：`cond` 求值 + `MS_OP_JUMP_IF_FALSE` 到本级回边汇合点。验证：`[x for x in xs if x > 0]` 正例与恒假条件得空表。
3. 解包目标与多重 `for` 级联：`MS_OP_UNPACK` 绑定、`compileComprehensionLevel` 递归、内层 iterable 每轮重取迭代器、`MS_OP_APPEND` 的 A 随嵌套深度递增。验证：`[x + y for x in a for y in b]` 顺序与内容、`[k: v for k, v in m]` 的解包绑定（dict 部分随步骤 5 全通）。
4. 迭代级 upvalue 关闭：循环变量 escape 标记接入回边前的 `MS_OP_CLOSE_UPVALS` 发射（含被过滤迭代）。验证：`[(lambda: x) for x in [1, 2, 3]]` 逐元素调用结果；外层循环内的推导式不吞掉外层 `break`/`continue` 语境。
5. dict 推导式：新增 `MS_OP_DICT_ADD` 指令（编码、VM 分支、`msDictSet` 语义接线）、`MS_OP_BUILD_DICT 0` 结果构建、key+elem 双表达式发射。验证：`{k: v * 2 for k, v in m}`、重复键原位覆盖、不可哈希键负例脚本退出码 1。
6. 嵌套推导式：确认递归路径无需特判，补作用域遮蔽与弹栈的正确性。验证：`[[y for y in row] for row in grid]`、元素为推导式、iterable 为推导式、循环变量不外泄（`x := 100` 前后断言）。
7. set 推导式占位：编译器对 `MS_AST_SET_COMP` 正常发射 `MS_OP_BUILD_SET` / `MS_OP_SET_ADD`；VM 两分支置 "set type not implemented (task 32)" 运行时错误。验证：set 推导式脚本当前以退出码 1 失败（负例口径），字节码反汇编（任务 09 的调试设施）确认指令序列完整。
8. 边界与内存复查：空 iterable、单元素、深层级联（3 层 `for`）的栈深统计断言；`msCloseState` 后任务 09 分配统计归零；Debug 构建（ASAN / `/RTC`）跑全部推导式测试无报告。验证：`run_tests.py` 全绿，Win/Linux/macOS × Debug/Release 构建通过。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试（`testing` 模块在任务 40 才存在，本阶段用内建 `assert` + `print` 自断言；负向用例以 `<name>.exit` 同伴文件声明预期退出码 1，由仓库根 `run_tests.py` 驱动，设施约定见任务 09）。本任务只交付设计文档，脚本随实现编写。

测试文件清单（`tests/ms/comprehensions/`）与覆盖点：

- `list_basic.ms`：空 iterable 得空表、单元素、数值/字符串元素、`[x * x for x in xs]`、元素为复杂表达式（调用、条件表达式、下标）；末尾 `print("list basic ok")`。
- `list_filter.ms`：单 `if` 过滤（含恒真/恒假）、条件引用循环变量与外层变量、过滤后为空表。
- `list_multi_for.ms`：双重 `for` 级联的结果顺序（外层慢变、内层快变）、内层 iterable 依赖外层循环变量（`[y for x in rows for y in x]`）、三级级联、级联 + 各级独立 `if`。
- `dict_basic.ms`：`{k: v * 2 for k, v in m}` 全量转换、`for k in m.keys()` 式单目标形式（依赖任务 16 方法）、结果插入序与源迭代序一致（经 `keys()` 比对）、重复键后者覆盖且不改首插位置。
- `dict_filter.ms`：dict 推导式带 `if` 过滤（条件可同时引用 k 与 v）、过滤后为空 dict。
- `nested.ms`：元素为推导式（`[[y * y for y in row] for row in grid]`）、内层 iterable 为推导式、三层嵌套；嵌套时内层引用外层循环变量。
- `scope.ms`：循环变量不外泄（推导式后外层同名变量值不变；外层无同名时引用报错的负例在独立脚本）；多重 `for` 中同名目标后者遮蔽前者；推导式与外层变量同名不冲突。
- `closure_capture.ms`：`[(lambda: x) for x in [1, 2, 3]]` 逐元素调用返回 1/2/3（迭代级绑定）；未被捕获的循环变量路径正确性（普通推导式回归）。
- `unpack.ms`：`for k, v in m` 键值对解包、`for i, x in enumerate(xs)` 双目标（enumerate 属任务 10 内建）、list 元素为 2 元素 list 的解包。
- 负例脚本（各配 `<name>.exit` 声明退出码 1）：`comp_non_iterable.ms`（`[x for x in 42]`）、`comp_unpack_arity.ms`（`for a, b in [[1, 2, 3]]`）、`dict_unhashable_key.ms`（`{[k]: 1 for k in ks}`）、`comp_var_leak.ms`（推导式后引用未声明的循环变量名）、`set_comp_placeholder.ms`（任务 32 前 `{x for x in xs}` 以退出码 1 失败；任务 32 落地后该脚本改写为正例，本文件清单届时由其任务接管）。

## 验收标准

- [ ] 编译器接通 `MS_AST_LIST_COMP` / `MS_AST_DICT_COMP` / `MS_AST_SET_COMP` 三种节点，采用就地展开策略：最外层 iterable 在推导式隐式作用域之外求值，循环变量经作用域栈声明、推导式结束不外泄。
- [ ] `if` 条件过滤以 `MS_OP_JUMP_IF_FALSE` 跳本级回边实现；多重 `for` 级联经逐级递归展开，内层 iterable 每轮外层迭代重新求值，各级 `if` 独立生效。
- [ ] 新增 `MS_OP_DICT_ADD` 指令并接线 `msDictSet` 语义（重复键原位覆盖、不可哈希键报错）；`MS_OP_APPEND` 的 A 操作数口径与本文统一；指令总数仍在 08-vm-internals §2.2 的 80 条目标内。
- [ ] set 推导式编译管线完整（发射 `MS_OP_BUILD_SET` / `MS_OP_SET_ADD`），任务 32 落地前 VM 分支为 "set type not implemented (task 32)" 占位运行时错误，且该占位在任务 32 验收时删除。
- [ ] 循环变量被闭包捕获时回边前发射 `MS_OP_CLOSE_UPVALS`，`[(lambda: x) for x in ...]` 各闭包读到各自迭代的值；未被捕获时无该指令。
- [ ] 推导式不干扰外层循环的 `break`/`continue` 语境；推导式栈布局纳入 `MsProto.stackSize` 统计，任意嵌套/级联下无求值栈越界。
- [ ] `tests/ms/comprehensions/` 下「测试方案」全部清单项实现并全数通过，`python run_tests.py` 退出码为 0；负例脚本以预期退出码 1 失败。
- [ ] 代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）；跨分配存活的局部 `MsObject*` 遵守根纪律，`msCloseState` 后分配统计归零；Debug 构建（ASAN / `/RTC`）无报告；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过。
- [ ] 无 TBD/TODO 占位（set 指令的 VM 占位分支为本文明确设计的过渡语义，不属此类）；与任务 07/26 的接口假定（`struct MsCompiler` 作用域接口、`MS_OP_GET_ITER`/`MS_OP_ITER_NEXT`/`MS_OP_UNPACK` 栈约定等）在实现时已对齐对应任务文档的定名。
