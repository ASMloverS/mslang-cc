#ifndef MSLANG_SRC_LEXER_MS_LEXER_H_
#define MSLANG_SRC_LEXER_MS_LEXER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mslang/error.h>

#include "core/ms_diag.h"

typedef enum {
  MS_TOKEN_EOF,             // end of file
  MS_TOKEN_INVALID,         // lexical error placeholder (diagnostic recorded, for parser sync)

  MS_TOKEN_IDENTIFIER,
  MS_TOKEN_INT,             // integer (arbitrary precision; lexer validates form only, no value conversion)
  MS_TOKEN_FLOAT,           // float64
  MS_TOKEN_STRING,          // "..." plain string (escapes validated, not decoded); also an
                            // f-string text segment, whose lexeme is the raw text without
                            // quotes and which decodes via msLexerUnescapeFstringText
  MS_TOKEN_RAW_STRING,      // `...` backquote raw string
  MS_TOKEN_BYTES,           // b"..." bytes literal
  MS_TOKEN_FSTRING_START,   // f" prefix (see f-string tokenization)
  MS_TOKEN_FSTRING_END,     // closing quote of an f-string
  MS_TOKEN_FSTRING_FORMAT,  // raw format spec after ':' inside an interpolation

  MS_TOKEN_SEMICOLON,       // explicit ';' or auto-inserted semicolon

  // Keywords (37, one-to-one with 01-lexical section 4)
  MS_TOKEN_KW_AND, MS_TOKEN_KW_AS, MS_TOKEN_KW_ASYNC, MS_TOKEN_KW_AWAIT,
  MS_TOKEN_KW_BREAK, MS_TOKEN_KW_CASE, MS_TOKEN_KW_CLASS, MS_TOKEN_KW_CONTINUE,
  MS_TOKEN_KW_DEFAULT, MS_TOKEN_KW_DEL, MS_TOKEN_KW_DIV, MS_TOKEN_KW_ELSE,
  MS_TOKEN_KW_EXCEPT, MS_TOKEN_KW_FALSE, MS_TOKEN_KW_FINALLY, MS_TOKEN_KW_FOR,
  MS_TOKEN_KW_FROM, MS_TOKEN_KW_FUNC, MS_TOKEN_KW_GLOBAL, MS_TOKEN_KW_IF,
  MS_TOKEN_KW_IMPORT, MS_TOKEN_KW_IN, MS_TOKEN_KW_IS, MS_TOKEN_KW_LAMBDA,
  MS_TOKEN_KW_NIL, MS_TOKEN_KW_NOT, MS_TOKEN_KW_OR, MS_TOKEN_KW_PASS,
  MS_TOKEN_KW_RAISE, MS_TOKEN_KW_RETURN, MS_TOKEN_KW_SELECT, MS_TOKEN_KW_SELF,
  MS_TOKEN_KW_SUPER, MS_TOKEN_KW_TRUE, MS_TOKEN_KW_TRY, MS_TOKEN_KW_WHILE,
  MS_TOKEN_KW_WITH,

  // Operators and delimiters (01-lexical section 6; "//" is always a line
  // comment and has no token -- floor division is the keyword div)
  MS_TOKEN_PLUS, MS_TOKEN_MINUS, MS_TOKEN_STAR, MS_TOKEN_SLASH,
  MS_TOKEN_PERCENT, MS_TOKEN_DOUBLE_STAR,
  MS_TOKEN_EQUAL_EQUAL, MS_TOKEN_BANG_EQUAL,
  MS_TOKEN_LESS, MS_TOKEN_LESS_EQUAL, MS_TOKEN_GREATER, MS_TOKEN_GREATER_EQUAL,
  MS_TOKEN_AMP, MS_TOKEN_PIPE, MS_TOKEN_CARET, MS_TOKEN_TILDE,
  MS_TOKEN_SHIFT_LEFT, MS_TOKEN_SHIFT_RIGHT,
  MS_TOKEN_COLON_EQUAL,     // :=
  MS_TOKEN_EQUAL, MS_TOKEN_PLUS_EQUAL, MS_TOKEN_MINUS_EQUAL,
  MS_TOKEN_STAR_EQUAL, MS_TOKEN_SLASH_EQUAL, MS_TOKEN_PERCENT_EQUAL,
  MS_TOKEN_DOUBLE_STAR_EQUAL,
  MS_TOKEN_AMP_EQUAL, MS_TOKEN_PIPE_EQUAL, MS_TOKEN_CARET_EQUAL,
  MS_TOKEN_SHIFT_LEFT_EQUAL, MS_TOKEN_SHIFT_RIGHT_EQUAL,
  MS_TOKEN_PLUS_PLUS,       // ++ (statement form in for clauses only)
  MS_TOKEN_MINUS_MINUS,     // --
  MS_TOKEN_LEFT_PAREN, MS_TOKEN_RIGHT_PAREN,
  MS_TOKEN_LEFT_BRACKET, MS_TOKEN_RIGHT_BRACKET,
  MS_TOKEN_LEFT_BRACE, MS_TOKEN_RIGHT_BRACE,
  MS_TOKEN_COMMA, MS_TOKEN_COLON, MS_TOKEN_DOT, MS_TOKEN_ELLIPSIS  // ...
} MsTokenType;

