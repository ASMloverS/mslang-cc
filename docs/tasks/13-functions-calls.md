# 13 函数与调用

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [09 最小可运行解释器](09-minimal-interpreter.md) |

## 任务目标

交付 mslang 的函数定义与调用机制，打通「`func` 定义 → `MsFunction` 对象 → `MS_OP_CALL`/`MS_OP_CALL_KW`/`MS_OP_TAIL_CALL`/`MS_OP_RETURN` 执行」的完整链路：

- 函数是一等值：可赋值、可作实参传递、可作返回值（[03-syntax.md](../language/03-syntax.md) §4）；本任务落地 `MS_TYPE_FUNCTION` 函数对象与 `MAKE_FUNCTION`/`MAKE_LAMBDA` 两条造函数指令的运行时语义。
- 完整调用约定：参数压栈次序、帧建立/拆除、返回值回传；参数绑定算法覆盖位置参数、默认参数（**定义时求值一次**、绑定共享引用）、关键字实参、`*args` 打包、`**kwargs` 打包及全部参数错误路径（TypeError 系列）。
- `lambda` 单表达式体编译为匿名子 Proto，运行时语义与 `func` 一致。
- `return` 的零值/单值/多值形态；`return f(...)` 形态编译为 `TAIL_CALL`，帧复用不增长调用栈。

本任务定稿任务 05 预留的 `hasVarArgs`/`hasKwArgs` 调用约定（[05-bytecode-proto.md](05-bytecode-proto.md) 明确「由任务 13 定稿」），并定稿任务 10 假定的 C 函数关键字参数约定。完成后任务 14（闭包与 upvalue）只需在 `MsFunction` 上挂接 upvalue 数组、在帧退出时补 `CLOSE_UPVALS`，不再改动本任务的调用约定。

明确不在本任务范围：闭包/upvalue 捕获（任务 14，嵌套函数引用外层局部变量在本任务为编译错误）；方法调用与 `self`（任务 15）；`__call__` 协议（任务 25）；`async func` 的调用语义（任务 43，本任务 `isAsync` 函数被调用时按运行错误处理）；异常对象体系（任务 23，本任务错误仍以错误槽消息 + `MS_ERROR_RUNTIME` 呈现）。

## 设计依据

- [03-syntax.md](../language/03-syntax.md)
  - §4 函数：`funcDecl`/`params`/`lambdaExpr` EBNF；参数固定次序（普通（可带默认值）→ `*ident` → `**ident`）；函数一等值；`lambda` 仅单表达式体；**默认参数在定义时求值一次**（与 Python 一致，含可变默认参数陷阱）。
  - §8 作用域：名字解析顺序（局部 → 闭包外层 → 模块全局 → 内建）；本任务只实现局部与全局两段，闭包外层段属任务 14。
  - §3 `returnStmt = "return" [ exprList ]`：多值返回的语法来源。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.1（`MsProto` 布局含 `paramCount`/`hasVarArgs`/`hasKwArgs`）、§2.2（调用类指令 `CALL`/`CALL_KW`/`TAIL_CALL`/`RETURN`，函数类指令 `MAKE_FUNCTION`/`MAKE_LAMBDA`）、§3（`MS_TYPE_FUNCTION` 类型标签）、§4（`MsCallFrame`：`proto`、返回地址、求值栈基址、upvalue 数组）。
- [05-bytecode-proto.md](05-bytecode-proto.md)：指令格式与操作数约定——`CALL`/`TAIL_CALL`：`A` = 位置参数个数；`CALL_KW`：`A` = 位置参数个数、`Bx` = 关键字名表的常量池下标；`MAKE_FUNCTION`/`MAKE_LAMBDA`：`Bx` = 子 Proto 的常量池下标；`RETURN`：返回值在栈顶。
- [04-parser-ast.md](04-parser-ast.md)：`MS_AST_FUNC_DECL`/`MS_AST_PARAM`（`MsParamKind` 区分普通/`*`/`**`，`defaultValue` 子树）、`MS_AST_LAMBDA`、`MS_AST_CALL`（`args` 中关键字实参为 `MS_AST_KW_ARG`）；参数次序/重名已由 parser 检查（E207/E208）；`lambda` 参数仅为标识符列表（无默认值/`*`/`**`）。
- [10-builtin-functions.md](10-builtin-functions.md) §4：C 函数关键字参数的**假定约定**（名字/值交错追加 `argv` 尾部并计入 `argc`），本任务将其定稿为正式约定（见「详细设计」第 5 节），实现时回头对齐任务 10 的 `print`。
- [04-exceptions.md](../language/04-exceptions.md) §4：内建异常层级中的 `TypeError`/`RecursionError` 名字；异常对象落地（任务 23）前表现为错误槽消息前缀（对齐任务 10 的做法）。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、include guard、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [11-project-layout.md](../language/11-project-layout.md) §1（`src/vm/` 目录位置）、§4（任务 09 起 ms 脚本测试层）。

