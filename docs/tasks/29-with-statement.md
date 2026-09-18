# 29 with 语句

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [23 异常系统](23-exceptions.md)、[25 class 继承与魔术方法](25-class-inheritance.md) |

## 任务目标

交付 mslang `with` 语句（[03-syntax.md](../language/03-syntax.md) §3.2）的编译期支持：

- **协议调用展开**：`with expr as x { body }` 在编译期脱糖为"求值 `expr` → 绑定 `__exit__` → 调用 `__enter__()` 并把结果绑定给 `as` 目标 → 受异常表保护的 `body` → 退出时调用 `__exit__(excType, excValue, traceback)`"的字节码序列，零运行时登记开销（复用任务 23 的零成本 try 异常表）。
- **异常语义**：正常退出（含 `return`/`break`/`continue` 穿越）以 `(nil, nil, nil)` 调用 `__exit__`；块内抛异常时以 `(异常类对象, 异常实例, traceback)` 调用，`__exit__` 返回真值（02-types §2 真值规则）则吞掉异常、否则原异常继续传播；`__exit__` 自身抛异常时新异常取代原异常传播。
- **多上下文管理器**：03-syntax 的 EBNF 只定义单管理器形式（无 Python 式逗号并列），多管理器经嵌套 `with` 表达，本任务保证嵌套语义正确——退出按 LIFO 次序、异常逐层经各层 `__exit__` 处理。
- **协议校验**：对象缺 `__enter__` 或 `__exit__` 抛 `AttributeError`；`__exit__` 的绑定先于 `__enter__` 的调用（缺 `__exit__` 时 `__enter__` 不会被调用，与 Python 一致）。

语法与 AST 节点（`MS_AST_WITH`，`as` 绑定可空）已由任务 04 落地；本任务只扩展编译器（`src/compiler/`）与 VM（`src/vm/`）：新增一条指令 `MS_OP_EXC_INFO`、实现 `with` 的代码生成模板、接通控制流出口的退出副本。完成后通过 `tests/ms/with/` 的 ms 脚本验证。本任务只交付设计文档；实现与测试代码随实现任务编写。

## 设计依据

- [03-syntax.md](../language/03-syntax.md)
  - §3 语句 EBNF：`withStmt = "with" expr [ "as" identifier ] block`——只定义单管理器形式。
  - §3.2 语义：`expr.__enter__()` 的值绑定给 `as` 目标；块退出（含异常）时调用 `__exit__(excType, excValue, traceback)`，其返回真值决定是否吞掉异常。
  - §8：块级作用域；名字解析顺序 局部 → 闭包外层 → 模块全局 → 内建。
- [02-types.md](../language/02-types.md)
  - §2 真值规则：`__exit__` 返回值按此表判定是否吞异常（`nil`/`false`/零/空容器为假）。
  - §8 魔术方法协议表：`__enter__`/`__exit__` 属"调用/上下文"类魔术方法。
- [08-vm-internals.md](../language/08-vm-internals.md)
  - §2.1：`MsProto` 布局含 `tryBlocks` 异常表；§2.2：调用指令 `MS_OP_LOAD_METHOD`/`MS_OP_CALL_METHOD`/`MS_OP_CALL`、跳转指令 `MS_OP_JUMP_IF_TRUE`/`MS_OP_JUMP_IF_FALSE`、属性指令 `MS_OP_GET_ATTR`、异常指令 `MS_OP_RERAISE`；指令总数目标 ≤ 80（本任务新增一条后仍在预算内）。
  - §4：帧 `MsCallFrame` 的局部槽窗口与求值栈；隐藏槽（编译器分配、脚本不可寻址的局部槽）随帧成为 GC 根。
