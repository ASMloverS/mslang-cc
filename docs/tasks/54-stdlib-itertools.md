# 54 标准库：itertools

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [26 for-in 迭代协议](26-iteration-protocol.md) |

## 任务目标

交付纯 .ms 实现的标准库模块 `lib/itertools.ms`：一组惰性迭代器工具——无限迭代器（`count`/`cycle`/`repeat`）、有限流变换（`chain`/`islice`/`zipLongest`/`takewhile`/`dropwhile`/`accumulate`）与分组组合（`groupby`/`permutations`/`combinations`），共十二个公开函数，API 清单以 `docs/language/07-stdlib.md` §22 为准。模块不含任何 C 代码；由于语言没有 `yield`（生成器列入路线图，见 `docs/language/11-project-layout.md`），全部迭代器实现为内部 class 实例，经任务 26 的实例慢路径（`__iter__`/`__next__`，耗尽抛 `StopIteration`）接入 `for-in` 与内建 `iter`/`next`——这是本任务依赖任务 26 的原因。完成后，脚本可以 `import "itertools"` 使用全部十二个函数；本任务通过 `tests/ms/` 下的 ms 脚本测试（testing 模块，任务 40 已先于本任务完成）独立验收。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`itertools` 为纯脚本模块（`lib/` 目录），功能是「迭代器工具」。
  - §22 itertools：本模块的完整公开 API 清单，共十二个函数：

    ```ms
    itertools.count(start=0, step=1)  itertools.cycle(xs)  itertools.repeat(v, n=-1)
    itertools.chain(a, b, ...)  itertools.islice(it, start, stop, step=1)
    itertools.zipLongest(a, b, fill=nil)
    itertools.groupby(xs, key)
    itertools.permutations(xs, r)  itertools.combinations(xs, r)
    itertools.accumulate(xs, f=nil)  itertools.takewhile(pred, xs)  itertools.dropwhile(pred, xs)
    ```

- `docs/language/02-types.md`
  - §5.1 list 语义：下标读写、`append`、`len(xs)`，各迭代器类以 list 为内部缓存与产出容器。
  - §7 迭代协议：`iter(xs)` 调用 `xs.__iter__()`；`next(it)` 调用 `it.__next__()`，耗尽抛 `StopIteration`。
- `docs/language/03-syntax.md`
  - §4 函数：默认参数（`n = -1`）、`*rest` 可变参数（`chain(a, b, ...)` 的实现手段）、`lambda` 单表达式体（测试与调用侧的谓词/键函数形态）。
  - §5 class：`__init__` 中经 `self.x = ...` 动态建属性、单继承（各迭代器类继承公共基类）、`_` 前缀内部属性约定。
  - §8 作用域：名字解析局部 → 闭包外层 → 模块全局 → 内建。
  - §9 内建函数：`iter`/`next`/`list`/`len`/`range`/`isinstance`/`callable`/`str` 与 f-string。
