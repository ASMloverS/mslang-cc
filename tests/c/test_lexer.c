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
  // The shared lexing harness above is exercised by later tasks; reference it
  // here so -Wunused-function stays silent in the meantime.
  (void)msTestLexAll;
  (void)msTestLexemeEq;
  (void)msTestLexemeEqN;
  (void)MS_TEST_MAX_TOKENS;

  MS_ASSERT_TRUE(strcmp(msTokenTypeName(MS_TOKEN_EOF), "MS_TOKEN_EOF") == 0);
  MS_ASSERT_TRUE(strcmp(msTokenTypeName(MS_TOKEN_KW_IF), "MS_TOKEN_KW_IF") == 0);
  MS_ASSERT_TRUE(strcmp(msTokenTypeName(MS_TOKEN_SHIFT_LEFT_EQUAL), "MS_TOKEN_SHIFT_LEFT_EQUAL") == 0);
  MS_ASSERT_TRUE(strcmp(msTokenTypeName(MS_TOKEN_ELLIPSIS), "MS_TOKEN_ELLIPSIS") == 0);
}

static const MsTestCase msTests[] = {
    {"Lexer.TokenTypeNameCoversEveryValue", testLexerTokenTypeNameCoversEveryValue},
    {"Lexer.TokenTypeNameSpots", testLexerTokenTypeNameSpots},
};
MS_TEST_MAIN(msTests)
