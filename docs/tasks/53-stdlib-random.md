# 53 标准库：random

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [21 标准库：math](21-stdlib-math.md)、[39 标准库：time](39-stdlib-time.md) |

## 任务目标

交付纯 .ms 实现的标准库模块 `lib/random.ms`：伪随机数生成器与常用分布/采样函数——`seed`（播种）、`random`（[0, 1) 均匀浮点）、`randInt`（闭区间整数）、`choice` / `shuffle` / `sample`（序列采样）、`gauss`（正态分布）。生成算法采用规范指定的 xorshift128+，状态为模块级全局单例（对齐 Python `random` 的使用形态）；默认种子取自 `time` 模块的时钟，显式播种后整个序列完全可复现。模块不含任何 C 代码，依赖任务 21（math，对数/三角/常量）与任务 39（time，默认种子来源），并依赖任务 31 已落地的任意精度整数做 64 位掩码运算。完成后脚本 `import "random"` 即可使用全部六个函数；本任务通过 `tests/ms/` 下的 ms 脚本测试（testing 模块，任务 40 已先于本任务完成）独立验收。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`random` 为纯脚本模块（`lib/` 目录），注明「伪随机数（xorshift128+）」——**生成算法由规范固定为 xorshift128+**，不是自由选型。
  - §21 random：本模块的完整公开 API 清单，共六个函数：

    ```ms
    random.seed(n)
    random.random()                      // [0, 1)
    random.randInt(a, b)                 // closed interval [a, b]
    random.choice(xs)  random.shuffle(xs)  random.sample(xs, k)
    random.gauss(mu, sigma)
    ```

- `docs/language/02-types.md`
  - §3.1：`int` 为任意精度有符号整数（大整数任务 31 已落地）——xorshift128+ 的 64 位状态字必须以显式掩码维持，不能依赖溢出回绕；字面量 `0xFFFFFFFFFFFFFFFF`（> int64 上限）本身即大整数。
  - §3.2：`float` 为 IEEE 754 双精度，`random()` 的 53 位精度构造基于此。
  - §3.3：不存在 float → int 隐式转换，`randInt` 拒绝 float 实参；显式转换 `int(3.9)`（截断）用于默认种子的浮点时钟取整。
- `docs/language/04-exceptions.md` §4：异常层级含 `TypeError` / `ValueError` / `IndexError`，参数校验失败的抛出类型据此选择。
- `docs/language/05-modules.md` §2：`lib/` 纯脚本模块在解析顺序第一档「内建/已注册模块」中命中；§4：模块首次导入时顶层代码执行一次并缓存（模块级状态单例的语言基础）。
- `docs/language/11-project-layout.md` §1：纯脚本标准库模块置于 `lib/`；§4 测试策略：脚本测试位于 `tests/ms/`。
- `docs/language/12-ms-style.md`：§2 源文件结构（import → 常量 → 变量 → 函数 → 顶层代码）、§4 命名（内部函数 `_` 前缀、模块级可变状态大写蛇形）、§5.3（浮点不做 `==` 比较）、§5.6（模块级可变状态须注释理由与并发安全性）、§6（公开函数的英文前置文档注释）。
- 依赖任务 21（math）的文档已存在，本文引用其既定接口：`math.sqrt`、`math.log`（一元形式）、`math.sin`、`math.cos`、`math.pi`，均接受 int/float 并返回 float。
- 依赖任务 39（time）与任务 40（testing）的文档尚不存在：`time.now()`（返回 float Unix 秒）、`time.monotonic()`（float 单调时钟）的接口名取自 `docs/language/07-stdlib.md` §9；`testing.run()` 与 `testing/assert` 的 `equal`/`isTrue`/`raises` 取自 §19。实现时以对应任务文档定名为准。

## 详细设计

本任务是纯 .ms 模块，无 C 结构体与 C 函数签名；接口级设计以 ms 函数签名与语义约定表达。ms 代码遵循 `docs/language/12-ms-style.md`（4 空格缩进、行宽 120、小驼峰函数名、内部名字 `_` 前缀、公开函数带英文前置文档注释）。

### 文件与模块骨架

- 唯一交付文件：`lib/random.ms`。文件头注释一句话说明职责；import 仅 `math` 与 `time` 两个标准库模块；不写 `__name__` 守卫（纯库文件，12-ms-style §2.2）。
- 模块经任务 24 的模块系统以 `"random"` 名注册/解析，脚本侧 `import "random"` 后以 `random.random()` 形式调用（05-modules §1 绑定规则）。
- 模块级状态（12-ms-style §5.6 要求的大写蛇形命名与理由注释）：

