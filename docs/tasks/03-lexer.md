# 03 词法分析器（Lexer）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [02 核心基础设施](02-core-infrastructure.md) |

## 任务目标

交付 mslang 的词法分析器模块（`src/lexer/ms_lexer.h` / `src/lexer/ms_lexer.c`）：把 `.ms` 源文件的 UTF-8 字节流切分为带位置信息（行/列）的 token 流，完整覆盖 `docs/language/01-lexical.md` 的全部词法规则——37 个关键字（含向下取整除法的词运算符 `div`）、整数/浮点/字符串/raw string/f-string/字节串字面量、全部运算符与定界符、注释跳过，以及 Go 同款分号自动插入。词法错误不中断扫描，而是经任务 02 的诊断收集器汇总（单文件上限 20 条）。完成后，任务 04（语法分析器）可以以 `msLexerNext` / `msLexerPeek` 两个调用驱动整个解析过程；本任务自身通过 `tests/c/test_lexer.c` 的 C 单元测试独立验证，不依赖后续任何模块。

## 设计依据

- `docs/language/01-lexical.md`
  - §1 源文件：UTF-8 无 BOM（带 BOM 非法）、LF/CRLF 统一为 LF。
  - §2 注释：行注释 `//`、块注释 `/* */` 不嵌套。
  - §3 标识符：字母开头（含 `_` 与 unicodeLetter），区分大小写。
  - §4 关键字：37 个（含词运算符 `div`）；`chan` 等内建函数名不是关键字。
  - §5 字面量：任意精度整数（下划线分隔、`0x`/`0o`/`0b` 前缀）、float64（`.5` 合法、`5.` 非法、指数形式）、字符串转义序列、反引号 raw string、f-string（`{expr}` 与 `:` 格式说明）、`b"..."` 字节串。
  - §6 运算符与定界符：全部符号的完整清单。
  - §7 分号自动插入：行尾触发 token 清单、续行规则、空语句不合法。
- `docs/language/08-vm-internals.md` §1：编译错误收集模式——单文件最多报告 20 个错误后中止，错误含文件/行/列与错误码。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则（内部结构体不 typedef）、§5 错误处理与资源管理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）。
- `docs/language/11-project-layout.md`：`src/lexer/ms_lexer.{c,h}` 的目录位置；`tests/c/` 用自研 `ms_test.h`（任务 02 提供）做模块单元测试。
- `docs/language/09-c-api.md` §4：`MsResult` 枚举（`MS_OK` / `MS_ERROR_SYNTAX` / `MS_ERROR_OOM`）的取值约定。
- 任务 02 核心基础设施提供：`msAlloc/msRealloc/msFree`、`MS_ASSERT`、`struct MsDiagList` 诊断收集器（条目含文件/行/列/错误码/消息，容量上限 20）、`ms_test.h` 测试头。本文假定其接口名；若任务 02 文档定名不同，以实现时对齐为准。

## 详细设计

### 文件与头文件骨架

- 头文件 `src/lexer/ms_lexer.h`，include guard `MSLANG_SRC_LEXER_MS_LEXER_H_`，自包含（自行 include `<stdbool.h>` `<stddef.h>` `<stdint.h>` 及任务 02 的 `"core/ms_diag.h"`）。
- 实现文件 `src/lexer/ms_lexer.c`；扫描辅助函数一律文件内 `static`。
- 模块不持有堆内存：token 词素为指向调用者源码缓冲区的切片，唯一分配点是 `msLexerUnescape` 的输出缓冲区（调用者用 `msFree` 释放，所有者明确）。

### token 类型枚举

枚举允许 typedef（10-c-style §4），枚举值 `MS_` 大写蛇形、全局唯一：

