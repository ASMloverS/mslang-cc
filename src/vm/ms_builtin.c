#include "vm/ms_builtin.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "object/ms_string.h"
#include "vm/ms_function.h"
#include "vm/ms_state_internal.h"

struct MsBuiltinBuffer {
  char* data;
  size_t len;
  size_t cap;
};

static bool msBuiltinBufferAppend(MsState* L, struct MsBuiltinBuffer* buffer,
    const char* data, size_t dataLen) {
  if (dataLen > SIZE_MAX - buffer->len) {
    msStateSetOom(L);
    return false;
  }
  size_t required = buffer->len + dataLen;
  if (required > buffer->cap) {
    size_t newCap = buffer->cap == 0 ? 64 : buffer->cap;
    while (newCap < required) {
      if (newCap > SIZE_MAX / 2) {
        newCap = required;
        break;
      }
      newCap *= 2;
    }
    char* newData = (char*)msRealloc(buffer->data, newCap);
    if (newData == NULL) {
      msStateSetOom(L);
      return false;
    }
    buffer->data = newData;
    buffer->cap = newCap;
  }
  if (dataLen > 0) {
    memcpy(buffer->data + buffer->len, data, dataLen);
  }
  buffer->len = required;
  return true;
}

static bool msBuiltinBufferAppendChar(MsState* L, struct MsBuiltinBuffer* buffer, char c) {
  return msBuiltinBufferAppend(L, buffer, &c, 1);
}

static struct MsString* msBuiltinBufferFinish(MsState* L, struct MsBuiltinBuffer* buffer) {
  struct MsString* string = msStringNew(&L->heap, buffer->data, buffer->len);
  if (string == NULL) {
    msStateSetOom(L);
  }
  msFree(buffer->data);
  buffer->data = NULL;
  buffer->len = 0;
  buffer->cap = 0;
  return string;
}

static bool msBuiltinWriteFloat(MsState* L, struct MsBuiltinBuffer* buffer, double value) {
  char candidate[64];
  char selected[64] = {0};
  for (int precision = 1; precision <= 17; ++precision) {
    int written = snprintf(candidate, sizeof(candidate), "%.*g", precision, value);
    if (written < 0 || (size_t)written >= sizeof(candidate)) {
      msRaiseRuntimeError(L, "failed to format float");
      return false;
    }
    memcpy(selected, candidate, (size_t)written + 1);
    char* end = NULL;
    double parsed = strtod(candidate, &end);
    if (*end == '\0' && memcmp(&parsed, &value, sizeof(value)) == 0) {
      break;
    }
  }
  if (!msBuiltinBufferAppend(L, buffer, selected, strlen(selected))) {
    return false;
  }
  if (isfinite(value) && strpbrk(selected, ".eE") == NULL) {
    return msBuiltinBufferAppend(L, buffer, ".0", 2);
  }
  return true;
}

static bool msBuiltinWriteString(MsState* L, struct MsBuiltinBuffer* buffer,
    const struct MsString* string, bool repr) {
  if (!repr) {
    return msBuiltinBufferAppend(L, buffer, msStringData(string), msStringLen(string));
  }
  if (!msBuiltinBufferAppendChar(L, buffer, '"')) {
    return false;
  }
  static const char hex[] = "0123456789abcdef";
  const unsigned char* data = (const unsigned char*)msStringData(string);
  for (size_t i = 0; i < msStringLen(string); ++i) {
    unsigned char byte = data[i];
    const char* escape = NULL;
    switch (byte) {
      case '\\': escape = "\\\\"; break;
      case '"': escape = "\\\""; break;
      case '\n': escape = "\\n"; break;
      case '\r': escape = "\\r"; break;
      case '\t': escape = "\\t"; break;
      default: break;
    }
    if (escape != NULL) {
      if (!msBuiltinBufferAppend(L, buffer, escape, 2)) {
        return false;
      }
    } else if (byte < 0x20 || byte == 0x7f) {
      char control[] = {'\\', 'x', hex[byte >> 4], hex[byte & 0x0f]};
      if (!msBuiltinBufferAppend(L, buffer, control, sizeof(control))) {
        return false;
      }
    } else if (!msBuiltinBufferAppendChar(L, buffer, (char)byte)) {
      return false;
    }
  }
  return msBuiltinBufferAppendChar(L, buffer, '"');
}

