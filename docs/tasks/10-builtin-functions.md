# 10 内建函数

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [09 最小可运行解释器](09-minimal-interpreter.md) |

## 任务目标

交付 mslang 的内建函数模块（`src/vm/ms_builtin.h` / `src/vm/ms_builtin.c`）：在任务 09 挂接的临时内建（`print`/`assert`）基础上，建立正式的内建函数注册机制，并落地 v0.1 必需的内建函数全集——`print`（完整签名）、`assert`、`len`、`type`、`int`、`float`、`str`、`bool`、`range`、`abs`、`min`、`max`。完成后：

- `msNewState` 一次性注册全部 v0.1 内建，脚本侧经 `MS_OP_LOAD_GLOBAL` 直接调用；
- `print` 支持 `sep`/`end`/`file`/`flush` 完整签名（[03-syntax.md](../language/03-syntax.md) §9.1），与任务 19 的 `fmt` 模块分工明确：常规输出一律 `print`，格式控制才用 `fmt`；
- 类型转换内建（`int`/`float`/`str`/`bool`）覆盖 v0.1 全部核心类型（nil/bool/int64/float/str/list/dict），失败时报 TypeError/ValueError；
- 本任务是第一个以 ms 脚本测试为主验收方式的非里程碑任务：全部行为用 `tests/ms/builtin/` 下的脚本 + 内建 `assert`/`print` 验证（`testing` 模块在任务 40 才存在）。

明确不在本任务范围：`cap`/`chan`（v0.3 并发）、`isinstance`/`issubclass`（依赖 class 体系，任务 15/25）、`repr`（依赖魔术方法协议，任务 25）、`bytes`/`tuple`/`set`/`list`/`dict` 构造器（任务 16/32）、`enumerate`/`zip`/`map`/`filter`/`sorted`/`reversed`/`iter`/`next`（依赖迭代协议，任务 26）、`open`/`input`（任务 36/37）、`id`/`hash`/`callable`/`vars`/`globals`/`locals`/`dir`（后续版本排期）。

## 设计依据

- [03-syntax.md](../language/03-syntax.md) §9（内建函数全集清单）、§9.1（`print` 完整签名 `print(*values, sep=" ", end="\n", file=nil, flush=false)` 与逐参数语义）、§8（名字解析顺序：局部 → 闭包外层 → 模块全局 → 内建，故内建写入全局命名空间即可被遮蔽）。
- [02-types.md](../language/02-types.md) §2（真值规则：`nil`/`false`/数值零/空字符串/空容器为假）、§3.3（显式转换语义：`int("42")`、`int(3.9)` 截断、`float("3.14")`、`str(42)`、`bool(x)`；无 float→int 隐式转换）、§4（str 按 Unicode 码点计长）、§7（`range` 规范上返回惰性序列对象）、§9（`type(x)` 返回类型对象）。
- [09-c-api.md](../language/09-c-api.md) §9（`MsCFunction` 签名 `MsObject* (*)(MsState* L, int64_t argc, MsObject** argv)`，内建函数对象即 `MS_TYPE_C_FUNCTION` 类型）、§8（错误处理约定：C 函数出错时 `msRaiseTypeError`/`msRaiseValueError` 设置错误并返回 `NULL`，VM 转为脚本级失败）、§3（GC 根纪律：内建实现中跨分配点存活的局部 `MsObject*` 必须入根）。
- [08-vm-internals.md](../language/08-vm-internals.md) §3（`MsTypeTag` 枚举含 `MS_TYPE_C_FUNCTION`；对象头 `MsObjectHeader.type` 指向类型对象；小整数驻留）。
- [07-stdlib.md](../language/07-stdlib.md) §1（print 与 fmt 的分工约定：fmt 只负责格式化输出，刻意不提供 `fmt.print`/`fmt.println`；简单插值优先 f-string）。
- [04-exceptions.md](../language/04-exceptions.md) §4（内建异常层级中的 `TypeError`/`ValueError` 名字；异常对象体系在任务 23 落地，本任务阶段错误仍以任务 09 的错误槽 + `MS_ERROR_RUNTIME` 形式呈现，消息文本即异常类型名前缀，如 `TypeError: ...`）。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、include guard、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [11-project-layout.md](../language/11-project-layout.md) §1（内建函数归属 `src/vm/`）、§4（测试策略：任务 09 起脚本测试层）。
- [09-minimal-interpreter.md](09-minimal-interpreter.md) §4（临时内建 `print`/`assert` 的挂接方式即正式机制的最小样例，本任务将其扩展为完整模块，`msBuiltinRegisterMinimal` 由 `msBuiltinRegisterAll` 取代）。
- 以下接口名在依赖任务文档（06 对象模型、08 VM 执行核心、13 函数与调用）中尚未定稿，本文为其假定命名：`msNewCFunction`（创建 C 函数对象）、`msObjectStr`/`msObjectRepr`（值的 str/repr 表示）、`msObjectCompare`（值比较）、关键字参数传递到 C 函数的调用约定。**实现时以对应任务文档定名为准。**

