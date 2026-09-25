#include "lexer/ms_lexer.h"

#include <stdio.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

static const char* const msLexerTokenTypeNames[] = {
    "MS_TOKEN_EOF",
    "MS_TOKEN_INVALID",
    "MS_TOKEN_IDENTIFIER",
    "MS_TOKEN_INT",
    "MS_TOKEN_FLOAT",
    "MS_TOKEN_STRING",
    "MS_TOKEN_RAW_STRING",
    "MS_TOKEN_BYTES",
    "MS_TOKEN_FSTRING_START",
    "MS_TOKEN_FSTRING_END",
    "MS_TOKEN_FSTRING_FORMAT",
    "MS_TOKEN_SEMICOLON",
    "MS_TOKEN_KW_AND",
    "MS_TOKEN_KW_AS",
    "MS_TOKEN_KW_ASYNC",
    "MS_TOKEN_KW_AWAIT",
    "MS_TOKEN_KW_BREAK",
    "MS_TOKEN_KW_CASE",
    "MS_TOKEN_KW_CLASS",
    "MS_TOKEN_KW_CONTINUE",
    "MS_TOKEN_KW_DEFAULT",
    "MS_TOKEN_KW_DEL",
    "MS_TOKEN_KW_DIV",
    "MS_TOKEN_KW_ELSE",
    "MS_TOKEN_KW_EXCEPT",
    "MS_TOKEN_KW_FALSE",
    "MS_TOKEN_KW_FINALLY",
    "MS_TOKEN_KW_FOR",
    "MS_TOKEN_KW_FROM",
    "MS_TOKEN_KW_FUNC",
    "MS_TOKEN_KW_GLOBAL",
    "MS_TOKEN_KW_IF",
    "MS_TOKEN_KW_IMPORT",
    "MS_TOKEN_KW_IN",
    "MS_TOKEN_KW_IS",
    "MS_TOKEN_KW_LAMBDA",
    "MS_TOKEN_KW_NIL",
    "MS_TOKEN_KW_NOT",
    "MS_TOKEN_KW_OR",
    "MS_TOKEN_KW_PASS",
    "MS_TOKEN_KW_RAISE",
    "MS_TOKEN_KW_RETURN",
    "MS_TOKEN_KW_SELECT",
    "MS_TOKEN_KW_SELF",
    "MS_TOKEN_KW_SUPER",
    "MS_TOKEN_KW_TRUE",
    "MS_TOKEN_KW_TRY",
    "MS_TOKEN_KW_WHILE",
    "MS_TOKEN_KW_WITH",
    "MS_TOKEN_PLUS",
    "MS_TOKEN_MINUS",
    "MS_TOKEN_STAR",
    "MS_TOKEN_SLASH",
    "MS_TOKEN_PERCENT",
    "MS_TOKEN_DOUBLE_STAR",
    "MS_TOKEN_EQUAL_EQUAL",
    "MS_TOKEN_BANG_EQUAL",
    "MS_TOKEN_LESS",
    "MS_TOKEN_LESS_EQUAL",
    "MS_TOKEN_GREATER",
    "MS_TOKEN_GREATER_EQUAL",
    "MS_TOKEN_AMP",
    "MS_TOKEN_PIPE",
    "MS_TOKEN_CARET",
    "MS_TOKEN_TILDE",
    "MS_TOKEN_SHIFT_LEFT",
    "MS_TOKEN_SHIFT_RIGHT",
    "MS_TOKEN_COLON_EQUAL",
    "MS_TOKEN_EQUAL",
    "MS_TOKEN_PLUS_EQUAL",
    "MS_TOKEN_MINUS_EQUAL",
    "MS_TOKEN_STAR_EQUAL",
    "MS_TOKEN_SLASH_EQUAL",
    "MS_TOKEN_PERCENT_EQUAL",
    "MS_TOKEN_DOUBLE_STAR_EQUAL",
    "MS_TOKEN_AMP_EQUAL",
    "MS_TOKEN_PIPE_EQUAL",
    "MS_TOKEN_CARET_EQUAL",
    "MS_TOKEN_SHIFT_LEFT_EQUAL",
    "MS_TOKEN_SHIFT_RIGHT_EQUAL",
    "MS_TOKEN_PLUS_PLUS",
    "MS_TOKEN_MINUS_MINUS",
    "MS_TOKEN_LEFT_PAREN",
    "MS_TOKEN_RIGHT_PAREN",
    "MS_TOKEN_LEFT_BRACKET",
    "MS_TOKEN_RIGHT_BRACKET",
    "MS_TOKEN_LEFT_BRACE",
    "MS_TOKEN_RIGHT_BRACE",
    "MS_TOKEN_COMMA",
    "MS_TOKEN_COLON",
    "MS_TOKEN_DOT",
    "MS_TOKEN_ELLIPSIS",
};

_Static_assert(MS_ARRAY_LEN(msLexerTokenTypeNames) == MS_TOKEN_ELLIPSIS + 1,
    "msLexerTokenTypeNames must cover every MsTokenType value");

// Reports code/message at an absolute position. Returns MS_OK while more
// diagnostics fit, MS_ERROR_SYNTAX once the cap is full (caller must then
// emit EOF).
static MsResult msLexerError(struct MsLexer* lexer, uint32_t line, uint32_t column,
    uint32_t code, const char* message) {
  if (!msDiagReport(lexer->diags, line, column, code, "%s", message)) {
    return MS_ERROR_SYNTAX;
  }
  return MS_OK;
}

static bool msLexerIsAtEnd(const struct MsLexer* lexer) {
  return lexer->pos >= lexer->sourceLen;
}

// Current byte, or '\0' at end of input. Embedded NUL bytes are not EOF;
// callers must check msLexerIsAtEnd separately.
static char msLexerCurrent(const struct MsLexer* lexer) {
  if (msLexerIsAtEnd(lexer)) {
    return '\0';
  }
  return lexer->source[lexer->pos];
}

// Byte one position past the cursor, or '\0' when that is past the end.
static char msLexerLookahead(const struct MsLexer* lexer) {
  if (lexer->pos + 1 >= lexer->sourceLen) {
    return '\0';
  }
  return lexer->source[lexer->pos + 1];
}

// Consumes one non-newline byte and returns it. Newlines must go through
// msLexerConsumeNewline so line/column stay correct.
static char msLexerAdvanceChar(struct MsLexer* lexer) {
  MS_ASSERT(!msLexerIsAtEnd(lexer));
  char c = lexer->source[lexer->pos];
  ++lexer->pos;
  ++lexer->column;
  return c;
}

// Consumes the current byte and returns true when it equals expected;
// otherwise leaves the cursor alone and returns false.
static bool msLexerMatch(struct MsLexer* lexer, char expected) {
  if (msLexerIsAtEnd(lexer) || msLexerCurrent(lexer) != expected) {
    return false;
  }
  msLexerAdvanceChar(lexer);
  return true;
}

// Consumes one newline: "\n", "\r\n" (folded into one), or a bare "\r",
// which is reported as E112 and treated as a newline for recovery.
static MsResult msLexerConsumeNewline(struct MsLexer* lexer) {
  MS_ASSERT(!msLexerIsAtEnd(lexer));
  MsResult result = MS_OK;
  if (msLexerCurrent(lexer) == '\r') {
    if (lexer->pos + 1 < lexer->sourceLen && lexer->source[lexer->pos + 1] == '\n') {
      lexer->pos += 2;
    } else {
      result = msLexerError(lexer, lexer->line, lexer->column, 112, "bare carriage return");
      ++lexer->pos;
    }
  } else {
    ++lexer->pos;
  }
  ++lexer->line;
  lexer->column = 1;
  return result;
}

static bool msLexerIsIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool msLexerIsIdentContinue(char c) {
  return msLexerIsIdentStart(c) || (c >= '0' && c <= '9');
}

