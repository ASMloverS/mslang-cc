#include "core/ms_common.h"

const char* msResultName(MsResult result) {
  switch (result) {
    case MS_OK:
      return "MS_OK";
    case MS_ERROR_RUNTIME:
      return "MS_ERROR_RUNTIME";
    case MS_ERROR_SYNTAX:
      return "MS_ERROR_SYNTAX";
    case MS_ERROR_OOM:
      return "MS_ERROR_OOM";
    default:
      return "MS_RESULT_UNKNOWN";
  }
}
