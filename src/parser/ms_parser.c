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

// After a simple statement: exactly one semicolon, explicit or
// auto-inserted. As in Go, the semicolon may be omitted before a closing
// '}' (left for the enclosing parseBlock).
static void consumeStatementEnd(struct MsParser* p) {
  if (parserCheck(p, MS_TOKEN_RIGHT_BRACE)) {
    return;
  }
  parserConsume(p, MS_TOKEN_SEMICOLON, "expected ';' after statement, got %s",
      msTokenTypeName(p->current.type));
}

// After a block: one optional semicolon (explicit or auto-inserted).
static void consumeOptionalStatementEnd(struct MsParser* p) {
  parserMatch(p, MS_TOKEN_SEMICOLON);
}

// Panic-mode recovery (04-parser-ast, "error recovery and sync points"): clears panicMode,
// then discards tokens until a statement boundary -- a semicolon (consumed),
// a statement-start keyword or '{'/'}' (left for the dispatcher / block
// loop), or EOF. Called from parseStatement when the statement just parsed
// recorded a diagnostic.
static void synchronize(struct MsParser* p) {
  p->panicMode = false;
  while (!parserCheck(p, MS_TOKEN_EOF)) {
    if (parserMatch(p, MS_TOKEN_SEMICOLON)) {
      return;   // consumed: the next token starts a fresh statement
    }
    switch (p->current.type) {
      case MS_TOKEN_KW_FUNC: case MS_TOKEN_KW_CLASS: case MS_TOKEN_KW_IF:
      case MS_TOKEN_KW_FOR: case MS_TOKEN_KW_WHILE: case MS_TOKEN_KW_TRY:
      case MS_TOKEN_KW_WITH: case MS_TOKEN_KW_SELECT: case MS_TOKEN_KW_IMPORT:
      case MS_TOKEN_KW_FROM: case MS_TOKEN_KW_RETURN: case MS_TOKEN_KW_RAISE:
      case MS_TOKEN_KW_DEL: case MS_TOKEN_KW_GLOBAL: case MS_TOKEN_KW_BREAK:
      case MS_TOKEN_KW_CONTINUE: case MS_TOKEN_KW_PASS:
      case MS_TOKEN_LEFT_BRACE:    // a leading '{' is a block statement
      case MS_TOKEN_RIGHT_BRACE:
        return;   // not consumed: the block loop / dispatcher handles it
      default:
        parserAdvance(p);
    }
  }
}

// Pratt precedence levels: enum order is increasing binding power,
// matching 03-syntax section 6 from level 15 (lambda) down to 1 (primary).
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
} MsPrecedence;

// Entry level for a full expression: everything except lambda folds.
#define MS_PREC_EXPR ((MsPrecedence)(MS_PREC_LAMBDA + 1))

typedef struct MsAst* (*MsParseFn)(struct MsParser* p, bool canAssign);

struct MsParseRule {
  MsParseFn prefix;      // NULL: token cannot start an expression
  MsParseFn infix;       // NULL: not an operator
  MsPrecedence prec;
};

static struct MsAst* parseLiteral(struct MsParser* p, bool canAssign);
static struct MsAst* parseGrouping(struct MsParser* p, bool canAssign);
static struct MsAst* parseUnary(struct MsParser* p, bool canAssign);
static struct MsAst* parseBinary(struct MsParser* p, bool canAssign);
static struct MsAst* parseLogical(struct MsParser* p, bool canAssign);
static struct MsAst* parseCall(struct MsParser* p, bool canAssign);
static struct MsAst* parseIndexOrSlice(struct MsParser* p, bool canAssign);
static struct MsAst* parseAttr(struct MsParser* p, bool canAssign);
static struct MsAst* parseCompareChain(struct MsParser* p, bool canAssign);
static struct MsAst* parseConditional(struct MsParser* p, bool canAssign);
static struct MsAst* parseAwait(struct MsParser* p, bool canAssign);
static struct MsAst* parseLambda(struct MsParser* p, bool canAssign);
static struct MsAst* parseSelfSuper(struct MsParser* p, bool canAssign);
static struct MsAst* parseListOrComprehension(struct MsParser* p, bool canAssign);
static struct MsAst* parseBraceLiteral(struct MsParser* p, bool canAssign);
static struct MsAst* parseFString(struct MsParser* p, bool canAssign);
static struct MsAst* parsePrecedence(struct MsParser* p, MsPrecedence minPrec);
static struct MsAst* parseStatement(struct MsParser* p);
static struct MsAst* parseBlock(struct MsParser* p);
static struct MsAst* parseSimpleStatement(struct MsParser* p);

// Pratt rule table indexed by MsTokenType. Tokens without an entry
// zero-initialize to { NULL, NULL, MS_PREC_NONE } -- neither prefix nor
// infix -- so the fold loop stops at statement boundaries (semicolon,
// ')', ',') on its own. Later tasks add the remaining entries.
static const struct MsParseRule msParseRules[MS_TOKEN_ELLIPSIS + 1] = {
    [MS_TOKEN_IDENTIFIER]  = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_INT]         = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_FLOAT]       = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_STRING]      = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_RAW_STRING]  = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_BYTES]       = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_KW_TRUE]     = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_KW_FALSE]    = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_KW_NIL]      = { parseLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_LEFT_PAREN]    = { parseGrouping, parseCall, MS_PREC_POSTFIX },
    [MS_TOKEN_LEFT_BRACKET]  = { parseListOrComprehension, parseIndexOrSlice, MS_PREC_POSTFIX },
    [MS_TOKEN_LEFT_BRACE]    = { parseBraceLiteral, NULL, MS_PREC_NONE },
    [MS_TOKEN_FSTRING_START] = { parseFString, NULL, MS_PREC_NONE },
    [MS_TOKEN_DOT]           = { NULL, parseAttr, MS_PREC_POSTFIX },
    [MS_TOKEN_TILDE]         = { parseUnary, NULL, MS_PREC_NONE },
    [MS_TOKEN_KW_NOT]        = { parseUnary, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_PLUS]          = { parseUnary, parseBinary, MS_PREC_TERM },
    [MS_TOKEN_MINUS]         = { parseUnary, parseBinary, MS_PREC_TERM },
    [MS_TOKEN_STAR]          = { NULL, parseBinary, MS_PREC_FACTOR },
    [MS_TOKEN_SLASH]         = { NULL, parseBinary, MS_PREC_FACTOR },
    [MS_TOKEN_PERCENT]       = { NULL, parseBinary, MS_PREC_FACTOR },
    [MS_TOKEN_KW_DIV]        = { NULL, parseBinary, MS_PREC_FACTOR },
    [MS_TOKEN_DOUBLE_STAR]   = { NULL, parseBinary, MS_PREC_POWER },
    [MS_TOKEN_SHIFT_LEFT]    = { NULL, parseBinary, MS_PREC_SHIFT },
    [MS_TOKEN_SHIFT_RIGHT]   = { NULL, parseBinary, MS_PREC_SHIFT },
    [MS_TOKEN_AMP]           = { NULL, parseBinary, MS_PREC_BIT_AND },
    [MS_TOKEN_CARET]         = { NULL, parseBinary, MS_PREC_BIT_XOR },
    [MS_TOKEN_PIPE]          = { NULL, parseBinary, MS_PREC_BIT_OR },
    [MS_TOKEN_EQUAL_EQUAL]   = { NULL, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_BANG_EQUAL]    = { NULL, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_LESS]          = { NULL, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_LESS_EQUAL]    = { NULL, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_GREATER]       = { NULL, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_GREATER_EQUAL] = { NULL, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_KW_IN]         = { NULL, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_KW_IS]         = { NULL, parseCompareChain, MS_PREC_COMPARE },
    [MS_TOKEN_KW_AND]        = { NULL, parseLogical, MS_PREC_AND },
    [MS_TOKEN_KW_OR]         = { NULL, parseLogical, MS_PREC_OR },
    [MS_TOKEN_KW_IF]         = { NULL, parseConditional, MS_PREC_CONDITIONAL },
    [MS_TOKEN_KW_AWAIT]      = { parseAwait, NULL, MS_PREC_NONE },
    [MS_TOKEN_KW_LAMBDA]     = { parseLambda, NULL, MS_PREC_NONE },
    [MS_TOKEN_KW_SELF]       = { parseSelfSuper, NULL, MS_PREC_NONE },
    [MS_TOKEN_KW_SUPER]      = { parseSelfSuper, NULL, MS_PREC_NONE },
};

// Prefix: literals, identifiers, and the keyword constants. The token is
// p->previous (parsePrecedence already advanced past it).
static struct MsAst* parseLiteral(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken token = p->previous;
  MsAstKind kind = MS_AST_INVALID;
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
      // Unreachable: the rule table routes only the tokens above here.
      MS_UNREACHABLE();
      break;
  }
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

// Prefix: '(' expr ')'.
static struct MsAst* parseGrouping(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsAst* expr = parsePrecedence(p, MS_PREC_EXPR);
  parserConsume(p, MS_TOKEN_RIGHT_PAREN, "expected ')' after expression, got %s",
      msTokenTypeName(p->current.type));
  return expr;
}

// Prefix: +x -x ~x not x. The operand starts one level tighter than UNARY,
// which is the pin that makes POWER fold into the operand (-2 ** 2 ==
// -(2 ** 2)) while FACTOR does not (-2 * 3 == (-2) * 3).
static struct MsAst* parseUnary(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken op = p->previous;
  struct MsAst* operand = parsePrecedence(p, MS_PREC_UNARY + 1);
  if (operand == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_UNARY, &op);
  if (node == NULL) {
    return NULL;
  }
  node->as.unary.op = op.type;
  node->as.unary.operand = operand;
  return node;
}

// Infix: arithmetic, bitwise, and shift operators. The left operand arrives
// via p->infixLeft (stashed by parsePrecedence). Left-associative operators
// recurse at prec + 1; '**' is right-associative and recurses at its own
// level, so 2 ** 3 ** 4 == 2 ** (3 ** 4).
static struct MsAst* parseBinary(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken op = p->previous;
  struct MsAst* left = p->infixLeft;
  MsPrecedence prec = msParseRules[op.type].prec;
  MsPrecedence rightPrec = op.type == MS_TOKEN_DOUBLE_STAR ? prec : prec + 1;
  struct MsAst* right = parsePrecedence(p, rightPrec);
  if (right == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_BINARY, &op);
  if (node == NULL) {
    return NULL;
  }
  node->as.binary.op = op.type;
  node->as.binary.left = left;
  node->as.binary.right = right;
  return node;
}

