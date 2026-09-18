# 28 f-string

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [19 标准库：fmt](19-stdlib-fmt.md) |

## 任务目标

交付 f-string 的编译与运行时支持，补上编译管线中最后一块字面量拼图：把任务 04 已组装的 `MS_AST_FSTRING` 节点（TEXT/EXPR 交替 parts、格式说明原文切片）编译为字节码——文本段编译期解码入常量池，插值表达式照常编译，格式化经新增的 `MS_OP_FORMAT_VALUE` / `MS_OP_BUILD_STRING` 两条指令在运行时完成。格式说明微语言（`f"{pi:.2f}"`、`f"{n:08d}"`，对齐 Python）的解析与渲染和任务 19 的 `fmt` 模块（Go 风格动词 `%08d`）共用同一份中间表示与渲染引擎，本任务在其上新增 Python 风格前端并补齐引擎缺口（填充字符、对齐方式等）。格式说明内的嵌套替换字段（`f"{n:{w}d}"`）在编译期二次解析、复用 lexer/parser 管线编译为内联表达式；f-string 插值内再嵌套 f-string 由既有的递归结构天然支持。

完成后，`f"x = {x + 1:08d}, {name}!"` 在脚本中可直接求值；本任务经 `tests/ms/fstring/` 下的 ms 脚本测试验证。

## 设计依据

