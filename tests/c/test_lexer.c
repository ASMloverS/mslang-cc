#include <string.h>

#include "core/ms_common.h"
#include "core/ms_diag.h"
#include "core/ms_memory.h"
#include "lexer/ms_lexer.h"
#include "ms_test.h"

#define MS_TEST_MAX_TOKENS 256

// Lexes the whole source into out; returns the token count (always >= 1,
// last token is MS_TOKEN_EOF unless the diag cap aborted the run).
static size_t msTestLexAll(const char* source, struct MsToken* out, size_t maxOut,
    struct MsDiagList* diags) {
  msDiagListInit(diags, "test.ms");
  struct MsLexer lexer;
  msLexerInit(&lexer, source, strlen(source), "test.ms", diags);
  size_t count = 0;
  while (count < maxOut) {
    MsResult result = msLexerNext(&lexer, &out[count]);
    ++count;
    if (result != MS_OK || out[count - 1].type == MS_TOKEN_EOF) {
      break;
    }
  }
  msLexerDestroy(&lexer);
  return count;
}

// True when the token's lexeme slice equals text.
static bool msTestLexemeEq(const struct MsToken* token, const char* text) {
  return token->length == strlen(text) && memcmp(token->start, text, token->length) == 0;
}

// True when the token's lexeme slice equals the len-byte text.
static bool msTestLexemeEqN(const struct MsToken* token, const char* text, size_t len) {
  return token->length == len && memcmp(token->start, text, len) == 0;
}

// Lexes source and asserts zero diagnostics and exactly the expected token
// type sequence followed by MS_TOKEN_EOF.
static void msTestExpectTypes(const char* source, const MsTokenType* expect, size_t expectCount) {
  struct MsDiagList diags;
  struct MsToken tokens[MS_TEST_MAX_TOKENS];
  size_t count = msTestLexAll(source, tokens, MS_TEST_MAX_TOKENS, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  MS_ASSERT_EQ(expectCount + 1, count);
  size_t check = count - 1 < expectCount ? count - 1 : expectCount;
  for (size_t i = 0; i < check; ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[count - 1].type);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, TokenTypeNameCoversEveryValue) {
  for (int type = MS_TOKEN_EOF; type <= MS_TOKEN_ELLIPSIS; ++type) {
    const char* name = msTokenTypeName((MsTokenType)type);
    MS_ASSERT_TRUE(name != NULL);
    MS_ASSERT_TRUE(strncmp(name, "MS_TOKEN_", 9) == 0);
  }
}

MS_TEST(Lexer, TokenTypeNameSpots) {
  MS_ASSERT_TRUE(strcmp(msTokenTypeName(MS_TOKEN_EOF), "MS_TOKEN_EOF") == 0);
  MS_ASSERT_TRUE(strcmp(msTokenTypeName(MS_TOKEN_KW_IF), "MS_TOKEN_KW_IF") == 0);
  MS_ASSERT_TRUE(strcmp(msTokenTypeName(MS_TOKEN_SHIFT_LEFT_EQUAL), "MS_TOKEN_SHIFT_LEFT_EQUAL") == 0);
  MS_ASSERT_TRUE(strcmp(msTokenTypeName(MS_TOKEN_ELLIPSIS), "MS_TOKEN_ELLIPSIS") == 0);
}

MS_TEST(Lexer, EmptySourceProducesEof) {
  struct MsDiagList diags;
  struct MsToken tokens[4];
  size_t count = msTestLexAll("", tokens, 4, &diags);
  MS_ASSERT_EQ(1, count);
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[0].type);
  MS_ASSERT_EQ(1, tokens[0].line);
  MS_ASSERT_EQ(1, tokens[0].column);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, BomIsSkippedAndReported) {
  struct MsDiagList diags;
  struct MsToken tokens[4];
  size_t count = msTestLexAll("\xEF\xBB\xBFx", tokens, 4, &diags);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  MS_ASSERT_EQ(101, msDiagListAt(&diags, 0)->code);
  MS_ASSERT_EQ(1, msDiagListAt(&diags, 0)->line);
  MS_ASSERT_EQ(1, msDiagListAt(&diags, 0)->column);
  // x is the first real token; its column counts the BOM bytes (Go-style byte columns).
  bool sawIdentifier = false;
  for (size_t i = 0; i < count; ++i) {
    if (tokens[i].type == MS_TOKEN_IDENTIFIER) {
      sawIdentifier = true;
      MS_ASSERT_TRUE(msTestLexemeEq(&tokens[i], "x"));
      MS_ASSERT_EQ(4, tokens[i].column);
    }
  }
  MS_ASSERT_TRUE(sawIdentifier);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, CrlfAndLfProduceSameTokens) {
  const char* lf = "a\nb";
  const char* crlf = "a\r\nb";
  struct MsDiagList diagsLf;
  struct MsDiagList diagsCrlf;
  struct MsToken tokensLf[16];
  struct MsToken tokensCrlf[16];
  size_t countLf = msTestLexAll(lf, tokensLf, 16, &diagsLf);
  size_t countCrlf = msTestLexAll(crlf, tokensCrlf, 16, &diagsCrlf);
  MS_ASSERT_EQ(countLf, countCrlf);
  for (size_t i = 0; i < countLf; ++i) {
    MS_ASSERT_EQ(tokensLf[i].type, tokensCrlf[i].type);
    MS_ASSERT_EQ(tokensLf[i].line, tokensCrlf[i].line);
  }
  // a, inserted semicolon (newline position, line 1), then b on line 2.
  MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, tokensCrlf[1].type);
  MS_ASSERT_EQ(1, tokensCrlf[1].line);
  MS_ASSERT_EQ(2, tokensCrlf[2].line);
  msDiagListDestroy(&diagsLf);
  msDiagListDestroy(&diagsCrlf);
}

