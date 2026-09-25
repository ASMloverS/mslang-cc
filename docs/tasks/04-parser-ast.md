# 04 语法分析器与 AST

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [03 词法分析器（Lexer）](03-lexer.md) |

## 任务目标

交付 mslang 的语法分析器与 AST 模块（`src/parser/ms_ast.h` / `src/parser/ms_ast.c` 与 `src/parser/ms_parser.h` / `src/parser/ms_parser.c`）：消费任务 03 产出的带位置 token 流，构建覆盖 `docs/language/03-syntax.md` 完整 EBNF 的抽象语法树。语句与声明用递归下降，表达式用 Pratt 解析（15 级优先级，含 `**` 右结合、链式比较、条件表达式、lambda）；f-string 的扁平 token 序列在此组装为结构化节点。语法错误经任务 02 的诊断收集器汇总（与词法错误共享单文件 20 条上限），panic 模式恢复到语句级同步点继续解析，一次编译报告尽量多的错误。完成后，任务 07（编译器）可以纯函数式地遍历 AST 生成字节码；本任务自身通过 `tests/c/test_parser.c` 的 C 单元测试独立验证。

AST 覆盖全量语法（含 try/with/select/import/推导式/f-string/class 继承等语义落在 v0.2/v0.3 的构造），后续任务只扩展编译器，不再回头改 parser 与 AST 结构。

## 设计依据

- `docs/language/03-syntax.md`
  - §2 顶层结构：文件 = 顶层语句序列，无入口函数概念。
  - §3 语句全集：`:=`/`=`/复合赋值、`if`/`for`（三分/迭代/裸循环）/`while`、`try`/`with`、`return`/`raise`/`del`/`global`/`break`/`continue`/`pass`、块。
  - §4 函数：`async func`、默认参数、`*args`、`**kwargs`（次序固定）、`lambda` 单表达式体。
  - §5 class：单继承 `<`、`static func`、`self`/`super`。
  - §6 表达式 15 级优先级表（含结合性）、链式比较、下标与切片、推导式、f-string。
  - §7 import 三种形式。
- `docs/language/06-concurrency.md` §3：`selectStmt`/`selectCase`/`recvStmt`/`sendStmt` 的 EBNF——03-syntax §3 的 statement 列表引用了 `selectStmt` 但未给产生式，语法以此处为准。
- `docs/language/01-lexical.md` §7：分号自动插入——parser 把 `MS_TOKEN_SEMICOLON` 视为语句终止符；空语句不合法（`;;` 或行首分号报语法错误，用 `pass` 代替）。
- `docs/language/08-vm-internals.md` §1：编译管线定位（Lexer → Parser → AST → Compiler）；编译错误收集模式——**单文件**最多 20 个错误后中止，故 parser 与 lexer 共享同一个诊断收集器及其计数。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则（内部结构体不 typedef）、§5 错误处理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）。
- `docs/language/11-project-layout.md` §1：`src/parser/` 目录位置；§4 测试策略：`tests/c/` + 自研 `ms_test.h`。
- 任务 03 提供：`struct MsLexer` / `struct MsToken` / `MsTokenType`、`msLexerNext` / `msLexerPeek`（1 token 前瞻，配合 parser 持有的当前 token 构成 2 token 前瞻）、f-string 的扁平 token 展开约定（`MS_TOKEN_FSTRING_START` … `MS_TOKEN_FSTRING_END`，见下「f-string 组装」）、`MS_TOKEN_INVALID` 错误占位。
- 任务 02 核心基础设施提供：`msAlloc/msRealloc/msFree`、`MS_ASSERT`、`struct MsDiagList` 与 `msDiagReport`、`ms_test.h`。任务 02 文档尚未存在，上述接口名为本文假定命名，实现时以对应任务文档定名为准。

规范歧义的处理（实现时按本文口径）：

- `static` 不在 01-lexical §4 的 37 个关键字中，但 03-syntax §5 的 class 示例使用 `static func`。parser 将 `static` 作**上下文关键字**处理：仅在 class 体内、紧邻 `func` 之前时识别为静态方法修饰符，其余位置是普通标识符。
- 03-syntax 的 EBNF 未给出容器字面量与推导式的产生式，但 §5/§6.2 示例使用 `[]`、`{k: v}`、三种推导式。parser 以示例语义补全：list 字面量 `[expr, ...]`、dict 字面量 `{k: v, ...}`、set 字面量 `{expr, ...}`、空 `{}` 一律为 dict（对齐 Python）；`[`/`{` 后跟 `for` 子句时切换为对应推导式。
- f-string 格式说明内的嵌套替换字段（`f"{n:{w}d}"`）：lexer 按约定产出单条 `MS_TOKEN_FSTRING_FORMAT` 原文切片，parser **不再展开**，存为原始文本节点；格式说明的二次解析由任务 28（f-string）在编译期复用 lexer/parser 管线完成。

