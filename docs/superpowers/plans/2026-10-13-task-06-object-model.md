# 任务 06 对象模型基础 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付 mslang 对象模型基础模块 `src/object/ms_object.{h,c}`（类型标签、对象头、堆上下文、统一分配入口、标量构造与访问、值相等/哈希/真值）与 `src/object/ms_string.{h,c}`（MsString、FNV-1a 哈希、短字符串驻留表），并把任务 05 常量池的指针去重升级为值去重。

**Architecture:** 一切值装箱为 `MsObject*`（首成员统一为 `MsObjectHeader{type, tag, markColor, gcNext}`，64 位下 24 字节）；`MsHeap` 承载全对象链表、小整数驻留缓存（-256..4095，内嵌数组）、20 个内建类型对象（内嵌数组、自指 `type`）、nil/bool 单例与短字符串驻留表（开放寻址、负载因子 0.75 倍增）。所有堆分配只经 `msAlloc/msRealloc/msFree`，分配收口于 `msObjectAlloc`。

**Tech Stack:** C11（MSVC `/W4 /WX`，GCC/Clang `-Wall -Wextra -Wpedantic -Werror`）、CMake + Ninja、自研 `ms_test.h`（`MS_TEST`/`MS_ASSERT_TRUE`/`MS_ASSERT_EQ`/`MS_TEST_MAIN`）。

**权威规格：** `docs/tasks/06-object-model.md`（设计、接口签名、验收标准以此为准；本计划不重复论证，只落地）。

---

## 代码库事实（实现者必读）

- 仓库根：`C:\Workspace\Repositories\mslang-cc`。所有工作在 worktree `.worktrees/task-06-object-model`（分支同名）上进行。
- 构建（worktree 内首次配置需显式 `-G Ninja`）：
  ```bash
  cmake -S . -B build -G Ninja
  cmake --build build
  ctest --test-dir build --output-on-failure
  ctest --test-dir build -R test_object --output-on-failure   # 单测
  ```
- `MsResult` 唯一定义点在公共头 `<mslang/error.h>`（`MS_OK`/`MS_ERROR_RUNTIME`/`MS_ERROR_SYNTAX`/`MS_ERROR_OOM`）。
- 内存接口 `src/core/ms_memory.h`：`msAlloc/msRealloc/msFree`、`struct MsMemStats`、`msMemGetStats`、`msMemResetStats`、`msMemSetFailAfter(int64_t n)`（n 次成功分配后开始失败；<0 关闭）。
- 通用宏 `src/core/ms_common.h`：`MS_ASSERT`（NDEBUG 下编译为 `((void)0)`）、`MS_UNREACHABLE`、`MS_UNUSED`、`MS_ARRAY_LEN`。
- **NDEBUG 陷阱（任务 05 后修复过两次）：** 只在 `MS_ASSERT` 里引用的形参/变量在 Release 下会报 C4100/C4189 并被 `/WX` 当作错误。凡仅被断言引用的名字，断言后必须紧跟 `MS_UNUSED(name);`。
- 测试基建 `tests/c/ms_test.h` 无 setUp/tearDown fixture：每个 `MS_TEST` 函数自建自毁 `MsHeap`，用 `msMemResetStats()` 开头、`msMemGetStats` 断言 `liveBlocks == 0` 结尾。MSVC 下测试目标已禁用 C4127（do-while(0) 宏）。
- 提交规范（AGENTS.md 强制）：`<gitmoji> <type>(<scope>): <msg>`，gitmoji 从规定列表选。源文件必须纯 ASCII（MSVC C4819）、UTF-8 无 BOM、LF、2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef。
- C 的 TDD「红」步是编译/链接失败（标识符未定义），属正常预期。
- 头文件自包含验证方式：测试文件的**第一个** include 就是被测头。

## 对规格的四处显式处理（实现与评审以规格文档「设计依据」节为准）

1. 头部补充 `tag` 字段（64 位下不涨头部长度，有 `_Static_assert` 守住 24 字节）。
2. `MsTypeTag` 为 20 个类型标签 + `MS_TYPE_COUNT` 哨兵（共 21 个枚举值）。
3. 堆上下文自立为 `struct MsHeap`（不依赖任务 08 的 `MsState`）。
4. `struct MsType` 取最小定义 `{header, name}`。

## 不在本计划范围（规格明确排除）

- `msObjectBinaryOp`/`msObjectUnaryOp`/`msLen`/`MsState`（任务 08/09）。
- GC 标记-清除算法与根栈纪律（任务 17/18）；本任务只承载字段与链表维护。
- 大整数（任务 31）；本任务 `msIntIsWord` 恒真。
- `msProtoDump` 常量打印升级（任务 05 的 `#k<idx>` 占位保留）。

## 文件结构

| 文件 | 责任 |
|---|---|
| `src/object/ms_object.h` | `MsTypeTag`/`MsMarkColor`、对象头与基座结构体、`struct MsHeap`、全部对象级函数声明；自包含 |
| `src/object/ms_object.c` | 堆初始化/销毁、分配入口、单例与 int 缓存、标量构造访问、值相等/哈希/真值、类型名表 |
| `src/object/ms_string.h` | `struct MsString`、驻留阈值、字符串函数声明 |
| `src/object/ms_string.c` | FNV-1a、驻留表（探测/插入/倍增 rehash）、构造与比较 |
| `tests/c/test_object.c` | ms_object 单元测试 |
| `tests/c/test_string.c` | ms_string 单元测试 |
| `CMakeLists.txt` | 库源文件与测试目标接线 |
| `src/vm/ms_proto.{h,c}` + `tests/c/test_proto.c` | 任务 7 衔接：常量池值去重 |

---

### Task 1: `ms_object.h` 骨架 + 类型标签名表 + CMake 接线

**Files:**
- Create: `src/object/ms_object.h`
- Create: `src/object/ms_object.c`
- Create: `tests/c/test_object.c`
- Modify: `CMakeLists.txt`（库源文件列表 + 测试列表）

- [ ] **Step 1: 写失败测试 `tests/c/test_object.c`**

第一个 include 就是被测头，以此验证头文件自包含：

```c
#include "object/ms_object.h"

#include <string.h>

#include "ms_test.h"

MS_TEST(object, typeTagCountIs20) {
  MS_ASSERT_EQ(20, MS_TYPE_COUNT);
}

MS_TEST(object, typeTagNamesCompleteDistinctOrdered) {
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    const char* name = msTypeTagName((MsTypeTag)i);
    MS_ASSERT_TRUE(name != NULL);
    MS_ASSERT_TRUE(strlen(name) > 0);
    for (int j = i + 1; j < (int)MS_TYPE_COUNT; ++j) {
      MS_ASSERT_TRUE(strcmp(name, msTypeTagName((MsTypeTag)j)) != 0);
    }
  }
  MS_ASSERT_TRUE(strcmp(msTypeTagName(MS_TYPE_NIL), "MS_TYPE_NIL") == 0);
  MS_ASSERT_TRUE(strcmp(msTypeTagName(MS_TYPE_STRING), "MS_TYPE_STRING") == 0);
  MS_ASSERT_TRUE(strcmp(msTypeTagName(MS_TYPE_C_TYPE), "MS_TYPE_C_TYPE") == 0);
  MS_ASSERT_TRUE(strcmp(msTypeTagName((MsTypeTag)MS_TYPE_COUNT), "MS_TYPE_INVALID") == 0);
  MS_ASSERT_TRUE(strcmp(msTypeTagName((MsTypeTag)-1), "MS_TYPE_INVALID") == 0);
}

static const MsTestCase msTestCases[] = {
  {"object.typeTagCountIs20", testobjecttypeTagCountIs20},
  {"object.typeTagNamesCompleteDistinctOrdered", testobjecttypeTagNamesCompleteDistinctOrdered},
};

MS_TEST_MAIN(msTestCases)
```

- [ ] **Step 2: CMake 接线**

`CMakeLists.txt` 的 `add_library(mslang ...)` 源列表中，在 `src/ms_version.c` 之后插入 `src/object/ms_object.c`；在测试列表 `mslang_add_c_test(test_ms_test)` 之后插入 `mslang_add_c_test(test_object)`。保持现有缩进（4 空格）。

- [ ] **Step 3: 运行测试确认失败**

```bash
cmake --build build
```

预期：编译失败，`msTypeTagName` 未定义（`ms_object.c` 尚不存在或为空 → 链接错误 LNK2019）。

- [ ] **Step 4: 写 `src/object/ms_object.h`**

