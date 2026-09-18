# 19 标准库：fmt

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [18 C API 基础](18-c-api-foundation.md) |

## 任务目标

交付 C 内建标准库模块 `fmt`（`stdlib/fmt/ms_mod_fmt.h` / `stdlib/fmt/ms_mod_fmt.c`），实现 `docs/language/07-stdlib.md` §1 的三个函数：`fmt.sprintf(format, ...)`（返回格式化字符串）、`fmt.printf(format, ...)`（写 stdout）、`fmt.fprintf(writer, format, ...)`（写鸭子类型 Writer）。格式化微语言为 Go 风格动词（`%v %s %d %f %x %o %b %q %t %p %%`，含宽度/精度/旗标修饰）。

本任务同时交付两件超出 fmt 本身的基础设施：

1. **v0.1 标准库注册机制**：`msRegisterModule`（09-c-api §9 的静态注册子集）、模块对象与内建注册表、`msNewState` 初始化末尾的标准库注册链 `msStdlibRegisterAll`，以及 import 落地（任务 24）前的过渡挂接——模块对象以全局名预绑定，脚本直接 `fmt.sprintf(...)` 调用。任务 20（strings）、21（math）按同一模式接入。
2. **共享格式核心**（`src/object/ms_format.{c,h}`）：中间表示 `struct MsFormatSpec` 与渲染引擎 `msFormatValue`，外加 Go 动词前端 `msFormatGo`。任务 28（f-string）在其上叠加 Python 风格说明前端，两套语法共用一份引擎（"两套格式语法，一份引擎"），本任务是该引擎的建立方。

完成后，需要宽度、精度、进制、对齐控制的输出在脚本中可用；常规输出仍走内建 `print`（分工见 `docs/language/03-syntax.md` §9.1）。本任务经 `tests/ms/stdlib/fmt/` 下的 ms 脚本测试验证。

## 设计依据

