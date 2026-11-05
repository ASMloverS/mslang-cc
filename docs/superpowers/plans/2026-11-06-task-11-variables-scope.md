# Task 11 Variables and Scope Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver strict declaration/assignment checks, lexical block scope, multi-assignment, list unpacking, and `global` semantics for mslang.

**Architecture:** Extract local/global name state into `MsScopeCtx`, keep bytecode emission in the compiler, and add one list-only `UNPACK` VM path. Preserve existing function, control-flow, hidden-slot, and container implementations.

**Tech Stack:** C11, MSVC/CMake on Windows, CTest, Python 3 script driver, mslang script tests.

**Design:** `docs/superpowers/specs/2026-11-06-task-11-variables-scope-design.md`

**Worktree:** `.worktrees/task-11-variables-scope`; branch `feat/task-11-variables-scope`.

---

## File map

- Create `src/compiler/ms_scope.h/.c`: local slots, block stack, and global-name set.
- Modify `src/compiler/ms_compiler.c/.h`: scope ownership, strict name resolution, declaration/assignment generation, and state access.
- Modify `src/vm/ms_eval.c`: provide the current `MsState` to the compiler.
- Modify `src/parser/ms_parser.c`: use E305 for multi-item augmented assignment.
- Modify `tests/c/test_parser.c`: update the existing augmented-assignment diagnostic expectation.
- Modify `src/vm/ms_opcode.h`: define the no-star `UNPACK` operand sentinel.
- Modify `src/vm/ms_vm.c`: execute list-only `UNPACK` with typed errors.
- Modify `CMakeLists.txt`: compile `ms_scope.c`.
- Create `tests/ms/variables/`: positive scripts and `.out`/`.exit`/`.err` companions.
- Modify `docs/tasks/11-variables-scope.md` and `docs/tasks/README.md`: record verified completion.

Run all commands from the worktree root. The baseline is 14/14 Debug CTests passing. All build output stays under `build/`; this is a multi-config Visual Studio build, so CTest always uses `-C Debug` or `-C Release`.

### Task 1: Extract the scope context without changing behavior

**Files:**
- Create: `src/compiler/ms_scope.h`
- Create: `src/compiler/ms_scope.c`
- Modify: `src/compiler/ms_compiler.c:17-47,154-176,750-805,1100-1176`
- Modify: `CMakeLists.txt:41-61`

- [ ] **Step 1: Add the self-contained scope interface**

Create `src/compiler/ms_scope.h`:

```c
#ifndef MSLANG_SRC_COMPILER_MS_SCOPE_H_
#define MSLANG_SRC_COMPILER_MS_SCOPE_H_

#include <stdbool.h>

#include <mslang/error.h>

struct MsString;

#define MS_SCOPE_MAX_LOCALS 256
#define MS_SCOPE_MAX_DEPTH 128

struct MsLocal {
  struct MsString* name;
  int depth;
  bool isCaptured;
};

struct MsScopeBlock {
  int localBase;
  int discardSlot;
};

struct MsScopeCtx {
  struct MsLocal locals[MS_SCOPE_MAX_LOCALS];
  int localCount;
  int localMax;
  int scopeDepth;
  struct MsScopeBlock blocks[MS_SCOPE_MAX_DEPTH];
  struct MsString** globalNames;
  int globalCount;
  int globalCap;
  bool isModule;
};

void msScopeCtxInit(struct MsScopeCtx* ctx, bool isModule);
void msScopeCtxDestroy(struct MsScopeCtx* ctx);
void msScopeBeginBlock(struct MsScopeCtx* ctx);
void msScopeEndBlock(struct MsScopeCtx* ctx);
int msScopeDeclareLocal(struct MsScopeCtx* ctx, struct MsString* name);
int msScopeAllocHiddenLocal(struct MsScopeCtx* ctx);
int msScopeResolveLocal(const struct MsScopeCtx* ctx, const struct MsString* name);
bool msScopeHasLocalAtDepth(const struct MsScopeCtx* ctx, const struct MsString* name);
MsResult msScopeMarkGlobal(struct MsScopeCtx* ctx, struct MsString* name);
bool msScopeIsMarkedGlobal(const struct MsScopeCtx* ctx, const struct MsString* name);

#endif  // MSLANG_SRC_COMPILER_MS_SCOPE_H_
```

- [ ] **Step 2: Implement local storage and block rewind**

Create `src/compiler/ms_scope.c` with the local/block operations:

