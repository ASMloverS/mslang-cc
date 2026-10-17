#include "compiler/ms_compiler.h"

#include <string.h>

#include "core/ms_memory.h"
#include "lexer/ms_lexer.h"
#include "object/ms_string.h"
#include "parser/ms_ast.h"
#include "parser/ms_parser.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"
#include "ms_test.h"

typedef struct {
  struct MsHeap heap;
  struct MsDiagList diags;
  struct MsAstArena arena;
  struct MsLexer lexer;
  struct MsParser parser;
  struct MsCompiler compiler;
  struct MsAst* root;
  struct MsProto* proto;
  MsResult result;
} MsTestFixture;

static void msTestCompile(MsTestFixture* fix, const char* source, uint32_t flags) {
  memset(fix, 0, sizeof(*fix));
  msMemResetStats();
  MS_ASSERT_EQ(MS_OK, msHeapInit(&fix->heap));
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&fix->diags, "test.ms"));
  msAstArenaInit(&fix->arena);
  msLexerInit(&fix->lexer, source, strlen(source), "test.ms", &fix->diags);
  msParserInit(&fix->parser, &fix->lexer, &fix->arena, &fix->diags);
  msCompilerInit(&fix->compiler, &fix->heap, &fix->diags, "test.ms", flags);
  fix->result = msParserParse(&fix->parser, &fix->root);
  msLexerDestroy(&fix->lexer);
  if (fix->result == MS_OK) {
    fix->result = msCompilerCompile(&fix->compiler, fix->root, &fix->proto);
  }
}

static void msTestFixtureDestroy(MsTestFixture* fix) {
  msMemSetFailAfter(-1);
  msCompilerDestroy(&fix->compiler);
  msProtoFree(fix->proto);
  msAstArenaDestroy(&fix->arena);
  msHeapDestroy(&fix->heap);
  msDiagListDestroy(&fix->diags);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

static void msTestAssertOps(const struct MsProto* proto, const MsOpCode* ops, size_t count) {
  MS_ASSERT_TRUE(proto != NULL);
  if (proto == NULL) {
    return;
  }
  MS_ASSERT_EQ((long long)count, proto->codeLen);
  size_t n = count < (size_t)proto->codeLen ? count : (size_t)proto->codeLen;
  for (size_t i = 0; i < n; ++i) {
    MS_ASSERT_EQ(ops[i], msOpDecodeOp(proto->code[i]));
  }
}

MS_TEST(compiler, emptyModule) {
  MsTestFixture fix;
  msTestCompile(&fix, "", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(0, (long long)msDiagListCount(&fix.diags));
  static const MsOpCode expected[] = {MS_OP_LOAD_NIL, MS_OP_RETURN};
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(0, fix.proto->paramCount);
  MS_ASSERT_EQ(0, fix.proto->localCount);
  MS_ASSERT_EQ(1, fix.proto->stackSize);
  MS_ASSERT_TRUE(strcmp(msStringData(fix.proto->name), "<main>") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(fix.proto->sourceFile), "test.ms") == 0);
  MS_ASSERT_TRUE(!fix.proto->isAsync && !fix.proto->hasVarArgs && !fix.proto->hasKwArgs);
  msTestFixtureDestroy(&fix);
}

static const MsTestCase msTestCases[] = {
  {"Compiler.EmptyModule", testcompileremptyModule},
};

MS_TEST_MAIN(msTestCases)
