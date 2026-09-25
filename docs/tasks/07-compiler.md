# 07 编译器：作用域解析与字节码生成

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [04 语法分析器与 AST](04-parser-ast.md)、[05 字节码格式与 MsProto](05-bytecode-proto.md) |

## 任务目标

交付 mslang 的编译器模块（`src/compiler/ms_compiler.h` / `src/compiler/ms_compiler.c`）：消费任务 04 产出的 AST，**单趟遍历**直接生成任务 05 定义的字节码——无独立 IR、无独立优化 pass（[08-vm-internals.md](../language/08-vm-internals.md) §1）。核心职责：

- **作用域解析**：块级作用域（`{}` 引入新作用域）的词法作用域栈、局部槽位分配与回收（兄弟块复用槽位）、名字解析（局部 → 闭包外层 → 模块全局/内建）；模块顶层深度 0 的 `:=` 走全局命名空间（延续 [09 最小可运行解释器](09-minimal-interpreter.md) §3 的约定）。
- **字节码生成框架**：语句/表达式分派入口、跳转发射与回填原语、逐指令的求值栈深跟踪（产出 `MsProto.stackSize`）、行号登记、编译段错误码 E3xx 与统一的「尚未实现」占位路径。
- **窥孔优化（仅两项）**：常量折叠与死跳转消除，均在生成/回填过程中内联完成。
- **Proto 嵌套结构**：每个函数（含模块顶层）编译为一个 `MsProto`；嵌套函数的子 Proto 存入外层 Proto 的常量池，定义点经 `MS_OP_MAKE_FUNCTION` / `MS_OP_MAKE_LAMBDA` 物化。

完成后，[09 最小可运行解释器](09-minimal-interpreter.md) 可以经 `msCompilerCompile` 把模块顶层编译为 `MsProto` 并交任务 08 执行；任务 11/12/13/14 在本任务的骨架上分别叠加严格声明检查、控制流、完整调用约定与闭包捕获。本任务自身经 `tests/c/test_compiler.c` 的 C 单元测试独立验证。

## 设计依据

- [08-vm-internals.md](../language/08-vm-internals.md) §1：编译管线定位（AST → Compiler → Proto）；**无独立优化 pass**，窥孔优化只做常量折叠与死跳转消除，编译器一次遍历生成；每个函数（含模块顶层）编译为一个 `MsProto`，嵌套函数是外层 Proto 常量池中的子 Proto；编译错误收集模式（单文件最多 20 条后中止）。
- [03-syntax.md](../language/03-syntax.md) §8：块级作用域；函数内赋值默认局部，`global` 声明写模块级；闭包按引用捕获（任务 14）；名字解析顺序「局部 → 闭包外层 → 模块全局 → 内建」。§3/§4：语句与函数形式。
- [04-parser-ast.md](04-parser-ast.md)：`struct MsAst` / `MsAstKind` 节点形态（`MS_AST_SOURCE_FILE` 根、`literal`/`name` 源码切片、`binary`/`unary`/`compareChain`/`conditional`/`call`/`decl`/`func` 等联合分支）、arena 生命周期（不得超过源码缓冲区）、字符串转义解码由编译期经 `msLexerUnescape`（任务 03）完成。
- [05-bytecode-proto.md](05-bytecode-proto.md)：`MsOpCode` v0.1 子集与操作数约定（`LOAD_LOCAL`/`STORE_LOCAL` 的 `A` 为槽位、`LOAD_GLOBAL`/`STORE_GLOBAL` 的 `Bx` 为名字常量池下标、`MAKE_FUNCTION`/`MAKE_LAMBDA` 的 `Bx` 为子 Proto 常量池下标、跳转 `sAx` 相对下一条指令）、`struct MsProtoBuilder` 全部构建器 API（粘性错误模型）、常量池去重与上限 65536、「指令流末尾由编译器保证以 `MS_OP_RETURN` 结束」。
- [06-object-model.md](06-object-model.md)（已成文）：常量对象构造 `msIntNew` / `msFloatNew` / `msStringNew` / `msStringFromCStr` / `msHeapNil` / `msHeapBool`、堆上下文 `struct MsHeap`、值相等 `msObjectValueEquals`（常量池值去重已随任务 06 接入）。README 依赖图未列 T06→T07 的边，但常量池构造必然引用任务 06 接口，本文直接引用其定名。
- [08-vm-core.md](08-vm-core.md)：帧窗口布局（`localCount` 槽 + `stackSize` 暂存区）；`CMP_CHAIN` 操作数约定已定稿于该文，**本任务不生成 `CMP_CHAIN`**（n ≥ 2 链式比较属 [12 控制流语句](12-control-flow.md)）。
- [09-minimal-interpreter.md](09-minimal-interpreter.md) §3：顶层 Proto 约定（`name = "<main>"`、`paramCount = 0`、末尾补 `RETURN`、顶层 `:=` 走 `STORE_GLOBAL`）。
- [10-c-style.md](../language/10-c-style.md)：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则、§5 错误处理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）、§8 断言。
- [11-project-layout.md](../language/11-project-layout.md)：`src/compiler/` 目录位置；`tests/c/` 用自研 `ms_test.h`（任务 01 提供）。
- 任务 02 核心基础设施（[02-core-infrastructure.md](02-core-infrastructure.md)，已成文）提供：`msAlloc/msRealloc/msFree` 与分配统计 `msMemGetStats`/`msMemResetStats`/`msMemSetFailAfter`（泄漏断言与 OOM 注入）、`MsResult`（`include/mslang/error.h`）、`MS_ASSERT`（`src/core/ms_common.h`）、`struct MsDiagList` 与 `msDiagReport`（`src/core/ms_diag.h`，与词法/语法错误共享单文件 20 条上限）。

