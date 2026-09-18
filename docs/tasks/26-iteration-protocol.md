# 26 for-in 迭代协议

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [12 控制流语句](12-control-flow.md)、[23 异常系统](23-exceptions.md) |

## 任务目标

交付 mslang 的迭代协议与 `for x in xs` 语句的完整实现，覆盖 [02-types.md](../language/02-types.md) §7 与 [03-syntax.md](../language/03-syntax.md) §3.1：

- **协议运行时**（新模块 `src/object/ms_iterator.{c,h}`）：`struct MsIterator` 迭代器对象模型（`MS_TYPE_ITERATOR`）、协议入口 `msIterGetIter`/`msIterNext`（原生容器快路径 + 实例 `__iter__`/`__next__` 慢路径的统一分派）；
- **内建可迭代对象的迭代器**：`list`（下标游标）、`dict`（插入序键值对）、`str`（按 Unicode 码点）；`bytes`/`tuple`/`set` 的迭代器分支由任务 32 在本任务的机制上补注册；
- **惰性 `range`**：新增 `struct MsRange` 与类型标签 `MS_TYPE_RANGE`，替换任务 10 的物化 list 实现，支持 `len`/整数下标/重复取迭代器；
- **指令定稿与 VM 接线**：`MS_OP_GET_ITER`/`MS_OP_ITER_NEXT`/`MS_OP_UNPACK` 的语义与栈约定（任务 05 编码、任务 27 已假定）在本任务定稿，另新增 `MS_OP_POP` 供循环退出的栈清理；
- **for-in 代码生成**：编译器 `MS_FOR_IN` 分支的循环模板、目标绑定（含解包）、`break`/`continue` 出口、循环变量作用域与迭代级闭包捕获；
- **StopIteration 衔接**：`for`（与任务 27 推导式）只消化迭代器耗尽处的 `StopIteration`，循环体内显式抛出的 `StopIteration` 照常传播；显式 `next()` 不消化；
- **迭代相关内建**：`iter`/`next`（含默认值形式）、`enumerate`/`zip`/`map`/`filter`（惰性迭代器）、`sorted`/`reversed`——任务 10 已把这一组显式排到「依赖迭代协议」。

完成后脚本可写 `for x in xs`、`for k, v in m`、`for i, x in enumerate(xs)`，可用 `iter`/`next` 手工驱动迭代并以 `except StopIteration` 兜底，自定义 class 实现 `__iter__`/`__next__` 即接入 `for-in`。本任务只交付设计文档；实现与测试代码随实现任务编写。

## 设计依据

- [02-types.md](../language/02-types.md)
  - §1 类型总表：`iterator` 为内建类型（协议产物）；类型表中无 `range`，本任务新增 `MS_TYPE_RANGE` 属对规范的显式扩展（见「详细设计」§3）。
  - §7 迭代协议：`iter(xs)` 调用 `xs.__iter__()`；`next(it)` 调用 `it.__next__()`，耗尽抛 `StopIteration`；`next(it, d)` 带默认值；`for x in xs` 是该协议的语法糖；`range(start, stop, step)` 返回惰性序列对象。
- [03-syntax.md](../language/03-syntax.md)
  - §3.1：`for x in xs`、`for i, x in enumerate(xs)`、`for k, v in m`（dict 迭代产出键值对）；**`for` 无 `else` 子句**（刻意删减，编译器不实现）。
  - §3：解包赋值「右侧必须是可迭代对象，长度须匹配」——`MS_OP_UNPACK` 的通用语义据此定稿。
  - §8：块级作用域（`{}` 引入新作用域，含 `for` 块）；闭包按引用捕获。
  - §9：内建清单中的 `iter(x) next(it, default) range(...) enumerate(x, start=0) zip(a, b, ...) map(f, x) filter(f, x) sorted(x, key=nil, reverse=false) reversed(x)`。