## 详细设计

### 文件与模块划分

- `src/parser/ms_ast.h` / `ms_ast.c`：AST 节点定义、arena 分配器、调试转储（测试用）。include guard `MSLANG_SRC_PARSER_MS_AST_H_`。
- `src/parser/ms_parser.h` / `ms_parser.c`：parser 状态、公开入口、递归下降 + Pratt 实现。include guard `MSLANG_SRC_PARSER_MS_PARSER_H_`。语句/表达式解析函数一律文件内 `static`。
- 头文件自包含：自行 include `<stdbool.h>` `<stddef.h>` `<stdint.h>`、任务 02 的 `"core/ms_diag.h"`、任务 03 的 `"lexer/ms_lexer.h"`。

### AST 内存模型：arena 分配

一次解析的所有节点与辅助数组由一个 arena 统一管理，编译完成后整体释放，无逐节点 `msFree`：

```c
#define MS_AST_ARENA_BLOCK_SIZE 4096

struct MsAstArenaBlock {
  struct MsAstArenaBlock* next;
  size_t used;
  uint8_t data[MS_AST_ARENA_BLOCK_SIZE];  // 实际块长按分配量计，数组尾部位移分配
};

struct MsAstArena {
  struct MsAstArenaBlock* head;     // 块链表，msAlloc 分配、msFree 整体回收
  size_t totalAllocated;            // 统计与测试断言用
};

struct MsAstList {                  // 变长子节点表（块内语句、调用实参等）
  struct MsAst** items;             // arena 分配，容量倍增
  size_t count;
};
```

- 块尺寸按需 `msAlloc`（至少 `MS_AST_ARENA_BLOCK_SIZE`），释放集中在 `msAstArenaDestroy`，所有者即编译管线调用方。
- 节点字面量字段（标识符名、数字原文、字符串原文）为指向调用者源码缓冲区的切片，不复制、不求值：整数任意精度转值属后续任务，字符串转义解码由编译期经 `msLexerUnescape` 完成。**AST 生命周期不得超过源码缓冲区**，在头文件注释中显式声明。

```c
// Arena lifecycle: arena owns every node it hands out; one msAstArenaDestroy
// reclaims everything. Nodes borrow slices of the caller's source buffer.
void msAstArenaInit(struct MsAstArena* arena);
void msAstArenaDestroy(struct MsAstArena* arena);

// Allocates a zeroed node of the given kind. Returns NULL on OOM (caller
// converts to MS_ERROR_OOM); arena grows internally via msAlloc.
struct MsAst* msAstNew(struct MsAstArena* arena, MsAstKind kind, uint32_t line, uint32_t column);

// Appends to an arena-managed child list (capacity doubling).
bool msAstListPush(struct MsAstArena* arena, struct MsAstList* list, struct MsAst* node);

// Static name table for diagnostics and tests ("MS_AST_IF" etc.).
const char* msAstKindName(MsAstKind kind);

// Writes an S-expression dump of the tree (one node per line, 2-space indent)
// for golden-file style unit tests.
void msAstDump(const struct MsAst* node, FILE* out);
```

### AST 节点：类型标签 + 联合

内部结构体不 typedef，字段 lowerCamelCase：

