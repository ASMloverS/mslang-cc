# 23 异常系统

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [09 最小可运行解释器](09-minimal-interpreter.md)、[12 控制流语句](12-control-flow.md) |

## 任务目标

交付 mslang 的完整异常系统，覆盖 `docs/language/04-exceptions.md` 的全部语义：

- **零成本 try**：`try` 块进入/退出不产生任何运行时指令开销，异常处理走 `MsProto.tryBlocks` 异常表查表法，代价集中在实际抛出时刻。
- **`raise` / `raise ... from`**：支持抛出实例、裸类名（自动实例化）、重抛已有实例（保留原 traceback）、`from` 显式链化（`__cause__`）；`except` 块内无 `from` 抛新异常时自动挂 `__context__`（隐式链化）。
- **`except` 分派**：子句按序匹配、先命中先执行；支持单类型、多类型（括号组）捕获与 `as` 绑定；裸 `except` 等价于 `except Exception`，不捕获 `SystemExit`/`KeyboardInterrupt`。
- **内建异常层级**：按规范 §4 预建 28 个内建异常类，层级关系在解释器初始化时固化。
- **`finally` 语义**：正常结束、`return`/`break`/`continue`、异常传播四条路径都保证执行；`finally` 自身的 `return`/异常覆盖原控制流。
- **traceback**：抛出时捕获调用栈快照（文件、行号、函数名），顶层未捕获异常打印 `TypeName: message` + traceback 到 stderr 并以非零码退出。

交付物为新增模块 `src/object/ms_exception.{c,h}`，以及对编译器（任务 07）、VM 执行核心（任务 08）的扩展：try 语句代码生成、异常表发射、`MS_OP_RAISE`/`MS_OP_RERAISE`/异常匹配指令的分派与栈展开。完成后，脚本可完整使用 `try/except/finally/raise`，运行时错误（除零、类型错误等）统一以脚本异常形式报告。本任务只交付设计文档；实现与测试代码随实现任务编写。

## 设计依据

- `docs/language/04-exceptions.md`（全文）
  - §1 基本语法：`except` 按序匹配、类型匹配含子类、多类型捕获、裸 `except` 等价于 `except Exception`、`except` 与 `finally` 至少出现其一（裸 `try` 无 `except`/`finally` 是编译错误）、`try/finally`（无 except）合法、无匹配时继续传播且 `finally` 照常执行。
  - §2 抛出与链化：`raise` 的四种形式；`raise` 对象必须是 `BaseException` 实例或子类，否则抛 `TypeError`；`except` 块内隐式链化 `__context__`。
  - §3 异常对象：`message`/`__cause__`/`__context__`/`traceback` 四属性；`str(e)` 为 `"TypeName: message"`（无消息时为 `"TypeName"`）。
  - §4 内建异常层级：28 个类的完整层级树；C 扩展可注册自定义异常（09-c-api，任务 33 落地）。
  - §5 与控制流的交互：`finally` 先行与覆盖规则；`StopIteration` 被 `for` 自动消化（for-in 迭代协议为任务 26，且 08-vm-internals §2.2 的 `MS_OP_ITER_NEXT` 以跳转消化迭代耗尽、不经异常，本任务只建 `StopIteration` 类型）；协程未捕获异常存句柄、`await` 重抛（属任务 43）；顶层未捕获打印并退出非零码。
  - §6 性能约定：零成本 try，异常表查表法，抛出时捕获当前协程调用栈快照。
- `docs/language/08-vm-internals.md`
  - §2.1：`MsProto` 布局含 `MsTryBlock* tryBlocks` 异常表，元素布局 `{pcStart, pcEnd, handlerPc, finallyPc}`（本任务在此基础上扩展字段，见「详细设计」）。
  - §2.2：异常类指令 `MS_OP_SETUP_TRY` `MS_OP_POP_TRY` `MS_OP_RAISE` `MS_OP_RERAISE`；指令表为代表性列举，目标指令总数 ≤ 80。
  - §3 对象模型：`MsObjectHeader.type` 指向 `MsType*`；属性访问为 dict 查找 + 类 MRO 线性查找。
  - §4：帧 `MsCallFrame`（`proto`、返回地址、求值栈基址、upvalue 数组）。
