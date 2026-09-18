# 52 标准库：datetime

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [39 标准库：time](39-stdlib-time.md) |

## 任务目标

交付纯 .ms 实现的标准库模块 `lib/datetime.ms`：对象化的日期时间 API——`DateTime`（日期+时刻）、`Time`（时刻）、`TimeDelta`（时长）三个公开类，以及 `now` / `fromTimestamp` / `date` / `time` / `timedelta` / `strptime` 六个模块级工厂/解析函数与 `MIN_YEAR` / `MAX_YEAR` 两个常量。`DateTime` 支持 `dt.year` 等字段访问、`dt + datetime.timedelta(days=1)` 风格的算术、比较与哈希、`strftime` 格式化；`strptime` 为其逆解析。模块不含任何 C 代码：历法换算（格里高利历，含闰年）在模块内以整数算法实现（Howard Hinnant 的 days-from-civil 算法），历元换算建立在内建任意精度 int 之上，唯一的运行时外部输入是任务 39 的 `time.now()`（`now()` 的时钟来源）。完成后脚本 `import "datetime"` 即可使用 07-stdlib §9 示例的全部形态；本任务通过 `tests/ms/` 下的 ms 脚本测试（testing 模块，任务 40 已先于本任务完成）独立验收。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`datetime` 为纯脚本模块（`lib/` 目录），注明「日期时间对象（基于 time）」——实现方式与依赖方向由规范固定。
  - §9 time 与 datetime：本模块的公开 API 来源。规范原文为示例式清单：

    ```ms
    dt := datetime.now()                 // a DateTime object
    dt.year dt.month dt.day dt.hour dt.minute dt.second
    dt + datetime.timedelta(days=1)      // arithmetic
    dt.strftime("%Y-%m-%d")
    datetime.strptime(text, fmt)
    datetime.date(2025, 1, 1)  datetime.time(12, 30)
    ```

  - §9 time 原语：`time.now()` 返回 float Unix 秒（含小数）——`now()` 的唯一时钟来源；`time.sleep`/`time.monotonic`/`time.format`/`time.parse` 本模块不使用（理由见「设计取舍说明」）。
- `docs/language/02-types.md`
  - §3.1：int 任意精度——历元微秒计数、Hinnant 算法中间值与时间跨度运算无需考虑溢出回绕，是本模块「全程整数、无浮点误差」设计的基础。
  - §3.3：`int / int` → float、`//` 为向下取整除法；不存在 float → int 隐式转换（`fromTimestamp` 对 float 时间戳显式舍入）；本文假定 `%` 与 `//` 一致为向下取整（floor）语义（Python 同），负天数换算依赖此性质。
  - §6：相等与哈希契约——定义 `__eq__` 必须同时定义 `__hash__`，三个公开类均遵守。
  - §8：魔术方法协议表（`__init__`/`__str__`/`__repr__`/比较六个/`__hash__`/`__add__`/`__sub__`/`__mul__`/`__truediv__`/`__neg__`）；反射方法（`__radd__` 等）首版不支持——`1 + td` 之类反向运算不承诺。
- `docs/language/03-syntax.md` §4：默认参数与关键字实参（`timedelta(days=1)` 的语法基础）；§5：class 语法（`__init__` 中经 `self.x = ...` 建属性）；§6：条件表达式 `a if cond else b`、链式比较。
- `docs/language/04-exceptions.md` §4：异常层级含 `TypeError` / `ValueError` / `OverflowError`（ArithmeticError 子类）——字段校验、类型校验与范围溢出的抛出类型据此选择。
- `docs/language/05-modules.md` §2：`lib/` 纯脚本模块的解析档位；模块的发现、执行与缓存由任务 24（模块系统）负责，本任务只交付模块源码。
- `docs/language/11-project-layout.md` §1：纯脚本标准库模块置于 `lib/`；§4 测试策略：脚本测试位于 `tests/ms/`。
- `docs/language/12-ms-style.md`：§2 源文件结构（import → 常量 → 函数 → class）、§3 格式化（4 空格缩进、行宽 120、禁止单行块）、§4 命名（class 大驼峰、函数小驼峰、模块级常量大写蛇形、内部名字 `_` 前缀）、§5.4（不用异常做常规控制流）、§6（公开类/函数/常量的英文前置文档注释）。
- 任务 20（strings，已定稿）提供 `strings.padLeft(s, n, pad)`（宽度按码点、`pad` 须单码点）——`strftime` 的数字零填充经它实现，不引入 fmt 依赖。
- 依赖任务 39（time）与任务 40（testing）的文档尚不存在：`time.now()` 的接口名取自 07-stdlib §9；`testing.run()` 与 `testing/assert` 的 `equal`/`isTrue`/`raises` 取自 §19。任务 25（class 继承与魔术方法）的文档亦不存在：本文引用的魔术方法协议以 02-types §8 为准（含「定义 `__eq__` 须配 `__hash__`」「无反射方法」）。实现时以对应任务文档定名为准。