MS_TEST(Lexer, BareCarriageReturnIsE112) {
  struct MsDiagList diags;
  struct MsToken tokens[8];
  msTestLexAll("a\rb", tokens, 8, &diags);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  MS_ASSERT_EQ(112, msDiagListAt(&diags, 0)->code);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, CommentOnlySourceProducesEof) {
  struct MsDiagList diags;
  struct MsToken tokens[8];
  size_t count = msTestLexAll("// hello\n/* block\ncomment */\n", tokens, 8, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[count - 1].type);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, TokensAroundComments) {
  struct MsDiagList diags;
  struct MsToken tokens[16];
  size_t count = msTestLexAll("a /* x\ny */ b // tail\nc", tokens, 16, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  int identifiers = 0;
  for (size_t i = 0; i < count; ++i) {
    if (tokens[i].type == MS_TOKEN_IDENTIFIER) {
      ++identifiers;
      if (msTestLexemeEq(&tokens[i], "b")) {
        // Line tracking must survive the newline inside the block comment.
        MS_ASSERT_EQ(2, tokens[i].line);
      }
    }
  }
  MS_ASSERT_EQ(3, identifiers);  // a, b, c
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, UnclosedBlockCommentIsE105) {
  struct MsDiagList diags;
  struct MsToken tokens[8];
  msTestLexAll("a\n/* x", tokens, 8, &diags);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  MS_ASSERT_EQ(105, msDiagListAt(&diags, 0)->code);
  // The diagnostic points at the comment's start, not at end of input.
  MS_ASSERT_EQ(2, msDiagListAt(&diags, 0)->line);
  MS_ASSERT_EQ(1, msDiagListAt(&diags, 0)->column);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, BlockCommentsDoNotNest) {
  struct MsDiagList diags;
  struct MsToken tokens[8];
  // "/* a /* b */ ident" ends at the first "*/"; the trailing "ident" is code.
  size_t count = msTestLexAll("/* a /* b */ ident", tokens, 8, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  bool sawIdent = false;
  for (size_t i = 0; i < count; ++i) {
    if (tokens[i].type == MS_TOKEN_IDENTIFIER && msTestLexemeEq(&tokens[i], "ident")) {
      sawIdent = true;
    }
  }
  MS_ASSERT_TRUE(sawIdent);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, AllKeywordsScan) {
  static const struct {
    const char* text;
    MsTokenType type;
  } kKeywordCases[] = {
      {"and", MS_TOKEN_KW_AND},       {"as", MS_TOKEN_KW_AS},
      {"async", MS_TOKEN_KW_ASYNC},   {"await", MS_TOKEN_KW_AWAIT},
      {"break", MS_TOKEN_KW_BREAK},   {"case", MS_TOKEN_KW_CASE},
      {"class", MS_TOKEN_KW_CLASS},   {"continue", MS_TOKEN_KW_CONTINUE},
      {"default", MS_TOKEN_KW_DEFAULT}, {"del", MS_TOKEN_KW_DEL},
      {"div", MS_TOKEN_KW_DIV},
      {"else", MS_TOKEN_KW_ELSE},     {"except", MS_TOKEN_KW_EXCEPT},
      {"false", MS_TOKEN_KW_FALSE},   {"finally", MS_TOKEN_KW_FINALLY},
      {"for", MS_TOKEN_KW_FOR},       {"from", MS_TOKEN_KW_FROM},
      {"func", MS_TOKEN_KW_FUNC},     {"global", MS_TOKEN_KW_GLOBAL},
      {"if", MS_TOKEN_KW_IF},         {"import", MS_TOKEN_KW_IMPORT},
      {"in", MS_TOKEN_KW_IN},         {"is", MS_TOKEN_KW_IS},
      {"lambda", MS_TOKEN_KW_LAMBDA}, {"nil", MS_TOKEN_KW_NIL},
      {"not", MS_TOKEN_KW_NOT},       {"or", MS_TOKEN_KW_OR},
      {"pass", MS_TOKEN_KW_PASS},     {"raise", MS_TOKEN_KW_RAISE},
      {"return", MS_TOKEN_KW_RETURN}, {"select", MS_TOKEN_KW_SELECT},
      {"self", MS_TOKEN_KW_SELF},     {"super", MS_TOKEN_KW_SUPER},
      {"true", MS_TOKEN_KW_TRUE},     {"try", MS_TOKEN_KW_TRY},
      {"while", MS_TOKEN_KW_WHILE},   {"with", MS_TOKEN_KW_WITH},
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(kKeywordCases); ++i) {
    struct MsDiagList diags;
    struct MsToken tokens[4];
    msTestLexAll(kKeywordCases[i].text, tokens, 4, &diags);
    MS_ASSERT_EQ(kKeywordCases[i].type, tokens[0].type);
    MS_ASSERT_EQ(0, msDiagListCount(&diags));
    msDiagListDestroy(&diags);
  }
}

MS_TEST(Lexer, BuiltinsAndCaseVariantsAreIdentifiers) {
  const char* source = "chan len print And IF selfx _ _x9";
  struct MsDiagList diags;
  struct MsToken tokens[32];
  size_t count = msTestLexAll(source, tokens, 32, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  int identifiers = 0;
  for (size_t i = 0; i < count; ++i) {
    if (tokens[i].type == MS_TOKEN_IDENTIFIER) {
      ++identifiers;
    }
  }
  MS_ASSERT_EQ(8, identifiers);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, NonAsciiIdentifierAndInvalidUtf8) {
  struct MsDiagList diags;
  struct MsToken tokens[8];
  msTestLexAll("\xC3\xA9tude", tokens, 8, &diags);  // U+00E9 + "tude"
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokens[0].type);
  msDiagListDestroy(&diags);

  struct MsDiagList diagsBad;
  struct MsToken tokensBad[8];
  size_t count = msTestLexAll("\xC3(", tokensBad, 8, &diagsBad);  // truncated UTF-8
  MS_ASSERT_TRUE(msDiagListCount(&diagsBad) >= 1);
  MS_ASSERT_EQ(102, msDiagListAt(&diagsBad, 0)->code);
  // The INVALID token covers exactly the malformed lead byte.
  MS_ASSERT_EQ(MS_TOKEN_INVALID, tokensBad[0].type);
  MS_ASSERT_EQ(1, tokensBad[0].length);
  bool sawInvalid = false;
  for (size_t i = 0; i < count; ++i) {
    if (tokensBad[i].type == MS_TOKEN_INVALID) {
      sawInvalid = true;
    }
  }
  MS_ASSERT_TRUE(sawInvalid);
  msDiagListDestroy(&diagsBad);
}

MS_TEST(Lexer, NumberLiteralsScan) {
  static const struct {
    const char* source;
    MsTokenType type;
  } kNumberCases[] = {
      {"42", MS_TOKEN_INT},        {"0", MS_TOKEN_INT},
      {"1_000_000", MS_TOKEN_INT}, {"0x1F", MS_TOKEN_INT},
      {"0x_1F", MS_TOKEN_INT},     {"0X2a", MS_TOKEN_INT},
      {"0o755", MS_TOKEN_INT},     {"0O755", MS_TOKEN_INT},
      {"0b1010", MS_TOKEN_INT},    {"0B1010", MS_TOKEN_INT},
      {"3.14", MS_TOKEN_FLOAT},    {"1e-9", MS_TOKEN_FLOAT},
      {"2.5e+4", MS_TOKEN_FLOAT},  {".5", MS_TOKEN_FLOAT},
      {"1e9", MS_TOKEN_FLOAT},     {"0.5e2", MS_TOKEN_FLOAT},
      {".5e2", MS_TOKEN_FLOAT},
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(kNumberCases); ++i) {
    struct MsDiagList diags;
    struct MsToken tokens[4];
    size_t count = msTestLexAll(kNumberCases[i].source, tokens, 4, &diags);
    MS_ASSERT_EQ(kNumberCases[i].type, tokens[0].type);
    MS_ASSERT_TRUE(msTestLexemeEq(&tokens[0], kNumberCases[i].source));
    MS_ASSERT_EQ(1, tokens[0].line);
    MS_ASSERT_EQ(1, tokens[0].column);
    MS_ASSERT_EQ(0, msDiagListCount(&diags));
    for (size_t j = 0; j < count; ++j) {
      MS_ASSERT_TRUE(tokens[j].type != MS_TOKEN_INVALID);
    }
    msDiagListDestroy(&diags);
  }
}

MS_TEST(Lexer, NumberLiteralsReportErrors) {
  static const struct {
    const char* source;
    uint32_t code;
  } kNumberErrorCases[] = {
      {"1_", 107},     {"1__2", 107},   {"0x", 107},
      {"0b102", 107},  {"0o8", 107},    {"0xG", 107},
      {"1e", 107},     {"1e+", 107},
      {"1.", 108},     {"5..", 108},    {"5...", 108},
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(kNumberErrorCases); ++i) {
    struct MsDiagList diags;
    struct MsToken tokens[8];
    size_t count = msTestLexAll(kNumberErrorCases[i].source, tokens, 8, &diags);
    MS_ASSERT_TRUE(msDiagListCount(&diags) >= 1);
    MS_ASSERT_EQ(kNumberErrorCases[i].code, msDiagListAt(&diags, 0)->code);
    bool sawInvalid = false;
    for (size_t j = 0; j < count; ++j) {
      if (tokens[j].type == MS_TOKEN_INVALID) {
        sawInvalid = true;
      }
    }
    MS_ASSERT_TRUE(sawInvalid);
    msDiagListDestroy(&diags);
  }
}

MS_TEST(Lexer, NumberErrorPositionPinned) {
  // The diagnostic for a bad base digit points at the offending digit, and
  // the INVALID token covers the whole would-be literal. "0b102": the '2'
  // sits at column 5 (0=1, b=2, 1=3, 0=4, 2=5).
  struct MsDiagList diags;
  struct MsToken tokens[8];
  msTestLexAll("0b102", tokens, 8, &diags);
  MS_ASSERT_TRUE(msDiagListCount(&diags) >= 1);
  MS_ASSERT_EQ(107, msDiagListAt(&diags, 0)->code);
  MS_ASSERT_EQ(1, msDiagListAt(&diags, 0)->line);
  MS_ASSERT_EQ(5, msDiagListAt(&diags, 0)->column);
  MS_ASSERT_EQ(MS_TOKEN_INVALID, tokens[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[0], "0b102"));
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, DotFiveVersusMemberAccess) {
  // Locked judgment call: maximal munch on '.' + digit makes ".5" a float
  // even directly after an identifier, so "x.5" is IDENTIFIER + FLOAT.
  struct MsDiagList diags;
  struct MsToken tokens[8];
  size_t count = msTestLexAll("x.5", tokens, 8, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  MS_ASSERT_TRUE(count >= 3);
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokens[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[0], "x"));
  MS_ASSERT_EQ(MS_TOKEN_FLOAT, tokens[1].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[1], ".5"));
  msDiagListDestroy(&diags);

  struct MsDiagList diagsMember;
  struct MsToken tokensMember[8];
  msTestLexAll("x.y", tokensMember, 8, &diagsMember);
  MS_ASSERT_EQ(0, msDiagListCount(&diagsMember));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokensMember[0].type);
  MS_ASSERT_EQ(MS_TOKEN_DOT, tokensMember[1].type);
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokensMember[2].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokensMember[2], "y"));
  msDiagListDestroy(&diagsMember);
}

MS_TEST(Lexer, StringLiteralsScan) {
  // Plain string: the lexeme includes both quotes.
  struct MsDiagList diagsString;
  struct MsToken tokensString[4];
  msTestLexAll("\"abc\"", tokensString, 4, &diagsString);
  MS_ASSERT_EQ(0, msDiagListCount(&diagsString));
  MS_ASSERT_EQ(MS_TOKEN_STRING, tokensString[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokensString[0], "\"abc\""));
  msDiagListDestroy(&diagsString);

  // Raw string with a real newline inside the backticks.
  struct MsDiagList diagsRaw;
  struct MsToken tokensRaw[8];
  size_t countRaw = msTestLexAll("`raw\nline`", tokensRaw, 8, &diagsRaw);
  MS_ASSERT_EQ(0, msDiagListCount(&diagsRaw));
  MS_ASSERT_EQ(MS_TOKEN_RAW_STRING, tokensRaw[0].type);
  MS_ASSERT_TRUE(msTestLexemeEqN(&tokensRaw[0], "`raw\nline`", 10));
  MS_ASSERT_EQ(1, tokensRaw[0].line);
  MS_ASSERT_TRUE(countRaw >= 2);
  MS_ASSERT_EQ(2, tokensRaw[1].line);  // the compensating semicolon lands on line 2
  msDiagListDestroy(&diagsRaw);

  // Bytes literal: b prefix + quoted body, lexeme includes prefix and quotes.
  struct MsDiagList diagsBytes;
  struct MsToken tokensBytes[4];
  msTestLexAll("b\"\\x00\\x01\"", tokensBytes, 4, &diagsBytes);
  MS_ASSERT_EQ(0, msDiagListCount(&diagsBytes));
  MS_ASSERT_EQ(MS_TOKEN_BYTES, tokensBytes[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokensBytes[0], "b\"\\x00\\x01\""));
  msDiagListDestroy(&diagsBytes);

  // A "b" not immediately followed by '"' is a plain identifier.
  struct MsDiagList diagsB;
  struct MsToken tokensB[8];
  msTestLexAll("b", tokensB, 8, &diagsB);
  MS_ASSERT_EQ(0, msDiagListCount(&diagsB));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokensB[0].type);
  msDiagListDestroy(&diagsB);

  struct MsDiagList diagsBx;
  struct MsToken tokensBx[8];
  msTestLexAll("bx", tokensBx, 8, &diagsBx);
  MS_ASSERT_EQ(0, msDiagListCount(&diagsBx));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokensBx[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokensBx[0], "bx"));
  msDiagListDestroy(&diagsBx);
}

