# Task 12 Control Flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver branches, loops, loop control, conditional expressions, and short-circuit comparison chains for mslang scripts.

**Architecture:** Extend the existing compiler with function-local dynamic loop contexts and shared jump patching. Use `CMP_CHAIN` for directly representable comparisons and hidden-local conditional jumps for negated segments; keep container membership semantics in task 16.

**Tech Stack:** C11, MSVC/CMake on Windows, CTest, Python 3 script driver, mslang script tests.

**Design:** `docs/superpowers/specs/2026-11-07-task-12-control-flow-design.md`

**Worktree:** `.worktrees/task-12-control-flow`; branch `feat/task-12-control-flow`.

---

## File map

- Modify `src/compiler/ms_compiler.c`: loop contexts, statement dispatch, `for` scope, inc/dec, and comparison-chain generation.
- Modify `src/vm/ms_vm.c`: accept `IS`/`IN` in `CMP_CHAIN` and dispatch ordinary `IN` through the existing comparison path; do not add container membership semantics.
- Create `tests/ms/control_flow/`: positive scripts and E209 negative companions.
- Modify `docs/tasks/12-control-flow.md` and `docs/tasks/README.md`: record verified completion.
- Modify `docs/superpowers/plans/2026-11-07-task-12-control-flow.md`: mark executed steps and record necessary deviations.

Run all commands from the worktree root. The clean baseline is 14/14 Debug CTests passing. All build output stays under `build/`; this is a multi-config Visual Studio build, so CTest always uses `-C Debug` or `-C Release`.

### Task 1: Lock down branch and conditional-expression behavior

**Files:**
- Create: `tests/ms/control_flow/if_else.ms`
- Create: `tests/ms/control_flow/cond_expr.ms`

- [ ] **Step 1: Add the branch fixture**

Create `if_else.ms`:

```ms
if true {
    assert(true)
} else {
    assert(false)
}
if false {
    assert(false)
} else if 0 {
    assert(false)
} else if "" {
    assert(false)
} else if nil {
    assert(false)
} else if "selected" {
    assert(true)
} else {
    assert(false)
}
if 0.0 {
    assert(false)
} else {
    assert(true)
}
if -1 {
    if 1 {
        assert(true)
    } else {
        assert(false)
    }
}
value := 1
if true {
    value := value + 1
    assert(value == 2)
}
assert(value == 1)
print("if else ok")
```

- [ ] **Step 2: Add the conditional-expression fixture**

Create `cond_expr.ms`:

```ms
isTrue := true
isFalse := false
zero := 0
truthyText := "yes"
first := 1 if isTrue else 2
second := 1 if isFalse else 2
third := 1 if zero else 2
fourth := 1 if truthyText else 2
assert(first == 1)
assert(second == 2)
assert(third == 2)
assert(fourth == 1)
nested := 1 if true else 2 if false else 3
assert(nested == 1)
precedence := 2 * 3 if false else 4
assert(precedence == 4)
assert((1 if true else 2) + 3 == 4)
assert((1 if false else 2) < 3)
print("cond expr ok")
```

- [ ] **Step 3: Run the fixtures before changing production code**

Run:

```bash
python run_tests.py --filter 'control_flow/if_else.ms'
python run_tests.py --filter 'control_flow/cond_expr.ms'
```

Expected: both pass because these two paths already exist. If either fails, record the actual diagnostic and make the smallest correction in the existing `msCompilerCompileIf` or `msCompilerCompileConditional`; do not rewrite working code.

- [ ] **Step 4: Run regression tests and commit the coverage**

Run:

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: build succeeds and 14/14 CTests pass. Commit:

```bash
git add tests/ms/control_flow/if_else.ms tests/ms/control_flow/cond_expr.ms src/compiler/ms_compiler.c
git commit -m '✅ test(control-flow): cover branches and conditional expressions'
```

If no compiler correction was needed, do not stage `src/compiler/ms_compiler.c`; the commit contains only tests.

### Task 2: Add loop contexts, while, bare for, break, and continue

**Files:**
- Modify: `src/compiler/ms_compiler.c:19-38,100-136,1135-1261,1264-1345`
- Create: `tests/ms/control_flow/while_loop.ms`
- Create: `tests/ms/control_flow/bare_for.ms`
- Create: `tests/ms/control_flow/break_continue.ms`
- Create: `tests/ms/control_flow/err_break_outside.ms`, `.exit`, `.err`
- Create: `tests/ms/control_flow/err_continue_outside.ms`, `.exit`, `.err`

