# 16 容器 list 与 dict

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [06 对象模型基础](06-object-model.md)、[09 最小可运行解释器](09-minimal-interpreter.md) |

## 任务目标

交付 mslang 的两个可变容器类型——`list`（动态数组）与 `dict`（保持插入序的哈希表）——的对象实现与 VM 语义接线：

- 对象模块 `src/object/ms_list.{c,h}` 与 `src/object/ms_dict.{c,h}`：`struct MsList` / `struct MsDict` 的存储结构、扩容策略、查找/插入/删除算法；
- 公开 C API 的容器部分落地（[09-c-api.md](../language/09-c-api.md) §5 的 `msNewList`/`msNewDict` 与 §6 的 `msLen`/`msListGet`/`msListSet`/`msListAppend`/`msDictGet`/`msDictSet`/`msDictContains`/`msDictDelete`）；
- VM 容器指令的语义实现：`MS_OP_BUILD_LIST` / `MS_OP_BUILD_DICT` / `MS_OP_APPEND` / `MS_OP_INDEX` / `MS_OP_SET_INDEX` / `MS_OP_DEL_INDEX` / `MS_OP_IN`（[08-vm-internals.md](../language/08-vm-internals.md) §2.2），指令分派骨架属任务 08，本任务填充 list/dict 两个分支；
- 容器方法集：`xs.append/insert/pop/remove` 与 `m.get/pop/keys/values/items/update`，以及 `len(xs)`、`"a" in m`、按值 `==` 的完整语义。

完成后，脚本能写 `xs := [1, "two", 3.0]`、`m := {"a": 1}`，做下标读写（含负索引）、`del m["a"]`、键成员测试与方法调用，并通过 `tests/ms/containers/` 的 ms 脚本验证。切片（`xs[1:3]`）属任务 30，bytes/tuple/set 属任务 32，迭代协议（`for x in xs`）属任务 26，均不在本任务范围。

## 设计依据

- [02-types.md](../language/02-types.md)
  - §1 类型总表：`list` 可变动态数组、`dict` 可变哈希表且保持插入序（迭代顺序即插入顺序）。
  - §2 真值规则：空容器 `[]`、`{}` 为假，其余为真。
  - §5.1 list 语义与示例：`append(4)`、`insert(0, "x")`、`pop()`、`remove("two")`、`len(xs)`。
  - §5.3 dict 语义与示例：字面量 `{k: v}`、下标写入、`del m["a"]`、`"a" in m`、方法 `keys() values() items() get(k, default) pop(k) update(other)`；键必须可哈希（不可变类型 + 定义 `__hash__` 的实例）。
  - §6 相等与哈希：`==` 按值比较；`int/float/bool/str/bytes/tuple` 递归按值且可哈希；`list/dict/set` 不可哈希。
