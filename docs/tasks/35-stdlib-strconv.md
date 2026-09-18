# 35 标准库：strconv

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [18 C API 基础](18-c-api-foundation.md) |

## 任务目标

交付 C 实现的标准库模块 `strconv`（`stdlib/ms_std_strconv.h` / `stdlib/ms_std_strconv.c`），完整覆盖 `docs/language/07-stdlib.md` §3 的全部 6 个函数：`parseInt(s, base=10)`、`parseFloat(s)`、`formatInt(i, base=10)`、`formatFloat(f, prec=-1)`、`quote(s)`、`unquote(s)`。

核心语义决策：`parseInt` 产出**任意精度** int——小整数走 int64 快路径，超出 int64 时透明落入任务 31 的大整数表示，对脚本作者无感知（02-types §3.1）；`formatInt` 反向支持 2–36 进制；`parseFloat` 严格按语言浮点字面量文法校验后经 `strtod` 转换；`formatFloat(f, -1)` 产出**最短往返**（shortest round-trip）表示；`quote`/`unquote` 实现与语言字符串字面量一致的转义编解码。转换失败一律抛 `ValueError`（07-stdlib §3），参数类型错误抛 `TypeError`。

为此本任务同时交付一层纯 C 转换算法层（`src/object/ms_num_conv.{c,h}`），只操作字节切片、不触碰 `MsObject`，供 strconv 模块、VM 内 `str(float)` 默认转换及后续浮点字面量求值共享。完成后脚本侧 `import "strconv"` 即可使用全部功能（import 语句由任务 24 交付，不再做 v0.1 的全局预绑定），经 `tests/ms/stdlib/strconv/` 下的 ms 脚本测试验证。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0：`strconv` 属 **C 内建模块**，源码位于 `stdlib/` 目录；函数命名小驼峰。
  - §3：函数清单与签名（本任务的功能边界，逐条对应，不增不减）；`parseInt` 失败抛 `ValueError`；`formatFloat(f, prec=-1)` 的 `prec=-1` 语义为最短往返表示。
- `docs/language/02-types.md`
  - §3.1：int 任意精度，VM 内部小整数机器字优化、溢出自动提升大整数——`parseInt`/`formatInt` 与大整数协作的依据。
  - §3.2：float 为 IEEE 754 双精度，特殊值 `inf`/`nan`。
  - §3.3：`int op float` 提升为 float；**不存在 float → int 隐式转换**；显式转换 `int("42")`/`float("3.14")`/`str(42)` 与本模块语义的边界见「详细设计」第 7 节。
