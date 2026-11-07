# 12 控制流语句

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ✅ | [09 最小可运行解释器](09-minimal-interpreter.md) |

## 任务目标

在任务 07 的编译器模块内落地全部控制流构造的字节码生成，使 `mslang` CLI 能正确执行以下脚本级语法（`docs/language/03-syntax.md` §3、§6）：

- `if` / `else if` / `else` 条件语句（else-if 链）；
- `while` 循环与裸 `for { ... }` 无限循环；
- Go 风格三段式 `for`（`for i := 0; i < 10; i++ { ... }`，含 `i++`/`i--` 后置语句）；
- `break` / `continue`（含嵌套循环的最内层语义）；
- 条件表达式 `a if cond else b`；
- 链式比较 `a < b <= c` 的短路展开（中间操作数只求值一次）。

交付物为编译器侧的代码生成逻辑：统一的跳转发射/回填原语、循环上下文栈（break/continue 的跳转目标管理）、以及上述各构造的指令模式。`for ... in` 迭代语句不在本任务范围（迭代协议属任务 26）；`and`/`or` 逻辑运算符的短路生成属任务 07 的表达式编译，本任务只与之共享跳转原语。完成后，脚本可以编写完整的循环与分支程序，本任务经 `tests/ms/control_flow/` 下的 ms 脚本测试验证。

## 设计依据

- [03-syntax.md](../language/03-syntax.md)
  - §3：`ifStmt` / `whileStmt` / `forStmt` / `breakStmt` / `continueStmt` 的 EBNF；`if`/`while` 条件为任意表达式、按真值规则判定、不允许赋值表达式作条件。
  - §3.1：`for` 的两种形态（三段式子句与 `in` 迭代）、`i++`/`i--` 语句仅在三段式 `for` 子句中合法、`for` 无 else 子句（刻意删减）。
  - §6：条件表达式优先级 14、右结合；比较运算符优先级 11、链式——`a < b <= c` 等价于 `(a < b) and (b <= c)` 且 `b` 只求值一次。
  - §8：块级作用域（`{}` 引入新作用域，含 `if`/`for` 块）——作用域解析本身属任务 07/11，本任务只消费其结论。
- [02-types.md](../language/02-types.md) §2：真值规则——`nil`、`false`、数值零、空字符串/字节串、空容器为假，其余为真。条件跳转的判定以此为准。
- [08-vm-internals.md](../language/08-vm-internals.md) §1（无独立优化 pass，窥孔优化只做常量折叠与死跳转消除）、§2.2（跳转类指令 `MS_OP_JUMP` / `MS_OP_JUMP_IF_FALSE` / `MS_OP_JUMP_IF_TRUE` / `MS_OP_JUMP_IF_NIL`）。
- [05 字节码格式与 MsProto](05-bytecode-proto.md)：`msProtoEmitSAx` / `msProtoPatchSAx` 构建器 API；sAx 为相对跳转指令自身下一条指令的有符号 24 位偏移（`target = pc + 1 + sAx`），先占位后回填；`MS_OP_NOP`；构建器粘性错误模型（偏移越界报 `MS_ERROR_SYNTAX`）。
- [08 VM 执行核心](08-vm-core.md)（已定稿的执行约定，本任务的生成模式必须与之吻合）：
  - 条件跳转弹栈顶并按真值规则判定，`JUMP` 无条件；VM 侧取指后 pc 已自增，故运行期目标为 `pc + sAx`。
  - 链式比较约定：`MS_OP_CMP_CHAIN`（ABC 格式），`A` = 本段比较操作码，`Bx` = 失败时相对下一条指令的无符号前向偏移；语义为弹 `right`、`left`，比较为真则压回 `right` 作为下一段左操作数并顺序执行，为假则压入 `false` 并跳转 `pc + Bx`（链尾）。
