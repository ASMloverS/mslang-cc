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

static const MsTestCase msTests[] = {
    {"parser.kindNameTable", testparserkindNameTable},
    {"parser.arenaMultiBlock", testparserarenaMultiBlock},
    {"parser.arenaListPushGrowth", testparserarenaListPushGrowth},
    {"parser.arenaOom", testparserarenaOom},
};
MS_TEST_MAIN(msTests)
