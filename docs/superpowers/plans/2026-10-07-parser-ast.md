# Parser & AST (Task 04) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver the mslang parser + AST module (`src/parser/ms_ast.{h,c}` and `src/parser/ms_parser.{h,c}`) that turns the task-03 token stream into a full-syntax AST per `docs/language/03-syntax.md`, verified by C unit tests in `tests/c/test_parser.c`.

**Architecture:** Arena-allocated AST (tagged union `struct MsAst`, zero-copy source slices). Recursive descent for statements, precedence-table-driven Pratt parsing for expressions (15 levels per 03-syntax section 6). Panic-mode error recovery with statement-level synchronization; diagnostics share the lexer's `MsDiagList` (combined cap of 20).

**Tech Stack:** C11, CMake + Ninja (existing `build/`), task-02 core infra (`msAlloc/msFree`, `MsDiagList`, `MS_ASSERT`), task-03 lexer (`msLexerNext/msLexerPeek`), self-built `tests/c/ms_test.h`.

**Spec (authoritative, read fully before each task):** `docs/tasks/04-parser-ast.md` — it contains the complete interface design: the `MsAstKind` enum, the `struct MsAst` union shape, `struct MsParser` fields, public signatures, the E201–E212 error-code table, and the error-recovery algorithm. Grammar source of truth: `docs/language/03-syntax.md` (statements, expressions, 15-level precedence table, import forms) and `docs/language/06-concurrency.md` section 3 (`selectStmt`/`selectCase`/`recvStmt`/`sendStmt` EBNF). Do NOT deviate from those definitions; this plan pins only the gaps the spec explicitly leaves open ("按同一体例补全" branches, dump format, representation choices).

**Style (binding):** `docs/language/10-c-style.md` — 2-space indent, 120 col, K&R braces, `Type* name` (star to type), internal structs not typedef'd, `//` comments only (English), includes ordered: own header → C system → project headers, guards `MSLANG_SRC_PARSER_MS_AST_H_` / `MSLANG_SRC_PARSER_MS_PARSER_H_`. Source files must be pure ASCII (MSVC C4819 breaks the build otherwise).

**Task-02/03 interfaces (verified against the headers, use exactly these):**
- `src/core/ms_memory.h`: `void* msAlloc(size_t)`, `void* msRealloc(void*, size_t)`, `void msFree(void*)`, `struct MsMemStats` (fields `liveBlocks`, `currentBytes`, ...), `msMemGetStats`, `msMemResetStats`, `msMemSetFailAfter(int64_t)`.
- `src/core/ms_diag.h`: `struct MsDiagList`, `msDiagListInit(list, chunkName)`, `msDiagListDestroy`, `bool msDiagReport(list, line, column, code, fmt, ...)` (code is the numeric part: E201 → `201`; returns false when the 20-entry cap is full), `msDiagListCount`, `msDiagListAt` → `struct MsDiag` with fields `line/column/code/message`.
- `src/core/ms_common.h`: `MS_ASSERT`, `MS_ARRAY_LEN`, `MS_UNREACHABLE`.
- `include/mslang/error.h`: `MsResult` = `MS_OK` / `MS_ERROR_RUNTIME` / `MS_ERROR_SYNTAX` / `MS_ERROR_OOM`.
- `src/lexer/ms_lexer.h`: `struct MsLexer`, `struct MsToken { type, start, length, line, column }`, `msLexerInit(lexer, source, sourceLen, chunkName, diags)`, `msLexerDestroy`, `MsResult msLexerNext(lexer, &token)` (returns `MS_ERROR_SYNTAX` once the diag cap is hit, token set to `MS_TOKEN_EOF` — parser must stop), `MsTokenType msLexerPeek(lexer)` (1-token lookahead; parser's `current` + this = 2-token lookahead), `msTokenTypeName`. Full `MsTokenType` enum: see the header; note `MS_TOKEN_KW_DIV` (floor division keyword), `MS_TOKEN_KW_SELF` / `MS_TOKEN_KW_SUPER` are real keywords, f-string tokens are `MS_TOKEN_FSTRING_START` / `MS_TOKEN_STRING` (text segments) / `MS_TOKEN_LEFT_BRACE` / `MS_TOKEN_FSTRING_FORMAT` / `MS_TOKEN_RIGHT_BRACE` / `MS_TOKEN_FSTRING_END`, and `MS_TOKEN_INVALID` may have an empty lexeme.

**Build / test commands (run from the worktree root):**
```bash
cmake -S . -B build                 # re-configure after CMakeLists.txt changes
cmake --build build                 # incremental build (ninja + MSVC /W4 /WX)
ctest --test-dir build --output-on-failure -R test_parser   # parser tests only
ctest --test-dir build --output-on-failure                  # full suite (must stay green)
```

**Commit convention (AGENTS.md, binding):** `<gitmoji> <type>(<scope>): <message>`. Use `✨ feat(parser):` for implementation steps, `✅ test(parser):` for tests-only commits.

---

## Spec-gap pins (decisions this plan makes where the spec delegates)

1. **`struct MsAstList` gains a `capacity` field** (`items`/`count`/`capacity`) — capacity doubling is impossible otherwise. Arena arrays never shrink, never individually freed.
2. **Arena blocks use a C11 flexible array member** `uint8_t data[]`; each block is `msAlloc(sizeof(struct MsAstArenaBlock) + max(needed, MS_AST_ARENA_BLOCK_SIZE))`. All arena allocations are rounded up to 16 bytes (`MS_AST_ARENA_ALIGN`); `msAlloc` (malloc) guarantees 16-byte alignment on Win64, and every AST type's alignment is <= 8, so this is safe. Add `void* msAstArenaAlloc(struct MsAstArena* arena, size_t size)` as a public primitive (the parser needs it for the compare-chain ops array); `msAstNew`/`msAstListPush` are built on it.
3. **Union branches the spec leaves open are pinned in Task 1's full header listing** (while/with/raise/import/select-case/global/del/return branches). Notable representations:
   - Unpack target `*ident` (in `:=` and `for-in`): `MS_AST_UNARY` with `op == MS_TOKEN_STAR` and operand an `MS_AST_IDENT`. The spec's enum has no unpack kind and forbids growing it later; this keeps it closed.
   - `GLOBAL` names and `FROM_IMPORT` names are `MS_AST_IDENT` child nodes in a list (a plain name-slice cannot represent `global x, y`).
   - `AWAIT` gets its own `awaitExpr` branch (operand only).
   - Comprehension clauses reuse `MS_AST_FOR` (`forKind == MS_FOR_IN`, `body == NULL`) and `MS_AST_IF` (`thenBlock`/`elseBranch == NULL`) nodes in the `clauses` list.
   - `MS_AST_INC_DEC` reuses the `augAssign` branch with `value == NULL`.
   - select case: `MsSelectCaseKind { MS_SELECT_RECV, MS_SELECT_SEND, MS_SELECT_DEFAULT }`; `case ch.recv()` decomposes into `channel` = `ch` subtree + optional `targets`; `case ch.send(v)` into `channel` + `value`; `default` has neither. `isDecl` records `:=` vs `=`.
4. **Dump format** is pinned in Task 2 (golden tests depend on byte-exact output).
5. **Pratt recursion rules** (Task 4/5): left-assoc infix parses its right operand at `prec + 1`; right-assoc (`**`, conditional `else` branch) at `prec`; unary operand at `MS_PREC_UNARY + 1` (so `-2 ** 2` == `-(2 ** 2)` but `-2 * 3` == `(-2) * 3`, matching 03-syntax section 6 where unary is level 4 and `**` level 3); `await` operand at `MS_PREC_POSTFIX`; conditional's `cond` at `MS_PREC_OR`; lambda body at `MS_PREC_CONDITIONAL` (a bare nested lambda needs parens).
6. **Pratt loop condition:** fold while the current token's infix rule has `prec >= minPrec`; right-associativity falls out of the recursion rule above.
7. **`not in` / `is not`:** at COMPARE level, current `MS_TOKEN_KW_NOT` + `msLexerPeek == MS_TOKEN_KW_IN` → `MS_CMP_NOT_IN` (a leading `not` NOT followed by `in` is unary `not`, handled by the prefix rule at UNARY level — so `not a in b` parses as `(not a) in b` per the MS precedence table, which differs from Python deliberately); `MS_TOKEN_KW_IS` + optional following `KW_NOT` → `MS_CMP_IS` / `MS_CMP_IS_NOT`.
8. **Keyword argument detection in calls:** current token `MS_TOKEN_IDENTIFIER` + `msLexerPeek == MS_TOKEN_EQUAL` → parse `name = expr` as `MS_AST_KW_ARG`; otherwise parse a normal expression. (`==` never collides: peek would be `MS_TOKEN_EQUAL_EQUAL`.)
9. **`self` / `super`** parse as `MS_AST_IDENT` nodes (lexemes `"self"` / `"super"`); the context check (E210) fires when used outside a class method. Additive parser field `bool inMethod` (lexer precedent: additive fields beyond the spec struct are allowed and documented with a comment).
10. **`func` / `class` declarations are allowed as statements in any block**, not only at top level. The EBNF in 03-syntax section 2 (`topLevelStmt = statement | funcDecl | classDecl`) is silent about nesting, but the task spec's test plan explicitly requires "循环中的 func 体内 break 报 E209" (closure barrier inside a loop), which is only expressible if nested `func` parses. Ruling: `parseStatement` dispatches `func`/`async func`/`class` directly; `parseTopLevelStmt` is just the top-level call site of the same dispatch. This also matches the "Python 的表达力" goal (nested named functions as closures). Entering a func body resets `loopDepth` to 0 (closures cannot break an outer loop).
11. **Empty statement:** a `MS_TOKEN_SEMICOLON` at statement-start position → E204 (covers both `;;` and line-leading `;`, since both surface as a semicolon token where a statement is expected).
12. **Simple-statement termination:** after a simple statement, consume one `MS_TOKEN_SEMICOLON`; missing → E203 "expected ';'". Compound statements ending in `}` optionally consume one following semicolon (auto-inserted at the newline after `}`); more than one → the extra hits E204 via pin 11.
13. **`div`** (`MS_TOKEN_KW_DIV`) is a FACTOR-level infix operator producing `MS_AST_BINARY` with `op == MS_TOKEN_KW_DIV`. There is no `//` operator token (line comment).
14. **f-string format spec:** a `MS_TOKEN_FSTRING_FORMAT` token's lexeme slice is stored verbatim (`fmt`/`fmtLen`, `hasFmt = true`), never expanded (task 28's job).

---

## File Structure

| File | Responsibility |
|---|---|
| `src/parser/ms_ast.h` | `MsAstKind`, `MsCompareOp`, `MsForKind`, `MsParamKind`, `MsSelectCaseKind`, `struct MsAstList`, `struct MsAst`, arena types, public function declarations. Self-contained header. |
| `src/parser/ms_ast.c` | Arena (block list, aligned bump allocation), `msAstNew`, `msAstListPush`, `msAstKindName`, `msAstDump` (+ static compare-op name table). |
| `src/parser/ms_parser.h` | `struct MsParser`, `msParserInit`, `msParserParse`. Self-contained header. |
| `src/parser/ms_parser.c` | Recursive-descent statements + Pratt expressions. All helpers file-local `static`. No heap allocation outside the arena. |
| `tests/c/test_parser.c` | All unit tests (harness below + one `MS_TEST` per case family). |
| `CMakeLists.txt` | Add the three new sources to the `mslang` library; register `mslang_add_c_test(test_parser)`. |

---

## Shared test harness (Task 3 creates it inside `tests/c/test_parser.c`; every later task extends the same file)

