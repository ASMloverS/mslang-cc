# Task 10 Builtin Functions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver all 12 v0.1 builtin functions with script-level tests, plus the minimal list/dict runtime needed by those functions.

**Architecture:** Use a table-driven builtin registry in `src/vm/ms_builtin.c`. Keep list/dict storage in independent object modules, connect only literal construction in the compiler/VM, and defer complete container operations to task 16.

**Tech Stack:** C11, MSVC/Ninja on Windows, CMake/CTest, Python 3 test driver, mslang script tests.

**Design:** `docs/superpowers/specs/2026-10-31-task-10-builtin-functions-design.md`

**Worktree:** `.worktrees/task-10-builtin-functions`; branch `feat/task-10-builtin-functions`.

---

## File map

- Modify `run_tests.py`: optional `.err` stderr substring companions.
- Modify `src/vm/ms_state.c`, `src/vm/ms_state_internal.h`: ValueError and AssertionError prefixes.
- Modify `src/vm/ms_builtin.c`, `src/vm/ms_builtin.h`: registry, all builtins, representation and comparison helpers.
- Create `src/object/ms_list.c/.h`, `src/object/ms_dict.c/.h`: minimal container storage.
- Modify `src/object/ms_object.c`: container truthiness and storage release.
- Modify `src/compiler/ms_compiler.c`, `src/vm/ms_vm.c`: list/dict literal compilation and execution.
- Modify `CMakeLists.txt`: compile the two new object modules.
- Create `tests/ms/builtin/*.ms` and `.out`/`.exit`/`.err` companions.
- Modify `docs/tasks/10-builtin-functions.md`, `docs/tasks/16-containers-list-dict.md`, `docs/tasks/README.md`: final status and actual module boundary.

Baseline already verified in this worktree: 14/14 CTests and 5/5 scripts pass. Run every command from the worktree root. Build output must remain under `build/`.

### Task 1: stderr companions and assertion errors

**Files:**
- Modify: `run_tests.py:48-79`
- Modify: `src/vm/ms_state_internal.h:31-33`
- Modify: `src/vm/ms_state.c:173-185`
- Modify: `src/vm/ms_builtin.c:45-66`
- Test: `tests/ms/builtin/err_assert.ms`, `.exit`, `.err`

- [ ] **Step 1: Add the failing assertion fixture**

Create `tests/ms/builtin/err_assert.ms`:

```ms
assert(false, "boom")
```

Create `err_assert.exit` containing `1` and `err_assert.err` containing `AssertionError: boom`, each followed by LF.

- [ ] **Step 2: Add stderr companion checking**

Insert this block after the stdout check in `run_test`, before the failure stderr dump:

```python
    error_companion = script.with_suffix(".err")
    if error_companion.is_file():
        expected_error = error_companion.read_text(encoding="utf-8").strip()
        if expected_error not in result.stderr:
            failures.append(
                f"stderr: expected substring {expected_error!r}, "
                f"got {result.stderr!r}"
            )
```

- [ ] **Step 3: Run the fixture to expose the missing prefix**

Run: `python run_tests.py --filter 'builtin/err_assert.ms'`
Expected: FAIL; exit code is 1 but stderr does not contain `AssertionError: boom`.

- [ ] **Step 4: Add named error prefixes and fix assert**

Declare in `ms_state_internal.h`:

```c
void msRaiseValueError(MsState* L, const char* fmt, ...);
void msRaiseAssertionError(MsState* L, const char* fmt, ...);
```

Define in `ms_state.c` using the existing `msStateRaiseV` pattern:

```c
void msRaiseValueError(MsState* L, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  msStateRaiseV(L, "ValueError: ", fmt, args);
  va_end(args);
}

void msRaiseAssertionError(MsState* L, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  msStateRaiseV(L, "AssertionError: ", fmt, args);
  va_end(args);
}
```

In `msBuiltinAssert`, use `msRaiseAssertionError(L, "assertion failed")` for one argument. For a string message use `msRaiseAssertionError(L, "%s", msStringData(message))`; for other values format first and pass the buffer to the same function. Preserve the one-or-two argument check and successful nil return.

- [ ] **Step 5: Verify green and commit**