// Decoding expectations shared by Lexer.UnescapeDecodesEscapes and
// Lexer.UnescapeLeakFree. raw is a literal body without surrounding quotes.
static const struct {
  const char* raw;
  const char* expect;
  size_t expectLen;
} msTestEscapeCases[] = {
    {"a\\nb", "a\nb", 3},
    {"\\t\\r\\\\", "\t\r\\", 3},
    {"\\\"", "\"", 1},
    {"\\'", "'", 1},
    {"\\0", "\0", 1},
    {"\\x41", "A", 1},
    {"\\u4e2d", "\xE4\xB8\xAD", 3},          // U+4E2D
    {"\\U0001F600", "\xF0\x9F\x98\x80", 4},  // U+1F600
    {"a{b", "a{b", 3},                       // braces are ordinary bytes
    {"{{", "{{", 2},                         // no {{/}} decoding in plain strings
};

MS_TEST(Lexer, UnescapeDecodesEscapes) {
  for (size_t i = 0; i < MS_ARRAY_LEN(msTestEscapeCases); ++i) {
    char* out = NULL;
    size_t outLen = 0;
    MsResult result = msLexerUnescape(msTestEscapeCases[i].raw,
        strlen(msTestEscapeCases[i].raw), &out, &outLen);
    MS_ASSERT_EQ(MS_OK, result);
    if (result == MS_OK) {
      MS_ASSERT_EQ(msTestEscapeCases[i].expectLen, outLen);
      MS_ASSERT_TRUE(out != NULL);
      if (out != NULL) {
        MS_ASSERT_TRUE(memcmp(out, msTestEscapeCases[i].expect, outLen) == 0);
      }
    }
    msFree(out);
  }
}

MS_TEST(Lexer, StringErrors) {
  static const struct {
    const char* source;
    uint32_t code;
  } kStringErrorCases[] = {
      {"\"abc", 103},             // end of input before closing quote
      {"\"ab\ncd\"", 103},        // unescaped real newline ends the string
      {"`unterminated", 104},
      {"\"\\x0\"", 106},          // too few hex digits
      {"\"\\uD800\"", 106},       // surrogate code point
      {"\"\\U00110000\"", 106},   // above U+10FFFF
      {"\"\\q\"", 106},           // unknown escape letter
      {"\"\\", 106},              // backslash right before end of input
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(kStringErrorCases); ++i) {
    struct MsDiagList diags;
    struct MsToken tokens[8];
    size_t count = msTestLexAll(kStringErrorCases[i].source, tokens, 8, &diags);
    MS_ASSERT_TRUE(msDiagListCount(&diags) >= 1);
    if (msDiagListCount(&diags) >= 1) {
      MS_ASSERT_EQ(kStringErrorCases[i].code, msDiagListAt(&diags, 0)->code);
    }
    bool sawInvalid = false;
    for (size_t j = 0; j < count; ++j) {
      if (tokens[j].type == MS_TOKEN_INVALID) {
        sawInvalid = true;
      }
    }
    MS_ASSERT_TRUE(sawInvalid);
    msDiagListDestroy(&diags);
  }
}

MS_TEST(Lexer, StringErrorPositionPinned) {
  // E106 points at the backslash of the offending escape. Source "\"ab\\q\"":
  // '"'=column 1, a=2, b=3, backslash=4.
  struct MsDiagList diags;
  struct MsToken tokens[8];
  msTestLexAll("\"ab\\q\"", tokens, 8, &diags);
  MS_ASSERT_TRUE(msDiagListCount(&diags) >= 1);
  MS_ASSERT_EQ(106, msDiagListAt(&diags, 0)->code);
  MS_ASSERT_EQ(1, msDiagListAt(&diags, 0)->line);
  MS_ASSERT_EQ(4, msDiagListAt(&diags, 0)->column);
  MS_ASSERT_EQ(MS_TOKEN_INVALID, tokens[0].type);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, UnescapeLeakFree) {
  struct MsMemStats before;
  msMemGetStats(&before);
  struct MsDiagList diags;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&diags, "test.ms"));
  for (size_t i = 0; i < MS_ARRAY_LEN(msTestEscapeCases); ++i) {
    char* out = NULL;
    size_t outLen = 0;
    MsResult result = msLexerUnescape(msTestEscapeCases[i].raw,
        strlen(msTestEscapeCases[i].raw), &out, &outLen);
    MS_ASSERT_EQ(MS_OK, result);
    msFree(out);
  }
  msDiagListDestroy(&diags);
  struct MsMemStats after;
  msMemGetStats(&after);
  MS_ASSERT_EQ(before.liveBlocks, after.liveBlocks);
  MS_ASSERT_EQ(before.currentBytes, after.currentBytes);
}

MS_TEST(Lexer, UnescapeOom) {
  // msMemSetFailAfter(0) makes the very next allocation fail (ms_memory.c
  // consumeFailAfter), so the output buffer msAlloc must report OOM.
  msMemResetStats();
  msMemSetFailAfter(0);
  char* out = NULL;
  size_t outLen = 0;
  MsResult result = msLexerUnescape("a", 1, &out, &outLen);
  MS_ASSERT_EQ(MS_ERROR_OOM, result);
  MS_ASSERT_TRUE(out == NULL);
  MS_ASSERT_EQ(0, outLen);
  msMemSetFailAfter(-1);
}

MS_TEST(Lexer, StringRoundTrip) {
  // Scan a string token, then decode its body (lexeme minus the quotes).
  struct MsDiagList diags;
  struct MsToken tokens[4];
  msTestLexAll("\"a\\nb\"", tokens, 4, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  MS_ASSERT_EQ(MS_TOKEN_STRING, tokens[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[0], "\"a\\nb\""));
  char* out = NULL;
  size_t outLen = 0;
  MsResult result = msLexerUnescape(tokens[0].start + 1, tokens[0].length - 2, &out, &outLen);
  MS_ASSERT_EQ(MS_OK, result);
  if (result == MS_OK) {
    MS_ASSERT_EQ(3, outLen);
    MS_ASSERT_TRUE(out != NULL);
    if (out != NULL) {
      MS_ASSERT_TRUE(memcmp(out, "a\nb", 3) == 0);
    }
  }
  msFree(out);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, OperatorsScan) {
  static const struct {
    const char* source;
    MsTokenType type;
  } kOperatorCases[] = {
      {"+", MS_TOKEN_PLUS},       {"-", MS_TOKEN_MINUS},
      {"*", MS_TOKEN_STAR},       {"/", MS_TOKEN_SLASH},
      {"%", MS_TOKEN_PERCENT},    {"**", MS_TOKEN_DOUBLE_STAR},
      {"==", MS_TOKEN_EQUAL_EQUAL}, {"!=", MS_TOKEN_BANG_EQUAL},
      {"<", MS_TOKEN_LESS},       {"<=", MS_TOKEN_LESS_EQUAL},
      {">", MS_TOKEN_GREATER},    {">=", MS_TOKEN_GREATER_EQUAL},
      {"&", MS_TOKEN_AMP},        {"|", MS_TOKEN_PIPE},
      {"^", MS_TOKEN_CARET},      {"~", MS_TOKEN_TILDE},
      {"<<", MS_TOKEN_SHIFT_LEFT}, {">>", MS_TOKEN_SHIFT_RIGHT},
      {":=", MS_TOKEN_COLON_EQUAL}, {"=", MS_TOKEN_EQUAL},
      {"+=", MS_TOKEN_PLUS_EQUAL}, {"-=", MS_TOKEN_MINUS_EQUAL},
      {"*=", MS_TOKEN_STAR_EQUAL}, {"/=", MS_TOKEN_SLASH_EQUAL},
      {"%=", MS_TOKEN_PERCENT_EQUAL}, {"**=", MS_TOKEN_DOUBLE_STAR_EQUAL},
      {"&=", MS_TOKEN_AMP_EQUAL},  {"|=", MS_TOKEN_PIPE_EQUAL},
      {"^=", MS_TOKEN_CARET_EQUAL}, {"<<=", MS_TOKEN_SHIFT_LEFT_EQUAL},
      {">>=", MS_TOKEN_SHIFT_RIGHT_EQUAL}, {"++", MS_TOKEN_PLUS_PLUS},
      {"--", MS_TOKEN_MINUS_MINUS}, {"(", MS_TOKEN_LEFT_PAREN},
      {")", MS_TOKEN_RIGHT_PAREN}, {"[", MS_TOKEN_LEFT_BRACKET},
      {"]", MS_TOKEN_RIGHT_BRACKET}, {"{", MS_TOKEN_LEFT_BRACE},
      {",", MS_TOKEN_COMMA},
      {":", MS_TOKEN_COLON},      {".", MS_TOKEN_DOT},
      {"...", MS_TOKEN_ELLIPSIS}, {";", MS_TOKEN_SEMICOLON},
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(kOperatorCases); ++i) {
    struct MsDiagList diags;
    struct MsToken tokens[4];
    msTestLexAll(kOperatorCases[i].source, tokens, 4, &diags);
    MS_ASSERT_EQ(kOperatorCases[i].type, tokens[0].type);
    MS_ASSERT_TRUE(msTestLexemeEq(&tokens[0], kOperatorCases[i].source));
    // "}" is absent from the table: a lone '}' has no pairing '{' and is
    // E111 + INVALID by design. MS_TOKEN_RIGHT_BRACE is exercised by
    // Lexer.BracesPairInPlainCode and the f-string tests instead.
    MS_ASSERT_EQ(0, msDiagListCount(&diags));
    msDiagListDestroy(&diags);
  }
}

MS_TEST(Lexer, OperatorsMaximalMunch) {
  {
    const MsTokenType expect[] = {MS_TOKEN_SHIFT_LEFT_EQUAL};
    msTestExpectTypes("<<=", expect, MS_ARRAY_LEN(expect));
  }
  {
    const MsTokenType expect[] = {MS_TOKEN_SHIFT_LEFT, MS_TOKEN_LESS_EQUAL, MS_TOKEN_LESS};
    msTestExpectTypes("<< <= <", expect, MS_ARRAY_LEN(expect));
  }
  {
    // '.' priority: "..." > float ('.' + digit) > DOT.
    struct MsDiagList diags;
    struct MsToken tokens[8];
    size_t count = msTestLexAll("... .5 .", tokens, 8, &diags);
    MS_ASSERT_EQ(0, msDiagListCount(&diags));
    MS_ASSERT_EQ(4, count);
    MS_ASSERT_EQ(MS_TOKEN_ELLIPSIS, tokens[0].type);
    MS_ASSERT_EQ(MS_TOKEN_FLOAT, tokens[1].type);
    MS_ASSERT_TRUE(msTestLexemeEq(&tokens[1], ".5"));
    MS_ASSERT_EQ(MS_TOKEN_DOT, tokens[2].type);
    MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[3].type);
    msDiagListDestroy(&diags);
  }
  {
    const MsTokenType expect[] = {
        MS_TOKEN_DOUBLE_STAR_EQUAL, MS_TOKEN_DOUBLE_STAR,
        MS_TOKEN_STAR_EQUAL, MS_TOKEN_STAR,
    };
    msTestExpectTypes("**= ** *= *", expect, MS_ARRAY_LEN(expect));
  }
  {
    const MsTokenType expect[] = {MS_TOKEN_SLASH_EQUAL, MS_TOKEN_SLASH};
    msTestExpectTypes("/= /", expect, MS_ARRAY_LEN(expect));
  }
  {
    // "++"/"--" end a statement, so EOF compensates with a semicolon.
    const MsTokenType expect[] = {MS_TOKEN_PLUS_PLUS, MS_TOKEN_MINUS_MINUS, MS_TOKEN_SEMICOLON};
    msTestExpectTypes("++ --", expect, MS_ARRAY_LEN(expect));
  }
}