- [任务 23 异常系统](23-exceptions.md)：`MsTryBlock` 扩展布局（`pcStart/pcEnd/handlerPc/finallyPc/excSlot/pendingSlot`）、`MS_OP_SETUP_TRY`/`MS_OP_POP_TRY` 仅作编译期锚点、handler 进入时异常对象写入 `excSlot`、`MS_OP_RERAISE` 重抛语义、finally 三类副本（fall-through / 控制流出口 / 异常路径）的生成机制、`msVmRaise` 展开算法、隐式 `__context__` 链化的守卫条目判据、traceback 在脚本侧物化为 list of dict（`{"file", "line", "function"}`）。本任务的代码生成直接复用其异常表发射与控制流副本机制。
- [任务 15 class 基础](15-class-basics.md)：`MS_OP_LOAD_METHOD`/`MS_OP_CALL_METHOD` 的免分配方法调用约定（`LOAD_METHOD Bx` 把 `[recv]` 变换为 `[recvSlot, callable]`，`CALL_METHOD A` 以 `recvSlot` 为 `argv[0]`）；`MS_OP_GET_ATTR` 产生 `MsBoundMethod`；`MS_OP_CALL` 遇 `MS_TYPE_BOUND_METHOD` 自动插入接收者。
- [任务 25 class 继承与魔术方法](25-class-inheritance.md)：该任务文档尚未定稿。本文假定魔术方法（含 `__enter__`/`__exit__`）的查找沿类 MRO 线性链进行（与 08-vm-internals §4 的属性查找约定一致），实例属性遮蔽与否、`__exit__` 是否允许实例字典覆盖等细节以任务 25 文档定名为准。
- 任务 07（编译器）文档尚未定稿。本文引用的编译器内部接口（`struct MsCompiler`、作用域栈 `msCompilerPushScope`/`msCompilerPopScope`、局部声明 `msCompilerDeclareLocal`、隐藏槽分配、异常表发射与跳转补丁接口）均为假定命名，实现时以对应任务文档定名为准。
- [10-c-style.md](../language/10-c-style.md)：全部 C 代码遵循其规范（2 空格缩进、120 列行宽、K&R、指针星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。

规范歧义的处理（实现时按本文口径）：

- **多管理器形式**：03-syntax §3 的 `withStmt` 产生式无逗号并列项，与 Python 的 `with a as x, b as y:` 不同。本任务不扩展语法，多管理器一律写作嵌套 `with`；若日后补并列语法，其语义即嵌套脱糖，不改变本任务的任何运行时约定。
- **`as` 绑定的作用域**：规范未规定。取 Python 语义——`as` 目标作为普通局部变量声明在 `with` 语句**所在**作用域（而非 `with` 块引入的新作用域），块结束后仍可见；与外层已有名字冲突时报重声明错误（走任务 07/11 的常规定名检查）。
- **协议方法的查找层级**：Python 在类型上查找 `__enter__`/`__exit__`（实例字典不遮蔽）；ms 规范未区分。本任务按任务 25 的魔术方法统一查找约定执行（假定沿类 MRO），不单独为 `with` 引入口径；若任务 25 定为"实例字典可遮蔽魔术方法"，`with` 随之继承，不视为本任务的偏差。

## 详细设计

### 1. 编译策略决策：脱糖为 try/except 等价物，复用异常表

候选方案二选一：

- **专用运行时指令**（CPython `SETUP_WITH`/`WITH_EXCEPT_START` 式）：VM 维护 with 块栈，进入/退出有运行时登记开销，展开逻辑与任务 23 的异常表展开并行存在两套。
- **编译期脱糖（本任务选定）**：`with` 体编译为异常表保护区间，退出逻辑（正常/异常两路径的 `__exit__` 调用）全部落在静态字节码中，运行时零登记。

选定脱糖方案的理由：

1. **单一展开机制**。任务 23 的 `msVmRaise` 展开只认 `MsTryBlock` 异常表；`with` 的异常路径本质是"一个会吞异常的 except 子句"，直接用同一套查表展开，不引入第二套运行时栈。
2. **零成本进入**。与 try 同理，`with` 的进入不产生任何运行时指令开销（锚点指令编译期删除），代价集中在实际抛出时刻，符合 04-exceptions §6 的性能约定精神。
3. **控制流副本复用**。`return`/`break`/`continue` 穿越 `with` 体时需先调 `__exit__(nil, nil, nil)`——这与任务 23 的"控制流出口 finally 副本"是同构问题，编译器把"以 nil 三参调用已绑定的 `__exit__`"登记为退出 epilogue thunk，直接复用任务 23 的逐层内联机制，穿越多层嵌套 `with`/`try` 时按由内向外顺序逐层内联。

代价（显式接受）：