- `docs/language/10-c-style.md`：命名、格式化、内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）、内部结构体不 typedef、include guard 规则。
- `docs/language/11-project-layout.md`：`src/object/`（对象模型与各类型实现）、`src/vm/`、`src/compiler/` 的目录划分；`tests/ms/` 脚本测试约定。
- 任务 05（`MsProto`/`MsTryBlock` 定义与常量池）、任务 06（`MsType`、`MsObject` 分配）、任务 07（编译器语句/表达式代码生成接口）、任务 08（VM 分派循环、`MsCallFrame`、求值栈操作）、任务 10（内建函数注册机制、`assert`）、任务 14（帧弹出时的 upvalue 关闭）、任务 15（class 对象的调用语义）均先于本任务完成，本文引用其接口时使用假定命名；任务 09、12 及其余被引用任务的文档若定名不同，实现时以对应任务文档定名为准。

## 详细设计

### 模块划分与文件

- 新增 `src/object/ms_exception.h` / `src/object/ms_exception.c`：异常对象模型——内建异常类层级的构造与注册、异常实例分配、类型匹配（沿基类链）、traceback 数据结构、未捕获异常格式化输出。头文件自包含，include guard `MSLANG_SRC_OBJECT_MS_EXCEPTION_H_`。
- 扩展编译器（任务 07，`src/compiler/`）：`try/except/finally`、`raise` 语句的代码生成与异常表条目发射。
- 扩展 VM（任务 08，`src/vm/`）：新增指令分派、`msVmRaise` 抛出入口、栈展开算法、顶层未捕获出口。
- 内建函数与运行时各错误点（除零、类型错误、断言失败等）统一改经 `msVmRaiseFmt` 抛脚本异常；本任务只接入 v0.1 已存在的错误点（除零、`assert`、类型/名称错误），后续任务各自接入。

### 内建异常层级与类型对象

规范 §4 的 28 个类在 `MsState` 初始化时由 `msExceptionInitTypes` 一次性创建为 `MsType` 对象并注册进全局名表（与任务 10 内建函数同机制，可被用户遮蔽）。层级不走脚本 `class X(Y)` 语法（class 继承是任务 25，晚于本任务），而是直接设置 `MsType.base` 指针形成基类链；任务 25/33 落地时复用同一基类链机制，脚本自定义异常与 C 扩展异常自然接入。

```c
// 内建异常类的稳定标识，供运行时代码快速取类型对象（如抛 TypeError）。
typedef enum {
  MS_EXC_BASE_EXCEPTION,
  MS_EXC_SYSTEM_EXIT,
  MS_EXC_KEYBOARD_INTERRUPT,
  MS_EXC_EXCEPTION,
  MS_EXC_ARITHMETIC_ERROR,
  MS_EXC_ZERO_DIVISION_ERROR,
  MS_EXC_OVERFLOW_ERROR,
  MS_EXC_ASSERTION_ERROR,
  MS_EXC_ATTRIBUTE_ERROR,
  MS_EXC_CHANNEL_CLOSED_ERROR,
  MS_EXC_EOF_ERROR,
  MS_EXC_IMPORT_ERROR,
  MS_EXC_LOOKUP_ERROR,
  MS_EXC_INDEX_ERROR,
  MS_EXC_KEY_ERROR,
  MS_EXC_NAME_ERROR,
  MS_EXC_OS_ERROR,
  MS_EXC_FILE_NOT_FOUND_ERROR,
  MS_EXC_PERMISSION_ERROR,
  MS_EXC_TIMEOUT_ERROR,
  MS_EXC_RUNTIME_ERROR,
  MS_EXC_RECURSION_ERROR,
  MS_EXC_NOT_IMPLEMENTED_ERROR,
  MS_EXC_CANCELLED_ERROR,
  MS_EXC_STOP_ITERATION,
  MS_EXC_TYPE_ERROR,
  MS_EXC_VALUE_ERROR,
  MS_EXC_UNICODE_ERROR,
  MS_EXC_COUNT
} MsExceptionId;
```

层级以文件级 `static const` 表描述（`{id, name, parentId}` 三元组，28 项），初始化按拓扑序建类并写 `base`；`ChannelClosedError`、`CancelledError` 等 v0.3 才产生实际抛出点的类本任务只建类型。`KeyboardInterrupt` 同样只建类型与匹配语义，Ctrl+C 注入依赖平台信号抽象（任务 42），不在本任务范围。

