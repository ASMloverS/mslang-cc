# 05 字节码格式与 MsProto

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [02 核心基础设施](02-core-infrastructure.md) |

## 任务目标

交付 mslang 栈式字节码的格式定义与 `MsProto`（函数原型）模块：

- `src/vm/ms_opcode.{h,c}`：全部操作码枚举（含 v0.2/v0.3 预留）、两种定长 4 字节指令格式（`opcode(8)|A(8)|Bx(16)` 与 `opcode(8)|sAx(24, 有符号)`）的编码/解码内联函数、操作码元数据表（名称与格式）。
- `src/vm/ms_proto.{h,c}`：`struct MsProto` 完整定义（指令流、常量池、异常表、行号表、参数/局部变量/栈深元数据、名称与来源文件、三个标志位）、面向编译器的 `struct MsProtoBuilder` 构建器（发射指令、追加常量/异常表项、记录行号、回填跳转、定稿收缩）。

本模块是编译器（任务 07）的产物格式与 VM（任务 08）的输入格式：完成后，任务 07 可以只经构建器 API 生成字节码，任务 08 可以只读 `MsProto` 字段执行。本任务自身经 `tests/c/test_opcode.c` 与 `tests/c/test_proto.c` 的 C 单元测试独立验证，不依赖 lexer/parser/对象模型的任何实现。

## 设计依据

- `docs/language/08-vm-internals.md`
  - §1：每个函数（含模块顶层）编译为一个 `MsProto`；嵌套函数是外层 Proto 常量池中的子 Proto。
  - §2.1：栈式 VM、定长 4 字节指令、两种操作数格式、`MsProto` 布局、异常表元素 `{pcStart, pcEnd, handlerPc, finallyPc}`。
  - §2.2：指令集分类代表清单、`MS_OP_` 前缀命名、目标指令数控制在 80 条以内、首版 switch 分派。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化、§3 命名（枚举值 `MS_` 大写蛇形、全局唯一）、§4 typedef 规则（内部结构体不 typedef、枚举允许 typedef）、§5 错误处理（`MsResult` + 输出参数置尾）、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）、§8 断言（内部不变量用 `MS_ASSERT`）。
- `docs/language/11-project-layout.md`：§1 `src/vm/` 目录位置；§4 `tests/c/` 用自研 `ms_test.h` 做模块单元测试（框架由任务 01 交付）。
- 任务 02 核心基础设施提供：`msAlloc/msRealloc/msFree`、`MsResult`（`MS_OK`/`MS_ERROR_OOM`/`MS_ERROR_SYNTAX`）、`MS_ASSERT`、分配统计接口（用于泄漏断言）。本文假定其接口名；若任务 02 文档定名不同，以实现时对齐为准。
- 对象类型 `struct MsObject` / `struct MsString` 由任务 06（对象模型基础）定义，本文按 `docs/language/08-vm-internals.md` §2.1/§3 假定其命名，头文件中只做前向声明（常量池与名称字段均为指针，不完整类型即可编译）；实现时以任务 06 文档定名为准。

对规范的两处显式处理（实现与评审时以此为据）：

1. **本任务的 `MsProto` 不含 §2.1 中的 `MsObjectHeader header` 字段。** GC 对象化属任务 06/17 职责，且按 `docs/tasks/README.md` 的编号依赖规则，任务 05 不得依赖任务 06。本任务中 Proto 是纯数据制品，经 `msAlloc` 分配、`msProtoFree` 释放，所有权在编译器与 `MsState` 之间显式移交；任务 06 落地时在结构体头部插入 `header` 字段并把创建路径切换为 GC 分配，本任务全部 API 不暴露内部布局，迁移不影响调用方（见「详细设计·与对象模型的衔接」）。
2. **规范未给出 `tryBlocks` / `lines` 的长度字段。** 仅有指针无法界定数组，本任务补充 `tryBlocksLen` / `linesLen` 两个 `int` 字段，实现时与任务 08 文档对齐。

