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
  // Additive implementation detail beyond the spec struct: parsePrecedence
  // stashes the already-parsed left operand here before invoking an infix
  // rule, because MsParseFn carries no left parameter. Infix rules MUST copy
  // this to a local before any nested parsePrecedence call (the nested fold
  // loop overwrites it).
  struct MsAst* infixLeft;
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
