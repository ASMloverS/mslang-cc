#include <stdio.h>
#include <string.h>

#include "core/ms_diag.h"
#include "core/ms_memory.h"
#include "lexer/ms_lexer.h"
#include "parser/ms_ast.h"
#include "parser/ms_parser.h"
#include "ms_test.h"

#define MS_TEST_DUMP_CAP 65536

typedef struct {
  struct MsDiagList diags;
  struct MsAstArena arena;
  struct MsLexer lexer;
  struct MsParser parser;
  struct MsAst* root;
  MsResult result;
} MsTestFixture;

// Parses source end-to-end into fix. Call msTestFixtureDestroy afterwards.
static void msTestParse(MsTestFixture* fix, const char* source) {
  msDiagListInit(&fix->diags, "test.ms");
  msAstArenaInit(&fix->arena);
  msLexerInit(&fix->lexer, source, strlen(source), "test.ms", &fix->diags);
  msParserInit(&fix->parser, &fix->lexer, &fix->arena, &fix->diags);
  fix->root = NULL;
  fix->result = msParserParse(&fix->parser, &fix->root);
  msLexerDestroy(&fix->lexer);
}

static void msTestFixtureDestroy(MsTestFixture* fix) {
  msAstArenaDestroy(&fix->arena);
  msDiagListDestroy(&fix->diags);
}

// Dumps root into buf (NUL-terminated).
static void msTestDump(const struct MsAst* root, char* buf, size_t bufSize) {
  FILE* out = tmpfile();
  MS_ASSERT_TRUE(out != NULL);
  msAstDump(root, out);
  rewind(out);
  size_t n = fread(buf, 1, bufSize - 1, out);
  buf[n] = '\0';
  fclose(out);
}

// Asserts: parse succeeds, zero diagnostics, dump matches expected exactly.
static void msTestExpectDump(const char* source, const char* expected) {
  MsTestFixture fix;
  msTestParse(&fix, source);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(0, (long long)msDiagListCount(&fix.diags));
  char buf[MS_TEST_DUMP_CAP];
  msTestDump(fix.root, buf, sizeof(buf));
  if (strcmp(buf, expected) != 0) {
    printf("--- expected ---\n%s--- actual ---\n%s---\n", expected, buf);
    MS_ASSERT_TRUE(false);
  }
  msTestFixtureDestroy(&fix);
}