```c
#include "compiler/ms_scope.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "object/ms_string.h"

void msScopeCtxInit(struct MsScopeCtx* ctx, bool isModule) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->isModule = isModule;
  ctx->blocks[0].discardSlot = -1;
}

void msScopeCtxDestroy(struct MsScopeCtx* ctx) {
  msFree(ctx->globalNames);
  memset(ctx, 0, sizeof(*ctx));
}

void msScopeBeginBlock(struct MsScopeCtx* ctx) {
  MS_ASSERT(ctx->scopeDepth + 1 < MS_SCOPE_MAX_DEPTH);
  ++ctx->scopeDepth;
  ctx->blocks[ctx->scopeDepth].localBase = ctx->localCount;
  ctx->blocks[ctx->scopeDepth].discardSlot = -1;
}

void msScopeEndBlock(struct MsScopeCtx* ctx) {
  MS_ASSERT(ctx->scopeDepth > 0);
  ctx->localCount = ctx->blocks[ctx->scopeDepth].localBase;
  ctx->blocks[ctx->scopeDepth].discardSlot = -1;
  --ctx->scopeDepth;
}

static int msScopeAllocLocal(struct MsScopeCtx* ctx, struct MsString* name) {
  if (ctx->localCount >= MS_SCOPE_MAX_LOCALS) {
    return -1;
  }
  int slot = ctx->localCount++;
  ctx->locals[slot].name = name;
  ctx->locals[slot].depth = ctx->scopeDepth;
  ctx->locals[slot].isCaptured = false;
  if (ctx->localCount > ctx->localMax) {
    ctx->localMax = ctx->localCount;
  }
  return slot;
}

int msScopeDeclareLocal(struct MsScopeCtx* ctx, struct MsString* name) {
  MS_ASSERT(name != NULL);
  return msScopeAllocLocal(ctx, name);
}

int msScopeAllocHiddenLocal(struct MsScopeCtx* ctx) {
  return msScopeAllocLocal(ctx, NULL);
}

int msScopeResolveLocal(const struct MsScopeCtx* ctx, const struct MsString* name) {
  for (int i = ctx->localCount - 1; i >= 0; --i) {
    if (ctx->locals[i].name != NULL && msStringEquals(ctx->locals[i].name, name)) {
      return i;
    }
  }
  return -1;
}

bool msScopeHasLocalAtDepth(const struct MsScopeCtx* ctx, const struct MsString* name) {
  for (int i = ctx->localCount - 1; i >= 0; --i) {
    const struct MsLocal* local = &ctx->locals[i];
    if (local->depth == ctx->scopeDepth && local->name != NULL
        && msStringEquals(local->name, name)) {
      return true;
    }
  }
  return false;
}
```

- [ ] **Step 3: Add the global-name set**

Append to `ms_scope.c`:

```c
bool msScopeIsMarkedGlobal(const struct MsScopeCtx* ctx, const struct MsString* name) {
  for (int i = 0; i < ctx->globalCount; ++i) {
    if (msStringEquals(ctx->globalNames[i], name)) {
      return true;
    }
  }
  return false;
}

MsResult msScopeMarkGlobal(struct MsScopeCtx* ctx, struct MsString* name) {
  if (msScopeResolveLocal(ctx, name) >= 0) {
    return MS_ERROR_SYNTAX;
  }
  if (msScopeIsMarkedGlobal(ctx, name)) {
    return MS_OK;
  }
  if (ctx->globalCount == ctx->globalCap) {
    if (ctx->globalCap > INT_MAX / 2) {
      return MS_ERROR_OOM;
    }
    int newCap = ctx->globalCap == 0 ? 4 : ctx->globalCap * 2;
    if ((size_t)newCap > SIZE_MAX / sizeof(*ctx->globalNames)) {
      return MS_ERROR_OOM;
    }
    struct MsString** names = msRealloc(ctx->globalNames,
        (size_t)newCap * sizeof(*names));
    if (names == NULL) {
      return MS_ERROR_OOM;
    }
    ctx->globalNames = names;
    ctx->globalCap = newCap;
  }
  ctx->globalNames[ctx->globalCount++] = name;
  return MS_OK;
}
```

- [ ] **Step 4: Move compiler state into `MsScopeCtx`**

Include `compiler/ms_scope.h`, add `src/compiler/ms_scope.c` to the `mslang` target, remove `MsLocalSlot` and `MsScope`, and replace the corresponding `MsFuncCtx` fields with:

```c
struct MsFuncCtx {
  struct MsFuncCtx* parent;
  struct MsProtoBuilder builder;
  const struct MsAst* funcNode;
  const struct MsAst* currentNode;
  struct MsScopeCtx scope;
  int stackDepth;
  int stackMax;
  bool isModule;
  bool isAsync;
  bool replPrint;
};
```