对规范与既定文档的处理（实现与评审时以此为据）：

1. **与任务 11/12/13/14 的边界。** 这些任务的文档写作时本任务未定稿，其中「新增某某生成」的表述以本文为基准对齐为**扩展关系**：本任务交付编译器骨架与基础子集（字面量、标识符、算术/位/逻辑/条件表达式、单段比较、位置参数调用、单目标 `:=`/`=`、块、表达式语句、`return` 零/单值、基础 `func`/`lambda` 编译）；任务 11 叠加 `:=`/`=` 严格检查与多目标/解包，任务 12 叠加控制流语句与链式比较，任务 13 叠加默认参数/`*args`/`**kwargs`/`CALL_KW`/`TAIL_CALL`/多值返回，任务 14 叠加 upvalue 捕获。本任务对未覆盖的 AST 节点一律走统一的「尚未实现」占位路径（E307），由对应任务逐项替换。
2. **表达式语句的丢弃机制。** 任务 05 的操作码枚举无 `POP`；本任务定名丢弃机制为**每函数一个隐藏槽位**：表达式语句求值后 `STORE_LOCAL` 写入该槽（惰性分配、全函数复用、计入 `localCount`）。任务 45 假定的 `MS_OP_POP` 以本约定为准，不新增操作码。由此模块顶层的 `localCount` 也可大于 0——对齐任务 12 对任务 09「`localCount` 可为 0」简化的既有修正。
3. **`and`/`or` 的求值语义。** 条件跳转弹栈（任务 05/08 约定）且无 `DUP` 指令，为保持「`a and b` 在 `a` 为假时结果为 `a`」的 Python 语义，短路生成经隐藏槽中转（见「逻辑表达式」），栈深两条路径一致。
4. **常量折叠白名单。** 只折叠结果与运行时语义**逐位一致**的情形：数值 `+ - *` 与一元 `+ - ~`、`not`；int 运算先检出溢出，溢出即不折叠（保留任务 06/08 的运行时错误路径）；`/` `div` `%` `**`、比较、字符串运算一律不折叠（保留除零等运行时错误与 NaN 语义）。
5. **一元 `+`。** 规范未定义其对非数值的行为；字面量经常量折叠，非字面量生成 `LOAD_CONST(0)` + `MS_OP_ADD`，把类型检查留给 VM 的 `ADD` 慢路径。
6. **「内建」解析段无独立表。** 内建函数由任务 09/10 预注册进全局命名空间，解析落到 `LOAD_GLOBAL`；读取未声明名不做编译期检查，推迟到运行时「undefined name」错误（与任务 11 歧义说明第 2 条一致）。
7. **名字解析的「闭包外层」段。** 本任务解析到外层函数局部时报编译错误 E310（upvalue 属任务 14；与任务 13 歧义说明第 6 条一致），解析结构（`MS_RESOLVE_UPVALUE`）先行定义。
8. **编译段错误码 E3xx 的登记处以本文为准**（任务 11/13 已声明「定名以任务 07 为准」）：E301–E306 按任务 11 已定含义收录，本任务自用码自 E307 起（见「错误码表」）。
9. **接口定名。** 语句/表达式分派入口定名 `msCompilerCompileStmt` / `msCompilerCompileExpr`（统一任务 12 假定的 `msCompileStmt`/`msCompileExpr` 与任务 28 假定的 `msCompilerCompileExpr`）；作用域钩子定名 `msCompilerPushScope`/`msCompilerPopScope`/`msCompilerDeclareLocal`（任务 27/29 的假定名）；隐藏槽分配定名 `msCompilerAllocHiddenLocal`（统一任务 12 的 `msCompilerAllocHiddenLocal` 与任务 29 的 `msCompilerDeclareHiddenLocal`）；REPL 选项位定名 `MS_COMPILE_REPL`（任务 22 的假定名）；编译入口定名 `msCompilerCompile`（任务 09 管线图所用名；任务 24 假定的 `msCompileFile` 为后续任务的文件级封装，不在本任务）。实现时以上述定名为准回改各任务文档引用。

## 详细设计

### 文件与模块边界

