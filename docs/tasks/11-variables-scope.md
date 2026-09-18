# 11 变量与作用域（`:=`/`=`、global）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [09 最小可运行解释器](09-minimal-interpreter.md) |

## 任务目标

交付 mslang 的变量与词法作用域体系，把任务 09「一切名字皆全局」的最小形态升级为 [03-syntax.md](../language/03-syntax.md) §3/§8 规定的完整语义：

- **声明与赋值严格区分**：`:=` 声明新变量（左侧至少一个新名字，多重声明按 Go 语义逐名处理），`=` 只能写给已声明的名字，违例均为**编译期错误**（诊断含文件/行/列，CLI 退出码 2）。
- **块级作用域**：`{}`（含 `if`/`for`/裸块）引入新作用域；块内 `:=` 产生块局部变量，占用所在帧的寄存器窗口槽位，出块即失效，兄弟块复用槽位；内层可读/写外层局部（遮蔽与穿透赋值）。
- **多重赋值与交换**：`a, b = b, a`——右侧先整体按从左到右求值，再写回各目标；两侧个数静态可判时不匹配为编译错误。
- **解包赋值**：`a, b := pair`、`a, *rest := xs` 经 `MS_OP_UNPACK` 实现；本任务把该指令的 VM 语义从任务 26 上提前置，v0.1 只支持 list 来源（含一个星号目标收集余项）。
- **`global` 语句**：模块顶层为合法 no-op；函数作用域内的「强制解析到模块全局」语义与冲突检查规则在本文定稿，可被任务 13/14 直接采用。

具体交付：新建 `src/compiler/ms_scope.h` / `src/compiler/ms_scope.c`（作用域上下文模块）；扩展任务 07 的编译器（`src/compiler/ms_compiler.c`，`MS_AST_DECL`/`MS_AST_ASSIGN`/`MS_AST_AUG_ASSIGN`/`MS_AST_GLOBAL`/`MS_AST_BLOCK` 的生成与检查）与任务 08 的 VM（`src/vm/ms_vm.c` 增加 `MS_OP_UNPACK` 分派分支）。完成后任务 12（控制流）经本任务的作用域 API 实现 `for` 子句变量的循环级作用域，任务 13（函数）在同一 API 上叠加函数作用域与参数槽位。

明确不在本任务范围：upvalue/闭包捕获（任务 14，本文仅预留 `isCaptured` 标记）；`a[i] = v` / `o.x = v` 形式的下标/属性赋值目标（任务 16/15，本任务编译期拒绝）；`for-in` 迭代目标的绑定（任务 12/26 用同一声明路径）；裸标识符 `del x`（v0.1 编译期拒绝，列入路线图；`del` 容器/属性目标随任务 15/16）；`UNPACK` 对任意可迭代对象的泛化（任务 26）。

## 设计依据

- [03-syntax.md](../language/03-syntax.md)
  - §3：`declStmt = identList ":=" exprList`、`assignStmt = targetList ("=" | augOp) exprList`、`globalStmt = "global" identList`、`block = "{" { statement } "}"`；要点——「`:=` 声明新变量，至少左侧一个新名字；`=` 赋值给已声明的名字，混用非法」「多重赋值与交换：`a, b = b, a`；右侧先整体求值」「解包赋值：`a, b := pair`（长度须匹配；`a, *rest := xs` 星号收集剩余）」。
  - §8 作用域：块级作用域（`{}` 引入新作用域，含 `if`/`for` 块）；函数内赋值默认局部，`global x` 声明写模块级变量；名字解析顺序「局部 → 闭包外层 → 模块全局 → 内建」。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.1（`MsProto` 的 `localCount` 寄存器窗口）、§2.2（`LOAD_LOCAL`/`STORE_LOCAL`/`LOAD_GLOBAL`/`STORE_GLOBAL`/`UNPACK` 指令分类）。