```ms
// Generator state for the module-level xorshift128+ singleton.
// Rationale: mirrors Python's module-level Random instance so that
// random.random() works without explicit generator plumbing.
// Concurrency: NOT thread-safe under the M:N scheduler (no locking);
// coroutines on a single thread are safe (functions never yield).
_RNG_S0 := 0
_RNG_S1 := 0

// Cached second Box-Muller value for gauss(), nil when empty.
_GAUSS_NEXT = nil
```

- 模块级常量：`_MASK64 := 0xFFFFFFFFFFFFFFFF`（2⁶⁴ − 1，掩码）、`_TWO_POW53 := 9007199254740992.0`（2⁵³，`random()` 的归一因子）、splitmix64 用的三个 64 位混合常数。常量为大整数字面量，依赖任务 31 的任意精度 int。

### 公开函数

```ms
// seed reseeds the module-level generator.
// n must be an int; negative seeds are normalized by magnitude, and seeds
// wider than 64 bits are folded down. seed() or seed(nil) derives the seed
// from the time module's clocks (non-reproducible). Seeding also clears the
// cached gauss() value. Raises TypeError for non-int, non-nil n.
func seed(n = nil)

// random returns the next float uniformly distributed in [0.0, 1.0),
// with 53 bits of precision.
func random()

// randInt returns an int uniformly distributed over the closed interval
// [a, b]. a and b must be ints (no implicit float truncation). Arbitrary
// precision is supported (b - a may exceed 64 bits).
// Raises TypeError on non-int arguments, ValueError if a > b.
func randInt(a, b)

// choice returns a random element of the non-empty sequence xs.
// Raises IndexError if xs is empty, TypeError if xs has no length.
func choice(xs)

// shuffle shuffles the list xs in place (Fisher-Yates) and returns nil.
// Raises TypeError if xs is not a list.
func shuffle(xs)

// sample returns a new list of k distinct elements drawn from xs without
// replacement; the input list is not modified.
// Raises ValueError if k < 0 or k > len(xs), TypeError on non-int k.
func sample(xs, k)

// gauss returns a float from the normal distribution N(mu, sigma^2),
// computed via Box-Muller with second-value caching. mu and sigma must be
// numbers (int or float); sigma may be negative (it merely flips the sign
// of the offset). Raises TypeError on non-numeric arguments.
func gauss(mu, sigma)
```

语义与边界约定：

- **六个函数与 07-stdlib §21 清单一一对应，不增删**；不提供 Python 的 `randrange`/`uniform`/`getstate`/`setstate`（未列入清单）。
- `seed(n)` 只接受 int 与 nil：float/str 种子（Python 支持）刻意不实现——规范未要求，且 str 哈希引入无收益的实现复杂度；其他类型一律 `TypeError`（消息含实参的 `str()` 表示）。`seed()` / `seed(nil)` 用 `int(time.now() * 1.0e9)` 与 `int(time.monotonic() * 1.0e9)` 异或混合后经 splitmix64 展开，不可复现。
- `random()` 的返回类型恒为 float，值域 `[0.0, 1.0)`：53 位有效位保证不含 1.0。
- `randInt(a, b)` 为**闭区间**（规范明示 closed interval），`randInt(5, 5)` 恒返回 `5`。`a > b` 抛 `ValueError`。bool 不是 int（02-types §3 中 bool 是独立类型），`randInt(true, 3)` 抛 `TypeError`，与任务 21「bool 不是数值参数」的约定一致。
- `shuffle` 原地修改并返回 `nil`（对齐 Python，防止误以为是纯函数）；`sample` 返回新 list、不改输入（对齐 Python）。
- `choice` 对空序列抛 `IndexError`（对齐 Python 的 "Cannot choose from an empty sequence"），不用 `ValueError`：空序列是「取下标越界」的退化情形，04-exceptions §4 层级中 `IndexError` 更贴切。
- `gauss` 的 mu/sigma 接受 int 或 float（int 经算术自然提升为 float），其余类型由显式检查抛 `TypeError`；返回恒为 float。

### 生成器核心：xorshift128+

内部函数 `_nextUint64()` 返回 `[0, 2⁶⁴)` 内的下一个状态字。每步按 Vigna 的 xorshift128+ 递推，**每处可能超出 64 位的中间结果都立即与 `_MASK64` 掩码**——MS 的 int 是任意精度，不会自动回绕，且所有状态保持非负，位运算（`^`、`>>`）在非负大整数上的行为与定宽无符号语义一致：

