#ifndef MSLANG_SRC_COMPILER_MS_COMPILER_H_
#define MSLANG_SRC_COMPILER_MS_COMPILER_H_

#include <stdint.h>

#include <mslang/error.h>

#include "core/ms_diag.h"

struct MsAst;
struct MsFuncCtx;
struct MsHeap;
struct MsProto;

typedef enum {
  MS_COMPILE_DEFAULT = 0,
  MS_COMPILE_REPL = 1u << 0
} MsCompileFlags;

struct MsCompiler {
  struct MsHeap* heap;
  struct MsDiagList* diags;
  const char* sourceFile;
  uint32_t flags;
  struct MsFuncCtx* fn;
  int nodeDepth;
  MsResult sticky;
};

void msCompilerInit(struct MsCompiler* c, struct MsHeap* heap, struct MsDiagList* diags,
    const char* sourceFile, uint32_t flags);
void msCompilerDestroy(struct MsCompiler* c);
MsResult msCompilerCompile(struct MsCompiler* c, const struct MsAst* root, struct MsProto** out);

#endif  // MSLANG_SRC_COMPILER_MS_COMPILER_H_