- [05-bytecode-proto.md](05-bytecode-proto.md)：指令格式与操作数——`LOAD_LOCAL`/`STORE_LOCAL` 的 `A` 为槽位（8 位，故单函数局部上限 256）；`LOAD_GLOBAL`/`STORE_GLOBAL` 的 `Bx` 为名字符串常量池下标；`UNPACK` 已定 `A` = 解包目标个数，本文补充 `Bx` 约定（见「多重赋值与解包的代码生成」）。
- [08-vm-core.md](08-vm-core.md)：帧窗口布局（`baseIndex` 起 `localCount` 槽，压帧时 nil 填充）；全局指令语义（`LOAD_GLOBAL` 未命中报运行时错误「undefined name」）；「指令实现范围」表把 `UNPACK` 划给任务 26 并注明可上提的协调点——本任务按该注释把 `UNPACK` 的 list 最小语义上提到此处，任务 26 再以迭代协议泛化，栈/帧约定不变。
- [09-minimal-interpreter.md](09-minimal-interpreter.md)：顶层 `:=` 一律走全局命名空间（`STORE_GLOBAL`）、顶层 Proto 的帧约定；ms 脚本测试设施（`run_tests.py`、`.out`/`.exit` 同伴文件）与退出码约定（编译错误 2、运行时错误 1）。
- [10-builtin-functions.md](10-builtin-functions.md)：内建函数写入全局命名空间、可被脚本 `:=` 遮蔽（[03-syntax.md](../language/03-syntax.md) §8 解析顺序的直接推论）。
- [04-parser-ast.md](04-parser-ast.md)：`MS_AST_DECL`（目标可含至多一个 `*ident`）/`MS_AST_ASSIGN`/`MS_AST_AUG_ASSIGN`/`MS_AST_GLOBAL`/`MS_AST_BLOCK` 节点形态；解包目标个数与位置合法性已由 parser 检查（E212），目标可赋值性（E205）已由 parser 检查；「`:=` 至少一个新名字、`=`
  目标已声明」需作用域信息，parser 明确留给本任务。
- 以下接口名在依赖任务文档中尚未定稿，本文为其假定命名：任务 07 的 `struct MsCompiler` 及其发射助手（本文假设编译器持有 `MsState* L`、当前函数编译上下文与 `struct MsProtoBuilder`）；任务 06 的字符串/列表原语 `msNewString`、`msNewList`、`msListAppend`、`msListLength`、`msListGet` 与全局命名空间查询 `msStateGetGlobal`。**实现时以对应任务文档定名为准。**

规范歧义的处理（实现与评审时以本节口径为准）：

1. **`:=` 的「至少一个新名字」按 Go 语义逐名处理**：对每个目标名，当前（最内层）作用域已存在则视为普通赋值目标，不存在则在该作用域声明新变量；全部目标都已存在才报编译错误。判定只看当前作用域，外层同名不阻止内层声明（即允许遮蔽）。
2. **赋值目标的「已声明」判定**：名字在局部作用域链、本编译单元已见的顶层 `:=`/`global` 声明、或编译时刻 `MsState` 全局命名空间（内建函数由 C 侧预注册于此，故 `print = 5` 合法）三者之一命中即为已声明；否则 `=`/复合赋值报编译错误。**读取**未声明名不做编译期检查——全局命名空间是动态的（C API 注入、未来的 import），统一走 `LOAD_GLOBAL` 的运行时「undefined name」错误。
3. **多重赋值的求值与写回顺序**：右侧表达式列表先整体按从左到右求值，再写回目标（「右侧先整体求值」的原文落实）。v0.1 目标仅为标识符，写回次序不可观察，生成上按从右到左弹栈存储（栈自然排空）；下标/属性目标落地（任务 15/16）时改为「右值先入临时槽、目标从左到右写回」方案，纯标识符目标的行为不变。
4. **复合赋值限单目标**：EBNF 允许 `targetList augOp exprList`，但 `a, b += 1, 2` 语义混乱且无收益，对齐 Python 报编译错误。
5. **`global` 在模块顶层为合法 no-op**：顶层名字本就在全局命名空间；为与函数内语义一致，`global x` 视为把 `x` 记入「已声明全局名」集合，其后的 `x = 5` 无需 `:=` 即合法（`global` 本身即一种声明）。
6. **`UNPACK` 的 `Bx` 操作数**：任务 05 只定了 `A` = 目标个数，本文定稿 `Bx` = 星号目标位置（0 起始）或 `0xFFFF`（无星号），任务 26 泛化时沿用。
7. **裸名 `del x` 不支持**：删除局部名会让槽位管理复杂化且收益低，v0.1 报编译错误并列入路线图；`del a[i]`/`del o.x` 随任务 15/16 落地。