// Infix: and / or (short-circuit logical, left-associative).
static struct MsAst* parseLogical(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken op = p->previous;
  struct MsAst* left = p->infixLeft;
  struct MsAst* right = parsePrecedence(p, msParseRules[op.type].prec + 1);
  if (right == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_LOGICAL, &op);
  if (node == NULL) {
    return NULL;
  }
  node->as.binary.op = op.type;
  node->as.binary.left = left;
  node->as.binary.right = right;
  return node;
}

// Infix: call arguments. Grammar: "(" [expr {"," expr} [","]] ")"; an
// identifier directly followed by '=' (2-token lookahead) is a keyword
// argument.
static struct MsAst* parseCall(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken paren = p->previous;
  struct MsAst* callee = p->infixLeft;
  struct MsAst* node = parserNewNode(p, MS_AST_CALL, &paren);
  if (node == NULL) {
    return NULL;
  }
  node->as.call.callee = callee;
  while (!parserCheck(p, MS_TOKEN_RIGHT_PAREN) && !parserCheck(p, MS_TOKEN_EOF)) {
    struct MsAst* arg = NULL;
    if (parserCheck(p, MS_TOKEN_IDENTIFIER) && msLexerPeek(p->lexer) == MS_TOKEN_EQUAL) {
      struct MsToken name = p->current;
      parserAdvance(p);   // name
      parserAdvance(p);   // '='
      struct MsAst* value = parsePrecedence(p, MS_PREC_EXPR);
      if (value == NULL) {
        return NULL;
      }
      arg = parserNewNode(p, MS_AST_KW_ARG, &name);
      if (arg == NULL) {
        return NULL;
      }
      arg->as.kwArg.start = name.start;
      arg->as.kwArg.length = name.length;
      arg->as.kwArg.value = value;
    } else {
      arg = parsePrecedence(p, MS_PREC_EXPR);
      if (arg == NULL) {
        return NULL;
      }
    }
    if (!msAstListPush(p->arena, &node->as.call.args, arg)) {
      parserErrorAt(p, &paren, 0, "out of memory");
      return NULL;
    }
    if (!parserMatch(p, MS_TOKEN_COMMA)) {
      break;
    }
  }
  parserConsume(p, MS_TOKEN_RIGHT_PAREN, "expected ')' after arguments, got %s",
      msTokenTypeName(p->current.type));
  return node;
}

// Infix: subscript a[i] or slice a[i:j:k] with every component optional.
// A component expression parses only when the current token is neither ':'
// nor ']'. "a[]" (no component, no colon) is E203.
static struct MsAst* parseIndexOrSlice(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken bracket = p->previous;
  struct MsAst* target = p->infixLeft;
  struct MsAst* start = NULL;
  struct MsAst* stop = NULL;
  struct MsAst* step = NULL;
  if (!parserCheck(p, MS_TOKEN_COLON) && !parserCheck(p, MS_TOKEN_RIGHT_BRACKET)) {
    start = parsePrecedence(p, MS_PREC_EXPR);
    if (start == NULL) {
      return NULL;
    }
  }
  bool isSlice = parserMatch(p, MS_TOKEN_COLON);
  if (isSlice) {
    if (!parserCheck(p, MS_TOKEN_COLON) && !parserCheck(p, MS_TOKEN_RIGHT_BRACKET)) {
      stop = parsePrecedence(p, MS_PREC_EXPR);
      if (stop == NULL) {
        return NULL;
      }
    }
    if (parserMatch(p, MS_TOKEN_COLON) && !parserCheck(p, MS_TOKEN_RIGHT_BRACKET)) {
      step = parsePrecedence(p, MS_PREC_EXPR);
      if (step == NULL) {
        return NULL;
      }
    }
  } else if (start == NULL) {
    parserErrorAt(p, &p->current, 203, "expected index or slice, got %s",
        msTokenTypeName(p->current.type));
    // Keep the tree complete: an INVALID placeholder stands in for the
    // missing index so no INDEX child is ever NULL.
    start = parserNewNode(p, MS_AST_INVALID, &p->current);
    if (start == NULL) {
      return NULL;
    }
  }
  parserConsume(p, MS_TOKEN_RIGHT_BRACKET, "expected ']' after subscript, got %s",
      msTokenTypeName(p->current.type));
  struct MsAst* node = parserNewNode(p, isSlice ? MS_AST_SLICE : MS_AST_INDEX, &bracket);
  if (node == NULL) {
    return NULL;
  }
  if (isSlice) {
    node->as.slice.target = target;
    node->as.slice.start = start;
    node->as.slice.stop = stop;
    node->as.slice.step = step;
  } else {
    node->as.index.target = target;
    node->as.index.index = start;
  }
  return node;
}

// Infix: attribute access a.b. The name must be a plain identifier ('self'
// and 'super' are keywords and cannot be attribute names).
static struct MsAst* parseAttr(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken dot = p->previous;
  struct MsAst* target = p->infixLeft;
  parserConsume(p, MS_TOKEN_IDENTIFIER, "expected attribute name after '.', got %s",
      msTokenTypeName(p->current.type));
  struct MsToken name = p->previous;   // the identifier, or '.' on error
  struct MsAst* node = parserNewNode(p, MS_AST_ATTR, &dot);
  if (node == NULL) {
    return NULL;
  }
  node->as.attr.start = name.start;
  node->as.attr.length = name.length;
  node->as.attr.target = target;
  return node;
}

// Normalizes a single-token comparison operator; two-token operators
// ('not in', 'is not') are resolved by parseCompareChain directly.
static MsCompareOp compareOpForToken(MsTokenType type) {
  switch (type) {
    case MS_TOKEN_EQUAL_EQUAL: return MS_CMP_EQ;
    case MS_TOKEN_BANG_EQUAL: return MS_CMP_NE;
    case MS_TOKEN_LESS: return MS_CMP_LT;
    case MS_TOKEN_LESS_EQUAL: return MS_CMP_LE;
    case MS_TOKEN_GREATER: return MS_CMP_GT;
    case MS_TOKEN_GREATER_EQUAL: return MS_CMP_GE;
    case MS_TOKEN_KW_IN: return MS_CMP_IN;
    case MS_TOKEN_KW_IS: return MS_CMP_IS;
    default:
      // Unreachable: the rule table routes only the tokens above here.
      MS_UNREACHABLE();
      return MS_CMP_EQ;
  }
}

// Infix: chained comparison a < b <= c. The first comparison creates the
// COMPARE_CHAIN node; further compare-level operators are consumed by the
// internal loop (operands at COMPARE + 1) instead of the Pratt fold loop,
// so the whole chain lands in one node. The ops array is arena-grown with
// capacity doubling, like msAstListPush.
static struct MsAst* parseCompareChain(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken op = p->previous;
  struct MsAst* left = p->infixLeft;
  struct MsAst* node = parserNewNode(p, MS_AST_COMPARE_CHAIN, &op);
  if (node == NULL) {
    return NULL;
  }
  if (!msAstListPush(p->arena, &node->as.compareChain.operands, left)) {
    parserErrorAt(p, &op, 0, "out of memory");
    return NULL;
  }
  MsCompareOp* ops = NULL;
  size_t opCount = 0;
  size_t opCapacity = 0;
  for (;;) {
    MsCompareOp cmp = MS_CMP_EQ;
    if (op.type == MS_TOKEN_KW_NOT) {
      // Reaching here as infix guarantees the next token is 'in' (the fold
      // loop checks msLexerPeek before folding KW_NOT).
      parserAdvance(p);   // 'in'
      cmp = MS_CMP_NOT_IN;
    } else if (op.type == MS_TOKEN_KW_IS) {
      cmp = parserMatch(p, MS_TOKEN_KW_NOT) ? MS_CMP_IS_NOT : MS_CMP_IS;
    } else {
      cmp = compareOpForToken(op.type);
    }
    if (opCount == opCapacity) {
      size_t newCapacity = opCapacity != 0 ? opCapacity * 2 : 4;
      MsCompareOp* newOps = msAstArenaAlloc(p->arena, newCapacity * sizeof(*newOps));
      if (newOps == NULL) {
        parserErrorAt(p, &op, 0, "out of memory");
        return NULL;
      }
      if (ops != NULL) {
        memcpy(newOps, ops, opCount * sizeof(*ops));
      }
      ops = newOps;
      opCapacity = newCapacity;
    }
    ops[opCount] = cmp;
    ++opCount;
    struct MsAst* operand = parsePrecedence(p, MS_PREC_COMPARE + 1);
    if (operand == NULL) {
      return NULL;
    }
    if (!msAstListPush(p->arena, &node->as.compareChain.operands, operand)) {
      parserErrorAt(p, &op, 0, "out of memory");
      return NULL;
    }
    // After an operand at COMPARE + 1, nothing tighter than COMPARE can
    // remain; a COMPARE-level infix here is always another chain link.
    // 'not' links only when followed by 'in'.
    if (msParseRules[p->current.type].prec != MS_PREC_COMPARE
        || msParseRules[p->current.type].infix == NULL
        || (p->current.type == MS_TOKEN_KW_NOT && msLexerPeek(p->lexer) != MS_TOKEN_KW_IN)) {
      break;
    }
    parserAdvance(p);
    op = p->previous;
  }
  node->as.compareChain.ops = ops;
  node->as.compareChain.opCount = opCount;
  return node;
}

// Infix: conditional expression "value if cond else elseValue" (right
// associative: the else branch recurses at CONDITIONAL). The condition
// parses at OR so it cannot swallow a nested 'if' or lambda.
static struct MsAst* parseConditional(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken ifToken = p->previous;
  struct MsAst* value = p->infixLeft;
  struct MsAst* cond = parsePrecedence(p, MS_PREC_OR);
  if (cond == NULL) {
    return NULL;
  }
  parserConsume(p, MS_TOKEN_KW_ELSE, "expected 'else' in conditional expression, got %s",
      msTokenTypeName(p->current.type));
  struct MsAst* elseValue = parsePrecedence(p, MS_PREC_CONDITIONAL);
  if (elseValue == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_CONDITIONAL, &ifToken);
  if (node == NULL) {
    return NULL;
  }
  node->as.conditional.value = value;
  node->as.conditional.cond = cond;
  node->as.conditional.elseValue = elseValue;
  return node;
}

