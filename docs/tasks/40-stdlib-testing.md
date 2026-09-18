# 40 标准库：testing 与 testing/assert

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [34 mslang test 子命令](34-cli-test-command.md) |

## 任务目标

交付纯 .ms 实现的标准库测试包 `lib/testing/`：`testing` 模块（`lib/testing/__init__.ms`，测试发现与运行器 `testing.run()`）与 `testing/assert` 子模块（`lib/testing/assert.ms`，断言函数集），完整落地 `docs/language/07-stdlib.md` §19 的既定用法——测试文件 `xxx_test.ms`、测试函数 `test` 前缀、`testing.run()` 发现并运行当前模块的 `test*` 函数。纯 .ms 的实现需要一个脚本层内省原语来取得调用方模块的命名空间，因此本任务附带一个 C 侧增量：内建函数 `globals()`（任务 10 排期项的前移，见「设计依据」）。

本任务与任务 34 的 `mslang test` 子命令是同一测试文件的两种驱动：本任务在「详细设计」第 5 节钉定两者的协作协议，保证对同一测试文件给出一致的通过/失败判定。**里程碑约定**：本任务完成后，`tests/ms/` 下所有新增 ms 测试统一改用 testing 模块编写（既有内建 `assert` + `print` 风格的测试不强制回填），写法样板见第 6 节。本任务以自举方式验收——测试 testing 模块自身的测试文件即按 testing 风格编写。

## 设计依据

- [07-stdlib.md](../language/07-stdlib.md)
  - §0 模块清单：`testing` 为纯脚本模块（`lib/` 目录）。
  - §19 testing：本任务的范围唯一来源——文件命名 `xxx_test.ms`、函数名 `test` 前缀、`testing.run()` 发现当前模块 `test*` 函数、`testing/assert` 的 `assert.equal` / `assert.isTrue` / `assert.raises` 三个既定接口、`mslang test ./...` 汇总报告。
- [04-exceptions.md](../language/04-exceptions.md)
  - §1：不带类型的 `except` 等价于 `except Exception`，**不捕获** `SystemExit`/`KeyboardInterrupt`——运行器逐测试受保护执行的语义基础。
  - §3：异常对象属性 `message` / `traceback`；`str(e)` 为 `"TypeName: message"`——失败消息的构成依据。
  - §4 异常层级：`AssertionError` 的注释即 "assert / testing 模块"——内建 `assert` 与 `testing/assert` 共用同一失败异常类型，是两种驱动判定一致的关键。
  - §5：顶层未捕获异常以非零码退出——`testing.run()` 失败时以抛出结束顶层的退出码依据（任务 09 的退出码表：运行时错误即退出码 1）。
