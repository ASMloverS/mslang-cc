#include <stdio.h>
#include <string.h>

#include "core/ms_common.h"
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

MS_TEST(parser, postfixMixedChain) {
  // m.a[0](x).b
  msTestExpectDump("m.a[0](x).b",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_ATTR \"b\"\n"
      "      MS_AST_CALL\n"
      "        MS_AST_INDEX\n"
      "          MS_AST_ATTR \"a\"\n"
      "            MS_AST_IDENT \"m\"\n"
      "          MS_AST_INT \"0\"\n"
      "        MS_AST_IDENT \"x\"\n");
}

MS_TEST(parser, callKeywordArgs) {
  msTestExpectDump("f(1, x = 2)",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"f\"\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_KW_ARG \"x\"\n"
      "        MS_AST_INT \"2\"\n");
}

MS_TEST(parser, callTrailingCommaAndEmpty) {
  msTestExpectDump("f()\ng(a, b, )",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"f\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"g\"\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, sliceCombinations) {
  msTestExpectDump("a[:]\nb[::2]\nc[1:]\nd[1:2:3]\ne[1:2]",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=\n"
      "      MS_AST_IDENT \"a\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=step\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=start\n"
      "      MS_AST_IDENT \"c\"\n"
      "      MS_AST_INT \"1\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=start,stop,step\n"
      "      MS_AST_IDENT \"d\"\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "      MS_AST_INT \"3\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SLICE parts=start,stop\n"
      "      MS_AST_IDENT \"e\"\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n");
}

MS_TEST(parser, emptySubscriptIsError) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("a[]", codes, 1);
}

MS_TEST(parser, compareChain) {
  msTestExpectDump("a < b <= c == d",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_LT,MS_CMP_LE,MS_CMP_EQ\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_IDENT \"c\"\n"
      "      MS_AST_IDENT \"d\"\n");
}