// Prefix: await expr. The operand parses at POSTFIX, so 'await f(x) + 1'
// is '(await f(x)) + 1'. Context check (E210): only legal inside an async
// function; funcDepth/funcIsAsync track the innermost function.
static struct MsAst* parseAwait(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken token = p->previous;
  if (p->funcDepth == 0 || !p->funcIsAsync) {
    parserErrorAt(p, &token, 210, "'await' outside async function");
  }
  struct MsAst* operand = parsePrecedence(p, MS_PREC_POSTFIX);
  if (operand == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_AWAIT, &token);
  if (node == NULL) {
    return NULL;
  }
  node->as.awaitExpr.operand = operand;
  return node;
}

// Prefix: lambda [ident {"," ident}] ":" expr. Reuses the func union branch
// with a NULL name; the body is a single expression at CONDITIONAL (a bare
// nested lambda needs parens). The body is a new function scope: funcDepth
// and funcIsAsync are saved and reset around it, so 'await'/'return' context
// does not leak across the boundary in either direction.
static struct MsAst* parseLambda(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken token = p->previous;
  struct MsAst* node = parserNewNode(p, MS_AST_LAMBDA, &token);
  if (node == NULL) {
    return NULL;
  }
  while (parserCheck(p, MS_TOKEN_IDENTIFIER)) {
    struct MsToken name = p->current;
    parserAdvance(p);
    struct MsAst* param = parserNewNode(p, MS_AST_PARAM, &name);
    if (param == NULL) {
      return NULL;
    }
    param->as.param.name = name.start;
    param->as.param.nameLen = name.length;
    param->as.param.paramKind = MS_PARAM_NORMAL;
    if (!msAstListPush(p->arena, &node->as.func.params, param)) {
      parserErrorAt(p, &name, 0, "out of memory");
      return NULL;
    }
    if (!parserMatch(p, MS_TOKEN_COMMA)) {
      break;
    }
  }
  parserConsume(p, MS_TOKEN_COLON, "expected ':' after lambda parameters, got %s",
      msTokenTypeName(p->current.type));
  int savedLoopDepth = p->loopDepth;
  int savedFuncDepth = p->funcDepth;
  bool savedFuncIsAsync = p->funcIsAsync;
  p->loopDepth = 0;
  p->funcDepth = savedFuncDepth + 1;
  p->funcIsAsync = false;
  struct MsAst* body = parsePrecedence(p, MS_PREC_CONDITIONAL);
  p->loopDepth = savedLoopDepth;
  p->funcDepth = savedFuncDepth;
  p->funcIsAsync = savedFuncIsAsync;
  if (body == NULL) {
    return NULL;
  }
  node->as.func.body = body;
  return node;
}

// Prefix: self / super. Both parse as plain IDENT nodes; the context check
// (E210) fires outside a class method.
static struct MsAst* parseSelfSuper(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken token = p->previous;
  if (!p->inMethod) {
    parserErrorAt(p, &token, 210, "'self'/'super' outside class method");
  }
  struct MsAst* node = parserNewNode(p, MS_AST_IDENT, &token);
  if (node == NULL) {
    return NULL;
  }
  node->as.literal.start = token.start;
  node->as.literal.length = token.length;
  return node;
}

// Parses the clause tail of a comprehension: one or more 'for'/'if' clauses
// appended to comp->as.comprehension.clauses in order. A for-clause reuses
// MS_AST_FOR (forKind MS_FOR_IN, body NULL); an if-clause reuses MS_AST_IF
// (thenBlock/elseBranch NULL). The iterable and the if condition parse at
// MS_PREC_OR, the pin that stops them from swallowing a following clause:
// 'if' as an infix is CONDITIONAL (< OR) and 'for' has no infix rule at all.
// Returns false on OOM.
static bool parseComprehensionClauses(struct MsParser* p, struct MsAst* comp) {
  while (parserCheck(p, MS_TOKEN_KW_FOR) || parserCheck(p, MS_TOKEN_KW_IF)) {
    struct MsToken keyword = p->current;
    parserAdvance(p);
    struct MsAst* clause = NULL;
    if (keyword.type == MS_TOKEN_KW_FOR) {
      clause = parserNewNode(p, MS_AST_FOR, &keyword);
      if (clause == NULL) {
        return false;
      }
      clause->as.forStmt.forKind = MS_FOR_IN;
      do {
        parserConsume(p, MS_TOKEN_IDENTIFIER, "expected identifier after 'for', got %s",
            msTokenTypeName(p->current.type));
        struct MsToken name = p->previous;   // the identifier, or the token before the error
        struct MsAst* target = parserNewNode(p, MS_AST_IDENT, &name);
        if (target == NULL) {
          return false;
        }
        target->as.literal.start = name.start;
        target->as.literal.length = name.length;
        if (!msAstListPush(p->arena, &clause->as.forStmt.targets, target)) {
          parserErrorAt(p, &keyword, 0, "out of memory");
          return false;
        }
      } while (parserMatch(p, MS_TOKEN_COMMA));
      parserConsume(p, MS_TOKEN_KW_IN, "expected 'in' in comprehension, got %s",
          msTokenTypeName(p->current.type));
      struct MsAst* iterable = parsePrecedence(p, MS_PREC_OR);
      if (iterable == NULL) {
        return false;
      }
      clause->as.forStmt.iterable = iterable;
    } else {
      struct MsAst* cond = parsePrecedence(p, MS_PREC_OR);
      if (cond == NULL) {
        return false;
      }
      clause = parserNewNode(p, MS_AST_IF, &keyword);
      if (clause == NULL) {
        return false;
      }
      clause->as.ifStmt.cond = cond;
    }
    if (!msAstListPush(p->arena, &comp->as.comprehension.clauses, clause)) {
      parserErrorAt(p, &keyword, 0, "out of memory");
      return false;
    }
  }
  return true;
}

// Prefix: list literal or list/dict comprehension. '[]' is an empty
// LIST_LIT; otherwise the first element decides: ':' starts the bracket
// dict comprehension [k: v for ...] (03-syntax section 6.2; there is no
// bracket dict literal, so 'for' is mandatory), KW_FOR means LIST_COMP
// (elem + clauses), anything else means LIST_LIT (','-separated elements,
// trailing comma allowed).
static struct MsAst* parseListOrComprehension(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken bracket = p->previous;
  struct MsAst* node = parserNewNode(p, MS_AST_LIST_LIT, &bracket);
  if (node == NULL) {
    return NULL;
  }
  if (parserMatch(p, MS_TOKEN_RIGHT_BRACKET)) {
    return node;
  }
  struct MsAst* first = parsePrecedence(p, MS_PREC_EXPR);
  if (first == NULL) {
    return NULL;
  }
  if (parserMatch(p, MS_TOKEN_COLON)) {
    struct MsAst* value = parsePrecedence(p, MS_PREC_EXPR);
    if (value == NULL) {
      return NULL;
    }
    node->kind = MS_AST_DICT_COMP;
    node->as.comprehension.key = first;
    node->as.comprehension.elem = value;
    if (!parserCheck(p, MS_TOKEN_KW_FOR)) {
      parserErrorAt(p, &p->current, 203, "expected 'for' in dict comprehension, got %s",
          msTokenTypeName(p->current.type));
    }
    if (!parseComprehensionClauses(p, node)) {
      return NULL;
    }
  } else if (parserCheck(p, MS_TOKEN_KW_FOR)) {
    node->kind = MS_AST_LIST_COMP;
    node->as.comprehension.elem = first;
    if (!parseComprehensionClauses(p, node)) {
      return NULL;
    }
  } else {
    if (!msAstListPush(p->arena, &node->as.block, first)) {
      parserErrorAt(p, &bracket, 0, "out of memory");
      return NULL;
    }
    while (parserMatch(p, MS_TOKEN_COMMA) && !parserCheck(p, MS_TOKEN_RIGHT_BRACKET)) {
      struct MsAst* elem = parsePrecedence(p, MS_PREC_EXPR);
      if (elem == NULL) {
        return NULL;
      }
      if (!msAstListPush(p->arena, &node->as.block, elem)) {
        parserErrorAt(p, &bracket, 0, "out of memory");
        return NULL;
      }
    }
  }
  parserConsume(p, MS_TOKEN_RIGHT_BRACKET, "expected ']' after list, got %s",
      msTokenTypeName(p->current.type));
  return node;
}

// Prefix: set/dict literal or set/dict comprehension. '{}' is always an
// empty DICT_LIT (Python rule). Otherwise the first expression decides:
// ':' starts the dict path (DICT_COMP when KW_FOR follows the value -- the
// brace dict-comp form, kept alongside the spec's bracket form; DICT_LIT
// with key/value pairs interleaved otherwise); KW_FOR directly means
// SET_COMP; anything else means SET_LIT. Trailing commas allowed.
// A '{' at statement start is a block: parseStatement dispatches to
// parseBlock before any expression parsing, so this rule only fires in
// expression position.
static struct MsAst* parseBraceLiteral(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken brace = p->previous;
  struct MsAst* node = parserNewNode(p, MS_AST_DICT_LIT, &brace);
  if (node == NULL) {
    return NULL;
  }
  if (parserMatch(p, MS_TOKEN_RIGHT_BRACE)) {
    return node;
  }
  struct MsAst* first = parsePrecedence(p, MS_PREC_EXPR);
  if (first == NULL) {
    return NULL;
  }
  if (parserMatch(p, MS_TOKEN_COLON)) {
    struct MsAst* value = parsePrecedence(p, MS_PREC_EXPR);
    if (value == NULL) {
      return NULL;
    }
    if (parserCheck(p, MS_TOKEN_KW_FOR)) {
      node->kind = MS_AST_DICT_COMP;
      node->as.comprehension.key = first;
      node->as.comprehension.elem = value;
      if (!parseComprehensionClauses(p, node)) {
        return NULL;
      }
    } else {
      if (!msAstListPush(p->arena, &node->as.block, first)
          || !msAstListPush(p->arena, &node->as.block, value)) {
        parserErrorAt(p, &brace, 0, "out of memory");
        return NULL;
      }
      while (parserMatch(p, MS_TOKEN_COMMA) && !parserCheck(p, MS_TOKEN_RIGHT_BRACE)) {
        struct MsAst* key = parsePrecedence(p, MS_PREC_EXPR);
        if (key == NULL) {
          return NULL;
        }
        parserConsume(p, MS_TOKEN_COLON, "expected ':' in dict literal, got %s",
            msTokenTypeName(p->current.type));
        value = parsePrecedence(p, MS_PREC_EXPR);
        if (value == NULL) {
          return NULL;
        }
        if (!msAstListPush(p->arena, &node->as.block, key)
            || !msAstListPush(p->arena, &node->as.block, value)) {
          parserErrorAt(p, &brace, 0, "out of memory");
          return NULL;
        }
      }
    }
  } else if (parserCheck(p, MS_TOKEN_KW_FOR)) {
    node->kind = MS_AST_SET_COMP;
    node->as.comprehension.elem = first;
    if (!parseComprehensionClauses(p, node)) {
      return NULL;
    }
  } else {
    node->kind = MS_AST_SET_LIT;
    if (!msAstListPush(p->arena, &node->as.block, first)) {
      parserErrorAt(p, &brace, 0, "out of memory");
      return NULL;
    }
    while (parserMatch(p, MS_TOKEN_COMMA) && !parserCheck(p, MS_TOKEN_RIGHT_BRACE)) {
      struct MsAst* elem = parsePrecedence(p, MS_PREC_EXPR);
      if (elem == NULL) {
        return NULL;
      }
      if (!msAstListPush(p->arena, &node->as.block, elem)) {
        parserErrorAt(p, &brace, 0, "out of memory");
        return NULL;
      }
    }
  }
  parserConsume(p, MS_TOKEN_RIGHT_BRACE, "expected '}' after literal, got %s",
      msTokenTypeName(p->current.type));
  return node;
}

