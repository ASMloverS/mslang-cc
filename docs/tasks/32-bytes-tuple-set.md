# 32 bytes、tuple 与 set 类型

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [16 容器 list 与 dict](16-containers-list-dict.md) |

## 任务目标

交付 mslang 的三个内建容器/序列类型——`bytes`（不可变原始字节序列）、`tuple`（不可变定长异构序列）、`set`（可变哈希集合）——的对象实现与 VM 语义接线：

- 对象模块 `src/object/ms_bytes.{c,h}`、`src/object/ms_tuple.{c,h}`、`src/object/ms_set.{c,h}`：三个类型的存储结构、相等/哈希实现（bytes/tuple 可哈希，set 不可哈希）、tuple 的惰性哈希缓存、set 的开放寻址哈希表；
- 公开 C API 的对应部分落地（[09-c-api.md](../language/09-c-api.md) §5 已声明的 `msNewBytes` / `msNewTuple`）；
- 字面量与构建指令的语义实现：`b"..."` 字节串字面量（lexer 已产出 `MS_TOKEN_BYTES`，见任务 03）编译为常量池中的 bytes 常量；`MS_OP_BUILD_TUPLE` / `MS_OP_BUILD_SET` 的 VM 分支；
- 下标/成员/算术指令的分派：`bytes` 与 `tuple` 的 `MS_OP_INDEX` 只读下标（负索引归一化）、`MS_OP_IN` 成员测试、`MS_OP_ADD` 拼接、`MS_OP_MUL` 重复；`set` 的 `MS_OP_IN`；
- 方法集：`bytes.toString()` 与 `set.add/remove/contains/union/intersect/diff`；`str.toBytes()`（与前者互为逆操作，[02-types.md](../language/02-types.md) §4）在本任务一并补齐；
- 内建函数分派补齐：`len`、`hash`、`bytes(x)`/`tuple(x)`/`set(x)` 转换、真值判定与 `repr` 对三个新类型的支持。

完成后，脚本能写 `b"\x00\x01"`、`(1, "two")`、`{1, 2, 3}` 与 `set()`，做下标读取、成员测试、集合运算，把 bytes/tuple 用作 dict 键与 set 元素，并通过 `tests/ms/` 的 ms 脚本验证。切片（`t[1:3]`）属任务 30，迭代协议（`for x in t`）属任务 26，均不在本任务范围（本任务只提供它们所需的迭代原语）。

## 设计依据

- [02-types.md](../language/02-types.md)
  - §1 类型总表：`bytes` 不可变原始字节序列、`tuple` 不可变定长异构序列、`set` 可变哈希集合。
  - §2 真值规则：空字节串 `b""`、空容器 `()`、`set()` 为假。
  - §4 str 与编码：`s.toBytes()` → bytes；`bytes.toString()` → str。
  - §5.2 tuple 语义：`(1, "two")` 字面量、1 元组须带尾逗号 `(1,)`、解包赋值 `a, b := t`。
  - §5.4 set 语义：`{1, 2, 3}` 字面量（无冒号，与 dict 区分）、`{}` 恒为空 dict、空集用 `set()`、方法 `add/remove/contains/union/intersect/diff`。
  - §6 相等与哈希：`int/float/bool/str/bytes/tuple` 递归按值且可哈希；`list/dict/set` 不可哈希。
