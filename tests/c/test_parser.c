#include <string.h>

#include "core/ms_memory.h"
#include "parser/ms_ast.h"
#include "ms_test.h"

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

static const MsTestCase msTests[] = {
    {"parser.kindNameTable", testparserkindNameTable},
    {"parser.arenaMultiBlock", testparserarenaMultiBlock},
    {"parser.arenaListPushGrowth", testparserarenaListPushGrowth},
    {"parser.arenaOom", testparserarenaOom},
    {"parser.dumpHandBuiltTree", testparserdumpHandBuiltTree},
    {"parser.dumpEscapesLexemes", testparserdumpEscapesLexemes},
};
MS_TEST_MAIN(msTests)
