#include "lexer/ms_lexer.h"

#include <string.h>

#include "core/ms_common.h"

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
    "MS_TOKEN_DOUBLE_SLASH",
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
    "MS_TOKEN_DOUBLE_SLASH_EQUAL",
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

// The 36 keywords of 01-lexical section 4. Sorted by name length, then
// lexicographically within each length -- the binary search in
// msLexerKeywordType compares on that same key.
static const struct {
  const char* name;
  MsTokenType type;
} msLexerKeywords[] = {
    {"as", MS_TOKEN_KW_AS},       {"if", MS_TOKEN_KW_IF},
    {"in", MS_TOKEN_KW_IN},       {"is", MS_TOKEN_KW_IS},
    {"or", MS_TOKEN_KW_OR},       {"and", MS_TOKEN_KW_AND},
    {"del", MS_TOKEN_KW_DEL},     {"for", MS_TOKEN_KW_FOR},
    {"nil", MS_TOKEN_KW_NIL},     {"not", MS_TOKEN_KW_NOT},
    {"try", MS_TOKEN_KW_TRY},     {"case", MS_TOKEN_KW_CASE},
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

_Static_assert(MS_ARRAY_LEN(msLexerKeywords) == 36,
    "msLexerKeywords must cover exactly the 36 keywords of 01-lexical section 4");

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

// Consumes a block comment through the first "*/" (comments do not nest).
// Newlines inside go through msLexerConsumeNewline so line/column stay
// correct. Reaching the end of input first reports E105 at the comment's
// start position.
static MsResult msLexerSkipBlockComment(struct MsLexer* lexer) {
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
// can start a token (or at end of input) without consuming it.
static MsResult msLexerSkipTrivia(struct MsLexer* lexer) {
  for (;;) {
    char c = msLexerCurrent(lexer);
    if (c == ' ' || c == '\t') {
      msLexerAdvanceChar(lexer);
      continue;
    }
    if (c == '\r' || c == '\n') {
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
        if (msLexerSkipBlockComment(lexer) != MS_OK) {
          return MS_ERROR_SYNTAX;
        }
        continue;
      }
      // A lone '/' starts an operator (scanned in a later task); leave it.
    }
    return MS_OK;
  }
}

// Fills out with an EOF token at the lexer's current position.
static void msLexerMakeEof(const struct MsLexer* lexer, struct MsToken* out) {
  out->type = MS_TOKEN_EOF;
  out->start = lexer->source + lexer->pos;
  out->length = 0;
  out->line = lexer->line;
  out->column = lexer->column;
}

static bool msLexerIsDigit(char c) {
  return c >= '0' && c <= '9';
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
    if (msLexerError(lexer, errLine, errColumn, errCode, errMessage) != MS_OK) {
      msLexerMakeEof(lexer, out);
      return MS_ERROR_SYNTAX;
    }
    return MS_OK;
  }
  out->length = (size_t)(lexer->source + lexer->pos - out->start);
  return MS_OK;
}

// Scans the next token after skipping trivia (semicolon insertion arrives in
// a later task; newlines are plain whitespace for now).
static MsResult msLexerScan(struct MsLexer* lexer, struct MsToken* out) {
  for (;;) {
    if (msLexerSkipTrivia(lexer) != MS_OK) {
      msLexerMakeEof(lexer, out);
      return MS_ERROR_SYNTAX;
    }
    if (msLexerIsAtEnd(lexer)) {
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
        return MS_OK;
      }
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
      return MS_OK;
    }
    if (msLexerIsDigit(c)) {
      return msLexerScanNumber(lexer, out);
    }
    if (c == '.') {
      if (msLexerIsDigit(msLexerLookahead(lexer))) {
        // Maximal munch on '.' + digit: ".5" is a float even right after an
        // identifier (locked judgment call; see Lexer.DotFiveVersusMemberAccess).
        return msLexerScanNumber(lexer, out);
      }
      // A '.' not followed by a digit is a DOT token for now; the "..."
      // maximal munch arrives with the operator dispatch in task 7.
      out->type = MS_TOKEN_DOT;
      out->start = lexer->source + lexer->pos;
      out->length = 1;
      out->line = lexer->line;
      out->column = lexer->column;
      msLexerAdvanceChar(lexer);
      return MS_OK;
    }
    // Fallback until the remaining scanners land: skip the byte silently.
    msLexerAdvanceChar(lexer);
  }
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

MsResult msLexerUnescape(const char* raw, size_t rawLen, char** out, size_t* outLen) {
  MS_UNUSED(raw);
  MS_UNUSED(rawLen);
  *out = NULL;
  *outLen = 0;
  return MS_ERROR_SYNTAX;
}

const char* msTokenTypeName(MsTokenType type) {
  if (type < MS_TOKEN_EOF || type > MS_TOKEN_ELLIPSIS) {
    return "<unknown token>";
  }
  return msLexerTokenTypeNames[type];
}