- `docs/language/01-lexical.md` §5.3：f-string 语法 `{expr[:format]}`、「格式微语言对齐 Python」；任务 03 确立的扁平 token 流约定（`FSTRING_START` … `FSTRING_END`，`{{`/`}}` 归入文本段由 `msLexerUnescape` 还原）。
- `docs/language/03-syntax.md` §6.3：`{` 字面量写作 `{{`；f-string 求值时机为运行时，内嵌表达式可调用任意函数。
- `docs/language/02-types.md` §4（str 不可变 UTF-8、码点口径）、§8（魔术方法封闭集：有 `__str__`/`__repr__`，**无 `__format__`**——见下「规范歧义的处理」）。
- `docs/language/07-stdlib.md` §1：fmt 的职责定位与 Go 风格动词表；「简单插值优先用 f-string」——两套格式语法并存是刻意的，共享引擎属本任务设计。
- `docs/language/08-vm-internals.md` §1（编译管线）、§2.2（指令集代表清单、指令数 ≤80 为软目标）、§5（求值栈是 GC 根集合）。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列、K&R、星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- `docs/language/11-project-layout.md` §1：`src/compiler/`、`src/object/`、`src/vm/` 的目录位置；§4 测试策略。
- `docs/tasks/README.md` 测试约定：本任务编号 ≥ 09，一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`，负向用例以 `<name>.exit` 同伴文件声明预期退出码（约定见 [09-minimal-interpreter.md](09-minimal-interpreter.md) 测试方案），由仓库根 `run_tests.py` 驱动。
- [03 词法分析器（Lexer）](03-lexer.md)：f-string 扁平 token 流、`MS_TOKEN_FSTRING_FORMAT` 原文切片（含嵌套 `{...}` 原文、不再展开）、`msLexerUnescape` 的 `{{`/`}}` 还原职责。
- [04 语法分析器与 AST](04-parser-ast.md)：`MS_AST_FSTRING`（`parts` 为 TEXT/EXPR 交替列表）与 `MS_AST_FSTRING_EXPR`（`fmt`/`fmtLen`/`hasFmt` 原文切片）节点结构；「格式说明的二次解析由任务 28 在编译期复用 lexer/parser 管线完成」的既定分工。
- [05 字节码格式与 MsProto](05-bytecode-proto.md)：操作码枚举与元数据表的扩展方式（v1.0 冻结前允许调整枚举数值）；`MsProtoBuilder` 发射/常量池 API。
- [20 标准库：strings](20-stdlib-strings.md)：`struct MsStrBuf` 增长缓冲（`src/object/ms_str_op.h`），渲染引擎的输出载体。
- 任务 07（编译器）、08（VM 执行核心）、19（fmt）的设计文档本文撰写时尚不存在：编译器表达式入口 `msCompilerCompileExpr`、VM 指令分派挂接点、fmt 共享格式核心的结构体与函数名均为本文假定命名，实现时以对应任务文档定名为准。

规范歧义的处理（实现与评审时以此为据）：

1. **两套格式语法，一份引擎。** 01-lexical 说 f-string 格式微语言「对齐 Python」（`:08d`、`:.2f`），07-stdlib 说 fmt 动词「对齐 Go」（`%08d`、`%.2f`）。两者描述同一格式化能力，本任务定义统一的中间表示 `struct MsFormatSpec` 与渲染引擎 `msFormatValue`（假定由任务 19 建立）；Go 动词前端与 Python 说明前端都只是把它填满的解析器。
2. **无 `__format__` 魔术方法。** 02-types §8 的魔术方法表是封闭集，不含 `__format__`。自定义实例的格式化协议为：先经 `__str__`（缺省回落 `__repr__`）得字符串，再按字符串口径应用宽度/对齐/精度；数值动词（`d`/`f`/`x` 等）作用于非数值类型直接报 ValueError，不走实例委托。`__format__` 列入路线图候选。
3. **不支持 Python 的 `!r`/`!s`/`!a` 转换字段。** 01-lexical 未定义；`!` 出现在插值顶层属词法/语法错误（E102/E201），出现在格式说明内按说明解析规则处理。
4. **嵌套替换字段的校验时机。** 无嵌套字段的静态格式说明在**编译期**完整校验（报错定位到源码行列）；含嵌套字段的说明其静态骨架无法先验合法（如 `{w}d` 中 `w` 的运行期值决定合法性），延至运行时校验、非法时报 ValueError。与 Python 的运行时报错行为对齐，静态情形则比 Python 更早报错。

## 详细设计

### 1. 文件布局与职责

```
src/compiler/
├── ms_fstring.h         # f-string 编译入口声明（guard MSLANG_SRC_COMPILER_MS_FSTRING_H_）
└── ms_fstring.c         # MS_AST_FSTRING 代码生成、格式说明二次解析（嵌套字段）
src/object/
├── ms_format.h          # 共享格式核心（任务 19 假定已建；本任务扩展声明）
└── ms_format.c          # 渲染引擎 + 两个前端（Go 动词前端属任务 19；Python 前端属本任务）
src/vm/
├── ms_opcode.{h,c}      # 枚举与元数据表追加两条指令（扩展方式见任务 05）
└── ms_vm.c              # 分派循环新增两个 case（文件名以任务 08 定名为准）
```

- 编译器侧全部函数除公开入口外一律文件内 `static`；本任务不改 lexer/parser/AST——token 流与节点结构在任务 03/04 已定型。
- 渲染引擎（`msFormatValue`）不感知来源：VM 的 `MS_OP_FORMAT_VALUE` 与 `stdlib/fmt` 的 `printf`/`sprintf` 都是它的调用方。

### 2. 共享格式核心：中间表示与渲染引擎

假定任务 19 已在 `src/object/ms_format.h`（guard `MSLANG_SRC_OBJECT_MS_FORMAT_H_`）建立如下核心；若其定名或字段划分不同，以任务 19 文档为准并按本节语义对齐。若任务 19 的中间表示未覆盖 Python 前端的需要（`fill` 多字节码点、`^`/`=` 对齐、`%` 类型），本任务扩展该结构并同步更新 fmt 前端（行为不变，由任务 19 既有测试守住），扩展属本任务范围。

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

// Formats value per spec, appending UTF-8 bytes to out. Numeric verbs require
// MS_TYPE_INT/BOOL (bool counts as 0/1) or MS_TYPE_FLOAT per the matrix in §7;
// violations raise ValueError. Returns MS_OK, or MS_ERROR_* with the script
// error already set on L. out is caller-owned (msStrBufInit/msStrBufFree).
MsResult msFormatValue(MsState* L, MsObject* value, const struct MsFormatSpec* spec, struct MsStrBuf* out);
```

引擎内部流程（任务 19 建立，本任务补齐缺口）：值 → 基本文本（`d` 转十进制、`f`/`g` 转浮点文本、无类型时经 `str()` 协议等）→ 符号/前缀/基数修饰 → 精度应用 → 宽度与对齐填充（**宽度按码点计**，与 strings 模块口径一致；`=` 对齐在符号之后填充）。全部填充经 `msStrBufPutRune`，填充码点可以是任意 Unicode 码点。