- [03-syntax.md](../language/03-syntax.md) §6.1：下标 `a[i]`，负索引从尾部计数；切片 `a[i:j:k]` 本任务不实现（任务 30「切片与下标完善」）。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.2：容器类指令清单；§3：对象模型（`MsObjectHeader`、`MsTypeTag` 中已有 `MS_TYPE_LIST`/`MS_TYPE_DICT`，所有值都是装箱 `MsObject*`）。
- [09-c-api.md](../language/09-c-api.md) §3：GC 根纪律（`msRootPush`/`msRootPop`，跨分配的局部 `MsObject*` 必须入根）；§5：值构造；§6：容器操作公开 API 签名；§8：错误处理约定（返回 `NULL` + 错误槽，`msRaiseTypeError` 等）；§9：`MsCFunction` 与 `MsMethodDef` 方法表定义。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R、指针星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`、include guard）。
- [11-project-layout.md](../language/11-project-layout.md) §1：`src/object/` 与 `tests/ms/` 的目录位置；§5 路线图 v0.1：核心类型含 list/dict，v0.1 的 int 仅 int64（大整数属任务 31）。
- 任务 02（核心基础设施）提供 `msAlloc`/`msRealloc`/`msFree` 与 `MsResult`；任务 06（对象模型基础）提供 `MsObjectHeader`、`MsType` 类型对象与方法表机制、按值比较 `msObjEqual`、哈希 `msObjHash`、真值判定分派；任务 08（VM 执行核心）提供容器指令的分派点；任务 10（内建函数）提供 `len` 内建与内建方法注册通路。其中任务 06、08、10 的文档尚未定稿，本文引用的接口名（`msObjEqual`、`msObjHash`、类型对象方法表注册函数等）为假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件与模块边界

- `src/object/ms_list.h`（guard `MSLANG_SRC_OBJECT_MS_LIST_H_`）与 `src/object/ms_list.c`：list 存储结构与全部操作。
- `src/object/ms_dict.h`（guard `MSLANG_SRC_OBJECT_MS_DICT_H_`）与 `src/object/ms_dict.c`：dict 存储结构与全部操作。
- 两个头文件均自包含，包含 `<stdbool.h>` `<stdint.h>` 与任务 06 的对象模型头文件（假定 `"object/ms_object.h"`）。公开 C API（`msNewList` 等）声明在 `include/mslang/object.h` / `include/mslang/container.h`，其实现落在本任务的两个 `.c` 文件中。
- VM 侧只在本任务修改各容器指令的 `case` 分支（任务 08 的分派循环内），调用本模块的语义函数；指令编码、取栈/压栈约定不变。
- 方法与 `len` 的挂接：list/dict 的类型对象在 `msNewState` 初始化时创建（任务 06），本任务向其方法表注册下述 `MsMethodDef` 数组；`len` 内建（任务 10）对 `MS_TYPE_LIST`/`MS_TYPE_DICT` 的分派在本任务补齐。

### 2. MsList：动态数组

```c
struct MsList {
  MsObjectHeader header;
  MsObject** items;      // msAlloc'd element buffer, capacity slots
  int64_t    len;        // element count, 0 <= len <= capacity
  int64_t    capacity;   // allocated slots
};
```

不变量：`items == NULL` 当且仅当 `capacity == 0`；`len <= capacity`。

- 扩容策略：`capacity == 0` 时首次扩容到 4；此后 `len == capacity` 时倍增（4 → 8 → 16 …）。经 `msRealloc` 就地扩；OOM 时 `msRealloc` 返回失败，调用方走统一 OOM 路径，原缓冲区保持不变。
- 缩容策略：v0.1 不做缩容——`pop`/`remove`/`del xs[i]` 只减 `len`，不归还缓冲区（避免反复抖动；缩容列入性能路线图，见 [08-vm-internals.md](../language/08-vm-internals.md) §7）。
- 负索引归一化：脚本层索引 `i` 在进入存储前统一经 `msListNormalizeIndex(list->len, i)` 处理——`i < 0` 时加 `len`；归一化后仍在 `[0, len)` 外即越界。
- 元素所有权：容器持有对元素的强引用语义（无引用计数；任务 17 的 GC 标记时遍历 `items[0..len)`）。`pop`/`remove`/覆盖写只调整数组内容，不释放元素对象。

公开与模块内接口（`MsObject*` 参数为调用方持有的根，返回值遵循 09-c-api §3 的转移语义——立即消费或入根）：

```c
// Public C API implementations (declared in include/mslang/).
// msListGet returns NULL and raises IndexError-style error when out of range.
MsObject* msNewList(MsState* L, int64_t capacity);
MsObject* msListGet(MsState* L, MsObject* list, int64_t index);
void      msListSet(MsState* L, MsObject* list, int64_t index, MsObject* v);
void      msListAppend(MsState* L, MsObject* list, MsObject* v);

// Module-internal operations on the concrete struct (src/object/ms_list.h).
// All index parameters accept negative indices (normalized internally).
MsResult  msListInsert(MsState* L, struct MsList* list, int64_t index, MsObject* v);
MsObject* msListPop(MsState* L, struct MsList* list);              // removes the tail; error when empty
bool      msListRemove(MsState* L, struct MsList* list, MsObject* v);  // first occurrence by msObjEqual
bool      msListContains(MsState* L, struct MsList* list, MsObject* v);
bool      msListEqual(MsState* L, struct MsList* a, struct MsList* b);

