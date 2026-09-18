# 50 标准库：errors

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [23 异常系统](23-exceptions.md) |

## 任务目标

交付纯 .ms 实现的标准库模块 `lib/errors.ms`：一组围绕异常对象的辅助工具——构造（`new`）、包装并抛出（`wrap`）、沿 `__cause__` 链判定类型（`isInstance`）、取显式原因（`unwrap`）。模块不含任何 C 代码，全部能力建立在任务 23（异常系统）提供的 `raise ... from` 显式链化与异常对象属性之上。完成后，脚本可以 `import "errors"` 使用全部四个函数；本任务通过 `tests/ms/` 下的 ms 脚本测试（testing 模块，任务 40 已先于本任务完成）独立验收。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`errors` 为纯脚本模块（`lib/` 目录）。
  - §10 errors：本模块的完整公开 API 清单，共四个函数：

    ```ms
    errors.new(message)                  // constructs a RuntimeError instance
    errors.wrap(err, message)            // wrapping: convenient form of raise X from err
    errors.isInstance(err, type)         // semantic alias of isinstance (walks the __cause__ chain)
    errors.unwrap(err)                   // returns __cause__
    ```

- `docs/language/04-exceptions.md`
  - §2 抛出与链化：`raise X from e` 是设置 `__cause__` 的唯一语言机制；`raise` 的对象必须是 `BaseException` 实例，否则抛 `TypeError`。
  - §3 异常对象属性：`message` / `__cause__` / `__context__` / `traceback`；`str(e)` 返回 `"TypeName: message"`。
  - §4 内建异常层级：`errors.new` 构造的 `RuntimeError` 是 `Exception` 的子类。
- `docs/language/05-modules.md` §2：`lib/` 纯脚本模块在解析顺序第一档「内建/已注册模块」中命中；模块的发现、执行与缓存由任务 24（模块系统）负责，本任务只交付模块源码，不涉及加载机制本身。
- `docs/language/11-project-layout.md` §1：纯脚本标准库模块置于 `lib/`；§4 测试策略：脚本测试位于 `tests/ms/`。
- `docs/language/12-ms-style.md`：§2 源文件结构、§4 命名（禁止遮蔽内建名）、§5.4 异常实践（`raise` 具体类型并附有用消息）、§6 注释与文档（公开函数的前置英文文档注释，代码块围栏用 `ms`）。
- 依赖任务 23（异常系统）的文档尚不存在：本文引用的 `__cause__` 语义、`raise ... from` 行为与异常类名以 `docs/language/04-exceptions.md` 为准；若任务 23 文档的实现细节（如异常属性的可写性）与本文假定不同，实现时以对应任务文档定名为准。

## 详细设计

本任务是纯 .ms 模块，无 C 结构体与 C 函数签名；接口级设计以 ms 函数签名与语义约定表达。ms 代码遵循 `docs/language/12-ms-style.md`（4 空格缩进、行宽 120、小驼峰函数名、公开函数带英文前置文档注释）。

### 文件与模块骨架

- 唯一交付文件：`lib/errors.ms`。文件头注释一句话说明职责；无 import（只依赖内建异常类与 `isinstance`/`str` 等内建函数）；无模块级可变状态；不写 `__name__` 守卫（纯库文件，12-ms-style §2.2）。
- 模块经任务 24 的模块系统以 `"errors"` 名注册/解析，脚本侧用 `import "errors"` 引入，`errors.new` 形式调用（05-modules §1 绑定规则）。

### 公开函数

```ms
// new returns a new RuntimeError instance carrying the given message.
func new(message)

// wrap raises RuntimeError(message) with err as the explicit cause,
// i.e. it is the convenient form of `raise RuntimeError(message) from err`.
// This function never returns normally. err must be a BaseException instance.
func wrap(err, message)

// isInstance reports whether err itself or any exception on its __cause__
// chain is an instance of errType (including subclasses). Only the explicit
// __cause__ chain is walked; __context__ is not consulted.
func isInstance(err, errType)

// unwrap returns the explicit cause (__cause__) of err, or nil if none.
func unwrap(err)
```

语义与边界约定：