- [04 语法分析器与 AST](04-parser-ast.md)（已定稿的 AST 形状，本任务的输入）：
  - `MS_AST_IF`（else-if 链即 `elseBranch` 再挂 `MS_AST_IF`）、`MS_AST_WHILE`、`MS_AST_FOR`（`MsForKind` 区分 `MS_FOR_BARE` / `MS_FOR_CLAUSE` / `MS_FOR_IN`）、`MS_AST_BREAK`、`MS_AST_CONTINUE`、`MS_AST_INC_DEC`、`MS_AST_CONDITIONAL`、`MS_AST_COMPARE_CHAIN`（`operands` 列表 + `MsCompareOp` 数组，`not in`/`is not` 已归一为 `MS_CMP_NOT_IN`/`MS_CMP_IS_NOT`）。
  - 语境检查已在 parser 完成：循环外 `break`/`continue` 报 E209、`++`/`--` 出三段式 `for` 报 E206；编译器侧以 `MS_ASSERT` 守住不变量，不重复检查。
- [09 最小可运行解释器](09-minimal-interpreter.md)：`tests/ms/` + `run_tests.py` 测试设施（`<name>.exit` 同伴文件声明预期退出码）；内建 `assert`/`print`；编译错误 → 退出码 2、运行时错误 → 退出码 1 的链路。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [11-project-layout.md](../language/11-project-layout.md) §4：任务 09 起一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`。
- 任务 07/11 已提供实际编译接口：`struct MsCompiler` 以 `struct MsFuncCtx` 保存当前 `MsProtoBuilder`、行号、循环与栈深状态，语句/表达式入口为 `msCompilerCompileStmt` / `msCompilerCompileExpr`；`MsScopeCtx` 与 `msCompilerPushScope` / `msCompilerPopScope` 管理块作用域，`msCompilerAllocHiddenLocal` 经 `msScopeAllocHiddenLocal` 分配隐藏局部槽。

对规范歧义与空白的显式处理（实现与评审时以此为据）：

1. **三段式 `for` 的条件不可缺省。** 03-syntax §3 的 `forClause = simpleStmt ";" expr ";" simpleStmt` 要求条件为必现表达式，Go 的 `for ;; {}` 写法在 MS 中不合法；无限循环用裸 `for { ... }`（`MS_FOR_BARE`）。parser 已按此分派，编译器无需处理缺省条件。
2. **`for ... in` 推迟到任务 26。** `MS_AST_FOR` 的 `MS_FOR_IN` 形态依赖 `MS_OP_GET_ITER`/`MS_OP_ITER_NEXT` 的迭代协议语义，本任务遇到该形态按「尚未实现」报编译错误（任务 07 的通用未实现节点路径），不生成代码。
3. **链式比较的 `is`/`in` 段扩展了任务 08 的 `A` 操作数域。** 任务 08 文档把 `CMP_CHAIN` 的 `A` 描述为 `MS_OP_EQ..MS_OP_GE`；本任务把 `MS_OP_IS`/`MS_OP_IN` 纳入可用操作码（二者同样满足「比较为真则压回 `right` 继续」的语义），实现时与任务 08 的 VM 分支对齐。含 `is not`/`not in` 段的链无法用单条 `CMP_CHAIN` 表达（缺取反通道），整条链改走「隐藏临时槽 + 条件跳转」的通用展开（见「详细设计·链式比较」），保证任意 `MsCompareOp` 组合语义正确。

## 详细设计

### 模块边界

本任务不新增模块与公开头文件。生产代码改动落在 `src/compiler/ms_compiler.c` 和 `src/vm/ms_vm.c` 的既有比较分派中；新增编译器辅助函数均为文件内 `static`，循环上下文栈存放在函数级 `struct MsFuncCtx` 中。

### 跳转原语：占位发射与回填

所有控制流共用一组跳转助手，封装任务 05 的构建器（粘性错误经构建器汇聚，助手不单独返回错误）：

```c
// Emits a JUMP-family instruction with placeholder sAx = 0; returns its pc
// for later patching. op must be one of MS_OP_JUMP / MS_OP_JUMP_IF_FALSE /
// MS_OP_JUMP_IF_TRUE / MS_OP_JUMP_IF_NIL (MS_ASSERT'd).
static int msCompilerEmitJumpPlaceholder(struct MsCompiler* c, MsOpCode op);

