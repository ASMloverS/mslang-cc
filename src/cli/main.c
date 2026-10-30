#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <mslang/error.h>
#include <mslang/object.h>
#include <mslang/state.h>
#include <mslang/version.h>

enum {
  MS_EXIT_OK = 0,
  MS_EXIT_RUNTIME = 1,
  MS_EXIT_SYNTAX = 2,
  MS_EXIT_OOM = 3,
  MS_EXIT_USAGE = 64,
  MS_EXIT_IO = 74
};

static void printUsage(void) {
  fprintf(stderr, "usage: mslang <script.ms>\n       mslang --version\n");
}

static int resultExitCode(MsResult result) {
  switch (result) {
    case MS_OK: return MS_EXIT_OK;
    case MS_ERROR_RUNTIME: return MS_EXIT_RUNTIME;
    case MS_ERROR_SYNTAX: return MS_EXIT_SYNTAX;
    case MS_ERROR_OOM: return MS_EXIT_OOM;
    default: return MS_EXIT_RUNTIME;
  }
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("mslang %s\n", msVersionString());
    return MS_EXIT_OK;
  }
  if (argc != 2 || argv[1][0] == '-' || strcmp(argv[1], "test") == 0) {
    if (argc > 1 && (strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "test") == 0)) {
      fprintf(stderr, "mslang: '%s' is not supported yet\n", argv[1]);
    }
    printUsage();
    return MS_EXIT_USAGE;
  }

  const char* path = argv[1];
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "mslang: %s: I/O error (errno %d)\n", path, errno);
    return MS_EXIT_IO;
  }
  fclose(file);

  MsState* L = msNewState();
  if (L == NULL) {
    fprintf(stderr, "mslang: out of memory\n");
    return MS_EXIT_OOM;
  }
  MsResult result = msEvalFile(L, path);
  if (result != MS_OK) {
    MsObject* error = msErrorGet(L);
    const char* message = error == NULL ? NULL : msAsCString(L, error);
    fprintf(stderr, "%s\n", message == NULL ? "mslang: evaluation failed" : message);
  }
  msCloseState(L);
  return resultExitCode(result);
}
