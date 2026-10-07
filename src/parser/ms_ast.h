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
  size_t capacity;
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
    struct { struct MsAst* expr; } exprStmt;                        // EXPR_STMT
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

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC C mode (-std:c11) does not implement C11 flexible array members and
// warns C4200; the layout and semantics used here are the standard ones.
#pragma warning(push)
#pragma warning(disable: 4200)
#endif

struct MsAstArenaBlock {
  struct MsAstArenaBlock* next;
  size_t used;
  size_t capacity;           // byte length of data[] (varies per block)
  uint8_t data[];            // flexible array member; block is
                             // msAlloc(sizeof(struct MsAstArenaBlock) + capacity)
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

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
// indent) for golden-file style unit tests. (Implemented in Task 2.)
void msAstDump(const struct MsAst* node, FILE* out);

#endif  // MSLANG_SRC_PARSER_MS_AST_H_