```ms
func _nextUint64() {
    _ensureSeeded()
    x := _RNG_S0
    y := _RNG_S1
    _RNG_S0 = y
    x = x ^ ((x << 23) & _MASK64)
    _RNG_S1 = (x ^ y ^ (x >> 18) ^ (y >> 5)) & _MASK64
    return (_RNG_S1 + y) & _MASK64
}
```

要点：

- `(x << 23)` 与末行加法会产生超过 64 位的值，必须掩码；`>>` 的输入已掩码为非负，无需处理符号扩展。
- **惰性播种**：`_ensureSeeded()` 在 `_RNG_S0 == 0 and _RNG_S1 == 0` 时调用 `_seedFromTime()`。xorshift 的全零状态本身非法（退化为恒零序列），`seed()` 保证永不落入全零（见下），故「全零即未播种」是无歧义的判定。惰性而非导入时播种的好处：`import "random"` 无副作用、不读时钟，且未显式播种的首次抽取才真正消耗时钟。
- 模块全局状态、不加锁：单线程协程下函数体内无让出点，天然安全；v0.3 M:N 调度器下跨工作线程并发调用可能交错更新状态字（结果序列未定义但无内存安全问题）。此限制写入 `_RNG_S0` 的声明注释（12-ms-style §5.6），本任务不引入 `sync`（任务 48 虽可用，但为随机数加锁的收益不成比例，且 Python 的模块级 random 同样不承诺每步原子性）。

### 播种：splitmix64 展开

`seed(n)`（int 分支）把任意整数折叠为两个非零状态字：

1. 负种子取幅度：`if n < 0 { n = -n }`（避免依赖负大整数位运算的二补码语义约定）。
2. 折叠超宽种子：循环 `while n > _MASK64 { n = (n & _MASK64) ^ (n >> 64) }`，把任意精度种子压进 64 位。
3. splitmix64 展开两遍，分别得 `_RNG_S0`、`_RNG_S1`：

```ms
// splitmix64 finalizer; each call advances the local accumulator.
z = (z + 0x9E3779B97F4A7C15) & _MASK64
z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & _MASK64
z = z ^ (z >> 31)
```

4. 全零防护：若展开后 `_RNG_S0 == 0 and _RNG_S1 == 0`，置 `_RNG_S1 = 0x9E3779B97F4A7C15`。
5. 清空 gauss 缓存：`_GAUSS_NEXT = nil`（对齐 Python：`seed()` 使 `gauss` 序列也整体可复现）。

`_seedFromTime()` 复用同一展开路径：`seed(int(time.now() * 1.0e9) ^ int(time.monotonic() * 1.0e9))`——双时钟异或降低同毫秒并发进程的种子碰撞率。

### random()：53 位归一

```ms
func random() {
    return float(_nextUint64() >> 11) * (1.0 / _TWO_POW53)
}
```

取高 53 位（`>> 11`）除以 2⁵³：低位的线性复杂度更高，取高位是 xorshift 家族的标准用法；结果严格小于 1.0。与乘法相比的位移替代方案（直接拼 IEEE 尾数）在纯 .ms 中无法表达位级浮点构造，故用乘法定点化。

### randInt()：拒绝采样消偏

朴素取模（`a + next() % w`）在 `w` 不整除 2⁶⁴ 时有模偏置，且 `w` 可超过 64 位。采用任意精度拒绝采样：

1. `w := b - a + 1`；`w == 1` 时直接返回 `a`。
2. `bits := _bitLength(w - 1)`（内部函数：循环右移计数，`w - 1` 非负）。
3. 循环：从 `_nextUint64()` 逐字拼出 `bits` 位候选 `r`（高位补零截断到 `bits` 位）；若 `r < w` 则返回 `a + r`，否则重试。接受概率 ≥ 1/2，期望迭代次数 < 2，分布严格均匀。

### shuffle 与 sample：Fisher-Yates

- `shuffle(xs)`：标准原地 Fisher-Yates，`i` 从 `len(xs) - 1` 递减到 1，每次 `j := randInt(0, i)` 后交换 `xs[i]` 与 `xs[j]`。类型检查 `isinstance(xs, list)`，失败抛 `TypeError`。
- `sample(xs, k)`：先校验 `0 <= k <= len(xs)`（越界抛 `ValueError`，对齐 Python）；复制 `pool := xs[:]`（浅拷贝，切片语义见任务 30），做前 `k` 步 Fisher-Yates（`i` 从 0 到 `k - 1`，`j := randInt(i, len(pool) - 1)`），返回 `pool[:k]`。输入不被修改。
- `choice(xs)`：`len(xs) == 0` 抛 `IndexError`；否则返回 `xs[randInt(0, len(xs) - 1)]`。鸭子类型——任何支持 `len` 与整数下标的序列（含字符串）都可用，不强制 `isinstance(xs, list)`。