// The 37 keywords of 01-lexical section 4. Sorted by name length, then
// lexicographically within each length -- the binary search in
// msLexerKeywordType compares on that same key.
static const struct {
  const char* name;
  MsTokenType type;
} msLexerKeywords[] = {
    {"as", MS_TOKEN_KW_AS},       {"if", MS_TOKEN_KW_IF},
    {"in", MS_TOKEN_KW_IN},       {"is", MS_TOKEN_KW_IS},
    {"or", MS_TOKEN_KW_OR},       {"and", MS_TOKEN_KW_AND},
    {"del", MS_TOKEN_KW_DEL},     {"div", MS_TOKEN_KW_DIV},
    {"for", MS_TOKEN_KW_FOR},     {"nil", MS_TOKEN_KW_NIL},
    {"not", MS_TOKEN_KW_NOT},     {"try", MS_TOKEN_KW_TRY},
    {"case", MS_TOKEN_KW_CASE},
    {"else", MS_TOKEN_KW_ELSE},   {"from", MS_TOKEN_KW_FROM},
    {"func", MS_TOKEN_KW_FUNC},   {"pass", MS_TOKEN_KW_PASS},
    {"self", MS_TOKEN_KW_SELF},   {"true", MS_TOKEN_KW_TRUE},
    {"with", MS_TOKEN_KW_WITH},   {"async", MS_TOKEN_KW_ASYNC},
    {"await", MS_TOKEN_KW_AWAIT}, {"break", MS_TOKEN_KW_BREAK},
    {"class", MS_TOKEN_KW_CLASS}, {"false", MS_TOKEN_KW_FALSE},
    {"raise", MS_TOKEN_KW_RAISE}, {"super", MS_TOKEN_KW_SUPER},
    {"while", MS_TOKEN_KW_WHILE}, {"except", MS_TOKEN_KW_EXCEPT},
    {"global", MS_TOKEN_KW_GLOBAL}, {"import", MS_TOKEN_KW_IMPORT},
    {"lambda", MS_TOKEN_KW_LAMBDA}, {"return", MS_TOKEN_KW_RETURN},
    {"select", MS_TOKEN_KW_SELECT}, {"default", MS_TOKEN_KW_DEFAULT},
    {"finally", MS_TOKEN_KW_FINALLY}, {"continue", MS_TOKEN_KW_CONTINUE},
};

_Static_assert(MS_ARRAY_LEN(msLexerKeywords) == 37,
    "msLexerKeywords must cover exactly the 37 keywords of 01-lexical section 4");

// Maps an identifier lexeme to its keyword token type, or
// MS_TOKEN_IDENTIFIER when it is not a keyword (builtins like "chan"/"len"
// deliberately miss the table; 01-lexical section 4).
static MsTokenType msLexerKeywordType(const char* start, size_t length) {
  size_t lo = 0;
  size_t hi = MS_ARRAY_LEN(msLexerKeywords);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t nameLen = strlen(msLexerKeywords[mid].name);
    int cmp = 0;
    if (length != nameLen) {
      cmp = length < nameLen ? -1 : 1;
    } else {
      cmp = memcmp(start, msLexerKeywords[mid].name, length);
    }
    if (cmp < 0) {
      hi = mid;
    } else if (cmp > 0) {
      lo = mid + 1;
    } else {
      return msLexerKeywords[mid].type;
    }
  }
  return MS_TOKEN_IDENTIFIER;
}

// Returns the byte length of the shape-valid UTF-8 sequence at the cursor,
// or 0 on a shape error: lead byte outside 0xC2-0xF4, truncation at end of
// buffer, or continuation byte outside 0x80-0xBF. Shape only -- overlong
// forms, surrogates, and code points above U+10FFFF pass by design (v0.1).
static size_t msLexerUtf8Length(const struct MsLexer* lexer) {
  MS_ASSERT(!msLexerIsAtEnd(lexer));
  unsigned char lead = (unsigned char)lexer->source[lexer->pos];
  size_t length;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
  } else {
    return 0;
  }
  if (lexer->pos + length > lexer->sourceLen) {
    return 0;
  }
  for (size_t i = 1; i < length; ++i) {
    unsigned char c = (unsigned char)lexer->source[lexer->pos + i];
    if (c < 0x80 || c > 0xBF) {
      return 0;
    }
  }
  return length;
}

// Consumes a "//" comment through (not including) its terminating newline;
// the newline itself is handled by the caller's newline path.
static void msLexerSkipLineComment(struct MsLexer* lexer) {
  MS_ASSERT(msLexerCurrent(lexer) == '/' && msLexerLookahead(lexer) == '/');
  while (!msLexerIsAtEnd(lexer) && msLexerCurrent(lexer) != '\r'
      && msLexerCurrent(lexer) != '\n') {
    msLexerAdvanceChar(lexer);
  }
}

// The first newline consumed while skipping trivia: its position is where an
// auto-inserted semicolon goes (01-lexical section 7).
struct MsLexerNewline {
  bool seen;
  size_t pos;
  uint32_t line;
  uint32_t column;
};

// Records the cursor position as the first newline of a trivia run; later
// newlines in the same run keep the first one's position.
static void msLexerNoteNewline(const struct MsLexer* lexer, struct MsLexerNewline* newline) {
  if (!newline->seen) {
    newline->seen = true;
    newline->pos = lexer->pos;
    newline->line = lexer->line;
    newline->column = lexer->column;
  }
}

// Consumes a block comment through the first "*/" (comments do not nest).
// Newlines inside go through msLexerConsumeNewline so line/column stay
// correct, and are noted for semicolon insertion like any other newline.
// Reaching the end of input first reports E105 at the comment's start
// position.
static MsResult msLexerSkipBlockComment(struct MsLexer* lexer, struct MsLexerNewline* newline) {
  MS_ASSERT(msLexerCurrent(lexer) == '/' && msLexerLookahead(lexer) == '*');
  uint32_t startLine = lexer->line;
  uint32_t startColumn = lexer->column;
  msLexerAdvanceChar(lexer);
  msLexerAdvanceChar(lexer);
  for (;;) {
    if (msLexerIsAtEnd(lexer)) {
      return msLexerError(lexer, startLine, startColumn, 105, "unclosed block comment");
    }
    char c = msLexerCurrent(lexer);
    if (c == '\r' || c == '\n') {
      msLexerNoteNewline(lexer, newline);
      if (msLexerConsumeNewline(lexer) != MS_OK) {
        return MS_ERROR_SYNTAX;
      }
      continue;
    }
    if (c == '*' && msLexerLookahead(lexer) == '/') {
      msLexerAdvanceChar(lexer);
      msLexerAdvanceChar(lexer);
      return MS_OK;
    }
    msLexerAdvanceChar(lexer);
  }
}

// Skips spaces, tabs, newlines and comments; stops on the first byte that
// can start a token (or at end of input) without consuming it. Every newline
// consumed (including inside block comments) is noted in *newline so the
// caller can apply semicolon insertion.
static MsResult msLexerSkipTrivia(struct MsLexer* lexer, struct MsLexerNewline* newline) {
  for (;;) {
    char c = msLexerCurrent(lexer);
    if (c == ' ' || c == '\t') {
      msLexerAdvanceChar(lexer);
      continue;
    }
    if (c == '\r' || c == '\n') {
      msLexerNoteNewline(lexer, newline);
      if (msLexerConsumeNewline(lexer) != MS_OK) {
        return MS_ERROR_SYNTAX;
      }
      continue;
    }
    if (c == '/') {
      if (msLexerLookahead(lexer) == '/') {
        msLexerSkipLineComment(lexer);
        continue;
      }
      if (msLexerLookahead(lexer) == '*') {
        if (msLexerSkipBlockComment(lexer, newline) != MS_OK) {
          return MS_ERROR_SYNTAX;
        }
        continue;
      }
      // A lone '/' starts an operator (SLASH / SLASH_EQUAL); leave it for
      // the operator dispatcher.
    }
    return MS_OK;
  }
}

// Fills out with an EOF token at the lexer's current position. EOF never
// ends a statement, so it clears the semicolon-insertion state.
static void msLexerMakeEof(struct MsLexer* lexer, struct MsToken* out) {
  out->type = MS_TOKEN_EOF;
  out->start = lexer->source + lexer->pos;
  out->length = 0;
  out->line = lexer->line;
  out->column = lexer->column;
  lexer->canEndStatement = false;
}