- [04-exceptions.md](../language/04-exceptions.md) §4：`StopIteration` 在异常层级中直属 `Exception`；§5：`StopIteration` 在 `for` 循环与推导式中被自动捕获并终止迭代，显式 `next()` 由调用者自行处理。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.2：迭代指令 `MS_OP_GET_ITER` `MS_OP_ITER_NEXT`（**失败时跳转，避免 StopIteration 异常开销**）`MS_OP_UNPACK`；指令总数目标 ≤ 80；§3：`MS_TYPE_ITERATOR` 标签已存在。
- [任务 05 字节码格式与 MsProto](05-bytecode-proto.md)：三条迭代指令的编码已登记——`GET_ITER` 为 ABC 无操作数（弹可迭代对象压迭代器）、`ITER_NEXT` 为 sAx（迭代器在栈顶，耗尽时跳转、否则压入下一元素）、`UNPACK` 为 ABC（A = 解包目标个数）。本任务的语义定稿不得偏离该编码。
- [任务 23 异常系统](23-exceptions.md)：`msVmRaise`/`msVmRaiseFmt` 抛出入口、`msExceptionIsA` 类型匹配、`msExceptionTypeOf(L, MS_EXC_STOP_ITERATION)` 取类型对象；本任务全部错误点（`TypeError`/`ValueError`/`IndexError`/`StopIteration`）经其抛出真实异常对象。
- [任务 16 容器 list 与 dict](16-containers-list-dict.md)：`struct MsList`/`struct MsDict` 布局；`msDictNext` 游标迭代原语与「迭代期间禁止结构性修改、不检测」的约定；`items()` 以 2 元素 list 产出键值对的先例（tuple 属任务 32）。
- [任务 10 内建函数](10-builtin-functions.md)：内建注册表 `msBuiltinTable` 机制；`range` 的物化 list 实现（本任务替换）与参数规则（1/2/3 参、仅 int、`step == 0` 报 `ValueError`）；`enumerate/zip/map/filter/sorted/reversed/iter/next` 已声明「依赖迭代协议（任务 26）」。
- [任务 27 推导式](27-comprehensions.md)：已假定本任务的指令栈约定（`GET_ITER` 弹一物压一器；`ITER_NEXT sAx` 成功压值顺序执行、耗尽弹器跳转；`UNPACK A` 长度不符报运行时错误），本任务**按该假定定稿**，两文档互洽；其迭代级 `MS_OP_CLOSE_UPVALS` 约定同样适用于 for-in。
- 任务 07（编译器）、任务 08（VM 分派循环）、任务 12（控制流语句）、任务 13（调用约定，`msVmCallFunction` 假定名）、任务 14（upvalue 关闭）、任务 15/25（class 实例与魔术方法查找）、任务 17（GC 遍历挂接）的文档在本任务写作时部分尚未定稿，本文引用的其接口名（`struct MsCompiler` 作用域栈接口、`msCompilerLoopContext`、魔术方法查找入口、GC 逐类型遍历槽等）为假定命名，**实现时以对应任务文档定名为准**。

## 详细设计

### 1. 模块划分与文件

- 新增 `src/object/ms_iterator.h`（guard `MSLANG_SRC_OBJECT_MS_ITERATOR_H_`）与 `src/object/ms_iterator.c`：迭代器对象模型、原生迭代器快路径、惰性 range、协议入口 `msIterGetIter`/`msIterNext`。头文件自包含，include `<stdbool.h>` `<stdint.h>` 与 `"object/ms_object.h"`（任务 06）。
- 扩展 VM（任务 08，`src/vm/`）：`MS_OP_GET_ITER`/`MS_OP_ITER_NEXT`/`MS_OP_UNPACK` 的分派分支与新指令 `MS_OP_POP`。
- 扩展编译器（任务 07，`src/compiler/`）：`MS_FOR_IN` 节点的代码生成（语法与 AST 已由任务 04 落地：`forStmt { targets, iterable, body }`）。
- 扩展内建表（任务 10，`src/vm/ms_builtin.c`）：`msBuiltinTable` 追加 `iter`/`next`/`enumerate`/`zip`/`map`/`filter`/`sorted`/`reversed` 八项；`range` 实现整体替换。
- 全部错误经任务 23 的异常系统抛出（`msVmRaiseFmt`），不再使用任务 16 时期的「错误槽 + 退出码 1」路径。

### 2. 迭代器对象模型

```c
typedef enum {
  MS_ITER_LIST,          // source = list; cursor = 下标
  MS_ITER_DICT,          // source = dict; cursor = msDictNext 游标
  MS_ITER_STR,           // source = str; cursor = UTF-8 字节偏移
  MS_ITER_RANGE,         // source = NULL; current/stop/step 直存
  MS_ITER_ENUMERATE,     // source = 内层迭代器; cursor = 计数器
  MS_ITER_ZIP,           // source = 内层迭代器 list
  MS_ITER_MAP,           // source = 内层迭代器; aux = 映射函数
  MS_ITER_FILTER,        // source = 内层迭代器; aux = 谓词（nil 表真值过滤）
  MS_ITER_REVERSED       // source = list/str/range; cursor = 反向游标
} MsIterKind;

struct MsIterator {
  MsObjectHeader header;    // tag == MS_TYPE_ITERATOR
  MsIterKind kind;
  MsObject* source;         // 被迭代对象或内层迭代器；range 迭代器为 NULL
  MsObject* aux;            // map/filter 的函数；其余 kind 为 nil
  int64_t cursor;           // kind 相关游标（见上）
  int64_t current;          // MS_ITER_RANGE：当前值
  int64_t stop;             // MS_ITER_RANGE：终止值（不含）
  int64_t step;             // MS_ITER_RANGE：步长
};
```