- `new(message)`：直接委托内建构造器 `RuntimeError(message)` 并返回实例，不做额外参数校验——`message` 的类型与归一规则由任务 23 的异常构造器决定（04-exceptions §3 规定 `message` 为消息字符串）。`new` 固定构造 `RuntimeError`，与 07-stdlib §10 注释一致；需要其他异常类型时脚本直接调用对应类，不经过本模块。
- `wrap(err, message)`：参数校验 `isinstance(err, BaseException)`，不成立时 `raise TypeError(f"errors.wrap: err must be a BaseException instance, got {str(err)}")`；通过后执行 `raise RuntimeError(message) from err`。该函数**不返回**（恒抛异常），调用点位于 `try/except` 或向上传播路径。
- `unwrap(err)`：参数校验同上（非 `BaseException` 实例抛 `TypeError`）；返回 `err.__cause__`，无显式原因时为 `nil`。
- `isInstance(err, errType)`：
  - `err` 非 `BaseException` 实例时抛 `TypeError`（消息说明实参类型，与 `wrap` 同款格式）。
  - `errType` 不做前置校验：直接交给内建 `isinstance(current, errType)`，非类实参的 `TypeError` 由内建函数抛出，行为与 Python 一致。
  - 判定从 `err` 自身开始（含自身），沿 `__cause__` 逐级向下；任一级 `isinstance(current, errType)` 为真即返回 `true`；链耗尽（`__cause__` 为 `nil`）返回 `false`。
  - **只走 `__cause__` 显式链，不看 `__context__`**：07-stdlib §10 明确标注 "walks the `__cause__` chain"；隐式上下文不体现「包装」意图，刻意排除。

### 链遍历算法与环防护

`isInstance` 的核心流程（接口级伪代码，非完整实现）：

```ms
func isInstance(err, errType) {
    if not isinstance(err, BaseException) {
        raise TypeError(f"errors.isInstance: err must be a BaseException instance, got {str(err)}")
    }
    seen := []
    current := err
    while current != nil {
        if isinstance(current, errType) {
            return true
        }
        for s in seen {
            if current is s {
                return false    // cycle in __cause__ chain
            }
        }
        seen.append(current)
        current = current.__cause__
    }
    return false
}
```

要点：

- 环防护：04-exceptions 未禁止 `raise e from e` 之类的自环链（`__cause__` 指向自身或成环）。遍历时用 `seen` 列表配合身份判定 `is`（12-ms-style §5.3 允许的「明确的单例身份判断」用法）检测重复节点，命中即返回 `false`，保证任何输入都终止。
- `unwrap` 只跳一级，不构成循环，无需防护。
- 命名说明：07-stdlib §10 清单把 `isInstance` 的第二参数写作 `type`，但 `type` 是内建函数名，12-ms-style §4 明确禁止遮蔽内建名；实现中参数定名 `errType`，对外语义与文档签名一致。

### 设计取舍说明

- `wrap` 选择「包装并立即抛出」而非「返回包装后的异常对象」：07-stdlib §10 将 `wrap` 注释为 "convenient form of raise X from err"，且 04-exceptions 中 `raise ... from` 是设置 `__cause__` 的唯一 documented 机制；返回对象形式要求 `__cause__` 属性可写，规范未作此承诺，故取抛出形式。该解读使 `errors.wrap` 等价于 Go 生态 `errors.Wrap` + `return err` 的合并写法。
- 四个函数与 07-stdlib §10 清单一一对应，不增删：`errors.is`（Go 风格按身份比较）之类未列入清单的能力不在本任务范围。

## 实现步骤

1. 建 `lib/errors.ms` 骨架：文件头注释、四个函数的签名与英文文档注释，函数体先 `pass`。验证：`import "errors"` 成功，`errors.new` 等四个名字均可访问（`dir(errors)` 或逐个属性读取）。
2. 实现 `new` 与 `unwrap`（委托构造器 / 读取 `__cause__`，含 `unwrap` 的 `TypeError` 校验）。验证：ms 脚本断言 `errors.new("x")` 的类型、message 与 `str()` 输出，`errors.unwrap` 的正常与报错路径。
3. 实现 `wrap`：`BaseException` 校验 + `raise RuntimeError(message) from err`。验证：`try/except` 捕获后断言类型、message、`__cause__` 身份；非异常实参触发 `TypeError`。
4. 实现 `isInstance`：链遍历 + `seen` 环防护。验证：直接命中、链中多级命中、未命中、子类匹配、自环链终止。
5. 编写完整测试文件（见「测试方案」），经仓库根 `run_tests.py` 调用 mslang CLI 全量运行通过。