- `src/compiler/ms_compiler.h`（guard `MSLANG_SRC_COMPILER_MS_COMPILER_H_`）：`MsCompileFlags`、`struct MsCompiler` 完整定义（任务 12 需向其内嵌循环上下文字段，故结构体必须在头文件可见）、公开函数声明。自包含：include `<stdint.h>`、任务 02 的 `"mslang/error.h"` 与 `"core/ms_diag.h"`；前向声明 `struct MsAst`、`struct MsHeap`、`struct MsProto`、`struct MsFuncCtx`。
- `src/compiler/ms_compiler.c`：函数编译上下文、作用域栈、分派与生成、折叠与回填的全部实现；除公开函数外一律文件内 `static`。
- 内存纪律：编译期堆分配仅函数编译上下文（`msAlloc`/`msFree`，随 `msCompilerPopFunc` 对称释放）；常量对象经任务 06 构造器分配在 `MsHeap` 上，所有权随常量池移交（GC 落地前由任务 09 的「关闭时统一回收」兜底）；产出的 `MsProto` 由调用方以 `msProtoFree` 释放。AST 与源码缓冲区的生命周期归调用方，编译器只读。

### 公开接口

```c
typedef enum {
  MS_COMPILE_DEFAULT = 0,
  MS_COMPILE_REPL = 1u << 0    // task 22: echo the last top-level expression via MS_OP_PRINT_EXPR
} MsCompileFlags;

struct MsCompiler {
  struct MsHeap* heap;         // borrowed: constant objects are allocated here
  struct MsDiagList* diags;    // borrowed: shared with lexer/parser (per-file cap 20)
  const char* sourceFile;      // borrowed chunk name -> proto->sourceFile
  uint32_t flags;              // MsCompileFlags bitmask
  struct MsFuncCtx* fn;        // current (innermost) function context
  int nodeDepth;               // AST recursion budget (MS_COMPILER_MAX_NODE_DEPTH)
  MsResult sticky;             // first failure; codegen short-circuits once set
};

// heap, diags and sourceFile must outlive the compiler; none is copied.
void msCompilerInit(struct MsCompiler* c, struct MsHeap* heap, struct MsDiagList* diags,
    const char* sourceFile, uint32_t flags);
void msCompilerDestroy(struct MsCompiler* c);

// Compiles an MS_AST_SOURCE_FILE tree into the module top-level proto
// (name "<main>"). On success *out is owned by the caller (msProtoFree).
// Errors are recorded in diags; returns MS_ERROR_SYNTAX or MS_ERROR_OOM.
MsResult msCompilerCompile(struct MsCompiler* c, const struct MsAst* root, struct MsProto** out);
```

粘性错误模型与任务 05 的构建器一致：任何生成失败（诊断满、OOM、规模超限）记入 `c->sticky` 后，后续生成调用短路；`msCompilerCompile` 集中返回。

### 函数编译上下文与作用域栈

```c
#define MS_COMPILER_MAX_LOCALS 256        // LOAD_LOCAL/STORE_LOCAL 的 A 为 8 位
#define MS_COMPILER_MAX_SCOPE_DEPTH 128   // 块嵌套深度上限（防御性）
#define MS_COMPILER_MAX_NODE_DEPTH 512    // AST 递归预算（防御病态嵌套，f-string 等复用）

struct MsLocalSlot {
  struct MsString* name;   // interned, borrowed from the heap; NULL = hidden slot
  int depth;               // block depth at declaration (0 = function body level)
  bool isCaptured;         // reserved for task 14 upvalue capture; always false here
};

struct MsFuncCtx {                 // one per function/module-top proto; file-local to ms_compiler.c
  struct MsFuncCtx* parent;        // enclosing function context (NULL at module top)
  struct MsProtoBuilder builder;   // task 05 builder, embedded
  const struct MsAst* funcNode;    // MS_AST_FUNC_DECL / MS_AST_LAMBDA; NULL for module top
  struct MsLocalSlot locals[MS_COMPILER_MAX_LOCALS];
  int localCount;                  // currently live slots
  int localMax;                    // high-water mark -> proto->localCount
  int scopeDepth;                  // 0 = function body top level; +1 per '{'
  int stackDepth;                  // current eval-stack usage above the register window
  int stackMax;                    // high-water mark -> proto->stackSize
  int discardSlot;                 // hidden slot for expression-statement results; -1 until used
  bool isModule;                   // module top level ("<main>")
  bool isAsync;
};
```

作用域与槽位操作（文件内 `static`；即任务 27/29/12 假定的作用域钩子的定名）：

```c
// Enters a block scope. Overflow of MS_COMPILER_MAX_SCOPE_DEPTH reports E306.
static void msCompilerPushScope(struct MsCompiler* c);

// Leaves a block scope: rewinds localCount to the scope base so sibling blocks
// reuse slots. Checkpoint for task 14: if a popped slot has isCaptured set,
// emit MS_OP_CLOSE_UPVALS here (never triggers in this task).
static void msCompilerPopScope(struct MsCompiler* c);

// Declares a named local at the current depth; returns its slot, or -1
// (caller reports E306). Shadowing falls out of innermost-first lookup.
static int msCompilerDeclareLocal(struct MsCompiler* c, struct MsString* name);

// Allocates a hidden (name == NULL, script-unreachable) local slot; same
// limits and rewind discipline as named locals. Used by logical-expression
// short-circuiting and the expression-statement discard slot.
static int msCompilerAllocHiddenLocal(struct MsCompiler* c);

// Innermost-first lookup among live locals of the CURRENT function context;
// returns the slot or -1.
static int msCompilerResolveLocal(struct MsCompiler* c, const struct MsString* name);
```