```c
typedef enum {
  MS_AST_INVALID,            // 错误占位（诊断已记录），保证树结构完整可遍历

  // 字面量与主表达式
  MS_AST_INT,                // 原文切片，未转值（任意精度）
  MS_AST_FLOAT,
  MS_AST_STRING,             // "..."（转义未解码）
  MS_AST_RAW_STRING,         // `...`
  MS_AST_BYTES,              // b"..."
  MS_AST_BOOL,               // true / false
  MS_AST_NIL,
  MS_AST_IDENT,
  MS_AST_FSTRING,            // 见「f-string 组装」
  MS_AST_FSTRING_TEXT,       // f-string 文本段
  MS_AST_FSTRING_EXPR,       // f-string 插值 {expr[:format]}
  MS_AST_LIST_LIT,
  MS_AST_DICT_LIT,           // 键值对交错存放于子节点表（空 {} 归入此类）
  MS_AST_SET_LIT,
  MS_AST_LIST_COMP,
  MS_AST_DICT_COMP,
  MS_AST_SET_COMP,

  // 运算表达式
  MS_AST_UNARY,              // +x -x ~x not x
  MS_AST_BINARY,             // 算术/位运算
  MS_AST_COMPARE_CHAIN,      // 链式比较（含 in / not in / is / is not）
  MS_AST_LOGICAL,            // and / or（短路，编译期生成跳转）
  MS_AST_CONDITIONAL,        // a if cond else b
  MS_AST_AWAIT,
  MS_AST_LAMBDA,
  MS_AST_CALL,
  MS_AST_KW_ARG,             // 调用中的 name=expr
  MS_AST_INDEX,              // a[i]
  MS_AST_SLICE,              // a[i:j:k]，三个分量均可空
  MS_AST_ATTR,               // a.b

  // 语句
  MS_AST_SOURCE_FILE,        // 根节点
  MS_AST_BLOCK,
  MS_AST_DECL,               // identList := exprList（目标可含至多一个 *ident 解包）
  MS_AST_ASSIGN,             // targetList = exprList
  MS_AST_AUG_ASSIGN,         // += -= *= ... >>=（op 存原 token 类型）
  MS_AST_EXPR_STMT,
  MS_AST_INC_DEC,            // i++ / i--（仅 for 子句后置语句）
  MS_AST_IF,                 // else-if 链即 elseBranch 再挂 MS_AST_IF
  MS_AST_FOR,                // forKind 区分三分/迭代/裸循环
  MS_AST_WHILE,
  MS_AST_TRY,                // except 子句为 MS_AST_EXCEPT_CLAUSE 列表
  MS_AST_EXCEPT_CLAUSE,
  MS_AST_WITH,
  MS_AST_SELECT,             // case 为 MS_AST_SELECT_CASE 列表
  MS_AST_SELECT_CASE,
  MS_AST_IMPORT,             // import "path" [as name]
  MS_AST_FROM_IMPORT,        // from "path" import ...
  MS_AST_RETURN,
  MS_AST_RAISE,              // raise expr [from expr]
  MS_AST_DEL,
  MS_AST_GLOBAL,
  MS_AST_BREAK,
  MS_AST_CONTINUE,
  MS_AST_PASS,

  // 声明
  MS_AST_FUNC_DECL,          // async / static 用标志位
  MS_AST_PARAM,              // 普通（可带默认值）/ *varargs / **kwargs
  MS_AST_CLASS_DECL
} MsAstKind;
```

节点主体：

```c
typedef enum {
  MS_CMP_LT, MS_CMP_LE, MS_CMP_GT, MS_CMP_GE, MS_CMP_EQ, MS_CMP_NE,
  MS_CMP_IN, MS_CMP_NOT_IN, MS_CMP_IS, MS_CMP_IS_NOT
} MsCompareOp;               // 两 token 运算符在此归一

typedef enum { MS_FOR_BARE, MS_FOR_CLAUSE, MS_FOR_IN } MsForKind;
typedef enum { MS_PARAM_NORMAL, MS_PARAM_VARARGS, MS_PARAM_KWARGS } MsParamKind;

struct MsAst {
  MsAstKind kind;
  uint32_t line;
  uint32_t column;
  union {
    struct { const char* start; size_t length; } literal;    // INT/FLOAT/STRING/.../IDENT：token 切片
    struct { const char* start; size_t length; } name;       // GLOBAL/DEL/KW_ARG 等的名字切片
    struct { MsTokenType op; struct MsAst* operand; } unary;
    struct { MsTokenType op; struct MsAst* left; struct MsAst* right; } binary;   // LOGICAL 同构复用
    struct { struct MsAstList operands; MsCompareOp* ops; size_t opCount; } compareChain;
    struct { struct MsAst* value; struct MsAst* cond; struct MsAst* elseValue; } conditional;
    struct { struct MsAst* callee; struct MsAstList args; } call;      // args 元素为表达式或 MS_AST_KW_ARG
    struct { struct MsAst* target; struct MsAst* index; } index;
    struct { struct MsAst* target; struct MsAst* start; struct MsAst* stop; struct MsAst* step; } slice;
    struct { struct MsAstList parts; } fstring;              // TEXT / EXPR 交替
    struct { struct MsAst* expr; const char* fmt; size_t fmtLen; bool hasFmt; } fstringExpr;
    struct { struct MsAstList clauses; struct MsAst* elem; struct MsAst* key; } comprehension;
    struct { struct MsAstList targets; struct MsAstList values; } decl;           // ASSIGN 同构复用
    struct { MsTokenType op; struct MsAst* target; struct MsAst* value; } augAssign;
    struct { struct MsAst* cond; struct MsAst* thenBlock; struct MsAst* elseBranch; } ifStmt;
    struct { MsForKind forKind; struct MsAst* init; struct MsAst* cond; struct MsAst* post;
             struct MsAstList targets; struct MsAst* iterable; struct MsAst* body; } forStmt;
    struct { struct MsAst* body; struct MsAstList excepts; struct MsAst* finallyBlock; } tryStmt;
    struct { struct MsAst* typeExpr; const char* name; size_t nameLen; struct MsAst* body; } exceptClause;
    struct { bool isAsync; bool isStatic; const char* name; size_t nameLen;
             struct MsAstList params; struct MsAst* body; } func;                 // lambda：name 为 NULL、body 为表达式
    struct { const char* name; size_t nameLen; MsParamKind paramKind; struct MsAst* defaultValue; } param;
    struct { const char* name; size_t nameLen; struct MsAst* superclass; struct MsAst* body; } classDecl;
    struct MsAstList block;                                  // SOURCE_FILE / BLOCK / SET_LIT / LIST_LIT / SELECT 等
  } as;
};
```