## 详细设计

### 1. 文件与模块骨架

- 头文件 `src/vm/ms_builtin.h`，include guard `MSLANG_SRC_VM_MS_BUILTIN_H_`，自包含（include `<mslang/state.h>`）。只公开一个函数：

```c
// Registers all v0.1 builtin functions into L's global namespace.
// Called exactly once by msNewState, after the global namespace is
// initialized. Replaces task 09's msBuiltinRegisterMinimal.
void msBuiltinRegisterAll(MsState* L);
```

- 实现文件 `src/vm/ms_builtin.c`；全部内建函数与辅助函数为文件内 `static`。
- 模块自身不持有堆状态：注册表是 `static const` 数据，内建函数对象由 `MsState` 的全局命名空间持有。

### 2. 注册机制

注册表为文件级 `static const` 数组（内部结构体不 typedef）：

```c
struct MsBuiltinEntry {
  const char* name;      // global name visible to scripts, e.g. "print"
  MsCFunction func;      // implementation, signature per 09-c-api.md section 9
};

static const struct MsBuiltinEntry msBuiltinTable[] = {
  {"print",  msBuiltinPrint},
  {"assert", msBuiltinAssert},
  {"len",    msBuiltinLen},
  {"type",   msBuiltinType},
  {"int",    msBuiltinInt},
  {"float",  msBuiltinFloat},
  {"str",    msBuiltinStr},
  {"bool",   msBuiltinBool},
  {"range",  msBuiltinRange},
  {"abs",    msBuiltinAbs},
  {"min",    msBuiltinMin},
  {"max",    msBuiltinMax},
};
```

`msBuiltinRegisterAll` 流程：遍历 `msBuiltinTable`，对每项调用 `msNewCFunction(L, name, func)`（任务 06/08 提供，创建 `MS_TYPE_C_FUNCTION` 对象）得到函数对象，立即经 `msSetGlobal(L, name, fnObj)` 写入全局命名空间；创建失败（OOM）时按任务 09 的 `msNewState` 失败路径向上传播。全局命名空间即「内建」层：脚本用 `:=` 同名声明即可遮蔽内建（[03-syntax.md](../language/03-syntax.md) §8 的解析顺序保证），无需独立的内建表。

### 3. 公共辅助函数（文件内 static）

```c
// Reports "name() takes at least/at most N argument(s)" as TypeError and
// returns false when argc is outside [min, max]. max < 0 means unbounded.
static bool msBuiltinCheckArgc(MsState* L, const char* name, int64_t argc, int64_t min, int64_t max);

// Reports "name() argument must be <want>, not <got>" as TypeError, false on mismatch.
static bool msBuiltinCheckType(MsState* L, const char* name, MsObject* arg, MsTypeTag want);

// Formats the truthiness of any v0.1 value per 02-types.md section 2.
static bool msBuiltinTruthy(MsObject* obj);
```