### 异常实例

异常实例使用专用 C 结构（类型标签复用 `MS_TYPE_INSTANCE`，`header.type` 指向异常 `MsType`），固定字段直接内联以保证 C 侧 O(1) 访问：

```c
struct MsTracebackEntry {
  MsString* funcName;     // 函数名；模块顶层为 "<module>"
  MsString* fileName;     // 源文件名
  int line;               // 1 起始
};

struct MsTraceback {
  struct MsTracebackEntry* entries;  // 动态数组，栈顶（最内层帧）在前
  int count;
  int capacity;
};

struct MsException {
  MsObjectHeader header;            // header.type 指向异常 MsType
  MsObject* message;                // MsString 或 nil
  MsObject* cause;                  // __cause__，nil 表示无
  MsObject* context;                // __context__，nil 表示无
  struct MsTraceback* traceback;    // 首次抛出时捕获；NULL 表示尚未捕获
};
```

- `message`、`__cause__`、`__context__`、`traceback` 经异常类的属性表暴露为脚本可读写属性（与任务 15 的属性访问机制一致）；`traceback` 在脚本侧物化为 list，元素为 dict `{"file": str, "line": int, "function": str}`（tuple 属任务 32，故用 dict）。固定字段之外的属性落入常规实例 dict。
- 四个字段均为 `MsObject*`，GC 标记阶段（任务 17）必须遍历；`MsTraceback` 内的 `MsString*` 同属标记范围。
- 构造：调用异常类对象（如 `ValueError("bad input")`）走任务 15 的类调用路径，分配 `MsException` 并存入 `message`。v0.2 限定参数为 0 或 1 个，多于 1 个抛 `TypeError`（规范未定义多参数形式，从简）。`str(e)` 返回 `"TypeName: message"`，无消息时为 `"TypeName"`。

```c
// Creates the 28 builtin exception types and registers them in the global
// name table. Called once from state initialization, before any script runs.
MsResult msExceptionInitTypes(MsState* L);

// Returns the type object of a builtin exception (fast path for runtime
// error sites).
MsType* msExceptionTypeOf(MsState* L, MsExceptionId id);

// Allocates an exception instance of type with the given message (may be
// nil). Returns NULL on OOM.
MsObject* msExceptionNew(MsState* L, MsType* type, MsObject* message);

// Returns true when exc is an instance of type or of any type on its base
// chain. exc need not be an exception: returns false for other objects.
bool msExceptionIsA(MsState* L, MsObject* exc, MsType* type);

// Formats an uncaught exception ("TypeName: message" plus traceback chain,
// __cause__/__context__ chains included) and writes it to stderr.
void msExceptionPrintUncaught(MsState* L, MsObject* exc);
```

### 异常表 `MsTryBlock`（零成本 try 的核心）

在 08-vm-internals §2.1 的四字段草图基础上扩展为：

```c
#define MS_NO_SLOT 0xFFFF

struct MsTryBlock {
  uint32_t pcStart;      // 受保护代码区间 [pcStart, pcEnd)
  uint32_t pcEnd;
  uint32_t handlerPc;    // except 分派代码入口；0 表示无 except（纯 finally 守卫）
  uint32_t finallyPc;    // 异常路径 finally 副本入口；0 表示无 finally
  uint16_t excSlot;      // 进入 handler 时存放异常对象的帧内槽位；MS_NO_SLOT 表示本区间不是 handler 覆盖区
  uint16_t pendingSlot;  // 跳入异常路径 finally 前暂存待传播异常的帧内槽位
};
```

要点：

- `try` 的进入/退出**不发射任何运行时指令**。`MS_OP_SETUP_TRY`/`MS_OP_POP_TRY` 只作编译期锚点：编译器在它们划定的 pc 区间上生成 `MsTryBlock` 条目后将其从指令流中删除（08-vm-internals §2.2 的清单由此得到解释——这两条指令不占用运行时成本）。
- 每个受保护区间一条目；**编译器额外为 handler 覆盖区生成条目**：当 try 同时有 except 与 finally 时，除 `[tryStart, tryEnd)` 的主条目外，再为 `[handlerPc, handlerEndPc)` 生成一条 `handlerPc=0`、`finallyPc` 指向同一 finally 副本、`excSlot` 与主条目相同的守卫条目。这样 except 子句体内抛出的异常仍会触发本层 finally（经典 javac 式子例程覆盖），且该条目同时充当隐式链化的判据（见下）。
- 嵌套 try 的条目按"内层优先"排序（编译器发射顺序保证）：查表线性扫描取首个 `pcStart <= pc < pcEnd` 的条目即为最内层守卫。异常表通常只有个位数据条目，线性扫描足够，不做二分。
- 异常表随 `MsProto` 一起分配/释放（任务 05 的 Proto 所有权），运行时不修改。