规范歧义的处理（实现与评审时以本节口径为准）：

1. **`*args` 打包为 list 而非 tuple。** 03-syntax §4 注明 `*args` 是 tuple，但 tuple 类型属任务 32（v0.2），本任务阶段只有 list/dict（创建原语来自任务 06 对象模型，容器语法语义属任务 16）。v0.1 一律打包为新 list，脚本可见行为（`len`/下标/迭代）与 tuple 一致；任务 32 落地 tuple 后切换打包类型，行为不变。
2. **多值 `return` 打包为 list。** 规范只给了 `return exprList` 语法，未定运行时表示。v0.1 编译为 `BUILD_LIST n` + `RETURN`，调用方经解包赋值（`a, b = f()`）拆开；任务 32 后改为 `BUILD_TUPLE`。
3. **默认参数必须集中在普通参数尾部**（无默认值参数不得出现在有默认值参数之后）。EBNF 未强制，但「跳过中间参数按名绑定」语义复杂且无收益，对齐 Python；编译器在生成期检查，违例报编译错误（错误码以任务 07 的错误码表定名为准）。
4. **`CALL_KW` 的关键字名表是常量池中的字符串 list 对象**，元素顺序与栈上关键字值一一对应；任务 05 只说「名字表」，此处定稿。
5. **`MsProto` 增补 `paramNames` 与 `defaultCount` 两个字段**（任务 05 结构的扩展，见「详细设计」第 2 节）；任务 14 将再增 `upvalueCount`。实现时与任务 05 代码对齐。
6. **嵌套函数引用外层局部变量暂为编译错误。** 名字解析的「闭包外层」段依赖 upvalue（任务 14）；本任务中嵌套函数只可引用自身局部与模块全局，编译器解析到外层局部名时报编译错误并在任务 14 解除。

以下接口名在依赖任务文档（06 对象模型、07 编译器、08 VM 执行核心）中尚未定稿，本文为其假定命名：`msNewFunction` 等对象创建原语、`msNewList`/`msListAppend`/`msNewDict`/`msDictSet`、`struct MsCoroutine` 的帧栈字段与 `msVmPushFrame` 等帧原语、编译器入口 `struct MsCompiler` 及其发射助手。**实现时以对应任务文档定名为准。**

## 详细设计

### 1. 文件与模块边界

- 新建 `src/vm/ms_function.h`（include guard `MSLANG_SRC_VM_MS_FUNCTION_H_`）与 `src/vm/ms_function.c`：函数对象 `struct MsFunction`、创建/装配接口、可调用判定、参数绑定算法。头文件自包含（include `<stdbool.h>`、`<mslang/state.h>`，前向声明 `struct MsProto`/`struct MsString`/`struct MsUpvalue`）。
- 编译器侧（任务 07 的 `src/compiler/`）新增：`funcDecl`/`lambda`/调用表达式/`return` 的字节码生成（见第 9 节），不新增文件。
- VM 侧（任务 08 的 `src/vm/ms_vm.c`）新增：`MS_OP_CALL`/`MS_OP_CALL_KW`/`MS_OP_TAIL_CALL`/`MS_OP_RETURN`/`MS_OP_MAKE_FUNCTION`/`MS_OP_MAKE_LAMBDA` 六个分派分支，帧原语沿用任务 08。
- 堆分配纪律：函数对象经任务 06 的 GC 对象分配原语（假定名 `msObjectNewFunction`，内部走 `msAlloc` 系）；绑定用的 scratch 缓冲区挂在协程结构上、经 `msRealloc` 增长、随协程销毁释放。