Run: `cmake --build build && python run_tests.py --filter 'builtin/err_assert.ms'`
Expected: PASS, 1 of 1.

```bash
git add run_tests.py src/vm/ms_state.c src/vm/ms_state_internal.h src/vm/ms_builtin.c tests/ms/builtin/err_assert.ms tests/ms/builtin/err_assert.exit tests/ms/builtin/err_assert.err
git commit -m '✨ feat(builtins): add typed assertion errors'
```

### Task 2: registry, len, type, and bool

**Files:**
- Modify: `src/vm/ms_builtin.h`
- Modify: `src/vm/ms_builtin.c`
- Modify: `src/vm/ms_state.c:79`
- Test: `tests/ms/builtin/len.ms`, `type.ms`, `convert.ms`

- [ ] **Step 1: Write scalar tests**

Create `len.ms`:

```ms
assert(len("") == 0)
assert(len("hello") == 5)
assert(len("中文字符") == 4)
print("len ok")
```

Create `type.ms`:

```ms
assert(type(1) == type(2))
assert(type(1) != type(1.0))
assert(type("s") != type(true))
assert(type(nil) == type(nil))
print("type ok")
```

Create `convert.ms` with the scalar truthiness subset:

```ms
assert(!bool(nil))
assert(!bool(false))
assert(!bool(0))
assert(!bool(0.0))
assert(!bool(-0.0))
assert(!bool(""))
assert(bool(true))
assert(bool(42))
assert(bool(-1))
assert(bool(0.5))
assert(bool("x"))
assert(bool(print))
print("convert ok")
```

- [ ] **Step 2: Verify undefined-global failures**

Run: `cmake --build build && python run_tests.py --filter 'builtin/{len,type,convert}.ms'`
Expected: the three fixtures fail because `len`, `type`, and `bool` are not registered. If the Python glob filter does not expand braces, run each with `--filter 'builtin/len.ms'`, `--filter 'builtin/type.ms'`, and `--filter 'builtin/convert.ms'`.

- [ ] **Step 3: Install the table-driven registry**

Change the public declaration to:

```c
void msBuiltinRegisterAll(MsState* L);
```

Add the internal entry type and initial table:

```c
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
```

Implement `msBuiltinRegisterAll` as a loop that stops on the first error:

```c
void msBuiltinRegisterAll(MsState* L) {
  for (size_t i = 0; i < MS_ARRAY_LEN(msBuiltinTable) && L->errorResult == MS_OK; ++i) {
    msBuiltinRegister(L, msBuiltinTable[i].name, msBuiltinTable[i].func);
  }
}
```

Include `core/ms_common.h` for `MS_ARRAY_LEN`, and change `msNewState` to call `msBuiltinRegisterAll`.

- [ ] **Step 4: Implement the three builtins**

Add a shared arity helper returning false after `msRaiseTypeError`:

```c
static bool msBuiltinCheckArgc(MsState* L, const char* name, int64_t argc,
    int64_t min, int64_t max) {
  if (argc >= min && (max < 0 || argc <= max)) {
    return true;
  }
  msRaiseTypeError(L, "%s() argument count outside [%lld, %lld]", name,
      (long long)min, (long long)max);
  return false;
}
```

`msBuiltinLen` requires one argument. For strings, scan bytes and count every byte that is not a UTF-8 continuation byte:

```c
size_t count = 0;
const unsigned char* bytes = (const unsigned char*)msStringData(string);
for (size_t i = 0; i < msStringLen(string); ++i) {
  if ((bytes[i] & 0xC0u) != 0x80u) {
    ++count;
  }
}
return msNewInt(L, (int64_t)count);
```

Other tags report `TypeError: object of type '<script-name>' has no len()`. Obtain the script name from `msHeapType(&L->heap, msObjectTag(value))->name`.

`msBuiltinType` requires one argument and returns `value->header.type` cast to `struct MsObject*`. `msBuiltinBool` requires one argument and returns `msNewBool(L, msObjectIsTruthy(argv[0]))`.

- [ ] **Step 5: Verify scalar behavior and commit**

Run: `cmake --build build && python run_tests.py --filter 'builtin/len.ms' && python run_tests.py --filter 'builtin/type.ms' && python run_tests.py --filter 'builtin/convert.ms' && python run_tests.py --filter 'smoke/*.ms'`
Expected: all selected scripts pass.

