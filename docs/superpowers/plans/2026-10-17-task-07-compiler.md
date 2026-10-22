# Task 07 Compiler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付任务 07 的单趟 AST → 字节码编译器，并补齐嵌套 Proto 的安全常量表示、递归所有权及完整 C 单元测试。

**Architecture:** `src/compiler/ms_compiler.c` 以函数上下文和作用域栈直接驱动 `MsProtoBuilder`，不设置 IR 或独立优化 pass；常量折叠和死跳转消除内联于生成。`MsProto` 使用内部 `MS_TYPE_PROTO` 对象头标签但不挂入 `MsHeap`，父 Proto/builder 递归拥有子 Proto，顶层产物仍由 `msProtoFree` 释放。

**Tech Stack:** C11、CMake + Ninja、MSVC `/W4 /WX`、自研 `ms_test.h`、现有 lexer/parser/AST、对象模型和 Proto builder。

**权威规格：** `docs/tasks/07-compiler.md` 与已批准的 `docs/superpowers/specs/2026-10-17-task-07-compiler-design.md`。两者冲突时以后者及本计划中明确记录的修正为准。

---

## 代码库事实（实现者必读）

- 主仓库为 `C:\Workspace\Repositories\mslang-cc`；实施在独立 worktree `.worktrees/task-07-compiler` 中进行，以下命令均以该 worktree 根目录为当前目录。
- 首次配置：`cmake -S . -B build -G Ninja`。增量构建：`cmake --build build`。
- 单测：`ctest --test-dir build -R '^test_compiler$' --output-on-failure`；全量：`ctest --test-dir build --output-on-failure`。
- C 的 TDD 红步可以是缺少标识符导致的编译/链接失败。新增测试后先看到预期红，再写实现。
- `tests/c/ms_test.h` 没有 setUp/tearDown；每个测试通过 fixture 自建自毁资源。`MS_ASSERT_EQ` 只适合整数，指针用 `MS_ASSERT_TRUE`，浮点符号位用 `signbit` 或 `memcpy`。
- `msMemSetFailAfter(n)` 不会在 `msMemResetStats()` 时复位；每个 OOM 测试拆卸前必须调用 `msMemSetFailAfter(-1)`。
- Release 下只在 `MS_ASSERT` 中引用的变量必须用 `MS_UNUSED` 消除 `/WX` 未使用警告。
- 所有 C 源文件使用 ASCII、UTF-8 无 BOM、LF、2 空格缩进、120 列、K&R；内部结构体不 typedef，堆分配只经 `msAlloc/msRealloc/msFree`。
- Task 5 在分派前提供 stmt/expr 前置声明，并把分派定义插入 `msCompilerInit` 之前；Task 6 起新增的 static 生成助手插入前置声明之后、`msCompilerCompileExpr` 定义之前，避免隐式声明和定义顺序错误。
- 提交格式严格为 `<gitmoji> <type>(<scope>): <message>`；每个任务只提交本任务列出的文件。

## 文件结构

| 文件 | 责任 |
|---|---|
| `src/object/ms_object.h` | 增加脚本不可见的内部 `MS_TYPE_PROTO` 标签，保持 20 个堆类型不变 |
| `src/object/ms_object.c` | 内部标签名称查询；普通堆分配仍拒绝 Proto 标签 |
| `src/vm/ms_proto.h` | Proto 对象头、子 Proto 常量接口与所有权注释 |
| `src/vm/ms_proto.c` | 子 Proto 入池/查询/递归释放、浮点零去重保护 |
| `src/compiler/ms_compiler.h` | 编译选项、编译器状态和公开入口 |
| `src/compiler/ms_compiler.c` | 函数上下文、作用域、字面量、表达式、函数、诊断和单趟生成 |
| `tests/c/test_object.c` | 内部标签不扩大脚本类型数的回归测试 |
| `tests/c/test_proto.c` | 子 Proto 所有权和浮点零常量池测试 |
| `tests/c/test_compiler.c` | 真实 lexer → parser → compiler 端到端 C 单元测试 |
| `CMakeLists.txt` | 编译器库源文件及 `test_compiler` 注册 |
| `docs/tasks/05-bytecode-proto.md` | Proto 对象头、内部标签和递归所有权同步 |
| `docs/tasks/06-object-model.md` | 内部 Proto 标签与 20 个堆类型的边界同步 |
| `docs/tasks/07-compiler.md` | 隐藏槽、空模块栈深及实现验收状态同步 |
| `docs/tasks/README.md` | 任务 07 完成状态 |

---

### Task 1: 增加内部 Proto 类型标签

**Files:**
- Modify: `src/object/ms_object.h:14-24,31-36`
- Modify: `src/object/ms_object.c:24-29`
- Modify: `tests/c/test_object.c:11-35,590-617`

- [x] **Step 1: 写失败测试**

在 `tests/c/test_object.c` 增加：

```c
MS_TEST(object, internalProtoTagIsOutsideHeapTypes) {
  MS_ASSERT_EQ(20, MS_TYPE_COUNT);
  MS_ASSERT_TRUE(MS_TYPE_PROTO > MS_TYPE_COUNT);
  MS_ASSERT_TRUE(strcmp(msTypeTagName(MS_TYPE_PROTO), "MS_TYPE_PROTO") == 0);
  MS_ASSERT_TRUE(strcmp(msTypeTagName(MS_TYPE_COUNT), "MS_TYPE_INVALID") == 0);
}
```

在 `msTestCases` 的 `object.typeTagNamesCompleteDistinctOrdered` 项之后注册：

```c
  {"object.internalProtoTagIsOutsideHeapTypes", testobjectinternalProtoTagIsOutsideHeapTypes},
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_object && ctest --test-dir build -R '^test_object$' --output-on-failure`

Expected: 编译失败，`MS_TYPE_PROTO` 未声明。

- [x] **Step 3: 实现内部标签**

在 `MsTypeTag` 中把 `MS_TYPE_PROTO` 放在 `MS_TYPE_COUNT` 后，保持前 20 个标签和 `MS_TYPE_COUNT` 数值不变：

```c
typedef enum {
  MS_TYPE_NIL, MS_TYPE_BOOL, MS_TYPE_INT, MS_TYPE_FLOAT,
  MS_TYPE_STRING, MS_TYPE_BYTES, MS_TYPE_LIST, MS_TYPE_TUPLE,
  MS_TYPE_DICT, MS_TYPE_SET, MS_TYPE_FUNCTION, MS_TYPE_CLASS,
  MS_TYPE_INSTANCE, MS_TYPE_MODULE, MS_TYPE_CHANNEL, MS_TYPE_COROUTINE,
  MS_TYPE_ITERATOR, MS_TYPE_BOUND_METHOD, MS_TYPE_C_FUNCTION,
  MS_TYPE_C_TYPE,
  MS_TYPE_COUNT,
  MS_TYPE_PROTO
} MsTypeTag;
```

把对象头 `type` 注释改为：

```c
  struct MsType* type;       // heap objects: never NULL; internal protos: NULL
```

在 `msTypeTagName` 范围检查前增加：

```c
  if (tag == MS_TYPE_PROTO) {
    return "MS_TYPE_PROTO";
  }
```

`msObjectAlloc` 的 `tag < MS_TYPE_COUNT` 断言保持不变，禁止把 Proto 当普通堆对象分配。

- [x] **Step 4: 运行测试并确认绿**

Run: `cmake --build build --target test_object && ctest --test-dir build -R '^test_object$' --output-on-failure`

Expected: `100% tests passed, 0 tests failed out of 1`，可执行文件显示 `27 of 27 tests passed`。

- [x] **Step 5: 提交**

```bash
git add src/object/ms_object.h src/object/ms_object.c tests/c/test_object.c
git commit -m "✨ feat(object): add internal proto type tag"
```

### Task 2: 保留常量池中的浮点零符号

**Files:**
- Modify: `src/vm/ms_proto.c:1-8,93-119`
- Modify: `tests/c/test_proto.c:1-10,700-734`

- [x] **Step 1: 写失败测试**

在 `tests/c/test_proto.c` 增加 `<math.h>` include，并加入：

```c
MS_TEST(proto, addConstPreservesSignedZero) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  struct MsFloat* positive = msFloatNew(&heap, 0.0);
  struct MsFloat* negative = msFloatNew(&heap, -0.0);
  MS_ASSERT_TRUE(positive != NULL && negative != NULL);
  int first = msProtoAddConst(&b, (struct MsObject*)positive);
  int second = msProtoAddConst(&b, (struct MsObject*)negative);
  MS_ASSERT_EQ(0, first);
  MS_ASSERT_EQ(1, second);
  MS_ASSERT_EQ(2, b.proto.constsLen);
  MS_ASSERT_TRUE(!signbit(msFloatValue(b.proto.consts[0])));
  MS_ASSERT_TRUE(signbit(msFloatValue(b.proto.consts[1])));
  msProtoBuilderDestroy(&b);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}
```

注册：

```c
  {"Proto.AddConstPreservesSignedZero", testprotoaddConstPreservesSignedZero},
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_proto && ctest --test-dir build -R '^test_proto$' --output-on-failure`

Expected: `second == 1` 和 `constsLen == 2` 断言失败；当前实现把两个零去重为一个常量。

- [x] **Step 3: 实现同类型常量比较助手**

在 `ms_proto.c` include 区加入 `<math.h>`，并新增：

```c
static bool msProtoConstEquals(const struct MsObject* a, const struct MsObject* b) {
  if (a->header.tag != b->header.tag) {
    return false;
  }
  if (a->header.tag == MS_TYPE_FLOAT) {
    double av = msFloatValue(a);
    double bv = msFloatValue(b);
    if (av == 0.0 && bv == 0.0 && signbit(av) != signbit(bv)) {
      return false;
    }
  }
  return msObjectValueEquals(a, b);
}
```

把 `msProtoAddConst` 去重循环的条件改为：

```c
    if (msProtoConstEquals(b->proto.consts[i], value)) {
      return i;
    }
```

- [x] **Step 4: 运行 Proto 全测**

Run: `cmake --build build --target test_proto && ctest --test-dir build -R '^test_proto$' --output-on-failure`

Expected: 全部通过，既有 int/float 分槽和 NaN 不去重行为不变。

- [x] **Step 5: 提交**

```bash
git add src/vm/ms_proto.c tests/c/test_proto.c
git commit -m "🐛 fix(vm): preserve signed zero constants"
```

### Task 3: 子 Proto 常量接口与递归所有权

**Files:**
- Modify: `src/vm/ms_proto.h:1-15,31-48,61-105`
- Modify: `src/vm/ms_proto.c:93-203`
- Modify: `tests/c/test_proto.c`

- [x] **Step 1: 写失败测试**

在 `tests/c/test_proto.c` 增加构建助手和测试：

```c
static struct MsProto* msTestMakeRawProto(void) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_RETURN, 0, 0));
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  return proto;
}

MS_TEST(proto, nestedProtoOwnershipAndLookup) {
  msMemResetStats();
  struct MsProtoBuilder parent;
  msProtoBuilderInit(&parent);
  MS_ASSERT_EQ(MS_TYPE_PROTO, parent.proto.header.tag);
  struct MsProto* child = msTestMakeRawProto();
  MS_ASSERT_TRUE(child != NULL);
  MS_ASSERT_EQ(0, msProtoAddProto(&parent, child));
  MS_ASSERT_EQ(0, msProtoEmitABC(&parent, MS_OP_RETURN, 0, 0));
  struct MsProto* built = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&parent, &built));
  MS_ASSERT_TRUE(msProtoConstProto(built, 0) == child);
  MS_ASSERT_TRUE(msProtoConstProto(built, 1) == NULL);
  msProtoFree(built);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, failedProtoAddLeavesOwnershipWithCaller) {
  msMemResetStats();
  struct MsProtoBuilder parent;
  msProtoBuilderInit(&parent);
  MS_ASSERT_EQ(-1, msProtoEmitSAx(&parent, MS_OP_JUMP, 8388608));
  struct MsProto* child = msTestMakeRawProto();
  MS_ASSERT_EQ(-1, msProtoAddProto(&parent, child));
  msProtoFree(child);
  msProtoBuilderDestroy(&parent);
  msProtoBuilderInit(&parent);
  child = msTestMakeRawProto();
  MS_ASSERT_EQ(-1, msProtoAddConst(&parent, (struct MsObject*)child));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, parent.error);
  msProtoFree(child);
  msProtoBuilderDestroy(&parent);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, builderDestroyReleasesNestedProtos) {
  msMemResetStats();
  struct MsProtoBuilder parent;
  msProtoBuilderInit(&parent);
  struct MsProto* grandchild = msTestMakeRawProto();
  struct MsProtoBuilder childBuilder;
  msProtoBuilderInit(&childBuilder);
  MS_ASSERT_EQ(0, msProtoAddProto(&childBuilder, grandchild));
  MS_ASSERT_EQ(0, msProtoEmitABC(&childBuilder, MS_OP_RETURN, 0, 0));
  struct MsProto* nested = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&childBuilder, &nested));
  MS_ASSERT_EQ(0, msProtoAddProto(&parent, nested));
  msProtoBuilderDestroy(&parent);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}
```