- 每个 `with` 语句在帧上常驻两个隐藏局部槽（管理器对象、`__exit__` 绑定方法），并有一次 `MsBoundMethod` 分配（`GET_ATTR` 路径，每条 `with` 一次，频次低，可接受；免分配的 `LOAD_METHOD` 缓存方案留待性能路线图）。
- 退出副本随控制流出口数线性复制字节码，与 finally 副本同口径，按 `__exit__` 调用序列极小接受。

### 2. 编译器内部接口（假定命名）

全部函数为 `src/compiler/` 内文件级 `static`：

```c
// Dispatches MS_AST_WITH. Evaluates the manager expression once, binds
// __exit__ before __enter__ runs, emits the protected body and both exit
// paths. Errors go to the shared diagnostic list (compile-time codes only;
// protocol violations are runtime AttributeError/TypeError, not compile
// errors).
static void compileWithStmt(struct MsCompiler* c, const struct MsAst* node);

// Emits the exit call "exitSlot(nil, nil, nil); drop result". Used verbatim
// as the normal fall-through tail and registered as the exit-epilogue thunk
// for control-flow statements (return/break/continue) crossing this with
// body (task 23's epilogue-copy machinery invokes it at each crossing site).
static void compileWithExitEpilogue(struct MsCompiler* c, uint16_t exitSlot);
```

复用任务 07/23 的既有接口（假定命名，实现时以对应任务文档定名为准）：

- `msCompilerDeclareHiddenLocal(c)` → 返回帧内槽位号：分配脚本不可寻址的隐藏局部（名字解析不可达、参与 `localCount` 与栈深统计、随帧成为 GC 根）。
- `msCompilerEmitTryAnchor(c)` / `msCompilerEmitTryBlock(c, &entry)`：锚点发射与 `MsTryBlock` 条目登记（任务 23 机制）。
- `msCompilerPushExitEpilogue(c, thunk)` / `msCompilerPopExitEpilogue(c)`：任务 23 为 finally 副本维护的 epilogue 栈；`with` 在编译体期间把"nil 三参退出调用"压栈，`return`/`break`/`continue` 代码生成穿越时逐层回调。

`MS_AST_WITH` 节点字段按任务 04 的体例补全为 `withStmt` 联合分支：`{ struct MsAst* expr; const char* name; size_t nameLen; struct MsAst* body; }`（`name == NULL` 表示无 `as` 绑定）。

### 3. 新指令 `MS_OP_EXC_INFO`

handler 入口处的异常对象位于 `excSlot`，但 `__exit__` 需要三个独立参数（类对象、实例、traceback），现有指令无法从异常实例取回其类对象（`type()` 内建可被脚本遮蔽，不可用于协议内部路径）。为此在异常类别下新增一条指令（总数仍在 80 条预算内）：

| 指令 | 操作数 | 栈行为 | 语义 |
|---|---|---|---|
| `MS_OP_EXC_INFO` | A = 帧内槽位 | 压入 3 个值 | 读槽 A 中的异常对象，依次压入：异常类对象（`MsType*`）、异常实例本身、traceback（脚本侧物化形态，list of dict，任务 23 §异常实例的物化约定）；槽 A 非异常对象为编译器缺陷，Debug 构建 `MS_ASSERT`，Release 置运行时错误 |

traceback 物化按任务 23 的约定惰性进行：`MS_OP_EXC_INFO` 是唯一的脚本侧物化触发点之外的新增触发点，实现上直接调用任务 23 的物化函数（假定名 `msExceptionMaterializeTraceback`，实现时以任务 23 文档定名为准），未捕获打印路径不受影响。

### 4. 单管理器的字节码模板

以 `with expr as x { body }` 为例。槽位：`m` = 隐藏管理器槽，`e` = 隐藏 `__exit__` 绑定方法槽，`x` = `as` 绑定（声明在 `with` 所在作用域），`exc` = 异常表条目的 `excSlot`（隐藏槽）。