```c
#include <stdio.h>
#include <string.h>

#include "core/ms_diag.h"
#include "core/ms_memory.h"
#include "lexer/ms_lexer.h"
#include "parser/ms_ast.h"
#include "parser/ms_parser.h"
#include "ms_test.h"

#define MS_TEST_DUMP_CAP 65536

typedef struct {
  struct MsDiagList diags;
  struct MsAstArena arena;
  struct MsLexer lexer;
  struct MsParser parser;
  struct MsAst* root;
  MsResult result;
} MsTestFixture;

// Parses source end-to-end into fix. Call msTestFixtureDestroy afterwards.
static void msTestParse(MsTestFixture* fix, const char* source) {
  msDiagListInit(&fix->diags, "test.ms");
  msAstArenaInit(&fix->arena);
  msLexerInit(&fix->lexer, source, strlen(source), "test.ms", &fix->diags);
  msParserInit(&fix->parser, &fix->lexer, &fix->arena, &fix->diags);
  fix->root = NULL;
  fix->result = msParserParse(&fix->parser, &fix->root);
  msLexerDestroy(&fix->lexer);
}

static void msTestFixtureDestroy(MsTestFixture* fix) {
  msAstArenaDestroy(&fix->arena);
  msDiagListDestroy(&fix->diags);
}

// Dumps root into buf (NUL-terminated).
static void msTestDump(const struct MsAst* root, char* buf, size_t bufSize) {
  FILE* out = tmpfile();
  MS_ASSERT_TRUE(out != NULL);
  msAstDump(root, out);
  rewind(out);
  size_t n = fread(buf, 1, bufSize - 1, out);
  buf[n] = '\0';
  fclose(out);
}

// Asserts: parse succeeds, zero diagnostics, dump matches expected exactly.
static void msTestExpectDump(const char* source, const char* expected) {
  MsTestFixture fix;
  msTestParse(&fix, source);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(0, (long long)msDiagListCount(&fix.diags));
  char buf[MS_TEST_DUMP_CAP];
  msTestDump(fix.root, buf, sizeof(buf));
  if (strcmp(buf, expected) != 0) {
    printf("--- expected ---\n%s--- actual ---\n%s---\n", expected, buf);
    MS_ASSERT_TRUE(false);
  }
  msTestFixtureDestroy(&fix);
}

// Asserts: parse fails with MS_ERROR_SYNTAX and the diagnostic codes in
// order equal expectedCodes[0..expectedCount).
static void msTestExpectDiags(const char* source, const uint32_t* expectedCodes,
    size_t expectedCount) {
  MsTestFixture fix;
  msTestParse(&fix, source);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ((long long)expectedCount, (long long)msDiagListCount(&fix.diags));
  size_t n = msDiagListCount(&fix.diags);
  if (n > expectedCount) {
    n = expectedCount;
  }
  for (size_t i = 0; i < n; ++i) {
    MS_ASSERT_EQ((long long)expectedCodes[i],
        (long long)msDiagListAt(&fix.diags, i)->code);
  }
  msTestFixtureDestroy(&fix);
}
```

Convention for every test below: expected dump strings are written as C adjacent-string-literal concatenation, one line of dump per literal, each literal ending in `\n`, e.g.:

```c
msTestExpectDump("1",
    "MS_AST_SOURCE_FILE\n"
    "  MS_AST_EXPR_STMT\n"
    "    MS_AST_INT \"1\"\n");
```

---

## Task 1: AST module — types, arena, kind names

**Files:**
- Create: `src/parser/ms_ast.h`
- Create: `src/parser/ms_ast.c`
- Test: `tests/c/test_parser.c` (starts with harness-minus-parser parts; see note below)
- Modify: `CMakeLists.txt` (library sources + `mslang_add_c_test(test_parser)`)

Note: the full harness above references `ms_parser.h`, which only exists from Task 3. For Tasks 1–2 write `tests/c/test_parser.c` with only the includes it needs (`core/ms_memory.h`, `parser/ms_ast.h`, `ms_test.h`); Task 3 grows the file into the full harness.

- [ ] **Step 1: Write `src/parser/ms_ast.h` in full** (this is the complete pinned header — the union branches the spec leaves open are resolved here per "Spec-gap pins"):

```c
#ifndef MSLANG_SRC_PARSER_MS_AST_H_
#define MSLANG_SRC_PARSER_MS_AST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lexer/ms_lexer.h"

typedef enum {
  MS_AST_INVALID,            // error placeholder (diagnostic already recorded)

  // Literals and primaries
  MS_AST_INT,                // raw slice, not converted (arbitrary precision)
  MS_AST_FLOAT,
  MS_AST_STRING,             // "..." (escapes not decoded)
  MS_AST_RAW_STRING,         // `...`
  MS_AST_BYTES,              // b"..."
  MS_AST_BOOL,               // true / false
  MS_AST_NIL,
  MS_AST_IDENT,
  MS_AST_FSTRING,            // parts: TEXT / EXPR alternating
  MS_AST_FSTRING_TEXT,       // f-string text segment
  MS_AST_FSTRING_EXPR,       // f-string interpolation {expr[:format]}
  MS_AST_LIST_LIT,
  MS_AST_DICT_LIT,           // key/value pairs interleaved in child list ({} is dict)
  MS_AST_SET_LIT,
  MS_AST_LIST_COMP,
  MS_AST_DICT_COMP,
  MS_AST_SET_COMP,

  // Operator expressions
  MS_AST_UNARY,              // +x -x ~x not x; also unpack target (op == MS_TOKEN_STAR)
  MS_AST_BINARY,             // arithmetic / bitwise (div is op == MS_TOKEN_KW_DIV)
  MS_AST_COMPARE_CHAIN,      // chained comparison (in / not in / is / is not included)
  MS_AST_LOGICAL,            // and / or (short-circuit)
  MS_AST_CONDITIONAL,        // a if cond else b
  MS_AST_AWAIT,
  MS_AST_LAMBDA,
  MS_AST_CALL,
  MS_AST_KW_ARG,             // name=expr inside a call
  MS_AST_INDEX,              // a[i]
  MS_AST_SLICE,              // a[i:j:k], each component optional
  MS_AST_ATTR,               // a.b

  // Statements
  MS_AST_SOURCE_FILE,        // root
  MS_AST_BLOCK,
  MS_AST_DECL,               // identList := exprList (at most one *ident unpack target)
  MS_AST_ASSIGN,             // targetList = exprList
  MS_AST_AUG_ASSIGN,         // += -= *= ... >>= (op keeps the token type)
  MS_AST_EXPR_STMT,
  MS_AST_INC_DEC,            // i++ / i-- (three-part for post statement only)
  MS_AST_IF,                 // else-if chain: elseBranch holds another MS_AST_IF
  MS_AST_FOR,                // forKind: bare / three-part clause / for-in
  MS_AST_WHILE,
  MS_AST_TRY,                // excepts is a list of MS_AST_EXCEPT_CLAUSE
  MS_AST_EXCEPT_CLAUSE,
  MS_AST_WITH,
  MS_AST_SELECT,             // child list of MS_AST_SELECT_CASE
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

  // Declarations
  MS_AST_FUNC_DECL,          // async / static via flags
  MS_AST_PARAM,              // normal (optional default) / *varargs / **kwargs
  MS_AST_CLASS_DECL
} MsAstKind;

typedef enum {
  MS_CMP_LT, MS_CMP_LE, MS_CMP_GT, MS_CMP_GE, MS_CMP_EQ, MS_CMP_NE,
  MS_CMP_IN, MS_CMP_NOT_IN, MS_CMP_IS, MS_CMP_IS_NOT
} MsCompareOp;               // two-token operators are normalized into this

typedef enum { MS_FOR_BARE, MS_FOR_CLAUSE, MS_FOR_IN } MsForKind;
typedef enum { MS_PARAM_NORMAL, MS_PARAM_VARARGS, MS_PARAM_KWARGS } MsParamKind;
typedef enum { MS_SELECT_RECV, MS_SELECT_SEND, MS_SELECT_DEFAULT } MsSelectCaseKind;

struct MsAst;

struct MsAstList {           // variable-length child table (block statements, call args)
  struct MsAst** items;      // arena-allocated, capacity doubling
  size_t count;
  size_t capacity;           // additive beyond the spec: doubling needs it
};

struct MsAst {
  MsAstKind kind;
  uint32_t line;
  uint32_t column;
  union {
    // INT / FLOAT / STRING / RAW_STRING / BYTES / IDENT / FSTRING_TEXT:
    // raw source slice, not copied, not decoded.
    struct { const char* start; size_t length; } literal;
    struct { bool value; } boolLit;                                 // BOOL
    struct { MsTokenType op; struct MsAst* operand; } unary;        // UNARY
    struct { MsTokenType op; struct MsAst* left; struct MsAst* right; } binary;  // BINARY, LOGICAL
    struct { struct MsAstList operands; MsCompareOp* ops; size_t opCount; } compareChain;
    struct { struct MsAst* value; struct MsAst* cond; struct MsAst* elseValue; } conditional;
    struct { struct MsAst* operand; } awaitExpr;                    // AWAIT
    struct { struct MsAst* callee; struct MsAstList args; } call;   // CALL (args are exprs or KW_ARG)
    struct { const char* start; size_t length; struct MsAst* value; } kwArg;
    struct { struct MsAst* target; struct MsAst* index; } index;
    struct { struct MsAst* target; struct MsAst* start; struct MsAst* stop; struct MsAst* step; } slice;
    struct { const char* start; size_t length; struct MsAst* target; } attr;
    struct { struct MsAstList parts; } fstring;                     // FSTRING
    struct { struct MsAst* expr; const char* fmt; size_t fmtLen; bool hasFmt; } fstringExpr;
    struct { struct MsAstList clauses; struct MsAst* elem; struct MsAst* key; } comprehension;
    struct { struct MsAstList targets; struct MsAstList values; } decl;      // DECL, ASSIGN
    struct { struct MsAst* expr; } exprStmt;                    // EXPR_STMT
    struct { MsTokenType op; struct MsAst* target; struct MsAst* value; } augAssign;  // AUG_ASSIGN, INC_DEC (value NULL)
    struct { struct MsAst* cond; struct MsAst* thenBlock; struct MsAst* elseBranch; } ifStmt;
    struct { MsForKind forKind; struct MsAst* init; struct MsAst* cond; struct MsAst* post;
             struct MsAstList targets; struct MsAst* iterable; struct MsAst* body; } forStmt;
    struct { struct MsAst* cond; struct MsAst* body; } whileStmt;
    struct { struct MsAst* body; struct MsAstList excepts; struct MsAst* finallyBlock; } tryStmt;
    struct { struct MsAst* typeExpr; const char* name; size_t nameLen; struct MsAst* body; } exceptClause;
    struct { struct MsAst* context; const char* name; size_t nameLen; struct MsAst* body; } withStmt;
    struct { MsSelectCaseKind caseKind; bool isDecl; struct MsAstList targets;
             struct MsAst* channel; struct MsAst* value; struct MsAst* body; } selectCase;
    struct { const char* path; size_t pathLen;
             const char* alias; size_t aliasLen; } importStmt;      // IMPORT (alias NULL when absent)
    struct { const char* path; size_t pathLen; struct MsAstList names; } fromImport;  // names: IDENT nodes
    struct { struct MsAstList values; } returnStmt;
    struct { struct MsAst* value; struct MsAst* fromValue; } raiseStmt;
    struct { struct MsAstList targets; } delStmt;
    struct { struct MsAstList names; } globalStmt;                  // GLOBAL, names: IDENT nodes
    struct { bool isAsync; bool isStatic; const char* name; size_t nameLen;
             struct MsAstList params; struct MsAst* body; } func;   // FUNC_DECL; LAMBDA: name NULL, body is expr
    struct { const char* name; size_t nameLen; MsParamKind paramKind;
             struct MsAst* defaultValue; } param;
    struct { const char* name; size_t nameLen; struct MsAst* superclass;
             struct MsAst* body; } classDecl;
    struct MsAstList block;   // SOURCE_FILE / BLOCK / LIST_LIT / SET_LIT / DICT_LIT (k,v interleaved) / SELECT
  } as;
};

#define MS_AST_ARENA_BLOCK_SIZE 4096
#define MS_AST_ARENA_ALIGN 16

struct MsAstArenaBlock {
  struct MsAstArenaBlock* next;
  size_t used;
  size_t capacity;           // byte length of data[] (varies per block)
  uint8_t data[];            // flexible array member; block is
                             // msAlloc(sizeof(struct MsAstArenaBlock) + capacity)
};

struct MsAstArena {
  struct MsAstArenaBlock* head;
  size_t totalAllocated;     // statistics and test assertions
};

// Arena lifecycle: the arena owns every node it hands out; one
// msAstArenaDestroy reclaims everything. Nodes borrow slices of the caller's
// source buffer -- the AST must not outlive that buffer.
void msAstArenaInit(struct MsAstArena* arena);
void msAstArenaDestroy(struct MsAstArena* arena);

// Raw arena allocation (16-byte aligned, NOT zeroed). Returns NULL on OOM.
void* msAstArenaAlloc(struct MsAstArena* arena, size_t size);

// Allocates a zeroed node of the given kind. Returns NULL on OOM (caller
// converts to MS_ERROR_OOM).
struct MsAst* msAstNew(struct MsAstArena* arena, MsAstKind kind, uint32_t line, uint32_t column);

// Appends to an arena-managed child list (capacity doubling). false on OOM.
bool msAstListPush(struct MsAstArena* arena, struct MsAstList* list, struct MsAst* node);

// Static name table for diagnostics and tests ("MS_AST_IF" etc.).
const char* msAstKindName(MsAstKind kind);

// Writes an S-expression dump of the tree (one node per line, 2-space
// indent) for golden-file style unit tests. Format pinned in Task 2.
void msAstDump(const struct MsAst* node, FILE* out);

#endif  // MSLANG_SRC_PARSER_MS_AST_H_
```