- `docs/language/04-exceptions.md` §4：`StopIteration`/`TypeError`/`ValueError` 在异常层级中的位置；§5：`StopIteration` 在 `for` 循环中被自动捕获终止迭代，显式 `next()` 由调用者处理——本模块的迭代器类正是在 `__next__` 中抛 `StopIteration` 来表达耗尽。
- `docs/language/05-modules.md` §2：`lib/` 纯脚本模块在解析顺序第一档「内建/已注册模块」中命中；模块的发现、执行与缓存由任务 24（模块系统）负责，本任务只交付模块源码，不涉及加载机制本身。
- `docs/language/11-project-layout.md` §1：纯脚本标准库模块置于 `lib/`；§4 测试策略：脚本测试位于 `tests/ms/`。同文档确认生成器（`yield`）列入路线图——本模块不得以生成器为前提设计。
- `docs/language/12-ms-style.md`：§2 源文件结构、§3 格式化（4 空格缩进、行宽 120、禁止单行块）、§4 命名（class 大驼峰、内部函数与内部属性 `_` 前缀、禁止单字母名字——清单中的 `f`/`r` 等示意名在实现中改名）、§5.4 异常使用约定、§5.6 模块级状态、§6 注释与文档（公开函数的前置英文文档注释，代码块围栏用 `ms`）。
- [任务 26 for-in 迭代协议](26-iteration-protocol.md)：已定稿的协议接口是本任务的对齐基准——实例实现 `__iter__`/`__next__` 即接入 `for-in`；`msIterNext` 实例慢路径仅在 `__next__` 出口吞下 `StopIteration`，其余异常原样传播；迭代器是一次性对象，耗尽后停留在耗尽态；`iter(it)` 对迭代器幂等（返回自身）。本模块全部迭代器类遵守同一约定，不另起机制。
- [任务 40 标准库：testing 与 testing/assert](40-stdlib-testing.md)：测试接口 `assert.equal`/`assert.isTrue`/`assert.raises`/`testing.run` 按 07-stdlib §19 的既定命名引用。
- 补充假定：内建 `list(x)` 接受任意可迭代对象并经协议物化为新 list（任务 10/16 范畴，测试的收集口径依赖此行为）；class 与魔术方法运行时的接口名以任务 15/25 的文档为准（经任务 26 传递依赖，本任务不直接引用其 C 接口）。以上假定若与对应任务文档不同，**实现时以对应任务文档定名为准**。

## 详细设计

本任务是纯 .ms 模块，无 C 结构体与 C 函数签名；接口级设计以 ms 函数签名、内部 class 布局与语义约定表达。ms 代码遵循 `docs/language/12-ms-style.md`（4 空格缩进、行宽 120、公开函数带英文前置文档注释、内部 class 与内部属性 `_` 前缀）。

### 文件与模块骨架

- 唯一交付文件：`lib/itertools.ms`。文件头注释一句话说明职责；无 import（只依赖内建函数与内建异常类）；无模块级可变状态；不写 `__name__` 守卫（纯库文件，12-ms-style §2.2）。
- 模块经任务 24 的模块系统以 `"itertools"` 名注册/解析，脚本侧用 `import "itertools"` 引入，`itertools.count` 形式调用（05-modules §1 绑定规则）。
- 文件内结构（12-ms-style §2 的固定段落顺序）：文件头注释 → 内部 `_fetch`/`_check*` 助手函数 → 十二个公开函数 → 迭代器 class 定义收拢于文件后段。单文件规模预计 400–500 行，处于 §5.2 的 500 行软阈值内；若实现时超出，按 class 分组拆注释段即可，不拆文件（模块名与文件一一对应）。

### 惰性求值约定

本模块全体函数遵守统一的惰性约定（模块文档注释中显式声明）：

1. **调用不消费输入**：每个公开函数在调用时只完成参数校验与经内建 `iter()` 取输入迭代器，立即返回迭代器实例；输入元素在每次 `__next__` 时才被拉取。例外（语义要求的物化点，逐一注明）：`permutations`/`combinations` 在调用时把输入复制为 pool list；`cycle` 在首遍迭代中边产出边缓存（输入为无限迭代器时首遍不终止，与 Python `cycle` 一致）；`groupby` 每次产出时物化当前分组为 list。
2. **一次性迭代器**：与任务 26 的约定一致，迭代器耗尽后停留在耗尽态，再次 `next` 仍抛 `StopIteration`，不复位。
3. **自迭代**：所有迭代器实例的 `__iter__` 返回 `self`，`iter(it)` 幂等，可直接用于 `for-in`。
4. **产出形态**：多元素产出（`zipLongest` 的配对、`groupby` 的键值对）一律为 2 元素 list，与任务 26 的 dict 键值对、`enumerate`/`zip` 产出形态一致。

### 迭代器基类与内部助手

```ms
// _Iterator is the common base of all itertools iterators: __iter__ returns
// the iterator itself, so every subclass instance satisfies the iteration
// protocol directly.
class _Iterator {
    func __iter__(self) {
        return self
    }
}
```

