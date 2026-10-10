#include <stdbool.h>

#include "core/ms_memory.h"
#include "vm/ms_proto.h"
#include "ms_test.h"

MS_TEST(proto, builderInitZeroed) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_TRUE(b.proto.code == NULL);
  MS_ASSERT_TRUE(b.proto.consts == NULL);
  MS_ASSERT_TRUE(b.proto.tryBlocks == NULL);
  MS_ASSERT_TRUE(b.proto.lines == NULL);
  MS_ASSERT_EQ(0, b.proto.codeLen);
  MS_ASSERT_EQ(0, b.proto.constsLen);
  MS_ASSERT_EQ(0, b.proto.tryBlocksLen);
  MS_ASSERT_EQ(0, b.proto.linesLen);
  MS_ASSERT_EQ(0, b.proto.paramCount);
  MS_ASSERT_EQ(0, b.proto.localCount);
  MS_ASSERT_EQ(0, b.proto.stackSize);
  MS_ASSERT_TRUE(b.proto.name == NULL);
  MS_ASSERT_TRUE(b.proto.sourceFile == NULL);
  MS_ASSERT_TRUE(!b.proto.isAsync);
  MS_ASSERT_TRUE(!b.proto.hasVarArgs);
  MS_ASSERT_TRUE(!b.proto.hasKwArgs);
  MS_ASSERT_EQ(0, b.codeCap);
  MS_ASSERT_EQ(0, b.constsCap);
  MS_ASSERT_EQ(0, b.tryCap);
  MS_ASSERT_EQ(0, b.linesCap);
  MS_ASSERT_TRUE(!b.hasPendingLine);
  MS_ASSERT_EQ(MS_OK, b.error);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, builderDestroyEmptyNoLeaks) {
  msMemResetStats();
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msProtoBuilderDestroy(&b);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.allocCount);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

static const MsTestCase msTestCases[] = {
  {"Proto.BuilderInitZeroed", testprotobuilderInitZeroed},
  {"Proto.BuilderDestroyEmptyNoLeaks", testprotobuilderDestroyEmptyNoLeaks}
};

MS_TEST_MAIN(msTestCases)