- `docs/language/01-lexical.md` §5：整数字面量的进制前缀（`0x`/`0o`/`0b`）与下划线规则（`base=0` 自动检测的依据）；浮点字面量文法（`.5` 合法、`5.` 非法、指数形式，`parseFloat` 校验文法的来源）；§5.3 字符串转义表（`\n \t \r \\ \" \' \0 \xHH \uHHHH \UHHHHHHHH`，`unquote` 的解码依据）。
- `docs/language/04-exceptions.md` §4 内建异常层级：`TypeError`/`ValueError` 类名约定；任务 23 已落地异常系统，本任务直接抛真实异常对象（与 v0.1 各 stdlib 任务的字符串错误占位不同），ms 测试可用 `try/except` 断言负向行为。
- `docs/language/05-modules.md` §2/§7：内建 C 模块经内建注册表解析、优先级高于搜索路径，不可被同名脚本遮蔽。
- `docs/language/09-c-api.md` §3（GC 根栈纪律）、§5（`msNewIntFromString`/`msNewFloat`/`msNewStringN`/`msAsCString`/`msStringLen` 等值构造与转换；`msAsCString` 缓冲区「下一次分配前有效」）、§8（`msRaiseTypeError`/`msRaiseValueError` 错误约定，C 函数出错返回 `NULL`）、§9（`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule` 注册机制）。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）；§5「禁止 errno 跨层传播」——`strtod` 的 `ERANGE` 判定在调用点立即转换为返回值语义，不外泄。
- `docs/language/11-project-layout.md` §1（`stdlib/` 为 C 标准库模块目录、`src/object/` 为类型实现目录、`tests/ms/` 为脚本测试目录）、§4（脚本测试层约定）。
- `docs/tasks/README.md` 测试约定：任务编号 ≥ 09 一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`，由仓库根 `run_tests.py` 驱动（设施约定见 [09-minimal-interpreter.md](09-minimal-interpreter.md) 测试方案）。
- 任务 18（C API 基础）提供：公开头文件族（`object.h`/`error.h`/`gc.h`）、`msRegisterModule`、标准库启动注册链（`msNewState` 内统一调用各 stdlib 模块注册函数）。
- [20 标准库：strings](20-stdlib-strings.md) 提供：共享算法层自备增长缓冲 `struct MsStrBuf`（`src/object/ms_str_op.h`：`msStrBufInit`/`msStrBufFree`/`msStrBufPut`），本任务的格式化与 quote/unquote 输出直接复用，不重复造缓冲。
- 任务 23（异常系统）提供 `ValueError`/`TypeError` 异常类，`msRaiseValueError`/`msRaiseTypeError` 产出真实异常实例；任务 24（模块系统）提供 `import` 语句与内建注册表解析。
- 任务 31（任意精度整数）提供：`msNewIntFromString`（09-c-api §5 声明、任务 18 明确推迟至任务 31 落地）与大整数的进制格式化接口。任务 31 的设计文档本文撰写时尚不存在，其内部接口名（本文假定为 `msIntIsBig` / `msBigintFormat`）均为假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件布局与分层

```
stdlib/
├── ms_std_strconv.h     # 模块对外声明：注册入口
└── ms_std_strconv.c     # 6 个 MsCFunction 包装、MsModuleDef 注册表
src/object/
├── ms_num_conv.h        # 纯 C 转换算法层声明
└── ms_num_conv.c        # 算法层实现
```

分层职责（与任务 20 strings 的「算法层 + 模块层」模式一致）：

- **算法层**（`src/object/ms_num_conv.*`）：纯 C 函数，只操作 `const char* + size_t` 字节切片与 `struct MsStrBuf`，不触碰 `MsObject`、`MsState` 与脚本层概念。负责：整数字面量语法校验与规范化、int64 进制格式化、`strtod` 浮点解析的严格前置校验、最短往返格式化、quote/unquote 编解码。
- **模块层**（`stdlib/ms_std_strconv.c`）：`MsCFunction` 包装——参数个数/类型校验、取字节切片、调用算法层、装箱为脚本对象、把算法层的失败原因转成 `ValueError`/`TypeError`。全部 `static`，唯一导出符号是注册入口 `msStdStrconvRegister`。
- **大整数协作**：模块层在 parse/format 的装箱点对接任务 31 的接口（见第 5 节），任意精度算法本身不在本任务实现。

### 2. 函数语义总表（唯一权威）

07-stdlib §3 只给出签名；下表逐函数固定语义，规范未写明处以本表为准：

| 函数 | 语义 | 边界与失败 |
|---|---|---|
| `parseInt(s, base=10)` | 字符串 → int（任意精度） | `base ∈ {0} ∪ [2, 36]`，否则 ValueError；`base == 0` 按前缀自动检测（见第 3 节）；失败抛 ValueError |
| `parseFloat(s)` | 字符串 → float | 文法 = 语言浮点字面量 + 可选符号 + `inf`/`nan` 拼写；失败抛 ValueError；上溢得 `±inf`、下溢得 `±0.0`，不报错 |
| `formatInt(i, base=10)` | int → 字符串 | `base ∈ [2, 36]`，否则 ValueError；小写字母数字；负数带 `-` 前缀 |
| `formatFloat(f, prec=-1)` | float → 字符串 | `prec == -1` 最短往返；`prec >= 0` 为有效数字位数（`%.*g` 语义）；`prec < -1` 抛 ValueError；接受 int 实参（提升为 float） |
| `quote(s)` | 加引号并转义 → 字符串 | 永不失败（输入恒为合法 UTF-8 的 str） |
| `unquote(s)` | 解析带引号字符串 → 字符串 | 接受双引号（含转义）与反引号 raw 两种；失败抛 ValueError |

通用约定：

- 首参数必须是 str（`formatInt`/`formatFloat` 为数值），参数个数或类型不符抛 `TypeError`；上表标注 ValueError 的情形抛 `ValueError`。错误消息含函数名与输入原文，形如 `strconv.parseInt: parsing "abc": invalid syntax`。
- **不做空白剥离**：`parseInt(" 12")`、`parseFloat("3.14 ")` 均抛 ValueError（对齐 Go 的严格性；剥离是调用方职责，用 `strings.trimSpace`）。07-stdlib §3 未规定，本文定为模块契约。
- **bool 不是数值参数**：`formatInt(true)`、`formatFloat(true)` 抛 `TypeError`（与任务 21 math 的参数约定一致）。
- `parseInt`/`parseFloat` 接受**纯整数形式**输入浮点解析（`parseFloat("42") == 42.0`，与 Go/Python 一致）。

### 3. parseInt 语法与进制处理

输入文法（`base` 已确定后；`digit` 的取值必须 `< base`，`a`–`z` 大小写不敏感映射为 10–35）：

```
intInput  := sign? body
body      := prefix? digits          // 仅 base == 0 时允许 prefix 与下划线
sign      := '+' | '-'
prefix    := '0x' | '0X' | '0o' | '0O' | '0b' | '0B'
```

规则：

- **`base == 0` 自动检测**（07-stdlib §3 未规定 `base=0`，本文对齐 Go 与语言字面量前缀，定为扩展）：`0x`/`0X` → 16、`0o`/`0O` → 8、`0b`/`0B` → 2、其余 → 10。此模式下允许数字间的下划线，规则与语言整数字面量一致（01-lexical §5：不得在首尾、不得连续、进制前缀后允许一个，即 `0x_1F` 合法）。
- **`base != 0`**：不接受任何进制前缀（`parseInt("0x1F", 16)` 抛 ValueError，对齐 Go），不接受下划线（`parseInt("1_000", 10)` 抛 ValueError；下划线是字面量特性而非数据格式特性）。
- 空串、仅符号、符号后无前缀无数字、前缀后无数字、出现 `< base` 之外的数字（如 `parseInt("12", 2)`）→ 一律 ValueError。
- `parseInt("0", 10)` 等全零输入合法，得 int `0`；`+`/`-` 符号作用于整个值，`-0` 得 `0`。

处理流程（模块层）：

1. 校验 `argc ∈ [1, 2]`、`argv[0]` 为 str、`argv[1]`（若给）为 int 且落在 `{0} ∪ [2, 36]`。
2. 调算法层 `msNumConvScanInt`（见第 4 节）做语法校验：产出「规范化视图」——符号、去前缀去下划线后的数字切片（下划线经 `struct MsStrBuf` 临时缓冲消除；无下划线时零拷贝直接引用原切片）。
3. 调 `msNewIntFromString(L, norm, base)`（任务 31）装箱：**任意精度分派发生在任务 31 内部**——其内部先按 int64 累加、溢出自动提升大整数。本任务不实现任何进位累加。
4. `msNewIntFromString` 在已校验输入上不应失败；防御性处理其 `NULL` 返回（OOM 时直接传播错误状态）。

### 4. 算法层接口（src/object/ms_num_conv.h）

include guard `MSLANG_SRC_OBJECT_MS_NUM_CONV_H_`，自包含（`<stdbool.h>`/`<stddef.h>`/`<stdint.h>` + `"object/ms_str_op.h"` 取 `struct MsStrBuf`）。内部结构体不 typedef：

```c
struct MsIntScan {          // result of integer syntax scanning; non-owning views
  bool negative;            // leading '-' present
  const char* digits;       // digit span (underscore-free when the input had none)
  size_t digitsLen;
  int base;                 // resolved base after prefix detection
};

