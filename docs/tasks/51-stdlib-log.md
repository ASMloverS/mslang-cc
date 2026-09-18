# 51 标准库：log

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [19 标准库：fmt](19-stdlib-fmt.md)、[39 标准库：time](39-stdlib-time.md) |

## 任务目标

交付纯 .ms 实现的标准库模块 `lib/log.ms`：带级别过滤的包级日志设施——级别常量 `DEBUG` / `INFO` / `WARN` / `ERROR`，变参日志函数 `debug` / `info` / `warn` / `error`，以及三个配置函数 `setLevel`（级别阈值）、`setOutput`（输出目标，默认 `io.stderr`）、`setFormat`（记录模板，占位符 `{time}` / `{level}` / `{message}`）。模块不含任何 C 代码：消息取值渲染委托任务 19（fmt）的 `%v` 语义，时间戳委托任务 39（time）的 `time.now` / `time.format`，默认输出目标与 Writer 鸭子协议来自任务 37（io）。完成后脚本 `import "log"` 即可使用全部公开接口；本任务通过 `tests/ms/` 下的 ms 脚本测试（testing 模块，任务 40 已先于本任务完成）独立验收。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`log` 为纯脚本模块（`lib/` 目录）。
  - §11 log：本模块的完整公开 API 清单，即全部设计输入：

    ```ms
    log.debug(...) log.info(...) log.warn(...) log.error(...)
    log.setLevel(log.INFO)               // DEBUG < INFO < WARN < ERROR
    log.setOutput(writer)                // defaults to stderr
    log.setFormat("{time} [{level}] {message}")
    ```

    清单未给出默认级别、级别数值、消息装配语义与错误行为——这些规范空白由本文「详细设计」逐条固定。
- `docs/language/03-syntax.md`
  - §4 函数：变参定义 `func f(*rest)`；调用点**无展开语法**——`*args` 只在定义侧打包（任务 13 定为打包成 list）。这直接决定消息装配不能采用「首参数为 printf 格式串 + 其余转发 `fmt.sprintf`」的 Python logging 式形态（无法把运行期实参列表转发给变参函数），只能采用 print 式拼接（见「详细设计」）。
  - §9 内建函数：`hasattr(o, n)`（Writer 鸭子协议检查）、`str(x)`、`isinstance(x, T)`。
  - §9.1 print：多个值以 `str()` 表示、单个空格分隔——log 的消息装配语义与之对齐。
- `docs/language/02-types.md` §3：`bool` 是独立类型而非 `int` 的子类型——`setLevel(true)` 因 `isinstance(level, int)` 不成立被自然拒绝，无需额外分支（与任务 21/53 的既定约定一致）。
- `docs/language/04-exceptions.md` §4：参数类型错误抛 `TypeError`、取值/格式非法抛 `ValueError` 的层级依据。
- `docs/language/05-modules.md` §2：`lib/` 纯脚本模块在解析顺序第一档「内建/已注册模块」中命中；§4：模块首次导入时顶层代码执行一次并缓存——包级状态（当前级别/输出目标/模板）作为模块级单例的语言基础（对齐 Go `log` 包的标准 logger 形态）。
- `docs/language/11-project-layout.md` §1：纯脚本标准库模块置于 `lib/`；§4 测试策略：脚本测试位于 `tests/ms/`。
- `docs/language/12-ms-style.md`：§2.1 import 分组与字母序、§2.2 纯库文件不写 `__name__` 守卫、§4 命名（内部函数 `_` 前缀、模块级常量大写蛇形）、§5.7（简单插值用 f-string）、§5.6（模块级可变状态须大写蛇形命名并注释理由与并发安全性）、§6（公开函数的英文前置文档注释）。
- 依赖任务 37（io）的文档已存在：`io.stderr` 是实现 Writer 协议（`write(data)` → int、`flush()`）的 File 实例，且该文档明确将「`log.setOutput`（任务 51）」列为 `io.stderr` 的预期消费方——默认输出目标据此定为 `io.stderr`。任务 37 编号小于 51 且先于本任务完成，属顺序依赖（README 依赖图仅标注非平凡跨分支边），不另列入依赖表。
- 依赖任务 19（fmt）、39（time）与任务 40（testing）的文档本文撰写时**尚不存在**：`fmt.sprintf(format, ...)`（含 `%v` 动词）取自 07-stdlib §1；`time.now()`（float Unix 秒）与 `time.format(ts, layout)` 取自 §9，layout 语法假定为 Go 参考时间布局（`"2006-01-02 15:04:05"`）；`testing.run()` 与 `testing/assert` 的 `equal` / `isTrue` / `raises` 取自 §19。实现时以对应任务文档定名为准。