// Back-patches the jump at jumpPc so that it lands on the instruction that
// will be emitted next (i.e. the current pc): sAx = curPc - (jumpPc + 1).
static void msCompilerPatchToCurrent(struct MsCompiler* c, int jumpPc, int expectedStackDepth);

// Emits a back edge: MS_OP_JUMP whose sAx points back to targetPc
// (sAx = targetPc - (pc + 1), negative).
static void msCompilerEmitBackEdge(struct MsCompiler* c, int targetPc);

// Current pc (next instruction index) of the proto under construction.
static int msCompilerCurrentPc(struct MsCompiler* c);
```

条件跳转（`JUMP_IF_FALSE` 等）弹栈顶判定，故编译条件表达式后栈上恰好留一个待弹值，每个分支入口的栈深度一致——这是各生成模式共同的栈平衡不变量。回填助手的 `expectedStackDepth` 用 `MS_ASSERT` 校验该不变量。

死跳转消除（08-vm-internals §1 允许的唯一跳转类窥孔）：`msCompilerPatchToCurrent` 回填无条件 `JUMP` 时若目标恰为 `jumpPc + 1`（跳转到下一条指令），把该指令改写为 `MS_OP_NOP`。**条件跳转不做此改写**——它有弹栈副作用，删掉会打破栈平衡。

### 循环上下文栈（break/continue 目标管理）

`break`/`continue` 的目标地址在语句出现处大多尚未确定（`break` 的循环末尾、`for` 的后置语句），需要暂存跳转 pc、待目标确定后统一回填。编译器维护一个循环上下文栈：

```c
#define MS_COMPILER_LOOP_INIT_CAP 8

struct MsJumpPatchList {       // pcs 经 msAlloc/msRealloc 增长、msFree 释放
  int* pcs;                    // 待回填的跳转指令 pc 列表
  int count;
  int cap;
};

struct MsLoopContext {
  int loopHead;                // 循环头 pc（条件求值处 / 裸循环体首指令）
  struct MsJumpPatchList breaks;     // 回填到循环末尾
  struct MsJumpPatchList continues;  // 回填到 continue 目标（仅三段式 for 使用）
  bool continueIsHead;         // true：continue 直接回边到 loopHead（while / 裸 for）
};

// struct MsFuncCtx 内的函数级字段：
//   struct MsLoopContext* loops;   // msRealloc'd dynamic array, capacity-grown
//   int loopDepth;
//   int loopCap;
```

内部助手（文件内 `static`）：

```c
// Pushes a loop context; loopHead must already be known (it is the pc of the
// first instruction of the condition or, for a bare loop, of the body).
static bool msCompilerLoopEnter(struct MsCompiler* c, int loopHead, bool continueIsHead);

// Patches the conditional exit and breaks, then releases the top context. The
// caller must have resolved continues already (continues.count == 0).
static void msCompilerLoopLeave(struct MsCompiler* c, int exitJump, int expectedStackDepth);

// MS_AST_BREAK / MS_AST_CONTINUE codegen. loopDepth == 0 is a programming
// error (the parser already rejected it with E209) — MS_ASSERT'd.
static void msCompilerCompileBreak(struct MsCompiler* c);
static void msCompilerCompileContinue(struct MsCompiler* c);