```bash
git add src/vm/ms_builtin.c src/vm/ms_builtin.h src/vm/ms_state.c tests/ms/builtin/len.ms tests/ms/builtin/type.ms tests/ms/builtin/convert.ms
git commit -m '✨ feat(builtins): add len type and bool'
```

### Task 3: shared value representation and print

**Files:**
- Modify: `src/vm/ms_builtin.c`, `src/vm/ms_state_internal.h`
- Test: `tests/ms/builtin/print.ms`, `print.out`, `type.ms`, `type.out`, `convert.ms`

- [ ] **Step 1: Add failing representation tests**

Create `print.ms`:

```ms
assert(print() == nil)
assert(print(1, "two", true, nil, 2.5, 3.0) == nil)
print("sep", "x")
print("a", "b")
print(-0.0)
print(print)
print(type(1))
print("print ok")
```

Create `print.out` with exactly:

```text

1 two true nil 2.5 3.0
sep x
a b
-0.0
<builtin function print>
<type 'int'>
print ok
```

Replace `type.ms` with:

```ms
assert(type(1) == type(2))
assert(type(1) != type(1.0))
print(type(nil))
print(type(true))
print(type(1))
print(type(1.0))
print(type("s"))
print(type(print))
print("type ok")
```

Create `type.out`:

```text
<type 'NilType'>
<type 'bool'>
<type 'int'>
<type 'float'>
<type 'str'>
<type 'cfunction'>
type ok
```

Append to `convert.ms` before its final print:

```ms
assert(str(nil) == "nil")
assert(str(true) == "true")
assert(str(false) == "false")
assert(str(42) == "42")
assert(str(2.5) == "2.5")
assert(str(3.0) == "3.0")
assert(str("hello") == "hello")
```

- [ ] **Step 2: Run tests to verify the old formatter fails**

Run: `cmake --build build && python run_tests.py --filter 'builtin/print.ms'`
Expected: FAIL on missing `str` during convert or stdout differences for float, C function, and type representation.

- [ ] **Step 3: Implement the dynamic representation builder**

Add a file-local buffer owning `char* data`, `size_t len`, and `size_t cap`. Implement these exact operations:

```c
struct MsBuiltinBuffer {
  char* data;
  size_t len;
  size_t cap;
};

static bool msBuiltinBufferAppend(MsState* L, struct MsBuiltinBuffer* buffer,
    const char* data, size_t len);
static struct MsString* msBuiltinBufferFinish(MsState* L, struct MsBuiltinBuffer* buffer);
```

`Append` grows geometrically with `msRealloc`, checks `SIZE_MAX`, sets OOM on failure, and never leaves a partially updated buffer. `Finish` calls `msStringNew(&L->heap, buffer->data, buffer->len)`, sets OOM on failure, frees the temporary buffer, and resets its fields.

Implement recursive file-local `msBuiltinWriteValue(L, buffer, value, bool repr)`. String values write their bytes directly when `repr` is false; repr mode writes double quotes and escapes `\\`, `\"`, `\n`, `\r`, `\t`, and other ASCII control bytes. C functions read `struct MsCFunctionObject.name`; type objects read `struct MsType.name`. This task adds scalar cases only; Task 5 adds list/dict cases after their APIs exist.

Implement float formatting by trying precision 1 through 17 with `snprintf(buffer, size, "%.*g", precision, value)` and accepting the first candidate for which `strtod` returns a bit-identical double. Append `.0` when neither `.` nor `e`/`E` occurs. Handle finite values, infinities, NaN, and signed zero without integer casts.

Add `msBuiltinStr` that checks one argument, builds its non-repr representation, and returns `msBuiltinBufferFinish`.

- [ ] **Step 4: Route print and assert through the shared representation**

Replace fixed 128-byte value formatting with the builder. `msBuiltinPrintValue` writes the resulting string with `fwrite` and preserves explicit byte lengths. On assertion failure with a message, build the non-repr message and call:

```c
msRaiseAssertionError(L, "%s", msStringData(message));
```

Add `{"str", msBuiltinStr}` to the registry after `bool` to match the planned final ordering when conversion functions join.

- [ ] **Step 5: Add the documented keyword parser entry**