## 详细设计

### 1. 文件与模块边界

- 新建 `src/compiler/ms_scope.h`（include guard `MSLANG_SRC_COMPILER_MS_SCOPE_H_`）与 `src/compiler/ms_scope.c`：作用域上下文 `struct MsScopeCtx` 及其操作，是编译器内**每个函数（含模块顶层）编译上下文**的组成部分。头文件自包含（include `<stdbool.h>` 与任务 02 的 `"core/ms_result.h"`（假定名），前向声明 `struct MsString`）。
- 修改任务 07 的 `src/compiler/ms_compiler.c`：块/声明/赋值/复合赋值/global 语句的生成函数（文件内 `static`），持有并驱动 `MsScopeCtx`。
- 修改任务 08 的 `src/vm/ms_vm.c`：新增 `MS_OP_UNPACK` 分派分支（list 最小语义）。
- 内存纪律：`MsScopeCtx` 唯一的堆分配是 `globalNames` 数组（经 `msAlloc`/`msRealloc` 增长、`msFree` 释放，所有者为编译上下文）；局部名与全局名的 `MsString*` 均为**借用**（归编译器的常量池/驻留表所有），本模块不释放。

### 2. 作用域上下文（ms_scope.h）

内部结构体不 typedef，字段 lowerCamelCase：

```c
#define MS_SCOPE_MAX_LOCALS 256   // LOAD_LOCAL/STORE_LOCAL 的 A 操作数为 8 位
#define MS_SCOPE_MAX_DEPTH 128    // 块嵌套深度上限，防御性

struct MsLocal {
  struct MsString* name;    // borrowed: owned by the compiler's constant pool
  int slot;                 // register-window slot in the current frame
  int depth;                // block depth at declaration (0 = function body level)
  bool isCaptured;          // reserved for task 14 upvalue capture; always false here
};

struct MsScopeCtx {               // per-function scope state, owned by the compiler
  struct MsLocal locals[MS_SCOPE_MAX_LOCALS];
  int localCount;                 // currently live locals
  int localMax;                   // high-water mark; contributes to proto->localCount
  int scopeDepth;                 // 0 = function body top level; +1 per '{'
  struct MsString** globalNames;  // msAlloc'd name set; see below for dual use
  int globalCount;
  int globalCap;
  bool isModule;                  // true for the module top-level ("<main>") context
};

void msScopeCtxInit(struct MsScopeCtx* ctx, bool isModule);
void msScopeCtxDestroy(struct MsScopeCtx* ctx);

// Enter/leave a block scope. Leaving rewinds localCount to the scope base
// (slots become reusable by sibling blocks). Overflow of MS_SCOPE_MAX_DEPTH
// is reported by the caller as a compile error.
void msScopeBeginBlock(struct MsScopeCtx* ctx);
void msScopeEndBlock(struct MsScopeCtx* ctx);

// Declares a local at the current depth; returns its slot, or -1 when
// MS_SCOPE_MAX_LOCALS is exceeded (caller reports E306).
int msScopeDeclareLocal(struct MsScopeCtx* ctx, struct MsString* name);

// Innermost-first lookup among live locals; returns the slot or -1.
int msScopeResolveLocal(const struct MsScopeCtx* ctx, const struct MsString* name);

// True when name is live and was declared exactly at the current depth
// (the := no-new-name check, ambiguity note 1).
bool msScopeHasLocalAtDepth(const struct MsScopeCtx* ctx, const struct MsString* name);

// Adds name to the ctx's global name set (idempotent). Fails with
// MS_ERROR_SYNTAX when name is already a live local in this ctx (E304).
MsResult msScopeMarkGlobal(struct MsScopeCtx* ctx, struct MsString* name);
bool msScopeIsMarkedGlobal(const struct MsScopeCtx* ctx, const struct MsString* name);
```

