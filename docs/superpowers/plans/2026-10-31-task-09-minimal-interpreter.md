# Task 09 Minimal Interpreter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a standalone `mslang script.ms` interpreter with minimal builtins, script-level tests, and the required CLI exit-code contract.

**Architecture:** `msEvalFile` owns a thin source-to-Proto pipeline over the existing lexer, parser, compiler, and VM. `MsState` retains ownership of runtime objects and globals; focused builtin, evaluation, CLI, and test-driver modules provide process-independent behavior.

**Tech Stack:** C11, CMake/CTest, Python 3 standard library, MS smoke scripts.

---

## File map

- Create `src/vm/ms_builtin.h` and `src/vm/ms_builtin.c` for `print`/`assert` registration and behavior.
- Create `src/vm/ms_eval.c` for file reading, diagnostics, compilation, execution, and cleanup.
- Create `include/mslang/object.h` for the minimal opaque object/string conversion API.
- Modify `include/mslang/state.h`, `include/mslang/error.h`, `src/vm/ms_state_internal.h`, and `src/vm/ms_state.c` for evaluation and error access.
- Modify `src/compiler/ms_compiler.c` for the minimal `if` and three-part `for` paths required by task 09.
- Replace `src/cli/main.c` with the script/version CLI.
- Extend `run_tests.py`, add `tests/ms/smoke/`, and register script tests in `CMakeLists.txt`.
- Adjust `tests/c/test_vm.c` expectations because new states now contain two builtins.
- Update the task 09 status and acceptance record only after the verification commands pass.

### Task 1: Script fixtures and test driver

**Files:**
- Create: `tests/ms/smoke/hello.ms`, `hello.out`
- Create: `tests/ms/smoke/arithmetic.ms`, `arithmetic.out`
- Create: `tests/ms/smoke/control_flow.ms`, `control_flow.out`
- Create: `tests/ms/smoke/syntax_error.ms`, `syntax_error.exit`
- Create: `tests/ms/smoke/assert_failure.ms`, `assert_failure.exit`
- Modify: `run_tests.py`

- [ ] **Step 1: Add the positive smoke scripts**

`hello.ms`:
```ms
print("hello")
```

`hello.out`:
```text
hello
```

`arithmetic.ms`:
```ms
x := 1 + 2 * 3
assert(x == 7)
assert(10 / 4 == 2.5)
assert(7 % 3 == 1)
print("arithmetic ok")
```

`arithmetic.out`:
```text
arithmetic ok
```

`control_flow.ms`:
```ms
sum := 0
for i := 1; i <= 10; i += 1 {
    sum += i
}
if sum == 55 {
    assert(true)
} else {
    assert(false, "incorrect sum")
}
assert(sum == 55)
print("control flow ok")
```

`control_flow.out`:
```text
control flow ok
```

- [ ] **Step 2: Add the negative smoke scripts**

`syntax_error.ms`:
```ms
value :=
```

`syntax_error.exit`:
```text
2
```

`assert_failure.ms`:
```ms
assert(false, "expected failure")
```

`assert_failure.exit`:
```text
1
```

- [ ] **Step 3: Implement companion files, environment selection, and failure details in `run_tests.py`**