对规范歧义与空白的显式处理（实现与评审时以此为据，细节见「设计取舍说明」）：

1. **时区**：规范全文未提及时区/本地化。裁定：本模块为**无时区（naive）UTC 历法**——`now()`/`fromTimestamp()` 把 Unix 秒按 UTC 分解为历法字段，模块不提供任何时区转换。此裁定使全部测试确定性可复现；本地时区格式化由任务 39 的 `time.format` 承担。
2. **字段集**：规范示例只列 `year..second` 六个字段；`time.now()` 带小数秒，丢弃小数会静默损失精度。裁定：`DateTime`/`Time`/`TimeDelta` 增加 `microsecond` 字段（对齐 Python），作为对示例清单的最小扩充。
3. **`datetime.date()` / `datetime.time()` 的返回类型**：规范未指明。裁定：`date(y, m, d)` 返回当日 00:00:00 的 `DateTime`（不引入独立 `Date` 类）；`time(h, m, ...)` 返回 `Time` 对象（不承载日期，不提供算术）。
4. **年份范围**：对齐 Python 取 `[1, 9999]`（前推格里高利历、无第 0 年、无闰秒），构造越界抛 `ValueError`，算术结果越界抛 `OverflowError`；`MIN_YEAR`/`MAX_YEAR` 导出为模块常量。

## 详细设计

本任务是纯 .ms 模块，无 C 结构体与 C 函数签名；接口级设计以 ms 类/函数签名与语义约定表达。ms 代码遵循 `docs/language/12-ms-style.md`（4 空格缩进、行宽 120、内部名字 `_` 前缀、公开 API 带英文前置文档注释）。

### 文件与模块骨架

- 唯一交付文件：`lib/datetime.ms`。文件头注释一句话说明职责；import 仅两个标准库模块（按 12-ms-style §2.1 字母序）：

  ```ms
  import "strings"
  import "time"
  ```

  不写 `__name__` 守卫（纯库文件，12-ms-style §2.2）。模块经任务 24 的模块系统以 `"datetime"` 名解析，脚本侧 `import "datetime"` 后以 `datetime.now()` 形式调用（05-modules §1 绑定规则）。
- 模块级常量（只读，大写蛇形）：

  ```ms
  // Minimum and maximum supported years (proleptic Gregorian calendar).
  MIN_YEAR := 1
  MAX_YEAR := 9999
  ```

  内部常量（`_` 前缀）：`_SECONDS_PER_DAY := 86400`、`_MICROS_PER_SECOND := 1_000_000`、`_MICROS_PER_DAY := 86_400_000_000`，以及英文名称表 `_WEEKDAY_NAMES_FULL` / `_WEEKDAY_NAMES_ABBR` / `_MONTH_NAMES_FULL` / `_MONTH_NAMES_ABBR`（list 字面量，按 12-ms-style §5.6 视为只读配置，声明处注释「内容恒定、不得修改」；strftime/strptime 固定使用英文名称，不做本地化）。
- 无模块级可变状态：`DateTime`/`Time`/`TimeDelta` 实例构造后字段不再被模块代码改写（用户直写字段属约定外行为，见 `DateTime` 节）。

### 公开 API 总览

| 名字 | 类别 | 说明 |
|---|---|---|
| `MIN_YEAR` / `MAX_YEAR` | 常量 | 支持的年份闭区间 `[1, 9999]` |
| `now()` | 函数 | 当前时刻的 `DateTime`（UTC，等价 `fromTimestamp(time.now())`） |
| `fromTimestamp(ts)` | 函数 | Unix 秒（int/float）→ `DateTime`（UTC） |
| `date(year, month, day)` | 函数 | 当日 00:00:00 的 `DateTime` |
| `time(hour, minute, second = 0, microsecond = 0)` | 函数 | `Time` 对象 |
| `timedelta(days = 0, seconds = 0, ...)` | 函数 | `TimeDelta` 对象（关键字参数见下） |
| `strptime(text, fmt)` | 函数 | 按格式串解析为 `DateTime` |
| `DateTime` / `Time` / `TimeDelta` | class | 见后三节；经模块属性暴露，供 `isinstance` 与直接构造 |