`globalNames` 集合的双重用途（同一数据结构、两种语义）：

- **模块顶层 ctx**（`isModule == true`）：记录本编译单元已声明的顶层全局名（顶层 `:=` 与 `global` 均写入），供 `=` 的「已声明」判定（歧义说明第 2 条）。集合查找按 `MsString` 内容比较（哈希缓存由任务 06 的字符串保证）。
- **函数 ctx**（任务 13 起）：记录 `global` 语句强制的名字，解析时绕过局部与 upvalue 直达全局（见「global 语句」）。

### 3. 名字解析与槽位分配

编译器为每个 Proto 的编译持有一个 `MsScopeCtx`。名字解析按 [03-syntax.md](../language/03-syntax.md) §8 的顺序实现为两级查询（「闭包外层」段为任务 14 的桩，本任务恒未命中；「内建」段即全局命名空间，无独立表）：

```
resolveForStore(name):                      resolveForLoad(name):
  slot = msScopeResolveLocal(ctx, name)       slot = msScopeResolveLocal(ctx, name)
  if slot >= 0        → STORE_LOCAL slot      if slot >= 0        → LOAD_LOCAL slot
  if marked global    → STORE_GLOBAL name     if marked global    → LOAD_GLOBAL name
  if upvalue (task 14 stub) → ...             if upvalue (task 14 stub) → ...
  if module-declared or builtin → STORE_GLOBAL else               → LOAD_GLOBAL name
  else                → E301 编译错误            （未声明名的读推迟到运行时）
```

- **槽位分配**：`msScopeDeclareLocal` 取 `localCount` 为槽位号并递增，`localMax` 记高水位；`msScopeEndBlock` 把 `localCount` 回绕到块入口值，兄弟块因此复用槽位。Proto 定稿时 `localCount = localMax`（模块顶层在任务 09 的 0 基础上按需增长；VM 侧窗口布局不变，见 [08-vm-core.md](08-vm-core.md)）。槽位号不得超过 255，超出报 E306。
- **模块顶层的双层语义**：顶层语句直接位于深度 0 时，`:=` 目标**不入局部表**，而是把名字写入 `globalNames` 并生成 `STORE_GLOBAL`（延续 [09-minimal-interpreter.md](09-minimal-interpreter.md) §3 的约定）；一旦进入任何块（深度 ≥ 1），`:=` 产生块局部（`STORE_LOCAL` 槽位）。即「顶层即全局、块内即局部」。
- **遮蔽**：内层块 `:=` 同名变量时 `msScopeDeclareLocal` 直接追加新槽位，解析的最内层优先天然实现遮蔽；出块后外层名字恢复可见。
- **`x := x` 自引用**：声明发生在**右值生成之后**（先生成 RHS 代码，再声明目标名），故 RHS 中的 `x` 解析到外层同名变量或（运行时）未定义全局，而不是未初始化的自身。

### 4. 声明与赋值的编译期检查

编译器在生成 `MS_AST_DECL`/`MS_AST_ASSIGN`/`MS_AST_AUG_ASSIGN` 时执行（诊断经任务 02 的 `struct MsDiagList` 收集，与词法/语法错误共享单文件 20 条上限；错误码归入编译段 E3xx，定名以任务 07 的错误码表为准）：

| 错误码 | 情形 | 消息示例 |
|---|---|---|
| E301 | `=`/复合赋值目标未声明 | `assignment to undeclared name 'x'` |
| E302 | `:=` 左侧无新名字 | `no new variables on left side of :=` |
| E303 | 多重赋值两侧个数不匹配（右侧个数 > 1，静态可判） | `cannot assign 3 values to 2 targets` |
| E304 | `global` 冲突：名字在当前作用域已作局部声明，或 `:=` 目标已被 `global` 强制 | `name 'x' is local before global declaration` |
| E305 | 复合赋值多目标（`a, b += ...`） | `augmented assignment takes a single target` |
| E306 | 局部槽位或块深度超限 | `too many local variables (max 256)` |