```c
#ifndef MSLANG_SRC_OBJECT_MS_OBJECT_H_
#define MSLANG_SRC_OBJECT_MS_OBJECT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mslang/error.h>

#include "core/ms_common.h"

struct MsString;

// Type tags: order and names match 08-vm-internals section 3. MS_TYPE_COUNT
// is a sentinel, not a tag (20 tags total).
typedef enum {
  MS_TYPE_NIL, MS_TYPE_BOOL, MS_TYPE_INT, MS_TYPE_FLOAT,
  MS_TYPE_STRING, MS_TYPE_BYTES, MS_TYPE_LIST, MS_TYPE_TUPLE,
  MS_TYPE_DICT, MS_TYPE_SET, MS_TYPE_FUNCTION, MS_TYPE_CLASS,
  MS_TYPE_INSTANCE, MS_TYPE_MODULE, MS_TYPE_CHANNEL, MS_TYPE_COROUTINE,
  MS_TYPE_ITERATOR, MS_TYPE_BOUND_METHOD, MS_TYPE_C_FUNCTION,
  MS_TYPE_C_TYPE,
  MS_TYPE_COUNT              // sentinel: number of type tags (20)
} MsTypeTag;

typedef enum {
  MS_MARK_WHITE = 0,         // unmarked; sweep candidate once GC lands (task 17)
  MS_MARK_BLACK = 1          // marked / permanent (singletons, cache, type objects)
} MsMarkColor;

struct MsObjectHeader {
  struct MsType* type;       // the type object; never NULL after construction
  uint8_t tag;               // MsTypeTag, cached for O(1) dispatch
  uint8_t markColor;         // MsMarkColor
  struct MsObject* gcNext;   // next node in the heap's all-objects list
};

struct MsObject {
  struct MsObjectHeader header;
};

struct MsBool {
  struct MsObjectHeader header;
  bool value;
};

struct MsInt {
  struct MsObjectHeader header;
  int64_t value;             // machine-word inline storage; bigint promotion is task 31
};

struct MsFloat {
  struct MsObjectHeader header;
  double value;              // IEEE 754 double
};

// Minimal type object; methods, MRO and instance layout are tasks 15/25.
struct MsType {
  struct MsObjectHeader header;   // tag == MS_TYPE_CLASS; type == &heap->types[MS_TYPE_CLASS]
  struct MsString* name;          // interned type name ("int", "str", ...)
};

#define MS_INT_CACHE_MIN (-256)
#define MS_INT_CACHE_MAX 4095
#define MS_INT_CACHE_SIZE (MS_INT_CACHE_MAX - MS_INT_CACHE_MIN + 1)   // 4352

struct MsHeap {
  struct MsObject* allObjects;            // head of the all-objects list (gcNext chain)
  size_t objectCount;                     // linked (GC-managed) objects; threshold input for task 17
  struct MsObject nilObject;              // the nil singleton
  struct MsBool trueObject;               // the true singleton
  struct MsBool falseObject;              // the false singleton
  struct MsInt intCache[MS_INT_CACHE_SIZE];        // small-int residency, embedded
  struct MsType types[MS_TYPE_COUNT];              // builtin type objects, embedded
  struct MsString** internSlots;          // short-string intern table, msAlloc'd bucket array
  size_t internCap;                       // bucket count, power of two
  size_t internLen;                       // interned string count
};

_Static_assert(MS_TYPE_COUNT == 20, "MsTypeTag must carry exactly 20 type tags");
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(struct MsObjectHeader) == 24,
               "the cached tag must not grow the 24-byte header on 64-bit");
#endif

// MS_ERROR_OOM leaves *heap zeroed.
MsResult msHeapInit(struct MsHeap* heap);

// Frees every linked object, then the intern buckets. NULL or zeroed heap is a no-op.
void msHeapDestroy(struct MsHeap* heap);

// Core allocation: msAlloc's size bytes, initializes the header, links into the
// all-objects list (gcNext), bumps objectCount. Returns NULL on OOM.
// All concrete constructors funnel through here; direct msAlloc of objects is
// forbidden outside this module.
struct MsObject* msObjectAlloc(struct MsHeap* heap, MsTypeTag tag, size_t size);

// Unlinks obj from the all-objects list and frees it. Used by msHeapDestroy and
// by the GC sweep (task 17); NOT part of normal value lifecycle before then.
void msObjectFree(struct MsHeap* heap, struct MsObject* obj);

// Singleton accessors (per-heap; no mutable globals).
struct MsObject* msHeapNil(struct MsHeap* heap);
struct MsObject* msHeapBool(struct MsHeap* heap, bool value);

// Int constructor: values in [MS_INT_CACHE_MIN, MS_INT_CACHE_MAX] return the
// resident cache entry (same pointer every time); others allocate via
// msObjectAlloc. Returns NULL on OOM.
struct MsInt* msIntNew(struct MsHeap* heap, int64_t value);

struct MsFloat* msFloatNew(struct MsHeap* heap, double value);

// Accessors take the boxed base pointer so the VM fast path (task 08) needs
// no downcast. Wrong-tag input is a programming error (MS_ASSERT in debug).
bool msIntIsWord(const struct MsObject* obj);        // machine-word int form; always true in v0.1
int64_t msIntWordValue(const struct MsObject* obj);  // valid only when msIntIsWord holds
double msFloatValue(const struct MsObject* obj);
bool msBoolValue(const struct MsObject* obj);

static inline MsTypeTag msObjectTag(const struct MsObject* obj) {
  return (MsTypeTag)obj->header.tag;
}

struct MsType* msHeapType(struct MsHeap* heap, MsTypeTag tag);     // &heap->types[tag]
const char* msTypeTagName(MsTypeTag tag);                          // static name table

// Value equality per 02-types section 6. Signature fixed by task 05's assumption.
bool msObjectValueEquals(const struct MsObject* a, const struct MsObject* b);

// Hash consistent with msObjectValueEquals: equal values hash equal.
// Hashable scalars only in this task (nil/bool/int/float/string); other tags
// MS_ASSERT in debug (identity hashing for instances is a later task).
uint32_t msObjectHash(const struct MsObject* obj);

// Truthiness per 02-types section 2, scalar subset: nil, false, numeric zero
// and the empty string are falsy. Container emptiness joins in task 16; heap
// objects of other tags are truthy.
bool msObjectIsTruthy(const struct MsObject* obj);

#endif  // MSLANG_SRC_OBJECT_MS_OBJECT_H_
```

- [ ] **Step 5: 写 `src/object/ms_object.c`（仅名表）**

```c
#include "object/ms_object.h"

static const char* const msTypeTagNames[MS_TYPE_COUNT] = {
  "MS_TYPE_NIL", "MS_TYPE_BOOL", "MS_TYPE_INT", "MS_TYPE_FLOAT",
  "MS_TYPE_STRING", "MS_TYPE_BYTES", "MS_TYPE_LIST", "MS_TYPE_TUPLE",
  "MS_TYPE_DICT", "MS_TYPE_SET", "MS_TYPE_FUNCTION", "MS_TYPE_CLASS",
  "MS_TYPE_INSTANCE", "MS_TYPE_MODULE", "MS_TYPE_CHANNEL", "MS_TYPE_COROUTINE",
  "MS_TYPE_ITERATOR", "MS_TYPE_BOUND_METHOD", "MS_TYPE_C_FUNCTION",
  "MS_TYPE_C_TYPE"
};

// Script-visible names per 02-types section 1. Used by msHeapInit (task: heap init).
static const char* const msTypeScriptNames[MS_TYPE_COUNT] = {
  "NilType", "bool", "int", "float", "str", "bytes", "list", "tuple",
  "dict", "set", "function", "type", "instance", "module", "chan",
  "coroutine", "iterator", "bound method", "cfunction", "ctype"
};

const char* msTypeTagName(MsTypeTag tag) {
  if (tag < 0 || tag >= MS_TYPE_COUNT) {
    return "MS_TYPE_INVALID";
  }
  return msTypeTagNames[tag];
}
```

注意：`msTypeScriptNames` 此刻未被引用，MSVC 不报未使用静态变量警告（仅 C 局部变量报 C4189）；若构建报警告，先在该数组上方加 `// used by msHeapInit (Task 4)` 注释并保留，Task 4 落地后自然被引用。

- [ ] **Step 6: 构建并运行测试**

```bash
cmake --build build && ctest --test-dir build -R test_object --output-on-failure
```

预期：PASS，`2 of 2 tests passed`。再跑全量 `ctest --test-dir build --output-on-failure` 确认 9/9 绿（原 8 个 + test_object）。

- [ ] **Step 7: 提交**

```bash
git add src/object/ms_object.h src/object/ms_object.c tests/c/test_object.c CMakeLists.txt
git commit -m "✨ feat(object): add type tags, object header layout and heap context"
```

---

### Task 2: `msObjectAlloc` / `msObjectFree` / `msHeapDestroy` 与全对象链表

**Files:**
- Modify: `src/object/ms_object.c`
- Modify: `tests/c/test_object.c`

本任务的测试使用**清零的栈上 `MsHeap`**（`msHeapInit` 属 Task 4）：`header.type` 指向内嵌 `types[]` 数组在清零后即可用。

- [ ] **Step 1: 追加失败测试**

`#include` 区追加 `#include "core/ms_memory.h"` 与 `#include <string.h>` 已有则不动。测试用例：