规范 §9 的示例形态全部覆盖：`datetime.now()`、`dt.year` 等字段、`dt + datetime.timedelta(days=1)`、`dt.strftime("%Y-%m-%d")`、`datetime.strptime(text, fmt)`、`datetime.date(2025, 1, 1)`、`datetime.time(12, 30)`。刻意不提供的 Python API（`replace`/`timedelta.total_days`/`fromisoformat`/`astimezone`/`combine` 等）见「设计取舍说明」。

### 历法换算核心

全程整数运算（任意精度 int），内部统一以「历元微秒」（自 1970-01-01 00:00:00 UTC 的微秒数）为换算中介。日期 ↔ 连续天数采用 Howard Hinnant 的 days-from-civil 算法（公域算法，正确性覆盖全部支持年份）：

```ms
// _isLeapYear reports whether y is a leap year in the proleptic
// Gregorian calendar.
func _isLeapYear(y)

// _daysInMonth returns the number of days in month m (1-12) of year y.
// Raises ValueError if m is out of range.
func _daysInMonth(y, m)

// _daysFromCivil returns the number of days from 1970-01-01 to the
// proleptic Gregorian date y-m-d (negative for earlier dates).
func _daysFromCivil(y, m, d) {
    yy := y - (1 if m <= 2 else 0)
    era := yy // 400
    yoe := yy - era * 400
    mp := m + (-3 if m > 2 else 9)
    doy := (153 * mp + 2) // 5 + d - 1
    doe := yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468
}

// _civilFromDays is the inverse of _daysFromCivil: it returns the
// (y, m, d) tuple for the day count z relative to 1970-01-01.
func _civilFromDays(z) {
    zz := z + 719468
    era := zz // 146097
    doe := zz - era * 146097
    yoe := (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y := yoe + era * 400
    doy := doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp := (5 * doy + 2) // 153
    d := doy - (153 * mp + 2) // 5 + 1
    m := mp + (3 if mp < 10 else -9)
    return (y + (1 if m <= 2 else 0), m, d)
}
```

要点：

- 两个函数依赖 `//` 的向下取整语义（负 `zz` 时 `era` 向下而非截断），与 02-types §3.3 一致；`%` 的 floor 语义同理（`micros % _MICROS_PER_DAY` 恒非负）。
- 历元微秒换算与回解（内部函数，负责范围检查）：

  ```ms
  // _epochMicros returns the epoch-microsecond count of the given fields;
  // the fields must already be validated.
  func _epochMicros(y, m, d, hh, mi, ss, us)

  // _dateTimeFromEpochMicros converts an epoch-microsecond count back to
  // a DateTime (UTC). Raises OverflowError if the resulting year is
  // outside [MIN_YEAR, MAX_YEAR].
  func _dateTimeFromEpochMicros(micros)
  ```

  `_epochMicros` = `(_daysFromCivil(y, m, d) * _SECONDS_PER_DAY + hh * 3600 + mi * 60 + ss) * _MICROS_PER_SECOND + us`；`_dateTimeFromEpochMicros` 以 `days := micros // _MICROS_PER_DAY`、`rem := micros % _MICROS_PER_DAY`（floor 语义保证 `rem` 非负）拆出日与日内微秒，`_civilFromDays(days)` 回解日期并检查年份范围，再拆时/分/秒/微秒后构造 `DateTime`。
- 星期：`_weekdayFromDays(z)` 返回 `(z + 3) % 7`（1970-01-01 是星期四，Monday-first 取值 0–6，对齐 Python `weekday()`）。
- 参数校验助手：`_checkInt(value, name)`（非 int——含 bool，bool 是独立类型故 `isinstance(v, int)` 天然排除——时抛 `TypeError`，消息含参数名）与 `_checkRange(value, lo, hi, name)`（越界抛 `ValueError`），供三个类的构造函数复用。

### TimeDelta 类

时长值对象。公开属性对齐 Python 的归一化三元组，内部另存总微秒数使运算/比较/哈希为 O(1) 整数操作：

```ms
// TimeDelta is a duration. Public attributes days, seconds and
// microseconds are the normalized triple (same convention as Python:
// 0 <= seconds < 86400, 0 <= microseconds < 1000000, the sign is
// carried by days alone). Instances are immutable by convention.
class TimeDelta {
    // Constructs from a total microsecond count (internal use).
    func __init__(self, totalMicros)

    // totalSeconds returns the duration as a float number of seconds.
    func totalSeconds(self)

    // magic: __eq__ __ne__ __lt__ __le__ __gt__ __ge__ __hash__
    // __add__ __sub__ __mul__ __truediv__ __neg__ __str__ __repr__
}
```

