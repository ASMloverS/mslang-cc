#include "object/ms_object.h"

static const char* const msTypeTagNames[MS_TYPE_COUNT] = {
  "MS_TYPE_NIL", "MS_TYPE_BOOL", "MS_TYPE_INT", "MS_TYPE_FLOAT",
  "MS_TYPE_STRING", "MS_TYPE_BYTES", "MS_TYPE_LIST", "MS_TYPE_TUPLE",
  "MS_TYPE_DICT", "MS_TYPE_SET", "MS_TYPE_FUNCTION", "MS_TYPE_CLASS",
  "MS_TYPE_INSTANCE", "MS_TYPE_MODULE", "MS_TYPE_CHANNEL", "MS_TYPE_COROUTINE",
  "MS_TYPE_ITERATOR", "MS_TYPE_BOUND_METHOD", "MS_TYPE_C_FUNCTION",
  "MS_TYPE_C_TYPE"
};

// Script-visible names per 02-types section 1. Used by msHeapInit (task: heap init).
static const char* const msTypeScriptNames[MS_TYPE_COUNT] = {
  "NilType", "bool", "int", "float", "str", "bytes", "list", "tuple",
  "dict", "set", "function", "type", "instance", "module", "chan",
  "coroutine", "iterator", "bound method", "cfunction", "ctype"
};

const char* msTypeTagName(MsTypeTag tag) {
  if (tag < 0 || tag >= MS_TYPE_COUNT) {
    return "MS_TYPE_INVALID";
  }
  return msTypeTagNames[tag];
}