## 详细设计

本任务是纯 .ms 模块，无 C 结构体与 C 函数签名；接口级设计以 ms 函数签名与语义约定表达。ms 代码遵循 `docs/language/12-ms-style.md`（4 空格缩进、行宽 120、小驼峰函数名、内部名字 `_` 前缀、公开函数带英文前置文档注释）。

### 文件与模块骨架

- 唯一交付文件：`lib/log.ms`。文件头注释一句话说明职责；import 仅标准库一组、按路径字母序：`fmt`、`io`、`time`；不写 `__name__` 守卫（纯库文件，12-ms-style §2.2）。
- 模块经任务 24 的模块系统以 `"log"` 名注册/解析，脚本侧 `import "log"` 后以 `log.info(...)` 形式调用（05-modules §1 绑定规则）。
- 模块级常量（只读，大写蛇形）：

```ms
// Severity levels. Numeric spacing mirrors Python logging so that custom
// intermediate thresholds (e.g. setLevel(15)) remain meaningful.
const-like: DEBUG := 10  INFO := 20  WARN := 30  ERROR := 40   // 以 := 声明，按常量纪律只读

// _DEFAULT_FORMAT is the record template used until setFormat is called.
_DEFAULT_FORMAT := "{time} [{level}] {message}"

// _TIME_LAYOUT renders the {time} placeholder (Go reference layout).
_TIME_LAYOUT := "2006-01-02 15:04:05"
```

- 模块级可变状态（12-ms-style §5.6 要求的大写蛇形命名与理由注释）：

```ms
// Package-level logger configuration (singleton).
// Rationale: mirrors Go's log package standard logger so that log.info(...)
// works without explicit logger plumbing.
// Concurrency: NOT thread-safe under the M:N scheduler (no locking);
// coroutines on a single thread are safe (functions never yield; writer
// writes may block the thread but do not suspend the coroutine).
_LEVEL := INFO
_WRITER := io.stderr
_FORMAT := _DEFAULT_FORMAT
```

### 公开接口

```ms
// debug logs a message at DEBUG level. Arguments are rendered with fmt's
// %v semantics and joined with single spaces (like print); zero arguments
// produce an empty message. The call is a no-op when DEBUG < current level.
func debug(*args)

// info / warn / error: same contract as debug, at their respective levels.
func info(*args)
func warn(*args)
func error(*args)

// setLevel sets the minimum severity that is emitted; lower-severity calls
// become no-ops. level must be an int (bool is rejected, as bool is not int);
// any int is accepted, so intermediate thresholds are allowed. The default
// is INFO. Raises TypeError on non-int level.
func setLevel(level)

// setOutput redirects log output to writer, which must implement the
// io.Writer protocol (a write(data) method; flush() is called after each
// record when present). nil resets the output to the default io.stderr.
// Raises TypeError if writer lacks a write method.
func setOutput(writer)

// setFormat sets the record template. Supported placeholders are {time},
// {level} and {message}; {{ and }} emit literal braces. The template is
// validated eagerly. nil resets to the default "{time} [{level}] {message}".
// Raises TypeError on non-str format, ValueError on an unknown placeholder
// or unbalanced braces.
func setFormat(format)
```

语义与边界约定：