- 十二个迭代器类（`_Count`/`_Cycle`/`_Repeat`/`_Chain`/`_ISlice`/`_ZipLongest`/`_GroupBy`/`_Permutations`/`_Combinations`/`_Accumulate`/`_TakeWhile`/`_DropWhile`）均继承 `_Iterator`，各自只定义 `__init__` 与 `__next__`；耗尽时 `raise StopIteration(...)`，由任务 26 的 `msIterNext` 慢路径在边界消化。
- 手动驱动内层迭代器是本模块无法回避的协议动作（`chain`/`zipLongest` 等必须跨 `__next__` 调用持有并步进内层迭代器），统一封装为一个内部助手，避免在每个类里散写 `try/except StopIteration`：

  ```ms
  // _fetch advances it and returns (value, true); on clean exhaustion it
  // returns (nil, false). Any other exception propagates unchanged.
  func _fetch(it) {
      try {
          return next(it), true
      } except StopIteration {
          return nil, false
      }
  }
  ```

  该助手是本模块对 12-ms-style §5.4「不以异常做常规控制流」的唯一例外：此处 `StopIteration` 的捕获对象是协议机制本身（04-exceptions §5 明确显式 `next()` 由调用者自行处理），在评审中以此为理由说明，且例外收拢于单点。
- 校验助手：`_checkInt(value, funcName, paramName)`（`type(value) != int` 时抛 `TypeError`，消息形如 `f"itertools.{funcName}: {paramName} must be an int, got {str(type(value))}"`；bool 因 `type(true) != int` 自然被拒）、`_checkIterable` 不单独存在——输入可迭代性统一由内建 `iter()` 在调用点判定，非可迭代对象的 `TypeError` 由其抛出并原样传播。

### 无限迭代器

```ms
// count returns an infinite iterator yielding start, start + step, ... .
// start and step must be int or float. Raises TypeError otherwise.
func count(start = 0, step = 1)

// cycle returns an infinite iterator repeating the elements of xs in order.
// Elements are cached during the first pass; if xs is empty the iterator is
// immediately exhausted. Raises TypeError if xs is not iterable.
func cycle(xs)

// repeat returns an iterator yielding value n times; n = -1 (the default)
// means forever. Raises TypeError if n is not an int, ValueError if n < -1.
func repeat(value, n = -1)
```

- `_Count`：字段 `_current`、`_step`；`__next__` 返回 `_current` 后 `_current += _step`，永不耗尽。`start`/`step` 限定 int 或 float（`isinstance(x, int) or isinstance(x, float)`），其余类型抛 `TypeError`——规范未限定类型，此收窄使浮点计数可用（对齐 Python）又避免任意 `+` 对象的隐式语义，属显式裁定。
- `_Cycle`：字段 `_source`（首遍的内层迭代器，耗尽后置 nil）、`_saved`（缓存 list）、`_index`（回放游标）。`__next__`：`_source` 非 nil 时经 `_fetch` 取一元素，命中则 `append` 到 `_saved` 并返回；未命中则置 `_source = nil`、`_index = 0`——此时 `_saved` 为空则抛 `StopIteration`（空输入即刻耗尽）。回放阶段返回 `_saved[_index]` 并把 `_index` 环回（`(_index + 1) % len(_saved)`）。
- `_Repeat`：字段 `_value`、`_remaining`（`-1` 表无限）。`__next__`：`_remaining == 0` 抛 `StopIteration`；`_remaining > 0` 时自减；返回 `_value`。`n` 校验：非 int 抛 `TypeError`；`n < -1` 抛 `ValueError`（`-1` 已被规范定义为无限哨兵，更小取值无含义，快速失败优于静默产空，属显式裁定）。

### 有限流变换

