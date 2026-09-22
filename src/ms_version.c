#include <mslang/version.h>

#define MS_STRINGIFY_IMPL(x) #x
#define MS_STRINGIFY(x) MS_STRINGIFY_IMPL(x)

const char* msVersionString(void) {
  return MS_STRINGIFY(MS_VERSION_MAJOR) "." MS_STRINGIFY(MS_VERSION_MINOR) "." MS_STRINGIFY(MS_VERSION_PATCH);
}