Use these exact mappings throughout `ms_compiler.c`:

```text
fn->locals                   -> fn->scope.locals
fn->localCount               -> fn->scope.localCount
fn->localMax                 -> fn->scope.localMax
fn->scopeDepth               -> fn->scope.scopeDepth
fn->scopes                   -> fn->scope.blocks
MS_COMPILER_MAX_LOCALS       -> MS_SCOPE_MAX_LOCALS
MS_COMPILER_MAX_SCOPE_DEPTH  -> MS_SCOPE_MAX_DEPTH
```

Replace the bodies of local/scope wrappers with calls to `msScopeAllocHiddenLocal`, `msScopeResolveLocal`, `msScopeDeclareLocal`, `msScopeBeginBlock`, and `msScopeEndBlock`. `msCompilerDiscardSlot` reads and writes `c->fn->scope.blocks[c->fn->scope.scopeDepth].discardSlot`.

Call `msScopeCtxInit(&fn->scope, funcNode == NULL)` after zeroing a new function context, call `msScopeCtxDestroy(&fn->scope)` in `msCompilerAbortFunc` before `msFree(fn)`, and set `fn->builder.proto.localCount = fn->scope.localMax` when finalizing a proto.

- [ ] **Step 5: Verify the behavior-preserving extraction**

Run:

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: build succeeds and 14/14 tests pass, including zero live allocations in the compiler fixture.

- [ ] **Step 6: Commit the extraction**

```bash
git add CMakeLists.txt src/compiler/ms_scope.h src/compiler/ms_scope.c src/compiler/ms_compiler.c
git commit -m '♻️ refactor(compiler): extract scope context'
```

### Task 2: Enforce declaration and assignment rules

**Files:**
- Modify: `src/compiler/ms_compiler.h:20-33`
- Modify: `src/compiler/ms_compiler.c:807-905`
- Modify: `src/vm/ms_eval.c:110-120`
- Modify: `src/parser/ms_parser.c:1119-1127`
- Modify: `tests/c/test_parser.c:805-810`
- Test: `tests/ms/variables/decl_assign.ms`
- Test: `tests/ms/variables/builtin_assign.ms`
- Test: `tests/ms/variables/aug_assign.ms`
- Test: `tests/ms/variables/err_undeclared_assign.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_undeclared_aug.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_no_new_name.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_aug_multi.ms`, `.exit`, `.err`

- [ ] **Step 1: Write strict declaration and assignment fixtures**

Create `decl_assign.ms`:

```ms
value := 1
value = 2
assert(value == 2)
{
    value := value + 1
    assert(value == 3)
}
assert(value == 2)
len := 5
assert(len == 5)
print("decl_assign ok")
```

Create `builtin_assign.ms`:

```ms
len = 5
assert(len == 5)
print("builtin_assign ok")
```

Create `aug_assign.ms`:

```ms
value := 2
value += 3
assert(value == 5)
value -= 1
assert(value == 4)
value *= 3
assert(value == 12)
value **= 2
assert(value == 144)
value %= 10
assert(value == 4)
value <<= 3
assert(value == 32)
value >>= 2
assert(value == 8)
value |= 3
assert(value == 11)
value &= 10
assert(value == 10)
value ^= 3
assert(value == 9)
value /= 2
assert(value == 4.5)
{
    value := 1
    value += 2
    assert(value == 3)
}
assert(value == 4.5)
print("aug_assign ok")
```

Create the negative sources, each with `.exit` containing `2` and the listed `.err` substring:

```ms
// err_undeclared_assign.ms; E301
missing = 1
```

```ms
// err_undeclared_aug.ms; E301
missing += 1
```

```ms
// err_no_new_name.ms; E302
value := 1
value := 2
```

```ms
// err_aug_multi.ms; E305
left, right += 1, 2
```

Use `.err` contents `error E301:`, `error E302: no new variables on left side of :=`, and `error E305: augmented assignment takes a single target` respectively.

- [ ] **Step 2: Establish red negative tests and positive regression baselines**

Run each positive and negative fixture through `python run_tests.py --filter 'variables/<name>.ms'`. The three positive scripts already exercise supported single-target behavior and must pass before and after the change. Expected red results: undeclared assignment executes instead of producing E301, undeclared compound assignment exits at runtime instead of compile time, repeated declaration executes instead of E302, and augmented multi-target reports E205 instead of E305.

- [ ] **Step 3: Provide read-only access to registered globals**

Add `struct MsState* state;` to `MsCompiler`, forward-declare `struct MsState`, initialize it to `NULL`, and declare:

```c
void msCompilerSetState(struct MsCompiler* c, struct MsState* state);
```