// Validates intInput syntax (see module contract) against baseHint
// (0 = auto-detect, else 2..36). On success fills scan; when the input
// contains underscores, copies the underscore-free digits into normBuf
// (otherwise scan->digits points into the input and normBuf is untouched).
// Returns false on any syntax error and sets *errPos to the offending byte.
bool msNumConvScanInt(const char* s, size_t len, int baseHint,
    struct MsIntScan* scan, struct MsStrBuf* normBuf, const char** errPos);

// Formats v in base (2..36, lowercase digits) into out. Caller validates
// base. Never fails except OOM (MS_ERROR_OOM).
MsResult msNumConvFormatInt64(int64_t v, int base, struct MsStrBuf* out);

// Validates the language float literal grammar (optional sign, decimal
// point and exponent per 01-lexical §5.2, plus case-insensitive "inf"/
// "nan" with optional sign) and converts via strtod. Overflow yields
// ±HUGE_VAL, underflow ±0.0 (no error). Returns false on syntax error.
bool msNumConvParseFloat(const char* s, size_t len, double* out);

// prec == -1: shortest representation that round-trips through
// msNumConvParseFloat. prec >= 0: significant-digit count (%.*g style).
// Appends to out; only fails on OOM. inf/nan render as "inf"/"-inf"/"nan".
MsResult msNumConvFormatFloat(double v, int64_t prec, struct MsStrBuf* out);

