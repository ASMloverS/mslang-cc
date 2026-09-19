#include <stdio.h>

#include <mslang/version.h>

int main(void) {
  printf("embed-example: mslang %s\n", msVersionString());
  return 0;
}