- [ ] **Step 1: Write the loop and loop-control fixtures**

Create `while_loop.ms`:

```ms
index := 1
sum := 0
while index <= 100 {
    sum += index
    index += 1
}
assert(sum == 5050)
zero := 0
while false {
    zero += 1
}
assert(zero == 0)
outer := 0
innerTotal := 0
while outer < 3 {
    inner := 0
    while inner < 2 {
        innerTotal += 1
        inner += 1
    }
    outer += 1
}
assert(innerTotal == 6)
print("while ok")
```

Create `bare_for.ms`:

```ms
count := 0
for {
    count += 1
    if count == 3 {
        break
    }
}
assert(count == 3)
visited := 0
skipped := 0
for {
    visited += 1
    if visited < 4 {
        continue
    }
    skipped += 1
    break
}
assert(visited == 4)
assert(skipped == 1)
print("bare for ok")
```

Create `break_continue.ms`:

```ms
count := 0
while true {
    count += 1
    if count > 2 {
        if true {
            break
        }
    }
}
assert(count == 3)
outer := 0
innerRuns := 0
while outer < 3 {
    outer += 1
    inner := 0
    while inner < 4 {
        inner += 1
        if inner == 2 {
            continue
        }
        if inner == 4 {
            break
        }
        innerRuns += 1
    }
}
assert(outer == 3)
assert(innerRuns == 6)
print("break continue ok")
```

Create the negative sources and companions:

```ms
// err_break_outside.ms
break
```

```ms
// err_continue_outside.ms
continue
```

Both `.exit` files contain `2`; both `.err` files contain `error E209:`.

- [ ] **Step 2: Verify the new behavior is red**

Run:

```bash
python run_tests.py --filter 'control_flow/while_loop.ms'
python run_tests.py --filter 'control_flow/bare_for.ms'
python run_tests.py --filter 'control_flow/break_continue.ms'
python run_tests.py --filter 'control_flow/err_*_outside.ms'
```

Expected: the positive scripts fail with E307 or break/continue unsupported diagnostics; the two negative scripts already pass with parser E209 and exit code 2.

- [ ] **Step 3: Add function-local loop storage and patch-list helpers**

Add `#include <limits.h>` and `#include <stdint.h>` as needed. Define these private types near `MsGlobalDecl`:

```c
#define MS_COMPILER_LOOP_INIT_CAP 8

struct MsJumpPatchList {
  int* pcs;
  int count;
  int cap;
};

struct MsLoopContext {
  int loopHead;
  struct MsJumpPatchList breaks;
  struct MsJumpPatchList continues;
  bool continueIsHead;
};
```

Add to `MsFuncCtx`:

```c
  struct MsLoopContext* loops;
  int loopDepth;
  int loopCap;
```

Add these helpers after `msCompilerPatchToCurrent`:

```c
static void msCompilerEmitBackEdge(struct MsCompiler* c, int targetPc) {
  int jumpPc = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
  if (jumpPc < 0 || c->sticky != MS_OK) {
    return;
  }
  MsResult result = msProtoPatchSAx(&c->fn->builder, jumpPc, targetPc - (jumpPc + 1));
  if (result != MS_OK) {
    msCompilerSyncBuilder(c);
  }
}

static void msCompilerPatchListDestroy(struct MsJumpPatchList* list) {
  msFree(list->pcs);
  memset(list, 0, sizeof(*list));
}

static bool msCompilerPatchListAppend(struct MsCompiler* c, struct MsJumpPatchList* list, int pc) {
  if (pc < 0 || c->sticky != MS_OK) {
    return false;
  }
  if (list->count == list->cap) {
    if (list->cap > INT_MAX / 2) {
      msCompilerFail(c, c->fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
      return false;
    }
    int newCap = list->cap == 0 ? MS_COMPILER_LOOP_INIT_CAP : list->cap * 2;
    if ((size_t)newCap > SIZE_MAX / sizeof(*list->pcs)) {
      msCompilerFail(c, c->fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
      return false;
    }
    int* pcs = msRealloc(list->pcs, (size_t)newCap * sizeof(*pcs));
    if (pcs == NULL) {
      msCompilerFail(c, c->fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
      return false;
    }
    list->pcs = pcs;
    list->cap = newCap;
  }
  list->pcs[list->count++] = pc;
  return true;
}

static bool msCompilerLoopEnter(struct MsCompiler* c, int loopHead, bool continueIsHead) {
  struct MsFuncCtx* fn = c->fn;
  if (fn->loopDepth == fn->loopCap) {
    if (fn->loopCap > INT_MAX / 2) {
      msCompilerFail(c, fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
      return false;
    }
    int newCap = fn->loopCap == 0 ? MS_COMPILER_LOOP_INIT_CAP : fn->loopCap * 2;
    if ((size_t)newCap > SIZE_MAX / sizeof(*fn->loops)) {
      msCompilerFail(c, fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
      return false;
    }
    struct MsLoopContext* loops = msRealloc(fn->loops, (size_t)newCap * sizeof(*loops));
    if (loops == NULL) {
      msCompilerFail(c, fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
      return false;
    }
    fn->loops = loops;
    fn->loopCap = newCap;
  }
  struct MsLoopContext* loop = &fn->loops[fn->loopDepth++];
  memset(loop, 0, sizeof(*loop));
  loop->loopHead = loopHead;
  loop->continueIsHead = continueIsHead;
  return true;
}

static void msCompilerResolveContinues(struct MsCompiler* c, int expectedStackDepth) {
  struct MsLoopContext* loop = &c->fn->loops[c->fn->loopDepth - 1];
  MS_ASSERT(!loop->continueIsHead);
  for (int i = 0; i < loop->continues.count; ++i) {
    msCompilerPatchToCurrent(c, loop->continues.pcs[i], expectedStackDepth);
  }
  msCompilerPatchListDestroy(&loop->continues);
}

static void msCompilerLoopLeave(struct MsCompiler* c, int exitJump, int expectedStackDepth) {
  struct MsFuncCtx* fn = c->fn;
  MS_ASSERT(fn->loopDepth > 0);
  struct MsLoopContext* loop = &fn->loops[fn->loopDepth - 1];
  MS_ASSERT(loop->continues.count == 0);
  if (exitJump >= 0) {
    msCompilerPatchToCurrent(c, exitJump, expectedStackDepth);
  }
  for (int i = 0; i < loop->breaks.count; ++i) {
    msCompilerPatchToCurrent(c, loop->breaks.pcs[i], expectedStackDepth);
  }
  msCompilerPatchListDestroy(&loop->breaks);
  msCompilerPatchListDestroy(&loop->continues);
  --fn->loopDepth;
}

static void msCompilerCompileBreak(struct MsCompiler* c) {
  MS_ASSERT(c->fn->loopDepth > 0);
  int jump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
  msCompilerPatchListAppend(c, &c->fn->loops[c->fn->loopDepth - 1].breaks, jump);
}

static void msCompilerCompileContinue(struct MsCompiler* c) {
  MS_ASSERT(c->fn->loopDepth > 0);
  struct MsLoopContext* loop = &c->fn->loops[c->fn->loopDepth - 1];
  if (loop->continueIsHead) {
    msCompilerEmitBackEdge(c, loop->loopHead);
  } else {
    int jump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
    msCompilerPatchListAppend(c, &loop->continues, jump);
  }
}
```

- [ ] **Step 4: Compile while and bare-for with structured cleanup**

Add:

```c
static void msCompilerCompileWhile(struct MsCompiler* c, const struct MsAst* node) {
  int baseDepth = c->fn->stackDepth;
  int loopHead = msCompilerCurrentPc(c);
  if (!msCompilerLoopEnter(c, loopHead, true)) {
    return;
  }
  msCompilerCompileExpr(c, node->as.whileStmt.cond);
  int exitJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
  msCompilerCompileStmt(c, node->as.whileStmt.body);
  msCompilerEmitBackEdge(c, loopHead);
  msCompilerLoopLeave(c, exitJump, baseDepth);
}

static void msCompilerCompileBareFor(struct MsCompiler* c, const struct MsAst* node) {
  int baseDepth = c->fn->stackDepth;
  int loopHead = msCompilerCurrentPc(c);
  if (!msCompilerLoopEnter(c, loopHead, true)) {
    return;
  }
  msCompilerCompileStmt(c, node->as.forStmt.body);
  msCompilerEmitBackEdge(c, loopHead);
  msCompilerLoopLeave(c, -1, baseDepth);
}
```