MS_TEST(Lexer, SlashSlashIsAlwaysComment) {
  // Ruling: "//" UNCONDITIONALLY starts a line comment -- there is no "//"
  // or "//=" operator token; floor division is the keyword "div".
  struct MsDiagList diags;
  struct MsToken tokens[4];
  size_t count = msTestLexAll("// just a comment", tokens, 4, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  MS_ASSERT_EQ(1, count);
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[0].type);
  msDiagListDestroy(&diags);

  struct MsDiagList diagsEq;
  struct MsToken tokensEq[4];
  size_t countEq = msTestLexAll("//= also a comment", tokensEq, 4, &diagsEq);
  MS_ASSERT_EQ(0, msDiagListCount(&diagsEq));
  MS_ASSERT_EQ(1, countEq);
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokensEq[0].type);
  msDiagListDestroy(&diagsEq);

  // "a // b" is the identifier a followed by a comment, NOT floor division.
  // The statement ends at end of input, so EOF compensates with a semicolon.
  struct MsDiagList diagsTail;
  struct MsToken tokensTail[4];
  size_t countTail = msTestLexAll("a // b", tokensTail, 4, &diagsTail);
  MS_ASSERT_EQ(0, msDiagListCount(&diagsTail));
  MS_ASSERT_EQ(3, countTail);
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokensTail[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokensTail[0], "a"));
  MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, tokensTail[1].type);
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokensTail[2].type);
  msDiagListDestroy(&diagsTail);
}