// Normalizes i into [0, len) accounting for negatives; returns false when out of range.
bool msListNormalizeIndex(int64_t len, int64_t i, int64_t* out);
```

- `msListInsert`：`index` 归一化后钳制到 `[0, len]`（越界不报错，插到端点，对齐 Python `list.insert` 语义）；`memmove` 右移尾部一格后写入。
- `msListPop`：空表时报运行时错误（"pop from empty list"）；否则返回 `items[--len]`。
- `msListRemove`：线性扫描，首个 `msObjEqual(v)` 为真的元素左移覆盖删除；未找到时报运行时错误（"list.remove(x): x not in list"），返回 `false` 且错误槽已置。
- `msListEqual`：长度不等即 `false`；逐元素 `msObjEqual` 递归按值比较；`a == b`（同指针）快速路径直接 `true`，同时作为自引用结构的递归终止保护（v0.1 简化：不检测更一般的引用环，极端嵌套自引用导致栈溢出属已知限制，列入路线图评估）。
- `msListContains`：与 `msListRemove` 同一扫描，只判存在性，供 `MS_OP_IN` 使用。

### 3. MsDict：保持插入序的哈希表

采用 Python 3.7 式组合表（combined table 的稠密化变体）：**稠密条目数组 + 稀疏索引数组**。插入序由稠密数组的下标天然保证，无需额外链表。

```c
struct MsDictEntry {
  MsObject* key;      // NULL = deleted slot (tombstone)
  MsObject* value;    // undefined when key == NULL
  uint32_t  hash;     // cached key hash, avoids rehash on resize
};

struct MsDict {
  MsObjectHeader      header;
  struct MsDictEntry* entries;    // dense, insertion-ordered
  int64_t             entriesLen; // capacity of entries (power-of-two growth)
  int64_t             used;       // live (non-deleted) entries
  int64_t             fill;       // used + deleted entries
  int32_t*            indices;    // sparse open-addressing table, size = mask + 1
  int64_t             mask;       // indices size - 1; -1 when indices == NULL (empty dict)
};
```

- 索引数组长度为 2 的幂，`indices` 槽存稠密下标 + 1，`0` 表示空槽（避免单独的空标记常量）。空字典 `entries == NULL`、`indices == NULL`，首次插入时初始化：索引数组 8 槽、稠密数组 8 条。
- 探测序列：`slot = hash & mask`，冲突时 `slot = (slot * 5 + hash + 1) & mask`（Python 同款 perturb 简化版，保证遍历全部槽位）。
- 负载因子：`fill * 3 >= (mask + 1) * 2`（约 2/3）时扩容：索引数组与稠密数组均倍增；扩容同时**压实**稠密数组——丢弃墓碑条目、按插入序重排存活条目、以缓存的 `hash` 重建索引数组（不重新求哈希）。压实保证墓碑不无限累积。
- 删除：置 `entries[ix].key = NULL`（墓碑），`used--`，`fill` 不变；查找遇墓碑跳过，插入遇墓碑复用（`fill` 不变、`used++`）。`msDictDelete` 删除不存在的键时报运行时错误（供 `del m[k]` 与方法 `pop(k)` 区分调用）。
- 替换语义：已存在键的 `msDictSet` 只覆盖 `value`，**不改变插入位置**（保持插入序承诺）。
- 键的哈希与相等：经任务 06 的 `msObjHash(MsObject*)` / `msObjEqual(MsObject*, MsObject*)` 分派。本任务对其提出两条硬性要求（由任务 06 保证，此处显式声明为契约）：
  1. 数值统一：`1`、`1.0`、`true` 互相 `==` 且哈希相同（Python 规则），故 `m[1]` 与 `m[1.0]` 是同一个键；
  2. 不可哈希键（`list`/`dict`/`set`/未定义 `__hash__` 的实例）调用 `msObjHash` 时报 TypeError 式运行时错误（"unhashable type: 'list'"）——错误在键入口处产生，dict 自身不做类型特判。

```c
// Public C API implementations (declared in include/mslang/).
// msDictGet returns nil (not an error) when the key is missing.
MsObject* msNewDict(MsState* L);
MsObject* msDictGet(MsState* L, MsObject* dict, MsObject* key);
void      msDictSet(MsState* L, MsObject* dict, MsObject* key, MsObject* v);
bool      msDictContains(MsState* L, MsObject* dict, MsObject* key);
void      msDictDelete(MsState* L, MsObject* dict, MsObject* key);  // error when missing