- [01-lexical.md](../language/01-lexical.md) §5.4：`b"..."` 字节串字面量，转义规则同普通字符串（`\xHH` 即原始字节）；lexer（任务 03）已产出 `MS_TOKEN_BYTES` token，本任务负责其在编译期落地为 bytes 常量。
- [03-syntax.md](../language/03-syntax.md) §6.1：下标 `a[i]`，负索引从尾部计数；§9：内建函数 `bytes(x) tuple(x) set(x) len(x) hash(x)`。
- [04-exceptions.md](../language/04-exceptions.md) §3 异常层级：本任务运行于任务 23（异常系统）之后，全部错误点抛出真实异常对象——`TypeError`（类型错误、不可哈希）、`ValueError`（字节值越界、UTF-8 解码失败，后者对应其子类 `UnicodeError`）、`IndexError`（下标越界）、`KeyError` 式"元素不存在"（`set.remove` 缺失元素）。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.2：容器指令含 `MS_OP_BUILD_TUPLE` / `MS_OP_BUILD_SET` / `MS_OP_INDEX` / `MS_OP_IN` / `MS_OP_UNPACK`；§3 对象模型：`MsTypeTag` 已含 `MS_TYPE_BYTES` / `MS_TYPE_TUPLE` / `MS_TYPE_SET`，所有值都是装箱 `MsObject*`。
- [09-c-api.md](../language/09-c-api.md) §3：GC 根纪律；§5：`msNewBytes(MsState* L, const uint8_t* data, size_t len)` 与 `msNewTuple(MsState* L, int64_t len)` 的既定签名；§8：错误处理约定（`NULL` + 错误槽、`msRaiseTypeError` 等）；§9：`MsCFunction` 与 `MsMethodDef` 方法表定义。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R、指针星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`、include guard）。
- [11-project-layout.md](../language/11-project-layout.md) §1：`src/object/` 与 `tests/ms/` 的目录位置；§5 路线图 v0.2：bytes、tuple、set 属本期。
- 任务 16（容器 list 与 dict）提供：哈希/相等契约（数值统一：`1`/`1.0`/`true` 相等且同哈希）、组合表实现先例、容器指令分派的接入点与 `.exit` 负例测试约定。
- 任务 06（对象模型基础）、任务 10（内建函数）、任务 17（GC 标记-清除）的文档尚未定稿，本文引用的接口名（`msObjEqual`、`msObjHash`、真值分派、GC 遍历挂接点、内建函数注册通路等）为假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件与模块边界

- `src/object/ms_bytes.h`（guard `MSLANG_SRC_OBJECT_MS_BYTES_H_`）与 `ms_bytes.c`：bytes 存储结构与全部操作。
- `src/object/ms_tuple.h`（guard `MSLANG_SRC_OBJECT_MS_TUPLE_H_`）与 `ms_tuple.c`：tuple 存储结构与全部操作。
- `src/object/ms_set.h`（guard `MSLANG_SRC_OBJECT_MS_SET_H_`）与 `ms_set.c`：set 哈希表与全部操作。
- 三个头文件均自包含，包含 `<stdbool.h>` `<stdint.h>` `<stddef.h>` 与任务 06 的对象模型头文件（假定 `"object/ms_object.h"`）。`msNewBytes`/`msNewTuple` 的公开声明已存在于 `include/mslang/object.h`（09-c-api §5），本任务落地其实现；set 暂不提供公开 C API（仅内部模块接口），公开化需求由任务 33（C 扩展）评估。
- 字面量落地：parser（任务 04）已把 `b"..."`、`(a, b)`、`{a, b}` 解析为对应 AST（set 字面量以无冒号区分 dict），compiler（任务 07）为字节串字面量生成 `MS_OP_LOAD_CONST`（常量经 `msLexerUnescape` 解码后以 `msNewBytes` 构建为 bytes 常量，bytes 不可变故常量可安全共享）、为 tuple/set 字面量生成 `MS_OP_BUILD_TUPLE n` / `MS_OP_BUILD_SET n`。若任务 04/07 对上述语法留有"尚未支持"占位，本任务接通之；若尚未解析，则由本任务补齐 parser/compiler 的对应分支（最小改动：AST 节点 + 代码生成各一处）。
- VM 侧只填充任务 08 分派循环中各指令对 `MS_TYPE_BYTES`/`MS_TYPE_TUPLE`/`MS_TYPE_SET` 的 `case` 分支，指令编码与栈约定不变。
- 方法表注册、真值/相等/哈希/repr 分派、内建函数分派均沿用任务 06/10/16 确立的机制，本任务只注册新条目。

### 2. MsBytes：不可变字节序列

```c
struct MsBytes {
  MsObjectHeader header;
  int64_t  len;        // byte count, >= 0
  uint64_t hash;       // lazy hash cache; 0 = not computed
  uint8_t  data[];     // flexible array, len bytes, allocated together with the header
};
```

- 单次分配：对象本体与 `data` 由一次 `msAlloc(sizeof(struct MsBytes) + len)` 获得，GC 回收时一次 `msFree` 释放；`len == 0` 时 `data` 为零长，仍合法。bytes 是叶子对象，无子对象遍历。
- 不可变性：`data` 在构造时一次性写入（`msNewBytes` 从调用者缓冲区 `memcpy`，`data == NULL` 且 `len > 0` 属编程错误，debug 下 `MS_ASSERT`），此后无任何写路径；哈希可安全缓存。
- 哈希：`msBytesHash` 惰性计算并缓存——FNV-1a 64 位（初值 `14695981039346656037ULL`，逐字节 `h = (h ^ b) * 1099511628211ULL`）；计算结果为 `0` 时重映射为固定非零常量（如 `14695981039346656037ULL`），保持 `0` 作为"未计算"哨兵。具体算法常量属实现细节，唯一硬约束是「相等 ⇒ 同哈希」且与任务 06 的数值统一契约兼容（bytes 只与 bytes 相等，无跨类型哈希约束）。
- 相等：`msBytesEqual`——同指针快速路径 `true`；`len` 不等即 `false`；否则 `memcmp`。bytes 与 str 不判等（`b"a" == "a"` 为 `false`，与 Python 一致）。
- 下标：`b[i]` 返回第 `i` 个字节的无符号值装箱成的 int（0–255），负索引归一化（模块内 `static` 辅助函数，不复用任务 16 的 list 内部函数），越界抛 `IndexError`。`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX` 对 bytes 一律抛 `TypeError`（"bytes does not support item assignment/deletion"）。
- 拼接与重复：`b1 + b2` 构建新 bytes（`msBytesConcat`）；`b * n` 重复（`msBytesRepeat`），`n <= 0` 得空 bytes，`n` 非 int 抛 `TypeError`；结果长度溢出 `int64_t` 或超内存上限走统一 OOM 路径。
- 成员测试 `x in b`（设计决策，02-types 未明示）：`x` 为 int 时判字节存在（值须在 0–255，否则抛 `ValueError`）；`x` 为 bytes 时做子序列搜索（`b"" in b` 恒 `true`）；其余类型抛 `TypeError`。

```c
// Public C API implementation (declared in include/mslang/object.h).
MsObject* msNewBytes(MsState* L, const uint8_t* data, size_t len);