// Patches the top context's continue list to the current pc; called by the
// three-part-for codegen when it reaches the post-statement position.
static void msCompilerResolveContinues(struct MsCompiler* c, int expectedStackDepth);
```

语义要点：

- 嵌套循环：`break`/`continue` 只作用于栈顶上下文（最内层循环），与 03-syntax 的常规语义一致；每个函数使用独立的 `MsFuncCtx` 循环栈，保证循环内的闭包不能 `break` 穿透——parser 的 `loopDepth` 复位已先行拒绝此类源码。
- `continue` 的两类目标：`while`/裸 `for` 的目标（循环头）在进入循环时已知，`msCompilerCompileContinue` 直接 `msCompilerEmitBackEdge(c, loopHead)`，不使用暂存列表；三段式 `for` 的目标（后置语句起点）在编译循环体时未知，暂存于 `continues`，编译到后置语句位置时经 `msCompilerResolveContinues` 统一回填。`continueIsHead` 标记区分两种策略。
- 容量：`loops` 数组与两个暂存列表均按需倍增增长，无固定嵌套上限；增长失败经粘性错误路径报告 `MS_ERROR_OOM`。
- 本任务不新增编译错误码：循环外 `break`/`continue` 已由 parser E209 拒绝；跳转偏移超 sAx 24 位范围由构建器报 `MS_ERROR_SYNTAX`。

### if / else if / else 生成模式

`MS_AST_IF`（else-if 链即 `elseBranch` 再挂 `MS_AST_IF`）递归生成：

```
<cond>                        # msCompilerCompileExpr(cond)
JUMP_IF_FALSE -> Lelse        # 占位，弹栈顶判定
<thenBlock>                   # 块作用域进出走任务 07 钩子
JUMP -> Lend                  # 仅当 elseBranch 非空时发射；else-if 链上每个
                              # 分支的此跳转 pc 收集到本层局部列表，链尾统一回填
Lelse:                        # patchToCurrent(JUMP_IF_FALSE)
<elseBranch>                  # else 块，或嵌套 MS_AST_IF 递归（else if）
Lend:                         # 逐一回填收集到的 JUMP
```

无 `else` 时不发射 `JUMP -> Lend`，`Lelse` 与 `Lend` 重合。条件即真值判定，不要求布尔类型（02-types §2）。

### while 与裸 for 生成模式

`MS_AST_WHILE`：

```
Lhead:                        # loopEnter(loopHead = 此处 pc, continueIsHead = true)
<cond>
JUMP_IF_FALSE -> Lend         # 占位
<body>
JUMP -> Lhead                 # 回边（msCompilerEmitBackEdge）
Lend:                         # loopLeave：回填 JUMP_IF_FALSE 与全部 breaks
```

`MS_FOR_BARE`（`for { body }`）：无条件与条件跳转，循环头即循环体首指令：

```
Lhead:                        # loopEnter(loopHead = 此处 pc, continueIsHead = true)
<body>
JUMP -> Lhead
Lend:                         # 回填全部 breaks；正常执行不会到达 Lend，
                              # 只有 break 能离开（死跳转消除不适用回边）