另注：§2.2 的代表性清单按类别展开为 82 条（算术类的 `BITAND/OR/XOR/SHL/SHR/INVERT` 展开为 6 条），略超「80 条以内」的目标。本任务以枚举为唯一事实源：v0.1 子集 63 条为硬约束；全集以 ≤80 为 v1.0 冻结前的软目标，若超限优先合并 `MS_OP_SELECT_ADD_RECV/ADD_SEND` 或下封 `MS_OP_CMP_CHAIN`（见「v0.1 指令子集」）。

## 详细设计

### 文件与模块边界

- `src/vm/ms_opcode.h`（guard `MSLANG_SRC_VM_MS_OPCODE_H_`）：操作码枚举、格式枚举、编码/解码 `static inline` 函数；自包含，只 include `<stdint.h>` 与任务 02 的 `"core/ms_result.h"`（假定名）。
- `src/vm/ms_opcode.c`：操作码元数据表（`static const` 数组 + 查询函数）。
- `src/vm/ms_proto.h`（guard `MSLANG_SRC_VM_MS_PROTO_H_`）：`struct MsTryBlock`、`struct MsLineEntry`、`struct MsProto`、`struct MsProtoBuilder` 与全部构建器/查询函数声明；前向声明 `struct MsObject` / `struct MsString`；自包含。
- `src/vm/ms_proto.c`：构建器与查询函数实现；数组增长、定稿收缩等辅助函数一律文件内 `static`。
- 模块的所有堆分配（四条数组 + Proto 本体）经 `msAlloc/msRealloc/msFree`，所有者明确：构建器持有未定稿数组，`msProtoBuild` 成功后移交调用者，调用者以 `msProtoFree` 释放。
- 字节码序列化（落盘 `.msc` 之类）不在本任务范围，操作码数值因此是内部表示，v1.0 冻结前允许调整顺序。

### 指令编码（ms_opcode.h）

指令为 `uint32_t`，按位域划分（与主机字节序无关，纯位移实现）：

```
ABC 格式:  bits [0:8)   opcode
           bits [8:16)  A     (无符号 8 位)
           bits [16:32) Bx    (无符号 16 位)
sAx 格式:  bits [0:8)   opcode
           bits [8:32)  sAx   (有符号 24 位，二进制补码)
```

- `Bx` 取值 0..65535：常量池下标、容器元素数等。常量池因此单 Proto 上限 65536 项，溢出按编译错误处理（见构建器）。
- `sAx` 取值 -8388608..8388607：跳转偏移。约定为**相对跳转指令自身的下一条指令**的有符号偏移：`target = pc + 1 + sAx`。编译器先以占位 0 发射、确定目标后回填（`msProtoPatchSAx`）。

```c
typedef enum {
  MS_OPFMT_ABC,    // opcode | A | Bx
  MS_OPFMT_SAX     // opcode | sAx
} MsOpFormat;

static inline uint32_t msOpEncodeABC(MsOpCode op, uint8_t a, uint16_t bx);
static inline uint32_t msOpEncodeSAx(MsOpCode op, int32_t sax);   // sax out of range is MS_ASSERT'd
static inline MsOpCode msOpDecodeOp(uint32_t instr);
static inline uint8_t  msOpDecodeA(uint32_t instr);
static inline uint16_t msOpDecodeBx(uint32_t instr);
static inline int32_t  msOpDecodeSAx(uint32_t instr);             // sign-extended from 24 bits

// Returns the declared format of op. Table-driven; total order with the enum
// is verified by unit tests.
MsOpFormat msOpFormatOf(MsOpCode op);

// Static name table for diagnostics, dumps and tests ("MS_OP_ADD" etc.).
const char* msOpName(MsOpCode op);
```

### 操作码枚举

全集按 §2.2 的类别顺序排列，`MS_OP_COUNT` 哨兵收尾；注释标注引入阶段，v0.1 之外的条目本任务只占位（枚举与元数据完整，编译器与 VM 后续任务逐阶段启用）：