MS_TEST(Lexer, BareBangIsE102) {
  // The language has no logical-not symbol (logical not is the keyword
  // "not"), so a bare '!' is an illegal character; "!=" is the only '!' token.
  struct MsDiagList diags;
  struct MsToken tokens[4];
  size_t count = msTestLexAll("!", tokens, 4, &diags);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  MS_ASSERT_EQ(102, msDiagListAt(&diags, 0)->code);
  MS_ASSERT_EQ(1, msDiagListAt(&diags, 0)->line);
  MS_ASSERT_EQ(1, msDiagListAt(&diags, 0)->column);
  MS_ASSERT_TRUE(count >= 1);
  MS_ASSERT_EQ(MS_TOKEN_INVALID, tokens[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[0], "!"));
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, DivKeywordScans) {
  struct MsDiagList diags;
  struct MsToken tokens[8];
  size_t count = msTestLexAll("a div b", tokens, 8, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  // Trailing identifier ends a statement; EOF compensates with a semicolon.
  MS_ASSERT_EQ(5, count);
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokens[0].type);
  MS_ASSERT_EQ(MS_TOKEN_KW_DIV, tokens[1].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[1], "div"));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokens[2].type);
  MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, tokens[3].type);
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[4].type);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, UnexpectedByteIsE102) {
  // The generic E102 branch must name the offending byte, and scanning must
  // continue after the error.
  struct MsDiagList diags;
  struct MsToken tokens[8];
  size_t count = msTestLexAll("? x", tokens, 8, &diags);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  MS_ASSERT_EQ(102, msDiagListAt(&diags, 0)->code);
  // The diagnostic message renders the offending byte.
  MS_ASSERT_TRUE(strstr(msDiagListAt(&diags, 0)->message, "'?'") != NULL);
  MS_ASSERT_EQ(MS_TOKEN_INVALID, tokens[0].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[0], "?"));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, tokens[1].type);
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[1], "x"));
  MS_ASSERT_TRUE(count >= 2);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, SemicolonInsertedAfterStatementEnders) {
  const char* source = "x := 1\ny := 2\n";
  struct MsDiagList diags;
  struct MsToken tokens[16];
  size_t count = msTestLexAll(source, tokens, 16, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  const MsTokenType expect[] = {
      MS_TOKEN_IDENTIFIER, MS_TOKEN_COLON_EQUAL, MS_TOKEN_INT, MS_TOKEN_SEMICOLON,
      MS_TOKEN_IDENTIFIER, MS_TOKEN_COLON_EQUAL, MS_TOKEN_INT, MS_TOKEN_SEMICOLON,
      MS_TOKEN_EOF,
  };
  MS_ASSERT_EQ(MS_ARRAY_LEN(expect), count);
  for (size_t i = 0; i < MS_ARRAY_LEN(expect); ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  // Auto-inserted semicolons have empty lexemes at the newline position.
  MS_ASSERT_EQ(0, tokens[3].length);
  MS_ASSERT_EQ(1, tokens[3].line);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, SemicolonInsertedAtEof) {
  struct MsDiagList diags;
  struct MsToken tokens[8];
  size_t count = msTestLexAll("x := 1", tokens, 8, &diags);  // no trailing newline
  MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, tokens[count - 2].type);
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[count - 1].type);
  // The compensating semicolon has an empty lexeme at the EOF position.
  MS_ASSERT_EQ(0, tokens[count - 2].length);
  MS_ASSERT_EQ(tokens[count - 1].line, tokens[count - 2].line);
  MS_ASSERT_EQ(tokens[count - 1].column, tokens[count - 2].column);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, NoSemicolonAfterContinuationTokens) {
  const char* source = "x := 1 +\n2\ny := (\n3)\n";
  struct MsDiagList diags;
  struct MsToken tokens[24];
  size_t count = msTestLexAll(source, tokens, 24, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  const MsTokenType expect[] = {
      MS_TOKEN_IDENTIFIER, MS_TOKEN_COLON_EQUAL, MS_TOKEN_INT, MS_TOKEN_PLUS,
      MS_TOKEN_INT, MS_TOKEN_SEMICOLON,
      MS_TOKEN_IDENTIFIER, MS_TOKEN_COLON_EQUAL, MS_TOKEN_LEFT_PAREN,
      MS_TOKEN_INT, MS_TOKEN_RIGHT_PAREN, MS_TOKEN_SEMICOLON,
      MS_TOKEN_EOF,
  };
  MS_ASSERT_EQ(MS_ARRAY_LEN(expect), count);
  for (size_t i = 0; i < MS_ARRAY_LEN(expect); ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, ExplicitAndRepeatedSemicolonsPassThrough) {
  struct MsDiagList diags;
  struct MsToken tokens[12];
  size_t count = msTestLexAll("a;; b", tokens, 12, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));  // lexer does not reject ;;
  const MsTokenType expect[] = {
      MS_TOKEN_IDENTIFIER, MS_TOKEN_SEMICOLON, MS_TOKEN_SEMICOLON,
      MS_TOKEN_IDENTIFIER, MS_TOKEN_SEMICOLON, MS_TOKEN_EOF,
  };
  MS_ASSERT_EQ(MS_ARRAY_LEN(expect), count);
  for (size_t i = 0; i < MS_ARRAY_LEN(expect); ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[1], ";"));   // explicit: real lexeme
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, NewlineInsideBlockCommentTriggersInsertion) {
  struct MsDiagList diags;
  struct MsToken tokens[12];
  size_t count = msTestLexAll("a /* x\ny */ b", tokens, 12, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  const MsTokenType expect[] = {
      MS_TOKEN_IDENTIFIER, MS_TOKEN_SEMICOLON, MS_TOKEN_IDENTIFIER,
      MS_TOKEN_SEMICOLON, MS_TOKEN_EOF,
  };
  MS_ASSERT_EQ(MS_ARRAY_LEN(expect), count);
  for (size_t i = 0; i < MS_ARRAY_LEN(expect); ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, NewlineAfterLineCommentTriggersInsertion) {
  struct MsDiagList diags;
  struct MsToken tokens[12];
  size_t count = msTestLexAll("a // note\nb", tokens, 12, &diags);
  const MsTokenType expect[] = {
      MS_TOKEN_IDENTIFIER, MS_TOKEN_SEMICOLON, MS_TOKEN_IDENTIFIER,
      MS_TOKEN_SEMICOLON, MS_TOKEN_EOF,
  };
  MS_ASSERT_EQ(MS_ARRAY_LEN(expect), count);
  for (size_t i = 0; i < MS_ARRAY_LEN(expect); ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, BlankLinesProduceNoExtraSemicolons) {
  const MsTokenType expect[] = {
      MS_TOKEN_IDENTIFIER, MS_TOKEN_SEMICOLON,
      MS_TOKEN_IDENTIFIER, MS_TOKEN_SEMICOLON,
  };
  msTestExpectTypes("x\n\ny", expect, MS_ARRAY_LEN(expect));
}

MS_TEST(Lexer, SemicolonTriggerSetCoversAllEnders) {
  // Every statement-ender token of 01-lexical section 7, each as
  // "<trigger>\nx": the token right before the trailing "x" must be an
  // auto-inserted semicolon.
  static const char* const kEnderCases[] = {
      "foo\nx",      // identifier
      "42\nx",       // int
      "3.14\nx",     // float
      "\"s\"\nx",    // string
      "`r`\nx",      // raw string
      "b\"b\"\nx",   // bytes
      "true\nx",     // keyword literals
      "false\nx",
      "nil\nx",
      "return\nx",   // statement keywords
      "break\nx",
      "continue\nx",
      "pass\nx",
      "raise\nx",
      "(1)\nx",      // right paren
      "[1]\nx",      // right bracket
      "{1}\nx",      // right brace (paired '{' '}' in plain code)
      "f\"{a}\"\nx", // f-string end
      "i++\nx",      // ++
      "i--\nx",      // --
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(kEnderCases); ++i) {
    struct MsDiagList diags;
    struct MsToken tokens[16];
    size_t count = msTestLexAll(kEnderCases[i], tokens, 16, &diags);
    MS_ASSERT_EQ(0, msDiagListCount(&diags));
    bool found = false;
    for (size_t j = 0; j < count; ++j) {
      if (tokens[j].type == MS_TOKEN_IDENTIFIER && msTestLexemeEq(&tokens[j], "x")) {
        found = true;
        MS_ASSERT_TRUE(j > 0);
        if (j > 0) {
          MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, tokens[j - 1].type);
          MS_ASSERT_EQ(0, tokens[j - 1].length);
        }
      }
    }
    MS_ASSERT_TRUE(found);
    msDiagListDestroy(&diags);
  }
}

MS_TEST(Lexer, SemicolonNotAfterContinuers) {
  // Continuers of 01-lexical section 7: operators, ",", "(", "[", "{", ".",
  // ":", ":=", and the word operators (div included, same class as and/or)
  // never trigger insertion -- the token after the trigger is the next line's
  // identifier, not a semicolon.
  static const char* const kContinuerCases[] = {
      "a +\nx", "a ,\nx", "a (\nx", "a [\nx", "a {\nx", "a .\nx",
      "a :\nx", "a :=\nx", "a =\nx", "a ==\nx",
      "a div\nx", "a and\nx", "a or\nx", "a not\nx", "a is\nx", "a in\nx",
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(kContinuerCases); ++i) {
    struct MsDiagList diags;
    struct MsToken tokens[16];
    size_t count = msTestLexAll(kContinuerCases[i], tokens, 16, &diags);
    MS_ASSERT_EQ(0, msDiagListCount(&diags));
    bool found = false;
    for (size_t j = 0; j < count; ++j) {
      if (tokens[j].type == MS_TOKEN_IDENTIFIER && msTestLexemeEq(&tokens[j], "x")) {
        found = true;
        MS_ASSERT_TRUE(j > 0);
        if (j > 0) {
          MS_ASSERT_TRUE(tokens[j - 1].type != MS_TOKEN_SEMICOLON);
        }
      }
    }
    MS_ASSERT_TRUE(found);
    msDiagListDestroy(&diags);
  }
}

MS_TEST(Lexer, PeekAcrossInsertedSemicolon) {
  // A manually driven lexer on "x\ny": peeking the auto-inserted semicolon
  // twice must return the cached token without duplicating it or its diags,
  // and the following msLexerNext must pop exactly that semicolon.
  struct MsDiagList diags;
  msDiagListInit(&diags, "test.ms");
  const char* source = "x\ny";
  struct MsLexer lexer;
  msLexerInit(&lexer, source, strlen(source), "test.ms", &diags);
  struct MsToken token;
  MS_ASSERT_EQ(MS_OK, msLexerNext(&lexer, &token));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, token.type);
  MS_ASSERT_TRUE(msTestLexemeEq(&token, "x"));
  MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, msLexerPeek(&lexer));
  MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, msLexerPeek(&lexer));
  MS_ASSERT_EQ(MS_OK, msLexerNext(&lexer, &token));
  MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, token.type);
  MS_ASSERT_EQ(0, token.length);
  MS_ASSERT_EQ(MS_OK, msLexerNext(&lexer, &token));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, token.type);
  MS_ASSERT_TRUE(msTestLexemeEq(&token, "y"));
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  msLexerDestroy(&lexer);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, BareCrTriggersInsertion) {
  // A bare '\r' is E112 but still acts as a newline, so it triggers
  // semicolon insertion like any other line ending.
  struct MsDiagList diags;
  struct MsToken tokens[8];
  size_t count = msTestLexAll("a\rb", tokens, 8, &diags);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  MS_ASSERT_EQ(112, msDiagListAt(&diags, 0)->code);
  const MsTokenType expect[] = {
      MS_TOKEN_IDENTIFIER, MS_TOKEN_SEMICOLON,
      MS_TOKEN_IDENTIFIER, MS_TOKEN_SEMICOLON,
      MS_TOKEN_EOF,
  };
  MS_ASSERT_EQ(MS_ARRAY_LEN(expect), count);
  for (size_t i = 0; i < MS_ARRAY_LEN(expect); ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  msDiagListDestroy(&diags);
}

// A {type, lexeme} expectation for msTestExpectSequence.
struct MsTestTokenExpect {
  MsTokenType type;
  const char* lexeme;
};

// Lexes source and asserts zero diagnostics and exactly the expected
// {type, lexeme} sequence followed by MS_TOKEN_EOF.
static void msTestExpectSequence(const char* source, const struct MsTestTokenExpect* expect,
    size_t expectCount) {
  struct MsDiagList diags;
  struct MsToken tokens[MS_TEST_MAX_TOKENS];
  size_t count = msTestLexAll(source, tokens, MS_TEST_MAX_TOKENS, &diags);
  MS_ASSERT_EQ(0, msDiagListCount(&diags));
  MS_ASSERT_EQ(expectCount + 1, count);
  size_t check = count - 1 < expectCount ? count - 1 : expectCount;
  for (size_t i = 0; i < check; ++i) {
    MS_ASSERT_EQ(expect[i].type, tokens[i].type);
    MS_ASSERT_TRUE(msTestLexemeEq(&tokens[i], expect[i].lexeme));
  }
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[count - 1].type);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, FstringExpandsToTokenSequence) {
  // The spec example of 03-lexer "f-string tokenization". Text-segment STRING
  // lexemes are the raw text without quotes; FSTRING_FORMAT excludes ':'
  // and the closing '}'; FSTRING_START is "f\"", FSTRING_END is "\"".
  static const struct MsTestTokenExpect expect[] = {
      {MS_TOKEN_FSTRING_START, "f\""},
      {MS_TOKEN_STRING, "x = "},
      {MS_TOKEN_LEFT_BRACE, "{"},
      {MS_TOKEN_IDENTIFIER, "x"},
      {MS_TOKEN_PLUS, "+"},
      {MS_TOKEN_INT, "1"},
      {MS_TOKEN_FSTRING_FORMAT, "08d"},
      {MS_TOKEN_RIGHT_BRACE, "}"},
      {MS_TOKEN_STRING, ", "},
      {MS_TOKEN_LEFT_BRACE, "{"},
      {MS_TOKEN_IDENTIFIER, "name"},
      {MS_TOKEN_RIGHT_BRACE, "}"},
      {MS_TOKEN_STRING, "!"},
      {MS_TOKEN_FSTRING_END, "\""},
      {MS_TOKEN_SEMICOLON, ""},
  };
  msTestExpectSequence("f\"x = {x + 1:08d}, {name}!\"", expect, MS_ARRAY_LEN(expect));
}