- [05-modules.md](../language/05-modules.md)：§1 `import "testing/assert"` 绑定末段名 `assert`；§2/§3 包与子模块解析（`lib/testing/__init__.ms` 与 `lib/testing/assert.ms` 的布局依据）；§5 `__name__` 主模块判定与守卫惯用法（协作协议的基石）。
- [03-syntax.md](../language/03-syntax.md) §8：名字解析顺序「局部 → 闭包外层 → 模块全局 → 内建」——`import "testing/assert"` 在测试文件内绑定的 `assert` 遮蔽内建 `assert`，属规范行为。[01-lexical.md](../language/01-lexical.md) §4 的内建名清单**不含** `assert`（它是任务 09/10 挂接的临时内建），故该遮蔽不违反 [12-ms-style.md](../language/12-ms-style.md) §4 的内建名遮蔽禁令；§19 规范示例本身即此用法。
- [03-syntax.md](../language/03-syntax.md) §9 与 [01-lexical.md](../language/01-lexical.md) §4：`globals`、`repr`、`dir` 均在内建函数清单中。[10-builtin-functions.md](10-builtin-functions.md) 把 `globals`/`dir`/`callable` 等排期为「后续版本」；**本任务将 `globals()` 一个内建前移至 v0.2 随本任务交付**（`testing.run()` 的发现协议依赖它，见第 2 节），`dir`/`callable`/`locals`/`vars` 不在本任务范围。`repr` 由任务 25（class 继承与魔术方法）交付，接口名以 03-syntax §9 为准。
- [10-builtin-functions.md](10-builtin-functions.md)：内建注册机制 `msBuiltinTable` / `msBuiltinRegisterAll`（`src/vm/ms_builtin.c`），本任务的 `globals()` 以追加一行表项 + 一个文件内 `static` 函数接入，无新头文件。
- [24-modules-import.md](24-modules-import.md)：模块对象 `struct MsModule` 的 `attrs` dict 即模块命名空间（`globals()` 的返回物）；VM 帧记录所属模块指针（假定字段名 `module`）；`__main__` 不入注册表。
- [09-c-api.md](../language/09-c-api.md) §9：`MsCFunction` 签名 `MsObject* (*)(MsState* L, int64_t argc, MsObject** argv)`。
- [34-cli-test-command.md](34-cli-test-command.md)：测试发现（`test` 前缀、值为函数、必选参数数为 0、名字字典序、`-run` 子串过滤）、逐函数受保护调用、失败后继续、文件级隔离、报告与退出码——本任务的协作协议逐条对齐其已定稿语义；其「任务 40 落地后本执行器无需改动即可兼容」条款是本协议的设计约束。
- [12-ms-style.md](../language/12-ms-style.md)：全部 .ms 代码遵循其规范（4 空格缩进、行宽 120、小驼峰函数名、内部名字 `_` 前缀、公开函数带英文前置文档注释、§2.2 `__name__` 守卫、§5.6 原则上无模块级可变状态）。
- [10-c-style.md](../language/10-c-style.md)：C 增量遵循其规范（2 空格缩进、120 列、K&R、星号贴类型、`ms`/`Ms`/`MS_` 命名）。
- [README.md](README.md)「测试约定」：任务 40 完成后新测试统一使用 testing 模块；`run_tests.py` 逐文件直调 `mslang <file>` 并按退出码与输出判定（任务 09 建立、任务 22 扩展 `.out`/`.exit`/`.stdin`/`.cliargs`/`.env` 同伴文件）。
- 假定命名声明：任务 08（VM 执行核心）的协程帧栈字段名、任务 24 的帧 `module` 字段名、任务 25 交付的 `repr` 行为细节、任务 13 的关键字参数传递到 C 函数的约定，均以对应任务文档定名为准。

## 详细设计

本任务的主体是纯 .ms 模块，接口级设计以 ms 函数签名与语义约定表达；唯一的 C 增量是 `globals()` 内建（第 2 节）。

### 1. 交付物与文件布局

```
lib/testing/
├── __init__.ms          # testing 包入口：run()
└── assert.ms            # testing/assert 子模块：断言函数集
```

- 解析（任务 24 / 05-modules §2/§3）：`import "testing"` 命中 `lib/testing/__init__.ms` 并绑定 `testing`；`import "testing/assert"` 优先匹配子模块文件 `lib/testing/assert.ms` 并绑定 `assert`。
- **零 import 纪律**：两个模块不 import 任何标准库模块（含 `strings`、`sort`），只依赖内建函数——测试基础设施被全仓测试依赖，必须保持最小自举面，避免对生态模块的传递依赖。前缀判断、子串匹配、名字排序均以本地辅助函数实现。
- 两个模块均无模块级可变状态（12-ms-style §5.6）：`run()` 的计数与失败记录全部是局部变量；模块级只有内部常量与函数。
- C 增量落点：`src/vm/ms_builtin.c`（任务 10 既有文件）的 `msBuiltinTable` 追加一行；无新头文件、无新 include guard。

### 2. 内建 `globals()`（C 侧唯一增量）

`testing.run()` 须发现**调用方模块**的 `test*` 函数。mslang 的命名空间按模块隔离（任务 24：模块顶层环境即 `MsModule.attrs`），纯 .ms 函数无法取得调用方的模块命名空间，故提供调用帧感知的内建：