// Module-internal operations (src/object/ms_bytes.h).
MsObject* msBytesConcat(MsState* L, struct MsBytes* a, struct MsBytes* b);
MsObject* msBytesRepeat(MsState* L, struct MsBytes* b, int64_t n);
bool      msBytesEqual(struct MsBytes* a, struct MsBytes* b);
uint64_t  msBytesHash(struct MsBytes* b);  // cached; cannot fail
// Indexing: returns the byte value, or -1 with an IndexError raised when out of range.
int64_t   msBytesGetAt(MsState* L, struct MsBytes* b, int64_t index);
bool      msBytesContainsByte(MsState* L, struct MsBytes* b, int64_t byte);
bool      msBytesContainsSub(struct MsBytes* haystack, struct MsBytes* needle);
// Decodes as UTF-8 into a new str; invalid sequences raise ValueError (UnicodeError).
MsObject* msBytesToString(MsState* L, struct MsBytes* b);
```

方法表 `msBytesMethods`：`toString()` → `msBytesToString`。另在 str 的方法表（任务 06/09 既有）补注册 `toBytes()`：以 UTF-8 字节序构建新 bytes，永不失败（str 已保证合法 UTF-8）。

### 3. MsTuple：不可变定长序列

```c
struct MsTuple {
  MsObjectHeader header;
  int64_t  len;          // element count, >= 0
  uint64_t hash;         // lazy hash cache; 0 = not computed
  MsObject* items[];     // flexible array, len slots, allocated together with the header
};
```

- 单次分配同 bytes。`msNewTuple(L, len)` 把 `items` 全部预填 `nil`，保证构建期任何时刻 GC 遍历都看到合法对象——这是本结构的硬性不变量；编译器/VM 随后以 `msTupleSetItem` 逐槽覆盖填入真实元素（`MS_OP_BUILD_TUPLE` 弹栈填入时**逆序写回**以保持字面量序）。
- 不可变性指槽位不再改写：填满后 VM 与方法集都不提供写路径；`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX` 对 tuple 抛 `TypeError`。元素本身的可变性不受约束（tuple 可含 list）。
- 相等：`msTupleEqual`——同指针快速路径 `true`（同时作为自引用结构的递归终止保护，同任务 16 的约定）；`len` 不等即 `false`；逐元素 `msObjEqual` 递归按值比较。tuple 与 list 不判等。
- 哈希与缓存（本任务的要点之一）：`msTupleHash` 惰性计算——以 `len` 混入的固定种子起，逐元素取 `msObjHash` 后按 `h = (h ^ elemHash) * 1099511628211ULL` 组合，结果缓存于 `hash` 字段（`0` 哨兵重映射规则同 bytes）。要点：
  1. 任一元素不可哈希（如含 list/dict/set）时哈希失败，`TypeError` 照常抛出且**不缓存**，下次再试仍重新计算；
  2. 缓存成立的前提是「可哈希元素皆不可变」（02-types §6 的语言级契约）；定义了 `__hash__` 的可变实例若中途变异，缓存随之失效属已知限制，与 Python 行为一致，文档注明不额外处理；
  3. 相等 tuple 必同哈希：元素哈希经统一 `msObjHash` 分派，数值统一契约（`1`/`1.0`/`true`）在元素层已保证。
- 下标：`t[i]` 返回元素（负索引归一化），越界抛 `IndexError`。
- 拼接与重复：`(1, 2) + (3,)`、`t * n` 构建新 tuple；`t1 + t2` 要求双方均为 tuple，否则抛 `TypeError`。
- 成员测试：`v in t` 逐元素 `msObjEqual` 线性扫描。
- 解包：`MS_OP_UNPACK n` 遇 tuple 时校验 `t->len == n`（不等抛 `ValueError`），按序把元素压栈；其他可迭代对象的解包属任务 26 的通用迭代协议，若任务 11/26 已实现类型无关的解包，本任务的分支即 tuple 快路径。

```c
// Public C API implementation (declared in include/mslang/object.h).
// All slots are pre-filled with nil so the tuple is always GC-traversable.
MsObject* msNewTuple(MsState* L, int64_t len);

