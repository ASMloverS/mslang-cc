#include "object/ms_object.h"

#include <string.h>

#include "ms_test.h"

MS_TEST(object, typeTagCountIs20) {
  MS_ASSERT_EQ(20, MS_TYPE_COUNT);
}

MS_TEST(object, typeTagNamesCompleteDistinctOrdered) {
  static const char* const expected[MS_TYPE_COUNT] = {
    "MS_TYPE_NIL", "MS_TYPE_BOOL", "MS_TYPE_INT", "MS_TYPE_FLOAT",
    "MS_TYPE_STRING", "MS_TYPE_BYTES", "MS_TYPE_LIST", "MS_TYPE_TUPLE",
    "MS_TYPE_DICT", "MS_TYPE_SET", "MS_TYPE_FUNCTION", "MS_TYPE_CLASS",
    "MS_TYPE_INSTANCE", "MS_TYPE_MODULE", "MS_TYPE_CHANNEL", "MS_TYPE_COROUTINE",
    "MS_TYPE_ITERATOR", "MS_TYPE_BOUND_METHOD", "MS_TYPE_C_FUNCTION",
    "MS_TYPE_C_TYPE"
  };
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    const char* name = msTypeTagName((MsTypeTag)i);
    MS_ASSERT_TRUE(name != NULL);
    MS_ASSERT_TRUE(strlen(name) > 0);
    MS_ASSERT_TRUE(strcmp(name, expected[i]) == 0);
    for (int j = i + 1; j < (int)MS_TYPE_COUNT; ++j) {
      MS_ASSERT_TRUE(strcmp(name, msTypeTagName((MsTypeTag)j)) != 0);
    }
  }
  MS_ASSERT_TRUE(strcmp(msTypeTagName((MsTypeTag)MS_TYPE_COUNT), "MS_TYPE_INVALID") == 0);
  MS_ASSERT_TRUE(strcmp(msTypeTagName((MsTypeTag)-1), "MS_TYPE_INVALID") == 0);
}

static const MsTestCase msTestCases[] = {
  {"object.typeTagCountIs20", testobjecttypeTagCountIs20},
  {"object.typeTagNamesCompleteDistinctOrdered", testobjecttypeTagNamesCompleteDistinctOrdered},
};

MS_TEST_MAIN(msTestCases)