- [ ] **Step 2: Write the failing tests** in `tests/c/test_parser.c` (initial version: includes `core/ms_memory.h`, `parser/ms_ast.h`, `ms_test.h` only):

```c
MS_TEST(parser, kindNameTable) {
  // Every enum value has a distinct non-NULL name starting with "MS_AST_".
  for (int kind = MS_AST_INVALID; kind <= MS_AST_CLASS_DECL; ++kind) {
    const char* name = msAstKindName((MsAstKind)kind);
    MS_ASSERT_TRUE(name != NULL);
    MS_ASSERT_TRUE(strncmp(name, "MS_AST_", 7) == 0);
  }
  MS_ASSERT_EQ(0, strcmp(msAstKindName(MS_AST_SOURCE_FILE), "MS_AST_SOURCE_FILE"));
  MS_ASSERT_EQ(0, strcmp(msAstKindName(MS_AST_CLASS_DECL), "MS_AST_CLASS_DECL"));
}

MS_TEST(parser, arenaMultiBlock) {
  msMemResetStats();
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  // Far more nodes than one 4096-byte block holds: forces block growth.
  struct MsAst* last = NULL;
  for (int i = 0; i < 400; ++i) {
    last = msAstNew(&arena, MS_AST_INT, (uint32_t)i + 1, 1);
    MS_ASSERT_TRUE(last != NULL);
  }
  MS_ASSERT_TRUE(arena.totalAllocated >= 2 * MS_AST_ARENA_BLOCK_SIZE);
  MS_ASSERT_EQ(MS_AST_INT, last->kind);
  MS_ASSERT_EQ(400, (long long)last->line);
  // Zeroed allocation: the union starts zeroed.
  MS_ASSERT_TRUE(last->as.literal.start == NULL);
  msAstArenaDestroy(&arena);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(parser, arenaListPushGrowth) {
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  struct MsAstList list = { NULL, 0, 0 };
  for (int i = 0; i < 100; ++i) {
    struct MsAst* node = msAstNew(&arena, MS_AST_NIL, 1, 1);
    MS_ASSERT_TRUE(msAstListPush(&arena, &list, node));
    MS_ASSERT_TRUE(list.items[i] == node);
  }
  MS_ASSERT_EQ(100, (long long)list.count);
  MS_ASSERT_TRUE(list.capacity >= 100);
  msAstArenaDestroy(&arena);
}

MS_TEST(parser, arenaOom) {
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  msMemSetFailAfter(0);
  MS_ASSERT_TRUE(msAstNew(&arena, MS_AST_INT, 1, 1) == NULL);
  struct MsAstList list = { NULL, 0, 0 };
  MS_ASSERT_TRUE(!msAstListPush(&arena, &list, NULL));
  msMemSetFailAfter(-1);
  msAstArenaDestroy(&arena);
}
```

(Remember the `MS_TEST_MAIN` case table at the bottom of the file — grow it with every task.)

- [ ] **Step 3: Run tests to verify they fail to build**

Run: `cmake --build build` → fails: `parser/ms_ast.h` not found (test not yet in CMake either). Wire `CMakeLists.txt` first (add sources + `mslang_add_c_test(test_parser)` under the existing `if` block that registers the other tests, mirroring `test_lexer`), then: `cmake -S . -B build && cmake --build build` → compile/link fails. That is the expected red state.

- [ ] **Step 4: Implement `src/parser/ms_ast.c`** — arena + `msAstNew` + `msAstListPush` + `msAstKindName` (dump lands in Task 2; declare a minimal `msAstDump` stub that prints nothing so the library links).

Key implementation points:

```c
#include "parser/ms_ast.h"

#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

void* msAstArenaAlloc(struct MsAstArena* arena, size_t size) {
  size = (size + MS_AST_ARENA_ALIGN - 1) / MS_AST_ARENA_ALIGN * MS_AST_ARENA_ALIGN;
  if (size == 0) {
    size = MS_AST_ARENA_ALIGN;
  }
  struct MsAstArenaBlock* block = arena->head;
  if (block == NULL || block->used + size > block->capacity) {
    // allocate a new block sized max(size, MS_AST_ARENA_BLOCK_SIZE) ...
  }
  // bump-allocate inside block->data, return pointer
}
```

New blocks link at `arena->head` (most-recent first); `capacity` is set to `max(size, MS_AST_ARENA_BLOCK_SIZE)` at allocation and `totalAllocated += capacity` per new block. `msAstArenaDestroy` walks the list and `msFree`s each block.

`msAstNew`: `struct MsAst* node = msAstArenaAlloc(arena, sizeof(struct MsAst)); if (node) { memset(node, 0, sizeof(*node)); node->kind = kind; node->line = line; node->column = column; }`.

`msAstListPush`: grow to `capacity ? capacity * 2 : 4` via a fresh `msAstArenaAlloc` + `memcpy` of the old items (old array stays garbage-in-arena, reclaimed at destroy — that is the arena deal).

`msAstKindName`: file-static `static const char* const msAstKindNames[] = { "MS_AST_INVALID", ... };` in exact enum order + `MS_ASSERT(kind >= MS_AST_INVALID && kind <= MS_AST_CLASS_DECL)`.

- [ ] **Step 5: Run tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_parser`
Expected: 4/4 parser tests PASS; then full suite `ctest --test-dir build --output-on-failure` stays green.

- [ ] **Step 6: Commit**

```bash
git add src/parser/ms_ast.h src/parser/ms_ast.c tests/c/test_parser.c CMakeLists.txt
git commit -m "✨ feat(parser): add AST module with arena allocator"
```

---

## Task 2: `msAstDump` — S-expression debug dump

**Files:**
- Modify: `src/parser/ms_ast.c`
- Test: `tests/c/test_parser.c`

**Pinned dump format (golden tests compare byte-exactly):**

- One node per line; indent is 2 spaces per depth; every line ends with `\n`.
- A line is `MS_AST_KIND` followed by space-separated fields; child nodes follow on their own lines at depth + 1, in the fixed order given below.
- Quoted slices use escaping: `\` → `\\`, `"` → `\"`, newline → `\n`, CR → `\r`, tab → `\t`; all other bytes raw.
- NULL optional children/fields are omitted entirely (never printed as a placeholder).
- No line/column in the dump (position is tested by direct field access in Task 3).

Field table (`children:` lists the fixed child order; only present (non-NULL) children appear):

| Kind(s) | Trailing fields | Children |
|---|---|---|
| INT, FLOAT, STRING, RAW_STRING, BYTES, IDENT, FSTRING_TEXT | `"<escaped lexeme>"` | — |
| BOOL | `true` or `false` | — |
| NIL, BREAK, CONTINUE, PASS, INVALID | — | — |
| UNARY | `op=<msTokenTypeName>` | operand |
| BINARY, LOGICAL | `op=<msTokenTypeName>` | left, right |
| COMPARE_CHAIN | `ops=<MS_CMP_x>,...` (comma-joined, static name table in ms_ast.c) | operands in order (opCount + 1) |
| CONDITIONAL | — | value, cond, elseValue |
| AWAIT | — | operand |
| CALL | — | callee, then args in order |
| KW_ARG | `"<name>"` | value |
| INDEX | — | target, index |
| SLICE | `parts=` + comma-joined names of the present components among `start,stop,step` (`parts=` alone when all absent, e.g. `a[:]`) | target, then present components in start/stop/step order |
| ATTR | `"<name>"` | target |
| LIST_LIT, SET_LIT | — | elements |
| DICT_LIT | — | k0, v0, k1, v1, ... |
| LIST_COMP, SET_COMP | — | elem, then clauses |
| DICT_COMP | — | key, value (elem), then clauses |
| FSTRING | — | parts in order (TEXT/EXPR) |
| FSTRING_EXPR | `fmt="<escaped raw spec>"` iff hasFmt | expr |
| DECL, ASSIGN | — | targets in order, then values in order |
| AUG_ASSIGN, INC_DEC | `op=<msTokenTypeName>` | target, value (INC_DEC: value omitted) |
| EXPR_STMT | — | expr |
| IF | — | cond, thenBlock, elseBranch |
| FOR | `kind=MS_FOR_BARE` / `MS_FOR_CLAUSE` / `MS_FOR_IN` | CLAUSE: init, cond, post, body; IN: targets, iterable, body; BARE: body |
| WHILE | — | cond, body |
| TRY | — | body, except clauses in order, finallyBlock |
| EXCEPT_CLAUSE | `as="<name>"` iff bound | typeExpr, body |
| WITH | `as="<name>"` iff bound | context, body |
| SELECT | — | cases in order |
| SELECT_CASE | `kind=recv` / `send` / `default`, plus ` decl` iff `:=` | recv: targets, channel, body; send: channel, value, body; default: body |
| IMPORT | `"<path>"`, plus ` as="<alias>"` iff present | — |
| FROM_IMPORT | `"<path>"` | names (IDENT) in order |
| RETURN | — | values in order |
| RAISE | — | value, fromValue |
| DEL | — | targets in order |
| GLOBAL | — | names (IDENT) in order |
| FUNC_DECL | `"<name>"`, plus ` async` / ` static` flags | params in order, then body |
| LAMBDA | — | params in order, then body expression |
| PARAM | `"<name>" kind=MS_PARAM_NORMAL` / `MS_PARAM_VARARGS` / `MS_PARAM_KWARGS` | defaultValue |
| CLASS_DECL | `"<name>"` | superclass, body |
| SOURCE_FILE, BLOCK | — | statements in order |

- [ ] **Step 1: Write the failing tests** (hand-built tree; no parser yet). Append to `tests/c/test_parser.c`:

```c
// Builds: (sourceFile (exprStmt (binary + (int "1") (binary * (int "2") (int "3")))))
MS_TEST(parser, dumpHandBuiltTree) {
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  struct MsAst* one = msAstNew(&arena, MS_AST_INT, 1, 1);
  one->as.literal.start = "1";
  one->as.literal.length = 1;
  struct MsAst* two = msAstNew(&arena, MS_AST_INT, 1, 3);
  two->as.literal.start = "2";
  two->as.literal.length = 1;
  struct MsAst* three = msAstNew(&arena, MS_AST_INT, 1, 5);
  three->as.literal.start = "3";
  three->as.literal.length = 1;
  struct MsAst* mul = msAstNew(&arena, MS_AST_BINARY, 1, 4);
  mul->as.binary.op = MS_TOKEN_STAR;
  mul->as.binary.left = two;
  mul->as.binary.right = three;
  struct MsAst* add = msAstNew(&arena, MS_AST_BINARY, 1, 2);
  add->as.binary.op = MS_TOKEN_PLUS;
  add->as.binary.left = one;
  add->as.binary.right = mul;
  struct MsAst* stmt = msAstNew(&arena, MS_AST_EXPR_STMT, 1, 1);
  stmt->as.exprStmt.expr = add;
  struct MsAst* root = msAstNew(&arena, MS_AST_SOURCE_FILE, 1, 1);
  MS_ASSERT_TRUE(msAstListPush(&arena, &root->as.block, stmt));

  FILE* out = tmpfile();
  MS_ASSERT_TRUE(out != NULL);
  msAstDump(root, out);
  rewind(out);
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, out);
  buf[n] = '\0';
  fclose(out);
  const char* expected =
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_INT \"2\"\n"
      "        MS_AST_INT \"3\"\n";
  if (strcmp(buf, expected) != 0) {
    printf("--- expected ---\n%s--- actual ---\n%s---\n", expected, buf);
    MS_ASSERT_TRUE(false);
  }
  msAstArenaDestroy(&arena);
}

MS_TEST(parser, dumpEscapesLexemes) {
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  struct MsAst* str = msAstNew(&arena, MS_AST_STRING, 1, 1);
  str->as.literal.start = "a\\tb\"c";   // raw bytes: a \ t b " c
  str->as.literal.length = 6;
  FILE* out = tmpfile();
  MS_ASSERT_TRUE(out != NULL);
  msAstDump(str, out);
  rewind(out);
  char buf[256];
  size_t n = fread(buf, 1, sizeof(buf) - 1, out);
  buf[n] = '\0';
  fclose(out);
  MS_ASSERT_EQ(0, strcmp(buf, "MS_AST_STRING \"a\\\\tb\\\"c\"\n"));
  msAstArenaDestroy(&arena);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_parser`
Expected: FAIL (dump stub prints nothing / mismatch).

- [ ] **Step 3: Implement `msAstDump` in `src/parser/ms_ast.c`**

