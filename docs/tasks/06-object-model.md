# 06 对象模型基础

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [02 核心基础设施](02-core-infrastructure.md) |

## 任务目标

交付 mslang 的对象模型基础模块：

- `src/object/ms_object.{h,c}`：`MsTypeTag` 全量枚举（20 个类型标签 + `MS_TYPE_COUNT` 哨兵）、`struct MsObjectHeader` 与基座 `struct MsObject`、对象堆上下文 `struct MsHeap`（全对象链表、小整数驻留缓存、内建类型对象表、nil/bool 单例）、统一分配入口 `msObjectAlloc`、标量类型（nil/bool/int/float）的构造与访问、类型查询、值相等 / 哈希 / 真值判定。
- `src/object/ms_string.{h,c}`：`struct MsString`（长度 + 哈希缓存 + UTF-8 字节）、短字符串驻留表、字符串构造与比较。

本模块是一切后续运行时组件的地基：任务 05 的常量池以 `MsObject*` 为元素并依赖本模块的 `msObjectValueEquals` 做值去重；任务 08（VM）在 `MsState` 中内嵌 `MsHeap`；任务 16（容器）、任务 17（GC 标记-清除）、任务 31（大整数）都在本任务的头部布局与分配入口上扩展。本任务自身经 `tests/c/test_object.c` 与 `tests/c/test_string.c` 的 C 单元测试独立验证，不依赖 lexer/parser/compiler 的任何实现。

## 设计依据

- `docs/language/08-vm-internals.md`
  - §3 对象模型：一切值装箱为 `MsObject*`（首版不做 NaN-boxing/指针打包，列入性能路线图）；`MsObjectHeader{type, markColor, gcNext}`；`MsTypeTag` 枚举；小整数（-256..4095）与短字符串驻留；`int` 机器字内直存、溢出转大整数；`MsString` 长度 + 哈希缓存 + UTF-8 字节、码点索引 O(n)。
  - §5 GC：全对象链表（`gcNext`）、标记色、分配计数阈值——本任务只承载字段与链表维护，标记-清除算法属任务 17。
  - §7：NaN-boxing（任务 65）、字符串分层表示均列入性能路线图，首版不做。
- `docs/language/02-types.md`
  - §1 内建类型总表：17 种脚本可见类型的名称与可变性（`NilType`/`bool`/`int`/`float`/`str`/`bytes`/`list`/`tuple`/`dict`/`set`/`function`/`type`/`module`/`chan`/`coroutine`/`iterator` 等）。
  - §2 真值规则：`nil`、`false`、数值零、空字符串/字节串、空容器为假。
  - §3.1 与 `docs/language/11-project-layout.md` §5 路线图：v0.1 的 `int` 仅 int64，任意精度大整数属任务 31。
  - §6 相等与哈希：`==` 按值比较、`is` 按身份；驻留是实现细节，脚本不得依赖 `is` 判断值相等。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则（枚举允许 typedef、内部结构体不 typedef）、§5 错误处理（`MsResult` 或 `NULL` 报告失败）、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）、§8 断言（`MS_ASSERT`）。
- `docs/language/11-project-layout.md`：§1 `src/object/` 目录位置；§4 `tests/c/` 用自研 `ms_test.h` 做模块单元测试。
- 任务 05 字节码格式与 MsProto 已假定本模块提供 `bool msObjectValueEquals(const struct MsObject* a, const struct MsObject* b)`（见其「常量池约定」与「与对象模型的衔接」），本任务以该签名为准落地，保证两个任务文档互洽。
- 任务 02 核心基础设施（[02-core-infrastructure.md](02-core-infrastructure.md)，已成文）提供：`msAlloc/msRealloc/msFree` 与分配统计接口 `msMemGetStats`/`msMemResetStats`（泄漏断言用）、`MsResult`（`MS_OK`/`MS_ERROR_OOM`，唯一定义点为 `include/mslang/error.h`）、`MS_ASSERT`（`src/core/ms_common.h`）。本文接口名与其定名一致。`ms_test.h` 测试头按任务 02 文档的归属说明属任务 01 交付物。

