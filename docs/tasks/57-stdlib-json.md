# 57 标准库：encoding/json

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [18 C API 基础与嵌入示例](18-c-api-foundation.md) |

## 任务目标

交付 C 实现的标准库模块 `encoding/json`（`stdlib/encoding/ms_json.h` / `stdlib/encoding/ms_json.c`）：一个严格符合 RFC 8259 的 JSON 解析器（`json.loads` / `json.load`）与序列化器（`json.dumps` / `json.dump`），完整实现 `docs/language/07-stdlib.md` §13 的类型映射（object↔dict、array↔list、number↔int/float、true/false/nil 直译、class 实例经 `__json__()` 定制序列化）。解析器带嵌套深度上限与含行/列的错误报告；序列化器支持 `indent` 美化输出、非有限浮点拒绝、非 str 字典键拒绝。模块只经任务 18 落地的公开 C API（`include/mslang/*.h`）与解释器交互，不触碰 VM 内部。完成后脚本侧可直接 `import "encoding/json"` 使用四个函数，行为经 `tests/ms/` 下的 ms 脚本测试（testing 模块）验证。

## 设计依据

- `docs/language/07-stdlib.md`
  - §13 encoding/json：函数集 `dumps(value, indent=0)` / `loads(text)` / `dump(value, writer)` / `load(reader)`；类型映射规则；`__json__()` 定制序列化钩子。
  - §7 io：Reader/Writer 鸭子类型协议（`readAll()` / `write(data)`），`dump`/`load` 的参数只需满足协议，不依赖具体类型。
  - §3 strconv：`formatFloat(f, prec=-1)` 的最短可回读表示语义，浮点序列化对齐此规则。
  - §19 testing：测试文件 `xxx_test.ms`、测试函数 `test` 前缀、`testing.run()` 约定。
- `docs/language/09-c-api.md`：§3 GC 根规则（`msRootPush`/`msRootPop`、返回值在下一次分配前有效）、§5 值构造与类型判断、§6 容器操作、§7 调用与属性（`msCallMethod`/`msHasAttr`）、§8 错误处理（`msRaiseValueError`/`msRaiseTypeError`）、§9 扩展模块注册（`MsMethodDef`/`MsModuleDef`/`msRegisterModule`）。
- `docs/language/10-c-style.md`：§1 include guard、§2 格式化、§3 命名、§4 内部结构体不 typedef、§6 堆分配只经 `msAlloc/msRealloc/msFree`。
- `docs/language/11-project-layout.md`：§1 C 标准库模块位于 `stdlib/`；§4 脚本测试位于 `tests/ms/`。
- RFC 8259：JSON 文法（严格模式——无注释、无尾随逗号、字符串必须双引号、控制字符必须转义、数字不允许前导零）。
- 依赖任务文档（18）尚未撰写，本文引用的 C API 接口名均取自 `docs/language/09-c-api.md`，实现时以对应任务文档定名为准；`mslang` 启动时对 C 内建模块的集中注册机制假定由任务 18/24 提供，本文只交付模块自身的注册入口。

## 详细设计

### 文件与头文件骨架

- 头文件 `stdlib/encoding/ms_json.h`，include guard `MSLANG_STDLIB_ENCODING_MS_JSON_H_`，自包含（include `<mslang/mslang.h>`）。对外只暴露一个注册入口：

```c
// Registers the "encoding/json" builtin module into L (all four functions
// as a MsModuleDef, see 09-c-api §9). Called once during interpreter
// startup by the stdlib bootstrap.
MsResult msJsonRegister(MsState* L);
```

- 实现文件 `stdlib/encoding/ms_json.c`；解析器、序列化器、四个 `MsCFunction` 实现一律文件内 `static`。
- 模块注册表（09-c-api §9 约定，`{NULL, NULL, NULL}` 结尾）：

```c
static const MsMethodDef msJsonMethods[] = {
  {"dumps", msJsonDumps, "dumps(value, indent=0) -> str"},
  {"loads", msJsonLoads, "loads(text) -> value"},
  {"dump",  msJsonDump,  "dump(value, writer)"},
  {"load",  msJsonLoad,  "load(reader) -> value"},
  {NULL, NULL, NULL},
};

static const MsModuleDef msJsonModule = {
  "encoding/json", "JSON encoding and decoding (RFC 8259)", msJsonMethods,
};
```