Define the setter and call it from `msEvalFile` immediately after `msCompilerInit`:

```c
void msCompilerSetState(struct MsCompiler* c, struct MsState* state) {
  c->state = state;
}
```

```c
msCompilerInit(&compiler, &L->heap, &diags, path, MS_COMPILE_DEFAULT);
msCompilerSetState(&compiler, L);
```

Include `vm/ms_state_internal.h` in `ms_compiler.c` for `msStateGetGlobal`.

- [ ] **Step 4: Separate store validation from emission**

Add a helper that accepts locals, existing upvalue placeholders, marked globals, module declarations, and registered globals:

```c
static bool msCompilerValidateStore(struct MsCompiler* c, struct MsString* name) {
  int slot = -1;
  MsResolveKind kind = msCompilerResolveName(c, name, &slot);
  if (kind != MS_RESOLVE_GLOBAL || msScopeIsMarkedGlobal(&c->fn->scope, name)) {
    return true;
  }
  if (c->state != NULL && msStateGetGlobal(c->state, name) != NULL) {
    return true;
  }
  if (c->sticky == MS_OK) {
    uint32_t line = c->fn->currentNode != NULL ? c->fn->currentNode->line : 0;
    uint32_t column = c->fn->currentNode != NULL ? c->fn->currentNode->column : 0;
    msDiagReport(c->diags, line, column, 301, "assignment to undeclared name '%s'",
        msStringData(name));
    c->sticky = MS_ERROR_SYNTAX;
  }
  return false;
}
```

Before `msCompilerEmitStoreName` handles a global in `MS_AST_ASSIGN` or `MS_AST_AUG_ASSIGN`, call this helper and return on false. Keep load resolution unchanged so undefined reads remain runtime errors.

- [ ] **Step 5: Implement single-target `:=` checks before generalizing arity**

For a single identifier declaration, retain RHS-first generation. At module depth 0, reject a second `:=` only when `globalNames` already contains the name, otherwise mark it with `msScopeMarkGlobal`. In a block or function, use `msScopeHasLocalAtDepth` to distinguish redeclaration from a new local. Existing same-depth targets are stored normally; if none is new, report:

```c
msCompilerFail(c, node, MS_ERROR_SYNTAX, 302, "no new variables on left side of :=");
```

For a non-module context, a marked-global declaration reports E304. For a module context, `global` and `:=` may coexist.

Change both parser diagnostics at `ms_parser.c:1124-1126` to:

```c
parserErrorAtNode(p, targets->items[1], 305, "augmented assignment takes a single target");
parserErrorAtNode(p, values.items[1], 305, "augmented assignment takes a single target");
```

Also update the adjacent comment to name E305. Update `tests/c/test_parser.c` so `parser.augAssignMultiTarget` expects `{ 305 }` and its two-line comment names E305.

- [ ] **Step 6: Run strict assignment tests and regressions**

Run:

```bash
cmake --build build --config Debug
python run_tests.py --filter 'variables/*.ms'
ctest --test-dir build -C Debug --output-on-failure
```

Expected: the new declaration/assignment fixtures pass; no other variable fixtures exist yet; all 14 CTests pass.

- [ ] **Step 7: Commit strict assignment**

```bash
git add src/compiler/ms_compiler.c src/compiler/ms_compiler.h src/vm/ms_eval.c src/parser/ms_parser.c tests/c/test_parser.c tests/ms/variables
git commit -m '✨ feat(scope): enforce declaration and assignment rules'
```

### Task 3: Add multi-target assignment and swap semantics

**Files:**
- Modify: `src/compiler/ms_compiler.c:852-905`
- Modify: `tests/c/test_compiler.c:718-730`
- Test: `tests/ms/variables/multi_assign.ms`
- Test: `tests/ms/variables/err_no_new_names.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_arity_mismatch.ms`, `.exit`, `.err`

- [ ] **Step 1: Write multi-assignment fixtures**

Create `multi_assign.ms`:

```ms
first, second := 1, 2
assert(first == 1)
assert(second == 2)
first, fourth := 4, 4
assert(first == 4)
assert(fourth == 4)
first, second = second, first
assert(first == 2)
assert(second == 4)
third := 3
first, second, third = second, third, first
assert(first == 4)
assert(second == 3)
assert(third == 2)
left := 1
right := 2
left, right = left + right, left - right
assert(left == 3)
assert(right == -1)
print("multi_assign ok")
```

Create `err_no_new_names.ms` with E302 expectations:

```ms
first, second := 1, 2
first, second := 3, 4
```

Create `err_arity_mismatch.ms` with E303 expectations:

```ms
first, second = 1, 2, 3
```

- [ ] **Step 2: Verify multi-target rejection**

Run the three scripts. Expected: the positive and no-new-names scripts fail with task-11 E307; the arity script also reaches the old task-11 path instead of E303.

- [ ] **Step 3: Implement shared target metadata and static arity checks**

Add helpers that unwrap a star target and find its index:

```c
static const struct MsAst* msCompilerTargetIdent(const struct MsAst* target) {
  if (target->kind == MS_AST_UNARY && target->as.unary.op == MS_TOKEN_STAR) {
    target = target->as.unary.operand;
  }
  return target->kind == MS_AST_IDENT ? target : NULL;
}

static int msCompilerStarTarget(const struct MsAstList* targets) {
  for (size_t i = 0; i < targets->count; ++i) {
    const struct MsAst* target = targets->items[i];
    if (target->kind == MS_AST_UNARY && target->as.unary.op == MS_TOKEN_STAR) {
      return (int)i;
    }
  }
  return -1;
}
```

Replace `msCompilerCompileDeclOrAssign` with a list-based implementation that:

1. Rejects non-identifier targets with E301 and an indexed/attribute assignment message.
2. When `values.count > 1`, requires `values.count == targets.count` without a star, or `values.count >= targets.count - 1` with a star; otherwise reports E303 using the actual counts.
3. Generates RHS values from left to right. For `values.count == 1 && targets.count > 1`, emit `UNPACK` as specified in Task 4; until that task, report the existing E307 so this commit contains only static-list assignment.
4. For declarations, walks targets left to right after RHS generation, marking new globals or allocating new locals and counting whether any target is new. Reports E302 if none is new and E304 for function-global conflicts.
5. Stores targets from right to left with the existing local/upvalue/global emission path, validating every `=` target before its store.

Use a node-local `bool declaredAny = false;` and `int starIndex = msCompilerStarTarget(&node->as.decl.targets);`. Do not add temporary arrays: target metadata is already in the AST and local slots remain in `MsScopeCtx`.

Remove the `{"a, b := 1, 2;", 307, 1}` case from `compiler.unsupportedAndLimitDiagnostics` in `tests/c/test_compiler.c`; the new `multi_assign.ms` fixture now owns that behavior.

- [ ] **Step 4: Verify multi-assignment and diagnostics**

Run:

```bash
cmake --build build --config Debug
python run_tests.py --filter 'variables/{multi_assign,err_no_new_names,err_arity_mismatch}.ms'
```

If Python's `Path.match` does not accept braces, run each filter separately. Expected: all three pass; swap and rotation assertions succeed.

- [ ] **Step 5: Commit multi-assignment**

```bash
git add src/compiler/ms_compiler.c tests/c/test_compiler.c tests/ms/variables/multi_assign.ms tests/ms/variables/err_no_new_names.ms tests/ms/variables/err_no_new_names.exit tests/ms/variables/err_no_new_names.err tests/ms/variables/err_arity_mismatch.ms tests/ms/variables/err_arity_mismatch.exit tests/ms/variables/err_arity_mismatch.err
git commit -m '✨ feat(compiler): add multi-target assignment'
```

### Task 4: Compile and execute list unpacking

**Files:**
- Modify: `src/vm/ms_opcode.h:8-12`
- Modify: `src/compiler/ms_compiler.c` declaration/assignment generator from Task 3
- Modify: `src/vm/ms_vm.c:501-839`
- Test: `tests/ms/variables/unpack.ms`, `unpack.out`
- Test: `tests/ms/variables/err_unpack_count.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_unpack_few.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_unpack_type.ms`, `.exit`, `.err`

- [ ] **Step 1: Write unpacking fixtures**

Create `unpack.ms`:

```ms
first, second, third := range(3)
assert(first == 0)
assert(second == 1)
assert(third == 2)
head, *rest := range(5)
assert(rest == [1, 2, 3, 4])
left, *middle, right := range(5)
assert(left == 0)
assert(middle == [1, 2, 3])
assert(right == 4)
first, second = range(2)
assert(first == 0)
assert(second == 1)
begin, *items, end := 10, 11, 12, 13
assert(begin == 10)
assert(items == [11, 12])
assert(end == 13)
print(rest)
print("unpack ok")
```

Create `unpack.out`:

```text
[1, 2, 3, 4]
unpack ok
```

Create three negative scripts with `.exit` `1`:

```ms
// err_unpack_count.ms; .err = ValueError: expected 2 values to unpack, got 3
first, second := range(3)
```