**模块顶层的双层语义**（与任务 11 §3 的既定规则一致）：`isModule` 且 `scopeDepth == 0` 时的 `:=` 目标**不入局部表**，直接生成 `STORE_GLOBAL`；进入任何块（深度 ≥ 1）后 `:=` 产生块局部。函数上下文的所有声明一律入局部表。

### 名字解析

```c
typedef enum {
  MS_RESOLVE_LOCAL,      // LOAD_LOCAL/STORE_LOCAL with *slot
  MS_RESOLVE_UPVALUE,    // structural result; emission is task 14 (E310 in this task)
  MS_RESOLVE_GLOBAL      // LOAD_GLOBAL/STORE_GLOBAL; runtime "undefined name" on miss
} MsResolveKind;

// Resolves name per 03-syntax §8: current-function locals (innermost first),
// then enclosing function contexts (found -> MS_RESOLVE_UPVALUE), then global
// (module-level names and builtins share the global namespace; no separate
// builtin table). *slot is valid for MS_RESOLVE_LOCAL.
static MsResolveKind msCompilerResolveName(struct MsCompiler* c, const struct MsString* name, int* slot);
```

标识符发射辅助 `msCompilerEmitLoadName` / `msCompilerEmitStoreName`（`static`）：按解析结果发射 `LOAD_LOCAL A` / `LOAD_GLOBAL Bx`（名字符串经 `msStringNew` + `msProtoAddConst` 入池）等；`MS_RESOLVE_UPVALUE` 报 E310。

### 发射与回填原语

```c
// Emits one ABC instruction with the current node line; stackDelta updates
// stackDepth/stackMax (MS_ASSERT(stackDepth >= 0)). Returns pc or -1 (sticky).
static int msCompilerEmitABC(struct MsCompiler* c, MsOpCode op, uint8_t a, uint16_t bx, int stackDelta);

// Emits a jump with placeholder offset; returns its pc for later patching.
static int msCompilerEmitJumpPlaceholder(struct MsCompiler* c, MsOpCode op);

// Current instruction count of the innermost builder (== next pc).
static int msCompilerCurrentPc(const struct MsCompiler* c);

// Back-patches jumpPc to the current pc via msProtoPatchSAx.
// Dead-jump elimination: an unconditional MS_OP_JUMP whose target is the very
// next instruction (target == jumpPc + 1) is rewritten to MS_OP_NOP, keeping
// codeLen stable so previously recorded pcs stay valid. Conditional jumps are
// never rewritten (they pop the stack; deleting one would break stack
// balance). sAx range overflow is mapped to E309 via the builder's sticky
// error.
static void msCompilerPatchToCurrent(struct MsCompiler* c, int jumpPc);
```

栈深不变量：编译器生成的每条路径在汇合点栈深一致（由生成模式保证）；回填点以 `MS_ASSERT` 校验当前 `stackDepth` 等于该跳转发射时记录的期望值。每个 Proto 定稿时 `stackSize = stackMax`、`localCount = localMax`。

### 语句与表达式分派

```c
static void msCompilerCompileStmt(struct MsCompiler* c, const struct MsAst* node);
static void msCompilerCompileExpr(struct MsCompiler* c, const struct MsAst* node);
```

入口先检查 `c->sticky`（短路）、递增 `nodeDepth`（超限报 E311）、`msProtoSetLine(&fn->builder, node->line)`。本任务的分派覆盖：

| 节点 | 生成 | 备注 |
|---|---|---|
| `MS_AST_SOURCE_FILE` | 逐语句编译；REPL 模式下最后一条顶层表达式语句改发 `PRINT_EXPR` | 见「模块顶层与 REPL」 |
| `MS_AST_BLOCK` | `msCompilerPushScope` → 逐语句 → `msCompilerPopScope` | |
| `MS_AST_EXPR_STMT` | 求值 + `STORE_LOCAL discardSlot`（惰性分配） | 歧义说明第 2 条 |
| `MS_AST_DECL` / `MS_AST_ASSIGN` | 单目标标识符形式：RHS 先生成，再声明/解析并 `STORE_*` | `x := x` 读外层旧值；多目标/解包/严格检查属任务 11，本任务报 E307 |
| `MS_AST_PASS` | 无代码 | |
| `MS_AST_RETURN` | 零值 `LOAD_NIL + RETURN`；单值 `expr + RETURN` | 多值属任务 13（E307） |
| `MS_AST_FUNC_DECL` | 见「函数编译」 | 默认参数/`*`/`**` 报 E307（任务 13） |
| 字面量 / `MS_AST_IDENT` | 见「常量与名字」 | |
| `MS_AST_UNARY` / `MS_AST_BINARY` / `MS_AST_LOGICAL` / `MS_AST_CONDITIONAL` / `MS_AST_COMPARE_CHAIN`（单段）/ `MS_AST_CALL`（纯位置） | 见下 | 链 n ≥ 2 属任务 12（E307）；含 `MS_AST_KW_ARG` 的调用属任务 13（E307） |
| 其余全部节点 | 统一 E307「尚未实现」占位 | 逐项由任务 11/12/13/14/15/16/23/24/26/28/30/32/43 等替换 |