Keep positional behavior as the only script-callable path. Add `MsState.cKeywordCount` without changing the public `MsCFunction` signature. The file-local print option parser reads exactly `cKeywordCount` trailing interleaved name/value entries for `sep`, `end`, `file`, and `flush`; ordinary `CALL` leaves the count at zero. Validate the count before indexing, reject unknown or repeated names with TypeError, require string `sep`/`end`, reject non-nil `file`, and use `msObjectIsTruthy` for `flush`. Defaults are `" "`, `"\n"`, nil, and false. Write `sep` and `end` with their explicit lengths and call `fflush(stdout)` when requested.

The explicit count keeps ordinary positional strings such as `print("sep", "x")` unambiguous. Task 13 will set and restore the count around C callbacks when implementing `CALL_KW`; executable keyword-call coverage remains in task 13, as explicitly allowed by the task document.

- [ ] **Step 6: Verify output byte-for-byte and commit**

Run: `cmake --build build && python run_tests.py --filter 'builtin/print.ms' && python run_tests.py --filter 'builtin/type.ms' && python run_tests.py --filter 'builtin/convert.ms'`
Expected: all three pass with exact `.out` matches.

```bash
git add src/vm/ms_builtin.c tests/ms/builtin/print.ms tests/ms/builtin/print.out tests/ms/builtin/type.ms tests/ms/builtin/type.out tests/ms/builtin/convert.ms
git commit -m '✨ feat(builtins): add shared value formatting'
```

### Task 4: int and float conversion

**Files:**
- Modify: `src/vm/ms_builtin.c`
- Test: `tests/ms/builtin/convert.ms`, `err_int_str.ms`, `err_int_overflow.ms`, `err_float_str.ms`, `err_int_nan.ms` and companions

- [ ] **Step 1: Extend conversion tests**

Insert before the final `convert.ms` print:

```ms
assert(int(3.9) == 3)
assert(int(-3.9) == -3)
assert(int(true) == 1)
assert(int(false) == 0)
assert(int("42") == 42)
assert(int("-42") == -42)
assert(int("1F", 16) == 31)
assert(int("0x1F", 16) == 31)
assert(int("101", 2) == 5)
assert(int("17", 8) == 15)
assert(str(float(2)) == "2.0")
assert(str(float(true)) == "1.0")
assert(str(float("3.14")) == "3.14")
```

Create negative fixtures with the shown call, `.exit` content `1`, and `.err` content:

```ms
// err_int_str.ms: ValueError: invalid literal for int()
int("abc")
```

```ms
// err_int_overflow.ms: ValueError: int() overflow
int("99999999999999999999")
```

```ms
// err_float_str.ms: ValueError: invalid literal for float()
float("abc")
```

```ms
// err_int_nan.ms: ValueError: cannot convert float to int
int(float("nan"))
```

Each `.ms` file contains only its call, not the descriptive comment above; each companion ends with LF.

- [ ] **Step 2: Verify conversion failures**

Run: `python run_tests.py --filter 'builtin/convert.ms'`
Expected: FAIL because `int` and `float` are undefined. Run each negative fixture individually and confirm it also fails for that reason rather than its expected ValueError.

- [ ] **Step 3: Implement strict int parsing and numeric conversion**

Register `int` and `float`. For string input, copy the explicit-length string into a temporary NUL-terminated `msAlloc` buffer, set `errno = 0`, and call `strtoll(text, &end, (int)base)`. Accept only base 2/8/10/16, require at least one digit and `*end == '\0'`, and reject `ERANGE`. Free the buffer on every return.

Reject an explicit base for non-string input with TypeError. Return int input directly, map bool to 0/1, and truncate float only when finite and within the guarded interval `[-9223372036854775808.0, 9223372036854775808.0)`; all other float values report ValueError.

- [ ] **Step 4: Implement float conversion**

Return float input directly, convert int/bool with `(double)`, and parse strings using `strtod` with the same temporary-buffer discipline. Require full consumption and reject `ERANGE` overflow with ValueError; accept the platform's `nan` and `inf` spellings so the negative int conversion is script-testable.

- [ ] **Step 5: Verify the conversion matrix and commit**