// Fills out with a token of the given type spanning [start, lexer->pos);
// line/column are the ones captured before the token's first byte.
static void msLexerMakeToken(const struct MsLexer* lexer, struct MsToken* out,
    MsTokenType type, const char* start, uint32_t line, uint32_t column) {
  out->type = type;
  out->start = start;
  out->length = (size_t)(lexer->source + lexer->pos - start);
  out->line = line;
  out->column = column;
}

// The statement-ender set of 01-lexical section 7: after one of these tokens
// a newline (or end of input) inserts a semicolon. Everything else -- all
// operators, the word operators div/and/or/not/is/in, ",", "(", "[", "{",
// ".", ":", and INVALID -- is a continuer and suppresses insertion.
static bool msLexerTokenEndsStatement(MsTokenType type) {
  switch (type) {
    case MS_TOKEN_IDENTIFIER:
    case MS_TOKEN_INT:
    case MS_TOKEN_FLOAT:
    case MS_TOKEN_STRING:
    case MS_TOKEN_RAW_STRING:
    case MS_TOKEN_BYTES:
    case MS_TOKEN_FSTRING_END:
    case MS_TOKEN_KW_TRUE:
    case MS_TOKEN_KW_FALSE:
    case MS_TOKEN_KW_NIL:
    case MS_TOKEN_KW_RETURN:
    case MS_TOKEN_KW_BREAK:
    case MS_TOKEN_KW_CONTINUE:
    case MS_TOKEN_KW_PASS:
    case MS_TOKEN_KW_RAISE:
    case MS_TOKEN_RIGHT_PAREN:
    case MS_TOKEN_RIGHT_BRACKET:
    case MS_TOKEN_RIGHT_BRACE:
    case MS_TOKEN_PLUS_PLUS:
    case MS_TOKEN_MINUS_MINUS:
      return true;
    default:
      return false;
  }
}

// Commits a freshly scanned token: updates the semicolon-insertion state
// from its final type. Every token-producing path must route through this
// (INVALID included -- it clears the state), and only the auto-inserted
// semicolon and EOF bypass it (they maintain the state themselves).
// (FSTRING_START/FORMAT/LEFT_BRACE are continuers already; FSTRING_END is
// the one f-string token in the ender set.)
static void msLexerCommitToken(struct MsLexer* lexer, const struct MsToken* token) {
  lexer->canEndStatement = msLexerTokenEndsStatement(token->type);
  // A STRING token committed in FSTRING_TEXT mode is an f-string text
  // segment mid-expression, not a string literal: it must not trigger
  // semicolon insertion even though plain STRING is a statement ender.
  // Strings scanned inside an interpolation (NORMAL mode, frames live) are
  // ordinary literals and follow the standard rules.
  if (lexer->mode == MS_LEXMODE_FSTRING_TEXT && token->type == MS_TOKEN_STRING) {
    lexer->canEndStatement = false;
  }
}

// Fills out with an auto-inserted semicolon: an empty lexeme at the position
// of the newline (or end of input) that triggered it, and clears the
// insertion state so blank lines insert nothing further.
static void msLexerMakeSemicolon(struct MsLexer* lexer, struct MsToken* out,
    size_t pos, uint32_t line, uint32_t column) {
  out->type = MS_TOKEN_SEMICOLON;
  out->start = lexer->source + pos;
  out->length = 0;
  out->line = line;
  out->column = column;
  lexer->canEndStatement = false;
}

static bool msLexerIsDigit(char c) {
  return c >= '0' && c <= '9';
}

static bool msLexerIsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Value of a hex digit; the caller must have checked msLexerIsHexDigit.
static uint32_t msLexerHexValue(char c) {
  MS_ASSERT(msLexerIsHexDigit(c));
  if (c >= '0' && c <= '9') {
    return (uint32_t)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (uint32_t)(c - 'a') + 10;
  }
  return (uint32_t)(c - 'A') + 10;
}

// True when c is a valid digit in base (2, 8, 10 or 16).
static bool msLexerIsBaseDigit(char c, int base) {
  if (msLexerIsDigit(c)) {
    return c - '0' < base;
  }
  if (base == 16) {
    return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  }
  return false;
}

// Consumes a run of base digits, applying the underscore rule (single
// underscores between digits only). stopAtExponent makes 'e'/'E' end the
// run cleanly (decimal integer and fraction parts only). Returns true on a
// clean run with *digitCount set; on the first violation returns false with
// *errLine/*errColumn at the offending byte (a digit or letter invalid in
// this base, a misplaced underscore, or a trailing underscore), leaving the
// cursor on it -- or just past it for a trailing underscore.
static bool msLexerScanDigits(struct MsLexer* lexer, int base, bool stopAtExponent,
    size_t* digitCount, uint32_t* errLine, uint32_t* errColumn) {
  *digitCount = 0;
  bool lastUnderscore = false;
  for (;;) {
    char c = msLexerCurrent(lexer);
    if (msLexerIsBaseDigit(c, base)) {
      msLexerAdvanceChar(lexer);
      ++*digitCount;
      lastUnderscore = false;
      continue;
    }
    if (c == '_') {
      if (*digitCount == 0 || lastUnderscore) {
        *errLine = lexer->line;
        *errColumn = lexer->column;
        return false;
      }
      msLexerAdvanceChar(lexer);
      lastUnderscore = true;
      continue;
    }
    if (stopAtExponent && (c == 'e' || c == 'E')) {
      break;
    }
    if (msLexerIsIdentContinue(c)) {
      // [0-9A-Za-z] byte that is not a valid digit in this base
      // (0b102, 0o8, 0xG).
      *errLine = lexer->line;
      *errColumn = lexer->column;
      return false;
    }
    break;
  }
  if (lastUnderscore) {
    // Trailing underscore: it is the byte right before the cursor.
    *errLine = lexer->line;
    *errColumn = lexer->column - 1;
    return false;
  }
  return true;
}