```c
typedef enum {
  MS_TOKEN_EOF,             // 文件结束
  MS_TOKEN_INVALID,         // 词法错误占位（诊断已记录，供 parser 同步）

  MS_TOKEN_IDENTIFIER,
  MS_TOKEN_INT,             // 整数（任意精度，lexer 只校验形式，不转值）
  MS_TOKEN_FLOAT,           // float64
  MS_TOKEN_STRING,          // "..." 普通字符串（转义已校验，未解码）
  MS_TOKEN_RAW_STRING,      // `...` 反引号 raw string
  MS_TOKEN_BYTES,           // b"..." 字节串
  MS_TOKEN_FSTRING_START,   // f" 前缀（见 f-string token 化）
  MS_TOKEN_FSTRING_END,     // f-string 的闭合引号
  MS_TOKEN_FSTRING_FORMAT,  // 插值内 ':' 之后的格式说明原文

  MS_TOKEN_SEMICOLON,       // 显式 ';' 或自动插入的分号

  // 关键字（37 个，与 01-lexical §4 一一对应）
  MS_TOKEN_KW_AND, MS_TOKEN_KW_AS, MS_TOKEN_KW_ASYNC, MS_TOKEN_KW_AWAIT,
  MS_TOKEN_KW_BREAK, MS_TOKEN_KW_CASE, MS_TOKEN_KW_CLASS, MS_TOKEN_KW_CONTINUE,
  MS_TOKEN_KW_DEFAULT, MS_TOKEN_KW_DEL, MS_TOKEN_KW_DIV, MS_TOKEN_KW_ELSE,
  MS_TOKEN_KW_EXCEPT, MS_TOKEN_KW_FALSE, MS_TOKEN_KW_FINALLY, MS_TOKEN_KW_FOR,
  MS_TOKEN_KW_FROM, MS_TOKEN_KW_FUNC, MS_TOKEN_KW_GLOBAL, MS_TOKEN_KW_IF,
  MS_TOKEN_KW_IMPORT, MS_TOKEN_KW_IN, MS_TOKEN_KW_IS, MS_TOKEN_KW_LAMBDA,
  MS_TOKEN_KW_NIL, MS_TOKEN_KW_NOT, MS_TOKEN_KW_OR, MS_TOKEN_KW_PASS,
  MS_TOKEN_KW_RAISE, MS_TOKEN_KW_RETURN, MS_TOKEN_KW_SELECT, MS_TOKEN_KW_SELF,
  MS_TOKEN_KW_SUPER, MS_TOKEN_KW_TRUE, MS_TOKEN_KW_TRY, MS_TOKEN_KW_WHILE,
  MS_TOKEN_KW_WITH,

  // 运算符与定界符（01-lexical §6；`//` 恒为行注释，无对应 token）
  MS_TOKEN_PLUS, MS_TOKEN_MINUS, MS_TOKEN_STAR, MS_TOKEN_SLASH,
  MS_TOKEN_PERCENT, MS_TOKEN_DOUBLE_STAR,
  MS_TOKEN_EQUAL_EQUAL, MS_TOKEN_BANG_EQUAL,
  MS_TOKEN_LESS, MS_TOKEN_LESS_EQUAL, MS_TOKEN_GREATER, MS_TOKEN_GREATER_EQUAL,
  MS_TOKEN_AMP, MS_TOKEN_PIPE, MS_TOKEN_CARET, MS_TOKEN_TILDE,
  MS_TOKEN_SHIFT_LEFT, MS_TOKEN_SHIFT_RIGHT,
  MS_TOKEN_COLON_EQUAL,     // :=
  MS_TOKEN_EQUAL, MS_TOKEN_PLUS_EQUAL, MS_TOKEN_MINUS_EQUAL,
  MS_TOKEN_STAR_EQUAL, MS_TOKEN_SLASH_EQUAL, MS_TOKEN_PERCENT_EQUAL,
  MS_TOKEN_DOUBLE_STAR_EQUAL,
  MS_TOKEN_AMP_EQUAL, MS_TOKEN_PIPE_EQUAL, MS_TOKEN_CARET_EQUAL,
  MS_TOKEN_SHIFT_LEFT_EQUAL, MS_TOKEN_SHIFT_RIGHT_EQUAL,
  MS_TOKEN_PLUS_PLUS,       // ++（仅 for 子句语句形式，见下「说明」）
  MS_TOKEN_MINUS_MINUS,     // --
  MS_TOKEN_LEFT_PAREN, MS_TOKEN_RIGHT_PAREN,
  MS_TOKEN_LEFT_BRACKET, MS_TOKEN_RIGHT_BRACKET,
  MS_TOKEN_LEFT_BRACE, MS_TOKEN_RIGHT_BRACE,
  MS_TOKEN_COMMA, MS_TOKEN_COLON, MS_TOKEN_DOT, MS_TOKEN_ELLIPSIS  // ...
} MsTokenType;
```

说明：

- `++`/`--` 未出现在 01-lexical §6 的运算符表中，但 §7 分号插入规则要求识别 `i++`/`i--`，故 lexer 产出这两个 token；使用语境（仅 `for` 子句中的语句形式）由 parser 限制，与 lexer 无关。
- `//` 恒为行注释起点，不产生运算符 token（枚举中无 `MS_TOKEN_DOUBLE_SLASH`/`MS_TOKEN_DOUBLE_SLASH_EQUAL`）；向下取整除法是关键字 `div`（`MS_TOKEN_KW_DIV`，无 `div=` 复合赋值）。旧设计中 `//`/`//=` 运算符与行注释的冲突以此裁决消除。
- 01-lexical §5 的示例只出现双引号字符串，未定义单引号字符串。v0.1 只支持 `"`（普通/f-string/字节串）与反引号（raw string）两种引号；转义表中的 `\'` 保留在校验器中以便将来扩展。
- 普通字符串内容、整数/浮点数值都不在 lexer 阶段求值：整数是任意精度，转值交给后续任务；字符串转义解码由 `msLexerUnescape` 按需执行（见下）。