- 深度与缩进上限：

```c
#define MS_JSON_MAX_DEPTH 100   // 解析与序列化共用的嵌套上限
#define MS_JSON_MAX_INDENT 16   // dumps 的 indent 参数上限（空格数）
```

### 类型映射表

| JSON | loads → MS | dumps ← MS |
|---|---|---|
| object | dict（保持文档键序） | dict（键必须是 str，否则 TypeError） |
| array | list | list / tuple |
| number（无 `.`/`e`/`E`） | int（int64 内直转，溢出经 `msNewIntFromString` 落大整数） | int（含大整数）→ 原样十进制 |
| number（含 `.`/`e`/`E`） | float | float → 最短可回读表示；NaN/±inf 拒绝（ValueError） |
| string | str（UTF-8） | str（转义控制字符，非 ASCII 原样输出 UTF-8） |
| true / false | bool | bool |
| null | nil | nil |
| —（无对应） | — | 其他类型：先查 `__json__()` 钩子，无则 TypeError |

`__json__()` 协议（07-stdlib §13）：`dumps`/`dump` 遇到上表之外的值（典型为 class 实例）时，若 `msHasAttr(L, v, "__json__")` 为真，经 `msCallMethod(L, v, "__json__", 0, NULL)` 取得替代值，再对替代值递归序列化（计入深度）；钩子返回不可序列化值或调用抛错时按原样向上传播。

### 解析器（loads）

递归下降，内部结构体不 typedef：

```c
struct MsJsonParser {
  MsState* L;
  const char* text;    // 输入，指向 argv[0] 字符串内部缓冲，不复制
  size_t len;
  size_t pos;          // 当前字节偏移
  uint32_t line;       // 1 起始
  uint32_t column;     // 1 起始，按字节计
  int depth;           // 当前 object/array 嵌套深度
};

// Core entry: parses one JSON value. Returns NULL with the error state set
// (ValueError) on any syntax/depth error.
static MsObject* msJsonParseValue(struct MsJsonParser* p);
static MsObject* msJsonParseObject(struct MsJsonParser* p);
static MsJsonArrayDoc* msJsonParseArray(struct MsJsonParser* p);  // 实际返回 MsObject*，见下
static MsObject* msJsonParseString(struct MsJsonParser* p);       // 含 \uXXXX 解码与代理对
static MsObject* msJsonParseNumber(struct MsJsonParser* p);
static void msJsonSkipWhitespace(struct MsJsonParser* p);
static MsObject* msJsonFail(struct MsJsonParser* p, const char* fmt, ...);  // 恒返 NULL
```

流程与规则：