Run: `cmake --build build && python run_tests.py --filter 'builtin/convert.ms' && python run_tests.py --filter 'builtin/err_*.ms'`
Expected: all current builtin tests pass; each negative fixture matches its `.err` substring.

```bash
git add src/vm/ms_builtin.c tests/ms/builtin/convert.ms tests/ms/builtin/err_int_str.ms tests/ms/builtin/err_int_str.exit tests/ms/builtin/err_int_str.err tests/ms/builtin/err_int_overflow.ms tests/ms/builtin/err_int_overflow.exit tests/ms/builtin/err_int_overflow.err tests/ms/builtin/err_float_str.ms tests/ms/builtin/err_float_str.exit tests/ms/builtin/err_float_str.err tests/ms/builtin/err_int_nan.ms tests/ms/builtin/err_int_nan.exit tests/ms/builtin/err_int_nan.err
git commit -m '✨ feat(builtins): add numeric conversions'
```

### Task 5: minimal list/dict objects and literals

**Files:**
- Create: `src/object/ms_list.h`, `src/object/ms_list.c`, `src/object/ms_dict.h`, `src/object/ms_dict.c`
- Modify: `CMakeLists.txt:41-59`, `src/object/ms_object.c`, `src/compiler/ms_compiler.c`, `src/vm/ms_vm.c`
- Modify: `src/vm/ms_builtin.c`
- Test: `tests/ms/builtin/len.ms`, `type.ms`, `type.out`, `print.ms`, `print.out`, `convert.ms`

- [ ] **Step 1: Add failing container behavior to existing fixtures**

Append to `len.ms` before its final print:

```ms
assert(len([]) == 0)
assert(len([1, 2, 3]) == 3)
assert(len({}) == 0)
assert(len({"a": 1, "b": 2}) == 2)
```

Append to `type.ms` before its final print:

```ms
print(type([]))
print(type({}))
```

Append the corresponding lines to `type.out` before `type ok`:

```text
<type 'list'>
<type 'dict'>
```

Append to `print.ms` before its final print:

```ms
print([1, "two", true, nil])
print({"a": 1, "b": "two"})
```

Append to `print.out` before `print ok`:

```text
[1, "two", true, nil]
{"a": 1, "b": "two"}
```

Append to `convert.ms` before its final print:

```ms
assert(!bool([]))
assert(!bool({}))
assert(bool([1]))
assert(bool({"a": 1}))
assert(str([1, "two"]) == "[1, \"two\"]")
assert(str({"a": 1}) == "{\"a\": 1}")
```

- [ ] **Step 2: Confirm literal compilation is the red point**

Run: `cmake --build build && python run_tests.py --filter 'builtin/len.ms'`
Expected: FAIL with compiler error E307 for the list/dict expression construct.

- [ ] **Step 3: Implement minimal list storage**

Define the self-contained internal header API:

```c
struct MsList {
  struct MsObjectHeader header;
  struct MsObject** items;
  int64_t len;
  int64_t capacity;
};

struct MsObject* msNewList(MsState* L, int64_t capacity);
MsResult msListAppend(MsState* L, struct MsList* list, struct MsObject* value);
void msListReleaseStorage(struct MsList* list);

static inline int64_t msListLen(const struct MsList* list) {
  return list->len;
}
```

`msNewList` rejects negative capacity with ValueError, allocates through `msObjectAlloc`, and preallocates `capacity` item slots with overflow checks. `msListAppend` grows from 0 to 4 and then doubles with `msRealloc`, preserving the old buffer on OOM and setting `msStateSetOom`. `msListReleaseStorage` frees `items` and resets all storage fields.

- [ ] **Step 4: Implement insertion-ordered dict storage**

Define:

```c
struct MsDictEntry {
  struct MsObject* key;
  struct MsObject* value;
};

struct MsDict {
  struct MsObjectHeader header;
  struct MsDictEntry* entries;
  int64_t len;
  int64_t capacity;
};

struct MsObject* msNewDict(MsState* L);
MsResult msDictSet(MsState* L, struct MsDict* dict, struct MsObject* key, struct MsObject* value);
bool msDictNext(const struct MsDict* dict, int64_t* cursor,
    struct MsObject** key, struct MsObject** value);
void msDictReleaseStorage(struct MsDict* dict);
```

