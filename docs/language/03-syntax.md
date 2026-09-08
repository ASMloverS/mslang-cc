# 03 语法规范

> mslang 语法：Go 的花括号骨架 + Python 的表达力语法糖。词法元素见 [01-lexical.md](01-lexical.md)。

## 1. 语法记法

采用扩展 BNF：`{ x }` 表示零或多次，`[ x ]` 表示可选，`x | y` 表示选择。终结符用引号标注。

## 2. 顶层结构

```ebnf
sourceFile  = { topLevelStmt }
topLevelStmt = statement | funcDecl | classDecl
```

## 3. 语句

```ebnf
statement   = declStmt | assignStmt | exprStmt | ifStmt | forStmt
            | whileStmt | tryStmt | withStmt | selectStmt
            | importStmt | returnStmt | raiseStmt | delStmt
            | breakStmt | continueStmt | passStmt | globalStmt | block

block       = "{" { statement } "}"

declStmt    = identList ":=" exprList
assignStmt  = targetList ( "=" | augOp ) exprList
augOp       = "+=" | "-=" | "*=" | "/=" | "//=" | "%=" | "**="
            | "&=" | "|=" | "^=" | "<<=" | ">>="

ifStmt      = "if" expr block { "else" "if" expr block } [ "else" block ]

forStmt     = "for" [ forClause | identList "in" expr ] block
forClause   = simpleStmt ";" expr ";" simpleStmt     // Go 风格三段式

whileStmt   = "while" expr block

tryStmt     = "try" block exceptClause { exceptClause } [ "finally" block ]
exceptClause = "except" [ typeExpr [ "as" identifier ] ] block

withStmt    = "with" expr [ "as" identifier ] block

returnStmt  = "return" [ exprList ]
raiseStmt   = "raise" expr [ "from" expr ]
delStmt     = "del" targetList
globalStmt  = "global" identList
breakStmt   = "break"
continueStmt = "continue"
passStmt    = "pass"
```

要点：

- `:=` 声明新变量，至少左侧一个新名字；`=` 赋值给已声明的名字，混用非法。
- 多重赋值与交换：`a, b = b, a`；右侧先整体求值。
- 解包赋值：`a, b := pair`（右侧必须是可迭代对象，长度须匹配；`a, *rest := xs` 星号收集剩余）。
- `if`/`while` 的条件是任意表达式，按真值规则判定；**不允许赋值表达式作为条件**（首版无 `:=` 条件内声明，列入路线图）。

### 3.1 for 循环

两种形态：

```go
for i := 0; i < 10; i++ { ... }     // 三段式（i++/i-- 仅此处合法的语句形式）
for x in xs { ... }                 // 迭代协议
for i, x in enumerate(xs) { ... }
for k, v in m { ... }               // dict 迭代产出 (key, value) 对
```

`for` 无 else 子句（刻意删减）。

### 3.2 with 语句

```go
with open("a.txt") as f {
    data := f.readAll()
}
```

语义：`expr.__enter__()` 的值绑定给 `as` 目标；块退出（含异常）时调用 `__exit__(excType, excValue, traceback)`，其返回真值决定是否吞掉异常。

## 4. 函数

```ebnf
funcDecl    = [ "async" ] "func" identifier "(" [ params ] ")" block
params      = param { "," param } [ "," ]
param       = identifier [ "=" expr ] | "*" identifier | "**" identifier
lambdaExpr  = "lambda" [ identList ] ":" expr
```

```go
func add(a, b) { return a + b }
func greet(name, greeting = "hello") { ... }     // 默认参数
func sum(*nums) { ... }                          // 可变位置参数（tuple）
func config(**opts) { ... }                      // 关键字参数（dict）
func f(a, b = 1, *rest, **kw) { ... }            // 组合，顺序固定如此

add(1, 2)
greet("bob", greeting = "hi")                    // 关键字实参
```

- 函数是一等值，支持闭包（词法作用域，upvalue 捕获）。
- `async func` 定义协程函数，调用语义见 [06-concurrency.md](06-concurrency.md)。
- `lambda` 仅支持单表达式函数体。
- 默认参数在**定义时**求值一次（与 Python 一致，文档中明确警示可变默认参数陷阱）。

## 5. class

```ebnf
classDecl   = "class" identifier [ "<" expr ] block
```

```go
class Animal {
    func __init__(self, name) { self.name = name }
    func speak(self) { return "..." }
    static func create(name) { return Animal(name) }   // 静态方法
}

class Dog < Animal {
    func __init__(self, name) {
        super.__init__(name)
        self.tricks = []
    }
    func speak(self) { return f"{self.name} barks" }
}
```

规则：