// Module-internal operations (src/object/ms_tuple.h).
void      msTupleSetItem(struct MsTuple* t, int64_t index, MsObject* v);  // construction-time only
MsObject* msTupleGet(MsState* L, struct MsTuple* t, int64_t index);       // NULL + IndexError
MsObject* msTupleConcat(MsState* L, struct MsTuple* a, struct MsTuple* b);
MsObject* msTupleRepeat(MsState* L, struct MsTuple* t, int64_t n);
bool      msTupleEqual(MsState* L, struct MsTuple* a, struct MsTuple* b);
// Returns false with a TypeError raised when any element is unhashable; caches on success.
bool      msTupleHash(MsState* L, struct MsTuple* t, uint64_t* out);
bool      msTupleContains(MsState* L, struct MsTuple* t, MsObject* v);
```

tuple 不提供脚本可见的方法（02-types §5.2 未定义任何方法，`index`/`count` 等列入路线图候选）；脚本能力全部经运算符与内建函数表达。

### 4. MsSet：开放寻址哈希集合

02-types 未承诺 set 保持插入序（仅 dict 有该承诺），故不沿用任务 16 的组合表，采用更简单的**单一开放寻址表 + 显式槽状态**；迭代顺序未指定（实现细节，脚本不得依赖），文档显式注明。

```c
typedef enum {
  MS_SET_SLOT_EMPTY,     // never used
  MS_SET_SLOT_LIVE,      // holds an element
  MS_SET_SLOT_DELETED    // tombstone
} MsSetSlotState;

struct MsSetEntry {
  MsObject*     key;     // element; NULL when state != MS_SET_SLOT_LIVE
  uint32_t      hash;    // cached key hash, avoids rehash on resize
  MsSetSlotState state;
};