工厂函数（规范 §9 的 `datetime.timedelta(days=1)` 形态；参数名与顺序对齐 Python）：

```ms
// timedelta returns a TimeDelta built from the given components.
// Each component must be an int or float; floats are rounded to the
// nearest microsecond per component, then summed exactly.
// Raises TypeError on non-numeric components.
func timedelta(days = 0, seconds = 0, microseconds = 0, milliseconds = 0, minutes = 0, hours = 0, weeks = 0)
```

语义约定：

- 归一化：构造时 `total` 经 floor 语义拆分——`days := total // _MICROS_PER_DAY`、`seconds := (total % _MICROS_PER_DAY) // _MICROS_PER_SECOND`、`microseconds := total % _MICROS_PER_SECOND`，并存 `self._totalMicros = total`。`timedelta(microseconds=-1)` 得 `days=-1, seconds=86399, microseconds=999999`（Python 同）。
- 无范围上限：任意精度 int 承载，`TimeDelta` 自身不抛 `OverflowError`；越界检查发生在与 `DateTime` 混合运算的结果端。
- 算术：`td ± td` → `TimeDelta`；`td + dt` → `DateTime`（委托同一模块内的 `_dateTimeFromEpochMicros(dt._epochMicros() + self._totalMicros)`，与 `dt + td` 对称——02-types §8 无反射方法，此方向须在 `TimeDelta.__add__` 内显式处理）；`td * n`、`td / n`（n 为 int/float，结果舍入到微秒，`int(round(...))`）；`td / td` → float 比值；`-td`。其余操作数类型抛 `TypeError`。
- 比较/哈希：按 `_totalMicros` 整数比较；`__eq__` 对非 `TimeDelta` 返回 `false`、排序比较对非 `TimeDelta` 抛 `TypeError`；`__hash__` 返回 `_totalMicros`（int，满足 §6 的等值同哈希契约）。
- `__str__`：Python 风格 `"[-]D day[s], ]H:MM:SS[.ffffff]"`（`days == 0` 时省略日前缀，`microseconds == 0` 时省略小数部分）；`__repr__`：`"datetime.TimeDelta(days=1, seconds=3600)"`（只列非零分量，全零为 `"datetime.TimeDelta(0)"`）。
- 浮点分量舍入为**逐参数舍入再求和**（与 Python 先求和再舍入存在 ±1 微秒级差异），此裁定在文档注释中显式注明。

### Time 类

时刻值对象（不承载日期），对应规范示例 `datetime.time(12, 30)`：

```ms
// Time is a time of day, independent of any date. Attributes: hour,
// minute, second, microsecond. Immutable by convention.
class Time {
    // Raises TypeError on non-int fields, ValueError on out-of-range
    // fields (hour 0-23, minute/second 0-59, microsecond 0-999999).
    func __init__(self, hour = 0, minute = 0, second = 0, microsecond = 0)

    // isoformat returns "HH:MM:SS" with ".ffffff" appended when the
    // microsecond field is non-zero.
    func isoformat(self)

    // magic: __eq__ __ne__ __lt__ __le__ __gt__ __ge__ __hash__ __str__ __repr__
}
```

- 比较/哈希按「日内微秒数」整数；跨类型规则同 `TimeDelta`（`__eq__` 返 `false`、排序抛 `TypeError`）。
- 不提供算术（对齐 Python 的 `datetime.time`，规范亦未要求）；`__str__` 即 `isoformat()`；`__repr__` 为 `"datetime.Time(12, 30)"` 风格（省略取默认值的尾部参数）。
- 工厂 `time(hour, minute, second = 0, microsecond = 0)` 直接转发构造。

### DateTime 类

