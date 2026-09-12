# 04 异常模型

> mslang 采用 Python 式异常：异常是对象，沿调用栈（含协程边界）传播，`try/except/finally` 捕获。

## 1. 基本语法

```ms
try {
    risky()
} except ValueError as e {
    print("值错误: ", e)
} except (TypeError, KeyError) as e {      // catch multiple types
    print("类型或键错误: ", e)
} except {                                  // catch any Exception subclass
    print("未知错误")
} finally {
    cleanup()                               // always runs
}
```

规则：

- `except` 子句按序匹配，先命中先执行。
- `except TypeName` 匹配该类及其子类的异常。
- 不带类型的 `except` 等价于 `except Exception`（**不捕获** `SystemExit`/`KeyboardInterrupt`）。
- `except` 与 `finally` 至少出现其一；`try/finally`（无 except）合法。
- 无匹配的 `except` 时异常继续向上传播，`finally` 照常执行。

## 2. 抛出与链化

```ms
raise ValueError("bad input")               // calling the class constructs the exception instance
raise ValueError                            // equivalent to raise ValueError()
raise e                                     // re-raise (keeps the original traceback)
raise RuntimeError("wrap") from e           // explicit chaining: __cause__ = e
```

`raise` 的对象必须是 `BaseException` 的实例或子类，否则抛 `TypeError`。

在 `except` 块内抛出新异常而未用 `from` 时，原异常自动挂到新异常的 `__context__` 上（隐式链化）。

## 3. 异常对象

每个异常实例具有以下属性：

| 属性 | 说明 |
|---|---|
| `message` | 构造时传入的消息字符串 |
| `__cause__` | 显式原因（`raise ... from`） |
| `__context__` | 隐式上下文异常 |
| `traceback` | 调用栈帧列表（文件、行号、函数名） |

`str(e)` 返回 `"TypeName: message"`（无消息时为 `"TypeName"`）。

## 4. 内建异常层级

```
BaseException
├── SystemExit                      // exit() 触发，解释器退出
├── KeyboardInterrupt               // Ctrl+C
└── Exception
    ├── ArithmeticError
    │   ├── ZeroDivisionError
    │   └── OverflowError
    ├── AssertionError              // assert / testing 模块
    ├── AttributeError
    ├── ChannelClosedError          // 对已关闭且排空的 channel recv
    ├── EOFError
    ├── ImportError
    ├── LookupError
    │   ├── IndexError
    │   └── KeyError
    ├── NameError
    ├── OSError
    │   ├── FileNotFoundError
    │   ├── PermissionError
    │   └── TimeoutError
    ├── RuntimeError
    │   ├── RecursionError
    │   ├── NotImplementedError
    │   └── CancelledError            // 协程被 h.cancel() 取消时在让出点注入
    ├── StopIteration               // 迭代器耗尽；for 循环内部消化
    ├── TypeError
    └── ValueError
        └── UnicodeError
```

C 扩展模块可注册自定义异常类型（见 [09-c-api.md](09-c-api.md)）。

## 5. 异常与控制流的交互

- `return`/`break`/`continue` 在 `try` 块内执行时，`finally` 先运行；若 `finally` 自身抛出异常或 `return`，覆盖原控制流。
- `StopIteration` 在 `for` 循环、推导式中被自动捕获并终止迭代；显式 `next()` 调用需要调用者自行处理。
- 协程中未捕获的异常不崩溃解释器：存于协程句柄，`await` 该句柄时**重新抛出**（见 [06-concurrency.md](06-concurrency.md)）。
- 顶层未捕获异常：打印 `TypeName: message` + traceback 到 stderr，解释器以非零码退出。

## 6. 性能约定

异常仅用于异常路径。VM 实现上采用**零成本 try**：`try` 块的进入/退出不付出运行时开销（异常表查表法），代价集中在实际抛出时刻（捕获当前协程的调用栈快照）。