1. `msJsonLoads` 校验 `argc == 1` 且 `argv[0]` 为 str（否则 TypeError），取内部缓冲初始化 parser，跳空白后 `msJsonParseValue`，再跳空白；未到 EOF 报「trailing data」。
2. 文法严格按 RFC 8259：值分发看首字符（`{` `[` `"` `t` `f` `n` `-` 或数字），其余首字符报错；`true`/`false`/`null` 逐字匹配，前缀匹配失败报错。
3. object：`{` 后允许空对象；循环 `str 键 → ':' → value → ',' 或 '}'`；尾随逗号、缺冒号、非字符串键均为语法错误。键经 `msJsonParseString` 解码后用 `msDictSet` 写入。
4. array：同构循环，元素经 `msListAppend` 收集。
5. 深度控制：进入 object/array 前 `depth++`，超过 `MS_JSON_MAX_DEPTH` 报「maximum nesting depth exceeded」；返回前 `depth--`。解析是纯递归，此上限同时约束 C 栈。
6. 字符串：逐字节扫描到闭合 `"`；`< 0x20` 的未转义控制字符、非法 UTF-8 序列（长度/续字节/超范围码点校验）均报错；转义支持 `\" \\ \/ \b \f \n \r \t \uXXXX`，`\u` 做代理对配对（高代理必须紧跟 `\uDC00`–`\uDFFF`，否则报错），解码为 UTF-8 后经 `msNewStringN` 构造。
7. 数字：按 RFC 文法扫描 `-?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?`（前导零如 `01` 非法、`.` 前后缺数字非法）；无小数点/指数部分时先按 int64 转换，溢出则把切片交给 `msNewIntFromString(L, text, 10)` 落任意精度整数；含小数点/指数时按 float64（`strtod` 语义，实现内部完成，不经 C 库 locale 敏感路径）。
8. 错误报告：所有解析错误经 `msJsonFail` → `msRaiseValueError(L, "json: line %u column %u: ...", p->line, p->column, ...)`，消息含具体情形（unexpected character / unterminated string / invalid escape / invalid number / trailing data / depth exceeded 等），恒返回 `NULL` 交由 VM 转为脚本异常。
9. GC 纪律：解析过程中的中间对象始终满足 09-c-api §3——新建值在下一次分配前立即挂入父容器（`msDictSet`/`msListAppend`）或作为返回值上交；`msJsonParseObject`/`msJsonParseArray` 在构造期间把半成品容器 `msRootPush` 入根、返回前 `msRootPop`，防止收集中途回收。

### 序列化器（dumps）

输出为可增长字节缓冲，内部结构体：

```c
struct MsJsonBuf {
  char* data;          // msAlloc/msRealloc 管理
  size_t len;
  size_t cap;
};

struct MsJsonWriter {
  MsState* L;
  struct MsJsonBuf buf;
  int64_t indent;      // 0 = 紧凑输出；>0 = 每层缩进的空格数
  int depth;
};

static MsResult msJsonBufPutc(struct MsJsonBuf* b, char c);
static MsResult msJsonBufWrite(struct MsJsonBuf* b, const char* s, size_t n);
static MsResult msJsonWriteValue(struct MsJsonWriter* w, MsObject* v);
static MsResult msJsonWriteString(struct MsJsonWriter* w, const char* s, size_t n);
static MsResult msJsonWriteIndent(struct MsJsonWriter* w);   // 换行 + depth*indent 个空格
```

流程与规则：