- 单一结构体 + kind 枚举分派（不引入函数指针表）：种类封闭且全部内建，switch 分派与项目其余模块风格一致。实例迭代器（脚本 class 实现 `__next__`）**不**用本结构体——实例对象自身（`MS_TYPE_INSTANCE`）即迭代器，走 §4 的慢路径。
- `source`/`aux` 持有对象引用，须纳入 GC 遍历（§9）；`zip` 的内层迭代器集合以 list 装箱存入 `source`，遍历随之复用 list 的既有挂接。
- 迭代器是**一次性**对象：耗尽后停留于耗尽态，再次 `next` 仍报耗尽（`StopIteration`），不复位。

```c
// Allocates a native iterator of the given kind over source (may be NULL for
// MS_ITER_RANGE). Returns NULL on OOM.
struct MsObject* msIterNew(MsState* L, MsIterKind kind, struct MsObject* source);

// Protocol entry for iter(x): native containers and range produce a fresh
// iterator; an existing iterator returns itself; instances dispatch through
// __iter__ (task 25 magic-method lookup, assumed name msObjectLookupMagic);
// anything else raises TypeError ("object is not iterable: '<type>'").
MsResult msIterGetIter(MsState* L, struct MsObject* obj, struct MsObject** out);

// Advances iter. MS_OK with *hasValue == true: *out holds the next value.
// MS_OK with *hasValue == false: clean exhaustion, NO exception raised.
// MS_ERROR_RUNTIME: a non-StopIteration exception is propagating (raised by a
// script-level __next__ or by an element-access error).
MsResult msIterNext(MsState* L, struct MsObject* iter, struct MsObject** out, bool* hasValue);
```

`msIterNext` 的三态返回是本任务的核心约定：**干净耗尽不产生异常对象**（呼应 08-vm-internals §2.2「避免 StopIteration 异常开销」），`StopIteration` 只在脚本可见的 `next()` 内建与实例 `__next__` 边界物化。

### 3. 惰性 range

```c
struct MsRange {
  MsObjectHeader header;    // tag == MS_TYPE_RANGE（新增标签，见下）
  int64_t start;
  int64_t stop;
  int64_t step;             // != 0，构造时校验
};

// start/stop/step are validated by the caller (the range builtin).
struct MsObject* msRangeNew(MsState* L, int64_t start, int64_t stop, int64_t step);
// Element count, clamped to >= 0. Exact arithmetic, no overflow (int64 domain).
int64_t msRangeLen(const struct MsRange* r);
```

- **类型标签扩展（对任务 06 的显式修改点）**：`MsTypeTag` 在 `MS_TYPE_C_TYPE` 之后、`MS_TYPE_COUNT` 之前追加 `MS_TYPE_RANGE`，任务 06 的 `_Static_assert(MS_TYPE_COUNT == 20, ...)` 相应改为 21，内建类型对象表同步增建名称为 `"range"` 的类型对象（`type(range(3))` 输出 `<type 'range'>`）。02-types §1 未列 range 类型，此扩展在本任务显式承认。
- 语义对齐 Python 的 range：不可变、可重复迭代（`iter(r)` 每次产生**新** `MS_ITER_RANGE` 迭代器，互不影响）、支持 `len()`（`msRangeLen`，元素个数按步长公式计算、负值钳为 0）与整数下标（负索引归一化，越界抛 `IndexError`；`MS_OP_INDEX` 的 range 分支在本任务接线，`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX` 抛 `TypeError`）。切片（`r[1:3]`）属任务 30。
- 个数公式（`step > 0`）：`stop > start` 时 `(stop - start + step - 1) / step`，否则 0；`step < 0` 对称。int64 域内无溢出（差值与步长同号时先判方向再计算）。
- `range` 内建替换任务 10 的物化实现，参数规则不变：1 参 `[0, start)`、2 参 `[start, stop)`、3 参带 `step`；仅接受 int（bool 拒绝，`TypeError`）；`step == 0` 抛 `ValueError`。脚本可见差异仅在「不再物化」：`tests/ms/builtin/range.ms`（任务 10）的断言口径随之改为 `len`/下标/迭代收集比对（本任务实现时同步更新该文件）。
- `MsRange` 是叶子对象（三个 int64），无 GC 子对象遍历。

### 4. 协议分派：原生快路径与实例慢路径