- `docs/language/07-stdlib.md` §0（`fmt` 为 C 内建模块、置于 `stdlib/` 目录）、§1（三个函数签名、Go 风格动词清单、"不提供 `fmt.print`/`fmt.println`"、`fmt` 与内建 `print` 及 f-string 的职责分工）。动词清单是 v0.1 范围的封闭集。
- `docs/language/03-syntax.md` §9.1：常规输出一律用内建 `print`；需要格式控制时才用 `fmt`；简单插值优先 f-string。本任务不实现任何 `fmt.print*` 变体。
- `docs/language/05-modules.md` §2/§7：内建 C 模块经内建注册表解析、优先级高于搜索路径。import 语句由任务 24 实现；v0.1 期间模块对象经全局命名空间暴露（与任务 20 同一过渡策略）。
- `docs/language/02-types.md` §4（str 不可变 UTF-8、一切宽度/索引按 **Unicode 码点**口径——格式化的宽度与字符串精度同此口径）、§8（魔术方法封闭集：有 `__str__`/`__repr__`、**无 `__format__`**，自定义实例的格式化先经 `__str__`、缺省回落 `__repr__`，再按字符串口径应用修饰）。
- `docs/language/09-c-api.md` §3（GC 根栈纪律）、§5（`msNewStringN`/`msAsCString`/`msStringLen` 等）、§8（C 函数出错置错误并返回 `NULL`；v0.1 异常系统未落地，格式错误以运行时错误呈现、进程退出码 1）、§9（`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule` 与 `mslangInit_<name>` 约定）。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- `docs/language/11-project-layout.md` §1（`stdlib/` 为 C 标准库模块目录、`src/object/` 为类型实现目录、`include/mslang/` 为公开头文件目录）、§4（三层测试策略中的脚本测试层）。
- `docs/tasks/README.md` 测试约定：本任务编号 ≥ 09 一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`，负向用例以 `<name>.exit` 同伴文件声明预期退出码（约定见 [09-minimal-interpreter.md](09-minimal-interpreter.md) 测试方案），stdout 全文比对用 `<name>.out` 同伴文件，由仓库根 `run_tests.py` 驱动。
- [18 C API 基础](18-c-api-foundation.md)：`MsState`/`MsObject` 不透明句柄、`MsResult` 枚举、`msRaiseTypeError`/`msRaiseValueError`、`msSetGlobal`、根栈与错误槽约定，以及 `src/api/` 适配层与 `src/vm/ms_state_internal.h` 内部状态组织的既有格局。
- [20 标准库：strings](20-stdlib-strings.md)：`struct MsStrBuf` 缓冲四件套（`msStrBufInit`/`msStrBufFree`/`msStrBufPut`/`msStrBufPutRune`，`src/object/ms_str_op.h`）的定名、"注册入口 + 全局预绑定"的模块挂接模式（该文档声明此模式由本任务先行建立）。
- [28 f-string](28-fstring.md) §2：共享格式核心的既定约定——`struct MsFormatSpec` 的字段集与 `msFormatValue` 的签名、引擎内部流程（值 → 基本文本 → 符号/前缀/基数修饰 → 精度应用 → 宽度与对齐填充）、宽度按码点计、`=` 对齐在符号后填充。本任务按该假定逐字段落地，任务 28 以此为准对齐。

规范歧义的处理（实现与评审时以此为据）：

1. **module.h 提前落地。** 任务 18 文档把 `module.h`/`ctype.h` 划入任务 33（v0.2），但 fmt/strings/math 属 v0.1 且 05-modules §7 要求 C 模块经注册表解析，09-c-api §9 的注册结构无从等待。本任务把 §9 的**静态注册子集**（`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule`）提前落地为 `include/mslang/module.h`；`ctype.h`（`msDefineType`）与动态加载约定仍属任务 33，任务 33 实现时该子集已存在、只需叠加。
2. **Go 前端的缺省对齐。** Go 对一切类型在指定宽度时缺省**右对齐**；共享引擎的缺省（任务 28 约定）是"字符串左、数值右"。差异由 Go 前端在解析期消化：宽度存在且无 `-` 旗标时显式写入 `align = '>'`，引擎语义不变。
3. **参数个数不匹配。** Go 以 `%!verb(MISSING)` 等带内噪音报告，ms 采用显式错误：实参不足或多余一律 ValueError（Python 式动态语义，错误不入输出流）。
4. **`fmt.fprintf` 的 Writer。** io 模块（任务 37，v0.2）才定稿 Reader/Writer 协议；`fmt.fprintf` 属本任务范围（07-stdlib §1 列出）。v0.1 按 07-stdlib §7 的草案协议做鸭子类型调用：要求 `writer` 有可调用的 `write(str)` 方法且返回 int；协议定稿若有出入以任务 37 为准回归。
5. **动词集封闭。** v0.1 只接受 07-stdlib §1 列出的动词（`v s d f x o b q t p %`）。引擎 IR 的 `type` 字段另有 `X e E g G F`（f-string 前端需要，任务 28），但 Go 前端遇到未列动词（含 `%X`、`%g`）一律 ValueError，待语言文档扩充清单再放开。
6. **`s.format(...)` 不在本任务。** 02-types §4 的内建 str 方法清单含 `s.format(...)`，但语言文档未定义其语义（Python 风格 `{}` 字段还是 Go 风格动词未定），且 Python 风格说明前端属任务 28。本任务不实现它，内建方法侧如何呈现（缺席或置未实现错误）以任务 06/28 定稿为准。
7. **int 的精度修饰。** Go 允许 `%.3d`；共享引擎（任务 28 约定）的 precision 语义只有"浮点小数位 / 字符串最大码点数"两种，整数动词遇精度修饰按引擎权威报 ValueError。

假定命名（实现时以对应任务文档定名为准）：任务 06 的 UTF-8 码点原语（`msUtf8EncodeRune` 等）、C 函数对象构造器（假定 `msNewCFunction`）、对象模型的属性查找分派入口（假定 `msObjectGetAttr`）；任务 10 的 `str()` 内部入口（假定 `msObjectStr`）；任务 17 的 GC 对象标记分派挂接点。

## 详细设计

### 1. 文件布局与职责

```
include/mslang/
└── module.h           # 09-c-api §9 静态注册子集（guard MSLANG_INCLUDE_MSLANG_MODULE_H_）
src/api/
└── ms_module.c        # msRegisterModule 实现（内建注册表写入）
src/object/
├── ms_module.h        # struct MsModule 模块对象（guard MSLANG_SRC_OBJECT_MS_MODULE_H_）
├── ms_module.c        # 模块对象构造/查找、GC trace 挂接
├── ms_str_op.h        # struct MsStrBuf 四件套骨架（guard MSLANG_SRC_OBJECT_MS_STR_OP_H_）
├── ms_str_op.c        # 缓冲实现；任务 20 在同文件追加字符串算法例程
├── ms_format.h        # 共享格式核心（guard MSLANG_SRC_OBJECT_MS_FORMAT_H_）
└── ms_format.c        # 渲染引擎 msFormatValue + Go 动词前端 msFormatGo
stdlib/
├── ms_stdlib.h        # 标准库注册链入口（guard MSLANG_STDLIB_MS_STDLIB_H_）
├── ms_stdlib.c        # msStdlibRegisterAll：依序调用各模块注册函数
└── fmt/
    ├── ms_mod_fmt.h   # msFmtRegister / mslangInit_fmt（guard MSLANG_STDLIB_FMT_MS_MOD_FMT_H_）
    └── ms_mod_fmt.c   # 3 个 MsCFunction 包装 + MsModuleDef 注册表