```ms
// chain returns an iterator yielding the elements of each iterable in turn.
// Raises TypeError if any argument is not iterable.
func chain(*iterables)

// islice returns an iterator yielding items of it at absolute indexes
// start, start + step, ... while the index is below stop. stop = nil means
// unbounded. start must be >= 0 and step >= 1 (ValueError otherwise);
// a negative stop raises ValueError, and stop <= start yields nothing.
func islice(it, start, stop = nil, step = 1)

// zipLongest returns an iterator yielding [left, right] pairs from two
// iterables, substituting fill for the shorter side after it is exhausted;
// the pair stream ends when both sides are exhausted.
func zipLongest(left, right, fill = nil)

// takewhile returns an iterator yielding items of xs while pred(item) is
// truthy, then is permanently exhausted. The first failing item is consumed.
func takewhile(pred, xs)

// dropwhile returns an iterator skipping items of xs while pred(item) is
// truthy, then yielding the first failing item and everything after it.
func dropwhile(pred, xs)

// accumulate returns an iterator of running fold results: the first item as
// is, then fn(acc, item) at each step; fn = nil means addition.
func accumulate(xs, fn = nil)
```

命名说明：07-stdlib §22 清单中的 `it`/`f` 是示意名；12-ms-style §4 禁止单字母名字，`accumulate` 的 `f` 在实现中定名 `fn`（与任务 55 functools 的同一裁定一致），`islice` 的 `it` 保留（双字母且语义明确）。对外语义与文档签名一致。

- `_Chain`：字段 `_sources`（调用时经 `iter()` 逐一取得的迭代器 list）、`_index`。`__next__` 循环：`_index` 越界则抛 `StopIteration`；否则 `_fetch(_sources[_index])`，命中返回值，未命中 `_index += 1` 续循环。零参 `chain()` 合法，即刻耗尽。
- `_ISlice`：字段 `_source`、`_index`（已拉取的绝对下标）、`_start`、`_stop`（nil 表无界）、`_step`、`_done`。`__next__`：`_done` 则抛 `StopIteration`；先把 `_index` 推进到 `_start`（逐个 `_fetch` 丢弃，内层耗尽则置 `_done` 并抛 `StopIteration`）；`_stop` 非 nil 且 `_index >= _stop` 时置 `_done` 抛 `StopIteration`；取当前元素作为产出，`_index += 1`，再预丢弃 `_step - 1` 个元素（丢弃越过 `_stop` 无害——此后本就不会再产出）。注意产出条件以「产出下标 < stop」判定，丢弃阶段不做 stop 检查，语义即产出下标序列 `start, start+step, ...` 中小于 `stop` 的项。
- `_ZipLongest`：字段 `_left`/`_right`（迭代器）、`_leftDone`/`_rightDone`、`_fill`。`__next__`：两者皆 done 抛 `StopIteration`；未 done 侧经 `_fetch` 取值（未命中则标记该侧 done），已 done 侧用 `_fill`；产出 `[lv, rv]`。只支持两个可迭代对象（清单签名 `zipLongest(a, b, fill=nil)` 即两参形态，刻意不扩展为 Python 的任意元数）。
- `_TakeWhile`：字段 `_source`、`_pred`、`_done`。`__next__`：`_done` 抛 `StopIteration`；`_fetch` 未命中则置 `_done` 抛 `StopIteration`；`pred(v)` 为真返回 `v`，为假置 `_done` 并抛 `StopIteration`（该元素被消费、不产出，对齐 Python）。
- `_DropWhile`：字段 `_source`、`_pred`、`_dropping`（初始 `true`）。`__next__`：`_dropping` 为真时循环 `_fetch` 丢弃，直到首个 `pred(v)` 为假的元素——置 `_dropping = false` 并返回该元素；内层耗尽则 `StopIteration` 原样传播。此后 `_dropping` 为假，`__next__` 退化为 `return next(self._source)`。
- `_Accumulate`：字段 `_source`、`_fn`（nil 表加法）、`_acc`、`_started`。`__next__`：未开始则取首元素作 `_acc`（内层耗尽的 `StopIteration` 原样传播）、置 `_started = true` 并返回；否则取下一元素，`acc := _fn(acc, v)`（`_fn is nil` 时 `acc + v`），返回新 `_acc`。
- `pred`/`fn`/`key` 的可调用性不做前置校验（与任务 55 的 `reduce` 同一理由）：调用不可调用值时 VM 自然抛 `TypeError`；其抛出的异常不经包装原样传播（任务 26 慢路径只吞 `StopIteration`）。**注意**：`pred`/`fn`/`key` 若抛出 `StopIteration` 会被协议边界吞掉并表现为「迭代提前结束」——这是任务 26 协议的固有语义，本模块在文档注释中显式注明、不做防御。