### 指令集调整

在 08-vm-internals §2.2 异常类别下，运行时实际保留三条指令并新增一条（总数仍在 80 以内）：

| 指令 | 操作数 | 语义 |
|---|---|---|
| `MS_OP_SETUP_TRY` / `MS_OP_POP_TRY` | — | 仅编译期锚点，不进入最终指令流 |
| `MS_OP_RAISE` | A | 抛出槽 A 中的对象：异常实例直接使用（已有 traceback 则保留）；异常类对象则零参实例化；其余对象抛 `TypeError` |
| `MS_OP_RERAISE` | A | 重新抛出槽 A（当前帧 `pendingSlot`）中的待传播异常，不做隐式链化、不重捕 traceback。用于异常路径 finally 副本末尾与 except 无匹配兜底 |
| `MS_OP_EXCEPTION_MATCH` | A、Bx | 新增。取槽 A 中异常对象与常量池 Bx 处的异常类对象，调用 `msExceptionIsA`，结果 bool 压栈。多类型捕获由编译器展开为多条本指令 + 跳转链，不引元组常量 |

`raise E from c` 不新增指令，编译为等价序列：构造实例到临时槽 → `MS_OP_SET_ATTR` 写 `__cause__`（`c` 必须是 `BaseException` 实例，否则抛 `TypeError`）→ `MS_OP_RAISE`。

### except 分派代码生成

编译器把 `except` 子句序列编译为 handler 入口处的分派链（异常对象已在 `excSlot`）：

```
handlerPc:
  ; 子句 1：except ValueError as e
  MATCH excSlot, const(ValueError)   ; MS_OP_EXCEPTION_MATCH + JUMP_IF_FALSE 到下一条目
  绑定 e（STORE_LOCAL）→ 子句体 → 跳到 afterTry（途经 finally 内联副本，见下）
  ; 子句 2：except (TypeError, KeyError) as e
  MATCH excSlot, const(TypeError)    ; 命中跳子句体
  MATCH excSlot, const(KeyError)
  ...
  ; 裸 except：编译为 MATCH excSlot, const(Exception)，天然不含 SystemExit/KeyboardInterrupt
  ; 全部不命中：跳 finally 异常路径副本（有 finally）或 RERAISE pendingSlot
```

匹配是运行时的基类链上溯，故"子类异常被父类子句捕获""先命中先执行"自然成立；子句顺序即编译顺序。`as` 绑定的名字作用域为 except 子句块（与任务 12 的块作用域一致），块结束即失效，不做 Python 式删除语义。

### finally 代码生成

`finally` 体由编译器复制为多个副本，无任何运行时登记：

1. **正常路径副本**：try 体与每个 except 子句体的末尾（fall-through 出口）各内联一份 finally 体，随后进入 afterTry。
2. **控制流出口副本**：try 体内（含嵌套层穿越本 try 的）每个 `return`/`break`/`continue`，先内联一份 finally 体再执行原控制流指令。穿越多层嵌套 try 时按由内向外顺序逐层内联。
3. **异常路径副本**：`finallyPc` 指向的独立副本，进入前 VM 已把待传播异常存入 `pendingSlot`；副本末尾发射 `MS_OP_RERAISE pendingSlot`，正常落到底即继续传播。

覆盖规则由此自然成立：finally 体内执行 `return` 时，异常路径副本中的 `return` 直接返回（丢弃 `pendingSlot`），正常/控制流副本同理覆盖原控制流；finally 体内抛出新异常则经异常表重新展开，原待传播异常被丢弃。复制带来的代码体积增长按 finally 体通常极小接受；同一 finally 的副本数 = 1（异常路径）+ fall-through 出口数 + 穿越控制流语句数。

### 抛出与栈展开算法

VM 侧统一抛出入口（指令分派与 C 侧运行时错误共用）：