注册三个用例：

```c
  {"Proto.NestedProtoOwnershipAndLookup", testprotonestedProtoOwnershipAndLookup},
  {"Proto.FailedProtoAddLeavesOwnershipWithCaller", testprotofailedProtoAddLeavesOwnershipWithCaller},
  {"Proto.BuilderDestroyReleasesNestedProtos", testprotobuilderDestroyReleasesNestedProtos},
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_proto`

Expected: 编译失败，`msProtoAddProto`、`msProtoConstProto` 和 `MsProto.header` 不存在。

- [x] **Step 3: 扩展 Proto 头文件和 API**

`ms_proto.h` 改为 include `"object/ms_object.h"`，保留 `struct MsString` 前向声明；在 `struct MsProto` 首行加入：

```c
  struct MsObjectHeader header; // internal MS_TYPE_PROTO; not linked into MsHeap
```

在 `msProtoAddConst` 后声明：

```c
// Adds a child proto without dedup. Success transfers ownership to b; on
// failure the caller still owns proto and must free it.
int msProtoAddProto(struct MsProtoBuilder* b, struct MsProto* proto);

// Returns the child proto at index, or NULL for a bad index/non-proto entry.
const struct MsProto* msProtoConstProto(const struct MsProto* proto, int index);
```

更新 `msProtoFree` 注释：递归释放子 Proto，但不释放堆常量、名称和来源文件。

- [x] **Step 4: 实现递归释放和子 Proto 入池**

在 `ms_proto.c` 增加：

```c
static void msProtoReleaseContents(struct MsProto* proto) {
  for (int i = 0; i < proto->constsLen; ++i) {
    struct MsObject* value = proto->consts[i];
    if (value != NULL && value->header.tag == MS_TYPE_PROTO) {
      msProtoFree((struct MsProto*)value);
    }
  }
  msFree(proto->code);
  msFree(proto->consts);
  msFree(proto->tryBlocks);
  msFree(proto->lines);
}

int msProtoAddProto(struct MsProtoBuilder* b, struct MsProto* proto) {
  if (b->error != MS_OK) {
    return -1;
  }
  if (proto == NULL || proto->header.tag != MS_TYPE_PROTO) {
    b->error = MS_ERROR_SYNTAX;
    return -1;
  }
  if (b->proto.constsLen >= MS_PROTO_CONSTS_MAX) {
    b->error = MS_ERROR_SYNTAX;
    return -1;
  }
  if (b->proto.constsLen == b->constsCap
      && !msProtoGrowArray((void**)&b->proto.consts, &b->constsCap, sizeof(struct MsObject*))) {
    b->error = MS_ERROR_OOM;
    return -1;
  }
  b->proto.consts[b->proto.constsLen] = (struct MsObject*)proto;
  return b->proto.constsLen++;
}

const struct MsProto* msProtoConstProto(const struct MsProto* proto, int index) {
  if (proto == NULL || index < 0 || index >= proto->constsLen) {
    return NULL;
  }
  struct MsObject* value = proto->consts[index];
  if (value == NULL || value->header.tag != MS_TYPE_PROTO) {
    return NULL;
  }
  return (const struct MsProto*)value;
}
```

在 `msProtoAddConst` 的健康检查后加入：

```c
  if (value->header.tag == MS_TYPE_PROTO) {
    b->error = MS_ERROR_SYNTAX;
    return -1;
  }
```

`msProtoBuilderInit` 在 `memset` 后初始化对象头：

```c
  b->proto.header.type = NULL;
  b->proto.header.tag = (uint8_t)MS_TYPE_PROTO;
  b->proto.header.markColor = MS_MARK_WHITE;
  b->proto.header.gcNext = NULL;
```

`msProtoBuilderDestroy` 改为：

```c
void msProtoBuilderDestroy(struct MsProtoBuilder* b) {
  msProtoReleaseContents(&b->proto);
  msProtoBuilderInit(b);
}
```

`msProtoFree` 改为：

```c
void msProtoFree(struct MsProto* proto) {
  if (proto == NULL) {
    return;
  }
  msProtoReleaseContents(proto);
  msFree(proto);
}
```

- [x] **Step 5: 运行 Proto 与对象全测**

Run: `cmake --build build --target test_proto test_object && ctest --test-dir build -R '^test_(proto|object)$' --output-on-failure`

Expected: 两个测试全部通过，无泄漏；既有 builder/build/OOM 用例不因递归释放而双重释放。

- [x] **Step 6: 提交**

```bash
git add src/vm/ms_proto.h src/vm/ms_proto.c tests/c/test_proto.c
git commit -m "✨ feat(vm): own nested proto constants"
```

### Task 4: 编译器骨架、测试 fixture 与空模块

**Files:**
- Create: `src/compiler/ms_compiler.h`
- Create: `src/compiler/ms_compiler.c`
- Create: `tests/c/test_compiler.c`
- Modify: `CMakeLists.txt:41-52,89-99`

- [x] **Step 1: 写空模块失败测试和 fixture**

创建 `tests/c/test_compiler.c`，第一个 include 是被测头：

```c
#include "compiler/ms_compiler.h"

#include <string.h>

#include "core/ms_memory.h"
#include "lexer/ms_lexer.h"
#include "object/ms_string.h"
#include "parser/ms_ast.h"
#include "parser/ms_parser.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"
#include "ms_test.h"

typedef struct {
  struct MsHeap heap;
  struct MsDiagList diags;
  struct MsAstArena arena;
  struct MsLexer lexer;
  struct MsParser parser;
  struct MsCompiler compiler;
  struct MsAst* root;
  struct MsProto* proto;
  MsResult result;
} MsTestFixture;

static void msTestCompile(MsTestFixture* fix, const char* source, uint32_t flags) {
  memset(fix, 0, sizeof(*fix));
  msMemResetStats();
  MS_ASSERT_EQ(MS_OK, msHeapInit(&fix->heap));
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&fix->diags, "test.ms"));
  msAstArenaInit(&fix->arena);
  msLexerInit(&fix->lexer, source, strlen(source), "test.ms", &fix->diags);
  msParserInit(&fix->parser, &fix->lexer, &fix->arena, &fix->diags);
  msCompilerInit(&fix->compiler, &fix->heap, &fix->diags, "test.ms", flags);
  fix->result = msParserParse(&fix->parser, &fix->root);
  msLexerDestroy(&fix->lexer);
  if (fix->result == MS_OK) {
    fix->result = msCompilerCompile(&fix->compiler, fix->root, &fix->proto);
  }
}

static void msTestFixtureDestroy(MsTestFixture* fix) {
  msMemSetFailAfter(-1);
  msCompilerDestroy(&fix->compiler);
  msProtoFree(fix->proto);
  msAstArenaDestroy(&fix->arena);
  msHeapDestroy(&fix->heap);
  msDiagListDestroy(&fix->diags);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

static void msTestAssertOps(const struct MsProto* proto, const MsOpCode* ops, size_t count) {
  MS_ASSERT_TRUE(proto != NULL);
  if (proto == NULL) {
    return;
  }
  MS_ASSERT_EQ((long long)count, proto->codeLen);
  size_t n = count < (size_t)proto->codeLen ? count : (size_t)proto->codeLen;
  for (size_t i = 0; i < n; ++i) {
    MS_ASSERT_EQ(ops[i], msOpDecodeOp(proto->code[i]));
  }
}

MS_TEST(compiler, emptyModule) {
  MsTestFixture fix;
  msTestCompile(&fix, "", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(0, (long long)msDiagListCount(&fix.diags));
  static const MsOpCode expected[] = {MS_OP_LOAD_NIL, MS_OP_RETURN};
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(0, fix.proto->paramCount);
  MS_ASSERT_EQ(0, fix.proto->localCount);
  MS_ASSERT_EQ(1, fix.proto->stackSize);
  MS_ASSERT_TRUE(strcmp(msStringData(fix.proto->name), "<main>") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(fix.proto->sourceFile), "test.ms") == 0);
  MS_ASSERT_TRUE(!fix.proto->isAsync && !fix.proto->hasVarArgs && !fix.proto->hasKwArgs);
  msTestFixtureDestroy(&fix);
}

static const MsTestCase msTestCases[] = {
  {"Compiler.EmptyModule", testcompileremptyModule},
};

MS_TEST_MAIN(msTestCases)
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_compiler`

Expected: 配置或编译失败，`src/compiler/ms_compiler.c` / `ms_compiler.h` 不存在，目标也未注册。

- [x] **Step 3: 创建公开头**

创建 `src/compiler/ms_compiler.h`：

```c
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
```

- [x] **Step 4: 实现最小函数上下文和空模块生成**

在 `ms_compiler.c` 中定义后续任务继续使用的完整上下文：

```c
#include "compiler/ms_compiler.h"

#include <stdbool.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "object/ms_object.h"
#include "object/ms_string.h"
#include "parser/ms_ast.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"

#define MS_COMPILER_MAX_LOCALS 256
#define MS_COMPILER_MAX_SCOPE_DEPTH 128
#define MS_COMPILER_MAX_NODE_DEPTH 512

struct MsLocalSlot {
  struct MsString* name;
  int depth;
  bool isCaptured;
};

struct MsScope {
  int localBase;
  int discardSlot;
};

struct MsFuncCtx {
  struct MsFuncCtx* parent;
  struct MsProtoBuilder builder;
  const struct MsAst* funcNode;
  const struct MsAst* currentNode;
  struct MsLocalSlot locals[MS_COMPILER_MAX_LOCALS];
  int localCount;
  int localMax;
  struct MsScope scopes[MS_COMPILER_MAX_SCOPE_DEPTH];
  int scopeDepth;
  int stackDepth;
  int stackMax;
  bool isModule;
  bool isAsync;
};

static void msCompilerReport(struct MsCompiler* c, const struct MsAst* node,
    uint32_t code, const char* message) {
  if (c->sticky == MS_ERROR_OOM) {
    return;
  }
  uint32_t line = node != NULL ? node->line : 0;
  uint32_t column = node != NULL ? node->column : 0;
  bool reported = msDiagReport(c->diags, line, column, code, "%s", message);
  if ((!reported || msDiagListIsFull(c->diags)) && c->sticky == MS_OK) {
    c->sticky = MS_ERROR_SYNTAX;
  }
}

static void msCompilerFail(struct MsCompiler* c, const struct MsAst* node,
    MsResult result, uint32_t code, const char* message) {
  if (c->sticky != MS_OK) {
    return;
  }
  msCompilerReport(c, node, code, message);
  c->sticky = result;
}

static void msCompilerSyncBuilder(struct MsCompiler* c) {
  MsResult result = c->fn->builder.error;
  if (result == MS_ERROR_OOM) {
    msCompilerFail(c, c->fn->currentNode, result, 0, "out of memory");
  } else if (result != MS_OK) {
    msCompilerFail(c, c->fn->currentNode, result, 309, "proto size limit exceeded");
  }
}

static int msCompilerEmitABC(struct MsCompiler* c, MsOpCode op, uint8_t a,
    uint16_t bx, int stackDelta) {
  if (c->sticky != MS_OK) {
    return -1;
  }
  int pc = msProtoEmitABC(&c->fn->builder, op, a, bx);
  if (pc < 0) {
    msCompilerSyncBuilder(c);
    return -1;
  }
  c->fn->stackDepth += stackDelta;
  MS_ASSERT(c->fn->stackDepth >= 0);
  if (c->fn->stackDepth > c->fn->stackMax) {
    c->fn->stackMax = c->fn->stackDepth;
  }
  return pc;
}

static void msCompilerAbortFunc(struct MsCompiler* c) {
  struct MsFuncCtx* fn = c->fn;
  MS_ASSERT(fn != NULL);
  c->fn = fn->parent;
  msProtoBuilderDestroy(&fn->builder);
  msFree(fn);
}

static struct MsFuncCtx* msCompilerPushFunc(struct MsCompiler* c, const struct MsAst* funcNode) {
  struct MsFuncCtx* fn = msAlloc(sizeof(*fn));
  if (fn == NULL) {
    msCompilerFail(c, funcNode, MS_ERROR_OOM, 0, "out of memory");
    return NULL;
  }
  memset(fn, 0, sizeof(*fn));
  fn->parent = c->fn;
  fn->funcNode = funcNode;
  fn->currentNode = funcNode;
  fn->isModule = funcNode == NULL;
  fn->scopes[0].discardSlot = -1;
  msProtoBuilderInit(&fn->builder);
  c->fn = fn;
  const char* name = "<main>";
  struct MsString* protoName = msStringFromCStr(c->heap, name);
  if (protoName == NULL) {
    msCompilerFail(c, funcNode, MS_ERROR_OOM, 0, "out of memory");
  } else {
    fn->builder.proto.name = protoName;
  }
  if (c->sourceFile != NULL) {
    fn->builder.proto.sourceFile = msStringFromCStr(c->heap, c->sourceFile);
    if (fn->builder.proto.sourceFile == NULL) {
      msCompilerFail(c, funcNode, MS_ERROR_OOM, 0, "out of memory");
    }
  }
  if (c->sticky != MS_OK) {
    msCompilerAbortFunc(c);
    return NULL;
  }
  return fn;
}

static MsResult msCompilerPopFunc(struct MsCompiler* c, struct MsProto** out) {
  struct MsFuncCtx* fn = c->fn;
  struct MsProto* proto = NULL;
  *out = NULL;
  if (c->sticky == MS_OK) {
    int codeLen = fn->builder.proto.codeLen;
    if (codeLen == 0
        || msOpDecodeOp(fn->builder.proto.code[codeLen - 1]) != MS_OP_RETURN) {
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      msCompilerEmitABC(c, MS_OP_RETURN, 0, 0, -1);
    }
    fn->builder.proto.localCount = fn->localMax;
    fn->builder.proto.stackSize = fn->stackMax;
    MsResult result = msProtoBuild(&fn->builder, &proto);
    if (result != MS_OK) {
      msCompilerSyncBuilder(c);
    }
  }
  if (c->sticky == MS_OK) {
    c->fn = fn->parent;
    msFree(fn);
    *out = proto;
    return MS_OK;
  }
  MsResult result = c->sticky;
  msCompilerAbortFunc(c);
  return result;
}

void msCompilerInit(struct MsCompiler* c, struct MsHeap* heap, struct MsDiagList* diags,
    const char* sourceFile, uint32_t flags) {
  c->heap = heap;
  c->diags = diags;
  c->sourceFile = sourceFile;
  c->flags = flags;
  c->fn = NULL;
  c->nodeDepth = 0;
  c->sticky = MS_OK;
}

void msCompilerDestroy(struct MsCompiler* c) {
  if (c == NULL) {
    return;
  }
  while (c->fn != NULL) {
    msCompilerAbortFunc(c);
  }
  c->heap = NULL;
  c->diags = NULL;
  c->sourceFile = NULL;
}

MsResult msCompilerCompile(struct MsCompiler* c, const struct MsAst* root, struct MsProto** out) {
  if (out == NULL) {
    return MS_ERROR_SYNTAX;
  }
  *out = NULL;
  if (c->fn != NULL || c->sticky != MS_OK) {
    return c->sticky != MS_OK ? c->sticky : MS_ERROR_SYNTAX;
  }
  size_t diagStart = msDiagListCount(c->diags);
  if (msCompilerPushFunc(c, NULL) == NULL) {
    return c->sticky;
  }
  if (root == NULL || root->kind != MS_AST_SOURCE_FILE) {
    msCompilerReport(c, root, 307, "expected a source-file AST");
  } else if (root->as.block.count != 0) {
    msCompilerReport(c, root->as.block.items[0], 307, "statement compilation is not implemented");
  }
  if (c->sticky == MS_OK && msDiagListCount(c->diags) == diagStart) {
    return msCompilerPopFunc(c, out);
  }
  MsResult result = c->sticky != MS_OK ? c->sticky : MS_ERROR_SYNTAX;
  msCompilerAbortFunc(c);
  return result;
}
```