`msIterGetIter` 判定顺序：

1. `obj` 是 `MS_TYPE_ITERATOR`：直接返回 `obj`（迭代器自迭代，`__iter__` 返回自身）。
2. `obj` 是 `MS_TYPE_LIST`/`MS_TYPE_DICT`/`MS_TYPE_STR`/`MS_TYPE_RANGE`：`msIterNew` 建对应原生迭代器。
3. 其余堆对象：经任务 25 的魔术方法查找 `__iter__`；命中则以任务 13 的调用约定零参调用，返回值经 `msIterCheckIterator` 校验（见下）后返回；未命中抛 `TypeError`。
4. 标量（int/float/bool/nil）与无 `__iter__` 的对象：抛 `TypeError: object is not iterable: '<type>'`。

`msIterNext` 判定顺序：

1. `iter` 是 `MS_TYPE_ITERATOR`：按 `kind` 走原生分派（§5），干净耗尽置 `*hasValue = false` 返回 `MS_OK`。
2. 否则按实例慢路径：查找 `__next__`，未命中抛 `TypeError: object is not an iterator: '<type>'`；命中则零参调用——
   - 正常返回：`*out` = 返回值，`*hasValue = true`；
   - 抛出 `StopIteration`（`msExceptionIsA(exc, msExceptionTypeOf(L, MS_EXC_STOP_ITERATION))` 判定）：**捕获并清除该异常**，置 `*hasValue = false` 返回 `MS_OK`；
   - 抛出其余异常：原样传播，返回 `MS_ERROR_RUNTIME`。
3. `MS_TYPE_ITERATOR` 的 `__next__` 方法表条目（脚本侧 `it.__next__()` 可达）同样注册到 iterator 类型对象，语义即原生分派 + 耗尽时物化 `StopIteration` 抛出——保证「原生迭代器也是协议对象」，`next(it)` 内建无需区分两条路径之外的第三种形态。

`msIterCheckIterator`（文件内 `static`）：`__iter__` 的返回值必须是 `MS_TYPE_ITERATOR` 或定义了 `__next__` 的实例，否则抛 `TypeError: __iter__ returned non-iterator`（对齐 Python 的协议校验）。

### 5. 原生迭代器语义

| kind | 产出 | 耗尽条件 | 说明 |
|---|---|---|---|
| `MS_ITER_LIST` | `items[cursor++]` | `cursor == len` | 下标语义：迭代期间 `append` 的新元素会被迭代到；修改不检测、其余修改效果未指定（承任务 16 约定，文档注明） |
| `MS_ITER_DICT` | `[k, v]` 2 元素 list（新分配） | `msDictNext` 返回 false | 插入序；迭代期间禁止结构性修改（承任务 16 `msDictNext` 约定）。tuple 属任务 32，键值对暂以 list 产出，任务 32 落地后可切换为 2 元素 tuple |
| `MS_ITER_STR` | 单码点 str（新分配） | 字节偏移到尾 | 从 `cursor` 解码一个 UTF-8 码点、构造 1 字符字符串、推进偏移；与 `s[i]` 的码点语义一致（02-types §4） |
| `MS_ITER_RANGE` | int `current`，随后 `current += step` | step > 0 时 `current >= stop`；step < 0 时 `current <= stop` | `source == NULL`，无引用持有 |
| `MS_ITER_ENUMERATE` | `[cursor, v]` 2 元素 list，随后 `cursor++` | 内层迭代器耗尽 | `v` 为内层 `msIterNext` 产出 |
| `MS_ITER_ZIP` | `[a_i, b_i, ...]` list | 任一内层迭代器耗尽（最短者截断） | 内层迭代器 list 在构造时经 `msIterGetIter` 逐一取得 |
| `MS_ITER_MAP` | `aux(v)` | 内层耗尽 | `aux` 经任务 13 调用约定单参调用；其异常原样传播 |
| `MS_ITER_FILTER` | `v`（`aux` 为 nil 时按真值过滤，否则 `aux(v)` 为真才产出） | 内层耗尽 | 内层循环直至命中或耗尽 |
| `MS_ITER_REVERSED` | 反向序列元素 | 游标越界 | 构造时要求 `source` 为 list/str/range，其余抛 `TypeError`（对齐 Python「reversed 要求序列」）；list 按下标倒序、str 按码点倒序（先解码定位全部码点边界属实现细节）、range 以 `start + (len-1-i)*step` 计算 |

### 6. 指令语义定稿

任务 05 已登记编码，本任务定稿语义（与任务 27 的假定一致）：

