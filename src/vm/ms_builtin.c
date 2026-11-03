#include "vm/ms_builtin.h"

#include <stdbool.h>
#include <stdio.h>

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

static struct MsObject* msBuiltinAssert(MsState* L, int64_t argc, struct MsObject** argv) {
  if (argc < 1 || argc > 2) {
    msRaiseRuntimeError(L, "assert() expects one or two arguments");
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

void msBuiltinRegisterMinimal(MsState* L) {
  msBuiltinRegister(L, "print", msBuiltinPrint);
  if (L->errorResult == MS_OK) {
    msBuiltinRegister(L, "assert", msBuiltinAssert);
  }
}