对规范的四处显式处理（实现与评审时以此为据）：

1. **头部补充 `tag` 字段。** 规范的 `MsObjectHeader` 只有 `{type, markColor, gcNext}`，但 `MsTypeTag` 枚举必须落在某处才能支撑 O(1) 类型分派（08-vm-internals §4 的算术快路径要求廉价判定「双操作数均为机器字 int」）。64 位布局下 `{ptr, u8, ptr}` 有 7 字节对齐填充，插入 `uint8_t tag` 不增加头部长度（仍 24 字节）。本任务在头部加 `tag`，并保证 `tag == type` 所指类型对象的标签恒等。
2. **「21 个 MsTypeTag」的口径。** 规范枚举为 20 个类型标签（`MS_TYPE_NIL`..`MS_TYPE_C_TYPE`）+ `MS_TYPE_COUNT` 哨兵，共 21 个枚举值。本文以「20 个类型标签 + 哨兵」表述。
3. **堆上下文自立。** `MsState` 由任务 08 定义，本任务不得依赖编号更大的任务，故定义独立的 `struct MsHeap` 承载全对象链表、驻留缓存与类型对象表；任务 08 将 `MsHeap` 内嵌为 `MsState` 成员（假定名，以任务 08 文档定名为准），本任务全部 API 以 `MsHeap*` 为上下文参数，迁移不改签名形状。
4. **`struct MsType` 取最小定义。** 规范未给出类型对象的字段。本任务只承载 `{header, name}`（类对象本体、方法表、MRO 属任务 15/25 扩展点）；20 个内建类型对象在堆初始化时一次建成，使任何对象的 `header.type` 恒非空。

## 详细设计

### 文件与模块边界

- `src/object/ms_object.h`（guard `MSLANG_SRC_OBJECT_MS_OBJECT_H_`）：`MsTypeTag`、`MsMarkColor`、`struct MsObjectHeader`、`struct MsObject`、`struct MsBool`/`MsInt`/`MsFloat`、`struct MsType`、`struct MsHeap` 及全部对象级函数声明；自包含，自行 include `<stdbool.h>` `<stddef.h>` `<stdint.h>` 与任务 02 的 `"mslang/error.h"`（`MsResult` 的公开定义点）与 `"core/ms_common.h"`（`MS_ASSERT`）。
- `src/object/ms_object.c`：堆初始化/销毁、分配入口、单例与 int 缓存、值相等/哈希/真值、类型名表；辅助函数一律文件内 `static`。
- `src/object/ms_string.h`（guard `MSLANG_SRC_OBJECT_MS_STRING_H_`）：`struct MsString` 与字符串函数声明；include `"object/ms_object.h"`。
- `src/object/ms_string.c`：驻留表（开放寻址哈希）、构造与比较实现。
- 模块的全部堆分配（对象本体、驻留表桶数组）经 `msAlloc/msRealloc/msFree`；`MsHeap` 内嵌的单例、int 缓存、类型对象表为内嵌数组，不单独分配。销毁路径单一：`msHeapDestroy` 沿全对象链表回收全部已链接对象后释放桶数组，任务 02 的分配统计据此断言无泄漏。
- GC（任务 17）落地前没有回收，对象只随 `msHeapDestroy` 释放，故本任务阶段无需根栈纪律（`msRootPush` 属任务 17/18 配套）；驻留表的 GC 处置（弱引用或根集合成员）同样由任务 17 定稿，本任务只在结构上预留。

### 类型标签与标记色

```c
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
```

- 枚举值与 `docs/language/08-vm-internals.md` §3 同序同名；`_Static_assert(MS_TYPE_COUNT == 20, ...)` 守住规范数量。
- 灰色不单独取值：§5 的标记实为「黑白两色 + 标记栈」，待标记对象经栈暂存（任务 17）。
- `msTypeTagName` 提供静态名称表（`"MS_TYPE_INT"` 等）供诊断与测试；另为 20 个内建类型对象取脚本侧名称，按 02-types §1：`NilType`/`bool`/`int`/`float`/`str`/`bytes`/`list`/`tuple`/`dict`/`set`/`function`/`type`/`instance`/`module`/`chan`/`coroutine`/`iterator`/`bound method`/`cfunction`/`ctype`（后四个无脚本字面名的取内部描述名，任务 15/25/33 定稿时可改）。