MS_TEST(Lexer, FstringEmptyAndTextOnly) {
  {
    // f"" has no empty STRING segment between START and END.
    static const struct MsTestTokenExpect expect[] = {
        {MS_TOKEN_FSTRING_START, "f\""},
        {MS_TOKEN_FSTRING_END, "\""},
        {MS_TOKEN_SEMICOLON, ""},
    };
    msTestExpectSequence("f\"\"", expect, MS_ARRAY_LEN(expect));
  }
  {
    static const struct MsTestTokenExpect expect[] = {
        {MS_TOKEN_FSTRING_START, "f\""},
        {MS_TOKEN_STRING, "plain"},
        {MS_TOKEN_FSTRING_END, "\""},
        {MS_TOKEN_SEMICOLON, ""},
    };
    msTestExpectSequence("f\"plain\"", expect, MS_ARRAY_LEN(expect));
  }
}

MS_TEST(Lexer, FstringEscapedBraces) {
  // '{{' and '}}' stay raw in the text-segment lexeme.
  static const struct MsTestTokenExpect expect[] = {
      {MS_TOKEN_FSTRING_START, "f\""},
      {MS_TOKEN_STRING, "{{x}}"},
      {MS_TOKEN_FSTRING_END, "\""},
      {MS_TOKEN_SEMICOLON, ""},
  };
  msTestExpectSequence("f\"{{x}}\"", expect, MS_ARRAY_LEN(expect));

  // The dedicated f-string text decode path collapses '{{'->'{' and
  // '}}'->'}' while ordinary escapes still decode.
  char* out = NULL;
  size_t outLen = 0;
  MS_ASSERT_EQ(MS_OK, msLexerUnescapeFstringText("{{x}}", 5, &out, &outLen));
  MS_ASSERT_EQ(3, outLen);
  MS_ASSERT_TRUE(out != NULL);
  if (out != NULL) {
    MS_ASSERT_TRUE(memcmp(out, "{x}", 3) == 0);
  }
  msFree(out);
  out = NULL;
  MS_ASSERT_EQ(MS_OK, msLexerUnescapeFstringText("{{\\n}}", 6, &out, &outLen));
  MS_ASSERT_EQ(3, outLen);
  MS_ASSERT_TRUE(out != NULL);
  if (out != NULL) {
    MS_ASSERT_TRUE(memcmp(out, "{\n}", 3) == 0);
  }
  msFree(out);

  // Plain msLexerUnescape still passes braces through untouched.
  out = NULL;
  MS_ASSERT_EQ(MS_OK, msLexerUnescape("{{x}}", 5, &out, &outLen));
  MS_ASSERT_EQ(5, outLen);
  MS_ASSERT_TRUE(out != NULL);
  if (out != NULL) {
    MS_ASSERT_TRUE(memcmp(out, "{{x}}", 5) == 0);
  }
  msFree(out);
}

MS_TEST(Lexer, FstringNestedFormat) {
  // The format collector tracks nested '{' '}' pairs (Python-style nested
  // replacement fields); the lexeme excludes ':' and the closing '}'.
  static const struct MsTestTokenExpect expect[] = {
      {MS_TOKEN_FSTRING_START, "f\""},
      {MS_TOKEN_LEFT_BRACE, "{"},
      {MS_TOKEN_IDENTIFIER, "n"},
      {MS_TOKEN_FSTRING_FORMAT, "{w}d"},
      {MS_TOKEN_RIGHT_BRACE, "}"},
      {MS_TOKEN_FSTRING_END, "\""},
      {MS_TOKEN_SEMICOLON, ""},
  };
  msTestExpectSequence("f\"{n:{w}d}\"", expect, MS_ARRAY_LEN(expect));
}

MS_TEST(Lexer, FstringNestedFstring) {
  // An interpolation may contain another f-string; after the inner
  // FSTRING_END pops its frame, the outer interpolation (braceDepth 1)
  // resumes and its '}' returns the outer f-string to text mode.
  static const struct MsTestTokenExpect expect[] = {
      {MS_TOKEN_FSTRING_START, "f\""},
      {MS_TOKEN_STRING, "a"},
      {MS_TOKEN_LEFT_BRACE, "{"},
      {MS_TOKEN_FSTRING_START, "f\""},
      {MS_TOKEN_STRING, "b"},
      {MS_TOKEN_LEFT_BRACE, "{"},
      {MS_TOKEN_IDENTIFIER, "x"},
      {MS_TOKEN_RIGHT_BRACE, "}"},
      {MS_TOKEN_FSTRING_END, "\""},
      {MS_TOKEN_RIGHT_BRACE, "}"},
      {MS_TOKEN_STRING, "c"},
      {MS_TOKEN_FSTRING_END, "\""},
      {MS_TOKEN_SEMICOLON, ""},
  };
  msTestExpectSequence("f\"a{f\"b{x}\"}c\"", expect, MS_ARRAY_LEN(expect));
}

MS_TEST(Lexer, FstringDictBraceDepth) {
  // The '{' '}' of a dict literal inside an interpolation are tracked via
  // the frame's braceDepth, so the dict's ':' (depth 2) emits COLON instead
  // of opening a format segment, and the final '}' (depth -> 0) returns to
  // text mode. Plain strings are allowed inside interpolations.
  static const struct MsTestTokenExpect expect[] = {
      {MS_TOKEN_FSTRING_START, "f\""},
      {MS_TOKEN_LEFT_BRACE, "{"},
      {MS_TOKEN_LEFT_BRACE, "{"},
      {MS_TOKEN_STRING, "\"a\""},
      {MS_TOKEN_COLON, ":"},
      {MS_TOKEN_INT, "1"},
      {MS_TOKEN_RIGHT_BRACE, "}"},
      {MS_TOKEN_RIGHT_BRACE, "}"},
      {MS_TOKEN_FSTRING_END, "\""},
      {MS_TOKEN_SEMICOLON, ""},
  };
  msTestExpectSequence("f\"{ {\"a\": 1} }\"", expect, MS_ARRAY_LEN(expect));
}

MS_TEST(Lexer, FstringStringInInterpolation) {
  // Interpolation braces hold any expression, including plain string
  // literals with the same quote: an inner string's closing quote never
  // closes the outer f-string. Text-segment STRING lexemes are quote-free
  // bodies; interpolation strings keep their quotes.
  static const struct MsTestTokenExpect expect[] = {
      {MS_TOKEN_FSTRING_START, "f\""},
      {MS_TOKEN_STRING, "v="},
      {MS_TOKEN_LEFT_BRACE, "{"},
      {MS_TOKEN_STRING, "\"s\""},
      {MS_TOKEN_PLUS, "+"},
      {MS_TOKEN_STRING, "\"t\""},
      {MS_TOKEN_RIGHT_BRACE, "}"},
      {MS_TOKEN_FSTRING_END, "\""},
      {MS_TOKEN_SEMICOLON, ""},
  };
  msTestExpectSequence("f\"v={\"s\" + \"t\"}\"", expect, MS_ARRAY_LEN(expect));
}

MS_TEST(Lexer, FstringColonInsideBrackets) {
  // A ':' nested in brackets is not the format-spec opener (Python's rule):
  // slices and lambdas are valid interpolation expressions.
  {
    static const struct MsTestTokenExpect expect[] = {
        {MS_TOKEN_FSTRING_START, "f\""},
        {MS_TOKEN_LEFT_BRACE, "{"},
        {MS_TOKEN_IDENTIFIER, "a"},
        {MS_TOKEN_LEFT_BRACKET, "["},
        {MS_TOKEN_INT, "1"},
        {MS_TOKEN_COLON, ":"},
        {MS_TOKEN_INT, "2"},
        {MS_TOKEN_RIGHT_BRACKET, "]"},
        {MS_TOKEN_RIGHT_BRACE, "}"},
        {MS_TOKEN_FSTRING_END, "\""},
        {MS_TOKEN_SEMICOLON, ""},
    };
    msTestExpectSequence("f\"{a[1:2]}\"", expect, MS_ARRAY_LEN(expect));
  }
  {
    static const struct MsTestTokenExpect expect[] = {
        {MS_TOKEN_FSTRING_START, "f\""},
        {MS_TOKEN_LEFT_BRACE, "{"},
        {MS_TOKEN_LEFT_PAREN, "("},
        {MS_TOKEN_KW_LAMBDA, "lambda"},
        {MS_TOKEN_IDENTIFIER, "x"},
        {MS_TOKEN_COLON, ":"},
        {MS_TOKEN_IDENTIFIER, "x"},
        {MS_TOKEN_RIGHT_PAREN, ")"},
        {MS_TOKEN_RIGHT_BRACE, "}"},
        {MS_TOKEN_FSTRING_END, "\""},
        {MS_TOKEN_SEMICOLON, ""},
    };
    msTestExpectSequence("f\"{(lambda x: x)}\"", expect, MS_ARRAY_LEN(expect));
  }
}