// Asserts: parse fails with MS_ERROR_SYNTAX and the diagnostic codes in
// order equal expectedCodes[0..expectedCount).
static void msTestExpectDiags(const char* source, const uint32_t* expectedCodes,
    size_t expectedCount) {
  MsTestFixture fix;
  msTestParse(&fix, source);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ((long long)expectedCount, (long long)msDiagListCount(&fix.diags));
  size_t n = msDiagListCount(&fix.diags);
  if (n > expectedCount) {
    n = expectedCount;
  }
  for (size_t i = 0; i < n; ++i) {
    MS_ASSERT_EQ((long long)expectedCodes[i],
        (long long)msDiagListAt(&fix.diags, i)->code);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(parser, kindNameTable) {
  // Every enum value has a distinct non-NULL name starting with "MS_AST_".
  for (int kind = MS_AST_INVALID; kind <= MS_AST_CLASS_DECL; ++kind) {
    const char* name = msAstKindName((MsAstKind)kind);
    MS_ASSERT_TRUE(name != NULL);
    MS_ASSERT_TRUE(strncmp(name, "MS_AST_", 7) == 0);
  }
  MS_ASSERT_EQ(0, strcmp(msAstKindName(MS_AST_SOURCE_FILE), "MS_AST_SOURCE_FILE"));
  MS_ASSERT_EQ(0, strcmp(msAstKindName(MS_AST_CLASS_DECL), "MS_AST_CLASS_DECL"));
}

MS_TEST(parser, arenaMultiBlock) {
  msMemResetStats();
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  // Far more nodes than one 4096-byte block holds: forces block growth.
  struct MsAst* last = NULL;
  for (int i = 0; i < 400; ++i) {
    last = msAstNew(&arena, MS_AST_INT, (uint32_t)i + 1, 1);
    MS_ASSERT_TRUE(last != NULL);
  }
  MS_ASSERT_TRUE(arena.totalAllocated >= 2 * MS_AST_ARENA_BLOCK_SIZE);
  MS_ASSERT_EQ(MS_AST_INT, last->kind);
  MS_ASSERT_EQ(400, (long long)last->line);
  // Zeroed allocation: the union starts zeroed.
  MS_ASSERT_TRUE(last->as.literal.start == NULL);
  msAstArenaDestroy(&arena);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(parser, arenaListPushGrowth) {
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  struct MsAstList list = { NULL, 0, 0 };
  for (int i = 0; i < 100; ++i) {
    struct MsAst* node = msAstNew(&arena, MS_AST_NIL, 1, 1);
    MS_ASSERT_TRUE(msAstListPush(&arena, &list, node));
    MS_ASSERT_TRUE(list.items[i] == node);
  }
  MS_ASSERT_EQ(100, (long long)list.count);
  MS_ASSERT_TRUE(list.capacity >= 100);
  msAstArenaDestroy(&arena);
}

MS_TEST(parser, arenaOom) {
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  msMemSetFailAfter(0);
  MS_ASSERT_TRUE(msAstNew(&arena, MS_AST_INT, 1, 1) == NULL);
  struct MsAstList list = { NULL, 0, 0 };
  MS_ASSERT_TRUE(!msAstListPush(&arena, &list, NULL));
  msMemSetFailAfter(-1);
  msAstArenaDestroy(&arena);
}

// Builds: (sourceFile (exprStmt (binary + (int "1") (binary * (int "2") (int "3")))))
MS_TEST(parser, dumpHandBuiltTree) {
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  struct MsAst* one = msAstNew(&arena, MS_AST_INT, 1, 1);
  one->as.literal.start = "1";
  one->as.literal.length = 1;
  struct MsAst* two = msAstNew(&arena, MS_AST_INT, 1, 3);
  two->as.literal.start = "2";
  two->as.literal.length = 1;
  struct MsAst* three = msAstNew(&arena, MS_AST_INT, 1, 5);
  three->as.literal.start = "3";
  three->as.literal.length = 1;
  struct MsAst* mul = msAstNew(&arena, MS_AST_BINARY, 1, 4);
  mul->as.binary.op = MS_TOKEN_STAR;
  mul->as.binary.left = two;
  mul->as.binary.right = three;
  struct MsAst* add = msAstNew(&arena, MS_AST_BINARY, 1, 2);
  add->as.binary.op = MS_TOKEN_PLUS;
  add->as.binary.left = one;
  add->as.binary.right = mul;
  struct MsAst* stmt = msAstNew(&arena, MS_AST_EXPR_STMT, 1, 1);
  stmt->as.exprStmt.expr = add;
  struct MsAst* root = msAstNew(&arena, MS_AST_SOURCE_FILE, 1, 1);
  MS_ASSERT_TRUE(msAstListPush(&arena, &root->as.block, stmt));

  FILE* out = tmpfile();
  MS_ASSERT_TRUE(out != NULL);
  msAstDump(root, out);
  rewind(out);
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, out);
  buf[n] = '\0';
  fclose(out);
  const char* expected =
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_INT \"2\"\n"
      "        MS_AST_INT \"3\"\n";
  if (strcmp(buf, expected) != 0) {
    printf("--- expected ---\n%s--- actual ---\n%s---\n", expected, buf);
    MS_ASSERT_TRUE(false);
  }
  msAstArenaDestroy(&arena);
}

MS_TEST(parser, dumpEscapesLexemes) {
  struct MsAstArena arena;
  msAstArenaInit(&arena);
  struct MsAst* str = msAstNew(&arena, MS_AST_STRING, 1, 1);
  str->as.literal.start = "a\\tb\"c\nd\re\tf";   // raw bytes: a \ t b " c LF d CR e TAB f
  str->as.literal.length = 12;
  FILE* out = tmpfile();
  MS_ASSERT_TRUE(out != NULL);
  msAstDump(str, out);
  rewind(out);
  char buf[256];
  size_t n = fread(buf, 1, sizeof(buf) - 1, out);
  buf[n] = '\0';
  fclose(out);
  MS_ASSERT_EQ(0, strcmp(buf, "MS_AST_STRING \"a\\\\tb\\\"c\\nd\\re\\tf\"\n"));
  msAstArenaDestroy(&arena);
}

MS_TEST(parser, emptySource) {
  msTestExpectDump("", "MS_AST_SOURCE_FILE\n");
}

MS_TEST(parser, literalExprStmt) {
  msTestExpectDump("42",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"42\"\n");
}

MS_TEST(parser, twoStatementsTwoLines) {
  msTestExpectDump("1\n2",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"1\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"2\"\n");
}

MS_TEST(parser, explicitSemicolons) {
  msTestExpectDump("1; 2;",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"1\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_INT \"2\"\n");
}

MS_TEST(parser, emptyStatement) {
  static const uint32_t codes[] = { 204 };
  msTestExpectDiags(";", codes, 1);
}

MS_TEST(parser, missingSemicolon) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("1 2", codes, 1);   // auto-insertion only at line end
}

MS_TEST(parser, nodePositions) {
  MsTestFixture fix;
  msTestParse(&fix, "x\n  y");
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(2, (long long)fix.root->as.block.count);
  struct MsAst* first = fix.root->as.block.items[0];
  struct MsAst* second = fix.root->as.block.items[1];
  MS_ASSERT_EQ(1, (long long)first->line);
  MS_ASSERT_EQ(1, (long long)first->column);
  MS_ASSERT_EQ(2, (long long)second->line);
  MS_ASSERT_EQ(3, (long long)second->column);
  msTestFixtureDestroy(&fix);
}

MS_TEST(parser, precedenceTermFactor) {
  msTestExpectDump("1 + 2 * 3",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_INT \"2\"\n"
      "        MS_AST_INT \"3\"\n");
}

MS_TEST(parser, powerRightAssoc) {
  msTestExpectDump("2 ** 3 ** 4",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_DOUBLE_STAR\n"
      "      MS_AST_INT \"2\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_DOUBLE_STAR\n"
      "        MS_AST_INT \"3\"\n"
      "        MS_AST_INT \"4\"\n");
}

MS_TEST(parser, unaryBindsLooserThanPower) {
  // -2 ** 2 == -(2 ** 2): unary is level 4, ** is level 3 (tighter).
  msTestExpectDump("-2 ** 2",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_UNARY op=MS_TOKEN_MINUS\n"
      "      MS_AST_BINARY op=MS_TOKEN_DOUBLE_STAR\n"
      "        MS_AST_INT \"2\"\n"
      "        MS_AST_INT \"2\"\n");
}

MS_TEST(parser, unaryBindsTighterThanFactor) {
  // -2 * 3 == (-2) * 3.
  msTestExpectDump("-2 * 3",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "      MS_AST_UNARY op=MS_TOKEN_MINUS\n"
      "        MS_AST_INT \"2\"\n"
      "      MS_AST_INT \"3\"\n");
}

MS_TEST(parser, logicalPrecedence) {
  // not a and b or c == ((not a) and b) or c
  msTestExpectDump("not a and b or c",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LOGICAL op=MS_TOKEN_KW_OR\n"
      "      MS_AST_LOGICAL op=MS_TOKEN_KW_AND\n"
      "        MS_AST_UNARY op=MS_TOKEN_KW_NOT\n"
      "          MS_AST_IDENT \"a\"\n"
      "        MS_AST_IDENT \"b\"\n"
      "      MS_AST_IDENT \"c\"\n");
}

MS_TEST(parser, bitwiseLevelChain) {
  msTestExpectDump("a | b ^ c & d << 1",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PIPE\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_CARET\n"
      "        MS_AST_IDENT \"b\"\n"
      "        MS_AST_BINARY op=MS_TOKEN_AMP\n"
      "          MS_AST_IDENT \"c\"\n"
      "          MS_AST_BINARY op=MS_TOKEN_SHIFT_LEFT\n"
      "            MS_AST_IDENT \"d\"\n"
      "            MS_AST_INT \"1\"\n");
}

MS_TEST(parser, floorDivKeyword) {
  msTestExpectDump("a div b",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_KW_DIV\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, parensOverride) {
  msTestExpectDump("(1 + 2) * 3",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "      MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "        MS_AST_INT \"1\"\n"
      "        MS_AST_INT \"2\"\n"
      "      MS_AST_INT \"3\"\n");
}

MS_TEST(parser, groupingOnInfixRight) {
  // Exercises the infixLeft stash through parseGrouping on the right side of
  // an infix: the nested fold loop must not clobber the outer left operand.
  msTestExpectDump("1 + (2 * 3)",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_INT \"2\"\n"
      "        MS_AST_INT \"3\"\n");
}

MS_TEST(parser, unaryPlusAndTilde) {
  msTestExpectDump("+x\n~x",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_UNARY op=MS_TOKEN_PLUS\n"
      "      MS_AST_IDENT \"x\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_UNARY op=MS_TOKEN_TILDE\n"
      "      MS_AST_IDENT \"x\"\n");
}

MS_TEST(parser, trailingOperatorIsE201) {
  static const uint32_t codes[] = { 201 };
  msTestExpectDiags("1 +", codes, 1);   // the expression ends at EOF
}

MS_TEST(parser, literalKinds) {
  // The lexer keeps quotes/backticks/prefix in STRING/RAW_STRING/BYTES
  // lexemes (see Lexer.StringLiteralsScan), so the dumps show them too.
  msTestExpectDump("1.5\n`s raw `\nb\"xy\"\ntrue\nfalse\nnil",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FLOAT \"1.5\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_RAW_STRING \"`s raw `\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BYTES \"b\\\"xy\\\"\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BOOL true\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BOOL false\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_NIL\n");
}

static const MsTestCase msTests[] = {
    {"parser.kindNameTable", testparserkindNameTable},
    {"parser.arenaMultiBlock", testparserarenaMultiBlock},
    {"parser.arenaListPushGrowth", testparserarenaListPushGrowth},
    {"parser.arenaOom", testparserarenaOom},
    {"parser.dumpHandBuiltTree", testparserdumpHandBuiltTree},
    {"parser.dumpEscapesLexemes", testparserdumpEscapesLexemes},
    {"parser.emptySource", testparseremptySource},
    {"parser.literalExprStmt", testparserliteralExprStmt},
    {"parser.twoStatementsTwoLines", testparsertwoStatementsTwoLines},
    {"parser.explicitSemicolons", testparserexplicitSemicolons},
    {"parser.emptyStatement", testparseremptyStatement},
    {"parser.missingSemicolon", testparsermissingSemicolon},
    {"parser.nodePositions", testparsernodePositions},
    {"parser.precedenceTermFactor", testparserprecedenceTermFactor},
    {"parser.powerRightAssoc", testparserpowerRightAssoc},
    {"parser.unaryBindsLooserThanPower", testparserunaryBindsLooserThanPower},
    {"parser.unaryBindsTighterThanFactor", testparserunaryBindsTighterThanFactor},
    {"parser.logicalPrecedence", testparserlogicalPrecedence},
    {"parser.bitwiseLevelChain", testparserbitwiseLevelChain},
    {"parser.floorDivKeyword", testparserfloorDivKeyword},
    {"parser.parensOverride", testparserparensOverride},
    {"parser.literalKinds", testparserliteralKinds},
    {"parser.groupingOnInfixRight", testparsergroupingOnInfixRight},
    {"parser.unaryPlusAndTilde", testparserunaryPlusAndTilde},
    {"parser.trailingOperatorIsE201", testparsertrailingOperatorIsE201},
};
MS_TEST_MAIN(msTests)