### 3. Python 格式说明前端

本任务在 `ms_format.{c,h}` 新增说明文本 → `MsFormatSpec` 的解析器：

```c
// Parses a resolved (brace-free) Python-style format spec of
// [spec, spec+specLen) into *out. On success returns MS_OK. On failure
// returns MS_ERROR_SYNTAX and sets *errAt to the byte offset of the
// offending character (callers convert to a compile diagnostic or a
// runtime ValueError). Unset spec (specLen == 0) yields the all-default spec.
MsResult msFormatParsePySpec(const char* spec, size_t specLen, struct MsFormatSpec* out, size_t* errAt);
```

语法（`{`/`}` 已在进入本函数前剥离，见 §5）：

```
formatSpec = [ [ fill ] align ] [ sign ] [ "#" ] [ "0" ] [ width ] [ "." precision ] [ type ]
fill       = <任意单码点，含多字节 UTF-8>
align      = "<" | ">" | "^" | "="
sign       = "+" | "-" | " "
width      = digit+
precision  = digit+
type       = "s" | "d" | "b" | "o" | "x" | "X" | "e" | "E" | "f" | "F" | "g" | "G" | "%"
```

v0.2 刻意不收的 Python 子集（遇到即解析失败）：分组选项 `,` 与 `_`、`n`/`c` 类型、`=` 之外的对齐扩展语义。判定细节：

- `fill`+`align` 的识别：首码点之后紧跟 `<`/`>`/`^`/`=` 之一时首码点为 fill；否则首码点本身若是 align 则仅设 align（故 fill 不能是 `<>=^` 之一，与 Python 一致）。
- `0` 在 width 之前出现时置 `zeroPad` 且隐含数值的符号感知填充（等价 Python 的 `=` 对齐 + fill `0`，由引擎统一处理）。
- width/precision 只允许十进制数字，无符号；超过 `INT32_MAX` 报解析失败（防御病态输入，Python 无此限制，属有意收窄）。
- 解析是纯函数：不分配堆内存、不触碰 `MsState`，可同时服务编译期校验与运行时解析。

### 4. f-string 编译：AST → 字节码

编译器遍历表达式节点时，`MS_AST_FSTRING` 分派到本任务的入口（`struct MsCompiler` 及其表达式入口 `msCompilerCompileExpr` 假定由任务 07 定义）：

```c
// Emits bytecode for an MS_AST_FSTRING node: text parts become string
// constants, interpolation expressions are compiled in place, each value is
// formatted via MS_OP_FORMAT_VALUE, and parts are joined by
// MS_OP_BUILD_STRING. Errors (bad static spec, nested-field parse failure)
// are reported to the compiler's diagnostic list; returns MS_ERROR_SYNTAX /
// MS_ERROR_OOM on failure.
MsResult msFstringCompile(struct MsCompiler* c, const struct MsAst* node);
```

生成流程（对每个 part 顺序发射）：

1. **TEXT 段**：`msLexerUnescape` 解码（转义展开 + `{{`/`}}` 还原为 `{`/`}`）→ `msProtoAddConst` 入常量池 → `MS_OP_LOAD_CONST`。解码在编译期完成，运行时不重复付出；空文本段（`f"{x}"` 首尾的零长 TEXT）不发射。
2. **EXPR 段**：`msCompilerCompileExpr` 编译插值表达式（值留在栈上）；随后发射规格：
   - `hasFmt == false`：`MS_OP_LOAD_NIL` 作为规格；
   - 静态格式说明（无 `{`）：原文切片 `msAlloc` 一份 NUL 结尾拷贝入常量池（常量对象归 GC 托管，拷贝的临时缓冲立即 `msFree`），`MS_OP_LOAD_CONST`；**同时**以 `msFormatParsePySpec` 做编译期校验，失败报 E320（诊断位置 = 节点行列 + `errAt` 列偏移）；
   - 含嵌套字段的说明：按 §5 展开为一段「规格字符串求值」代码，结果串留在栈上。
   - 随后发射 `MS_OP_FORMAT_VALUE`：弹规格（栈顶）与值，压入格式化后的 str。