```c
// Implements globals(depth = 0): returns the namespace dict (MsModule.attrs)
// of the module owning the script frame `depth` levels outward from the frame
// that invoked the builtin; 0 is the invoking frame itself. C-function frames
// occupy no script frame (task 13 calling convention), so the topmost script
// frame is always the invoker. Returns nil when the call chain is shorter
// than depth. Raises TypeError when depth is not an int, ValueError when
// depth < 0.
static MsObject* msBuiltinGlobals(MsState* L, int64_t argc, MsObject** argv);
```

- 注册：`msBuiltinTable` 追加 `{"globals", msBuiltinGlobals}`；`msBuiltinRegisterAll` 流程不变（任务 10）。
- 帧遍历：沿当前协程的脚本帧栈自顶向下数 `depth` 帧，取该帧 `module` 字段（任务 24 假定名）所指模块的 `attrs` dict，作为既有对象返回——**无堆分配**，内存纪律自然满足。模块对象经注册表或主模块持有（GC 根可达），返回引用生命周期安全。
- 返回的是**真实**命名空间 dict（可写，与 Python `globals()` 语义一致），脚本可经它增删改模块级名字；`testing.run()` 只读。
- REPL 与 `-e` 片段同样在某模块命名空间内执行（任务 24），`globals()` 语义一致；`depth` 超出调用链深度返回 `nil`，由调用方决定如何处理（`run()` 见第 4 节）。
- 关键字参数形式 `globals(depth=1)` 的支持依赖任务 13 的 `CALL_KW` 到 C 函数的传递约定（假定名，实现时对齐）；位置参数形式不依赖该约定。

### 3. `lib/testing/assert.ms`：断言库

全部断言失败以 `raise AssertionError(<消息>)` 表达（04-exceptions §4 把 `AssertionError` 明确划归「assert / testing 模块」），与内建 `assert` 的失败类型一致，两种驱动因此无法区分也不必区分失败来源。`msg` 参数统一约定：非 `nil` 时作为自定义前缀，失败消息为 `f"{msg}: {默认消息}"`。值在消息中的表示一律用内建 `repr`（字符串带引号、容器 repr 风格，便于区分 `1` 与 `"1"`）。

```ms
// equal raises AssertionError unless actual == expected (value comparison).
// Message: "assert.equal failed: got {repr(actual)}, want {repr(expected)}".
func equal(actual, expected, msg = nil)

// notEqual raises AssertionError when actual == expected.
// Message: "assert.notEqual failed: both sides are {repr(actual)}".
func notEqual(actual, expected, msg = nil)

// isTrue raises AssertionError unless value is truthy (02-types §2 truth
// rules, same as an `if` condition). For a strict bool check use
// equal(value, true). Message: "assert.isTrue failed: got {repr(value)}".
func isTrue(value, msg = nil)

// isFalse raises AssertionError unless value is falsy.
// Message: "assert.isFalse failed: got {repr(value)}".
func isFalse(value, msg = nil)

// isNil raises AssertionError unless value is nil (identity check).
// Message: "assert.isNil failed: got {repr(value)}".
func isNil(value, msg = nil)

// isNotNil raises AssertionError when value is nil.
// Message: "assert.isNotNil failed".
func isNotNil(value, msg = nil)

// raises calls f (a zero-argument function) and expects it to raise an
// exception matching excType (isinstance semantics, subclasses included).
// Returns the caught exception object for further assertions (e.g. on its
// message). Raises AssertionError when f returns normally ("... not raised")
// or raises a non-matching exception; SystemExit and KeyboardInterrupt are
// never caught (04-exceptions §1). Raises TypeError when f is not a function.
// Message prefix: "assert.raises failed: ..."; the rendering of exception
// and class objects in the message follows str().
func raises(excType, f, msg = nil)

// fail unconditionally raises AssertionError (unreachable-code and
// explicit-failure use). Message: "assert.fail" or the given msg.
func fail(msg = nil)
```

语义与边界约定：