```

分层职责：`src/object/ms_format.*` 不感知调用来源（fmt 模块与任务 28 的 VM 指令都是其调用方）；`stdlib/fmt/` 只做参数校验、装箱与输出落点；注册机制（`module.h`、`src/api/ms_module.c`、`src/object/ms_module.*`、`stdlib/ms_stdlib.*`）为全部 v0.1 C 标准库模块共用。

`struct MsStrBuf` 四件套的归属说明：任务 20 文档将 `ms_str_op.*` 描述为整体交付，但任务编号 19 < 20 不允许本任务依赖任务 20 的产物。本任务先建 `ms_str_op.{c,h}` 骨架（仅缓冲四件套），定名与任务 20 文档逐字一致，任务 20 实现时在同文件追加算法例程、无需改动骨架。

### 2. v0.1 标准库注册机制

**module.h（公开头文件，自包含）：**

```c
typedef MsObject* (*MsCFunction)(MsState* L, int64_t argc, MsObject** argv);

typedef struct {
  const char* name;     // "sprintf"
  MsCFunction func;
  const char* doc;      // docstring, may be NULL
} MsMethodDef;

typedef struct {
  const char*        name;     // module name: "fmt"
  const char*        doc;
  const MsMethodDef* methods;  // array terminated by {NULL, NULL, NULL}
} MsModuleDef;

// Registers def into the interpreter's builtin module registry; the module
// object is reachable via the registry (an internal GC root). Fails with
// MS_ERROR_RUNTIME on duplicate names, MS_ERROR_OOM on allocation failure.
MS_API MsResult msRegisterModule(MsState* L, const MsModuleDef* def);
```

`MsModuleDef`/`MsMethodDef` 是公开 POD（typedef 允许，同 `MsConfig` 先例）；动态加载的 `mslangInit_<name>` 只是命名约定，本任务的 `mslangInit_fmt` 顺带满足它，共享库加载机制本身属任务 33。

**模块对象（`src/object/ms_module.h`，内部结构体不 typedef）：**

```c
struct MsModule {           // GC object, tag MS_TYPE_MODULE
  // object header per task 06 layout
  MsObject* name;           // str, e.g. "fmt"
  MsObject* attrs;          // dict: name -> C function object / str
};

// Creates a module object from def: wraps each method into a C function
// object, sets __name__/__doc__ in attrs. Assumed constructor msNewCFunction
// (task 06/13) performs the wrapping.
MsObject* msModuleNew(MsState* L, const MsModuleDef* def);

// Looks up a registered builtin module by name; NULL when absent.
MsObject* msModuleFind(MsState* L, const char* name);
```

- `MsTypeTag` 尾部追加 `MS_TYPE_MODULE`（任务 18 的枚举追加规则允许按任务扩展）。
- 属性查找：对象模型的属性分派（假定入口 `msObjectGetAttr`，VM 的属性访问与公开 `msGetAttr` 同经此路）补 `MS_TYPE_MODULE` 分支——读：查 `attrs` 字典，缺失置 AttributeError；写：一律置 AttributeError（v0.1 模块命名空间只读，可写性由任务 24 定稿模块语义时评估）。
- GC：模块对象含两个 `MsObject*` 字段，任务 17 的标记分派补 `MS_TYPE_MODULE` 分支（挂接点定名以任务 17 为准）。
- 内建注册表：`MsState` 内部结构（`src/vm/ms_state_internal.h`）新增 `MsObject* builtinModules; // dict: name -> module`，作为 `MsState` 内部根随全局命名空间一并标记；`msNewState` 初始化为空字典，`msCloseState` 无需特判（GC 托管）。

**注册链（`stdlib/ms_stdlib.h`）：**

```c
// Registers all v0.1 C stdlib modules and pre-binds each module object under
// its global name (transitional until import lands in task 24).
// Called at the end of msNewState's initialization sequence.
MsResult msStdlibRegisterAll(MsState* L);
```

