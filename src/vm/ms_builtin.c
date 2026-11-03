#include "vm/ms_builtin.h"

#include <stdbool.h>
#include <stdio.h>

#include "core/ms_common.h"
#include "object/ms_string.h"
#include "vm/ms_function.h"
#include "vm/ms_state_internal.h"

static int msBuiltinFormatValue(const struct MsObject* value, char* out, size_t outSize) {
  switch (msObjectTag(value)) {
    case MS_TYPE_NIL: return snprintf(out, outSize, "nil");
    case MS_TYPE_BOOL: return snprintf(out, outSize, "%s", msBoolValue(value) ? "true" : "false");
    case MS_TYPE_INT: return snprintf(out, outSize, "%lld", (long long)msIntWordValue(value));
    case MS_TYPE_FLOAT: return snprintf(out, outSize, "%.17g", msFloatValue(value));
    case MS_TYPE_STRING: return snprintf(out, outSize, "%s", msStringData((const struct MsString*)value));
    default: return snprintf(out, outSize, "<%s>", msTypeTagName(msObjectTag(value)));
  }
}

static bool msBuiltinPrintValue(const struct MsObject* value) {
  if (msObjectTag(value) == MS_TYPE_STRING) {
    const struct MsString* string = (const struct MsString*)value;
    return fwrite(msStringData(string), 1, msStringLen(string), stdout) == msStringLen(string);
  }
  char buffer[128];
  msBuiltinFormatValue(value, buffer, sizeof(buffer));
  return fputs(buffer, stdout) != EOF;
}

static struct MsObject* msBuiltinPrint(MsState* L, int64_t argc, struct MsObject** argv) {
  for (int64_t i = 0; i < argc; ++i) {
    if ((i > 0 && fputc(' ', stdout) == EOF) || !msBuiltinPrintValue(argv[i])) {
      msRaiseRuntimeError(L, "failed to write to stdout");
      return NULL;
    }
  }
  if (fputc('\n', stdout) == EOF) {
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
  char buffer[128];
  msBuiltinFormatValue(argv[1], buffer, sizeof(buffer));
  msRaiseAssertionError(L, "%s", buffer);
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