```c
MS_TEST(object, allocLinksIntoAllObjectsList) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  struct MsObject* a = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  struct MsObject* b = msObjectAlloc(&heap, MS_TYPE_FLOAT, sizeof(struct MsFloat));
  MS_ASSERT_TRUE(a != NULL && b != NULL);
  MS_ASSERT_TRUE(heap.allObjects == b);          // LIFO head insertion
  MS_ASSERT_TRUE(b->header.gcNext == a);
  MS_ASSERT_TRUE(a->header.gcNext == NULL);
  MS_ASSERT_EQ(2, (long long)heap.objectCount);
  MS_ASSERT_TRUE(a->header.type == &heap.types[MS_TYPE_INT]);
  MS_ASSERT_EQ(MS_TYPE_INT, a->header.tag);
  MS_ASSERT_EQ(MS_MARK_WHITE, a->header.markColor);
  MS_ASSERT_TRUE(b->header.type == &heap.types[MS_TYPE_FLOAT]);
  MS_ASSERT_EQ(MS_TYPE_FLOAT, b->header.tag);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, freeUnlinksHeadMidTail) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  struct MsObject* a = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  struct MsObject* b = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  struct MsObject* c = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  struct MsObject* d = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  MS_ASSERT_TRUE(d != NULL);                     // list: d -> c -> b -> a
  MS_ASSERT_EQ(4, (long long)heap.objectCount);
  msObjectFree(&heap, d);                        // head
  MS_ASSERT_TRUE(heap.allObjects == c);
  MS_ASSERT_EQ(3, (long long)heap.objectCount);
  msObjectFree(&heap, b);                        // middle
  MS_ASSERT_TRUE(c->header.gcNext == a);
  MS_ASSERT_EQ(2, (long long)heap.objectCount);
  msObjectFree(&heap, a);                        // tail
  MS_ASSERT_TRUE(c->header.gcNext == NULL);
  MS_ASSERT_EQ(1, (long long)heap.objectCount);
  msObjectFree(&heap, c);                        // only node
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, allocOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  msMemSetFailAfter(0);
  struct MsObject* obj = msObjectAlloc(&heap, MS_TYPE_INT, sizeof(struct MsInt));
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(obj == NULL);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  msHeapDestroy(&heap);
}

MS_TEST(object, heapDestroyNullAndZeroedSafe) {
  msHeapDestroy(NULL);
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  msHeapDestroy(&heap);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
}

MS_TEST(object, allocMinimumSizeIsBaseObject) {
  msMemResetStats();
  struct MsHeap heap;
  memset(&heap, 0, sizeof(heap));
  struct MsObject* obj = msObjectAlloc(&heap, MS_TYPE_NIL, sizeof(struct MsObject));
  MS_ASSERT_TRUE(obj != NULL);
  MS_ASSERT_EQ(1, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}
```

并把 5 个用例名注册进 `msTestCases`（命名模式沿用现有：`{"object.freeUnlinksHeadMidTail", testobjectfreeUnlinksHeadMidTail}` 等）。

- [ ] **Step 2: 运行确认失败**

```bash
cmake --build build
```

预期：链接失败，`msObjectAlloc` / `msObjectFree` / `msHeapDestroy` 未定义（LNK2019）。

- [ ] **Step 3: 实现**

`ms_object.c` 的 include 区追加 `#include <string.h>` 与 `#include "core/ms_memory.h"`，文件尾追加：

```c
struct MsObject* msObjectAlloc(struct MsHeap* heap, MsTypeTag tag, size_t size) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(tag >= 0 && tag < MS_TYPE_COUNT);
  MS_ASSERT(size >= sizeof(struct MsObject));
  struct MsObject* obj = (struct MsObject*)msAlloc(size);
  if (obj == NULL) {
    return NULL;
  }
  obj->header.type = &heap->types[tag];
  obj->header.tag = (uint8_t)tag;
  obj->header.markColor = MS_MARK_WHITE;
  obj->header.gcNext = heap->allObjects;
  heap->allObjects = obj;
  ++heap->objectCount;
  return obj;
}

void msObjectFree(struct MsHeap* heap, struct MsObject* obj) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(obj != NULL);
  struct MsObject** link = &heap->allObjects;
  while (*link != NULL && *link != obj) {
    link = &(*link)->header.gcNext;
  }
  // Only linked objects may be freed; MS_ASSERT guards debug, this guards release.
  if (*link == NULL) {
    return;
  }
  MS_ASSERT(*link == obj);
  *link = obj->header.gcNext;
  --heap->objectCount;
  msFree(obj);
}

void msHeapDestroy(struct MsHeap* heap) {
  if (heap == NULL) {
    return;
  }
  struct MsObject* obj = heap->allObjects;
  while (obj != NULL) {
    struct MsObject* next = obj->header.gcNext;
    msFree(obj);
    obj = next;
  }
  msFree(heap->internSlots);
  memset(heap, 0, sizeof(*heap));
}
```

- [ ] **Step 4: 构建并运行测试**

```bash
cmake --build build && ctest --test-dir build -R test_object --output-on-failure
```

预期：`7 of 7 tests passed`。全量 ctest 保持全绿。

- [ ] **Step 5: 提交**

```bash
git add src/object/ms_object.c tests/c/test_object.c
git commit -m "✨ feat(object): add unified allocation entry and all-objects list"
```

---

### Task 3: `ms_string.{h,c}` — MsString、FNV-1a、短字符串驻留表

**Files:**
- Create: `src/object/ms_string.h`
- Create: `src/object/ms_string.c`
- Create: `tests/c/test_string.c`
- Modify: `CMakeLists.txt`（库源列表加 `src/object/ms_string.c`、测试列表加 `test_string`）

`msHeapInit` 属 Task 4，本任务测试用**测试内引导辅助函数**初始化堆的驻留桶数组（与 Task 4 的 `msHeapInit` 同款逻辑），对象本体由 Task 2 的 `msObjectAlloc` 提供，清理由 `msHeapDestroy` 完成。

- [ ] **Step 1: 写 `src/object/ms_string.h`**

```c
#ifndef MSLANG_SRC_OBJECT_MS_STRING_H_
#define MSLANG_SRC_OBJECT_MS_STRING_H_

#include "object/ms_object.h"

#define MS_STRING_SHORT_MAX_LEN 32   // residency threshold in bytes
#define MS_STRING_INTERN_INITIAL_CAP 64   // bucket count at heap init; shared with msHeapInit

struct MsString {
  struct MsObjectHeader header;
  uint32_t hash;             // cached FNV-1a-32 over the bytes, computed once at construction
  size_t byteLen;            // UTF-8 byte length; code-point count is NOT stored (O(n) indexing)
  char data[];               // byteLen + 1 bytes, NUL-terminated for C interop
};

// Constructors. Content of length <= MS_STRING_SHORT_MAX_LEN is interned: equal
// content returns the same object (same pointer). Longer strings are always
// freshly allocated. Return NULL on OOM.
struct MsString* msStringNew(struct MsHeap* heap, const char* data, size_t len);
struct MsString* msStringFromCStr(struct MsHeap* heap, const char* cstr);

static inline const char* msStringData(const struct MsString* s) {
  return s->data;
}

static inline size_t msStringLen(const struct MsString* s) {
  return s->byteLen;
}

static inline uint32_t msStringHash(const struct MsString* s) {
  return s->hash;
}

// Byte-wise content equality.
bool msStringEquals(const struct MsString* a, const struct MsString* b);

#endif  // MSLANG_SRC_OBJECT_MS_STRING_H_
```

- [ ] **Step 2: 写失败测试 `tests/c/test_string.c`**

第一个 include 是被测头（自包含验证）。引导辅助函数重复 `msHeapInit` 的驻留桶初始化（Task 4 落地后不动此文件，两个引导路径并存是刻意的——本文件不依赖 Task 4 的类型对象与单例）。