MS_TEST(parser, compareTwoTokenOps) {
  msTestExpectDump("a not in b\na is not b\na in b\na is b",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_NOT_IN\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_IS_NOT\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_IN\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_IS\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, notAInBVsANotInB) {
  // MS precedence: unary not (4) binds tighter than in (11), so
  // `not a in b` is `(not a) in b` -- deliberately unlike Python.
  msTestExpectDump("not a in b",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_IN\n"
      "      MS_AST_UNARY op=MS_TOKEN_KW_NOT\n"
      "        MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, conditionalRightAssoc) {
  msTestExpectDump("a if c1 else b if c2 else d",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CONDITIONAL\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"c1\"\n"
      "      MS_AST_CONDITIONAL\n"
      "        MS_AST_IDENT \"b\"\n"
      "        MS_AST_IDENT \"c2\"\n"
      "        MS_AST_IDENT \"d\"\n");
}

MS_TEST(parser, awaitBindsAtPostfix) {
  // await f(x) + 1 == (await f(x)) + 1. Top-level await reports E210
  // (context check), but the tree must still be correct.
  MsTestFixture fix;
  msTestParse(&fix, "await f(x) + 1");
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(1, (long long)msDiagListCount(&fix.diags));
  MS_ASSERT_EQ(210, (long long)msDiagListAt(&fix.diags, 0)->code);
  char buf[MS_TEST_DUMP_CAP];
  msTestDump(fix.root, buf, sizeof(buf));
  const char* expected =
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "      MS_AST_AWAIT\n"
      "        MS_AST_CALL\n"
      "          MS_AST_IDENT \"f\"\n"
      "          MS_AST_IDENT \"x\"\n"
      "      MS_AST_INT \"1\"\n";
  MS_ASSERT_EQ(0, strcmp(buf, expected));
  msTestFixtureDestroy(&fix);
}

MS_TEST(parser, lambdaEmptyAndMultiParam) {
  msTestExpectDump("f(lambda: 0, lambda a, b: a + b)",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"f\"\n"
      "      MS_AST_LAMBDA\n"
      "        MS_AST_INT \"0\"\n"
      "      MS_AST_LAMBDA\n"
      "        MS_AST_PARAM \"a\" kind=MS_PARAM_NORMAL\n"
      "        MS_AST_PARAM \"b\" kind=MS_PARAM_NORMAL\n"
      "        MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "          MS_AST_IDENT \"a\"\n"
      "          MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, compareChainOpsGrowth) {
  // Six operands push the ops array past its initial capacity of 4,
  // exercising the arena-grown doubling path.
  msTestExpectDump("a < b < c < d < e < f",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_LT,MS_CMP_LT,MS_CMP_LT,MS_CMP_LT,MS_CMP_LT\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_IDENT \"c\"\n"
      "      MS_AST_IDENT \"d\"\n"
      "      MS_AST_IDENT \"e\"\n"
      "      MS_AST_IDENT \"f\"\n");
}

MS_TEST(parser, selfSuperContextError) {
  // self/super outside a class method: E210. ('=' assignment is Task 7; the
  // E210 from parseSelfSuper trips panic mode, so the trailing '=' adds no
  // second diagnostic.)
  static const uint32_t codes[] = { 210 };
  msTestExpectDiags("self.x = 1", codes, 1);
  msTestExpectDiags("super", codes, 1);
}

MS_TEST(parser, selfTreeDespiteContextError) {
  // The E210 context error must not break the tree: bare 'self' still
  // produces an IDENT node.
  MsTestFixture fix;
  msTestParse(&fix, "self");
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(1, (long long)msDiagListCount(&fix.diags));
  MS_ASSERT_EQ(210, (long long)msDiagListAt(&fix.diags, 0)->code);
  char buf[MS_TEST_DUMP_CAP];
  msTestDump(fix.root, buf, sizeof(buf));
  const char* expected =
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_IDENT \"self\"\n";
  MS_ASSERT_EQ(0, strcmp(buf, expected));
  msTestFixtureDestroy(&fix);
}

MS_TEST(parser, listAndSetAndDict) {
  // Statement-start '{' is a block (Task 7 dispatch), so the brace literals
  // are exercised in expression position.
  msTestExpectDump("[1, 2]\n({1, 2})\n({1: 2})\n({})",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SET_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_DICT_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_DICT_LIT\n");
}

MS_TEST(parser, emptyList) {
  msTestExpectDump("[]",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_LIT\n");
}

MS_TEST(parser, listComprehension) {
  msTestExpectDump("[x * x for x in xs if x > 0]",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_COMP\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"x\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"xs\"\n"
      "      MS_AST_IF\n"
      "        MS_AST_COMPARE_CHAIN ops=MS_CMP_GT\n"
      "          MS_AST_IDENT \"x\"\n"
      "          MS_AST_INT \"0\"\n");
}

MS_TEST(parser, comprehensionNestedFor) {
  msTestExpectDump("[x + y for x in a for y in b]",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_COMP\n"
      "      MS_AST_BINARY op=MS_TOKEN_PLUS\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"y\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"a\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"y\"\n"
      "        MS_AST_IDENT \"b\"\n");
}

MS_TEST(parser, dictAndSetComprehension) {
  // Dict comprehension uses the bracket form per 03-syntax section 6.2;
  // the brace form is accepted too (same AST kind).
  msTestExpectDump("[k: v * 2 for k, v in m]\n({x % 10 for x in xs})",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_DICT_COMP\n"
      "      MS_AST_IDENT \"k\"\n"
      "      MS_AST_BINARY op=MS_TOKEN_STAR\n"
      "        MS_AST_IDENT \"v\"\n"
      "        MS_AST_INT \"2\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"k\"\n"
      "        MS_AST_IDENT \"v\"\n"
      "        MS_AST_IDENT \"m\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SET_COMP\n"
      "      MS_AST_BINARY op=MS_TOKEN_PERCENT\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_INT \"10\"\n"
      "      MS_AST_FOR kind=MS_FOR_IN\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_IDENT \"xs\"\n");
}

MS_TEST(parser, comprehensionForMissingIn) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("[x for x xs]", codes, 1);
}

MS_TEST(parser, dictCompMissingFor) {
  // No bracket dict literal exists: [k: v] without 'for' is E203.
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("[k: v]", codes, 1);
}

MS_TEST(parser, containerTruncatedEof) {
  // Truncated containers report exactly one diagnostic (panic-mode
  // suppression) and terminate. The missing element/iterable surfaces as
  // E201 (expected expression) at EOF or the closing bracket.
  static const uint32_t codes[] = { 201 };
  msTestExpectDiags("[1,", codes, 1);
  msTestExpectDiags("[x for x in]", codes, 1);
  msTestExpectDiags("({,})", codes, 1);   // statement-start '{' is a block now
}

MS_TEST(parser, containerTrailingComma) {
  // Trailing commas are legal in all three literal forms; the dumps match
  // the non-trailing forms. Brace forms sit in expression position because
  // statement-start '{' is a block.
  msTestExpectDump("[1, 2,]\n({1: 2,})\n({1, 2,})",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_LIST_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_DICT_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_SET_LIT\n"
      "      MS_AST_INT \"1\"\n"
      "      MS_AST_INT \"2\"\n");
}

MS_TEST(parser, declAndAssign) {
  msTestExpectDump("a := 1\na = 2",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_DECL\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_INT \"1\"\n"
      "  MS_AST_ASSIGN\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_INT \"2\"\n");
}

MS_TEST(parser, swapAssign) {
  msTestExpectDump("a, b = b, a",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_ASSIGN\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_IDENT \"b\"\n"
      "    MS_AST_IDENT \"b\"\n"
      "    MS_AST_IDENT \"a\"\n");
}

MS_TEST(parser, unpackDecl) {
  msTestExpectDump("a, *rest := xs",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_DECL\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_UNARY op=MS_TOKEN_STAR\n"
      "      MS_AST_IDENT \"rest\"\n"
      "    MS_AST_IDENT \"xs\"\n");
}

MS_TEST(parser, assignTargetsAttrIndex) {
  msTestExpectDump("o.f, a[i] = 1, 2",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_ASSIGN\n"
      "    MS_AST_ATTR \"f\"\n"
      "      MS_AST_IDENT \"o\"\n"
      "    MS_AST_INDEX\n"
      "      MS_AST_IDENT \"a\"\n"
      "      MS_AST_IDENT \"i\"\n"
      "    MS_AST_INT \"1\"\n"
      "    MS_AST_INT \"2\"\n");
}

MS_TEST(parser, augAssignOps) {
  static const struct { const char* source; const char* op; } cases[] = {
    { "a += 1", "MS_TOKEN_PLUS_EQUAL" },
    { "a -= 1", "MS_TOKEN_MINUS_EQUAL" },
    { "a *= 1", "MS_TOKEN_STAR_EQUAL" },
    { "a /= 1", "MS_TOKEN_SLASH_EQUAL" },
    { "a %= 1", "MS_TOKEN_PERCENT_EQUAL" },
    { "a **= 1", "MS_TOKEN_DOUBLE_STAR_EQUAL" },
    { "a &= 1", "MS_TOKEN_AMP_EQUAL" },
    { "a |= 1", "MS_TOKEN_PIPE_EQUAL" },
    { "a ^= 1", "MS_TOKEN_CARET_EQUAL" },
    { "a <<= 1", "MS_TOKEN_SHIFT_LEFT_EQUAL" },
    { "a >>= 1", "MS_TOKEN_SHIFT_RIGHT_EQUAL" },
  };
  for (size_t i = 0; i < MS_ARRAY_LEN(cases); ++i) {
    char expected[512];
    snprintf(expected, sizeof(expected),
        "MS_AST_SOURCE_FILE\n"
        "  MS_AST_AUG_ASSIGN op=%s\n"
        "    MS_AST_IDENT \"a\"\n"
        "    MS_AST_INT \"1\"\n", cases[i].op);
    msTestExpectDump(cases[i].source, expected);
  }
}

MS_TEST(parser, augAssignMultiTarget) {
  // Augmented assignment is strictly single-target: extra targets are E205
  // (the node is still built from the first pair, so the tree stays whole).
  static const uint32_t codes[] = { 205 };
  msTestExpectDiags("a, b += 1", codes, 1);
}

MS_TEST(parser, badAssignTarget) {
  static const uint32_t codes[] = { 205 };
  msTestExpectDiags("1 = 2", codes, 1);
  msTestExpectDiags("a + b := 1", codes, 1);
}

MS_TEST(parser, badUnpack) {
  static const uint32_t codes[] = { 212 };
  msTestExpectDiags("*a, *b := xs", codes, 1);     // more than one star target
  msTestExpectDiags("*a = xs", codes, 1);          // star target only in :=/for-in
}

MS_TEST(parser, exprListWithoutAssign) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("a, b", codes, 1);   // expression list with no assignment operator
}

MS_TEST(parser, blockAsStatement) {
  msTestExpectDump("{ 1\n2 }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_BLOCK\n"
      "    MS_AST_EXPR_STMT\n"
      "      MS_AST_INT \"1\"\n"
      "    MS_AST_EXPR_STMT\n"
      "      MS_AST_INT \"2\"\n");
  // A block may be followed by one optional semicolon (explicit or
  // auto-inserted); the dump matches the bare form.
  msTestExpectDump("{ 1 };",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_BLOCK\n"
      "    MS_AST_EXPR_STMT\n"
      "      MS_AST_INT \"1\"\n");
}

MS_TEST(parser, unclosedBlock) {
  static const uint32_t codes[] = { 211 };
  msTestExpectDiags("{ 1", codes, 1);   // missing '}' is E211, not E203
}

MS_TEST(parser, incDecOutsideForClause) {
  static const uint32_t codes[] = { 206 };
  msTestExpectDiags("a++", codes, 1);   // '++'/'--' only in three-part for post
}

MS_TEST(parser, doubleEmptyStatement) {
  static const uint32_t codes[] = { 204 };
  msTestExpectDiags("1;;", codes, 1);   // panic suppression: one error
}

MS_TEST(parser, ifElseIfElse) {
  msTestExpectDump("if a { 1 } else if b { 2 } else { 3 }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_IF\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_INT \"1\"\n"
      "    MS_AST_IF\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_INT \"2\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_INT \"3\"\n");
}

MS_TEST(parser, forThreePart) {
  msTestExpectDump("for i := 0; i < 10; i++ { }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FOR kind=MS_FOR_CLAUSE\n"
      "    MS_AST_DECL\n"
      "      MS_AST_IDENT \"i\"\n"
      "      MS_AST_INT \"0\"\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_LT\n"
      "      MS_AST_IDENT \"i\"\n"
      "      MS_AST_INT \"10\"\n"
      "    MS_AST_INC_DEC op=MS_TOKEN_PLUS_PLUS\n"
      "      MS_AST_IDENT \"i\"\n"
      "    MS_AST_BLOCK\n");
}

MS_TEST(parser, forIn) {
  msTestExpectDump("for k, v in m { }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FOR kind=MS_FOR_IN\n"
      "    MS_AST_IDENT \"k\"\n"
      "    MS_AST_IDENT \"v\"\n"
      "    MS_AST_IDENT \"m\"\n"
      "    MS_AST_BLOCK\n");
}

MS_TEST(parser, forBare) {
  msTestExpectDump("for { }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FOR kind=MS_FOR_BARE\n"
      "    MS_AST_BLOCK\n");
}

MS_TEST(parser, incDecOutsideFor) {
  static const uint32_t codes[] = { 206 };
  msTestExpectDiags("i++", codes, 1);
}

MS_TEST(parser, incDecInForInit) {
  static const uint32_t codes[] = { 206 };
  // '++'/'--' is legal only in the POST segment of a three-part for clause.
  msTestExpectDiags("for i++; i < 3; i++ { }", codes, 1);
}

MS_TEST(parser, forClauseTrailingSemicolon) {
  static const uint32_t codes[] = { 203 };
  // The clause ends exactly at the body's '{': no ';' may follow the post
  // segment.
  msTestExpectDiags("for i := 0; i < 3; i++; { }", codes, 1);
}

MS_TEST(parser, whileStmt) {
  msTestExpectDump("while x < 1 { x = 2 }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_WHILE\n"
      "    MS_AST_COMPARE_CHAIN ops=MS_CMP_LT\n"
      "      MS_AST_IDENT \"x\"\n"
      "      MS_AST_INT \"1\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_ASSIGN\n"
      "        MS_AST_IDENT \"x\"\n"
      "        MS_AST_INT \"2\"\n");
}

MS_TEST(parser, tryExceptFinally) {
  msTestExpectDump("try { 1 } except IOError as e { 2 } except { 3 } finally { 4 }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_TRY\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_INT \"1\"\n"
      "    MS_AST_EXCEPT_CLAUSE as=\"e\"\n"
      "      MS_AST_IDENT \"IOError\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_INT \"2\"\n"
      "    MS_AST_EXCEPT_CLAUSE\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_INT \"3\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_INT \"4\"\n");
}

MS_TEST(parser, withAs) {
  msTestExpectDump("with open(\"f\") as f { g(f) }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_WITH as=\"f\"\n"
      "    MS_AST_CALL\n"
      "      MS_AST_IDENT \"open\"\n"
      "      MS_AST_STRING \"\\\"f\\\"\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_CALL\n"
      "          MS_AST_IDENT \"g\"\n"
      "          MS_AST_IDENT \"f\"\n");
}

MS_TEST(parser, selectCases) {
  msTestExpectDump(
      "select {\n"
      "case v := ch.recv():\n"
      "  use(v)\n"
      "case ch2.send(x):\n"
      "  pass\n"
      "default:\n"
      "  pass\n"
      "}",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_SELECT\n"
      "    MS_AST_SELECT_CASE kind=recv decl\n"
      "      MS_AST_IDENT \"v\"\n"
      "      MS_AST_IDENT \"ch\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_EXPR_STMT\n"
      "          MS_AST_CALL\n"
      "            MS_AST_IDENT \"use\"\n"
      "            MS_AST_IDENT \"v\"\n"
      "    MS_AST_SELECT_CASE kind=send\n"
      "      MS_AST_IDENT \"ch2\"\n"
      "      MS_AST_IDENT \"x\"\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_PASS\n"
      "    MS_AST_SELECT_CASE kind=default\n"
      "      MS_AST_BLOCK\n"
      "        MS_AST_PASS\n");
}

MS_TEST(parser, importForms) {
  msTestExpectDump(
      "import \"fmt\"\n"
      "import \"encoding/json\" as encjson\n"
      "from \"strings\" import toUpper, split\n"
      "from \"os\" import (getenv,)",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_IMPORT \"\\\"fmt\\\"\"\n"
      "  MS_AST_IMPORT \"\\\"encoding/json\\\"\" as=\"encjson\"\n"
      "  MS_AST_FROM_IMPORT \"\\\"strings\\\"\"\n"
      "    MS_AST_IDENT \"toUpper\"\n"
      "    MS_AST_IDENT \"split\"\n"
      "  MS_AST_FROM_IMPORT \"\\\"os\\\"\"\n"
      "    MS_AST_IDENT \"getenv\"\n");
}

MS_TEST(parser, importPathMustBeString) {
  static const uint32_t codes[] = { 203 };
  msTestExpectDiags("import fmt", codes, 1);
}

MS_TEST(parser, funcDeclFull) {
  msTestExpectDump("func f(a, b = 1, *rest, **kw) { return a }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FUNC_DECL \"f\"\n"
      "    MS_AST_PARAM \"a\" kind=MS_PARAM_NORMAL\n"
      "    MS_AST_PARAM \"b\" kind=MS_PARAM_NORMAL\n"
      "      MS_AST_INT \"1\"\n"
      "    MS_AST_PARAM \"rest\" kind=MS_PARAM_VARARGS\n"
      "    MS_AST_PARAM \"kw\" kind=MS_PARAM_KWARGS\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_RETURN\n"
      "        MS_AST_IDENT \"a\"\n");
}

MS_TEST(parser, asyncFunc) {
  msTestExpectDump("async func f() { await g() }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FUNC_DECL \"f\" async\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_EXPR_STMT\n"
      "        MS_AST_AWAIT\n"
      "          MS_AST_CALL\n"
      "            MS_AST_IDENT \"g\"\n");
}

MS_TEST(parser, paramOrderViolation) {
  static const uint32_t codes[] = { 207 };
  msTestExpectDiags("func f(*rest, a) { }", codes, 1);       // normal after varargs
  msTestExpectDiags("func f(a = 1, b) { }", codes, 1);       // non-default after default
}

MS_TEST(parser, paramDuplicate) {
  static const uint32_t codes[] = { 208 };
  msTestExpectDiags("func f(a, a) { }", codes, 1);
}

MS_TEST(parser, classDecl) {
  msTestExpectDump(
      "class Dog < Animal {\n"
      "  func __init__(self) { }\n"
      "  static func create() { }\n"
      "}",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_CLASS_DECL \"Dog\"\n"
      "    MS_AST_IDENT \"Animal\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_FUNC_DECL \"__init__\"\n"
      "        MS_AST_PARAM \"self\" kind=MS_PARAM_NORMAL\n"
      "        MS_AST_BLOCK\n"
      "      MS_AST_FUNC_DECL \"create\" static\n"
      "        MS_AST_BLOCK\n");
}

MS_TEST(parser, staticOutsideClassIsIdent) {
  msTestExpectDump("static := 1",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_DECL\n"
      "    MS_AST_IDENT \"static\"\n"
      "    MS_AST_INT \"1\"\n");
}

MS_TEST(parser, breakOutsideLoop) {
  static const uint32_t codes[] = { 209 };
  msTestExpectDiags("break", codes, 1);
}

MS_TEST(parser, breakInLoopClosureBarrier) {
  static const uint32_t codes[] = { 209 };
  // loopDepth resets on entering a func body: the nested function cannot
  // break the outer loop.
  msTestExpectDiags("while true { func g() { break } }", codes, 1);
}

MS_TEST(parser, breakInsideLoop) {
  msTestExpectDump("while true { break\ncontinue }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_WHILE\n"
      "    MS_AST_BOOL true\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_BREAK\n"
      "      MS_AST_CONTINUE\n");
}

MS_TEST(parser, returnAtTopLevel) {
  static const uint32_t codes[] = { 210 };
  msTestExpectDiags("return 1", codes, 1);
}

MS_TEST(parser, awaitOutsideAsync) {
  static const uint32_t codes[] = { 210 };
  msTestExpectDiags("await f()", codes, 1);
  msTestExpectDiags("func g() { await f() }", codes, 1);
}

MS_TEST(parser, selfOutsideMethod) {
  static const uint32_t codes[] = { 210 };
  msTestExpectDiags("self.x = 1", codes, 1);
}

MS_TEST(parser, selfInsideMethod) {
  msTestExpectDump("class A {\n  func m(self) { self.x = 1\nsuper.m() }\n}",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_CLASS_DECL \"A\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_FUNC_DECL \"m\"\n"
      "        MS_AST_PARAM \"self\" kind=MS_PARAM_NORMAL\n"
      "        MS_AST_BLOCK\n"
      "          MS_AST_ASSIGN\n"
      "            MS_AST_ATTR \"x\"\n"
      "              MS_AST_IDENT \"self\"\n"
      "            MS_AST_INT \"1\"\n"
      "          MS_AST_EXPR_STMT\n"
      "            MS_AST_CALL\n"
      "              MS_AST_ATTR \"m\"\n"
      "                MS_AST_IDENT \"super\"\n");
}

MS_TEST(parser, nestedFuncDecl) {
  // func/class are statements: nested declarations are legal.
  msTestExpectDump("func outer() { func inner() { return 1 } }",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FUNC_DECL \"outer\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_FUNC_DECL \"inner\"\n"
      "        MS_AST_BLOCK\n"
      "          MS_AST_RETURN\n"
      "            MS_AST_INT \"1\"\n");
}

MS_TEST(parser, returnRaiseDelGlobalPass) {
  msTestExpectDump(
      "func f() {\n"
      "  raise E from cause\n"
      "}\n"
      "del a, b[i]\n"
      "global g1, g2\n"
      "pass",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_FUNC_DECL \"f\"\n"
      "    MS_AST_BLOCK\n"
      "      MS_AST_RAISE\n"
      "        MS_AST_IDENT \"E\"\n"
      "        MS_AST_IDENT \"cause\"\n"
      "  MS_AST_DEL\n"
      "    MS_AST_IDENT \"a\"\n"
      "    MS_AST_INDEX\n"
      "      MS_AST_IDENT \"b\"\n"
      "      MS_AST_IDENT \"i\"\n"
      "  MS_AST_GLOBAL\n"
      "    MS_AST_IDENT \"g1\"\n"
      "    MS_AST_IDENT \"g2\"\n"
      "  MS_AST_PASS\n");
}

MS_TEST(parser, fstringTextOnly) {
  msTestExpectDump("f\"hello\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_TEXT \"hello\"\n");
}

MS_TEST(parser, fstringInterpolation) {
  msTestExpectDump("f\"a{x}b{y:03d}c\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_TEXT \"a\"\n"
      "      MS_AST_FSTRING_EXPR\n"
      "        MS_AST_IDENT \"x\"\n"
      "      MS_AST_FSTRING_TEXT \"b\"\n"
      "      MS_AST_FSTRING_EXPR fmt=\"03d\"\n"
      "        MS_AST_IDENT \"y\"\n"
      "      MS_AST_FSTRING_TEXT \"c\"\n");
}

MS_TEST(parser, fstringNestedBracesInFormat) {
  // The format spec slice is stored raw, nested fields not expanded (task 28).
  msTestExpectDump("f\"{n:{w}d}\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_EXPR fmt=\"{w}d\"\n"
      "        MS_AST_IDENT \"n\"\n");
}

MS_TEST(parser, fstringNestedFstring) {
  // There are no single-quoted f-strings; nesting reuses the same quote (the
  // lexer's frame stack keeps the inner closing quote from ending the outer).
  msTestExpectDump("f\"{f\"{x}\"}\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_EXPR\n"
      "        MS_AST_FSTRING\n"
      "          MS_AST_FSTRING_EXPR\n"
      "            MS_AST_IDENT \"x\"\n");
}

MS_TEST(parser, fstringExprWithCallAndSlice) {
  msTestExpectDump("f\"{f(a)[1:]}\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n"
      "      MS_AST_FSTRING_EXPR\n"
      "        MS_AST_SLICE parts=start\n"
      "          MS_AST_CALL\n"
      "            MS_AST_IDENT \"f\"\n"
      "            MS_AST_IDENT \"a\"\n"
      "          MS_AST_INT \"1\"\n");
}

MS_TEST(parser, fstringEmpty) {
  // f"" lexes as FSTRING_START + FSTRING_END with no text segment, so the
  // FSTRING node has zero parts.
  msTestExpectDump("f\"\"",
      "MS_AST_SOURCE_FILE\n"
      "  MS_AST_EXPR_STMT\n"
      "    MS_AST_FSTRING\n");
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
    {"parser.postfixMixedChain", testparserpostfixMixedChain},
    {"parser.callKeywordArgs", testparsercallKeywordArgs},
    {"parser.callTrailingCommaAndEmpty", testparsercallTrailingCommaAndEmpty},
    {"parser.sliceCombinations", testparsersliceCombinations},
    {"parser.emptySubscriptIsError", testparseremptySubscriptIsError},
    {"parser.compareChain", testparsercompareChain},
    {"parser.compareTwoTokenOps", testparsercompareTwoTokenOps},
    {"parser.notAInBVsANotInB", testparsernotAInBVsANotInB},
    {"parser.conditionalRightAssoc", testparserconditionalRightAssoc},
    {"parser.awaitBindsAtPostfix", testparserawaitBindsAtPostfix},
    {"parser.lambdaEmptyAndMultiParam", testparserlambdaEmptyAndMultiParam},
    {"parser.compareChainOpsGrowth", testparsercompareChainOpsGrowth},
    {"parser.selfSuperContextError", testparserselfSuperContextError},
    {"parser.selfTreeDespiteContextError", testparserselfTreeDespiteContextError},
    {"parser.listAndSetAndDict", testparserlistAndSetAndDict},
    {"parser.emptyList", testparseremptyList},
    {"parser.listComprehension", testparserlistComprehension},
    {"parser.comprehensionNestedFor", testparsercomprehensionNestedFor},
    {"parser.dictAndSetComprehension", testparserdictAndSetComprehension},
    {"parser.comprehensionForMissingIn", testparsercomprehensionForMissingIn},
    {"parser.dictCompMissingFor", testparserdictCompMissingFor},
    {"parser.containerTruncatedEof", testparsercontainerTruncatedEof},
    {"parser.containerTrailingComma", testparsercontainerTrailingComma},
    {"parser.declAndAssign", testparserdeclAndAssign},
    {"parser.swapAssign", testparserswapAssign},
    {"parser.unpackDecl", testparserunpackDecl},
    {"parser.assignTargetsAttrIndex", testparserassignTargetsAttrIndex},
    {"parser.augAssignOps", testparseraugAssignOps},
    {"parser.augAssignMultiTarget", testparseraugAssignMultiTarget},
    {"parser.badAssignTarget", testparserbadAssignTarget},
    {"parser.badUnpack", testparserbadUnpack},
    {"parser.exprListWithoutAssign", testparserexprListWithoutAssign},
    {"parser.blockAsStatement", testparserblockAsStatement},
    {"parser.unclosedBlock", testparserunclosedBlock},
    {"parser.incDecOutsideForClause", testparserincDecOutsideForClause},
    {"parser.doubleEmptyStatement", testparserdoubleEmptyStatement},
    {"parser.ifElseIfElse", testparserifElseIfElse},
    {"parser.forThreePart", testparserforThreePart},
    {"parser.forIn", testparserforIn},
    {"parser.forBare", testparserforBare},
    {"parser.incDecOutsideFor", testparserincDecOutsideFor},
    {"parser.incDecInForInit", testparserincDecInForInit},
    {"parser.forClauseTrailingSemicolon", testparserforClauseTrailingSemicolon},
    {"parser.whileStmt", testparserwhileStmt},
    {"parser.tryExceptFinally", testparsertryExceptFinally},
    {"parser.withAs", testparserwithAs},
    {"parser.selectCases", testparserselectCases},
    {"parser.importForms", testparserimportForms},
    {"parser.importPathMustBeString", testparserimportPathMustBeString},
    {"parser.funcDeclFull", testparserfuncDeclFull},
    {"parser.asyncFunc", testparserasyncFunc},
    {"parser.paramOrderViolation", testparserparamOrderViolation},
    {"parser.paramDuplicate", testparserparamDuplicate},
    {"parser.classDecl", testparserclassDecl},
    {"parser.staticOutsideClassIsIdent", testparserstaticOutsideClassIsIdent},
    {"parser.breakOutsideLoop", testparserbreakOutsideLoop},
    {"parser.breakInLoopClosureBarrier", testparserbreakInLoopClosureBarrier},
    {"parser.breakInsideLoop", testparserbreakInsideLoop},
    {"parser.returnAtTopLevel", testparserreturnAtTopLevel},
    {"parser.awaitOutsideAsync", testparserawaitOutsideAsync},
    {"parser.selfOutsideMethod", testparserselfOutsideMethod},
    {"parser.selfInsideMethod", testparserselfInsideMethod},
    {"parser.nestedFuncDecl", testparsernestedFuncDecl},
    {"parser.returnRaiseDelGlobalPass", testparserreturnRaiseDelGlobalPass},
    {"parser.fstringTextOnly", testparserfstringTextOnly},
    {"parser.fstringInterpolation", testparserfstringInterpolation},
    {"parser.fstringNestedBracesInFormat", testparserfstringNestedBracesInFormat},
    {"parser.fstringNestedFstring", testparserfstringNestedFstring},
    {"parser.fstringExprWithCallAndSlice", testparserfstringExprWithCallAndSlice},
    {"parser.fstringEmpty", testparserfstringEmpty},
};
MS_TEST_MAIN(msTests)