| 指令 | 操作数 | 栈行为 | 语义 |
|---|---|---|---|
| `MS_OP_GET_ITER` | 无 | 弹 1（可迭代对象），压 1（迭代器） | 即 `msIterGetIter`；失败抛 `TypeError` |
| `MS_OP_ITER_NEXT` | sAx = 耗尽跳转偏移 | 迭代器位于栈顶（不弹出）；成功时压入下一元素；耗尽时弹出迭代器并按 sAx 跳转 | 即 `msIterNext`：`*hasValue == true` 压值顺序执行；`false` 弹器跳转；`MS_ERROR_RUNTIME` 照常进入任务 23 的展开流程 |
| `MS_OP_UNPACK` | A = 目标个数 | 弹 1（可迭代对象），压 A（按迭代序，先产出的在深处） | 经协议取迭代器后连取 A 个值；不足 A 个抛 `ValueError: not enough values to unpack`；随后再取一次必须耗尽，否则抛 `ValueError: too many values to unpack` |
| `MS_OP_POP`（新增） | A = 弹出个数 | 弹 A | 循环 `break` 出口与异常路径外的栈清理用；追加到任务 05 的指令枚举末尾（总数仍在 80 以内），任务 08 补分派——对任务 05/08 的显式扩展点 |

- `UNPACK` 的「长度须匹配」依据 03-syntax §3 定稿为**精确匹配**（多退少补均 `ValueError`）。带星号收集（`a, *rest := xs`）的编码（如 A 的高位标志或独立指令）由任务 11 定稿，本任务只保证 A 为纯计数的形式不被堵死。
- dict 的 `for k, v in m` 即 `ITER_NEXT` 产出 `[k, v]` 后接 `UNPACK 2` 的编译序列；单目标 `for pair in m` 直接绑定该对。

### 7. for-in 代码生成

编译器对任务 04 的 `MS_FOR_IN` 节点（`{targets, iterable, body}`）生成（栈注释中 `it` 为迭代器）：

```
  <iterable 表达式求值>            ; [..., iterable]（在隐式作用域之外求值）
  MS_OP_GET_ITER                  ; [..., it]
  ; ---- 压入 for-in 隐式块作用域，声明全部目标变量 ----
L0:
  MS_OP_ITER_NEXT -> Lend         ; 成功: [..., it, v]；耗尽: 弹 it 跳 Lend
  ; ---- 绑定目标 ----
  单目标: MS_OP_STORE_LOCAL t
  多目标: MS_OP_UNPACK n + 逐个 MS_OP_STORE_LOCAL
  ; ---- 循环体 ----
  <body>                          ; continue -> Lcont；break -> Lbreak
Lcont:
  [MS_OP_CLOSE_UPVALS t]          ; 仅当目标变量被闭包捕获（见下）
  MS_OP_JUMP -> L0
Lbreak:                           ; break 的出口（迭代器仍在栈上）
  MS_OP_POP 1
Lend:                             ; 正常耗尽与 break 的汇合点
  ; ---- 弹出隐式块作用域 ----
```

要点：

- **目标变量作用域（规范歧义的显式裁定）**：03-syntax 未明文 `for` 目标变量是否外泄。本任务裁定：编译器为整条 for-in 语句压入一个隐式块作用域（与任务 27 推导式同款机制），目标变量在其中声明、循环结束即不可见；与「块级作用域含 `for` 块」（§8）一致，也与推导式「循环变量不外泄」自洽。
- **迭代级绑定**：目标变量被循环体内的 lambda/闭包捕获时（任务 07 作用域解析的 escape 标记），在回边前发射 `MS_OP_CLOSE_UPVALS` 使每轮迭代产生新绑定——`fs := []; for x in [1, 2, 3] { fs.append(lambda: x) }` 的三个闭包分别读到 1/2/3（承任务 27 §7 的同一约定）。
- **`break`/`continue`**：`continue` 跳 `Lcont`（迭代器在栈顶，天然满足 `ITER_NEXT` 约定）；`break` 跳 `Lbreak` 先 `MS_OP_POP 1` 弹出迭代器再汇合——循环退出后求值栈深度与进入前一致，编译期静态可证。循环上下文（break/continue 目标标签）复用任务 12 的机制（假定名 `msCompilerLoopContext`，以任务 12 文档定名为准）；for-in 与任务 12 的三段式 `for` 共用该上下文管理。
- **无 for-else**：parser 不接受 `for ... else`（任务 04 的语法域），本任务不引入任何相关代码路径。
- 嵌套循环、循环内 `try/finally`（任务 23 的控制流出口副本）与本模板正交：`break`/`continue` 穿越 `try` 时的 finally 内联由任务 23 的编译器逻辑统一处理。

### 8. StopIteration 与 for 循环的衔接