检查流程：

- `MS_AST_DECL`（`t := …`）：RHS 生成后，逐目标判定——`msScopeIsMarkedGlobal` 为真报 E304；`msScopeHasLocalAtDepth` 为真则按赋值目标处理（生成 `STORE_LOCAL`/`STORE_GLOBAL`）；否则声明新名字（模块深度 0 → 记 `globalNames` + `STORE_GLOBAL`；否则 `msScopeDeclareLocal` + `STORE_LOCAL`）。全部目标均为「已存在」时整句报 E302。
- `MS_AST_ASSIGN`（`t = …`）：逐目标走 `resolveForStore`，未声明报 E301。目标为 `MS_AST_INDEX`/`MS_AST_ATTR` 时报 E301 同款编译错误（消息注明「indexed/attribute assignment lands in task 15/16」），属阶段性边界（见「任务目标」范围外清单）。
- `MS_AST_AUG_ASSIGN`：目标数 > 1 报 E305；单目标解析（未声明报 E301）后生成「`LOAD` 目标 → RHS → 对应算术指令 → `STORE` 目标」（`+=`→`ADD`、`-=`→`SUB`、…、`>>=`→`SHR`，映射按 [05-bytecode-proto.md](05-bytecode-proto.md) 的操作码枚举）。
- `MS_AST_GLOBAL`：见「global 语句」。

### 5. 多重赋值与解包的代码生成

**静态右侧列表**（`exprList` 个数 m > 1）：目标数 n 必须等于 m，不等报 E303。生成：m 个 RHS 表达式从左到右依次求值压栈（「右侧先整体求值」），然后**从右到左**对 n 个目标发射 `STORE_*`（栈顶是最后一个右值，弹栈次序天然排空；歧义说明第 3 条）。交换赋值 `a, b = b, a` 因此先求出 `b`、`a` 的旧值再分别写回，天然正确。

**单右侧解包**（m == 1 且 n ≥ 2，含星号形式）：生成 RHS 表达式后发射 `MS_OP_UNPACK`，再从右到左存储各目标。`UNPACK` 操作数约定（在 [05-bytecode-proto.md](05-bytecode-proto.md) 的 `A` 之上定稿 `Bx`）：

```
MS_OP_UNPACK  ABC 格式
  A  = 解包目标个数 n（1..255，超出为编译错误）
  Bx = 星号目标位置的 0 起始下标；无星号为 MS_UNPACK_NO_STAR (0xFFFF)
```

**静态右侧列表含星号**（`a, *rest := 1, 2, 3`）：不发 `UNPACK`，编译期直接展开——前 k 个与尾部目标逐个求值存储，中间余项由 `BUILD_LIST` 收集（`BUILD_LIST` 在任务 16 落地；在此之前该形式报编译错误「star target with literal list lands in task 16」，与下标目标同属阶段性边界）。星号位置 k 即 `Bx` 约定的同一下标。

`stackSize` 核算：解包序列的峰值栈深为 `1 + n`（源对象 + n 个结果），编译器按此更新高水位。

### 6. `MS_OP_UNPACK` 的 v0.1 VM 语义（自任务 26 上提）

在任务 08 的 `ms_vm.c` 分派循环新增一个 case（对齐 [08-vm-core.md](08-vm-core.md) 「指令实现范围」表的上提协调点；任务 26 以 `GET_ITER`/`ITER_NEXT` 迭代协议泛化时替换实现，操作数约定不变）：