struct MsToken {
  MsTokenType type;
  const char* start;   // lexeme start; points into the caller-owned source buffer, not copied
  size_t length;       // lexeme length in bytes
  uint32_t line;       // 1-based
  uint32_t column;     // 1-based, byte count (same as Go)
};

typedef enum {
  MS_LEXMODE_NORMAL,          // plain code
  MS_LEXMODE_FSTRING_TEXT,    // f-string text segment (between interpolations)
  MS_LEXMODE_FSTRING_FORMAT   // format spec segment after ':' inside an interpolation
} MsLexerModeKind;

struct MsLexerFrame {         // one frame per f-string interpolation level
  char quote;                 // closing quote of the f-string ('"')
  int braceDepth;             // nesting depth of '{' '}' inside the interpolation expression
};

#define MS_LEXER_MAX_FSTRING_DEPTH 8

struct MsLexer {
  const char* source;         // source buffer, caller-owned; lexer does not own it
  size_t sourceLen;
  size_t pos;                 // current scan byte offset
  uint32_t line;              // current line, 1-based
  uint32_t column;            // current column, 1-based, byte count
  const char* chunkName;      // file/chunk name, caller-owned, written into diagnostics
  bool canEndStatement;       // previous token allows semicolon insertion at end of line
  MsLexerModeKind mode;
  struct MsLexerFrame frames[MS_LEXER_MAX_FSTRING_DEPTH];
  size_t frameCount;          // current f-string interpolation nesting level
  struct MsToken peeked;      // single-token lookahead cache
  MsResult peekedResult;
  bool hasPeeked;
  struct MsDiagList* diags;   // diagnostic collector (task 02), caller-owned
  // Additive implementation detail beyond the spec: sticky flag set once a
  // report fills the diagnostic cap; scanning then yields EOF forever.
  bool hitDiagCap;
  // Additive implementation detail beyond the spec: pairing depth of '{'
  // '}' in plain code (frameCount == 0), so a '}' with no pairing '{'
  // anywhere (E111) can be told apart from a block or dict/set close.
  int blockBraceDepth;
};

// Initializes lexer over [source, sourceLen). source and chunkName must
// outlive the lexer; neither is copied. diags receives lexical errors
// (cap 20 per file). A UTF-8 BOM is reported (E101) and skipped.
void msLexerInit(struct MsLexer* lexer, const char* source, size_t sourceLen,
    const char* chunkName, struct MsDiagList* diags);

// Releases lexer-internal state. Does not free source, chunkName, or diags.
void msLexerDestroy(struct MsLexer* lexer);

// Produces the next token into out.
// Returns MS_OK on a normal token (including MS_TOKEN_INVALID placeholders
// for recoverable errors); returns MS_ERROR_SYNTAX once the diagnostic cap
// is reached, with out set to MS_TOKEN_EOF, signalling the caller to abort.
MsResult msLexerNext(struct MsLexer* lexer, struct MsToken* out);

// Returns the type of the next token without consuming it (1-token
// lookahead, cached). Never fails visibly: an error-limit state reports
// MS_TOKEN_EOF; a bad token reports MS_TOKEN_INVALID.
MsTokenType msLexerPeek(struct MsLexer* lexer);

// Decodes the escape sequences of an already-validated string or bytes
// literal body (raw slice without surrounding quotes/prefix) into a newly
// msAlloc'd buffer. Caller owns *out and frees it with msFree. Braces are
// NOT special here: '{' and '}' pass through as ordinary bytes -- f-string
// text segments use msLexerUnescapeFstringText instead.
// raw must come from a token this lexer produced; invalid input is a
// programming error (MS_ASSERT in debug builds).
MsResult msLexerUnescape(const char* raw, size_t rawLen, char** out, size_t* outLen);

// Decodes the body of an f-string text segment (the lexeme of a STRING
// token produced in FSTRING_TEXT mode, without surrounding quotes) into a
// newly msAlloc'd buffer: the same escapes as msLexerUnescape, plus the
// literal-brace escapes '{{' -> '{' and '}}' -> '}'. Caller owns *out and
// frees it with msFree. raw must come from a token this lexer produced;
// invalid input (including a lone '{' or '}') is a programming error
// (MS_ASSERT in debug builds).
MsResult msLexerUnescapeFstringText(const char* raw, size_t rawLen, char** out, size_t* outLen);

// Static name table for diagnostics and tests ("MS_TOKEN_KW_IF" etc.).
const char* msTokenTypeName(MsTokenType type);

#endif  // MSLANG_SRC_LEXER_MS_LEXER_H_