### gauss()：Box-Muller 与缓存

```ms
func gauss(mu, sigma) {
    // type checks: mu, sigma must be int or float (bool excluded)
    if _GAUSS_NEXT is not nil {
        z := _GAUSS_NEXT
        _GAUSS_NEXT = nil
        return mu + z * sigma
    }
    u1 := 1.0 - random()        // (0.0, 1.0], avoids log(0)
    u2 := random()
    radius := math.sqrt(-2.0 * math.log(u1))
    theta := 2.0 * math.pi * u2
    _GAUSS_NEXT = radius * math.sin(theta)
    return mu + radius * math.cos(theta) * sigma
}
```

要点：

- `u1 = 1.0 - random()` 落在 `(0, 1]`：`random()` 不含 1.0 故 `u1 > 0`，`math.log` 不会触发定义域 `ValueError`（任务 21 的约定）。
- 第二个值缓存于 `_GAUSS_NEXT`，下次调用直接消费——两次调用产出一对相关正态值，缓存保证不浪费半次 Box-Muller；`seed()` 清缓存（见播种节）。
- 不做 `sigma >= 0` 校验：负 sigma 仅翻转偏移符号，数学上仍是正态分布（与 Python `random.gauss` 行为一致）。

## 实现步骤

1. 建 `lib/random.ms` 骨架：文件头注释、import（`math`/`time`）、模块级常量（`_MASK64`、`_TWO_POW53`、splitmix64 常数）与状态变量（含 12-ms-style §5.6 要求的理由/并发注释）、六个公开函数与内部函数的签名及英文文档注释，函数体先 `pass`。验证：`import "random"` 成功，六个名字均可访问。
2. 实现 `_ensureSeeded` / `_seedFromTime` / `seed`（含类型校验、幅度归一、折叠、splitmix64 展开、全零防护、清 gauss 缓存）与 `_nextUint64`。验证：ms 脚本断言 `seed(42)` 后两次独立运行产生相同序列；`seed("x")`/`seed(1.5)` 抛 `TypeError`；不播种时 `random()` 可用且落在值域内。
3. 实现 `random()`（53 位归一）。验证：固定种子下值序列逐点可复现；大量采样全部满足 `0.0 <= r < 1.0`。
4. 实现 `_bitLength` 与 `randInt`（拒绝采样）。验证：边界 `randInt(5, 5)`、负区间、跨 64 位的大区间、`a > b` 抛 `ValueError`、float/bool 实参抛 `TypeError`、固定种子可复现。
5. 实现 `choice` / `shuffle` / `sample`。验证：空序列 `IndexError`、shuffle 返回 nil 且为原列表的排列、sample 的 k 元素互异且来自输入、输入未被修改、`k` 越界抛 `ValueError`。
6. 实现 `gauss`（Box-Muller + 缓存）。验证：固定种子可复现（含缓存路径）、int/float 参数、非数值参数 `TypeError`、大样本均值/方差落在宽容差内。
7. 编写完整测试文件（见「测试方案」），经仓库根 `run_tests.py` 调用 mslang CLI 全量运行通过。

## 测试方案

按 README 测试约定：本任务晚于任务 40（testing 模块），测试一律使用 `testing` 与 `testing/assert` 模块（`assert.equal` / `assert.isTrue` / `assert.raises`）。测试文件 `tests/ms/stdlib/random_test.ms`，测试函数以 `test` 开头，末尾 `testing.run()`；由 `run_tests.py` 递归发现并驱动。

**可复现性原则**：生成器算法与播种展开均为本文固定规范，固定种子的序列完全确定。涉及序列的用例统一模式：「`random.seed(固定值)` → 采样记录 → 再次 `random.seed(同一值)` → 采样比对逐点相等」，不需外部参考向量即可断言确定性；浮点统计量用宽容差比较（12-ms-style §5.3 禁止浮点 `==`），统计断言只在固定种子下进行，无偶发失败。

覆盖清单：

- `seed`：
  - 固定种子（如 42）后 `random`/`randInt`/`gauss` 各自序列重播种后逐点复现。
  - 不同种子（1 vs 2）产生的 10 元素序列不相等。
  - 负种子（`seed(-42)`）合法且可复现；超大整数种子（> 2⁶⁴）合法且可复现。
  - `seed(nil)` 与 `seed()` 不抛错、后续函数正常。
  - `seed("x")`、`seed(1.5)`、`seed(true)` 抛 `TypeError`。
  - `seed` 清除 gauss 缓存：播种后首个 `gauss` 值与上次播种后首个值相等（缓存不残留）。