```
  <expr 求值>                        ; [mgr]（只求值一次）
  MS_OP_STORE_LOCAL m
  ; ---- 先绑定 __exit__：缺失在此报错，__enter__ 尚未调用 ----
  MS_OP_LOAD_LOCAL m
  MS_OP_GET_ATTR const("__exit__")   ; [boundExit]（魔术方法查找，任务 25 口径）
  MS_OP_STORE_LOCAL e
  ; ---- 调用 __enter__ ----
  MS_OP_LOAD_LOCAL m
  MS_OP_LOAD_METHOD const("__enter__")
  MS_OP_CALL_METHOD 0                ; [value]
  MS_OP_STORE_LOCAL x                ; 无 as 时改为 MS_OP_POP
  ; ==== 受保护区间 [bodyStart, bodyEnd)：异常表条目 H ====
  ;      H = {bodyStart, bodyEnd, handlerPc, 0, exc, MS_NO_SLOT}
  <body>
  ; ---- 正常路径：__exit__(nil, nil, nil)，返回值丢弃 ----
  <compileWithExitEpilogue(e)>       ; LOAD_LOCAL e; LOAD_NIL×3; CALL 3; POP
  MS_OP_JUMP -> after
  ; ==== 异常处理入口 ====
handlerPc:                           ; 异常对象已在 exc（任务 23 展开算法写入）
  MS_OP_LOAD_LOCAL e
  MS_OP_EXC_INFO exc                 ; [e, excType, excValue, traceback]
  MS_OP_CALL 3                       ; BOUND_METHOD 分支自动插入接收者 → [result]
  MS_OP_JUMP_IF_TRUE -> suppress     ; 真值：吞异常（假定条件跳转弹出操作数，
                                     ; 若任务 12 约定不弹则此处补 MS_OP_POP）
  MS_OP_RERAISE exc                  ; 假：原异常继续传播（不重置 traceback）
suppress:
after:
```

要点：

- **`__exit__` 先于 `__enter__` 绑定**。`GET_ATTR` 缺失 `__exit__` 时在 `__enter__` 调用之前抛 `AttributeError`，与 Python 的"两个方法都先解析再进入"一致；`__enter__` 缺失经 `LOAD_METHOD` 的常规属性缺失路径抛 `AttributeError`。
- **异常路径只调一次 `__exit__`**。异常表条目 `finallyPc = 0`、`handlerPc != 0`，展开算法把异常写入 `exc` 后直接跳入 handler；handler 末尾要么 `RERAISE`（此时该异常向外层继续展开，外层 `with`/`try` 的条目照常命中），要么落入 `suppress` 汇合点。吞异常后 `exc` 槽内容不再使用，无清理指令。
- **`__exit__` 自身抛异常**：发生在 handler 覆盖区内。编译器按任务 23 的"handler 覆盖区守卫条目"机制，为 `[handlerPc, handlerEndPc)` 额外登记一条 `handlerPc=0`、`finallyPc=0`、仅 `excSlot` 非 `MS_NO_SLOT` 的守卫条目——它不拦截展开（无 handler/finally 入口，视为无命中继续向外展开），只充当隐式 `__context__` 链化的判据：新异常的 `__context__` 自动挂到正在处理的 `exc` 上（任务 23 §抛出与栈展开第 2 步）。`__exit__` 新异常由此取代原异常传播，与 Python 一致。
- **正常路径 `__exit__` 的返回值无条件丢弃**（Python 同）；该调用自身抛异常则按常规异常路径传播（此时已无保护区间覆盖，`after` 之前的收尾段不在条目 H 内，异常直接向更外层展开）。
- **`__exit__` 参数签名为 3 个位置参数**（`self` 之外）：绑定方法经 `MS_OP_CALL 3` 调用，参数个数不符由任务 13 的调用装配路径抛 `TypeError`，`with` 不另做检查。
- **真值判定**经 `MS_OP_JUMP_IF_TRUE` 的既有真值规则（02-types §2，含 `__bool__`/`__len__` 回落），不引入新判定路径。

### 5. 控制流穿越：return / break / continue

`with` 体内的 `return`/`break`/`continue`（含嵌套在块内循环中的）必须先以 `(nil, nil, nil)` 调用 `__exit__` 再执行原控制流：