```ms
// DateTime is a naive UTC date-time on the proleptic Gregorian
// calendar. Attributes: year, month, day, hour, minute, second,
// microsecond. Instances are immutable by convention: arithmetic
// returns new instances, and writing fields directly is unsupported.
class DateTime {
    // Raises TypeError on non-int fields; ValueError when a field is
    // out of range (year in [MIN_YEAR, MAX_YEAR], month 1-12, day
    // 1..daysInMonth, hour 0-23, minute/second 0-59, microsecond
    // 0-999999).
    func __init__(self, year, month, day, hour = 0, minute = 0, second = 0, microsecond = 0)

    // strftime formats this DateTime per fmt (see the directive table).
    // Raises TypeError if fmt is not a str, ValueError on an
    // unsupported directive.
    func strftime(self, fmt)

    // isoformat returns "YYYY-MM-DD<sep>HH:MM:SS" with ".ffffff"
    // appended when the microsecond field is non-zero.
    func isoformat(self, sep = "T")

    // timestamp returns the epoch seconds as a float (the inverse of
    // fromTimestamp; precision is limited by float64).
    func timestamp(self)

    // weekday returns 0 (Monday) through 6 (Sunday).
    func weekday(self)

    // magic: __eq__ __ne__ __lt__ __le__ __gt__ __ge__ __hash__
    // __add__ __sub__ __str__ __repr__
}
```

语义约定：

- 构造即校验并缓存 `self._epochMicros`（`_epochMicros` 内部函数的计算结果），比较/哈希/算术全部读缓存。
- 算术：`dt + td` / `dt - td` → `DateTime`（历元微秒加减后经 `_dateTimeFromEpochMicros` 回解，结果年份越界抛 `OverflowError`）；`dt - dt` → `TimeDelta`；其他操作数抛 `TypeError`。
- 比较/哈希：按历元微秒；跨类型规则同上（`__eq__` 返 `false`、排序抛 `TypeError`）；`__hash__` 返回历元微秒 int。等值判定是「同一时刻」判定（naive UTC 下无歧义）。
- `__str__` 为 `isoformat(" ")`（日期与时刻间空格分隔，Python 同）；`__repr__` 为 `"datetime.DateTime(2025, 1, 2, 3, 4, 5)"` 风格（尾部默认值参数省略，`microsecond` 非零时才出现）。
- 属性只读约定：MS 无只读属性机制，直接写 `dt.year = ...` 会破坏 `_epochMicros` 缓存一致性——裁定「实例按约定不可变」，文档注释明示，不提供 setter 与 `replace()`（见「设计取舍说明」）。
- 模块级函数：`now()` 即 `fromTimestamp(time.now())`；`fromTimestamp(ts)` 接受 int（精确乘 `_MICROS_PER_SECOND`）或 float（`int(round(ts * 1000000.0))`，就近舍入），其余类型抛 `TypeError`，结果年份越界抛 `OverflowError`；`date(y, m, d)` 即 `DateTime(y, m, d)`。

### strftime 指令表

`strftime(fmt)` 逐码点扫描 `fmt`，`%` 引导指令，其余字符原样输出；数字零填充经 `strings.padLeft(str(n), width, "0")`；输出片段收集到 list 后经 `strings.join(parts, "")` 合成。支持的指令集：

| 指令 | 输出 | 示例（2025-01-02 03:04:05.123456，星期四） |
|---|---|---|
| `%Y` | 年，零填充至 4 位 | `2025` |
| `%y` | 年后两位，零填充 | `25` |
| `%m` / `%d` | 月 / 日，零填充 2 位 | `01` / `02` |
| `%H` / `%I` | 时（24 制）/ 时（12 制），零填充 2 位 | `03` / `03` |
| `%M` / `%S` | 分 / 秒，零填充 2 位 | `04` / `05` |
| `%f` | 微秒，零填充 6 位 | `123456` |
| `%j` | 年内日序，零填充 3 位 | `002` |
| `%p` | `AM` / `PM` | `AM` |
| `%a` / `%A` | 星期缩写 / 全名（英文） | `Thu` / `Thursday` |
| `%b` / `%B` | 月缩写 / 全名（英文） | `Jan` / `January` |
| `%z` / `%Z` | 空串（naive 无时区信息，对齐 Python naive 行为） | `` |
| `%%` | 字面 `%` | `%` |

未列出的指令（含 `%c`/`%x`/`%U`/`%W`/`%G`/`%V` 等本地化或 ISO 周指令）抛 `ValueError`——指令集是封闭清单，不静默透传。`%I` 的 12 制换算：`h12 := hour % 12`，`h12 == 0` 时取 `12`。

### strptime 解析器

`strptime(text, fmt)` 是 `strftime` 的逆：双指针逐码点扫描（`pos` 指向 `text`，指令循环扫 `fmt`），产出字段 dict 后装配 `DateTime`。规则：