代表性分支如上；未列出的分支（`whileStmt`、`withStmt`、`raiseStmt`、import 各形式、select case 的 recv/send 字段等）按同一体例在头文件中补全，字段命名延续上表风格。

### parser 结构与公开入口

```c
struct MsParser {
  struct MsLexer* lexer;        // 借用，调用者持有
  struct MsAstArena* arena;     // 借用，节点唯一分配来源
  struct MsDiagList* diags;     // 借用，与 lexer 共享（单文件合计 20 条上限）
  struct MsToken current;       // 当前 token
  struct MsToken previous;      // 上一个 token（节点位置来源）
  bool panicMode;               // 抑制级联错误，直到 synchronize()
  int loopDepth;                // break/continue 语境检查
  int funcDepth;                // return 语境检查
  bool funcIsAsync;             // await 语境检查（当前最内层函数）
  int classDepth;               // self/super 语境检查
};

// lexer must be initialized by the caller and outlive the parser; arena and
// diags are borrowed. diags is typically the same list the lexer reports into
// (per-file cap of 20 applies to lexical and syntactic errors combined).
void msParserInit(struct MsParser* parser, struct MsLexer* lexer,
    struct MsAstArena* arena, struct MsDiagList* diags);

// Parses the whole source file. On success *out is an MS_AST_SOURCE_FILE node
// owned by arena. On failure returns MS_ERROR_SYNTAX (errors recorded in
// diags) or MS_ERROR_OOM; *out may still hold a partial tree for inspection.
MsResult msParserParse(struct MsParser* parser, struct MsAst** out);
```

内部助手（文件内 `static`，lowerCamelCase）：`parserAdvance`（取下一 token 并跳过 `MS_TOKEN_INVALID`，其诊断已由 lexer 记录）、`parserCheck` / `parserMatch` / `parserConsume(type, code, fmt, ...)`、`parserErrorAt(token, code, fmt, ...)`（panic 模式下静默）、`synchronize`（见「错误恢复」）。

### 语句解析（递归下降）