Replace the file with:
```python
"""Unified test driver: discovers tests/ms/**/*.ms and runs them via mslang."""

import argparse
import difflib
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
TESTS_MS_DIR = REPO_ROOT / "tests" / "ms"


def find_mslang_binary(explicit: Path | None) -> Path | None:
    """Resolves the CLI from an argument, MSLANG_BIN, or build output."""
    selected = explicit
    if selected is None and os.environ.get("MSLANG_BIN"):
        selected = Path(os.environ["MSLANG_BIN"])
    if selected is not None:
        return selected if selected.is_file() else None
    candidates: list[Path] = []
    for subdir in ("", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
        for name in ("mslang", "mslang.exe"):
            candidates.append(REPO_ROOT / "build" / subdir / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def discover_tests(pattern: str) -> list[Path]:
    """Returns sorted tests/ms/**/*.ms paths matching the glob filter."""
    if not TESTS_MS_DIR.is_dir():
        return []
    return sorted(
        path for path in TESTS_MS_DIR.rglob("*.ms") if path.match(pattern)
    )


def expected_exit_code(script: Path) -> int:
    """Reads an optional .exit companion, defaulting to success."""
    companion = script.with_suffix(".exit")
    if not companion.is_file():
        return 0
    return int(companion.read_text(encoding="utf-8").strip())


def run_test(mslang: Path, script: Path) -> tuple[bool, str]:
    """Runs one script and returns its pass status and failure details."""
    expected_exit = expected_exit_code(script)
    result = subprocess.run(
        [str(mslang), str(script)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    failures: list[str] = []
    if result.returncode != expected_exit:
        failures.append(
            f"exit code: expected {expected_exit}, got {result.returncode}"
        )
    output_companion = script.with_suffix(".out")
    if output_companion.is_file():
        expected_output = output_companion.read_text(encoding="utf-8")
        if result.stdout != expected_output:
            diff = "".join(
                difflib.unified_diff(
                    expected_output.splitlines(keepends=True),
                    result.stdout.splitlines(keepends=True),
                    fromfile="expected stdout",
                    tofile="actual stdout",
                )
            )
            failures.append(f"stdout difference:\n{diff}")
    if failures and result.stderr:
        failures.append(f"stderr:\n{result.stderr}")
    return not failures, "\n".join(failures)


def main(argv: list[str] | None = None) -> int:
    """Entry point; returns the process exit code."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mslang",
        type=Path,
        default=None,
        help="path to the mslang interpreter binary",
    )
    parser.add_argument(
        "--filter",
        default="*.ms",
        help="glob filter for test scripts (default: *.ms)",
    )
    args = parser.parse_args(argv)

    tests = discover_tests(args.filter)
    if not tests:
        print("no tests found")
        return 0

    mslang = find_mslang_binary(args.mslang)
    if mslang is None:
        print(
            "error: mslang binary not found; build first or pass --mslang",
            file=sys.stderr,
        )
        return 1

    failed: list[Path] = []
    for script in tests:
        passed, details = run_test(mslang, script)
        relative = script.relative_to(REPO_ROOT)
        if passed:
            print(f"PASS {relative}")
        else:
            failed.append(script)
            print(f"FAIL {relative}")
            print(details, file=sys.stderr)

    print(f"{len(tests) - len(failed)} of {len(tests)} tests passed")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the script tests and verify the old CLI fails**

Run: `python run_tests.py --mslang build/Debug/mslang.exe`
Expected: the suite fails; the old CLI rejects all positive scripts, and its accidental exit-2 match for the syntax fixture does not constitute end-to-end coverage.

- [ ] **Step 5: Commit the test fixtures and driver**

```bash
git add run_tests.py tests/ms
git commit -m '✅ test(cli): add interpreter smoke scripts'
```

### Task 2: Minimal builtins and error access

**Files:**
- Create: `include/mslang/object.h`, `src/vm/ms_builtin.h`, `src/vm/ms_builtin.c`
- Modify: `include/mslang/error.h`, `include/mslang/state.h`
- Modify: `src/vm/ms_state_internal.h`, `src/vm/ms_state.c`, `CMakeLists.txt`
- Test: `tests/c/test_vm.c`

- [ ] **Step 1: Declare the public and internal interfaces**

Add to `state.h` after the lifecycle declarations:
```c
// Compiles and executes path on the main coroutine. Returns the first error.
MsResult msEvalFile(MsState* L, const char* path);
```
Add `#include <mslang/error.h>` near the top of `state.h`.

Append to `error.h` before its guard:
```c
typedef struct MsObject MsObject;
typedef struct MsState MsState;

// Returns and clears the current error object; NULL means no error.
MsObject* msErrorGet(MsState* L);
```

Create `object.h` with a `MSLANG_INCLUDE_MSLANG_OBJECT_H_` guard:
```c
#include <mslang/state.h>

typedef struct MsObject MsObject;

// Returns a string object's internal buffer, or NULL for invalid input.
const char* msAsCString(MsState* L, MsObject* obj);
```

Create `ms_builtin.h` with a `MSLANG_SRC_VM_MS_BUILTIN_H_` guard:
```c
#include <mslang/state.h>

void msBuiltinRegisterMinimal(MsState* L);
```