```ms
// err_unpack_few.ms; .err = ValueError: expected at least 2 values to unpack, got 1
first, second, *rest := range(1)
```

```ms
// err_unpack_type.ms; .err = TypeError: cannot unpack non-list 'int'
first, second := 1
```

- [ ] **Step 2: Verify unpacking is unimplemented**

Run the four scripts. Expected: positives fail with the task-11 compiler diagnostic; negative scripts do not produce the required VM errors.

- [ ] **Step 3: Emit `UNPACK` and literal-list star collection**

Add to `ms_opcode.h`:

```c
#define MS_UNPACK_NO_STAR 0xFFFFu
```

In the single-RHS/multi-target branch, emit:

```c
msCompilerEmitABC(c, MS_OP_UNPACK, (uint8_t)targets->count,
    starIndex < 0 ? MS_UNPACK_NO_STAR : (uint16_t)starIndex,
    (int)targets->count - 1);
if (c->sticky == MS_OK && c->fn->stackMax < c->fn->stackDepth + 1) {
  c->fn->stackMax = c->fn->stackDepth + 1;
}
```

Reject target counts above 255 with E306 before this cast. For multi-RHS with a star, compile values before the star normally, compile the middle expressions followed by `MS_OP_BUILD_LIST` with the middle count, then compile trailing values. The resulting stack contains exactly one value per target and uses the same right-to-left stores.

- [ ] **Step 4: Implement VM validation and value construction**

Add a `MS_OP_UNPACK` case before `MS_OP_CALL`. Use this complete algorithm:

```c
case MS_OP_UNPACK: {
  int count = msOpDecodeA(instr);
  int star = msOpDecodeBx(instr);
  if (!msVmCanPop(co, frame)) {
    result = msVmErrorAt(L, frame, "evaluation stack underflow");
    break;
  }
  struct MsObject* source = co->stack[co->stackTop - 1];
  if (msObjectTag(source) != MS_TYPE_LIST) {
    msRaiseTypeError(L, "cannot unpack non-list '%s'", msTypeTagName(msObjectTag(source)));
    result = msVmPropagatedErrorAt(L, frame);
    break;
  }
  struct MsList* list = (struct MsList*)source;
  int64_t length = msListLen(list);
  bool valid = star == MS_UNPACK_NO_STAR ? length == count : length >= count - 1;
  if (!valid) {
    if (star == MS_UNPACK_NO_STAR) {
      msRaiseValueError(L, "expected %d values to unpack, got %lld", count, (long long)length);
    } else {
      msRaiseValueError(L, "expected at least %d values to unpack, got %lld",
          count - 1, (long long)length);
    }
    result = msVmPropagatedErrorAt(L, frame);
    break;
  }
  if (count < 1 || (star != MS_UNPACK_NO_STAR && (star < 0 || star >= count))
      || co->stackTop - 1 + count >= windowEnd) {
    result = msVmErrorAt(L, frame, "invalid unpack instruction");
    break;
  }
  int sourceIndex = co->stackTop - 1;
  int64_t middleLen = star == MS_UNPACK_NO_STAR ? 0 : length - (count - 1);
  for (int i = 0; i < count; ++i) {
    struct MsObject* value = NULL;
    if (i == star) {
      value = msNewList(L, middleLen);
      if (value == NULL) {
        result = msVmPropagatedErrorAt(L, frame);
        break;
      }
      co->stack[sourceIndex + 1 + i] = value;
      for (int64_t j = 0; j < middleLen; ++j) {
        result = msListAppend(L, (struct MsList*)value, list->items[star + j]);
        if (result != MS_OK) {
          result = msVmPropagatedErrorAt(L, frame);
          break;
        }
      }
    } else {
      int64_t index = star != MS_UNPACK_NO_STAR && i > star
          ? length - (count - i)
          : i;
      value = list->items[index];
      co->stack[sourceIndex + 1 + i] = value;
    }
    if (result != MS_OK) {
      break;
    }
  }
  if (result != MS_OK) {
    break;
  }
  co->stack[sourceIndex] = co->stack[sourceIndex + 1];
  for (int i = 1; i < count; ++i) {
    co->stack[sourceIndex + i] = co->stack[sourceIndex + i + 1];
  }
  co->stackTop = sourceIndex + count;
  break;
}
```

Keep the source at `sourceIndex` throughout allocation. The newly allocated rest list is immediately placed in the frame stack before appending elements.

- [ ] **Step 5: Verify all unpack paths**

Run:

```bash
cmake --build build --config Debug
python run_tests.py --filter 'variables/unpack.ms'
python run_tests.py --filter 'variables/err_unpack_count.ms'
python run_tests.py --filter 'variables/err_unpack_few.ms'
python run_tests.py --filter 'variables/err_unpack_type.ms'
ctest --test-dir build -C Debug --output-on-failure
```