### 2. 函数对象与 MsProto 增补

```c
// src/vm/ms_function.h
struct MsFunction {
  MsObjectHeader header;         // object model (task 06)
  struct MsProto* proto;         // compiled body (constants, code, metadata)
  struct MsObject** defaults;    // default values, evaluated once at the definition site
  int defaultsLen;               // == proto->defaultCount
  struct MsUpvalue** upvalues;   // task 14; NULL/0 in this task
  int upvaluesLen;
};

// Creates a function object wrapping proto with empty defaults/upvalues.
// The MAKE_FUNCTION/MAKE_LAMBDA handler attaches popped arrays right after.
MsObject* msNewFunction(MsState* L, struct MsProto* proto);

// Hands ownership of the popped definition-site defaults array to fn
// (defaults may be NULL when len is 0). Called only by the MAKE_* handlers.
void msFunctionAttachDefaults(struct MsFunction* fn, struct MsObject** defaults, int defaultsLen);

// True when obj may sit in the callee slot of CALL (MS function or C
// function; the __call__ protocol arrives with task 25).
bool msFunctionIsCallable(const struct MsObject* obj);
```

`MsProto` 增补（在任务 05 的结构上追加，字段语义如下）：

```c
struct MsString** paramNames;   // normal-parameter names, paramCount entries (excludes * and **)
int defaultCount;               // trailing normal params carrying a default value
```

参数槽位约定：`paramCount` 只计普通参数（不含 `*`/`**`）；`*args` 参数占槽 `paramCount`（当 `hasVarArgs`），`**kwargs` 参数占槽 `paramCount + (hasVarArgs ? 1 : 0)`（当 `hasKwArgs`）；普通局部变量槽位排在其后。编译器据此分配 `localCount`，满足 `localCount >= paramCount + (hasVarArgs ? 1 : 0) + (hasKwArgs ? 1 : 0)`。

### 3. 调用约定：栈布局与帧建立

`MS_OP_CALL`（`A` = 位置参数个数 n）执行前的调用方求值栈：

```
... | callable | arg0 | ... | arg(n-1) |  ← 栈顶
```

`MS_OP_CALL_KW`（`A` = n，`Bx` → 常量池中的关键字名表 list，含 k 个 `MsString`）：

```
... | callable | arg0 | ... | arg(n-1) | kwv0 | ... | kwv(k-1) |  ← 栈顶
```

约束：n ≤ 255（`A` 操作数 8 位，编译器超限报错）；k ≤ 65535（常量池下标 16 位，实际受名表长度限制）。栈上总实参数为 n + k。

帧建立流程（callee 为 `MS_TYPE_FUNCTION`，对应 VM 助手 `msVmCallFunction`，假定名）：

1. 检查调用深度：当前协程帧数达到 `MS_VM_MAX_FRAMES`（取 1000，定义在 VM 内部头文件，与任务 08 对齐）时置错误槽 `RecursionError: maximum recursion depth exceeded`，返回 `MS_ERROR_RUNTIME`。
2. `base = 栈顶 - (n + k)`，`callable` 位于 `base - 1`。
3. 参数绑定（第 4 节）：结果写入协程持有的 scratch 槽数组（长度 S = `paramCount` + var/kw 占位），绑定失败（TypeError）直接中止，实参与 callable 由统一错误路径清栈。
4. 窗口落成：弹出全部 n + k 个实参；把 scratch 的 S 个槽值写入 `base` 起的连续槽位；窗口扩展至 `localCount`，其余槽补 nil；栈顶设为 `base + localCount`，求值栈从窗口上方重新计起。
5. 压入新 `MsCallFrame`（任务 08）：`proto`、返回地址（当前 pc）、求值栈基址 `base`、upvalue 数组（本任务恒空）；pc 跳到 `proto->code` 起点。

scratch 采用「先绑定后写回」的两阶段方案，避免写回覆盖未读实参；scratch 中的对象在绑定期间必须遵守根纪律（任务 17 落地时注册为 GC 根，v0.1 由「关闭时统一回收」策略兜底，见 [09-minimal-interpreter.md](09-minimal-interpreter.md) §2）。