3. **拼接**：parts 产物全部在栈上后发射 `MS_OP_BUILD_STRING Bx`（`Bx` = 实际发射的段数）；窥孔优化：恰好一段时不发射 `BUILD_STRING`（`f""`、`f"纯文本"` 退化为单条 `LOAD_CONST`）。

示例展开（`f"x = {x + 1:08d}, {name}!"`，`#k*` 为常量池下标）：

```
LOAD_CONST   #k0        ; "x = "
LOAD_LOCAL   x
LOAD_CONST   #k1        ; 1
ADD
LOAD_CONST   #k2        ; "08d"（编译期已校验）
FORMAT_VALUE
LOAD_CONST   #k3        ; ", "
LOAD_LOCAL   name
LOAD_NIL                ; 无格式说明
FORMAT_VALUE
LOAD_CONST   #k4        ; "!"
BUILD_STRING 5
```

行号：f-string 内全部指令登记为 `MS_AST_FSTRING` 节点的起始行（f-string 文本段不跨行——lexer 的 E103 保证；插值表达式内的换行归属插值首行，与运行时回溯的可读性取齐）。

### 5. 格式说明的二次解析与嵌套替换字段

任务 04 把格式说明存为原文切片（如 `{w}d`、`.{p}f`），其中的 `{...}` 是**嵌套替换字段**：说明的一部分要在运行期先求值。编译期处理（`ms_fstring.c` 内 `static` 函数）：

1. **切分**：单遍扫描原文，`{{`/`}}` 归为字面文本（还原为单个 `{`/`}`）；单个 `{` 开始一个嵌套字段，按大括号配对深度收集到匹配的 `}`；配不到对的 `{`/`}` 报 E321。
2. **嵌套字段 = 表达式 + 可选子说明**：对字段文本按**顶层**（配对深度 0）首个 `:` 切开；前半是表达式原文，后半是子说明，子说明递归套用本流程（Python 允许 `f"{a:{b:>{w}}"` 式的层层嵌套，递归天然支持）。无 `:` 时无子说明。
3. **表达式编译**：为表达式原文切片起一个子 `MsLexer` + 子 `MsParser`（复用任务 03/04 管线，共享同一诊断收集器，行列以切片基址加偏移回填），解析**单条表达式**——约定子解析接受表达式后跟一个可选的自动分号再 EOF（子 lexer 的分号自动插入会对行尾补 `;`，属预期）；产出 AST 后直接经 `msCompilerCompileExpr` 编译进**当前** proto（作用域就是 f-string 所在的作用域，嵌套字段里可自由引用局部变量）。表达式解析失败报 E322（子诊断已由共享收集器记录，本条只做定位汇总）。随后若无子说明发 `LOAD_NIL`，否则递归展开子说明，再发 `FORMAT_VALUE` 得到该字段的文本。
4. **拼装**：字面段（常量）与嵌套字段结果交替发射，最后 `BUILD_STRING` 拼成**已解析规格串**（保证不含 `{`/`}`），留在栈上交给外层 `FORMAT_VALUE`。已解析规格串的运行时合法性由 `FORMAT_VALUE` 内的 `msFormatParsePySpec` 兜底，失败抛 ValueError（见「规范歧义的处理」第 4 条）。

嵌套字段的常见形态是动态宽度/精度（`{w}`、`{p}`），其值经 `FORMAT_VALUE`（无子说明时即 `str()` 转换）变成十进制数字文本嵌进规格串；嵌套值含非法字符（如 `{name}` 中 `name = "x"` 时规格串 `"xd"`）在运行时解析失败，抛 ValueError。

### 6. 新增 VM 指令

在任务 05 的操作码枚举末尾（`MS_OP_PRINT_EXPR`/`MS_OP_NOP` 之前，「其他」类别）追加两条，标注 v0.2 启用，并同步 `opInfoTable`（v1.0 前枚举数值可调，任务 05 已声明）：