- `msParserParse` 循环调用 `parseTopLevelStmt` 至 `MS_TOKEN_EOF`，结果挂入根节点子表。
- 语句终止：每条简单语句结束后消费一个 `MS_TOKEN_SEMICOLON`（自动插入或显式）；语句起始位置遇到 `MS_TOKEN_SEMICOLON` 报 E204 空语句。块结尾语句（`if`/`for`/`func`/`class` 等以 `}` 收尾）之后可选消费一个自动插入的分号，不强制。
- 声明/赋值/表达式语句的区分采用**先解析后 reinterpret**：按表达式列表解析左侧，若随后出现 `:=`/`=`/复合赋值符，则校验每个左项的可赋值性（`MS_AST_IDENT`/`MS_AST_INDEX`/`MS_AST_ATTR`，`:=` 额外允许至多一个 `*ident` 解包目标），不可赋值报 E205，否则转换为 `MS_AST_DECL`/`MS_AST_ASSIGN`/`MS_AST_AUG_ASSIGN`；否则单个表达式落为 `MS_AST_EXPR_STMT`，多表达式列表无赋值符时报 E203。`:=`「至少一个新名字」与 `=`「目标已声明」需作用域信息，留给任务 07 检查，parser 不判。
- `for`：先看 `{`（裸循环）→ 否则解析首段，按随后的 `in` / `;` / `{` 分派迭代/三分/裸循环；`in` 目标列表与 `:=` 同规则。三分子句的 `simpleStmt` 允许声明、赋值、表达式语句与 `++`/`--` 语句。
- `++`/`--`：仅在三分 `for` 子句后置语句位置接受（生成 `MS_AST_INC_DEC`），其余位置报 E206。
- 语境检查用 `loopDepth`/`funcDepth`/`funcIsAsync`/`classDepth` 计数器，进入函数体时保存并复位（循环内的闭包不允许 `break` 穿透）：`break`/`continue` 在 `loopDepth == 0` 报 E209；`return` 在 `funcDepth == 0`（模块顶层）报 E210；`await` 在非 async 函数内报 E210；`self`/`super` 在 class 方法外报 E210。
- `func` 参数表：解析后校验次序（普通参数（可带默认值）→ `*ident` → `**ident`，03-syntax §4 固定次序）与重名，违例分别报 E207/E208；`lambda` 参数仅为标识符列表。
- class 体：成员为 `[static] func`（`static` 上下文关键字判定：当前 token 是标识符 `"static"` 且 `msLexerPeek` 为 `MS_TOKEN_KW_FUNC`）；`<` 后为任意表达式（超类）。`self`/`super` 的可用性由 `classDepth` 与「当前处于方法内」联合判定。
- `try`：`except` 子句的类型表达式、`as` 绑定均可空（裸 `except`）；`finally` 至多一个。`with`：`as` 绑定可空。`select`：按 06-concurrency §3 解析 `case`（recv 可带 `identList :=/=` 前缀，`send` 带实参）与 `default`。
- import：`import` 后必须是 `MS_TOKEN_STRING`（模块路径），可带 `as` 别名；`from` 形式的名字列表支持括号包围与尾逗号（03-syntax §7）。

### 表达式解析（Pratt）

核心为优先级表驱动的经典 Pratt 循环：

```c
typedef enum {
  MS_PREC_NONE,
  MS_PREC_LAMBDA,        // 15 lambda
  MS_PREC_CONDITIONAL,   // 14 a if cond else b（右结合）
  MS_PREC_OR,            // 13
  MS_PREC_AND,           // 12
  MS_PREC_COMPARE,       // 11 比较/in/is（链式收集，不真正左结合）
  MS_PREC_BIT_OR,        // 10 |
  MS_PREC_BIT_XOR,       // 9  ^
  MS_PREC_BIT_AND,       // 8  &
  MS_PREC_SHIFT,         // 7  << >>
  MS_PREC_TERM,          // 6  + -
  MS_PREC_FACTOR,        // 5  * / % div
  MS_PREC_UNARY,         // 4  +x -x ~x not x
  MS_PREC_POWER,         // 3  **（右结合）
  MS_PREC_POSTFIX,       // 2  [] 切片 属性 调用 await
  MS_PREC_PRIMARY        // 1  字面量/标识符/(expr)
} MsPrecedence;          // 枚举序即绑定力升序，对应 03-syntax §6 从高到低的 1–15 级

typedef struct MsAst* (*MsParseFn)(struct MsParser* p, bool canAssign);

struct MsParseRule {
  MsParseFn prefix;      // 前缀（nud），NULL 表示该 token 不能起始表达式
  MsParseFn infix;       // 中缀/后缀（led），NULL 表示非运算符
  MsPrecedence prec;
};

static struct MsAst* parsePrecedence(struct MsParser* p, MsPrecedence minPrec);
```