`MS_OP_RETURN` 帧拆除：返回值在栈顶；弹出整个窗口与 `callable` 槽，把返回值压入 `callable` 原槽位（即调用方视角「实参与 callable 被单个返回值替代」）；恢复调用方帧与返回地址。编译器保证每个 Proto 末尾有 `RETURN`（[09-minimal-interpreter.md](09-minimal-interpreter.md) §3 的顶层约定同样适用于函数 Proto）。

callee 非可调用对象：置错误槽 `TypeError: '<type>' object is not callable`，`MS_ERROR_RUNTIME`。

### 4. 参数绑定算法

对外视图与入口：

```c
struct MsCallArgs {                    // read-only view over the caller's stack
  struct MsObject** pos;               // positional args, posLen entries
  int posLen;
  struct MsString** kwNames;           // keyword names from the const-pool table; NULL for CALL
  struct MsObject** kwValues;          // keyword values on the stack, kwLen entries
  int kwLen;
};

// Binds args into out slots[0 .. P + var + kw), where P = fn->proto->paramCount.
// Packs surplus positionals into a fresh list (*args) and unmatched keywords
// into a fresh dict (**kwargs); fills defaults by shared reference (no copy).
// On mismatch reports TypeError via the error slot and returns
// MS_ERROR_RUNTIME; OOM propagates as MS_ERROR_OOM.
MsResult msFunctionBindArgs(MsState* L, struct MsFunction* fn,
    const struct MsCallArgs* args, struct MsObject** slots);
```

算法步骤（P = `paramCount`，`filled` 为 P 位位图，分配在栈上小数组）：

1. `filled` 清零。位置段：`m = min(posLen, P)`，`slots[0..m) = pos[0..m)` 并标记。`posLen > P` 时：`hasVarArgs` 则超出部分留给第 5 步打包，否则报 `TypeError: <name>() takes at most P positional argument(s) (posLen given)`。
2. 关键字段：对每个 `(kwNames[j], kwValues[j])` 线性扫描 `paramNames[0..P)`（P 很小，线性即可）：
   - 命中下标 i：`filled[i]` 已置位 → `TypeError: <name>() got multiple values for argument '<kw>'`；否则 `slots[i] = kwValues[j]` 并标记。
   - 未命中：`hasKwArgs` 则 `msDictSet(kwargs, kwNames[j], kwValues[j])`（**kwargs dict 在首个未命中关键字时惰性创建并入根），否则报 `TypeError: <name>() got an unexpected keyword argument '<kw>'`。
3. 默认填充：对 `i ∈ [0, P)` 中未标记者：`i >= P - defaultsLen` 时 `slots[i] = defaults[i - (P - defaultsLen)]`（**共享引用，不复制**，见第 6 节）；否则报 `TypeError: <name>() missing required positional argument '<param>'`。
4. `*args` 槽：`hasVarArgs` 时把 `pos[P..posLen)` 打包为新 list（`posLen <= P` 时为空 list）写入 `slots[P]`；**总是打包**，不为 NULL。
5. `**kwargs` 槽：`hasVarArgs` 时写 `slots[P + 1]`，否则写 `slots[P]`；第 2 步未触发创建时补空 dict。

打包分配点之间存活的中间对象（kwargs dict、已绑定的 slots）遵守根纪律（[09-c-api.md](../language/09-c-api.md) §3）。

### 5. C 函数的关键字参数约定（定稿任务 10 的假定）

callee 为 `MS_TYPE_C_FUNCTION` 时不建帧，VM 把实参整理为连续 `argv` 后直接回调（签名 `MsObject* (*)(MsState* L, int64_t argc, MsObject** argv)`，[09-c-api.md](../language/09-c-api.md) §9）：

- `CALL`：`argc = n`，`argv[0..n)` 即位置参数（借用栈区，不复制）。
- `CALL_KW`：`argc = n + 2k`；`argv[0..n)` 为位置参数；`argv[n + 2j]` 为第 j 个关键字名的 `MsString` 对象，`argv[n + 2j + 1]` 为其值（名字/值交错追加在尾部，与任务 10 §4 的假定一致，此处定稿）。因栈上关键字值连续而约定要求交错，VM 经 scratch 重排后传入。
- 返回值压栈替代 callable 与实参区；返回 `NULL` 表示错误槽已置，VM 中止当前执行（v0.1）或转为异常（任务 23 后）。