`msBuiltinTruthy` 的 v0.1 判定集：`nil`、`false`、`0`（含驻留小整数）、`0.0`/`-0.0`、`""`、空 list、空 dict 为假，其余为真；`__bool__`/`__len__` 协议随任务 25 接入，此处预留单一入口便于替换。

### 4. `print`：完整签名与 fmt 分工

签名对齐 [03-syntax.md](../language/03-syntax.md) §9.1：`print(*values, sep=" ", end="\n", file=nil, flush=false)`。

- 语义：逐个把 `*values` 经 `msObjectStr`（值的 str 表示，见第 5 节）转为字符串，以 `sep` 连接写入输出目标，末尾追加 `end`；返回 nil。
- `file`：`nil`（默认）表示 stdout；非 nil 要求实现 `io.Writer` 协议（`write`/`flush`），v0.1 尚无 io 模块（任务 37），本阶段对非 nil 的 `file` 一律报 `TypeError`，协议分派在 io 落地后补齐。
- `flush`：为真时对输出目标冲刷；stdout 路径即 `fflush(stdout)`。
- 关键字参数通道：`MsCFunction` 签名（[09-c-api.md](../language/09-c-api.md) §9）本身不携带关键字参数；`MS_OP_CALL_KW` 的调用约定由任务 13 定义。本任务的假定约定：VM 以 `CALL_KW` 调用 C 函数时，把关键字参数以「名字字符串对象、值对象」交错追加在 `argv` 尾部并计入 `argc`；`msBuiltinPrint` 从尾部扫描名字为 `sep`/`end`/`file`/`flush` 的对并消费，其余条目作为 `*values`。未知名字报 `TypeError: print() got an unexpected keyword argument 'x'`。**该约定以实现时任务 13 文档定名为准**；在 `CALL_KW` 落地前，脚本侧只能使用位置参数与默认 `sep`/`end`。
- 与 fmt 的分工（[07-stdlib.md](../language/07-stdlib.md) §1）：常规输出只用 `print`；宽度、精度、进制、对齐等格式控制属于任务 19 的 `fmt.printf`/`fmt.sprintf`/`fmt.fprintf`；fmt 不提供 `print`/`println`（与内建重叠，刻意删除）。本任务不在 print 内实现任何格式动词——`print` 只输出 str 表示。

### 5. 值的字符串表示（str 表示层）

`str(x)` 内建与 `print` 共用同一表示函数 `msObjectStr(L, obj)`（假定名，任务 06 提供；若任务 06 未覆盖容器表示，本模块以 `static` 函数补齐并随任务 06 对齐）。v0.1 各类型表示：

| 类型 | 表示 | 示例 |
|---|---|---|
| nil | `nil` | `nil` |
| bool | `true` / `false` | `true` |
| int | 十进制 | `-42` |
| float | 最短往返表示 | `2.5`、`1e-9`；整数值得带 `.0`，即 `3.0` |
| str | 自身 | `hello` |
| list | `[e0, e1, ...]`，元素用 repr 风格（字符串元素带双引号） | `[1, "two"]` |
| dict | `{"k": v, ...}`，键值同 repr 风格，保持插入序 | `{"a": 1}` |
| C 函数 | `<builtin function print>` | — |
| 类型对象 | `<type 'int'>` | — |

float 的最短往返算法：从 `%.17g` 起步逐步降低精度，取第一个 `strtod` 往返相等的表示；若结果无小数点与指数标记则补 `.0`（对齐 `strconv.formatFloat(f, prec=-1)` 的目标行为，正式实现随任务 35 收敛）。

### 6. 逐个内建的语义与错误行为

统一错误风格：参数个数/类型不符报 `TypeError`，值域错误报 `ValueError`，均以 `msRaiseTypeError`/`msRaiseValueError` 置错误并返回 `NULL`（[09-c-api.md](../language/09-c-api.md) §8）；异常对象体系落地（任务 23）前表现为错误槽消息 + `MS_ERROR_RUNTIME`，CLI 退出码 1。