- `parsePrecedence`：取当前 token 的 prefix 规则解析首项，随后只要当前 token 的 infix 规则优先级 `>= minPrec` 就继续折叠。左结合运算符的右操作数以 `prec + 1` 递归；右结合（`**`、条件表达式）以 `prec` 递归。
- 规则表为文件级 `static const struct MsParseRule rules[MS_TOKEN_...]`，按 `MsTokenType` 索引；关键字运算符（`and`/`or`/`not`/`in`/`is`/`lambda`/`if` 条件式）同样入表。
- 链式比较：进入比较级后循环收集运算符与操作数，产出单个 `MS_AST_COMPARE_CHAIN`（`b` 只求值一次的展开属任务 07）。双 token 运算符借助 `msLexerPeek` 归一：`not` 后必须为 `in`（否则按一元 `not` 处理）、`is` 后可选 `not`，映射为 `MS_CMP_NOT_IN`/`MS_CMP_IS_NOT`。
- 后缀链：主表达式之后循环处理 `[`（下标或切片，切片按 `:` 分隔的三个可空分量）、`.`（属性，后跟标识符）、`(`（调用，实参为表达式列表，识别 `name = expr` 关键字实参——按标识符 + `=` 的 2 token 前瞻判定，与条件表达式不冲突因为此处 `=` 不是运算符）。调用点不引入 `*args`/`**kwargs` 展开语法（规范未列，列入路线图）。
- `await` 按 POSTFIX 级前缀解析（绑定力强于 `**`，`await f(x)` 等价 `await (f(x))`）。
- 切片三个分量均可缺省（`a[:]`），但 `[` `]` 内至少一个分量或冒号，空 `[]` 在下标位置报 E203。
- 推导式：`[`/`{` 解析首元素后遇 `for` 关键字切换为推导式：`for` 子句列表（`for targets in expr [if cond]`，支持多重 `for` 级联）挂入 `MS_AST_*_COMP` 的 `clauses`；dict 推导式首元素为 `k: v` 对。生成器表达式不在 v0.1 语法内（03-syntax §6.2 路线图说明）。

### f-string 组装

lexer 按任务 03 的约定把一条 f-string 展开为扁平 token 序列；parser 的 `MS_TOKEN_FSTRING_START` 前缀规则负责组装：

```
MS_TOKEN_FSTRING_START
  MS_TOKEN_STRING            → MS_AST_FSTRING_TEXT（词素切片原样保留，
                               {{ / }} 还原与转义解码属编译期 msLexerUnescape）
  MS_TOKEN_LEFT_BRACE
    <parsePrecedence 全级表达式>
    [ MS_TOKEN_FSTRING_FORMAT ]   → 原文切片存入 hasFmt/fmt（不展开嵌套字段，
                                    见「设计依据」歧义说明）
    MS_TOKEN_RIGHT_BRACE       → 合并为 MS_AST_FSTRING_EXPR
  ...
MS_TOKEN_FSTRING_END         → 终止
```

- 组装结果：`MS_AST_FSTRING` 节点的 `parts` 为 TEXT/EXPR 交替列表；`f""` 与无插值情形也统一走此结构（parts 可为单 TEXT）。
- 插值内的词法错误（E103/E110 等）已由 lexer 记录并产出 `MS_TOKEN_INVALID`；parser 侧遇 `INVALID` 跳过、遇 `EOF`/`FSTRING_END` 提前终止则报 E211 并同步到下一个分号。
- f-string 内嵌套 f-string 已由 lexer 的帧栈展开为连续 token 流，parser 无需特殊处理——内层 `FSTRING_START` 在表达式解析中作为普通前缀规则递归命中。

### 错误恢复与同步点

panic 模式（与诊断收集器协同）：

1. `parserErrorAt` 在 `panicMode == true` 时静默（抑制级联），否则经 `msDiagReport` 记录（文件/行/列/错误码/消息）并置 `panicMode = true`。
2. 语句级错误传播到 `parseTopLevelStmt`/`parseStatement` 边界的 `synchronize`：`panicMode = false`，然后丢弃 token 直至——
   - `MS_TOKEN_SEMICOLON`：消费后返回；
   - 语句起始关键字（`func` `class` `if` `for` `while` `try` `with` `select` `import` `from` `return` `raise` `del` `global` `break` `continue` `pass`）：不消费，直接返回；
   - `MS_TOKEN_RIGHT_BRACE`：不消费，交还块解析循环收尾；
   - `MS_TOKEN_EOF`：返回，主循环自然结束。
3. 表达式级错误产出 `MS_AST_INVALID` 占位节点（位置为出错 token），保证上层树结构完整；panic 期间已记录的 `INVALID` token 不重复报语法错误（其词法诊断已存在）。
4. 诊断达 20 条上限后，lexer 侧开始返回 `MS_ERROR_SYNTAX` + 强制 EOF，parser 检测到该状态立即停止解析，`msParserParse` 返回 `MS_ERROR_SYNTAX`。