## 测试方案

按 README 测试约定：本任务晚于任务 40（testing 模块），测试一律使用 `testing` 与 `testing/assert` 模块。测试文件 `tests/ms/stdlib/errors_test.ms`，测试函数以 `test` 开头，末尾 `testing.run()`；由 `run_tests.py` 递归发现并驱动。链上属性的检查（如捕获后比对 `__cause__`）在测试函数内用 `try/except` 手动完成，类型级断言用 `assert.raises`（07-stdlib §19 既定接口）。

覆盖清单：

- `new`：
  - `errors.new("boom")` 是 `RuntimeError` 实例、同时是 `Exception` 实例；`e.message == "boom"`；`str(e) == "RuntimeError: boom"`（04-exceptions §3）。
  - 空消息：`errors.new("")` 的 `str()` 输出符合任务 23 对无消息异常的约定。
- `wrap`：
  - 基本：`errors.wrap(e0, "context")` 抛出 `RuntimeError`，`message == "context"`，捕获对象的 `__cause__` 与 `e0` 身份相等（`is`）。
  - 链式：`wrap` 的结果再被 `wrap`，形成两级 `__cause__` 链，逐级身份断言。
  - 类型断言：`assert.raises(RuntimeError, lambda: errors.wrap(e0, "x"))`。
  - 负例：`errors.wrap("not an error", "x")` 与 `errors.wrap(nil, "x")` 抛 `TypeError`。
- `isInstance`：
  - 直接命中：`errors.isInstance(ValueError("v"), ValueError)` 为 `true`。
  - 子类命中：`errors.isInstance(ZeroDivisionError("z"), ArithmeticError)` 为 `true`（04-exceptions §4 层级）。
  - 链命中：`A from B from C` 三级链上，对 `C` 的类型、中间级类型、顶层类型分别判定均为 `true`。
  - 未命中：链上无 `KeyError` 时 `errors.isInstance(e, KeyError)` 为 `false`。
  - 只走显式链：在 `except` 块内隐式抛出新异常（`__context__` 挂旧异常、`__cause__` 为 `nil`），断言 `isInstance` 对旧异常类型返回 `false`。
  - 环防护：构造 `raise e from e` 自环，`errors.isInstance(e, KeyError)` 返回 `false` 且调用终止。
  - 负例：第一参数非异常对象抛 `TypeError`。
- `unwrap`：
  - 有因：`errors.wrap` 产生的异常经捕获后 `errors.unwrap(e)` 与原异常身份相等。
  - 无因：普通构造的异常 `errors.unwrap(e) is nil`。
  - 负例：非异常实参抛 `TypeError`。

## 验收标准

- [ ] `lib/errors.ms` 存在，实现且仅实现 07-stdlib §10 的四个函数 `new` / `wrap` / `isInstance` / `unwrap`；无 C 代码，无模块级可变状态。
- [ ] ms 代码符合 12-ms-style：4 空格缩进、行宽 120、不遮蔽内建名（第二参数定名 `errType`）、公开函数带英文前置文档注释、文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- [ ] `errors.new(message)` 返回 `RuntimeError` 实例；`errors.wrap(err, message)` 以 `raise RuntimeError(message) from err` 语义抛出且不返回；`errors.unwrap` 返回 `__cause__` 或 `nil`；三者对非 `BaseException` 实参抛 `TypeError`。
- [ ] `errors.isInstance` 从 `err` 自身起沿 `__cause__` 链逐级做 `isinstance` 判定（含子类匹配），不访问 `__context__`，并对成环链保证终止。
- [ ] `import "errors"` 在解释器中可用，模块经 `lib/` 解析档命中（机制属任务 24，本任务不改动加载器）。
- [ ] `tests/ms/stdlib/errors_test.ms` 覆盖「测试方案」全部清单项，`run_tests.py` 全量通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 23 的接口假定（`__cause__` 语义、异常构造器行为）在实现时已对齐。
