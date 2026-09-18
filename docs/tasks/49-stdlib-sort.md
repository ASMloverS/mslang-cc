# 49 标准库：sort

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [14 闭包与 upvalue](14-closures.md)、[16 容器 list 与 dict](16-containers-list-dict.md) |

## 任务目标

交付纯 .ms 实现的标准库模块 `lib/sort.ms`：list 的就地稳定排序（`sort`）、返回新列表的排序（`sorted`）、键函数排序（`sortBy`）、原地反转（`reverse`）、二分查找插入点（`binarySearch`）与有序判定（`isSorted`），共六个公开函数。模块不含任何 C 代码，全部能力建立在任务 16 的 list 下标读写/长度语义与任务 14 的闭包（键函数常以 lambda/闭包形式传入）之上，经任务 24 的模块系统以 `lib/` 解析档加载。完成后，脚本可以 `import "sort"` 使用全部六个函数；本任务通过 `tests/ms/` 下的 ms 脚本测试（testing 模块，任务 40 已先于本任务完成）独立验收。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`sort` 为纯脚本模块（`lib/` 目录），功能是「排序与二分」。
  - §5 sort：本模块的完整公开 API 清单，共六个函数：

    ```ms
    sort.sort(xs)                        // sorts in place
    sort.sorted(xs)                      // returns a new list
    sort.sortBy(xs, key)                 // key is a function
    sort.reverse(xs)
    sort.binarySearch(xs, v)             // returns the insertion index (bisect semantics)
    sort.isSorted(xs)
    ```

- `docs/language/02-types.md`
  - §5.1 list 语义：下标读写、`len(xs)`、可变动态数组（实现属任务 16）。
  - §8 魔术方法协议：比较运算走 `__lt__`/`__le__`/`__gt__`/`__ge__`，自定义 class 实例定义 `__lt__` 即可参与默认排序。
- `docs/language/03-syntax.md`
  - §6 运算符优先级：比较运算符 `== != < <= > >=` 左结合、可链式；本模块默认比较只用 `<`。
  - §8 作用域：闭包对外层局部变量按引用捕获，内层可直接写捕获变量（`sortBy` 的键函数依赖此语义，任务 14）。
  - §9 内建函数：`len`/`list`/`min`/`callable`/`range`；另有内建 `sorted(x, key=nil, reverse=false)`（实现属任务 26 迭代协议），与本模块的 `sort.sorted` 的关系见「设计取舍说明」。
- `docs/language/05-modules.md` §2：`lib/` 纯脚本模块在解析顺序第一档「内建/已注册模块」中命中；模块的发现、执行与缓存由任务 24（模块系统）负责，本任务只交付模块源码，不涉及加载机制本身。
- `docs/language/11-project-layout.md` §1：纯脚本标准库模块置于 `lib/`；§4 测试策略：脚本测试位于 `tests/ms/`。
- `docs/language/12-ms-style.md`：§2 源文件结构、§3 格式化（4 空格缩进、行宽 120、禁止单行块）、§4 命名（内部函数 `_` 前缀、禁止遮蔽内建名）、§6 注释与文档（公开函数的前置英文文档注释，代码块围栏用 `ms`）。
- 任务 16（容器 list 与 dict）提供 list 的下标读写（含负索引归一化）、`len` 与按值 `==`，其文档已定稿，接口以该文档为准。
- 依赖任务 14（闭包与 upvalue）的文档尚不存在：本文引用的闭包语义以 `docs/language/03-syntax.md` §8 为准（按引用捕获、内层可写）；若任务 14 文档的实现细节与本文假定不同，实现时以对应任务文档定名为准。此外，规范未明确定义混合类型 `<` 比较（如 `1 < "a"`）的行为，本文假定其与 Python 一致抛出 `TypeError`；str 之间的 `<` 假定为按码点字典序。这两处是运算符语义（任务 06/08 范畴），本模块只是传播其结果，实现时以对应任务文档定名为准。

## 详细设计

本任务是纯 .ms 模块，无 C 结构体与 C 函数签名；接口级设计以 ms 函数签名与语义约定表达。ms 代码遵循 `docs/language/12-ms-style.md`（4 空格缩进、行宽 120、小驼峰函数名、内部函数 `_` 前缀、公开函数带英文前置文档注释）。