// Scans an integer or float literal (01-lexical section 5). The cursor is on
// a digit, or on '.' with a digit right after it (".5" form). Only the
// lexical shape is validated: the token keeps the raw slice, values are
// never converted. Errors report E107 (bad underscore placement, digit
// outside the base, missing exponent digits) or E108 (dot with no digit
// after it) and yield an INVALID token covering the erroneous fragment.
static MsResult msLexerScanNumber(struct MsLexer* lexer, struct MsToken* out) {
  out->start = lexer->source + lexer->pos;
  out->line = lexer->line;
  out->column = lexer->column;
  out->type = MS_TOKEN_INT;

  bool error = false;
  uint32_t errCode = 107;
  uint32_t errLine = 0;
  uint32_t errColumn = 0;
  const char* errMessage = "invalid number literal";
  bool decimal = true;  // only decimal literals can switch to float

  if (msLexerCurrent(lexer) == '.') {
    // ".5" form; the caller guaranteed a digit after the dot.
    out->type = MS_TOKEN_FLOAT;
    msLexerAdvanceChar(lexer);
    size_t digits;
    error = !msLexerScanDigits(lexer, 10, true, &digits, &errLine, &errColumn);
  } else {
    int base = 10;
    if (msLexerCurrent(lexer) == '0') {
      char next = msLexerLookahead(lexer);
      if (next == 'x' || next == 'X') {
        base = 16;
      } else if (next == 'o' || next == 'O') {
        base = 8;
      } else if (next == 'b' || next == 'B') {
        base = 2;
      }
    }
    if (base != 10) {
      decimal = false;
      msLexerAdvanceChar(lexer);  // '0'
      msLexerAdvanceChar(lexer);  // prefix letter
      if (msLexerCurrent(lexer) == '_') {
        // Exactly one underscore may follow a base prefix (0x_1F, Go-style).
        msLexerAdvanceChar(lexer);
      }
      size_t digits;
      error = !msLexerScanDigits(lexer, base, false, &digits, &errLine, &errColumn);
      if (!error && digits == 0) {
        // "0x" or "0x_" with no digit: point just past what was consumed.
        error = true;
        errLine = lexer->line;
        errColumn = lexer->column;
        errMessage = "expected digit after base prefix";
      }
    } else {
      size_t digits;
      error = !msLexerScanDigits(lexer, 10, true, &digits, &errLine, &errColumn);
      if (!error && msLexerCurrent(lexer) == '.') {
        if (msLexerIsDigit(msLexerLookahead(lexer))) {
          out->type = MS_TOKEN_FLOAT;
          msLexerAdvanceChar(lexer);
          size_t fracDigits;
          error = !msLexerScanDigits(lexer, 10, true, &fracDigits, &errLine, &errColumn);
        } else {
          // "5." with no digit after the dot is E108 by design (01-lexical
          // section 5.2); there is no fallback to member access. Consume
          // just the dot for the INVALID span; later scans lex the rest.
          error = true;
          errCode = 108;
          errLine = lexer->line;
          errColumn = lexer->column;
          errMessage = "expected digit after '.'";
          msLexerAdvanceChar(lexer);
        }
      }
    }
  }
  if (!error && decimal && (msLexerCurrent(lexer) == 'e' || msLexerCurrent(lexer) == 'E')) {
    out->type = MS_TOKEN_FLOAT;
    msLexerAdvanceChar(lexer);
    if (msLexerCurrent(lexer) == '+' || msLexerCurrent(lexer) == '-') {
      msLexerAdvanceChar(lexer);
    }
    size_t digits;
    error = !msLexerScanDigits(lexer, 10, false, &digits, &errLine, &errColumn);
    if (!error && digits == 0) {
      error = true;
      errLine = lexer->line;
      errColumn = lexer->column;
      errMessage = "expected digit in exponent";
    }
  }

  if (error) {
    if (errCode == 107) {
      // Cover the whole would-be literal: consume the remaining run of
      // [0-9A-Za-z_] so the INVALID token spans the erroneous fragment.
      while (!msLexerIsAtEnd(lexer) && msLexerIsIdentContinue(msLexerCurrent(lexer))) {
        msLexerAdvanceChar(lexer);
      }
    }
    out->type = MS_TOKEN_INVALID;
    out->length = (size_t)(lexer->source + lexer->pos - out->start);
    // Progress guarantee: an error token never has an empty lexeme.
    MS_ASSERT(out->length > 0);
    if (msLexerError(lexer, errLine, errColumn, errCode, errMessage) != MS_OK) {
      msLexerMakeEof(lexer, out);
      return MS_ERROR_SYNTAX;
    }
    msLexerCommitToken(lexer, out);
    return MS_OK;
  }
  out->length = (size_t)(lexer->source + lexer->pos - out->start);
  msLexerCommitToken(lexer, out);
  return MS_OK;
}

// Validates the escape sequence at the cursor (a backslash) and consumes it
// completely on success (01-lexical section 5.3: \n \t \r \\ \" \' \0,
// \xHH, \uHHHH, \UHHHHHHHH). Returns false without consuming anything on a
// bad escape letter, too few or non-hex digits, or a \u/\U code point above
// U+10FFFF or in the surrogate range U+D800-U+DFFF.
static bool msLexerScanEscape(struct MsLexer* lexer) {
  MS_ASSERT(!msLexerIsAtEnd(lexer) && msLexerCurrent(lexer) == '\\');
  if (lexer->pos + 1 >= lexer->sourceLen) {
    return false;  // lone backslash at end of input
  }
  char kind = lexer->source[lexer->pos + 1];
  switch (kind) {
    case 'n':
    case 't':
    case 'r':
    case '\\':
    case '"':
    case '\'':
    case '0':
      msLexerAdvanceChar(lexer);  // backslash
      msLexerAdvanceChar(lexer);  // escape letter
      return true;
    case 'x':
    case 'u':
    case 'U': {
      int digits = kind == 'x' ? 2 : (kind == 'u' ? 4 : 8);
      uint32_t value = 0;
      for (int i = 0; i < digits; ++i) {
        if (lexer->pos + 2 + (size_t)i >= lexer->sourceLen
            || !msLexerIsHexDigit(lexer->source[lexer->pos + 2 + (size_t)i])) {
          return false;
        }
        // At most 8 hex digits, so value always fits in uint32_t.
        value = value * 16 + msLexerHexValue(lexer->source[lexer->pos + 2 + (size_t)i]);
      }
      if (kind != 'x' && (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))) {
        return false;
      }
      for (int i = 0; i < 2 + digits; ++i) {
        msLexerAdvanceChar(lexer);
      }
      return true;
    }
    default:
      return false;
  }
}

// Scans a "..." string or, with isBytes, a b"..." bytes literal whose 'b'
// prefix sits at the cursor. The lexeme includes the prefix and both quotes.
// Escapes are validated byte-wise; decoding is deferred to msLexerUnescape.
// The first malformed escape reports E106 at its backslash; the rest of the
// string is then scanned without further escape checks (one diagnostic per
// string, no spam) and the token comes out INVALID. An unescaped newline
// (left unconsumed) or end of input before the closing quote reports E103
// at the string start instead, again producing an INVALID token.
static MsResult msLexerScanString(struct MsLexer* lexer, struct MsToken* out, bool isBytes) {
  out->start = lexer->source + lexer->pos;
  out->line = lexer->line;
  out->column = lexer->column;
  uint32_t startLine = lexer->line;
  uint32_t startColumn = lexer->column;
  if (isBytes) {
    msLexerAdvanceChar(lexer);  // 'b'
  }
  msLexerAdvanceChar(lexer);  // opening quote
  bool error = false;
  uint32_t errCode = 0;
  uint32_t errLine = 0;
  uint32_t errColumn = 0;
  const char* errMessage = NULL;
  bool checkEscapes = true;
  for (;;) {
    if (msLexerIsAtEnd(lexer)) {
      if (!error) {
        error = true;
        errCode = 103;
        errLine = startLine;
        errColumn = startColumn;
        errMessage = "unterminated string";
      }
      break;
    }
    char c = msLexerCurrent(lexer);
    if (c == '\r' || c == '\n') {
      // Strings do not span lines; the newline stays for the trivia path.
      if (!error) {
        error = true;
        errCode = 103;
        errLine = startLine;
        errColumn = startColumn;
        errMessage = "unterminated string";
      }
      break;
    }
    if (c == '"') {
      msLexerAdvanceChar(lexer);  // closing quote
      break;
    }
    if (c == '\\' && checkEscapes) {
      if (!msLexerScanEscape(lexer)) {
        if (!error) {
          error = true;
          errCode = 106;
          errLine = lexer->line;
          errColumn = lexer->column;  // still on the backslash
          errMessage = "invalid escape sequence";
        }
        checkEscapes = false;
        msLexerAdvanceChar(lexer);  // consume the backslash, keep scanning
      }
      continue;
    }
    msLexerAdvanceChar(lexer);
  }
  out->length = (size_t)(lexer->source + lexer->pos - out->start);
  if (!error) {
    out->type = isBytes ? MS_TOKEN_BYTES : MS_TOKEN_STRING;
    msLexerCommitToken(lexer, out);
    return MS_OK;
  }
  out->type = MS_TOKEN_INVALID;
  // Progress guarantee: the opening quote was consumed, so length >= 1.
  MS_ASSERT(out->length > 0);
  if (msLexerError(lexer, errLine, errColumn, errCode, errMessage) != MS_OK) {
    msLexerMakeEof(lexer, out);
    return MS_ERROR_SYNTAX;
  }
  msLexerCommitToken(lexer, out);
  return MS_OK;
}