Static helpers: `dumpIndent(FILE*, int depth)` (2 spaces × depth), `dumpEscaped(FILE*, const char* start, size_t length)` (escape table above), `dumpList(FILE*, const struct MsAstList*, int depth)` (dump each child), and the recursive `dumpNode(const struct MsAst*, FILE*, int depth)` — a `switch (node->kind)` following the field table exactly. Compare-op names: `static const char* const msCompareOpNames[] = { "MS_CMP_LT", "MS_CMP_LE", "MS_CMP_GT", "MS_CMP_GE", "MS_CMP_EQ", "MS_CMP_NE", "MS_CMP_IN", "MS_CMP_NOT_IN", "MS_CMP_IS", "MS_CMP_IS_NOT" };`. For-kind names likewise: `"MS_FOR_BARE"` / `"MS_FOR_CLAUSE"` / `"MS_FOR_IN"`; param kinds and select-case kinds print as shown in the table. `msAstDump(node, out)` = `dumpNode(node, out, 0)`; `MS_ASSERT(node != NULL)`.

- [ ] **Step 4: Run tests** — `cmake --build build && ctest --test-dir build --output-on-failure -R test_parser` → all PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
git add src/parser/ms_ast.h src/parser/ms_ast.c tests/c/test_parser.c
git commit -m "✨ feat(parser): add AST S-expression dump"
```

---

## Task 3: Parser skeleton — init, token helpers, main loop

**Files:**
- Create: `src/parser/ms_parser.h`
- Create: `src/parser/ms_parser.c`
- Test: `tests/c/test_parser.c` (grow into the full shared harness)

- [ ] **Step 1: Write `src/parser/ms_parser.h`** (per spec, plus the pinned additive field):

```c
#ifndef MSLANG_SRC_PARSER_MS_PARSER_H_
#define MSLANG_SRC_PARSER_MS_PARSER_H_

#include <stdbool.h>

#include <mslang/error.h>

#include "core/ms_diag.h"
#include "lexer/ms_lexer.h"
#include "parser/ms_ast.h"

struct MsParser {
  struct MsLexer* lexer;        // borrowed, caller-owned
  struct MsAstArena* arena;     // borrowed, sole node allocation source
  struct MsDiagList* diags;     // borrowed, shared with the lexer (combined cap 20)
  struct MsToken current;       // current token
  struct MsToken previous;      // previous token (node position source)
  bool panicMode;               // suppresses cascading errors until synchronize()
  int loopDepth;                // break/continue context check
  int funcDepth;                // return context check
  bool funcIsAsync;             // await context check (innermost function)
  int classDepth;               // self/super context check
  // Additive beyond the spec struct: true while parsing a class method body,
  // combined with classDepth for the self/super check.
  bool inMethod;
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

#endif  // MSLANG_SRC_PARSER_MS_PARSER_H_
```

- [ ] **Step 2: Upgrade `tests/c/test_parser.c` to the full shared harness** (replace the ad-hoc includes with the harness from "Shared test harness" above, keeping the Task 1/2 tests), and add the failing tests:

```c
MS_TEST(parser, emptySource) {
  msTestExpectDump("", "MS_AST_SOURCE_FILE\n");
}

MS_TEST(parser, literalExprStmt) {
  msTestExpectDump("42",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"42\"\n");
}

MS_TEST(parser, twoStatementsTwoLines) {
  msTestExpectDump("1\n2",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"1\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"2\"\n");
}

MS_TEST(parser, explicitSemicolons) {
  msTestExpectDump("1; 2;",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"1\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"2\"\n");
}

MS_TEST(parser, emptyStatement) {
  static const uint32_t codes[] = { 204 };
  msTestExpectDiags(";", codes, 1);
}

MS_TEST(parser, missingSemicolon) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("1 2", codes, 1);   // auto-insertion only at line end
}

MS_TEST(parser, nodePositions) {
  MsTestFixture fix;
  msTestParse(&fix, "x\n  y");
  MS_ASSERT_EQ(MS_OK, fix.result);
  struct MsAst* first = fix.root->as.block.items[0];
  struct MsAst* second = fix.root->as.block.items[1];
  MS_ASSERT_EQ(1, (long long)first->line);
  MS_ASSERT_EQ(1, (long long)first->column);
  MS_ASSERT_EQ(2, (long long)second->line);
  MS_ASSERT_EQ(3, (long long)second->column);
  msTestFixtureDestroy(&fix);
}
```

- [ ] **Step 3: Run tests to verify they fail** — `cmake --build build` fails: `parser/ms_parser.h` not found.

- [ ] **Step 4: Implement the skeleton in `src/parser/ms_parser.c`**

Static helpers (file-local):

```c
static void parserAdvance(struct MsParser* p);   // shift current->previous, pull next token;
                                                 // loops over MS_TOKEN_INVALID (lexer already
                                                 // reported those); if msLexerNext returns
                                                 // MS_ERROR_SYNTAX (diag cap), fabricate EOF
static bool parserCheck(struct MsParser* p, MsTokenType type);   // current.type == type
static bool parserMatch(struct MsParser* p, MsTokenType type);   // check + advance
static void parserConsume(struct MsParser* p, MsTokenType type, const char* fmt, ...);
// parserConsume: match else parserErrorAt(current, 203, fmt, ...)
static void parserErrorAt(struct MsParser* p, const struct MsToken* token, uint32_t code,
    const char* fmt, ...);   // no-op when panicMode; else msDiagReport + panicMode = true
static struct MsAst* parserNewNode(struct MsParser* p, MsAstKind kind, const struct MsToken* at);
// wraps msAstNew; on NULL reports OOM once (a sticky flag or panicMode reuse) and returns NULL
```

Statement termination (pin 12): `consumeStatementEnd(p)` = expect `MS_TOKEN_SEMICOLON` (E203 when missing) after simple statements; `consumeOptionalStatementEnd(p)` = `parserMatch(p, MS_TOKEN_SEMICOLON)` after `}`-terminated compound statements.

Main loop (temporary minimal form — full dispatch lands in Task 7):

```c
MsResult msParserParse(struct MsParser* p, struct MsAst** out) {
  parserAdvance(p);   // prime current
  struct MsAst* root = parserNewNode(p, MS_AST_SOURCE_FILE, &p->current);
  if (root == NULL) { *out = NULL; return MS_ERROR_OOM; }
  while (!parserCheck(p, MS_TOKEN_EOF)) {
    // minimal: parse one expression (Task 4 wires parsePrecedence; for now a
    // literal/identifier primary is enough to go green), wrap in EXPR_STMT,
    // consumeStatementEnd, push into root->as.block.
    // A SEMICOLON here is E204 (pin 11).
  }
  *out = root;
  return msDiagListCount(p->diags) > 0 ? MS_ERROR_SYNTAX : MS_OK;
}
```

Important: the return value is `MS_ERROR_SYNTAX` iff the shared diag list gained entries (lexical or syntactic) during this parse — simplest correct rule: snapshot `msDiagListCount` at entry in a local, compare at exit. OOM: any `parserNewNode` NULL → report once, abort parse loop, return `MS_ERROR_OOM`.

`msParserInit`: zero the struct, store the four borrowed pointers.

For this task only, the "expression" inside the loop is a temporary primary: `MS_TOKEN_INT`/`FLOAT`/`STRING`/`RAW_STRING`/`BYTES`/`IDENTIFIER` → literal node (slice from token); `KW_TRUE`/`KW_FALSE` → BOOL; `KW_NIL` → NIL; anything else → E201 + skip token. Task 4 replaces this with the real Pratt entry.

- [ ] **Step 5: Run tests** — `cmake --build build && ctest --test-dir build --output-on-failure -R test_parser` → all PASS; full suite green.

- [ ] **Step 6: Commit**

```bash
git add src/parser/ms_parser.h src/parser/ms_parser.c tests/c/test_parser.c
git commit -m "✨ feat(parser): add parser skeleton with token helpers"
```

---

## Task 4: Pratt core — precedence table, literals, unary, binary, logical

**Files:**
- Modify: `src/parser/ms_parser.c`
- Test: `tests/c/test_parser.c`

- [ ] **Step 1: Write the failing tests**

```c
MS_TEST(parser, precedenceTermFactor) {
  msTestExpectDump("1 + 2 * 3",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_INT \"2\"\n"
      "        MS_AST_INT \"3\"\n");
}

MS_TEST(parser, powerRightAssoc) {
  msTestExpectDump("2 ** 3 ** 4",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_DOUBLE_STAR\n"
      "      MS_AST_INT \"2\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_DOUBLE_STAR\n"
      "        MS_AST_INT \"3\"\n"
      "        MS_AST_INT \"4\"\n");
}

MS_TEST(parser, unaryBindsLooserThanPower) {
  // -2 ** 2 == -(2 ** 2): unary is level 4, ** is level 3 (tighter).
  msTestExpectDump("-2 ** 2",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_UNARY op=MS_TOKEN_MINUS\n"
      "      MS_AST_BINARY op=MS_TOKEN_DOUBLE_STAR\n"
      "        MS_AST_INT \"2\"\n"
      "        MS_AST_INT \"2\"\n");
}

MS_TEST(parser, unaryBindsTighterThanFactor) {
  // -2 * 3 == (-2) * 3.
  msTestExpectDump("-2 * 3",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "      MS_AST_UNARY op=MS_TOKEN_MINUS\n"
      "        MS_AST_INT \"2\"\n"
      "      MS_AST_INT \"3\"\n");
}

MS_TEST(parser, logicalPrecedence) {
  // not a and b or c == ((not a) and b) or c
  msTestExpectDump("not a and b or c",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LOGICAL op=MS_TOKEN_KW_OR\n"
      "      MS_AST_LOGICAL op=MS_TOKEN_KW_AND\n"
      "        MS_AST_UNARY op=MS_TOKEN_KW_NOT\n"
      "          MS_AST_IDENT \"a\"\n"
      "        MS_AST_IDENT \"b\"\n"
      "      MS_AST_IDENT \"c\"\n");
}

MS_TEST(parser, bitwiseLevelChain) {
  // a | b ^ c & d << 1: shift(7) < bit-and(8) < bit-xor(9) < bit-or(10) in
  // table order => parse tree nests & inside ^ inside |; << is tighter than &.
  msTestExpectDump("a | b ^ c & d << 1",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PIPE\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_CARET\n"
      "        MS_AST_IDENT \"b\"\n"
      "        MS_AST_BINARY op=MS_TOKEN_AMP\n"
      "          MS_AST_IDENT \"c\"\n"
      "          MS_AST_BINARY op=MS_TOKEN_SHIFT_LEFT\n"
      "            MS_AST_IDENT \"d\"\n"
      "            MS_AST_INT \"1\"\n");
}