1. `msJsonDumps` 校验 `argc` 为 1 或 2；`argv[1]`（indent）须为 int 且 `0 <= indent <= MS_JSON_MAX_INDENT`，否则 TypeError/ValueError。kwargs 形式的 `indent=` 由 VM 调用约定归位为第二位置参数（任务 13 的机制，此处不感知）。
2. `msJsonWriteValue` 按 `msTypeOf` 分派：nil/bool 直写字面量；int 十进制；float 先查有限性（NaN/±inf → ValueError），再按最短可回读表示写入（复用 VM 内部浮点转字符串例程，与 `str()`/`print` 输出一致，实现时以其定名为准）。
3. str：`msJsonWriteString` 输出 `"` 包围体；`"` `\` 及 `< 0x20` 控制字符转义（`\b \f \n \r \t` 优先，其余 `\u00XX`）；`/` 不转义；非 ASCII 字节原样输出（UTF-8 直出，不做 `\uXXXX` 转义，对齐 Go `json.Marshal` 的默认可读性取向）。
4. list/tuple：遍历元素递归写入；dict：先取键列表（经脚本层 `keys()` 方法获得 list，即 `msCallMethod(L, dict, "keys", 0, NULL)`，保持插入序），逐键检查类型为 str（否则 TypeError）后 `msDictGet` 取值递归写入。写入前把当前容器 `msRootPush` 入根，完成后 `msRootPop`。
5. 分隔符：`indent == 0` 时紧凑输出（元素间 `,`、键后 `:`，均无空格）；`indent > 0` 时每个元素/成员独占一行、按 `depth * indent` 空格缩进、键后 `: `；空 list/dict 始终输出 `[]`/`{}` 单行。
6. 深度控制与解析侧一致（共享 `MS_JSON_MAX_DEPTH`），超限报 ValueError；`__json__()` 递归链同样计深，可阻止钩子自引用造成的无限递归。
7. 完成后 `msNewStringN(L, buf.data, buf.len)` 构造返回值，`msFree` 释放缓冲；任何中途错误保证缓冲已释放（单一出口或 goto 清理，10-c-style §5）。
8. 循环引用：除深度上限兜底外不做专门检测（与 Python `json` 的 RecursionError 策略对应，深度上限即本实现的等价物）。

### dump / load（io 协议适配）

- `msJsonLoad`：校验 `argc == 1`，`msCallMethod(L, reader, "readAll", 0, NULL)` 取全文（须为 str，否则 TypeError），随后走与 `msJsonLoads` 完全相同的解析路径（解析器初始化抽成公共 `static` 函数）。reader 缺 `readAll` 方法或调用抛错时错误原样传播。
- `msJsonDump`：校验 `argc == 2`，先按 dumps 路径把 `argv[0]` 序列化为 str，再 `msCallMethod(L, writer, "write", 1, &strObj)` 写出；`write` 的返回值不校验（协议约定返回写入字节数，本模块不消费）。返回 nil。
- 两者都只依赖鸭子类型协议（07-stdlib §7），不链接任务 37 io 模块的任何符号。

## 实现步骤

1. 建 `stdlib/encoding/ms_json.h` / `ms_json.c` 骨架：`MsModuleDef`/`MsMethodDef` 表、`msJsonRegister`、四个 `MsCFunction` 的参数个数/类型校验桩（一律先报 TypeError）。验证：解释器启动注册后 `import "encoding/json"` 成功，`json.dumps` 等四个名字存在。
2. 实现 `struct MsJsonBuf` 增长缓冲（`msAlloc/msRealloc/msFree`，倍增扩容）与序列化分派骨架：nil/bool/int 直写、str 转义表。验证：`dumps(nil)`/`dumps(true)`/`dumps(42)`/`dumps("a\"b\nc")` 的输出断言。
3. list/tuple/dict 序列化 + 紧凑分隔符 + 深度上限 + 容器入根。验证：嵌套结构的紧凑输出、超限报 ValueError、非 str 键报 TypeError。
4. float 序列化：有限性检查、最短可回读表示接线。验证：`1.5`、`1e300`、`0.1` 的输出可回读；NaN/inf 报 ValueError。
5. `indent` 美化输出：换行缩进、`": "` 键分隔、空容器单行。验证：`indent=2`/`indent=4` 的多行输出逐字节断言；`indent=-1`/`indent=17` 报错。
6. `__json__()` 钩子：`msHasAttr` + `msCallMethod` + 替代值递归（计深）。验证：自定义 class 实例序列化、钩子返回非法类型报 TypeError、钩子自引用触发深度上限。
7. 解析器：空白跳过、值分发、`true`/`false`/`null` 字面量、行/列跟踪。验证：标量正例与 `tru`/`nul` 等负例的错误消息含正确行/列。
8. 字符串解析：全转义、`\uXXXX` 与代理对、控制字符拒绝、UTF-8 校验。验证：正例解码字节断言；`\q`、`\uD800` 孤立、裸控制字符、非法 UTF-8 各负例。
9. 数字解析：严格文法（前导零、`1.`、`.5`、`1e` 均拒绝）、int64/大整数/float 三分支。验证：`0`/`-0`/`01`/`9007199254740993`/超大整数/`1e-9` 的类型与值断言。
10. object/array 解析 + 深度上限 + 半成品容器入根 + trailing data 检查。验证：嵌套文档往返（`loads(dumps(x)) == x`）、尾随逗号/缺冒号/空输入/深度超限各负例。
11. `dump`/`load` io 适配。验证：脚本内自拟 Reader/Writer 对象（duck typing）的读写往返；缺 `readAll`/`write` 方法时错误传播。
12. 注册接线与构建集成：`msJsonRegister` 挂入 stdlib 引导；确认产物只落 `build/`。验证：全量脚本测试经 `run_tests.py` 通过。

## 测试方案

本任务晚于任务 40（testing 模块），一律用 ms 脚本测试。测试文件 `tests/ms/stdlib/encoding_json_test.ms`（本任务只交付本设计文档，脚本随实现编写），遵循 07-stdlib §19 约定：测试函数 `test` 前缀、`testing/assert` 断言、末尾 `testing.run()`；由仓库根 `run_tests.py` 驱动。

覆盖清单：

- dumps 标量：`nil`→`null`、bool、int（含负数、int64 边界、大整数原样输出）、float（`1.5`、`0.1` 可回读、`1e300`）、str 全转义表（`\" \\ \b \f \n \r \t`、`\u001f`、`/` 不转义、非 ASCII 原样）。
- dumps 容器：空 list/dict 单行、嵌套结构紧凑输出逐字节断言、list 内 tuple 按 array 输出、dict 保持插入序、非 str 键（int/None 键）抛 TypeError。
- dumps indent：`indent=2`/`4` 的多行格式（缩进量、键后 `": "`、尾随无逗号）、`indent=0` 与默认等价、`indent=-1`/`17`/非 int 类型分别抛 ValueError/TypeError。
- dumps 拒绝项：NaN/inf 抛 ValueError；无 `__json__` 的 class 实例、set、bytes、函数对象抛 TypeError。
- `__json__()`：自定义实例序列化为其返回值、钩子返回值再递归（返回 dict 含实例）、钩子抛异常原样传播、钩子自引用触发深度上限（ValueError）。
- loads 标量：`null`/`true`/`false`、int（`0`、`-0`、int64 边界、超 int64 落大整数且值正确）、float（含指数形式）、字符串全转义与代理对（`"𝄞"` 解码正确）。
- loads 结构：空对象/数组、深层嵌套往返一致（`loads(dumps(x))` 与 `x` 相等）、键序保持。
- loads 负例（均抛 ValueError 且消息含正确 line/column）：空输入、trailing data、尾随逗号、单引号字符串、未闭合字符串/数组/对象、缺冒号、前导零 `01`、`1.`、`.5`、`1e`、非法转义 `\q`、孤立代理 `\uD800`、未转义控制字符、嵌套 101 层超限（100 层合法）。
- dump/load：自拟 duck-typed Writer（收集 `write` 内容）与 Reader（`readAll` 返回文本）往返一致；`dump` 返回 nil；reader 缺 `readAll` 抛错传播。