### token 与 lexer 结构体

内部结构体不 typedef（10-c-style §4），字段 lowerCamelCase：

```c
struct MsToken {
  MsTokenType type;
  const char* start;   // 词素起点，指向调用者持有的源码缓冲区，不复制
  size_t length;       // 词素字节长度
  uint32_t line;       // 1 起始
  uint32_t column;     // 1 起始，按字节计（与 Go 一致）
};

typedef enum {
  MS_LEXMODE_NORMAL,          // 普通代码
  MS_LEXMODE_FSTRING_TEXT,    // f-string 的文本段（两个插值之间）
  MS_LEXMODE_FSTRING_FORMAT   // 插值内 ':' 之后的格式说明段
} MsLexerModeKind;

struct MsLexerFrame {         // 每层 f-string 插值一帧
  char quote;                 // f-string 的闭合引号（'"'）
  int braceDepth;             // 插值表达式内 '{' '}' 的嵌套深度
};

#define MS_LEXER_MAX_FSTRING_DEPTH 8

struct MsLexer {
  const char* source;         // 源码缓冲区，调用者持有，lexer 不拥有
  size_t sourceLen;
  size_t pos;                 // 当前扫描字节偏移
  uint32_t line;              // 当前行，1 起始
  uint32_t column;            // 当前列，1 起始，按字节计
  const char* chunkName;      // 文件名/块名，调用者持有，写入诊断
  bool canEndStatement;       // 上一个 token 允许在行尾插入分号
  MsLexerModeKind mode;
  struct MsLexerFrame frames[MS_LEXER_MAX_FSTRING_DEPTH];
  size_t frameCount;          // 当前 f-string 插值嵌套层数
  struct MsToken peeked;      // 单 token 前瞻缓存
  MsResult peekedResult;
  bool hasPeeked;
  struct MsDiagList* diags;   // 诊断收集器（任务 02），调用者持有
};
```

### 公开函数