- 函数集以 §19 的三个既定接口（`equal`/`isTrue`/`raises`）为基线，补全 `notEqual`/`isFalse`/`isNil`/`isNotNil`/`fail` 五个对偶与兜底形式——它们不改变任何既定接口的语义，属同一断言族的自然补全。浮点近似断言（`almostEqual`）**不提供**：需要容差比较的测试在函数体内自行容差计算后用 `isTrue` 断言（与 12-ms-style §5.3 的浮点比较约定一致）。
- `equal`/`notEqual` 用 `==` 值比较（02-types 的值语义）；身份比较需求用 `isTrue(a is b)` 表达，不单设接口。
- `raises` 的参数校验：`f` 须为函数类型（`type(f) is _FUNC_TYPE`，见第 4 节的类型判定约定），否则抛 `TypeError`（非 `AssertionError`——用法错误区别于测试失败）；`excType` 不单独校验，直接交由内建 `isinstance`（其对非类实参的行为以任务 15/25 定稿为准）。
- `raises` 返回捕获到的异常对象：测试可继续断言 `e.message` 内容。消息文本断言只校验包含关系与前缀，不断言类对象的 `str()` 具体渲染。
- 模块骨架：文件头注释一句话说明职责；内部常量 `_FUNC_TYPE` 与内部函数 `_fail(defaultMsg, msg)`（统一消息前缀拼接与抛出）以 `_` 前缀命名（12-ms-style §4）；不写 `__name__` 守卫（纯库文件，§2.2）。

### 4. `lib/testing/__init__.ms`：测试发现与运行器

```ms
// run discovers the test* functions in ns and executes them one by one in
// name order. ns defaults to the caller module's own namespace (globals(1)),
// i.e. the plain `testing.run()` call at a test file's top level discovers
// that file's tests. nameFilter, when not nil, skips functions whose name
// does not contain it as a substring (skipped functions are not counted),
// matching the -run semantics of `mslang test` (task 34).
//
// Each test function runs protected: a normal return counts as passed, any
// uncaught exception counts as failed and its str(e) is recorded; execution
// continues with the remaining functions. Failures are reported as
// "--- FAIL: <name>" lines followed by the exception text, and a summary
// line "testing: <passed> passed, <failed> failed" is always printed.
//
// Returns a summary dict {"passed": int, "failed": int, "failures":
// [{"name": str, "message": str}, ...]}. When failed > 0, run raises
// AssertionError after printing the report, so a directly executed test
// file exits with a non-zero status. Raises RuntimeError when the caller
// namespace cannot be determined.
func run(ns = nil, nameFilter = nil)
```

收集与执行流程（接口级伪代码）：

```ms
func run(ns = nil, nameFilter = nil) {
    if ns is nil {
        ns = globals(1)                 // the frame that called run()
    }
    if ns is nil {
        raise RuntimeError("testing.run: cannot determine caller namespace")
    }
    names := []
    for name in ns {                    // dict iteration yields keys
        value := ns[name]
        if _hasTestPrefix(name) && type(value) is _FUNC_TYPE {
            if nameFilter is nil || _containsSub(name, nameFilter) {
                names.append(name)
            }
        }
    }
    _sortStrings(names)                 // insertion sort; test counts are small
    passed := 0
    failures := []
    for name in names {
        try {
            ns[name]()
            passed += 1
        } except e {                    // catches Exception, not SystemExit
            failures.append({"name": name, "message": str(e)})
            print(f"--- FAIL: {name}")
            print(f"    {str(e)}")
        }
    }
    failed := len(failures)
    print(f"testing: {passed} passed, {failed} failed")
    if failed > 0 {
        raise AssertionError(f"testing.run: {failed} of {passed + failed} failed")
    }
    return {"passed": passed, "failed": failed, "failures": failures}
}
```

要点：