// Prefix: f-string assembly. The lexer expands f"..." into a flat token
// sequence (FSTRING_START, then alternating text-segment STRINGs and
// interpolation groups '{' expr [FSTRING_FORMAT] '}', terminated by
// FSTRING_END); nested f-strings recurse through this same prefix rule.
// A text segment keeps the token's raw slice; the format spec is stored raw
// too (nested fields are not expanded here). An unterminated f-string (EOF
// before FSTRING_END) or a missing '}' after an interpolation is E211, the
// unclosed-construct code, not the generic E203.
static struct MsAst* parseFString(struct MsParser* p, bool canAssign) {
  MS_UNUSED(canAssign);
  struct MsToken start = p->previous;   // FSTRING_START
  struct MsAst* node = parserNewNode(p, MS_AST_FSTRING, &start);
  if (node == NULL) {
    return NULL;
  }
  for (;;) {
    if (parserMatch(p, MS_TOKEN_FSTRING_END)) {
      break;
    }
    if (parserCheck(p, MS_TOKEN_EOF)) {
      parserErrorAt(p, &p->current, 211, "unterminated f-string");
      break;
    }
    if (parserCheck(p, MS_TOKEN_STRING)) {
      struct MsToken text = p->current;
      parserAdvance(p);
      struct MsAst* part = parserNewNode(p, MS_AST_FSTRING_TEXT, &text);
      if (part == NULL) {
        return NULL;
      }
      part->as.literal.start = text.start;
      part->as.literal.length = text.length;
      if (!msAstListPush(p->arena, &node->as.fstring.parts, part)) {
        parserErrorAt(p, &text, 0, "out of memory");
        return NULL;
      }
      continue;
    }
    if (parserMatch(p, MS_TOKEN_LEFT_BRACE)) {
      struct MsToken brace = p->previous;
      struct MsAst* expr = parsePrecedence(p, MS_PREC_EXPR);
      if (expr == NULL) {
        return NULL;
      }
      struct MsAst* part = parserNewNode(p, MS_AST_FSTRING_EXPR, &brace);
      if (part == NULL) {
        return NULL;
      }
      part->as.fstringExpr.expr = expr;
      if (parserCheck(p, MS_TOKEN_FSTRING_FORMAT)) {
        part->as.fstringExpr.fmt = p->current.start;
        part->as.fstringExpr.fmtLen = p->current.length;
        part->as.fstringExpr.hasFmt = true;
        parserAdvance(p);
      }
      if (!parserMatch(p, MS_TOKEN_RIGHT_BRACE)) {
        parserErrorAt(p, &p->current, 211, "expected '}' after interpolation, got %s",
            msTokenTypeName(p->current.type));
      }
      if (!msAstListPush(p->arena, &node->as.fstring.parts, part)) {
        parserErrorAt(p, &brace, 0, "out of memory");
        return NULL;
      }
      continue;
    }
    parserErrorAt(p, &p->current, 211, "unexpected %s in f-string",
        msTokenTypeName(p->current.type));
    break;
  }
  return node;
}

// Pratt fold loop: parse one prefix, then keep folding infix operators
// whose binding power is at least minPrec. 'not' folds as the infix
// operator of a comparison chain only when 'in' follows (msLexerPeek);
// otherwise a leading 'not' stays the unary prefix (so 'not a in b' is
// '(not a) in b', deliberately unlike Python). A prefix miss reports E201
// WITHOUT consuming the offending token, so the statement-level
// synchronize() still sees boundary tokens (';', '}') that follow it.
static struct MsAst* parsePrecedence(struct MsParser* p, MsPrecedence minPrec) {
  MsParseFn prefix = msParseRules[p->current.type].prefix;
  if (prefix == NULL) {
    parserErrorAt(p, &p->current, 201, "expected expression, got %s",
        msTokenTypeName(p->current.type));
    return parserNewNode(p, MS_AST_INVALID, &p->current);   // tree stays complete
  }
  parserAdvance(p);
  struct MsAst* left = prefix(p, true);
  while (left != NULL && msParseRules[p->current.type].infix != NULL
      && msParseRules[p->current.type].prec >= minPrec
      && (p->current.type != MS_TOKEN_KW_NOT || msLexerPeek(p->lexer) == MS_TOKEN_KW_IN)) {
    parserAdvance(p);
    p->infixLeft = left;
    left = msParseRules[p->previous.type].infix(p, true);
  }
  return left;
}

// True for the augmented-assignment operators, which are contiguous in
// MsTokenType (MS_TOKEN_PLUS_EQUAL .. MS_TOKEN_SHIFT_RIGHT_EQUAL).
static bool parserIsAugAssign(MsTokenType type) {
  return type >= MS_TOKEN_PLUS_EQUAL && type <= MS_TOKEN_SHIFT_RIGHT_EQUAL;
}

// True for the '*' unpack target form built by parseSimpleStatement's
// pre-pass, the only source of UNARY nodes with op == MS_TOKEN_STAR.
static bool astIsUnpackTarget(const struct MsAst* node) {
  return node->kind == MS_AST_UNARY && node->as.unary.op == MS_TOKEN_STAR
      && node->as.unary.operand != NULL && node->as.unary.operand->kind == MS_AST_IDENT;
}

// Reports at an already-consumed node's position: reinterpretation errors
// (E205/E212) surface only after the target tokens are gone.
static void parserErrorAtNode(struct MsParser* p, const struct MsAst* node, uint32_t code,
    const char* message) {
  struct MsToken at;
  memset(&at, 0, sizeof(at));
  at.line = node->line;
  at.column = node->column;
  parserErrorAt(p, &at, code, "%s", message);
}

// True for the assignment target forms: plain identifiers, subscripts, and
// attribute accesses.
static bool astIsAssignableTarget(const struct MsAst* node) {
  return node->kind == MS_AST_IDENT || node->kind == MS_AST_INDEX || node->kind == MS_AST_ATTR;
}

// Validates an assignment/declaration target list (E205/E212): IDENT, INDEX
// and ATTR are assignable; a '*' unpack target is legal only when allowUnpack
// (':=' declarations and for-in) and at most once per list.
static void validateAssignTargets(struct MsParser* p, const struct MsAstList* targets, bool allowUnpack) {
  size_t starCount = 0;
  struct MsAst* lastStar = NULL;
  for (size_t i = 0; i < targets->count; ++i) {
    struct MsAst* target = targets->items[i];
    if (astIsUnpackTarget(target)) {
      ++starCount;
      lastStar = target;
      continue;
    }
    if (!astIsAssignableTarget(target)) {
      parserErrorAtNode(p, target, 205, "invalid assignment target");
    }
  }
  if (starCount > 1) {
    parserErrorAtNode(p, lastStar, 212, "too many unpack targets");
  } else if (starCount == 1 && !allowUnpack) {
    parserErrorAtNode(p, lastStar, 212, "unpack target only allowed in ':=' or 'for'-'in'");
  }
}

// Collects the comma-separated item list that opens a simple statement,
// parsing each item at minPrec. A '*' directly followed by an identifier
// starts an unpack target: STAR has no prefix rule, so the pre-pass builds
// UNARY(STAR, IDENT) here or '*rest := xs' would surface as E201.
// Returns false on OOM.
static bool parseSimpleItems(struct MsParser* p, const struct MsToken* start, struct MsAstList* items,
    MsPrecedence minPrec) {
  for (;;) {
    struct MsAst* item = NULL;
    if (parserCheck(p, MS_TOKEN_STAR) && msLexerPeek(p->lexer) == MS_TOKEN_IDENTIFIER) {
      struct MsToken star = p->current;
      parserAdvance(p);   // '*'
      struct MsToken name = p->current;
      parserAdvance(p);   // identifier
      struct MsAst* operand = parserNewNode(p, MS_AST_IDENT, &name);
      if (operand == NULL) {
        return false;
      }
      operand->as.literal.start = name.start;
      operand->as.literal.length = name.length;
      item = parserNewNode(p, MS_AST_UNARY, &star);
      if (item == NULL) {
        return false;
      }
      item->as.unary.op = MS_TOKEN_STAR;
      item->as.unary.operand = operand;
    } else {
      item = parsePrecedence(p, minPrec);
      if (item == NULL) {
        return false;
      }
    }
    if (!msAstListPush(p->arena, items, item)) {
      parserErrorAt(p, start, 0, "out of memory");
      return false;
    }
    if (!parserMatch(p, MS_TOKEN_COMMA)) {
      return true;
    }
  }
}