```c
typedef enum {
  // Constants and moves (v0.1)
  MS_OP_LOAD_CONST, MS_OP_LOAD_NIL, MS_OP_LOAD_TRUE, MS_OP_LOAD_FALSE, MS_OP_MOVE,
  // Locals and upvalues (v0.1)
  MS_OP_LOAD_LOCAL, MS_OP_STORE_LOCAL, MS_OP_LOAD_UPVAL, MS_OP_STORE_UPVAL, MS_OP_CLOSE_UPVALS,
  // Globals and modules (LOAD/STORE_GLOBAL: v0.1; IMPORT*: v0.2)
  MS_OP_LOAD_GLOBAL, MS_OP_STORE_GLOBAL, MS_OP_IMPORT, MS_OP_IMPORT_FROM,
  // Arithmetic (v0.1)
  MS_OP_ADD, MS_OP_SUB, MS_OP_MUL, MS_OP_DIV, MS_OP_FLOORDIV, MS_OP_MOD, MS_OP_POW,
  MS_OP_NEG, MS_OP_NOT,
  MS_OP_BITAND, MS_OP_BITOR, MS_OP_BITXOR, MS_OP_SHL, MS_OP_SHR, MS_OP_INVERT,
  // Comparison (v0.1)
  MS_OP_EQ, MS_OP_NE, MS_OP_LT, MS_OP_LE, MS_OP_GT, MS_OP_GE, MS_OP_IS, MS_OP_IN, MS_OP_CMP_CHAIN,
  // Jumps (v0.1)
  MS_OP_JUMP, MS_OP_JUMP_IF_FALSE, MS_OP_JUMP_IF_TRUE, MS_OP_JUMP_IF_NIL,
  // Containers (LIST/DICT/INDEX/SET_INDEX/DEL_INDEX/APPEND: v0.1; TUPLE/SET/SLICE: v0.2)
  MS_OP_BUILD_LIST, MS_OP_BUILD_TUPLE, MS_OP_BUILD_DICT, MS_OP_BUILD_SET,
  MS_OP_INDEX, MS_OP_SET_INDEX, MS_OP_DEL_INDEX, MS_OP_SLICE, MS_OP_APPEND,
  // Calls (v0.1)
  MS_OP_CALL, MS_OP_CALL_KW, MS_OP_TAIL_CALL, MS_OP_RETURN, MS_OP_LOAD_METHOD, MS_OP_CALL_METHOD,
  // Functions and classes (v0.1)
  MS_OP_MAKE_FUNCTION, MS_OP_MAKE_CLASS, MS_OP_MAKE_LAMBDA,
  // Attributes (v0.1)
  MS_OP_GET_ATTR, MS_OP_SET_ATTR, MS_OP_DEL_ATTR,
  // Iteration (v0.1)
  MS_OP_GET_ITER, MS_OP_ITER_NEXT, MS_OP_UNPACK,
  // Exceptions (v0.2)
  MS_OP_SETUP_TRY, MS_OP_POP_TRY, MS_OP_RAISE, MS_OP_RERAISE,
  // Concurrency (v0.3)
  MS_OP_SPAWN, MS_OP_AWAIT,
  MS_OP_CHAN_NEW, MS_OP_CHAN_SEND, MS_OP_CHAN_RECV, MS_OP_CHAN_TRY_RECV,
  MS_OP_SELECT_BEGIN, MS_OP_SELECT_ADD_RECV, MS_OP_SELECT_ADD_SEND, MS_OP_SELECT_EXEC,
  // Miscellaneous (v0.1)
  MS_OP_PRINT_EXPR, MS_OP_NOP,
  MS_OP_COUNT
} MsOpCode;
```

元数据表 `ms_opcode.c` 内 `static const struct { const char* name; MsOpFormat format; } opInfoTable[MS_OP_COUNT]`，与枚举同序一一对应；`msOpFormatOf` / `msOpName` 查表，越界入参属编程错误（`MS_ASSERT`）。`_Static_assert(MS_OP_COUNT <= 256, ...)` 守住 8 位 opcode 上限。

### 操作数约定与 v0.1 指令子集

