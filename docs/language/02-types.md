# 02 类型与值

> mslang 是动态类型语言：变量无类型，值自带类型。所有值在堆上分配，由 GC 管理（见 [08-vm-internals.md](08-vm-internals.md)）。

## 1. 内建类型总表

| 类型 | 类型对象名 | 可变性 | 说明 |
|---|---|---|---|
| 空值 | `NilType` | — | 唯一值 `nil` |
| 布尔 | `bool` | 不可变 | `true` / `false` |
| 整数 | `int` | 不可变 | 任意精度，溢出自动扩展（大整数） |
| 浮点 | `float` | 不可变 | IEEE 754 双精度 |
| 字符串 | `str` | 不可变 | UTF-8，按 Unicode 码点索引 |
| 字节串 | `bytes` | 不可变 | 原始字节序列 |
| 列表 | `list` | 可变 | 动态数组 |
| 元组 | `tuple` | 不可变 | 定长异构序列 |
| 字典 | `dict` | 可变 | 哈希表，保持插入序 |
| 集合 | `set` | 可变 | 哈希集合 |
| 函数 | `function` | — | 含闭包 |
| 类 | `type` | — | `class` 定义产生 |
| 实例 | — | 可变 | class 的实例，类型即其类 |
| 模块 | `module` | — | import 产物 |
| 通道 | `chan` | 可变 | 协程间通信（见 [06-concurrency.md](06-concurrency.md)） |
| 协程句柄 | `coroutine` | — | `async` 函数调用的返回值 |
| 迭代器 | `iterator` | — | 迭代协议产物 |

## 2. 真值规则（truthiness）

以下值为假，其余皆为真：

- `nil`
- `false`
- 数值零（`0`、`0.0`）
- 空字符串 `""`、空字节串 `b""`
- 空容器：`[]`、`()`、`{}`、`set()`

自定义类型可通过 `__bool__` 魔术方法覆盖；未定义 `__bool__` 时回落到 `__len__`（长度为 0 为假）。

## 3. 数字

### 3.1 int

任意精度有符号整数。VM 内部对小整数使用机器字优化存储，运算溢出时自动提升为大整数表示——对脚本作者透明。

### 3.2 float

IEEE 754 双精度。特殊值：`math.inf`、`math.nan`（`nan != nan`，用 `math.isNaN(x)` 判断）。

### 3.3 混合运算与转换

- `int op float` → `float`；`int / int` → `float`；`int div int` → `int`（向下取整除法，`div` 是关键字，见 01-lexical §4）。
- 显式转换：`int("42")`、`int(3.9)`（截断）、`float("3.14")`、`str(42)`、`bool(x)`。
- `float → int` 隐式转换**不存在**，避免精度静默丢失。

## 4. str

- 不可变 UTF-8 序列。索引 `s[i]` 返回第 i 个 **Unicode 码点** 组成的单字符字符串。
- 拼接：`"a" + "b"`；重复：`"ab" * 3`。
- 常用方法（小驼峰）：`s.len() s.toUpper() s.toLower() s.split(sep) s.join(xs) s.contains(sub) s.hasPrefix(p) s.hasSuffix(p) s.replace(old, new) s.trimSpace() s.indexOf(sub) s.format(...)`。
- 编码：`s.toBytes()` → bytes；`bytes.toString()` → str。

## 5. 容器

### 5.1 list

```ms
xs := [1, "two", 3.0]
xs.append(4)
xs.insert(0, "x")
xs.pop()
xs.remove("two")
xs[1:3]            // slice, returns a new list
xs[::2]            // slice with step
len(xs)
```

### 5.2 tuple

```ms
t := (1, "two")    // parentheses + comma
single := (1,)     // a 1-tuple requires a trailing comma
a, b := t          // unpacking
```

### 5.3 dict

```ms
m := {"a": 1, "b": 2}
m["c"] = 3
del m["a"]
"a" in m           // key membership test
m.keys() m.values() m.items() m.get(k, default) m.pop(k) m.update(other)
```

键必须是可哈希类型（不可变类型 + 定义了 `__hash__` 的实例）。dict 保持插入序，迭代顺序即插入顺序。

### 5.4 set

```ms
s := {1, 2, 3}     // unlike a dict literal: no colons
empty := set()     // empty set ({} is an empty dict)
s.add(4)
s.remove(2)
s.contains(1)
s.union(t) s.intersect(t) s.diff(t)
```

## 6. 相等与哈希

- `==` 按值比较，可哈希类型同时定义哈希：`int/float/bool/str/bytes/tuple` 递归按值；`list/dict/set` 不可哈希；实例默认按身份，定义 `__eq__` 时必须同时定义 `__hash__`（否则哈希回落为身份哈希并产生一次警告）。
- `is` 比较对象身份。小整数与短字符串的驻留是实现细节，脚本不得依赖 `is` 判断值相等。

## 7. 迭代协议

```ms
it := iter(xs)     // calls xs.__iter__()
v := next(it)      // calls it.__next__(); raises StopIteration when exhausted
v := next(it, d)   // with a default value
```

`for x in xs` 即上述协议的语法糖。`range(start, stop, step)` 返回惰性序列对象。

## 8. 魔术方法协议（class 定制行为）

| 类别 | 方法 |
|---|---|
| 构造/表示 | `__init__` `__str__` `__repr__` `__bool__` `__int__` `__float__` |
| 容器 | `__len__` `__iter__` `__next__` `__getitem__` `__setitem__` `__delitem__` `__contains__` |
| 比较/哈希 | `__eq__` `__ne__` `__lt__` `__le__` `__gt__` `__ge__` `__hash__` |
| 算术 | `__add__` `__sub__` `__mul__` `__truediv__` `__floordiv__` `__mod__` `__pow__` `__neg__` `__pos__` |
| 位运算 | `__and__` `__or__` `__xor__` `__invert__` `__lshift__` `__rshift__` |
| 调用/上下文 | `__call__` `__enter__` `__exit__` |

二元运算的反射方法（`__radd__` 等）首版不支持，列入路线图。

## 9. 类型判断

```ms
type(x)                    // the type object
isinstance(x, int)         // includes subclasses
isinstance(x, (int, float))
issubclass(Dog, Animal)
```
