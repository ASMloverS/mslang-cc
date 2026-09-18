# 55 标准库：functools

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [14 闭包与 upvalue](14-closures.md) |

## 任务目标

交付纯 .ms 实现的标准库模块 `lib/functools.ms`：一组高阶函数工具——折叠（`reduce`）、参数预绑定（`partial`）、LRU 记忆化装饰器（`lruCache`）、函数复合（`compose`），API 清单以 `docs/language/07-stdlib.md` §23 为准。模块不含任何 C 代码；`partial`/`lruCache`/`compose` 的包装函数全部实现为捕获外层局部变量的嵌套函数闭包，这是本任务依赖任务 14（闭包与 upvalue）的原因。完成后，脚本可以 `import "functools"` 使用全部四个函数；本任务通过 `tests/ms/` 下的 ms 脚本测试（testing 模块，任务 40 已先于本任务完成）独立验收。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`functools` 为纯脚本模块（`lib/` 目录）。
  - §23 functools：本模块的完整公开 API 清单，共四个函数：

    ```ms
    functools.reduce(f, xs, init)  // init is optional
    functools.partial(f, *args, **kwargs)
    functools.lruCache(maxSize=128)      // decorator usage: see below
    functools.compose(f, g)              // compose(f, g)(x) == f(g(x))
    ```

  - §23 同时注明：装饰器语法（`@decorator`）列入路线图，首版用显式包装 `fib = functools.lruCache()(fib)`。
- `docs/language/03-syntax.md`
  - §4 函数：定义侧支持 `*rest`（收集为元组）与 `**kw`（收集为 dict），次序固定；`lambda` 仅支持 `identList` 形参与单表达式体，**不能声明可变参数**——故本模块所有包装函数一律用嵌套 `func` 而非 `lambda`。
  - §8 作用域：闭包对外层局部变量按引用捕获，内层可直接写捕获变量——`partial`/`lruCache` 的预绑定参数与缓存 dict 依赖此语义。
- [04 语法分析器与 AST](04-parser-ast.md)：调用点**不引入** `*args`/`**kwargs` 展开语法（规范未列，列入路线图）。这是本任务的核心约束：纯 .ms 代码无法把运行期长度的参数序列直接转发给被包装函数，`partial` 与 `lruCache` 的设计围绕该限制展开（见「详细设计」）。
- `docs/language/02-types.md`：§5.2 tuple 不可变；§5.3 dict 保持插入序（`lruCache` 以插入序充当 LRU 顺序）；§6 tuple 递归按值可哈希（元素须可哈希），list/dict/set 不可哈希；§7 迭代协议（`reduce` 对 `xs` 用 `for ... in`）。
- `docs/language/04-exceptions.md` §4 内建异常层级：`TypeError` / `ValueError` / `RuntimeError` 及其子类 `NotImplementedError` 可用。
- `docs/language/05-modules.md` §2：`lib/` 纯脚本模块在解析顺序第一档「内建/已注册模块」中命中；模块的发现、执行与缓存由任务 24（模块系统）负责，本任务只交付模块源码，不涉及加载机制本身。
- `docs/language/11-project-layout.md` §1：纯脚本标准库模块置于 `lib/`；§4 测试策略：脚本测试位于 `tests/ms/`。
- `docs/language/12-ms-style.md`：§2 源文件结构、§4 命名（禁止单字母参数名，07-stdlib 清单中的 `f`/`g` 在实现中改名）、§5.6 模块级状态、§6 注释与文档（公开函数的前置英文文档注释，代码块围栏用 `ms`）。
- 依赖任务 14（闭包与 upvalue）的文档尚不存在：本文假定其交付 03-syntax §8 所述的按引用捕获语义（嵌套函数读写外层局部变量）。若任务 14 文档的实现细节与本文假定不同，实现时以对应任务文档定名为准。

## 详细设计

本任务是纯 .ms 模块，无 C 结构体与 C 函数签名；接口级设计以 ms 函数签名与语义约定表达。ms 代码遵循 `docs/language/12-ms-style.md`（4 空格缩进、行宽 120、小驼峰函数名、公开函数带英文前置文档注释）。

### 文件与模块骨架

- 唯一交付文件：`lib/functools.ms`。文件头注释一句话说明职责；无 import（只依赖内建函数与内建异常类）；无模块级可变状态；不写 `__name__` 守卫（纯库文件，12-ms-style §2.2）。
- 模块经任务 24 的模块系统以 `"functools"` 名注册/解析，脚本侧用 `import "functools"` 引入，`functools.reduce` 形式调用（05-modules §1 绑定规则）。
- 模块级常量（只读配置，不受 §5.6 限制）：

  ```ms
  // Maximum number of positional arguments _applyList can forward.
  _PARTIAL_MAX_ARGS := 16
  ```

