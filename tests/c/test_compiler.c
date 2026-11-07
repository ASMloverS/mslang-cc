#include "compiler/ms_compiler.h"

#include <math.h>
#include <stdbool.h>
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

static void msTestCompileWithFailAfter(MsTestFixture* fix, const char* source,
    uint32_t flags, int64_t failAfter) {
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
    msMemSetFailAfter(failAfter);
    fix->result = msCompilerCompile(&fix->compiler, fix->root, &fix->proto);
    msMemSetFailAfter(-1);
  }
}

static void msTestCompile(MsTestFixture* fix, const char* source, uint32_t flags) {
  msTestCompileWithFailAfter(fix, source, flags, -1);
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
  if (count > 0) {
    MS_ASSERT_TRUE(proto->code != NULL);
    if (proto->code == NULL) {
      return;
    }
  }
  size_t codeLen = proto->codeLen > 0 ? (size_t)proto->codeLen : 0;
  size_t n = count < codeLen ? count : codeLen;
  for (size_t i = 0; i < n; ++i) {
    MS_ASSERT_EQ(ops[i], msOpDecodeOp(proto->code[i]));
  }
}

static const struct MsObject* msTestConstAt(const struct MsProto* proto, int index,
    MsTypeTag expectedTag) {
  MS_ASSERT_TRUE(proto != NULL);
  if (proto == NULL) {
    return NULL;
  }
  MS_ASSERT_TRUE(proto->consts != NULL);
  if (proto->consts == NULL) {
    return NULL;
  }
  MS_ASSERT_TRUE(index >= 0 && index < proto->constsLen);
  if (index < 0 || index >= proto->constsLen) {
    return NULL;
  }
  const struct MsObject* value = proto->consts[index];
  MS_ASSERT_TRUE(value != NULL);
  if (value == NULL) {
    return NULL;
  }
  MsTypeTag actualTag = msObjectTag(value);
  MS_ASSERT_EQ(expectedTag, actualTag);
  if (actualTag != expectedTag) {
    return NULL;
  }
  return value;
}

static uint32_t msTestCodeAt(const struct MsProto* proto, int index) {
  MS_ASSERT_TRUE(proto != NULL);
  if (proto == NULL) {
    return 0;
  }
  MS_ASSERT_TRUE(proto->code != NULL);
  if (proto->code == NULL) {
    return 0;
  }
  MS_ASSERT_TRUE(index >= 0 && index < proto->codeLen);
  if (index < 0 || index >= proto->codeLen) {
    return 0;
  }
  return proto->code[index];
}