### 对象头与基座

内部结构体不 typedef，字段 lowerCamelCase：

```c
struct MsObjectHeader {
  struct MsType* type;       // the type object; never NULL after construction
  uint8_t tag;               // MsTypeTag, cached for O(1) dispatch (see 设计依据 1)
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

struct MsType {
  struct MsObjectHeader header;   // tag == MS_TYPE_CLASS; type == &heap->types[MS_TYPE_CLASS]
  struct MsString* name;          // interned type name ("int", "str", ...)
};
```

- 所有对象首成员均为 `struct MsObjectHeader`，`MsObject*` 与各具体类型指针之间可安全互转；访问宏/内联函数只做转换与读取，越类型访问属编程错误（`MS_ASSERT` 守住）。
- `nil` 无负载，直接用基座 `struct MsObject` 的单例表示。
- `MsType` 的类型对象自指：全部 20 个 `MsType` 实例的 `header.tag == MS_TYPE_CLASS`、`header.type == &types[MS_TYPE_CLASS]`（含 `types[MS_TYPE_CLASS]` 自身，名称为 `"type"`，对齐 02-types §1「类 → `type`」）。方法与继承语义由任务 15/25 在此结构上扩字段。

### 堆上下文与统一分配入口

```c
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

MsResult msHeapInit(struct MsHeap* heap);   // MS_ERROR_OOM leaves *heap zeroed
void msHeapDestroy(struct MsHeap* heap);    // frees every linked object, then the intern buckets

// Core allocation: msAlloc's size bytes, initializes the header, links into the
// all-objects list (gcNext), bumps objectCount. Returns NULL on OOM.
// All concrete constructors funnel through here; direct msAlloc of objects is
// forbidden outside this module.
struct MsObject* msObjectAlloc(struct MsHeap* heap, MsTypeTag tag, size_t size);

// Unlinks obj from the all-objects list and frees it. Used by msHeapDestroy and
// by the GC sweep (task 17); NOT part of normal value lifecycle before then.
void msObjectFree(struct MsHeap* heap, struct MsObject* obj);
```

- 单例、int 缓存、类型对象表永久存活：初始化时置 `markColor = MS_MARK_BLACK`、`gcNext = NULL`，**不链入**全对象链表，GC 永不触碰（任务 17 只扫链表）。内嵌不堆分配的选择同时消除初始化期的 OOM 分支与 ~136 KB（4352 × 32 字节）缓存的额外指针间接；该内存开销在任务 65（NaN-boxing）落地后消失。
- `msHeapInit` 顺序：清零 → 驻留表桶数组（初始容量 64）→ 20 个类型对象（名称经驻留表创建）→ 单例与 int 缓存。类型名创建失败按获取逆序回滚并返回 `MS_ERROR_OOM`。
- `header.type` 由 `msObjectAlloc` 统一设为 `&heap->types[tag]`，构造路径上无手工填头的余地。

### 标量构造与访问

```c
// Singleton accessors (per-heap; no mutable globals, 10-c-style §4).
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

static inline MsTypeTag msObjectTag(const struct MsObject* obj);   // reads header.tag
struct MsType* msHeapType(struct MsHeap* heap, MsTypeTag tag);     // &heap->types[tag]
const char* msTypeTagName(MsTypeTag tag);                          // static name table
```