```c
#include "object/ms_string.h"

#include <stdio.h>
#include <string.h>

#include "core/ms_memory.h"
#include "ms_test.h"

// Bootstraps just the intern table (same logic as msHeapInit); the object
// model tasks land heap init in Task 4, and these tests must not depend on it.
static void msTestHeapBootstrap(struct MsHeap* heap) {
  memset(heap, 0, sizeof(*heap));
  heap->internCap = MS_STRING_INTERN_INITIAL_CAP;
  heap->internSlots =
      (struct MsString**)msAlloc(heap->internCap * sizeof(*heap->internSlots));
  MS_ASSERT_TRUE(heap->internSlots != NULL);
  memset(heap->internSlots, 0, heap->internCap * sizeof(*heap->internSlots));
}

static void msTestAssertNoLeaks(void) {
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(string, newRoundtrip) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* s = msStringNew(&heap, "hello", 5);
  MS_ASSERT_TRUE(s != NULL);
  MS_ASSERT_EQ(5, (long long)msStringLen(s));
  MS_ASSERT_TRUE(strcmp(msStringData(s), "hello") == 0);
  MS_ASSERT_EQ('\0', msStringData(s)[5]);
  MS_ASSERT_EQ(MS_TYPE_STRING, s->header.tag);
  MS_ASSERT_TRUE(s->header.type == &heap.types[MS_TYPE_STRING]);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, fromCStrMatchesNew) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* a = msStringFromCStr(&heap, "mslang");
  struct MsString* b = msStringNew(&heap, "mslang", 6);
  MS_ASSERT_TRUE(a == b);                        // same content, same interned pointer
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, emptyStringInterned) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* a = msStringNew(&heap, NULL, 0);   // NULL data + 0 len == empty
  struct MsString* b = msStringFromCStr(&heap, "");
  MS_ASSERT_TRUE(a == b);
  MS_ASSERT_EQ(0, (long long)msStringLen(a));
  MS_ASSERT_EQ('\0', msStringData(a)[0]);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, fnv1aGoldenVectors) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* hello = msStringFromCStr(&heap, "hello");
  struct MsString* empty = msStringFromCStr(&heap, "");
  MS_ASSERT_EQ(0x4f9f2cab, (long long)msStringHash(hello));   // FNV-1a-32("hello")
  MS_ASSERT_EQ(2166136261, (long long)msStringHash(empty));   // offset basis
  struct MsString* hello2 = msStringNew(&heap, "hello", 5);
  MS_ASSERT_EQ((long long)msStringHash(hello), (long long)msStringHash(hello2));
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, shortStringsInterned) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* a = msStringNew(&heap, "short", 5);
  struct MsString* b = msStringNew(&heap, "short", 5);
  MS_ASSERT_TRUE(a == b);
  // Multibyte UTF-8 content ("e" + combining sequences are fine raw bytes here).
  struct MsString* u1 = msStringNew(&heap, "\xc3\xa9\xc3\xa8", 4);
  struct MsString* u2 = msStringNew(&heap, "\xc3\xa9\xc3\xa8", 4);
  MS_ASSERT_TRUE(u1 == u2);
  MS_ASSERT_EQ(4, (long long)msStringLen(u1));
  // Embedded NUL is content, addressed by length.
  struct MsString* n1 = msStringNew(&heap, "a\0b", 3);
  struct MsString* n2 = msStringNew(&heap, "a\0b", 3);
  MS_ASSERT_TRUE(n1 == n2);
  MS_ASSERT_EQ(3, (long long)msStringLen(n1));
  MS_ASSERT_EQ('\0', msStringData(n1)[1]);
  MS_ASSERT_TRUE(!msStringEquals(n1, msStringFromCStr(&heap, "a")));
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, longStringsNotInterned) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  char buf33[34];
  memset(buf33, 'x', 33);
  buf33[33] = '\0';
  struct MsString* a = msStringNew(&heap, buf33, 33);
  struct MsString* b = msStringNew(&heap, buf33, 33);
  MS_ASSERT_TRUE(a != b);                        // 33 > 32: fresh allocation each time
  MS_ASSERT_TRUE(msStringEquals(a, b));
  MS_ASSERT_EQ(2, (long long)heap.objectCount);
  MS_ASSERT_EQ(0, (long long)heap.internLen);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, shortMaxBoundary) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  char buf32[33];
  memset(buf32, 'y', 32);
  buf32[32] = '\0';
  struct MsString* a = msStringNew(&heap, buf32, 32);
  struct MsString* b = msStringNew(&heap, buf32, 32);
  MS_ASSERT_TRUE(a == b);                        // exactly 32: still interned
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, prefixNotEqual) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* ab = msStringFromCStr(&heap, "ab");
  struct MsString* abc = msStringFromCStr(&heap, "abc");
  MS_ASSERT_TRUE(!msStringEquals(ab, abc));
  MS_ASSERT_TRUE(!msStringEquals(abc, ab));
  MS_ASSERT_TRUE(msStringEquals(ab, ab));
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, internGrowthKeepsHits) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  struct MsString* ptrs[200];
  char buf[16];
  for (int i = 0; i < 200; ++i) {
    snprintf(buf, sizeof(buf), "k%d", i);
    ptrs[i] = msStringFromCStr(&heap, buf);
    MS_ASSERT_TRUE(ptrs[i] != NULL);
  }
  // Load factor 0.75 with doubling: 64 -> 128 -> 256 -> 512 for 200 entries.
  MS_ASSERT_EQ(200, (long long)heap.internLen);
  MS_ASSERT_EQ(512, (long long)heap.internCap);
  for (int i = 0; i < 200; ++i) {
    snprintf(buf, sizeof(buf), "k%d", i);
    MS_ASSERT_TRUE(msStringFromCStr(&heap, buf) == ptrs[i]);   // still hits after rehash
  }
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, newOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  msMemSetFailAfter(0);
  struct MsString* s = msStringFromCStr(&heap, "oom-target");
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(s == NULL);
  MS_ASSERT_EQ(0, (long long)heap.internLen);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

MS_TEST(string, internGrowOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  msTestHeapBootstrap(&heap);
  char buf[16];
  for (int i = 0; i < 48; ++i) {                 // fill to the 0.75 load edge of 64 buckets
    snprintf(buf, sizeof(buf), "s%d", i);
    MS_ASSERT_TRUE(msStringFromCStr(&heap, buf) != NULL);
  }
  MS_ASSERT_EQ(64, (long long)heap.internCap);
  msMemSetFailAfter(0);                          // the rehash bucket allocation fails
  struct MsString* s = msStringFromCStr(&heap, "s48");
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(s == NULL);
  MS_ASSERT_EQ(48, (long long)heap.internLen);
  MS_ASSERT_EQ(64, (long long)heap.internCap);
  msHeapDestroy(&heap);
  msTestAssertNoLeaks();
}

static const MsTestCase msTestCases[] = {
  {"string.newRoundtrip", teststringnewRoundtrip},
  {"string.fromCStrMatchesNew", teststringfromCStrMatchesNew},
  {"string.emptyStringInterned", teststringemptyStringInterned},
  {"string.fnv1aGoldenVectors", teststringfnv1aGoldenVectors},
  {"string.shortStringsInterned", teststringshortStringsInterned},
  {"string.longStringsNotInterned", teststringlongStringsNotInterned},
  {"string.shortMaxBoundary", teststringshortMaxBoundary},
  {"string.prefixNotEqual", teststringprefixNotEqual},
  {"string.internGrowthKeepsHits", teststringinternGrowthKeepsHits},
  {"string.newOomReturnsNull", teststringnewOomReturnsNull},
  {"string.internGrowOomReturnsNull", teststringinternGrowOomReturnsNull},
};

MS_TEST_MAIN(msTestCases)
```

- [ ] **Step 3: CMake 接线并确认失败**

`CMakeLists.txt`：库源列表加 `src/object/ms_string.c`（紧随 `src/object/ms_object.c`），测试列表 `mslang_add_c_test(test_object)` 后加 `mslang_add_c_test(test_string)`。

```bash
cmake --build build
```

预期：链接失败，`msStringNew` / `msStringFromCStr` / `msStringEquals` 未定义（LNK2019）。

- [ ] **Step 4: 写 `src/object/ms_string.c`**

```c
#include "object/ms_string.h"

#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

// FNV-1a, 32-bit: offset basis 2166136261, prime 16777619.
static uint32_t msStringFnv1a(const char* data, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= (uint8_t)data[i];
    hash *= 16777619u;
  }
  return hash;
}

// Open-addressing intern table: linear probing over the cached hash, load
// factor 0.75 with doubling rehash. Buckets hold MsString*; NULL means empty.
static struct MsString* msStringInternFind(const struct MsHeap* heap, const char* data,
                                           size_t len, uint32_t hash) {
  MS_ASSERT(heap->internSlots != NULL);
  MS_ASSERT(heap->internCap > 0);
  size_t mask = heap->internCap - 1;
  size_t idx = hash & mask;
  while (heap->internSlots[idx] != NULL) {
    struct MsString* s = heap->internSlots[idx];
    if (s->hash == hash && s->byteLen == len
        && (len == 0 || memcmp(s->data, data, len) == 0)) {
      return s;
    }
    idx = (idx + 1) & mask;
  }
  return NULL;
}

// Inserts an already-allocated string; caller guarantees a free slot exists.
static void msStringInternInsert(struct MsHeap* heap, struct MsString* s) {
  size_t mask = heap->internCap - 1;
  size_t idx = s->hash & mask;
  while (heap->internSlots[idx] != NULL) {
    idx = (idx + 1) & mask;
  }
  heap->internSlots[idx] = s;
  ++heap->internLen;
}

static bool msStringInternGrow(struct MsHeap* heap) {
  size_t newCap = heap->internCap * 2;
  struct MsString** newSlots = (struct MsString**)msAlloc(newCap * sizeof(*newSlots));
  if (newSlots == NULL) {
    return false;
  }
  memset(newSlots, 0, newCap * sizeof(*newSlots));
  for (size_t i = 0; i < heap->internCap; ++i) {
    struct MsString* s = heap->internSlots[i];
    if (s == NULL) {
      continue;
    }
    size_t idx = s->hash & (newCap - 1);
    while (newSlots[idx] != NULL) {
      idx = (idx + 1) & (newCap - 1);
    }
    newSlots[idx] = s;
  }
  msFree(heap->internSlots);
  heap->internSlots = newSlots;
  heap->internCap = newCap;
  return true;
}

static struct MsString* msStringAlloc(struct MsHeap* heap, const char* data, size_t len,
                                      uint32_t hash) {
  struct MsString* s = (struct MsString*)msObjectAlloc(heap, MS_TYPE_STRING,
                                                       sizeof(struct MsString) + len + 1);
  if (s == NULL) {
    return NULL;
  }
  s->hash = hash;
  s->byteLen = len;
  if (len > 0) {
    memcpy(s->data, data, len);
  }
  s->data[len] = '\0';
  return s;
}

struct MsString* msStringNew(struct MsHeap* heap, const char* data, size_t len) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(data != NULL || len == 0);
  uint32_t hash = msStringFnv1a(data, len);
  if (len > MS_STRING_SHORT_MAX_LEN) {
    return msStringAlloc(heap, data, len, hash);
  }
  // Lookup hashes the input bytes in place: no allocate-then-discard on a hit.
  struct MsString* found = msStringInternFind(heap, data, len, hash);
  if (found != NULL) {
    return found;
  }
  if ((heap->internLen + 1) * 4 > heap->internCap * 3 && !msStringInternGrow(heap)) {
    return NULL;
  }
  struct MsString* s = msStringAlloc(heap, data, len, hash);
  if (s == NULL) {
    return NULL;
  }
  // Interned strings join the all-objects list like any other object; their
  // survival under GC (table as root-set member) is task 17's call.
  msStringInternInsert(heap, s);
  return s;
}

struct MsString* msStringFromCStr(struct MsHeap* heap, const char* cstr) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(cstr != NULL);
  return msStringNew(heap, cstr, strlen(cstr));
}

bool msStringEquals(const struct MsString* a, const struct MsString* b) {
  MS_ASSERT(a != NULL);
  MS_ASSERT(b != NULL);
  if (a == b) {
    return true;
  }
  if (a->byteLen != b->byteLen) {
    return false;
  }
  return a->byteLen == 0 || memcmp(a->data, b->data, a->byteLen) == 0;
}
```

