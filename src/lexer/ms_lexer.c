#include "lexer/ms_lexer.h"

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

// Current byte, or '\0' at end of input.
static char msLexerCurrent(const struct MsLexer* lexer) {
  if (msLexerIsAtEnd(lexer)) {
    return '\0';
  }
  return lexer->source[lexer->pos];
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

// Fills out with an EOF token at the lexer's current position.
static void msLexerMakeEof(const struct MsLexer* lexer, struct MsToken* out) {
  out->type = MS_TOKEN_EOF;
  out->start = lexer->source + lexer->pos;
  out->length = 0;
  out->line = lexer->line;
  out->column = lexer->column;
}

// Scans the next token, skipping spaces/tabs and newlines (semicolon
// insertion arrives in a later task; newlines are plain whitespace for now).
static MsResult msLexerScan(struct MsLexer* lexer, struct MsToken* out) {
  for (;;) {
    char c = msLexerCurrent(lexer);
    if (c == ' ' || c == '\t') {
      msLexerAdvanceChar(lexer);
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (msLexerConsumeNewline(lexer) != MS_OK) {
        msLexerMakeEof(lexer, out);
        return MS_ERROR_SYNTAX;
      }
      continue;
    }
    if (msLexerIsAtEnd(lexer)) {
      msLexerMakeEof(lexer, out);
      return MS_OK;
    }
    if (msLexerIsIdentStart(c)) {
      out->type = MS_TOKEN_IDENTIFIER;
      out->start = lexer->source + lexer->pos;
      out->line = lexer->line;
      out->column = lexer->column;
      do {
        msLexerAdvanceChar(lexer);
      } while (msLexerIsIdentContinue(msLexerCurrent(lexer)));
      out->length = (size_t)(lexer->source + lexer->pos - out->start);
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