- int 驻留是规范明确点名的实现细节（02-types §6：`is` 不得依赖）；缓存命中判定集中在 `msIntNew` 一处，无二处路径。
- v0.1 的 `MsInt` 就是 int64：溢出提升为大整数由任务 31 在保持本接口的前提下替换表示（「统一走 MsInt 接口」，08-vm-internals §3）；届时 `msIntIsWord` 由恒真变为真实谓词，调用方（任务 08 快路径）无需改动。
- 与任务 08 的接口互洽：任务 08（VM 执行核心）假定本模块提供 `msIntIsWord`、`msIntWordValue`、`msFloatValue(const struct MsObject*)` 与 `msObjectValueEquals`（不取堆上下文的形态），本文按同名同签名落地，两处文档无歧义衔接；其假定的 `msObjectBinaryOp`/`msObjectUnaryOp`/`msLen` 与最小 `MsState` 属任务 08/09 的职责（见「设计依据」第 3 条），不在本模块。
- 机器字 int 的运算溢出策略在此定稿（任务 08 的慢路径按其执行）：本模块不含算术；任务 31 落地前，检出 int64 溢出即报运行时错误（整数溢出，不允许静默截断）；任务 31 落地后改为自动提升为大整数，该策略切换只发生在慢路径内部。

### MsString 与驻留表（ms_string）

```c
#define MS_STRING_SHORT_MAX_LEN 32   // residency threshold in bytes (tunable; spec leaves it open)

struct MsString {
  struct MsObjectHeader header;
  uint32_t hash;             // cached FNV-1a-32 over the bytes, computed once at construction
  size_t byteLen;            // UTF-8 byte length; code-point count is NOT stored (O(n) indexing per spec)
  char data[];               // byteLen + 1 bytes, NUL-terminated for C interop
};

// Constructors. Content of length <= MS_STRING_SHORT_MAX_LEN is interned: equal
// content returns the same object (same pointer). Longer strings are always
// freshly allocated. Return NULL on OOM.
struct MsString* msStringNew(struct MsHeap* heap, const char* data, size_t len);
struct MsString* msStringFromCStr(struct MsHeap* heap, const char* cstr);

static inline const char* msStringData(const struct MsString* s);
static inline size_t msStringLen(const struct MsString* s);
static inline uint32_t msStringHash(const struct MsString* s);

bool msStringEquals(const struct MsString* a, const struct MsString* b);   // byte-wise content equality
```

- 哈希：FNV-1a 32 位（offset 2166136261、prime 16777619），构造时一次算好缓存进 `hash` 字段，字符串不可变故缓存恒有效；dict（任务 16）与驻留表都直接复用该缓存。
- 驻留表为开放寻址哈希表：桶存 `MsString*`，按 `hash` 二次探测（线性探测即可，负载因子 0.75 触发倍增 rehash，初始容量 64）。查找以输入字节现场算哈希（不落对象），命中直接返回已驻留对象，未命中才经 `msObjectAlloc` 创建并插入——避免「先分配后丢弃」。
- 驻留字符串链入全对象链表（与普通对象同生命周期）；其在 GC 下的存活保障（驻留表作为根集合成员）由任务 17 定稿，本任务注释声明即可。
- `byteLen` 按字节计、码点索引 O(n) 是规范取舍（08-vm-internals §3）；`data` 末尾补 `\0` 仅为 C API（任务 18）与标准库互操作方便，不作为长度依据。

### 值相等、哈希与真值

```c
// Value equality per 02-types §6. Signature fixed by task 05's assumption.
bool msObjectValueEquals(const struct MsObject* a, const struct MsObject* b);

// Hash consistent with msObjectValueEquals: equal values hash equal.
// Hashable scalars only in this task (nil/bool/int/float/string); other tags
// MS_ASSERT in debug (identity hashing for instances is a later task).
uint32_t msObjectHash(const struct MsObject* obj);

// Truthiness per 02-types §2, scalar subset: nil, false, numeric zero and the
// empty string are falsy. Container emptiness joins in task 16; heap objects of
// other tags are truthy.
bool msObjectIsTruthy(const struct MsObject* obj);
```

- `msObjectValueEquals` 语义：指针相同即真（驻留快路径）；`nil` 同值；`bool` 比值；`int`/`float` 跨标签数值比较（`42 == 42.0` 为真），float 比较含 `nan != nan`（02-types §3.2）；`str` 按内容；其余标签本任务按指针相等（容器/实例的值语义属任务 16/25，在此函数内扩展，调用方无感）。
- `msObjectHash` 一致性约束：整数先经 64 位混合（如 splitmix64 终态混合的低 32 位）；**整数值的 float（如 `42.0`）按 int 路径求哈希**，保证 `hash(42) == hash(42.0)`——这是 dict 键查找（任务 16）在数值键上不出错的必要条件；字符串直接用缓存的 `hash`；`bool`/`nil` 取定值。
- 两个函数均不取 `MsHeap*`：字符串按内容比较/哈希无需驻留表，与任务 05 假定签名一致。

