#include "parser/ms_parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/ms_common.h"

void msParserInit(struct MsParser* parser, struct MsLexer* lexer,
    struct MsAstArena* arena, struct MsDiagList* diags) {
  MS_ASSERT(parser != NULL);
  MS_ASSERT(lexer != NULL);
  MS_ASSERT(arena != NULL);
  MS_ASSERT(diags != NULL);
  memset(parser, 0, sizeof(*parser));
  parser->lexer = lexer;
  parser->arena = arena;
  parser->diags = diags;
}

// Shifts current into previous and pulls the next token. MS_TOKEN_INVALID
// tokens are skipped silently (the lexer already recorded their diagnostics).
// Once the diagnostic cap is hit the lexer reports MS_ERROR_SYNTAX with an
// EOF token and keeps yielding EOF (sticky), so the main loop simply exits.
static void parserAdvance(struct MsParser* p) {
  p->previous = p->current;
  for (;;) {
    if (msLexerNext(p->lexer, &p->current) != MS_OK) {
      p->current.type = MS_TOKEN_EOF;
      return;
    }
    if (p->current.type != MS_TOKEN_INVALID) {
      return;
    }
  }
}

static bool parserCheck(struct MsParser* p, MsTokenType type) {
  return p->current.type == type;
}

static bool parserMatch(struct MsParser* p, MsTokenType type) {
  if (!parserCheck(p, type)) {
    return false;
  }
  parserAdvance(p);
  return true;
}

static void parserErrorAtV(struct MsParser* p, const struct MsToken* token, uint32_t code,
    const char* fmt, va_list args) {
  if (p->panicMode) {
    return;
  }
  p->panicMode = true;
  // msDiagReport has no va_list entry point: format here, forward as text.
  char message[MS_DIAG_MESSAGE_LEN];
  vsnprintf(message, sizeof(message), fmt, args);
  msDiagReport(p->diags, token->line, token->column, code, "%s", message);
}

static void parserErrorAt(struct MsParser* p, const struct MsToken* token, uint32_t code,
    const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  parserErrorAtV(p, token, code, fmt, args);
  va_end(args);
}

static void parserConsume(struct MsParser* p, MsTokenType type, const char* fmt, ...) {
  if (parserMatch(p, type)) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  parserErrorAtV(p, &p->current, 203, fmt, args);
  va_end(args);
}

// Wraps msAstNew; on OOM reports once (panicMode acts as the sticky flag --
// the parse aborts before any synchronize() could clear it) and returns NULL.
static struct MsAst* parserNewNode(struct MsParser* p, MsAstKind kind, const struct MsToken* at) {
  struct MsAst* node = msAstNew(p->arena, kind, at->line, at->column);
  if (node == NULL) {
    parserErrorAt(p, at, 0, "out of memory");
  }
  return node;
}

// After a simple statement: exactly one semicolon, explicit or auto-inserted.
static void consumeStatementEnd(struct MsParser* p) {
  parserConsume(p, MS_TOKEN_SEMICOLON, "expected ';' after statement, got %s",
      msTokenTypeName(p->current.type));
}

// Temporary primary for the skeleton: literal and identifier tokens only.
// Task 4 replaces this with the real Pratt parser entry (parsePrecedence).
static struct MsAst* parseExpression(struct MsParser* p) {
  struct MsToken token = p->current;
  MsAstKind kind;
  switch (token.type) {
    case MS_TOKEN_INT:
      kind = MS_AST_INT;
      break;
    case MS_TOKEN_FLOAT:
      kind = MS_AST_FLOAT;
      break;
    case MS_TOKEN_STRING:
      kind = MS_AST_STRING;
      break;
    case MS_TOKEN_RAW_STRING:
      kind = MS_AST_RAW_STRING;
      break;
    case MS_TOKEN_BYTES:
      kind = MS_AST_BYTES;
      break;
    case MS_TOKEN_IDENTIFIER:
      kind = MS_AST_IDENT;
      break;
    case MS_TOKEN_KW_TRUE:
    case MS_TOKEN_KW_FALSE:
      kind = MS_AST_BOOL;
      break;
    case MS_TOKEN_KW_NIL:
      kind = MS_AST_NIL;
      break;
    default:
      parserErrorAt(p, &token, 201, "expected expression, got %s", msTokenTypeName(token.type));
      parserAdvance(p);
      // Placeholder keeps the tree traversable; the diagnostic stands above.
      return parserNewNode(p, MS_AST_INVALID, &token);
  }
  parserAdvance(p);
  struct MsAst* node = parserNewNode(p, kind, &token);
  if (node == NULL) {
    return NULL;
  }
  if (kind == MS_AST_BOOL) {
    node->as.boolLit.value = token.type == MS_TOKEN_KW_TRUE;
  } else if (kind != MS_AST_NIL) {
    node->as.literal.start = token.start;
    node->as.literal.length = token.length;
  }
  return node;
}

MsResult msParserParse(struct MsParser* p, struct MsAst** out) {
  MS_ASSERT(p != NULL);
  MS_ASSERT(out != NULL);
  *out = NULL;
  size_t diagsAtEntry = msDiagListCount(p->diags);
  parserAdvance(p);   // prime current
  struct MsAst* root = parserNewNode(p, MS_AST_SOURCE_FILE, &p->current);
  if (root == NULL) {
    return MS_ERROR_OOM;
  }
  *out = root;   // partial tree stays inspectable on failure
  while (!parserCheck(p, MS_TOKEN_EOF)) {
    if (parserCheck(p, MS_TOKEN_SEMICOLON)) {
      // A semicolon where a statement is expected: empty statement (E204).
      // No node is produced; just skip it.
      parserErrorAt(p, &p->current, 204, "empty statement; use 'pass' instead");
      parserAdvance(p);
      continue;
    }
    struct MsToken start = p->current;
    struct MsAst* expr = parseExpression(p);
    if (expr == NULL) {
      return MS_ERROR_OOM;
    }
    struct MsAst* stmt = parserNewNode(p, MS_AST_EXPR_STMT, &start);
    if (stmt == NULL) {
      return MS_ERROR_OOM;
    }
    stmt->as.exprStmt.expr = expr;
    consumeStatementEnd(p);
    if (!msAstListPush(p->arena, &root->as.block, stmt)) {
      parserErrorAt(p, &start, 0, "out of memory");
      return MS_ERROR_OOM;
    }
  }
  return msDiagListCount(p->diags) > diagsAtEntry ? MS_ERROR_SYNTAX : MS_OK;
}