| 指令 | 格式 | 操作数 | 栈语义 |
|---|---|---|---|
| `MS_OP_FORMAT_VALUE` | ABC | 无 | 弹规格（栈顶，str 或 nil）与值，压入格式化结果 str |
| `MS_OP_BUILD_STRING` | ABC | `Bx` = 段数 | 弹 `Bx` 个 str，压入拼接结果 |

- `FORMAT_VALUE`：规格为 nil 时取全默认 `MsFormatSpec`（即 `str()` 转换 + 无修饰）；规格为 str 时 `msFormatParsePySpec` 解析（失败抛 ValueError），再 `msFormatValue` 渲染。规格非 str/nil 属编译器不变量破坏，`MS_ASSERT` 防御。
- `BUILD_STRING`：先累加各段字节长，一次性分配结果串再逐段拷贝（避免 O(N²)）；元素必为 str 是编译器保证的不变量（`MS_ASSERT`）。
- 中间产物全部位于求值栈上，求值栈是 GC 根集合（08-vm-internals §5），拼接与格式化中的分配触发 GC 无悬挂风险；`msFormatValue` 内部跨分配存活的 `MsObject*` 局部遵守 `msRootPush`/`msRootPop` 纪律（09-c-api §3）。
- 指令数账面：任务 05 枚举 82 条 + 本任务 2 条 = 84 条，超过 08-vm-internals §2.2「80 条以内」的软目标。任务 05 已将该目标定性为 v1.0 冻结前的软约束并给出回收预案（合并 `SELECT_ADD_RECV/ADD_SEND`、下封 `CMP_CHAIN`）；本任务不再为此牺牲这两条指令的直译性，v1.0 冻结前统一清点。

### 7. 运行时格式化语义

类型 × 动词矩阵（唯一权威；未列组合抛 ValueError）：

| 值类型 | 无类型 / `s` | `d` `b` `o` `x` `X` | `e` `E` `f` `F` `g` `G` `%` |
|---|---|---|---|
| int | `str()` 后按字符串规则 | 按对应进制/大小写 | 转 float64 后按浮点规则 |
| bool | `"true"`/`"false"` | 按 1/0 数值处理 | 按 1.0/0.0 处理 |
| float | `str()` 后按字符串规则 | ValueError | 按浮点规则（`%` 渲染为百分数并追加 `%`） |
| str | 原串 | ValueError | ValueError |
| nil | `"nil"` | ValueError | ValueError |
| 其他（list/dict/实例/…） | `str()` 协议（实例走 `__str__`，缺省 `__repr__`）后按字符串规则 | ValueError | ValueError |

字符串规则（右列两类之外的全部情形）：取基本文本后，precision ≥ 0 时按**码点**截断；再按 width/fill/align 填充（align 缺省左对齐；`=` 对非数值报 ValueError）。数值规则：align 缺省右对齐；`sign`/`#`/`zeroPad` 对非数值报 ValueError；`#` 对 `x`/`o`/`b` 加 `0x`/`0o`/`0b` 前缀；浮点精度缺省 6 位（`f`）/最短往返（无类型时的 `str()`），与任务 19 的 `%f`/`%v` 口径一致（同一引擎保证）。

无格式说明的插值 `{expr}` 即上表「无类型」列：等价 `str(expr)` 后无修饰。`f"{x}"` 与 `str(x)` 恒等是本设计的可测不变量。

### 8. 嵌套 f-string

插值内嵌套 f-string 无需本任务特殊处理：lexer 帧栈（任务 03）把内外两层展开为连续 token 流，parser 递归组装（任务 04），编译时内层 `MS_AST_FSTRING` 在外层插值表达式的编译中递归命中 `msFstringCompile`。词法嵌套深度上限 8（`MS_LEXER_MAX_FSTRING_DEPTH`）守住病态输入；格式说明内的嵌套字段递归（§5）共享同一递归预算，实现时以编译器既有的递归深度限制（任务 07）兜底，不另设上限。

### 9. 错误处理

编译期（记入共享诊断收集器，E3xx 号段假定归编译器，以任务 07 文档为准）：

| 错误码 | 情形 |
|---|---|
| E320 | 静态格式说明非法（`msFormatParsePySpec` 失败；消息含说明原文与出错列） |
| E321 | 格式说明内嵌套字段大括号不配对 |
| E322 | 嵌套字段表达式解析失败（附子诊断） |

