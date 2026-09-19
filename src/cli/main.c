#include <stdio.h>
#include <string.h>

#include <mslang/version.h>

static void printUsage(void) {
  fprintf(stderr, "usage: mslang [--version]\n");
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("mslang %s\n", msVersionString());
    return 0;
  }
  printUsage();
  return 2;
}