Add to `ms_state_internal.h`:
```c
void msStateSetOom(MsState* L);
void msStateSetError(MsState* L, MsResult result, const char* message, size_t messageLen);
```

- [ ] **Step 2: Implement state error access and minimal builtins**

Make `msStateSetOom` non-static and add to `ms_state.c`:
```c
void msStateSetError(MsState* L, MsResult result, const char* message, size_t messageLen) {
  struct MsString* error = msStringNew(&L->heap, message, messageLen);
  if (error == NULL) {
    msStateSetOom(L);
    return;
  }
  L->error = error;
  L->errorResult = result;
}

MsObject* msErrorGet(MsState* L) {
  if (L == NULL) {
    return NULL;
  }
  struct MsString* error = L->error;
  msStateClearError(L);
  return (struct MsObject*)error;
}

const char* msAsCString(MsState* L, MsObject* obj) {
  if (L == NULL || obj == NULL || msObjectTag(obj) != MS_TYPE_STRING) {
    if (L != NULL) {
      msRaiseTypeError(L, "msAsCString() requires a string");
    }
    return NULL;
  }
  return msStringData((struct MsString*)obj);
}
```

Implement `ms_builtin.c` with scalar formatting and registration:
```c
#include "vm/ms_builtin.h"

#include <stdbool.h>
#include <stdio.h>

#include "object/ms_string.h"
#include "vm/ms_function.h"
#include "vm/ms_state_internal.h"

static int msBuiltinFormatValue(struct MsObject* value, char* out, size_t outSize) {
  switch (msObjectTag(value)) {
    case MS_TYPE_NIL: return snprintf(out, outSize, "nil");
    case MS_TYPE_BOOL: return snprintf(out, outSize, "%s", msBoolValue(value) ? "true" : "false");
    case MS_TYPE_INT: return snprintf(out, outSize, "%lld", (long long)msIntWordValue(value));
    case MS_TYPE_FLOAT: return snprintf(out, outSize, "%.17g", msFloatValue(value));
    case MS_TYPE_STRING: return snprintf(out, outSize, "%s", msStringData((struct MsString*)value));
    default: return snprintf(out, outSize, "<%s>", msTypeTagName(msObjectTag(value)));
  }
}

static bool msBuiltinPrintValue(struct MsObject* value) {
  if (msObjectTag(value) == MS_TYPE_STRING) {
    struct MsString* string = (struct MsString*)value;
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
    msRaiseRuntimeError(L, "assertion failed");
    return NULL;
  }
  if (msObjectTag(argv[1]) == MS_TYPE_STRING) {
    struct MsString* message = (struct MsString*)argv[1];
    msStateSetError(L, MS_ERROR_RUNTIME, msStringData(message), msStringLen(message));
    return NULL;
  }
  char buffer[128];
  msBuiltinFormatValue(argv[1], buffer, sizeof(buffer));
  msRaiseRuntimeError(L, "%s", buffer);
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
```
Include `vm/ms_builtin.h` in `ms_state.c`, call `msBuiltinRegisterMinimal(L)` after `msCoroutineInit`, and close/return `NULL` when registration sets an error. Register `src/vm/ms_builtin.c` in the `mslang` library.

- [ ] **Step 3: Adjust VM tests for preinstalled builtins**

In `stateLifecycleGlobalsAndErrors`, capture `int initialGlobals = L->globalsLen;` after the null check and replace both `MS_ASSERT_EQ(10, L->globalsLen)` assertions with:
```c
MS_ASSERT_EQ(initialGlobals + 10, L->globalsLen);
```

Before the failing `msStateSetGlobal` in `targetedAllocationFailuresSetOom`, fill existing spare capacity:
```c
int filler = 0;
while (L->globalsLen < L->globalsCap) {
  char fillerName[32];
  snprintf(fillerName, sizeof(fillerName), "filler_%d", filler++);
  struct MsString* name = msStringFromCStr(&L->heap, fillerName);
  MS_ASSERT_TRUE(name != NULL);
  MS_ASSERT_EQ(MS_OK, msStateSetGlobal(L, name, value));
}
```
Replace the zero-length/capacity assertions with:
```c
MS_ASSERT_EQ(L->globalsCap, L->globalsLen);
MS_ASSERT_TRUE(L->globalsCap > 0);
```
Increase the `stateCreationOomEventuallySucceeds` injection bound from 39 to 64.