### 常量与名字

- 整数字面量：词素去下划线后按前缀（`0x`/`0o`/`0b`/十进制）以 `strtoll` 转值；溢出 int64 报 E308（任务 31 落地大整数后解除，该文已约定此占位）；`msIntNew` 构造后经 `msProtoAddConst` 入池发 `LOAD_CONST Bx`。
- 浮点：`strtod` 转值（词法形式已由任务 03 校验）；`msFloatNew` 入池。
- 字符串/raw string：词素切片经 `msLexerUnescape` 解码（raw string 原文直取），`msStringNew` 入池；`{{`/`}}` 还原属任务 28 的 f-string 路径，本任务不处理 `MS_AST_FSTRING*`（E307）。
- `true`/`false`/`nil`：`MS_OP_LOAD_TRUE` / `MS_OP_LOAD_FALSE` / `MS_OP_LOAD_NIL`，不入常量池（任务 05 约定）。
- 字节串字面量：E307（任务 32 接通）。
- 常量池去重、65536 上限及其溢出映射 E309 均沿用构建器行为（任务 06 落地后 `msProtoAddConst` 为值去重，小整数经驻留缓存天然同指针）。

### 表达式生成模式

- **二元算术/位运算**：`MsTokenType` → 操作码的 `static const` 映射表（`PLUS→ADD`、`KW_DIV→FLOORDIV`、`DOUBLE_STAR→POW`、`SHIFT_LEFT→SHL` 等）；生成「左 → 右 → 指令」，栈深 +1。
- **一元**：`-`→`NEG`、`~`→`INVERT`、`not`→`NOT`、`+` 见歧义说明第 5 条。
- **逻辑表达式**（弹栈跳转 + 隐藏槽中转，歧义说明第 3 条）：

```
# a and b                              # a or b
eval a                                 eval a
STORE_LOCAL t                          STORE_LOCAL t
LOAD_LOCAL t                           LOAD_LOCAL t
JUMP_IF_FALSE -> Lend                  JUMP_IF_TRUE -> Lend
eval b                                 eval b
STORE_LOCAL t                          STORE_LOCAL t
Lend: LOAD_LOCAL t                     Lend: LOAD_LOCAL t
```

隐藏槽 `t` 经 `msCompilerAllocHiddenLocal` 取得、用后随作用域回绕；两条路径栈深均净 +1。左操作数折叠为常量时按常量条件消除（见「窥孔优化」）：`false and x` → `LOAD_FALSE`，`true or x` → `LOAD_TRUE`，`true and x` / `false or x` → 仅编译 `x`——与短路不求值 `x` 的运行时语义一致。

- **条件表达式** `a if cond else b`：`eval cond` → `JUMP_IF_FALSE` 占位 → `eval a` → `JUMP` 占位 → 回填 else 入口 → `eval b` → 回填汇合点；两路径栈深均净 +1。
- **单段比较**：`MsCompareOp` 直接映射 `EQ/NE/LT/LE/GT/GE/IS/IN`；`MS_CMP_NOT_IN` → `IN` + `NOT`，`MS_CMP_IS_NOT` → `IS` + `NOT`。
- **位置调用**：被调表达式 → 实参从左到右压栈 → `MS_OP_CALL A=n`；`n > 255` 报 E312。栈形与任务 08 的 `CALL` 约定一致（`[... callable arg0 ...]`），栈深净 `-n`（callable + n 实参替换为 1 个返回值）。
- **折叠优先**：`MS_AST_UNARY`/`MS_AST_BINARY` 进入生成前先试 `msCompilerEvalConst`，命中则只发单条载入（见下）。

### 窥孔优化（仅两项，内联于单趟生成）

**常量折叠**：

```c
typedef enum { MS_CONST_INT, MS_CONST_FLOAT, MS_CONST_BOOL, MS_CONST_NIL } MsConstKind;

struct MsConstValue {
  MsConstKind kind;
  int64_t intValue;      // MS_CONST_INT
  double floatValue;     // MS_CONST_FLOAT
  bool boolValue;        // MS_CONST_BOOL
};

// Bottom-up constant evaluation over the foldable whitelist (设计依据 4):
// literals; unary + - ~ on numbers; not on any constant; binary + - * on
// int (no fold on int64 overflow) and on float/int-float mixes (promotion
// per 02-types §3.3). Returns false for anything else.
static bool msCompilerEvalConst(const struct MsAst* node, struct MsConstValue* out);

// Emits a folded value: LOAD_CONST for int/float, LOAD_TRUE/FALSE/NIL otherwise.
static void msCompilerEmitConst(struct MsCompiler* c, const struct MsConstValue* v);
```