- `msNewState`（`src/api/ms_state.c`）初始化序列末尾调用；失败时关闭状态并返回 `NULL`。
- v0.1 链内只有 `msFmtRegister`；任务 20/21 追加 `msStringsRegister`/`msMathRegister`，顺序即注册表查询的确定性保证。
- 各模块注册函数的统一形态（任务 20/21 复用同一模式）：

```c
// stdlib/fmt/ms_mod_fmt.h
const MsModuleDef* mslangInit_fmt(void);   // 09-c-api §9 naming convention
// Registers the module and binds the module object as global "fmt".
MsResult msFmtRegister(MsState* L);
```

  `msFmtRegister` = `msRegisterModule(L, mslangInit_fmt())` + `msSetGlobal(L, "fmt", msModuleFind(L, "fmt"))`。任务 24 落地 import 后是否保留全局预绑定，以任务 24 文档定稿为准。

### 3. 共享格式核心：中间表示与渲染引擎

`src/object/ms_format.h`，按任务 28 §2 的既定约定落地（字段集与签名逐字一致，任务 28 以本文件为准）：

```c
struct MsFormatSpec {      // format-request intermediate form shared by fmt and f-string
  uint32_t fill;           // pad rune, ' ' when unset (multi-byte fill allowed)
  char align;              // '<' | '>' | '^' | '=' | 0 (0 = default: left for str, right for numbers)
  char sign;               // '+' | '-' | ' ' | 0 (0 = negative only)
  bool alternate;          // '#': 0x/0o/0b prefix, forced decimal point for floats
  bool zeroPad;            // '0': zero padding for numerics (implies '='-style sign-aware padding)
  int64_t width;           // total field width in runes; -1 = unset
  int64_t precision;       // digits after point (floats) / max runes (strings); -1 = unset
  char type;               // 's' 'd' 'b' 'o' 'x' 'X' 'e' 'E' 'f' 'F' 'g' 'G' '%' | 0 (0 = default)
};

// Initializes *spec to the all-default request (fill ' ', everything unset).
void msFormatSpecInit(struct MsFormatSpec* spec);

// Formats value per spec, appending UTF-8 bytes to out. Numeric verbs require
// MS_TYPE_INT/BOOL (bool counts as 0/1) or MS_TYPE_FLOAT per the matrix in §5;
// violations raise ValueError. Returns MS_OK, or MS_ERROR_* with the script
// error already set on L. out is caller-owned (msStrBufInit/msStrBufFree).
MsResult msFormatValue(MsState* L, MsObject* value, const struct MsFormatSpec* spec, struct MsStrBuf* out);
```

引擎内部流程（单值，五段管线，与任务 28 §2 约定一致）：

1. **基本文本**：按 `type` 与值类型求未修饰文本——`d`/`b`/`o`/`x` 转对应进制数字串；`f` 转定精度浮点文本；`type == 0`（`%v`/无类型）经 `str()` 协议（假定入口 `msObjectStr`：实例走 `__str__`，缺省回落 `__repr__`）得文本。
2. **符号/前缀/基数修饰**：负数取 `-`；`sign` 为非负值加 `+`/空格；`alternate` 对 `x`/`o`/`b` 加 `0x`/`0o`/`0b` 前缀。
3. **精度应用**：浮点在第 1 步内完成（`f` 缺省 6 位）；字符串按码点截断到 `precision`。
4. **宽度与对齐填充**：宽度按**码点**计（经任务 06 码点原语计数），不足时按 `align` 以 `fill` 码点经 `msStrBufPutRune` 填充；`=` 对齐在符号之后填充；`zeroPad` 对数值等价 `fill='0'` + 符号感知填充，对非数值等价 `fill='0'` + 右对齐；`zeroPad` 与显式 `align='<'` 同现时 `zeroPad` 失效（与 Go 一致）。
5. 引擎不感知来源：fmt 的 `msFormatGo` 与任务 28 的 `MS_OP_FORMAT_VALUE` 都是调用方；`fill`/`^`/`=` 与 `X e E g G F` 类型字母为任务 28 预留，本任务的 Go 前端不产生这些取值，但引擎一次实现到位。

引擎内部跨分配存活的局部 `MsObject*`（如 `str()` 协议产物）遵守 `msRootPush`/`msRootPop` 纪律（09-c-api §3）。

### 4. Go 动词前端（msFormatGo）

```c
// Walks a Go-style format string [fmt, fmt+fmtLen): literal bytes pass
// through, each %verb consumes one argument from args[0..argc) in order and
// is rendered via msFormatValue. Raises ValueError on unknown verbs, bad
// modifiers, or argument-count mismatch. out is caller-owned.
MsResult msFormatGo(MsState* L, const char* fmt, size_t fmtLen,
    int64_t argc, MsObject** args, struct MsStrBuf* out);
```