- 单继承，`<` 表示"继承自"（刻意避开 `:` 与 Go 风格统一花括号）。
- 方法第一个参数必须是 `self`（关键字，隐式传入实例）。
- `super.method(...)` 显式调用父类方法（非 `super()` 零参形式）。
- `static func` 定义静态方法，无 `self`，通过类名调用。
- 属性在 `__init__` 中通过 `self.x = ...` 动态创建；没有访问控制关键字，约定 `_` 前缀为内部属性。
- 无多继承、无元类（列入路线图候选）。

## 6. 表达式与优先级

从高到低：

| 优先级 | 运算符 | 结合性 |
|---|---|---|
| 1 | 字面量、标识符、`( expr )` | — |
| 2 | 下标 `a[i]`、切片 `a[i:j:k]`、属性 `a.b`、调用 `f(x)`、`await` | 左 |
| 3 | `**` | **右** |
| 4 | 一元 `+x -x ~x not x` | 右 |
| 5 | `* / // %` | 左 |
| 6 | `+ -` | 左 |
| 7 | `<< >>` | 左 |
| 8 | `&` | 左 |
| 9 | `^` | 左 |
| 10 | `|` | 左 |
| 11 | 比较 `== != < <= > >=`、`in`、`not in`、`is`、`is not` | 左（链式） |
| 12 | `and` | 左（短路） |
| 13 | `or` | 左（短路） |
| 14 | 条件表达式 `a if cond else b` | 右 |
| 15 | `lambda` | — |

链式比较：`a < b <= c` 等价于 `(a < b) and (b <= c)`，`b` 只求值一次。

### 6.1 下标与切片

```go
a[i]                 // 负索引从尾部计数
a[start:stop]        // 半开区间
a[start:stop:step]
a[:]                 // 浅拷贝
```

### 6.2 推导式

```go
[x * x for x in xs]
[x for x in xs if x > 0]
[k: v * 2 for k, v in m]            // dict 推导
{x % 10 for x in xs}                // set 推导
```

嵌套循环：`[x + y for x in a for y in b]`。生成器表达式（惰性版本）列入路线图。

### 6.3 f-string

`f"{expr:format}"`，`{` 字面量写作 `{{`。求值时机为运行时，内嵌表达式可调用任意函数。

## 7. import 语句

```ebnf
importStmt  = "import" stringLit [ "as" identifier ]
            | "from" stringLit "import" identList
            | "from" stringLit "import" "(" identList [ "," ] ")"
```

```go
import "fmt"
import "encoding/json"                    // 绑定名 json（路径末段）
import "encoding/json" as encjson
from "strings" import toUpper, split
```

解析规则见 [05-modules.md](05-modules.md)。

## 8. 作用域

- 块级作用域：`{}` 引入新作用域（含 `if`/`for` 块）。
- 函数内赋值默认为局部变量；用 `global x` 声明写模块级变量。
- 闭包对外层局部变量按**引用**捕获（无 `nonlocal`，内层可直接写捕获变量——与 Python 不同，这是刻意的简化）。
- 名字解析顺序：局部 → 闭包外层 → 模块全局 → 内建。

## 9. 内建函数

```
len(x)  cap(c)                 // cap 仅对 channel 有意义
type(x) isinstance(x, T) issubclass(A, B)
str(x) int(x) float(x) bool(x) bytes(x) repr(x)
list(x) tuple(x) dict(x) set(x)
range(start, stop, step) enumerate(x, start=0) zip(a, b, ...)
map(f, x) filter(f, x) sorted(x, key=nil, reverse=false) reversed(x)
abs(x) min(...) max(...) sum(x) round(x, n=0) divmod(a, b) pow(a, b)
chr(i) ord(c) hex(i) oct(i) bin(i)
hasattr(o, n) getattr(o, n, d=nil) setattr(o, n, v) delattr(o, n)
id(x) hash(x) callable(x)
iter(x) next(it, default) 
open(path, mode="r") input(prompt="")
chan(capacity=0)               // 创建 channel，见并发文档
vars() globals() locals() dir(x)
```

### 9.1 print：常规输出

```
print(*values, sep=" ", end="\n", file=nil, flush=false)
```

- `*values`：任意个数、任意类型的值，输出其 `str()` 表示（实例走 `__str__`/`__repr__` 协议）
- `sep`：值间分隔符，默认空格；`end`：结尾字符，默认换行（不换行用 `end=""`）
- `file`：输出目标，需实现 `io.Writer` 协议；`nil` 表示标准输出
- `flush`：是否立即冲刷，默认 `false`；返回 `nil`

```go
print("hello, mslang")              // hello, mslang
print(1, 2, 3)                      // 1 2 3
print("a", "b", sep=", ")           // a, b
print("no newline", end="")
print("err msg", file=os.stderr)
```

分工约定：**常规输出一律用内建 `print`；需要格式控制（宽度、精度、进制、对齐）时才用 `fmt` 模块**（`fmt.printf`/`fmt.sprintf`，见 [07-stdlib.md](07-stdlib.md) §1）。简单插值优先用 f-string 而非 `fmt.sprintf`。