- [ ] **Step 4: Build and run internal regression tests**

Run: `cmake -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 13 existing CTests pass; script fixtures still fail because evaluation/CLI are not connected.

- [ ] **Step 5: Commit the runtime slice**

```bash
git add CMakeLists.txt include/mslang src/vm tests/c/test_vm.c
git commit -m '✨ feat(vm): add minimal builtins and error access'
```

### Task 3: File evaluation pipeline

**Files:**
- Create: `src/vm/ms_eval.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Implement file reading and diagnostic aggregation**

Create `ms_eval.c` with these helpers:
```c
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
    msRaiseRuntimeError(L, "%s: %s", path, strerror(errno));
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    msRaiseRuntimeError(L, "%s: %s", path, strerror(errno));
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    msRaiseRuntimeError(L, "%s: %s", path, strerror(errno));
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
```

- [ ] **Step 2: Implement `msEvalFile` with reverse-order cleanup**

Append:
```c
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

  if (result == MS_ERROR_SYNTAX) {
    msEvalSetDiagnostics(L, &diags);
    result = msStateErrorResult(L);
  } else if (result != MS_OK && msStateErrorResult(L) == MS_OK) {
    msStateSetOom(L);
  }
  if (result == MS_OK) {
    struct MsObject* ignored = NULL;
    result = msCallProto(L, &L->main, proto, 0, NULL, &ignored);
  }
  msProtoFree(proto);
  msDiagListDestroy(&diags);
  msFree(source);
  return result;
}
```
Register `src/vm/ms_eval.c` in `CMakeLists.txt`.

- [ ] **Step 3: Build and exercise the public pipeline through the next CLI task**

Run: `cmake -B build && cmake --build build --config Debug`
Expected: build succeeds with `/W4 /WX`; `run_tests.py` still fails until the CLI accepts paths.

- [ ] **Step 4: Commit evaluation**

```bash
git add CMakeLists.txt src/vm/ms_eval.c
git commit -m '✨ feat(vm): connect file evaluation pipeline'
```

### Task 4: CLI contract and CTest integration

**Files:**
- Modify: `src/cli/main.c`, `CMakeLists.txt`

- [ ] **Step 1: Implement script/version handling and exit mapping**

Replace `main.c` with:
```c
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
  if (argc != 2 || argv[1][0] == '-') {
    if (argc > 1 && (strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "test") == 0)) {
      fprintf(stderr, "mslang: '%s' is not supported yet\n", argv[1]);
    }
    printUsage();
    return MS_EXIT_USAGE;
  }

  const char* path = argv[1];
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "mslang: %s: %s\n", path, strerror(errno));
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
```

- [ ] **Step 2: Register the script test layer**

Add inside `if(MSLANG_BUILD_TESTS)` after the C test registrations:
```cmake
  add_test(NAME test_ms_scripts
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/run_tests.py"
          --mslang "$<TARGET_FILE:mslang-cli>")
```

- [ ] **Step 3: Run script tests and verify only control flow is blocked**

Run: `python run_tests.py --mslang build/Debug/mslang.exe`
Expected: hello, arithmetic, syntax error, and assertion failure pass; control flow fails with exit 2/E307 because its compiler path is Task 5.

- [ ] **Step 4: Verify CLI-only acceptance cases**

Run each command and inspect its status:
```bash
build/Debug/mslang.exe --version
build/Debug/mslang.exe
build/Debug/mslang.exe does-not-exist.ms
```
Expected: version prints `mslang 0.1.0`/status 0; no arguments prints usage to stderr/status 64; missing file prints an OS error to stderr/status 74.

- [ ] **Step 5: Commit the CLI slice**

```bash
git add CMakeLists.txt src/cli/main.c
git commit -m '✨ feat(cli): execute mslang script files'
```

### Task 5: Minimal `if` and three-part `for` compilation

**Files:**
- Modify: `src/compiler/ms_compiler.c`
- Test: `tests/ms/smoke/control_flow.ms`