- **公开面与 07-stdlib §11 清单一一对应，不增删**：四个级别常量、四个日志函数、三个配置函数，共十一个公开名。不提供 `fatal`/`panic`（规范未列入；进程退出经 `os.exit`）、不提供具名 Logger 对象（Go 的 `log.New` 形态留待路线图评估）。
- **消息装配为 print 式拼接**：`len(args) == 0` 时消息为 `""`；否则每个实参经 `fmt.sprintf("%v", a)` 渲染后以单个空格连接（Go `log` 包 `fmt.Sprint` 语义的 MS 对应）。首参数**不**被当作 printf 格式串解释——MS 无调用点展开语法（03-syntax §4），无法把运行期实参列表转发给 `fmt.sprintf` 的变参形，printf 式签名在纯 .ms 中不可实现，故作此取舍并在此显式承认与 Python logging 的差异。需要格式控制的消息由调用方自行用 f-string 或 `fmt.sprintf` 拼好后单参数传入（12-ms-style §5.7 的分工约定）。
- `%v` 对任意取值的渲染规则（str 不加引号、float 最短往返表示等）以任务 19 文档为准；若任务 19 定稿的 `%v` 不支持任意值或语义不同，消息渲染退回内建 `str()`（行为对 str/数值等价），实现时对齐。
- **级别过滤先于一切副作用**：`_emit` 入口比较 `level < _LEVEL` 即返回，不渲染消息、不取时钟、不触碰 writer——被过滤的调用对输出目标零副作用（可据此用「write 必抛错的 writer」断言过滤路径）。
- **记录发射**：渲染后的记录追加一个 `"\n"`（每条日志恰好一行，对齐 Go），经 `_WRITER.write(...)` 写出；`hasattr(_WRITER, "flush")` 为真时再调 `flush()`——`flush` 对 Writer 是可选的（io 协议含 `flush`，但鸭子类型的简易 writer 可省略），write 是必需的。write/flush 抛出的异常（如 File 的 `OSError`）**原样传播给调用方**，log 不吞 IO 错误（显式取舍：Python logging 默认吞掉，Go log 忽略；MS 的异常哲学选择让调用方可见）。
- `{time}` 渲染：`time.format(time.now(), _TIME_LAYOUT)`，每次发射取一次时钟；layout 固定不可配（配置维度只有整条模板），layout 语法假定以任务 39 文档为准。
- **模板替换单次完成**：`{message}` 的替换文本原样进入结果，**不做二次扫描**——消息内容里的 `{level}` 等字符序列不被当作占位符。
- `setLevel` 接受任意 int（含负值与大于 40 的值，阈值语义自然成立）；非 int（含 bool、float、str、nil）抛 `TypeError`。
- `setOutput(nil)` 与 `setFormat(nil)` 均为「恢复默认」；`setFormat("")` 合法（记录为空行），不设额外门槛。

### 内部函数与核心流程

```ms
// _joinMessage renders args with fmt's %v semantics and joins them with
// single spaces. Zero args yield "".
func _joinMessage(args)

// _validateFormat scans a template: "{" must start a known placeholder
// ({time}/{level}/{message}) or be doubled ({{); "}" must be doubled (}}).
// Raises ValueError otherwise. Called only from setFormat (fail-fast), so
// _renderRecord needs no error path.
func _validateFormat(format)

// _renderRecord performs a single pass over _FORMAT: literal text is copied,
// the three placeholders are replaced (timestamp computed once via
// time.format(time.now(), _TIME_LAYOUT)), {{ and }} collapse to one brace.
func _renderRecord(levelName, message)

// _emit is the shared body of the four logging functions.
func _emit(level, levelName, args) {
    if level < _LEVEL {
        return    // filtered: no formatting, no clock read, no writer access
    }
    message := _joinMessage(args)
    record := _renderRecord(levelName, message)
    _WRITER.write(f"{record}\n")
    if hasattr(_WRITER, "flush") {
        _WRITER.flush()
    }
}
```

- 四个公开日志函数均为一行委托：`debug(*args)` → `_emit(DEBUG, "DEBUG", args)`，其余同理。级别名按发射函数固定，不做数值到名字的查表——自定义级别只影响 `setLevel` 的阈值比较，不改变四个函数的名字输出。
- 配置函数写模块级状态前需 `global _LEVEL` 等声明（03-syntax §8）；校验（TypeError/ValueError）先于赋值，非法调用不留下半个新状态。

## 实现步骤