栈式语义约定（操作数粒度的最终语义以任务 08 文档为对齐基准）：`LOAD_*` 系列把值压入求值栈，`STORE_*` 系列弹栈写入目标；二元算术/比较弹二压一，一元弹一压一；局部变量位于帧的「寄存器窗口」，`LOAD_LOCAL A` 压入槽 `A` 的值，`STORE_LOCAL A` 弹栈写入槽 `A`，`MOVE A Bx` 做窗口内槽 `Bx` → 槽 `A` 的拷贝（不触碰求值栈）。

v0.1 子集共 63 条（枚举中标注 v0.1 的全部条目），格式与操作数：

| 类别 | 指令 | 格式 | 操作数 |
|---|---|---|---|
| 常量/移动 | `LOAD_CONST` | ABC | `Bx` = 常量池下标 |
| | `LOAD_NIL` `LOAD_TRUE` `LOAD_FALSE` | ABC | 无 |
| | `MOVE` | ABC | `A` = 目标槽，`Bx` = 源槽 |
| 局部/闭包 | `LOAD_LOCAL` `STORE_LOCAL` `LOAD_UPVAL` `STORE_UPVAL` `CLOSE_UPVALS` | ABC | `A` = 槽位 / upvalue 下标（`CLOSE_UPVALS`：关闭槽 ≥ `A` 的 upvalue） |
| 全局 | `LOAD_GLOBAL` `STORE_GLOBAL` | ABC | `Bx` = 名字符串的常量池下标 |
| 算术 | `ADD` `SUB` `MUL` `DIV` `FLOORDIV` `MOD` `POW` `BITAND` `BITOR` `BITXOR` `SHL` `SHR` | ABC | 无（弹二压一） |
| | `NEG` `NOT` `INVERT` | ABC | 无（弹一压一） |
| 比较 | `EQ` `NE` `LT` `LE` `GT` `GE` `IS` `IN` | ABC | 无（弹二压一布尔） |
| | `CMP_CHAIN` | ABC | `A` = 链中剩余比较段数（链式比较 `a < b < c` 的操作数排列由任务 07/08 约定） |
| 跳转 | `JUMP` `JUMP_IF_FALSE` `JUMP_IF_TRUE` `JUMP_IF_NIL` | sAx | 相对偏移；条件跳转弹栈顶测试 |
| 容器 | `BUILD_LIST` `BUILD_DICT` | ABC | `Bx` = 元素数（dict 为键值对数），自栈弹出 |
| | `INDEX` `SET_INDEX` `DEL_INDEX` | ABC | 无（容器与下标自栈弹出） |
| | `APPEND` | ABC | 无（弹值追加到栈次顶 list，供构建/推导式用） |
| 调用 | `CALL` `TAIL_CALL` `CALL_METHOD` | ABC | `A` = 位置参数个数 |
| | `CALL_KW` | ABC | `A` = 位置参数个数，`Bx` = 关键字名表的常量池下标 |
| | `LOAD_METHOD` | ABC | `Bx` = 方法名的常量池下标（压入方法与接收者） |
| | `RETURN` | ABC | 无（返回值在栈顶，编译器保证空栈时补 nil） |
| 函数/类 | `MAKE_FUNCTION` `MAKE_LAMBDA` | ABC | `Bx` = 子 Proto 的常量池下标（upvalue 自栈弹出） |
| | `MAKE_CLASS` | ABC | `Bx` = 类名的常量池下标（方法表自栈弹出） |
| 属性 | `GET_ATTR` `SET_ATTR` `DEL_ATTR` | ABC | `Bx` = 属性名的常量池下标 |
| 迭代 | `GET_ITER` | ABC | 无（弹可迭代对象压迭代器） |
| | `ITER_NEXT` | sAx | 迭代器在栈顶；耗尽时按 `sAx` 跳转，否则压入下一元素（对齐 §2.2「失败时跳转」） |
| | `UNPACK` | ABC | `A` = 解包目标个数 |
| 其他 | `PRINT_EXPR` | ABC | 无（REPL：弹栈顶，非 nil 则打印表示） |
| | `NOP` | ABC | 无 |

