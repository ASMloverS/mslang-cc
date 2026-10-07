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
static struct MsAst* parsePrecedence(struct MsParser* p, MsPrecedence minPrec);

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
    [MS_TOKEN_LEFT_BRACKET]  = { NULL, parseIndexOrSlice, MS_PREC_POSTFIX },
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
// nested lambda needs parens).
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
  struct MsAst* body = parsePrecedence(p, MS_PREC_CONDITIONAL);
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

// Pratt fold loop: parse one prefix, then keep folding infix operators
// whose binding power is at least minPrec. 'not' folds as the infix
// operator of a comparison chain only when 'in' follows (msLexerPeek);
// otherwise a leading 'not' stays the unary prefix (so 'not a in b' is
// '(not a) in b', deliberately unlike Python).
static struct MsAst* parsePrecedence(struct MsParser* p, MsPrecedence minPrec) {
  parserAdvance(p);
  MsParseFn prefix = msParseRules[p->previous.type].prefix;
  if (prefix == NULL) {
    parserErrorAt(p, &p->previous, 201, "expected expression, got %s",
        msTokenTypeName(p->previous.type));
    return parserNewNode(p, MS_AST_INVALID, &p->previous);   // tree stays complete
  }
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
    struct MsAst* expr = parsePrecedence(p, MS_PREC_EXPR);
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