- `random`：
  - 1000 次采样全部满足 `0.0 <= r < 1.0`；返回值是 float。
  - 固定种子下逐点复现（float 序列经重播种后逐项 `==` 成立——同一确定性算法产生的 float 位级相同，此处 `==` 合法，注释说明）。
- `randInt`：
  - 闭区间边界：`randInt(1, 3)` 的 200 次采样覆盖到 1 与 3（固定种子下断言集合含两端点）；`randInt(5, 5) == 5`。
  - 负区间 `randInt(-10, -1)`、混合区间 `randInt(-5, 5)` 的采样全部在界内。
  - 大区间：`randInt(0, 2**64 + 12345)` 采样在界内且可复现（验证任意精度路径）。
  - 负例：`randInt(3, 1)` 抛 `ValueError`；`randInt(1.5, 3)`、`randInt(1, "3")`、`randInt(true, 3)` 抛 `TypeError`。
- `choice`：
  - 100 次选择结果均为原列表元素；固定种子可复现。
  - 空列表抛 `IndexError`；字符串序列可用（`choice("abc")` 是单字符字符串）。
- `shuffle`：
  - 返回 `nil`；原列表被原地重排；重排后是原列表的排列（用 dict 计数比对多重集，不引入 `sort` 依赖）。
  - 空列表与单元素列表不抛错；固定种子下两次 shuffle 结果相同。
  - 负例：`shuffle("abc")`（不可变序列）抛 `TypeError`。
- `sample`：
  - `k` 个元素互异、全部来自输入、输入列表内容不变；`k == 0` 返回 `[]`；`k == len(xs)` 返回全体的一个排列。
  - 固定种子可复现。
  - 负例：`k < 0`、`k > len(xs)` 抛 `ValueError`；`k` 为 float 抛 `TypeError`。
- `gauss`：
  - 固定种子下逐点复现，含缓存消费路径（连续两次调用取到一对 Box-Muller 值，重播种后该对值逐点相等）。
  - int 参数（`gauss(0, 1)`）返回 float。
  - 大样本统计：固定种子取 10000 个 `gauss(10.0, 2.0)` 样本，均值落在 `[9.9, 10.1]`、样本标准差落在 `[1.9, 2.1]`（容差为固定种子下的宽松界，实现时若实测值贴近边界可收紧并重录，但断言阈值须在文档语义范围内）。
  - 负例：`gauss("0", 1)` 抛 `TypeError`。
- 默认播种：不显式 `seed`，直接调用各函数均在值域内正常返回（验证 `_ensureSeeded` 惰性路径）。

## 验收标准

- [ ] `lib/random.ms` 存在，实现且仅实现 07-stdlib §21 的六个函数 `seed` / `random` / `randInt` / `choice` / `shuffle` / `sample` / `gauss`（六个公开名，清单不增删）；无 C 代码，import 仅 `math` 与 `time`。
- [ ] ms 代码符合 12-ms-style：4 空格缩进、行宽 120、内部函数 `_` 前缀、模块级可变状态大写蛇形并附理由与并发安全性注释、公开函数带英文前置文档注释、不遮蔽内建名、文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- [ ] 生成器为规范指定的 xorshift128+：状态两字维持 `[0, 2⁶⁴)` 非负 int，所有移位/加法后经 `_MASK64` 掩码；全零状态不可达（播种防护 + 惰性播种判定）。
- [ ] `random()` 返回 `[0.0, 1.0)` 的 float（53 位精度）；`randInt(a, b)` 闭区间、拒绝采样无模偏置、支持任意精度区间、`a > b` 抛 `ValueError`；`choice` 空序列抛 `IndexError`；`shuffle` 原地且返回 `nil`；`sample` 不改输入、`k` 越界抛 `ValueError`；`gauss` 用 Box-Muller 且缓存第二值、`seed` 清缓存。
- [ ] `seed(n)` 接受 int（负值取幅度、超宽折叠、splitmix64 展开）与 nil/缺省（time 双时钟混合），其他类型抛 `TypeError`；固定种子下全部六个函数的输出序列完全可复现。
- [ ] `import "random"` 在解释器中可用且导入时不读时钟（惰性播种），模块经 `lib/` 解析档命中（机制属任务 24，本任务不改动加载器）。
- [ ] `tests/ms/stdlib/random_test.ms` 覆盖「测试方案」全部清单项，`run_tests.py` 全量通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 39（`time.now`/`time.monotonic`）、任务 40（`testing`/`assert` 接口）的接口假定在实现时已对齐。