在 `CMakeLists.txt` 的 `mslang` 源列表加入 `src/compiler/ms_compiler.c`，在测试注册区加入：

```cmake
  mslang_add_c_test(test_compiler)
```

- [x] **Step 5: 构建并运行空模块测试**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: `1 of 1 tests passed`，CTest 全绿。

- [x] **Step 6: 运行既有全测并提交**

Run: `ctest --test-dir build --output-on-failure`

Expected: 11 个测试目标全部通过。

```bash
git add CMakeLists.txt src/compiler/ms_compiler.h src/compiler/ms_compiler.c tests/c/test_compiler.c
git commit -m "✨ feat(compiler): add module proto skeleton"
```

### Task 5: 字面量、表达式语句与丢弃槽

**Files:**
- Modify: `src/compiler/ms_compiler.c`
- Modify: `tests/c/test_compiler.c`

- [x] **Step 1: 写字面量与丢弃槽失败测试**

增加以下用例；每个用例末尾都调用 `msTestFixtureDestroy(&fix)`：

```c
MS_TEST(compiler, scalarLiteralsAndDedup) {
  MsTestFixture fix;
  msTestCompile(&fix, "1; 1; true; false; nil;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(1, fix.proto->constsLen);
  MS_ASSERT_EQ(1, msIntWordValue(fix.proto->consts[0]));
  MS_ASSERT_EQ(1, fix.proto->localCount);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_CONST, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_CONST, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_TRUE, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_FALSE, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_NIL, MS_OP_STORE_LOCAL,
    MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  for (int i = 1; i < 10; i += 2) {
    MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[i]));
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, numericLiteralBasesAndUnderscores) {
  MsTestFixture fix;
  msTestCompile(&fix, "0xff; 0o17; 0b1010; 1_000; 1_2.5_0e+1;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(5, fix.proto->constsLen);
  MS_ASSERT_EQ(255, msIntWordValue(fix.proto->consts[0]));
  MS_ASSERT_EQ(15, msIntWordValue(fix.proto->consts[1]));
  MS_ASSERT_EQ(10, msIntWordValue(fix.proto->consts[2]));
  MS_ASSERT_EQ(1000, msIntWordValue(fix.proto->consts[3]));
  MS_ASSERT_TRUE(msFloatValue(fix.proto->consts[4]) == 125.0);
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, stringLiteralDecoding) {
  MsTestFixture fix;
  msTestCompile(&fix, "\"a\\n\\u0041\"; `a\\n`;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(2, fix.proto->constsLen);
  const struct MsString* escaped = (const struct MsString*)fix.proto->consts[0];
  const struct MsString* raw = (const struct MsString*)fix.proto->consts[1];
  MS_ASSERT_EQ(3, (long long)msStringLen(escaped));
  MS_ASSERT_TRUE(memcmp(msStringData(escaped), "a\nA", 3) == 0);
  MS_ASSERT_EQ(3, (long long)msStringLen(raw));
  MS_ASSERT_TRUE(memcmp(msStringData(raw), "a\\n", 3) == 0);
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, integerOverflowReportsE308) {
  MsTestFixture fix;
  msTestCompile(&fix, "9223372036854775808;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_TRUE(fix.proto == NULL);
  MS_ASSERT_EQ(1, (long long)msDiagListCount(&fix.diags));
  const struct MsDiag* diag = msDiagListAt(&fix.diags, 0);
  MS_ASSERT_EQ(308, (long long)diag->code);
  MS_ASSERT_EQ(1, (long long)diag->line);
  MS_ASSERT_EQ(1, (long long)diag->column);
  msTestFixtureDestroy(&fix);
}
```

在 `msTestCases` 注册：

```c
  {"Compiler.ScalarLiteralsAndDedup", testcompilerscalarLiteralsAndDedup},
  {"Compiler.NumericLiteralBasesAndUnderscores", testcompilernumericLiteralBasesAndUnderscores},
  {"Compiler.StringLiteralDecoding", testcompilerstringLiteralDecoding},
  {"Compiler.IntegerOverflowReportsE308", testcompilerintegerOverflowReportsE308},
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 新用例失败，当前骨架对第一条语句报 E307；空模块仍通过。

- [x] **Step 3: 实现节点进入、隐藏槽和数字切片转换**

include 区增加 `<errno.h>`、`<stdlib.h>`，并加入：

```c
static bool msCompilerEnterNode(struct MsCompiler* c, const struct MsAst* node) {
  if (c->sticky != MS_OK) {
    return false;
  }
  if (node == NULL) {
    msCompilerReport(c, NULL, 307, "missing AST node");
    return false;
  }
  ++c->nodeDepth;
  if (c->nodeDepth > MS_COMPILER_MAX_NODE_DEPTH) {
    msCompilerFail(c, node, MS_ERROR_SYNTAX, 311, "AST nesting depth exceeded");
    --c->nodeDepth;
    return false;
  }
  c->fn->currentNode = node;
  msProtoSetLine(&c->fn->builder, node->line);
  return true;
}

static int msCompilerAllocHiddenLocal(struct MsCompiler* c) {
  struct MsFuncCtx* fn = c->fn;
  if (fn->localCount >= MS_COMPILER_MAX_LOCALS) {
    msCompilerFail(c, fn->currentNode, MS_ERROR_SYNTAX, 306, "too many local slots");
    return -1;
  }
  int slot = fn->localCount++;
  fn->locals[slot].name = NULL;
  fn->locals[slot].depth = fn->scopeDepth;
  fn->locals[slot].isCaptured = false;
  if (fn->localCount > fn->localMax) {
    fn->localMax = fn->localCount;
  }
  return slot;
}

static int msCompilerDiscardSlot(struct MsCompiler* c) {
  struct MsScope* scope = &c->fn->scopes[c->fn->scopeDepth];
  if (scope->discardSlot < 0) {
    scope->discardSlot = msCompilerAllocHiddenLocal(c);
  }
  return scope->discardSlot;
}

static char* msCompilerCleanNumber(struct MsCompiler* c, const struct MsAst* node) {
  size_t len = node->as.literal.length;
  char* clean = msAlloc(len + 1);
  if (clean == NULL) {
    msCompilerFail(c, node, MS_ERROR_OOM, 0, "out of memory");
    return NULL;
  }
  size_t out = 0;
  for (size_t i = 0; i < len; ++i) {
    if (node->as.literal.start[i] != '_') {
      clean[out++] = node->as.literal.start[i];
    }
  }
  clean[out] = '\0';
  return clean;
}

static bool msCompilerParseInt(struct MsCompiler* c, const struct MsAst* node,
    bool negate, int64_t* out) {
  char* clean = msCompilerCleanNumber(c, node);
  if (clean == NULL) {
    return false;
  }
  int base = 10;
  char* digits = clean;
  if (clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
    base = 16;
    digits += 2;
  } else if (clean[0] == '0' && (clean[1] == 'o' || clean[1] == 'O')) {
    base = 8;
    digits += 2;
  } else if (clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B')) {
    base = 2;
    digits += 2;
  }
  errno = 0;
  char* end = NULL;
  unsigned long long magnitude = strtoull(digits, &end, base);
  unsigned long long limit = negate ? (unsigned long long)INT64_MAX + 1ULL
                                    : (unsigned long long)INT64_MAX;
  bool ok = end != digits && end != NULL && *end == '\0' && errno != ERANGE && magnitude <= limit;
  if (ok) {
    if (negate && magnitude == (unsigned long long)INT64_MAX + 1ULL) {
      *out = INT64_MIN;
    } else if (negate) {
      *out = -(int64_t)magnitude;
    } else {
      *out = (int64_t)magnitude;
    }
  }
  msFree(clean);
  return ok;
}