错误码表（parser 段，E2xx；E1xx 属 lexer）：

| 错误码 | 情形 |
|---|---|
| E201 | 期望表达式（前缀规则缺失的 token 出现在表达式位置） |
| E202 | 意外的 token（语句起始位置无法分派） |
| E203 | 期望指定 token（消息模板含期望与实际，如缺 `)` `]` `:`、import 路径非字符串） |
| E204 | 空语句（语句起始位置的分号） |
| E205 | 非法赋值/声明目标 |
| E206 | `++`/`--` 出现在三分 `for` 子句之外 |
| E207 | 参数次序非法（默认参数/`*`/`**` 排列违反固定次序） |
| E208 | 参数重名 |
| E209 | `break`/`continue` 在循环外 |
| E210 | 语境非法（顶层 `return`、非 async 内 `await`、方法外 `self`/`super`） |
| E211 | 块/构造未闭合（期望 `}` 或 f-string 插值未闭合） |
| E212 | 解包目标非法（`*` 目标多于一个或不在 `:=`/`for-in` 中） |

## 实现步骤

1. 建 `src/parser/ms_ast.h` / `ms_ast.c`：`MsAstKind` 全量枚举、`struct MsAst` 联合、arena（`msAstArenaInit`/`msAstNew`/`msAstListPush`/`msAstArenaDestroy`）、`msAstKindName`。验证：单元测试断言枚举与名称表一一对应；arena 多块增长后 `Destroy` 无泄漏（任务 02 分配统计归零）。
2. 实现 `msAstDump`（S 表达式转储）。验证：手工构造小树的输出与期望文本一致——此后全部 parser 测试用 dump 比对。
3. parser 骨架：`msParserInit`、token 推进助手（`parserAdvance` 跳过 `INVALID`、`parserCheck`/`parserMatch`/`parserConsume`）、`parserErrorAt` 与 panic 抑制、`msParserParse` 主循环。验证：空源码产出空 `MS_AST_SOURCE_FILE`；单条字面量表达式语句的 dump。
4. Pratt 核心：`MsPrecedence` 枚举、规则表、`parsePrecedence`。先接入字面量、标识符、括号、一元/二元/逻辑运算符。验证：优先级与结合性用例（`1 + 2 * 3`、`2 ** 3 ** 4`、`not a and b`）的树形 dump。
5. 后缀与比较：调用/下标/切片/属性链、关键字实参、链式比较与 `not in`/`is not` 归一、条件表达式、`await`、`lambda`。验证：`a.b[i:j](k, x=1)`、`a < b <= c`、`-2 ** 2`（`-` 绑定力低于 `**`）等边界用例。
6. 容器与推导式：list/dict/set 字面量、空 `{}` 归 dict、三种推导式与多重 `for` 级联、`if` 过滤子句。验证：各形式 dump。
7. 语句分派：`parseStatement` 全语句表、分号终止与空语句 E204、块解析、块后可空分号。声明/赋值的先解析后 reinterpret（含 `*ident` 解包目标、可赋值性校验 E205/E212）。验证：`a, b = b, a`、`a, *rest := xs`、连续 `;;` 报 E204。
8. 复合语句：`if`/`else if` 链、`for` 三分派（含 `++`/`--` 语境限制 E206）、`while`、`try`/`except`/`finally`、`with`、`select`、import 三形式、`func`（async、参数次序 E207/重名 E208）、`class`（`static` 上下文关键字、`<` 超类）、语境计数器检查（E209/E210）。验证：每构造至少一个正例 dump + 对应负例诊断码。
9. f-string 组装：`FSTRING_START` 前缀规则、TEXT/EXPR 交替 parts、格式说明切片保留、嵌套 f-string 递归命中、未闭合恢复。验证：任务 03 展开序列的逐条组装结果（含 `{pi:.2f}`、嵌套 `f"{f'{x}'}"`）。
10. 错误恢复与上限：`synchronize` 同步点、`MS_AST_INVALID` 占位、级联抑制、20 条上限后停止。验证：单文件多错误源码的诊断序列（无级联重复）、AST 仍可遍历、上限中止行为。

## 测试方案

本任务早于最小可运行解释器（任务 09），只能用 C 单元测试。测试文件 `tests/c/test_parser.c`（本任务只交付本设计文档，测试代码随实现任务编写），使用任务 02 的 `ms_test.h`。每个用例以源码字符串初始化 lexer + arena + parser，解析后用 `msAstDump` 输出与期望 S 表达式文本比对，再断言诊断列表（错误码/行/列）；负例同时断言 `msParserParse` 返回值。