```c
// Begins exception propagation for exc. Validates that exc is a
// BaseException instance or class (a class is instantiated; anything else
// raises TypeError instead). Performs implicit __context__ chaining and
// traceback capture, then unwinds to the nearest handler.
// Returns MS_OK if a handler was found and the dispatch loop should resume;
// returns MS_ERROR_RUNTIME when the exception propagates past the top frame
// (uncaught), with exc stored in L->uncaughtException for the caller (CLI /
// C API boundary) to report.
MsResult msVmRaise(MsState* L, MsObject* exc);

// Convenience wrapper for runtime error sites: builds the exception of the
// given builtin id with a formatted message, then calls msVmRaise.
MsResult msVmRaiseFmt(MsState* L, MsExceptionId id, const char* fmt, ...);
```

`msVmRaise` 流程：

1. **校验与实例化**：类对象零参实例化；非 `BaseException` 实例则改抛 `TypeError`（原对象丢弃）。
2. **隐式链化**：若未设置 `__cause__`（无 `from`），在当前帧异常表中查"pc 落在某条 `excSlot != MS_NO_SLOT` 的守卫条目区间内"的最内层条目；命中即把该帧 `excSlot` 中的处理中异常挂到新异常的 `__context__`。`from` 已设 `__cause__` 时跳过本步（v0.2 不实现 `__suppress_context__`，`__context__` 与 `__cause__` 可同时存在，打印时优先展示 cause 链——与 CPython 输出细节的差异在此显式承认）。
3. **traceback 捕获**：`exc->traceback == NULL` 时，沿当前调用栈自顶向下逐帧记录 `{funcName, fileName, line}`（行号经 `MsProto.lines` 由返回地址 pc 换算）。重抛已有 traceback 的实例不重复捕获（规范 §2："re-raise keeps the original traceback"）。
4. **展开**：自顶向下遍历调用帧，在当前帧的 `proto->tryBlocks` 中找最内层命中条目：
   - 条目 `handlerPc != 0`：异常对象写入帧的 `excSlot`，pc 置 `handlerPc`，返回 `MS_OK` 恢复分派。
   - 否则条目 `finallyPc != 0`：异常对象写入 `pendingSlot`，pc 置 `finallyPc`，恢复分派（副本末尾 `MS_OP_RERAISE` 以该异常重新进入本流程的第 4 步，跳过 1–3）。
   - 无命中条目：关闭该帧未决 upvalue（任务 14 接口），弹帧，继续向调用者帧展开。
5. **展开至栈空**：异常存入 `L->uncaughtException`，返回 `MS_ERROR_RUNTIME` 给顶层驱动。

### 顶层未捕获与 SystemExit

CLI / 嵌入边界（任务 09、22 的求值入口）收到 `MS_ERROR_RUNTIME` 后：

- `SystemExit` 实例：`message` 为 int 时作为退出码，为字符串时打印到 stderr 并以 1 退出，nil 时以 0 退出；**不打印 traceback**（规范 §4：`exit()` 触发解释器退出）。
- 其余异常：`msExceptionPrintUncaught` 打印 `TypeName: message` + traceback（含 `__cause__`/`__context__` 链，`cause` 以 "The above exception was the direct cause of the following exception" 风格前缀连接，`context` 相应标注 "During handling of the above exception"），进程以非零码（1）退出。
- C API 边界（任务 18）：未捕获异常经任务 18 的错误通道传给嵌入方，具体 `MsResult` 取值以任务 18 文档为准。
- 协程边界的存留与 `await` 重抛（规范 §5）属任务 43，本任务在 `msVmRaise` 的栈空出口预留挂接点（协程句柄字段由任务 43 定义）。

### GC 与根纪律

- 展开全程异常对象只经求值栈槽、帧槽（`excSlot`/`pendingSlot`）与 `L->uncaughtException` 持有，均为 GC 根（任务 17 的根集合含调用栈/求值栈；`uncaughtException` 字段需加入根扫描，属本任务对任务 17 的扩展点）。
- `msVmRaiseFmt` 等 C 侧构造函数在分配之间经任务 18 的根栈（`msRootPush/msRootPop`）保护半成品对象。

## 实现步骤