注意 `msStringFnv1a(data, len)` 当 `data == NULL && len == 0` 时循环体不执行，安全。

- [ ] **Step 5: 构建并运行测试**

```bash
cmake --build build && ctest --test-dir build -R test_string --output-on-failure
```

预期：`11 of 11 tests passed`。全量 ctest 10/10 绿。

- [ ] **Step 6: 提交**

```bash
git add src/object/ms_string.h src/object/ms_string.c tests/c/test_string.c CMakeLists.txt
git commit -m "✨ feat(object): add MsString with short-string interning"
```

---

### Task 4: `msHeapInit` — 内建类型对象表、`msHeapType`、单例与 int 缓存初始化

**Files:**
- Modify: `src/object/ms_object.c`
- Modify: `tests/c/test_object.c`

初始化顺序（规格定死）：清零 → 驻留表桶数组（容量 64）→ 20 个类型对象（头部自指，名称经驻留表创建）→ nil/bool 单例与 int 缓存。类型名创建失败调用 `msHeapDestroy` 回滚并返回 `MS_ERROR_OOM`（`*heap` 因 destroy 的 memset 而归零，满足「OOM leaves *heap zeroed」）。

- [ ] **Step 1: 追加失败测试**

`test_object.c` include 区追加 `#include "object/ms_string.h"`（访问 `MsString` 字段）。测试用例：

```c
MS_TEST(object, heapInitDestroyNoLeaks) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
  MS_ASSERT_TRUE(heap.allObjects == NULL);       // destroy re-zeroes
  MS_ASSERT_TRUE(heap.internSlots == NULL);
}

MS_TEST(object, heapInitOomLeavesHeapZeroed) {
  // Failure on the intern bucket allocation.
  msMemResetStats();
  msMemSetFailAfter(0);
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_ERROR_OOM, msHeapInit(&heap));
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_TRUE(heap.internSlots == NULL);
  MS_ASSERT_EQ(0, (long long)heap.objectCount);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
  // Failure on the first type-name string: buckets freed by rollback.
  msMemResetStats();
  msMemSetFailAfter(1);
  MS_ASSERT_EQ(MS_ERROR_OOM, msHeapInit(&heap));
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(heap.allObjects == NULL);
  MS_ASSERT_TRUE(heap.internSlots == NULL);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, typeObjectsCompleteAndSelfReferential) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    struct MsType* t = msHeapType(&heap, (MsTypeTag)i);
    MS_ASSERT_TRUE(t == &heap.types[i]);
    MS_ASSERT_EQ(MS_TYPE_CLASS, t->header.tag);
    MS_ASSERT_TRUE(t->header.type == &heap.types[MS_TYPE_CLASS]);
    MS_ASSERT_EQ(MS_MARK_BLACK, t->header.markColor);
    MS_ASSERT_TRUE(t->header.gcNext == NULL);    // not on the all-objects list
    MS_ASSERT_TRUE(t->name != NULL);
    MS_ASSERT_EQ(MS_TYPE_STRING, t->name->header.tag);
    for (int j = i + 1; j < (int)MS_TYPE_COUNT; ++j) {
      MS_ASSERT_TRUE(t != &heap.types[j]);       // 20 distinct type objects
    }
  }
  // Name spot checks per 02-types section 1.
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_NIL].name), "NilType") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_BOOL].name), "bool") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_INT].name), "int") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_FLOAT].name), "float") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_STRING].name), "str") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_BYTES].name), "bytes") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_LIST].name), "list") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_TUPLE].name), "tuple") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_DICT].name), "dict") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_SET].name), "set") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_FUNCTION].name), "function") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_CLASS].name), "type") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_INSTANCE].name), "instance") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_MODULE].name), "module") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_CHANNEL].name), "chan") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_COROUTINE].name), "coroutine") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_ITERATOR].name), "iterator") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_BOUND_METHOD].name), "bound method") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_C_FUNCTION].name), "cfunction") == 0);
  MS_ASSERT_TRUE(strcmp(msStringData(heap.types[MS_TYPE_C_TYPE].name), "ctype") == 0);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, permanentObjectsNotLinked) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  // Exactly the 20 interned type names are linked heap objects after init.
  MS_ASSERT_EQ(20, (long long)heap.objectCount);
  MS_ASSERT_EQ(20, (long long)heap.internLen);
  // Singletons and cache entries: black, unlinked, do not change objectCount.
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.nilObject.header.markColor);
  MS_ASSERT_TRUE(heap.nilObject.header.gcNext == NULL);
  MS_ASSERT_EQ(MS_TYPE_NIL, heap.nilObject.header.tag);
  MS_ASSERT_TRUE(heap.nilObject.header.type == &heap.types[MS_TYPE_NIL]);
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.trueObject.header.markColor);
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.falseObject.header.markColor);
  MS_ASSERT_TRUE(heap.trueObject.header.gcNext == NULL);
  MS_ASSERT_TRUE(heap.trueObject.value);
  MS_ASSERT_TRUE(!heap.falseObject.value);
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.intCache[0].header.markColor);
  MS_ASSERT_EQ(MS_MARK_BLACK, heap.intCache[MS_INT_CACHE_SIZE - 1].header.markColor);
  MS_ASSERT_EQ(-256, (long long)heap.intCache[0].value);
  MS_ASSERT_EQ(4095, (long long)heap.intCache[MS_INT_CACHE_SIZE - 1].value);
  MS_ASSERT_TRUE(heap.intCache[0].header.gcNext == NULL);
  MS_ASSERT_TRUE(heap.intCache[0].header.type == &heap.types[MS_TYPE_INT]);
  MS_ASSERT_EQ(20, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}
```

注册 4 个用例进 `msTestCases`。

- [ ] **Step 2: 运行确认失败**

```bash
cmake --build build
```

预期：链接失败，`msHeapInit` / `msHeapType` 未定义（LNK2019）。

- [ ] **Step 3: 实现**

`ms_object.c` include 区追加 `#include "object/ms_string.h"`。文件尾追加：

```c
struct MsType* msHeapType(struct MsHeap* heap, MsTypeTag tag) {
  MS_ASSERT(heap != NULL);
  MS_ASSERT(tag >= 0 && tag < MS_TYPE_COUNT);
  return &heap->types[tag];
}

MsResult msHeapInit(struct MsHeap* heap) {
  MS_ASSERT(heap != NULL);
  memset(heap, 0, sizeof(*heap));
  heap->internCap = MS_STRING_INTERN_INITIAL_CAP;
  heap->internSlots =
      (struct MsString**)msAlloc(heap->internCap * sizeof(*heap->internSlots));
  if (heap->internSlots == NULL) {
    return MS_ERROR_OOM;
  }
  memset(heap->internSlots, 0, heap->internCap * sizeof(*heap->internSlots));
  // Type objects: permanent (black), unlinked, self-referential type pointer.
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    heap->types[i].header.type = &heap->types[MS_TYPE_CLASS];
    heap->types[i].header.tag = MS_TYPE_CLASS;
    heap->types[i].header.markColor = MS_MARK_BLACK;
    heap->types[i].header.gcNext = NULL;
    heap->types[i].name = NULL;
  }
  // Type names come from the intern table; on OOM roll back in reverse
  // acquisition order via msHeapDestroy (which also re-zeroes *heap).
  for (int i = 0; i < (int)MS_TYPE_COUNT; ++i) {
    heap->types[i].name = msStringFromCStr(heap, msTypeScriptNames[i]);
    if (heap->types[i].name == NULL) {
      msHeapDestroy(heap);
      return MS_ERROR_OOM;
    }
  }
  heap->nilObject.header.type = &heap->types[MS_TYPE_NIL];
  heap->nilObject.header.tag = MS_TYPE_NIL;
  heap->nilObject.header.markColor = MS_MARK_BLACK;
  heap->nilObject.header.gcNext = NULL;
  heap->trueObject.header.type = &heap->types[MS_TYPE_BOOL];
  heap->trueObject.header.tag = MS_TYPE_BOOL;
  heap->trueObject.header.markColor = MS_MARK_BLACK;
  heap->trueObject.header.gcNext = NULL;
  heap->trueObject.value = true;
  heap->falseObject.header.type = &heap->types[MS_TYPE_BOOL];
  heap->falseObject.header.tag = MS_TYPE_BOOL;
  heap->falseObject.header.markColor = MS_MARK_BLACK;
  heap->falseObject.header.gcNext = NULL;
  heap->falseObject.value = false;
  for (size_t i = 0; i < MS_INT_CACHE_SIZE; ++i) {
    heap->intCache[i].header.type = &heap->types[MS_TYPE_INT];
    heap->intCache[i].header.tag = MS_TYPE_INT;
    heap->intCache[i].header.markColor = MS_MARK_BLACK;
    heap->intCache[i].header.gcNext = NULL;
    heap->intCache[i].value = (int64_t)MS_INT_CACHE_MIN + (int64_t)i;
  }
  return MS_OK;
}
```