- `assert(cond, msg=nil)`（沿用任务 09，纳入注册表）：`cond` 按 `msBuiltinTruthy` 判定，为假时报 `AssertionError`（带 `msg` 的 str 表示）；为真返回 nil。
- `len(x)`：str → Unicode **码点数**（非字节数，[02-types.md](../language/02-types.md) §4）；list → 元素数；dict → 键数。其余类型报 `TypeError: object of type 'int' has no len()`。
- `type(x)`：返回 `x` 的类型对象（对象头 `type` 字段装箱为脚本可见值，内建类型表由任务 06 维护）。`str(type(1))` 为 `<type 'int'>`，供脚本测试断言。`type` 的结果参与 `==` 比较时按身份（类型对象唯一）。
- `int(x, base=10)`：
  - int → 原值；bool → `0`/`1`（对齐 Python；规范未明文，此为假定语义）；float → 向零截断，`nan`/`inf` 报 ValueError；
  - str → 按 `base`（允许 2/8/10/16）解析，允许前导符号，空串或含非法字符报 `ValueError`；解析走 `msNewIntFromString`（[09-c-api.md](../language/09-c-api.md) §5）；v0.1 整数为 int64，溢出报 `ValueError`（任意精度随任务 31 解除该限制）；
  - str 以外的类型配合显式 `base` 报 `TypeError`；其余类型报 `TypeError`。
- `float(x)`：float → 原值；int/bool → 转换；str → 解析（十进制浮点形式），失败报 `ValueError`；其余报 `TypeError`。
- `str(x)`：第 5 节的表示；永不失败（OOM 除外）。
- `bool(x)`：`msBuiltinTruthy` 的结果。
- `range(start, stop=nil, step=1)`：
  - 1 参表示 `[0, start)`；2 参 `[start, stop)`；3 参带 `step`；`step == 0` 报 `ValueError`；负 `step` 支持（向下计数）；
  - 参数只接受 int（bool 不接受，报 `TypeError`；与 `int()` 的 bool 宽容不同，此处从严，避免 `range(true)` 之类的隐式语义）；
  - v0.1 返回**物化的 int list**（`msNewList` + `msListAppend` 逐项填充）。规范（[02-types.md](../language/02-types.md) §7）要求惰性序列对象，但惰性对象依赖迭代协议（任务 26）；物化 list 在 `len`/下标/`for-in`（任务 26 后）下的脚本可见行为与惰性对象一致，届时以惰性 range 类型替换实现、行为不变。此偏差须在本任务文档显式承认；
  - 物化前计算元素个数，个数为负按 0 处理；分配失败经 `msAlloc` 统一走 `MS_ERROR_OOM`。
- `abs(x)`：int → 绝对值（`INT64_MIN` 溢出报 `ValueError`，大整数落地后自然解除）；float → 绝对值；其余 `TypeError`。
- `min(...)` / `max(...)`：两种形态——`min(a, b, ...)`（≥1 个位置参数）或 `min(xs)`（单参为 list，取元素）。空参或空 list 报 `ValueError: min() arg is an empty sequence`。比较经 `msObjectCompare`（假定名，任务 06）：v0.1 可比较对为 int/float 混合（int 提升为 float）与 str/str；不可比较的组合报 `TypeError`。返回最大/最小值本身（不复制）。

### 7. GC 与内存纪律

- 内建实现中，任何可能触发分配的调用（`msNewString`、`msNewList`、`msObjectStr` 等）之间存活的局部 `MsObject*` 必须 `msRootPush`/`msRootPop`（[09-c-api.md](../language/09-c-api.md) §3）；`range` 的逐项填充循环是主要风险点：list 先入根，再循环 append。
- 函数参数 `argv` 自动是根（调用约定保证），返回值在下一次分配前有效；内建返回新建对象时遵循转移所有权语义。
- 本模块不直接调用 `malloc`；临时缓冲（如 float 格式化）经 `msAlloc`/`msFree`。