// Appends the double-quoted, escaped form of s to out; only fails on OOM.
MsResult msNumConvQuote(const char* s, size_t len, struct MsStrBuf* out);

// Decodes a quoted literal (double-quoted with escapes, or backquoted raw)
// into out. Returns false on syntax error (msStrBuf content unspecified).
bool msNumConvUnquote(const char* s, size_t len, struct MsStrBuf* out);
```

核心算法要点：

- **`msNumConvFormatInt64`**：在 `uint64_t` 幅度上反复取余生成逆序数字再反转，避免 `INT64_MIN` 取负的未定义行为；符号最后前置。缓冲区经 `msStrBufPut` 追加，无固定栈上数组的长度假设问题（int64 最长 66 字节：符号 + 64 位二进制）。
- **`msNumConvParseFloat`**：两遍处理——第一遍自建校验器严格按文法扫描（只认 `.` 小数点、`e|E` 指数、指数可选符号；**拒绝下划线**与十六进制浮点；`.5` 合法、`5.` 非法、`1e` 非法），通过后第二遍把切片拷入 `msAlloc` 的 NUL 结尾临时缓冲调 `strtod`（`errno = 0` 前置、调用点立即检查 `ERANGE` 并转为「上溢 ±inf / 下溢 ±0.0」语义，不跨层传播，10-c-style §5）。`inf`/`nan` 拼写不走 `strtod`，直接置 `INFINITY`/`NAN`（带符号）。
- **`msNumConvFormatFloat` 最短往返（`prec == -1`）**：不引入 Ryu/Grisu，采用「迭代精度 + 往返校验」：对 `p` 从 1 到 17 递增，`snprintf("%.*e", p - 1, v)` 得科学计数法候选，`strtod` 读回与 `v` 位级相等（含 `-0.0` 的符号位）即取 `p` 为最短有效数字数；随后按 Python `repr` 风格排版——小数指数落在 `[-4, 16)` 内用定点记法（如 `0.001`、`1234567890123456.0`），否则用科学记法（如 `1e-05`、`1.234567890123456e+16`）；**输出恒含小数点或指数**（`3.0` 而非 `3`），保证 `str(3.0)` 与 `str(3)` 可区分且往返后仍是 float。17 位内必收敛（IEEE 754 双精度性质）。正确性优先；性能若被任务 64 基准证明显著再换 Ryu。
- **`msNumConvFormatFloat`（`prec >= 0`）**：`snprintf("%.*g", (int)prec, v)` 语义（有效数字、去尾零、必要时指数），`prec == 0` 按 C 语义即 1 位有效数字；输出原样采用，不强制补小数点（与 `prec == -1` 档的排版承诺不同，调用方自选）。
- **locale 限制**：`strtod`/`snprintf` 的小数点受 locale 影响。算法层以 **"C" locale 为前提**——mslang CLI 不调 `setlocale`，进程默认即 "C"；嵌入方若改动全局 locale 则浮点转换行为未定义，此限制写入模块头注释（首版不实现 locale 无关解析，列入路线图）。
- **`msNumConvQuote`**：单遍输出——`"`、`\`、`\n`、`\t`、`\r`、`\0` 用对应转义；其余 C0 控制码（`< 0x20`）与 `0x7F` 用 `\xHH`；可打印 Unicode 码点**原样保留**（不做 `\u` 转义，对齐 Go `strconv.Quote` 的默认行为；ASCII-only 转义变体列入路线图）。算法层先解码码点（复用任务 06 的 UTF-8 原语），ASCII 快路径直接判定。
- **`msNumConvUnquote`**：首字节判定形式——`"` 进入转义解码（转义表与 01-lexical §5.3 逐字一致：`\n \t \r \\ \" \' \0 \xHH \uHHHH \UHHHHHHHH`，位数/码点范围校验同 lexer 规则，`\u`/`\U` 编码为 UTF-8 追加），`` ` `` 进入 raw 模式（原样拷贝，不得含反引号）；其余首字节（含单引号——语言无单引号字符串）直接失败。与任务 03 的 `msLexerUnescape` 语义同源但**独立实现**：lexer 版本假定输入已校验、不解引号，本函数边校验边解码、处理引号形式，两者不复用代码。

### 5. 与大整数（任务 31）的协作边界

- **方向一（parseInt → 装箱）**：本任务只做语法校验与规范化（第 3 节第 2 步），数值累加与大整数提升完全委托 `msNewIntFromString(L, text, base)`（09-c-api §5 签名，任务 31 落地）。本任务**不读、不写**大整数的内部表示，不知道其 limb 布局。
- **方向二（formatInt ← 取值）**：模块层先按对象内部分派——小整数（机器字存储）走算法层 `msNumConvFormatInt64` 快路径；大整数对象委托任务 31 的进制格式化接口。假定接口（实现时以任务 31 定名为准）：

```c
// Assumed task-31 interfaces (src/object/ms_int.h or similar):
bool msIntIsBig(MsObject* intObj);                                    // small-int vs bigint dispatch
int64_t msIntAsInt64(MsObject* intObj);                               // valid only when !msIntIsBig
MsResult msBigintFormat(MsObject* bigObj, int base, struct MsStrBuf* out);  // base 2..36
```

  若任务 31 定稿的进制格式化只覆盖部分进制（如仅 10/16），超出部分由任务 31 补齐，本任务的脚本契约（2–36 全进制）不变。
- **溢出语义统一**：`parseInt` 永不因数值大小失败（任意精度），只可能因语法或 OOM 失败；不存在「parseInt 溢出」错误路径。`formatInt` 同理无范围限制。
- **不变式**：`parseInt(formatInt(i, b), b) == i` 对任意 int `i`（含大整数）与任意合法 `b` 成立，测试中专项断言。

### 6. 模块封装层（stdlib/ms_std_strconv.c）

- include guard `MSLANG_STDLIB_MS_STD_STRCONV_H_`（`stdlib/` 不在 `src/` 下，前缀为 `MSLANG_STDLIB_`）；头文件自包含（`<mslang/mslang.h>`），唯一导出符号：

```c
// stdlib/ms_std_strconv.h