`msDictSet` first finds an equal key with `msObjectValueEquals`; replacement only changes value. New keys grow the dense array geometrically and append at the end. `msDictNext` yields entries by increasing index and returns false at `len`. Release frees only the entry array, never the key/value objects.

- [ ] **Step 5: Connect destruction and truthiness**

Add both modules to `add_library(mslang ...)` in CMake. In `ms_object.c`, include both headers and add a storage-release helper:

```c
static void msObjectReleaseStorage(struct MsObject* obj) {
  switch (msObjectTag(obj)) {
    case MS_TYPE_LIST:
      msListReleaseStorage((struct MsList*)obj);
      break;
    case MS_TYPE_DICT:
      msDictReleaseStorage((struct MsDict*)obj);
      break;
    default:
      break;
  }
}
```

Call it for each heap object in `msHeapDestroy` before `msFree`, and in `msObjectFree` before unlinking/freeing. Extend `msObjectIsTruthy` with `MS_TYPE_LIST` returning `len != 0` and `MS_TYPE_DICT` likewise.

- [ ] **Step 6: Compile and execute literals**

Add a compiler helper:

```c
static void msCompilerCompileContainer(struct MsCompiler* c, const struct MsAst* node) {
  size_t count = node->as.block.count;
  bool isDict = node->kind == MS_AST_DICT_LIT;
  size_t items = isDict ? count / 2 : count;
  if (items > 255) {
    msCompilerReport(c, node, 312, "container literal exceeds 255 entries");
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    msCompilerCompileExpr(c, node->as.block.items[i]);
  }
  msCompilerEmitABC(c, isDict ? MS_OP_BUILD_DICT : MS_OP_BUILD_LIST,
      (uint8_t)items, 0, 1 - (int)count);
}
```

Dispatch `MS_AST_LIST_LIT` and `MS_AST_DICT_LIT` to this helper. Parser invariants guarantee an even dict child count.

In the VM, add `BUILD_LIST` and `BUILD_DICT` cases. For list, check `count` stack values, call `msNewList(L, count)`, append in original order, then replace all inputs with the list. For dict, check `2 * count` values, call `msNewDict`, and set each key/value pair in original order. Propagate OOM via `msVmPropagatedErrorAt`.

- [ ] **Step 7: Extend representation, len, and type**

Add list/dict cases to `msBuiltinLen`, using the new length accessors. In representation, surround list elements with `[`/`]` and dict entries with `{`/`}`, use `", "` separators, and call `msBuiltinWriteValue(..., true)` for every contained key/value. `type` already works through object headers.

- [ ] **Step 8: Verify all container-enabled fixtures and commit**

Run: `cmake --build build && python run_tests.py --filter 'builtin/len.ms' && python run_tests.py --filter 'builtin/type.ms' && python run_tests.py --filter 'builtin/print.ms' && python run_tests.py --filter 'builtin/convert.ms' && ctest --test-dir build --output-on-failure`
Expected: all scripts and all 14 CTests pass; no leak is reported by the allocation accounting tests.

```bash
git add CMakeLists.txt src/object/ms_list.c src/object/ms_list.h src/object/ms_dict.c src/object/ms_dict.h src/object/ms_object.c src/compiler/ms_compiler.c src/vm/ms_vm.c src/vm/ms_builtin.c tests/ms/builtin/len.ms tests/ms/builtin/type.ms tests/ms/builtin/type.out tests/ms/builtin/print.ms tests/ms/builtin/print.out tests/ms/builtin/convert.ms
git commit -m '✨ feat(object): add minimal list and dict literals'
```

### Task 6: materialized range

**Files:**
- Modify: `src/vm/ms_builtin.c`
- Test: `tests/ms/builtin/range.ms`, `err_range_step.ms`, `err_range_bool.ms` and companions

- [ ] **Step 1: Write range tests**

Create `range.ms`:

```ms
assert(str(range(5)) == "[0, 1, 2, 3, 4]")
assert(str(range(2, 8)) == "[2, 3, 4, 5, 6, 7]")
assert(str(range(0, 10, 3)) == "[0, 3, 6, 9]")
assert(str(range(5, 0, -1)) == "[5, 4, 3, 2, 1]")
assert(str(range(3, 3)) == "[]")
assert(str(range(3, 1)) == "[]")
assert(len(range(0, 100, 7)) == 15)
print("range ok")
```