### 6. 默认参数：定义时求值与共享语义

- 默认值表达式由编译器生成在**外层 Proto 的定义点**（`MAKE_FUNCTION` 之前），按声明顺序求值并压栈；`MAKE_FUNCTION` 弹出 `proto->defaultCount` 个值（其下若有 upvalue 则先弹 upvalue，本任务 `upvalueCount` 恒 0），装配进 `MsFunction.defaults`。定义点语义保证「函数对象存在时默认值已固定」，与 Python 的 `__defaults__` 一致。
- 绑定时 `slots[i] = defaults[...]` 直接引用同一对象，**不复制**：可变默认值（list/dict）在多次调用间共享，函数体内对其原地修改会累积——这是规范明示的陷阱（03-syntax §4），脚本测试须固化该行为（见测试方案）。
- `MAKE_FUNCTION` 栈布局（d = `defaultCount`，u = `upvalueCount` = 0）：

```
... | default0 | ... | default(d-1) |  ← 栈顶（定义点按声明序压入）
执行后：弹出 d + u 个值，压入新 MsFunction 对象
```

- `MAKE_LAMBDA` 语义与 `defaultCount == 0` 的 `MAKE_FUNCTION` 完全相同；保留独立操作码仅为转储/诊断可区分（任务 05 已定枚举，本任务不合并）。

### 7. return 与多返回值

编译器对 `returnStmt` 的生成规则：

- `return`（无值）：`LOAD_NIL` + `RETURN`。
- `return e`：生成 `e`，再 `RETURN`。
- `return e0, e1, ...`（n ≥ 2）：依次生成各表达式，`BUILD_LIST n` + `RETURN`（歧义说明第 2 条：v0.1 打包 list，任务 32 后改 tuple）。调用方 `a, b = f()` 的解包由赋值侧的 `MS_OP_UNPACK` 完成（任务 11/07 已约定解包赋值走 `UNPACK`），本任务不涉及。

### 8. 尾调用

- 编译器识别 `return <调用表达式>` 形态，把其中的 `CALL`/`CALL_KW` 改发为 `TAIL_CALL`（操作数同 `CALL`）。
- VM 语义：callee 为 `MS_TYPE_FUNCTION` 时，先在 scratch 完成参数绑定（第 4 节），再回收当前帧（本任务无 upvalue；任务 14 落地时在回收前补 `CLOSE_UPVALS`），把绑定结果搬移到当前帧 `base`（覆盖旧窗口），pc 归零复用帧——调用深度不增长。callee 为 C 函数时退化为普通调用 + 用其结果立即 `RETURN`。
- 不做更激进的部分尾调用/互递归之外的检测；非尾位置的调用一律普通 `CALL`。

### 9. lambda 与编译器生成约定

- `lambda x, y: expr`：编译为匿名子 Proto——`paramCount = 参数个数`、`hasVarArgs = hasKwArgs = false`、`defaultCount = 0`、`name = "<lambda>"`；函数体即 `expr` 的生成代码 + `RETURN`（单表达式体即返回值）。定义点发射 `MAKE_LAMBDA Bx`。
- `func name(params) { body }`：编译子 Proto（`name` 为函数名；`paramNames` 收集普通参数名；默认参数表达式留在外层定义点生成）后发射 `MAKE_FUNCTION Bx`；顶层定义随后 `STORE_GLOBAL name`，函数内定义 `STORE_LOCAL` 对应槽位。`isAsync` 置位照传（parser 已给标志），本任务对 `isAsync` 函数的调用统一报 `RuntimeError: async call requires the scheduler (v0.3)`。
- 调用表达式：被调表达式 → 位置实参 → 关键字实参值依次压栈；无关键字实参发 `CALL A=n`，有则把关键字名按压栈次序组成字符串 list 入常量池（`msProtoAddConst`）并发 `CALL_KW A=n, Bx=<表下标>`。
- 编译期检查（本任务新增，错误码归入任务 07 的 E3xx 表，定名以其为准）：默认参数不尾随（歧义说明第 3 条）；位置实参超过 255；同一调用点关键字名重复（名表为字面量，可静态判重）；嵌套函数引用外层局部（歧义说明第 6 条）。