- 弹栈顶源对象 `src`；要求 `src` 为 list（`msTypeOf(src) == MS_TYPE_LIST`），否则报运行时错误 `TypeError: cannot unpack non-list '<type>'`（任务 26 后扩展为任意可迭代对象）。
- 长度校验：无星号要求 `len == n`；星号在位置 k 要求 `len >= n - 1`。违例报 `ValueError: expected n values to unpack, got m` / `ValueError: expected at least n-1 values to unpack, got m`。
- 按目标次序压入 n 个值：星号前的目标取 `msListGet(src, 0..k-1)`；星号目标收集中段元素为新 list（`msNewList` + 逐项 `msListAppend`）；其余目标取尾部元素。压栈后由调用方（编译器生成的 `STORE_*` 序列）从右到左消费。
- 新建的中段 list 是分配点：跨分配点存活的局部 `MsObject*` 遵守根纪律（[09-c-api.md](../language/09-c-api.md) §3；GC 落地前由 [09-minimal-interpreter.md](09-minimal-interpreter.md) 的「关闭时统一回收」兜底）。

### 7. global 语句

`global x, y` 的编译期语义（函数作用域在任务 13 才存在，本任务定稿规则并实现数据结构，顶层行为即可验证部分）：

- **模块顶层**（`isModule` ctx）：合法 no-op；名字记入 `globalNames`（歧义说明第 5 条），其后的 `x = 5` 视为对已声明全局的赋值。不与任何顶层 `:=` 冲突（两者同义）。
- **函数 ctx**（任务 13 起生效，本文规则）：`msScopeMarkGlobal` 把名字写入该函数的 `globalNames`；之后该函数体内（含其块）对该名的读写经 `resolveForStore`/`resolveForLoad` 直达 `LOAD_GLOBAL`/`STORE_GLOBAL`，绕过局部与 upvalue 查询。冲突检查：名字已是当前 ctx 的存活局部时报 E304；反向地，已 `global` 的名字再作 `:=` 目标也报 E304。
- `global` 不产生任何指令；它只影响后续名字解析，是纯粹的编译期声明。

### 8. 与后续任务的衔接

- **任务 12（控制流）**：`if`/`while`/`for` 的块与三分 `for` 子句（`for i := 0; ...`）经 `msScopeBeginBlock`/`msScopeEndBlock` 获得循环级/块级作用域；本任务 API 不因此变动。
- **任务 13（函数）**：每个函数 Proto 的编译新建一个 `isModule == false` 的 `MsScopeCtx`；参数占据槽 `0..paramCount-1`（先登记为深度 0 局部），`global` 语义按本文第 7 节生效；嵌套函数引用外层局部在任务 14 前仍为编译错误（任务 13 已定）。
- **任务 14（闭包）**：`MsLocal.isCaptured` 标记启用；被捕获的块局部在 `msScopeEndBlock` 时由编译器补发 `MS_OP_CLOSE_UPVALS`，槽位复用规则不变（upvalue 关闭后原槽可安全复用）。
- **任务 15/16**：下标/属性赋值目标接入，多重赋值写回切换为「临时槽 + 从左到右」方案（歧义说明第 3 条），标识符目标行为不变。
- **任务 26（迭代协议）**：`UNPACK` 的源对象判定从「必须 list」泛化为「任意可迭代对象」，操作数约定与错误消息风格不变。

## 实现步骤