## 实现步骤

1. 建 `src/vm/ms_builtin.h`（guard `MSLANG_SRC_VM_MS_BUILTIN_H_`）与注册表骨架：`struct MsBuiltinEntry`、`msBuiltinTable`（先只含 `print`/`assert`）、`msBuiltinRegisterAll`；`msNewState` 改调 `msBuiltinRegisterAll` 取代任务 09 的 `msBuiltinRegisterMinimal`。验证：任务 09 的冒烟脚本（`hello.ms`/`arithmetic.ms`）全部保持通过。
2. 实现公共辅助 `msBuiltinCheckArgc`/`msBuiltinCheckType`/`msBuiltinTruthy`，统一错误消息格式（`TypeError: ...`/`ValueError: ...` 前缀）。验证：`assert` 走新真值入口后冒烟脚本仍通过。
3. 实现 str 表示层：nil/bool/int/float/str 的基础表示（含 float 最短往返）与 list/dict 的容器表示（元素 repr 风格）；`str` 内建落地。验证：ms 脚本断言 `str` 各类型输出与容器格式。
4. 升级 `print` 至完整签名：`sep`/`end`/`file`/`flush` 解析（含假定的 kwarg 尾部扫描），`file != nil` 报 TypeError，`flush` 冲刷 stdout。验证：`.out` 同伴文件比对多值、`sep`/`end` 默认行为的 stdout 全文。
5. 实现 `len` 与 `type`：str 码点计数（UTF-8 解码逐码点）、list/dict 长度；`type` 返回类型对象并保证 `<type 'int'>` 表示。验证：多字节字符串的 `len` 断言、`print(type(x))` 输出比对。
6. 实现 `int`/`float`/`bool`：全部分支与错误路径（含 `int("0x1F", 16)`、`int(3.9)`、`int(nan)` 报 ValueError、溢出报 ValueError）。验证：转换矩阵逐格断言。
7. 实现 `range` 物化：参数形态判定、`step == 0` 检查、负 step、预计算个数、list 入根后逐项填充。验证：1/2/3 参与负 step 的内容断言，`range(1, 10, 0)` 负例退出码。
8. 实现 `abs`/`min`/`max`：两种调用形态、空序列 ValueError、混合数值提升、不可比较 TypeError。验证：各形态正例与负例。
9. 全量回归：任务 09 冒烟脚本 + 本任务全部 `tests/ms/builtin/` 脚本经 `run_tests.py` 与 `ctest --test-dir build` 通过；Debug 构建（ASAN / `/RTC`）无内存错误，`msCloseState` 后无残余分配计数。

## 测试方案

本任务晚于最小可运行解释器（任务 09），一律用 ms 脚本测试，由仓库根 `run_tests.py` 驱动 mslang CLI（设施在任务 09 建立）；`testing` 模块（任务 40）之前用内建 `assert` + `print` 自断言。正向脚本成功路径以 `print("<name> ok")` 收尾、退出码 0；需要精确输出比对的脚本配 `<name>.out` 同伴文件全文比对 stdout；负向用例配 `<name>.exit`（内容 `1`）声明预期退出码。测试目录 `tests/ms/builtin/`（脚本随实现编写，本文只列清单与覆盖点）：