In the statement dispatcher, add `MS_AST_WHILE`, `MS_AST_BREAK`, and `MS_AST_CONTINUE` cases calling the helpers. Route `MS_FOR_BARE` to `msCompilerCompileBareFor`; keep `MS_FOR_IN` on an E307 path with message `for-in iteration belongs to task 26`.

- [ ] **Step 5: Release loop storage on every function-context exit**

In both successful `msCompilerPopFunc` cleanup and `msCompilerAbortFunc`, assert `fn->loopDepth == 0` and call `msFree(fn->loops)` before `msFree(fn)`. All loop compile functions must reach `msCompilerLoopLeave` after a successful enter, including when a child sets `c->sticky`.

- [ ] **Step 6: Build and verify loop behavior**

Run:

```bash
cmake --build build --config Debug
python run_tests.py --filter 'control_flow/while_loop.ms'
python run_tests.py --filter 'control_flow/bare_for.ms'
python run_tests.py --filter 'control_flow/break_continue.ms'
python run_tests.py --filter 'control_flow/err_*_outside.ms'
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all selected scripts and 14/14 CTests pass, with no live-allocation increase in compiler fixtures.

- [ ] **Step 7: Commit loop support**

```bash
git add src/compiler/ms_compiler.c tests/ms/control_flow
git commit -m '✨ feat(compiler): add loop control flow'
```

**Execution note:** Follow-up coverage executes three-part-`for` break/continue with `i += 1` post statements in both loop-nesting directions. The existing compiler OOM fixture now grows loop and patch-list storage beyond eight entries, covers deferred continues and nested-function loop cleanup, and iterates fail-after injection until compilation succeeds.

### Task 3: Complete three-part for and inc/dec

**Files:**
- Modify: `src/compiler/ms_compiler.c` loop and statement helpers
- Create: `tests/ms/control_flow/for_clause.ms`

- [ ] **Step 1: Write the three-part-for fixture**

Create `for_clause.ms`:

```ms
sum := 0
for i := 0; i < 10; i++ {
    sum += i
}
assert(sum == 45)
count := 3
for i := 3; i > 0; i-- {
    count -= 1
}
assert(count == 0)
stepped := 0
for i := 0; i < 6; i += 2 {
    stepped += 1
}
assert(stepped == 3)
for i := 0; i < 0; i++ {
    assert(false)
}
for i := 0; i < 1; i++ {
    pass
}
oddSum := 0
for i := 0; i < 10; i++ {
    if i % 2 == 0 {
        continue
    }
    oddSum += i
}
assert(oddSum == 25)
escaped := 0
for i := 0; i < 10; i++ {
    if i == 4 {
        break
    }
    escaped += 1
}
assert(escaped == 4)
print("for clause ok")
```

The second one-iteration loop proves the earlier loop variable is out of scope and can be redeclared.

- [ ] **Step 2: Verify the fixture fails before implementation**

Run:

```bash
python run_tests.py --filter 'control_flow/for_clause.ms'
```

Expected: FAIL with an unsupported inc/dec statement or a repeated loop-variable declaration, depending on which old path is reached first.

- [ ] **Step 3: Compile inc/dec through existing name resolution**

Add:

```c
static void msCompilerCompileIncDec(struct MsCompiler* c, const struct MsAst* node) {
  const struct MsAst* target = node->as.augAssign.target;
  if (target == NULL || target->kind != MS_AST_IDENT) {
    msCompilerReport(c, node, 307, "indexed/attribute inc/dec belongs to task 15/16");
    return;
  }
  struct MsString* name = msCompilerNameFromSlice(c, target->as.literal.start, target->as.literal.length);
  if (name == NULL || !msCompilerValidateStore(c, name, target)) {
    return;
  }
  msCompilerEmitLoadName(c, name);
  msCompilerEmitConstObject(c, (struct MsObject*)msIntNew(c->heap, 1));
  MsOpCode op = node->as.augAssign.op == MS_TOKEN_PLUS_PLUS ? MS_OP_ADD : MS_OP_SUB;
  MS_ASSERT(node->as.augAssign.op == MS_TOKEN_PLUS_PLUS || node->as.augAssign.op == MS_TOKEN_MINUS_MINUS);
  msCompilerEmitABC(c, op, 0, 0, -1);
  msCompilerEmitStoreName(c, name);
}
```

Dispatch `MS_AST_INC_DEC` to this helper.

- [ ] **Step 4: Replace the old three-part-for skeleton**

Use this complete structure:

```c
static void msCompilerCompileFor(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.forStmt.forKind == MS_FOR_BARE) {
    msCompilerCompileBareFor(c, node);
    return;
  }
  if (node->as.forStmt.forKind != MS_FOR_CLAUSE) {
    msCompilerReport(c, node, 307, "for-in iteration belongs to task 26");
    return;
  }
  int baseDepth = c->fn->stackDepth;
  msCompilerPushScope(c);
  if (c->sticky != MS_OK) {
    return;
  }
  msCompilerCompileStmt(c, node->as.forStmt.init);
  int loopHead = msCompilerCurrentPc(c);
  if (!msCompilerLoopEnter(c, loopHead, false)) {
    msCompilerPopScope(c);
    return;
  }
  msCompilerCompileExpr(c, node->as.forStmt.cond);
  int exitJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
  msCompilerCompileStmt(c, node->as.forStmt.body);
  msCompilerResolveContinues(c, baseDepth);
  msCompilerCompileStmt(c, node->as.forStmt.post);
  msCompilerEmitBackEdge(c, loopHead);
  msCompilerLoopLeave(c, exitJump, baseDepth);
  msCompilerPopScope(c);
}
```

Do not return early between successful loop entry and loop leave. The body and post must still be visited under the sticky-error model so cleanup remains structured.

- [ ] **Step 5: Verify for semantics and regressions**

Run:

```bash
cmake --build build --config Debug
python run_tests.py --filter 'control_flow/for_clause.ms'
python run_tests.py --filter 'control_flow/*.ms'
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all control-flow scripts and 14/14 CTests pass. In particular, `continue` executes `i++`, `break` skips it, and loop variables can be redeclared.