// Registers the "strconv" builtin module into the builtin module registry.
// Called once during interpreter startup (msNewState's stdlib wiring).
MsResult msStdStrconvRegister(MsState* L);
```

- 6 个包装函数均为 `static MsObject* strconvXxx(MsState* L, int64_t argc, MsObject** argv)`，统一骨架（以 `parseInt` 为例）：

```c
static MsObject* strconvParseInt(MsState* L, int64_t argc, MsObject** argv) {
  int64_t base = 10;
  if (argc < 1 || argc > 2 || msTypeOf(argv[0]) != MS_TYPE_STR) {
    msRaiseTypeError(L, "strconv.parseInt() requires (str, base=10)");
    return NULL;
  }
  if (argc == 2) {
    if (msTypeOf(argv[1]) != MS_TYPE_INT) {
      msRaiseTypeError(L, "strconv.parseInt() base must be int");
      return NULL;
    }
    base = msAsInt(L, argv[1]);
    if (base != 0 && (base < 2 || base > 36)) {
      msRaiseValueError(L, "strconv.parseInt() base must be 0 or in [2, 36]");
      return NULL;
    }
  }
  const char* s = msAsCString(L, argv[0]);    // argv are roots; no API call until boxing
  size_t sLen = msStringLen(argv[0]);
  struct MsIntScan scan;
  struct MsStrBuf normBuf;
  msStrBufInit(&normBuf);
  const char* errPos = NULL;
  if (!msNumConvScanInt(s, sLen, (int)base, &scan, &normBuf, &errPos)) {
    msStrBufFree(&normBuf);
    msRaiseValueError(L, "strconv.parseInt: parsing \"%s\": invalid syntax", s);
    return NULL;
  }
  MsObject* result = msNewIntFromString(L, scan.digits, scan.base);  // sign folded per scan.negative
  msStrBufFree(&normBuf);
  return result;                              // NULL propagates OOM
}
```

  （符号处理细节：`scan.negative` 与数字切片在调用 `msNewIntFromString` 前合成规范化串，或经任务 31 接口的符号参数传达，实现时二选一并对齐任务 31 定名；行为契约不变。）

- `msAsCString` 指针窗口纪律与任务 20 相同：「先取指针与长度、算法层跑完、再装箱」，窗口内不调用其他可能分配的 C API。
- 可选参数的默认值（`base=10`、`prec=-1`）在 C 层按 `argc` 分派模拟（与任务 21 的 `log` 同模式）。
- 方法表与注册：

```c
static const MsMethodDef strconvMethods[] = {
  {"parseInt",    strconvParseInt,    "parseInt(s, base=10) -> int"},
  {"parseFloat",  strconvParseFloat,  "parseFloat(s) -> float"},
  {"formatInt",   strconvFormatInt,   "formatInt(i, base=10) -> str"},
  {"formatFloat", strconvFormatFloat, "formatFloat(f, prec=-1) -> str"},
  {"quote",       strconvQuote,       "quote(s) -> str"},
  {"unquote",     strconvUnquote,     "unquote(s) -> str"},
  {NULL, NULL, NULL},
};