```c
// Initializes lexer over [source, sourceLen). source and chunkName must
// outlive the lexer; neither is copied. diags receives lexical errors
// (cap 20 per file). A UTF-8 BOM is reported (E101) and skipped.
void msLexerInit(struct MsLexer* lexer, const char* source, size_t sourceLen,
    const char* chunkName, struct MsDiagList* diags);

// Releases lexer-internal state. Does not free source, chunkName, or diags.
void msLexerDestroy(struct MsLexer* lexer);

// Produces the next token into out.
// Returns MS_OK on a normal token (including MS_TOKEN_INVALID placeholders
// for recoverable errors); returns MS_ERROR_SYNTAX once the diagnostic cap
// is reached, with out set to MS_TOKEN_EOF, signalling the caller to abort.
MsResult msLexerNext(struct MsLexer* lexer, struct MsToken* out);

// Returns the type of the next token without consuming it (1-token
// lookahead, cached). Never fails visibly: an error-limit state reports
// MS_TOKEN_EOF; a bad token reports MS_TOKEN_INVALID.
MsTokenType msLexerPeek(struct MsLexer* lexer);

// Decodes the escape sequences of an already-validated string or bytes
// literal body (raw slice without surrounding quotes/prefix) into a newly
// msAlloc'd buffer. Caller owns *out and frees it with msFree.
// raw must come from a token this lexer produced; invalid input is a
// programming error (MS_ASSERT in debug builds).
MsResult msLexerUnescape(const char* raw, size_t rawLen, char** out, size_t* outLen);

// Static name table for diagnostics and tests ("MS_TOKEN_KW_IF" etc.).
const char* msTokenTypeName(MsTokenType type);
```

### 扫描主循环与位置跟踪

- `msLexerNext` 若有前瞻缓存则直接弹出；否则进入扫描：跳过空白（空格、制表符）与注释，处理换行（见分号插入），再按当前 `mode` 分派到 token 扫描。
- `\r\n` 归一为一次换行；裸 `\r`（后非 `\n`）报 E112 非法字符后按换行处理，保证恢复。
- 行/列在消费字符时维护，均为 1 起始；token 的行列记录其第一个字符的位置。
- UTF-8：多字节序列做形式校验（长度与续字节），非法序列报 E102；非 ASCII 码点一律允许出现在标识符中（v0.1 简化：不查 Unicode 字母类别表，完整 `unicodeLetter` 判定留待后续版本，此处与 01-lexical §3 的差异须在任务文档中显式承认）。

### 关键字表

- 文件级 `static const` 表：`{const char* name; MsTokenType type;}`，37 项按字典序排列，查找用二分。
- 扫描出标识符后查表：命中则改记为关键字 token，未命中为 `MS_TOKEN_IDENTIFIER`。`chan`、`len` 等内建函数名不在表中，自然落为标识符，可被遮蔽（01-lexical §4）。

### 数字字面量

只校验词法形式并产出切片，不转值。规则：

- 十进制整数：数字串，允许数字间的单下划线（`1_000_000`）；下划线不得出现在首尾、不得连续，进制前缀后允许一个下划线（`0x_1F`，与 Go 一致）。
- 进制前缀：`0x`/`0X` 十六进制、`0o`/`0O` 八进制、`0b`/`0B` 二进制；前缀后至少一个合法数字，出现非法数字（如 `0b102`、`0o8`）报 E107。
- 浮点：`[digits] . digits [exponent]` 或 `digits exponent` 或 `. digits [exponent]`；exponent 为 `e|E [+|-] digits`。
  - `.5` 合法：`.` 后紧跟数字时进入浮点扫描，否则 `.` 按定界符处理（`...` 优先匹配省略号）。
  - `5.` 非法：`digits .` 后无数字时报 E108（01-lexical §5.2：避免与属性访问歧义）。注意 `5..` 与 `5...` 同样先报 E108，不尝试回退为属性访问。
- 整数与浮点的区分在扫描中一次完成：先按整数扫，遇到 `.`+数字 或 指数标记则切换为浮点。

### 字符串、raw string 与字节串