- [ ] **Step 6: Commit three-part-for support**

```bash
git add src/compiler/ms_compiler.c tests/ms/control_flow/for_clause.ms
git commit -m '✨ feat(compiler): complete three-part for loops'
```

### Task 4: Emit direct comparison chains and align VM opcodes

**Files:**
- Modify: `src/compiler/ms_compiler.c:611-642` and patch-list helpers
- Modify: `src/vm/ms_vm.c:669-704`
- Create: `tests/ms/control_flow/chain_compare.ms`

- [ ] **Step 1: Write direct-chain and short-circuit fixtures**

Create `chain_compare.ms`:

```ms
assert(1 < 2 < 3)
assert((1 < 2 > 3) == false)
assert(3 > 2 > 1)
assert(1 == 1 == 1)
assert(1 < 2 <= 2)
value := 5
assert(1 < value < 10)
assert(-2 < -1 < 0 < 1 < 2)
assert(nil is nil is nil)
assert((nil is nil is false) == false)
assert((2 < 1 < 1 / 0) == false)
print("chain compare ok")
```

Do not add a successful `IN` script here: list/dict membership execution belongs to task 16. This task only widens the VM's chain-opcode validation to include `IN`.

- [ ] **Step 2: Verify the fixture is red**

Run:

```bash
python run_tests.py --filter 'control_flow/chain_compare.ms'
```

Expected: FAIL with `comparison chains belong to task 12` before the new tests execute.

- [ ] **Step 3: Add shared comparison mapping**

Replace the local switch with file-private helpers:

```c
static MsOpCode msCompilerComparisonOpcode(MsCompareOp compare) {
  switch (compare) {
    case MS_CMP_LT: return MS_OP_LT;
    case MS_CMP_LE: return MS_OP_LE;
    case MS_CMP_GT: return MS_OP_GT;
    case MS_CMP_GE: return MS_OP_GE;
    case MS_CMP_EQ: return MS_OP_EQ;
    case MS_CMP_NE: return MS_OP_NE;
    case MS_CMP_IN:
    case MS_CMP_NOT_IN: return MS_OP_IN;
    case MS_CMP_IS:
    case MS_CMP_IS_NOT: return MS_OP_IS;
    default: return MS_OP_COUNT;
  }
}

static bool msCompilerComparisonIsNegated(MsCompareOp compare) {
  return compare == MS_CMP_NOT_IN || compare == MS_CMP_IS_NOT;
}
```