static const MsModuleDef strconvModuleDef = {
  "strconv", "string <-> number conversions", strconvMethods,
};
```

  `msStdStrconvRegister` 调 `msRegisterModule(L, &strconvModuleDef)` 并返回其结果；挂入任务 18 建立的标准库注册链（`msNewState` 启动流程）。本任务**不做全局名预绑定**（那是任务 20 在 import 缺失期的过渡策略），脚本统一 `import "strconv"`（任务 24）。

### 7. 与内建转换函数的边界划分

- 02-types §3.3 的显式转换 `int("42")`/`float("3.14")`/`str(42)` 是内建函数（任务 10 的地界），本任务不实现、不改动。语义对齐要求：
  - **`str(float)` 的排版必须与 `formatFloat(f, -1)` 一致**：VM 内 float → str 默认转换（任务 06/10 若已以 `%.17g` 之类实现）在本任务重构为调用 `msNumConvFormatFloat(v, -1)`，保证脚本里 `str(0.1) == strconv.formatFloat(0.1)` 且可往返。此重构改变 `str(3.14)` 的输出格式（更短），属有意的行为修正，在测试中锁定。
  - `int(s)`/`float(s)` 的文法是否比 `parseInt(s)`/`parseFloat(s)` 宽松（如允许空白），由内建函数任务定稿，不在本任务约束；本任务只保证 strconv 自身的严格契约。
- `fmt` 模块（任务 19）的 `%d`/`%x`/`%f` 等动词与本模块格式化结果**不要求逐字一致**（fmt 是 printf 风格排版，strconv 是数据转换）；两者不共享代码，避免互相牵制。

### 8. 内存与 GC 纪律清单

- 算法层分配：仅 `struct MsStrBuf` 增长（`msRealloc`，复用任务 20 设施）与 `msNumConvParseFloat` 的 NUL 结尾临时缓冲（`msAlloc`，函数内释放）两处；所有权在函数文档注释写明（缓冲由调用者 `msStrBufFree`）。
- 模块层：参数自动是根（09-c-api §3）；跨分配存活的中间对象仅规范化数字串（`normBuf`，C 侧缓冲不涉 GC）与最终装箱结果——装箱是失败即返回的最后一步，无需入根。
- 所有失败路径先 `msStrBufFree` 再置错误返回 `NULL`，资源按获取逆序释放；`msNumConvScanInt` 失败时 `normBuf` 内容未定义但已初始化，`msStrBufFree` 恒安全。

## 实现步骤

1. 建 `src/object/ms_num_conv.h` / `ms_num_conv.c` 骨架：guard、`struct MsIntScan`、全部函数的空实现；接入构建系统（`mslang` 库目标纳入新源文件）。验证：全平台编译通过。
2. 实现 `msNumConvScanInt`：符号、base==0 前缀检测、下划线规则、数字合法性、规范化视图（含 `normBuf` 消除下划线）。验证：算法层对正/负例矩阵的判定结果（可暂以 `src/` 内临时调试入口或经步骤 7 的模块层脚本断言间接受验）。
3. 实现 `msNumConvFormatInt64`（uint64 幅度、逆序生成、`INT64_MIN` 路径）。验证：各进制已知值（`255` → `"ff"`/`"377"`/`"11111111"`、`INT64_MIN` 十进制）。
4. 实现 `msNumConvParseFloat`：文法校验器 + `strtod` 转换 + `ERANGE` 语义 + `inf`/`nan` 拼写。验证：`.5`/`5.`/`1e` 等文法边界、上溢得 `inf`、下溢得 `0.0`。
5. 实现 `msNumConvFormatFloat`：`prec >= 0` 的 `%.*g` 封装；`prec == -1` 的迭代精度最短往返 + Python-repr 风格排版（定点/科学记法分档、恒含小数点或指数、`-0.0` 符号）。验证：0.1、1/3、1e16、1e-5、`-0.0` 的输出与往返相等性。
6. 实现 `msNumConvQuote` / `msNumConvUnquote`：转义表全项、`\xHH`/`\uHHHH`/`\UHHHHHHHH` 编解码、反引号 raw、单引号拒绝。验证：`unquote(quote(s)) == s` 对含全部转义情形的串成立。
7. 建 `stdlib/ms_std_strconv.{c,h}`：6 个 `MsCFunction` 包装（含 `base`/`prec` 校验、第 5 节的大小整数分派）、方法表、`msStdStrconvRegister`，挂入任务 18 的 stdlib 注册链。验证：脚本 `import "strconv"` 后 `strconv.parseInt("42") == 42`。
8. 对接任务 31：`msNewIntFromString` 实参对齐、`msIntIsBig`/`msBigintFormat` 假定名对齐为实际定名；大整数 parse/format 往返打通。验证：超出 int64 的 30 位十进制串 parse 后算术与 format 回原串。
9. `str(float)` 对齐重构（第 7 节）：VM 默认 float → str 转换改调 `msNumConvFormatFloat(v, -1)`，修正受其影响的既有测试期望。验证：`str(0.1) == "0.1"`、任务 10/19 既有测试保持绿色或按新排版更新期望。
10. 编写 `tests/ms/stdlib/strconv/` 全部测试脚本（见测试方案），`python run_tests.py` 全绿；Win/Linux/macOS × Debug/Release 构建验证，Debug（ASAN / `/RTC`）无内存错误与泄漏。

## 测试方案

本任务编号 ≥ 09，一律用 ms 脚本测试（`tests/ms/stdlib/strconv/`），由仓库根 `run_tests.py` 驱动；任务 40（testing 模块）尚不存在，断言用内建 `assert` + `print`，成功脚本末尾 `print("<用例名> ok")`。异常系统（任务 23）已落地，**负向用例用 `try/except ValueError`/`except TypeError` 在正向脚本内断言**（捕获后校验类型，未捕获即 `assert(false)`），不再拆 `.exit` 负向文件。浮点比较除往返相等性外用 epsilon 容差辅助函数。大整数用例依赖任务 31（本任务编号更靠后，可用）。本任务只交付设计文档，测试实体随实现步骤编写。

测试文件清单与覆盖点：

- `parse_int.ms`：十进制正/负/零/`+` 前缀；显式进制 `parseInt("ff", 16) == 255`、`parseInt("1111", 2)`、`parseInt("z", 36) == 35`；`base=0` 自动检测（`"0x1F"`/`"0o17"`/`"0b101"`/无前缀按十进制）；`base=0` 下划线（`"1_000_000"`、`"0x_1F"`）；大小写混合数字（`"FF"`/`"ff"` 同值）。负向：空串、`"abc"`、数字越界（`parseInt("12", 2)`）、`base != 0` 带前缀（`parseInt("0x1F", 16)`）、`base != 0` 带下划线（`parseInt("1_0", 10)`）、仅符号（`"+"`）、前后空白（`" 12"`、`"12 "`）、base 越界（1、37、-2）抛 ValueError；`parseInt(42)`、`parseInt("1", "10")` 抛 TypeError。
- `parse_int_big.ms`：超出 int64 的十进制/十六进制串（如 `"123456789012345678901234567890"`）解析后与字面量相等、参与算术（`+ 1`、`* 2`）正确；`parseInt("-" + s) == -parseInt(s)`；极大值（百位级数字）不报错（任意精度承诺）。
- `parse_float.ms`：`"3.14"`、`".5" == 0.5`、`"42" == 42.0`、`"1e-9"`、`"-2.5e+4"`、`"inf"`/`"-inf"`/`"nan"`（`nan != nan` 且 `math.isNaN` 为真）；上溢 `parseFloat("1e400")` 得 `inf` 不抛错；下溢 `parseFloat("1e-400")` 得 `0.0`；`-0.0` 解析。负向：`""`、`"abc"`、`"5."`、`"1e"`、`"1_0.5"`（下划线拒绝）、`"0x1.8p1"`（十六进制浮点拒绝）、含空白。注：`math` 模块（任务 21）可作辅助断言，不可用则以纯算术谓词替代（`x != x` 判 NaN、`x > 1e308` 判 inf）。
- `format_int.ms`：各进制（`formatInt(255, 16) == "ff"`、`formatInt(-255, 16) == "-ff"`、`formatInt(8, 2) == "1000"`、`formatInt(35, 36) == "z"`、`formatInt(0, 2) == "0"`）；小写字母；大整数格式化（`formatInt(2 ** 100, 16)` 与已知值比对）；**往返不变式**：对一批 int（含 `INT64_MIN`、大整数、0、负数）与各进制断言 `parseInt(formatInt(i, b), b) == i`；base 越界抛 ValueError；`formatInt(true)`、`formatInt("5")` 抛 TypeError。
- `format_float.ms`：`prec=-1` 最短往返——对 `0.1`、`1.0/3.0`、`1e16`、`1e-5`、`1234567890123456.7`、`-0.0` 断言 `parseFloat(formatFloat(f)) == f`（含符号位：`-0.0` 输出 `"-0.0"`）；排版承诺（`formatFloat(3.0) == "3.0"` 含小数点、`1e-5` 档用科学记法）；`prec >= 0` 有效数字（`formatFloat(3.14159, 3) == "3.14"`）；`inf`/`-inf`/`nan` 输出 `"inf"`/`"-inf"`/`"nan"`；int 实参提升（`formatFloat(3) == "3.0"`）；`prec < -1` 抛 ValueError。
- `quote_unquote.ms`：`quote` 转义表（`"\n"`→`"\"\\n\""`、`"\"`→`\\\"`、`\\`→`\\\\`、控制码 `\x01`→`\\x01`、`0x7F`、可打印多字节码点如 `"é"`/`"中"` 原样保留）；`unquote` 全部转义形式（`\n \t \r \\ \" \' \0 \xHH \uHHHH \UHHHHHHHH`）解码正确、反引号 raw 原样（含 `\n` 两字符不展开）；**往返**：对含全部转义情形的串断言 `unquote(quote(s)) == s`。负向：未闭合引号、非法转义（`\q`）、`\x` 位数不足、`\uD800`（代理区）、单引号形式（`"'a'"`）、无引号裸串——均抛 ValueError。
- `str_parity.ms`：`str(0.1) == strconv.formatFloat(0.1)`、`str(3.0) == "3.0"`、`str(1.0/3.0)` 可经 `parseFloat` 往返——锁定第 7 节的对齐约定。