### 文件与模块骨架

- 唯一交付文件：`lib/sort.ms`。文件头注释一句话说明职责；无 import（只依赖内建函数与运算符）；无模块级可变状态；不写 `__name__` 守卫（纯库文件，12-ms-style §2.2）。
- 模块经任务 24 的模块系统以 `"sort"` 名注册/解析，脚本侧用 `import "sort"` 引入，`sort.sort` 形式调用（05-modules §1 绑定规则）。

### 公开函数

```ms
// sort sorts xs in place in ascending order and returns nil.
// The sort is stable: equal elements keep their original relative order.
// Elements are compared with the < operator. Raises TypeError if xs is not
// a list; a TypeError raised by an incomparable element pair propagates.
func sort(xs)

// sorted returns a new list containing the elements of xs in ascending
// order; xs is not modified. Comparison and error semantics match sort.
func sorted(xs)

// sortBy sorts xs in place in ascending order of key(x) and returns nil.
// key must be callable and is called exactly once per element, before any
// comparison; keys are compared with the < operator. The sort is stable.
// Raises TypeError if xs is not a list or key is not callable; an exception
// raised by key propagates.
func sortBy(xs, key)

// reverse reverses xs in place and returns nil.
// Raises TypeError if xs is not a list.
func reverse(xs)

// binarySearch returns the leftmost insertion index of v in xs, which must
// already be sorted in ascending order by < (bisect-left semantics): all
// elements before the result are < v. The result is unspecified when xs is
// not sorted; this is not checked.
func binarySearch(xs, v)

// isSorted reports whether xs is in ascending order by <. Adjacent equal
// elements count as sorted; empty and single-element lists are sorted.
func isSorted(xs)
```

### 语义与边界约定

- 参数校验：六个函数中，`sort`/`sorted`/`sortBy`/`reverse`/`binarySearch`/`isSorted` 的第一参数先经内部 `_checkList(xs, funcName)` 校验——`isinstance(xs, list)` 不成立时 `raise TypeError(f"sort.{funcName}: xs must be a list, got {str(type(xs))}")`。`sortBy` 追加 `callable(key)` 校验，不可调用时 `raise TypeError("sort.sortBy: key must be callable")`。其余实参（如 `binarySearch` 的 `v`）不做前置类型校验，不可比较时的 `TypeError` 由 `<` 运算符在比较点抛出并原样传播。
- 比较约定：默认排序只使用 `<` 运算符（不使用 `<=`，保证对自定义 `__lt__` 的最小协议依赖）。比较不求值相等性：两元素 `a`、`b` 满足 `not (a < b)` 且 `not (b < a)` 即视为「顺序等价」，其相对位置由稳定性契约保护。`nil`、混合不可比较类型（如 `1` 与 `"a"`）的比较结果由运算符语义决定（假定为 `TypeError`，见「设计依据」），本模块不捕获。
- 稳定性语义：`sort`/`sorted`/`sortBy` 均为**稳定排序**——顺序等价的元素在结果中保持原列表中的相对先后顺序。该契约对 `sortBy` 尤其重要：多轮按键排序可叠加（先按次要键、再按主要键）实现多级排序。
- 键函数约定（`sortBy`）：`key` 是一元可调用对象；每个元素**恰好调用一次**，在第一次比较之前全部求值完毕（Schwartzian 变换式预提取），因此 `key` 的调用次数恒为 `len(xs)`，与数据分布无关。比较只作用于预提取的键，不重复调用 `key`。`key` 抛出的异常不经包装直接传播，此时 `xs` 处于未修改状态（键提取发生在任何元素移动之前）。
- `sorted` 的不变性：`sorted(xs)` 先经 `list(xs)` 做浅拷贝再就地排序副本；原列表及其元素对象均不被修改（浅拷贝：元素对象本身共享）。
- `binarySearch` 前置条件：`xs` 必须已按 `<` 升序排列；不满足时返回值无定义（不报错、不校验，与 Go `sort.Search` 的契约风格一致）。返回值为最左插入点：若 `v` 已存在，返回其首个出现的下标；若不存在，返回值 `i` 满足插入后 `xs` 仍有序；空列表返回 `0`，全部元素小于 `v` 时返回 `len(xs)`。
- `isSorted`：相邻元素满足 `xs[i] < xs[i - 1]` 即判定无序返回 `false`；相等相邻不算无序（`[1, 1]` 为 `true`），与稳定排序的「顺序等价」定义自洽。空列表与单元素列表返回 `true`。
- 浮点 `nan`：`nan` 与任何值的 `<` 均为假，含 `nan` 的列表排序结果仍确定（算法确定）但不构成全序，文档不承诺其位置语义；`binarySearch`/`isSorted` 对含 `nan` 的输入同理。