Keep single-segment behavior unchanged except for calling these helpers and emitting `NOT` for negated comparisons.

- [ ] **Step 4: Add Bx patching and direct-chain generation**

Add:

```c
static void msCompilerPatchChainToCurrent(struct MsCompiler* c, int chainPc) {
  if (chainPc < 0 || c->sticky != MS_OK) {
    return;
  }
  int offset = msCompilerCurrentPc(c) - (chainPc + 1);
  if (offset < 0 || offset > UINT16_MAX) {
    msCompilerFail(c, c->fn->currentNode, MS_ERROR_SYNTAX, 309, "comparison chain size limit exceeded");
    return;
  }
  uint32_t* instruction = &c->fn->builder.proto.code[chainPc];
  MS_ASSERT(msOpDecodeOp(*instruction) == MS_OP_CMP_CHAIN);
  *instruction = msOpEncodeABC(MS_OP_CMP_CHAIN, msOpDecodeA(*instruction), (uint16_t)offset);
}
```

Add a direct-chain helper. It must leave one result on both success and failure and free its pc list on every return:

```c
static void msCompilerCompileDirectChain(struct MsCompiler* c, const struct MsAst* node) {
  size_t count = node->as.compareChain.opCount;
  struct MsJumpPatchList chains = { NULL, 0, 0 };
  msCompilerCompileExpr(c, node->as.compareChain.operands.items[0]);
  msCompilerCompileExpr(c, node->as.compareChain.operands.items[1]);
  for (size_t i = 0; i + 1 < count && c->sticky == MS_OK; ++i) {
    int chain = msCompilerEmitABC(c, MS_OP_CMP_CHAIN,
        (uint8_t)msCompilerComparisonOpcode(node->as.compareChain.ops[i]), 0, -1);
    if (!msCompilerPatchListAppend(c, &chains, chain)) {
      break;
    }
    msCompilerCompileExpr(c, node->as.compareChain.operands.items[i + 2]);
  }
  if (c->sticky == MS_OK) {
    msCompilerEmitABC(c, msCompilerComparisonOpcode(node->as.compareChain.ops[count - 1]), 0, 0, -1);
  }
  for (int i = 0; i < chains.count; ++i) {
    msCompilerPatchChainToCurrent(c, chains.pcs[i]);
  }
  msCompilerPatchListDestroy(&chains);
}
```

In `msCompilerCompileComparison`, validate `operands.count == opCount + 1`; route `opCount == 1` through the existing single comparison and `opCount > 1` with no negated segment through this helper. Leave negated multi-chains to Task 5, initially reporting E307 without emitting a partial expression.

- [ ] **Step 5: Align VM comparison dispatch**

Add `case MS_OP_IN:` beside `case MS_OP_IS:` for ordinary comparisons. Change chain validation to:

```c
        if (op < MS_OP_EQ || op > MS_OP_IN) {
          result = msVmErrorAt(L, frame, "invalid comparison chain opcode");
          break;
        }
```

This reuses `msVmCompare`; it does not implement list/dict membership, which remains task 16.

- [ ] **Step 6: Verify direct chains and regressions**

Run:

```bash
cmake --build build --config Debug
python run_tests.py --filter 'control_flow/chain_compare.ms'
python run_tests.py --filter 'control_flow/*.ms'
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all selected tests pass. The `1 / 0` operand is not evaluated after the first comparison fails.

- [ ] **Step 7: Commit direct comparison chains**

```bash
git add src/compiler/ms_compiler.c src/vm/ms_vm.c tests/ms/control_flow/chain_compare.ms
git commit -m '✨ feat(compiler): add short-circuit comparison chains'
```

### Task 5: Compile negated chains with a hidden local

**Files:**
- Modify: `src/compiler/ms_compiler.c` comparison generator
- Modify: `tests/ms/control_flow/chain_compare.ms`

- [ ] **Step 1: Add negated-chain and generic-path short-circuit assertions**

Insert before the final `print` in `chain_compare.ms`:

```ms
assert(nil is nil is not false)
assert(nil is not false is false)
assert(3 > 2 is not nil)
assert((1 < 0 is not 1 / 0) == false)
```

The first three prove success and result types through mixed direct/negated segments; the fourth proves the generic path also skips unevaluated operands after failure.

- [ ] **Step 2: Verify the negated cases fail**

Run:

```bash
python run_tests.py --filter 'control_flow/chain_compare.ms'
```

Expected: FAIL with the Task 4 placeholder E307 for a negated multi-segment chain.

- [ ] **Step 3: Implement the generic expansion**

Add this helper and route every multi-segment chain containing `IS_NOT` or `NOT_IN` to it:

```c
static void msCompilerCompileGenericChain(struct MsCompiler* c, const struct MsAst* node) {
  size_t count = node->as.compareChain.opCount;
  int baseDepth = c->fn->stackDepth;
  int savedLocals = c->fn->scope.localCount;
  int slot = msCompilerAllocHiddenLocal(c);
  struct MsJumpPatchList failures = { NULL, 0, 0 };
  if (slot < 0) {
    return;
  }
  for (size_t i = 0; i < count && c->sticky == MS_OK; ++i) {
    if (i == 0) {
      msCompilerCompileExpr(c, node->as.compareChain.operands.items[0]);
    } else {
      msCompilerEmitABC(c, MS_OP_LOAD_LOCAL, (uint8_t)slot, 0, 1);
    }
    msCompilerCompileExpr(c, node->as.compareChain.operands.items[i + 1]);
    msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
    msCompilerEmitABC(c, MS_OP_LOAD_LOCAL, (uint8_t)slot, 0, 1);
    msCompilerEmitABC(c, msCompilerComparisonOpcode(node->as.compareChain.ops[i]), 0, 0, -1);
    if (msCompilerComparisonIsNegated(node->as.compareChain.ops[i])) {
      msCompilerEmitABC(c, MS_OP_NOT, 0, 0, 0);
    }
    int failure = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
    if (!msCompilerPatchListAppend(c, &failures, failure)) {
      break;
    }
  }
  if (c->sticky == MS_OK) {
    msCompilerEmitABC(c, MS_OP_LOAD_TRUE, 0, 0, 1);
  }
  int endJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
  c->fn->stackDepth = baseDepth;
  for (int i = 0; i < failures.count; ++i) {
    msCompilerPatchToCurrent(c, failures.pcs[i], baseDepth);
  }
  if (c->sticky == MS_OK) {
    msCompilerEmitABC(c, MS_OP_LOAD_FALSE, 0, 0, 1);
  }
  msCompilerPatchToCurrent(c, endJump, baseDepth + 1);
  msCompilerPatchListDestroy(&failures);
  c->fn->scope.localCount = savedLocals;
  if (c->sticky != MS_OK) {
    c->fn->stackDepth = baseDepth;
  }
}
```

Every successful non-final iteration begins with one saved right operand; every conditional jump returns compilation to `baseDepth`. The true and false paths each push exactly one boolean before the common end.

- [ ] **Step 4: Verify all comparison-chain paths**

Run:

```bash
cmake --build build --config Debug
python run_tests.py --filter 'control_flow/chain_compare.ms'
python run_tests.py --filter 'control_flow/*.ms'
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all selected tests pass and compiler fixtures report no leaked allocations.

- [ ] **Step 5: Commit generic comparison chains**

```bash
git add src/compiler/ms_compiler.c tests/ms/control_flow/chain_compare.ms
git commit -m '✨ feat(compiler): support negated comparison chains'
```

### Task 6: Update task status and verify the deliverable

**Files:**
- Modify: `docs/tasks/12-control-flow.md`
- Modify: `docs/tasks/README.md:53`
- Modify: `docs/superpowers/specs/2026-11-07-task-12-control-flow-design.md`
- Modify: `docs/superpowers/plans/2026-11-07-task-12-control-flow.md`

- [ ] **Step 1: Record actual behavior and verified limits**

Change task 12's status to `✅` in both task files. Check only acceptance items verified in this worktree. Preserve these boundaries: `for ... in` belongs to task 26, side-effect call counting for middle operands follows task 13, container membership execution belongs to task 16, and Linux/macOS/ASAN are not claimed on this Windows MSVC host. Mark executed plan checkboxes `[x]` and record any code-level deviation beside the affected step.

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
git add docs/tasks/12-control-flow.md docs/tasks/README.md docs/superpowers/specs/2026-11-07-task-12-control-flow-design.md docs/superpowers/plans/2026-11-07-task-12-control-flow.md
git commit -m '📝 docs(tasks): complete control flow'
```