Create `err_range_step.ms` containing `range(1, 10, 0)`, `.exit` containing `1`, and `.err` containing `ValueError: range() step must not be zero`.
Create `err_range_bool.ms` containing `range(true)`, `.exit` containing `1`, and `.err` containing `TypeError: range() arguments must be int`.

- [ ] **Step 2: Verify range is unavailable**

Run: `python run_tests.py --filter 'builtin/range.ms'`
Expected: FAIL with undefined global `range`.

- [ ] **Step 3: Implement count calculation and filling**

Register `range`. Require 1–3 arguments and exact `MS_TYPE_INT` tags. Normalize one argument to start 0/stop argument, and default step to 1. Reject zero step with ValueError.

Use unsigned deltas to avoid signed overflow:

```c
uint64_t count = 0;
if (step > 0 && start < stop) {
  uint64_t delta = (uint64_t)stop - (uint64_t)start;
  count = 1 + (delta - 1) / (uint64_t)step;
} else if (step < 0 && start > stop) {
  uint64_t delta = (uint64_t)start - (uint64_t)stop;
  uint64_t distance = (uint64_t)(-(step + 1)) + 1;
  count = 1 + (delta - 1) / distance;
}
```

Reject `count > INT64_MAX` with ValueError. Create `msNewList(L, (int64_t)count)`, then append exactly `count` integers. Update the current value with checked signed arithmetic or stop based on the loop index so no signed overflow occurs. Return the list; any constructor/append failure returns NULL with OOM set.

- [ ] **Step 4: Verify all forms and commit**

Run: `cmake --build build && python run_tests.py --filter 'builtin/range.ms' && python run_tests.py --filter 'builtin/err_range_*.ms'`
Expected: all three scripts pass.

```bash
git add src/vm/ms_builtin.c tests/ms/builtin/range.ms tests/ms/builtin/err_range_step.ms tests/ms/builtin/err_range_step.exit tests/ms/builtin/err_range_step.err tests/ms/builtin/err_range_bool.ms tests/ms/builtin/err_range_bool.exit tests/ms/builtin/err_range_bool.err
git commit -m '✨ feat(builtins): add materialized range'
```

### Task 7: abs, min, max, and registration coverage

**Files:**
- Modify: `src/vm/ms_builtin.c`
- Test: `tests/ms/builtin/minmax.ms`, `shadow.ms`, `err_len_int.ms`, `err_abs_min.ms`, `err_min_empty.ms`, `err_min_mixed.ms` and companions

- [ ] **Step 1: Write positive tests**

Create `minmax.ms`:

```ms
assert(abs(-3) == 3)
assert(abs(3) == 3)
assert(abs(-2.5) == 2.5)
assert(min(3, 1, 2) == 1)
assert(max(3, 1, 2) == 3)
assert(min([5, 2, 8]) == 2)
assert(max([5, 2, 8]) == 8)
assert(min(1) == 1)
assert(max(1, 2.5) == 2.5)
assert(min("a", "b") == "a")
assert(max("a", "b") == "b")
print("minmax ok")
```

Create `shadow.ms`; the deliberate builtin shadowing is the behavior under test:

```ms
// Verify that a global declaration replaces the builtin binding.
len := 42
assert(len == 42)
print("shadow ok")
```

- [ ] **Step 2: Write remaining negative fixtures**

Create each call with `.exit` content `1` and the listed `.err` substring:

- `err_len_int.ms`: `len(1)`; `TypeError: object of type 'int' has no len()`
- `err_abs_min.ms`: `abs(-9223372036854775808)`; `ValueError: abs() overflow`
- `err_min_empty.ms`: `min([])`; `ValueError: min() arg is an empty sequence`
- `err_min_mixed.ms`: `min(1, "a")`; `TypeError: min() cannot compare values`

- [ ] **Step 3: Verify the new functions fail red**

Run: `python run_tests.py --filter 'builtin/minmax.ms'`
Expected: FAIL with undefined `abs`. Run the four negative fixtures and confirm the first three are not yet producing their required error; `err_len_int` already passes and serves as regression coverage.

- [ ] **Step 4: Implement abs and ordered comparison**