- [ ] **Step 1: Add control-flow generation helpers**

Add forward declarations beside the existing statement helpers:
```c
static void msCompilerCompileIf(struct MsCompiler* c, const struct MsAst* node);
static void msCompilerCompileFor(struct MsCompiler* c, const struct MsAst* node);
```

Add implementations before `msCompilerCompileStmt`:
```c
static void msCompilerCompileIf(struct MsCompiler* c, const struct MsAst* node) {
  int baseDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.ifStmt.cond);
  int elseJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
  msCompilerCompileStmt(c, node->as.ifStmt.thenBlock);
  if (node->as.ifStmt.elseBranch != NULL) {
    int endJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
    msCompilerPatchToCurrent(c, elseJump, baseDepth);
    msCompilerCompileStmt(c, node->as.ifStmt.elseBranch);
    msCompilerPatchToCurrent(c, endJump, baseDepth);
  } else {
    msCompilerPatchToCurrent(c, elseJump, baseDepth);
  }
}

static void msCompilerCompileFor(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.forStmt.forKind != MS_FOR_CLAUSE) {
    msCompilerReport(c, node, 307, "only three-part for is implemented in task 09");
    return;
  }
  msCompilerCompileStmt(c, node->as.forStmt.init);
  int loopHead = msCompilerCurrentPc(c);
  int baseDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.forStmt.cond);
  int exitJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
  msCompilerCompileStmt(c, node->as.forStmt.body);
  msCompilerCompileStmt(c, node->as.forStmt.post);
  int backJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
  if (backJump >= 0 && c->sticky == MS_OK) {
    MsResult result = msProtoPatchSAx(&c->fn->builder, backJump, loopHead - (backJump + 1));
    if (result != MS_OK) {
      msCompilerSyncBuilder(c);
    }
  }
  msCompilerPatchToCurrent(c, exitJump, baseDepth);
}
```

Add these cases to the statement switch before `MS_AST_PASS`:
```c
    case MS_AST_IF:
      msCompilerCompileIf(c, node);
      break;
    case MS_AST_FOR:
      msCompilerCompileFor(c, node);
      break;
```

- [ ] **Step 2: Run the smoke suite and full Debug tests**

Run:
```bash
cmake --build build --config Debug
python run_tests.py --mslang build/Debug/mslang.exe
ctest --test-dir build -C Debug --output-on-failure
```
Expected: 5 of 5 scripts pass; all 14 CTests pass, including `test_ms_scripts`.

- [ ] **Step 3: Commit minimal control flow**

```bash
git add src/compiler/ms_compiler.c
git commit -m '✨ feat(compiler): add minimal interpreter control flow'
```

### Task 6: Documentation and final verification

**Files:**
- Modify: `docs/tasks/09-minimal-interpreter.md`, `docs/tasks/README.md`
- Modify: `docs/superpowers/specs/2026-10-31-task-09-minimal-interpreter-design.md` only if final behavior differs from the approved design

- [ ] **Step 1: Update completion status without claiming untested platforms**

Mark locally verified acceptance boxes complete. Change the task 09 index status to ✅ only after the following commands pass; retain a note that Linux/macOS were not executed in this Windows-only environment.

- [ ] **Step 2: Verify direct CLI behavior and stdout/stderr**

Run the three success scripts individually, the two negative scripts, no-argument invocation, missing-file invocation, and `--version`.
Expected outputs/statuses: `hello`/0, `arithmetic ok`/0, `control flow ok`/0, syntax status 2, assert status 1, usage status 64, missing file status 74, version 0. Every failure message goes to stderr.

- [ ] **Step 3: Run the Release suite**

Run:
```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
Expected: all 14 Release tests pass.

- [ ] **Step 4: Check formatting, artifacts, and repository state**

Run:
```bash
git diff --check
python tools/check_build_artifacts.py .
git status --short --branch
```
Expected: no whitespace errors or root artifacts; only intentional source/document changes are uncommitted.

- [ ] **Step 5: Commit documentation and completion metadata**

```bash
git add docs/tasks docs/superpowers
git commit -m '📝 docs(tasks): complete minimal interpreter'
```