v0.2 启用：`IMPORT` `IMPORT_FROM`（`Bx` = 模块名常量下标）、`BUILD_TUPLE` `BUILD_SET`（同 `BUILD_LIST`）、`SLICE`、`SETUP_TRY`（`Bx` = `tryBlocks` 下标，运行时装配异常处理器）、`POP_TRY`、`RAISE`、`RERAISE`。v0.3 启用：并发类 10 条。这些条目本任务只定枚举、格式与元数据，不定义语义细节（由对应阶段任务文档定稿）。

### MsProto 结构（ms_proto.h）

内部结构体不 typedef，字段 lowerCamelCase；字段顺序对齐 §2.1，补充两个长度字段：

```c
struct MsTryBlock {        // exception table entry (layout per 08-vm-internals §2.1)
  uint32_t pcStart;        // protected range [pcStart, pcEnd)
  uint32_t pcEnd;
  uint32_t handlerPc;      // catch entry; MS_PROTO_PC_NONE if absent
  uint32_t finallyPc;      // finally entry; MS_PROTO_PC_NONE if absent
};

#define MS_PROTO_PC_NONE UINT32_MAX

struct MsLineEntry {       // pc -> line map, sorted by pc, run-encoded
  uint32_t pc;             // first pc of this run
  uint32_t line;           // 1-based source line
};

struct MsProto {
  uint32_t* code;          // instruction stream, msAlloc'd, codeLen entries
  struct MsObject** consts; // constant pool: numbers, strings, child protos
  int codeLen;
  int constsLen;
  int paramCount;          // parameter count
  int localCount;          // register window size (local variable slots; first paramCount hold args)
  int stackSize;           // max eval stack depth (computed at compile time)
  struct MsTryBlock* tryBlocks;  // exception table; NULL/0 until exceptions land (v0.2)
  int tryBlocksLen;
  struct MsLineEntry* lines;     // pc -> line map
  int linesLen;
  struct MsString* name;         // function name; "<main>" for module top level
  struct MsString* sourceFile;
  bool isAsync;
  bool hasVarArgs;
  bool hasKwArgs;
};
```

字段语义补充：

- `codeLen/constsLen` 等计数沿用规范的 `int`；循环下标可用 `int`（10-c-style §4）。
- 帧窗口 = `localCount` 个槽，前 `paramCount` 槽由调用方以实参初始化；求值栈按 `stackSize` 上限分配（任务 08 据两值确定帧布局）。
- 标志位与函数语义（async/可变参数/关键字参数）对应 `hasVarArgs`/`hasKwArgs` 的调用约定由任务 13 定稿；本任务只承载字段。
- 每个 `MsProto` 的指令流末尾由编译器保证以 `MS_OP_RETURN` 结束（任务 07 补齐，本任务在校验层面不强制）。

### 常量池约定

- 下标 0 起始；`LOAD_CONST Bx`、`MAKE_FUNCTION Bx` 等直接索引 `consts[Bx]`；单 Proto 上限 65536 项（含子 Proto），溢出属编译错误。
- 池内容：数字（int/float）、字符串、子 Proto 三类对象；`nil`/布尔不入池（专用指令 `LOAD_NIL`/`LOAD_TRUE`/`LOAD_FALSE`）。
- 构建期去重：同类型同值的常量共享槽位（数字按位值比较、字符串按内容比较、子 Proto 不去重）；去重在 `msProtoAddConst` 内做线性扫描（单 Proto 常量数有限，O(n²) 可接受；若基准测试暴露问题，在任务 07 侧加哈希索引，构建器 API 不变）。
- 常量对象的比较需要对象模型（任务 06）的值相等接口；本任务假定接口名 `bool msObjectValueEquals(const struct MsObject* a, const struct MsObject* b)`，实现时以任务 06 文档定名为准。在任务 06 就绪前，`msProtoAddConst` 先按指针相等去重，值去重随任务 06 接入（验收标准不含值去重用例，测试方案中单独标注）。
- 所有权：入池对象的所有权随 Proto 移交（GC 化后改为 GC 托管）；`msProtoFree` 只释放数组与 Proto 本体，不释放池内对象（对象的释放一律归对象模型/GC 管，避免双重释放）。