- 普通字符串 `"..."`：扫描到闭合引号，逐字符校验转义；支持 `\n \t \r \\ \" \' \0 \xHH \uHHHH \UHHHHHHHH`（01-lexical §5.3）。`\x` 必须跟 2 位十六进制、`\u` 4 位、`\U` 8 位，位数不足或非法报 E106；`\u`/`\U` 码点超过 U+10FFFF 或为代理区报 E106。行内遇未转义换行或 EOF 报 E103（字符串不跨行）。token 词素含首尾引号，解码留给 `msLexerUnescape`。
- raw string `` `...` ``：反引号包围，不处理转义，可跨行；遇 EOF 未闭合报 E104。CRLF 已在位置跟踪层归一。
- 字节串 `b"..."`：`b` 前缀后跟双引号；扫描与转义规则同普通字符串（`\xHH` 即原始字节）。判定顺序：`b` 后跟 `"` 时按字节串扫描，否则 `b` 是普通标识符起点。
- 解码器 `msLexerUnescape`：单遍把转义序列展开为 UTF-8 字节（`\u`/`\U` 编码为 UTF-8），输出缓冲区经 `msAlloc`，长度前缀确定（先算后写或两遍扫描，实现自选）；OOM 返回 `MS_ERROR_OOM`。

### f-string token 化

f-string 含任意表达式，无法在单 token 内表达。采用模式栈把一条 f-string 展开为 token 序列，parser 负责组装（求值与格式化属后续任务）：

```
f"x = {x + 1:08d}, {name}!"
→ MS_TOKEN_FSTRING_START("f\"")
  MS_TOKEN_STRING("x = ")            // 文本段，转义规则同普通字符串
  MS_TOKEN_LEFT_BRACE                // 进入插值
  MS_TOKEN_IDENTIFIER(x) MS_TOKEN_PLUS MS_TOKEN_INT(1)
  MS_TOKEN_FSTRING_FORMAT("08d")     // 插值内顶层的 ':' 之后
  MS_TOKEN_RIGHT_BRACE               // 离开插值
  MS_TOKEN_STRING(", ")
  MS_TOKEN_LEFT_BRACE
  MS_TOKEN_IDENTIFIER(name)
  MS_TOKEN_RIGHT_BRACE
  MS_TOKEN_STRING("!")
  MS_TOKEN_FSTRING_END("\"")         // 闭合引号
```

算法：

1. `NORMAL` 模式下扫到 `f"` 时产出 `MS_TOKEN_FSTRING_START`，压入一帧 `{quote='"', braceDepth=0}`，切到 `FSTRING_TEXT`。
2. `FSTRING_TEXT` 按普通字符串规则扫描文本段（产出 `MS_TOKEN_STRING`），并识别边界：
   - `{{` / `}}` 是字面量大括号，归入文本段（解码时还原为单个 `{}`，由 `msLexerUnescape` 处理）；
   - 单个 `{`：产出 `MS_TOKEN_LEFT_BRACE`，`braceDepth=1`，切回 `NORMAL`；
   - 闭合引号：产出 `MS_TOKEN_FSTRING_END`，弹帧；若弹空则回到 `NORMAL`，否则回到外层的 `FSTRING_TEXT`（f-string 插值里可再嵌套 f-string）；
   - 换行或 EOF 未闭合：报 E103，恢复为弹帧。
3. `NORMAL` 模式下、位于帧内（`frameCount > 0`）时：
   - `{`/`}` 照常产出 `MS_TOKEN_LEFT_BRACE`/`MS_TOKEN_RIGHT_BRACE` 并维护顶层帧的 `braceDepth`；`}` 使 `braceDepth` 归零时切回 `FSTRING_TEXT`；
   - 顶层帧 `braceDepth == 1` 处遇到 `:`：切到 `FSTRING_FORMAT`；
   - 帧外遇到裸 `}`（无配对 `{`）：报 E111。