int 溢出检出复用任务 08 快路径的边界比较规则（严禁有符号溢出 UB）；折叠结果与 VM 结果逐位一致是白名单的入选标准。

**死跳转消除**：即 `msCompilerPatchToCurrent` 的 `JUMP`→`NOP` 改写规则（见「发射与回填原语」）。本任务子集内无条件跳转仅来自条件表达式，其目标恒非下一条指令，故该改写主要由任务 12 的控制流触发；本任务另将**常量条件分支消除**（常量折叠与死代码消除的复合，不越出 §1 允许的两项）：条件表达式/逻辑表达式条件折叠为常量时，只生成被取分支，不发射任何跳转——这使消除在本任务的转储测试中可观测。

不做其他一切优化：无公共子表达式消除、无强度削弱、无跳转链压缩（跳转到跳转），留待性能路线图。

### 函数编译：每函数一个 MsProto

```c
// Pushes a new function context (msAlloc'd), initializing its builder;
// funcNode == NULL marks the module top level ("<main>").
static struct MsFuncCtx* msCompilerPushFunc(struct MsCompiler* c, const struct MsAst* funcNode);

// Appends LOAD_NIL + RETURN unless the last emitted op is already RETURN,
// fills metadata (paramCount, localCount = localMax, stackSize = stackMax,
// name, sourceFile, isAsync, hasVarArgs/hasKwArgs), finalizes via
// msProtoBuild, frees the context and hands the proto to *out.
static MsResult msCompilerPopFunc(struct MsCompiler* c, struct MsProto** out);
```

- `MS_AST_FUNC_DECL`：参数仅普通参数（带默认值/`*`/`**` 报 E307，任务 13 解除）；子上下文中参数按序声明为槽 `0..paramCount-1`（深度 0 局部），编译函数体，`msCompilerPopFunc` 得子 Proto；子 Proto 经 `msProtoAddConst` 入**外层**常量池（子 Proto 不去重，任务 05 约定），定义点发射 `MS_OP_MAKE_FUNCTION Bx`；随后按声明语义绑定函数名（模块深度 0 → `STORE_GLOBAL`，否则声明局部 + `STORE_LOCAL`）。`isAsync` 标志照传入子 Proto（调用语义属任务 43）。
- `MS_AST_LAMBDA`：同上，`name = "<lambda>"`（任务 13 定名），函数体即单表达式 + `RETURN`，定义点发射 `MS_OP_MAKE_LAMBDA Bx`。
- 嵌套函数引用外层局部：解析命中 `MS_RESOLVE_UPVALUE` 报 E310（任务 14 解除）。
- 模块顶层：`msCompilerCompile` 以 `isModule = true` 压入首个上下文，`name = "<main>"`、`paramCount = 0`、各标志位为假（任务 09 §3 约定）。

### 模块顶层与 REPL

- 顶层语句逐条编译；`flags & MS_COMPILE_REPL` 时，最后一条顶层 `MS_AST_EXPR_STMT` 求值后改发 `MS_OP_PRINT_EXPR`（弹栈顶、非 nil 则打印，任务 05 已定格式，VM 语义属任务 22），其余语句不变。
- 顶层 Proto 末尾由 `msCompilerPopFunc` 保证以 `RETURN` 结束（任务 05/09 约定）。

### 行号与诊断

- 每条语句/每个表达式子节点首次发射前 `msProtoSetLine(node->line)`，构建器按游程编码自动追加（任务 05）。
- 编译诊断经 `msDiagReport(c->diags, node->line, node->column, code, fmt, ...)`，与词法/语法错误共享单文件 20 条上限；`msDiagReport` 返回 `false`（已满）时置粘性错误并中止生成。

错误码表（编译段 E3xx；E301–E306 按任务 11 已定含义收录，本任务自用码自 E307 起，后续任务顺延并登记回本表）：

| 错误码 | 情形 | 归属 |
|---|---|---|
| E301–E306 | 声明/赋值/global/槽位与块深度检查（任务 11 已定名） | 任务 11（本任务复用 E306 于参数/槽位超限） |
| E307 | 构造尚未实现（统一占位路径，消息注明归属任务） | 本任务 |
| E308 | 整数字面量超出 int64（任务 31 解除） | 本任务 |
| E309 | Proto 规模超限：常量池 > 65536 项、跳转偏移越 24 位 | 本任务（构建器粘性错误的诊断映射） |
| E310 | 嵌套函数引用外层局部变量（upvalue 属任务 14） | 本任务 |
| E311 | AST 嵌套深度超 `MS_COMPILER_MAX_NODE_DEPTH` | 本任务 |
| E312 | 调用位置实参 > 255 | 本任务（任务 13 的其余调用检查自 E313 顺延） |

## 实现步骤