1. 建 `src/compiler/ms_scope.h` / `ms_scope.c`：`struct MsLocal`、`struct MsScopeCtx`、`msScopeCtxInit`/`msScopeCtxDestroy`（`globalNames` 惰性分配、倍增增长、逆序释放）。验证：编译空脚本与含块脚本后，任务 02 分配统计归零。
2. 实现块作用域与槽位分配：`msScopeBeginBlock`/`msScopeEndBlock`（回绕 `localCount`、维护 `localMax`）、`msScopeDeclareLocal`/`msScopeResolveLocal`/`msScopeHasLocalAtDepth`（最内层优先）。验证：ms 脚本断言块内声明、出块失效（运行时 undefined name 负例）、兄弟块槽位复用（行为透明，经嵌套遮蔽断言）。
3. 编译器接入 `MsScopeCtx`：模块顶层 ctx（`isModule = true`）；`MS_AST_BLOCK` 经 begin/end 包裹；`MsProto.localCount` 取 `localMax`。验证：任务 09 冒烟脚本无回归（纯顶层 `:=` 仍走 `STORE_GLOBAL`）。
4. 实现 `MS_AST_DECL` 生成与检查：RHS 先行、逐名「同深度已存在→赋值 / 否则声明」、E302/E304/E306、模块深度 0 走全局集合。验证：`:=` 正例、`x := 1; x := 2` 报 E302（退出码 2）、`global x` 后 `x := x` 报 E304。
5. 实现 `MS_AST_ASSIGN` 与 `MS_AST_AUG_ASSIGN`：`resolveForStore`（含编译时刻 `msStateGetGlobal` 查询内建）、E301/E305、复合赋值的 load-op-store 生成。验证：`x = 1` 未声明报 E301；`print = 5` 合法；`x += 1` 等价 `x = x + 1`；`a, b += 1, 2` 报 E305。
6. 实现多重赋值：静态双列表的求值次序与从右到左写回、E303。验证：`a, b = b, a` 交换断言、个数不匹配报 E303。
7. 实现 `MS_OP_UNPACK` VM 分支与编译器解包生成（`Bx` 星号约定、长度校验、中段 list 收集）。验证：`a, b, c := range(3)` 逐值断言、`first, *rest := range(5)` 经 `print` + `.out` 比对、`a, b := range(3)` 报 ValueError（退出码 1）、`a, b := 1` 报 TypeError。
8. 实现 `MS_AST_GLOBAL`：顶层 no-op 记名、函数 ctx 的 `msScopeMarkGlobal` 与 E304（数据结构先行，函数侧行为由任务 13 的测试观测）。验证：顶层 `global x; x = 5; assert(x == 5)`。
9. 全量回归：任务 09/10 既有 `tests/ms/` 脚本与本任务脚本经 `run_tests.py` 与 `ctest --test-dir build` 全绿；Debug 构建（ASAN / `/RTC`）无内存错误，`msCloseState` 后无残余分配计数。

## 测试方案

本任务晚于最小可运行解释器（任务 09），一律用 ms 脚本测试，由仓库根 `run_tests.py` 驱动 mslang CLI；`testing` 模块（任务 40）之前用内建 `assert` + `print` 自断言。正向脚本成功路径以 `print("<name> ok")` 收尾、退出码 0；需要精确输出比对的脚本配 `<name>.out` 同伴文件全文比对 stdout；负向用例配 `<name>.exit`（编译错误内容 `2`，运行时错误内容 `1`）。本任务阶段无 list 字面量与下标（任务 16），list 来源用内建 `range`，list 内容断言经 `print` + `.out` 或 `len` + `==`（整体值相等）。测试目录 `tests/ms/variables/`（脚本随实现编写，本文只列清单与覆盖点）：