- [ ] **Step 4: 构建并运行测试**

```bash
cmake --build build && ctest --test-dir build -R test_object --output-on-failure
```

预期：`11 of 11 tests passed`。全量 ctest 全绿。

- [ ] **Step 5: 提交**

```bash
git add src/object/ms_object.c tests/c/test_object.c
git commit -m "✨ feat(object): add heap init with builtin type objects"
```

---

### Task 5: 标量构造与访问 — 单例访问器、`msIntNew`/`msFloatNew`、访问器

**Files:**
- Modify: `src/object/ms_object.c`
- Modify: `tests/c/test_object.c`

- [ ] **Step 1: 追加失败测试**

include 区追加 `#include <math.h>`（`NAN`/`INFINITY`）与 `#include <stdint.h>`（位级比较，已有则不动）。测试辅助与用例：

```c
static uint64_t msTestDoubleBits(double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

MS_TEST(object, singletonAccessorsIdentity) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  MS_ASSERT_TRUE(msHeapNil(&heap) == msHeapNil(&heap));
  MS_ASSERT_TRUE(msHeapNil(&heap) == &heap.nilObject);
  MS_ASSERT_TRUE(msHeapBool(&heap, true) == msHeapBool(&heap, true));
  MS_ASSERT_TRUE(msHeapBool(&heap, false) == msHeapBool(&heap, false));
  MS_ASSERT_TRUE(msHeapBool(&heap, true) != msHeapBool(&heap, false));
  MS_ASSERT_TRUE(msHeapBool(&heap, true) != msHeapNil(&heap));
  size_t countBefore = heap.objectCount;
  msHeapNil(&heap);
  msHeapBool(&heap, true);
  MS_ASSERT_EQ((long long)countBefore, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, intCacheIdentityAtBoundaries) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  int64_t cached[] = {-256, 0, 4095};
  for (size_t i = 0; i < sizeof(cached) / sizeof(cached[0]); ++i) {
    struct MsInt* a = msIntNew(&heap, cached[i]);
    struct MsInt* b = msIntNew(&heap, cached[i]);
    MS_ASSERT_TRUE(a == b);                      // resident: same pointer
    MS_ASSERT_EQ(cached[i], (long long)a->value);
    MS_ASSERT_EQ(MS_TYPE_INT, a->header.tag);
    MS_ASSERT_EQ(MS_MARK_BLACK, a->header.markColor);
  }
  MS_ASSERT_EQ(20, (long long)heap.objectCount); // cache hits never allocate
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, intUncachedAllocates) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  int64_t uncached[] = {-257, 4096, INT64_MIN, INT64_MAX};
  size_t countBefore = heap.objectCount;
  for (size_t i = 0; i < sizeof(uncached) / sizeof(uncached[0]); ++i) {
    struct MsInt* a = msIntNew(&heap, uncached[i]);
    struct MsInt* b = msIntNew(&heap, uncached[i]);
    MS_ASSERT_TRUE(a != NULL && b != NULL);
    MS_ASSERT_TRUE(a != b);                      // fresh allocation each time
    MS_ASSERT_EQ(uncached[i], (long long)a->value);
    MS_ASSERT_EQ(MS_MARK_WHITE, a->header.markColor);
  }
  MS_ASSERT_EQ((long long)(countBefore + 8), (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, intAccessorsRoundtrip) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsInt* cached = msIntNew(&heap, 42);
  struct MsInt* fresh = msIntNew(&heap, 5000);
  MS_ASSERT_TRUE(fresh != NULL);
  MS_ASSERT_TRUE(msIntIsWord((struct MsObject*)cached));
  MS_ASSERT_TRUE(msIntIsWord((struct MsObject*)fresh));
  MS_ASSERT_EQ(42, (long long)msIntWordValue((struct MsObject*)cached));
  MS_ASSERT_EQ(5000, (long long)msIntWordValue((struct MsObject*)fresh));
  MS_ASSERT_EQ(MS_TYPE_INT, msObjectTag((struct MsObject*)cached));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, intNewOomReturnsNull) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  size_t countBefore = heap.objectCount;
  msMemSetFailAfter(0);
  struct MsInt* obj = msIntNew(&heap, 5000);
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(obj == NULL);
  MS_ASSERT_EQ((long long)countBefore, (long long)heap.objectCount);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, floatRoundtripPreservesBits) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  double values[] = {0.0, -0.0, 3.14, -2.5e300, INFINITY, -INFINITY, NAN};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    struct MsFloat* f = msFloatNew(&heap, values[i]);
    MS_ASSERT_TRUE(f != NULL);
    MS_ASSERT_EQ(MS_TYPE_FLOAT, f->header.tag);
    MS_ASSERT_TRUE(f->header.type == &heap.types[MS_TYPE_FLOAT]);
    MS_ASSERT_EQ((long long)msTestDoubleBits(values[i]),
                 (long long)msTestDoubleBits(msFloatValue((struct MsObject*)f)));
  }
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, boolValueAccessor) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  MS_ASSERT_TRUE(msBoolValue(msHeapBool(&heap, true)));
  MS_ASSERT_TRUE(!msBoolValue(msHeapBool(&heap, false)));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}
```

注册 7 个用例进 `msTestCases`。

- [ ] **Step 2: 运行确认失败**

```bash
cmake --build build
```

预期：链接失败，`msHeapNil` / `msHeapBool` / `msIntNew` / `msFloatNew` / `msIntIsWord` / `msIntWordValue` / `msFloatValue` / `msBoolValue` 未定义（LNK2019）。

- [ ] **Step 3: 实现**

`ms_object.c` 文件尾追加：

```c
struct MsObject* msHeapNil(struct MsHeap* heap) {
  MS_ASSERT(heap != NULL);
  return &heap->nilObject;
}

struct MsObject* msHeapBool(struct MsHeap* heap, bool value) {
  MS_ASSERT(heap != NULL);
  return value ? (struct MsObject*)&heap->trueObject : (struct MsObject*)&heap->falseObject;
}

struct MsInt* msIntNew(struct MsHeap* heap, int64_t value) {
  MS_ASSERT(heap != NULL);
  // Residency is an implementation detail (02-types section 6); the cache-hit
  // decision lives here and nowhere else.
  if (value >= MS_INT_CACHE_MIN && value <= MS_INT_CACHE_MAX) {
    return &heap->intCache[value - MS_INT_CACHE_MIN];
  }
  struct MsInt* obj = (struct MsInt*)msObjectAlloc(heap, MS_TYPE_INT, sizeof(struct MsInt));
  if (obj == NULL) {
    return NULL;
  }
  obj->value = value;
  return obj;
}

struct MsFloat* msFloatNew(struct MsHeap* heap, double value) {
  MS_ASSERT(heap != NULL);
  struct MsFloat* obj =
      (struct MsFloat*)msObjectAlloc(heap, MS_TYPE_FLOAT, sizeof(struct MsFloat));
  if (obj == NULL) {
    return NULL;
  }
  obj->value = value;
  return obj;
}

bool msIntIsWord(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL && obj->header.tag == MS_TYPE_INT);
  MS_UNUSED(obj);                // Release (NDEBUG): the assert compiles out
  return true;                   // v0.1 ints are always machine-word; task 31 changes this
}

int64_t msIntWordValue(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL && obj->header.tag == MS_TYPE_INT);
  MS_ASSERT(msIntIsWord(obj));
  return ((const struct MsInt*)obj)->value;
}

double msFloatValue(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL && obj->header.tag == MS_TYPE_FLOAT);
  return ((const struct MsFloat*)obj)->value;
}

bool msBoolValue(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL && obj->header.tag == MS_TYPE_BOOL);
  return ((const struct MsBool*)obj)->value;
}
```

- [ ] **Step 4: 构建并运行测试（含 Release 验证 NDEBUG 无警告）**

```bash
cmake --build build && ctest --test-dir build -R test_object --output-on-failure
```