指令语法（`%` 后依序，全部可选段空缺即缺省）：

```
directive = "%" [ flags ] [ width ] [ "." precision ] verb
flags     = { "-" | "+" | " " | "#" | "0" }   // 重复幂等
width     = digit+
precision = digit+                            // "." 后无数字按 0 计（对齐 Go）
verb      = "v" | "s" | "d" | "f" | "x" | "o" | "b" | "q" | "t" | "p" | "%"
```

扫描算法（单遍，`ms_format.c` 内 `static` 辅助）：

1. 遇到非 `%` 字节：把到下一个 `%`（或串尾）的字节段原样 `msStrBufPut`（格式串是合法 UTF-8，无需逐码点处理）。
2. `%%`：`%` 后紧跟 `%`，输出一个字面 `%`，**不消耗实参**；`%%` 不接受任何修饰（`%5%` 等形式报 ValueError）。
3. 其余 `%`：解析 flags → width → precision → verb；verb 不在上表报 ValueError；随后消耗一个实参。
4. 全程记录消耗数：扫描结束时消耗数 < argc 报 ValueError（实参多余）；解析中实参耗尽报 ValueError（实参不足）。
5. width/precision 只收无符号十进制，解析上限 `INT32_MAX`（防御病态输入，与任务 28 的 Python 前端同约定）。

旗标到 `MsFormatSpec` 的映射（唯一权威）：

| Go 元素 | 映射 |
|---|---|
| `-` | `align = '<'` |
| 宽度存在且无 `-` | `align = '>'`（Go 全类型缺省右对齐，见「设计依据」歧义 2） |
| `+` / ` ` | `sign = '+'` / `' '` |
| `#` | `alternate = true` |
| `0` | `zeroPad = true` |
| width / precision | 同名字段 |
| `v` | `type = 0`（str() 协议） |
| `s` `d` `f` `x` `o` `b` | `type` 同名字母 |

`%t`/`%q`/`%p` 不在 IR 的 `type` 字母集内，由前端脱糖为文本 + 字符串规则修饰：

- `%t`：实参须为 bool（否则 ValueError），产出 `"true"`/`"false"` 文本后以 `type = 's'` 过引擎应用宽度/对齐。
- `%q`：实参须为 str（否则 ValueError），先经 `static` 引用例程加双引号并转义——`\\` `\"` `\n` `\t` `\r` 用具名转义，码点 < 0x20 与 0x7F 用 `\xHH`，其余（含非 ASCII）原样；引用结果装箱为临时 str（`msRootPush` 保护）后以 `type = 's'` 过引擎。与 Go 的差异：Go 对不可打印 Unicode 用 `\uHHHH`，v0.1 从简只处理控制字符，差异记录于此。
- `%p`：任意实参，产出 `0x` + 对象指针的十六进制（`uintptr_t`，小写）文本后以 `type = 's'` 过引擎。小整数/短字符串驻留是实现细节（02-types §6），`%p` 只承诺同一对象同次运行内输出稳定，不承诺跨运行稳定。

### 5. 类型 × 动词矩阵（fmt 权威表）

与任务 28 §7 同源同文（同一份引擎，两侧文档必须逐格一致；未列组合抛 ValueError）：

| 值类型 | `%v` / `%s` | `%d` `%b` `%o` `%x` | `%f` | `%t` | `%q` | `%p` |
|---|---|---|---|---|---|---|
| int | `str()` 后按字符串规则 | 按对应进制 | 转 float64 后按浮点规则 | ValueError | ValueError | 地址文本 |
| bool | `"true"`/`"false"` | 按 1/0 数值处理 | 按 1.0/0.0 处理 | `"true"`/`"false"` | ValueError | 地址文本 |
| float | `str()` 后按字符串规则 | ValueError | 定精度浮点（缺省 6 位） | ValueError | ValueError | 地址文本 |
| str | 原串 | ValueError | ValueError | ValueError | 引用串 | 地址文本 |
| nil | `"nil"` | ValueError | ValueError | ValueError | ValueError | 地址文本 |
| 其他（list/dict/实例/…） | `str()` 协议（实例走 `__str__`，缺省 `__repr__`）后按字符串规则 | ValueError | ValueError | ValueError | ValueError | 地址文本 |