- 字段初值：year=1900、month=1、day=1、时/分/秒/微秒=0（Python 缺省同）；同一字段多次出现时后者覆盖前者。
- 数字指令（`%Y %y %m %d %H %I %M %S %j`）：按指令的 1..max 位宽读取连续数字（`%Y` 1–4 位，`%j` 1–3 位，`%f` 1–6 位，其余 1–2 位），不足最小位宽或全无数字抛 `ValueError`；读取用内部 `_readDigits(text, pos, minLen, maxLen)`（返回 `(value, newPos)`），不做「异常当控制流」（12-ms-style §5.4）。
- `%y` 两位年按 Python 转折规则展开：`<= 68` → 2000 系，否则 1900 系。
- `%f` 按小数位解释：`"123"` → 123000 微秒（右侧补零至 6 位，Python 同）。
- 名称指令（`%a %A %b %B`）：大小写不敏感地匹配内部名称表；星期名解析后**忽略**（不做与日期的 consistency 检查，Python 同）；月名设置 month。
- `%p`：大小写不敏感匹配 `AM`/`PM`；仅在同一 `fmt` 中出现 `%I` 时生效（12 AM→0、12 PM→12、其余 PM +12），否则忽略（Python 同）。
- `%j` 年内日序：经 `_isLeapYear` 校验上界（平年 ≤ 365）后换算为 month/day；与 `%m`/`%d` 同时出现时**以 `%j` 为准**（显式裁定）。
- 字面字符须与 `text` 逐码点相等；`fmt` 中的连续空白匹配 `text` 中**至少一个**连续空白；`%%` 匹配字面 `%`。
- `fmt` 耗尽而 `text` 有剩余（或反之）抛 `ValueError`；未知指令、以及 `%z`/`%Z`（本模块 naive，不解析时区）抛 `ValueError`。
- 解析成功的字段经 `DateTime(...)` 构造，字段越界（如 `month=13`）自然落到构造器的 `ValueError`。

`strftime`/`strptime` 在上表指令集内互逆：对任意合法 `dt`，`strptime(dt.strftime(fmt), fmt) == dt`（`fmt` 含 `%y` 时 1900–1999/2000–2068 外的年份除外；含 `%p` 而无 `%I` 时时刻除外）——此性质是测试方案的核心断言形态。

### 设计取舍说明

- **naive UTC 裁定**（设计依据第 1 条）：规范未定义时区；若要求 `now()` 返回本地时间，纯 .ms 无法从 §9 的 time 原语取得本地偏移，且测试将依赖运行环境时区。取 UTC 使模块自洽、测试确定；将来任务 39 若暴露本地时区原语，可追加 `nowLocal()` 类入口而不破坏现有 API。
- **不用 `time.format`/`time.parse` 做字段分解**：§9 的 `time.format(ts, layout)` 输出字符串，用它反解字段等于「格式化再解析」的往返，慢且引入 layout 方言依赖；历法算法在模块内整数实现后，`time` 模块的唯一用途收敛为 `now()` 的时钟源，耦合最小。
- **示例清单的最小扩充**：`microsecond` 字段、`isoformat`/`timestamp`/`weekday`/`totalSeconds` 四个方法、`fromTimestamp` 函数是使 §9 示例语义闭环（含小数秒不丢失、时间戳往返、星期可测）的最小集合；其余 Python API（`replace`/`timetuple`/`fromisoformat`/`combine`/`astimezone`、ISO 周、时区系列）刻意不提供，需要时另立任务增补。
- **`date()` 不引入 `Date` 类**：规范示例只有三处类型线索（DateTime 对象、timedelta、time）；独立 `Date` 类会引入 `Date`/`DateTime` 比较与混合运算的一整套歧义，收益不成比例。`date()` 返回午夜 `DateTime` 满足示例用法。
- **不可变按约定**：MS 无只读属性机制；算术一律返回新实例，文档注释声明不支持直写字段。与 Python datetime 的用户可见语义一致（Python 靠 C 层强制，MS 靠约定）。

## 实现步骤

