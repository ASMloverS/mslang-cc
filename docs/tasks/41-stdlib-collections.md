# 41 标准库：collections

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [16 容器 list 与 dict](16-containers-list-dict.md) |

## 任务目标

交付纯 .ms 实现的标准库模块 `lib/collections.ms`：一组建立在任务 16 的 `list`/`dict` 之上的集合类容器——双端队列（`deque`）、最小堆（`heap`）、计数字典（`counter`）、默认值字典（`defaultdict`）与插入序字典的显式别名（`orderedDict`），公开 API 以 `docs/language/07-stdlib.md` §20 的清单为准。模块不含任何 C 代码，全部能力由 list/dict 的既有操作、class 魔术方法协议（任务 25）与迭代协议（任务 26）组合而成。完成后，脚本可以 `import "collections"` 使用全部五个工厂函数；本任务通过 `tests/ms/stdlib/collections_test.ms` 的 ms 脚本测试（testing 模块，任务 40 已先于本任务完成）独立验收。

## 设计依据

- [07-stdlib.md](../language/07-stdlib.md)
  - §0 模块清单：`collections` 为纯脚本模块（`lib/` 目录），说明文字 "deque/heap/Counter/defaultdict"。
  - §20 collections：本模块的完整公开 API 清单，共五个工厂函数：

    ```ms
    collections.deque(xs=nil)            // append/appendLeft/pop/popLeft/rotate
    collections.heap()                   // push/pop/peek/pushPop (min-heap)
    collections.counter(xs)              // counting dict; mostCommon(n)
    collections.defaultdict(factory)     // calls factory for missing keys
    collections.orderedDict()            // dict already keeps insertion order; this is an explicit semantic alias
    ```

- [02-types.md](../language/02-types.md)
  - §5.1/§5.3：list 与 dict 的语义（本模块的存储底座）；dict 保持插入序（`orderedDict` 别名的成立依据）；键必须可哈希。
  - §7 迭代协议：`__iter__` 返回带 `__next__` 的迭代器对象，耗尽时抛 `StopIteration`；`for x in xs` 是其语法糖。
  - §8 魔术方法协议：`__len__`/`__iter__`/`__next__`/`__getitem__`/`__setitem__`/`__contains__`/`__repr__` 是本模块容器接入 `len()`、`for-in`、`in`、下标与 `print` 的通道。
- [03-syntax.md](../language/03-syntax.md) §4：默认参数在定义时只求值一次，故 `deque(xs = nil)` 用 `nil` 占位而非可变默认值（[12-ms-style.md](../language/12-ms-style.md) §5.5 的强制约定）；§5：class 语法（`__init__`、`self`、`_` 前缀内部属性约定）；§9：内建函数 `len`/`iter`/`callable`/`str`。
- [04-exceptions.md](../language/04-exceptions.md) §4 内建异常层级：空容器弹出/窥探抛 `IndexError`（`LookupError` 子类），缺键抛 `KeyError`，参数类型错误抛 `TypeError`。
- [05-modules.md](../language/05-modules.md) §2：`lib/` 纯脚本模块在解析顺序第一档「内建/已注册模块」中命中；模块的发现、执行与缓存由任务 24（模块系统）负责，本任务只交付模块源码，不涉及加载机制本身。
- [11-project-layout.md](../language/11-project-layout.md) §1：纯脚本标准库模块置于 `lib/`；§4：脚本测试位于 `tests/ms/`。
- [12-ms-style.md](../language/12-ms-style.md)：4 空格缩进、行宽 120、class 大驼峰、函数小驼峰、内部属性/函数 `_` 前缀、公开函数带英文前置文档注释、模块级无可变状态、纯库文件不写 `__name__` 守卫。
- 任务 [16 容器 list 与 dict](16-containers-list-dict.md) 提供本模块依赖的全部底座能力：list 的 `append`/`pop` 与下标读写、负索引归一化，dict 的 `get(k, default)`/下标读写/`in`/`len`/`items()`（返回 2 元素 list 的 list、保持插入序），以及不可哈希键报 TypeError 式错误的契约。
- 任务 23（异常系统）、24（模块系统）、25（class 继承与魔术方法）、26（for-in 迭代协议）、40（testing 模块）的文档尚不存在：本文引用的 `raise` 语句、`import "collections"` 绑定形式、魔术方法分派行为、`StopIteration` 迭代终止约定与 `testing.run()`/`assert.*` 接口，分别以 `docs/language/04-exceptions.md`、`05-modules.md`、`02-types.md` §7/§8、`07-stdlib.md` §19 为准；若对应任务文档的实现细节与本文假定不同，实现时以对应任务文档定名为准。