补充口径：字符串规则下 `precision` 按码点截断、`width`/`fill`/`align` 按码点填充（缺省对齐随「设计依据」歧义 2 的前端映射）；数值规则下 `sign`/`#`/`zeroPad` 对非数值报 ValueError；`#` 对 `x`/`o`/`b` 加 `0x`/`0o`/`0b` 前缀；无类型 float 经 `str()` 得最短往返文本。`f"{x}" == str(x)` 与 `fmt.sprintf("%v", x) == str(x)` 同为可测不变量。

### 6. 数值转文本要点

- **整数**：v0.1 int 即 int64（大整数属任务 31）。进制转换集中在一个 `static` 例程（除法取余逆序产出数字，`b`/`o`/`x` 无符号化按位口径——负数的 `%x` 按 Go 口径先取负号再转绝对值数字，即 `-ff` 而非补码）；任务 31 落地大整数时泛化该例程入口，行为不变。
- **浮点定精度（`%f`）**：经 C 库 `snprintf("%.*f")` 生成候选文本。locale 不变量：解释器全进程不调用 `setlocale`，默认 C locale 下小数点恒为 `.`——该不变量写入 `ms_format.c` 头注释，调试构建对候选文本 `MS_ASSERT` 不含 locale 相关字符。
- **最短往返**：`%v`/无类型 float 不自行实现 dtoa，直接经 `str()` 协议（任务 10 已按 07-stdlib §3 的最短往返口径实现 `str(float)`）；`%f` 显式精度不经过最短往返。
- 宽度/精度解析上限 `INT32_MAX`（§4）；渲染产物长度不受额外钳制，病态宽度（如 `%1000000000d`）以 OOM 错误兜底。

### 7. 模块封装层（stdlib/fmt/ms_mod_fmt.c）

三个 `static MsObject* fmtXxx(MsState* L, int64_t argc, MsObject** argv)` 包装，统一骨架：校验 `argc >= 1`、`argv[0]` 为 str（否则 TypeError）→ `msStrBufInit` → `msFormatGo(L, fmt, fmtLen, argc - 1, argv + 1, &buf)`（失败则 `msStrBufFree` 并返回 `NULL`，错误已置）→ 按落点分派 → `msStrBufFree` 返回。落点分派：

- `sprintf`：`msNewStringN(L, buf.data, buf.len)` 装箱返回 str。
- `printf`：`fwrite(buf.data, 1, buf.len, stdout)`，返回写入字节数 int（对齐 Go 的 `n` 返回值；不追加换行、不主动 flush，与内建 `print` 默认行为一致——输出语义差异是 fmt 与 print 分工的一部分）。
- `fprintf`：`argv[0]` 为 writer（格式串相应后移为 `argv[1]`，实参从 `argv + 2` 起）。鸭子类型校验：`msHasAttr(L, writer, "write")` 为假置 TypeError；将完整结果装箱为 str（`msRootPush` 保护）后 `msCallMethod(L, writer, "write", 1, &strObj)`，返回值须为 int（否则 TypeError），原样作为 `fprintf` 的返回字节数。调用失败原样传播（返回 `NULL`）。

模块表与注册：

```c
static const MsMethodDef fmtMethods[] = {
  {"printf", fmtPrintf, "printf(format, ...) -> int"},
  {"sprintf", fmtSprintf, "sprintf(format, ...) -> str"},
  {"fprintf", fmtFprintf, "fprintf(writer, format, ...) -> int"},
  {NULL, NULL, NULL},
};

static const MsModuleDef fmtModuleDef = {
  "fmt", "formatted output (printf/sprintf/fprintf)", fmtMethods,
};
```

### 8. 内存与 GC 纪律

- 引擎与前端的堆分配仅 `MsStrBuf` 增长（`msRealloc`）一处；缓冲所有权归调用方（`msStrBufInit`/`msStrBufFree` 配对），失败路径先释放缓冲再返回。
- 跨分配存活的局部 `MsObject*`：`%q`/`%p` 的临时文本对象、`fprintf` 的待写 str、`str()` 协议产物——一律 `msRootPush`/`msRootPop` 严格 LIFO 配对（09-c-api §3）；包装层参数自动是根。
- `msAsCString` 取格式串指针后、到 `msFormatGo` 消费完之间不调用其他 C API（指针有效期纪律，同任务 20 的包装层约定）。
- 注册期对象（模块、C 函数、注册表字典）创建后立即入注册表/全局命名空间成为内部根，无悬挂窗口。

## 实现步骤