1. 建 `lib/datetime.ms` 骨架：文件头注释、两个 import、公开常量 `MIN_YEAR`/`MAX_YEAR` 与内部常量（含名称表）、三个公开类与六个模块级函数的签名及英文文档注释，函数体先 `pass`。验证：`import "datetime"` 成功，全部公开名字可访问（`dir(datetime)` 或逐属性读取）。
2. 实现历法核心：`_isLeapYear`、`_daysInMonth`、`_daysFromCivil`、`_civilFromDays`、`_weekdayFromDays`、`_epochMicros`、`_dateTimeFromEpochMicros`、`_checkInt`/`_checkRange`。验证：ms 脚本断言历元锚点（`date(1970, 1, 1)` 的历元微秒为 0、weekday 为 3）、闰年表（2000 闰、1900 平、2024 闰）、采样区间内 days→civil→days 往返一致。
3. 实现 `TimeDelta` 与 `timedelta()`：校验、归一化、算术（含 `td + dt` 委托）、比较/哈希、`totalSeconds`/`__str__`/`__repr__`。验证：归一化样例（`timedelta(microseconds=-1)`）、四则与取负、跨类型 `TypeError`、哈希等值一致。
4. 实现 `DateTime` 与 `now()`/`fromTimestamp()`/`date()`：字段校验、历元微秒缓存、算术（含 `OverflowError` 边界）、比较/哈希、`isoformat`/`timestamp`/`weekday`/`__str__`/`__repr__`。验证：已知时间戳锚点（`fromTimestamp(0)`、`fromTimestamp(1000000000)`、负时间戳）、`dt ± td`、`dt - dt`、年份边界溢出。
5. 实现 `Time` 与 `time()`：校验、比较/哈希、`isoformat`。验证：字段校验负例、排序、`str` 输出。
6. 实现 `strftime`：指令分派表、零填充、名称表、`%%`、未知指令 `ValueError`。验证：指令表逐条样例（含 `%j`、`%I`/`%p`、`%z`/`%Z` 空串）。
7. 实现 `strptime`：扫描器、`_readDigits`、名称匹配、`%y` 转折、`%j` 换算、`%p`/`%I` 联动、空白与字面匹配、各类 `ValueError`。验证：与 `strftime` 的往返断言、各负例。
8. 编写完整测试文件（见「测试方案」），经仓库根 `run_tests.py` 调用 mslang CLI 全量运行通过。

## 测试方案

按 README 测试约定：本任务晚于任务 40（testing 模块），测试一律使用 `testing` 与 `testing/assert` 模块（`assert.equal` / `assert.isTrue` / `assert.raises`）。测试文件 `tests/ms/stdlib/datetime_test.ms`，测试函数以 `test` 开头，末尾 `testing.run()`；由 `run_tests.py` 递归发现并驱动。全部用例在 naive UTC 裁定下确定性可复现（仅 `now()` 用例外包络断言，不断言具体字段值）。本任务只交付设计文档，脚本随实现编写。

覆盖清单：

- 历法核心（经公开 API 间接断言）：`date(1970, 1, 1).timestamp()` 为 0、`weekday()` 为 3；已知锚点 `fromTimestamp(1000000000)` → 2001-09-09 01:46:40、`fromTimestamp(-1)` → 1969-12-31 23:59:59；闰年——`date(2000, 2, 29)` 合法、`date(1900, 2, 29)` 与 `date(2025, 2, 29)` 抛 `ValueError`；星期抽查（2024-01-01 周一 = 0，2025-01-01 周三 = 2）；连续区间内 `d` 与 `d + timedelta(days=1)` 的日期演进跨年/跨月/跨闰日正确。
- `DateTime` 构造校验：每个字段的越界负例（year 0 与 10000、month 0/13、day 0/32、hour 24、minute 60、second 60、microsecond 1000000）抛 `ValueError`；非 int 字段（含 `true`、float、str）抛 `TypeError`。
- 算术：`date(2025, 1, 1) + timedelta(days=1)` → 2025-01-02；`timedelta(hours=-1)` 跨日回退；`timedelta(days=1) + dt`（反向加）同结果；`dt1 - dt2` → `TimeDelta` 值正确（含负时长）；`date(9999, 12, 31) + timedelta(days=1)` 与 `date(1, 1, 1) - timedelta(days=1)` 抛 `OverflowError`；`dt + 1`、`dt - "x"` 抛 `TypeError`；算术不改写原实例（原 `dt` 字段不变）。
- 比较/相等/哈希：`==`/`!=`/`<`/`<=`/`>`/`>=` 按时刻序；等值不同对象哈希相等且可作 dict 键（`d[dt1] = "x"; d[dt2] == "x"`，`dt1 == dt2` 且 `dt1 is not dt2`）；`dt == 42` 为 `false`；`dt < Time(...)` 抛 `TypeError`。
- `TimeDelta`：归一化（`timedelta(days=1, hours=25)` → `days=2, seconds=3600`；`timedelta(microseconds=-1)` → `days=-1, seconds=86399, microseconds=999999`）；`td * 2`、`td / 2`、`td1 / td2` 为 float、`-td`；float 分量（`timedelta(hours=1.5)` → 5400 秒）；比较与排序；`totalSeconds()` 值；`str`/`repr` 形态（含负时长、全零）；非数值分量抛 `TypeError`。
- `Time`：`time(12, 30)` 字段与 `isoformat()` == `"12:30:00"`；微秒非零时的 `.ffffff` 后缀；排序与等值；越界与类型负例。
- `strftime`：全指令逐条断言（固定 `dt := DateTime(2025, 1, 2, 3, 4, 5, 123456)`）：`"%Y-%m-%d"` → `"2025-01-02"`、`%y`、`%I`/`%p`（凌晨与午后两例）、`%f` → `"123456"`、`%j`（2025-12-31 → `"365"`，闰年 2024-12-31 → `"366"`）、`%a`/`%A`/`%b`/`%B` 英文名、`%%` → `"%"`、`%z`/`%Z` → 空串；未知指令 `%Q` 抛 `ValueError`；非 str 实参抛 `TypeError`。
- `strptime`：与 `strftime` 的往返（`"%Y-%m-%d %H:%M:%S"`、`"%Y-%m-%dT%H:%M:%S.%f"`、`"%a %b %d %H:%M:%S %Y"`）；`%y` 转折（`"68"` → 2068、`"69"` → 1969）；`%j`（`strptime("2025-060", "%Y-%j")` → 2025-03-01，闰年 `2024-060` → 2024-02-29）；`%p` 联动（`"%I %p"` 的 12 AM/PM 边界）；字面与空白匹配；负例——文本与格式不符、尾部多余字符、数字位不足、非法名称、`%z` 指令、字段越界（`month=13`）均抛 `ValueError`。
- `now()`：`t0 := time.now()` 与 `t1 := time.now()` 夹逼，`t0 <= datetime.now().timestamp() <= t1` 成立（浮点比较用 `<=`，不做 `==`）。
- 常量：`MIN_YEAR == 1`、`MAX_YEAR == 9999`；`isinstance` 判定（`isinstance(dt, datetime.DateTime)` 等三类）。