1. 建 `lib/log.ms` 骨架：文件头注释、import（`fmt`/`io`/`time`）、级别常量与 `_DEFAULT_FORMAT`/`_TIME_LAYOUT`、模块级状态（含 12-ms-style §5.6 要求的理由/并发注释）、十一个公开名与四个内部函数的签名及英文文档注释，函数体先 `pass`。验证：`import "log"` 成功，`log.INFO` 等常量与十一个名字均可访问，常量值满足 `DEBUG < INFO < WARN < ERROR`。
2. 实现 `_joinMessage`（`%v` 逐参渲染 + 空格连接 + 空实参情形）。验证：`setOutput` 暂以假 writer 配合 `setFormat("{message}")`，断言零/单/多实参与非 str 取值（int、float、list、nil、true）的装配结果。
3. 实现 `_validateFormat` 与 `_renderRecord`（单遍模板替换、`{{`/`}}` 折叠、消息不再扫描）与 `setFormat`（nil 恢复默认、TypeError/ValueError 路径、先校验后赋值）。验证：自定义模板、占位符乱序与重复、字面量大括号、非法模板各负例。
4. 实现 `setLevel` 与 `_emit` 的过滤前置。验证：各级别在阈值上/下的发射与抑制；自定义中间阈值；非 int 负例。
5. 实现 `setOutput` 与发射路径（`write` 必需检查、换行追加、可选 `flush`、nil 恢复 `io.stderr`、异常原样传播）。验证：假 writer 捕获逐行断言、flush 计数、无 flush 方法的 writer 可用、缺 write 方法抛 TypeError、write 抛错传播。
6. 接入时间戳（`time.now` + `time.format`）并确认默认模板 `" {time} [{level}] {message}"` 的整行结构。验证：默认模板下记录包含 `[INFO] ` 级别段与消息段，时间戳段长度与 `_TIME_LAYOUT` 渲染宽度一致（结构断言，不比对具体时刻）。
7. 编写完整测试文件（见「测试方案」），经仓库根 `run_tests.py` 调用 mslang CLI 全量运行通过。

## 测试方案

按 README 测试约定：本任务晚于任务 40（testing 模块），测试一律使用 `testing` 与 `testing/assert` 模块（`assert.equal` / `assert.isTrue` / `assert.raises`）。测试文件 `tests/ms/stdlib/log_test.ms`，测试函数以 `test` 开头，末尾 `testing.run()`；由 `run_tests.py` 递归发现并驱动。

测试基础设施（测试文件内自带，非模块交付物）：

- `class ListWriter`：实现 `write(self, data)`（追加到 `self.lines` 并返回 `len(data)`，履行 Writer 的字节数契约）与 `flush(self)`（`self.flushCount` 自增）；另备 `NoFlushWriter`（只有 `write`）与 `BadWriter`（`write` 抛 `OSError`）两个变体。
- 辅助函数 `resetLog()`：`log.setLevel(log.INFO)` + `log.setOutput(nil)` + `log.setFormat(nil)`。模块级状态在测试间共享，每个涉及配置的测试函数首尾调用它，避免交叉污染（`testing.run` 同模块顺序执行的既定行为下保证隔离）。
- 时间戳不做精确断言（ wall-clock 依赖），只对默认模板输出做结构断言（占位段位置、时间戳段宽度、级别段与消息段内容）。

覆盖清单：

- 常量与默认值：`DEBUG`/`INFO`/`WARN`/`ERROR` 严格递增且为 10/20/30/40；未经任何配置时 `log.info` 经 `setOutput` 到假 writer 后的记录符合默认模板 `"{time} [{level}] {message}"` 的结构。
- 消息装配：
  - 零实参：`log.info()` 记录的 `{message}` 段为空串。
  - 单实参：int、float、str、list、nil、true 的 `%v` 渲染逐类断言（如 `log.info(42)` → 消息段 `"42"`）。
  - 多实参：`log.info("a", 1, true)` → 消息段 `"a 1 true"`（单空格连接）。
  - 首实参含 `%`（如 `log.info("100% done")`）原样输出，不被当作格式串。
- 级别过滤：
  - `setLevel(log.WARN)` 后 `debug`/`info` 不产出、`warn`/`error` 产出；`setLevel(log.DEBUG)` 后四级全部产出。
  - 自定义阈值 `setLevel(25)`：`info`（20）被抑制、`warn`（30）产出。
  - 过滤零副作用：`setLevel(log.ERROR)` + `BadWriter`（write 必抛），`log.debug(...)` 不抛错（证明未触碰 writer、未读时钟渲染消息）。
  - 四个级别名分别出现在各自记录的级别段（`"[DEBUG] "` 等）。