### 行号表与异常表

- 行号表为按 `pc` 升序的游程编码：仅当行号变化时追加一条 `{pc, line}`；`msProtoLineAt(proto, pc)` 二分查找最后一个 `entry.pc <= pc` 的条目，表空返回 0。编译器在发射前用 `msProtoSetLine` 登记当前行，构建器在行号变化时自动追加条目。
- 异常表同样按 `pcStart` 升序追加；`pcStart` 含、`pcEnd` 不含；`handlerPc`/`finallyPc` 缺省取 `MS_PROTO_PC_NONE`。v0.1 无异常语句，本任务只定义结构与追加 API，装配与匹配语义属任务 23（异常系统）。

### Proto 构建器

编译器（任务 07）的唯一生成入口；粘性错误模型：任一调用失败即记入 `b->error` 且后续调用短路，`msProtoBuild` 集中报告，编译器不必逐步检查。

```c
struct MsProtoBuilder {
  struct MsProto proto;    // arrays grow to capacity; shrunk and detached by msProtoBuild
  int codeCap;
  int constsCap;
  int tryCap;
  int linesCap;
  uint32_t pendingLine;    // line registered for the next emit
  bool hasPendingLine;
  MsResult error;          // sticky first failure (MS_OK while healthy)
};

void msProtoBuilderInit(struct MsProtoBuilder* b);
void msProtoBuilderDestroy(struct MsProtoBuilder* b);  // safe after a failed build; no-op after success

// Emits one instruction with the pending line; returns its pc, or -1 (sticky
// MS_ERROR_OOM, or MS_ERROR_SYNTAX when sax is out of 24-bit range).
// MS_ASSERTs that op's declared format matches the emit variant used.
int msProtoEmitABC(struct MsProtoBuilder* b, MsOpCode op, uint8_t a, uint16_t bx);
int msProtoEmitSAx(struct MsProtoBuilder* b, MsOpCode op, int32_t sax);

// Registers the source line for subsequently emitted instructions.
void msProtoSetLine(struct MsProtoBuilder* b, uint32_t line);

// Rewrites the sAx operand of a previously emitted jump (back-patching).
// pc must address an MS_OPFMT_SAX instruction.
MsResult msProtoPatchSAx(struct MsProtoBuilder* b, int pc, int32_t sax);

// Adds a constant with dedup; returns the pool index, or -1 (sticky
// MS_ERROR_OOM, or MS_ERROR_SYNTAX when the pool exceeds 65536 entries).
int msProtoAddConst(struct MsProtoBuilder* b, struct MsObject* value);

// Appends an exception table entry (v0.2 consumers; defined now per spec layout).
MsResult msProtoAddTryBlock(struct MsProtoBuilder* b, uint32_t pcStart, uint32_t pcEnd,
    uint32_t handlerPc, uint32_t finallyPc);

// Shrinks all arrays to exact length and hands the proto to *out (caller owns,
// frees with msProtoFree). Fails with the sticky error if any call failed,
// or with MS_ERROR_SYNTAX if codeLen == 0. On success the builder is left
// empty; on failure the caller still owns the builder and must destroy it.
MsResult msProtoBuild(struct MsProtoBuilder* b, struct MsProto** out);

// Frees a proto's arrays and the struct itself. Does NOT free consts entries
// or name/sourceFile (owned by the object model / GC once task 06 lands).
void msProtoFree(struct MsProto* proto);

// Binary-searches the line table; returns 0 when the table is empty.
uint32_t msProtoLineAt(const struct MsProto* proto, int pc);

// Disassembles the instruction stream and tables for debugging and tests.
// Constants print as "#k<index>" placeholders until the object model lands.
void msProtoDump(const struct MsProto* proto, FILE* out);
```

实现要点：