1. 落地 `include/mslang/module.h` 与 `src/api/ms_module.c`：`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule`（重复名报 `MS_ERROR_RUNTIME`）；`MsState` 内部新增 `builtinModules` 字典并在 `msNewState`/`msCloseState` 接线、登记为内部根。验证：任务 18 既有测试（含 embed-demo）保持绿色。
2. 实现 `src/object/ms_module.{c,h}`：`struct MsModule`、`msModuleNew`（方法包装 + `__name__`/`__doc__`）、`msModuleFind`；`MsTypeTag` 尾部追加 `MS_TYPE_MODULE`；属性分派补模块分支（读查字典、写置 AttributeError）；GC 标记分派补模块分支。验证：C 侧注册一个临时 dummy 模块后可经 `msGetAttr` 取到其函数对象（embed 层抽查）。
3. 建 `src/object/ms_str_op.{c,h}` 骨架：`struct MsStrBuf` 与四件套（定名与任务 20 文档逐字一致）。验证：被 `ms_format.c` 链接调用。
4. 实现 `src/object/ms_format.{c,h}` 渲染引擎：`msFormatSpecInit`、`msFormatValue` 五段管线、§5 矩阵全格、码点宽度/截断、符号感知零填充。验证：经步骤 6 后的 sprintf 脚本间接全覆盖（引擎无独立脚本入口，不违反「任务 09 起一律 ms 脚本测试」约定）。
5. 实现 Go 前端 `msFormatGo`：单遍扫描、旗标映射（含缺省右对齐消化）、`%%`、参数计数错误、`%t`/`%q`/`%p` 脱糖。验证：同上。
6. 建 `stdlib/ms_stdlib.{c,h}` 与 `stdlib/fmt/ms_mod_fmt.{c,h}`：三个包装、`fmtModuleDef`、`mslangInit_fmt`、`msFmtRegister`；`msStdlibRegisterAll` 接入 `msNewState` 末尾并全局预绑定 `"fmt"`。验证：脚本 `print(fmt.sprintf("[%08.2f]", 3.14159))` 输出 `[00003.14]`。
7. 编写 `tests/ms/stdlib/fmt/` 全部测试脚本与同伴文件（见测试方案），`python run_tests.py` 全绿。
8. Win/Linux/macOS × Debug/Release 构建验证；Debug（ASAN / `/RTC`）下跑全部 fmt 与回归测试无内存错误、无泄漏（含 `%q` 临时对象与 `fprintf` 路径的 GC 根纪律、注册期对象在 `msCloseState` 后分配计数归零）。

## 测试方案

本任务只交付本设计文档；测试脚本随实现编写。一律使用 ms 脚本测试（`tests/ms/stdlib/fmt/`，内建 `assert` + `print`——任务 40 之前不用 testing 模块），成功脚本末尾 `print("<用例名> ok")`；负向用例配 `<name>.exit`（内容为预期退出码 `1`）；stdout 全文比对配 `<name>.out`；由仓库根 `run_tests.py` 驱动并并入 `ctest --test-dir build`。

测试文件清单与覆盖点：

- `sprintf_verbs.ms`：动词全表正例——`%v` 对 int/float/bool/nil/str/list/dict 逐类型（含 `fmt.sprintf("%v", x) == str(x)` 不变量）；`%s`；`%d` 正负零；`%x`/`%o`/`%b`（含负数按 `-ff` 口径）；`%f` 缺省 6 位与 `%.2f`；`%t`；`%q` 的转义（`\n`、引号、反斜杠、控制字符 `\xHH`、非 ASCII 原样）；`%%` 不消耗实参；`%p` 断言 `"0x"` 前缀与同对象两次输出相等。
- `sprintf_flags.ms`：宽度与对齐（`%8d`、`%-8s`、字符串缺省右对齐——歧义 2 的锁定用例）；`%08d` 与符号位置（`-42` → `-0000042`）；`%+d`/`% d`；`%#x`/`%#o`/`%#b` 前缀；字符串精度按码点截断（多字节串 `%.2s`）；宽度按码点（多字节串填充到 `%5s`）；`%.f`（精度 0）；`%0` 与 `-` 同现时零填充失效。
- `matrix.ms`：§5 矩阵正例格——int 走 `%f`、bool 走 `%d`（`1`/`0`）与 `%f`、nil 的 `%v` 为 `"nil"`、list/dict 的 `%v` 与 `str()` 相等；定义带 `__str__` 的 class 实例验证实例委托（缺省回落 `__repr__` 另测）；`__str__` 结果再按宽度/精度修饰。
- `printf.ms` + `printf.out`：stdout 全文比对——`fmt.printf` 不追加换行、与 `print` 混用时输出交错顺序正确、返回值为写入字节数。
- `fprintf.ms`：脚本定义带 `write(s)` 方法的 class 作鸭子类型 writer（累积到实例字段），`fmt.fprintf(w, "%d-%s", 1, "a")` 后断言累积串与返回字节数；`write` 被多次调用语义（本实现单次写完整串，断言调用次数为 1 锁定行为）。
- `errors_format_*.ms`（各配 `.exit`，按首触发点逐文件拆分）：未知名词 `%z`、未开放动词 `%g`/`%X`；`%%` 带修饰；width/precision 超 `INT32_MAX`；实参不足、实参多余。
- `errors_type_*.ms`（各配 `.exit`）：格式串非 str（TypeError）；`%d` 对 str、`%f` 对 str、`%t` 对 int、`%q` 对 int、int 动词带精度（歧义 7）；`fprintf` 的 writer 无 `write` 方法、`write` 返回非 int。