struct MsSet {
  MsObjectHeader      header;
  struct MsSetEntry*  entries;  // open-addressing table, size = mask + 1
  int64_t             used;     // live elements
  int64_t             fill;     // used + tombstones
  int64_t             mask;     // table size - 1; -1 when entries == NULL (empty set)
};
```

- 空集合 `entries == NULL`，首次 `msSetAdd` 时初始化为 8 槽；表长恒为 2 的幂。
- 探测序列：`slot = hash & mask`，冲突时 `slot = (slot * 5 + hash + 1) & mask`（与任务 16 dict 同款扰动策略，保证遍历全部槽位）。
- 负载因子：`fill * 3 >= (mask + 1) * 2` 时扩容倍增；扩容只重插 `MS_SET_SLOT_LIVE` 条目（以缓存的 `hash` 重建，不重新求哈希），墓碑自然压实回收。
- 删除：置槽为 `MS_SET_SLOT_DELETED`，`used--`、`fill` 不变；查找遇墓碑跳过，插入遇墓碑复用。
- 元素哈希/相等经任务 06 的 `msObjHash`/`msObjEqual` 分派，数值统一契约自动生效：`{1, 1.0, true}` 只有一个元素。不可哈希元素（list/dict/set/未定义 `__hash__` 的实例）在 `msSetAdd`/`msSetContains` 入口处由 `msObjHash` 抛 `TypeError`（"unhashable type"），set 自身不做类型特判。
- `msSetNext` 是唯一迭代原语（供 `for x in s`（任务 26）、`set(x)` 转换与 `repr` 使用）：按 `entries` 下标游标返回存活元素，顺序未指定；迭代期间禁止结构性修改（同任务 16 `msDictNext` 的约定，v0.2 不做修改检测）。

```c
// Module-internal operations (src/object/ms_set.h); no public C API in v0.2.
MsObject* msNewSet(MsState* L);
MsResult  msSetAdd(MsState* L, struct MsSet* s, MsObject* v);       // no-op when already present
bool      msSetContains(MsState* L, struct MsSet* s, MsObject* v);  // raises TypeError if unhashable
MsResult  msSetRemove(MsState* L, struct MsSet* s, MsObject* v);    // error when missing
bool      msSetEqual(MsState* L, struct MsSet* a, struct MsSet* b);
MsObject* msSetUnion(MsState* L, struct MsSet* a, struct MsSet* b);
MsObject* msSetIntersect(MsState* L, struct MsSet* a, struct MsSet* b);
MsObject* msSetDiff(MsState* L, struct MsSet* a, struct MsSet* b);
// Iteration over live elements in unspecified order: pass *cursor = 0 to start.
bool      msSetNext(struct MsSet* s, int64_t* cursor, MsObject** key);
```

- `msSetEqual`：同指针快速路径 `true`；`used` 不等即 `false`；对 `a` 的每个元素 `msSetContains(b, v)`。
- 集合运算（均返回**新 set**，不修改操作数；参数必须是 set，否则抛 `TypeError`——接受任意可迭代对象的泛化依赖任务 26 的迭代协议，列入其范围）：
  - `msSetUnion`：以 `a->used + b->used` 预估容量建新集，先装入 `a` 全部元素再逐装 `b`；
  - `msSetIntersect`：遍历 `used` 较小者，命中较大者即装入新集；
  - `msSetDiff`：遍历 `a`，不在 `b` 中者装入新集。

### 5. 指令语义接线

各指令在任务 08 的分派循环中按操作数类型分派，本任务填充三个新类型的分支：

| 指令 | bytes 分支 | tuple 分支 | set 分支 |
|---|---|---|---|
| `MS_OP_BUILD_TUPLE n` | — | 弹 n 个元素逆序填入 `msNewTuple(L, n)` | — |
| `MS_OP_BUILD_SET n` | — | — | 弹 n 个元素逐一 `msSetAdd` 进 `msNewSet`；重复元素自然坍缩，不报错 |
| `MS_OP_INDEX` | `msBytesGetAt`，越界抛 `IndexError` | `msTupleGet`，越界抛 `IndexError` | 抛 `TypeError`（"set is not subscriptable"） |
| `MS_OP_SET_INDEX` / `MS_OP_DEL_INDEX` | 抛 `TypeError`（不可变） | 抛 `TypeError`（不可变） | 抛 `TypeError`（不可下标；元素删除用 `remove` 方法） |
| `MS_OP_IN`（`v in c` / `not in`） | int 查字节 / bytes 查子序列 / 其他抛 `TypeError` | `msTupleContains` 线性按值 | `msSetContains`（含不可哈希错误传播） |
| `MS_OP_ADD` | `msBytesConcat` | `msTupleConcat` | 抛 `TypeError`（集合运算用方法） |
| `MS_OP_MUL` | `msBytesRepeat`（另一操作数须为 int） | `msTupleRepeat`（同左） | 抛 `TypeError` |
| `MS_OP_UNPACK n` | 按字节序列解包为 n 个 int（长度不等抛 `ValueError`） | 见 §3 | 不保证顺序，解包 set 抛 `TypeError`（顺序未指定，刻意禁止） |
| `MS_OP_SLICE` | 占位错误，任务 30 落地后替换（同任务 16 的处理） | 同左 | 同左 |

- 全部错误点经任务 23 的异常系统抛出对应异常对象（不再是任务 16 时期的"错误槽 + 退出码 1"）；错误消息文本与任务 16 的风格保持一致。
- `MS_OP_EQ`/`MS_OP_NE` 经任务 06 的 `msObjEqual` 分派到本任务的三个 `*Equal` 函数；跨类型一律不相等。

### 6. 内建函数、真值与 repr 接线

- `len(x)`（任务 10）：新增三个分派——bytes 取 `len`、tuple 取 `len`、set 取 `used`。
- `hash(x)`（任务 10）：bytes 走 `msBytesHash`，tuple 走 `msTupleHash`（失败传播 `TypeError`），set 未注册落到默认分支抛 "unhashable type: 'set'"。
- 转换内建（任务 10 的 `bytes(x)`/`tuple(x)`/`set(x)` 增加分派；同源返回原对象，不做防御性拷贝——不可变类型无拷贝必要）：
  - `bytes(x)`：x 为 bytes → 原样返回；x 为 str → 同 `toBytes()`；x 为 list/tuple → 逐元素须为 0–255 的 int（越界抛 `ValueError`、非 int 抛 `TypeError`）装入新 bytes；其余类型抛 `TypeError`。
  - `tuple(x)`：x 为 tuple → 原样返回；x 为 list → 按序装入新 tuple；其余类型（含 dict/set/str）抛 `TypeError` 并注明"待任务 26 迭代协议后接受任意可迭代对象"。
  - `set(x)`：无参 `set()` → 空集（02-types §5.4 的空集写法）；x 为 set → **浅拷贝**新集（set 可变，同源返回会引入别名陷阱，与 Python `set(s)` 一致）；x 为 list/tuple → 逐元素 `msSetAdd`；x 为 dict → 装入其键（与 Python 一致）；其余类型抛 `TypeError`。
- 真值（任务 06 分派）：`len == 0` / `len == 0` / `used == 0` 为假。
- `repr`（任务 06 的逐类型表示槽）：bytes → `b"..."`（非可打印字节与引号/反斜杠转义为 `\xHH` 等，与字面量规则互逆）；tuple → `(1, "two")`，空 tuple `()`，1 元组 `(1,)`（尾逗号不可省）；set → `{1, 2, 3}`，空集 `set()`（因 `{}` 是 dict 的 repr），元素顺序未指定。
- dict 键与 set 元素能力随之自动获得：`{b"k": 1}`、`{(1, 2): "x"}`、`{1, b"a", (1, 2)}` 均为合法字面量（哈希分派注册后即生效，无需 dict 侧改动）。

### 7. 内存与 GC 纪律

- 全部堆分配经任务 02 的 `msAlloc`/`msRealloc`/`msFree`；bytes/tuple 为单次分配（含柔性数组），set 为对象本体 + `entries` 两块。
- GC 挂接（任务 17 的逐类型遍历机制，假定类型对象上有遍历/析构槽，以实现时定名为准）：bytes 无子对象；tuple 标记 `items[0..len)`；set 标记全部 `MS_SET_SLOT_LIVE` 条目的 `key`；set 析构按逆序 `msFree(entries)` 再释放本体。
- 根纪律（09-c-api §3）：`MS_OP_BUILD_TUPLE`/`MS_OP_BUILD_SET` 循环中已弹出求值栈的元素、集合运算构建期间的新集与中间值，凡跨分配存活的局部 `MsObject*` 一律 `msRootPush`/`msRootPop`（LIFO）。tuple 因 `msNewTuple` 预填 `nil`，填槽过程天然 GC 安全，但弹栈后的元素值本身仍需入根。

## 实现步骤

1. 建 `src/object/ms_bytes.h` / `ms_bytes.c` 骨架：`struct MsBytes`、`msNewBytes`、`msBytesEqual`、`msBytesHash`（含缓存与 0 哨兵重映射）。验证：ms 脚本 `b"\x00\x01"` 字面量求值、`==`/`!=`、空 bytes 真值为假。
2. 接通字节串字面量的编译期落地（`MS_TOKEN_BYTES` → `msLexerUnescape` 解码 → bytes 常量）与 bytes 的 `MS_OP_INDEX`/`MS_OP_ADD`/`MS_OP_MUL`/`MS_OP_IN` 分支、`len`/`hash`/`repr` 分派。验证：`b"ab"[1]`、负索引、拼接、重复、`1 in b"\x01"`、子序列包含的脚本断言。
3. 实现 `bytes.toString()` 与 `str.toBytes()`（含非法 UTF-8 抛 `ValueError` 的负例）及 `bytes(x)` 转换分派。验证：str/bytes 双向转换互逆、`bytes([65, 66]) == b"AB"`、越界字节值负例。
4. 建 `src/object/ms_tuple.h` / `ms_tuple.c`：`struct MsTuple`、`msNewTuple`（预填 `nil`）、`msTupleGet`/`msTupleConcat`/`msTupleRepeat`/`msTupleEqual`/`msTupleContains`。接通 `MS_OP_BUILD_TUPLE`、tuple 的 `MS_OP_INDEX`/`MS_OP_ADD`/`MS_OP_MUL`/`MS_OP_IN`/`MS_OP_UNPACK` 分支与 `len`/`repr` 分派。验证：`(1, "two")`、`(1,)`、`()`、下标、`a, b := t` 解包、拼接/重复、嵌套 tuple 相等的脚本断言。
5. 实现 `msTupleHash` 惰性缓存（含元素不可哈希失败路径）并注册哈希分派；`tuple(x)` 转换。验证：tuple 作 dict 键与 set 元素、重复 `hash(t)` 结果稳定、含 list 的 tuple 哈希抛 `TypeError`。
6. 建 `src/object/ms_set.h` / `ms_set.c`：`struct MsSet` 开放寻址表的查找/插入/删除/扩容压实、`msSetEqual`、三个集合运算、`msSetNext`。接通 `MS_OP_BUILD_SET` 与 `MS_OP_IN` 的 set 分支、`len`/`repr` 分派。验证：`{1, 2, 3}` 构建、重复元素坍缩、`s.add`/`s.remove`、数值统一（`{1, 1.0, true}` 单元素）、`in` 成员测试的脚本断言。
7. 注册 set 方法表（`add`/`remove`/`contains`/`union`/`intersect`/`diff`）与 `set(x)` 转换分派。验证：三个集合运算的返回新集语义（原集不变）、`set()` 空集、`set([1, 2])`/`set({...})`/`set(dict)`、`remove` 缺失元素抛异常的脚本断言。
8. 根纪律复查与内存收尾：构建与集合运算中跨分配的局部对象入根核对；GC 遍历/析构挂接核对。验证：Debug 构建（ASAN / `/RTC`）跑全部本任务测试无报告，`msCloseState` 后分配计数归零。
9. 全平台（Win/Linux/macOS）× Debug/Release 构建验证，`run_tests.py` 全绿。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试（`testing` 模块在任务 40 才存在，本阶段用内建 `assert` + `print` 自断言；异常系统已在任务 23 落地，负向用例优先以 `try/except` 捕获并断言异常类型，无法捕获的致命错误沿用 `<name>.exit` 同伴文件声明预期退出码 1 的约定，由 `run_tests.py` 驱动）。本任务只交付设计文档，脚本随实现编写。

测试文件清单与覆盖点：

- `tests/ms/bytes/basic.ms`：`b""`、`b"abc"`、`b"\x00\xff"` 字面量与 `repr` 互逆打印；`len`；正/负索引（`b"ab"[1] == 98`、`b"ab"[-1] == 98`）；真值（`assert(not b"")`）；`==`/`!=`（含与 str 不判等）；末尾 `print("bytes basic ok")`。
- `tests/ms/bytes/ops.ms`：拼接 `b"a" + b"b"`、重复 `b"ab" * 3`、`b * 0` 得空；成员测试（int 字节、bytes 子序列、`b"" in b` 恒真）；`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX` 抛 `TypeError`（`try/except TypeError` 断言）。
- `tests/ms/bytes/convert.ms`：`b"abc".toString() == "abc"`、`"héllo".toBytes()` 的逐字节断言、`str(b)`/互逆往返；`bytes("ab")`、`bytes([65, 66])`、`bytes(b"x")` 同源返回；负例：`bytes([256])` 抛 `ValueError`、`bytes(["a"])` 抛 `TypeError`、非法 UTF-8 字节串 `toString()` 抛 `ValueError`。
- `tests/ms/tuple/basic.ms`：字面量 `()`/`(1,)`/`(1, "two", 3.0)`（含 `repr` 的 `(1,)` 尾逗号断言）；`len`；正/负索引；空 tuple 真值为假；嵌套 tuple 与异构元素；`a, b := t` 解包（含长度不等抛 `ValueError`）。
- `tests/ms/tuple/ops.ms`：拼接、重复、成员测试（含嵌套容器元素按值命中）；`t[0] = 1` 与 `del t[0]` 抛 `TypeError`；tuple 与 list 不判等；`tuple([1, 2])`、`tuple(t)` 同源返回。
- `tests/ms/tuple/hash.ms`：`hash((1, "a", b"x"))` 两次调用结果相等；tuple 作 dict 键（`{(1, 2): "x"}[(1, 2)]`）与 set 元素；数值统一（`(1,) in {(1.0,)}`、`{(true,)}` 与 `{(1,)}` 相等）；含 list 的 tuple 作键/哈希抛 `TypeError`。
- `tests/ms/set/basic.ms`：`{1, 2, 3}` 构建与字面量去重（`{1, 1, 2}` 长度为 2）；`set()` 空集（`{}` 是 dict 的类型断言）；`len`；空集真值为假；`add`/`remove`/`contains` 与 `in`/`not in`；数值统一（`{1, 1.0, true}` 长度为 1）；元素类型混用（int/str/bytes/tuple）。
- `tests/ms/set/ops.ms`：`union`/`intersect`/`diff` 三运算的结果断言（经 `==` 与长度比对）且原操作数不变；空集参与的运算；参数非 set 抛 `TypeError`；`remove` 缺失元素抛异常；`s[0]` 抛 `TypeError`；set 不可哈希（`{s}`、`m[s] = 1`、`hash(s)` 均抛 `TypeError`）。
- `tests/ms/set/convert.ms`：`set([1, 2, 2])`、`set((1, 2))`、`set(s)` 浅拷贝后互不影响、`set({"k": 1})` 装入键；`set(1)` 抛 `TypeError`。
- `tests/ms/cross/hashable_keys.ms`：bytes/tuple 作 dict 键与 set 元素的交叉场景——`{b"k": 1, (1, 2): 2}` 读写、del、二次哈希稳定（哈希缓存路径回归）。
- 负例补充：错误消息文本不纳入断言（`run_tests.py` 不比对 stderr），实现时人工抽查一次各 `TypeError`/`ValueError`/`IndexError` 消息内容。

每个测试文件末尾以 `print("<name> ok")` 收尾；`try/except` 负例在 `except` 块断言异常类型、在未触发异常的路径上显式 `assert(false)`，确保"应抛未抛"能暴露。

## 验收标准

- [ ] `src/object/ms_bytes.{c,h}`、`src/object/ms_tuple.{c,h}`、`src/object/ms_set.{c,h}` 存在，guard 分别为 `MSLANG_SRC_OBJECT_MS_BYTES_H_` / `MSLANG_SRC_OBJECT_MS_TUPLE_H_` / `MSLANG_SRC_OBJECT_MS_SET_H_`，头文件自包含；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] [09-c-api.md](../language/09-c-api.md) §5 的 `msNewBytes`/`msNewTuple` 落地且语义与文档一致；`msNewTuple` 预填 `nil` 的 GC 安全不变量成立。
- [ ] `b"..."` 字面量经常量池正确求值（转义解码与任务 03 的 `msLexerUnescape` 一致）；`MS_OP_BUILD_TUPLE`/`MS_OP_BUILD_SET` 与三个类型在 `MS_OP_INDEX`/`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX`/`MS_OP_IN`/`MS_OP_ADD`/`MS_OP_MUL`/`MS_OP_UNPACK` 中的分支接通，语义与本文「详细设计」§5 一致。
- [ ] 不可变性成立：bytes/tuple 无脚本可达的写路径（下标写/删抛 `TypeError`）；bytes 哈希与 tuple 哈希正确缓存，tuple 含不可哈希元素时抛 `TypeError` 且不缓存。
- [ ] set 哈希表正确：去重、数值统一（`1`/`1.0`/`true` 同元素）、增删查与扩容压实后行为一致；`union`/`intersect`/`diff` 返回新集且不修改操作数；set 不可哈希。
- [ ] 方法集与内建接线完整：`bytes.toString()`、`str.toBytes()`、`set.add/remove/contains/union/intersect/diff`；`len`/`hash`/`repr`/`bytes(x)`/`tuple(x)`/`set(x)` 对三个新类型的分派正确；空值真值为假；bytes/tuple 可作 dict 键。
- [ ] 全部错误点经任务 23 的异常系统抛出 `TypeError`/`ValueError`/`IndexError` 对应对象；tuple/set 的 GC 子对象遍历挂接正确，跨分配局部对象遵守根纪律；Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告；构建产物只落在 `build/`。
- [ ] 「测试方案」全部清单项实现并全数通过，`python run_tests.py` 退出码为 0。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过。
- [ ] 无 TBD/TODO 占位；与任务 04/06/07/08/10/17/23 的接口假定（`msObjEqual`/`msObjHash`/真值与 repr 分派/GC 遍历槽/异常抛出函数等）在实现时已对齐对应任务文档的定名。