```

### 三段式 for 生成模式

`MS_FOR_CLAUSE`（`for init; cond; post { body }`）：

```
<init>                        # 声明/赋值/表达式语句；循环级作用域由任务 07/11 管理
Lhead:                        # loopEnter(loopHead = 此处 pc, continueIsHead = false)
<cond>
JUMP_IF_FALSE -> Lend         # 占位
<body>
Lpost:                        # msCompilerResolveContinues：continue 落到这里
<post>                        # 通常为 MS_AST_INC_DEC（见下）
JUMP -> Lhead                 # 回边
Lend:                         # loopLeave：回填 JUMP_IF_FALSE 与全部 breaks
```

- `continue` 落在 `post` **之前**：保证 `for i := 0; i < 10; i++` 中 `continue` 仍执行 `i++`，与 Go/C 语义一致。这是三段式 `for` 必须使用 `continues` 暂存列表而不能直接回边到 `loopHead` 的原因。
- `MS_AST_INC_DEC`（`i++`/`i--`，仅出现于 `post` 位置，parser 已限定）：编译为「载入目标 → 压入常量 1 → `MS_OP_ADD`/`MS_OP_SUB` → 存回目标」，读写目标走任务 07 的名字解析结论（全局 `LOAD_GLOBAL`/`STORE_GLOBAL` 或局部槽）；该节点不产生栈残留。
- `init`/`post` 的语句级编译复用任务 07 的 `msCompilerCompileStmt`，本任务只提供循环骨架与回填时序。

### 条件表达式生成模式

`MS_AST_CONDITIONAL`（`value if cond else b`，优先级 14、右结合——结合性已由 parser 定形，编译器按树直译）：

```
<cond>
JUMP_IF_FALSE -> Lelse        # 占位，弹栈顶
<value>                       # 真分支：栈上留一个结果值
JUMP -> Lend                  # 占位
Lelse:
<elseValue>                   # 假分支：栈上留一个结果值
Lend:
```

两个分支各留恰好一个栈值，汇合点栈深度一致；`else` 分支必现（条件表达式语法要求），不存在无 `else` 形态。

### 链式比较的短路展开

`MS_AST_COMPARE_CHAIN`：`operands` 为 n+1 个操作数、`ops` 为 n 个 `MsCompareOp`（n ≥ 1）。n == 1 的单段比较（普通 `a < b`）由任务 07 的表达式编译直接发射二元比较指令，不属本任务；本任务处理 n ≥ 2 的链。

**主路径（全部段的操作码 ∈ {EQ, NE, LT, LE, GT, GE, IS, IN}）**：按任务 08 的 `CMP_CHAIN` 约定生成，中间操作数只求值一次、失败即短路：

```
<operands[0]>
<operands[1]>
CMP_CHAIN A=op0, Bx=0         # 占位；真则压回 operands[1] 作下一段左操作数，
                              # 假则压入 false 跳链尾
<operands[2]>
CMP_CHAIN A=op1, Bx=0
...
<operands[n]>
<op(n-1)>                     # 末段用普通二元比较指令（EQ/NE/LT/.../IS/IN），
                              # 弹二压一布尔，链上留下最终结果
Lend:                         # 把所有 CMP_CHAIN 的 Bx 回填到此处
                              # （Bx = Lend - (chainPc + 1)，前向无符号）