- `decl_assign.ms`：顶层 `:=` 声明后 `=` 改写；`x := x` 读取外层/全局的旧值语义（先 `x := 1`，块内 `x := x + 1` 遮蔽并断言为 2）；内建遮蔽（`len := 5` 后 `assert(len == 5)`）。
- `block_scope.ms`：`if` 块内 `:=` 产生块局部；嵌套块读外层局部；嵌套块 `=` 穿透改写外层局部（`if true { x := 1; if true { x = 2 }; assert(x == 2) }`）；内层 `:=` 遮蔽同名全局，出块后全局值不变。
- `slot_reuse.ms`：兄弟块先后声明不同名局部（`{ x := 1 } { y := 2 }`）行为正确（槽位复用对脚本透明，回归用）。
- `multi_assign.ms`：`a, b := 1, 2` 逐值断言；`a, b = b, a` 交换；三目标轮转 `a, b, c = b, c, a`；右侧先整体求值（`a := 1; b := 2; a, b = a + b, a - b` 断言为 `(3, -1)`）。
- `aug_assign.ms`：全局与块局部的 `+=`/`-=`/`*=`/`//=`/`%=`/`<<=` 各一例；复合赋值作用于遮蔽后的块局部而非全局。
- `unpack.ms` + `unpack.out`：`a, b, c := range(3)` 逐值断言；`first, *rest := range(5)` 后 `print(rest)` 输出 `[1, 2, 3, 4]` 全文比对；`a, *mid, b := range(5)` 中段收集；`a, b = range(2)`（`=` 形式的解包）。
- `global_stmt.ms`：顶层 `global x` 后 `x = 5` 合法（无 `:=`）；`global a, b` 多名字；`global` 与既有顶层 `:=` 共存无冲突。
- 编译错误负例（各配 `.exit` 内容 `2`，诊断含行号）：`err_undeclared_assign.ms`（`x = 1` → E301）；`err_no_new_name.ms`（`x := 1; x := 2` → E302；同块 `a, b := 1, 2` 后 `a, b := 3, 4` 同样 E302）；`err_arity_mismatch.ms`（`a, b = 1, 2, 3` → E303）；`err_aug_multi.ms`（`a, b += 1, 2` → E305）；`err_global_conflict.ms`（块内 `x := 1` 后同函数 ctx 的 `global x` → E304，函数侧用例随任务 13 补充）；`err_index_target.ms`（`a := range(3); a[0] = 9` → 阶段性编译错误，任务 16 转为正例并移除）；`err_local_overflow.ms`（生成 300 个块内 `:=` → E306）。
- 运行时错误负例（各配 `.exit` 内容 `1`，消息含对应前缀）：`err_read_undefined.ms`（块外 `print` 块局部名 → undefined name）；`err_unpack_count.ms`（`a, b := range(3)` → ValueError）；`err_unpack_few.ms`（`a, *rest := range(1)` 的星号形态边界：`a, b, *rest := range(1)` 不足 → ValueError）；`err_unpack_type.ms`（`a, b := 1` → TypeError）。

## 验收标准

- [ ] `src/compiler/ms_scope.h` / `ms_scope.c` 存在，guard 为 `MSLANG_SRC_COMPILER_MS_SCOPE_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] `:=` 与 `=` 严格区分：`= `/复合赋值目标未声明报编译错误 E301（含对内建的 `msStateGetGlobal` 查询）；`:=` 左侧无新名字报 E302；同深度已存在的 `:=` 目标按赋值处理（Go 语义）；违例脚本退出码为 2。
- [ ] 块级作用域落实：`{}` 引入新作用域，块内 `:=` 占帧窗口槽位、出块失效、兄弟块复用槽位；嵌套块可读/写外层局部；内层 `:=` 遮蔽外层名字且出块恢复；模块顶层深度 0 的 `:=` 仍走全局命名空间（任务 09/10 无回归）。
- [ ] 多重赋值右侧先整体从左到右求值、再从右到左写回，`a, b = b, a` 交换正确；静态两侧个数不匹配报 E303；复合赋值限单目标（E305）。
- [ ] `MS_OP_UNPACK` 按本文约定实现（`A` = 目标数、`Bx` = 星号位置或 `0xFFFF`）：list 源的长度校验、星号中段收集为新 list、非 list 源报 TypeError、长度不符报 ValueError；任务 08 既有 VM 测试无回归。
- [ ] `global` 语句：模块顶层合法 no-op 且记入已声明集合；`msScopeMarkGlobal` 与 E304 冲突检查实现；函数作用域内的完整行为规则按本文第 7 节定稿，供任务 13 直接采用。
- [ ] 名字解析顺序「局部 → （upvalue 桩）→ 全局（含内建）」落实于 `resolveForLoad`/`resolveForStore`；读取未声明名推迟到运行时「undefined name」错误。
- [ ] `tests/ms/variables/` 覆盖「测试方案」全部清单项（含 `.out` 输出比对与 `.exit` 负例），`python run_tests.py` 与 `ctest --test-dir build` 全数通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；假定接口名（任务 07 的 `struct MsCompiler` 与发射助手、任务 06 的 `msNewString`/`msNewList`/`msListAppend`/`msListLength`/`msListGet`/`msStateGetGlobal`、编译段 E3xx 错误码）在实现时已与对应任务文档对齐；与任务 12/13/14/15/16/26 的衔接点（第 8 节）与 [README.md](README.md) 依赖图无冲突。