### 分组与组合

```ms
// groupby returns an iterator yielding [key, groupList] pairs for maximal
// runs of consecutive items with equal key(item). key is required and is
// called exactly once per item. Each group is materialized as a new list.
func groupby(xs, key)

// permutations returns an iterator yielding r-length permutations of the
// items of xs (by position, no repetition) as new lists, in lexicographic
// index order. r must be an int (TypeError); r < 0 raises ValueError;
// r > len(xs) yields nothing; r == 0 yields one empty list.
func permutations(xs, r)

// combinations returns an iterator yielding r-length combinations of the
// items of xs (by position) as new lists, in lexicographic index order.
// Error and edge semantics match permutations.
func combinations(xs, r)
```

- `_GroupBy`：字段 `_source`、`_key`、`_pending`/`_hasPending`（跨分组预读的一个元素）、`_done`。`__next__`：`_done` 抛 `StopIteration`；取本组首元素（`_hasPending` 时用 `_pending` 并清标志，否则 `_fetch`——未命中则置 `_done` 抛 `StopIteration`）；`groupKey := _key(first)`、`group := [first]`；随后循环 `_fetch`，`k := _key(v)` 与 `groupKey` 用 `==` 比较，相等则 `group.append(v)`，不等则记入 `_pending`/`_hasPending` 并结束本组；内层耗尽时置 `_done` 结束本组。产出 `[groupKey, group]`。
  - **规范歧义的裁定**：Python 的 `groupby` 产出共享底层迭代器的子迭代器（外层前进会使未消费的子迭代器失效），这在无 `yield` 的纯 .ms 实现里需要复杂的共享状态；本模块裁定为**每组物化为新 list**，语义是「Python groupby 各组立即收集」的等价物，键仍按调用序逐组惰性产出（外层不推进就不继续读输入）。清单中 `groupby(xs, key)` 的 `key` 为必选（无 Python 的缺省恒等形式），按清单执行。
- `_Permutations` / `_Combinations`：调用时 `pool := list(xs)` 物化并记 `n := len(pool)`；`r > n` 时迭代器即刻耗尽；`r == 0` 时产出一次空 list 后耗尽。按下标元组生成（元素按位置参与，重复值各自出现，与 Python 一致）：
  - `_Permutations` 字段：`_pool`、`_n`、`_r`、`_indices`（当前下标元组，初始 `[0.._r-1]`）、`_used`（长度 `_n` 的 bool list）、`_first`、`_done`。`__next__`：`_done` 抛 `StopIteration`；`_first` 为真则清标志并直接产出当前映射。否则推进：从 `_r - 1` 向 `0` 找最右可增位 `i`——释放 `_used[_indices[i]]` 后在 `( _indices[i], _n )` 内找首个未用下标 `j`；找到则 `_indices[i] = j` 并标记使用，随后 `i + 1 .. _r - 1` 位依次填最小未用下标，产出映射；找不到可增位则置 `_done` 抛 `StopIteration`。产出为按下标映射的**新 list**（`[pool[_indices[0]], ...]`）。
  - `_Combinations` 字段：`_pool`、`_n`、`_r`、`_indices`（初始 `[0.._r-1]`，严格递增不变量）、`_first`、`_done`。推进：从右向左找首个满足 `_indices[i] < _n - _r + i` 的位 `i`，`_indices[i] += 1` 后把后续位依次置为前一位 `+1`；找不到则置 `_done`。产出同样是新 list。
  - 两算法均为经典里程表（odometer）式推进，每步 O(r) 摊还，无递归（不受 VM 调用栈深度限制），状态只含 O(n + r) 的数组。

### 设计取舍说明

