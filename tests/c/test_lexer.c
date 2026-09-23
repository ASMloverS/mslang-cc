#include <string.h>

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
  MS_ASSERT_EQ(2, tokensCrlf[2].line);  // b lands on line 2
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

static const MsTestCase msTests[] = {
    {"Lexer.TokenTypeNameCoversEveryValue", testLexerTokenTypeNameCoversEveryValue},
    {"Lexer.TokenTypeNameSpots", testLexerTokenTypeNameSpots},
    {"Lexer.EmptySourceProducesEof", testLexerEmptySourceProducesEof},
    {"Lexer.BomIsSkippedAndReported", testLexerBomIsSkippedAndReported},
    {"Lexer.CrlfAndLfProduceSameTokens", testLexerCrlfAndLfProduceSameTokens},
    {"Lexer.BareCarriageReturnIsE112", testLexerBareCarriageReturnIsE112},
};
MS_TEST_MAIN(msTests)