## 详细设计

本任务是纯 .ms 模块，无 C 结构体与 C 函数签名；接口级设计以 ms 类/函数签名与语义约定表达。ms 代码遵循 [12-ms-style.md](../language/12-ms-style.md)（4 空格缩进、行宽 120、公开函数带英文前置文档注释）。

### 文件与模块骨架

- 唯一交付文件：`lib/collections.ms`。文件头注释一句话说明职责；无 import（只依赖内建函数与 list/dict）；无模块级可变状态；不写 `__name__` 守卫（纯库文件，12-ms-style §2.2）。
- 模块经任务 24 的模块系统以 `"collections"` 名注册/解析，脚本侧用 `import "collections"` 引入，`collections.deque(...)` 形式调用（05-modules §1 绑定规则）。
- 文件内段落顺序按 12-ms-style §2：文件头注释 → 函数（五个公开工厂 + 内部辅助 `_sortPairsByCountDesc`）→ class 定义（`Deque`/`_DequeIterator`/`Heap`/`Counter`/`DefaultDict`）。工厂函数与 class 分离的原因：12-ms-style §4 规定 class 名大驼峰，而 07-stdlib §20 清单给定的是小驼峰工厂名；工厂返回对应类的实例，与 `strings.builder()` → Builder 的既定先例一致。class 本身作为模块属性可访问，但公开 API 以五个工厂函数为准。

### deque(xs = nil)：双端队列

```ms
// deque returns a new double-ended queue containing the elements of xs
// (any iterable) in iteration order, or an empty deque when xs is nil.
func deque(xs = nil)

// Deque is a double-ended queue backed by a circular buffer.
// Supports O(1) append/pop at both ends.
class Deque {
    func __init__(self, xs = nil)
    func append(self, v)        // add v at the right end; returns nil
    func appendLeft(self, v)    // add v at the left end; returns nil
    func pop(self)              // remove and return the rightmost element
    func popLeft(self)          // remove and return the leftmost element
    func rotate(self, n = 1)    // rotate n steps to the right (n < 0: to the left)
    func __len__(self)          // element count
    func __iter__(self)         // left-to-right iterator
    func __repr__(self)         // e.g. "deque([1, 2, 3])"
}
```

存储结构（内部属性，`_` 前缀）：

- `_buf`：环形缓冲，list 定长槽位，空槽为 `nil`；`_head`：逻辑首元素在 `_buf` 中的下标；`_size`：元素个数。不变量：`0 <= _size <= len(_buf)`，逻辑第 i 个元素位于 `_buf[(_head + i) % len(_buf)]`。
- 容量管理：`_size == len(_buf)` 时倍增扩容（空缓冲首次扩到 8 槽），新建缓冲后按逻辑顺序拷贝、`_head` 归零；只扩不缩（与任务 16 的 list 不缩容策略一致，避免抖动）。`_buf` 的预填经 `for` 循环 + `append(nil)` 完成（ms 的 list 无重复构造语法）。ms 的 `%` 对负操作数的语义未在语言文档中明确，所有下标计算一律先把被减数加上 `len(_buf)` 再取模，保证操作数非负。
- `pop`/`popLeft`：读出端点元素后**把槽位写回 `nil`**（释放对元素对象的引用，避免已弹出元素被缓冲长期存活），`_size--`；空队列抛 `IndexError("pop from an empty deque")` / `IndexError("popLeft from an empty deque")`。
- `rotate(n)`：对齐 Python `deque.rotate`——`n > 0` 整体右移 n 步（等价于重复 `appendLeft(pop())`），`n < 0` 左移。实现先归一化 `r := n mod _size` 到 `[0, _size)`，`r == 0` 或空队列直接返回；再按 `r <= _size - r` 选择重复次数更少的一端逐位搬移（总搬移次数 `min(r, _size - r)`，每次 O(1)）。
- 迭代：`__iter__` 返回内部类 `_DequeIterator` 的实例（字段 `_deque`、`_pos`），`__next__` 经 `_deque` 的内部定位方法取逻辑第 `_pos` 个元素，`_pos` 达到 `_deque._size` 时抛 `StopIteration`。迭代期间对队列做结构性修改的行为未定义（与任务 16 对 dict 迭代的约定一致），不做修改检测。