- 四条数组初始容量 8、倍增增长；`msProtoBuild` 用 `msRealloc` 收缩到 `len == cap`（`len == 0` 的数组收缩为 `NULL`）。
- 编译器对 `proto` 的元数据字段（`paramCount`/`localCount`/`stackSize`/`name`/标志位等）在 `msProtoBuild` 前直接写 `b->proto`，构建器只管四条数组与行号登记。
- 格式一致性（ABC 指令走 `msProtoEmitABC` 等）由 `msOpFormatOf` 查表 + `MS_ASSERT` 守住，属编程错误而非运行期检查。

### 与对象模型的衔接（迁移说明）

任务 06 落地时对本模块的改动限于三点，API 形状不变：

1. `struct MsProto` 头部插入 `MsObjectHeader header;`，创建路径从 `msAlloc` 切换为 GC 对象分配（任务 06 的接口，假定名 `msObjectNewProto`，以任务 06 文档为准）；
2. `msProtoFree` 收缩为仅供测试/错误路径使用的内部释放，正常路径改由 GC 回收；
3. `msProtoAddConst` 的指针去重升级为 `msObjectValueEquals` 值去重；`msProtoDump` 的 `#k<idx>` 占位升级为打印常量值。

## 实现步骤

1. 建 `src/vm/ms_opcode.h`：`MsOpFormat`、`MsOpCode` 全量枚举（82 条 + `MS_OP_COUNT`）、`_Static_assert` 上限、六个编码/解码内联函数。验证：单元测试对全部操作码做编码→解码往返，含 `A`/`Bx`/`sAx` 边界值（0、255、65535、±8388608/8388607）。
2. 建 `src/vm/ms_opcode.c`：`opInfoTable` 与 `msOpFormatOf`/`msOpName`。验证：表长等于 `MS_OP_COUNT`、每条目名称非空且与枚举同序（测试按枚举序遍历断言），跳转类 4 条 + `ITER_NEXT` 格式为 `MS_OPFMT_SAX`、其余为 `MS_OPFMT_ABC`。
3. 建 `src/vm/ms_proto.h` / `ms_proto.c` 骨架：四个结构体、`MS_PROTO_PC_NONE`、构建器 init/destroy。验证：init 后全零状态、destroy 空构建器无分配残留。
4. 实现发射与行号登记：`msProtoEmitABC`/`msProtoEmitSAx`（含容量倍增、粘性错误、sAx 越界检查）、`msProtoSetLine`（行号变化才追加 `lines` 条目）。验证：发射序列的 pc 递增、数组增长、行号游程编码正确。
5. 实现 `msProtoPatchSAx`（回填校验：目标必须是 SAX 指令）。验证：先占位后回填的跳转偏移正确；越界/格式不符按约定失败。
6. 实现 `msProtoAddConst`（指针去重 + 65536 上限）与 `msProtoAddTryBlock`。验证：同指针去重、上限溢出报错、异常表追加顺序。
7. 实现 `msProtoBuild` / `msProtoFree` / `msProtoLineAt` / `msProtoDump`。验证：定稿收缩（`len == cap`）、空指令流报错、行号二分查找各分支、dump 文本快照、任务 02 分配统计显示无泄漏。
8. 接入 CMake `mslang` 库目标与 `mslang-tests`，`ctest` 全绿；`MSLANG_STRICT_WARNINGS` 无警告。

## 测试方案

本任务早于最小可运行解释器（任务 09），只能用 C 单元测试。测试文件两个（本任务只交付本设计文档，测试代码随实现任务编写），使用任务 01 的 `ms_test.h`（`MS_TEST`/`MS_ASSERT_EQ`）：

`tests/c/test_opcode.c` 覆盖：

