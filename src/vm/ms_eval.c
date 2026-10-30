#include <mslang/state.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "compiler/ms_compiler.h"
#include "core/ms_diag.h"
#include "core/ms_memory.h"
#include "lexer/ms_lexer.h"
#include "parser/ms_ast.h"
#include "parser/ms_parser.h"
#include "vm/ms_proto.h"
#include "vm/ms_state_internal.h"
#include "vm/ms_vm.h"

static char* msEvalReadFile(MsState* L, const char* path, size_t* outLen) {
  *outLen = 0;
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    msRaiseRuntimeError(L, "%s: I/O error (errno %d)", path, errno);
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    msRaiseRuntimeError(L, "%s: I/O error (errno %d)", path, errno);
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    msRaiseRuntimeError(L, "%s: I/O error (errno %d)", path, errno);
    fclose(file);
    return NULL;
  }
  char* source = msAlloc((size_t)size + 1);
  if (source == NULL) {
    msStateSetOom(L);
    fclose(file);
    return NULL;
  }
  size_t readLen = fread(source, 1, (size_t)size, file);
  if (readLen != (size_t)size) {
    msRaiseRuntimeError(L, "%s: failed to read file", path);
    msFree(source);
    fclose(file);
    return NULL;
  }
  fclose(file);
  source[readLen] = '\0';
  *outLen = readLen;
  return source;
}

static void msEvalSetDiagnostics(MsState* L, const struct MsDiagList* diags) {
  size_t count = msDiagListCount(diags);
  if (count == 0) {
    msStateSetError(L, MS_ERROR_SYNTAX, "syntax error", strlen("syntax error"));
    return;
  }
  size_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    total += (size_t)msDiagFormat(msDiagListAt(diags, i), NULL, 0);
    if (i + 1 < count) {
      ++total;
    }
  }
  char* message = msAlloc(total + 1);
  if (message == NULL) {
    msStateSetOom(L);
    return;
  }
  size_t offset = 0;
  for (size_t i = 0; i < count; ++i) {
    int written = msDiagFormat(msDiagListAt(diags, i), message + offset, total + 1 - offset);
    offset += (size_t)written;
    if (i + 1 < count) {
      message[offset++] = '\n';
    }
  }
  message[offset] = '\0';
  msStateSetError(L, MS_ERROR_SYNTAX, message, total);
  msFree(message);
}

MsResult msEvalFile(MsState* L, const char* path) {
  if (L == NULL) {
    return MS_ERROR_RUNTIME;
  }
  if (path == NULL) {
    msRaiseRuntimeError(L, "msEvalFile() requires a path");
    return MS_ERROR_RUNTIME;
  }
  msStateClearError(L);
  size_t sourceLen = 0;
  char* source = msEvalReadFile(L, path, &sourceLen);
  if (source == NULL) {
    return msStateErrorResult(L);
  }

  struct MsDiagList diags;
  MsResult result = msDiagListInit(&diags, path);
  if (result != MS_OK) {
    msStateSetOom(L);
    msFree(source);
    return result;
  }
  struct MsLexer lexer;
  struct MsAstArena arena;
  struct MsParser parser;
  struct MsCompiler compiler;
  struct MsAst* root = NULL;
  struct MsProto* proto = NULL;
  msLexerInit(&lexer, source, sourceLen, path, &diags);
  msAstArenaInit(&arena);
  msParserInit(&parser, &lexer, &arena, &diags);
  msCompilerInit(&compiler, &L->heap, &diags, path, MS_COMPILE_DEFAULT);

  result = msParserParse(&parser, &root);
  if (result == MS_OK) {
    result = msCompilerCompile(&compiler, root, &proto);
  }
  msCompilerDestroy(&compiler);
  msAstArenaDestroy(&arena);
  msLexerDestroy(&lexer);
  msFree(source);

  if (result == MS_ERROR_SYNTAX) {
    msEvalSetDiagnostics(L, &diags);
    result = msStateErrorResult(L);
  } else if (result != MS_OK && msStateErrorResult(L) == MS_OK) {
    msStateSetOom(L);
  }
  msDiagListDestroy(&diags);
  if (result == MS_OK) {
    struct MsObject* ignored = NULL;
    result = msCallProto(L, &L->main, proto, 0, NULL, &ignored);
  }
  msProtoFree(proto);
  return result;
}