- 编译 `with` 体期间，把 `compileWithExitEpilogue(e)` 经 `msCompilerPushExitEpilogue` 压入编译器的 epilogue 栈（任务 23 为 finally 副本建立的同一设施）；体编译完成后弹出。
- 任务 23 的 `return`/`break`/`continue` 代码生成在穿越每个登记层时回调对应 thunk 内联副本，`with` 层与 `try/finally` 层在同一栈上混排，穿越顺序由内向外自然正确（如 `try { with res { return } } finally { ... }` 先调 `__exit__` 再执行 finally）。
- epilogue 中 `__exit__` 抛异常或返回真值均不影响原控制流的"值"（`return` 的返回值已先求值暂存）；`__exit__` 抛异常时该异常取代原控制流向传播（与 finally 内 raise 覆盖规则一致）。
- `with` 体对 `loopDepth`/`funcDepth` 语境无影响（不是函数边界，也不引入循环语境）：体内 `break` 仍属外层循环，体内 `return` 仍属外层函数，均经 parser 既有检查。

### 6. 多上下文管理器（嵌套）语义

规范只提供单管理器形式，多管理器写作嵌套：

```ms
with open("a.txt") as f {
    with open("b.txt") as g {
        // ...
    }
}
```

语义由脱糖结构直接导出，无额外机制：

- **进入次序**：外层 `__enter__` 先于内层（表达式求值顺序即嵌套顺序）；内层表达式求值可引用外层的 `as` 绑定。
- **退出次序 LIFO**：内层块的正常/异常出口先调内层 `__exit__`；内层吞掉异常则外层以 `(nil, nil, nil)` 正常退出；内层不吞（或内层 `__exit__` 自身抛异常）则异常传播至外层 handler，外层 `__exit__` 收到该异常并独立决定吞/抛。
- **每层独立的隐藏槽**：嵌套编译时 `msCompilerDeclareHiddenLocal` 逐层分配，`MS_OP_EXC_INFO` 的 A 操作数各自指向本层 `exc` 槽，互不干扰。
- 栈深统计：每嵌套一层，求值栈常驻增量为常数（管理器求值临时 + 调用帧参数），编译器把 `with` 展开段纳入 `MsProto.stackSize` 的最大栈深计算。

### 7. 错误与边界语义

- 缺 `__enter__` / 缺 `__exit__` / 属性存在但不可调用：均沿任务 15/25 的属性与方法调用错误路径抛 `AttributeError`/`TypeError`，`with` 不做编译期检查（协议满足性是运行时性质）。
- `__exit__` 返回值的真值判定经 02-types §2 规则；返回任意类型均可（不强制 bool）。
- 异常参数形态：`excType` 是异常类对象（`MsType`，可与 `except` 子句的类型表达式、`isinstance` 比较），`excValue` 是异常实例（可读 `message`/`__cause__`/`__context__`），`traceback` 是 list of dict（`{"file", "line", "function"}`，与任务 23 的物化约定一致）。
- 无 `as` 形式：`__enter__` 返回值直接 `MS_OP_POP` 丢弃。
- 管理器表达式只求值一次，其结果由隐藏槽持有至 `with` 结束（GC 根：帧局部槽）。
- OOM：`MsBoundMethod` 分配与 `MS_OP_EXC_INFO` 的 traceback 物化分配经任务 06/23 的统一 OOM 路径（抛脚本层 `RuntimeError` 系或按任务 23 既定约定，实现时对齐）；编译期 arena 分配失败返回 `MS_ERROR_OOM`（任务 07 既有约定）。
- 诊断：`with` 的语法检查（`as` 后必须是标识符等）属任务 04 的 parser，本任务不新增错误码。

## 实现步骤