### 10. 错误消息一览

异常对象落地（任务 23）前均为错误槽消息 + `MS_ERROR_RUNTIME`（CLI 退出码 1），消息前缀即异常类型名（对齐任务 10 风格）：

| 情形 | 消息 |
|---|---|
| 调用非可调用对象 | `TypeError: 'int' object is not callable` |
| 位置参数过多（无 `*args`） | `TypeError: f() takes at most 2 positional argument(s) (3 given)` |
| 缺必需参数 | `TypeError: f() missing required positional argument 'b'` |
| 关键字重复绑定 | `TypeError: f() got multiple values for argument 'a'` |
| 未知关键字（无 `**kwargs`） | `TypeError: f() got an unexpected keyword argument 'x'` |
| 调用深度超限 | `RecursionError: maximum recursion depth exceeded` |
| 调用 async 函数 | `RuntimeError: async call requires the scheduler (v0.3)` |

## 实现步骤

1. 扩展 `MsProto`：追加 `paramNames`/`defaultCount` 字段并接通构建器/释放路径（与任务 05 实现对齐）。验证：任务 05 既有 C 单元测试无回归，新字段随 Proto 正确移交。
2. 建 `src/vm/ms_function.{h,c}`：`struct MsFunction`、`msNewFunction`/`msFunctionAttachDefaults`/`msFunctionIsCallable`。验证：创建/装配后经任务 02 分配统计断言无泄漏。
3. 编译器生成 `funcDecl`：子 Proto 编译（参数槽位约定、`paramNames`/`defaultCount` 填写）、定义点默认值表达式发射、`MAKE_FUNCTION` + `STORE_GLOBAL`/`STORE_LOCAL`。验证：ms 脚本 `func add(a, b) { return a + b }; assert(add(1, 2) == 3)`。
4. VM 实现 `CALL`/`RETURN` 与帧建立/拆除（第 3 节）、非可调用 TypeError、深度上限。验证：位置参数、缺省返回 nil（无 `return` 的函数返回 nil）、嵌套调用链、递归阶乘。
5. 实现 `msFunctionBindArgs` 全算法（默认值、`*args` 打包、缺参/超参错误）。验证：默认参数各组合、`*args` 收集与空收集、错误消息断言。
6. 实现 `CALL_KW`：名表读取、关键字匹配/查重/未知名、`**kwargs` 打包、C 函数交错 argv 通道；回头对齐任务 10 `print` 的 kwarg 扫描。验证：关键字实参乱序绑定、`print("a", "b", sep=", ")` 输出比对。
7. 实现 `return` 三形态与 `TAIL_CALL`（含 C 函数退化路径）。验证：多值返回解包、尾调用深递归不溢出。
8. 实现 `lambda` 编译与 `MAKE_LAMBDA`。验证：`f := lambda x: x * 2; assert(f(21) == 42)`、lambda 作实参传递。
9. 编译期检查四条（默认参数尾随、实参上限、关键字重名、外层局部引用拒绝）。验证：各负例脚本按预期退出码失败。
10. 全量回归：任务 09/10/11/12 既有 `tests/ms/` 脚本与本任务脚本经 `run_tests.py` 与 `ctest --test-dir build` 全绿；Debug 构建（ASAN / `/RTC`）无内存错误，`msCloseState` 后无残余分配计数。

## 测试方案

本任务晚于最小可运行解释器（任务 09），一律用 ms 脚本测试，由仓库根 `run_tests.py` 驱动 mslang CLI；`testing` 模块（任务 40）之前用内建 `assert` + `print` 自断言。正向脚本成功路径以 `print("<name> ok")` 收尾、退出码 0；需要精确输出比对的脚本配 `<name>.out` 同伴文件；负向用例配 `<name>.exit`（内容 `1`，编译错误类为 `2`）声明预期退出码。测试目录 `tests/ms/functions/`（脚本随实现编写，本文只列清单与覆盖点）：