## 验收标准

- [ ] `include/mslang/module.h`、`src/api/ms_module.c`、`src/object/ms_module.{c,h}`、`src/object/ms_str_op.{c,h}` 骨架、`src/object/ms_format.{c,h}`、`stdlib/ms_stdlib.{c,h}`、`stdlib/fmt/ms_mod_fmt.{c,h}` 存在；guard 分别为 `MSLANG_INCLUDE_MSLANG_MODULE_H_`、`MSLANG_SRC_OBJECT_MS_MODULE_H_`、`MSLANG_SRC_OBJECT_MS_STR_OP_H_`、`MSLANG_SRC_OBJECT_MS_FORMAT_H_`、`MSLANG_STDLIB_MS_STDLIB_H_`、`MSLANG_STDLIB_FMT_MS_MOD_FMT_H_`；头文件自包含，代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef），堆分配只经 `msAlloc`/`msRealloc`/`msFree`。
- [ ] `msRegisterModule`/`MsModuleDef`/`MsMethodDef` 按 09-c-api §9 静态子集落地；`struct MsModule` 为 `MS_TYPE_MODULE` GC 对象（属性读查字典、写置 AttributeError、GC 标记覆盖）；`msStdlibRegisterAll` 接入 `msNewState` 末尾，脚本可不经 import 直接 `fmt.sprintf(...)`。
- [ ] `struct MsFormatSpec` 与 `msFormatValue` 的字段集、签名、五段管线与 [28 f-string](28-fstring.md) §2 的既定约定逐字一致（fill/align/sign/alternate/zeroPad/width/precision/type 全字段，宽度按码点、`=` 符号后填充）；`struct MsStrBuf` 四件套定名与 [20 标准库：strings](20-stdlib-strings.md) 一致。
- [ ] Go 前端实现 §4 语法全表：旗标映射（含 Go 缺省右对齐的前端消化）、`%%` 不消耗实参且拒绝修饰、参数不足/多余报 ValueError、`%t`/`%q`/`%p` 脱糖、width/precision 上限 `INT32_MAX`；未列动词（含 `%g`/`%X`）报 ValueError。
- [ ] §5 类型 × 动词矩阵逐格实现：`%v` 走 `str()` 协议（实例 `__str__` 缺省 `__repr__`，不引入 `__format__`）、bool 按 0/1、`#` 进制前缀、负数 `%x` 按 `-ff` 口径、`%f` 缺省 6 位、字符串精度与宽度按码点；`fmt.sprintf("%v", x) == str(x)` 不变量成立。
- [ ] 三个模块函数语义与落点正确：`sprintf` 返回 str；`printf` 写 stdout、不追加换行、返回字节数；`fprintf` 鸭子类型调用 `write(str)` 并透传其 int 返回值；失败路径遵守「置错误 + 返回 `NULL`」约定且资源逆序释放。
- [ ] `tests/ms/stdlib/fmt/` 覆盖「测试方案」全部清单项（含 `printf.out` 全文比对与负向用例的预期退出码 1），`python run_tests.py` 全绿，`ctest --test-dir build` 并入通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）下 fmt 全部测试与回归无内存错误与泄漏（`msCloseState` 后分配计数归零，含注册期对象）。
- [ ] 无 TBD/TODO 占位；对任务 06/10/13/17/24/37 的接口假定（码点原语、`msObjectStr`、`msNewCFunction`、属性与 GC 标记分派挂接点、import 与 Writer 协议定稿）在实现时已按对应任务文档对齐；任务 18 关于 `module.h` 归属的出入已按「设计依据」歧义 1 处理。