### heap()：最小堆

```ms
// heap returns a new empty min-heap. Elements must be mutually
// comparable with `<`.
func heap()

// Heap is a binary min-heap over a list: the smallest element is
// always at index 0.
class Heap {
    func __init__(self)
    func push(self, v)          // add v; returns nil
    func pop(self)              // remove and return the smallest element
    func peek(self)             // return the smallest element without removing it
    func pushPop(self, v)       // push v, then pop and return the smallest
    func __len__(self)          // element count
    func __repr__(self)         // e.g. "heap([1, 3, 2])" (internal array order)
}
```

- 存储为单个内部 list `_items`，满足堆性质：对任意下标 `i > 0` 有 `_items[(i - 1) div 2] < _items[i]` 或其元素相等（元素间用 `<` 比较，不可比较的元素由 VM 自然抛 `TypeError`，堆自身不做类型校验）。
- `push`：尾部追加后**上浮**——与父节点 `(_i - 1) div 2` 比较，更小则交换，直至根或不再小于父节点。O(log n)。
- `pop`：空堆抛 `IndexError("pop from empty heap")`；否则取根部为返回值，把尾元素移到根部后**下沉**——与两个子节点中较小者比较，更大则交换，直至叶子或不大于子节点。O(log n)。
- `peek`：空堆抛 `IndexError("peek from empty heap")`；否则返回 `_items[0]`。O(1)。
- `pushPop`：语义等价于 `push(v)` 再 `pop()`，但走快速路径：堆非空且 `_items[0] < v` 时，根部元素作为返回值、`v` 替换根部并下沉；否则直接返回 `v`（堆不变）。空堆时返回 `v`。
- **不提供 `__iter__`**：内部数组序无语义价值，按序消费堆的正确方式是反复 `pop`；刻意省略以避免「迭代即有序」的误读。
- 构造器按 07-stdlib §20 清单固定为无参，不提供从既有 list 建堆的 heapify 入口（清单未列；脚本可循环 `push`）。

### counter(xs)：计数字典

```ms
// counter returns a new Counter tallying the elements of xs (any
// iterable). Elements must be hashable.
func counter(xs)

// Counter is a counting dict: reading a missing key yields 0 without
// inserting it. Iteration and len() behave like the underlying dict.
class Counter {
    func __init__(self, xs)
    func __getitem__(self, k)        // count of k; 0 when absent (no insertion)
    func __setitem__(self, k, v)     // set the count of k explicitly
    func __contains__(self, k)       // k in c
    func __len__(self)               // number of distinct keys
    func __iter__(self)              // delegates to the underlying dict
    func mostCommon(self, n = -1)    // the n most common [key, count] pairs; n < 0: all
    func __repr__(self)              // e.g. "counter({"a": 3, "b": 1})"
}
```