## 验收标准

- [ ] `stdlib/ms_std_strconv.{c,h}` 与 `src/object/ms_num_conv.{c,h}` 存在；guard 分别为 `MSLANG_STDLIB_MS_STD_STRCONV_H_` 与 `MSLANG_SRC_OBJECT_MS_NUM_CONV_H_`；头文件自包含；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、唯一导出符号 `msStdStrconvRegister`）。
- [ ] 07-stdlib §3 的 6 个函数全部实现并经 `import "strconv"` 可用；语义与「详细设计」第 2 节总表逐条一致（base 范围与 `base=0` 自动检测、下划线仅限 `base=0`、不做空白剥离、bool 拒绝、`prec` 各档位、inf/nan 处理）。
- [ ] `parseInt`/`formatInt` 支持任意精度：小整数走 int64 快路径，大整数经任务 31 接口分派；本任务代码不触碰大整数内部表示；`parseInt(formatInt(i, b), b) == i` 往返不变式对 int64 边界值与大整数成立。
- [ ] `formatFloat(f, -1)` 满足最短往返（`parseFloat` 读回位级相等）且输出恒含小数点或指数；`str(float)` 默认转换与其对齐（`str_parity.ms` 通过）。
- [ ] `parseFloat` 上溢得 `±inf`、下溢得 `±0.0` 而不抛错；语法错误抛 ValueError；`errno` 不跨层传播（调用点立即转换）。
- [ ] `quote`/`unquote` 与 01-lexical §5.3 转义表逐字一致，`unquote(quote(s)) == s` 成立；单引号形式被拒绝。
- [ ] 模块层遵守 GC 根纪律与 `msAsCString` 指针窗口约定；失败路径逆序释放；堆分配全部经 `msAlloc`/`msRealloc`/`msFree`。
- [ ] `tests/ms/stdlib/strconv/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿，`ctest --test-dir build` 并入通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告。
- [ ] 无 TBD/TODO 占位；与任务 31 的接口假定（`msNewIntFromString`、`msIntIsBig`、`msBigintFormat`）及与任务 20 的 `struct MsStrBuf` 复用在实现时已对齐定名。