- **`globals(1)` 的帧语义**：`run` 是 .ms 函数，其体内的 `globals(0)` 得到的是 `testing` 模块自身的命名空间；`globals(1)` 沿调用链向外一层，即调用 `run()` 的帧——测试文件顶层——所属模块的命名空间。规范示例的裸调用 `testing.run()` 由此成立。显式传 `ns`（任意 dict，含其他模块的 `attrs` 或合成命名空间）时完全不经 `globals()`，这也是运行器自身自举测试的入口。
- **收集规则对齐任务 34**：名字 `test` 前缀（`_hasTestPrefix`：`len(name) >= 4 && name[:4] == "test"`）、值为函数、按名字字典序执行（字符串按码位比较）、子串过滤跳过不计数。**已知差异（显式承认）**：任务 34 的 C 侧还检查「必选参数数为 0」并跳过带参函数；脚本层无参数数目内省能力，`run()` 不查此项——带必选参数的 `test*` 函数会在被调用时抛 `TypeError` 而计为**失败**而非跳过。约定测试函数一律零参（§19 示例与全部既定用法均如此）；带参 `test*` 函数属用法错误，`run()` 的计失败行为反而能暴露它。
- **类型判定约定**：模块级内部常量 `_FUNC_TYPE := type(_noop)`（`_noop` 为模块内空函数），以 `type(value) is _FUNC_TYPE` 判定函数类型——假定 `type()` 返回的类型对象按类型唯一（单例身份）；`MAKE_FUNCTION`/`MAKE_LAMBDA` 产物同为函数类型（任务 13），故 lambda 与 `func` 定义均被收集。若任务 06/25 对类型对象身份另有定稿，实现时以其为准。class（可调用但非函数类型）与任务 34「值为函数」的口径一致地不被收集。
- **受保护执行**：`except e` 不带类型，等价于 `except Exception`（04-exceptions §1），`SystemExit`/`KeyboardInterrupt` 不被吞没——测试内 `os.exit`（任务 36 后）仍会终止进程，与任务 34 的受保护调用口径一致。失败后继续执行后续函数，单函数失败不中断本文件。
- **输出确定性**：不打印耗时（`time` 模块不在本任务依赖链上），逐失败行与汇总行的格式固定，可直接被 `run_tests.py` 的 `.out` 同伴文件比对。成功路径只有一行汇总。
- **失败退出契约**：`failed > 0` 时打印报告后 `raise AssertionError(...)`；顶层不捕获即未捕获异常 → 进程退出码 1（04-exceptions §5、任务 09 退出码表），`run_tests.py` 据此判失败。返回值使 `run()` 也可被编程式复用（自举测试即捕获异常后断言 summary 不可得、或直接用全通过命名空间断言返回 dict）。
- **无状态**：模块不保存任何跨调用状态；同一命名空间重复调用 `run()` 即重复执行，无幂等标记。

### 5. 与 `mslang test` 子命令的协作协议

任务 34 已定稿「每文件独立 `MsState` → `msEvalFile` 执行顶层 → 枚举模块全局表中的 `test*` 函数 → 逐个受保护调用 → 汇总报告」的执行器，并声明测试函数体内 `testing/assert` 的失败以异常形式冒泡即可兼容、执行器无需改动。本任务在其上钉定以下协议（只钉定任务 34 未规定的点，不改变其发现、报告与退出码语义）：

1. **被测文件不以 `__main__` 身份执行**：`mslang test` 执行测试文件时，其 `__name__` 为模块名而非 `"__main__"`（任务 34 未规定被测文件的 `__name__`，此处补钉；实现时以其 `msEvalFile` 路径落任务 24 的模块语义为准）。这是 `__name__` 守卫能区分两种驱动的前提。
2. **守卫形式收尾**：仓库内测试文件一律以守卫形式调用运行器：

   ```ms
   if __name__ == "__main__" {
       testing.run()
   }
   ```

   直接执行（`mslang xxx_test.ms`、`run_tests.py` 逐文件模式）时 `__name__ == "__main__"`，`run()` 发现并执行全部测试，失败时抛出 → 退出码 1；`mslang test` 下守卫为假，`run()` 不执行，由 C 执行器枚举驱动——**不重复执行、不产生双份输出**。