1. 编译器接入 `MS_AST_WITH` 分派骨架：实现 `compileWithStmt`——`expr` 求值、隐藏槽 `m`/`e` 分配（`msCompilerDeclareHiddenLocal`）、`GET_ATTR "__exit__"` 预绑定、`LOAD_METHOD`/`CALL_METHOD 0` 调 `__enter__`、`as` 绑定或 `POP`、正常路径 `compileWithExitEpilogue(e)` + 收尾跳转。验证：ms 脚本用记录型管理器断言 `__enter__`/`__exit__` 各被调一次、正常路径 `__exit__` 三参全为 nil、`as` 绑定值为 `__enter__` 返回值。
2. 异常表条目与 handler 生成：接入任务 23 的锚点/条目发射接口，生成条目 H 与 handler 分派（`EXC_INFO` 占位前先用 `RERAISE` 收尾保证不吞时传播正确）。验证：块内 `raise` 且 `__exit__` 返回 false 时异常原样传出（外层 `except` 捕获到同一实例）。
3. VM 新增 `MS_OP_EXC_INFO`：枚举、编码（A 格式）、分派分支（读槽、取类对象、调任务 23 的 traceback 物化、压三值）。验证：`__exit__` 内断言 `excType` 是 `type(excValue)`、`traceback` 为 list 且元素含 `file`/`line`/`function` 键、行号指向 raise 点。
4. 吞异常判定：`JUMP_IF_TRUE` 接通真值规则；补 handler 覆盖区守卫条目以支持 `__exit__` 内抛异常的隐式 `__context__` 链化。验证：`__exit__` 返回 true 时块内异常被吞、脚本继续执行；`__exit__` 内 raise 的新异常外层可捕获且其 `__context__` 为原异常。
5. 控制流出口副本：`msCompilerPushExitEpilogue`/`Pop` 接入，确认与任务 23 finally 层在同一 epilogue 栈混排。验证：`with` 体内 `return`（返回值已求值后仍先调 `__exit__`）、循环内 `break`/`continue` 各路径的退出调用次序（list 追加记录轨迹）；`try/finally` 与 `with` 互嵌套的逐层次序。
6. 嵌套 `with`：确认逐层隐藏槽与 LIFO 退出无特判需求，补栈深统计断言。验证：双层/三层嵌套的进入与退出次序、内层吞异常后外层收到 nil 三参、内层不吞时外层收到异常并可独立决定。
7. 协议错误与边界：缺 `__enter__`/缺 `__exit__`/不可调用的负例；缺 `__exit__` 时 `__enter__` 未被调用的次序断言；无 `as` 形式；`__exit__` 参数个数不符的 `TypeError`；非异常槽 `EXC_INFO` 的 Debug 断言路径（仅 C 侧构造验证，不进脚本测试）。验证：负例脚本以预期退出码 1 失败且 stderr 含对应异常类型名。
8. 内存与跨平台复查：每条 `with` 路径（正常/吞/传播/控制流穿越）后帧槽无悬挂引用；`msCloseState` 后任务 09 分配统计归零；Debug 构建（ASAN / `/RTC`）跑全部 with 测试无报告；Win/Linux/macOS × Debug/Release 构建通过。验证：`run_tests.py` 全绿。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试（`testing` 模块在任务 40 才存在，本阶段用内建 `assert` + `print` 自断言；负向用例以 `<name>.exit` 同伴文件声明预期退出码 1，由仓库根 `run_tests.py` 驱动，设施约定见任务 09）。本任务只交付设计文档，脚本随实现编写。

测试文件清单（`tests/ms/with/`）与覆盖点；各文件自建记录型管理器 class（`__enter__`/`__exit__` 向共享 list 追加事件），末尾 `print("ok: <用例名>")`：

