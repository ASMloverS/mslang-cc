#include <string.h>

#include "core/ms_common.h"
#include "core/ms_diag.h"
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

MS_TEST(Lexer, TokenTypeNameCoversEveryValue) {
  for (int type = MS_TOKEN_EOF; type <= MS_TOKEN_ELLIPSIS; ++type) {
    const char* name = msTokenTypeName((MsTokenType)type);
    MS_ASSERT_TRUE(name != NULL);
    MS_ASSERT_TRUE(strncmp(name, "MS_TOKEN_", 9) == 0);
  }
}

MS_TEST(Lexer, TokenTypeNameSpots) {
  // Harness helpers not yet exercised by real tests; keep the references so
  // -Wunused-function stays silent until later tasks use them.
  (void)msTestLexemeEqN;
  (void)MS_TEST_MAX_TOKENS;

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
  MS_ASSERT_EQ(2, tokensCrlf[1].line);  // b lands on line 2
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
  bool sawInvalid = false;
  for (size_t i = 0; i < count; ++i) {
    if (tokensBad[i].type == MS_TOKEN_INVALID) {
      sawInvalid = true;
    }
  }
  MS_ASSERT_TRUE(sawInvalid);
  msDiagListDestroy(&diagsBad);
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
};
MS_TEST_MAIN(msTests)