static bool msCompilerParseFloat(struct MsCompiler* c, const struct MsAst* node, double* out) {
  char* clean = msCompilerCleanNumber(c, node);
  if (clean == NULL) {
    return false;
  }
  errno = 0;
  char* end = NULL;
  double value = strtod(clean, &end);
  bool ok = end != clean && end != NULL && *end == '\0' && errno != ERANGE;
  if (ok) {
    *out = value;
  }
  msFree(clean);
  return ok;
}
```

- [x] **Step 4: 实现常量发射和字面量分派**

加入以下助手；`msCompilerEmitConstObject` 同时服务后续常量折叠：

```c
static void msCompilerEmitConstObject(struct MsCompiler* c, struct MsObject* value) {
  if (c->sticky != MS_OK) {
    return;
  }
  if (value == NULL) {
    msCompilerFail(c, c->fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
    return;
  }
  int index = msProtoAddConst(&c->fn->builder, value);
  if (index < 0) {
    msCompilerSyncBuilder(c);
    return;
  }
  msCompilerEmitABC(c, MS_OP_LOAD_CONST, 0, (uint16_t)index, 1);
}

static void msCompilerEmitIntLiteral(struct MsCompiler* c, const struct MsAst* node) {
  int64_t value;
  if (!msCompilerParseInt(c, node, false, &value)) {
    if (c->sticky == MS_OK) {
      msCompilerReport(c, node, 308, "integer literal exceeds int64");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    }
    return;
  }
  msCompilerEmitConstObject(c, (struct MsObject*)msIntNew(c->heap, value));
}

static void msCompilerEmitFloatLiteral(struct MsCompiler* c, const struct MsAst* node) {
  double value;
  if (!msCompilerParseFloat(c, node, &value)) {
    if (c->sticky == MS_OK) {
      msCompilerReport(c, node, 308, "floating literal is out of range");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    }
    return;
  }
  msCompilerEmitConstObject(c, (struct MsObject*)msFloatNew(c->heap, value));
}

static void msCompilerEmitStringLiteral(struct MsCompiler* c, const struct MsAst* node) {
  const char* body = node->as.literal.start + 1;
  size_t bodyLen = node->as.literal.length - 2;
  if (node->kind == MS_AST_RAW_STRING) {
    msCompilerEmitConstObject(c,
        (struct MsObject*)msStringNew(c->heap, body, bodyLen));
    return;
  }
  char* decoded = NULL;
  size_t decodedLen = 0;
  MsResult result = msLexerUnescape(body, bodyLen, &decoded, &decodedLen);
  if (result != MS_OK) {
    msCompilerFail(c, node, result, result == MS_ERROR_OOM ? 0 : 307,
        result == MS_ERROR_OOM ? "out of memory" : "string decoding failed");
    return;
  }
  struct MsString* value = msStringNew(c->heap, decoded, decodedLen);
  msFree(decoded);
  msCompilerEmitConstObject(c, (struct MsObject*)value);
}

static void msCompilerCompileStmt(struct MsCompiler* c, const struct MsAst* node);
static void msCompilerCompileExpr(struct MsCompiler* c, const struct MsAst* node);

static void msCompilerCompileExpr(struct MsCompiler* c, const struct MsAst* node) {
  if (!msCompilerEnterNode(c, node)) {
    return;
  }
  switch (node->kind) {
    case MS_AST_INT:
      msCompilerEmitIntLiteral(c, node);
      break;
    case MS_AST_FLOAT:
      msCompilerEmitFloatLiteral(c, node);
      break;
    case MS_AST_STRING:
    case MS_AST_RAW_STRING:
      msCompilerEmitStringLiteral(c, node);
      break;
    case MS_AST_BOOL:
      msCompilerEmitABC(c, node->as.boolLit.value ? MS_OP_LOAD_TRUE : MS_OP_LOAD_FALSE, 0, 0, 1);
      break;
    case MS_AST_NIL:
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      break;
    default:
      msCompilerReport(c, node, 307, "expression construct is not implemented in task 07");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      break;
  }
  --c->nodeDepth;
}
```

include 区增加 `"lexer/ms_lexer.h"`。

- [x] **Step 5: 实现语句分派并接入根节点**

增加：

```c
static void msCompilerCompileStmt(struct MsCompiler* c, const struct MsAst* node) {
  if (!msCompilerEnterNode(c, node)) {
    return;
  }
  switch (node->kind) {
    case MS_AST_SOURCE_FILE:
      for (size_t i = 0; i < node->as.block.count && c->sticky == MS_OK; ++i) {
        msCompilerCompileStmt(c, node->as.block.items[i]);
      }
      break;
    case MS_AST_EXPR_STMT:
      msCompilerCompileExpr(c, node->as.exprStmt.expr);
      if (c->sticky == MS_OK) {
        int slot = msCompilerDiscardSlot(c);
        if (slot >= 0) {
          msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
        }
      }
      break;
    case MS_AST_PASS:
      break;
    default:
      msCompilerReport(c, node, 307, "statement construct is not implemented in task 07");
      break;
  }
  --c->nodeDepth;
}
```

把 `msCompilerCompile` 中检查 root 后的分支替换为：

```c
  if (root == NULL || root->kind != MS_AST_SOURCE_FILE) {
    msCompilerReport(c, root, 307, "expected a source-file AST");
  } else {
    msCompilerCompileStmt(c, root);
  }
```

- [x] **Step 6: 运行编译器测试并提交**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 5 个编译器用例全部通过，整数溢出返回语法错误且无泄漏。

```bash
git add src/compiler/ms_compiler.c tests/c/test_compiler.c
git commit -m "✨ feat(compiler): emit literal bytecode"
```

### Task 6: 块级作用域、声明赋值与长名字解析

**Files:**
- Modify: `src/compiler/ms_compiler.c`
- Modify: `tests/c/test_compiler.c`

- [x] **Step 1: 写作用域失败测试**

```c
MS_TEST(compiler, moduleDeclarationUsesGlobal) {
  MsTestFixture fix;
  msTestCompile(&fix, "x := 1; x;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_CONST, MS_OP_STORE_GLOBAL, MS_OP_LOAD_GLOBAL,
    MS_OP_STORE_LOCAL, MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(1, msOpDecodeBx(fix.proto->code[1]));
  MS_ASSERT_EQ(1, msOpDecodeBx(fix.proto->code[2]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[3]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, siblingBlocksReuseSlots) {
  MsTestFixture fix;
  msTestCompile(&fix, "x := 0; { x := 1; x; } { x := 2; x; } x;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(fix.proto->code[3]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[3]));
  MS_ASSERT_EQ(1, msOpDecodeA(fix.proto->code[5]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[7]));
  MS_ASSERT_EQ(1, msOpDecodeA(fix.proto->code[9]));
  MS_ASSERT_EQ(MS_OP_LOAD_GLOBAL, msOpDecodeOp(fix.proto->code[10]));
  MS_ASSERT_EQ(2, fix.proto->localCount);
  msTestFixtureDestroy(&fix);

  msTestCompile(&fix, "{ 1; } { x := 2; x; }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_LOAD_LOCAL, msOpDecodeOp(fix.proto->code[4]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[4]));
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(fix.proto->code[5]));
  MS_ASSERT_EQ(1, msOpDecodeA(fix.proto->code[5]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, longIdentifierResolvesByContent) {
  MsTestFixture fix;
  msTestCompile(&fix,
      "{ abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN := 1; abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN; }",
      MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(fix.proto->code[1]));
  MS_ASSERT_EQ(MS_OP_LOAD_LOCAL, msOpDecodeOp(fix.proto->code[2]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[1]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[2]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, declarationRhsReadsOuterSlot) {
  MsTestFixture fix;
  msTestCompile(&fix, "{ x := 1; { x := x; } }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_CONST, MS_OP_STORE_LOCAL, MS_OP_LOAD_LOCAL,
    MS_OP_STORE_LOCAL, MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[1]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[2]));
  MS_ASSERT_EQ(1, msOpDecodeA(fix.proto->code[3]));
  msTestFixtureDestroy(&fix);
}
```

注册：

```c
  {"Compiler.ModuleDeclarationUsesGlobal", testcompilermoduleDeclarationUsesGlobal},
  {"Compiler.SiblingBlocksReuseSlots", testcompilersiblingBlocksReuseSlots},
  {"Compiler.LongIdentifierResolvesByContent", testcompilerlongIdentifierResolvesByContent},
  {"Compiler.DeclarationRhsReadsOuterSlot", testcompilerdeclarationRhsReadsOuterSlot},
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 新用例因声明、标识符和块仍报 E307 而失败。

- [x] **Step 3: 实现作用域栈和内容名解析**

加入：

```c
typedef enum {
  MS_RESOLVE_LOCAL,
  MS_RESOLVE_UPVALUE,
  MS_RESOLVE_GLOBAL
} MsResolveKind;

static struct MsString* msCompilerNameFromSlice(struct MsCompiler* c, const char* start, size_t len) {
  struct MsString* name = msStringNew(c->heap, start, len);
  if (name == NULL) {
    msCompilerFail(c, c->fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
  }
  return name;
}

static int msCompilerResolveLocalInFunc(const struct MsFuncCtx* fn, const struct MsString* name) {
  for (int i = fn->localCount - 1; i >= 0; --i) {
    if (fn->locals[i].name != NULL && msStringEquals(fn->locals[i].name, name)) {
      return i;
    }
  }
  return -1;
}

static MsResolveKind msCompilerResolveName(struct MsCompiler* c, const struct MsString* name, int* slot) {
  *slot = msCompilerResolveLocalInFunc(c->fn, name);
  if (*slot >= 0) {
    return MS_RESOLVE_LOCAL;
  }
  for (struct MsFuncCtx* parent = c->fn->parent; parent != NULL; parent = parent->parent) {
    if (msCompilerResolveLocalInFunc(parent, name) >= 0) {
      return MS_RESOLVE_UPVALUE;
    }
  }
  return MS_RESOLVE_GLOBAL;
}

static int msCompilerDeclareLocal(struct MsCompiler* c, struct MsString* name) {
  struct MsFuncCtx* fn = c->fn;
  if (fn->localCount >= MS_COMPILER_MAX_LOCALS) {
    msCompilerFail(c, fn->currentNode, MS_ERROR_SYNTAX, 306, "too many local slots");
    return -1;
  }
  int slot = fn->localCount++;
  fn->locals[slot].name = name;
  fn->locals[slot].depth = fn->scopeDepth;
  fn->locals[slot].isCaptured = false;
  if (fn->localCount > fn->localMax) {
    fn->localMax = fn->localCount;
  }
  return slot;
}

static void msCompilerPushScope(struct MsCompiler* c) {
  struct MsFuncCtx* fn = c->fn;
  if (fn->scopeDepth + 1 >= MS_COMPILER_MAX_SCOPE_DEPTH) {
    msCompilerFail(c, fn->currentNode, MS_ERROR_SYNTAX, 306, "block nesting depth exceeded");
    return;
  }
  ++fn->scopeDepth;
  fn->scopes[fn->scopeDepth].localBase = fn->localCount;
  fn->scopes[fn->scopeDepth].discardSlot = -1;
}

static void msCompilerPopScope(struct MsCompiler* c) {
  struct MsFuncCtx* fn = c->fn;
  MS_ASSERT(fn->scopeDepth > 0);
  fn->localCount = fn->scopes[fn->scopeDepth].localBase;
  fn->scopes[fn->scopeDepth].discardSlot = -1;
  --fn->scopeDepth;
}
```

- [x] **Step 4: 实现名字载入/存储和声明赋值**

加入：

```c
static void msCompilerEmitLoadName(struct MsCompiler* c, struct MsString* name) {
  int slot = -1;
  MsResolveKind kind = msCompilerResolveName(c, name, &slot);
  if (kind == MS_RESOLVE_LOCAL) {
    msCompilerEmitABC(c, MS_OP_LOAD_LOCAL, (uint8_t)slot, 0, 1);
    return;
  }
  if (kind == MS_RESOLVE_UPVALUE) {
    msCompilerReport(c, c->fn->currentNode, 310, "upvalue capture belongs to task 14");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  int index = msProtoAddConst(&c->fn->builder, (struct MsObject*)name);
  if (index < 0) {
    msCompilerSyncBuilder(c);
    return;
  }
  msCompilerEmitABC(c, MS_OP_LOAD_GLOBAL, 0, (uint16_t)index, 1);
}

static void msCompilerEmitStoreName(struct MsCompiler* c, struct MsString* name) {
  int slot = -1;
  MsResolveKind kind = msCompilerResolveName(c, name, &slot);
  if (kind == MS_RESOLVE_LOCAL) {
    msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
    return;
  }
  if (kind == MS_RESOLVE_UPVALUE) {
    msCompilerReport(c, c->fn->currentNode, 310, "upvalue capture belongs to task 14");
    int saved = c->fn->localCount;
    int discard = msCompilerAllocHiddenLocal(c);
    if (discard >= 0) {
      msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)discard, 0, -1);
    }
    c->fn->localCount = saved;
    return;
  }
  int index = msProtoAddConst(&c->fn->builder, (struct MsObject*)name);
  if (index < 0) {
    msCompilerSyncBuilder(c);
    return;
  }
  msCompilerEmitABC(c, MS_OP_STORE_GLOBAL, 0, (uint16_t)index, -1);
}

static void msCompilerCompileDeclOrAssign(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.decl.targets.count != 1 || node->as.decl.values.count != 1
      || node->as.decl.targets.items[0]->kind != MS_AST_IDENT) {
    msCompilerReport(c, node, 307, "multiple or non-identifier assignment belongs to task 11");
    return;
  }
  msCompilerCompileExpr(c, node->as.decl.values.items[0]);
  if (c->sticky != MS_OK) {
    return;
  }
  const struct MsAst* target = node->as.decl.targets.items[0];
  struct MsString* name = msCompilerNameFromSlice(c, target->as.literal.start, target->as.literal.length);
  if (name == NULL) {
    return;
  }
  if (node->kind == MS_AST_ASSIGN) {
    msCompilerEmitStoreName(c, name);
    return;
  }
  if (c->fn->isModule && c->fn->scopeDepth == 0) {
    int index = msProtoAddConst(&c->fn->builder, (struct MsObject*)name);
    if (index >= 0) {
      msCompilerEmitABC(c, MS_OP_STORE_GLOBAL, 0, (uint16_t)index, -1);
    } else {
      msCompilerSyncBuilder(c);
    }
    return;
  }
  int slot = msCompilerDeclareLocal(c, name);
  if (slot >= 0) {
    msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
  }
}
```

在表达式分派增加：

```c
    case MS_AST_IDENT: {
      struct MsString* name = msCompilerNameFromSlice(c, node->as.literal.start, node->as.literal.length);
      if (name != NULL) {
        msCompilerEmitLoadName(c, name);
      }
      break;
    }
```

在语句分派增加：

```c
    case MS_AST_DECL:
    case MS_AST_ASSIGN:
      msCompilerCompileDeclOrAssign(c, node);
      break;
    case MS_AST_BLOCK: {
      int oldDepth = c->fn->scopeDepth;
      msCompilerPushScope(c);
      if (c->fn->scopeDepth != oldDepth) {
        for (size_t i = 0; i < node->as.block.count && c->sticky == MS_OK; ++i) {
          msCompilerCompileStmt(c, node->as.block.items[i]);
        }
        msCompilerPopScope(c);
      }
      break;
    }
```

- [x] **Step 5: 运行测试并提交**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 9 个编译器用例全部通过；长标识符解析为局部，兄弟块复用槽 0/1。

```bash
git add src/compiler/ms_compiler.c tests/c/test_compiler.c
git commit -m "✨ feat(compiler): resolve block scoped names"
```

### Task 7: 算术/一元表达式与白名单常量折叠

**Files:**
- Modify: `src/compiler/ms_compiler.c`
- Modify: `tests/c/test_compiler.c`

- [x] **Step 1: 写算术和折叠失败测试**

```c
MS_TEST(compiler, binaryOpcodeMapping) {
  static const struct {
    const char* source;
    MsOpCode op;
  } cases[] = {
    {"a + b;", MS_OP_ADD}, {"a - b;", MS_OP_SUB}, {"a * b;", MS_OP_MUL},
    {"a / b;", MS_OP_DIV}, {"a div b;", MS_OP_FLOORDIV}, {"a % b;", MS_OP_MOD},
    {"a ** b;", MS_OP_POW}, {"a << b;", MS_OP_SHL}, {"a >> b;", MS_OP_SHR},
    {"a & b;", MS_OP_BITAND}, {"a | b;", MS_OP_BITOR}, {"a ^ b;", MS_OP_BITXOR}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    MsTestFixture fix;
    msTestCompile(&fix, cases[i].source, MS_COMPILE_DEFAULT);
    MS_ASSERT_EQ(MS_OK, fix.result);
    MS_ASSERT_EQ(cases[i].op, msOpDecodeOp(fix.proto->code[2]));
    MS_ASSERT_EQ(2, fix.proto->stackSize);
    msTestFixtureDestroy(&fix);
  }
  MsTestFixture fix;
  msTestCompile(&fix, "-a; ~a; not a; +a;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_NEG, msOpDecodeOp(fix.proto->code[1]));
  MS_ASSERT_EQ(MS_OP_INVERT, msOpDecodeOp(fix.proto->code[4]));
  MS_ASSERT_EQ(MS_OP_NOT, msOpDecodeOp(fix.proto->code[7]));
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(fix.proto->code[9]));
  MS_ASSERT_EQ(MS_OP_ADD, msOpDecodeOp(fix.proto->code[11]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, constantFoldWhitelist) {
  MsTestFixture fix;
  msTestCompile(&fix, "1 + 2 * 3; -5; ~0; not nil; 1 + 2.0;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(4, fix.proto->constsLen);
  MS_ASSERT_EQ(7, msIntWordValue(fix.proto->consts[0]));
  MS_ASSERT_EQ(-5, msIntWordValue(fix.proto->consts[1]));
  MS_ASSERT_EQ(-1, msIntWordValue(fix.proto->consts[2]));
  MS_ASSERT_TRUE(msFloatValue(fix.proto->consts[3]) == 3.0);
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(fix.proto->code[0]));
  MS_ASSERT_EQ(MS_OP_LOAD_TRUE, msOpDecodeOp(fix.proto->code[6]));
  msTestFixtureDestroy(&fix);

  msTestCompile(&fix, "+5; 5 - 8;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(5, msIntWordValue(fix.proto->consts[0]));
  MS_ASSERT_EQ(-3, msIntWordValue(fix.proto->consts[1]));
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(fix.proto->code[0]));
  MS_ASSERT_EQ(MS_OP_LOAD_CONST, msOpDecodeOp(fix.proto->code[2]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, int64MinAndOverflowBehavior) {
  MsTestFixture fix;
  msTestCompile(&fix,
      "-9223372036854775808; 9223372036854775807 + 1; -9223372036854775808 * -1;",
      MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(INT64_MIN, msIntWordValue(fix.proto->consts[0]));
  MS_ASSERT_EQ(MS_OP_ADD, msOpDecodeOp(fix.proto->code[4]));
  MS_ASSERT_EQ(MS_OP_MUL, msOpDecodeOp(fix.proto->code[8]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, nonWhitelistOperationsRemain) {
  MsTestFixture fix;
  msTestCompile(&fix, "\"a\" + \"b\"; 1 / 0;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_ADD, msOpDecodeOp(fix.proto->code[2]));
  MS_ASSERT_EQ(MS_OP_DIV, msOpDecodeOp(fix.proto->code[6]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, foldedSignedZeroRemainDistinct) {
  MsTestFixture fix;
  msTestCompile(&fix, "-0.0; 0.0;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(2, fix.proto->constsLen);
  MS_ASSERT_TRUE(signbit(msFloatValue(fix.proto->consts[0])));
  MS_ASSERT_TRUE(!signbit(msFloatValue(fix.proto->consts[1])));
  msTestFixtureDestroy(&fix);
}
```

在测试文件 include 区加入 `<math.h>`，并注册：

```c
  {"Compiler.BinaryOpcodeMapping", testcompilerbinaryOpcodeMapping},
  {"Compiler.ConstantFoldWhitelist", testcompilerconstantFoldWhitelist},
  {"Compiler.Int64MinAndOverflowBehavior", testcompilerint64MinAndOverflowBehavior},
  {"Compiler.NonWhitelistOperationsRemain", testcompilernonWhitelistOperationsRemain},
  {"Compiler.FoldedSignedZeroRemainDistinct", testcompilerfoldedSignedZeroRemainDistinct},
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 算术/一元节点仍走 E307，新用例失败。

- [x] **Step 3: 实现安全整数折叠助手**

加入：

```c
static bool msCompilerIntAddOverflows(int64_t a, int64_t b) {
  return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
}

static bool msCompilerIntSubOverflows(int64_t a, int64_t b) {
  return (b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b);
}

static bool msCompilerIntMulOverflows(int64_t a, int64_t b) {
  if (a > 0) {
    if (b > 0) {
      return a > INT64_MAX / b;
    }
    return b < INT64_MIN / a;
  }
  if (b > 0) {
    return a < INT64_MIN / b;
  }
  return a != 0 && b < INT64_MAX / a;
}
```

- [x] **Step 4: 实现常量值、共享深度预算的求值器和发射器**

加入：

```c
typedef enum {
  MS_CONST_INT,
  MS_CONST_FLOAT,
  MS_CONST_BOOL,
  MS_CONST_NIL
} MsConstKind;

struct MsConstValue {
  MsConstKind kind;
  int64_t intValue;
  double floatValue;
  bool boolValue;
};

static bool msCompilerConstTruthy(const struct MsConstValue* value) {
  switch (value->kind) {
    case MS_CONST_INT:
      return value->intValue != 0;
    case MS_CONST_FLOAT:
      return value->floatValue != 0.0;
    case MS_CONST_BOOL:
      return value->boolValue;
    case MS_CONST_NIL:
      return false;
    default:
      MS_UNREACHABLE();
  }
  return false;
}

static bool msCompilerEvalConst(struct MsCompiler* c, const struct MsAst* node,
    int depth, struct MsConstValue* out) {
  if (node == NULL || c->sticky != MS_OK) {
    return false;
  }
  if (depth > 0 && c->nodeDepth + depth > MS_COMPILER_MAX_NODE_DEPTH) {
    msCompilerFail(c, node, MS_ERROR_SYNTAX, 311, "AST nesting depth exceeded");
    return false;
  }
  if (node->kind == MS_AST_INT) {
    out->kind = MS_CONST_INT;
    return msCompilerParseInt(c, node, false, &out->intValue);
  }
  if (node->kind == MS_AST_FLOAT) {
    out->kind = MS_CONST_FLOAT;
    return msCompilerParseFloat(c, node, &out->floatValue);
  }
  if (node->kind == MS_AST_BOOL) {
    out->kind = MS_CONST_BOOL;
    out->boolValue = node->as.boolLit.value;
    return true;
  }
  if (node->kind == MS_AST_NIL) {
    out->kind = MS_CONST_NIL;
    return true;
  }
  if (node->kind == MS_AST_UNARY) {
    if (node->as.unary.op == MS_TOKEN_MINUS && node->as.unary.operand->kind == MS_AST_INT) {
      if (c->nodeDepth + depth + 1 > MS_COMPILER_MAX_NODE_DEPTH) {
        msCompilerFail(c, node->as.unary.operand, MS_ERROR_SYNTAX, 311, "AST nesting depth exceeded");
        return false;
      }
      out->kind = MS_CONST_INT;
      return msCompilerParseInt(c, node->as.unary.operand, true, &out->intValue);
    }
    struct MsConstValue operand;
    if (!msCompilerEvalConst(c, node->as.unary.operand, depth + 1, &operand)) {
      return false;
    }
    if (node->as.unary.op == MS_TOKEN_KW_NOT) {
      out->kind = MS_CONST_BOOL;
      out->boolValue = !msCompilerConstTruthy(&operand);
      return true;
    }
    if (operand.kind != MS_CONST_INT && operand.kind != MS_CONST_FLOAT) {
      return false;
    }
    if (node->as.unary.op == MS_TOKEN_PLUS) {
      *out = operand;
      return true;
    }
    if (node->as.unary.op == MS_TOKEN_TILDE) {
      if (operand.kind != MS_CONST_INT) {
        return false;
      }
      out->kind = MS_CONST_INT;
      out->intValue = ~operand.intValue;
      return true;
    }
    if (node->as.unary.op != MS_TOKEN_MINUS) {
      return false;
    }
    if (operand.kind == MS_CONST_FLOAT) {
      out->kind = MS_CONST_FLOAT;
      out->floatValue = -operand.floatValue;
      return true;
    }
    if (operand.intValue == INT64_MIN) {
      return false;
    }
    out->kind = MS_CONST_INT;
    out->intValue = -operand.intValue;
    return true;
  }
  if (node->kind != MS_AST_BINARY
      || (node->as.binary.op != MS_TOKEN_PLUS && node->as.binary.op != MS_TOKEN_MINUS
          && node->as.binary.op != MS_TOKEN_STAR)) {
    return false;
  }
  struct MsConstValue left;
  struct MsConstValue right;
  if (!msCompilerEvalConst(c, node->as.binary.left, depth + 1, &left)
      || !msCompilerEvalConst(c, node->as.binary.right, depth + 1, &right)) {
    return false;
  }
  if ((left.kind != MS_CONST_INT && left.kind != MS_CONST_FLOAT)
      || (right.kind != MS_CONST_INT && right.kind != MS_CONST_FLOAT)) {
    return false;
  }
  if (left.kind == MS_CONST_INT && right.kind == MS_CONST_INT) {
    bool overflow = node->as.binary.op == MS_TOKEN_PLUS
        ? msCompilerIntAddOverflows(left.intValue, right.intValue)
        : node->as.binary.op == MS_TOKEN_MINUS
            ? msCompilerIntSubOverflows(left.intValue, right.intValue)
            : msCompilerIntMulOverflows(left.intValue, right.intValue);
    if (overflow) {
      return false;
    }
    out->kind = MS_CONST_INT;
    out->intValue = node->as.binary.op == MS_TOKEN_PLUS ? left.intValue + right.intValue
        : node->as.binary.op == MS_TOKEN_MINUS ? left.intValue - right.intValue
                                               : left.intValue * right.intValue;
    return true;
  }
  double a = left.kind == MS_CONST_INT ? (double)left.intValue : left.floatValue;
  double b = right.kind == MS_CONST_INT ? (double)right.intValue : right.floatValue;
  out->kind = MS_CONST_FLOAT;
  out->floatValue = node->as.binary.op == MS_TOKEN_PLUS ? a + b
      : node->as.binary.op == MS_TOKEN_MINUS ? a - b : a * b;
  return true;
}

static void msCompilerEmitConst(struct MsCompiler* c, const struct MsConstValue* value) {
  switch (value->kind) {
    case MS_CONST_INT:
      msCompilerEmitConstObject(c, (struct MsObject*)msIntNew(c->heap, value->intValue));
      break;
    case MS_CONST_FLOAT:
      msCompilerEmitConstObject(c, (struct MsObject*)msFloatNew(c->heap, value->floatValue));
      break;
    case MS_CONST_BOOL:
      msCompilerEmitABC(c, value->boolValue ? MS_OP_LOAD_TRUE : MS_OP_LOAD_FALSE, 0, 0, 1);
      break;
    case MS_CONST_NIL:
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      break;
    default:
      MS_UNREACHABLE();
  }
}
```

- [x] **Step 5: 实现普通算术和一元生成**

加入映射和生成函数：

```c
static MsOpCode msCompilerBinaryOp(MsTokenType token) {
  switch (token) {
    case MS_TOKEN_PLUS: return MS_OP_ADD;
    case MS_TOKEN_MINUS: return MS_OP_SUB;
    case MS_TOKEN_STAR: return MS_OP_MUL;
    case MS_TOKEN_SLASH: return MS_OP_DIV;
    case MS_TOKEN_KW_DIV: return MS_OP_FLOORDIV;
    case MS_TOKEN_PERCENT: return MS_OP_MOD;
    case MS_TOKEN_DOUBLE_STAR: return MS_OP_POW;
    case MS_TOKEN_SHIFT_LEFT: return MS_OP_SHL;
    case MS_TOKEN_SHIFT_RIGHT: return MS_OP_SHR;
    case MS_TOKEN_AMP: return MS_OP_BITAND;
    case MS_TOKEN_PIPE: return MS_OP_BITOR;
    case MS_TOKEN_CARET: return MS_OP_BITXOR;
    default: return MS_OP_COUNT;
  }
}

static void msCompilerCompileBinary(struct MsCompiler* c, const struct MsAst* node) {
  MsOpCode op = msCompilerBinaryOp(node->as.binary.op);
  if (op == MS_OP_COUNT) {
    msCompilerReport(c, node, 307, "binary operator is not implemented in task 07");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  msCompilerCompileExpr(c, node->as.binary.left);
  msCompilerCompileExpr(c, node->as.binary.right);
  msCompilerEmitABC(c, op, 0, 0, -1);
}

static void msCompilerCompileUnary(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.unary.op == MS_TOKEN_PLUS) {
    struct MsConstValue zero = {.kind = MS_CONST_INT, .intValue = 0};
    msCompilerEmitConst(c, &zero);
    msCompilerCompileExpr(c, node->as.unary.operand);
    msCompilerEmitABC(c, MS_OP_ADD, 0, 0, -1);
    return;
  }
  msCompilerCompileExpr(c, node->as.unary.operand);
  switch (node->as.unary.op) {
    case MS_TOKEN_MINUS:
      msCompilerEmitABC(c, MS_OP_NEG, 0, 0, 0);
      break;
    case MS_TOKEN_TILDE:
      msCompilerEmitABC(c, MS_OP_INVERT, 0, 0, 0);
      break;
    case MS_TOKEN_KW_NOT:
      msCompilerEmitABC(c, MS_OP_NOT, 0, 0, 0);
      break;
    default:
      msCompilerReport(c, node, 307, "unary operator is not implemented in task 07");
      break;
  }
}
```

在表达式分派的 `default` 前加入：

```c
    case MS_AST_UNARY: {
      struct MsConstValue folded;
      if (msCompilerEvalConst(c, node, 0, &folded)) {
        msCompilerEmitConst(c, &folded);
      } else if (c->sticky == MS_OK) {
        msCompilerCompileUnary(c, node);
      }
      break;
    }
    case MS_AST_BINARY: {
      struct MsConstValue folded;
      if (msCompilerEvalConst(c, node, 0, &folded)) {
        msCompilerEmitConst(c, &folded);
      } else if (c->sticky == MS_OK) {
        msCompilerCompileBinary(c, node);
      }
      break;
    }
```

- [x] **Step 6: 运行测试并提交**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 14 个编译器用例全部通过；折叠无符号溢出 UB，`INT64_MIN` 可用，溢出的二元运算保留原操作码。

```bash
git add src/compiler/ms_compiler.c tests/c/test_compiler.c
git commit -m "✨ feat(compiler): fold arithmetic constants"
```

### Task 8: 逻辑、条件、比较与位置调用

**Files:**
- Modify: `src/compiler/ms_compiler.c`
- Modify: `tests/c/test_compiler.c`

- [x] **Step 1: 写跳转和调用失败测试**

```c
MS_TEST(compiler, logicalShortCircuitShape) {
  MsTestFixture fix;
  msTestCompile(&fix, "a and b; a or b;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_JUMP_IF_FALSE, msOpDecodeOp(fix.proto->code[3]));
  MS_ASSERT_EQ(2, msOpDecodeSAx(fix.proto->code[3]));
  MS_ASSERT_EQ(MS_OP_JUMP_IF_TRUE, msOpDecodeOp(fix.proto->code[11]));
  MS_ASSERT_EQ(2, msOpDecodeSAx(fix.proto->code[11]));
  MS_ASSERT_EQ(2, fix.proto->localCount);
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, constantConditionsSelectOneBranch) {
  MsTestFixture fix;
  msTestCompile(&fix,
      "false and x; true or x; true and x; a if true else b; a if false else b;",
      MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  for (int i = 0; i < fix.proto->codeLen; ++i) {
    MsOpCode op = msOpDecodeOp(fix.proto->code[i]);
    MS_ASSERT_TRUE(op != MS_OP_JUMP && op != MS_OP_JUMP_IF_FALSE && op != MS_OP_JUMP_IF_TRUE);
  }
  MS_ASSERT_EQ(MS_OP_LOAD_FALSE, msOpDecodeOp(fix.proto->code[0]));
  MS_ASSERT_EQ(MS_OP_LOAD_TRUE, msOpDecodeOp(fix.proto->code[2]));
  MS_ASSERT_EQ(MS_OP_LOAD_GLOBAL, msOpDecodeOp(fix.proto->code[4]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, conditionalExpressionJumps) {
  MsTestFixture fix;
  msTestCompile(&fix, "a if c else b;", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_GLOBAL, MS_OP_JUMP_IF_FALSE, MS_OP_LOAD_GLOBAL, MS_OP_JUMP,
    MS_OP_LOAD_GLOBAL, MS_OP_STORE_LOCAL, MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(2, msOpDecodeSAx(fix.proto->code[1]));
  MS_ASSERT_EQ(1, msOpDecodeSAx(fix.proto->code[3]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, comparisonExpansion) {
  static const struct {
    const char* source;
    MsOpCode op;
    bool negate;
  } cases[] = {
    {"a < b;", MS_OP_LT, false}, {"a <= b;", MS_OP_LE, false},
    {"a > b;", MS_OP_GT, false}, {"a >= b;", MS_OP_GE, false},
    {"a == b;", MS_OP_EQ, false}, {"a != b;", MS_OP_NE, false},
    {"a in b;", MS_OP_IN, false}, {"a not in b;", MS_OP_IN, true},
    {"a is b;", MS_OP_IS, false}, {"a is not b;", MS_OP_IS, true}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    MsTestFixture fix;
    msTestCompile(&fix, cases[i].source, MS_COMPILE_DEFAULT);
    MS_ASSERT_EQ(MS_OK, fix.result);
    MS_ASSERT_EQ(cases[i].op, msOpDecodeOp(fix.proto->code[2]));
    MS_ASSERT_EQ(cases[i].negate ? MS_OP_NOT : MS_OP_STORE_LOCAL, msOpDecodeOp(fix.proto->code[3]));
    if (cases[i].negate) {
      MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(fix.proto->code[4]));
    }
    msTestFixtureDestroy(&fix);
  }
}

MS_TEST(compiler, positionalCallShape) {
  MsTestFixture fix;
  msTestCompile(&fix, "f(a, 1);", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_CALL, msOpDecodeOp(fix.proto->code[3]));
  MS_ASSERT_EQ(2, msOpDecodeA(fix.proto->code[3]));
  MS_ASSERT_EQ(3, fix.proto->stackSize);
  msTestFixtureDestroy(&fix);
}
```

注册：

```c
  {"Compiler.LogicalShortCircuitShape", testcompilerlogicalShortCircuitShape},
  {"Compiler.ConstantConditionsSelectOneBranch", testcompilerconstantConditionsSelectOneBranch},
  {"Compiler.ConditionalExpressionJumps", testcompilerconditionalExpressionJumps},
  {"Compiler.ComparisonExpansion", testcompilercomparisonExpansion},
  {"Compiler.PositionalCallShape", testcompilerpositionalCallShape},
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 五组节点仍走 E307，测试失败。

- [x] **Step 3: 实现跳转发射、栈深校验和回填**

加入：

```c
static int msCompilerCurrentPc(const struct MsCompiler* c) {
  return c->fn->builder.proto.codeLen;
}

static int msCompilerEmitJumpPlaceholder(struct MsCompiler* c, MsOpCode op) {
  if (c->sticky != MS_OK) {
    return -1;
  }
  int pc = msProtoEmitSAx(&c->fn->builder, op, 0);
  if (pc < 0) {
    msCompilerSyncBuilder(c);
    return -1;
  }
  c->fn->stackDepth += op == MS_OP_JUMP ? 0 : -1;
  MS_ASSERT(c->fn->stackDepth >= 0);
  return pc;
}

static void msCompilerPatchToCurrent(struct MsCompiler* c, int jumpPc, int expectedStackDepth) {
  struct MsFuncCtx* fn = c->fn;
  MS_ASSERT(fn->stackDepth == expectedStackDepth);
  MS_UNUSED(expectedStackDepth);
  if (jumpPc < 0 || c->sticky != MS_OK) {
    return;
  }
  int target = msCompilerCurrentPc(c);
  uint32_t* instruction = &fn->builder.proto.code[jumpPc];
  if (msOpDecodeOp(*instruction) == MS_OP_JUMP && target == jumpPc + 1) {
    *instruction = msOpEncodeABC(MS_OP_NOP, 0, 0);
    return;
  }
  MsResult result = msProtoPatchSAx(&fn->builder, jumpPc, target - (jumpPc + 1));
  if (result != MS_OK) {
    msCompilerSyncBuilder(c);
  }
}
```

- [x] **Step 4: 实现逻辑和条件表达式**

加入：

```c
static void msCompilerCompileLogical(struct MsCompiler* c, const struct MsAst* node) {
  struct MsConstValue constant;
  if (msCompilerEvalConst(c, node->as.binary.left, 0, &constant)) {
    bool isAnd = node->as.binary.op == MS_TOKEN_KW_AND;
    bool truthy = msCompilerConstTruthy(&constant);
    if ((isAnd && !truthy) || (!isAnd && truthy)) {
      msCompilerEmitConst(c, &constant);
    } else {
      msCompilerCompileExpr(c, node->as.binary.right);
    }
    return;
  }
  if (c->sticky != MS_OK) {
    return;
  }
  int savedLocals = c->fn->localCount;
  int slot = msCompilerAllocHiddenLocal(c);
  if (slot < 0) {
    return;
  }
  msCompilerCompileExpr(c, node->as.binary.left);
  msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
  msCompilerEmitABC(c, MS_OP_LOAD_LOCAL, (uint8_t)slot, 0, 1);
  MsOpCode jumpOp = node->as.binary.op == MS_TOKEN_KW_AND ? MS_OP_JUMP_IF_FALSE : MS_OP_JUMP_IF_TRUE;
  int jump = msCompilerEmitJumpPlaceholder(c, jumpOp);
  int branchDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.binary.right);
  msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
  msCompilerPatchToCurrent(c, jump, branchDepth);
  msCompilerEmitABC(c, MS_OP_LOAD_LOCAL, (uint8_t)slot, 0, 1);
  c->fn->localCount = savedLocals;
}

static void msCompilerCompileConditional(struct MsCompiler* c, const struct MsAst* node) {
  struct MsConstValue condition;
  if (msCompilerEvalConst(c, node->as.conditional.cond, 0, &condition)) {
    if (msCompilerConstTruthy(&condition)) {
      msCompilerCompileExpr(c, node->as.conditional.value);
    } else {
      msCompilerCompileExpr(c, node->as.conditional.elseValue);
    }
    return;
  }
  if (c->sticky != MS_OK) {
    return;
  }
  msCompilerCompileExpr(c, node->as.conditional.cond);
  int elseJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
  int elseDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.conditional.value);
  int endJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
  int endDepth = c->fn->stackDepth;
  c->fn->stackDepth = elseDepth;
  msCompilerPatchToCurrent(c, elseJump, elseDepth);
  msCompilerCompileExpr(c, node->as.conditional.elseValue);
  MS_ASSERT(c->fn->stackDepth == endDepth);
  msCompilerPatchToCurrent(c, endJump, endDepth);
}
```

- [x] **Step 5: 实现比较和位置调用**

加入：

```c
static void msCompilerCompileComparison(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.compareChain.opCount != 1 || node->as.compareChain.operands.count != 2) {
    msCompilerReport(c, node, 307, "comparison chains belong to task 12");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  msCompilerCompileExpr(c, node->as.compareChain.operands.items[0]);
  msCompilerCompileExpr(c, node->as.compareChain.operands.items[1]);
  MsCompareOp compare = node->as.compareChain.ops[0];
  MsOpCode op = MS_OP_COUNT;
  switch (compare) {
    case MS_CMP_LT: op = MS_OP_LT; break;
    case MS_CMP_LE: op = MS_OP_LE; break;
    case MS_CMP_GT: op = MS_OP_GT; break;
    case MS_CMP_GE: op = MS_OP_GE; break;
    case MS_CMP_EQ: op = MS_OP_EQ; break;
    case MS_CMP_NE: op = MS_OP_NE; break;
    case MS_CMP_IN:
    case MS_CMP_NOT_IN: op = MS_OP_IN; break;
    case MS_CMP_IS:
    case MS_CMP_IS_NOT: op = MS_OP_IS; break;
    default: break;
  }
  if (op == MS_OP_COUNT) {
    msCompilerReport(c, node, 307, "comparison operator is not implemented");
    return;
  }
  msCompilerEmitABC(c, op, 0, 0, -1);
  if (compare == MS_CMP_NOT_IN || compare == MS_CMP_IS_NOT) {
    msCompilerEmitABC(c, MS_OP_NOT, 0, 0, 0);
  }
}

static void msCompilerCompileCall(struct MsCompiler* c, const struct MsAst* node) {
  size_t count = node->as.call.args.count;
  if (count > 255) {
    msCompilerReport(c, node, 312, "more than 255 positional arguments");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    if (node->as.call.args.items[i]->kind == MS_AST_KW_ARG) {
      msCompilerReport(c, node, 307, "keyword calls belong to task 13");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      return;
    }
  }
  msCompilerCompileExpr(c, node->as.call.callee);
  for (size_t i = 0; i < count; ++i) {
    msCompilerCompileExpr(c, node->as.call.args.items[i]);
  }
  msCompilerEmitABC(c, MS_OP_CALL, (uint8_t)count, 0, -(int)count);
}
```

在表达式分派加入：

```c
    case MS_AST_LOGICAL:
      msCompilerCompileLogical(c, node);
      break;
    case MS_AST_CONDITIONAL:
      msCompilerCompileConditional(c, node);
      break;
    case MS_AST_COMPARE_CHAIN:
      msCompilerCompileComparison(c, node);
      break;
    case MS_AST_CALL:
      msCompilerCompileCall(c, node);
      break;
```

- [x] **Step 6: 运行测试并提交**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 19 个编译器用例全部通过，跳转偏移和栈高水位符合断言。

```bash
git add src/compiler/ms_compiler.c tests/c/test_compiler.c
git commit -m "✨ feat(compiler): emit branches and calls"
```

### Task 9: 函数、lambda、返回与嵌套 Proto

**Files:**
- Modify: `src/compiler/ms_compiler.c`
- Modify: `tests/c/test_compiler.c`

- [x] **Step 1: 写函数失败测试**

```c
MS_TEST(compiler, functionProtoMetadata) {
  MsTestFixture fix;
  msTestCompile(&fix, "func add(a, b) { return a + b }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_MAKE_FUNCTION, msOpDecodeOp(fix.proto->code[0]));
  MS_ASSERT_EQ(MS_OP_STORE_GLOBAL, msOpDecodeOp(fix.proto->code[1]));
  const struct MsProto* child = msProtoConstProto(fix.proto, 0);
  MS_ASSERT_TRUE(child != NULL);
  MS_ASSERT_EQ(2, child->paramCount);
  MS_ASSERT_EQ(2, child->localCount);
  MS_ASSERT_EQ(2, child->stackSize);
  MS_ASSERT_TRUE(strcmp(msStringData(child->name), "add") == 0);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_LOCAL, MS_OP_LOAD_LOCAL, MS_OP_ADD, MS_OP_RETURN
  };
  msTestAssertOps(child, expected, sizeof(expected) / sizeof(expected[0]));
  MS_ASSERT_EQ(0, msOpDecodeA(child->code[0]));
  MS_ASSERT_EQ(1, msOpDecodeA(child->code[1]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, lambdaAndImplicitReturn) {
  MsTestFixture fix;
  msTestCompile(&fix, "lambda x: x; func f() { pass }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  const struct MsProto* lambda = msProtoConstProto(fix.proto, 0);
  const struct MsProto* function = msProtoConstProto(fix.proto, 1);
  MS_ASSERT_TRUE(lambda != NULL && function != NULL);
  MS_ASSERT_TRUE(strcmp(msStringData(lambda->name), "<lambda>") == 0);
  MS_ASSERT_EQ(1, lambda->paramCount);
  MS_ASSERT_EQ(MS_OP_MAKE_LAMBDA, msOpDecodeOp(fix.proto->code[0]));
  MS_ASSERT_EQ(MS_OP_LOAD_NIL, msOpDecodeOp(function->code[0]));
  MS_ASSERT_EQ(MS_OP_RETURN, msOpDecodeOp(function->code[1]));
  MS_ASSERT_EQ(1, function->stackSize);
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, nestedFunctionProtoTree) {
  MsTestFixture fix;
  msTestCompile(&fix, "func outer() { func inner() { return 1; } }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  const struct MsProto* outer = msProtoConstProto(fix.proto, 0);
  MS_ASSERT_TRUE(outer != NULL);
  const struct MsProto* inner = msProtoConstProto(outer, 0);
  MS_ASSERT_TRUE(inner != NULL);
  MS_ASSERT_TRUE(strcmp(msStringData(inner->name), "inner") == 0);
  MS_ASSERT_EQ(MS_OP_MAKE_FUNCTION, msOpDecodeOp(outer->code[0]));
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(outer->code[1]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, asyncFlagIsPreserved) {
  MsTestFixture fix;
  msTestCompile(&fix, "async func f() { pass }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  const struct MsProto* child = msProtoConstProto(fix.proto, 0);
  MS_ASSERT_TRUE(child != NULL && child->isAsync);
  MS_ASSERT_TRUE(!child->hasVarArgs && !child->hasKwArgs);
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, defaultParamReportsE307) {
  static const char* const sources[] = {
    "func f(a = 1) { return a; }",
    "func f(*args) { pass }",
    "func f(**kwargs) { pass }"
  };
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
    MsTestFixture fix;
    msTestCompile(&fix, sources[i], MS_COMPILE_DEFAULT);
    MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
    MS_ASSERT_EQ(307, (long long)msDiagListAt(&fix.diags, 0)->code);
    msTestFixtureDestroy(&fix);
  }
}

MS_TEST(compiler, outerLocalReportsE310) {
  MsTestFixture fix;
  msTestCompile(&fix, "func outer() { x := 1; func inner() { return x; } }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_TRUE(fix.proto == NULL);
  MS_ASSERT_EQ(310, (long long)msDiagListAt(&fix.diags, 0)->code);
  msTestFixtureDestroy(&fix);
}
```

注册六个用例：

```c
  {"Compiler.FunctionProtoMetadata", testcompilerfunctionProtoMetadata},
  {"Compiler.LambdaAndImplicitReturn", testcompilerlambdaAndImplicitReturn},
  {"Compiler.NestedFunctionProtoTree", testcompilernestedFunctionProtoTree},
  {"Compiler.AsyncFlagIsPreserved", testcompilerasyncFlagIsPreserved},
  {"Compiler.DefaultParamReportsE307", testcompilerdefaultParamReportsE307},
  {"Compiler.OuterLocalReportsE310", testcompilerouterLocalReportsE310},
```

- [x] **Step 2: 运行测试并确认红**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: `func`、`lambda` 和 `return` 尚未支持，新用例失败。

- [x] **Step 3: 泛化函数上下文创建**

用以下逻辑替换 `msCompilerPushFunc` 中固定的 `<main>` 名称和标志初始化；保留既有 OOM 清理结构：

```c
  fn->isAsync = funcNode != NULL && funcNode->as.func.isAsync;
  fn->builder.proto.isAsync = fn->isAsync;
  struct MsString* protoName;
  if (funcNode == NULL) {
    protoName = msStringFromCStr(c->heap, "<main>");
  } else if (funcNode->kind == MS_AST_LAMBDA) {
    protoName = msStringFromCStr(c->heap, "<lambda>");
  } else {
    protoName = msStringNew(c->heap, funcNode->as.func.name, funcNode->as.func.nameLen);
  }
```

`fn->isModule` 仍为 `funcNode == NULL`；`sourceFile` 对每个子 Proto 同样设置。

- [x] **Step 4: 实现参数、函数体和 return**

加入：

```c
static bool msCompilerValidateParams(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.func.params.count > MS_COMPILER_MAX_LOCALS) {
    msCompilerFail(c, node, MS_ERROR_SYNTAX, 306, "too many parameter slots");
    return false;
  }
  for (size_t i = 0; i < node->as.func.params.count; ++i) {
    const struct MsAst* param = node->as.func.params.items[i];
    if (param->kind != MS_AST_PARAM || param->as.param.paramKind != MS_PARAM_NORMAL
        || param->as.param.defaultValue != NULL) {
      msCompilerReport(c, param, 307, "default and variadic parameters belong to task 13");
      return false;
    }
  }
  return true;
}

static void msCompilerCompileReturn(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.returnStmt.values.count > 1) {
    msCompilerReport(c, node, 307, "multiple return values belong to task 13");
    return;
  }
  if (node->as.returnStmt.values.count == 0) {
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
  } else {
    msCompilerCompileExpr(c, node->as.returnStmt.values.items[0]);
  }
  msCompilerEmitABC(c, MS_OP_RETURN, 0, 0, -1);
}

static bool msCompilerBuildFunctionProto(struct MsCompiler* c, const struct MsAst* node,
    struct MsProto** out) {
  *out = NULL;
  if (node->as.func.isStatic || !msCompilerValidateParams(c, node)) {
    if (node->as.func.isStatic) {
      msCompilerReport(c, node, 307, "static methods belong to task 15");
    }
    return false;
  }
  if (msCompilerPushFunc(c, node) == NULL) {
    return false;
  }
  for (size_t i = 0; i < node->as.func.params.count; ++i) {
    const struct MsAst* param = node->as.func.params.items[i];
    struct MsString* name = msCompilerNameFromSlice(c, param->as.param.name, param->as.param.nameLen);
    if (name != NULL) {
      msCompilerDeclareLocal(c, name);
    }
  }
  c->fn->builder.proto.paramCount = (int)node->as.func.params.count;
  if (node->kind == MS_AST_LAMBDA) {
    msCompilerCompileExpr(c, node->as.func.body);
    msCompilerEmitABC(c, MS_OP_RETURN, 0, 0, -1);
  } else if (node->as.func.body != NULL && node->as.func.body->kind == MS_AST_BLOCK) {
    for (size_t i = 0; i < node->as.func.body->as.block.count && c->sticky == MS_OK; ++i) {
      msCompilerCompileStmt(c, node->as.func.body->as.block.items[i]);
    }
  } else {
    msCompilerReport(c, node, 307, "function body must be a block");
  }
  return msCompilerPopFunc(c, out) == MS_OK;
}
```

- [x] **Step 5: 实现定义点物化和名字绑定**

加入：

```c
static void msCompilerBindFunctionName(struct MsCompiler* c, const struct MsAst* node) {
  struct MsString* name = msCompilerNameFromSlice(c, node->as.func.name, node->as.func.nameLen);
  if (name == NULL) {
    return;
  }
  if (c->fn->isModule && c->fn->scopeDepth == 0) {
    int index = msProtoAddConst(&c->fn->builder, (struct MsObject*)name);
    if (index >= 0) {
      msCompilerEmitABC(c, MS_OP_STORE_GLOBAL, 0, (uint16_t)index, -1);
    } else {
      msCompilerSyncBuilder(c);
    }
    return;
  }
  int slot = msCompilerDeclareLocal(c, name);
  if (slot >= 0) {
    msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
  }
}

static void msCompilerCompileFunction(struct MsCompiler* c, const struct MsAst* node) {
  struct MsProto* child = NULL;
  if (!msCompilerBuildFunctionProto(c, node, &child)) {
    if (node->kind == MS_AST_LAMBDA && c->sticky == MS_OK) {
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    }
    return;
  }
  int index = msProtoAddProto(&c->fn->builder, child);
  if (index < 0) {
    msProtoFree(child);
    msCompilerSyncBuilder(c);
    return;
  }
  MsOpCode op = node->kind == MS_AST_LAMBDA ? MS_OP_MAKE_LAMBDA : MS_OP_MAKE_FUNCTION;
  msCompilerEmitABC(c, op, 0, (uint16_t)index, 1);
  if (node->kind == MS_AST_FUNC_DECL) {
    msCompilerBindFunctionName(c, node);
  }
}
```

在语句分派加入：

```c
    case MS_AST_RETURN:
      msCompilerCompileReturn(c, node);
      break;
    case MS_AST_FUNC_DECL:
      msCompilerCompileFunction(c, node);
      break;
```

在表达式分派加入：

```c
    case MS_AST_LAMBDA:
      msCompilerCompileFunction(c, node);
      break;
```

- [x] **Step 6: 运行测试并提交**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 25 个编译器用例全部通过；嵌套 Proto 树递归释放且无泄漏。

```bash
git add src/compiler/ms_compiler.c tests/c/test_compiler.c
git commit -m "✨ feat(compiler): compile functions and lambdas"
```

### Task 10: REPL、防御路径、诊断上限与 OOM 验收

**Files:**
- Modify: `src/compiler/ms_compiler.c`
- Modify: `tests/c/test_compiler.c`

- [x] **Step 1: 写 REPL、赋值和行号测试**

```c
MS_TEST(compiler, replPrintsOnlyLastExpression) {
  MsTestFixture fix;
  msTestCompile(&fix, "1; 2;", MS_COMPILE_REPL);
  MS_ASSERT_EQ(MS_OK, fix.result);
  static const MsOpCode expected[] = {
    MS_OP_LOAD_CONST, MS_OP_STORE_LOCAL, MS_OP_LOAD_CONST,
    MS_OP_PRINT_EXPR, MS_OP_LOAD_NIL, MS_OP_RETURN
  };
  msTestAssertOps(fix.proto, expected, sizeof(expected) / sizeof(expected[0]));
  for (int i = 0; i < fix.proto->codeLen; ++i) {
    if (i != 3) {
      MS_ASSERT_TRUE(msOpDecodeOp(fix.proto->code[i]) != MS_OP_PRINT_EXPR);
    }
  }
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, assignmentUsesResolvedStore) {
  MsTestFixture fix;
  msTestCompile(&fix, "x := 0; x = 1; { y := 0; y = 2; }", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(MS_OP_STORE_GLOBAL, msOpDecodeOp(fix.proto->code[3]));
  MS_ASSERT_EQ(MS_OP_STORE_LOCAL, msOpDecodeOp(fix.proto->code[7]));
  MS_ASSERT_EQ(0, msOpDecodeA(fix.proto->code[7]));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, lineTableFollowsStatements) {
  MsTestFixture fix;
  msTestCompile(&fix, "1;\n2;\n", MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_OK, fix.result);
  MS_ASSERT_EQ(1, (long long)msProtoLineAt(fix.proto, 0));
  MS_ASSERT_EQ(2, (long long)msProtoLineAt(fix.proto, 2));
  msTestFixtureDestroy(&fix);
}
```

注册：

```c
  {"Compiler.ReplPrintsOnlyLastExpression", testcompilerreplPrintsOnlyLastExpression},
  {"Compiler.AssignmentUsesResolvedStore", testcompilerassignmentUsesResolvedStore},
  {"Compiler.LineTableFollowsStatements", testcompilerlineTableFollowsStatements},
```

- [x] **Step 2: 先运行 REPL 测试确认红，再实现**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: REPL 用例仍生成 `STORE_LOCAL` 而不是最后的 `PRINT_EXPR`；赋值和行号用例应通过。

在 `struct MsFuncCtx` 增加 `bool replPrint;`，然后把 SOURCE_FILE 循环改为：

```c
      for (size_t i = 0; i < node->as.block.count && c->sticky == MS_OK; ++i) {
        const struct MsAst* statement = node->as.block.items[i];
        c->fn->replPrint = (c->flags & MS_COMPILE_REPL) != 0
            && i + 1 == node->as.block.count && statement->kind == MS_AST_EXPR_STMT;
        msCompilerCompileStmt(c, statement);
        c->fn->replPrint = false;
      }
```

把 EXPR_STMT 分支改为：

```c
    case MS_AST_EXPR_STMT: {
      bool print = c->fn->isModule && c->fn->replPrint;
      c->fn->replPrint = false;
      msCompilerCompileExpr(c, node->as.exprStmt.expr);
      if (print) {
        msCompilerEmitABC(c, MS_OP_PRINT_EXPR, 0, 0, -1);
      } else if (c->sticky == MS_OK) {
        int slot = msCompilerDiscardSlot(c);
        if (slot >= 0) {
          msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
        }
      }
      break;
    }
```

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 28 个编译器用例全部通过。

- [x] **Step 3: 写诊断和规模上限测试**

```c
MS_TEST(compiler, unsupportedAndLimitDiagnostics) {
  static const struct {
    const char* source;
    uint32_t code;
  } cases[] = {
    {"b\"x\";", 307},
    {"a < b < c;", 307},
    {"f(x = 1);", 307},
    {"a, b := 1, 2;", 307},
    {"func f() { return 1, 2; }", 307}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    MsTestFixture fix;
    msTestCompile(&fix, cases[i].source, MS_COMPILE_DEFAULT);
    MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
    MS_ASSERT_TRUE(fix.proto == NULL);
    MS_ASSERT_EQ((long long)cases[i].code, (long long)msDiagListAt(&fix.diags, 0)->code);
    msTestFixtureDestroy(&fix);
  }
}

MS_TEST(compiler, callArgumentLimit) {
  char source[2048];
  size_t used = 0;
  used += (size_t)snprintf(source + used, sizeof(source) - used, "f(");
  for (int i = 0; i < 256; ++i) {
    used += (size_t)snprintf(source + used, sizeof(source) - used, "%s0", i == 0 ? "" : ",");
  }
  snprintf(source + used, sizeof(source) - used, ");");
  MsTestFixture fix;
  msTestCompile(&fix, source, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(312, (long long)msDiagListAt(&fix.diags, 0)->code);
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, diagnosticCapStopsAtTwenty) {
  char source[160];
  source[0] = '\0';
  for (int i = 0; i < 20; ++i) {
    strcat(source, "b\"\";");
  }
  MsTestFixture fix;
  msTestCompile(&fix, source, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(20, (long long)msDiagListCount(&fix.diags));
  msTestFixtureDestroy(&fix);
}

MS_TEST(compiler, constantFoldHonorsNodeDepth) {
  char source[4096];
  size_t used = 0;
  for (int i = 0; i < 520; ++i) {
    used += (size_t)snprintf(source + used, sizeof(source) - used, "not ");
  }
  snprintf(source + used, sizeof(source) - used, "nil;");
  MsTestFixture fix;
  msTestCompile(&fix, source, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(311, (long long)msDiagListAt(&fix.diags, 0)->code);
  msTestFixtureDestroy(&fix);
}
```

注册四个诊断/防御用例：

```c
  {"Compiler.UnsupportedAndLimitDiagnostics", testcompilerunsupportedAndLimitDiagnostics},
  {"Compiler.CallArgumentLimit", testcompilercallArgumentLimit},
  {"Compiler.DiagnosticCapStopsAtTwenty", testcompilerdiagnosticCapStopsAtTwenty},
  {"Compiler.ConstantFoldHonorsNodeDepth", testcompilerconstantFoldHonorsNodeDepth},
```

- [x] **Step 4: 运行诊断测试并修正明确的占位路径**

Run: `cmake --build build --target test_compiler && ctest --test-dir build -R '^test_compiler$' --output-on-failure`

Expected: 所有新负例返回 `MS_ERROR_SYNTAX`、诊断码正确、`fix.proto == NULL`。当前实现已含这些路径；若某个 case 仍发射父操作码，确保其错误分支只补一条 `LOAD_NIL`，不再继续生成该节点的子表达式。

- [x] **Step 5: 写槽位/块深度和全覆盖 OOM 测试**

增加：

```c
MS_TEST(compiler, localAndScopeLimits) {
  char locals[8192];
  size_t used = 0;
  used += (size_t)snprintf(locals + used, sizeof(locals) - used, "{");
  for (int i = 0; i < 257; ++i) {
    used += (size_t)snprintf(locals + used, sizeof(locals) - used, "a%d := 0;", i);
  }
  snprintf(locals + used, sizeof(locals) - used, "}");
  MsTestFixture fix;
  msTestCompile(&fix, locals, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(306, (long long)msDiagListAt(&fix.diags, 0)->code);
  msTestFixtureDestroy(&fix);

  char blocks[512];
  used = 0;
  for (int i = 0; i < 130; ++i) {
    used += (size_t)snprintf(blocks + used, sizeof(blocks) - used, "{");
  }
  used += (size_t)snprintf(blocks + used, sizeof(blocks) - used, "pass;");
  for (int i = 0; i < 130; ++i) {
    used += (size_t)snprintf(blocks + used, sizeof(blocks) - used, "}");
  }
  msTestCompile(&fix, blocks, MS_COMPILE_DEFAULT);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, fix.result);
  MS_ASSERT_EQ(306, (long long)msDiagListAt(&fix.diags, 0)->code);
  msTestFixtureDestroy(&fix);
}
```

把 `msTestCompile` 改为以下两个助手，保留原调用签名：

```c
static void msTestCompileWithFailAfter(MsTestFixture* fix, const char* source,
    uint32_t flags, int64_t failAfter) {
  memset(fix, 0, sizeof(*fix));
  msMemResetStats();
  msMemSetFailAfter(-1);
  MS_ASSERT_EQ(MS_OK, msHeapInit(&fix->heap));
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&fix->diags, "test.ms"));
  msAstArenaInit(&fix->arena);
  msLexerInit(&fix->lexer, source, strlen(source), "test.ms", &fix->diags);
  msParserInit(&fix->parser, &fix->lexer, &fix->arena, &fix->diags);
  msCompilerInit(&fix->compiler, &fix->heap, &fix->diags, "test.ms", flags);
  fix->result = msParserParse(&fix->parser, &fix->root);
  msLexerDestroy(&fix->lexer);
  if (fix->result == MS_OK) {
    msMemSetFailAfter(failAfter);
    fix->result = msCompilerCompile(&fix->compiler, fix->root, &fix->proto);
    msMemSetFailAfter(-1);
  }
}

static void msTestCompile(MsTestFixture* fix, const char* source, uint32_t flags) {
  msTestCompileWithFailAfter(fix, source, flags, -1);
}
```

新增 OOM 用例：

```c
MS_TEST(compiler, nestedProtoOomPathsHaveNoLeaks) {
  const char* source = "func outer() { x := 1; func inner(a) { return a + 2; } }";
  bool sawSuccess = false;
  for (int64_t failAfter = 0; failAfter < 80 && !sawSuccess; ++failAfter) {
    MsTestFixture fix;
    msTestCompileWithFailAfter(&fix, source, MS_COMPILE_DEFAULT, failAfter);
    if (fix.result == MS_OK) {
      sawSuccess = true;
    } else {
      MS_ASSERT_EQ(MS_ERROR_OOM, fix.result);
      MS_ASSERT_TRUE(fix.proto == NULL);
    }
    msTestFixtureDestroy(&fix);
  }
  MS_ASSERT_TRUE(sawSuccess);
}
```

注册：

```c
  {"Compiler.LocalAndScopeLimits", testcompilerlocalAndScopeLimits},
  {"Compiler.NestedProtoOomPathsHaveNoLeaks", testcompilernestedProtoOomPathsHaveNoLeaks},
```

- [x] **Step 6: 运行编译器与既有全测**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`

Expected: 34 个编译器用例和全部 11 个 CTest 目标通过；OOM 循环最终到达一次成功编译，每轮 `liveBlocks == 0`。

- [x] **Step 7: 提交**

```bash
git add src/compiler/ms_compiler.c tests/c/test_compiler.c
git commit -m "✅ test(compiler): cover diagnostics and OOM paths"
```

### Task 11: 文档同步、严格验收与最终提交

**Files:**
- Modify: `docs/tasks/05-bytecode-proto.md`
- Modify: `docs/tasks/06-object-model.md`
- Modify: `docs/tasks/07-compiler.md`
- Modify: `docs/tasks/README.md:48`
- Modify: `docs/superpowers/plans/2026-10-17-task-07-compiler.md`（仅勾选已完成步骤）

- [x] **Step 1: 同步 Proto 和对象模型文档**

在任务 05 的 `MsProto` 布局中补回首部的 `struct MsObjectHeader header`，把「与对象模型的衔接」改为当前事实：内部 `MS_TYPE_PROTO` 不挂入堆，父 Proto/builder 递归拥有子 Proto，`msProtoAddProto` 成功转移所有权、失败保留调用方所有权，`msProtoConstProto` 提供安全查询。

在任务 06 的类型标签节明确：`MS_TYPE_PROTO` 位于 `MS_TYPE_COUNT` 之后，不是第 21 个脚本堆类型；`MS_TYPE_COUNT == 20` 和 `heap->types[20]` 不变，`msObjectAlloc` 不接受该内部标签。

在常量池去重说明中补充：浮点零只有符号位相同才共享槽位，NaN 仍不按值去重。

- [x] **Step 2: 同步任务 07 设计偏差和验收状态**

对 `docs/tasks/07-compiler.md` 做以下明确替换：

- 丢弃槽从「每函数一个」改为「每作用域一个、惰性分配、出块失效」，并说明逻辑临时槽恢复 `localCount`。
- 空模块验证改为 `localCount == 0`、`stackSize == 1`。
- 名字解析明确按内容比较，不要求长字符串驻留。
- 数字转换明确切片复制/去下划线/NUL 终止和 `INT64_MIN` 直接负字面量语境。
- 诊断节明确可恢复节点错误与致命 sticky 的区分，以及第 20 条后停止。
- 子 Proto 入池接口改为 `msProtoAddProto`，产物树由 `msProtoFree` 递归释放。
- 验收项逐条与实际测试核对后勾选；只有全部满足才将文首状态改为 ✅。
- 将 `docs/tasks/README.md` 任务 07 行的 ⬜ 改为 ✅。

- [x] **Step 3: 运行格式检查和 Debug 全量验收**

Run:

```bash
git diff --check
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: `git diff --check` 无输出；构建零警告；`100% tests passed, 0 tests failed out of 11`。

- [x] **Step 4: 运行 Release 全量验收**

Run:

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Expected: Release 构建零警告，11/11 测试目标通过；所有产物位于 `build/` 内。

- [x] **Step 5: 勾选计划并提交文档**

把本计划中已完成的步骤复选框由 `- [ ]` 改为 `- [x]`，不得勾掉未实际执行或失败的步骤。

```bash
git add docs/tasks/README.md docs/tasks/05-bytecode-proto.md docs/tasks/06-object-model.md docs/tasks/07-compiler.md docs/superpowers/plans/2026-10-17-task-07-compiler.md
git commit -m "📝 docs(tasks): complete task 07 compiler"
```

- [x] **Step 6: 最终核对工作树**

Run: `git status --short && git log --oneline -13`

Expected: 工作树干净；日志包含 Proto 前置、编译器各层、测试硬化和文档提交，没有构建目录或计划外文件被提交。