MS_TEST(Lexer, FstringLoneBraceInTextIsE111) {
  // A lone '}' in f-string text has no pairing '{': E111, and the text
  // around it still scans as segments.
  struct MsDiagList diags;
  struct MsToken tokens[MS_TEST_MAX_TOKENS];
  size_t count = msTestLexAll("f\"a}b\"", tokens, MS_TEST_MAX_TOKENS, &diags);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  MS_ASSERT_EQ(111, msDiagListAt(&diags, 0)->code);
  const MsTokenType expect[] = {
      MS_TOKEN_FSTRING_START, MS_TOKEN_STRING, MS_TOKEN_INVALID,
      MS_TOKEN_STRING, MS_TOKEN_FSTRING_END, MS_TOKEN_SEMICOLON, MS_TOKEN_EOF,
  };
  MS_ASSERT_EQ(MS_ARRAY_LEN(expect), count);
  for (size_t i = 0; i < MS_ARRAY_LEN(expect); ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[1], "a"));
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[2], "}"));
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[3], "b"));
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, FstringFormatNewlineIsE110) {
  // A newline inside a format segment means the interpolation never closed:
  // E110. The quote after the newline then opens an ordinary string that
  // runs unterminated to end of input (E103).
  struct MsDiagList diags;
  struct MsToken tokens[MS_TEST_MAX_TOKENS];
  size_t count = msTestLexAll("f\"{x:08\n\"", tokens, MS_TEST_MAX_TOKENS, &diags);
  MS_ASSERT_EQ(2, msDiagListCount(&diags));
  MS_ASSERT_EQ(110, msDiagListAt(&diags, 0)->code);
  MS_ASSERT_EQ(103, msDiagListAt(&diags, 1)->code);
  MS_ASSERT_TRUE(count > 0);
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[count - 1].type);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, FstringTextBadEscapeIsE106) {
  // Text-segment escapes follow the plain-string rules: "\q" is E106 at the
  // backslash and the segment comes out INVALID.
  struct MsDiagList diags;
  struct MsToken tokens[MS_TEST_MAX_TOKENS];
  size_t count = msTestLexAll("f\"\\q\"", tokens, MS_TEST_MAX_TOKENS, &diags);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  MS_ASSERT_EQ(106, msDiagListAt(&diags, 0)->code);
  MS_ASSERT_EQ(1, msDiagListAt(&diags, 0)->line);
  MS_ASSERT_EQ(3, msDiagListAt(&diags, 0)->column);  // the backslash
  const MsTokenType expect[] = {
      MS_TOKEN_FSTRING_START, MS_TOKEN_INVALID, MS_TOKEN_FSTRING_END,
      MS_TOKEN_SEMICOLON, MS_TOKEN_EOF,
  };
  MS_ASSERT_EQ(MS_ARRAY_LEN(expect), count);
  for (size_t i = 0; i < MS_ARRAY_LEN(expect); ++i) {
    MS_ASSERT_EQ(expect[i], tokens[i].type);
  }
  MS_ASSERT_TRUE(msTestLexemeEq(&tokens[1], "\\q"));
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, BracesPairInPlainCode) {
  // '{' '}' pairs in plain code (blocks, dict/set literals) scan without
  // diagnostics; only a '}' with no pairing '{' anywhere is E111.
  {
    static const struct MsTestTokenExpect expect[] = {
        {MS_TOKEN_LEFT_BRACE, "{"},
        {MS_TOKEN_RIGHT_BRACE, "}"},
        {MS_TOKEN_SEMICOLON, ""},
    };
    msTestExpectSequence("{ }", expect, MS_ARRAY_LEN(expect));
  }
  {
    static const struct MsTestTokenExpect expect[] = {
        {MS_TOKEN_KW_IF, "if"},
        {MS_TOKEN_IDENTIFIER, "x"},
        {MS_TOKEN_LEFT_BRACE, "{"},
        {MS_TOKEN_IDENTIFIER, "y"},
        {MS_TOKEN_RIGHT_BRACE, "}"},
        {MS_TOKEN_SEMICOLON, ""},
    };
    msTestExpectSequence("if x { y }", expect, MS_ARRAY_LEN(expect));
  }
  {
    static const struct MsTestTokenExpect expect[] = {
        {MS_TOKEN_LEFT_BRACE, "{"},
        {MS_TOKEN_STRING, "\"a\""},
        {MS_TOKEN_COLON, ":"},
        {MS_TOKEN_INT, "1"},
        {MS_TOKEN_RIGHT_BRACE, "}"},
        {MS_TOKEN_SEMICOLON, ""},
    };
    msTestExpectSequence("{\"a\": 1}", expect, MS_ARRAY_LEN(expect));
  }
  {
    static const struct MsTestTokenExpect expect[] = {
        {MS_TOKEN_LEFT_BRACE, "{"},
        {MS_TOKEN_LEFT_BRACE, "{"},
        {MS_TOKEN_RIGHT_BRACE, "}"},
        {MS_TOKEN_RIGHT_BRACE, "}"},
        {MS_TOKEN_SEMICOLON, ""},
    };
    msTestExpectSequence("{ { } }", expect, MS_ARRAY_LEN(expect));
  }
  {
    // A truly bare '}' is E111 + INVALID.
    struct MsDiagList diags;
    struct MsToken tokens[8];
    msTestLexAll("}", tokens, 8, &diags);
    MS_ASSERT_EQ(1, msDiagListCount(&diags));
    MS_ASSERT_EQ(111, msDiagListAt(&diags, 0)->code);
    MS_ASSERT_EQ(MS_TOKEN_INVALID, tokens[0].type);
    msDiagListDestroy(&diags);
  }
}

// Builds "f\"{f\"{...x...}\"}\"" with depth nested f-strings into out
// (which must hold at least depth * 5 + 1 bytes) and returns the length.
static size_t msTestBuildNestedFstrings(char* out, int depth) {
  size_t n = 0;
  for (int i = 0; i < depth; ++i) {
    memcpy(out + n, "f\"{", 3);
    n += 3;
  }
  out[n++] = 'x';
  for (int i = 0; i < depth; ++i) {
    memcpy(out + n, "}\"", 2);
    n += 2;
  }
  out[n] = '\0';
  return n;
}

MS_TEST(Lexer, FstringErrors) {
  // (a) Nesting depth: MS_LEXER_MAX_FSTRING_DEPTH (8) nested f-strings are
  // fine; the 9th push overflows and reports E109, then recovery scans to
  // the closing quote as a plain string without further diagnostics.
  char nested[128];
  {
    msTestBuildNestedFstrings(nested, 8);
    struct MsDiagList diags;
    struct MsToken tokens[MS_TEST_MAX_TOKENS];
    msTestLexAll(nested, tokens, MS_TEST_MAX_TOKENS, &diags);
    MS_ASSERT_EQ(0, msDiagListCount(&diags));
    msDiagListDestroy(&diags);
  }
  {
    msTestBuildNestedFstrings(nested, 9);
    struct MsDiagList diags;
    struct MsToken tokens[MS_TEST_MAX_TOKENS];
    size_t count = msTestLexAll(nested, tokens, MS_TEST_MAX_TOKENS, &diags);
    MS_ASSERT_EQ(1, msDiagListCount(&diags));
    MS_ASSERT_EQ(109, msDiagListAt(&diags, 0)->code);
    // Recovery scans the 9th f-string to its closing quote as a plain
    // string; the 8 outer frames then close cleanly: START/LBRACE pairs,
    // the INVALID recovery token, RBRACE/END pairs, semicolon, EOF.
    MS_ASSERT_EQ(35, count);
    for (size_t i = 0; i < 8; ++i) {
      MS_ASSERT_EQ(MS_TOKEN_FSTRING_START, tokens[i * 2].type);
      MS_ASSERT_EQ(MS_TOKEN_LEFT_BRACE, tokens[i * 2 + 1].type);
    }
    MS_ASSERT_EQ(MS_TOKEN_INVALID, tokens[16].type);
    MS_ASSERT_TRUE(msTestLexemeEq(&tokens[16], "f\"{x}\""));
    for (size_t i = 0; i < 8; ++i) {
      MS_ASSERT_EQ(MS_TOKEN_RIGHT_BRACE, tokens[17 + i * 2].type);
      MS_ASSERT_EQ(MS_TOKEN_FSTRING_END, tokens[17 + i * 2 + 1].type);
    }
    MS_ASSERT_EQ(MS_TOKEN_SEMICOLON, tokens[33].type);
    MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[34].type);
    msDiagListDestroy(&diags);
  }
  // (b) f"{x" : plain strings are allowed inside interpolations, so the
  // quote starts a plain string that runs unterminated to end of input
  // (E103), and the interpolation's '{' is then unpaired at end of input
  // (E110).
  {
    struct MsDiagList diags;
    struct MsToken tokens[16];
    size_t count = msTestLexAll("f\"{x\"", tokens, 16, &diags);
    MS_ASSERT_EQ(2, msDiagListCount(&diags));
    MS_ASSERT_EQ(103, msDiagListAt(&diags, 0)->code);
    MS_ASSERT_EQ(110, msDiagListAt(&diags, 1)->code);
    MS_ASSERT_TRUE(count > 0);
    MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[count - 1].type);
    msDiagListDestroy(&diags);
  }
  // (c) A bare '}' in normal code is unpaired -> E111 + INVALID.
  {
    struct MsDiagList diags;
    struct MsToken tokens[8];
    size_t count = msTestLexAll("}", tokens, 8, &diags);
    MS_ASSERT_EQ(1, msDiagListCount(&diags));
    MS_ASSERT_EQ(111, msDiagListAt(&diags, 0)->code);
    MS_ASSERT_TRUE(count >= 1);
    MS_ASSERT_EQ(MS_TOKEN_INVALID, tokens[0].type);
    msDiagListDestroy(&diags);
  }
  // (d) f"abc : end of input inside a text segment -> E103.
  {
    struct MsDiagList diags;
    struct MsToken tokens[8];
    size_t count = msTestLexAll("f\"abc", tokens, 8, &diags);
    MS_ASSERT_EQ(1, msDiagListCount(&diags));
    MS_ASSERT_EQ(103, msDiagListAt(&diags, 0)->code);
    MS_ASSERT_TRUE(count > 0);
    MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[count - 1].type);
    msDiagListDestroy(&diags);
  }
}