4. `FSTRING_FORMAT`：收集原文直到使 `braceDepth` 归零的 `}`，期间允许 `{`/`}` 配对嵌套（格式说明内嵌套替换字段，对齐 Python）；产出 `MS_TOKEN_FSTRING_FORMAT`（词素不含首尾 `:`/`}`），随后该 `}` 照常产出 `MS_TOKEN_RIGHT_BRACE` 并切回 `FSTRING_TEXT`。未闭合报 E110。
5. 插值嵌套超过 `MS_LEXER_MAX_FSTRING_DEPTH` 报 E109，随后按普通字符串扫描到闭合引号恢复。

### 运算符与定界符

最长匹配（maximal munch），按前缀长度降序判定：`<<=`/`>>=`/`**=`/`...` → `<<`/`>>`/`**`/`==`/`!=`/`<=`/`>=`/`+=`/`-=`/`*=`/`/=`/`%=`/`&=`/`|=`/`^=`/`:=`/`++`/`--` → 单字符。`//` 恒为行注释起点（见 §2），不产生 `//`/`//=` 运算符 token；地板除是关键字 `div`（无 `div=` 复合赋值）。`.` 的判定优先级：`...` > 浮点（后跟数字） > `MS_TOKEN_DOT`。`!` 后非 `=` 报 E102（语言无逻辑非符号，逻辑非是关键字 `not`）。

### 分号自动插入

与 Go 同规则（01-lexical §7）。状态机：

- 每产出一个真实 token 后更新 `canEndStatement`：`true` 当且仅当 token ∈ {标识符， `MS_TOKEN_INT/FLOAT/STRING/RAW_STRING/BYTES/FSTRING_END`， `MS_TOKEN_KW_TRUE/FALSE/NIL`， `MS_TOKEN_KW_RETURN/BREAK/CONTINUE/PASS/RAISE`， `MS_TOKEN_RIGHT_PAREN/RIGHT_BRACKET/RIGHT_BRACE`， `MS_TOKEN_PLUS_PLUS/MINUS_MINUS`}；其余 token（运算符、`,`、`(`、`[`、`{`、`.`、`:` 等）置 `false`，行尾自然续行。词运算符 `div` 与 `and`/`or` 同类，不在触发集合中（行尾是 `div` 自然续行）。
- 扫描中遇到换行（含块注释内部含换行的情形，与 Go 一致）且 `canEndStatement == true`：产出一个 `MS_TOKEN_SEMICOLON`（词素为空切片，位置为换行处），置 `canEndStatement = false`。行注释 `//` 之后的换行同样生效。
- 显式 `;` 产出 `MS_TOKEN_SEMICOLON` 并置 `canEndStatement = false`（空语句不合法由 parser 判定，lexer 不拒绝连续分号）。
- EOF 处若 `canEndStatement == true`，先补一个 `MS_TOKEN_SEMICOLON` 再于下次调用产出 `MS_TOKEN_EOF`（Go 同款行为）。
- `MS_TOKEN_INVALID`、`MS_TOKEN_FSTRING_START/FORMAT/LEFT_BRACE` 等不改写已确立的分号语义：按上表统一置 `false`，错误恢复交给 parser。

### 错误收集与恢复

- 所有词法错误经任务 02 的诊断收集器记录：`msDiagReport(lexer->diags, line, column, code, fmt, ...)`，条目含 `chunkName`/行/列/错误码/消息，容量 20（08-vm-internals §1）。
- 记录错误后产出 `MS_TOKEN_INVALID` token（词素为出错片段）并继续扫描，供 parser 做语句级同步；达到 20 条后 `msLexerNext` 一律返回 `MS_ERROR_SYNTAX` 且输出 `MS_TOKEN_EOF`，编译中止。
- lexer 错误码表（错误码字符串 + 文件内 `static const` 消息模板）：