3. **裸调用行为（显式说明，非推荐）**：无守卫的裸 `testing.run()` 在 `mslang test` 下会在顶层先执行一遍全部测试——全过时 C 执行器随后再执行一遍（重复副作用），有失败时 `run()` 的抛出使整文件归类为 ERROR 而非逐函数 FAIL。仓库内测试文件禁止裸调用（第 6 节约定）；裸形式仅保留给不经 `mslang test` 驱动的独立脚本。
4. **判定语义一致**：两种驱动共用同一失败定义（测试函数抛出未被自身捕获的异常即失败，`AssertionError` 与任意其他异常同口径）与同一收集口径（`test` 前缀、函数类型、字典序、子串过滤跳过不计数；「零必选参数」差异见第 4 节），对同一良构测试文件的通过/失败判定必须一致——此一致性列入验收。
5. **失败消息口径**：`testing/assert` 的失败消息含期望/实际值的 `repr` 表示；`mslang test` 的 FAIL 详情与 `testing.run()` 的 `--- FAIL` 行都来自同一异常的 `str(e)`，两种驱动下可读信息一致。

### 6. 里程碑约定：新测试统一用 testing 模块

本任务完成后，`tests/ms/` 下**新增** ms 测试统一采用 testing 风格（README「测试约定」的落地写法）：

```ms
import "testing"
import "testing/assert"

func testSomething() {
    assert.equal(1 + 1, 2)
    assert.raises(ValueError, lambda: strconv.parseInt("abc"))
}

if __name__ == "__main__" {
    testing.run()
}
```

要点：文件名 `xxx_test.ms`；测试函数 `test` 前缀、零参数；断言一律经 `testing/assert`；守卫形式 `testing.run()` 收尾。既有内建 `assert` + `print` 风格测试不强制回填；`run_tests.py` 同时驱动两种风格（它递归发现 `tests/ms/**/*.ms` 逐文件直调，`testing` 风格文件经守卫在直接执行下自运行），`mslang test` 只收录 `*_test.ms`。

## 实现步骤

1. C 增量 `globals()`：`src/vm/ms_builtin.c` 实现 `msBuiltinGlobals`（参数校验、帧栈遍历、越界返回 `nil`）并在 `msBuiltinTable` 注册。验证：临时脚本以内建 `assert` 断言——顶层 `globals()` 含已知顶层名字、函数内 `globals(0)` 与顶层同一对象、经一层辅助函数后 `globals(1)` 回到调用方、深度越界为 `nil`、非 int 深度抛 `TypeError`、负深度抛 `ValueError`（最终测试文件在第 4 步以 testing 风格入库）。
2. `lib/testing/assert.ms`：骨架（文件头注释、`_FUNC_TYPE`、`_fail`）+ 八个公开函数。验证：临时驱动脚本（内建 `assert` + `try/except`）逐函数验证通过路径不抛、失败路径抛 `AssertionError` 且消息格式符合约定、`raises` 的返回异常对象与 `TypeError` 校验。
3. `lib/testing/__init__.ms`：`_hasTestPrefix`/`_containsSub`/`_sortStrings` 内部辅助 + `run()`。验证：临时驱动脚本用合成命名空间 dict（显式传 `ns`）断言——前缀与函数类型过滤、字典序执行（经副作用顺序）、`nameFilter` 跳过不计数、失败后继续、summary dict 内容、`failed > 0` 时打印报告后抛 `AssertionError`。
4. 自举测试入库：`tests/ms/stdlib/testing_assert_test.ms`、`tests/ms/stdlib/testing_run_test.ms`、`tests/ms/builtin/globals_test.ms` 全部以 testing 风格编写（自身即第 6 节样板的实例）。验证：`python run_tests.py` 全绿；`mslang test ./tests/ms/...` 收录并判定一致。
5. 协作协议夹具与 ctest：`tests/fixtures/testing/` 两个夹具（见「测试方案」），ctest 断言双驱动判定一致与守卫抑制。验证：`ctest --test-dir build` 全绿；`mslang test ./tests/ms/...` 自举冒烟退出码 0；构建产物只落在 `build/`。

