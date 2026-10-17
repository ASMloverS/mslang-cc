#include "compiler/ms_compiler.h"

#include <math.h>
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
};

MS_TEST_MAIN(msTestCases)