## 实现步骤

1. 建 `src/object/ms_object.h` 骨架：`MsTypeTag` 全量枚举 + `_Static_assert(MS_TYPE_COUNT == 20)`、`MsMarkColor`、`struct MsObjectHeader` 与各基座结构体、`struct MsHeap`、函数声明。验证：头文件自包含编译通过；`sizeof(struct MsObjectHeader) == 24`（64 位）的静态断言说明 `tag` 未涨头部。
2. 实现 `msHeapInit` / `msHeapDestroy`、内建类型对象表与 `msTypeTagName` / `msHeapType`。验证：init/destroy 往返后任务 02 分配统计归零；20 个类型对象名称与 `header.type` 自指/互指关系正确。
3. 实现 `msObjectAlloc` / `msObjectFree` 与全对象链表维护。验证：链表插入/摘除（头/中/尾）正确、`objectCount` 一致、destroy 后无泄漏。
4. 实现 nil/bool 单例、int 缓存、`msIntNew` / `msFloatNew` 与访问器。验证：缓存边界（-257/-256/0/4095/4096）的身份语义，访问器取值往返。
5. 建 `src/object/ms_string.{h,c}`：`struct MsString`、FNV-1a 哈希、驻留表（探测/插入/倍增 rehash）、`msStringNew` / `msStringFromCStr` / `msStringEquals`。验证：短字符串同内容同指针、长字符串不驻留、表增长后全部已驻留项仍可命中、UTF-8 多字节内容原样保留。
6. 实现 `msObjectValueEquals` / `msObjectHash` / `msObjectIsTruthy`。验证：跨数值相等与哈希一致性、NaN 自反不等、真值表逐条。
7. 接入 CMake `mslang` 库目标与 `mslang-tests`，`ctest` 全绿；`MSLANG_STRICT_WARNINGS` 无警告；构建产物只在 `build/`。

## 测试方案

本任务早于最小可运行解释器（任务 09），只能用 C 单元测试。测试文件两个（本任务只交付本设计文档，测试代码随实现任务编写），使用任务 01 的 `ms_test.h`（`MS_TEST`/`MS_ASSERT_EQ`）。每个用例在 `setUp` 建独立 `MsHeap`、`tearDown` 销毁并经任务 02 的 `msMemGetStats` 断言分配统计归零。

`tests/c/test_object.c` 覆盖：

- 枚举与名称表：`MS_TYPE_COUNT == 20`；`msTypeTagName` 全量非空、互不相同、与枚举同序。
- 类型对象表：20 个 `msHeapType` 结果互不相同、名称与 02-types §1 对应（`int`/`str`/`type` 等抽查）；全部类型对象 `tag == MS_TYPE_CLASS`；`types[MS_TYPE_CLASS].header.type == 自身`。
- 单例：两次 `msHeapNil` 返回同指针；`msHeapBool(h, true)`/`(h, false)` 各为同指针且互不相同；单例 `markColor == MS_MARK_BLACK` 且不在全对象链表（`objectCount` 不因访问单例变化）。
- int 缓存：-256、0、4095 两次构造同指针；-257、4096、`INT64_MIN`、`INT64_MAX` 两次构造不同指针且为链表对象（`objectCount` 递增）；`msIntIsWord` 对全部 int 对象为真、`msIntWordValue` 取值往返。
- float：`msFloatNew` + `msFloatValue` 往返，含 0.0、-0.0、inf、nan 位级保留。
- 值相等：同值 int/float 跨标签相等；`42 != 43`；`true == true`；`nil == nil`；`nan != nan`；不同标签（如 int vs string 对象）不等。
- 哈希一致性：`msObjectValueEquals(a,b)` 为真则 `msObjectHash(a) == msObjectHash(b)`，覆盖 int/float 同值对（`42`/`42.0`）、bool、字符串（内容相同的长字符串两实例）。
- 真值：`nil`、`false`、`0`、`0.0`、空字符串为假；`true`、`1`、`-1`、`0.5`、非空字符串、任意新分配非空标量对象为真。
- 链表与销毁：构造混合对象序列后 `msObjectFree` 摘头/中/尾节点，链表与计数一致；`msHeapDestroy` 后分配统计归零（无泄漏）。