MS_TEST(compiler, emptyModule) {
  MsTestFixture fix;
  msTestCompile(&fix, "", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(0, (long long)msDiagListCount(&fix.diags));
  static const MsOpCode expected[] = {MS_OP_LOAD_NIL, MS_OP_RETURN};
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(0, fix.proto->paramCount);
    MS_ASSERT_EQ(0, fix.proto->localCount);
    MS_ASSERT_EQ(1, fix.proto->stackSize);
    MS_ASSERT_TRUE(fix.proto->name != NULL);
    if (fix.proto->name != NULL) {
      MS_ASSERT_TRUE(strcmp(msStringData(fix.proto->name), "<main>") == 0);
    }
    MS_ASSERT_TRUE(fix.proto->sourceFile != NULL);
    if (fix.proto->sourceFile != NULL) {
      MS_ASSERT_TRUE(strcmp(msStringData(fix.proto->sourceFile), "test.ms") == 0);
    }
    MS_ASSERT_TRUE(!fix.proto->isAsync && !fix.proto->hasVarArgs && !fix.proto->hasKwArgs);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, scalarLiteralsAndDedup) {
  MsTestFixture fix;
  msTestCompile(&fix, "1; 1; true; false; nil;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_TRUE(fix.proto != NULL);
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(1, fix.proto->constsLen);
    const struct MsObject* one = msTestConstAt(fix.proto, 0, MS_TYPE_INT);
    if (one != NULL) {
      MS_ASSERT_EQ(1, msIntWordValue(one));
    }
    MS_ASSERT_EQ(1, fix.proto->localCount);
  }
  static const MsOpCode expected[] = {
    MS_OP_LOAD_CONST, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_CONST, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_TRUE, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_FALSE, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_NIL, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  if (fix.proto != NULL && fix.proto->code != NULL) {
    for (int i = 1; i < 10 && i < fix.proto->codeLen; i += 2) {
      MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[i]));
    }
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, numericLiteralBasesAndUnderscores) {
  MsTestFixture fix;
  msTestCompile(&fix, "0xff; 0o17; 0b1010; 1_000; 1_2.5_0e+1;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_TRUE(fix.proto != NULL);
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(5, fix.proto->constsLen);
    const struct MsObject* hexadecimal = msTestConstAt(fix.proto, 0, MS_TYPE_INT);
    if (hexadecimal != NULL) {
      MS_ASSERT_EQ(255, msIntWordValue(hexadecimal));
    }
    const struct MsObject* octal = msTestConstAt(fix.proto, 1, MS_TYPE_INT);
    if (octal != NULL) {
      MS_ASSERT_EQ(15, msIntWordValue(octal));
    }
    const struct MsObject* binary = msTestConstAt(fix.proto, 2, MS_TYPE_INT);
    if (binary != NULL) {
      MS_ASSERT_EQ(10, msIntWordValue(binary));
    }
    const struct MsObject* grouped = msTestConstAt(fix.proto, 3, MS_TYPE_INT);
    if (grouped != NULL) {
      MS_ASSERT_EQ(1000, msIntWordValue(grouped));
    }
    const struct MsObject* floating = msTestConstAt(fix.proto, 4, MS_TYPE_FLOAT);
    if (floating != NULL) {
      MS_ASSERT_TRUE(msFloatValue(floating) == 125.0);
    }
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, stringLiteralDecoding) {
  MsTestFixture fix;
  msTestCompile(&fix, "\"a\\n\\u0041\"; `a\\n`;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_TRUE(fix.proto != NULL);
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(2, fix.proto->constsLen);
    const struct MsString* escaped =
        (const struct MsString*)msTestConstAt(fix.proto, 0, MS_TYPE_STRING);
    const struct MsString* raw =
        (const struct MsString*)msTestConstAt(fix.proto, 1, MS_TYPE_STRING);
    if (escaped != NULL) {
      MS_ASSERT_EQ(3, (long long)msStringLen(escaped));
      if (msStringLen(escaped) >= 3) {
        MS_ASSERT_TRUE(memcmp(msStringData(escaped), "a\nA", 3) == 0);
      }
    }
    if (raw != NULL) {
      MS_ASSERT_EQ(3, (long long)msStringLen(raw));
      if (msStringLen(raw) >= 3) {
        MS_ASSERT_TRUE(memcmp(msStringData(raw), "a\\n", 3) == 0);
      }
    }
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, integerOverflowReportsE308) {
  MsTestFixture fix;
  msTestCompile(&fix, "9223372036854775808;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_TRUE(fix.proto == NULL);
  MS_ASSERT_EQ(1, (long long)msDiagListCount(&fix.diags));
  const struct MsDiag* diag = NULL;
  if (msDiagListCount(&fix.diags) > 0) {
    diag = msDiagListAt(&fix.diags, 0);
  }
  MS_ASSERT_TRUE(diag != NULL);
  if (diag != NULL) {
    MS_ASSERT_EQ(308, (long long)diag->code);
    MS_ASSERT_EQ(1, (long long)diag->line);
    MS_ASSERT_EQ(1, (long long)diag->column);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, moduleDeclarationUsesGlobal) {
  MsTestFixture fix;
  msTestCompile(&fix, "x := 1; x;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_CONST, MS_OP_STORE_GLOBAL, MS_OP_LOAD_GLOBAL,
    MS_OP_STORE_LOCAL, MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(1, msOpDecodeBx(msTestCodeAt(fix.proto, 1)));
  MS_ASSERT_EQ(1, msOpDecodeBx(msTestCodeAt(fix.proto, 2)));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 3)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, siblingBlocksReuseSlots) {
  MsTestFixture fix;
  msTestCompile(&fix, "x := 0; { x := 1; x; } { x := 2; x; } x;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(msTestCodeAt(fix.proto, 3)));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 3)));
  MS_ASSERT_EQ(1, msOpDecodeA(msTestCodeAt(fix.proto, 5)));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 7)));
  MS_ASSERT_EQ(1, msOpDecodeA(msTestCodeAt(fix.proto, 9)));
  MS_ASSERT_EQ(MS_OP_LOAD_GLOBAL, msOpDecodeOp(msTestCodeAt(fix.proto, 10)));
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(2, fix.proto->localCount);
  }
  msTestFixtureDestroy(&fix);

  msTestCompile(&fix, "{ 1; } { x := 2; x; }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_LOAD_LOCAL, msOpDecodeOp(msTestCodeAt(fix.proto, 4)));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 4)));
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(msTestCodeAt(fix.proto, 5)));
  MS_ASSERT_EQ(1, msOpDecodeA(msTestCodeAt(fix.proto, 5)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, longIdentifierResolvesByContent) {
  MsTestFixture fix;
  msTestCompile(&fix,
      "{ abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN := 1; abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN; }",
      MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(msTestCodeAt(fix.proto, 1)));
  MS_ASSERT_EQ(MS_OP_LOAD_LOCAL, msOpDecodeOp(msTestCodeAt(fix.proto, 2)));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 1)));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 2)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, declarationRhsReadsOuterSlot) {
  MsTestFixture fix;
  msTestCompile(&fix, "{ x := 1; { x := x; } }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_CONST, MS_OP_STORE_LOCAL, MS_OP_LOAD_LOCAL,
    MS_OP_STORE_LOCAL, MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 1)));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 2)));
  MS_ASSERT_EQ(1, msOpDecodeA(msTestCodeAt(fix.proto, 3)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, binaryOpcodeMapping) {
  static const struct {
    const char* source;
    MsOpCode op;
  } cases[] = {
    {"a + b;", MS_OP_ADD}, {"a - b;", MS_OP_SUB}, {"a * b;", MS_OP_MUL},
    {"a / b;", MS_OP_DIV}, {"a div b;", MS_OP_FLOORDIV}, {"a % b;", MS_OP_MOD},
    {"a ** b;", MS_OP_POW}, {"a << b;", MS_OP_SHL}, {"a >> b;", MS_OP_SHR},
    {"a & b;", MS_OP_BITAND}, {"a | b;", MS_OP_BITOR}, {"a ^ b;", MS_OP_BITXOR}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    MsTestFixture fix;
    msTestCompile(&fix, cases[i].source, MS_COMPILE_DEFAULT);
    MS_ASSERT_EQ(MS_OK, fix.result);
    MS_ASSERT_EQ(cases[i].op, msOpDecodeOp(msTestCodeAt(fix.proto, 2)));
    if (fix.proto != NULL) {
      MS_ASSERT_EQ(2, fix.proto->stackSize);
    }
    msTestFixtureDestroy(&fix);
  }
  MsTestFixture fix;
  msTestCompile(&fix, "-a; ~a; not a; +a;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_NEG, msOpDecodeOp(msTestCodeAt(fix.proto, 1)));
  MS_ASSERT_EQ(MS_OP_INVERT, msOpDecodeOp(msTestCodeAt(fix.proto, 4)));
  MS_ASSERT_EQ(MS_OP_NOT, msOpDecodeOp(msTestCodeAt(fix.proto, 7)));
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(msTestCodeAt(fix.proto, 9)));
  MS_ASSERT_EQ(MS_OP_ADD, msOpDecodeOp(msTestCodeAt(fix.proto, 11)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, constantFoldWhitelist) {
  MsTestFixture fix;
  msTestCompile(&fix, "1 + 2 * 3; -5; ~0; not nil; 1 + 2.0;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(4, fix.proto->constsLen);
  }
  const struct MsObject* seven = msTestConstAt(fix.proto, 0, MS_TYPE_INT);
  if (seven != NULL) {
    MS_ASSERT_EQ(7, msIntWordValue(seven));
  }
  const struct MsObject* minusFive = msTestConstAt(fix.proto, 1, MS_TYPE_INT);
  if (minusFive != NULL) {
    MS_ASSERT_EQ(-5, msIntWordValue(minusFive));
  }
  const struct MsObject* minusOne = msTestConstAt(fix.proto, 2, MS_TYPE_INT);
  if (minusOne != NULL) {
    MS_ASSERT_EQ(-1, msIntWordValue(minusOne));
  }
  const struct MsObject* three = msTestConstAt(fix.proto, 3, MS_TYPE_FLOAT);
  if (three != NULL) {
    MS_ASSERT_TRUE(msFloatValue(three) == 3.0);
  }
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(msTestCodeAt(fix.proto, 0)));
  MS_ASSERT_EQ(MS_OP_LOAD_TRUE, msOpDecodeOp(msTestCodeAt(fix.proto, 6)));
  msTestFixtureDestroy(&fix);

  msTestCompile(&fix, "+5; 5 - 8;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  const struct MsObject* five = msTestConstAt(fix.proto, 0, MS_TYPE_INT);
  if (five != NULL) {
    MS_ASSERT_EQ(5, msIntWordValue(five));
  }
  const struct MsObject* minusThree = msTestConstAt(fix.proto, 1, MS_TYPE_INT);
  if (minusThree != NULL) {
    MS_ASSERT_EQ(-3, msIntWordValue(minusThree));
  }
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(msTestCodeAt(fix.proto, 0)));
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(msTestCodeAt(fix.proto, 2)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, int64MinAndOverflowBehavior) {
  MsTestFixture fix;
  msTestCompile(&fix,
      "-9223372036854775808; 9223372036854775807 + 1; -9223372036854775808 * -1;",
      MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  const struct MsObject* int64Min = msTestConstAt(fix.proto, 0, MS_TYPE_INT);
  if (int64Min != NULL) {
    MS_ASSERT_EQ(INT64_MIN, msIntWordValue(int64Min));
  }
  MS_ASSERT_EQ(MS_OP_ADD, msOpDecodeOp(msTestCodeAt(fix.proto, 4)));
  MS_ASSERT_EQ(MS_OP_MUL, msOpDecodeOp(msTestCodeAt(fix.proto, 8)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, nonWhitelistOperationsRemain) {
  MsTestFixture fix;
  msTestCompile(&fix, "\"a\" + \"b\"; 1 / 0;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_ADD, msOpDecodeOp(msTestCodeAt(fix.proto, 2)));
  MS_ASSERT_EQ(MS_OP_DIV, msOpDecodeOp(msTestCodeAt(fix.proto, 6)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, foldedSignedZeroRemainDistinct) {
  MsTestFixture fix;
  msTestCompile(&fix, "-0.0; 0.0; +(-0.0);", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(2, fix.proto->constsLen);
  }
  const struct MsObject* negativeZero = msTestConstAt(fix.proto, 0, MS_TYPE_FLOAT);
  if (negativeZero != NULL) {
    MS_ASSERT_TRUE(signbit(msFloatValue(negativeZero)));
  }
  const struct MsObject* positiveZero = msTestConstAt(fix.proto, 1, MS_TYPE_FLOAT);
  if (positiveZero != NULL) {
    MS_ASSERT_TRUE(!signbit(msFloatValue(positiveZero)));
  }
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(msTestCodeAt(fix.proto, 4)));
  MS_ASSERT_EQ(msOpDecodeBx(msTestCodeAt(fix.proto, 2)),
      msOpDecodeBx(msTestCodeAt(fix.proto, 4)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, logicalShortCircuitShape) {
  MsTestFixture fix;
  msTestCompile(&fix, "a and b; a or b;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_JUMP_IF_FALSE, msOpDecodeOp(msTestCodeAt(fix.proto, 3)));
  MS_ASSERT_EQ(2, msOpDecodeSAx(msTestCodeAt(fix.proto, 3)));
  MS_ASSERT_EQ(MS_OP_JUMP_IF_TRUE, msOpDecodeOp(msTestCodeAt(fix.proto, 11)));
  MS_ASSERT_EQ(2, msOpDecodeSAx(msTestCodeAt(fix.proto, 11)));
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(2, fix.proto->localCount);
  }
  msTestFixtureDestroy(&fix);

  for (int64_t failAfter = 0; failAfter < 64; ++failAfter) {
    MsTestFixture injection;
    msTestCompileWithFailAfter(&injection, "f(); a and b;",
        MS_COMPILE_DEFAULT, failAfter);
    MS_ASSERT_TRUE(injection.result == MS_OK || injection.result == MS_ERROR_OOM);
    msTestFixtureDestroy(&injection);
  }
}

MS_TEST(compiler, constantConditionsSelectOneBranch) {
  MsTestFixture fix;
  msTestCompile(&fix,
      "false and x; true or x; true and x; a if true else b; a if false else b;",
      MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  if (fix.proto != NULL) {
    for (int i = 0; i < fix.proto->codeLen; ++i) {
      MsOpCode op = msOpDecodeOp(msTestCodeAt(fix.proto, i));
      MS_ASSERT_TRUE(op != MS_OP_JUMP && op != MS_OP_JUMP_IF_FALSE && op != MS_OP_JUMP_IF_TRUE);
    }
  }
  MS_ASSERT_EQ(MS_OP_LOAD_FALSE, msOpDecodeOp(msTestCodeAt(fix.proto, 0)));
  MS_ASSERT_EQ(MS_OP_LOAD_TRUE, msOpDecodeOp(msTestCodeAt(fix.proto, 2)));
  MS_ASSERT_EQ(MS_OP_LOAD_GLOBAL, msOpDecodeOp(msTestCodeAt(fix.proto, 4)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, conditionalExpressionJumps) {
  MsTestFixture fix;
  msTestCompile(&fix, "a if c else b;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_GLOBAL, MS_OP_JUMP_IF_FALSE, MS_OP_LOAD_GLOBAL, MS_OP_JUMP,
    MS_OP_LOAD_GLOBAL, MS_OP_STORE_LOCAL, MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(2, msOpDecodeSAx(msTestCodeAt(fix.proto, 1)));
  MS_ASSERT_EQ(1, msOpDecodeSAx(msTestCodeAt(fix.proto, 3)));
  msTestFixtureDestroy(&fix);

  for (int64_t failAfter = 0; failAfter < 64; ++failAfter) {
    MsTestFixture injection;
    msTestCompileWithFailAfter(&injection, "f(); nil; a if c else b;",
        MS_COMPILE_DEFAULT, failAfter);
    MS_ASSERT_TRUE(injection.result == MS_OK || injection.result == MS_ERROR_OOM);
    msTestFixtureDestroy(&injection);
  }
}

MS_TEST(compiler, comparisonExpansion) {
  static const struct {
    const char* source;
    MsOpCode op;
    bool negate;
  } cases[] = {
    {"a < b;", MS_OP_LT, false}, {"a <= b;", MS_OP_LE, false},
    {"a > b;", MS_OP_GT, false}, {"a >= b;", MS_OP_GE, false},
    {"a == b;", MS_OP_EQ, false}, {"a != b;", MS_OP_NE, false},
    {"a in b;", MS_OP_IN, false}, {"a not in b;", MS_OP_IN, true},
    {"a is b;", MS_OP_IS, false}, {"a is not b;", MS_OP_IS, true}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    MsTestFixture fix;
    msTestCompile(&fix, cases[i].source, MS_COMPILE_DEFAULT);
    MS_ASSERT_EQ(MS_OK, fix.result);
    MS_ASSERT_EQ(cases[i].op, msOpDecodeOp(msTestCodeAt(fix.proto, 2)));
    MS_ASSERT_EQ(cases[i].negate ? MS_OP_NOT : MS_OP_STORE_LOCAL,
        msOpDecodeOp(msTestCodeAt(fix.proto, 3)));
    if (cases[i].negate) {
      MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(msTestCodeAt(fix.proto, 4)));
    }
    msTestFixtureDestroy(&fix);
  }
}

MS_TEST(compiler, positionalCallShape) {
  MsTestFixture fix;
  msTestCompile(&fix, "f(a, 1);", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_CALL, msOpDecodeOp(msTestCodeAt(fix.proto, 3)));
  MS_ASSERT_EQ(2, msOpDecodeA(msTestCodeAt(fix.proto, 3)));
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(3, fix.proto->stackSize);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, functionProtoMetadata) {
  MsTestFixture fix;
  msTestCompile(&fix, "func add(a, b) { return a + b }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_MAKE_FUNCTION, msOpDecodeOp(msTestCodeAt(fix.proto, 0)));
  MS_ASSERT_EQ(MS_OP_STORE_GLOBAL, msOpDecodeOp(msTestCodeAt(fix.proto, 1)));
  const struct MsProto* child = fix.proto != NULL ? msProtoConstProto(fix.proto, 0) : NULL;
  MS_ASSERT_TRUE(child != NULL);
  if (child != NULL) {
    MS_ASSERT_EQ(2, child->paramCount);
    MS_ASSERT_EQ(2, child->localCount);
    MS_ASSERT_EQ(2, child->stackSize);
    MS_ASSERT_TRUE(child->name != NULL);
    if (child->name != NULL) {
      MS_ASSERT_TRUE(strcmp(msStringData(child->name), "add") == 0);
    }
    MS_ASSERT_TRUE(child->sourceFile != NULL);
    if (child->sourceFile != NULL) {
      MS_ASSERT_TRUE(strcmp(msStringData(child->sourceFile), "test.ms") == 0);
    }
    static const MsOpCode expected[] = {
      MS_OP_LOAD_LOCAL, MS_OP_LOAD_LOCAL, MS_OP_ADD, MS_OP_RETURN
    };
    msTestAssertOps(child, expected, sizeof(expected) / sizeof(expected[0]));
    MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(child, 0)));
    MS_ASSERT_EQ(1, msOpDecodeA(msTestCodeAt(child, 1)));
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, lambdaAndImplicitReturn) {
  MsTestFixture fix;
  msTestCompile(&fix, "lambda x: x; func f() { pass }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  const struct MsProto* lambda = fix.proto != NULL ? msProtoConstProto(fix.proto, 0) : NULL;
  const struct MsProto* function = fix.proto != NULL ? msProtoConstProto(fix.proto, 1) : NULL;
  MS_ASSERT_TRUE(lambda != NULL && function != NULL);
  if (lambda != NULL) {
    MS_ASSERT_TRUE(lambda->name != NULL);
    if (lambda->name != NULL) {
      MS_ASSERT_TRUE(strcmp(msStringData(lambda->name), "<lambda>") == 0);
    }
    MS_ASSERT_TRUE(lambda->sourceFile != NULL);
    if (lambda->sourceFile != NULL) {
      MS_ASSERT_TRUE(strcmp(msStringData(lambda->sourceFile), "test.ms") == 0);
    }
    MS_ASSERT_EQ(1, lambda->paramCount);
    MS_ASSERT_EQ(1, lambda->localCount);
    static const MsOpCode expected[] = {MS_OP_LOAD_LOCAL, MS_OP_RETURN};
    msTestAssertOps(lambda, expected, sizeof(expected) / sizeof(expected[0]));
    MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(lambda, 0)));
  }
  MS_ASSERT_EQ(MS_OP_MAKE_LAMBDA, msOpDecodeOp(msTestCodeAt(fix.proto, 0)));
  if (function != NULL) {
    MS_ASSERT_EQ(MS_OP_LOAD_NIL, msOpDecodeOp(msTestCodeAt(function, 0)));
    MS_ASSERT_EQ(MS_OP_RETURN, msOpDecodeOp(msTestCodeAt(function, 1)));
    MS_ASSERT_EQ(1, function->stackSize);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, nestedFunctionProtoTree) {
  MsTestFixture fix;
  msTestCompile(&fix, "func outer() { func inner() { return 1; } }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  const struct MsProto* outer = fix.proto != NULL ? msProtoConstProto(fix.proto, 0) : NULL;
  MS_ASSERT_TRUE(outer != NULL);
  const struct MsProto* inner = outer != NULL ? msProtoConstProto(outer, 0) : NULL;
  MS_ASSERT_TRUE(inner != NULL);
  if (inner != NULL) {
    MS_ASSERT_TRUE(inner->name != NULL);
    if (inner->name != NULL) {
      MS_ASSERT_TRUE(strcmp(msStringData(inner->name), "inner") == 0);
    }
  }
  if (outer != NULL) {
    MS_ASSERT_EQ(MS_OP_MAKE_FUNCTION, msOpDecodeOp(msTestCodeAt(outer, 0)));
    MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(msTestCodeAt(outer, 1)));
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, asyncFlagIsPreserved) {
  MsTestFixture fix;
  msTestCompile(&fix, "async func f() { pass }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  const struct MsProto* child = fix.proto != NULL ? msProtoConstProto(fix.proto, 0) : NULL;
  MS_ASSERT_TRUE(child != NULL && child->isAsync);
  if (child != NULL) {
    MS_ASSERT_TRUE(!child->hasVarArgs && !child->hasKwArgs);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, defaultParamReportsE307) {
  static const char* const sources[] = {
    "func f(a = 1) { return a; }",
    "func f(*args) { pass }",
    "func f(**kwargs) { pass }"
  };
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
    MsTestFixture fix;
    msTestCompile(&fix, sources[i], MS_COMPILE_DEFAULT);
    MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
    MS_ASSERT_TRUE(fix.proto == NULL);
    const struct MsDiag* diag = NULL;
    if (msDiagListCount(&fix.diags) > 0) {
      diag = msDiagListAt(&fix.diags, 0);
    }
    MS_ASSERT_TRUE(diag != NULL);
    if (diag != NULL) {
      MS_ASSERT_EQ(307, (long long)diag->code);
      MS_ASSERT_EQ(1, (long long)diag->line);
      MS_ASSERT_EQ(8, (long long)diag->column);
    }
    msTestFixtureDestroy(&fix);
  }
}

MS_TEST(compiler, outerLocalReportsE310) {
  MsTestFixture fix;
  msTestCompile(&fix, "func outer() { x := 1; func inner() { return x; } }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_TRUE(fix.proto == NULL);
  const struct MsDiag* diag = NULL;
  if (msDiagListCount(&fix.diags) > 0) {
    diag = msDiagListAt(&fix.diags, 0);
  }
  MS_ASSERT_TRUE(diag != NULL);
  if (diag != NULL) {
    MS_ASSERT_EQ(310, (long long)diag->code);
    MS_ASSERT_EQ(1, (long long)diag->line);
    MS_ASSERT_EQ(46, (long long)diag->column);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, replPrintsOnlyLastExpression) {
  MsTestFixture fix;
  msTestCompile(&fix, "1; 2;", MS_COMPILE_REPL);
  MS_ASSERT_EQ(MS_OK, fix.result);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_CONST, MS_OP_STORE_LOCAL, MS_OP_LOAD_CONST,
    MS_OP_PRINT_EXPR, MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  if (fix.proto != NULL && fix.proto->code != NULL) {
    for (int i = 0; i < fix.proto->codeLen; ++i) {
      if (i != 3) {
        MS_ASSERT_TRUE(msOpDecodeOp(fix.proto->code[i]) != MS_OP_PRINT_EXPR);
      }
    }
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, assignmentUsesResolvedStore) {
  MsTestFixture fix;
  msTestCompile(&fix, "x := 0; x = 1; { y := 0; y = 2; }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_STORE_GLOBAL, msOpDecodeOp(msTestCodeAt(fix.proto, 3)));
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(msTestCodeAt(fix.proto, 7)));
  MS_ASSERT_EQ(0, msOpDecodeA(msTestCodeAt(fix.proto, 7)));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, lineTableFollowsStatements) {
  MsTestFixture fix;
  msTestCompile(&fix, "1;\n2;\n", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_TRUE(fix.proto != NULL);
  if (fix.proto != NULL) {
    MS_ASSERT_EQ(1, (long long)msProtoLineAt(fix.proto, 0));
    MS_ASSERT_EQ(2, (long long)msProtoLineAt(fix.proto, 2));
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, unsupportedAndLimitDiagnostics) {
  static const struct {
    const char* source;
    uint32_t code;
    uint32_t column;
  } cases[] = {
    {"b\"x\";", 307, 1},
    {"a < b is not c;", 307, 3},
    {"f(x = 1);", 307, 2},
    {"func f() { return 1, 2; }", 307, 12}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    MsTestFixture fix;
    msTestCompile(&fix, cases[i].source, MS_COMPILE_DEFAULT);
    MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
    MS_ASSERT_TRUE(fix.proto == NULL);
    const struct MsDiag* diag = NULL;
    if (msDiagListCount(&fix.diags) > 0) {
      diag = msDiagListAt(&fix.diags, 0);
    }
    MS_ASSERT_TRUE(diag != NULL);
    if (diag != NULL) {
      MS_ASSERT_EQ((long long)cases[i].code, (long long)diag->code);
      MS_ASSERT_EQ(1, (long long)diag->line);
      MS_ASSERT_EQ((long long)cases[i].column, (long long)diag->column);
    }
    msTestFixtureDestroy(&fix);
  }
}

MS_TEST(compiler, callArgumentLimit) {
  char source[2048];
  size_t used = 0;
  used += (size_t)snprintf(source + used, sizeof(source) - used, "f(");
  for (int i = 0; i < 256; ++i) {
    used += (size_t)snprintf(source + used, sizeof(source) - used, "%s0", i == 0 ? "" : ",");
  }
  snprintf(source + used, sizeof(source) - used, ");");
  MsTestFixture fix;
  msTestCompile(&fix, source, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  const struct MsDiag* diag = NULL;
  if (msDiagListCount(&fix.diags) > 0) {
    diag = msDiagListAt(&fix.diags, 0);
  }
  MS_ASSERT_TRUE(diag != NULL);
  if (diag != NULL) {
    MS_ASSERT_EQ(312, (long long)diag->code);
    MS_ASSERT_EQ(1, (long long)diag->line);
    MS_ASSERT_EQ(2, (long long)diag->column);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, diagnosticCapStopsAtTwenty) {
  char source[160];
  size_t used = 0;
  for (int i = 0; i < MS_DIAG_MAX_COUNT; ++i) {
    used += (size_t)snprintf(source + used, sizeof(source) - used, "b\"\";");
  }
  used += (size_t)snprintf(source + used, sizeof(source) - used, "9223372036854775808;");
  MsTestFixture fix;
  msTestCompile(&fix, source, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(MS_DIAG_MAX_COUNT, (long long)msDiagListCount(&fix.diags));
  for (int i = 0; i < MS_DIAG_MAX_COUNT; ++i) {
    const struct MsDiag* diag = msDiagListAt(&fix.diags, i);
    MS_ASSERT_TRUE(diag != NULL);
    if (diag != NULL) {
      MS_ASSERT_EQ(307, (long long)diag->code);
    }
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, constantFoldHonorsNodeDepth) {
  char source[4096];
  size_t used = 0;
  for (int i = 0; i < 520; ++i) {
    used += (size_t)snprintf(source + used, sizeof(source) - used, "not ");
  }
  snprintf(source + used, sizeof(source) - used, "nil;");
  MsTestFixture fix;
  msTestCompile(&fix, source, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  const struct MsDiag* diag = NULL;
  if (msDiagListCount(&fix.diags) > 0) {
    diag = msDiagListAt(&fix.diags, 0);
  }
  MS_ASSERT_TRUE(diag != NULL);
  if (diag != NULL) {
    MS_ASSERT_EQ(311, (long long)diag->code);
    MS_ASSERT_EQ(1, (long long)diag->line);
    MS_ASSERT_EQ(2041, (long long)diag->column);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, localAndScopeLimits) {
  char locals[8192];
  size_t used = 0;
  used += (size_t)snprintf(locals + used, sizeof(locals) - used, "{");
  for (int i = 0; i < 257; ++i) {
    used += (size_t)snprintf(locals + used, sizeof(locals) - used, "a%d := 0;", i);
  }
  snprintf(locals + used, sizeof(locals) - used, "}");
  MsTestFixture fix;
  msTestCompile(&fix, locals, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  const struct MsDiag* diag = NULL;
  if (msDiagListCount(&fix.diags) > 0) {
    diag = msDiagListAt(&fix.diags, 0);
  }
  MS_ASSERT_TRUE(diag != NULL);
  if (diag != NULL) {
    MS_ASSERT_EQ(306, (long long)diag->code);
  }
  msTestFixtureDestroy(&fix);

  char blocks[512];
  used = 0;
  for (int i = 0; i < 130; ++i) {
    used += (size_t)snprintf(blocks + used, sizeof(blocks) - used, "{");
  }
  used += (size_t)snprintf(blocks + used, sizeof(blocks) - used, "pass;");
  for (int i = 0; i < 130; ++i) {
    used += (size_t)snprintf(blocks + used, sizeof(blocks) - used, "}");
  }
  msTestCompile(&fix, blocks, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  diag = NULL;
  if (msDiagListCount(&fix.diags) > 0) {
    diag = msDiagListAt(&fix.diags, 0);
  }
  MS_ASSERT_TRUE(diag != NULL);
  if (diag != NULL) {
    MS_ASSERT_EQ(306, (long long)diag->code);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, protoConstLimitReportsE309) {
  static const char statement[] = "lambda: nil;\n";
  static char source[65537 * (sizeof(statement) - 1) + 1];
  size_t used = 0;
  for (int i = 0; i < 65537; ++i) {
    memcpy(source + used, statement, sizeof(statement) - 1);
    used += sizeof(statement) - 1;
  }
  source[used] = '\0';
  MsTestFixture fix;
  msTestCompile(&fix, source, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_TRUE(fix.proto == NULL);
  MS_ASSERT_EQ(1, (long long)msDiagListCount(&fix.diags));
  const struct MsDiag* diag = msDiagListAt(&fix.diags, 0);
  MS_ASSERT_TRUE(diag != NULL);
  if (diag != NULL) {
    MS_ASSERT_EQ(309, (long long)diag->code);
    MS_ASSERT_EQ(65537, (long long)diag->line);
    MS_ASSERT_EQ(1, (long long)diag->column);
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, nestedProtoOomPathsHaveNoLeaks) {
  const char* source =
      "while true { while true { while true { "
      "while true { while true { while true { "
      "while true { while true { while true { "
      "for index := 0; index < 1; index++ { "
      "if true { break; } if true { break; } if true { break; } "
      "if true { break; } if true { break; } if true { break; } "
      "if true { break; } if true { break; } if true { break; } "
      "if true { continue; } if true { continue; } if true { continue; } "
      "if true { continue; } if true { continue; } if true { continue; } "
      "if true { continue; } if true { continue; } if true { continue; } "
      "} } } } } } } } } } "
      "func outer() { x := 1; func inner(a) { return a + 2; while false { break; continue; } } }";
  bool sawSuccess = false;
  for (int64_t failAfter = 0; failAfter < 512 && !sawSuccess; ++failAfter) {
    MsTestFixture fix;
    msTestCompileWithFailAfter(&fix, source, MS_COMPILE_DEFAULT, failAfter);
    if (fix.result == MS_OK) {
      sawSuccess = true;
    } else {
      MS_ASSERT_EQ(MS_ERROR_OOM, fix.result);
      MS_ASSERT_TRUE(fix.proto == NULL);
    }
    msTestFixtureDestroy(&fix);
  }
  MS_ASSERT_TRUE(sawSuccess);

  const char* conflict = "func outer() { global callback; func callback() { pass; } }";
  for (int64_t failAfter = 0; failAfter < 16; ++failAfter) {
    MsTestFixture fix;
    msTestCompileWithFailAfter(&fix, conflict, MS_COMPILE_DEFAULT, failAfter);
    MS_ASSERT_EQ(MS_ERROR_OOM, fix.result);
    MS_ASSERT_TRUE(fix.proto == NULL);
    msTestFixtureDestroy(&fix);
  }
}

static const MsTestCase msTestCases[] = {
  {"Compiler.EmptyModule", testcompileremptyModule},
  {"Compiler.ScalarLiteralsAndDedup", testcompilerscalarLiteralsAndDedup},
  {"Compiler.NumericLiteralBasesAndUnderscores", testcompilernumericLiteralBasesAndUnderscores},
  {"Compiler.StringLiteralDecoding", testcompilerstringLiteralDecoding},
  {"Compiler.IntegerOverflowReportsE308", testcompilerintegerOverflowReportsE308},
  {"Compiler.ModuleDeclarationUsesGlobal", testcompilermoduleDeclarationUsesGlobal},
  {"Compiler.SiblingBlocksReuseSlots", testcompilersiblingBlocksReuseSlots},
  {"Compiler.LongIdentifierResolvesByContent", testcompilerlongIdentifierResolvesByContent},
  {"Compiler.DeclarationRhsReadsOuterSlot", testcompilerdeclarationRhsReadsOuterSlot},
  {"Compiler.BinaryOpcodeMapping", testcompilerbinaryOpcodeMapping},
  {"Compiler.ConstantFoldWhitelist", testcompilerconstantFoldWhitelist},
  {"Compiler.Int64MinAndOverflowBehavior", testcompilerint64MinAndOverflowBehavior},
  {"Compiler.NonWhitelistOperationsRemain", testcompilernonWhitelistOperationsRemain},
  {"Compiler.FoldedSignedZeroRemainDistinct", testcompilerfoldedSignedZeroRemainDistinct},
  {"Compiler.LogicalShortCircuitShape", testcompilerlogicalShortCircuitShape},
  {"Compiler.ConstantConditionsSelectOneBranch", testcompilerconstantConditionsSelectOneBranch},
  {"Compiler.ConditionalExpressionJumps", testcompilerconditionalExpressionJumps},
  {"Compiler.ComparisonExpansion", testcompilercomparisonExpansion},
  {"Compiler.PositionalCallShape", testcompilerpositionalCallShape},
  {"Compiler.FunctionProtoMetadata", testcompilerfunctionProtoMetadata},
  {"Compiler.LambdaAndImplicitReturn", testcompilerlambdaAndImplicitReturn},
  {"Compiler.NestedFunctionProtoTree", testcompilernestedFunctionProtoTree},
  {"Compiler.AsyncFlagIsPreserved", testcompilerasyncFlagIsPreserved},
  {"Compiler.DefaultParamReportsE307", testcompilerdefaultParamReportsE307},
  {"Compiler.OuterLocalReportsE310", testcompilerouterLocalReportsE310},
  {"Compiler.ReplPrintsOnlyLastExpression", testcompilerreplPrintsOnlyLastExpression},
  {"Compiler.AssignmentUsesResolvedStore", testcompilerassignmentUsesResolvedStore},
  {"Compiler.LineTableFollowsStatements", testcompilerlineTableFollowsStatements},
  {"Compiler.UnsupportedAndLimitDiagnostics", testcompilerunsupportedAndLimitDiagnostics},
  {"Compiler.CallArgumentLimit", testcompilercallArgumentLimit},
  {"Compiler.DiagnosticCapStopsAtTwenty", testcompilerdiagnosticCapStopsAtTwenty},
  {"Compiler.ConstantFoldHonorsNodeDepth", testcompilerconstantFoldHonorsNodeDepth},
  {"Compiler.LocalAndScopeLimits", testcompilerlocalAndScopeLimits},
  {"Compiler.ProtoConstLimitReportsE309", testcompilerprotoConstLimitReportsE309},
  {"Compiler.NestedProtoOomPathsHaveNoLeaks", testcompilernestedProtoOomPathsHaveNoLeaks},
};

MS_TEST_MAIN(msTestCases)