## 验收标准

- [ ] `lib/datetime.ms` 存在，实现「公开 API 总览」的全部名字（2 常量 + 6 函数 + 3 类），覆盖 07-stdlib §9 示例的全部形态；无 C 代码，import 仅 `strings` 与 `time`，无模块级可变状态，无 `__name__` 守卫。
- [ ] ms 代码符合 12-ms-style：4 空格缩进、行宽 120、内部函数/常量 `_` 前缀、名称表声明处附只读约定注释、公开类/函数/常量带英文前置文档注释、不遮蔽内建名、文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- [ ] 历法核心为整数 days-from-civil 算法：`date(1970, 1, 1)` 历元微秒为 0、weekday 为 3；格里高利闰年规则正确；负天数（1970 年前日期）换算正确。
- [ ] naive UTC 语义落地：`now()`/`fromTimestamp()` 按 UTC 分解；模块无时区 API；`strftime` 的 `%z`/`%Z` 输出空串，`strptime` 对其抛 `ValueError`。
- [ ] `TimeDelta` 归一化与 Python 一致（`0 <= seconds < 86400`、`0 <= microseconds < 1000000`，符号由 days 承载）；`timedelta` 工厂的关键字参数集与顺序与本文一致；float 分量逐参数舍入到微秒。
- [ ] 算术规则落地：`dt ± td` 返回新 `DateTime`、`td + dt` 可用、`dt - dt` 得 `TimeDelta`、结果年份越出 `[MIN_YEAR, MAX_YEAR]` 抛 `OverflowError`；三个类的跨类型 `__eq__` 返回 `false`、排序比较抛 `TypeError`；定义 `__eq__` 的类均定义 `__hash__` 且等值同哈希。
- [ ] 校验完整：构造与工厂对非 int 字段抛 `TypeError`、越界字段抛 `ValueError`，消息含字段名；`strftime` 未知指令、`strptime` 解析失败均抛 `ValueError`。
- [ ] `strftime`/`strptime` 指令集为本文封闭清单且互逆（`%y`/`%p` 的注明例外除外）；`%y` 转折、`%j` 换算、`%p`/`%I` 联动、`%f` 右侧补零规则与本文一致。
- [ ] `import "datetime"` 在解释器中可用，模块经 `lib/` 解析档命中（机制属任务 24，本任务不改动加载器）。
- [ ] `tests/ms/stdlib/datetime_test.ms` 覆盖「测试方案」全部清单项，`run_tests.py` 全量通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 39（`time.now`）、任务 40（`testing`/`assert` 接口）、任务 25（魔术方法协议）的接口假定在实现时已对齐对应任务文档。