`tests/c/test_string.c` 覆盖：

- 基本构造：`msStringNew`/`msStringFromCStr` 的 `byteLen`、`data` 内容与末尾 NUL；空字符串（长度 0）驻留且同指针。
- 哈希：同一内容多次构造 `hash` 相同；已知 FNV-1a 向量的黄金值比对（如 `"hello"` 的 32 位 FNV-1a）。
- 驻留：长度 ≤ 32 的同内容字符串（含多字节 UTF-8、含内嵌 `\0` 的按长内容）两次构造同指针；33 字节及以上同内容两次构造不同指针但 `msStringEquals` 为真；`msStringEquals` 对前缀关系（`"ab"` vs `"abc"`）为假。
- 驻留表增长：插入超过负载因子的不同短字符串（如 200 个格式化串），逐一复查全部仍可命中同指针；`internLen`/`internCap` 语义正确。
- 边界：`MS_STRING_SHORT_MAX_LEN` 恰界值（32 驻留、33 不驻留）；`NULL` 数据 + 0 长度等价空串。
- 销毁：驻留与长字符串混合构造后 `msHeapDestroy`，分配统计归零。

## 验收标准

- [ ] `src/object/ms_object.{h,c}` 与 `src/object/ms_string.{h,c}` 存在，guard 分别为 `MSLANG_SRC_OBJECT_MS_OBJECT_H_` / `MSLANG_SRC_OBJECT_MS_STRING_H_`，头文件自包含，风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、枚举值 `MS_` 大写蛇形）。
- [ ] `MsTypeTag` 与 08-vm-internals §3 同序同名（20 标签 + `MS_TYPE_COUNT` 哨兵并有 `_Static_assert`）；`struct MsObjectHeader` 为 `{type, tag, markColor, gcNext}`（`tag` 为对规范的显式补充，理由见「设计依据」），64 位下头部长度仍为 24 字节。
- [ ] 一切值经 `MsObject*` 装箱表示（无 NaN-boxing）；全部对象分配收口于 `msObjectAlloc` 且经 `msAlloc`/`msFree`，分配统计无泄漏；单例/int 缓存/类型对象永久存活且不链入全对象链表。
- [ ] 小整数 -256..4095 驻留（同值同指针）、范围外正常分配；`int` 机器字（int64）直存，大整数留任务 31 的扩展点说明落实。
- [ ] `MsString` 为「长度 + 缓存哈希 + NUL 结尾 UTF-8 字节」布局；短字符串（≤ 32 字节）驻留同内容同指针、长字符串不驻留；驻留表倍增 rehash 后命中正确。
- [ ] 20 个内建类型对象随 `msHeapInit` 建成、名称对齐 02-types §1、`header.type` 恒非空；`MsType` 最小定义与任务 15/25 的扩展衔接在文中声明。
- [ ] `msObjectValueEquals`（签名与任务 05 的假定一致）、`msObjectHash`、`msObjectIsTruthy` 语义符合 02-types §2/§3/§6，含 int/float 跨标签相等与哈希一致性、`nan != nan`。
- [ ] `tests/c/test_object.c` 与 `tests/c/test_string.c` 覆盖「测试方案」全部清单项并全部通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；任务 02 接口（`msAlloc` 族、`MsResult` 于 `"mslang/error.h"`、分配统计）按其已成文文档使用；任务 08 的接口假定（`msIntIsWord`/`msIntWordValue`/`msFloatValue`/`msObjectValueEquals`、`MsState` 内嵌 `MsHeap`）与本文定名一致。