1. 建 `src/object/ms_exception.h` / `ms_exception.c` 骨架：`MsExceptionId` 全量枚举（28 项）、层级描述表、`msExceptionInitTypes`（建类、写 `base`、注册全局名）、`msExceptionTypeOf`。验证：脚本断言全部类名可见、`except Exception` 能接住手动构造的层级关系（先用 `msExceptionIsA` 的间接表现验证）。
2. 实现 `MsException` 分配与 `msExceptionNew`、异常类调用路径（0/1 参数校验）、`str(e)` 格式化、四属性的脚本侧暴露。验证：`e := ValueError("x")`、`print(e)` 输出 `ValueError: x`、`e.message` 读写。
3. 实现 `msExceptionIsA` 基类链匹配。验证：`except Exception` 捕获 `ValueError` 实例；`except ValueError` 不捕获 `TypeError`。
4. 编译器：try 语句解析接入（语法已在任务 12 预留或本任务补充其 AST 节点代码生成）、裸 `try`（无 `except` 且无 `finally`）报编译错误、`SETUP_TRY/POP_TRY` 锚点折叠为 `MsTryBlock` 条目、嵌套条目内层优先排序。验证：纯 `try/finally` 脚本正常执行且 finally 落地；裸 `try` 源码编译失败且诊断位置正确；反汇编（debug 构建的 Proto dump）确认无运行时 try 指令残留。
5. 编译器：except 分派链（单类型/多类型/裸 except/`as` 绑定）、`MS_OP_EXCEPTION_MATCH` 发射；VM 分派该指令。验证：按序匹配、子类命中、多类型命中、裸 except 排除 `SystemExit`。
6. VM：`msVmRaise`/`msVmRaiseFmt`、`MS_OP_RAISE`/`MS_OP_RERAISE` 分派、展开算法（查表、跳 handler/finally、upvalue 关闭、弹帧）、`L->uncaughtException` 出口与 GC 根扩展。验证：`raise` 四种形式；跨多层函数的展开；无匹配 except 时 finally 执行后继续上抛。
7. 编译器：finally 三类副本（fall-through、控制流出口、异常路径）与嵌套 try 的逐层内联。验证：`return`/`break`/`continue` 各路径 finally 先行；finally 内 `return`/raise 覆盖原控制流。
8. 链化与 traceback：`raise from`（`__cause__`）、隐式 `__context__`（守卫条目判据）、traceback 捕获与脚本侧物化、`msExceptionPrintUncaught` 链式输出。验证：链化属性断言；traceback 的帧序列与行号断言；顶层未捕获的 stderr 输出与退出码（人工/CTest 验证，见测试方案）。
9. 运行时错误点接入：v0.1 已有错误（除零 → `ZeroDivisionError`、类型错误 → `TypeError`、名称未定义 → `NameError`、`assert` 失败 → `AssertionError`、递归深度超限 → `RecursionError`）统一改经 `msVmRaiseFmt`。验证：每类错误可被对应 except 捕获。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试（`tests/ms/exception/`），驱动为仓库根 `run_tests.py` 调用 mslang CLI。任务 40（testing 模块）之前，断言用内建 `assert`（条件、消息）+ 结尾 `print("ok: <用例名>")`。测试代码随实现任务编写，本任务只交付方案与清单。