### 公开函数

```ms
// reduce folds the items of xs into a single value by repeatedly applying
// fn(accumulator, item). With init, the fold starts from init and empty xs
// returns init. Without init, the first item seeds the accumulator and empty
// xs raises TypeError. More than one init raises TypeError.
func reduce(fn, xs, *init)

// partial returns a closure that calls fn with args prepended to the
// call-time positional arguments. Keyword arguments are captured in kwargs
// but cannot be forwarded (call-site unpacking is a roadmap item): non-empty
// kwargs raises NotImplementedError immediately.
func partial(fn, *args, **kwargs)

// lruCache returns a decorator that memoizes a function with a bounded
// least-recently-used cache of maxSize entries (default 128). maxSize must
// be a positive int (TypeError/ValueError otherwise). Call-time keyword
// arguments raise NotImplementedError; unhashable positional arguments
// propagate the dict's TypeError.
func lruCache(maxSize = 128)

// compose returns a closure computing fn composition:
// compose(outer, inner)(x) == outer(inner(x)).
func compose(outer, inner)
```

命名说明：07-stdlib §23 清单中的参数名 `f`/`g`/`xs` 是示意；12-ms-style §4 禁止单字母名字，实现中 `f` 定名 `fn`、`compose` 的两个参数定名 `outer`/`inner`，对外语义与文档签名一致。

### reduce：折叠

- 用 `*init` 可变参数区分「未传 init」与「init 为 nil」：`len(init) == 0` 表示未提供，`len(init) > 1` 抛 `TypeError(f"functools.reduce: expected at most 3 arguments, got {len(init) + 2}")`。这一形态避免引入模块级哨兵对象（12-ms-style §5.6），调用面与清单签名 `reduce(f, xs, init)` 完全一致。
- 流程：`started := false`；`for x in xs` 逐项——未开始则 `acc := x; started = true`，否则 `acc = fn(acc, x)`；若提供了 init 则先置 `acc := init[0]; started = true` 再进入循环。循环结束后未开始（空序列且无 init）抛 `TypeError("functools.reduce: empty sequence with no initial value")`（对齐 Python 文案风格）。
- 不校验 `fn` 的可调用性：`callable` 内建未在 v0.1–v0.4 任务清单中排期（任务 10 将其列入后续版本），调用不可调用值时 VM 自然抛 `TypeError`，语义不差于前置校验。

### partial：参数预绑定与定长分派

- `partial(fn, *args, **kwargs)` 捕获 `args`（tuple）与 `kwargs`（dict），返回嵌套函数闭包 `bound`：

  ```ms
  func partial(fn, *args, **kwargs) {
      if len(kwargs) > 0 {
          raise NotImplementedError(
              "functools.partial: keyword argument binding requires call-site unpacking (roadmap)")
      }
      func bound(*rest, **kw) {
          // merge args + rest, then forward via _applyList
      }
      return bound
  }
  ```

- **关键字参数限制（规范歧义的处理）**：语言无调用点解包（`f(*xs)` / `f(**kw)`），07-stdlib §23 的 `**kwargs` 只能完成「捕获」而无法完成「转发」。v0.4 的处理：捕获侧非空（`partial(fn, k=1)`）在 `partial` 调用时**立即**抛 `NotImplementedError`（快速失败，错误定位到绑定发生处）；调用侧非空（`bound(k=1)`）在 `bound` 内抛同一类型异常。调用点解包落地后删除这两处守卫即获得完整语义，对外签名不变。此偏差在本节与模块文档注释中显式承认。
- 位置参数合并：`allArgs := list(args)`，再把 `rest` 逐项 `append`（tuple 不可变，先复制为 list；`list(tuple)` 构造器属任务 32，v0.2 已交付）。
- 转发机制 `_applyList(fn, allArgs)`：模块内部函数（`_` 前缀，12-ms-style §4），按 `len(allArgs)` 的 0..`_PARTIAL_MAX_ARGS` 共 17 个分支逐一显式调用 `fn()`、`fn(allArgs[0])`、`fn(allArgs[0], allArgs[1])`……；超过上限抛 `TypeError(f"functools.partial: too many arguments ({n}, max {_PARTIAL_MAX_ARGS})")`。定长分派是纯 .ms 在没有调用点解包时转发动态实参的唯一手段；上限 16 参照 Python 生态同类实现的常见取值，足以覆盖实际用途。`_applyList` 因分支数超出 12-ms-style §5.2 的 60 行函数软阈值而独立成函数，`bound` 自身保持短小；该拆分在评审中以此为理由说明。
- 预绑定零参时 `partial(fn)` 退化为定长分派转发器，行为与 `fn` 一致（除上限与 kwargs 守卫外）。
- 嵌套预绑定：`partial(partial(fn, 1), 2)` 自然成立——内层 `bound` 是普通函数值，外层闭包捕获之。