| 错误码 | 情形 |
|---|---|
| E101 | 文件含 UTF-8 BOM |
| E102 | 非法字符（含裸 `!`、非法 UTF-8 序列） |
| E103 | 字符串未闭合（含 f-string 文本段） |
| E104 | raw string 未闭合 |
| E105 | 块注释未闭合 |
| E106 | 非法转义序列 |
| E107 | 数字字面量非法（下划线位置、进制数字越界、指数缺数字） |
| E108 | 浮点 `.` 后缺数字（`5.` 形式） |
| E109 | f-string 插值嵌套超限 |
| E110 | f-string 插值 `{` 未配对 |
| E111 | 裸 `}` 无配对 |
| E112 | 裸 `\r` |

## 实现步骤

1. 建 `src/lexer/ms_lexer.h` / `ms_lexer.c` 骨架：`MsTokenType` 全量枚举、`struct MsToken`、`msTokenTypeName` 静态名称表。验证：单元测试断言枚举值与名称表一一对应、无空缺。
2. 实现 `msLexerInit` / `msLexerDestroy`、位置跟踪（行/列、CRLF 归一、裸 `\r` 报错）、BOM 检查（E101）、EOF 处理。验证：空源码只产出 `MS_TOKEN_EOF`；行列断言。
3. 空白与注释跳过：行注释、块注释（不嵌套）、块注释未闭合 E105、块注释内换行参与分号判定。验证：纯注释源码的 token 序列。
4. 标识符扫描 + 37 关键字二分查找表。验证：全部关键字逐一命中；`chan`、`len` 等内建名产出 `MS_TOKEN_IDENTIFIER`；非 ASCII 标识符。
5. 数字字面量扫描与校验：十进制/三个进制前缀、下划线规则、浮点各形式、`.5` 合法、`5.` 报 E108、E107 各子情形。验证：每类正例 + 全部负例的 token 类型与诊断码。
6. 普通字符串、raw string、字节串扫描 + `msLexerUnescape` 解码器。验证：全部转义序列的解码结果、E103/E104/E106、解码缓冲区所有权（`msFree` 后无泄漏，配合任务 02 的分配统计）。
7. 运算符与定界符最长匹配（含 `++`/`--`、`...`、`:=`、全部复合赋值）。验证：相邻符号组合（`<<=`、`<<`、`<=`、`<`）的切分结果。
8. 分号自动插入状态机：`canEndStatement` 更新、换行/块注释换行/显式 `;`/EOF 补分号。验证：§7 的每条触发 token 与每条续行情形。
9. f-string 模式栈：`FSTRING_START`/文本段/`{`/`}`/`FSTRING_FORMAT`/`FSTRING_END` 全序列、`{{`/`}}`、嵌套 f-string、E109/E110/E111。验证：逐 token 序列比对。
10. 错误恢复与上限：E102 后继续扫描、20 条诊断后 `MS_ERROR_SYNTAX` + 强制 EOF、`msLexerPeek` 缓存不重复记录诊断。验证：多错误源码的诊断列表内容与后续 token 流。

## 测试方案

本任务早于最小可运行解释器（任务 09），只能用 C 单元测试。测试文件 `tests/c/test_lexer.c`（本任务只交付本设计文档，测试代码随实现任务编写），使用任务 02 的 `ms_test.h`。每个用例用源码字符串初始化 lexer，逐 token 断言 `{type, 词素, line, column}` 四元组序列，再断言诊断列表。

覆盖清单：