## 验收标准

- [ ] `stdlib/encoding/ms_json.h` / `ms_json.c` 存在，guard 为 `MSLANG_STDLIB_ENCODING_MS_JSON_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体 `struct MsJsonParser`/`struct MsJsonWriter` 不 typedef）。
- [ ] 模块仅经公开 C API（`include/mslang/*.h`）与解释器交互；堆分配只经 `msAlloc/msRealloc/msFree`；解析/序列化路径上的中间对象满足 GC 根规则（半成品容器 `msRootPush`/`msRootPop`），ASAN/分配统计下无泄漏、无悬垂。
- [ ] `import "encoding/json"` 后 `dumps`/`loads`/`dump`/`load` 四函数可用，类型映射与本文「类型映射表」一致，含 number → int/大整数/float 的三分支。
- [ ] 解析器严格符合 RFC 8259（拒绝注释、尾随逗号、单引号、前导零、未转义控制字符），`\uXXXX` 代理对与 UTF-8 校验完整，嵌套上限 100，所有解析错误为 ValueError 且消息含 1 起始的行/列。
- [ ] 序列化器支持 `indent`（0–16，0 为紧凑）、字符串转义表、非 ASCII UTF-8 直出、NaN/inf 拒绝、非 str 字典键拒绝、共享深度上限 100、`__json__()` 钩子（含计深防自引用）。
- [ ] `dump`/`load` 仅依赖 io 鸭子类型协议（`write`/`readAll`），不链接 io 模块符号。
- [ ] `tests/ms/stdlib/encoding_json_test.ms` 覆盖「测试方案」全部清单项并全部通过；`run_tests.py` 汇总无失败；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 18/24 的接口假定（`msRegisterModule`、内建模块启动注册机制）在实现时已对齐对应任务文档。