// Module-internal operations (src/object/ms_dict.h).
bool      msDictEqual(MsState* L, struct MsDict* a, struct MsDict* b);
// Copies all live entries of src into dst, preserving src's insertion order.
MsResult  msDictUpdate(MsState* L, struct MsDict* dst, struct MsDict* src);
// Removes key and returns its value; returns NULL with error slot set when missing.
MsObject* msDictPop(MsState* L, struct MsDict* dict, MsObject* key);
// Iteration over live entries in insertion order: pass *cursor = 0 to start;
// returns false when exhausted. Callback-free design for VM/builtin use.
bool      msDictNext(struct MsDict* dict, int64_t* cursor, MsObject** key, MsObject** value);
```

- `msDictEqual`：`used` 不等即 `false`；对 `a` 的每个存活键在 `b` 中查找（须用与插入相同的哈希/相等契约），值经 `msObjEqual` 递归比较。同指针快速路径 `true`（同 3.2 节的递归保护约定）。
- `msDictNext` 是本模块唯一的迭代原语：`keys()`/`values()`/`items()`/`update` 与后续的 `for k, v in m`（任务 26）都建立在它之上。迭代期间**禁止结构性修改**（插入/删除/扩容）；v0.1 不做修改检测，行为未定义并在文档注明（修改检测的 version 标记列入路线图）。
- 内存：两块堆缓冲区（`entries`、`indices`）均经 `msAlloc`/`msRealloc`/`msFree`；扩容失败保持旧表可用，走统一 OOM 路径。

### 4. 下标读写与容器指令的语义接线

各指令在任务 08 的分派循环中按栈顶对象类型分派；本任务实现 `MS_TYPE_LIST`/`MS_TYPE_DICT` 两个分支（其他类型的下标语义——`str[i]` 等——由对应类型的任务负责）：

| 指令 | list 分支 | dict 分支 |
|---|---|---|
| `MS_OP_INDEX` | `msListGet`：负索引归一化，越界报 "list index out of range" | `msDictGet`；**键缺失报运行时错误**（"KeyError: <repr>"），与返回 nil 的 C API 语义区分：VM 分支先 `msDictContains` 判定，缺失才报错 |
| `MS_OP_SET_INDEX` | `msListSet`：同样越界检查 | `msDictSet`：存在则原位替换，不存在则按插入序追加 |
| `MS_OP_DEL_INDEX`（`del a[i]`） | 归一化 + 越界检查后左移删除 | `msDictDelete`，键缺失报错 |
| `MS_OP_IN`（`v in c` / `not in`） | `msListContains` 线性按值扫描 | `msDictContains` 键查找（含不可哈希键错误传播） |
| `MS_OP_BUILD_LIST n` | 弹 n 个元素，按栈序（即字面量序）填入 `msNewList(L, n)` | — |
| `MS_OP_BUILD_DICT n` | — | 弹 2n 个值（k, v 交替），按字面量序 `msDictSet` 填入 `msNewDict`；同键后者原位覆盖 |
| `MS_OP_APPEND` | `msListAppend`（推导式构建用，本任务接通指令，推导式语法属任务 27） | — |

- 越界、缺键、不可哈希键、类型错误（如对 int 下标）在 v0.1 均无异常对象：经 `msRaise*` 置错误槽，VM 中止执行返回 `MS_ERROR_RUNTIME`，CLI 退出码 1（任务 09 约定）。异常系统落地（任务 23）后这些错误点改为抛出对应异常对象，错误消息文本保持不变。
- 真值接线：任务 06 的真值分派对 `MS_TYPE_LIST`/`MS_TYPE_DICT` 分别以 `len == 0` / `used == 0` 判定，本任务提供 `msListLen`/`msDictLen` 内联访问器（头文件 `static inline`，直接读字段）。
- 切片操作数（`MS_OP_SLICE`）遇到 list 时分派到任务 30 的实现；本任务中该指令对 list/dict 一律报 "not subscriptable with slice" 占位错误，任务 30 落地后替换。

### 5. 容器方法集

方法以 `MsCFunction` 实现（签名见 09-c-api §9），按任务 06 的方法表机制以 `static const MsMethodDef` 数组注册到 `list`/`dict` 类型对象；脚本侧经 `MS_OP_LOAD_METHOD`/`MS_OP_CALL_METHOD` 取得绑定方法（`MS_TYPE_BOUND_METHOD`）调用。`argv[0]` 约定为接收者（self）。参数个数与类型不符时报 TypeError 式错误并返回 `NULL`。

list 方法表 `msListMethods`：

| 方法 | 语义 | 错误 |
|---|---|---|
| `append(v)` | 尾部追加，返回 nil | OOM |
| `insert(i, v)` | `msListInsert`（负索引归一化 + 钳制），返回 nil | `i` 非 int 报 TypeError |
| `pop()` | `msListPop`，返回尾元素 | 空表报 "pop from empty list" |
| `remove(v)` | `msListRemove`，返回 nil | 未找到报 "x not in list" |

dict 方法表 `msDictMethods`：

| 方法 | 语义 | 错误 |
|---|---|---|
| `get(k)` / `get(k, default)` | 命中返回值，未命中返回 default（缺省 nil）；**不报错** | 键不可哈希照常报错 |
| `pop(k)` / `pop(k, default)` | 命中则删除并返回其值；未命中时有 default 返回 default，否则报 "KeyError" 式错误 | 同上 |
| `keys()` | 按插入序把所有键装入新 list 返回 | OOM |
| `values()` | 同上，装值 | OOM |
| `items()` | 同上，每个条目装为 2 元素 list `[k, v]`（tuple 在任务 32 才存在；届时是否切换为 2 元素 tuple 由任务 32 决定，本任务固定为 list） | OOM |
| `update(other)` | `other` 须为 dict；按 `other` 的插入序逐条 `msDictSet`，返回 nil | `other` 非 dict 报 TypeError |

- `keys()`/`values()`/`items()` 返回的是**新 list 快照**（非视图），对结果 list 的修改不影响原 dict；迭代期间修改原 dict 的行为见 3.3 节 `msDictNext` 的约定。
- 方法实现复用第 2、3 节的模块内函数，自身只做参数校验与装箱/拆箱。
- `len(xs)`：任务 10 的内建 `len` 增加 `MS_TYPE_LIST`/`MS_TYPE_DICT` 分派，分别取 `len` 与 `used`。
- 字面量 `[]`/`{}`/`{k: v}` 的词法与语法已属任务 03/04 的范围（`{}` 恒为空 dict，与 [02-types.md](../language/02-types.md) §5.4 一致）；本任务只保证 `MS_OP_BUILD_*` 执行正确。

### 6. 相等与哈希接线

- 任务 06 的 `msObjEqual` 分派表为 `MS_TYPE_LIST`/`MS_TYPE_DICT` 注册 `msListEqual`/`msDictEqual`，使 `[1, [2]] == [1, [2]]` 与 `{"a": [1]} == {"a": [1]}` 递归按值成立。dict 相等**不**要求插入序一致（键集合与逐键值相等即可），与 Python 一致。
- list/dict 不可哈希：不在 `msObjHash` 分派表中注册，落到任务 06 的默认分支报 "unhashable type"。因此 `[1] in m`、 `m[[1]] = 2`、`d := {[1]: 2}` 字面量均为运行时错误。
- `nil`/`bool`/`int`/`float`/`str` 作为键的哈希与相等由任务 06 实现；本任务的 dict 测试覆盖这些键类型的混用（含 `1`/`1.0`/`true` 同键契约）。

### 7. 内存与 GC 纪律

- 全部堆分配经任务 02 的 `msAlloc`/`msRealloc`/`msFree`；容器析构（任务 17 GC 落地前的 `msCloseState` 全量回收路径，见任务 09）按逆序释放 `indices`、`entries`/`items` 与对象本体。
- 根纪律（09-c-api §3）：`msListAppend`/`msDictSet` 等可能触发扩容分配的函数，其 `v`/`key`/`value` 参数由调用约定保证是根（VM 求值栈上的值天然是根）；跨分配存活的局部 `MsObject*`（如 `MS_OP_BUILD_DICT` 循环中已弹出栈的键值对）必须 `msRootPush` 暂存、用完 `msRootPop`（LIFO）。`keys()`/`items()` 构建期间，结果 list 与中间条目同样入根。
- 为任务 17 留口：容器的子对象遍历点（list 的 `items[0..len)`、dict 存活条目的 key/value）集中在本模块的两个 `static` 辅助函数中，GC 标记阶段直接复用，不在 VM 中散落字段访问。

## 实现步骤

1. 建 `src/object/ms_list.h` / `ms_list.c` 骨架：`struct MsList`、`msNewList`、容量管理（含倍增扩容与 OOM 路径）、`msListGet`/`msListSet`/`msListAppend` 与负索引归一化。验证：ms 脚本 `[1, 2, 3]` 字面量构建 + `xs[0]`/`xs[-1]` 读取 + 越界负例脚本退出码 1。
2. 接通 `MS_OP_BUILD_LIST`、`MS_OP_INDEX`、`MS_OP_SET_INDEX`、`MS_OP_DEL_INDEX` 的 list 分支与真值/`len` 分派。验证：字面量、下标读写、`del xs[i]`、`len(xs)`、空表真值为假的脚本断言。
3. 实现 `msListInsert`/`msListPop`/`msListRemove`/`msListContains` 与 list 方法表注册（`append`/`insert`/`pop`/`remove`）。验证：方法行为与参数错误的脚本断言（含负例退出码 1）。
4. 建 `src/object/ms_dict.h` / `ms_dict.c` 骨架：`struct MsDict`、组合表的查找/插入/删除/扩容压实算法、`msNewDict` 与四个公开 C API、`msDictNext`。验证：`{"a": 1}` 构建、`m["a"]` 读写、键缺失报错、插入序经 `keys()` 打印比对。
5. 接通 `MS_OP_BUILD_DICT`、`MS_OP_INDEX`/`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX`、`MS_OP_IN` 的 dict 分支与 `MS_OP_IN` 的 list 分支。验证：`"a" in m`、`1 in xs`、`not in`、`del m["k"]` 的脚本断言。
6. 实现 dict 方法表（`get`/`pop`/`keys`/`values`/`items`/`update`）与 `msDictUpdate`/`msDictPop`。验证：每个方法的正常路径、缺省参数形式与错误路径脚本断言。
7. 注册 `msListEqual`/`msDictEqual` 到 `msObjEqual` 分派，实现同指针快速路径。验证：嵌套容器按值相等/不等、`m1 == m2` 与插入序无关、数值键统一（`m[1]` 与 `m[1.0]` 同键）的脚本断言。
8. 根纪律复查与内存收尾：构建与方法实现中跨分配的局部对象入根核对；`msCloseState` 后分配计数归零（任务 09 的统计口径）。验证：Debug 构建（ASAN / `/RTC`）跑全部容器测试无报告。
9. 全平台（Win/Linux/macOS）× Debug/Release 构建验证，`run_tests.py` 全绿。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试（`testing` 模块在任务 40 才存在，本阶段用内建 `assert` + `print` 自断言；负向用例以 `<name>.exit` 同伴文件声明预期退出码 1，由 `run_tests.py` 驱动，见任务 09 的设施约定）。本任务只交付设计文档，脚本随实现编写。

测试文件清单（`tests/ms/containers/`）与覆盖点：

- `list_basic.ms`：字面量（空表、异构元素 `[1, "two", 3.0]`）、`len`、正/负索引读、`xs[i] = v` 写、越界读写的负例在独立脚本；真值（`assert(not [])`、空表为假）；末尾 `print("list basic ok")`。
- `list_methods.ms`：`append` 扩容跨多次倍增（如追加 100 个元素后逐值断言）、`insert(0, v)`/`insert(-1, v)`/越界钳制、`pop()` 返回尾元素、`remove(v)` 删首个匹配、删除不存在值的负例（独立脚本）、`del xs[i]`（含负索引）；嵌套 list 的元素操作。
- `dict_basic.ms`：字面量（空 `{}`、多键）、下标读写、原位替换不改插入序、`del m["k"]`、`len(m)`、空 dict 真值为假；键类型混用（int/float/bool/str/nil 若可哈希则以任务 06 定名为准）；`m[1]` 与 `m[1.0]`、`m[true]` 同键契约。
- `dict_order.ms`：交错插入/替换/删除后重建，`keys()`/`values()`/`items()` 的返回顺序严格等于存活键的插入序（压实后仍保持）；与固定期望 list 用 `==` 比对。
- `dict_methods.ms`：`get(k)`/`get(k, d)` 命中与未命中两路径、`pop(k)`/`pop(k, d)`、`update(other)` 的追加与覆盖语义、`items()` 返回 2 元素 list 的 list 且元素可再下标。
- `membership.ms`：`v in xs`（按值线性扫描，含嵌套 list 元素）、`k in m`、`not in`；对非容器使用 `in` 的负例（独立脚本）。
- `equality.ms`：`[1, [2, "x"]] == [1, [2, "x"]]`、长度不等、元素不等、dict 相等与插入序无关、dict 值不等、list 与 dict 互不相等、`==` 不修改操作数。
- 负例脚本（各配 `<name>.exit` 声明退出码 1）：`list_index_oob.ms`（越界读/写/`del`）、`list_pop_empty.ms`、`list_remove_missing.ms`、`dict_key_missing.ms`（下标读与 `del`）、`dict_unhashable_key.ms`（`m[[1]] = 2` 与 `{[1]: 2}` 字面量）、`method_type_error.ms`（如 `xs.append()` 缺参、`m.update(1)`）。

每个负例同时用 `run_tests.py` 的退出码检查覆盖到错误槽 → `MS_ERROR_RUNTIME` → CLI 退出码 1 的完整链路；错误消息文本不纳入断言（run_tests.py 设施不比对 stderr），但实现时应人工抽查一次消息内容。

## 验收标准

- [ ] `src/object/ms_list.{c,h}` 与 `src/object/ms_dict.{c,h}` 存在，guard 分别为 `MSLANG_SRC_OBJECT_MS_LIST_H_` / `MSLANG_SRC_OBJECT_MS_DICT_H_`，头文件自包含；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsList`/`struct MsDict` 不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] [09-c-api.md](../language/09-c-api.md) §5/§6 的 list/dict 公开 API（`msNewList`/`msNewDict`/`msListGet`/`msListSet`/`msListAppend`/`msDictGet`/`msDictSet`/`msDictContains`/`msDictDelete`）全部落地且语义与文档一致（`msDictGet` 缺键返回 nil）。
- [ ] `MS_OP_BUILD_LIST`/`MS_OP_BUILD_DICT`/`MS_OP_APPEND`/`MS_OP_INDEX`/`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX`/`MS_OP_IN` 的 list 与 dict 分支接通，负索引归一化正确，越界/缺键/不可哈希键经错误槽以退出码 1 失败。
- [ ] dict 组合表保持插入序：替换不改序、删除后经压实重建仍保序，`keys()`/`values()`/`items()` 顺序与插入序一致（脚本断言覆盖）。
- [ ] 方法集完整且语义与 [02-types.md](../language/02-types.md) §5.1/§5.3 一致：`append`/`insert`/`pop`/`remove` 与 `get`（含 default）/`pop`（含 default）/`keys`/`values`/`items`（2 元素 list）/`update`；`len` 对两种容器正确；空容器真值为假。
- [ ] `==` 对 list/dict 递归按值比较，dict 相等与插入序无关；list/dict 作键报 "unhashable type"；数值键统一契约（`1`/`1.0`/`true` 同键）成立。
- [ ] 跨分配存活的局部 `MsObject*` 全部遵守根纪律；`msCloseState` 后无残余分配计数；Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告；构建产物只落在 `build/`。
- [ ] `tests/ms/containers/` 下「测试方案」全部清单项实现并全数通过，`python run_tests.py` 退出码为 0；负例脚本以预期退出码 1 失败。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过。
- [ ] 无 TBD/TODO 占位；与任务 06/08/10 的接口假定（`msObjEqual`/`msObjHash`/方法表注册等）在实现时已对齐对应任务文档的定名。