static bool msBuiltinWriteValue(MsState* L, struct MsBuiltinBuffer* buffer,
    const struct MsObject* value, bool repr) {
  switch (msObjectTag(value)) {
    case MS_TYPE_NIL:
      return msBuiltinBufferAppend(L, buffer, "nil", 3);
    case MS_TYPE_BOOL:
      if (msBoolValue(value)) {
        return msBuiltinBufferAppend(L, buffer, "true", 4);
      }
      return msBuiltinBufferAppend(L, buffer, "false", 5);
    case MS_TYPE_INT: {
      char text[32];
      int len = snprintf(text, sizeof(text), "%lld", (long long)msIntWordValue(value));
      return msBuiltinBufferAppend(L, buffer, text, (size_t)len);
    }
    case MS_TYPE_FLOAT:
      return msBuiltinWriteFloat(L, buffer, msFloatValue(value));
    case MS_TYPE_STRING:
      return msBuiltinWriteString(L, buffer, (const struct MsString*)value, repr);
    case MS_TYPE_C_FUNCTION: {
      const struct MsCFunctionObject* function = (const struct MsCFunctionObject*)value;
      return msBuiltinBufferAppend(L, buffer, "<builtin function ", 18)
          && msBuiltinBufferAppend(L, buffer, msStringData(function->name), msStringLen(function->name))
          && msBuiltinBufferAppendChar(L, buffer, '>');
    }
    case MS_TYPE_CLASS: {
      const struct MsType* type = (const struct MsType*)value;
      return msBuiltinBufferAppend(L, buffer, "<type '", 7)
          && msBuiltinBufferAppend(L, buffer, msStringData(type->name), msStringLen(type->name))
          && msBuiltinBufferAppend(L, buffer, "'>", 2);
    }
    default:
      return msBuiltinBufferAppendChar(L, buffer, '<')
          && msBuiltinBufferAppend(L, buffer, msTypeTagName(msObjectTag(value)),
              strlen(msTypeTagName(msObjectTag(value))))
          && msBuiltinBufferAppendChar(L, buffer, '>');
  }
}

static struct MsString* msBuiltinStringValue(MsState* L, const struct MsObject* value) {
  struct MsBuiltinBuffer buffer = {0};
  if (!msBuiltinWriteValue(L, &buffer, value, false)) {
    msFree(buffer.data);
    return NULL;
  }
  return msBuiltinBufferFinish(L, &buffer);
}

static bool msBuiltinWriteStdout(const char* data, size_t len) {
  return fwrite(data, 1, len, stdout) == len;
}

static bool msBuiltinPrintValue(MsState* L, const struct MsObject* value) {
  struct MsString* string = msBuiltinStringValue(L, value);
  return string != NULL && msBuiltinWriteStdout(msStringData(string), msStringLen(string));
}

enum MsBuiltinPrintOption {
  MS_BUILTIN_PRINT_OPTION_UNKNOWN = -1,
  MS_BUILTIN_PRINT_SEP,
  MS_BUILTIN_PRINT_END,
  MS_BUILTIN_PRINT_FILE,
  MS_BUILTIN_PRINT_FLUSH,
  MS_BUILTIN_PRINT_OPTION_COUNT
};

struct MsBuiltinPrintOptions {
  const char* sep;
  size_t sepLen;
  const char* end;
  size_t endLen;
  bool flush;
};

static bool msBuiltinStringIs(const struct MsString* string, const char* text) {
  size_t len = strlen(text);
  return msStringLen(string) == len && memcmp(msStringData(string), text, len) == 0;
}