## 测试方案

本任务晚于任务 09，一律用 ms 脚本测试；且本任务即任务 40，测试**自举**——测试 testing 模块的测试文件本身用 testing 模块编写（`test*` 函数 + `testing/assert` 断言 + 守卫 `testing.run()` 收尾，由 `run_tests.py` 与 `mslang test` 双驱动）。自举的自掩蔽风险（`run()` 的 bug 可能掩盖被测 `run()` 的失败）以两点缓解：断言库的自测试不经过 `run()` 的收集逻辑、直接用 `assert.raises` 捕获比对；实现步骤 1–3 先用内建 `assert` 的临时脚本独立验证后再切换自举。本任务只交付本设计文档，测试与夹具文件随实现编写。

测试文件清单与覆盖点：

- `tests/ms/builtin/globals_test.ms`（`globals()` 内建）：
  - 顶层 `globals()` 返回的 dict 含本文件已定义的顶层名字，且 `globals()["x"] is x` 身份成立（返回真实命名空间而非副本）。
  - 函数内 `globals(0)` 与顶层 `globals()` 是同一对象（函数与顶层同属本模块）。
  - 经一层辅助函数调用 `globals(1)` 得到调用方命名空间；两层嵌套 `globals(2)` 同理。
  - `depth` 超出调用链深度返回 `nil`；`globals("1")` 抛 `TypeError`、`globals(-1)` 抛 `ValueError`（`assert.raises` 断言）。
  - 经 `globals()` 写入新名字后顶层可读取（可写语义，写入名字用 `_` 前缀避免被 `test` 前缀收集）。
- `tests/ms/stdlib/testing_assert_test.ms`（断言库）：
  - 通过路径：八个函数各至少一个正例（含 `equal` 的容器值比较、`raises` 命中并返回异常对象、`raises` 子类匹配——`assert.raises(ArithmeticError, lambda: 1 / 0)` 命中 `ZeroDivisionError`）。
  - 失败路径：每个函数的失败形式经 `assert.raises(AssertionError, lambda: ...)` 断言抛 `AssertionError`，并捕获异常对象断言 `message` 含默认前缀（如 `"assert.equal failed"`）与双方 `repr` 值、含自定义 `msg` 前缀。
  - `raises` 专项：`f` 正常返回（未抛）→ `AssertionError` 且消息含 "not raised"；抛非匹配类型 → `AssertionError`；`f` 非函数（如 `42`）→ `TypeError`；`assert.raises(AssertionError, lambda: assert.fail())` 与 `assert.fail("boom")` 的消息。
  - 真值语义：`isTrue(1)`/`isTrue([0])` 通过、`isTrue(0)`/`isTrue("")` 失败、`isFalse(nil)` 通过（02-types §2 真值表抽查）。
- `tests/ms/stdlib/testing_run_test.ms`（运行器，显式 `ns` 合成命名空间为主）：
  - 收集：合成 dict 含 `testB`/`testA`/`helper`（非前缀）/ `testData`（值为非函数）——`run(ns)` 只执行 `testA`、`testB` 且按字典序（经追加副作用列表断言顺序）；非函数与非前缀名字被跳过。
  - 过滤：`nameFilter` 子串只运行匹配函数，被跳过者不计入 `passed`/`failed`（与任务 34 `-run` 口径一致）。
  - 执行与报告：全过命名空间 → 返回 `{"passed": 2, "failed": 0, "failures": []}` 且不抛；含一个失败函数（`raise ValueError("x")`）→ 失败后继续执行后续函数（副作用断言）、`failed == 1`、`failures[0]["name"]`/`["message"]` 正确，且整体抛 `AssertionError`（`assert.raises` 捕获）。
  - 空命名空间（无 `test*` 函数）→ `0 passed, 0 failed`，不抛（与任务 34「无 test* 函数记 ok」一致）。
  - 本文件自身的守卫 `testing.run()` 即 `ns` 缺省（`globals(1)`）路径的活例：直接执行时它收集并运行本文件全部 `test*` 函数。