- 组合而非继承：内部属性 `_counts`（dict）承载全部键值数据。不采用 `class Counter < dict`，因为内建类型的可继承性未在任何语言文档中承诺；组合经魔术方法接入全部所需协议。
- 构造：`for x in xs` 逐元素 `_counts[x] = _counts.get(x, 0) + 1`；不可哈希元素由 dict 的键契约自然抛 `TypeError`（任务 16 的约定），Counter 不做前置校验。`xs` 为必选参数（07-stdlib §20 清单形式），空计数器用 `counter([])` 构造。
- `__getitem__`：`return self._counts.get(k, 0)`——缺失键读到 `0` 且**不写入**（对齐 Python Counter），这使 `c["new"] += 1` 这类复合赋值自然成立（先读 0 再经 `__setitem__` 写入 1）。
- `__setitem__`/`__contains__`/`__len__`/`__iter__` 直接委托 `_counts`；迭代产出与 dict 一致（`for k, v in c` 得键值对）。
- `mostCommon(n)`：经 `_counts.items()`（任务 16 保证返回按插入序的 2 元素 list 的 list）取得 `[key, count]` 对，用模块内部函数 `_sortPairsByCountDesc(pairs)` 做**稳定**的插入排序按 count 降序排列（稳定性保证 count 相同的键保持首次计入的相对顺序）；`n < 0` 返回全部，否则返回前 `n` 对（`n` 超过键数时返回全部，不报错）。
- 排序不引用 `sort` 模块：sort 属任务 49（v0.4，编号晚于本任务，依赖方向不允许），故在模块内自带 O(k²) 稳定插入排序，k 为不同键数，对计数场景足够；函数名 `_sortPairsByCountDesc` 按 12-ms-style §4 加 `_` 前缀。
- 不提供 Counter 间的算术运算（`+`、`-`、`&` 等）：07-stdlib §20 清单未列，不增删。

### defaultdict(factory)：默认值字典

```ms
// defaultdict returns a new DefaultDict that calls factory() to
// produce a value for each missing key on subscript read. factory
// must be callable or nil (nil: behave like a plain dict).
func defaultdict(factory)

// DefaultDict is a dict wrapper that auto-vivifies missing keys
// with factory() on __getitem__.
class DefaultDict {
    func __init__(self, factory)
    func __getitem__(self, k)        // existing value, or factory() stored under k
    func __setitem__(self, k, v)
    func __contains__(self, k)
    func __len__(self)
    func __iter__(self)
    func __repr__(self)              // e.g. "defaultdict({...})"
}
```

- 内部属性 `_factory` 与 `_data`（dict）。
- 构造时校验 `factory is nil or callable(factory)`，不成立抛 `TypeError(f"collections.defaultdict: factory must be callable or nil, got {str(factory)}")`。
- `__getitem__`：`k in self._data` 命中则返回其值；未命中时，`_factory is nil` 直接委托 `self._data[k]`（由 dict 自身抛 `KeyError`，与任务 16 的下标语义一致），否则 `v := _factory()`、写入 `_data[k]` 并返回 `v`——缺失键**只在下标读时**物化，`in` 判定与 `len()` 不触发 factory（对齐 Python defaultdict）。
- 其余协议方法全部委托 `_data`。
- 典型用法 `defaultdict(list)`：内建构造器 `list()`（03-syntax §9）是合法的零参可调用值，无需包装。

### orderedDict()：插入序字典的语义别名

```ms
// orderedDict returns a new empty dict. dict already preserves
// insertion order; this factory exists as an explicit semantic alias
// for code that wants to state the ordering requirement.
func orderedDict()
```

- 实现为 `return {}`：每次调用返回全新的空 dict。不引入类，不包装——07-stdlib §20 明确这是 "explicit semantic alias"，任何包装都会无谓地复制 dict 的协议面。
- 与直接写 `{}` 的区别仅在可读性：声明「此处依赖插入序」的意图。

### 协议方法汇总与取舍说明

超出 07-stdlib §20 清单的方法只有协议集成所必需者，逐条说明依据（[02-types.md](../language/02-types.md) §8）：

- `__len__`（全部四个类）：接入内建 `len()`，容器不可用 `len()` 度量则无法参与真值判定与常规遍历控制。
- `__iter__`（`Deque`/`Counter`/`DefaultDict`）：接入 `for-in` 与解包；`Heap` 刻意不提供（见上）。
- `__getitem__`/`__setitem__`/`__contains__`（`Counter`/`DefaultDict`）：接入下标与 `in`，是其「dict 变体」语义的本体，非额外功能。
- `__repr__`（全部四个类）：接入 `print`/调试输出；格式为 `工厂名(内容)`，其中 `Heap` 展示内部数组序、`Counter`/`DefaultDict` 内嵌底层 dict 的 repr。
- 刻意省略：`Deque` 的下标访问（`d[i]`）、`Counter` 的算术运算、`Heap` 的迭代与 heapify 构造——清单未列，列入后续评估。