static enum MsBuiltinPrintOption msBuiltinPrintOption(const struct MsString* name) {
  if (msBuiltinStringIs(name, "sep")) {
    return MS_BUILTIN_PRINT_SEP;
  }
  if (msBuiltinStringIs(name, "end")) {
    return MS_BUILTIN_PRINT_END;
  }
  if (msBuiltinStringIs(name, "file")) {
    return MS_BUILTIN_PRINT_FILE;
  }
  if (msBuiltinStringIs(name, "flush")) {
    return MS_BUILTIN_PRINT_FLUSH;
  }
  return MS_BUILTIN_PRINT_OPTION_UNKNOWN;
}

static bool msBuiltinParsePrintOptions(MsState* L, int64_t argc, struct MsObject** argv,
    struct MsBuiltinPrintOptions* options, int64_t* valueCount) {
  *options = (struct MsBuiltinPrintOptions){
    .sep = " ",
    .sepLen = 1,
    .end = "\n",
    .endLen = 1,
    .flush = false,
  };
  int64_t keywordCount = L->cKeywordCount;
  if (argc < 0 || keywordCount < 0 || keywordCount > argc / 2) {
    msRaiseRuntimeError(L, "print() invalid C keyword argument count");
    return false;
  }
  *valueCount = argc - 2 * keywordCount;
  bool seen[MS_BUILTIN_PRINT_OPTION_COUNT] = {false};
  for (int64_t i = 0; i < keywordCount; ++i) {
    struct MsObject* nameObject = argv[*valueCount + 2 * i];
    if (msObjectTag(nameObject) != MS_TYPE_STRING) {
      msRaiseRuntimeError(L, "print() C keyword name must be str");
      return false;
    }
    const struct MsString* name = (const struct MsString*)nameObject;
    enum MsBuiltinPrintOption option = msBuiltinPrintOption(name);
    if (option == MS_BUILTIN_PRINT_OPTION_UNKNOWN) {
      msRaiseTypeError(L, "print() got an unexpected keyword argument '%s'", msStringData(name));
      return false;
    }
    if (seen[option]) {
      msRaiseTypeError(L, "print() got multiple values for argument '%s'", msStringData(name));
      return false;
    }
    seen[option] = true;
    struct MsObject* value = argv[*valueCount + 2 * i + 1];
    if (option == MS_BUILTIN_PRINT_SEP || option == MS_BUILTIN_PRINT_END) {
      if (msObjectTag(value) != MS_TYPE_STRING) {
        msRaiseTypeError(L, "print() %s must be str", msStringData(name));
        return false;
      }
      const struct MsString* string = (const struct MsString*)value;
      if (option == MS_BUILTIN_PRINT_SEP) {
        options->sep = msStringData(string);
        options->sepLen = msStringLen(string);
      } else {
        options->end = msStringData(string);
        options->endLen = msStringLen(string);
      }
    } else if (option == MS_BUILTIN_PRINT_FILE) {
      if (msObjectTag(value) != MS_TYPE_NIL) {
        msRaiseTypeError(L, "print() file must be nil");
        return false;
      }
    } else {
      options->flush = msObjectIsTruthy(value);
    }
  }
  return true;
}

static struct MsObject* msBuiltinPrint(MsState* L, int64_t argc, struct MsObject** argv) {
  struct MsBuiltinPrintOptions options;
  int64_t valueCount = 0;
  if (!msBuiltinParsePrintOptions(L, argc, argv, &options, &valueCount)) {
    return NULL;
  }
  for (int64_t i = 0; i < valueCount; ++i) {
    if ((i > 0 && !msBuiltinWriteStdout(options.sep, options.sepLen))
        || !msBuiltinPrintValue(L, argv[i])) {
      if (L->errorResult == MS_OK) {
        msRaiseRuntimeError(L, "failed to write to stdout");
      }
      return NULL;
    }
  }
  if (!msBuiltinWriteStdout(options.end, options.endLen)
      || (options.flush && fflush(stdout) == EOF)) {
    msRaiseRuntimeError(L, "failed to write to stdout");
    return NULL;
  }
  return msNewNil(L);
}