### 排序算法：自底向上迭代归并排序

核心排序采用自底向上（迭代式）归并排序，理由：天然稳定、最坏 O(n log n)、无递归调用（不受 VM 调用栈深度限制）、只需一次 O(n) 辅助分配。接口级结构（非完整实现）：

```ms
// _mergeSort sorts values[0..len(values)) in ascending order with a stable
// bottom-up merge sort. keys is nil (compare values directly) or a parallel
// list of precomputed keys of the same length, merged in lockstep with
// values; comparisons always read keys.
func _mergeSort(values, keys)

// _merge merges the two adjacent sorted runs [lo, mid) and [mid, hi) back
// into values (and keys when not nil), using buf (and keyBuf) as scratch.
func _merge(values, keys, buf, keyBuf, lo, mid, hi)
```

流程要点：

1. `n := len(values)`；`n < 2` 直接返回。`buf` 预分配为 `values` 的浅拷贝；`keys` 非 `nil` 时 `keyBuf` 同样预分配，整个排序过程复用，不做每趟分配。
2. 宽度倍增循环：`width` 从 1 起每次乘 2，直到 `width >= n`；每趟以 `lo` 步进 `2 * width`，取 `mid := min(lo + width, n)`、`hi := min(lo + 2 * width, n)`，调用 `_merge` 归并相邻两段。
3. 归并稳定性规则：每步从左右两段头部取键 `leftKey`、`rightKey`，当 `not (rightKey < leftKey)` 时取左段元素——相等（顺序等价）时左段（原下标更小的一段）优先，保证稳定。比较只读 `keys`（`keys is nil` 时直接读 `values`），写入同时落到 `values` 与 `keys`，两数组始终保持并行对应。
4. 段长不足时自然截断：`mid >= n` 时该组只剩一段，跳过归并。

`sort` 以 `_mergeSort(xs, nil)` 调用；`sorted` 以 `_mergeSort(copy, nil)` 调用；`sortBy` 先构建 `keys := []` 并逐元素 `keys.append(key(xs[i]))`，再以 `_mergeSort(xs, keys)` 调用（键与值并行归并，稳定性由第 3 点保证）。

### 二分查找与辅助函数

```ms
// _checkList raises TypeError naming funcName when xs is not a list.
func _checkList(xs, funcName)
```

- `binarySearch`：标准 lo/hi 循环——`lo := 0`、`hi := len(xs)`；`while lo < hi` 时 `mid := (lo + hi) // 2`，`xs[mid] < v` 则 `lo = mid + 1`，否则 `hi = mid`；返回 `lo`。只用 `<`，与排序的比较约定一致。
- `reverse`：双指针就地交换——`i := 0`、`j := len(xs) - 1`，`while i < j` 交换 `xs[i]` 与 `xs[j]` 后相向收缩。O(n) 时间、O(1) 额外空间。
- `isSorted`：`for i in range(1, len(xs))` 单遍扫描，发现 `xs[i] < xs[i - 1]` 即返回 `false`，扫完返回 `true`。
- `_checkList` 供六个公开函数复用，消息格式统一（见「语义与边界约定」）。

### 设计取舍说明