// Reinterprets a parsed item list as declStmt / assignStmt / incDecStmt /
// exprStmt by the token that follows it. Validation errors (E205/E212) still
// produce the DECL/ASSIGN node with every parsed child, and the right side is
// always consumed so the statement boundary is found. The '++'/'--' statement
// form yields MS_AST_INC_DEC only when p->allowIncDec is set (the post segment
// of a three-part for clause); everywhere else it is E206 and falls through to
// EXPR_STMT.
// Returns NULL only on OOM.
static struct MsAst* reinterpretAsSimpleStmt(struct MsParser* p, const struct MsToken* start,
    struct MsAstList* targets) {
  MsTokenType op = p->current.type;
  if (op == MS_TOKEN_COLON_EQUAL || op == MS_TOKEN_EQUAL || parserIsAugAssign(op)) {
    validateAssignTargets(p, targets, op == MS_TOKEN_COLON_EQUAL);
    parserAdvance(p);   // ':=' / '=' / augOp
    struct MsAstList values = { NULL, 0, 0 };
    do {
      struct MsAst* value = parsePrecedence(p, MS_PREC_EXPR);
      if (value == NULL) {
        return NULL;
      }
      if (!msAstListPush(p->arena, &values, value)) {
        parserErrorAt(p, start, 0, "out of memory");
        return NULL;
      }
    } while (parserMatch(p, MS_TOKEN_COMMA));
    if (parserIsAugAssign(op)) {
      // Augmented assignment is strictly single-target/single-value: extra
      // items still parse (the statement boundary must be found), but they
      // are E205 and the node is built from the first pair only.
      if (targets->count > 1) {
        parserErrorAtNode(p, targets->items[1], 205, "invalid augmented assignment target");
      } else if (values.count > 1) {
        parserErrorAtNode(p, values.items[1], 205, "invalid augmented assignment target");
      }
      struct MsAst* node = parserNewNode(p, MS_AST_AUG_ASSIGN, start);
      if (node == NULL) {
        return NULL;
      }
      node->as.augAssign.op = op;
      node->as.augAssign.target = targets->items[0];
      node->as.augAssign.value = values.items[0];
      return node;
    }
    struct MsAst* node = parserNewNode(p,
        op == MS_TOKEN_COLON_EQUAL ? MS_AST_DECL : MS_AST_ASSIGN, start);
    if (node == NULL) {
      return NULL;
    }
    node->as.decl.targets = *targets;
    node->as.decl.values = values;
    return node;
  }

  if (op == MS_TOKEN_PLUS_PLUS || op == MS_TOKEN_MINUS_MINUS) {
    struct MsToken opToken = p->current;
    parserAdvance(p);   // '++' / '--'
    if (!p->allowIncDec) {
      parserErrorAt(p, &opToken, 206, "'++'/'--' statement only allowed in a for clause");
      // Fall through to EXPR_STMT on purpose: the E206 above set panic mode,
      // so any follow-on diagnostic from the shared tail is suppressed.
    } else if (targets->count > 1) {
      parserErrorAt(p, &opToken, 203, "expected ':=' or '=' after expression list");
    } else if (!astIsAssignableTarget(targets->items[0])) {
      parserErrorAtNode(p, targets->items[0], 205, "invalid '++'/'--' target");
    } else {
      struct MsAst* node = parserNewNode(p, MS_AST_INC_DEC, start);
      if (node == NULL) {
        return NULL;
      }
      node->as.augAssign.op = op;
      node->as.augAssign.target = targets->items[0];
      node->as.augAssign.value = NULL;
      return node;
    }
  }

  if (targets->count > 1) {
    parserErrorAt(p, &p->current, 203, "expected ':=' or '=' after expression list");
  } else if (astIsUnpackTarget(targets->items[0])) {
    parserErrorAtNode(p, targets->items[0], 212, "unpack target only allowed in ':=' or 'for'-'in'");
  }
  struct MsAst* stmt = parserNewNode(p, MS_AST_EXPR_STMT, start);
  if (stmt == NULL) {
    return NULL;
  }
  stmt->as.exprStmt.expr = targets->items[0];
  return stmt;
}

// declStmt / assignStmt / exprStmt, by parse-then-reinterpret: the left side
// parses as a plain expression list and the token after it decides the node
// kind. Returns NULL only on OOM.
static struct MsAst* parseSimpleStatement(struct MsParser* p) {
  struct MsToken start = p->current;
  struct MsAstList targets = { NULL, 0, 0 };
  if (!parseSimpleItems(p, &start, &targets, MS_PREC_EXPR)) {
    return NULL;
  }
  return reinterpretAsSimpleStmt(p, &start, &targets);
}

// "{" { statement } "}". A missing '}' is E211 (unclosed construct), not
// the generic E203 parserConsume would report. Returns NULL only on OOM.
static struct MsAst* parseBlock(struct MsParser* p) {
  struct MsToken brace = p->current;
  parserAdvance(p);   // '{'
  struct MsAst* node = parserNewNode(p, MS_AST_BLOCK, &brace);
  if (node == NULL) {
    return NULL;
  }
  while (!parserCheck(p, MS_TOKEN_RIGHT_BRACE) && !parserCheck(p, MS_TOKEN_EOF)
      && !msDiagListIsFull(p->diags)) {
    struct MsToken start = p->current;
    struct MsAst* stmt = parseStatement(p);
    if (stmt == NULL) {
      // Empty statement (E204, no node) or OOM; see parseStatement.
      if (start.type != MS_TOKEN_SEMICOLON) {
        return NULL;
      }
      continue;
    }
    if (!msAstListPush(p->arena, &node->as.block, stmt)) {
      parserErrorAt(p, &brace, 0, "out of memory");
      return NULL;
    }
  }
  if (!parserMatch(p, MS_TOKEN_RIGHT_BRACE)) {
    parserErrorAt(p, &p->current, 211, "expected '}'");
  }
  return node;
}

// Parses the mandatory '{' block of a compound statement. A missing '{' is
// E203 and yields an empty placeholder block so the tree stays complete;
// returns NULL only on OOM.
static struct MsAst* parseRequiredBlock(struct MsParser* p) {
  if (!parserCheck(p, MS_TOKEN_LEFT_BRACE)) {
    parserErrorAt(p, &p->current, 203, "expected '{' to start a block, got %s",
        msTokenTypeName(p->current.type));
    return parserNewNode(p, MS_AST_BLOCK, &p->current);   // placeholder keeps the tree complete
  }
  return parseBlock(p);
}

// "if" expr block { "else" "if" expr block } [ "else" block ]. An else-if
// chain nests as another MS_AST_IF in elseBranch.
static struct MsAst* parseIf(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'if'
  struct MsAst* cond = parsePrecedence(p, MS_PREC_EXPR);
  if (cond == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_IF, &keyword);
  if (node == NULL) {
    return NULL;
  }
  node->as.ifStmt.cond = cond;
  node->as.ifStmt.thenBlock = parseRequiredBlock(p);
  if (node->as.ifStmt.thenBlock == NULL) {
    return NULL;
  }
  if (parserMatch(p, MS_TOKEN_KW_ELSE)) {
    struct MsAst* elseBranch =
        parserCheck(p, MS_TOKEN_KW_IF) ? parseIf(p) : parseRequiredBlock(p);
    if (elseBranch == NULL) {
      return NULL;
    }
    node->as.ifStmt.elseBranch = elseBranch;
  }
  return node;
}

// "for" ( block | forClause block | identList "in" expr block ). The first
// segment parses through the same item-list machinery as a simple statement,
// but one level tighter than comparisons (MS_PREC_COMPARE + 1) so a for-in
// 'in' is not folded into the last target as a comparison chain. 'in' then
// selects the for-in form (targets validated like ':=', unpack allowed);
// anything else is reinterpreted as the three-part clause's init statement
// (E203 when no ';' follows, so 'for x { }' is rejected). '++'/'--' is legal
// in the post segment only; the clause ends exactly at the body's '{' (no
// semicolon may follow the post segment).
static struct MsAst* parseFor(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'for'
  struct MsAst* node = parserNewNode(p, MS_AST_FOR, &keyword);
  if (node == NULL) {
    return NULL;
  }
  if (parserCheck(p, MS_TOKEN_LEFT_BRACE)) {
    node->as.forStmt.forKind = MS_FOR_BARE;
    ++p->loopDepth;
    node->as.forStmt.body = parseBlock(p);
    --p->loopDepth;
    return node->as.forStmt.body != NULL ? node : NULL;
  }
  struct MsToken start = p->current;
  struct MsAstList items = { NULL, 0, 0 };
  if (!parseSimpleItems(p, &start, &items, MS_PREC_COMPARE + 1)) {
    return NULL;
  }
  if (parserMatch(p, MS_TOKEN_KW_IN)) {
    node->as.forStmt.forKind = MS_FOR_IN;
    validateAssignTargets(p, &items, true);
    node->as.forStmt.targets = items;
    node->as.forStmt.iterable = parsePrecedence(p, MS_PREC_EXPR);
    if (node->as.forStmt.iterable == NULL) {
      return NULL;
    }
  } else {
    node->as.forStmt.forKind = MS_FOR_CLAUSE;
    node->as.forStmt.init = reinterpretAsSimpleStmt(p, &start, &items);
    if (node->as.forStmt.init == NULL) {
      return NULL;
    }
    parserConsume(p, MS_TOKEN_SEMICOLON, "expected ';' after for init statement, got %s",
        msTokenTypeName(p->current.type));
    node->as.forStmt.cond = parsePrecedence(p, MS_PREC_EXPR);
    if (node->as.forStmt.cond == NULL) {
      return NULL;
    }
    parserConsume(p, MS_TOKEN_SEMICOLON, "expected ';' after for condition, got %s",
        msTokenTypeName(p->current.type));
    bool savedAllowIncDec = p->allowIncDec;
    p->allowIncDec = true;
    node->as.forStmt.post = parseSimpleStatement(p);
    p->allowIncDec = savedAllowIncDec;
    if (node->as.forStmt.post == NULL) {
      return NULL;
    }
  }
  ++p->loopDepth;
  node->as.forStmt.body = parseRequiredBlock(p);
  --p->loopDepth;
  if (node->as.forStmt.body == NULL) {
    return NULL;
  }
  return node;
}

// "while" expr block.
static struct MsAst* parseWhile(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'while'
  struct MsAst* cond = parsePrecedence(p, MS_PREC_EXPR);
  if (cond == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_WHILE, &keyword);
  if (node == NULL) {
    return NULL;
  }
  node->as.whileStmt.cond = cond;
  ++p->loopDepth;
  node->as.whileStmt.body = parseRequiredBlock(p);
  --p->loopDepth;
  if (node->as.whileStmt.body == NULL) {
    return NULL;
  }
  return node;
}