Expected: four script tests pass with exact stdout/error fragments and 14/14 CTests pass.

- [ ] **Step 6: Commit unpacking**

```bash
git add src/vm/ms_opcode.h src/compiler/ms_compiler.c src/vm/ms_vm.c tests/ms/variables/unpack.ms tests/ms/variables/unpack.out tests/ms/variables/err_unpack_count.ms tests/ms/variables/err_unpack_count.exit tests/ms/variables/err_unpack_count.err tests/ms/variables/err_unpack_few.ms tests/ms/variables/err_unpack_few.exit tests/ms/variables/err_unpack_few.err tests/ms/variables/err_unpack_type.ms tests/ms/variables/err_unpack_type.exit tests/ms/variables/err_unpack_type.err
git commit -m '✨ feat(vm): add list unpacking'
```

### Task 5: Implement top-level and function `global`

**Files:**
- Modify: `src/compiler/ms_compiler.c` name resolver and statement dispatcher
- Test: `tests/ms/variables/global_stmt.ms`
- Test: `tests/ms/variables/err_global_conflict.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_global_decl_conflict.ms`, `.exit`, `.err`

- [ ] **Step 1: Write global fixtures**

Create `global_stmt.ms`:

```ms
global value
value = 5
global first, second
first = 1
second = 2
global declared
declared := 3
assert(declared == 3)
func updateGlobal() {
    global value
    value = value + 1
    assert(value == 6)
}
updateGlobal()
assert(value == 6)
print("global_stmt ok")
```

Create `err_global_conflict.ms`, `.exit` `2`, and `.err` `error E304:`:

```ms
func conflict() {
    value := 1
    global value
}
```

Create `err_global_decl_conflict.ms`, `.exit` `2`, and `.err` `error E304:`:

```ms
func conflict() {
    global value
    value := 1
}
```

- [ ] **Step 2: Verify `global` currently fails**

Run all three scripts. Expected: the positive reports the generic unsupported-statement E307; conflict scripts do not produce E304.

- [ ] **Step 3: Compile global declarations and prioritize marked names**

Add `msCompilerCompileGlobal`:

```c
static void msCompilerCompileGlobal(struct MsCompiler* c, const struct MsAst* node) {
  for (size_t i = 0; i < node->as.globalStmt.names.count && c->sticky == MS_OK; ++i) {
    const struct MsAst* item = node->as.globalStmt.names.items[i];
    struct MsString* name = msCompilerNameFromSlice(c,
        item->as.literal.start, item->as.literal.length);
    if (name == NULL) {
      return;
    }
    MsResult result = msScopeMarkGlobal(&c->fn->scope, name);
    if (result == MS_ERROR_SYNTAX) {
      msCompilerFail(c, item, result, 304, "name is local before global declaration");
    } else if (result != MS_OK) {
      msCompilerFail(c, item, result, 0, "out of memory");
    }
  }
}
```

Dispatch `MS_AST_GLOBAL` to it. In `msCompilerResolveName`, after the current-local lookup and before walking parent contexts, return `MS_RESOLVE_GLOBAL` when `msScopeIsMarkedGlobal(&c->fn->scope, name)` is true. In declaration handling, reject marked globals only when `!c->fn->isModule`; module `global` and `:=` remain compatible.

- [ ] **Step 4: Verify global behavior and conflicts**

Run the three script filters, then `ctest --test-dir build -C Debug --output-on-failure`. Expected: all three new scripts pass and the full suite remains green.

- [ ] **Step 5: Commit global semantics**

```bash
git add src/compiler/ms_compiler.c tests/ms/variables/global_stmt.ms tests/ms/variables/err_global_conflict.ms tests/ms/variables/err_global_conflict.exit tests/ms/variables/err_global_conflict.err tests/ms/variables/err_global_decl_conflict.ms tests/ms/variables/err_global_decl_conflict.exit tests/ms/variables/err_global_decl_conflict.err
git commit -m '✨ feat(scope): implement global declarations'
```

### Task 6: Complete scope behavior and defensive limits

**Files:**
- Modify: `src/compiler/ms_compiler.c` only if a fixture exposes a scope or limit defect
- Test: `tests/ms/variables/block_scope.ms`
- Test: `tests/ms/variables/slot_reuse.ms`
- Test: `tests/ms/variables/err_read_undefined.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_index_target.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_local_overflow.ms`, `.exit`, `.err`
- Test: `tests/ms/variables/err_scope_depth.ms`, `.exit`, `.err`