- `sort.reverse(xs)` 裁定为**原地反转**（等价 Python `list.reverse()`），而非「降序排序」：07-stdlib §5 中 `sort` 注释 "sorts in place"、`sorted` 注释 "returns a new list"，而 `reverse` 无排序类注释，取反转义最贴合签名；降序排序可由 `sort` + `reverse` 组合，或使用内建 `sorted(xs, reverse=true)`（03-syntax §9，实现属任务 26）。规范此处存在歧义，本裁定在实现评审时应显式确认。
- 不提供比较函数（cmp 风格）参数：07-stdlib §5 清单未列入，与 Python 3 弃用 `cmp` 仅保留 `key` 的取舍一致；需要自定义序时定义 `__lt__`（默认排序）或传键函数（`sortBy`）。也不提供 `sortedBy`/`binarySearchBy` 等清单外变体，保持与 §5 一一对应、不增删。
- 内建 `sorted`（任务 26）与本模块 `sort.sorted` 并存：前者是语言内建、带 `key`/`reverse` 参数；后者是 §5 规定的最小签名模块函数，内部直接调 `_mergeSort` 而非委托内建，保证本模块可独立验收、不引入对任务 26 的额外依赖。
- 选迭代归并而非 Timsort/quicksort+插入排序混合：纯脚本实现下常数因子差异有限，稳定性与最坏复杂度保证优先；自适应运行检测等优化列入性能路线图（任务 64 基准套件落地后再评估）。

## 实现步骤

1. 建 `lib/sort.ms` 骨架：文件头注释、六个公开函数的签名与英文文档注释，函数体先 `pass`。验证：`import "sort"` 成功，`sort.sort` 等六个名字均可访问（`dir(sort)` 或逐个属性读取）。
2. 实现 `_checkList`、`reverse`、`isSorted`（无算法依赖的最小子集）。验证：ms 脚本断言反转（奇/偶/空长度）、有序判定的各分支与非 list 实参的 `TypeError`。
3. 实现 `_merge`/`_mergeSort` 默认比较路径（`keys is nil`）与 `sort`/`sorted`。验证：空表、单元素、已序、逆序、随机、含重复元素的就地排序结果；`sorted` 不改原列表且返回新对象。
4. 实现 `sortBy`：键预提取 + 并行键归并路径。验证：键函数排序结果、`key` 调用次数恰为 `len(xs)`（闭包计数器断言）、稳定性（等键元素保持原序）、`key` 抛异常时原列表未被修改。
5. 实现 `binarySearch`。验证：命中/未命中的插入点、重复元素取最左、空列表、插入点为 `len(xs)` 的边界。
6. 补齐全部参数校验与错误路径（六个函数的 `TypeError` 分支）。验证：逐函数的负例断言。
7. 编写完整测试文件（见「测试方案」），经仓库根 `run_tests.py` 调用 mslang CLI 全量运行通过。

## 测试方案

按 README 测试约定：本任务晚于任务 40（testing 模块），测试一律使用 `testing` 与 `testing/assert` 模块。测试文件 `tests/ms/stdlib/sort_test.ms`，测试函数以 `test` 开头，末尾 `testing.run()`；由 `run_tests.py` 递归发现并驱动。异常类断言用 `assert.raises`（07-stdlib §19 既定接口）；闭包计数、别名可见性等过程性断言在测试函数内用 `assert.equal`/`assert.isTrue` 完成。本任务只交付设计文档，脚本随实现编写。

覆盖清单：

- `sort`：
  - 基本：空表、单元素、已升序、逆序、随机序列（如 `[5, 2, 8, 1, 9, 3]`）、含重复元素（`[3, 1, 3, 2, 1]`）、负数与零、int/float 混合（`[2, 1.5, 1]`）、字符串列表（码点字典序）；排序后与期望 list 用 `==` 比对。
  - 就地性：`alias := xs` 后 `sort.sort(xs)`，断言 `alias` 同步变化、返回值为 `nil`。
  - 稳定性：元素为 `[key, originalIndex]` 对的列表，经 `sort.sortBy` 按键排序后等键对按下标升序（此项与 `sortBy` 共用，见下）；`sort` 自身的稳定性经等值自定义实例（`__lt__` 恒判等价的 class）验证——若实现时 class 魔术方法（任务 25）语义有变，可仅用 `sortBy` 路径覆盖稳定性并在注释中说明。
  - 负例：`assert.raises(TypeError, lambda: sort.sort(1))`、字符串实参、混合不可比较元素 `[1, "a"]`（比较点 `TypeError` 传播）。