- **迭代器载体选 class 实例而非闭包**：语言无 `yield`（11-project-layout 路线图），而迭代协议（任务 26）只认「`__iter__`/`__next__` 实例」与内建原生迭代器两种形态；闭包函数值不是协议对象，无法接入 `for-in`。故每个公开函数返回继承 `_Iterator` 的内部 class 实例。`count` 一类简单迭代器本可用闭包 + 哨兵表达，但会破坏协议兼容性，一律不采用。
- **`islice` 的签名裁定**：清单写作 `islice(it, start, stop, step=1)`，未提供 Python 的单数字简写（`islice(it, 5)` 表 stop）。本模块按清单实现 `start` 必选、`stop = nil` 表无界（对齐 Python 的 `stop=None` 语义），不支持单数字简写——该形态与清单签名冲突，刻意收窄。
- **`zipLongest` 只收两个可迭代对象**（清单签名 `a, b`），不扩展 Python 的任意元数；需要多元时嵌套组合。`fill` 默认 `nil`，无法区分「未传 fill」与「fill 为 nil」，与清单签名一致，不引入哨兵。
- **`repeat` 的 `n = -1` 哨兵**：清单明确定义 `n=-1` 为无限；`n < -1` 无定义，裁定为 `ValueError`（快速失败），而非 Python 的「负数按 0 处理」——因为 `-1` 在此已非普通数值而是哨兵，静默产空会掩盖调用错误。
- **命名保留清单原样**：§22 清单混用 `zipLongest`（小驼峰）与 `takewhile`/`dropwhile`/`groupby`（全小写）。07-stdlib 是命名权威，本模块逐字保留，不在模块层做风格统一。
- **产出配对用 list 不用 tuple**：与任务 26 的 dict 键值对、`enumerate`/`zip` 的既定产出形态一致；tuple（任务 32）落地后是否迁移由彼时的一致性评审决定，本任务不先行。
- **参数校验时机**：形状错误（类型、负数参数）在**调用时**校验并快速失败（与任务 55 一致）；输入元素层面的错误（不可比较、谓词异常）在迭代点自然抛出、原样传播。

## 实现步骤

1. 建 `lib/itertools.ms` 骨架：文件头注释、惰性约定说明、`_Iterator` 基类、`_fetch` 与 `_checkInt` 助手、十二个公开函数的签名与英文文档注释（函数体先 `pass`）。验证：`import "itertools"` 成功，十二个名字均可访问（逐个属性读取）。
2. 实现 `count`/`repeat`/`cycle` 三个无限迭代器及其 class。验证：ms 脚本以 `next()` 逐步断言 `count` 的等差序列（int 与 float 步长）、`repeat(v, 3)` 计次与 `repeat(v)` 无限形态经 `islice` 截断取样、`cycle("ab")` 首遍缓存与回放、空输入 `cycle([])` 即刻耗尽；`repeat` 的 `n < -1` 抛 `ValueError`、`count("a")` 抛 `TypeError`。
3. 实现 `chain` 与 `islice`。验证：`chain` 多源顺序拼接、含空源、零参即刻耗尽；`islice` 的 start/stop/step 各组合、stop 为 nil 无界、stop <= start 产空、负参 `ValueError`、对 `count(0)` 的无限输入截断（`islice(itertools.count(), 0, 5)` 收集为 `[0, 1, 2, 3, 4]`）。
4. 实现 `zipLongest`/`takewhile`/`dropwhile`/`accumulate`。验证：长短两侧 fill 替换与双双耗尽终止、`fill` 默认 nil；`takewhile` 消费首个失败元素后永久耗尽、`dropwhile` 首假后原样直通；`accumulate` 默认加法与自定义 `fn`、空输入产空。
5. 实现 `groupby`。验证：连续同键分组、键交替时同值键不合并（`[1, 1, 2, 1]` 分三组）、`_pending` 跨组首元素正确、空输入产空、键函数每元素恰好调用一次（闭包计数器断言）。
6. 实现 `permutations`/`combinations`。验证：小输入（n ≤ 4）的全量结果与期望清单逐一比对（个数、字典序、按位置参与）、`r == 0` 产一个空 list、`r > n` 产空、`r < 0` 抛 `ValueError`、非 int 的 `r` 抛 `TypeError`。
7. 惰性核查：为每个函数补「调用不消费输入」断言——构造带副作用的自定义可迭代 class（`__next__` 内闭包计数器），调用各函数后断言计数为 0（`permutations`/`combinations` 按物化约定除外、断言恰为 n 次）。验证：全部通过。
8. 编写完整测试文件（见「测试方案」），经仓库根 `run_tests.py` 调用 mslang CLI 全量运行通过。