// "try" block { "except" [ typeExpr [ "as" ident ] ] block } [ "finally"
// block ]. At least one except or finally clause is required (E203); an
// 'except' directly followed by '{' is the bare form.
static struct MsAst* parseTry(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'try'
  struct MsAst* node = parserNewNode(p, MS_AST_TRY, &keyword);
  if (node == NULL) {
    return NULL;
  }
  node->as.tryStmt.body = parseRequiredBlock(p);
  if (node->as.tryStmt.body == NULL) {
    return NULL;
  }
  while (parserMatch(p, MS_TOKEN_KW_EXCEPT)) {
    struct MsToken exceptKw = p->previous;
    struct MsAst* clause = parserNewNode(p, MS_AST_EXCEPT_CLAUSE, &exceptKw);
    if (clause == NULL) {
      return NULL;
    }
    if (!parserCheck(p, MS_TOKEN_LEFT_BRACE)) {
      clause->as.exceptClause.typeExpr = parsePrecedence(p, MS_PREC_EXPR);
      if (clause->as.exceptClause.typeExpr == NULL) {
        return NULL;
      }
      if (parserMatch(p, MS_TOKEN_KW_AS)) {
        parserConsume(p, MS_TOKEN_IDENTIFIER, "expected identifier after 'as', got %s",
            msTokenTypeName(p->current.type));
        struct MsToken name = p->previous;   // the identifier, or 'as' on error
        clause->as.exceptClause.name = name.start;
        clause->as.exceptClause.nameLen = name.length;
      }
    }
    clause->as.exceptClause.body = parseRequiredBlock(p);
    if (clause->as.exceptClause.body == NULL) {
      return NULL;
    }
    if (!msAstListPush(p->arena, &node->as.tryStmt.excepts, clause)) {
      parserErrorAt(p, &exceptKw, 0, "out of memory");
      return NULL;
    }
  }
  if (parserMatch(p, MS_TOKEN_KW_FINALLY)) {
    node->as.tryStmt.finallyBlock = parseRequiredBlock(p);
    if (node->as.tryStmt.finallyBlock == NULL) {
      return NULL;
    }
  }
  if (node->as.tryStmt.excepts.count == 0 && node->as.tryStmt.finallyBlock == NULL) {
    parserErrorAt(p, &p->current, 203, "expected 'except' or 'finally' after try block, got %s",
        msTokenTypeName(p->current.type));
  }
  return node;
}

// "with" expr [ "as" ident ] block.
static struct MsAst* parseWith(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'with'
  struct MsAst* context = parsePrecedence(p, MS_PREC_EXPR);
  if (context == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_WITH, &keyword);
  if (node == NULL) {
    return NULL;
  }
  node->as.withStmt.context = context;
  if (parserMatch(p, MS_TOKEN_KW_AS)) {
    parserConsume(p, MS_TOKEN_IDENTIFIER, "expected identifier after 'as', got %s",
        msTokenTypeName(p->current.type));
    struct MsToken name = p->previous;   // the identifier, or 'as' on error
    node->as.withStmt.name = name.start;
    node->as.withStmt.nameLen = name.length;
  }
  node->as.withStmt.body = parseRequiredBlock(p);
  if (node->as.withStmt.body == NULL) {
    return NULL;
  }
  return node;
}

// The statements of a select case body run brace-less to the next
// 'case'/'default' or the closing '}' (06-concurrency section 3) and are
// wrapped in a synthetic BLOCK.
static struct MsAst* parseSelectCaseBody(struct MsParser* p, const struct MsToken* at) {
  struct MsAst* body = parserNewNode(p, MS_AST_BLOCK, at);
  if (body == NULL) {
    return NULL;
  }
  while (!parserCheck(p, MS_TOKEN_KW_CASE) && !parserCheck(p, MS_TOKEN_KW_DEFAULT)
      && !parserCheck(p, MS_TOKEN_RIGHT_BRACE) && !parserCheck(p, MS_TOKEN_EOF)) {
    struct MsToken start = p->current;
    struct MsAst* stmt = parseStatement(p);
    if (stmt == NULL) {
      // Empty statement (E204, no node) or OOM; see parseStatement.
      if (start.type != MS_TOKEN_SEMICOLON) {
        return NULL;
      }
      continue;
    }
    if (!msAstListPush(p->arena, &body->as.block, stmt)) {
      parserErrorAt(p, at, 0, "out of memory");
      return NULL;
    }
  }
  return body;
}

// "case" ( recvStmt | sendStmt ) ":" caseBody | "default" ":" caseBody. A
// recv target prefix (identList (":="|"=")) is recognized by lookahead: an
// identifier followed by ',', ':=' or '='. The channel expression must have
// the shape ch.recv() (0 args) or ch.send(v) (1 arg, no target prefix); the
// CALL/ATTR wrappers are discarded (arena-owned) and only the channel target
// subtree is kept. Anything else is E202.
static struct MsAst* parseSelectCase(struct MsParser* p) {
  struct MsToken keyword = p->current;
  struct MsAst* node = parserNewNode(p, MS_AST_SELECT_CASE, &keyword);
  if (node == NULL) {
    return NULL;
  }
  if (parserMatch(p, MS_TOKEN_KW_DEFAULT)) {
    node->as.selectCase.caseKind = MS_SELECT_DEFAULT;
    parserConsume(p, MS_TOKEN_COLON, "expected ':' after 'default', got %s",
        msTokenTypeName(p->current.type));
    node->as.selectCase.body = parseSelectCaseBody(p, &keyword);
    if (node->as.selectCase.body == NULL) {
      return NULL;
    }
    return node;
  }
  parserAdvance(p);   // 'case'
  if (parserCheck(p, MS_TOKEN_IDENTIFIER)) {
    MsTokenType peek = msLexerPeek(p->lexer);
    if (peek == MS_TOKEN_COMMA || peek == MS_TOKEN_COLON_EQUAL || peek == MS_TOKEN_EQUAL) {
      for (;;) {
        struct MsToken name = p->current;
        parserAdvance(p);   // identifier
        struct MsAst* target = parserNewNode(p, MS_AST_IDENT, &name);
        if (target == NULL) {
          return NULL;
        }
        target->as.literal.start = name.start;
        target->as.literal.length = name.length;
        if (!msAstListPush(p->arena, &node->as.selectCase.targets, target)) {
          parserErrorAt(p, &name, 0, "out of memory");
          return NULL;
        }
        if (!parserMatch(p, MS_TOKEN_COMMA)) {
          break;
        }
        if (!parserCheck(p, MS_TOKEN_IDENTIFIER)) {
          parserErrorAt(p, &p->current, 203, "expected identifier in select targets, got %s",
              msTokenTypeName(p->current.type));
          break;
        }
      }
      if (parserCheck(p, MS_TOKEN_COLON_EQUAL) || parserCheck(p, MS_TOKEN_EQUAL)) {
        node->as.selectCase.isDecl = parserCheck(p, MS_TOKEN_COLON_EQUAL);
        parserAdvance(p);   // ':=' / '='
      } else {
        parserErrorAt(p, &p->current, 203, "expected ':=' or '=' in select case, got %s",
            msTokenTypeName(p->current.type));
      }
    }
  }
  struct MsAst* channelExpr = parsePrecedence(p, MS_PREC_EXPR);
  if (channelExpr == NULL) {
    return NULL;
  }
  struct MsAst* channel = NULL;
  struct MsAst* value = NULL;
  MsSelectCaseKind caseKind = MS_SELECT_RECV;
  bool shapeOk = false;
  if (channelExpr->kind == MS_AST_CALL && channelExpr->as.call.callee->kind == MS_AST_ATTR) {
    struct MsAst* attr = channelExpr->as.call.callee;
    if (attr->as.attr.length == 4 && memcmp(attr->as.attr.start, "recv", 4) == 0
        && channelExpr->as.call.args.count == 0) {
      caseKind = MS_SELECT_RECV;
      channel = attr->as.attr.target;
      shapeOk = true;
    } else if (attr->as.attr.length == 4 && memcmp(attr->as.attr.start, "send", 4) == 0
        && channelExpr->as.call.args.count == 1
        && node->as.selectCase.targets.count == 0) {
      caseKind = MS_SELECT_SEND;
      channel = attr->as.attr.target;
      value = channelExpr->as.call.args.items[0];
      shapeOk = true;
    }
  }
  if (!shapeOk) {
    parserErrorAtNode(p, channelExpr, 202, "select case expects ch.recv() or ch.send(v)");
  }
  node->as.selectCase.caseKind = caseKind;
  node->as.selectCase.channel = channel;
  node->as.selectCase.value = value;
  parserConsume(p, MS_TOKEN_COLON, "expected ':' after select case, got %s",
      msTokenTypeName(p->current.type));
  node->as.selectCase.body = parseSelectCaseBody(p, &keyword);
  if (node->as.selectCase.body == NULL) {
    return NULL;
  }
  return node;
}

// "select" "{" { selectCase } "}".
static struct MsAst* parseSelect(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'select'
  struct MsAst* node = parserNewNode(p, MS_AST_SELECT, &keyword);
  if (node == NULL) {
    return NULL;
  }
  parserConsume(p, MS_TOKEN_LEFT_BRACE, "expected '{' after 'select', got %s",
      msTokenTypeName(p->current.type));
  while (!parserCheck(p, MS_TOKEN_RIGHT_BRACE) && !parserCheck(p, MS_TOKEN_EOF)) {
    if (!parserCheck(p, MS_TOKEN_KW_CASE) && !parserCheck(p, MS_TOKEN_KW_DEFAULT)) {
      parserErrorAt(p, &p->current, 202, "expected 'case' or 'default' in select, got %s",
          msTokenTypeName(p->current.type));
      parserAdvance(p);   // guarantee progress
      continue;
    }
    struct MsAst* selectCase = parseSelectCase(p);
    if (selectCase == NULL) {
      return NULL;
    }
    if (!msAstListPush(p->arena, &node->as.block, selectCase)) {
      parserErrorAt(p, &keyword, 0, "out of memory");
      return NULL;
    }
  }
  if (!parserMatch(p, MS_TOKEN_RIGHT_BRACE)) {
    parserErrorAt(p, &p->current, 211, "expected '}'");
  }
  return node;
}