预期：`18 of 18 tests passed`。随后验证 Release 配置零警告（`msIntIsWord` 的 `MS_UNUSED` 是关键）：

```bash
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
ctest --test-dir build-rel --output-on-failure
```

预期：零警告构建、全绿。**验证后删除 `build-rel/`**（AGENTS.md：构建产物只在 `build/`；`build-rel` 是临时验证目录，删除以保持仓库干净）。注意勿改动用户主仓库已有的 `build/` 配置——worktree 内的 `build/` 是自己配置的，保持默认（Debug）即可。

- [ ] **Step 5: 提交**

```bash
git add src/object/ms_object.c tests/c/test_object.c
git commit -m "✨ feat(object): add scalar constructors and accessors"
```

---

### Task 6: `msObjectValueEquals` / `msObjectHash` / `msObjectIsTruthy`

**Files:**
- Modify: `src/object/ms_object.c`
- Modify: `tests/c/test_object.c`

语义（规格定死）：指针相同即真（驻留快路径，**先于** NaN 判定——同一 NaN 对象与自身相等）；`nil` 同值；`bool` 比值；`int`/`float` 跨标签按 double 数值比较；`str` 按内容；其余标签按指针（容器/实例值语义属任务 16/25）。哈希一致性：整数值的 float 按 int 路径求哈希，保证 `hash(42) == hash(42.0)`。

- [ ] **Step 1: 追加失败测试**

```c
MS_TEST(object, valueEqualsScalars) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* nilA = msHeapNil(&heap);
  MS_ASSERT_TRUE(msObjectValueEquals(nilA, nilA));
  MS_ASSERT_TRUE(msObjectValueEquals(msHeapBool(&heap, true), msHeapBool(&heap, true)));
  MS_ASSERT_TRUE(!msObjectValueEquals(msHeapBool(&heap, true), msHeapBool(&heap, false)));
  struct MsInt* i42a = msIntNew(&heap, 5000);    // uncached: distinct objects, equal value
  struct MsInt* i42b = msIntNew(&heap, 5000);
  struct MsInt* i43 = msIntNew(&heap, 5001);
  struct MsFloat* f42 = msFloatNew(&heap, 5000.0);
  MS_ASSERT_TRUE(i42a != i42b);
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)i42a, (struct MsObject*)i42b));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)i42a, (struct MsObject*)i43));
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)i42a, (struct MsObject*)f42));
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)f42, (struct MsObject*)i42a));
  struct MsFloat* fHalf = msFloatNew(&heap, 5000.5);
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)i42a, (struct MsObject*)fHalf));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, valueEqualsNaNAndIdentity) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsFloat* nanA = msFloatNew(&heap, NAN);
  struct MsFloat* nanB = msFloatNew(&heap, NAN);
  MS_ASSERT_TRUE(nanA != nanB);
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)nanA, (struct MsObject*)nanB));
  // Identity fast path runs first: an object always equals itself.
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)nanA, (struct MsObject*)nanA));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, valueEqualsStringAndCrossTag) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  char buf[41];
  memset(buf, 'z', 40);
  buf[40] = '\0';
  struct MsString* s1 = msStringNew(&heap, buf, 40);   // long: not interned
  struct MsString* s2 = msStringNew(&heap, buf, 40);
  MS_ASSERT_TRUE(s1 != s2);
  MS_ASSERT_TRUE(msObjectValueEquals((struct MsObject*)s1, (struct MsObject*)s2));
  struct MsString* other = msStringFromCStr(&heap, "short-str");
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)s1, (struct MsObject*)other));
  struct MsInt* i1 = msIntNew(&heap, 1);
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)i1, (struct MsObject*)other));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)other, (struct MsObject*)i1));
  MS_ASSERT_TRUE(!msObjectValueEquals(NULL, (struct MsObject*)i1));
  MS_ASSERT_TRUE(!msObjectValueEquals((struct MsObject*)i1, NULL));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, hashConsistentWithEquals) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsInt* i42 = msIntNew(&heap, 5000);
  struct MsFloat* f42 = msFloatNew(&heap, 5000.0);
  struct MsFloat* fNeg7 = msFloatNew(&heap, -7.0);
  struct MsInt* iNeg7 = msIntNew(&heap, -7);
  // Equal values must hash equal (dict keys, task 16).
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)i42),
               (long long)msObjectHash((struct MsObject*)f42));
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)iNeg7),
               (long long)msObjectHash((struct MsObject*)fNeg7));
  char buf[41];
  memset(buf, 'q', 40);
  buf[40] = '\0';
  struct MsString* s1 = msStringNew(&heap, buf, 40);
  struct MsString* s2 = msStringNew(&heap, buf, 40);
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)s1),
               (long long)msObjectHash((struct MsObject*)s2));
  MS_ASSERT_EQ((long long)msObjectHash(msHeapBool(&heap, true)),
               (long long)msObjectHash(msHeapBool(&heap, true)));
  MS_ASSERT_EQ((long long)msObjectHash(msHeapNil(&heap)),
               (long long)msObjectHash(msHeapNil(&heap)));
  // Non-integral floats hash by bits; determinism check only.
  struct MsFloat* fHalfA = msFloatNew(&heap, 0.5);
  struct MsFloat* fHalfB = msFloatNew(&heap, 0.5);
  MS_ASSERT_EQ((long long)msObjectHash((struct MsObject*)fHalfA),
               (long long)msObjectHash((struct MsObject*)fHalfB));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(object, truthinessTable) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  MS_ASSERT_TRUE(!msObjectIsTruthy(msHeapNil(&heap)));
  MS_ASSERT_TRUE(!msObjectIsTruthy(msHeapBool(&heap, false)));
  MS_ASSERT_TRUE(msObjectIsTruthy(msHeapBool(&heap, true)));
  MS_ASSERT_TRUE(!msObjectIsTruthy((struct MsObject*)msIntNew(&heap, 0)));
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)msIntNew(&heap, 1)));
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)msIntNew(&heap, -1)));
  struct MsFloat* fZero = msFloatNew(&heap, 0.0);
  struct MsFloat* fNegZero = msFloatNew(&heap, -0.0);
  struct MsFloat* fHalf = msFloatNew(&heap, 0.5);
  MS_ASSERT_TRUE(!msObjectIsTruthy((struct MsObject*)fZero));
  MS_ASSERT_TRUE(!msObjectIsTruthy((struct MsObject*)fNegZero));
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)fHalf));
  MS_ASSERT_TRUE(!msObjectIsTruthy((struct MsObject*)msStringFromCStr(&heap, "")));
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)msStringFromCStr(&heap, "x")));
  // Heap objects of other tags are truthy (type object as the specimen).
  MS_ASSERT_TRUE(msObjectIsTruthy((struct MsObject*)msHeapType(&heap, MS_TYPE_CLASS)));
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}
```

注册 5 个用例进 `msTestCases`。

- [ ] **Step 2: 运行确认失败**

```bash
cmake --build build
```

预期：链接失败，`msObjectValueEquals` / `msObjectHash` / `msObjectIsTruthy` 未定义（LNK2019）。

- [ ] **Step 3: 实现**

`ms_object.c` 文件尾追加：

```c
// splitmix64 finalizer: well-mixed 64 -> 32 bit reduction.
static uint32_t msHashMix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return (uint32_t)x;
}

bool msObjectValueEquals(const struct MsObject* a, const struct MsObject* b) {
  if (a == b) {
    return true;                 // identity fast path (interning residency)
  }
  if (a == NULL || b == NULL) {
    return false;
  }
  if (a->header.tag != b->header.tag) {
    // Cross-tag numeric compare: 42 == 42.0 (02-types section 3.2). NaN falls
    // out naturally: da == db is false when either side is NaN.
    if ((a->header.tag == MS_TYPE_INT || a->header.tag == MS_TYPE_FLOAT)
        && (b->header.tag == MS_TYPE_INT || b->header.tag == MS_TYPE_FLOAT)) {
      double da = a->header.tag == MS_TYPE_INT
                      ? (double)((const struct MsInt*)a)->value
                      : ((const struct MsFloat*)a)->value;
      double db = b->header.tag == MS_TYPE_INT
                      ? (double)((const struct MsInt*)b)->value
                      : ((const struct MsFloat*)b)->value;
      return da == db;
    }
    return false;
  }
  switch (a->header.tag) {
    case MS_TYPE_NIL:
      return true;
    case MS_TYPE_BOOL:
      return ((const struct MsBool*)a)->value == ((const struct MsBool*)b)->value;
    case MS_TYPE_INT:
      return ((const struct MsInt*)a)->value == ((const struct MsInt*)b)->value;
    case MS_TYPE_FLOAT:
      return ((const struct MsFloat*)a)->value == ((const struct MsFloat*)b)->value;
    case MS_TYPE_STRING:
      return msStringEquals((const struct MsString*)a, (const struct MsString*)b);
    default:
      // Value semantics for containers/instances land in tasks 16/25 inside
      // this function; callers see no change.
      return false;
  }
}

uint32_t msObjectHash(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL);
  switch (obj->header.tag) {
    case MS_TYPE_NIL:
      return 0x27d4eb2fu;        // fixed constant; any stable value works
    case MS_TYPE_BOOL:
      return ((const struct MsBool*)obj)->value ? 0x9e3779b9u : 0x85ebca6bu;
    case MS_TYPE_INT:
      return msHashMix64((uint64_t)((const struct MsInt*)obj)->value);
    case MS_TYPE_FLOAT: {
      double v = ((const struct MsFloat*)obj)->value;
      // Integral doubles hash as ints so hash(42) == hash(42.0); required for
      // numeric dict keys (task 16). Bounds guard the int64 conversion.
      if (v >= -9223372036854775808.0 && v < 9223372036854775808.0) {
        int64_t iv = (int64_t)v;
        if ((double)iv == v) {
          return msHashMix64((uint64_t)iv);
        }
      }
      uint64_t bits;
      memcpy(&bits, &v, sizeof(bits));
      return msHashMix64(bits);
    }
    case MS_TYPE_STRING:
      return ((const struct MsString*)obj)->hash;
    default:
      // Identity hashing for instances/containers is a later task.
      MS_ASSERT(!"msObjectHash: tag not hashable in this task");
      return 0;
  }
}

bool msObjectIsTruthy(const struct MsObject* obj) {
  MS_ASSERT(obj != NULL);
  switch (obj->header.tag) {
    case MS_TYPE_NIL:
      return false;
    case MS_TYPE_BOOL:
      return ((const struct MsBool*)obj)->value;
    case MS_TYPE_INT:
      return ((const struct MsInt*)obj)->value != 0;
    case MS_TYPE_FLOAT:
      return ((const struct MsFloat*)obj)->value != 0.0;
    case MS_TYPE_STRING:
      return ((const struct MsString*)obj)->byteLen != 0;
    default:
      // Container emptiness joins in task 16; other heap objects are truthy.
      return true;
  }
}
```