- `tests/fixtures/testing/`（协作协议夹具，经 ctest 驱动，对齐任务 34 的双层测试结构）：
  - `pass_widget_test.ms`：testing 风格、两个 `test*` 函数全过、守卫收尾。直接 `mslang` 运行：退出码 0、stdout 含汇总行 `testing: 2 passed, 0 failed`；`mslang test <file>`：`ok` 行且退出码 0，stdout 不含 `testing:` 汇总行（守卫抑制了顶层 `run()`，以 ctest `FAIL_REGULAR_EXPRESSION` 断言——即无重复执行）。
  - `fail_widget_test.ms`：一个 `assert.equal` 失败 + 一个通过函数。直接 `mslang` 运行：退出码 1、stdout 含 `--- FAIL:` 行；`mslang test <file>`：归类 `FAIL`（非 `ERROR`）、失败详情含函数名、退出码 1——两种驱动判定一致。
- 自举冒烟：`mslang test ./tests/ms/...` 对全量脚本测试（含本任务三个 testing 风格测试文件）退出码 0，与 `python run_tests.py` 的逐文件判定一致。

## 验收标准

- [ ] `lib/testing/__init__.ms` 与 `lib/testing/assert.ms` 存在，ms 代码符合 12-ms-style（4 空格缩进、行宽 120、小驼峰、内部名字 `_` 前缀、公开函数英文前置文档注释、无模块级可变状态、纯库文件不写 `__name__` 守卫）；文件 UTF-8 无 BOM、LF 行尾、无行尾空白。
- [ ] 两个模块零 import（只用内建函数）；`import "testing"` 与 `import "testing/assert"` 经 `lib/` 解析档命中，后者绑定名 `assert` 可用（机制属任务 24，本任务不改动加载器）。
- [ ] `src/vm/ms_builtin.c` 新增 `globals(depth=0)` 内建并注册入 `msBuiltinTable`：调用方帧 `depth` 层外模块的命名空间 dict、C 函数不占脚本帧、越界返回 `nil`、非 int 抛 `TypeError`、负值抛 `ValueError`；无堆分配；C 代码通过 10-c-style 检查。
- [ ] `testing/assert` 实现且仅实现「详细设计」第 3 节的八个函数；失败一律抛 `AssertionError`，消息前缀与 `msg` 拼接规则、值经 `repr` 表示、真值语义、`raises` 的返回异常对象/`TypeError` 校验均与约定一致。
- [ ] `testing.run(ns = nil, nameFilter = nil)` 实现收集（`test` 前缀 + 函数类型 + 字典序 + 子串过滤不计数）、逐函数受保护执行、失败后继续、`--- FAIL` 与汇总行报告、summary dict 返回、`failed > 0` 时打印后抛 `AssertionError`；`ns` 缺省经 `globals(1)` 取调用方命名空间，无法确定时抛 `RuntimeError`。
- [ ] 协作协议落实：`mslang test` 下被测文件 `__name__` 非 `"__main__"`；守卫形式文件在两种驱动下不重复执行、判定一致（夹具 + ctest 断言，含 `FAIL` 不误归 `ERROR`、无 `testing:` 双份输出）。
- [ ] `tests/ms/stdlib/testing_assert_test.ms`、`tests/ms/stdlib/testing_run_test.ms`、`tests/ms/builtin/globals_test.ms` 覆盖「测试方案」全部清单项且自身为 testing 风格（自举）；`python run_tests.py` 与 `ctest --test-dir build` 全数通过；`mslang test ./tests/ms/...` 自举冒烟退出码 0。
- [ ] 三平台（Windows/Linux/macOS）Debug/Release 构建通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；假定命名（任务 08 帧栈字段、任务 24 帧 `module` 字段、`repr`、C 函数关键字参数约定）在实现时已按对应任务文档对齐。