- **吞下点唯一**：`StopIteration` 只在 `msIterNext` 的实例慢路径出口被消化（§4），即「迭代器取下一个值」这一动作内抛出的 `StopIteration`；**循环体内**显式 `raise StopIteration` 不经该路径，照常沿任务 23 的展开流程传播，外层 `except StopIteration` 可捕获。这给出明确不变量：`for` 循环永远不会因为循环体代码抛 `StopIteration` 而提前静默终止。
- 原生迭代器的耗尽不物化异常对象（§2 三态返回），`for` 热路径零异常开销（08-vm-internals §2.2 的性能约定）。
- `next(it)` 内建：`msIterNext` 干净耗尽时——无默认值形式以 `msVmRaiseFmt(L, MS_EXC_STOP_ITERATION, ...)` 抛出；有默认值形式返回默认值。两种形式都是协议规范的直接落地（02-types §7）。
- 推导式（任务 27）经同一 `MS_OP_ITER_NEXT` 接线，自动继承本节全部语义，无需额外处理。

### 9. 内建函数

经任务 10 的 `msBuiltinTable` 注册（新增八项、替换一项），参数校验复用任务 10 的 `msBuiltinCheckArgc`/`msBuiltinCheckType` 风格：

| 内建 | 语义 | 错误 |
|---|---|---|
| `iter(x)` | 即 `msIterGetIter` | 不可迭代抛 `TypeError`；不支持双参哨兵形式（规范未定义，多参报 `TypeError`） |
| `next(it)` / `next(it, default)` | `msIterNext`；耗尽时无 default 抛 `StopIteration`、有 default 返回 default | `it` 非迭代器抛 `TypeError` |
| `enumerate(x, start=0)` | `MS_ITER_ENUMERATE` 迭代器，`start` 须为 int | `x` 不可迭代抛 `TypeError` |
| `zip(a, b, ...)` | `MS_ITER_ZIP` 迭代器，≥1 个可迭代参数，最短者截断 | 参数不可迭代抛 `TypeError` |
| `map(f, x)` | `MS_ITER_MAP` 惰性迭代器 | `f` 不可调用抛 `TypeError` |
| `filter(f, x)` | `MS_ITER_FILTER` 惰性迭代器；`f` 为 nil 按真值过滤 | 同上 |
| `sorted(x, key=nil, reverse=false)` | 迭代 `x` 收集为新 list，C 侧稳定归并排序（比较经任务 06/16 的 `msObjectCompare` 假定名；`key` 非 nil 时先装饰为 `[key(v), v]` 对再排、排完拆饰）；返回新 list | 元素不可比较抛 `TypeError` |
| `reversed(x)` | `MS_ITER_REVERSED` 迭代器，仅接受 list/str/range | 其余类型抛 `TypeError` |

- 这组内建是任务 10 显式排到「依赖迭代协议」的集合，本任务一并收口；`sorted`/`reversed` 与 sort 模块（任务 49，纯脚本）的关系：`sorted` 内建提供基础能力，`sort.sorted` 等是其上层便利封装，两者不冲突。
- kwarg（`start=`/`key=`/`reverse=`）经任务 10 §4 假定的「名字/值交错追加 argv 尾部」约定解析，以任务 13 定稿为准。

### 10. 内存与 GC 纪律

- 全部堆分配经 `msAlloc`/`msRealloc`/`msFree`；`MsIterator`/`MsRange` 与产出值（dict 键值对 list、str 单字符等）的分配集中在 `msIterNew`/`msRangeNew` 与 `msIterNext` 的原生分派内。
- GC 遍历挂接（任务 17 的逐类型遍历槽，假定名）：`MS_TYPE_ITERATOR` 标记 `source` 与 `aux`；`MS_TYPE_RANGE` 为叶子。循环进行中的迭代器由求值栈持有（栈本身是根集合），无额外登记。
- 根纪律：`MS_OP_UNPACK` 连取 A 个值期间已取值压在求值栈上（天然是根）；`msIterGetIter`/`msIterNext` 内跨分配存活的局部 `MsObject*`（如 dict 迭代器先取 k 再分配 `[k, v]` 对）经任务 18 的 `msRootPush`/`msRootPop` 保护。

## 实现步骤