## 实现步骤

1. 建 `lib/collections.ms` 骨架：文件头注释、五个工厂函数与五个类的签名、英文文档注释，方法体先 `pass`。验证：`import "collections"` 成功，五个工厂可调用且返回对应类的实例（`type(collections.deque())` 等）。
2. 实现 `Deque` 的环形缓冲存储与 `append`/`appendLeft`/`pop`/`popLeft`/`__len__`：容量倍增、下标取模（保证非负操作数）、弹出槽位置 `nil`、空队列 `IndexError`。验证：两端交错增删、跨多次扩容（如 100 个元素）后顺序与逐值断言。
3. 实现 `rotate`（含方向归一化与较少搬移方向选择）与 `__iter__`/`_DequeIterator`、`__repr__`。验证：正/负/超界 n、空队列、`for x in d` 的顺序、`print(d)` 输出。
4. 实现 `Heap`：`push` 上浮、`pop` 下沉、`peek`、`pushPop` 快速路径、`__len__`、`__repr__`。验证：随机序列推入后逐次 `pop` 得升序；`pushPop` 的两种分支；空堆错误路径。
5. 实现 `Counter`：构造计数、`__getitem__` 缺失读 0 不插入、委托方法集、`mostCommon` 与 `_sortPairsByCountDesc` 稳定性、`__repr__`。验证：计数正确性、缺失键、`mostCommon` 的降序与同数保序、`c[k] += 1` 复合赋值。
6. 实现 `DefaultDict`：factory 校验、缺失键物化、`nil` factory 的 `KeyError` 路径、委托方法集。验证：`defaultdict(list)` 的典型用法（`d[k].append(v)`）、factory 副作用只发生一次（重复读不再调用）。
7. 实现 `orderedDict`：返回新空 dict。验证：两次调用返回不同对象（`is not`），插入序与 dict 一致。
8. 编写完整测试文件（见「测试方案」），经仓库根 `run_tests.py` 调用 mslang CLI 全量运行通过。

## 测试方案

按 README 测试约定：本任务晚于任务 40（testing 模块），测试一律使用 `testing` 与 `testing/assert` 模块。测试文件 `tests/ms/stdlib/collections_test.ms`，测试函数以 `test` 开头，末尾 `testing.run()`；由 `run_tests.py` 递归发现并驱动。错误路径断言用 `assert.raises`（07-stdlib §19 既定接口）。本任务只交付设计文档，测试脚本随实现编写。

覆盖清单：

- `deque`：
  - 构造：空构造、`deque([1, 2, 3])` 保序、`deque(nil)` 为空。
  - 两端操作：`append`/`appendLeft`/`pop`/`popLeft` 的交错序列与返回值的逐步断言；`len()` 同步；空队列 `pop`/`popLeft` 用 `assert.raises(IndexError, ...)` 断言。
  - 扩容：追加 100 个元素（跨多次倍增）后从左到右逐值断言，确认环形下标回绕正确。
  - `rotate`：`rotate(1)` 右移一位、`rotate(-2)` 左移两位、`rotate(n)` 中 `n` 为队列长度整数倍时不变、空队列与单元素队列为 no-op、默认参数 `rotate()` 等价 `rotate(1)`。
  - 迭代：`for x in d` 从左到右得全部元素；迭代顺序经收集为 list 后与期望 list 用 `==` 比对。
  - GC 引用释放：弹出后槽位置 `nil`（以 `len` 与后续 `append` 行为间接验证，不做内存级断言）。
- `heap`：
  - 基本：`push` 若干乱序元素后 `peek` 得最小值；逐次 `pop` 严格升序且 `len()` 递减至 0。
  - `pushPop`：新值大于根部时返回根部且新值入堆；新值小于等于根部时原样返回且堆不变；空堆 `pushPop(v)` 返回 `v`。
  - 错误：空堆 `pop`/`peek` 用 `assert.raises(IndexError, ...)` 断言。
  - 元素为字符串时按字符串序工作（元素比较由 VM 的 `<` 分派，不特判）。