## 测试方案

按 README 测试约定：本任务晚于任务 40（testing 模块），测试一律使用 `testing` 与 `testing/assert` 模块。测试文件 `tests/ms/stdlib/itertools_test.ms`，测试函数以 `test` 开头，末尾 `testing.run()`；由 `run_tests.py` 递归发现并驱动。异常断言用 `assert.raises`（07-stdlib §19 既定接口）；收集口径用 `list(it)` 物化后与期望 list 以 `assert.equal` 比对；无限迭代器一律经 `islice` 截断或定次 `next()` 取样，不构造无限收集。本任务只交付设计文档，脚本随实现编写。

覆盖清单（每个测试函数对应一个公开函数的一个侧面，命名 `testCountYieldsArithmeticSequence` 风格）：

- `count`：默认起点步长（前 5 项 `[0, 1, 2, 3, 4]`）、`count(10, -2)` 递减、float 步长 `count(0, 0.5)`；负例：`count("a")`、`count(0, "a")` 抛 `TypeError`。
- `cycle`：`list(islice(cycle("ab"), 0, 5)) == ["a", "b", "a", "b", "a"]`（覆盖首遍缓存与回放环回）；`cycle([])` 首次 `next` 即抛 `StopIteration`（`assert.raises`）；单元素循环；负例：不可迭代实参抛 `TypeError`。
- `repeat`：`list(repeat("x", 3))`、无限形态 `list(islice(repeat(7), 0, 3)) == [7, 7, 7]`、`repeat(v, 0)` 产空；耗尽后再 `next` 仍抛 `StopIteration`（一次性约定）；负例：`repeat(v, -2)` 抛 `ValueError`、`repeat(v, "x")` 抛 `TypeError`。
- `chain`：`list(chain([1, 2], [3], "ab")) == [1, 2, 3, "a", "b"]`（异质源）、含空 list 源、`chain()` 零参产空、源为迭代器（先 `iter` 再传入）；负例：含不可迭代实参抛 `TypeError`。
- `islice`：`islice(range(10), 2, 7)` → `[2, 3, 4, 5, 6]`、`islice(range(10), 1, 9, 3)` → `[1, 4, 7]`、`islice(xs, 2, nil)` 无界、`islice(xs, 5, 2)` 产空、`islice(xs, 0, 0)` 产空；负例：负 `start`、`step == 0`、负 `stop` 各抛 `ValueError`，非 int 抛 `TypeError`。
- `zipLongest`：`list(zipLongest([1, 2, 3], ["a"])) == [[1, "a"], [2, nil], [3, nil]]`（默认 fill）、显式 fill、左短右长对称、两侧等长、两侧皆空产空、产出为 2 元素 list；负例：不可迭代实参抛 `TypeError`。
- `groupby`：`groupby([1, 1, 2, 2, 1], identity)` 分三组且第三组为 `[1]`（同值不跨组合并）；`key` 按奇偶分组；组产出为新 list（修改产出组不影响后续迭代）；`key` 调用次数恰为元素个数（闭包计数器）；空输入产空。
- `permutations`：`permutations([1, 2, 3], 2)` 全量 6 项按字典序与期望清单逐一比对；`r == 1`；`r == n` 的全排列个数为阶乘（小 n 断言个数与无重复）；重复元素按位置参与（`permutations([1, 1], 2)` 产两个 `[1, 1]`）；`r == 0` 产 `[[]]`；`r > n` 产空；负例：`r < 0` 抛 `ValueError`、`r` 为 float 抛 `TypeError`。
- `combinations`：`combinations([1, 2, 3, 4], 2)` 全量 6 项字典序；`r == n` 产单项；`r == 0`、`r > n` 同 permutations 边界；负例同上。
- `accumulate`：默认加法 `list(accumulate([1, 2, 3, 4])) == [1, 3, 6, 10]`；自定义 `fn`（乘法、字符串拼接）；单元素原样产出且 `fn` 不被调用；空输入产空。
- `takewhile`/`dropwhile`：`takewhile(lambda x: x < 3, [1, 2, 3, 1])` → `[1, 2]`（失败元素消费且其后不产出）；`dropwhile` 同输入 → `[3, 1]`（首假后原样直通，不再判定）；谓词对全真/全假输入的两端边界。
- 协议与惰性横切：
  - 每个迭代器实例 `iter(it) is it` 不成立断言改为语义断言——`iter(it)` 返回的对象与 `it` 行为同一（继续取值得后续序列），并对实例直接 `for x in it` 收集（验证经任务 26 慢路径接入 for-in）；
  - 耗尽后再 `next` 仍抛 `StopIteration`（每类迭代器抽查）；
  - 「调用不消费输入」：自定义 `_CountingIterable`（`__next__` 内递增外层计数器），调用 `chain`/`islice`/`takewhile`/`dropwhile`/`accumulate`/`groupby`/`zipLongest` 后断言计数为 0，`permutations`/`combinations` 断言恰为 n 次；
  - `for` 循环体内 `break` 后迭代器状态保留（继续 `next` 取得断点后续），证明迭代器独立于循环机制。