1. 建 `src/object/ms_iterator.h` / `ms_iterator.c` 骨架：`MsIterKind`、`struct MsIterator`/`struct MsRange`、`msIterNew`/`msRangeNew`/`msRangeLen`；完成任务 06 的 `MS_TYPE_RANGE` 扩展（枚举、静态断言改 21、类型对象表建 `"range"`）。验证：头文件自包含编译；`type(range(3))` 经临时打印输出 `<type 'range'>`。
2. 实现 list/dict/str/range 四种原生迭代器的 `msIterNext` 分派与 `msIterGetIter` 的原生分支。验证：ms 脚本以 `iter`/`next` 手工驱动四种对象（`next` 内建先以最小实现挂接），逐值断言；dict 插入序、str 多字节码点、range 负步长。
3. 注册 `iter`/`next` 内建完整语义：`next` 默认值形式、耗尽抛 `StopIteration`、`iter` 幂等（迭代器返回自身）、非可迭代/非迭代器 `TypeError`。验证：脚本 `try/except StopIteration`/`except TypeError` 断言。
4. VM 接线：`MS_OP_GET_ITER`/`MS_OP_ITER_NEXT`/`MS_OP_UNPACK` 分派与新增 `MS_OP_POP`（任务 05 枚举追加、任务 08 分派、元数据表条目）。验证：推导式（任务 27 若已先行实现）回归通过；`a, b := [1, 2]` 解包与长度不符的 `ValueError`。
5. 编译器 for-in 代码生成：`MS_FOR_IN` 分支、隐式块作用域、单/多目标绑定、`break`/`continue` 出口（含 `MS_OP_POP 1`）、迭代级 `MS_OP_CLOSE_UPVALS`、栈深统计。验证：`for x in xs`、`for k, v in m`、嵌套 for、break/continue、循环变量不外泄、闭包逐轮捕获的脚本断言。
6. 实例慢路径：`__iter__`/`__next__` 查找与调用、`msIterCheckIterator` 校验、慢路径 `StopIteration` 吞下判定（仅 `msIterNext` 出口）。验证：脚本 class 自定义迭代器接入 `for-in`；`__iter__` 返回非迭代器抛 `TypeError`；循环体内显式 `raise StopIteration` 传播至外层 `except`。
7. 惰性 range 收尾：`range` 内建替换任务 10 物化实现、`MS_OP_INDEX` 的 range 分支与 `len` 分派、`MS_ITER_RANGE` 完备。验证：`tests/ms/builtin/range.ms` 按新口径更新后通过；`for i in range(2, 10, 3)`、同一 range 对象两次迭代互不干扰。
8. `enumerate`/`zip`/`map`/`filter`/`sorted`/`reversed` 六个内建与对应 `MsIterKind` 分派。验证：各自的脚本断言（见测试方案）。
9. GC 挂接与内存复查：iterator/range 的遍历槽登记、根纪律核对；任务 02 分配统计在 `msCloseState` 后归零。验证：Debug 构建（ASAN / `/RTC`）跑全部本任务测试无报告；Win/Linux/macOS × Debug/Release 构建通过，`run_tests.py` 全绿。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试（`testing` 模块在任务 40 才存在，本阶段用内建 `assert` + `print` 自断言；异常系统已在任务 23 落地，负向用例优先以 `try/except` 捕获并断言异常类型、`except` 未命中路径显式 `assert(false)`；仅无法捕获的致命错误沿用 `<name>.exit` 同伴文件约定，由仓库根 `run_tests.py` 驱动）。本任务只交付设计文档，脚本随实现编写。

测试文件清单（`tests/ms/iteration/`）与覆盖点：