MS_TEST(Lexer, MultipleErrorsRecordedAndScanningContinues) {
  struct MsDiagList diags;
  struct MsToken tokens[16];
  size_t count = msTestLexAll("! @ #", tokens, 16, &diags);
  MS_ASSERT_EQ(3, msDiagListCount(&diags));
  MS_ASSERT_EQ(102, msDiagListAt(&diags, 0)->code);
  int invalids = 0;
  for (size_t i = 0; i < count; ++i) {
    if (tokens[i].type == MS_TOKEN_INVALID) {
      ++invalids;
    }
  }
  MS_ASSERT_EQ(3, invalids);
  MS_ASSERT_EQ(MS_TOKEN_EOF, tokens[count - 1].type);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, DiagnosticCapAbortsWithSyntaxError) {
  // 21 illegal chars -> exactly 20 diagnostics; then forced EOF.
  const char* source = "! ! ! ! ! ! ! ! ! ! ! ! ! ! ! ! ! ! ! ! !";
  struct MsDiagList diags;
  msDiagListInit(&diags, "test.ms");
  struct MsLexer lexer;
  msLexerInit(&lexer, source, strlen(source), "test.ms", &diags);
  struct MsToken token;
  MsResult result = MS_OK;
  size_t guard = 0;
  do {
    result = msLexerNext(&lexer, &token);
    ++guard;
  } while (result == MS_OK && token.type != MS_TOKEN_EOF && guard < 128);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, result);
  MS_ASSERT_EQ(MS_TOKEN_EOF, token.type);
  MS_ASSERT_EQ(20, msDiagListCount(&diags));
  // Every later call stays at EOF without new diagnostics.
  result = msLexerNext(&lexer, &token);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, result);
  MS_ASSERT_EQ(MS_TOKEN_EOF, token.type);
  MS_ASSERT_EQ(20, msDiagListCount(&diags));
  // Peeking at the cap reports the cached EOF, again without diagnostics.
  MS_ASSERT_EQ(MS_TOKEN_EOF, msLexerPeek(&lexer));
  MS_ASSERT_EQ(20, msDiagListCount(&diags));
  msLexerDestroy(&lexer);
  msDiagListDestroy(&diags);
}

MS_TEST(Lexer, PeekCachesAndNeverDoubleReports) {
  struct MsDiagList diags;
  msDiagListInit(&diags, "test.ms");
  const char* source = "! x";
  struct MsLexer lexer;
  msLexerInit(&lexer, source, strlen(source), "test.ms", &diags);
  MS_ASSERT_EQ(MS_TOKEN_INVALID, msLexerPeek(&lexer));
  MS_ASSERT_EQ(MS_TOKEN_INVALID, msLexerPeek(&lexer));
  MS_ASSERT_EQ(1, msDiagListCount(&diags));  // no duplicate diagnostics
  struct MsToken token;
  MS_ASSERT_EQ(MS_OK, msLexerNext(&lexer, &token));
  MS_ASSERT_EQ(MS_TOKEN_INVALID, token.type);
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, msLexerPeek(&lexer));
  MS_ASSERT_EQ(MS_OK, msLexerNext(&lexer, &token));
  MS_ASSERT_EQ(MS_TOKEN_IDENTIFIER, token.type);
  MS_ASSERT_EQ(1, msDiagListCount(&diags));
  msLexerDestroy(&lexer);
  msDiagListDestroy(&diags);
}

static const MsTestCase msTests[] = {
    {"Lexer.TokenTypeNameCoversEveryValue", testLexerTokenTypeNameCoversEveryValue},
    {"Lexer.TokenTypeNameSpots", testLexerTokenTypeNameSpots},
    {"Lexer.EmptySourceProducesEof", testLexerEmptySourceProducesEof},
    {"Lexer.BomIsSkippedAndReported", testLexerBomIsSkippedAndReported},
    {"Lexer.CrlfAndLfProduceSameTokens", testLexerCrlfAndLfProduceSameTokens},
    {"Lexer.BareCarriageReturnIsE112", testLexerBareCarriageReturnIsE112},
    {"Lexer.CommentOnlySourceProducesEof", testLexerCommentOnlySourceProducesEof},
    {"Lexer.TokensAroundComments", testLexerTokensAroundComments},
    {"Lexer.UnclosedBlockCommentIsE105", testLexerUnclosedBlockCommentIsE105},
    {"Lexer.BlockCommentsDoNotNest", testLexerBlockCommentsDoNotNest},
    {"Lexer.AllKeywordsScan", testLexerAllKeywordsScan},
    {"Lexer.BuiltinsAndCaseVariantsAreIdentifiers", testLexerBuiltinsAndCaseVariantsAreIdentifiers},
    {"Lexer.NonAsciiIdentifierAndInvalidUtf8", testLexerNonAsciiIdentifierAndInvalidUtf8},
    {"Lexer.NumberLiteralsScan", testLexerNumberLiteralsScan},
    {"Lexer.NumberLiteralsReportErrors", testLexerNumberLiteralsReportErrors},
    {"Lexer.NumberErrorPositionPinned", testLexerNumberErrorPositionPinned},
    {"Lexer.DotFiveVersusMemberAccess", testLexerDotFiveVersusMemberAccess},
    {"Lexer.StringLiteralsScan", testLexerStringLiteralsScan},
    {"Lexer.UnescapeDecodesEscapes", testLexerUnescapeDecodesEscapes},
    {"Lexer.StringErrors", testLexerStringErrors},
    {"Lexer.StringErrorPositionPinned", testLexerStringErrorPositionPinned},
    {"Lexer.UnescapeLeakFree", testLexerUnescapeLeakFree},
    {"Lexer.UnescapeOom", testLexerUnescapeOom},
    {"Lexer.StringRoundTrip", testLexerStringRoundTrip},
    {"Lexer.OperatorsScan", testLexerOperatorsScan},
    {"Lexer.OperatorsMaximalMunch", testLexerOperatorsMaximalMunch},
    {"Lexer.SlashSlashIsAlwaysComment", testLexerSlashSlashIsAlwaysComment},
    {"Lexer.BareBangIsE102", testLexerBareBangIsE102},
    {"Lexer.DivKeywordScans", testLexerDivKeywordScans},
    {"Lexer.UnexpectedByteIsE102", testLexerUnexpectedByteIsE102},
    {"Lexer.SemicolonInsertedAfterStatementEnders", testLexerSemicolonInsertedAfterStatementEnders},
    {"Lexer.SemicolonInsertedAtEof", testLexerSemicolonInsertedAtEof},
    {"Lexer.NoSemicolonAfterContinuationTokens", testLexerNoSemicolonAfterContinuationTokens},
    {"Lexer.ExplicitAndRepeatedSemicolonsPassThrough", testLexerExplicitAndRepeatedSemicolonsPassThrough},
    {"Lexer.NewlineInsideBlockCommentTriggersInsertion", testLexerNewlineInsideBlockCommentTriggersInsertion},
    {"Lexer.NewlineAfterLineCommentTriggersInsertion", testLexerNewlineAfterLineCommentTriggersInsertion},
    {"Lexer.BlankLinesProduceNoExtraSemicolons", testLexerBlankLinesProduceNoExtraSemicolons},
    {"Lexer.SemicolonTriggerSetCoversAllEnders", testLexerSemicolonTriggerSetCoversAllEnders},
    {"Lexer.SemicolonNotAfterContinuers", testLexerSemicolonNotAfterContinuers},
    {"Lexer.PeekAcrossInsertedSemicolon", testLexerPeekAcrossInsertedSemicolon},
    {"Lexer.BareCrTriggersInsertion", testLexerBareCrTriggersInsertion},
    {"Lexer.FstringExpandsToTokenSequence", testLexerFstringExpandsToTokenSequence},
    {"Lexer.FstringEmptyAndTextOnly", testLexerFstringEmptyAndTextOnly},
    {"Lexer.FstringEscapedBraces", testLexerFstringEscapedBraces},
    {"Lexer.FstringNestedFormat", testLexerFstringNestedFormat},
    {"Lexer.FstringNestedFstring", testLexerFstringNestedFstring},
    {"Lexer.FstringDictBraceDepth", testLexerFstringDictBraceDepth},
    {"Lexer.FstringStringInInterpolation", testLexerFstringStringInInterpolation},
    {"Lexer.FstringColonInsideBrackets", testLexerFstringColonInsideBrackets},
    {"Lexer.FstringLoneBraceInTextIsE111", testLexerFstringLoneBraceInTextIsE111},
    {"Lexer.FstringFormatNewlineIsE110", testLexerFstringFormatNewlineIsE110},
    {"Lexer.FstringTextBadEscapeIsE106", testLexerFstringTextBadEscapeIsE106},
    {"Lexer.FstringErrors", testLexerFstringErrors},
    {"Lexer.BracesPairInPlainCode", testLexerBracesPairInPlainCode},
    {"Lexer.MultipleErrorsRecordedAndScanningContinues", testLexerMultipleErrorsRecordedAndScanningContinues},
    {"Lexer.DiagnosticCapAbortsWithSyntaxError", testLexerDiagnosticCapAbortsWithSyntaxError},
    {"Lexer.PeekCachesAndNeverDoubleReports", testLexerPeekCachesAndNeverDoubleReports},
};
MS_TEST_MAIN(msTests)