1. 建 `src/compiler/ms_compiler.h` / `ms_compiler.c` 骨架：`MsCompileFlags`、`struct MsCompiler`、`msCompilerInit`/`msCompilerDestroy`、函数上下文压弹（`msCompilerPushFunc`/`msCompilerPopFunc` 含末尾 `RETURN` 补齐与元数据填写）、粘性错误与 `nodeDepth` 预算。验证：空源码编译出仅含 `LOAD_NIL; RETURN` 的顶层 Proto，`name == "<main>"`、`paramCount/localCount/stackSize` 为 0，分配统计归零。
2. 常量与字面量生成：整数（去下划线、四进制、`strtoll`、E308）、浮点、字符串（`msLexerUnescape`）、`true`/`false`/`nil`；`msProtoAddConst` 入池与行号登记。验证：各字面量表达式语句的指令流解码断言与 `msProtoDump` 黄金文本。
3. 作用域栈与名字解析：push/pop/声明/最内层优先查找、模块双层语义、隐藏槽与丢弃槽；`MS_AST_DECL`/`MS_AST_ASSIGN`/`MS_AST_EXPR_STMT`/`MS_AST_BLOCK`/`MS_AST_PASS` 生成。验证：顶层 `:=` 发 `STORE_GLOBAL`、块内 `:=` 发 `STORE_LOCAL`、遮蔽与兄弟块槽位复用（转储断言槽号）、`x := x` 读外层、E306 槽位溢出。
4. 表达式生成：token→操作码映射、一元、逻辑（隐藏槽短路模式）、条件表达式、单段比较（含 `NOT_IN`/`IS_NOT` 展开）、位置调用；栈深跟踪与汇合点不变量断言。验证：逐构造的转储黄金文本与 `stackSize` 高水位正确性。
5. 窥孔优化：`msCompilerEvalConst` 白名单与溢出检出、`msCompilerEmitConst`、常量条件分支消除、`msCompilerPatchToCurrent` 的 `JUMP`→`NOP` 改写。验证：`1 + 2 * 3` 折叠为单条 `LOAD_CONST`；`INT64_MAX + 1` 不折叠（发 `ADD`）；`a if true else b` 无跳转指令；`false and x` 仅 `LOAD_FALSE`。
6. 函数编译：参数槽位、隐式 `RETURN`、`MAKE_FUNCTION`/`MAKE_LAMBDA`、子 Proto 入外层常量池、函数名绑定、`isAsync` 透传、E307（默认参数/`*`/`**`）、E310（外层局部引用）。验证：`func add(a, b) { return a + b }` 的外层转储含 `MAKE_FUNCTION` 且常量池含子 Proto、子 Proto 元数据逐项断言；lambda 名 `<lambda>`；嵌套函数（函数内定义函数）的两级常量池结构。
7. 占位与防御路径：统一 E307 分派、E311 递归预算、E309（构建器粘性错误映射）、E312、诊断满 20 条中止、REPL 选项位的 `PRINT_EXPR` 改写。验证：负例的诊断码/行/列断言与编译返回值。
8. 接入 CMake `mslang` 库目标与 `mslang-tests`，`ctest` 全绿；`MSLANG_STRICT_WARNINGS` 无警告；构建产物只在 `build/`。

## 测试方案

本任务早于最小可运行解释器（任务 09），只能用 C 单元测试。测试文件 `tests/c/test_compiler.c`（本任务只交付本设计文档，测试代码随实现任务编写），使用任务 01 的 `ms_test.h`（`MS_TEST`/`MS_ASSERT_EQ`）。统一测试手法：`setUp` 建独立 `MsHeap`（任务 06）与 `MsDiagList`（任务 02），测试助手把源码字符串串过 lexer（任务 03）→ parser（任务 04）→ `msCompilerCompile`，对产出的 `MsProto` 做两类断言——指令流逐条解码（`msOpDecodeOp/A/Bx/SAx`）比对操作码与操作数序列、元数据字段断言；复杂形态辅以 `msProtoDump` 黄金文本比对；负例断言诊断列表（错误码/行/列）与返回值。`tearDown` 按 `msProtoFree` → arena 销毁 → `msHeapDestroy` 顺序释放，断言任务 02 分配统计归零。

覆盖清单：