// Scans a backquote raw string (01-lexical section 5.3): no escape
// processing, may span lines. Newlines go through msLexerConsumeNewline so
// line tracking and CRLF folding stay intact. End of input before the
// closing backquote reports E104 at the string start and yields an INVALID
// token. The lexeme includes both backquotes.
static MsResult msLexerScanRawString(struct MsLexer* lexer, struct MsToken* out) {
  out->start = lexer->source + lexer->pos;
  out->line = lexer->line;
  out->column = lexer->column;
  uint32_t startLine = lexer->line;
  uint32_t startColumn = lexer->column;
  msLexerAdvanceChar(lexer);  // opening backquote
  for (;;) {
    if (msLexerIsAtEnd(lexer)) {
      out->type = MS_TOKEN_INVALID;
      out->length = (size_t)(lexer->source + lexer->pos - out->start);
      // Progress guarantee: the opening backquote was consumed.
      MS_ASSERT(out->length > 0);
      if (msLexerError(lexer, startLine, startColumn, 104, "unterminated raw string") != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
    char c = msLexerCurrent(lexer);
    if (c == '`') {
      msLexerAdvanceChar(lexer);  // closing backquote
      out->type = MS_TOKEN_RAW_STRING;
      out->length = (size_t)(lexer->source + lexer->pos - out->start);
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
    if (c == '\r' || c == '\n') {
      if (msLexerConsumeNewline(lexer) != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      continue;
    }
    msLexerAdvanceChar(lexer);
  }
}

// Pops the current f-string frame (after FSTRING_END or an error recovery)
// and resumes NORMAL mode. Invariant: a frame is only ever pushed in NORMAL
// mode (f-string starts are recognized nowhere else), and NORMAL mode under
// a frame means mid-interpolation (braceDepth >= 1) -- so the enclosing
// context after a pop is always plain code or an outer interpolation
// expression, never an interrupted text segment.
static void msLexerPopFstringFrame(struct MsLexer* lexer) {
  MS_ASSERT(lexer->frameCount > 0);
  --lexer->frameCount;
  lexer->mode = MS_LEXMODE_NORMAL;
}

// Scans "f\"" (the caller verified the '"' after the 'f') into
// MS_TOKEN_FSTRING_START, pushes a frame and switches to FSTRING_TEXT.
// Pushing past MS_LEXER_MAX_FSTRING_DEPTH reports E109 at the 'f' and
// recovers by scanning to the closing quote as if it were a plain string,
// without pushing a frame.
static MsResult msLexerScanFstringStart(struct MsLexer* lexer, struct MsToken* out) {
  MS_ASSERT(msLexerCurrent(lexer) == 'f' && msLexerLookahead(lexer) == '"');
  const char* start = lexer->source + lexer->pos;
  uint32_t line = lexer->line;
  uint32_t column = lexer->column;
  msLexerAdvanceChar(lexer);  // 'f'
  msLexerAdvanceChar(lexer);  // opening quote
  if (lexer->frameCount >= MS_LEXER_MAX_FSTRING_DEPTH) {
    for (;;) {
      if (msLexerIsAtEnd(lexer) || msLexerCurrent(lexer) == '\r' || msLexerCurrent(lexer) == '\n') {
        break;
      }
      char c = msLexerCurrent(lexer);
      if (c == '\\' && lexer->pos + 1 < lexer->sourceLen && lexer->source[lexer->pos + 1] != '\r'
          && lexer->source[lexer->pos + 1] != '\n') {
        msLexerAdvanceChar(lexer);  // keep an escaped quote out of the scan
        msLexerAdvanceChar(lexer);
        continue;
      }
      msLexerAdvanceChar(lexer);
      if (c == '"') {
        break;
      }
    }
    msLexerMakeToken(lexer, out, MS_TOKEN_INVALID, start, line, column);
    // Progress guarantee: at least the 'f' and the opening quote were consumed.
    MS_ASSERT(out->length >= 2);
    if (msLexerError(lexer, line, column, 109, "f-string nesting too deep") != MS_OK) {
      msLexerMakeEof(lexer, out);
      return MS_ERROR_SYNTAX;
    }
    msLexerCommitToken(lexer, out);
    return MS_OK;
  }
  lexer->frames[lexer->frameCount] = (struct MsLexerFrame){.quote = '"', .braceDepth = 0, .bracketDepth = 0};
  ++lexer->frameCount;
  lexer->mode = MS_LEXMODE_FSTRING_TEXT;
  msLexerMakeToken(lexer, out, MS_TOKEN_FSTRING_START, start, line, column);
  msLexerCommitToken(lexer, out);
  return MS_OK;
}

// Collects the raw format spec of an interpolation (FSTRING_FORMAT mode):
// everything up to the '}' that closes the interpolation, allowing nested
// '{' '}' pairs inside (Python-style nested replacement fields). Emits
// MS_TOKEN_FSTRING_FORMAT whose lexeme excludes the closing '}' (the ':'
// was consumed by the caller) and returns to NORMAL mode, leaving the '}'
// to the ordinary brace logic, which emits RIGHT_BRACE and switches back to
// FSTRING_TEXT. A newline or end of input first reports E110 and recovers
// by popping the frame; the newline stays for the trivia path.
static MsResult msLexerScanFstringFormat(struct MsLexer* lexer, struct MsToken* out) {
  MS_ASSERT(lexer->frameCount > 0);
  const char* start = lexer->source + lexer->pos;
  uint32_t line = lexer->line;
  uint32_t column = lexer->column;
  int localDepth = 0;
  for (;;) {
    char c = msLexerCurrent(lexer);
    if (msLexerIsAtEnd(lexer) || c == '\r' || c == '\n') {
      msLexerMakeToken(lexer, out, MS_TOKEN_INVALID, start, line, column);
      msLexerPopFstringFrame(lexer);
      if (msLexerError(lexer, line, column, 110, "unpaired '{' in f-string interpolation") != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
    if (c == '{') {
      ++localDepth;
    } else if (c == '}') {
      if (localDepth == 0) {
        break;  // the interpolation's closing brace; do not consume it
      }
      --localDepth;
    }
    msLexerAdvanceChar(lexer);
  }
  msLexerMakeToken(lexer, out, MS_TOKEN_FSTRING_FORMAT, start, line, column);
  lexer->mode = MS_LEXMODE_NORMAL;
  msLexerCommitToken(lexer, out);
  return MS_OK;
}

// Scans one f-string text segment (FSTRING_TEXT mode). The raw text is
// validated with the plain-string escape rules (E106) and emitted as
// MS_TOKEN_STRING whose lexeme has no quotes. Boundaries: '{{' / '}}' are
// literal-brace escapes and stay in the segment; a single '{' opens an
// interpolation (LEFT_BRACE, braceDepth = 1, back to NORMAL); the frame's
// closing quote ends the f-string (FSTRING_END, pop the frame); a lone '}'
// is E111. A boundary met with pending text first emits the STRING and
// leaves the boundary byte to the next scan call. An unescaped newline or
// end of input reports E103 at the segment start (strings do not span
// lines) and recovers by popping the frame as if the quote had closed; the
// newline itself stays for the trivia path.
static MsResult msLexerScanFstringText(struct MsLexer* lexer, struct MsToken* out) {
  MS_ASSERT(lexer->frameCount > 0);
  struct MsLexerFrame* frame = &lexer->frames[lexer->frameCount - 1];
  const char* start = lexer->source + lexer->pos;
  uint32_t line = lexer->line;
  uint32_t column = lexer->column;
  bool escapeError = false;
  bool checkEscapes = true;
  for (;;) {
    char c = msLexerCurrent(lexer);
    if (msLexerIsAtEnd(lexer) || c == '\r' || c == '\n') {
      msLexerMakeToken(lexer, out, MS_TOKEN_INVALID, start, line, column);
      msLexerPopFstringFrame(lexer);
      if (msLexerError(lexer, line, column, 103, "unterminated string") != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
    if (c == frame->quote) {
      if (lexer->source + lexer->pos > start) {
        break;  // emit the pending text; the quote is handled next call
      }
      msLexerAdvanceChar(lexer);  // closing quote
      msLexerMakeToken(lexer, out, MS_TOKEN_FSTRING_END, start, line, column);
      msLexerPopFstringFrame(lexer);
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
    if (c == '{') {
      if (msLexerLookahead(lexer) == '{') {
        msLexerAdvanceChar(lexer);  // '{{' decodes to a literal '{' later
        msLexerAdvanceChar(lexer);
        continue;
      }
      if (lexer->source + lexer->pos > start) {
        break;  // emit the pending text; the '{' is handled next call
      }
      msLexerAdvanceChar(lexer);
      frame->braceDepth = 1;
      lexer->mode = MS_LEXMODE_NORMAL;
      msLexerMakeToken(lexer, out, MS_TOKEN_LEFT_BRACE, start, line, column);
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
    if (c == '}') {
      if (msLexerLookahead(lexer) == '}') {
        msLexerAdvanceChar(lexer);  // '}}' decodes to a literal '}' later
        msLexerAdvanceChar(lexer);
        continue;
      }
      if (lexer->source + lexer->pos > start) {
        break;  // emit the pending text; the '}' is handled next call
      }
      msLexerAdvanceChar(lexer);  // a lone '}' in text has no pairing '{'
      msLexerMakeToken(lexer, out, MS_TOKEN_INVALID, start, line, column);
      if (msLexerError(lexer, line, column, 111, "unmatched '}'") != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
    if (c == '\\' && checkEscapes) {
      if (!msLexerScanEscape(lexer)) {
        if (!escapeError) {
          escapeError = true;
          // The cursor is still on the backslash of the bad escape.
          if (msLexerError(lexer, lexer->line, lexer->column, 106,
                  "invalid escape sequence") != MS_OK) {
            msLexerMakeEof(lexer, out);
            return MS_ERROR_SYNTAX;
          }
        }
        checkEscapes = false;
        msLexerAdvanceChar(lexer);  // consume the backslash, keep scanning
      }
      continue;
    }
    msLexerAdvanceChar(lexer);
  }
  msLexerMakeToken(lexer, out, escapeError ? MS_TOKEN_INVALID : MS_TOKEN_STRING, start, line, column);
  msLexerCommitToken(lexer, out);
  return MS_OK;
}

// Reports E102 for an offending byte, naming it: '%c' for printable ASCII,
// '\xHH' for control or non-ASCII bytes.
static MsResult msLexerErrorUnexpectedChar(struct MsLexer* lexer, uint32_t line,
    uint32_t column, char c) {
  char message[32];
  unsigned char byte = (unsigned char)c;
  if (byte >= 0x20 && byte < 0x7F) {
    snprintf(message, sizeof(message), "unexpected character '%c'", c);
  } else {
    snprintf(message, sizeof(message), "unexpected character '\\x%02X'", (unsigned)byte);
  }
  return msLexerError(lexer, line, column, 102, message);
}

// Scans an operator or delimiter (01-lexical section 6) with maximal munch:
// three-byte forms ("<<=", ">>=", "**=", "...") before two-byte forms before
// single bytes. "//" and "/*" never reach here -- trivia consumed them as
// comments -- so '/' only ever yields SLASH_EQUAL or SLASH. A bare '!'
// reports E102 (logical not is the keyword "not"); any other unmatched byte
// ('?', '$', '@', '#', backslash, control bytes) reports E102 as well. Both
// come out as INVALID tokens covering the offending byte so scanning can
// continue for error recovery.
static MsResult msLexerScanOperator(struct MsLexer* lexer, struct MsToken* out) {
  const char* start = lexer->source + lexer->pos;
  uint32_t line = lexer->line;
  uint32_t column = lexer->column;
  char c = msLexerAdvanceChar(lexer);
  MsTokenType type;
  switch (c) {
    case '+':
      type = msLexerMatch(lexer, '+') ? MS_TOKEN_PLUS_PLUS
          : msLexerMatch(lexer, '=') ? MS_TOKEN_PLUS_EQUAL : MS_TOKEN_PLUS;
      break;
    case '-':
      type = msLexerMatch(lexer, '-') ? MS_TOKEN_MINUS_MINUS
          : msLexerMatch(lexer, '=') ? MS_TOKEN_MINUS_EQUAL : MS_TOKEN_MINUS;
      break;
    case '*':
      if (msLexerMatch(lexer, '*')) {
        type = msLexerMatch(lexer, '=') ? MS_TOKEN_DOUBLE_STAR_EQUAL : MS_TOKEN_DOUBLE_STAR;
      } else {
        type = msLexerMatch(lexer, '=') ? MS_TOKEN_STAR_EQUAL : MS_TOKEN_STAR;
      }
      break;
    case '/':
      type = msLexerMatch(lexer, '=') ? MS_TOKEN_SLASH_EQUAL : MS_TOKEN_SLASH;
      break;
    case '%':
      type = msLexerMatch(lexer, '=') ? MS_TOKEN_PERCENT_EQUAL : MS_TOKEN_PERCENT;
      break;
    case '=':
      type = msLexerMatch(lexer, '=') ? MS_TOKEN_EQUAL_EQUAL : MS_TOKEN_EQUAL;
      break;
    case '!':
      if (msLexerMatch(lexer, '=')) {
        type = MS_TOKEN_BANG_EQUAL;
        break;
      }
      if (msLexerErrorUnexpectedChar(lexer, line, column, c) != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      type = MS_TOKEN_INVALID;
      break;
    case '<':
      if (msLexerMatch(lexer, '<')) {
        type = msLexerMatch(lexer, '=') ? MS_TOKEN_SHIFT_LEFT_EQUAL : MS_TOKEN_SHIFT_LEFT;
      } else {
        type = msLexerMatch(lexer, '=') ? MS_TOKEN_LESS_EQUAL : MS_TOKEN_LESS;
      }
      break;
    case '>':
      if (msLexerMatch(lexer, '>')) {
        type = msLexerMatch(lexer, '=') ? MS_TOKEN_SHIFT_RIGHT_EQUAL : MS_TOKEN_SHIFT_RIGHT;
      } else {
        type = msLexerMatch(lexer, '=') ? MS_TOKEN_GREATER_EQUAL : MS_TOKEN_GREATER;
      }
      break;
    case '&':
      type = msLexerMatch(lexer, '=') ? MS_TOKEN_AMP_EQUAL : MS_TOKEN_AMP;
      break;
    case '|':
      type = msLexerMatch(lexer, '=') ? MS_TOKEN_PIPE_EQUAL : MS_TOKEN_PIPE;
      break;
    case '^':
      type = msLexerMatch(lexer, '=') ? MS_TOKEN_CARET_EQUAL : MS_TOKEN_CARET;
      break;
    case '~':
      type = MS_TOKEN_TILDE;
      break;
    case ':':
      if (msLexerMatch(lexer, '=')) {
        type = MS_TOKEN_COLON_EQUAL;
        break;
      }
      if (lexer->frameCount > 0 && lexer->frames[lexer->frameCount - 1].braceDepth == 1
          && lexer->frames[lexer->frameCount - 1].bracketDepth == 0) {
        // At the top level of an interpolation, ':' opens the format
        // segment: it is consumed without a COLON token and the format
        // collector produces the next token instead. A ':' nested in
        // brackets (a slice like a[1:2], a lambda body) is a plain COLON.
        lexer->mode = MS_LEXMODE_FSTRING_FORMAT;
        return msLexerScanFstringFormat(lexer, out);
      }
      type = MS_TOKEN_COLON;
      break;
    case '.':
      // The caller routes '.' + digit to msLexerScanNumber; here the
      // three-byte "..." wins over a lone DOT.
      if (msLexerCurrent(lexer) == '.' && msLexerLookahead(lexer) == '.') {
        msLexerAdvanceChar(lexer);
        msLexerAdvanceChar(lexer);
        type = MS_TOKEN_ELLIPSIS;
      } else {
        type = MS_TOKEN_DOT;
      }
      break;
    case '(':
      if (lexer->frameCount > 0) {
        ++lexer->frames[lexer->frameCount - 1].bracketDepth;
      }
      type = MS_TOKEN_LEFT_PAREN;
      break;
    case ')':
      if (lexer->frameCount > 0) {
        // Never below zero: an unbalanced closer is the parser's job, and
        // the token is emitted regardless.
        struct MsLexerFrame* parenFrame = &lexer->frames[lexer->frameCount - 1];
        if (parenFrame->bracketDepth > 0) {
          --parenFrame->bracketDepth;
        }
      }
      type = MS_TOKEN_RIGHT_PAREN;
      break;
    case '[':
      if (lexer->frameCount > 0) {
        ++lexer->frames[lexer->frameCount - 1].bracketDepth;
      }
      type = MS_TOKEN_LEFT_BRACKET;
      break;
    case ']':
      if (lexer->frameCount > 0) {
        struct MsLexerFrame* bracketFrame = &lexer->frames[lexer->frameCount - 1];
        if (bracketFrame->bracketDepth > 0) {
          --bracketFrame->bracketDepth;
        }
      }
      type = MS_TOKEN_RIGHT_BRACKET;
      break;
    case '{':
      if (lexer->frameCount > 0) {
        // A '{' inside an interpolation (dict literal etc.) nests the
        // frame's brace depth; its matching '}' will not close the
        // interpolation.
        ++lexer->frames[lexer->frameCount - 1].braceDepth;
      } else {
        ++lexer->blockBraceDepth;
      }
      type = MS_TOKEN_LEFT_BRACE;
      break;
    case '}':
      if (lexer->frameCount > 0) {
        struct MsLexerFrame* frame = &lexer->frames[lexer->frameCount - 1];
        MS_ASSERT(frame->braceDepth > 0);
        --frame->braceDepth;
        if (frame->braceDepth == 0) {
          // The interpolation closed; the f-string continues in text mode.
          lexer->mode = MS_LEXMODE_FSTRING_TEXT;
        }
        type = MS_TOKEN_RIGHT_BRACE;
        break;
      }
      if (lexer->blockBraceDepth > 0) {
        // Closes a block or dict/set literal opened in plain code.
        --lexer->blockBraceDepth;
        type = MS_TOKEN_RIGHT_BRACE;
        break;
      }
      // A bare '}' with no pairing '{' anywhere.
      if (msLexerError(lexer, line, column, 111, "unmatched '}'") != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      type = MS_TOKEN_INVALID;
      break;
    case ',':
      type = MS_TOKEN_COMMA;
      break;
    case ';':
      type = MS_TOKEN_SEMICOLON;
      break;
    default:
      if (msLexerErrorUnexpectedChar(lexer, line, column, c) != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      type = MS_TOKEN_INVALID;
      break;
  }
  // Progress guarantee: the first byte was consumed on entry, so every
  // operator or INVALID token has a non-empty lexeme.
  MS_ASSERT(lexer->source + lexer->pos > start);
  msLexerMakeToken(lexer, out, type, start, line, column);
  msLexerCommitToken(lexer, out);
  return MS_OK;
}

// Scans the next token. Trivia skipping comes first; when it crossed a
// newline while the previous token could end a statement, an auto-inserted
// semicolon (empty lexeme at the newline position) is returned instead of
// scanning further (01-lexical section 7). At end of input a pending
// statement end yields one compensating semicolon, and only the following
// call produces EOF.
static MsResult msLexerScan(struct MsLexer* lexer, struct MsToken* out) {
  // f-string modes dispatch before trivia skipping: text and format segment
  // bytes are raw content, so spaces and comments are not skipped there.
  if (lexer->mode == MS_LEXMODE_FSTRING_TEXT) {
    return msLexerScanFstringText(lexer, out);
  }
  if (lexer->mode == MS_LEXMODE_FSTRING_FORMAT) {
    return msLexerScanFstringFormat(lexer, out);
  }
  struct MsLexerNewline newline = {false, 0, 0, 0};
  if (msLexerSkipTrivia(lexer, &newline) != MS_OK) {
    msLexerMakeEof(lexer, out);
    return MS_ERROR_SYNTAX;
  }
  if (newline.seen && lexer->canEndStatement) {
    msLexerMakeSemicolon(lexer, out, newline.pos, newline.line, newline.column);
    return MS_OK;
  }
  if (msLexerIsAtEnd(lexer)) {
    if (lexer->frameCount > 0) {
      // End of input inside an interpolation expression: the '{' was never
      // paired. Recover by popping the frame so scanning can terminate.
      uint32_t line = lexer->line;
      uint32_t column = lexer->column;
      msLexerMakeToken(lexer, out, MS_TOKEN_INVALID, lexer->source + lexer->pos, line, column);
      msLexerPopFstringFrame(lexer);
      if (msLexerError(lexer, line, column, 110,
              "unpaired '{' in f-string interpolation") != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
    if (lexer->canEndStatement) {
      msLexerMakeSemicolon(lexer, out, lexer->pos, lexer->line, lexer->column);
      return MS_OK;
    }
    msLexerMakeEof(lexer, out);
    return MS_OK;
  }
  char c = msLexerCurrent(lexer);
  size_t utf8Length = 0;
  if ((unsigned char)c >= 0x80) {
    utf8Length = msLexerUtf8Length(lexer);
    if (utf8Length == 0) {
      // Malformed sequence: consume just the lead byte, flag it INVALID,
      // and keep scanning. v0.1 treats any well-formed sequence as an
      // identifier byte, so only malformed bytes ever land here.
      out->type = MS_TOKEN_INVALID;
      out->start = lexer->source + lexer->pos;
      out->length = 1;
      out->line = lexer->line;
      out->column = lexer->column;
      MsResult result = msLexerError(lexer, lexer->line, lexer->column, 102,
          "invalid UTF-8 sequence");
      msLexerAdvanceChar(lexer);
      if (result != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      msLexerCommitToken(lexer, out);
      return MS_OK;
    }
  }
  if (c == '"') {
    // Also inside an interpolation (frames live): plain strings may nest in
    // interpolation expressions, and the inner string's closing quote can
    // never close the outer f-string -- FSTRING_END is only produced in
    // FSTRING_TEXT mode.
    return msLexerScanString(lexer, out, false);
  }
  if (c == '`') {
    return msLexerScanRawString(lexer, out);
  }
  if (c == 'f' && msLexerLookahead(lexer) == '"') {
    // 'f' immediately followed by '"' starts an f-string; any other 'f'
    // falls through to the identifier branch below.
    return msLexerScanFstringStart(lexer, out);
  }
  if (c == 'b' && msLexerLookahead(lexer) == '"') {
    // 'b' immediately followed by '"' starts a bytes literal; any other
    // 'b' falls through to the identifier branch below.
    return msLexerScanString(lexer, out, true);
  }
  if (msLexerIsIdentStart(c) || utf8Length > 0) {
    out->start = lexer->source + lexer->pos;
    out->line = lexer->line;
    out->column = lexer->column;
    // Identifiers are ASCII letters/digits/'_' mixed with any well-formed
    // multi-byte UTF-8 sequence (v0.1 simplification: no Unicode letter
    // category check). A malformed sequence ends the identifier; the next
    // scan call reports it as E102 above.
    for (;;) {
      if (msLexerIsAtEnd(lexer)) {
        break;
      }
      char next = msLexerCurrent(lexer);
      if (msLexerIsIdentContinue(next)) {
        msLexerAdvanceChar(lexer);
        continue;
      }
      size_t seqLength = 0;
      if ((unsigned char)next >= 0x80) {
        seqLength = msLexerUtf8Length(lexer);
      }
      if (seqLength == 0) {
        break;
      }
      for (size_t i = 0; i < seqLength; ++i) {
        msLexerAdvanceChar(lexer);
      }
    }
    out->length = (size_t)(lexer->source + lexer->pos - out->start);
    out->type = msLexerKeywordType(out->start, out->length);
    msLexerCommitToken(lexer, out);
    return MS_OK;
  }
  if (msLexerIsDigit(c)) {
    return msLexerScanNumber(lexer, out);
  }
  if (c == '.' && msLexerIsDigit(msLexerLookahead(lexer))) {
    // Maximal munch on '.' + digit: ".5" is a float even right after an
    // identifier (locked judgment call; see Lexer.DotFiveVersusMemberAccess).
    // "..." cannot reach this branch: its second byte is not a digit.
    return msLexerScanNumber(lexer, out);
  }
  return msLexerScanOperator(lexer, out);
}

void msLexerInit(struct MsLexer* lexer, const char* source, size_t sourceLen,
    const char* chunkName, struct MsDiagList* diags) {
  *lexer = (struct MsLexer){
      .source = source,
      .sourceLen = sourceLen,
      .pos = 0,
      .line = 1,
      .column = 1,
      .chunkName = chunkName,
      .canEndStatement = false,
      .mode = MS_LEXMODE_NORMAL,
      .frameCount = 0,
      .hasPeeked = false,
      .diags = diags,
      .hitDiagCap = false,
      .blockBraceDepth = 0,
  };
  if (sourceLen >= 3 && (unsigned char)source[0] == 0xEF && (unsigned char)source[1] == 0xBB
      && (unsigned char)source[2] == 0xBF) {
    // Columns count bytes, so the BOM occupies columns 1-3.
    lexer->pos = 3;
    lexer->column = 4;
    if (msLexerError(lexer, 1, 1, 101, "UTF-8 byte order mark") != MS_OK) {
      lexer->hitDiagCap = true;
    }
  }
}

void msLexerDestroy(struct MsLexer* lexer) {
  MS_UNUSED(lexer);
}

MsResult msLexerNext(struct MsLexer* lexer, struct MsToken* out) {
  if (lexer->hasPeeked) {
    *out = lexer->peeked;
    lexer->hasPeeked = false;
    return lexer->peekedResult;
  }
  if (lexer->hitDiagCap) {
    msLexerMakeEof(lexer, out);
    return MS_ERROR_SYNTAX;
  }
  MsResult result = msLexerScan(lexer, out);
  if (result != MS_OK) {
    lexer->hitDiagCap = true;
  }
  return result;
}

MsTokenType msLexerPeek(struct MsLexer* lexer) {
  if (!lexer->hasPeeked) {
    if (lexer->hitDiagCap) {
      msLexerMakeEof(lexer, &lexer->peeked);
      lexer->peekedResult = MS_ERROR_SYNTAX;
    } else {
      lexer->peekedResult = msLexerScan(lexer, &lexer->peeked);
      if (lexer->peekedResult != MS_OK) {
        lexer->hitDiagCap = true;
      }
    }
    lexer->hasPeeked = true;
  }
  return lexer->peeked.type;
}

// Encodes value (<= U+10FFFF, not a surrogate) as UTF-8 into out, which must
// have room for 4 bytes; NULL out performs a sizing-only pass. Returns the
// encoded byte count.
static size_t msLexerEncodeUtf8(uint32_t value, char* out) {
  MS_ASSERT(value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF));
  if (value < 0x80) {
    if (out != NULL) {
      out[0] = (char)value;
    }
    return 1;
  }
  if (value < 0x800) {
    if (out != NULL) {
      out[0] = (char)(0xC0 | (value >> 6));
      out[1] = (char)(0x80 | (value & 0x3F));
    }
    return 2;
  }
  if (value < 0x10000) {
    if (out != NULL) {
      out[0] = (char)(0xE0 | (value >> 12));
      out[1] = (char)(0x80 | ((value >> 6) & 0x3F));
      out[2] = (char)(0x80 | (value & 0x3F));
    }
    return 3;
  }
  if (out != NULL) {
    out[0] = (char)(0xF0 | (value >> 18));
    out[1] = (char)(0x80 | ((value >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((value >> 6) & 0x3F));
    out[3] = (char)(0x80 | (value & 0x3F));
  }
  return 4;
}

// Decodes the escape sequence at raw[*pos] (a backslash) of an
// already-validated literal body, advancing *pos past it and writing the
// decoded bytes (UTF-8 for \u/\U) into out; NULL out performs a sizing-only
// pass. Returns the decoded byte count. The input contract makes anything
// malformed a caller bug (MS_ASSERT).
static size_t msLexerDecodeEscape(const char* raw, size_t rawLen, size_t* pos, char* out) {
  MS_ASSERT(*pos + 1 < rawLen && raw[*pos] == '\\');
  char kind = raw[*pos + 1];
  switch (kind) {
    case 'n':
    case 't':
    case 'r':
    case '\\':
    case '"':
    case '\'':
    case '0': {
      char value = kind;  // \\, \" and \' decode to themselves
      if (kind == 'n') {
        value = '\n';
      } else if (kind == 't') {
        value = '\t';
      } else if (kind == 'r') {
        value = '\r';
      } else if (kind == '0') {
        value = '\0';
      }
      if (out != NULL) {
        out[0] = value;
      }
      *pos += 2;
      return 1;
    }
    case 'x':
    case 'u':
    case 'U': {
      int digits = kind == 'x' ? 2 : (kind == 'u' ? 4 : 8);
      MS_ASSERT(*pos + 2 + (size_t)digits <= rawLen);
      uint32_t value = 0;
      for (int i = 0; i < digits; ++i) {
        char h = raw[*pos + 2 + (size_t)i];
        MS_ASSERT(msLexerIsHexDigit(h));
        value = value * 16 + msLexerHexValue(h);
      }
      *pos += 2 + (size_t)digits;
      if (kind == 'x') {
        // \xHH is one raw byte, even in a string (not UTF-8 encoded).
        if (out != NULL) {
          out[0] = (char)value;
        }
        return 1;
      }
      return msLexerEncodeUtf8(value, out);
    }
    default:
      MS_ASSERT(!"unknown escape in a pre-validated literal body");
      *pos += 2;
      return 0;
  }
}

// Shared two-pass core of msLexerUnescape and msLexerUnescapeFstringText.
// With braceEscapes, the literal-brace escapes '{{' / '}}' of f-string text
// segments decode to a single '{' / '}'; the input contract (pre-validated
// token bodies) is unchanged, so a lone brace is a caller bug (MS_ASSERT).
static MsResult msLexerUnescapeImpl(const char* raw, size_t rawLen, bool braceEscapes,
    char** out, size_t* outLen) {
  // Pass 1: compute the decoded length (\u/\U expand to multi-byte UTF-8).
  size_t decodedLen = 0;
  size_t pos = 0;
  while (pos < rawLen) {
    if (raw[pos] == '\\') {
      decodedLen += msLexerDecodeEscape(raw, rawLen, &pos, NULL);
      continue;
    }
    if (braceEscapes && (raw[pos] == '{' || raw[pos] == '}')) {
      MS_ASSERT(pos + 1 < rawLen && raw[pos + 1] == raw[pos]);
      pos += 2;
      ++decodedLen;
      continue;
    }
    ++pos;
    ++decodedLen;
  }
  // msAlloc(0) returns NULL, so even an empty body allocates one byte.
  char* buffer = (char*)msAlloc(decodedLen > 0 ? decodedLen : 1);
  if (buffer == NULL) {
    *out = NULL;
    *outLen = 0;
    return MS_ERROR_OOM;
  }
  // Pass 2: write the decoded bytes.
  size_t outPos = 0;
  pos = 0;
  while (pos < rawLen) {
    if (raw[pos] == '\\') {
      outPos += msLexerDecodeEscape(raw, rawLen, &pos, buffer + outPos);
      continue;
    }
    if (braceEscapes && (raw[pos] == '{' || raw[pos] == '}')) {
      MS_ASSERT(pos + 1 < rawLen && raw[pos + 1] == raw[pos]);
      buffer[outPos++] = raw[pos];
      pos += 2;
      continue;
    }
    buffer[outPos++] = raw[pos++];
  }
  MS_ASSERT(outPos == decodedLen);
  *out = buffer;
  *outLen = decodedLen;
  return MS_OK;
}

MsResult msLexerUnescape(const char* raw, size_t rawLen, char** out, size_t* outLen) {
  // Braces have no special meaning in a plain string or bytes body and pass
  // through as ordinary bytes.
  return msLexerUnescapeImpl(raw, rawLen, false, out, outLen);
}

MsResult msLexerUnescapeFstringText(const char* raw, size_t rawLen, char** out, size_t* outLen) {
  return msLexerUnescapeImpl(raw, rawLen, true, out, outLen);
}

const char* msTokenTypeName(MsTokenType type) {
  if (type < MS_TOKEN_EOF || type > MS_TOKEN_ELLIPSIS) {
    return "<unknown token>";
  }
  return msLexerTokenTypeNames[type];
}