```

`MsCompareOp` → `MsOpCode` 由文件内 `static` 函数 `msCompilerComparisonOpcode` 的 switch 完成；`MS_CMP_IS`/`MS_CMP_IN` 映射到 `MS_OP_IS`/`MS_OP_IN`（见「设计依据」歧义说明第 3 条，实现时与任务 08 的 `CMP_CHAIN` 分支对齐）。

**通用展开（链中含 `MS_CMP_IS_NOT` 或 `MS_CMP_NOT_IN` 段）**：此类段需要取反，`CMP_CHAIN` 无法表达，整条链改用「隐藏临时槽 + 条件跳转」展开，语义与 `and` 连接等价且中间操作数仍只求值一次：

```
<operands[0]>                 # 仅首段直接生成 left
Lsegment:
<operands[i+1]>               # 栈：left right
STORE_LOCAL tmp               # tmp = right（编译器预留的隐藏槽）；栈：left
LOAD_LOCAL tmp                # 栈：left right
<opi 对应二元指令>            # IS_NOT/NOT_IN 段先发射 IS/IN 再补一条 MS_OP_NOT
JUMP_IF_FALSE -> Lfalse       # 短路：任一段失败即结束
LOAD_LOCAL tmp                # 仅非末段成功路径：right 成为下一段 left
...                           # 回到 Lsegment；单条链逐段复用同一 tmp
LOAD_TRUE                     # 全部段成功后的最终布尔结果
JUMP -> Lend
Lfalse:
LOAD_FALSE                    # 任一段失败后的最终布尔结果
Lend:                         # 两条路径都恰留一个布尔，而非保存的 right
```

隐藏临时槽是编译器在当前帧窗口内保留的匿名局部槽（不计入脚本可见名字），经实际钩子 `msCompilerAllocHiddenLocal` 调用 `msScopeAllocHiddenLocal` 获取；一个 Proto 内按链的最大并发需求复用，计入 `localCount`——这修正了任务 09「无嵌套函数时 `localCount` 可为 0」的简化：顶层代码若含走通用展开的链式比较，`localCount` 可大于 0。单条链逐段复用同一槽即可（段间不并行），嵌套链各自占槽。

### 行号登记

每条语句/表达式发射首条指令前，以 AST 节点的 `line` 调 `msProtoSetLine`（任务 05 构建器的游程编码自动去重）；条件跳转与回边登记所属语句行，保证任务 08 的错误行号与将来的调试信息准确。

## 实现步骤

1. 在编译器模块内实现跳转原语四助手（占位发射、`patchToCurrent`、回边、当前 pc）与无条件 `JUMP` 的死跳转 NOP 改写。验证：临时挂接 `if` 最简路径后，`if true { print("a") }` 脚本通过。
2. 实现 `MS_AST_IF` 生成（含 else-if 链的 JUMP 收集与链尾统一回填）。验证：`tests/ms/control_flow/if_else.ms` 全绿。
3. 实现循环上下文栈（动态数组 + 两个暂存列表的生命周期）与 `MS_AST_WHILE`、`MS_FOR_BARE` 生成。验证：`while_loop.ms`、`bare_for.ms` 全绿。
4. 实现 `break`/`continue`（两类 continue 策略）与嵌套循环。验证：`break_continue.ms` 全绿。
5. 实现三段式 `for` 骨架与 `MS_AST_INC_DEC`（含 `continues` 在后置语句位置的回填时序）。验证：`for_clause.ms` 全绿，含 `continue` 仍执行 `i++` 的用例。
6. 实现条件表达式生成。验证：`cond_expr.ms` 全绿（含右结合嵌套）。
7. 实现链式比较主路径（`msCompilerComparisonOpcode`、`CMP_CHAIN` 序列、末段普通比较、Bx 回填）并与任务 08 的 `CMP_CHAIN` 分支对齐 IS/IN 扩展。验证：`chain_compare.ms` 的数值链用例全绿。
8. 实现含 `is not`/`not in` 段的通用展开与隐藏临时槽复用。验证：`chain_compare.ms` 的 `is not` 链用例全绿。
9. 负向用例与回归：循环外 `break`/`continue` 的退出码 2 用例；任务 09–11 既有脚本全数保持通过；`python run_tests.py` 全绿；Windows MSVC Debug/Release 构建及完整 CTest 通过，编译器 OOM 注入 fixture 以零存活分配结束。Linux/macOS 与 ASAN 留待相应环境验证。

## 测试方案

本任务位于任务 09 之后，脚本测试一律用内建 `assert` + `print`（任务 40 之前），由仓库根 `run_tests.py` 调用 `mslang` CLI 驱动，负向用例配 `.exit`、`.err` 同伴文件。脚本位于 `tests/ms/control_flow/`，字节码形状和分配失败路径由 `tests/c/test_compiler.c` 补充验证。本任务阶段脚本使用 nil/bool/int/float/str，不依赖任务 13 的函数或任务 16 的容器执行语义。

实际覆盖清单：

- `if_else.ms`：单分支、双分支、else-if 链、真值规则、嵌套 `if` 与块作用域。
- `while_loop.ms`：累加、首轮即假、嵌套 `while`；`bare_for.ms`：裸循环的 `break`/`continue`。
- `for_clause.ms`：`i++`/`i--`、`+=` 步进、零次循环、循环变量重声明及外层同名变量遮蔽、全局索引 `=` 初始化；`break_continue.ms`：深层 if、两种循环方向嵌套、最内层语义，以及三段式 `for` 的 post 执行或跳过。
- `cond_expr.ms`：真/假路径、右结合、算术与比较优先级；以变量条件覆盖未被常量折叠的运行期条件跳转。
- `chain_compare.ms`：数值与 `IS` 直接链、含 `is not` 的通用链、超过八个非末段的长链；首段和后续段失败均以 `1 / 0` 验证短路。
- `tests/c/test_compiler.c`：直接链 `CMP_CHAIN` A/Bx 与栈元数据、非法操作码预检；九层循环和超过八项的跳转列表、延迟 continue、嵌套函数清理、长链及 `index++` 的逐点 OOM 注入，迭代至成功并检查零存活分配。
- 负向用例：循环外 `break`/`continue` 的 E209；`err_for_inc_dec_undeclared.ms` 的 E301、`err_for_inc_dec_indexed.ms` 的 E307；`err_for_clause_scope.ms` 的循环后未定义 `i`；`err_in_numeric.ms` 与 `err_chain_in_numeric.ms` 的普通/链式数值 `IN` TypeError。

中间操作数在两条比较链路径中均只生成一次求值代码；带副作用函数的精确调用计数直证留待任务 13。`for ... in` 仍由任务 26 实现；数值 `IN` 负例只证明当前 TypeError 边界，容器成员执行仍属任务 16。

## 验收标准

- [x] `if`/`else if`/`else`、`while`、裸 `for`、三段式 `for`、`break`/`continue`、条件表达式、链式比较的生成模式与本文「详细设计」一致；生产改动限编译器及 VM 既有比较分派，新增编译辅助函数为文件内 `static`，遵循 10-c-style。
- [x] 跳转统一经占位发射 + `msProtoPatchSAx` 回填，sAx 偏移约定（相对下一条指令）与任务 05/08 一致；无条件 `JUMP` 跳转到下一条指令时改写为 `MS_OP_NOP`，条件跳转不改写。
- [x] 循环上下文栈按需动态增长；`break` 跳到最内层循环末尾；`continue` 在 `while`/裸 `for` 中回到循环头、在三段式 `for` 中落在后置语句之前（`i++` 必执行）。
- [x] 直接链按 `CMP_CHAIN` 约定生成并接纳 IS/IN；含 `is not`/`not in` 的链走隐藏临时槽通用展开，非末段成功重载 right、全成功压入 true、失败压入 false；两条路径均只生成一次中间操作数求值代码并失败短路。
- [x] `for ... in` 保持任务 26 的明确 E307 边界，n == 1 的普通比较沿用二元指令路径；普通及链式数值 `IN` 保持 TypeError，容器成员执行不在本任务范围。
- [x] 循环外 `break`/`continue` 编译失败、CLI 退出码为 2，诊断含文件/行/列与 E209；递增/递减未声明目标、索引目标及循环变量作用域负例通过。
- [x] `python run_tests.py --mslang build/Debug/mslang.exe` 发现含 `tests/ms/control_flow/` 在内的全部脚本并全数通过，任务 09–11 既有脚本无回归。
- [x] 当前 Windows MSVC 主机的 Debug/Release 构建与各自完整 CTest 通过；循环、比较链和隐藏槽的 OOM/错误路径由编译器 fixture 覆盖并检查零存活分配；构建产物只落在 `build/`。
- [x] 无 TBD/TODO 占位；实际 `MsFuncCtx`、`msCompilerCompileStmt`/`msCompilerCompileExpr`、隐藏槽分配钩子已与代码对齐，VM 的 `CMP_CHAIN` IS/IN 扩展已对齐。
- [ ] 任务 13 后以带副作用函数精确验证中间操作数调用次数。
- [ ] Linux/macOS Debug/Release 与 ASAN 验证；当前 Windows MSVC worktree 未执行，不作完成声明。