- 基础：空源码 / 纯空白 / 纯注释源码只产出 EOF（必要时含结尾分号规则结果）；BOM 报 E101 且后续 token 正常；CRLF 与 LF 源码产出相同 token 流；裸 `\r` 报 E112。
- 标识符与关键字：37 个关键字全量；大小写敏感（`And` 是标识符）；内建函数名（`chan` `len` `print`）是标识符；`_`、`_x9`、含非 ASCII 字符的标识符；数字开头报 E107（按数字非法）或 E102。
- 整数：`42`、`0`、`1_000_000`、`0x1F`、`0x_1F`、`0o755`、`0b1010`；负例：`_1`、`1_`、`1__2`、`0x`、`0b102`、`0o8`、`0xG`。
- 浮点：`3.14`、`1e-9`、`2.5e+4`、`.5`、`1.`（E108）、`1e`（E107）、`1e+`（E107）、`5.x` 序列应为 `INT(5) DOT IDENT(x)` 前的 E108 行为（按设计 `5.` 直接报错）。
- 字符串：全转义序列正例；`\x0`（位数不足）、`\uD800`（代理区）、`\q`、未闭合（行尾/EOF）各负例；raw string 跨行、含 `\n` 原样、未闭合 E104；字节串 `b"\x00\x01"`、`b` 单独是标识符；`msLexerUnescape` 对每个转义序列的解码字节断言。
- 运算符：全表逐一；组合最长匹配 `<<=` vs `<<` vs `<=` vs `<`、`...` vs `.5` vs `.`、`/=` vs `/`；裸 `!` 报 E102。`//` 与 `//=` 开头的源码按行注释处理（跳过，不产生运算符 token）；`a div b` 产出 `IDENT KW_DIV IDENT`。
- 分号插入：
  - 触发：标识符/各字面量/`true`/`nil`/`return`/`break`/`continue`/`pass`/`raise`/`)`/`]`/`}`/`++`/`--` 后换行 → 插入分号；
  - 续行：二元运算符、`,`、`(`、`[`、`{`、`.`、`:` 行尾 → 不插入；
  - 显式 `;` 原样产出；连续 `;;` 不被 lexer 拒绝；
  - 块注释内含换行时按换行处理；行注释后的换行正常触发；
  - EOF 补分号：`x := 1`（无尾换行）产出 `... SEMICOLON EOF`。
- f-string：`f""`（空）、无插值、单插值、多插值、带格式说明 `{pi:.2f}`、格式说明内嵌套 `{n:{w}d}`、`{{`/`}}` 文本、插值内嵌套 f-string、表达式内 `{` 字典字面量的 braceDepth 配对；负例 E109/E110/E111 及文本段未闭合 E103。
- 错误收集：单文件多错误（如 3 处非法字符）全部记录且 token 流含对应 `MS_TOKEN_INVALID` 后继续；构造 21+ 处错误，断言只记录 20 条、`msLexerNext` 返回 `MS_ERROR_SYNTAX`、此后只产出 EOF；`msLexerPeek` 多次调用不重复记录诊断、不改变 token 流。

## 验收标准

- [ ] `src/lexer/ms_lexer.h` / `ms_lexer.c` 存在，guard 为 `MSLANG_SRC_LEXER_MS_LEXER_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsLexer` 不 typedef）。
- [ ] `MsTokenType` 覆盖 01-lexical §4 全部 37 个关键字与 §6 全部运算符/定界符，另有 `++`/`--`（§7 要求）；`msTokenTypeName` 表完整。
- [ ] token 携带类型/词素切片/行/列；词素不复制、指向调用者缓冲区；模块堆分配仅 `msLexerUnescape` 一处且经 `msAlloc`，调用者 `msFree`，任务 02 的分配统计显示无泄漏。
- [ ] 数字、字符串、raw string、字节串、f-string 的字面量规则与本文「详细设计」一致，含 `.5` 合法、`5.` 报 E108。
- [ ] 分号自动插入实现 §7 全部触发与续行情形，含块注释换行、显式 `;`、EOF 补分号。
- [ ] f-string 展开为 `FSTRING_START` … `FSTRING_END` 的 token 序列，支持格式说明与嵌套，深度上限 8。
- [ ] 词法错误经任务 02 诊断收集器记录（文件/行/列/错误码 E101–E112），错误后产出 `MS_TOKEN_INVALID` 继续扫描，满 20 条后返回 `MS_ERROR_SYNTAX` 中止。
- [ ] `tests/c/test_lexer.c` 覆盖「测试方案」全部清单项并全部通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 02 的接口假定（`struct MsDiagList`、`msAlloc` 等）在实现时已对齐。
