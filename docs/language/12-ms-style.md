# 12 MS 编码规范

> 以 [Google Java 风格指南](https://google.github.io/styleguide/javaguide.html)的章节结构为骨架，裁剪不适用于动态脚本语言的部分，并结合 mslang 的语言特性补充编程实践约定。语言层面的词法/语法硬性规定见 [01-lexical.md](01-lexical.md) 与 [03-syntax.md](03-syntax.md)；本文档约定的是"合法代码中如何选择写法"。冲突时以本文档为准。

## 1. 源文件基础

- 文件后缀 `.ms`；文件名小写蛇形：`http_client.ms`、`json_utils.ms`。
- 测试文件以 `_test` 结尾：`http_client_test.ms`（见 [11-project-layout.md](11-project-layout.md) §4）。
- 编码 UTF-8（无 BOM），换行 LF。
- 每文件一个明确职责；单文件超过 ~500 行是拆分信号。

## 2. 源文件结构

文件内各段落按固定顺序排列：

1. 文件头注释（可选）：一句话说明模块职责。
2. import 语句。
3. 模块级常量。
4. 模块级变量（原则上不应存在，见 §5.6）。
5. 函数定义。
6. class 定义。
7. 顶层执行代码（收拢于文件末尾）。

### 2.1 import 组织

- 分三组：**标准库 → 第三方 → 本地（相对路径）**，组间一个空行，组内按路径字母序：

```go
import "encoding/json"
import "strings"

import "github.com/user/lib"

import "./utils/helper"
```

- 优先 `import "path"` 全模块导入，通过 `module.name` 使用，保持命名空间显式。
- `from ... import` 仅限导入少量名字，且导入的名字不得与本模块内定义的名字冲突。
- 长名字或路径末段含义不清时用 `as` 别名：`import "encoding/json" as encjson`。
- 循环依赖时在函数内延迟导入（见 [05-modules.md](05-modules.md) §4）。

### 2.2 顶层执行代码

直接运行与被导入共存的脚本，用 `__name__` 守卫保护执行入口（见 [05-modules.md](05-modules.md) §5）：

```go
func doWork() {
    // ...
}

if __name__ == "__main__" {
    doWork()
}
```

纯库文件不写该守卫。

## 3. 格式化

### 3.1 缩进与行宽

- **缩进 4 空格**，禁止 Tab。
- **行宽 120 列**。唯一例外：不可拆分的长字符串字面量（如 URL）。

### 3.2 大括号与块

- 左花括号挂行尾（语言的分号插入规则已强制）；`else` / `except` / `finally` 与前一个 `}` 同行：

```go
if condition {
    doSomething()
} else {
    doOther()
}
```

- **禁止单行块**。即使只有一个语句也必须多行展开：

```go
// 禁止：if n < 2 { return n }
if n < 2 {
    return n
}
```

### 3.3 语句

- 不写显式分号，依赖分号自动插入。
- 一行只写一条语句。
- 空块用 `pass`，不留空。

### 3.4 表达式换行

- 二元运算符换行时**运算符在续行行首**，续行缩进 4 空格：

```go
ok := statusCode == 200
    && contentLength > 0
    && contentLength <= MAX_BODY_SIZE
```

- 函数调用/定义参数一行放不下时，每行一个参数、缩进 4 空格，右括号独占一行回到原缩进：

```go
func createClient(
    host,
    port,
    timeout = 30,
) {
    // ...
}
```

### 3.5 尾逗号

多行排版的字面量、参数表、实参列表，**最后一项强制尾逗号**（语法已支持），增删元素时 diff 最小：

```go
config := {
    "host": "localhost",
    "port": 8080,
}
```

单行排版不加尾逗号。

### 3.6 空白符

- 二元运算符两侧各一个空格；赋值（`:=` `=` `+=` 等）两侧各一个空格。
- 逗号后一个空格，逗号前无空格。
- 调用/下标/属性访问内部不加空格：`f(x)`、`a[i]`、`obj.field`。
- 关键字与条件之间一个空格：`if cond`、`for x in xs`、`while ok`。
- 切片冒号两侧不加空格（与 Python 一致）：`a[1:10]`、`a[::2]`。
- 默认参数 `=` 两侧加空格：`func f(retries = 3)`（与 Python PEP 8 不同，统一赋值视觉）。

### 3.7 空行

- 顶层函数、class 定义之间一个空行。
- import 分组之间一个空行（见 §2.1）。
- class 内方法之间一个空行。
- 函数体内逻辑段落之间可空一行；任何地方不连续使用两个以上空行。

## 4. 命名

| 实体 | 风格 | 示例 |
|---|---|---|
| 文件 / 模块 | 小写蛇形 | `http_client.ms` |
| class | 大驼峰 | `HttpClient` |
| 函数 / 方法 | 小驼峰 | `parseConfig()` `fetchAll()` |
| 局部变量 / 参数 | 小驼峰 | `retryCount` `userName` |
| 模块级常量 | 大写蛇形 | `MAX_RETRIES` `DEFAULT_TIMEOUT` |
| 内部属性 / 内部函数 | `_` 前缀 | `self._cache` `_helper()` |
| 测试函数 | `test` + 大驼峰主题 | `testParseConfigRejectsEmpty()` |

- 命名与标准库一致使用小驼峰（`strings.toUpper` 风格）；不使用蛇形变量名。
- **禁止遮蔽内建函数名**：`len`、`list`、`str`、`type`、`print` 等（内建完整清单见 [01-lexical.md](01-lexical.md) §4）。
- 禁止单字母名字，三个例外：循环下标 `i`/`j`/`k`、异常对象 `e`、坐标 `x`/`y`。
- 布尔变量/函数用 `is`/`has`/`can` 前缀：`isReady`、`hasItems()`。
- 魔术方法按协议名原样使用（`__init__`、`__str__`），不自造双下划线名字。

## 5. 编程实践

### 5.1 声明与赋值

- 变量声明即初始化，优先 `:=`；声明靠近首次使用处。
- 用 `=` 还是 `:=` 表达意图：新名字用 `:=`，更新已有名字用 `=`，不依赖解释器报错才发现拼写错误。
- 解包赋值右侧长度不确定时用星号收集：`first, *rest := xs`。

### 5.2 规模软阈值

以下数值是**重构信号**，不是硬性禁止，超出时需在评审中说明理由：

| 维度 | 阈值 |
|---|---|
| 函数体 | ≤ 60 行 |
| 函数参数 | ≤ 5 个（超出用 dict 参数或拆函数） |
| 嵌套深度 | ≤ 4 层 |
| 单文件 | ≤ 500 行 |

### 5.3 条件与比较

- 隐式真值判断完全允许（真值规则见 [02-types.md](02-types.md) §2）：`if xs`、`if name`、`if not ok` 都是规范写法。
- `==` 用于值比较；`is` 仅用于 `nil` 判断（`if x is nil`）与明确的单例身份判断。
- 浮点不直接用 `==` 比较，用 `math.isClose` 或容差比较。
- 链式比较优先于 `and` 连接：`if 0 <= i < len(xs)`。

### 5.4 异常

- 禁止裸 `except`（不指定异常类型）；捕获具体异常类型。
- `except` 块不得为空；确实忽略异常时用 `pass` 并注释说明理由：

```go
try {
    cache.clear()
} except CacheError {
    pass  // cache is best-effort; stale entries are harmless
}
```

- 异常用于异常情况，不用于常规控制流（迭代结束用 `for ... in`，不捕获 `StopIteration`）。
- `raise` 抛出具体异常类型并附有用消息：`raise ValueError(f"port out of range: {port}")`。

### 5.5 默认参数

默认参数在**定义时只求值一次**（见 [03-syntax.md](03-syntax.md) §4）。禁止可变对象作默认值，用 `nil` 占位再初始化：

```go
// 禁止：func f(items = []) { ... }
func f(items = nil) {
    if items is nil {
        items = []
    }
    // ...
}
```

### 5.6 模块级状态

- `global` 原则禁止：模块级可变状态使测试与并发推理困难。
- 确需可变模块状态时，命名大写蛇形并在声明处注释说明理由与并发安全性。
- 只读配置值是常量，不受此限。

### 5.7 字符串

- 插值一律用 f-string，不用 `+` 拼接。
- 需要宽度/精度/进制/对齐等格式控制时用 `fmt` 模块（分工约定见 [03-syntax.md](03-syntax.md) §9.1）。
- 多行/含大量转义的字符串用反引号原始字符串。

### 5.8 推导式

- 推导式仅用于无副作用的映射/过滤，一行放不下就改写 `for` 循环。
- 不为副作用使用推导式（禁止 `[print(x) for x in xs]`）。
- 嵌套推导式最多两层，更深改写循环。

### 5.9 并发

- channel 由**发送方负责关闭**；关闭语义见 [06-concurrency.md](06-concurrency.md)。
- 消费 channel 优先 `for v in c` 迭代，直到 channel 关闭且排空。
- `select` 无 `default` 分支时在注释中说明阻塞意图。
- `async` 函数返回的句柄必须被 `await` 或显式丢弃并注释理由，不静默泄漏协程。

## 6. 注释与文档

- 注释用英文（对齐 [10-c-style.md](10-c-style.md) §7）；设计文档用中文。
- 注释说明"为什么"，不复述代码。
- 公开函数、class、模块级常量用 `//` 前置注释块：首行一句话摘要（以名字开头、祈使句或陈述句），后接参数语义、返回值、可能抛出的异常：

```go
// parseConfig reads the config file at path and returns a dict.
// Raises IOError if the file is unreadable, ValueError on bad syntax.
func parseConfig(path) {
    // ...
}
```

- class 的文档注释说明职责与关键属性，不逐个罗列方法。
- TODO 格式：`// TODO(owner): 说明`，owner 为责任人标识。
- 已删除的代码直接删除，不注释保留——历史在 git 里。