- `call_basic.ms`：定义与调用、位置参数、无 `return` 函数返回 nil、函数作一等值（赋给变量、作实参传递、作返回值）、嵌套调用链 `f(g(x))`、递归（阶乘/斐波那契断言语句级结果）。
- `default_args.ms`：默认参数省略/部分省略/全部给出；默认值定义时只求值一次（用带副作用的默认表达式——如自增全局计数器——断言调用多次后计数不变）；**可变默认参数共享语义**：`func push(x, xs = []) { ... }` 两次调用后同一 list 累积（固化 Python 同款陷阱行为）。
- `keyword_args.ms` + `keyword_args.out`：关键字实参乱序与位置/关键字混用（位置在前）；经 `print` 的 `sep`/`end` 关键字（任务 10 的 C 函数 kwarg 通道）输出比对。
- `varargs.ms`：`*args` 收集 0/1/多个实参；`len` 与逐元素断言；`func f(a, *rest)` 混合形态。
- `kwargs.ms`：`**kwargs` 收集为 dict（键数、逐键断言）；空调用得空 dict；`func f(a, b = 1, *rest, **kw)` 全组合形态（03-syntax §4 的固定次序）。
- `multi_return.ms`：`return a, b` 经 `x, y = f()` 解包断言；单值与零值（nil）返回。
- `lambda.ms`：空参与多参 lambda；lambda 作实参（`apply(f, x)` 风格的自定义高阶函数）；lambda 体为单个表达式（含条件表达式）。
- `tail_call.ms`：`return f(...)` 形态的深度互递归（如 100000 层奇偶判定）不触发深度错误；对照的非尾深递归报 RecursionError（另配负例）。
- 负例（各配 `.exit`，预期退出码 1，错误消息含对应前缀）：`err_not_callable.ms`（`x := 1; x()` → TypeError）；`err_too_many_args.ms`；`err_missing_arg.ms`；`err_dup_kwarg.ms`（`f(1, a=2)` → multiple values）；`err_unknown_kwarg.ms`；`err_recursion.ms`（非尾无限递归 → RecursionError）；`err_async_call.ms`（调用 `async func` → RuntimeError）。
- 编译错误负例（预期退出码 2）：`err_default_order.ms`（默认参数后出现无默认值参数）、`err_kw_dup_literal.ms`（同一调用点重复关键字名）、`err_outer_local.ms`（嵌套函数引用外层局部，任务 14 后转为正例并移除）。

## 验收标准

- [ ] `src/vm/ms_function.h` / `ms_function.c` 存在，guard 为 `MSLANG_SRC_VM_MS_FUNCTION_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] `MsProto` 增补 `paramNames`/`defaultCount` 与本文一致；参数槽位约定（`*`/`**` 占槽位置）落实，任务 05 既有测试无回归。
- [ ] 调用约定的栈布局、两阶段帧建立、`RETURN` 帧拆除与本文一致；`CALL_KW` 名表为常量池字符串 list；C 函数 kwarg 交错约定落实且任务 10 的 `print` kwarg 用例通过。
- [ ] 参数绑定算法覆盖位置/默认/关键字/`*args`/`**kwargs` 全部路径：默认值**定义时求值一次**且**绑定共享引用**（可变默认参数共享行为有脚本固化）；`*args` 打包为 list、`**kwargs` 打包为 dict（含空收集）；七类运行错误的消息与第 10 节一致。
- [ ] `return` 零值（nil）/单值/多值（v0.1 打包 list）三形态正确；`return f(...)` 编译为 `TAIL_CALL` 且深度互递归不增长调用栈。
- [ ] `lambda` 编译为匿名子 Proto，运行语义与 `func` 一致；嵌套函数可定义但引用外层局部报编译错误（任务 14 解除）；调用 `async func` 报 RuntimeError。
- [ ] 调用深度上限 1000，超限报 `RecursionError`；Debug 构建（ASAN / `/RTC`）无内存错误与泄漏（`msCloseState` 后无残余分配计数）。
- [ ] `tests/ms/functions/` 覆盖「测试方案」全部清单项（含 `.out` 输出比对与 `.exit` 负例），`python run_tests.py` 与 `ctest --test-dir build` 全数通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；假定接口名（`msNewFunction`、`msNewList`/`msDictSet`、帧原语、`struct MsCompiler` 等）在实现时已与任务 06/07/08 文档对齐；任务 10 的 kwarg 假定约定已按本文第 5 节回改。