- `basic.ms`：正常进入/退出次序（`__enter__` → 体 → `__exit__`）；`as` 绑定值为 `__enter__` 返回值；正常路径 `__exit__` 三参全为 nil；`__exit__` 返回值在正常路径被忽略（返回真值也无副作用）；无 `as` 形式正常执行；`as` 绑定在 `with` 块结束后仍可见（作用域决策）。
- `exception_args.ms`：块内 `raise ValueError("boom")`，`__exit__` 内断言 `excType` 与 `type(excValue)` 同一、`excValue.message == "boom"`、`traceback` 为 list 且首元素含 `file`/`line`/`function` 键且行号为 raise 行；返回 true 吞异常后脚本继续执行后续语句。
- `suppress.ms`：`__exit__` 返回各类真值（`true`/非零数/非空串）均吞异常；返回各类假值（`false`/`nil`/`0`/`""`/`[]`）均不吞（异常由外层 `try/except` 接住，断言为同一实例）；`__exit__` 定义 `__bool__` 的管理器按魔术方法判定。
- `nested.ms`：双层与三层嵌套的进入次序（外→内）与退出次序（内→外，LIFO）；内层表达式引用外层 `as` 绑定；内层吞异常后外层收到 nil 三参；内层不吞时外层 `__exit__` 收到原异常且可独立吞/抛；外层吞、内层抛的组合。
- `control_flow.ms`：函数内 `with` 体中 `return`（断言 `__exit__` 在调用者恢复前已执行、返回值为 return 表达式原值）；循环内 `break`/`continue` 穿越 `with`（每轮迭代均配对进入/退出，轨迹断言）；`try { with ... { return } } finally` 的 `__exit__` 先于 finally 执行；`__exit__` 在 `return` 穿越路径上收到 nil 三参。
- `exit_raises.ms`：`__exit__` 内 raise 新异常，外层捕获到新异常且其 `__context__` 为块内原异常；正常路径收尾的 `__exit__` raise 同样传播；`__exit__` 参数个数不符（定义为 0/2 参）抛 `TypeError`。
- 负例脚本（各配 `<name>.exit` 声明退出码 1）：`missing_enter.ms`（管理器只有 `__exit__` → `AttributeError`）、`missing_exit.ms`（只有 `__enter__` → `AttributeError`，且断言 `__enter__` 未被调用——经 stderr 输出或轨迹文件由驱动核对；若驱动不支持则改为正例脚本内 try 捕获断言）、`propagate_uncaught.ms`（`__exit__` 返回 false 且外层无 try，未捕获 `ValueError` 顶层退出）。

## 验收标准

- [ ] 编译器接通 `MS_AST_WITH`：管理器表达式只求值一次；`__exit__` 经 `GET_ATTR` 预绑定（缺失先于 `__enter__` 抛 `AttributeError`）；`__enter__` 经 `LOAD_METHOD`/`CALL_METHOD 0` 调用，结果绑定 `as` 目标（声明在 `with` 所在作用域、块后可见）或丢弃。
- [ ] 异常路径经任务 23 的 `MsTryBlock` 异常表实现（`with` 进入/退出无运行时登记指令）；handler 以 `MS_OP_EXC_INFO` 取得 `(excType, excValue, traceback)` 三参调用 `__exit__`；返回真值吞异常、假值 `RERAISE` 原异常（traceback 不重置）。
- [ ] 新增 `MS_OP_EXC_INFO` 指令（A 格式：读帧槽异常对象，压入类对象/实例/traceback），traceback 物化复用任务 23 约定（list of dict）；指令总数仍在 08-vm-internals §2.2 的 80 条预算内。
- [ ] `__exit__` 自身抛异常时新异常取代原异常传播，且经 handler 覆盖区守卫条目自动挂隐式 `__context__`；正常/控制流路径 `__exit__` 的返回值丢弃、其异常照常传播。
- [ ] `return`/`break`/`continue` 穿越 `with` 体时先以 `(nil, nil, nil)` 调用 `__exit__`（复用任务 23 的 epilogue 副本机制，与 finally 层混排序次正确）；`__exit__` 抛异常覆盖原控制流。
- [ ] 嵌套 `with` 进入外→内、退出 LIFO；内层吞/不吞异常时外层语义正确；每层独立隐藏槽；`with` 展开段纳入 `MsProto.stackSize` 统计。
- [ ] 缺协议方法抛 `AttributeError`、不可调用/参数不符抛 `TypeError`，均沿任务 15/13 的既有错误路径，`with` 不新增编译期错误码。
- [ ] `tests/ms/with/` 下「测试方案」全部清单项实现并全数通过，`python run_tests.py` 退出码为 0；负例脚本以预期退出码 1 失败。
- [ ] 代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）；`msCloseState` 后分配统计归零；Debug 构建（ASAN / `/RTC`）无报告；构建产物只落在 `build/`；Win/Linux/macOS 三平台 Debug/Release 构建通过。
- [ ] 无 TBD/TODO 占位；对任务 07/13/15/25 的假定接口名（`msCompilerDeclareHiddenLocal`、epilogue 栈、`msExceptionMaterializeTraceback`、魔术方法查找约定等）在实现时已对齐对应任务文档的定名；本文「设计依据」记录的三处规范歧义处理（无逗号并列形式、`as` 作用域取外层、协议查找继承任务 25 口径）在实现中未被擅自改变。