- `sorted`：返回值是新列表（修改返回值不影响原列表）；原列表内容与顺序不变；语义与 `sort` 共用同一组数据用例。
- `sortBy`：
  - 键函数：`sortBy(xs, lambda x: x[0])` 对二元组列表按键排序；取反键 `lambda x: -x` 得降序；字符串长度键（闭包捕获外层偏移量的键函数，覆盖任务 14 的闭包捕获语义）。
  - 调用次数：闭包计数器（`count := 0` + 内层函数 `count += 1`，或等价 list 单元写法）断言 `key` 恰好被调用 `len(xs)` 次。
  - 稳定性：等键元素保持原相对顺序（`[[2, "a"], [1, "b"], [2, "c"], [1, "d"]]` 按 `x[0]` 排序后字符串部分顺序为 `["b", "d", "a", "c"]`）；两轮叠加排序（先次要键后主要键）结果与单轮多键期望一致。
  - 键异常传播：`key` 对某元素 `raise ValueError(...)`，断言异常类型与消息原样传出、且原列表未被修改。
  - 负例：非 callable 键（`sortBy(xs, 42)`）抛 `TypeError`；非 list 第一参数抛 `TypeError`。
- `reverse`：空表、单元素、奇数长度、偶数长度；就地性（别名可见）；返回值 `nil`；非 list 实参抛 `TypeError`。
- `binarySearch`：
  - 命中：`[1, 3, 5, 7]` 中查 `5` 返回 `2`；查重复值（`[1, 2, 2, 2, 3]` 查 `2`）返回最左下标 `1`。
  - 未命中：插入点居中（查 `4` 返回 `2`）、为最左（查 `0` 返回 `0`）、为最右（查 `9` 返回 `len(xs)`）；空列表返回 `0`。
  - 一致性抽查：把返回值作为插入点执行 `xs.insert(i, v)` 后 `sort.isSorted(xs)` 为 `true`（依赖任务 16 的 `insert`）。
  - 负例：非 list 实参抛 `TypeError`；对无序列表的行为不纳入断言（契约明示无定义）。
- `isSorted`：空表与单元素为 `true`；升序（含相邻相等 `[1, 1, 2]`）为 `true`；任一下降对（`[1, 3, 2]`）为 `false`；非 list 实参抛 `TypeError`。

## 验收标准

- [ ] `lib/sort.ms` 存在，实现且仅实现 07-stdlib §5 的六个函数 `sort` / `sorted` / `sortBy` / `reverse` / `binarySearch` / `isSorted`；无 C 代码，无模块级可变状态，无 `__name__` 守卫。
- [ ] ms 代码符合 12-ms-style：4 空格缩进、行宽 120、不遮蔽内建名、内部函数 `_` 前缀（`_mergeSort`/`_merge`/`_checkList`）、公开函数带英文前置文档注释、文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- [ ] 排序算法为稳定的自底向上迭代归并排序：顺序等价元素保持原相对顺序（`sort`/`sorted`/`sortBy` 三者均满足）；最坏时间复杂度 O(n log n)，辅助空间 O(n) 且整个排序过程只分配一次；无递归。
- [ ] 比较约定落地：默认排序只使用 `<`；`sortBy` 的键函数每元素恰好调用一次且在首次比较前完成；`key` 或 `<` 抛出的异常原样传播，`key` 异常时原列表未被修改。
- [ ] `sorted` 返回新列表且不修改入参；`sort`/`sortBy`/`reverse` 就地修改且返回 `nil`；`binarySearch` 为最左插入点语义；`isSorted` 对空表/单元素/含相邻相等的升序表返回 `true`。
- [ ] 参数校验完整：六个函数对非 list 第一参数抛 `TypeError`，`sortBy` 对非 callable 键抛 `TypeError`，消息含函数名。
- [ ] `import "sort"` 在解释器中可用，模块经 `lib/` 解析档命中（机制属任务 24，本任务不改动加载器）。
- [ ] `tests/ms/stdlib/sort_test.ms` 覆盖「测试方案」全部清单项，`run_tests.py` 全量通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 14 的接口假定（闭包按引用捕获）及比较运算符语义假定（混合类型 `<` 抛 `TypeError`、str 码点字典序）在实现时已对齐对应任务文档。