- `counter`：
  - 构造：`counter("aabbbc")` 之类可迭代输入的计数结果；`counter([])` 为空、真值为假。
  - 读：命中键返回计数；缺失键 `c["x"] == 0` 且 `"x" in c` 为假（读不插入）；`c["x"] += 1` 后计数为 1 且键存在。
  - 委托：`len(c)` 为不同键数；`for k, v in c` 遍历；`c[k] = v` 显式写入。
  - `mostCommon`：降序排列；同计数键保持首次计入的相对顺序（稳定性）；`mostCommon(2)` 截断、`mostCommon()`/`mostCommon(-1)` 返回全部、`n` 超过键数返回全部。
  - 错误：`counter([[1]])`（不可哈希元素）用 `assert.raises(TypeError, ...)` 断言。
- `defaultdict`：
  - 物化：`d := collections.defaultdict(list)` 后 `d["a"].append(1)`，`d["a"] == [1]`；重复读 `d["b"]` 返回同一对象（factory 只调用一次，可用自定义 factory 的调用计数闭包断言）。
  - 不物化路径：`"k" in d`、`len(d)` 不触发 factory。
  - `nil` factory：`collections.defaultdict(nil)` 缺失键下标读用 `assert.raises(KeyError, ...)` 断言。
  - 错误：非可调用且非 `nil` 的 factory（如 `defaultdict(42)`）用 `assert.raises(TypeError, ...)` 断言。
- `orderedDict`：返回 dict（`type(...) == dict` 或 `isinstance` 判定，以任务 10 内建的定名为准）；两次调用 `is not`；插入、替换、删除后迭代顺序与 dict 插入序契约一致。
- 模块级：`import "collections"` 可用；工厂返回值的方法集与本文「详细设计」一致（抽查 `dir()` 或直接调用覆盖）。

## 验收标准

- [ ] `lib/collections.ms` 存在，实现且仅实现 07-stdlib §20 的五个工厂函数 `deque`/`heap`/`counter`/`defaultdict`/`orderedDict`（协议方法除外，见「协议方法汇总与取舍说明」）；无 C 代码，无模块级可变状态，无 `__name__` 守卫。
- [ ] ms 代码符合 12-ms-style：4 空格缩进、行宽 120、class 大驼峰、内部属性/函数 `_` 前缀、不遮蔽内建名、公开函数与 class 带英文前置文档注释、文件 UTF-8 无 BOM、LF 行尾、无行尾空白；构建产物只落在 `build/`。
- [ ] `Deque` 为环形缓冲实现：两端 `append`/`pop` 均为摊还 O(1)，弹出槽位置 `nil`，空队列 `pop`/`popLeft` 抛 `IndexError`；`rotate(n)` 正右负左、按模归一化、空队列 no-op；`__len__`/`__iter__`/`__repr__` 正确。
- [ ] `Heap` 为最小堆：`pop` 逐次返回升序序列，`peek` 不删除，`pushPop` 语义等价于 push 后 pop 且含快速路径，空堆 `pop`/`peek` 抛 `IndexError`。
- [ ] `Counter` 缺失键读返回 `0` 且不插入；`mostCommon(n)` 按计数降序、同数稳定、`n < 0` 返回全部；委托方法（`__setitem__`/`__contains__`/`__len__`/`__iter__`）与底层 dict 一致。
- [ ] `DefaultDict` 缺失键下标读调用零参 factory 并物化一次；`in`/`len` 不触发 factory；`nil` factory 缺失键抛 `KeyError`；非可调用 factory 抛 `TypeError`。
- [ ] `orderedDict()` 每次调用返回全新的空 dict，行为与字面量 `{}` 完全一致。
- [ ] `import "collections"` 在解释器中可用，模块经 `lib/` 解析档命中（机制属任务 24，本任务不改动加载器）。
- [ ] `tests/ms/stdlib/collections_test.ms` 覆盖「测试方案」全部清单项，`run_tests.py` 全量通过。
- [ ] 无 TBD/TODO 占位；与任务 16 的接口约定（list/dict 方法集、`items()` 返回 2 元素 list、不可哈希键报错）及任务 23/24/25/26/40 的接口假定（异常类、import、魔术方法分派、迭代协议、testing 模块）在实现时已对齐对应任务文档的定名。