static bool msBuiltinCheckArgc(MsState* L, const char* name, int64_t argc, int64_t min, int64_t max) {
  if (argc >= min && (max < 0 || argc <= max)) {
    return true;
  }
  if (argc < min) {
    msRaiseTypeError(L, "%s() takes at least %lld argument(s)", name, (long long)min);
  } else {
    msRaiseTypeError(L, "%s() takes at most %lld argument(s)", name, (long long)max);
  }
  return false;
}

static struct MsObject* msBuiltinAssert(MsState* L, int64_t argc, struct MsObject** argv) {
  if (!msBuiltinCheckArgc(L, "assert", argc, 1, 2)) {
    return NULL;
  }
  if (msObjectIsTruthy(argv[0])) {
    return msNewNil(L);
  }
  if (argc == 1) {
    msRaiseAssertionError(L, "assertion failed");
    return NULL;
  }
  if (msObjectTag(argv[1]) == MS_TYPE_STRING) {
    struct MsString* message = (struct MsString*)argv[1];
    msRaiseAssertionError(L, "%s", msStringData(message));
    return NULL;
  }
  struct MsString* message = msBuiltinStringValue(L, argv[1]);
  if (message != NULL) {
    msRaiseAssertionError(L, "%s", msStringData(message));
  }
  return NULL;
}

static struct MsObject* msBuiltinLen(MsState* L, int64_t argc, struct MsObject** argv) {
  if (!msBuiltinCheckArgc(L, "len", argc, 1, 1)) {
    return NULL;
  }
  if (msObjectTag(argv[0]) != MS_TYPE_STRING) {
    const struct MsString* name = msHeapType(&L->heap, msObjectTag(argv[0]))->name;
    msRaiseTypeError(L, "object of type '%s' has no len()", msStringData(name));
    return NULL;
  }
  const struct MsString* string = (const struct MsString*)argv[0];
  const unsigned char* data = (const unsigned char*)msStringData(string);
  int64_t length = 0;
  for (size_t i = 0; i < msStringLen(string); ++i) {
    if ((data[i] & 0xC0) != 0x80) {
      ++length;
    }
  }
  return msNewInt(L, length);
}

static struct MsObject* msBuiltinType(MsState* L, int64_t argc, struct MsObject** argv) {
  if (!msBuiltinCheckArgc(L, "type", argc, 1, 1)) {
    return NULL;
  }
  return (struct MsObject*)argv[0]->header.type;
}

static struct MsObject* msBuiltinBool(MsState* L, int64_t argc, struct MsObject** argv) {
  if (!msBuiltinCheckArgc(L, "bool", argc, 1, 1)) {
    return NULL;
  }
  return msNewBool(L, msObjectIsTruthy(argv[0]));
}

static struct MsObject* msBuiltinStr(MsState* L, int64_t argc, struct MsObject** argv) {
  if (!msBuiltinCheckArgc(L, "str", argc, 1, 1)) {
    return NULL;
  }
  return (struct MsObject*)msBuiltinStringValue(L, argv[0]);
}

struct MsBuiltinEntry {
  const char* name;
  MsCFunction func;
};

static const struct MsBuiltinEntry msBuiltinTable[] = {
  {"print", msBuiltinPrint},
  {"assert", msBuiltinAssert},
  {"len", msBuiltinLen},
  {"type", msBuiltinType},
  {"bool", msBuiltinBool},
  {"str", msBuiltinStr},
};

static void msBuiltinRegister(MsState* L, const char* name, MsCFunction function) {
  struct MsString* nameString = msStringFromCStr(&L->heap, name);
  if (nameString == NULL) {
    msStateSetOom(L);
    return;
  }
  struct MsObject* object = msNewCFunction(L, nameString, function);
  if (object != NULL) {
    msStateSetGlobal(L, nameString, object);
  }
}

void msBuiltinRegisterAll(MsState* L) {
  for (size_t i = 0; i < MS_ARRAY_LEN(msBuiltinTable); ++i) {
    msBuiltinRegister(L, msBuiltinTable[i].name, msBuiltinTable[i].func);
    if (L->errorResult != MS_OK) {
      return;
    }
  }
}