- `tests/ms/exception/try_except_basic.ms`：try/except 基本捕获；`except ValueError as e` 绑定与 `e.message`；`str(e)` 的 `"TypeName: message"` 与无消息形式；`try/finally`（无 except）合法且 finally 执行；无异常时 except 不执行。
- `tests/ms/exception/except_matching.ms`：子句按序先命中（先 `ValueError` 后 `Exception` 与反序的行为差）；子类被父类子句捕获（`except Exception` 接 `KeyError`）；多类型 `except (TypeError, KeyError)` 两类均命中；裸 `except` 捕获普通异常、**不捕获** `SystemExit`（脚本内 `raise SystemExit` 后裸 except 不应命中，由外层 `except SystemExit` 接住）与 `KeyboardInterrupt`（同理构造）。
- `tests/ms/exception/raise_forms.ms`：`raise ValueError("m")`；`raise ValueError`（裸类自动实例化）；`raise e` 重抛保留原 traceback（断言 `traceback` 帧序列不变）；`raise 42` / `raise "str"` 抛 `TypeError`；`raise X from "str"` 抛 `TypeError`。
- `tests/ms/exception/chaining.ms`：`raise A from b` 置 `__cause__`；except 块内无 `from` 抛新异常自动挂 `__context__`；显式 `from` 时 `__cause__` 优先；无 `from` 时 `__cause__` 为 nil。
- `tests/ms/exception/finally.ms`：正常结束、`except` 命中后、except 无匹配传播、try 内 `return`/`break`/`continue`（循环内 try）、嵌套 try（内层异常经两层 finally 依序执行）六条路径的 finally 执行顺序断言（用 list 追加记录执行轨迹）；finally 内 `return` 覆盖 try 内 `return`；finally 内 raise 覆盖原异常。
- `tests/ms/exception/propagation.ms`：跨三层函数调用展开并在顶层 try 捕获；展开途中各层 finally 按序执行；无匹配 except 时 finally 执行后继续上抛。
- `tests/ms/exception/hierarchy.ms`：28 个内建类名全部可见；抽样断言层级关系（`except ArithmeticError` 接 `ZeroDivisionError`、`except LookupError` 接 `IndexError`/`KeyError`、`except OSError` 接 `FileNotFoundError`、`except ValueError` 接 `UnicodeError`、`except RuntimeError` 接 `RecursionError`）。
- `tests/ms/exception/runtime_errors.ms`：`1/0` → `ZeroDivisionError`、`1 + "x"` → `TypeError`、未定义变量 → `NameError`、`assert false` → `AssertionError`、深递归 → `RecursionError`，均可被对应子句捕获且消息非空。
- `tests/ms/exception/traceback.ms`：捕获异常的 `traceback` 属性为 list、元素含 `file`/`line`/`function` 键；帧序列自内向外、函数名与行号正确。
- `tests/ms/exception/uncaught_top.ms`：顶层未捕获异常。该用例预期进程以非零码退出且 stderr 含 `ValueError: boom` 与 traceback 行，属驱动级用例：经 CTest 的 `WILL_FAIL`/stderr 匹配挂接（11-project-layout §4 的 ctest 驱动）；`run_tests.py` 若尚无预期失败约定，本任务不为其新增约定，改在任务验收时人工运行该脚本核对输出。

## 验收标准

- [ ] `src/object/ms_exception.h` / `ms_exception.c` 存在，guard 为 `MSLANG_SRC_OBJECT_MS_EXCEPTION_H_`，头文件自包含；全部代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsException` 不 typedef、堆分配只经 `msAlloc/msRealloc/msFree`）。
- [ ] 28 个内建异常类按规范 §4 层级预建并注册为全局名，`base` 链正确；`KeyboardInterrupt` 注入、协程存留重抛按本任务声明的范围留给任务 42/43，无越界实现。
- [ ] 零成本 try：`try` 进入/退出无运行时指令，`MS_OP_SETUP_TRY`/`MS_OP_POP_TRY` 仅作编译期锚点；`MsTryBlock` 含扩展字段且随 `MsProto` 分配释放；嵌套条目内层优先。
- [ ] `raise` 四种形式、`raise from`（`__cause__`）、隐式 `__context__`、非 `BaseException` 抛 `TypeError` 均符合规范 §2；重抛保留原 traceback。
- [ ] `except` 按序匹配、子类命中、多类型捕获、`as` 绑定、裸 `except` 排除 `SystemExit`/`KeyboardInterrupt`，符合规范 §1。
- [ ] `finally` 在正常、控制流出口（`return`/`break`/`continue`）、异常传播路径全部先行执行；finally 内 `return`/raise 覆盖原控制流；嵌套 try 逐层执行，符合规范 §5。
- [ ] 抛出时捕获调用栈快照（文件/行号/函数名）；`traceback` 属性脚本侧为 list of dict；顶层未捕获打印 `TypeName: message` + 链式 traceback 到 stderr、退出码非零；`SystemExit` 不打印 traceback 并按消息决定退出码。
- [ ] v0.1 已有运行时错误（除零、类型、名称、`assert`、递归深度）统一改抛对应脚本异常，可被 except 捕获。
- [ ] `tests/ms/exception/` 下「测试方案」全部清单用例通过；`tests/ms/exception/uncaught_top.ms` 经 CTest 或人工验证退出码与 stderr；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 05/06/07/08/10/14/15/18 的假定接口名在实现时已对齐；与 CPython 的两处差异（无 `__suppress_context__`、重抛不追加新 raise 点）已在本文显式记录。