运行时（v0.2 异常系统已由任务 23 落地，均为脚本可捕获的异常）：

- 含嵌套字段的规格串解析失败 → ValueError；
- 类型 × 动词矩阵的非法组合（如 `f"{'a':d}"`、`f"{1.5:d}"`、`f"{x:=}"` 对非数值）→ ValueError；
- `__str__` 返回非 str → TypeError（沿用 `str()` 协议既有的检查点，本任务不新增）。

## 实现步骤

1. 扩展 `src/vm/ms_opcode.{h,c}`：追加 `MS_OP_FORMAT_VALUE`/`MS_OP_BUILD_STRING` 枚举与 `opInfoTable` 条目。验证：任务 05 既有 C 单元测试保持绿色（枚举/元数据同序断言覆盖新条目后总数 84）。
2. 在 `src/object/ms_format.{c,h}` 核对任务 19 的中间表示：缺 `fill`/`^`/`=`/`%` 等字段时扩展 `struct MsFormatSpec` 与 `msFormatValue` 引擎，并同步 fmt 前端（行为不变）。验证：任务 19 全部脚本测试保持绿色。
3. 实现 `msFormatParsePySpec`（§3 语法全表、fill/align 识别、`0` 填充语义、宽度上限防御）。验证：经后续 f-string 脚本测试间接全覆盖（本步无独立脚本入口，不违反「任务 09 起一律 ms 脚本测试」约定）。
4. VM 分派实现两条指令：`FORMAT_VALUE`（nil 规格默认路径、解析失败 ValueError、GC 根纪律）与 `BUILD_STRING`（一次分配拼接）。验证：`f"{x}"` 形态的端到端脚本（此时编译侧可先以手写 proto 或最小接线验证，随即被第 5 步取代）。
5. 实现 `src/compiler/ms_fstring.{c,h}` 主流程：TEXT 解码入池、EXPR 发射、静态说明编译期校验（E320）、`BUILD_STRING` 与单段窥孔。验证：`basic.ms`/`format.ms` 脚本（无嵌套字段部分）通过。
6. 实现 §5 二次解析：原文切分（E321）、子 lexer/parser 单表达式编译（E322、行列偏移回填）、子说明递归。验证：`nested.ms` 通过。
7. 编写 `tests/ms/fstring/` 全部测试脚本（见测试方案），`python run_tests.py` 全绿。
8. Win/Linux/macOS × Debug/Release 构建验证；Debug（ASAN / `/RTC`）下跑全部 f-string 与 fmt、strings 回归测试无内存错误、无泄漏。

## 测试方案

本任务只交付本设计文档；测试脚本随实现编写。一律使用 ms 脚本测试（`tests/ms/fstring/`，内建 `assert` + `print`——任务 40 之前不用 testing 模块），成功脚本末尾 `print("<用例名> ok")`，负向用例配 `<name>.exit`（内容为预期退出码 `1`），由仓库根 `run_tests.py` 驱动。

测试文件清单与覆盖点：