Register `abs`, `min`, and `max`, completing the 12-entry table. `abs` returns int input unchanged when nonnegative, rejects `INT64_MIN`, and uses `fabs` for float.

Implement a helper with this contract:

```c
static bool msBuiltinCompare(MsState* L, const char* name,
    const struct MsObject* lhs, const struct MsObject* rhs, int* order);
```

Int/int compares without floating conversion. Other numeric pairs use the VM's exact int/float ordering logic: handle NaN as unordered, infinities and out-of-range float bounds before casting, then compare the integer with truncation and fractional direction. String/string compares byte-wise with `memcmp` up to the shorter length, then by length. Unsupported combinations set TypeError and return false.

- [ ] **Step 5: Implement both min/max forms**

Use one helper with a `wantMax` flag and two public C callbacks. Require at least one argument. If the sole argument is a list, use its items and reject length zero with the specified ValueError; otherwise use `argv` directly. Keep the current winner as a borrowed object and replace it only when `order` indicates a later value is strictly greater for max or smaller for min. Return the winner without copying.

- [ ] **Step 6: Verify all builtin scripts and commit**

Run: `cmake --build build && python run_tests.py --filter 'builtin/minmax.ms' && python run_tests.py --filter 'builtin/shadow.ms' && python run_tests.py --filter 'builtin/err_*.ms' && python run_tests.py`
Expected: every fixture and the complete suite pass.

```bash
git add src/vm/ms_builtin.c tests/ms/builtin/minmax.ms tests/ms/builtin/shadow.ms tests/ms/builtin/err_len_int.ms tests/ms/builtin/err_len_int.exit tests/ms/builtin/err_len_int.err tests/ms/builtin/err_abs_min.ms tests/ms/builtin/err_abs_min.exit tests/ms/builtin/err_abs_min.err tests/ms/builtin/err_min_empty.ms tests/ms/builtin/err_min_empty.exit tests/ms/builtin/err_min_empty.err tests/ms/builtin/err_min_mixed.ms tests/ms/builtin/err_min_mixed.exit tests/ms/builtin/err_min_mixed.err
git commit -m '✨ feat(builtins): add abs min and max'
```

### Task 8: full verification and completion metadata

**Files:**
- Modify: `docs/tasks/10-builtin-functions.md`
- Modify: `docs/tasks/16-containers-list-dict.md`
- Modify: `docs/tasks/README.md`
- Modify: this plan and the design only to record verified implementation deviations

- [ ] **Step 1: Run clean Debug verification**

Remove only the worktree's generated `build/` directory, then configure and run everything:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
python run_tests.py
```

Expected: build exits 0, all 14 CTests pass, and all smoke/builtin scripts pass.

- [ ] **Step 2: Run Release verification**

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
python run_tests.py --mslang build/release/mslang.exe
```

Expected: the same CTest and script counts pass in Release.

- [ ] **Step 3: Exercise the CLI contract directly**

Run `build/mslang.exe tests/ms/builtin/print.ms`, `type.ms`, `range.ms`, and `minmax.ms`; confirm exit 0 and expected stdout. Run `err_int_str.ms`, `err_range_step.ms`, `err_min_mixed.ms`, and `err_assert.ms`; confirm exit 1, empty stdout, and the required TypeError/ValueError/AssertionError prefix on stderr.

- [ ] **Step 4: Update task documentation**

Change task 10 and the index to ✅. Check only acceptance items verified on Windows. Record that `range` is intentionally materialized and keyword-call execution remains task 13. Leave Linux/macOS verification unclaimed.

In task 16, state that task 10 now supplies minimal list/dict storage and literal construction; task 16 extends these modules with hashing, complete operations, methods, indexing, deletion, membership, equality, and iteration support instead of recreating the objects.

Append brief execution notes to this plan for any actual deviations, including test count changes or code structure adjustments.

- [ ] **Step 5: Check repository hygiene and commit documentation**

Run:

```bash
git diff --check
python tools/check_build_artifacts.py .
git status --short --branch
```

Expected: no whitespace errors, no artifacts outside `build/`, and only intentional documentation changes.

```bash
git add docs/tasks docs/superpowers
git commit -m '📝 docs(tasks): complete builtin functions'
```