## 验收标准

- [ ] `lib/itertools.ms` 存在，实现且仅实现 07-stdlib §22 的十二个函数 `count` / `cycle` / `repeat` / `chain` / `islice` / `zipLongest` / `groupby` / `permutations` / `combinations` / `accumulate` / `takewhile` / `dropwhile`；无 C 代码，无模块级可变状态，无 `__name__` 守卫。
- [ ] ms 代码符合 12-ms-style：4 空格缩进、行宽 120、无单行块、不遮蔽内建名、无单字母参数名（`fn`/`pred`/`key`）、内部 class 与内部属性 `_` 前缀、公开函数带英文前置文档注释、文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- [ ] 全部迭代器为继承 `_Iterator` 的 class 实例，`__iter__` 返回 `self`、耗尽抛 `StopIteration`、耗尽后不复位（任务 26 协议约定）；可直接用于 `for-in`、`iter`/`next` 与 `list()` 收集。
- [ ] 惰性约定落实：除显式注明的物化点（`permutations`/`combinations` 的 pool、`cycle` 的首遍缓存、`groupby` 的当前组）外，调用任一函数不消费输入元素；无限迭代器（`count`、无界 `repeat`、非空 `cycle`）可经 `islice` 截断收集。
- [ ] 边界与错误语义与本文一致：`repeat` 的 `n = -1` 无限 / `n < -1` 抛 `ValueError`；`islice` 的 `stop = nil` 无界、负参 `ValueError`；`zipLongest` 恰好两源、短侧填 `fill`、双双耗尽终止；`groupby` 键必选、按 `==` 连续分组、组物化为新 list；`permutations`/`combinations` 按位置参与、字典序、`r == 0` 产一个空 list、`r > n` 产空、`r < 0` 抛 `ValueError`；谓词/键/折叠函数抛出的非 `StopIteration` 异常原样传播。
- [ ] `import "itertools"` 在解释器中可用，模块经 `lib/` 解析档命中（机制属任务 24，本任务不改动加载器）。
- [ ] `tests/ms/stdlib/itertools_test.ms` 覆盖「测试方案」全部清单项（含协议接入、一次性约定、惰性横切断言），`run_tests.py` 全量通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对内建 `list()` 物化可迭代对象的假定及 class 运行时接口（任务 15/25，经任务 26 传递）在实现时已按对应任务文档对齐。