- [ ] **Step 4: 构建并运行测试**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

预期：全绿，`test_object` 23/23、`test_string` 11/11、其余既有测试不受影响。

- [ ] **Step 5: 提交**

```bash
git add src/object/ms_object.c tests/c/test_object.c
git commit -m "✨ feat(object): add value equality, hash and truthiness"
```

---

### Task 7: 任务 05 衔接 — 常量池值去重 + 文档状态翻转 + 全量验证

**Files:**
- Modify: `src/vm/ms_proto.c`（`msProtoAddConst` 去重逻辑）
- Modify: `src/vm/ms_proto.h`（陈旧注释）
- Modify: `tests/c/test_proto.c`（假指针替身换成真实堆对象）
- Modify: `docs/tasks/README.md`、`docs/tasks/06-object-model.md`（状态翻转）

任务 05 的 `msProtoAddConst` 目前是指针去重（`src/vm/ms_proto.c:101` 注释 "Pointer-identity dedup until task 06"）。升级为值去重后，`tests/c/test_proto.c` 里的 `msTestFakeConst*` 假指针替身会被 `msObjectValueEquals` 解引用（垃圾内存，UB），必须整体换成真实 `MsHeap` + `MsInt` 对象。

- [ ] **Step 1: 修改 `src/vm/ms_proto.c`**

include 区追加 `#include "object/ms_object.h"`。`msProtoAddConst` 中的去重循环改为：

```c
  // Value dedup per the constant-pool contract (msObjectValueEquals, task 06).
  for (int i = 0; i < b->proto.constsLen; ++i) {
    if (msObjectValueEquals(b->proto.consts[i], value)) {
      return i;
    }
  }
```

删除 `// Pointer-identity dedup until task 06 lands msObjectValueEquals.` 注释。

- [ ] **Step 2: 修改 `src/vm/ms_proto.h` 陈旧注释**

- 第 12-13 行 `// Incomplete until task 06 (object model); the pool and name fields are pointers only, so forward declarations suffice here.` 改为：
  ```c
  // struct MsObject / struct MsString live in the object model (src/object/);
  // the pool and name fields are pointers only, so forward declarations suffice.
  ```
- `msProtoAddConst` 声明上方注释中 `(Pointer-identity dedup ...)` 及 `Value dedup arrives with task 06 (msObjectValueEquals).` 表述改为 `(Value dedup via msObjectValueEquals: equal constants share one entry).`——以文件实际措辞为准做最小编辑，目标是去掉「等待任务 06」的时态。

- [ ] **Step 3: 迁移 `tests/c/test_proto.c` 的假指针替身**

删除 `msTestFakeConstA/B/C` 与 `msTestFakeConstPool` 及其注释。对每个调用 `msProtoAddConst` 的测试：函数开头建堆、结尾销毁并断言无泄漏，常量用**非缓存 int**（值 ≥ 4096，保证每次 `msIntNew` 返回不同对象）构造。模式：

```c
#include "object/ms_object.h"   // 追加到 include 区

// 每个涉及常量池的测试内部：
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* kA = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsObject* kB = (struct MsObject*)msIntNew(&heap, 5001);
  ...
  msProtoBuilderDestroy(&b);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
```

值去重的关键行为变化必须体现在测试里：

- 原「同指针去重」语义由值相等覆盖：`msIntNew(&heap, 5000)` 两次产生的**不同指针**在 `msProtoAddConst` 下返回**同一索引**（这是任务 05 预留衔接点的兑现，必须有一条显式测试断言）。
- 不同值（5000 vs 5001）进不同槽位。
- 常量池上限（`MS_PROTO_CONSTS_MAX`）行为测试原用假指针数组填充，改用循环 `msIntNew(&heap, 5000 + i)`（每个值唯一，i 从 0 起），注意保持堆活到 builder 销毁之后。
- `msProtoBuilderInit/Destroy`、emit、patch、try-block、lines、dump 等与常量池无关的测试不动。

- [ ] **Step 4: 构建并运行全量测试**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

预期：10/10 全绿（含改造后的 test_proto）。

- [ ] **Step 5: 提交衔接改动**

```bash
git add src/vm/ms_proto.c src/vm/ms_proto.h tests/c/test_proto.c
git commit -m "✨ feat(vm): dedup constant pool by value equality"
```

- [ ] **Step 6: 文档状态翻转**

- `docs/tasks/README.md`：任务 06 行 `| ⬜ | 06 |` 改为 `| ✅ | 06 |`。
- `docs/tasks/06-object-model.md`：头部表格状态 `⬜` 改为 `✅`。

- [ ] **Step 7: 双配置全量验证**

```bash
cmake --build build && ctest --test-dir build --output-on-failure        # Debug
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel && ctest --test-dir build-rel --output-on-failure # Release
rm -rf build-rel
```

预期：Debug 与 Release 均零警告构建（`/W4 /WX`）、ctest 10/10 全绿。

- [ ] **Step 8: 提交文档**

```bash
git add docs/tasks/README.md docs/tasks/06-object-model.md
git commit -m "📝 docs(tasks): mark task 06 object model complete"
```

---

## 验收对照（规格「验收标准」逐条 → 任务）

| 验收标准 | 落实处 |
|---|---|
| 四文件存在、guard 正确、头自包含、风格合规 | Task 1/3（每个测试文件首 include 即被测头） |
| `MsTypeTag` 20 标签 + 哨兵 + `_Static_assert`、头 24 字节 | Task 1（头内静态断言） |
| 分配收口 `msObjectAlloc`、无泄漏、永久对象不链入 | Task 2/4（每个测试结尾 `liveBlocks == 0`） |
| 小整数 -256..4095 驻留、范围外分配、int64 直存 | Task 5（边界身份测试） |
| MsString 布局、≤32 驻留、倍增 rehash 命中 | Task 3（边界/增长测试） |
| 20 个类型对象、名称对齐 02-types §1、`header.type` 恒非空 | Task 4 |
| `msObjectValueEquals` 签名与任务 05 一致、哈希一致性、NaN | Task 6 + Task 7（衔接兑现） |
| 测试覆盖「测试方案」全部清单 | Task 1-6 测试清单逐条对应 |
| 无 TBD/TODO、任务 08 接口假定一致 | Task 7 终审核对 |

## 自审记录（写计划后核对）

- **规格覆盖：** 规格「实现步骤」1-7 → 本计划 Task 1-7 一一对应；「测试方案」两个文件的清单项全部有对应 `MS_TEST`。
- **类型一致：** `msStringFromCStr`、`msObjectAlloc`、`msHeapDestroy` 在 Task 2/3/4 间签名一致；`MS_STRING_INTERN_INITIAL_CAP` 定义于 `ms_string.h`、被 `ms_object.c` 的 `msHeapInit` 与测试引导共用。
- **依赖顺序：** 头（1）→ 分配/销毁（2）→ 字符串（3，依赖 2）→ 堆初始化（4，依赖 3 的 `msStringFromCStr`）→ 标量（5，依赖 4 的单例/缓存初始化）→ 相等/哈希/真值（6，依赖 3/5）→ 衔接（7，依赖 6）。无环。
- **已知陷阱标注：** NDEBUG 下 `MS_ASSERT` 编译消失 → `msIntIsWord` 的 `MS_UNUSED`；`memcmp(NULL, x, 0)` 规避；跨标签 int/float 比较按 double（规格 §3.2 语义，精度取舍属规格既定）。