- 顶层 Proto 约定：空源码仅 `LOAD_NIL; RETURN`；`name == "<main>"`、`sourceFile`、全标志位为假；多语句序列的行号表（`msProtoLineAt` 抽查）。
- 字面量与常量池：各进制整数与下划线形式入池值正确；重复常量去重（`1; 1` 共享槽位）；浮点；字符串转义解码结果（`\n`、`\uHHHH`）；raw string 原文；`true`/`false`/`nil` 专用指令；整数溢出报 E308；`b"..."` 报 E307。
- 作用域：顶层 `:=` → `STORE_GLOBAL`、读取 → `LOAD_GLOBAL`；块内 `:=` → 局部槽、出块后同名读回落全局；嵌套块遮蔽（同名不同槽）；兄弟块槽位复用（转储中槽号相同）；`x := x` 的 RHS 解析到外层；隐藏丢弃槽计入 `localCount`；槽位/块深度超限报 E306。
- 表达式：映射表逐操作抽查（`+ - * / div % ** << >> & | ^`）；一元四种；优先级经 AST 保持（`1 + 2 * 3` 的结构由任务 04 保证，此处断言发射次序）；`and`/`or` 短路模式形状（含隐藏槽与两路径栈深一致）；条件表达式跳转形状；单段比较全算符（含 `not in`/`is not` 的 `IN+NOT`/`IS+NOT`）；n ≥ 2 链报 E307；位置调用栈形与 `A` 操作数、`n > 255` 报 E312；含关键字实参报 E307。
- 常量折叠：`1 + 2 * 3` → 单条 `LOAD_CONST 7`；`-5`、`~0`、`not nil` → `LOAD_TRUE`；`1 + 2.0` → float `3.0`；`INT64_MAX + 1` 与 `INT64_MIN * -1` 不折叠（发射 `ADD`/`MUL`）；`"a" + "b"` 不折叠；`1 / 0` 不折叠（保留运行时错误路径）。
- 死跳转消除：`a if true else b` 与 `a if false else b` 无跳转指令、只含被取分支；`false and x` → `LOAD_FALSE`、`true or x` → `LOAD_TRUE`、`true and x` → 仅 `x` 的代码；`JUMP`→`NOP` 改写规则的触发形态由任务 12 的测试覆盖（本文显式标注）。
- 函数：`func` 定义点 `MAKE_FUNCTION` + 名绑定（顶层 `STORE_GLOBAL`、块内 `STORE_LOCAL`）；子 Proto 元数据（`paramCount`、`localCount`、`stackSize`、名称）；无 `return` 函数末尾补 `LOAD_NIL; RETURN`，显式 `return` 不重复补；lambda 名 `<lambda>`；嵌套函数的两级常量池（外层池含子 Proto、子池含孙 Proto）；带默认参数/`*args` 报 E307；函数体内引用外层局部报 E310；`async func` 的 `isAsync` 透传。
- REPL：`MS_COMPILE_REPL` 下顶层最后一条表达式语句改发 `PRINT_EXPR`，此前语句仍为丢弃形态；非 REPL 模式无 `PRINT_EXPR`。
- 诊断与防御：E307/E308/E310/E311/E312 各一例（码/行/列断言）；病态嵌套表达式触发 E311；诊断满 20 条后编译中止返回 `MS_ERROR_SYNTAX`；`msMemSetFailAfter` 注入 OOM 时返回 `MS_ERROR_OOM` 且无泄漏。

## 验收标准

- [ ] `src/compiler/ms_compiler.h` / `ms_compiler.c` 存在，guard 为 `MSLANG_SRC_COMPILER_MS_COMPILER_H_`，头文件自包含，风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsCompiler`/`struct MsFuncCtx` 不 typedef）。
- [ ] 单趟编译：无独立 IR 与独立优化 pass；窥孔优化仅常量折叠与死跳转消除两项，均内联于生成/回填；折叠白名单与溢出检出规则如文所述，折叠结果与 VM 语义逐位一致。
- [ ] 块级作用域落实：作用域栈、最内层优先解析、遮蔽、出块回绕与兄弟块槽位复用；模块顶层深度 0 的 `:=` 走全局命名空间；名字解析顺序「局部 → 外层（E310 桩）→ 全局/内建」与 03-syntax §8 一致。
- [ ] 每函数/模块顶层一个 `MsProto`；嵌套函数子 Proto 入外层常量池、`MAKE_FUNCTION`/`MAKE_LAMBDA` 发射与名绑定、参数槽位、末尾 `RETURN` 补齐、元数据（`paramCount`/`localCount`/`stackSize`/名称/标志位）填写正确。
- [ ] 仅经任务 05 构建器 API 发射；跳转回填原语含 `JUMP`→`NOP` 死跳转改写且不改写条件跳转；栈深跟踪产出正确 `stackSize`；行号经 `msProtoSetLine` 登记。
- [ ] E3xx 错误码表如本文（E301–E306 收录任务 11 定名，E307–E312 本任务定义）；统一「尚未实现」占位路径覆盖全部未支持节点；诊断共享单文件 20 条上限。
- [ ] `MS_COMPILE_REPL` 选项位与顶层最后一条表达式语句的 `PRINT_EXPR` 改写落实；接口定名（`msCompilerCompile`、`msCompilerCompileStmt/Expr`、作用域钩子、`msCompilerAllocHiddenLocal` 等）与「设计依据」第 9 条一致。
- [ ] 全部堆分配经 `msAlloc/msRealloc/msFree`；测试拆卸后分配统计归零；OOM 注入路径返回 `MS_ERROR_OOM`。
- [ ] `tests/c/test_compiler.c` 覆盖「测试方案」全部清单项并全部通过；`ctest` 全绿；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 02/03/04/05/06 的接口引用（`msDiagReport`、`msLexerUnescape`、`struct MsAst`、`MsProtoBuilder` 族、`msIntNew`/`msStringNew` 等）均为对应已定稿文档的定名。