- `print.ms` + `print.out`：多值输出的空格分隔与末尾换行；`print()` 空参只输出换行；`print` 返回 nil（`assert(print(1) == nil)`）；`str` 表示经 print 输出（`true`/`nil`/`2.5`/`3.0`/list/dict 格式）。注：`sep`/`end`/`flush` 的 kwarg 用例在任务 13 的 `CALL_KW` 落地后补入本文件。
- `len.ms`：`len("hello") == 5`；多字节字符串按码点计（如 `len("中文字符") == 4`，具体码点数按脚本所用字符核定）；`len([1, 2, 3]) == 3`；`len({"a": 1}) == 1`；空容器为 0。
- `type.ms` + `type.out`：`print(type(1))` → `<type 'int'>`、`type(1.0)`/`type("s")`/`type(true)`/`type(nil)`/`type([])`/`type({})`/`type(print)` 逐行比对；`type(1) == type(2)` 为真（类型对象唯一）。
- `convert.ms`：转换矩阵——`int(3.9) == 3`、`int(-3.9) == -3`、`int(true) == 1`、`int("42") == 42`、`int("1F", 16) == 31`、`float(2) == 2.0`、`float("3.14") == 3.14`、`str(42) == "42"`、`str(nil) == "nil"`；`bool` 真值表全量（nil/false/0/0.0/""/[]/{} 为假，其余为真）。
- `range.ms`：`range(5)` 物化为 `[0, 1, 2, 3, 4]`；`range(2, 8)`；`range(0, 10, 3)`；`range(5, 0, -1)`；空区间 `range(3, 3)` 为 `[]`；`len(range(0, 100, 7))` 断言个数公式。
- `minmax.ms`：`abs(-3) == 3`、`abs(-2.5) == 2.5`；`min(3, 1, 2) == 1`、`max(3, 1, 2) == 3`；`min([5, 2, 8]) == 2`；混合 `max(1, 2.5) == 2.5`；`min("a", "b") == "a"`。
- 负例（各配 `.exit` 同伴，预期退出码 1，错误消息含对应异常名前缀）：`err_len_int.ms`（`len(1)` → TypeError）、`err_int_str.ms`（`int("abc")` → ValueError）、`err_int_overflow.ms`（`int("99999999999999999999")` → ValueError，v0.1 int64 上限）、`err_range_step.ms`（`range(1, 10, 0)` → ValueError）、`err_min_empty.ms`（`min([])` → ValueError）、`err_min_mixed.ms`（`min(1, "a")` → TypeError）、`err_assert.ms`（`assert(false, "boom")` → AssertionError，消息含 `boom`）。

## 验收标准

- [ ] `src/vm/ms_builtin.h` / `ms_builtin.c` 存在，guard 为 `MSLANG_SRC_VM_MS_BUILTIN_H_`，头文件自包含；代码风格符合 10-c-style（2 空格缩进、120 列、K&R、星号贴类型、`struct MsBuiltinEntry` 不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] 注册机制经 `static const` 表驱动，`msNewState` 调用 `msBuiltinRegisterAll` 一次注册全部 v0.1 内建；任务 09 的冒烟脚本无回归；内建可被脚本同名 `:=` 遮蔽。
- [ ] v0.1 内建全集 `print`/`assert`/`len`/`type`/`int`/`float`/`str`/`bool`/`range`/`abs`/`min`/`max` 的行为与本文「详细设计」第 4–6 节一致，含错误类型（TypeError/ValueError/AssertionError）与消息前缀。
- [ ] `print` 实现完整签名的解析入口（含 kwarg 通道假定），`file` 非 nil 报 TypeError；与 fmt 的分工不在 print 内引入任何格式动词。
- [ ] `len` 对 str 按 Unicode 码点计数；`float` 的 str 表示满足最短往返且整数值带 `.0`；`range` 物化语义与元素个数正确，`step == 0` 报 ValueError。
- [ ] 内建实现遵守 GC 根纪律（`range` 填充循环等跨分配点的局部对象入根），Debug 构建（ASAN / `/RTC`）无内存错误与泄漏。
- [ ] `tests/ms/builtin/` 覆盖「测试方案」全部清单项（含 `.out` 输出比对与 `.exit` 负例），`python run_tests.py` 与 `ctest --test-dir build` 全数通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；假定接口名（`msNewCFunction`、`msObjectStr`/`msObjectRepr`、`msObjectCompare`、kwarg 调用约定）在实现时已与任务 06/08/13 文档对齐。