- `for_list.ms`：list 的 for-in 全量顺序断言、空 list 零次迭代、`break`/`continue`、嵌套 for-in、循环体内 `append` 的可见性行为（与 §5 约定一致）；末尾 `print("for list ok")`。
- `for_dict.ms`：`for k, v in m` 的解包与插入序断言、单目标 `for pair in m` 得 2 元素 list、空 dict、迭代期间结构性修改不做（仅在文档层面约定，不构造该用例的断言）。
- `for_str.ms`：按码点迭代（含多字节字符，逐字符与 `s[i]` 比对）、空串零次迭代。
- `for_scope.ms`：循环变量循环后不可见（以「引用未定义名字报错的负例脚本」覆盖，配 `.exit`）；被 lambda 捕获的循环变量逐轮绑定（三闭包读 1/2/3）；外层同名变量不受遮蔽影响。
- `for_unpack.ms`：多目标解包（含 `enumerate` 双目标）、长度不符的两种 `ValueError`（多/少各一，`try/except` 断言）。
- `range.ms`：`range(5)`/`range(2, 8)`/`range(0, 10, 3)`/`range(5, 0, -1)` 的 `len`/下标/迭代收集三口径断言；空区间；同一 range 重复迭代互不干扰；`range(1, 10, 0)` 抛 `ValueError`；`range(true)` 抛 `TypeError`；`r[0] = 1` 抛 `TypeError`。任务 10 的 `tests/ms/builtin/range.ms` 按本任务口径同步更新（物化断言改惰性断言）。
- `iter_next.ms`：`iter` 对四种原生对象与迭代器自身（幂等）；`next` 逐值、耗尽抛 `StopIteration`（`try/except`）、`next(it, d)` 返回默认值且默认值只取一次语义；`iter(42)`/`next(42)` 抛 `TypeError`；迭代器耗尽后再 `next` 仍抛 `StopIteration`（不复位）。
- `stop_iteration.ms`：显式 `raise StopIteration` 被 `except StopIteration` 捕获；**循环体内** `raise StopIteration` 不被 for 消化（外层 `except` 命中、循环计数证明未静默终止）；实例迭代器的 `__next__` 抛 `StopIteration` 使 for 正常结束；`__next__` 抛 `ValueError` 穿透 for 被外层捕获。
- `custom_iterator.ms`：脚本 class 实现 `__iter__` 返回自身 + `__next__` 计数到界抛 `StopIteration`，`for x in obj` 收集断言；`__iter__` 返回独立迭代器对象的形式；`__iter__` 返回非迭代器抛 `TypeError`。
- `enumerate_zip.ms`：`enumerate(xs)`/`enumerate(xs, 5)` 的索引与值、`for i, x in enumerate(xs)`；`zip` 双参与三参、最短截断、空参数迭代得空。
- `map_filter.ms`：`map`/`filter` 惰性（配合 `next` 逐步断言）、`filter(nil, xs)` 真值过滤、经 for-in 收集。
- `sorted_reversed.ms`：`sorted` 稳定性（相等元素保持原序，以 `[key, v]` 结构观察）、`key`/`reverse`、返回新 list 不改原对象；`reversed` 对 list/str/range 的反序迭代、对 dict 抛 `TypeError`。

## 验收标准

- [ ] `src/object/ms_iterator.h` / `ms_iterator.c` 存在，guard 为 `MSLANG_SRC_OBJECT_MS_ITERATOR_H_`，头文件自包含；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsIterator`/`struct MsRange` 不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] `MsTypeTag` 追加 `MS_TYPE_RANGE`（任务 06 的静态断言同步改为 21、类型表建 `"range"` 类型对象），该扩展与 02-types §1 无 range 的偏差已在本文显式记录。
- [ ] `msIterGetIter`/`msIterNext` 三态约定落实：干净耗尽不物化异常；实例慢路径仅在 `__next__` 出口吞下 `StopIteration`，其余异常原样传播；`__iter__` 返回值校验（非迭代器抛 `TypeError`）。
- [ ] list/dict/str/range 原生迭代器语义与本文 §5 一致：list 下标游标、dict 插入序 `[k, v]` 对、str 按码点、range 惰性；迭代期间修改容器的约定已在文档注明。
- [ ] `MS_OP_GET_ITER`/`MS_OP_ITER_NEXT`/`MS_OP_UNPACK` 的语义与栈约定同任务 05 编码、任务 27 假定完全一致；`MS_OP_POP` 按本文口径追加且指令总数 ≤ 80；`UNPACK` 精确匹配（多/少均 `ValueError`）。
- [ ] for-in 代码生成与本文 §7 一致：iterable 在隐式作用域外求值、目标变量不外泄、`break` 先弹迭代器再汇合、`continue` 跳回边、被捕获目标逐轮 `MS_OP_CLOSE_UPVALS`；无 for-else 的任何代码路径。
- [ ] StopIteration 衔接不变量成立：循环体内显式 `raise StopIteration` 不被 for 消化；`next(it)` 耗尽抛 `StopIteration`、`next(it, d)` 返回默认值（04-exceptions §5）。
- [ ] 惰性 `range` 替换任务 10 的物化实现且参数规则不变（仅 int、`step == 0` 抛 `ValueError`），支持 `len`/整数下标/重复迭代；`tests/ms/builtin/range.ms` 口径已同步更新。
- [ ] `iter`/`next`/`enumerate`/`zip`/`map`/`filter`/`sorted`/`reversed` 八个内建注册并符合本文 §9；自定义 class 经 `__iter__`/`__next__` 接入 for-in。
- [ ] `tests/ms/iteration/` 下「测试方案」全部清单项实现并全数通过，`python run_tests.py` 退出码为 0；跨分配局部对象遵守根纪律，`msCloseState` 后分配统计归零，Debug 构建（ASAN / `/RTC`）无报告；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过。
- [ ] 无 TBD/TODO 占位；对任务 07/08/12/13/14/17/25 的假定接口名与对任务 05/06/10/16 的扩展点，在实现时已按对应任务文档对齐。