### lruCache：dict 插入序驱动的 LRU

- `lruCache(maxSize = 128)` 是装饰器工厂：校验后返回嵌套函数 `decorator(wrapped)`，`decorator` 再返回嵌套函数 `cached`——三层闭包（工厂 → 装饰器 → 包装函数），正是任务 14 闭包语义的直接运用。
- 校验：`maxSize` 非 int（`type(maxSize) != int`）抛 `TypeError`；`maxSize <= 0` 抛 `ValueError`。不提供 Python 的 `None`（无界）形态——规范清单只给出默认值语义，刻意收窄。
- 缓存容器：`cache := {}`，作为 `decorator` 的局部变量被 `cached` 闭包捕获，每个被装饰函数一份，无模块级状态。**dict 保持插入序（02-types §5.3），即以插入序充当 LRU 顺序**：
  - 键：调用侧 `*rest` 收集到的 tuple 直接作键（tuple 递归可哈希，02-types §6）；元素不可哈希（如 list）时 dict 操作自然抛 `TypeError`，不加前置校验。调用侧 `**kw` 非空时抛 `NotImplementedError`（同 partial 的转发限制）。
  - 命中判定用 `key in cache` 成员测试而非「取值判 nil」，使缓存 `nil` 结果与未缓存可区分。
  - 命中：先 `v := cache[key]`，再 `del cache[key]`、`cache[key] = v`（重插到末尾 = 最近使用），返回 `v`。
  - 未命中：调用 `wrapped`（经 `_applyList` 转发，复用 partial 的同一助手与 16 参上限），结果写入 `cache[key]`；写入后 `len(cache) > maxSize` 时逐出 `cache.keys()[0]`（最久未用，即插入序首位）。
- 并发语义：包装函数体内调用 `wrapped` 期间可能发生协程让出（如 `wrapped` 内部 `time.sleep`），两个协程可因此对同一键重复计算；dict 操作本身是单步的，缓存不损坏，语义为「可能重复计算、最后一次写入生效」，与 CPython `lru_cache` 的文档承诺一致。本模块不加锁。
- 规范示例形态必须可用：`fib = functools.lruCache()(fib)`（07-stdlib §23，`@` 装饰器语法属路线图）。

### compose：函数复合

- 返回单参数闭包 `func composed(x) { return outer(inner(x)) }`：07-stdlib §23 以 `compose(f, g)(x) == f(g(x))` 定义语义，只承诺单参数形态，刻意不扩展为多参数转发（多参数会立刻撞上调用点解包限制）。
- 不前置校验可调用性（同 `reduce` 的理由）；`compose` 的实参为 lambda、闭包、内建函数或其他 `compose` 产物均可，链式 `compose(h, compose(g, f))` 自然成立。

## 实现步骤

1. 建 `lib/functools.ms` 骨架：文件头注释、`_PARTIAL_MAX_ARGS` 常量、四个公开函数的签名与英文文档注释，函数体先 `pass`。验证：`import "functools"` 成功，四个名字均可访问（逐个属性读取）。
2. 实现 `reduce`：`*init` 个数校验、带/不带 init 的折叠循环、空序列 `TypeError`。验证：ms 脚本断言带 init、不带 init、空序列两类路径的结果与异常类型。
3. 实现 `compose`：单参数嵌套闭包。验证：`compose(f, g)(x) == f(g(x))` 的求值顺序、lambda/闭包实参、链式复合。
4. 实现 `_applyList` 与 `partial`：定长分派 17 分支、位置参数合并、kwargs 两处守卫、超上限 `TypeError`。验证：预绑定 0/1/多参、调用侧追加、嵌套预绑定、边界（16 参与 17 参）、两条 `NotImplementedError` 路径。
5. 实现 `lruCache`：maxSize 校验、三层闭包、`in` 判定命中、重插触达、逐出 `keys()[0]`。验证：命中不重算（闭包计数器）、maxSize=2 的逐出顺序、缓存 nil、不可哈希键 `TypeError`、kwarg 守卫。
6. 编写完整测试文件（见「测试方案」），经仓库根 `run_tests.py` 调用 mslang CLI 全量运行通过。