MS_TEST(parser, floorDivKeyword) {
  msTestExpectDump("a div b",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_KW_DIV\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, parensOverride) {
  msTestExpectDump("(1 + 2) * 3",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "      MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "        MS_AST_INT \"1\"\n"
      "        MS_AST_INT \"2\"\n"
      "      MS_AST_INT \"3\"\n");
}

MS_TEST(parser, literalKinds) {
  msTestExpectDump("1.5\n`s raw `\nb\"xy\"\ntrue\nfalse\nnil",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FLOAT \"1.5\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_RAW_STRING \"s raw \"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BYTES \"xy\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BOOL true\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BOOL false\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_NIL\n");
}
```

(Verify the RAW_STRING/BYTES lexeme slices against the lexer's actual contract while implementing — adjust the expected strings to whatever the token slice really is, quotes excluded per `ms_lexer.h`; do not change the lexer.)

- [ ] **Step 2: Run tests to verify they fail** — precedence cases produce wrong trees (temporary primary-only parser). `cmake --build build && ctest --test-dir build --output-on-failure -R test_parser` → FAIL.

- [ ] **Step 3: Implement the Pratt core in `src/parser/ms_parser.c`**

```c
typedef enum {
  MS_PREC_NONE,
  MS_PREC_LAMBDA,        // 15 lambda
  MS_PREC_CONDITIONAL,   // 14 a if cond else b (right assoc)
  MS_PREC_OR,            // 13
  MS_PREC_AND,           // 12
  MS_PREC_COMPARE,       // 11 comparisons / in / is (chained)
  MS_PREC_BIT_OR,        // 10 |
  MS_PREC_BIT_XOR,       // 9  ^
  MS_PREC_BIT_AND,       // 8  &
  MS_PREC_SHIFT,         // 7  << >>
  MS_PREC_TERM,          // 6  + -
  MS_PREC_FACTOR,        // 5  * / % div
  MS_PREC_UNARY,         // 4  +x -x ~x not x
  MS_PREC_POWER,         // 3  ** (right assoc)
  MS_PREC_POSTFIX,       // 2  [] slice attr call await
  MS_PREC_PRIMARY        // 1  literals / ident / (expr)
} MsPrecedence;          // enum order == increasing binding power

typedef struct MsAst* (*MsParseFn)(struct MsParser* p, bool canAssign);

struct MsParseRule {
  MsParseFn prefix;      // NULL: token cannot start an expression
  MsParseFn infix;       // NULL: not an operator
  MsPrecedence prec;
};

static struct MsAst* parsePrecedence(struct MsParser* p, MsPrecedence minPrec);
```

Rules (fill the whole table now; entries whose parse functions only land in later tasks are wired in those tasks — keep table entries `{ NULL, NULL, MS_PREC_NONE }` until then, except the ones listed here):

- prefix `parseLiteral` (INT/FLOAT/STRING/RAW_STRING/BYTES/IDENTIFIER/KW_TRUE/KW_FALSE/KW_NIL): literal/bool/nil/ident node from `p->previous`.
- prefix `parseGrouping` (LEFT_PAREN): `expr = parsePrecedence(p, MS_PREC_LAMBDA + 1)` — i.e. any expression tighter than a bare lambda; then `parserConsume(p, MS_TOKEN_RIGHT_PAREN, "expected ')'")`.
- prefix `parseUnary` (PLUS/MINUS/TILDE/KW_NOT): operand = `parsePrecedence(p, MS_PREC_UNARY + 1)` (pin 5).
- infix `parseBinary` (all arithmetic/bitwise/shift + KW_DIV): node `MS_AST_BINARY`, op = token type, right = `parsePrecedence(p, rule.prec + 1)` — except `MS_TOKEN_DOUBLE_STAR` whose right parses at `MS_PREC_POWER` (right assoc, pin 5).
- infix `parseLogical` (KW_AND/KW_OR): same shape, node `MS_AST_LOGICAL`, right at `prec + 1`.

`parsePrecedence` skeleton:

```c
static struct MsAst* parsePrecedence(struct MsParser* p, MsPrecedence minPrec) {
  parserAdvance(p);
  MsParseFn prefix = msParseRules[p->previous.type].prefix;
  if (prefix == NULL) {
    parserErrorAt(p, &p->previous, 201, "expected expression, got %s",
        msTokenTypeName(p->previous.type));
    return parserNewNode(p, MS_AST_INVALID, &p->previous);   // tree stays complete
  }
  struct MsAst* left = prefix(p, true);
  while (msParseRules[p->current.type].prec >= minPrec
      && msParseRules[p->current.type].infix != NULL) {
    parserAdvance(p);
    left = msParseRules[p->previous.type].infix(p, true);
  }
  return left;
}
```

`canAssign` is plumbed through for Task 7's reinterpret check (assignments are not expressions); for now every function ignores it. Replace Task 3's temporary primary with `parsePrecedence(p, MS_PREC_LAMBDA + 1)` in the statement loop.

Note the loop must not fold `MS_TOKEN_SEMICOLON`, `MS_TOKEN_COMMA`, `MS_TOKEN_RIGHT_PAREN` etc. — they have no infix rule (`prec == MS_PREC_NONE`).

- [ ] **Step 4: Run tests** — `cmake --build build && ctest --test-dir build --output-on-failure -R test_parser` → all PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add src/parser/ms_parser.c tests/c/test_parser.c
git commit -m "✨ feat(parser): add Pratt expression core"
```

---

## Task 5: Postfix chain, compare chain, conditional, await, lambda

**Files:**
- Modify: `src/parser/ms_parser.c`
- Test: `tests/c/test_parser.c`

- [ ] **Step 1: Write the failing tests**

```c
MS_TEST(parser, postfixMixedChain) {
  // m.a[0](x).b
  msTestExpectDump("m.a[0](x).b",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_ATTR \"b\"\n"
      "      MS_AST_CALL\n"
      "        MS_AST_INDEX\n"
      "          MS_AST_ATTR \"a\"\n"
      "            MS_AST_IDENT \"m\"\n"
      "          MS_AST_INT \"0\"\n"
      "        MS_AST_IDENT \"x\"\n");
}

MS_TEST(parser, callKeywordArgs) {
  msTestExpectDump("f(1, x = 2)",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"f\"\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_KW_ARG \"x\"\n"
      "        MS_AST_INT \"2\"\n");
}

MS_TEST(parser, callTrailingCommaAndEmpty) {
  msTestExpectDump("f()\ng(a, b, )",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"f\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"g\"\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, sliceCombinations) {
  msTestExpectDump("a[:]\nb[::2]\nc[1:]\nd[1:2:3]\ne[1:2]",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=\n"
      "      MS_AST_IDENT \"a\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=step\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=start\n"
      "      MS_AST_IDENT \"c\"\n"
      "      MS_AST_INT \"1\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=start,stop,step\n"
      "      MS_AST_IDENT \"d\"\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "      MS_AST_INT \"3\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=start,stop\n"
      "      MS_AST_IDENT \"e\"\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n");
}

MS_TEST(parser, emptySubscriptIsError) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("a[]", codes, 1);
}

MS_TEST(parser, compareChain) {
  msTestExpectDump("a < b <= c == d",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_LT,MS_CMP_LE,MS_CMP_EQ\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_IDENT \"c\"\n"
      "      MS_AST_IDENT \"d\"\n");
}

MS_TEST(parser, compareTwoTokenOps) {
  msTestExpectDump("a not in b\na is not b\na in b\na is b",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_NOT_IN\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_IS_NOT\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_IN\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_IS\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, notAInBVsANotInB) {
  // MS precedence: unary not (4) binds tighter than in (11), so
  // `not a in b` is `(not a) in b` -- deliberately unlike Python.
  msTestExpectDump("not a in b",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_IN\n"
      "      MS_AST_UNARY op=MS_TOKEN_KW_NOT\n"
      "        MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, conditionalRightAssoc) {
  msTestExpectDump("a if c1 else b if c2 else d",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CONDITIONAL\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"c1\"\n"
      "      MS_AST_CONDITIONAL\n"
      "        MS_AST_IDENT \"b\"\n"
      "        MS_AST_IDENT \"c2\"\n"
      "        MS_AST_IDENT \"d\"\n");
}

MS_TEST(parser, awaitBindsAtPostfix) {
  // await f(x) + 1 == (await f(x)) + 1
  msTestExpectDump("await f(x) + 1",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "      MS_AST_AWAIT\n"
      "        MS_AST_CALL\n"
      "          MS_AST_IDENT \"f\"\n"
      "          MS_AST_IDENT \"x\"\n"
      "      MS_AST_INT \"1\"\n");
}

MS_TEST(parser, lambdaEmptyAndMultiParam) {
  msTestExpectDump("f(lambda: 0, lambda a, b: a + b)",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"f\"\n"
      "      MS_AST_LAMBDA\n"
      "        MS_AST_INT \"0\"\n"
      "      MS_AST_LAMBDA\n"
      "        MS_AST_PARAM \"a\" kind=MS_PARAM_NORMAL\n"
      "        MS_AST_PARAM \"b\" kind=MS_PARAM_NORMAL\n"
      "        MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "          MS_AST_IDENT \"a\"\n"
      "          MS_AST_IDENT \"b\"\n");
}
```

Note for the lambda test: `MS_AST_LAMBDA` reuses the `func` union branch (name NULL, body = expression). The `:` between params and body is consumed by the lambda prefix rule; the lexer does not semicolon-split inside it because `,`/`:` do not trigger insertion — but the lambda BODY may span tokens until the expression ends; no semicolon handling needed inside the prefix rule beyond what Pratt gives.

- [ ] **Step 2: Run tests to verify they fail** — new constructs hit E201. `ctest -R test_parser` → FAIL.

- [ ] **Step 3: Implement**

New rules in the table:

- infix `parseCall` (LEFT_PAREN, prec POSTFIX): callee = left; loop args until RIGHT_PAREN: per arg, if `parserCheck(p, MS_TOKEN_IDENTIFIER) && msLexerPeek(p->lexer) == MS_TOKEN_EQUAL` → KW_ARG (consume ident, consume `=`, value = full expression); else expression. Allow trailing comma (pin: `[expr {"," expr} [","]]`). `parserConsume(RIGHT_PAREN, ...)`.
- infix `parseIndexOrSlice` (LEFT_BRACKET, prec POSTFIX): parse optional expr; if COLON → slice: components start/stop/step each optional (`parseComponent` = expression if current isn't COLON/RIGHT_BRACKET), at least one component or colon must exist — `a[]` errors E203 ("expected index or slice"); plain index otherwise.
- infix `parseAttr` (DOT, prec POSTFIX): consume IDENTIFIER (E203 otherwise; NOTE: `self`/`super` are keywords — accept `MS_TOKEN_KW_SELF`/`MS_TOKEN_KW_SUPER` as attribute names too? No: grammar says `.` followed by identifier; `super.__init__` works because `__init__` is an identifier. Keep strict identifier; the lexeme slice goes into `attr.start/length`).
- infix `parseCompareChain` (EQ_EQ/BANG_EQ/LESS/LESS_EQ/GREATER/GREATER_EQ/KW_IN/KW_IS, prec COMPARE) + special entry: KW_NOT gets BOTH prefix `parseUnary` AND infix `parseCompareChain` (only fires when peek is KW_IN, else the infix reports... no — pin 7: the infix on KW_NOT checks `msLexerPeek == MS_TOKEN_KW_IN`; if not, it must NOT have been treated as infix. Handle this inside the parsePrecedence loop: the while-condition for KW_NOT additionally requires `msLexerPeek(p->lexer) == MS_TOKEN_KW_IN`). Chain builder: first comparison creates the COMPARE_CHAIN node with `left` as operands[0]; subsequent comparisons at the same level append to the same node (detect: if left is already a chain built at this level — simplest: after building a chain node, loop internally over further compare operators instead of returning to the Pratt loop; the internal loop consumes operators and operands at `MS_PREC_COMPARE + 1`). Ops array: arena-grown `MsCompareOp*` (double via `msAstArenaAlloc`, or collect into a fixed local `MsCompareOp tmp[16]`-style — NO, chains are unbounded: grow in arena).
- infix `parseConditional` (KW_IF, prec CONDITIONAL): value = left; cond = `parsePrecedence(p, MS_PREC_OR)`; `parserConsume(KW_ELSE, ...)`; elseValue = `parsePrecedence(p, MS_PREC_CONDITIONAL)` (right assoc).
- prefix `parseAwait` (KW_AWAIT): operand = `parsePrecedence(p, MS_PREC_POSTFIX)`; context check `if (p->funcDepth == 0 || !p->funcIsAsync) error E210` — wire the check now (fields exist), tests for the diagnostic come in Task 8.
- prefix `parseLambda` (KW_LAMBDA): optional identifier list (each becomes MS_AST_PARAM normal), `parserConsume(COLON, ...)`, body = `parsePrecedence(p, MS_PREC_CONDITIONAL)`. Node = MS_AST_LAMBDA via `func` branch (`name = NULL`, `isAsync = false`, `isStatic = false`).

`await` context check note: `funcIsAsync` must reflect the INNERMOST function; save/restore happens in Task 8's func parsing. For Task 5, `await` at top level triggers E210 — that's fine, no positive await-in-async test until Task 8.

Also: `MS_TOKEN_KW_SELF` / `MS_TOKEN_KW_SUPER` prefix rule (pin 9): IDENT-like node; context check E210 when `!p->inMethod` — wire now, diagnostic tests in Task 8.

- [ ] **Step 4: Run tests** — `ctest -R test_parser` all PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add src/parser/ms_parser.c tests/c/test_parser.c
git commit -m "✨ feat(parser): add postfix chain, compare chain, conditional, await, lambda"
```

---

## Task 6: Container literals and comprehensions

**Files:**
- Modify: `src/parser/ms_parser.c`
- Test: `tests/c/test_parser.c`

- [ ] **Step 1: Write the failing tests**

```c
MS_TEST(parser, listAndSetAndDict) {
  msTestExpectDump("[1, 2]\n{1, 2}\n{1: 2}\n{}",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SET_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_DICT_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_DICT_LIT\n");
}

MS_TEST(parser, emptyList) {
  msTestExpectDump("[]",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_LIT\n");
}

MS_TEST(parser, listComprehension) {
  msTestExpectDump("[x * x for x in xs if x > 0]",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_COMP\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"x\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"xs\"\n"
      "      MS_AST_IF\n"
      "        MS_AST_COMPARE_CHAIN ops=MS_CMP_GT\n"
      "          MS_AST_IDENT \"x\"\n"
      "          MS_AST_INT \"0\"\n");
}

MS_TEST(parser, comprehensionNestedFor) {
  msTestExpectDump("[x + y for x in a for y in b]",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_COMP\n"
      "      MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"y\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"a\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"y\"\n"
      "        MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, dictAndSetComprehension) {
  msTestExpectDump("[k: v * 2 for k, v in m]\n{x % 10 for x in xs}",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_DICT_COMP\n"
      "      MS_AST_IDENT \"k\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_IDENT \"v\"\n"
      "        MS_AST_INT \"2\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"k\"\n"
      "        MS_AST_IDENT \"v\"\n"
      "        MS_AST_IDENT \"m\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SET_COMP\n"
      "      MS_AST_BINARY op=MS_TOKEN_PERCENT\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_INT \"10\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"xs\"\n");
}
```

(Resolved during implementation: 03-syntax section 6.2 specifies the bracket form `[k: v * 2 for k, v in m]` for dict comprehensions; braces are for set comprehensions. The test above uses the bracket form per spec. The parser additionally accepts the brace form `{k: v for ...}` for tolerance — see commit b5f5d9a.)

- [ ] **Step 2: Run tests to verify they fail** — `[`/`{` prefix missing → E201.

- [ ] **Step 3: Implement**

- prefix `parseListOrComprehension` (LEFT_BRACKET): empty `[]` → LIST_LIT. Else parse first element expr; if current is `KW_FOR` → LIST_COMP: elem = that expr, clauses = `parseComprehensionClauses`. Else LIST_LIT: loop `,` elements, trailing comma allowed, `parserConsume(RIGHT_BRACKET, ...)`.
- prefix `parseBraceLiteral` (LEFT_BRACE): `{}` → DICT_LIT (empty, pin from spec). Parse first expr; if COLON → dict path: consume `:`, value expr; if then KW_FOR → DICT_COMP (key/elem + clauses); else DICT_LIT loop `, k: v`. If no COLON: if KW_FOR → SET_COMP; else SET_LIT loop. Ambiguity note: `{` at statement start is a BLOCK — the statement dispatcher (Task 7) checks blocks BEFORE expression statements, so this prefix rule only fires in expression position. That matches Go-style grammar where a statement can't start with a literal brace.
- `parseComprehensionClauses`: loop: `for` → target ident list (idents, comma-separated), `parserConsume(KW_IN, ...)`, iterable = expression at `MS_PREC_OR` (must NOT swallow a following `if` clause or `for` clause — both are below OR? No: `if` (conditional) is CONDITIONAL < OR so an `if` WOULD be parsed as conditional... `[x for x in xs if x > 0]`: iterable parsed at full expression would consume `if x > 0` as a conditional tail! Pin: comprehension iterable parses at `MS_PREC_OR`... conditional prec is CONDITIONAL(14) which is < OR(13)? Enum order: CONDITIONAL < OR, so parsePrecedence(OR) folds infix with prec >= OR — KW_IF's CONDITIONAL(14)... wait enum order: NONE=0, LAMBDA=1, CONDITIONAL=2, OR=3, AND=4, ... CONDITIONAL(2) >= OR(3)? No, 2 < 3 → not folded. Good: iterable at `MS_PREC_OR` stops before `if` and before `for` (KW_FOR has no infix rule anyway).) Then optional `if` → clause IF node (cond = expression at MS_PREC_OR, thenBlock NULL). Loop continues while current is KW_FOR / KW_IF.

Clauses are `MS_AST_FOR` nodes with `forKind = MS_FOR_IN`, `targets` = idents, `iterable` = expr, `body = NULL`; and `MS_AST_IF` nodes with `cond`, `thenBlock = elseBranch = NULL` (pin 3).

- [ ] **Step 4: Run tests** — `ctest -R test_parser` all PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add src/parser/ms_parser.c tests/c/test_parser.c
git commit -m "✨ feat(parser): add container literals and comprehensions"
```

---

## Task 7: Statement dispatch, semicolons, blocks, decl/assign reinterpretation

**Files:**
- Modify: `src/parser/ms_parser.c`
- Test: `tests/c/test_parser.c`

- [ ] **Step 1: Write the failing tests**

```c
MS_TEST(parser, declAndAssign) {
  msTestExpectDump("a := 1\na = 2",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_DECL\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_INT \"1\"\n"
      "  MS_AST_ASSIGN\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_INT \"2\"\n");
}

MS_TEST(parser, swapAssign) {
  msTestExpectDump("a, b = b, a",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_ASSIGN\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_IDENT \"b\"\n"
      "    MS_AST_IDENT \"b\"\n"
      "    MS_AST_IDENT \"a\"\n");
}

MS_TEST(parser, unpackDecl) {
  msTestExpectDump("a, *rest := xs",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_DECL\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_UNARY op=MS_TOKEN_STAR\n"
      "      MS_AST_IDENT \"rest\"\n"
      "    MS_AST_IDENT \"xs\"\n");
}

MS_TEST(parser, assignTargetsAttrIndex) {
  msTestExpectDump("o.f, a[i] = 1, 2",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_ASSIGN\n"
      "    MS_AST_ATTR \"f\"\n"
      "      MS_AST_IDENT \"o\"\n"
      "    MS_AST_INDEX\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"i\"\n"
      "    MS_AST_INT \"1\"\n"
      "    MS_AST_INT \"2\"\n");
}

MS_TEST(parser, augAssignOps) {
  static const struct { const char* source; const char* op; } cases[] = {
    { "a += 1", "MS_TOKEN_PLUS_EQUAL" },
    { "a -= 1", "MS_TOKEN_MINUS_EQUAL" },
    { "a *= 1", "MS_TOKEN_STAR_EQUAL" },
    { "a /= 1", "MS_TOKEN_SLASH_EQUAL" },
    { "a %= 1", "MS_TOKEN_PERCENT_EQUAL" },
    { "a **= 1", "MS_TOKEN_DOUBLE_STAR_EQUAL" },
    { "a &= 1", "MS_TOKEN_AMP_EQUAL" },
    { "a |= 1", "MS_TOKEN_PIPE_EQUAL" },
    { "a ^= 1", "MS_TOKEN_CARET_EQUAL" },
    { "a <<= 1", "MS_TOKEN_SHIFT_LEFT_EQUAL" },
    { "a >>= 1", "MS_TOKEN_SHIFT_RIGHT_EQUAL" },
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(cases); ++i) {
    char expected[512];
    snprintf(expected, sizeof(expected),
        "MS_AST_SOURCE_FILE\n"
        "  MS_AST_AUG_ASSIGN op=%s\n"
        "    MS_AST_IDENT \"a\"\n"
        "    MS_AST_INT \"1\"\n", cases[i].op);
    msTestExpectDump(cases[i].source, expected);
  }
}

MS_TEST(parser, badAssignTarget) {
  static const uint32_t codes[] = { 205 };
  msTestExpectDiags("1 = 2", codes, 1);
  msTestExpectDiags("a + b := 1", codes, 1);
}

MS_TEST(parser, badUnpack) {
  static const uint32_t codes[] = { 212 };
  msTestExpectDiags("*a, *b := xs", codes, 1);     // more than one star target
  msTestExpectDiags("*a = xs", codes, 1);          // star target only in :=/for-in
}

MS_TEST(parser, exprListWithoutAssign) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("a, b", codes, 1);   // expression list with no assignment operator
}

MS_TEST(parser, blockAsStatement) {
  msTestExpectDump("{ 1\n2 }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_BLOCK\n"
      "    MS_AST_EXPR_STMT\n"
      "      MS_AST_INT \"1\"\n"
      "    MS_AST_EXPR_STMT\n"
      "      MS_AST_INT \"2\"\n");
}

MS_TEST(parser, doubleEmptyStatement) {
  static const uint32_t codes[] = { 204 };
  msTestExpectDiags("1;;", codes, 1);   // cascade suppression: one error
}
```

- [ ] **Step 2: Run tests to verify they fail.**

- [ ] **Step 3: Implement statement dispatch**

```c
static struct MsAst* parseStatement(struct MsParser* p);
static struct MsAst* parseBlock(struct MsParser* p);          // consumes { ... }
static struct MsAst* parseSimpleStatement(struct MsParser* p);// decl/assign/expr/inc-dec
```

`parseStatement` dispatch (compound forms land in Task 8 — for now: LEFT_BRACE → `parseBlock`; SEMICOLON → E204 + advance + return INVALID node... no: E204 reported, return NULL and let the caller skip the node entirely — pin: empty statement produces NO node, just the diagnostic; everything else → `parseSimpleStatement` + `consumeStatementEnd`).

`parseSimpleStatement` (the parse-then-reinterpret algorithm from the spec):

1. Special case `IDENT ++`/`--` here? No — `++`/`--` at statement level is E206 outside for-post (Task 8's for-clause parser sets a flag `bool allowIncDec` — additive parser field, or pass a parameter). For this task: after parsing the expression list, if current is PLUS_PLUS/MINUS_MINUS → E206 (for-clause support arrives in Task 8).
2. Parse first expression, then while COMMA: more expressions → a target list.
3. If current is COLON_EQUAL / EQUAL / an aug-assign token:
   - validate every left item: IDENT / INDEX / ATTR → OK; UNARY-with-STAR (operand IDENT) → only when COLON_EQUAL and at most one in the list (else E212); anything else → E205.
   - COLON_EQUAL → DECL; EQUAL → ASSIGN; aug token → AUG_ASSIGN (exactly ONE target required: `a, b += 1` → E205? Grammar: `targetList ( "=" | augOp ) exprList` — augOp with multiple targets is grammatically allowed but semantically dubious; pin: allow it syntactically, no extra error).
   - Parse right side as expression list (comma-separated full expressions at `MS_PREC_LAMBDA + 1`).
4. No operator: single expression → EXPR_STMT; multiple → E203 ("expected ':=' or '='").

Reinterpret detail: the left side is parsed with `canAssign` plumbing — since MS has no assignment expressions, just parse full expressions; validation happens structurally afterward. The `*ident` unpack target: at the START of a list item, if current is STAR and we're parsing a would-be decl... simpler: parse list items as expressions; STAR has no prefix rule → `*rest := xs` hits E201 before reinterpret. Fix: give `parseSimpleStatement` a pre-pass — when current is STAR and peek is IDENTIFIER, build the UNARY(STAR, IDENT) item directly instead of calling parsePrecedence. Only valid in target position; E212 validation happens at step 3 (star item present but operator is not COLON_EQUAL, or more than one star item).

`parseBlock`: consume LEFT_BRACE, loop `parseStatement` until RIGHT_BRACE/EOF, consume RIGHT_BRACE (missing → E211 "expected '}'"), return BLOCK node. Caller (Task 8 compound statements / Task 3 main loop for a top-level block statement) applies `consumeOptionalStatementEnd`.

`msParserParse` main loop now dispatches through `parseStatement` (top level and blocks share the same dispatch; func/class arms join in Task 8 per pin 10).

Add `MS_ARRAY_LEN` include (`core/ms_common.h`) to the test file if missing.

- [ ] **Step 4: Run tests** — `ctest -R test_parser` all PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add src/parser/ms_parser.c tests/c/test_parser.c
git commit -m "✨ feat(parser): add statement dispatch and assignment reinterpretation"
```

---

## Task 8: Compound statements and declarations — if/for/while/try/with/select/import/func/class + context checks

**Files:**
- Modify: `src/parser/ms_parser.c`
- Test: `tests/c/test_parser.c`

- [ ] **Step 1: Write the failing tests**

if / else-if / else:

```c
MS_TEST(parser, ifElseIfElse) {
  msTestExpectDump("if a { 1 } else if b { 2 } else { 3 }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_IF\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_INT \"1\"\n"
      "    MS_AST_IF\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_INT \"2\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_INT \"3\"\n");
}
```

for three forms + inc/dec:

```c
MS_TEST(parser, forThreePart) {
  msTestExpectDump("for i := 0; i < 10; i++ { }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FOR kind=MS_FOR_CLAUSE\n"
      "    MS_AST_DECL\n"
      "      MS_AST_IDENT \"i\"\n"
      "      MS_AST_INT \"0\"\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_LT\n"
      "      MS_AST_IDENT \"i\"\n"
      "      MS_AST_INT \"10\"\n"
      "    MS_AST_INC_DEC op=MS_TOKEN_PLUS_PLUS\n"
      "      MS_AST_IDENT \"i\"\n"
      "    MS_AST_BLOCK\n");
}

MS_TEST(parser, forIn) {
  msTestExpectDump("for k, v in m { }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FOR kind=MS_FOR_IN\n"
      "    MS_AST_IDENT \"k\"\n"
      "    MS_AST_IDENT \"v\"\n"
      "    MS_AST_IDENT \"m\"\n"
      "    MS_AST_BLOCK\n");
}

MS_TEST(parser, forBare) {
  msTestExpectDump("for { }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FOR kind=MS_FOR_BARE\n"
      "    MS_AST_BLOCK\n");
}

MS_TEST(parser, incDecOutsideFor) {
  static const uint32_t codes[] = { 206 };
  msTestExpectDiags("i++", codes, 1);
}
```

while / try / with / select / import:

```c
MS_TEST(parser, whileStmt) {
  msTestExpectDump("while x < 1 { x = 2 }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_WHILE\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_LT\n"
      "      MS_AST_IDENT \"x\"\n"
      "      MS_AST_INT \"1\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_ASSIGN\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_INT \"2\"\n");
}

MS_TEST(parser, tryExceptFinally) {
  msTestExpectDump("try { 1 } except IOError as e { 2 } except { 3 } finally { 4 }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_TRY\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_INT \"1\"\n"
      "    MS_AST_EXCEPT_CLAUSE as=\"e\"\n"
      "      MS_AST_IDENT \"IOError\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_INT \"2\"\n"
      "    MS_AST_EXCEPT_CLAUSE\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_INT \"3\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_INT \"4\"\n");
}

MS_TEST(parser, withAs) {
  msTestExpectDump("with open(\"f\") as f { g(f) }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_WITH as=\"f\"\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"open\"\n"
      "      MS_AST_STRING \"f\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_CALL\n"
      "          MS_AST_IDENT \"g\"\n"
      "          MS_AST_IDENT \"f\"\n");
}

MS_TEST(parser, selectCases) {
  msTestExpectDump(
      "select {\n"
      "case v := ch.recv():\n"
      "  use(v)\n"
      "case ch2.send(x):\n"
      "  pass\n"
      "default:\n"
      "  pass\n"
      "}",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_SELECT\n"
      "    MS_AST_SELECT_CASE kind=recv decl\n"
      "      MS_AST_IDENT \"v\"\n"
      "      MS_AST_IDENT \"ch\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_CALL\n"
      "            MS_AST_IDENT \"use\"\n"
      "            MS_AST_IDENT \"v\"\n"
      "    MS_AST_SELECT_CASE kind=send\n"
      "      MS_AST_IDENT \"ch2\"\n"
      "      MS_AST_IDENT \"x\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_PASS\n"
      "    MS_AST_SELECT_CASE kind=default\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_PASS\n");
}

MS_TEST(parser, importForms) {
  msTestExpectDump(
      "import \"fmt\"\n"
      "import \"encoding/json\" as encjson\n"
      "from \"strings\" import toUpper, split\n"
      "from \"os\" import (getenv,)",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_IMPORT \"fmt\"\n"
      "  MS_AST_IMPORT \"encoding/json\" as=\"encjson\"\n"
      "  MS_AST_FROM_IMPORT \"strings\"\n"
      "    MS_AST_IDENT \"toUpper\"\n"
      "    MS_AST_IDENT \"split\"\n"
      "  MS_AST_FROM_IMPORT \"os\"\n"
      "    MS_AST_IDENT \"getenv\"\n");
}

MS_TEST(parser, importPathMustBeString) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("import fmt", codes, 1);
}
```

func / lambda params / class:

```c
MS_TEST(parser, funcDeclFull) {
  msTestExpectDump("func f(a, b = 1, *rest, **kw) { return a }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FUNC_DECL \"f\"\n"
      "    MS_AST_PARAM \"a\" kind=MS_PARAM_NORMAL\n"
      "    MS_AST_PARAM \"b\" kind=MS_PARAM_NORMAL\n"
      "      MS_AST_INT \"1\"\n"
      "    MS_AST_PARAM \"rest\" kind=MS_PARAM_VARARGS\n"
      "    MS_AST_PARAM \"kw\" kind=MS_PARAM_KWARGS\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_RETURN\n"
      "        MS_AST_IDENT \"a\"\n");
}

MS_TEST(parser, asyncFunc) {
  msTestExpectDump("async func f() { await g() }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FUNC_DECL \"f\" async\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_AWAIT\n"
      "          MS_AST_CALL\n"
      "            MS_AST_IDENT \"g\"\n");
}

MS_TEST(parser, paramOrderViolation) {
  static const uint32_t codes[] = { 207 };
  msTestExpectDiags("func f(*rest, a) { }", codes, 1);       // normal after varargs
  msTestExpectDiags("func f(a = 1, b) { }", codes, 1);       // non-default after default
}

MS_TEST(parser, paramDuplicate) {
  static const uint32_t codes[] = { 208 };
  msTestExpectDiags("func f(a, a) { }", codes, 1);
}

MS_TEST(parser, classDecl) {
  msTestExpectDump(
      "class Dog < Animal {\n"
      "  func __init__(self) { }\n"
      "  static func create() { }\n"
      "}",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_CLASS_DECL \"Dog\"\n"
      "    MS_AST_IDENT \"Animal\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_FUNC_DECL \"__init__\"\n"
      "        MS_AST_PARAM \"self\" kind=MS_PARAM_NORMAL\n"
      "        MS_AST_BLOCK\n"
      "      MS_AST_FUNC_DECL \"create\" static\n"
      "        MS_AST_BLOCK\n");
}

MS_TEST(parser, staticOutsideClassIsIdent) {
  msTestExpectDump("static := 1",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_DECL\n"
      "    MS_AST_IDENT \"static\"\n"
      "    MS_AST_INT \"1\"\n");
}
```

Context checks:

```c
MS_TEST(parser, breakOutsideLoop) {
  static const uint32_t codes[] = { 209 };
  msTestExpectDiags("break", codes, 1);
}

MS_TEST(parser, breakInLoopClosureBarrier) {
  static const uint32_t codes[] = { 209 };
  // loopDepth resets on entering a func body: the nested function cannot
  // break the outer loop.
  msTestExpectDiags("while true { func g() { break } }", codes, 1);
}

```c
MS_TEST(parser, breakInsideLoop) {
  msTestExpectDump("while true { break\ncontinue }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_WHILE\n"
      "    MS_AST_BOOL true\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_BREAK\n"
      "      MS_AST_CONTINUE\n");
}

MS_TEST(parser, returnAtTopLevel) {
  static const uint32_t codes[] = { 210 };
  msTestExpectDiags("return 1", codes, 1);
}

MS_TEST(parser, awaitOutsideAsync) {
  static const uint32_t codes[] = { 210 };
  msTestExpectDiags("await f()", codes, 1);
  msTestExpectDiags("func g() { await f() }", codes, 1);
}

MS_TEST(parser, selfOutsideMethod) {
  static const uint32_t codes[] = { 210 };
  msTestExpectDiags("self.x = 1", codes, 1);
}

MS_TEST(parser, selfInsideMethod) {
  msTestExpectDump("class A {\n  func m(self) { self.x = 1\nsuper.m() }\n}",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_CLASS_DECL \"A\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_FUNC_DECL \"m\"\n"
      "        MS_AST_PARAM \"self\" kind=MS_PARAM_NORMAL\n"
      "        MS_AST_BLOCK\n"
      "          MS_AST_ASSIGN\n"
      "            MS_AST_ATTR \"x\"\n"
      "              MS_AST_IDENT \"self\"\n"
      "            MS_AST_INT \"1\"\n"
      "          MS_AST_EXPR_STMT\n"
      "            MS_AST_CALL\n"
      "              MS_AST_ATTR \"m\"\n"
      "                MS_AST_IDENT \"super\"\n");
}

MS_TEST(parser, nestedFuncDecl) {
  // func/class are statements (pin 10): nested declarations are legal.
  msTestExpectDump("func outer() { func inner() { return 1 } }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FUNC_DECL \"outer\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_FUNC_DECL \"inner\"\n"
      "        MS_AST_BLOCK\n"
      "          MS_AST_RETURN\n"
      "            MS_AST_INT \"1\"\n");
}
```

Remaining simple statements:

```c
MS_TEST(parser, returnRaiseDelGlobalPass) {
  msTestExpectDump(
      "func f() {\n"
      "  raise E from cause\n"
      "}\n"
      "del a, b[i]\n"
      "global g1, g2\n"
      "pass",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FUNC_DECL \"f\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_RAISE\n"
      "        MS_AST_IDENT \"E\"\n"
      "        MS_AST_IDENT \"cause\"\n"
      "  MS_AST_DEL\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_INDEX\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_IDENT \"i\"\n"
      "  MS_AST_GLOBAL\n"
      "    MS_AST_IDENT \"g1\"\n"
      "    MS_AST_IDENT \"g2\"\n"
      "  MS_AST_PASS\n");
}
```

(Note `global` at top level is syntactically valid; any semantic restriction is task 07's. `raise` reuses funcDepth? No — raise is legal anywhere; only `return` checks funcDepth.)

- [ ] **Step 2: Run tests to verify they fail.**

- [ ] **Step 3: Implement**

New statement arms in `parseStatement` (shared by top level and blocks, pin 10):

- `parseIf`: consume `if`, cond = expression (full, `MS_PREC_LAMBDA + 1`), thenBlock = `parseBlock`; while `else if` → chain into elseBranch; final `else` → block. `consumeOptionalStatementEnd` after.
- `parseFor`: consume `for`; if LEFT_BRACE → BARE. Else parse first segment: if it's an identifier list followed by KW_IN → IN form (targets via the same unpack rule as `:=`, i.e. `*ident` allowed; validate assignability, at most one star — E212). Otherwise it's a CLAUSE: reparse — pin: parse `parseSimpleStatement`-style init (allowing inc/dec via a parser flag `bool allowIncDec` set only around the post segment), then consume SEMICOLON (explicit `;` token — NOT auto-inserted; both forms arrive as MS_TOKEN_SEMICOLON, so just consume it), cond expression, SEMICOLON, post simple statement (inc/dec allowed). Then block. Detection detail: you cannot know IN vs CLAUSE until you see `in` or `;` — parse an expression/ident-list first (same pre-pass as parseSimpleStatement: star-target handling included), then dispatch on KW_IN vs SEMICOLON; for the clause path reinterpret the already-parsed list into DECL/ASSIGN/EXPR_STMT exactly like parseSimpleStatement does (factor a shared helper `reinterpretAsSimpleStmt(p, list)` used by both).
- `parseWhile`: cond + block.
- `parseTry`: body block; one or more `except` clauses (each: optional type expr, optional `as ident`, block); optional single `finally` block. At least one except or finally required (E203 otherwise).
- `parseWith`: expr, optional `as ident` (name slice into withStmt.name/nameLen), block.
- `parseSelect`: consume `select` `{`; loop cases until `}`: `case` → optional target prefix (ident list + COLON_EQUAL/EQUAL — detect via: parse idents while current is IDENTIFIER and peek is COMMA/COLON_EQUAL/EQUAL... simplest robust rule: if current is IDENTIFIER, scan with the star pre-pass logic: collect ident list while `,`; if then COLON_EQUAL or EQUAL, it was a target prefix) then a full expression; validate shape: expression must be `MS_AST_CALL` whose callee is `MS_AST_ATTR` with name `recv` (0 args → RECV) or `send` (1 arg → SEND, arg = value); otherwise E202. `channel` = the attr's target subtree. `default` → DEFAULT. Each case: consume COLON, then block (per 06-concurrency EBNF: `case ... ":" block`).
- `parseImport`: KW_IMPORT: expect STRING token (E203 otherwise) → path slice; optional `as ident`. KW_FROM: STRING path, consume KW_IMPORT, then either parenthesized ident list (trailing comma allowed) or bare ident list; names become IDENT child nodes.
- `parseFuncDecl(async)`: consume `func`, name ident, params, block. Params loop: STAR → VARARGS, DOUBLE_STAR → KWARGS, IDENTIFIER → NORMAL (optional `= defaultValue`). Order validation (E207): once a default-valued param appears, all following NORMAL params need defaults; VARARGS ends the NORMAL section; KWARGS must be last. Duplicate names → E208 (linear scan, param lists are short). Context: save `funcDepth/funcIsAsync/inMethod/loopDepth`, set funcDepth+1, funcIsAsync = async flag, inMethod per caller, loopDepth = 0 (closure barrier), parse body, restore.
- `parseClassDecl`: consume `class`, name, optional `<` + superclass expression, then block whose members are parsed by a class-member loop (NOT parseStatement): each member = optional contextual `static` (current IDENTIFIER with lexeme "static" AND `msLexerPeek == MS_TOKEN_KW_FUNC`) or KW_ASYNC, then KW_FUNC → `parseFuncDecl` with isStatic/isAsync flags and inMethod = true (non-static) — pin: static methods get inMethod = false (no self). Anything else in class body → E202. `classDepth` incremented around the body.
- Simple keyword statements: KW_RETURN (funcDepth == 0 → E210; optional expr list), KW_RAISE (expr, optional `from` expr), KW_DEL (target list, validate assignability E205), KW_GLOBAL (ident list → IDENT children), KW_BREAK/KW_CONTINUE (loopDepth == 0 → E209), KW_PASS.
- `parseTopLevelStmt` and `parseStatement` share one dispatch (pin 10): `KW_FUNC` / `KW_ASYNC`+`func` / `KW_CLASS` are valid in both. `KW_ASYNC` not followed by `KW_FUNC` → E202.

`parseStatement` additions: KW_IF/KW_FOR/KW_WHILE/KW_TRY/KW_WITH/KW_SELECT/KW_IMPORT/KW_FROM/KW_RETURN/KW_RAISE/KW_DEL/KW_GLOBAL/KW_BREAK/KW_CONTINUE/KW_PASS arms, each ending with `consumeOptionalStatementEnd` (compound, `}`-terminated) or `consumeStatementEnd` (simple).

- [ ] **Step 4: Run tests** — `ctest -R test_parser` all PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add src/parser/ms_parser.c tests/c/test_parser.c
git commit -m "✨ feat(parser): add compound statements, declarations, context checks"
```

---

## Task 9: f-string assembly

**Files:**
- Modify: `src/parser/ms_parser.c`
- Test: `tests/c/test_parser.c`

- [ ] **Step 1: Write the failing tests**

```c
MS_TEST(parser, fstringTextOnly) {
  msTestExpectDump("f\"hello\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_TEXT \"hello\"\n");
}

MS_TEST(parser, fstringInterpolation) {
  msTestExpectDump("f\"a{x}b{y:03d}c\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_TEXT \"a\"\n"
      "      MS_AST_FSTRING_EXPR\n"
      "        MS_AST_IDENT \"x\"\n"
      "      MS_AST_FSTRING_TEXT \"b\"\n"
      "      MS_AST_FSTRING_EXPR fmt=\"03d\"\n"
      "        MS_AST_IDENT \"y\"\n"
      "      MS_AST_FSTRING_TEXT \"c\"\n");
}

MS_TEST(parser, fstringNestedBracesInFormat) {
  // The format spec slice is stored raw, nested fields not expanded (task 28).
  msTestExpectDump("f\"{n:{w}d}\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_EXPR fmt=\"{w}d\"\n"
      "        MS_AST_IDENT \"n\"\n");
}

MS_TEST(parser, fstringNestedFstring) {
  msTestExpectDump("f\"{f'{x}'}\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_EXPR\n"
      "        MS_AST_FSTRING\n"
      "          MS_AST_FSTRING_EXPR\n"
      "            MS_AST_IDENT \"x\"\n");
}

MS_TEST(parser, fstringExprWithCallAndSlice) {
  msTestExpectDump("f\"{f(a)[1:]}\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_EXPR\n"
      "        MS_AST_SLICE parts=start\n"
      "          MS_AST_CALL\n"
      "            MS_AST_IDENT \"f\"\n"
      "            MS_AST_IDENT \"a\"\n"
      "          MS_AST_INT \"1\"\n");
}

MS_TEST(parser, fstringEmpty) {
  msTestExpectDump("f\"\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n");
}
```

(Caveat for the implementer: the exact FSTRING_TEXT lexeme slices and the empty-`f""` token shape come from the lexer's actual expansion — check `tests/c/test_lexer.c` f-string cases while writing these and adjust expected slices, not the lexer.)

- [ ] **Step 2: Run tests to verify they fail** — FSTRING_START has no prefix rule → E201.

- [ ] **Step 3: Implement `parseFString` prefix rule** (prec PRIMARY)

Loop until `MS_TOKEN_FSTRING_END` (consume it, done) or `MS_TOKEN_EOF` (E211, stop — do not consume EOF):

- `MS_TOKEN_STRING` → FSTRING_TEXT node (raw slice), advance.
- `MS_TOKEN_LEFT_BRACE` → advance, expr = `parsePrecedence(p, MS_PREC_LAMBDA + 1)`; optional `MS_TOKEN_FSTRING_FORMAT` → store raw slice (`hasFmt = true`); `parserConsume(p, MS_TOKEN_RIGHT_BRACE, ...)` (missing → E211); wrap in FSTRING_EXPR, push to parts.
- `MS_TOKEN_INVALID` → advance silently (lexer's diagnostic stands).
- anything else → E211 ("unterminated f-string interpolation" style message) + break out (the statement-level synchronize takes over).

A nested `FSTRING_START` inside an interpolation is just the prefix rule firing recursively (pin from spec) — no special code.

- [ ] **Step 4: Run tests** — `ctest -R test_parser` all PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add src/parser/ms_parser.c tests/c/test_parser.c
git commit -m "✨ feat(parser): add f-string assembly"
```

---

## Task 10: Error recovery, diagnostic cap, final sweep

**Files:**
- Modify: `src/parser/ms_parser.c`
- Test: `tests/c/test_parser.c`

- [ ] **Step 1: Write the failing tests**

```c
MS_TEST(parser, recoverAfterBadStatement) {
  // The bad first statement is replaced by an INVALID placeholder and the
  // following statements parse normally.
  MsTestFixture fix;
  msTestParse(&fix, "1 +\n2\n3");
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_TRUE(fix.root != NULL);
  char buf[MS_TEST_DUMP_CAP];
  msTestDump(fix.root, buf, sizeof(buf));
  MS_ASSERT_TRUE(strstr(buf, "MS_AST_INVALID") != NULL);
  MS_ASSERT_TRUE(strstr(buf, "MS_AST_INT \"2\"") != NULL);
  MS_ASSERT_TRUE(strstr(buf, "MS_AST_INT \"3\"") != NULL);
  msTestFixtureDestroy(&fix);
}

MS_TEST(parser, cascadeSuppression) {
  // One bad statement -> exactly one diagnostic, no cascade.
  static const uint32_t codes[] = { 201 };
  msTestExpectDiags("x = + + 1", codes, 1);
}

MS_TEST(parser, lexerInvalidNotReportedTwice) {
  // Bad character -> lexer E1xx only; the parser skips the INVALID token.
  MsTestFixture fix;
  msTestParse(&fix, "x := 1 $\ny := 2");
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(1, (long long)msDiagListCount(&fix.diags));
  uint32_t code = msDiagListAt(&fix.diags, 0)->code;
  MS_ASSERT_TRUE(code >= 100 && code < 200);   // lexical, not syntactic
  msTestFixtureDestroy(&fix);
}

MS_TEST(parser, diagCapStopsParse) {
  // 25 bad statements on separate lines -> exactly 20 diagnostics, parse
  // aborted early (MS_ERROR_SYNTAX).
  char source[256];
  source[0] = '\0';
  for (int i = 0; i < 25; ++i) {
    strcat(source, "1 +\n");
  }
  MsTestFixture fix;
  msTestParse(&fix, source);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(20, (long long)msDiagListCount(&fix.diags));
  msTestFixtureDestroy(&fix);
}

MS_TEST(parser, blockErrorSyncsToRightBrace) {
  // Error inside a block synchronizes at '}' and the outer statement after
  // the block still parses.
  MsTestFixture fix;
  msTestParse(&fix, "if a { 1 + }\n2");
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  char buf[MS_TEST_DUMP_CAP];
  msTestDump(fix.root, buf, sizeof(buf));
  MS_ASSERT_TRUE(strstr(buf, "MS_AST_INT \"2\"") != NULL);
  msTestFixtureDestroy(&fix);
}

MS_TEST(parser, unclosedBlock) {
  static const uint32_t codes[] = { 211 };
  msTestExpectDiags("if a { 1", codes, 1);
}
```

(The `$` in `lexerInvalidNotReportedTwice`: confirm `$` really is a lexical error in this lexer — check the lexer task's error table; any E1xx-producing byte works, adjust the source, not the lexer.)

- [ ] **Step 2: Run tests to verify they fail** (synchronize not implemented yet → cascades / hangs / wrong counts).

- [ ] **Step 3: Implement `synchronize` + integrate**

```c
static void synchronize(struct MsParser* p) {
  p->panicMode = false;
  while (!parserCheck(p, MS_TOKEN_EOF)) {
    if (parserMatch(p, MS_TOKEN_SEMICOLON)) {
      return;   // consumed: next token starts a fresh statement
    }
    switch (p->current.type) {
      case MS_TOKEN_KW_FUNC: case MS_TOKEN_KW_CLASS: case MS_TOKEN_KW_IF:
      case MS_TOKEN_KW_FOR: case MS_TOKEN_KW_WHILE: case MS_TOKEN_KW_TRY:
      case MS_TOKEN_KW_WITH: case MS_TOKEN_KW_SELECT: case MS_TOKEN_KW_IMPORT:
      case MS_TOKEN_KW_FROM: case MS_TOKEN_KW_RETURN: case MS_TOKEN_KW_RAISE:
      case MS_TOKEN_KW_DEL: case MS_TOKEN_KW_GLOBAL: case MS_TOKEN_KW_BREAK:
      case MS_TOKEN_KW_CONTINUE: case MS_TOKEN_KW_PASS:
      case MS_TOKEN_RIGHT_BRACE:
        return;   // not consumed: the block loop / dispatcher handles it
      default:
        parserAdvance(p);
    }
  }
}
```

Integration points:

- `parseTopLevelStmt` / `parseStatement`: wrap the dispatch — if `p->panicMode` was set during the statement (check flag before/after), call `synchronize` before returning. Pattern:

```c
static struct MsAst* parseStatement(struct MsParser* p) {
  bool wasPanic = p->panicMode;   // false in practice at entry
  struct MsAst* stmt = dispatchStatement(p);
  if (!wasPanic && p->panicMode) {
    synchronize(p);
  }
  return stmt;   // may be INVALID placeholder or NULL (empty statement)
}
```

- Expression-level errors produce `MS_AST_INVALID` nodes (already the case from Task 4's `parsePrecedence` prefix-miss path); make sure a statement whose expression turned INVALID still consumes its statement end normally — the synchronize only fires when panicMode is set, which already implies a diagnostic was recorded.
- Cap handling: `parserAdvance` already collapses the lexer's `MS_ERROR_SYNTAX` return into a permanent EOF token; the main loop then exits naturally. Additionally, in `msParserParse`, stop the statement loop when `msDiagListIsFull(p->diags)` becomes true (belt and braces: prevents infinite loops on pathological input where no token consumes).
- OOM: any NULL from `parserNewNode`/`msAstListPush` → report OOM once (guard with a sticky `bool oomReported` — additive field), unwind to `msParserParse`, return `MS_ERROR_OOM`.

- [ ] **Step 4: Run tests** — `ctest -R test_parser` all PASS; then full suite `ctest --test-dir build --output-on-failure` green.

- [ ] **Step 5: Final sweep**

- Re-read the acceptance criteria in `docs/tasks/04-parser-ast.md` and check each box:
  - guards `MSLANG_SRC_PARSER_MS_AST_H_` / `MSLANG_SRC_PARSER_MS_PARSER_H_`, headers self-contained (compile a TU that includes each header first).
  - style: 2-space indent, 120 col, K&R, star-to-type, no typedef'd internal structs, `//` comments only, pure ASCII.
  - no TBD/TODO in the new files.
- Coverage cross-check against the spec's 测试方案 list — every bullet maps to at least one test:
  - CRLF line/column: add one test parsing `"1\r\n  2"` asserting positions (2, 3).
  - `msAstArenaDestroy` → allocation stats zeroed: covered by Task 1's arena test pattern; add one parse-level leak test (`msMemResetStats` → parse → destroy fixture → `liveBlocks == 0`).
- Mark the checkboxes in `docs/tasks/04-parser-ast.md` (⬜ → ✅ in the header table; tick every 验收标准 box) and update `docs/tasks/README.md` row 04 to ✅.

- [ ] **Step 6: Commit**

```bash
git add src/parser/ms_parser.c tests/c/test_parser.c docs/tasks/04-parser-ast.md docs/tasks/README.md
git commit -m "✨ feat(parser): add error recovery and diagnostic cap handling"
```

---

## Self-review notes (plan author)

- Spec coverage: every 实现步骤 1–10 in `docs/tasks/04-parser-ast.md` maps to Task 1–10 here; every 验收标准 box is exercised by Step 5 of Task 10 plus the per-task tests.
- Pinned-beyond-spec decisions are all listed in "Spec-gap pins" — reviewers should check those against `docs/tasks/04-parser-ast.md` first.
- Known risk points for reviewers: (a) `not in` two-token handling inside the Pratt loop (pin 7), (b) for-clause vs for-in dispatch reusing `reinterpretAsSimpleStmt`, (c) select-case shape validation (E202), (d) f-string lexeme expectations vs actual lexer slices (Task 9 caveat), (e) pin 10 (nested func/class) deliberately resolves a conflict between the 03-syntax EBNF and the task-04 test plan — flag it in the final report for the user.