- `basic.ms`：`f""` 为空串；纯文本 f-string 与普通字符串相等；单插值、多插值、首尾插值；`f"{x}"` == `str(x)` 对 int/float/bool/nil/str/list/dict 逐类型成立；插值内任意表达式（调用、切片、条件表达式、lambda 调用）。
- `escape.ms`：文本段 `{{`/`}}` 渲染为单个大括号；`{{` 与插值相邻（`f"{{{x}}}"` → `{值}`）；转义序列 `\n`/`\t`/`\u4e2d` 正常展开；多字节文本与码点口径。
- `format.ms`：§3 语法全表正例——对齐 `<`/`>`/`^`/`=`、自定义 fill（含多字节码点 `f"{7:中>3}"`）、sign 三态、`#` 前缀（`#x`/`#o`/`#b`）、`0` 填充与符号位置（`f"{-42:08d}"` → `-0000042`）、宽度按码点（多字节串填充）、精度（`.2f`、字符串截断按码点）、`%` 类型、`g`/`G`；§7 矩阵正例逐格（int 走浮点动词、bool 按 0/1、float 的 `%`）。
- `nested.ms`：插值内嵌套 f-string（两层、含内层带格式说明）；动态宽度/精度（`f"{n:{w}d}"`、`f"{pi:.{p}f}"`）；嵌套字段再带子说明（`f"{a:{b:>{w}}}"`）；嵌套字段引用局部变量与外层同名遮蔽；规格内 `{{`/`}}` 字面大括号。
- `parity.ms`：与 fmt 同引擎等价断言——`f"{255:x}" == fmt.sprintf("%x", 255)`、`f"{pi:.2f}" == fmt.sprintf("%.2f", pi)`、`f"{42:08d}" == fmt.sprintf("%08d", 42)` 等，锁定「两套语法一份引擎」约定。
- `errors_spec_*.ms`（各配 `.exit`）：静态非法说明在**编译期**拒绝（`f"{x:}"` 空说明合法而 `f"{x:zz}"`、`f"{x:.}"` 非法——按首触发点逐文件拆分）；运行时非法（`f"{'a':d}"`、`f"{1.5:d}"`、非数值用 `=` 对齐、嵌套值拼出非法规格如 `f"{n:{s}d}"` 其中 `s = "x"`）→ 退出码 1；v0.2 已有异常系统，另附一脚本用 `try/except` 捕获 ValueError 断言类型与消息要点（不退出）。
- `errors_nest_*.ms`（各配 `.exit`）：规格内大括号不配对（E321）、嵌套字段表达式语法错误（E322）。

## 验收标准

- [ ] `src/compiler/ms_fstring.{c,h}` 存在，guard 为 `MSLANG_SRC_COMPILER_MS_FSTRING_H_`，头文件自包含；`src/object/ms_format.{c,h}` 与 `src/vm/ms_opcode.{c,h}` 的扩展就位；全部代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef），堆分配只经 `msAlloc`/`msRealloc`/`msFree`。
- [ ] `MS_AST_FSTRING` 按本文 §4 编译：文本段编译期 `msLexerUnescape` 解码入池（含 `{{`/`}}` 还原）、插值照常编译、`MS_OP_FORMAT_VALUE`/`MS_OP_BUILD_STRING` 发射与栈语义正确、单段窥孔生效。
- [ ] Python 格式说明前端 `msFormatParsePySpec` 实现 §3 全表（fill/align/sign/`#`/`0`/width/precision/13 个类型字母），刻意不收的 `,`/`_`/`n`/`c` 明确解析失败；静态说明编译期校验报 E320。
- [ ] 格式渲染与 fmt 共用同一 `struct MsFormatSpec` 与 `msFormatValue` 引擎（`parity.ms` 通过）；任务 19 既有测试保持绿色（引擎扩展不改 fmt 行为）。
- [ ] §7 类型 × 动词矩阵逐格实现：宽度/精度/填充按码点口径、bool 按 0/1、`%` 百分数、`#` 进制前缀、非数值遇数值修饰报 ValueError；实例走 `__str__`（缺省 `__repr__`），不引入 `__format__`。
- [ ] 嵌套替换字段按 §5 二次解析：子 lexer/parser 复用、表达式编译进当前 proto 且可引用局部变量、子说明递归、`{{`/`}}` 字面还原、E321/E322 诊断；含嵌套字段的规格运行时非法抛 ValueError。
- [ ] 嵌套 f-string（插值内再嵌套）正确求值，词法深度上限 8 的既有行为不变。
- [ ] 操作码枚举与元数据表追加两条指令，任务 05 的 C 单元测试更新后保持绿色；指令数账面（84 条）与软目标的关系已按本文 §6 记录。
- [ ] `tests/ms/fstring/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿（含负向用例的预期退出码 1 与 try/except 捕获断言）；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）下 f-string + fmt + strings 回归无内存错误与泄漏（格式化路径的 GC 根纪律经评审核对）。
- [ ] 无 TBD/TODO 占位；对任务 07/08/19 的接口假定（`msCompilerCompileExpr`、VM 分派挂接点、`struct MsFormatSpec`/`msFormatValue` 定名）在实现时已按对应任务文档对齐。
