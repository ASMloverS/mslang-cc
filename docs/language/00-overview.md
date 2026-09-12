# mslang 语言设计总览

> 版本：v0.1 设计草案 | 状态：待评审

## 1. 定位

mslang 是一门**通用脚本语言**，脚本文件以 `.ms` 为后缀。它以 Go 的语法外观、Python 的动态语义与实现方式为骨架，用**纯 C11** 实现一个字节码虚拟机，并提供对标 Python C API 的嵌入/扩展能力。

设计目标（按优先级）：

1. **通用脚本能力**：独立解释器可执行文件，写脚本、处理文本、自动化任务。
2. **嵌入能力一等公民**：C/C++ 程序可以方便地创建解释器、执行脚本、双向互调。
3. **实现简洁可读**：核心零第三方依赖，代码即文档，适合作为语言实现的参考。
4. **Go 风味的并发**：协程 + channel 语义，使用 `async`/`await` 关键字表达。

非目标（首版不做）：

- 静态类型检查、JIT、编译到机器码
- 包管理器、LSP/IDE 工具链
- ABI 稳定承诺（API 随版本演进）

## 2. 核心设计决策一览

| 维度 | 决策 | 参照 |
|---|---|---|
| 类型系统 | 动态类型 | Python |
| 执行模型 | 源码 → 字节码 → 栈式 VM | CPython/Lua |
| 内存管理 | 标记-清除 GC（STW，safepoint） | Lua |
| 错误处理 | `try/except/finally/raise` 异常 | Python |
| 并发 | 协程 + channel，M:N 多线程调度 | Go 语义，`async/await` 关键字 |
| 面向对象 | `class` 单继承 + 魔术方法，显式 `self` | Python |
| 模块系统 | `import` / `from ... import ...`，Go 风格模块路径 | Python 语义 + Go 路径 |
| 语法外观 | 花括号定界、分号自动插入、`:=` 声明 | Go |
| 数字类型 | 任意精度 int + float64 | Python |
| C API | `ms` 前缀 + 小驼峰函数命名 | Python C API 结构 + Java 命名 |
| 标准库命名 | Go 风格模块路径 + 小驼峰函数名 | Go 路径 + Java 命名 |
| 实现语言 | C11 + CMake，Win/Linux/macOS | — |

## 3. 快速上手

hello.ms（顶层语句即程序，无需入口函数）：

```ms
print("hello, mslang")
```

特性速览——以下是一个可直接 `mslang` 运行的完整脚本：

```ms
import "encoding/json"
import "log"
import "net/http"
import "strings"

// Variables and containers
x := 42
name := "world"
xs := [1, 2, 3]
m := {"a": 1, "b": 2}
squares := [v * v for v in xs if v > 1]

// Functions and classes
func fib(n) {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}

class Animal {
    func __init__(self, name) {
        self.name = name
    }
    func speak(self) {
        return f"{self.name} makes a sound"
    }
}

class Dog < Animal {                   // single inheritance
    func speak(self) {
        return f"{self.name} barks"
    }
}

// Exceptions
func parse(text) {
    try {
        return json.loads(text)
    } except ValueError as e {
        log.error(f"bad json: {e}")
        return nil
    }
}

// Concurrency
async func fetch(url) {
    resp := http.get(url)
    return resp.body
}

h1 := fetch("https://example.com/a")   // calling an async func spawns a coroutine and returns a handle
h2 := fetch("https://example.com/b")
body := await h1                       // suspend the current coroutine until h1 completes

c := chan(16)                          // buffered channel
c.send(body)                           // send (yields when the buffer is full)
v := c.recv()                          // receive (yields when the buffer is empty)

d := Dog("Rex")
print(d.speak())                       // Rex barks
print(strings.toUpper(name))           // WORLD
```

## 4. 设计文档索引

| 文档 | 内容 |
|---|---|
| [01-lexical.md](01-lexical.md) | 词法结构：编码、注释、关键字、字面量、分号插入 |
| [02-types.md](02-types.md) | 类型与值语义、真值规则、魔术方法协议 |
| [03-syntax.md](03-syntax.md) | 语法规范（EBNF）、运算符优先级、内建函数 |
| [04-exceptions.md](04-exceptions.md) | 异常模型与内建异常层级 |
| [05-modules.md](05-modules.md) | 模块系统与 import 解析 |
| [06-concurrency.md](06-concurrency.md) | async/await、channel、select、M:N 调度、内存模型 |
| [07-stdlib.md](07-stdlib.md) | 标准库模块 API 草案 |
| [08-vm-internals.md](08-vm-internals.md) | 编译管线、字节码指令集、GC、协程实现 |
| [09-c-api.md](09-c-api.md) | C API 完整规范：对象模型、嵌入、扩展、命名约定 |
| [10-c-style.md](10-c-style.md) | C 编码规范（Google 风格基础 + Java 命名映射） |
| [11-project-layout.md](11-project-layout.md) | 仓库结构、构建、测试策略、路线图 |
| [12-ms-style.md](12-ms-style.md) | MS 编码规范（Google Java 风格指南结构） |

## 5. 设计哲学

1. **显式优于隐式，但不为啰嗦买单**：显式 `self`、显式 `await`；同时保留 `:=`、推导式、f-string 等语法糖。
2. **一种事尽量一种做法**：循环只有 `for`，没有 `do-while`；字符串插值只有 f-string。
3. **并发是语言的一部分**：channel 与协程是内建类型，不是库。
4. **C API 不是事后补丁**：对象模型、GC 根管理、模块注册从第一天起就为嵌入/扩展设计。