// One name in a from-import list, appended as an IDENT child. Returns false
// on OOM.
static bool parseFromImportName(struct MsParser* p, struct MsAst* node) {
  parserConsume(p, MS_TOKEN_IDENTIFIER, "expected identifier in import list, got %s",
      msTokenTypeName(p->current.type));
  struct MsToken name = p->previous;   // the identifier, or the token before the error
  struct MsAst* ident = parserNewNode(p, MS_AST_IDENT, &name);
  if (ident == NULL) {
    return false;
  }
  ident->as.literal.start = name.start;
  ident->as.literal.length = name.length;
  if (!msAstListPush(p->arena, &node->as.fromImport.names, ident)) {
    parserErrorAt(p, &name, 0, "out of memory");
    return false;
  }
  return true;
}

// importStmt: "import" stringLit [ "as" ident ] | "from" stringLit "import"
// ( identList | "(" identList [ "," ] ")" ). The path must be a plain string
// token (E203 otherwise); the lexer keeps the quotes in the lexeme.
static struct MsAst* parseImport(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'import' / 'from'
  if (keyword.type == MS_TOKEN_KW_IMPORT) {
    struct MsAst* node = parserNewNode(p, MS_AST_IMPORT, &keyword);
    if (node == NULL) {
      return NULL;
    }
    if (!parserCheck(p, MS_TOKEN_STRING)) {
      parserErrorAt(p, &p->current, 203, "import path must be a string, got %s",
          msTokenTypeName(p->current.type));
    } else {
      node->as.importStmt.path = p->current.start;
      node->as.importStmt.pathLen = p->current.length;
      parserAdvance(p);   // path
    }
    if (parserMatch(p, MS_TOKEN_KW_AS)) {
      parserConsume(p, MS_TOKEN_IDENTIFIER, "expected identifier after 'as', got %s",
          msTokenTypeName(p->current.type));
      node->as.importStmt.alias = p->previous.start;
      node->as.importStmt.aliasLen = p->previous.length;
    }
    return node;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_FROM_IMPORT, &keyword);
  if (node == NULL) {
    return NULL;
  }
  if (!parserCheck(p, MS_TOKEN_STRING)) {
    parserErrorAt(p, &p->current, 203, "import path must be a string, got %s",
        msTokenTypeName(p->current.type));
  } else {
    node->as.fromImport.path = p->current.start;
    node->as.fromImport.pathLen = p->current.length;
    parserAdvance(p);   // path
  }
  parserConsume(p, MS_TOKEN_KW_IMPORT, "expected 'import' after 'from' path, got %s",
      msTokenTypeName(p->current.type));
  if (parserMatch(p, MS_TOKEN_LEFT_PAREN)) {
    while (!parserCheck(p, MS_TOKEN_RIGHT_PAREN) && !parserCheck(p, MS_TOKEN_EOF)) {
      if (!parseFromImportName(p, node)) {
        return NULL;
      }
      if (!parserMatch(p, MS_TOKEN_COMMA)) {
        break;
      }
    }
    parserConsume(p, MS_TOKEN_RIGHT_PAREN, "expected ')' after import list, got %s",
        msTokenTypeName(p->current.type));
  } else {
    if (!parseFromImportName(p, node)) {
      return NULL;
    }
    while (parserMatch(p, MS_TOKEN_COMMA)) {
      if (!parseFromImportName(p, node)) {
        return NULL;
      }
    }
  }
  return node;
}

// Parameter list of a func declaration: ident ["=" default] | "*" ident |
// "**" ident, comma-separated with an optional trailing comma. Enforces the
// fixed order normal (defaults only after defaults) -> *varargs -> **kwargs
// (E207) and rejects duplicate names (E208). Returns false on OOM.
static bool parseParams(struct MsParser* p, struct MsAst* func) {
  bool seenDefault = false;
  bool seenVarargs = false;
  bool seenKwargs = false;
  while (!parserCheck(p, MS_TOKEN_RIGHT_PAREN) && !parserCheck(p, MS_TOKEN_EOF)) {
    struct MsToken at = p->current;
    MsParamKind paramKind = MS_PARAM_NORMAL;
    if (parserMatch(p, MS_TOKEN_STAR)) {
      paramKind = MS_PARAM_VARARGS;
    } else if (parserMatch(p, MS_TOKEN_DOUBLE_STAR)) {
      paramKind = MS_PARAM_KWARGS;
    }
    // 'self' is a keyword (KW_SELF), not an identifier, but the spec's method
    // signature uses it as an ordinary parameter name.
    if (parserCheck(p, MS_TOKEN_IDENTIFIER) || parserCheck(p, MS_TOKEN_KW_SELF)) {
      parserAdvance(p);
    } else {
      parserErrorAt(p, &p->current, 203, "expected parameter name, got %s",
          msTokenTypeName(p->current.type));
    }
    struct MsToken name = p->previous;   // the name, or the token before the error
    struct MsAst* param = parserNewNode(p, MS_AST_PARAM, &at);
    if (param == NULL) {
      return false;
    }
    param->as.param.name = name.start;
    param->as.param.nameLen = name.length;
    param->as.param.paramKind = paramKind;
    for (size_t i = 0; i < func->as.func.params.count; ++i) {
      const struct MsAst* other = func->as.func.params.items[i];
      if (other->as.param.nameLen == name.length
          && memcmp(other->as.param.name, name.start, name.length) == 0) {
        parserErrorAt(p, &name, 208, "duplicate parameter name");
        break;
      }
    }
    if (paramKind == MS_PARAM_NORMAL && parserMatch(p, MS_TOKEN_EQUAL)) {
      param->as.param.defaultValue = parsePrecedence(p, MS_PREC_EXPR);
      if (param->as.param.defaultValue == NULL) {
        return false;
      }
    }
    bool orderOk = true;
    if (seenKwargs) {
      orderOk = false;   // nothing may follow **kwargs
    } else if (paramKind == MS_PARAM_NORMAL) {
      orderOk = !seenVarargs && (!seenDefault || param->as.param.defaultValue != NULL);
    } else if (paramKind == MS_PARAM_VARARGS) {
      orderOk = !seenVarargs;
    }
    if (!orderOk) {
      parserErrorAt(p, &name, 207, "parameters out of order (want normal, *varargs, **kwargs)");
    }
    seenDefault = seenDefault || param->as.param.defaultValue != NULL;
    seenVarargs = seenVarargs || paramKind == MS_PARAM_VARARGS;
    seenKwargs = seenKwargs || paramKind == MS_PARAM_KWARGS;
    if (!msAstListPush(p->arena, &func->as.func.params, param)) {
      parserErrorAt(p, &at, 0, "out of memory");
      return false;
    }
    if (!parserMatch(p, MS_TOKEN_COMMA)) {
      break;
    }
  }
  return true;
}

// "func" identifier "(" [ params ] ")" block ('async'/'static' are consumed
// by the caller and passed in as flags). Entering the body is a closure
// barrier: loopDepth resets (a nested function cannot break an outer loop)
// and funcDepth/funcIsAsync/inMethod are saved and restored. Static methods
// get inMethod == false (no self), instance methods true.
static struct MsAst* parseFuncDecl(struct MsParser* p, bool isAsync, bool isStatic, bool inMethod) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'func'
  parserConsume(p, MS_TOKEN_IDENTIFIER, "expected function name after 'func', got %s",
      msTokenTypeName(p->current.type));
  struct MsToken name = p->previous;   // the name, or 'func' on error
  struct MsAst* node = parserNewNode(p, MS_AST_FUNC_DECL, &keyword);
  if (node == NULL) {
    return NULL;
  }
  node->as.func.isAsync = isAsync;
  node->as.func.isStatic = isStatic;
  node->as.func.name = name.start;
  node->as.func.nameLen = name.length;
  parserConsume(p, MS_TOKEN_LEFT_PAREN, "expected '(' after function name, got %s",
      msTokenTypeName(p->current.type));
  if (!parseParams(p, node)) {
    return NULL;
  }
  parserConsume(p, MS_TOKEN_RIGHT_PAREN, "expected ')' after parameters, got %s",
      msTokenTypeName(p->current.type));
  int savedLoopDepth = p->loopDepth;
  int savedFuncDepth = p->funcDepth;
  bool savedFuncIsAsync = p->funcIsAsync;
  bool savedInMethod = p->inMethod;
  p->loopDepth = 0;
  p->funcDepth = savedFuncDepth + 1;
  p->funcIsAsync = isAsync;
  p->inMethod = inMethod;
  node->as.func.body = parseRequiredBlock(p);
  p->loopDepth = savedLoopDepth;
  p->funcDepth = savedFuncDepth;
  p->funcIsAsync = savedFuncIsAsync;
  p->inMethod = savedInMethod;
  if (node->as.func.body == NULL) {
    return NULL;
  }
  return node;
}

// "class" identifier [ "<" expr ] "{" { method } "}". Members are method
// declarations only: [static] [async] "func". 'static' is a contextual
// keyword, recognized only as an identifier lexeme directly followed by
// 'func' (so 'static async func' is rejected, per spec); anything else in
// the body is E202.
static struct MsAst* parseClassDecl(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'class'
  parserConsume(p, MS_TOKEN_IDENTIFIER, "expected class name after 'class', got %s",
      msTokenTypeName(p->current.type));
  struct MsToken name = p->previous;   // the name, or 'class' on error
  struct MsAst* node = parserNewNode(p, MS_AST_CLASS_DECL, &keyword);
  if (node == NULL) {
    return NULL;
  }
  node->as.classDecl.name = name.start;
  node->as.classDecl.nameLen = name.length;
  if (parserMatch(p, MS_TOKEN_LESS)) {
    node->as.classDecl.superclass = parsePrecedence(p, MS_PREC_EXPR);
    if (node->as.classDecl.superclass == NULL) {
      return NULL;
    }
  }
  parserConsume(p, MS_TOKEN_LEFT_BRACE, "expected '{' before class body, got %s",
      msTokenTypeName(p->current.type));
  struct MsAst* body = parserNewNode(p, MS_AST_BLOCK, &p->current);
  if (body == NULL) {
    return NULL;
  }
  node->as.classDecl.body = body;
  ++p->classDepth;
  while (!parserCheck(p, MS_TOKEN_RIGHT_BRACE) && !parserCheck(p, MS_TOKEN_EOF)) {
    bool isStatic = parserCheck(p, MS_TOKEN_IDENTIFIER)
        && p->current.length == 6
        && memcmp(p->current.start, "static", 6) == 0
        && msLexerPeek(p->lexer) == MS_TOKEN_KW_FUNC;
    if (isStatic) {
      parserAdvance(p);   // contextual 'static'
    }
    bool isAsync = parserCheck(p, MS_TOKEN_KW_ASYNC);
    if (isAsync) {
      if (msLexerPeek(p->lexer) != MS_TOKEN_KW_FUNC) {
        parserErrorAt(p, &p->current, 202, "expected 'func' after 'async', got %s",
            msTokenTypeName(msLexerPeek(p->lexer)));
      }
      parserAdvance(p);   // 'async'
    }
    if (!parserCheck(p, MS_TOKEN_KW_FUNC)) {
      parserErrorAt(p, &p->current, 202, "expected method declaration in class body, got %s",
          msTokenTypeName(p->current.type));
      parserAdvance(p);   // guarantee progress
      continue;
    }
    struct MsAst* method = parseFuncDecl(p, isAsync, isStatic, !isStatic);
    if (method == NULL) {
      --p->classDepth;
      return NULL;
    }
    if (!msAstListPush(p->arena, &body->as.block, method)) {
      parserErrorAt(p, &keyword, 0, "out of memory");
      --p->classDepth;
      return NULL;
    }
    consumeOptionalStatementEnd(p);
  }
  --p->classDepth;
  if (!parserMatch(p, MS_TOKEN_RIGHT_BRACE)) {
    parserErrorAt(p, &p->current, 211, "expected '}'");
  }
  return node;
}