- [ ] **Step 1: Add scope and slot-reuse scripts**

Create `block_scope.ms`:

```ms
value := 1
if true {
    value := value + 1
    assert(value == 2)
    if true {
        value = 3
        assert(value == 3)
    }
    assert(value == 3)
}
assert(value == 1)
print("block_scope ok")
```

Create `slot_reuse.ms`:

```ms
{
    first := 1
    assert(first == 1)
}
{
    second := 2
    assert(second == 2)
}
print("slot_reuse ok")
```

Create `err_read_undefined.ms` with `.exit` `1` and `.err` `undefined name 'value'`:

```ms
{
    value := 1
}
print(value)
```

Create `err_index_target.ms` with `.exit` `2` and `.err` `error E301:`:

```ms
items := range(3)
items[0] = 9
```

- [ ] **Step 2: Generate the two defensive-limit fixtures**

Generate 300 same-block declarations and 128 nested bare blocks with Python, writing UTF-8/LF source files and their companions:

```bash
python - <<'PY'
from pathlib import Path
root = Path('tests/ms/variables')
root.joinpath('err_local_overflow.ms').write_text(
    '{\n' + ''.join(f'    value{i} := {i}\n' for i in range(300)) + '}\n',
    encoding='utf-8', newline='\n')
root.joinpath('err_local_overflow.exit').write_text('2\n', encoding='utf-8', newline='\n')
root.joinpath('err_local_overflow.err').write_text('error E306:\n', encoding='utf-8', newline='\n')
root.joinpath('err_scope_depth.ms').write_text(
    '{\n' * 128 + 'pass\n' + '}\n' * 128,
    encoding='utf-8', newline='\n')
root.joinpath('err_scope_depth.exit').write_text('2\n', encoding='utf-8', newline='\n')
root.joinpath('err_scope_depth.err').write_text('error E306:\n', encoding='utf-8', newline='\n')
PY
```

- [ ] **Step 3: Run the scope fixtures before changing code**

Run all six scripts. Expected: block scope, slot reuse, undefined read, and limits may already pass after Task 1; indexed assignment must fail with the new explicit E301 rather than the old generic task-11 E307. If another result differs, make the smallest compiler correction covered by that fixture and rerun it.

- [ ] **Step 4: Run the full Debug suite**

```bash
cmake --build build --config Debug
python run_tests.py
ctest --test-dir build -C Debug --output-on-failure
```

Expected: every discovered script and all 14 CTests pass.

- [ ] **Step 5: Commit scope coverage and any required fix**

```bash
git add src/compiler/ms_compiler.c tests/ms/variables
git commit -m '✅ test(scope): cover block scope and limits'
```

If no compiler change was needed, the commit contains only the tests.

### Task 7: Update task status and verify the deliverable

**Files:**
- Modify: `docs/tasks/11-variables-scope.md`
- Modify: `docs/tasks/README.md:52`
- Modify: `docs/superpowers/plans/2026-11-06-task-11-variables-scope.md`

- [ ] **Step 1: Record actual behavior and verification**

Change task 11's status to `✅` in both files. Check only acceptance items verified in this worktree. Align the task's interface assumptions with actual names (`msStringNew`, `msNewList`, `msListLen`, direct checked `items` access, `MsScopeCtx`), record that function `global` is active now, and remove no longer accurate statements that multi-RHS star collection waits for task 16. Preserve future task boundaries for upvalues, general iterables, and indexed/attribute targets.

Mark executed plan checkboxes `[x]`; if execution required a code-level deviation, add a short note beside the affected step with the actual behavior and reason.

- [ ] **Step 2: Check documentation consistency and formatting**

Run:

```bash
git diff --check
python tools/check_build_artifacts.py .
```

Expected: no whitespace errors and no artifacts outside `build/`.

- [ ] **Step 3: Verify Debug and Release end to end**

Run:

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python run_tests.py --mslang build/Debug/mslang.exe
```

Expected: both configurations build without warnings, all 14 CTests pass in each configuration, and every script passes through the direct driver. Do not claim ASAN verification on this MSVC toolchain.

- [ ] **Step 4: Inspect the final diff and repository state**

Run:

```bash
git diff --stat main...HEAD
git status --short
```

Expected: only planned source, test, and documentation files are in the change set; build output is ignored; no unexpected files are untracked.

- [ ] **Step 5: Commit completion documentation**

```bash
git add docs/tasks/11-variables-scope.md docs/tasks/README.md docs/superpowers/plans/2026-11-06-task-11-variables-scope.md
git commit -m '📝 docs(tasks): complete variables and scope'
```