- 编码/解码往返：对 `MS_OP_COUNT` 逐一枚举值，分别以 `A`/`Bx` 边界组合（0/1/255、0/1/65535）编码 ABC 再解码还原；以 sAx 边界（0、±1、8388607、-8388608）编码 SAX 再解码还原（含符号扩展正确性）。
- 越界防御：`msOpEncodeSAx` 越界入参在 debug 构建触发 `MS_ASSERT`（以编译期 `_Static_assert` + 断言路径说明，不做运行期死亡测试）。
- 元数据表：表长 = `MS_OP_COUNT`；`msOpName` 全量非空、互不相同；`msOpFormatOf` 对 `JUMP`/`JUMP_IF_FALSE`/`JUMP_IF_TRUE`/`JUMP_IF_NIL`/`ITER_NEXT` 返回 `MS_OPFMT_SAX`，对其余返回 `MS_OPFMT_ABC`。
- 子集清点：遍历枚举断言 v0.1 子集恰为 63 条、全集恰为 82 条且 ≤ 256（80 条软目标在评审时人工核对）。

`tests/c/test_proto.c` 覆盖：

- 构建器生命周期：init 全零；空构建器 destroy 后分配统计归零。
- 发射：连续 `msProtoEmitABC`/`msProtoEmitSAx` 返回 pc 0、1、2…；超过初始容量后数组正确增长且内容不丢；sAx 越界返回 -1 且粘性错误为 `MS_ERROR_SYNTAX`，后续发射短路。
- 行号表：`msProtoSetLine` 同行不追加、变行追加；构造多段行号后 `msProtoLineAt` 对表首/表尾/中间/空表的查询结果正确。
- 回填：发射占位 `JUMP`（sAx=0）后 `msProtoPatchSAx` 写回，解码验证偏移；对 ABC 指令回填返回错误。
- 常量池：`msProtoAddConst` 同指针去重返回同一下标；不同对象递增下标；构造上限溢出返回 -1（粘性 `MS_ERROR_SYNTAX`）。值去重（同值不同指针对象）属任务 06 接入点，本任务不测，此处显式标注。
- 异常表：`msProtoAddTryBlock` 追加与字段原样保留；`MS_PROTO_PC_NONE` 哨兵语义。
- 定稿：`msProtoBuild` 成功后 `len == cap`、空数组为 `NULL`、构建器被掏空（二次 destroy 安全）；空指令流定稿返回 `MS_ERROR_SYNTAX`；构建器粘性错误未清时定稿失败且所有权仍在调用方。
- 释放：build → free 全路径后任务 02 分配统计归零（无泄漏）；`msProtoDump` 输出与黄金文本比对（含 `#k0` 常量占位格式）。

## 验收标准

- [ ] `src/vm/ms_opcode.{h,c}` 与 `src/vm/ms_proto.{h,c}` 存在，guard 分别为 `MSLANG_SRC_VM_MS_OPCODE_H_` / `MSLANG_SRC_VM_MS_PROTO_H_`，头文件自包含，风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef）。
- [ ] 指令定长 4 字节、两种格式位布局与本文一致；编码/解码内联函数对全操作码与边界操作数往返正确；`sAx` 有符号 24 位、跳转偏移相对下一条指令。
- [ ] `MsOpCode` 全集 82 条 + `MS_OP_COUNT`，v0.1 子集 63 条；元数据表与枚举同序完备，`msOpName`/`msOpFormatOf` 行为符合测试方案。
- [ ] `struct MsProto` 字段与本文一致（含补充的 `tryBlocksLen`/`linesLen`，不含留待任务 06 的 GC 头）；常量池上限 65536、去重与所有权约定落实。
- [ ] 构建器 API 完整（发射/行号/回填/常量/异常表/定稿/释放），粘性错误模型与所有权移交语义如文所述；全部堆分配经 `msAlloc/msRealloc/msFree`，分配统计无泄漏。
- [ ] 行号表游程编码 + 二分查询、异常表布局（含 `MS_PROTO_PC_NONE`）与本文一致。
- [ ] `tests/c/test_opcode.c` 与 `tests/c/test_proto.c` 覆盖「测试方案」全部清单项并全部通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 02/06 的接口假定（`msAlloc`、`MsResult`、`struct MsObject`、`msObjectValueEquals` 等）在实现时已按对应任务文档对齐。