// "return" [ exprList ]: legal only inside a function (E210 at the top
// level). The value list parses only when an expression can start here, so
// a bare 'return' before ';', '}' or EOF stays valueless.
static struct MsAst* parseReturn(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'return'
  if (p->funcDepth == 0) {
    parserErrorAt(p, &keyword, 210, "'return' outside function");
  }
  struct MsAst* node = parserNewNode(p, MS_AST_RETURN, &keyword);
  if (node == NULL) {
    return NULL;
  }
  while (msParseRules[p->current.type].prefix != NULL) {
    struct MsAst* value = parsePrecedence(p, MS_PREC_EXPR);
    if (value == NULL) {
      return NULL;
    }
    if (!msAstListPush(p->arena, &node->as.returnStmt.values, value)) {
      parserErrorAt(p, &keyword, 0, "out of memory");
      return NULL;
    }
    if (!parserMatch(p, MS_TOKEN_COMMA)) {
      break;
    }
  }
  return node;
}

// "raise" expr [ "from" expr ].
static struct MsAst* parseRaise(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'raise'
  struct MsAst* value = parsePrecedence(p, MS_PREC_EXPR);
  if (value == NULL) {
    return NULL;
  }
  struct MsAst* node = parserNewNode(p, MS_AST_RAISE, &keyword);
  if (node == NULL) {
    return NULL;
  }
  node->as.raiseStmt.value = value;
  if (parserMatch(p, MS_TOKEN_KW_FROM)) {
    node->as.raiseStmt.fromValue = parsePrecedence(p, MS_PREC_EXPR);
    if (node->as.raiseStmt.fromValue == NULL) {
      return NULL;
    }
  }
  return node;
}

// "del" targetList: every target must be assignable (E205).
static struct MsAst* parseDel(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'del'
  struct MsAst* node = parserNewNode(p, MS_AST_DEL, &keyword);
  if (node == NULL) {
    return NULL;
  }
  do {
    struct MsAst* target = parsePrecedence(p, MS_PREC_EXPR);
    if (target == NULL) {
      return NULL;
    }
    if (!astIsAssignableTarget(target)) {
      parserErrorAtNode(p, target, 205, "invalid delete target");
    }
    if (!msAstListPush(p->arena, &node->as.delStmt.targets, target)) {
      parserErrorAt(p, &keyword, 0, "out of memory");
      return NULL;
    }
  } while (parserMatch(p, MS_TOKEN_COMMA));
  return node;
}

// "global" identList: names become plain IDENT children.
static struct MsAst* parseGlobal(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);   // 'global'
  struct MsAst* node = parserNewNode(p, MS_AST_GLOBAL, &keyword);
  if (node == NULL) {
    return NULL;
  }
  do {
    parserConsume(p, MS_TOKEN_IDENTIFIER, "expected identifier after 'global', got %s",
        msTokenTypeName(p->current.type));
    struct MsToken name = p->previous;   // the identifier, or the token before the error
    struct MsAst* ident = parserNewNode(p, MS_AST_IDENT, &name);
    if (ident == NULL) {
      return NULL;
    }
    ident->as.literal.start = name.start;
    ident->as.literal.length = name.length;
    if (!msAstListPush(p->arena, &node->as.globalStmt.names, ident)) {
      parserErrorAt(p, &name, 0, "out of memory");
      return NULL;
    }
  } while (parserMatch(p, MS_TOKEN_COMMA));
  return node;
}

// "break" / "continue": legal only inside a loop body (E209 otherwise);
// loopDepth tracks the innermost loop and resets at function boundaries.
static struct MsAst* parseBreakContinue(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);
  if (p->loopDepth == 0) {
    parserErrorAt(p, &keyword, 209, "'%.*s' outside loop", (int)keyword.length, keyword.start);
  }
  return parserNewNode(p, keyword.type == MS_TOKEN_KW_BREAK ? MS_AST_BREAK : MS_AST_CONTINUE,
      &keyword);
}

// "pass": the explicit empty statement.
static struct MsAst* parsePass(struct MsParser* p) {
  struct MsToken keyword = p->current;
  parserAdvance(p);
  return parserNewNode(p, MS_AST_PASS, &keyword);
}

// Statement dispatch shared by the top level and blocks. A leading '{' is a
// block; a statement-start semicolon is an empty statement (E204, skipped, no
// node produced); keyword-led statements parse through their own functions
// (brace-terminated ones take an optional trailing semicolon, simple ones a
// mandatory one); anything else is a simple statement. func/class are
// statements, so nested declarations are legal. When a statement records a
// diagnostic (panicMode transitions during the dispatch), synchronize()
// replaces the normal statement-end consumption: it skips to the next
// statement boundary so parsing can recover per statement. Returns NULL for
// the empty statement and on OOM; callers tell the two apart by the token
// the statement started with (SEMICOLON == empty), since every other NULL
// path comes from allocation failure.
static struct MsAst* parseStatement(struct MsParser* p) {
  bool wasPanic = p->panicMode;
  if (parserCheck(p, MS_TOKEN_SEMICOLON)) {
    parserErrorAt(p, &p->current, 204, "empty statement; use 'pass' instead");
    parserAdvance(p);
    if (!wasPanic && p->panicMode) {
      synchronize(p);
    }
    return NULL;
  }
  struct MsAst* stmt = NULL;
  bool braceTerminated = true;
  if (parserCheck(p, MS_TOKEN_LEFT_BRACE)) {
    stmt = parseBlock(p);
  } else {
    switch (p->current.type) {
      case MS_TOKEN_KW_IF:
        stmt = parseIf(p);
        break;
      case MS_TOKEN_KW_FOR:
        stmt = parseFor(p);
        break;
      case MS_TOKEN_KW_WHILE:
        stmt = parseWhile(p);
        break;
      case MS_TOKEN_KW_TRY:
        stmt = parseTry(p);
        break;
      case MS_TOKEN_KW_WITH:
        stmt = parseWith(p);
        break;
      case MS_TOKEN_KW_SELECT:
        stmt = parseSelect(p);
        break;
      case MS_TOKEN_KW_CLASS:
        stmt = parseClassDecl(p);
        break;
      case MS_TOKEN_KW_FUNC:
        stmt = parseFuncDecl(p, false, false, false);
        break;
      case MS_TOKEN_KW_ASYNC:
        if (msLexerPeek(p->lexer) == MS_TOKEN_KW_FUNC) {
          parserAdvance(p);   // 'async'
          stmt = parseFuncDecl(p, true, false, false);
          break;
        }
        parserErrorAt(p, &p->current, 202, "expected 'func' after 'async', got %s",
            msTokenTypeName(msLexerPeek(p->lexer)));
        parserAdvance(p);   // 'async'; parse the tail as a simple statement
        stmt = parseSimpleStatement(p);
        braceTerminated = false;
        break;
      case MS_TOKEN_KW_IMPORT:
      case MS_TOKEN_KW_FROM:
        stmt = parseImport(p);
        braceTerminated = false;
        break;
      case MS_TOKEN_KW_RETURN:
        stmt = parseReturn(p);
        braceTerminated = false;
        break;
      case MS_TOKEN_KW_RAISE:
        stmt = parseRaise(p);
        braceTerminated = false;
        break;
      case MS_TOKEN_KW_DEL:
        stmt = parseDel(p);
        braceTerminated = false;
        break;
      case MS_TOKEN_KW_GLOBAL:
        stmt = parseGlobal(p);
        braceTerminated = false;
        break;
      case MS_TOKEN_KW_BREAK:
      case MS_TOKEN_KW_CONTINUE:
        stmt = parseBreakContinue(p);
        braceTerminated = false;
        break;
      case MS_TOKEN_KW_PASS:
        stmt = parsePass(p);
        braceTerminated = false;
        break;
      default:
        stmt = parseSimpleStatement(p);
        braceTerminated = false;
        break;
    }
  }
  if (stmt == NULL) {
    return NULL;
  }
  if (!wasPanic && p->panicMode) {
    synchronize(p);
  } else if (braceTerminated) {
    consumeOptionalStatementEnd(p);
  } else {
    consumeStatementEnd(p);
  }
  return stmt;
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
  // The loop also stops when the diagnostic cap fills: parserAdvance
  // collapses the lexer's cap state into a permanent EOF, but parser-side
  // errors alone never trip that path, so guard on the list directly
  // (belt and braces against pathological no-progress loops).
  while (!parserCheck(p, MS_TOKEN_EOF) && !msDiagListIsFull(p->diags)) {
    struct MsToken start = p->current;
    struct MsAst* stmt = parseStatement(p);
    if (stmt == NULL) {
      // Empty statement (E204, no node) or OOM; see parseStatement.
      if (start.type != MS_TOKEN_SEMICOLON) {
        return MS_ERROR_OOM;
      }
      continue;
    }
    if (!msAstListPush(p->arena, &root->as.block, stmt)) {
      parserErrorAt(p, &start, 0, "out of memory");
      return MS_ERROR_OOM;
    }
  }
  return msDiagListCount(p->diags) > diagsAtEntry ? MS_ERROR_SYNTAX : MS_OK;
}