- `setLevel` 负例：`"20"`、`1.5`、`true`、`nil` 均抛 `TypeError`。
- `setFormat`：
  - 自定义模板 `"{level}: {message}"` 的逐字节精确断言（消除时间戳不确定段）；占位符乱序与重复（`"{message}|{level}|{message}"`）。
  - 字面量大括号：`"{{level}} {message}"` 的 `{` 出现在输出中。
  - 消息不再扫描：`setFormat("{message}")` 后 `log.info("{level}")` 输出原样 `"{level}"`。
  - `setFormat(nil)` 恢复默认模板（结构断言同默认模板用例）。
  - 负例：`"{bad}"`（未知占位符）、`"{time"`（未闭合）、`"}"`（落单右括号）抛 `ValueError`；`setFormat(42)` 抛 `TypeError`；校验失败不覆盖旧模板（先设合法模板、再以非法模板调用、输出仍按旧模板）。
- `setOutput`：
  - 假 writer 逐行捕获，每条记录以 `"\n`" 结尾、恰好一行。
  - `flush` 每次发射恰好调用一次（`flushCount` 计数）；`NoFlushWriter` 可用（无 flush 方法不报错）。
  - `setOutput(nil)` 恢复 `io.stderr`：恢复后 `log.info` 不抛错且假 writer 不再增长（stderr 真实输出由驱动忽略，不做内容断言）。
  - 负例：`setOutput(42)`、`setOutput("s")`、无 `write` 方法的 class 实例抛 `TypeError`。
  - 异常传播：`BadWriter` 下 `log.error("x")` 抛 `OSError`（`assert.raises`）。
- 时间戳结构：默认模板下记录前 19 个字符为 `_TIME_LAYOUT` 渲染宽度的时间戳段，第 20 字符起为 `" [INFO] "` 级别段（结构断言，配合 `strings` 或逐下标比较，避免引入额外模块时可用切片与 `len`）。

## 验收标准

- [ ] `lib/log.ms` 存在，实现且仅实现 07-stdlib §11 的公开面：常量 `DEBUG`/`INFO`/`WARN`/`ERROR`，函数 `debug`/`info`/`warn`/`error`/`setLevel`/`setOutput`/`setFormat`（十一个公开名，清单不增删）；无 C 代码，import 仅 `fmt`/`io`/`time`。
- [ ] ms 代码符合 12-ms-style：4 空格缩进、行宽 120、内部函数 `_` 前缀、模块级可变状态大写蛇形并附理由与并发安全性注释、公开函数带英文前置文档注释、不遮蔽内建名、文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- [ ] 级别语义与本文一致：常量值 10/20/30/40 严格递增；`setLevel` 只接受 int（bool 被拒）否则 `TypeError`、任意 int 阈值可用；过滤先于消息渲染、时钟读取与 writer 访问。
- [ ] 消息装配为 `%v` 逐参渲染 + 单空格连接（零实参为空消息），首实参不作 printf 格式串解释；记录恰好一行（追加 `"\n"`）；`{message}` 替换文本不被二次扫描。
- [ ] 模板机制与本文一致：占位符恰为 `{time}`/`{level}`/`{message}`，`{{`/`}}` 折叠为字面大括号；`setFormat` 失败快速校验（未知占位符/括号未配对抛 `ValueError`，非 str 抛 `TypeError`），nil 恢复默认，校验失败不改变旧模板；`{time}` 经 `time.format(time.now(), _TIME_LAYOUT)` 渲染。
- [ ] 输出机制与本文一致：默认目标 `io.stderr`（任务 37 既定实例）；`setOutput` 要求 `write` 方法（缺失抛 `TypeError`）、`flush` 可选且每次发射后调用一次、nil 恢复默认；writer 抛出的异常原样传播。
- [ ] `import "log"` 在解释器中可用，模块经 `lib/` 解析档命中（机制属任务 24，本任务不改动加载器）。
- [ ] `tests/ms/stdlib/log_test.ms` 覆盖「测试方案」全部清单项，`run_tests.py` 全量通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 19（`fmt.sprintf`/`%v` 语义）、任务 39（`time.now`/`time.format` 与 layout 语法）、任务 40（`testing`/`assert` 接口）的接口假定在实现时已按对应任务文档对齐定名。