覆盖清单：

- 基础：空源码；纯表达式语句；显式分号与同行多语句；`;;` 报 E204；CRLF 源码行列正确。
- 优先级与结合性：15 级各取代表组合成链（如 `a or b and c`、`1 + 2 << 3`、`a | b ^ c & d`）；`**` 右结合；`-2 ** 2` 的一元/幂次序；条件表达式右结合（`a if c1 else b if c2 else d`）；`await f(x) + 1` 的绑定次序。
- 后缀链：调用（空参/多参/尾逗号/关键字实参）、下标、切片全组合（`a[:]`、`a[::2]`、`a[1:]`）、属性链、混合链 `m.a[0](x).b`。
- 比较：`a < b <= c == d` 单链节点；`in`/`not in`/`is`/`is not` 归一为 `MsCompareOp`；`a not in b` 与 `not a in b` 的区分。
- 字面量：int/float/string/raw/bytes/bool/nil 节点种类与原文切片；`{{`/`}}` 之外的容器字面量、空 `{}` 为 dict。
- 推导式：list/set/dict 三种、单 `if` 过滤、多重 `for` 级联、负例（`for` 缺 `in`）。
- f-string：空串、纯文本、单插值、多插值、带格式说明、格式说明原文含嵌套 `{...}` 时存原样切片、插值内嵌套 f-string、插值内任意表达式（调用/切片）。
- 语句：`if`/`else if`/`else` 全形态；`for` 三形态与 `i++` 后置；`while`；`try` 多 `except` + `finally`、裸 `except`、`except T as e`；`with ... as`；`select` 的 recv/send/default case；`return` 多值、`raise ... from ...`、`del` 多目标、`global` 多名字；多重赋值与交换、复合赋值全运算符、解包声明。
- 声明：`func` 默认参数/`*`/`**` 组合与次序违例 E207、重名 E208；`async func`；`lambda` 空参与多参；`class` 无超类/有超类/`static func`；`static` 在 class 体外是普通标识符（`static := 1` 合法）。
- 语境检查：顶层 `return` E210、循环外 `break` E209、函数内闭包重置（循环中的 `func` 体内 `break` 报 E209）、非 async 内 `await` E210、方法外 `self` E210；`for` 外 `i++` E206。
- 错误恢复：一条坏语句后后续语句正常解析且树完整（含 `MS_AST_INVALID`）；级联抑制（一行内连续错误只报首条）；块内错误同步到 `}` 后外层继续；21+ 处错误时仅 20 条诊断且解析中止；lexer 产出 `INVALID` 的位置 parser 不重复报错。

## 验收标准

- [ ] `src/parser/ms_ast.{h,c}` 与 `src/parser/ms_parser.{h,c}` 存在，guard 分别为 `MSLANG_SRC_PARSER_MS_AST_H_` / `MSLANG_SRC_PARSER_MS_PARSER_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef）。
- [ ] AST 以类型标签 + 联合表示，覆盖 03-syntax 全量语法与 06-concurrency §3 的 select；字面量与标识符存源码切片不求值；全部节点经 arena 分配，`msAstArenaDestroy` 后任务 02 分配统计归零。
- [ ] 表达式解析实现 15 级优先级表的全部结合性规则，含 `**` 右结合、链式比较单节点收集、`not in`/`is not` 归一、条件表达式、`lambda`、`await`、切片与调用（含关键字实参）。
- [ ] f-string 扁平 token 序列按本文约定组装为 `MS_AST_FSTRING`（TEXT/EXPR parts、格式说明切片保留、嵌套 f-string 递归）。
- [ ] 语句与声明全覆盖，含 `for` 三形态、`try`/`with`/`select`、import 三形式、`async func`、`static func` 上下文关键字、解包目标；`++`/`--` 仅限三分 `for` 子句。
- [ ] 语法错误经共享诊断收集器记录（E201–E212，文件/行/列），panic 模式同步到分号/语句关键字/`}`，级联抑制，与词法错误合计满 20 条后中止并返回 `MS_ERROR_SYNTAX`。
- [ ] `tests/c/test_parser.c` 覆盖「测试方案」全部清单项并全部通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 02/03 的接口假定（`struct MsDiagList`、`msDiagReport`、`msLexerPeek` 等）在实现时已对齐。