## 测试方案

按 README 测试约定：本任务晚于任务 40（testing 模块），测试一律使用 `testing` 与 `testing/assert` 模块。测试文件 `tests/ms/stdlib/functools_test.ms`，测试函数以 `test` 开头，末尾 `testing.run()`；由 `run_tests.py` 递归发现并驱动。异常断言用 `assert.raises`（07-stdlib §19 既定接口）。

覆盖清单：

- `reduce`：
  - 带 init：`reduce(add, [1, 2, 3], 10) == 16`；空序列带 init 返回 init 本身。
  - 不带 init：`reduce(add, [1, 2, 3]) == 6`；单元素序列返回该元素（`fn` 不被调用）。
  - 非数值折叠：`reduce(concat, ["a", "b"], "")` 一类的字符串拼接。
  - 负例：空序列且无 init 抛 `TypeError`；`reduce(fn, xs, 1, 2)` 抛 `TypeError`。
- `compose`：
  - 顺序：`compose(str, abs)(-3) == "3"`，验证是 `outer(inner(x))` 而非反向。
  - 实参形态：lambda、嵌套闭包（捕获外层变量）、内建函数。
  - 链式：`compose(h, compose(g, f))(x) == h(g(f(x)))`。
  - 负例：对不可调用实参求值时抛 `TypeError`。
- `partial`：
  - 预绑定：`partial(sub, 10)(1)`、多参前缀 `partial(add3, 1, 2)(3)`、零参预绑定等价直接调用。
  - 调用侧追加：捕获参数在前、调用参数在后的拼接顺序。
  - 嵌套：`partial(partial(sub, 100), 20)(3) == 77`。
  - 边界：合计 16 个位置参数正常转发，17 个抛 `TypeError`。
  - kwargs 守卫：`partial(fn, k = 1)` 立即抛 `NotImplementedError`；`bound(k = 1)` 抛 `NotImplementedError`。
- `lruCache`：
  - 命中不重算：被装饰函数内用闭包计数器记录调用次数，同参二次调用计数不增。
  - 逐出顺序：`maxSize = 2` 时依次访问 `a`、`b`，再访问 `a`（触达），插入 `c` 后 `b` 被逐出、`a` 保留——以计数器增量判定。
  - 缓存 nil：被装饰函数返回 nil 时二次调用不重算。
  - 默认值：`lruCache()` 可用且容量为 128（小样本行为断言，不构造 129 项慢测试，改以小容量等价路径覆盖逐出逻辑）。
  - 集成形态：`fib = functools.lruCache()(fib)` 后 `fib(20)` 结果正确且调用次数远小于朴素递归（计数器上界断言）。
  - 负例：`maxSize` 为字符串抛 `TypeError`；`maxSize = 0` / `-1` 抛 `ValueError`；list 实参抛 `TypeError`；带关键字参数调用抛 `NotImplementedError`。

## 验收标准

- [ ] `lib/functools.ms` 存在，实现且仅实现 07-stdlib §23 的四个函数 `reduce` / `partial` / `lruCache` / `compose`；无 C 代码，无模块级可变状态（`_PARTIAL_MAX_ARGS` 为只读常量）。
- [ ] ms 代码符合 12-ms-style：4 空格缩进、行宽 120、无单字母参数名（`fn`/`outer`/`inner`）、不遮蔽内建名、公开函数带英文前置文档注释、文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- [ ] `reduce` 用 `*init` 区分 init 缺省与 `nil`，空序列无 init 抛 `TypeError`，多余 init 抛 `TypeError`。
- [ ] `partial` 正确拼接预绑定与调用侧位置参数（经 `_applyList` 定长分派，上限 16），支持嵌套预绑定；捕获或调用侧出现关键字参数时抛 `NotImplementedError`，且该限制在模块文档注释中显式注明。
- [ ] `lruCache` 以 dict 插入序实现 LRU（命中重插、逐出 `keys()[0]`），`key in cache` 判定使命中 nil 可缓存；maxSize 校验（非 int `TypeError`、非正 `ValueError`）；`fib = functools.lruCache()(fib)` 形态可用。
- [ ] `compose(outer, inner)(x) == outer(inner(x))`，支持 lambda、闭包与链式复合。
- [ ] `import "functools"` 在解释器中可用，模块经 `lib/` 解析档命中（机制属任务 24，本任务不改动加载器）。
- [ ] `tests/ms/stdlib/functools_test.ms` 覆盖「测试方案」全部清单项，`run_tests.py` 全量通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 14 的接口假定（按引用捕获的闭包语义）在实现时已对齐。
