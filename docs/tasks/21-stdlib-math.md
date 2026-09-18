# 21 标准库：math

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [18 C API 基础](18-c-api-foundation.md) |

## 任务目标

交付 C 实现的标准库模块 `math`（`stdlib/ms_std_math.h` / `stdlib/ms_std_math.c`），完整覆盖 `docs/language/07-stdlib.md` §4 的全部条目：4 个常量（`pi`/`e`/`inf`/`nan`）与 23 个函数（取整/绝对值、幂与对数、三角函数、谓词、整数函数 gcd/lcm/factorial）。全部数值函数是 C11 `<math.h>` 对应函数的薄封装，但按 MS 语义补齐三件事：**int/float 参数的统一处理约定**、**定义域错误转为脚本错误（ValueError）而非返回 NaN**、**int 参数保持 int 结果类型**（`floor`/`ceil`/`trunc`/`abs`/`gcd`/`lcm`/`factorial`）。完成后脚本侧 `import "math"` 即可使用全部功能；本任务经 `tests/ms/stdlib/` 下的 ms 脚本独立验证，不依赖后续任何任务。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0：`math` 属 **C 内建模块**，源码位于 `stdlib/` 目录。
  - §4：常量与函数清单（本任务的功能边界，逐条对应，不增不减）。
- `docs/language/02-types.md`
  - §3.1/§3.2：v0.1 的 `int` 为 int64（任意精度大整数在任务 31 落地）；`float` 为 IEEE 754 双精度，特殊值即 `math.inf`/`math.nan`。
  - §3.3：`int op float` → `float`；**不存在 float → int 隐式转换**——math 函数的参数提取必须遵守此约定。
- `docs/language/04-exceptions.md` §错误层级：`TypeError`/`ValueError`/`OverflowError` 的类名约定。v0.1 异常系统（任务 23）尚未落地，本任务经 C API 的 `msRaiseTypeError`/`msRaiseValueError` 置错误状态，VM 转为执行失败；错误类的精确挂载待任务 23 对齐。
- `docs/language/05-modules.md` §2：模块解析第 1 级为「内建/已注册模块」，C 模块不可被路径同名脚本遮蔽；§7：C 扩展与脚本模块共用注册表。
- `docs/language/09-c-api.md`
  - §5：`msNewFloat`/`msNewInt`/`msNewBool`/`msTypeOf`/`msAsFloat`/`msAsInt`（值构造与转换）。
  - §8：`msRaiseTypeError`/`msRaiseValueError` 错误约定；C 函数出错返回 `NULL`。
  - §9：`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule` 扩展模块注册机制。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、include guard、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）；§5「禁止 errno 跨层传播」——本模块不读 `errno`，定义域检查全部在调用 C 库前显式完成。
- `docs/language/11-project-layout.md` §1：`stdlib/` 目录定位；§4：脚本测试层约定。
- 任务 18（C API 基础）提供：标准库注册入口（本文假定 `msNewState` 内统一调用各 stdlib 模块的注册函数）、`msRegisterModule`、以及注册后向模块对象写入属性的途径（本文假定为先经模块注册表取得模块对象、再以 `msSetAttr` 写入常量）。任务 18 文档尚不存在，上述接口名为本文假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 文件与头文件骨架

- 头文件 `stdlib/ms_std_math.h`，include guard `MSLANG_STDLIB_MS_STD_MATH_H_`，自包含（自行 include `<mslang/mslang.h>`）。
- 实现文件 `stdlib/ms_std_math.c`；include 顺序：对应头文件 → C 系统头（`<math.h>`、`<stdint.h>`）→ 项目内头文件。全部函数实现与辅助函数为文件内 `static`，唯一导出符号是注册函数。
- 模块自身**零堆分配**：函数结果对象经 `msNewInt`/`msNewFloat`/`msNewBool` 由 `MsState` 分配，常量表与方法表均为文件级 `static const`。

### 模块注册接口

```c
// stdlib/ms_std_math.h
#ifndef MSLANG_STDLIB_MS_STD_MATH_H_
#define MSLANG_STDLIB_MS_STD_MATH_H_

#include <mslang/mslang.h>

// Registers the "math" builtin module (constants + functions) into the
// builtin module registry. Called once during interpreter startup
// (msNewState's stdlib wiring, task 18).
MsResult msStdMathRegister(MsState* L);

#endif  // MSLANG_STDLIB_MS_STD_MATH_H_
```

注册流程（`msStdMathRegister` 内部）：

1. 以 `MsModuleDef` 注册 23 个函数：方法表 `static const MsMethodDef msStdMathMethods[]`，`{NULL, NULL, NULL}` 收尾，调用 `msRegisterModule(L, &msStdMathModuleDef)`。
2. 取得已注册的模块对象（任务 18 提供的注册表查询接口，假定名 `msGetRegisteredModule`），逐个写入 4 个常量：`msSetAttr(L, module, "pi", msNewFloat(L, MS_MATH_PI))` 等。常量对象在写入后立即被模块持有，无需额外入根（属性写入本身是最后一次分配点之前的消费；若任务 18 的注册时序不同，按根纪律补 `msRootPush`/`msRootPop`）。
3. 任一步失败早返回相应 `MsResult`（OOM 传播 `MS_ERROR_OOM`）。

常量定义（不用 `<math.h>` 的 `M_PI`/`M_E`——二者非 C11 标准、MSVC 需额外宏开启；`INFINITY`/`NAN` 是 C11 标准宏，直接使用）：

```c
#define MS_MATH_PI 3.14159265358979323846
#define MS_MATH_E  2.71828182845904523536

struct MsMathConstDef {     // 内部结构体，不 typedef
  const char* name;
  double value;
};

static const struct MsMathConstDef msStdMathConsts[] = {
  {"pi",  MS_MATH_PI},
  {"e",   MS_MATH_E},
  {"inf", INFINITY},
  {"nan", NAN},
};
```

### 参数处理约定（int/float）

模块内统一的两个参数提取辅助函数（文件内 `static`，返回 `false` 时错误状态已置好）：

```c
// Accepts int or float; int is converted to double (mixed-arithmetic
// promotion per 02-types §3.3). Any other type (including bool, str,
// nil) raises TypeError and returns false.
static bool mathArgAsDouble(MsState* L, MsObject* arg, double* out);

// Accepts int only; float is NOT implicitly truncated (02-types §3.3:
// no float → int implicit conversion). Anything else raises TypeError.
static bool mathArgAsInt64(MsState* L, MsObject* arg, int64_t* out);
```

约定要点：

- **bool 不是数值参数**：`math.sqrt(true)` 报 `TypeError`。bool → int 的隐式含义在 v0.1 未定义，math 模块不做先行者。
- 每个函数先校验 `argc`（不符报 `TypeError`，消息含期望参数个数），再逐参数提取。
- 返回值构造失败（OOM）时 `msNew*` 返回 `NULL` 且错误状态已置，直接向上传播 `NULL`。

### 函数清单与 C11 `<math.h>` 映射

返回类型列中「数值 → X」表示参数为 int 或 float 时统一返回 X；「保持数值种类」表示 int 入 int 出、float 入 float 出。

| 脚本函数 | C11 映射 | 参数 | 返回 | 定义域错误 |
|---|---|---|---|---|
| `math.floor(x)` | `floor` | 数值 | 保持数值种类 | 无 |
| `math.ceil(x)` | `ceil` | 数值 | 保持数值种类 | 无 |
| `math.trunc(x)` | `trunc` | 数值 | 保持数值种类 | 无 |
| `math.abs(x)` | `fabs`（float）/ 自实现（int） | 数值 | 保持数值种类 | `INT64_MIN` 溢出（见下） |
| `math.sqrt(x)` | `sqrt` | 数值 | float | `x < 0` → ValueError |
| `math.pow(x, y)` | `pow` | 数值 ×2 | float | 见下「pow 定义域」 |
| `math.exp(x)` | `exp` | 数值 | float | 无（溢出返回 `inf`） |
| `math.log(x, base=math.e)` | `log` | 数值，可选数值 base | float | `x <= 0`、`base <= 0` 或 `base == 1` → ValueError |
| `math.log2(x)` | `log2` | 数值 | float | `x <= 0` → ValueError |
| `math.log10(x)` | `log10` | 数值 | float | `x <= 0` → ValueError |
| `math.sin/cos/tan(x)` | `sin`/`cos`/`tan` | 数值 | float | 无 |
| `math.asin/acos(x)` | `asin`/`acos` | 数值 | float | `|x| > 1` → ValueError |
| `math.atan(x)` | `atan` | 数值 | float | 无 |
| `math.atan2(y, x)` | `atan2` | 数值 ×2 | float | 无（含 `(0, 0)`，依 C 语义返回 0） |
| `math.isNaN(x)` | `isnan` | 数值 | bool | 无（int 恒为 `false`） |
| `math.isInf(x)` | `isinf` | 数值 | bool | 无（int 恒为 `false`） |
| `math.isFinite(x)` | `isfinite` | 数值 | bool | 无（int 恒为 `true`） |
| `math.gcd(a, b)` | 自实现（Euclid） | int ×2 | int | 结果溢出 int64（见下） |
| `math.lcm(a, b)` | 自实现 | int ×2 | int | 结果溢出 int64 |
| `math.factorial(n)` | 自实现 | int | int | `n < 0` → ValueError；`n > 20` 溢出（见下） |

语义决策（规范未写明处，本文定为模块契约）：

- **「保持数值种类」**：`floor(3)` 返回 int `3` 而非 float（恒等，`math.floor` 对 int 是 no-op）；`floor(3.7)` 返回 float `3.0`。理由：02-types §3.3 禁止 float → int 隐式转换，math 模块不越过该约定制造 int。
- **`math.abs(INT64_MIN)`**：int64 无可表示结果，v0.1 报 `ValueError`（消息注明 overflow）；待任务 31 大整数落地后改为自动提升，`OverflowError` 类名于任务 23 对齐。`lcm`、`factorial`、`gcd(INT64_MIN, 0)` 的溢出同此处理。
- **溢出与下溢不是定义域错误**：`math.exp(1000.0)` 返回 `inf` 而非报错（IEEE 754 语义，与 `math.inf` 常量呼应）。只有「结果在实数域无定义」的情形报 `ValueError`。
- **不读 `errno`**：所有定义域判定在调用 C 库函数**之前**显式完成（10-c-style §5）。
- **`log` 的可选 base**：C 回调按 `argc` 分派，`argc == 1` 时 `return msNewFloat(L, log(x))`；`argc == 2` 时先校验 base 再返回 `log(x) / log(base)`。默认参数由 C 层模拟，v0.1 无关键字参数语义参与。
- **`atan2(y, x)`**：参数顺序与 07-stdlib §4 签名一致（先 y 后 x），与 C `atan2(y, x)` 同序。
- **`math.nan` 与相等**：`nan != nan`（02-types §3.2），判断 NaN 必须用 `math.isNaN`；常量比较走普通 float 语义，模块不做特殊处理。

### pow 定义域判定

`pow(x, y)` 在调用 C `pow` 前显式检查两种非法情形：

1. `x == 0.0 && y < 0.0` → ValueError（`0` 的负数次幂无定义）；
2. `x < 0.0 && y != floor(y)` → ValueError（负底数的非整数次幂结果为复数，float 无法表示）。

其余情形（含 `pow(0, 0) == 1.0`、负底数的整数次幂、结果溢出为 `inf`）直接取 C 库结果。

### 整数函数实现

- `gcd(a, b)`：Euclid 算法在 **uint64 幅度**上进行，避免 `INT64_MIN` 取负的未定义行为：

```c
static uint64_t mathAbsMagnitude(int64_t v);  // INT64_MIN-safe magnitude
```

  流程：`ua = mathAbsMagnitude(a)`、`ub = mathAbsMagnitude(b)` → 循环取余至 `ub == 0` → 结果恒为非负；`gcd(0, 0) == 0`。唯一溢出情形是结果为 2⁶³（仅当输入含 `INT64_MIN` 且另一参数为 0 或其倍数），此时按溢出约定报 `ValueError`。
- `lcm(a, b)`：`a == 0 || b == 0` 返回 int `0`；否则 `u = ua / gcd * ub`（先除后乘抑制中间溢出），`u > INT64_MAX` 报溢出错误；结果非负。
- `factorial(n)`：`n < 0` 报 `ValueError`；`n > 20` 按溢出约定报 `ValueError`（21! 超出 int64，20! = 2432902008176640000 是最后一个可表示值）；循环累乘，返回 int。

### 函数封装形态

23 个函数签名同为 `MsCFunction`（09-c-api §9）。每个函数为 3–8 行的薄封装，统一骨架（以 `sqrt` 为例）：

```c
static MsObject* msStdMathSqrt(MsState* L, int64_t argc, MsObject** argv) {
  if (argc != 1) {
    msRaiseTypeError(L, "math.sqrt() takes exactly 1 argument");
    return NULL;
  }
  double x;
  if (!mathArgAsDouble(L, argv[0], &x)) {
    return NULL;
  }
  if (x < 0.0) {
    msRaiseValueError(L, "math.sqrt() domain error");
    return NULL;
  }
  return msNewFloat(L, sqrt(x));
}
```

对无定义域检查的一元数值函数（`sin`/`cos`/`tan`/`atan`/`exp`），允许用文件内宏生成重复骨架（如 `MS_MATH_DEFINE_FLOAT_UNARY(scriptName, cFunc)`），这是函数式宏无法被函数替代的样板消除场景（10-c-style §4 允许）；有定义域检查或特殊返回类型的函数一律手写，保证可读性。

`isNaN`/`isInf`/`isFinite` 对 int 参数不经过 `mathArgAsDouble` 的转换路径也无妨——但为统一行为仍先走提取（int 转 double 后 `isnan` 恒假）；实现可任选「先类型分派」或「统一转 double」，行为必须一致：int 输入分别返回 `false`/`false`/`true`。

### 方法表

```c
static const MsMethodDef msStdMathMethods[] = {
  {"floor",     msStdMathFloor,     "floor(x) -> number"},
  {"ceil",      msStdMathCeil,      "ceil(x) -> number"},
  // ... 23 项，doc 字段为一行签名说明
  {"factorial", msStdMathFactorial, "factorial(n) -> int"},
  {NULL, NULL, NULL},
};

static const MsModuleDef msStdMathModuleDef = {
  "math", "mathematical functions and constants", msStdMathMethods,
};
```

方法名与 07-stdlib §4 的脚本名逐字一致（含小驼峰 `isNaN`/`isInf`/`isFinite`）。

## 实现步骤

1. 建 `stdlib/ms_std_math.h` / `ms_std_math.c` 骨架：guard、导出函数 `msStdMathRegister` 的空实现、接入构建系统（`mslang` 库目标纳入 `stdlib/` 源文件）。验证：全平台编译通过。
2. 实现 `mathArgAsDouble` / `mathArgAsInt64` 两个参数提取辅助与 `argc` 校验惯例。验证：以 `floor` 为首个落地函数，int/float/bool/str 四类参数的 ms 脚本断言行为。
3. 常量注册：`msStdMathConsts` 表 + `msStdMathRegister` 的属性写入流程。验证：脚本断言 `math.pi`/`math.e` 近似值、`math.inf > 1e308`、`math.nan != math.nan`。
4. 取整与绝对值组：`floor`/`ceil`/`trunc`/`abs`（含「保持数值种类」与 `abs(INT64_MIN)` 溢出分支）。验证：正负 int/float 参数矩阵的脚本断言。
5. 幂与对数组：`sqrt`（定义域）、`pow`（两条定义域规则）、`exp`、`log`（可选 base）、`log2`/`log10`。验证：已知值断言 + 各定义域负向脚本。
6. 三角函数组：`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`（asin/acos 定义域检查）。验证：特殊角断言（`sin(pi/2) == 1.0` 等，epsilon 比较）。
7. 谓词组：`isNaN`/`isInf`/`isFinite`，覆盖 int/float/inf/nan 输入矩阵。验证：脚本断言。
8. 整数函数组：`mathAbsMagnitude` + `gcd`（Euclid、非负结果、`INT64_MIN` 路径）+ `lcm`（先除后乘、溢出检查）+ `factorial`（0!、20!、负 n、n > 20）。验证：脚本断言 + 溢出负向脚本。
9. 将 `msStdMathRegister` 挂入任务 18 的标准库启动流程（`msNewState` 或统一 `msOpenLibs` 入口），确认 `import "math"` 经内建注册表第 1 级解析命中、不可被搜索路径同名文件遮蔽。验证：冒烟脚本 `import "math"` 成功。
10. 全平台（Win/Linux/macOS）× Debug/Release 构建验证，Debug 下无 ASAN 报告；`-ffast-math` 类优化保持关闭（NaN/inf 语义依赖 IEEE 严格行为）。

## 测试方案

本任务在最小可运行解释器（任务 09）之后，一律使用 ms 脚本测试；`testing` 模块（任务 40）尚不存在，断言用内建 `assert` + `print`，由仓库根 `run_tests.py` 驱动（任务 09 建立的设施：正向脚本退出码 0，负向脚本以 `<name>.exit` 同伴文件声明预期退出码）。浮点比较一律用 epsilon 容差（`|a - b| < 1e-12`，脚本内自定义辅助函数），不断言 float 的精确位相等。

测试文件清单（`tests/ms/stdlib/` 目录）：

- `math_constants.ms`（正向）：4 个常量存在性与数值属性——`pi`/`e` 落在已知区间、`math.inf` 大于任意有限数、`math.inf == math.inf`、`math.nan != math.nan`、`math.isNaN(math.nan)`。
- `math_round_abs.ms`（正向）：`floor`/`ceil`/`trunc`/`abs` 的 int/float × 正/负/零矩阵（如 `floor(-2.5) == -3.0`、`floor(7)` 的类型仍为 int 且值不变、`abs(-3) == 3`、`abs(-2.5) == 2.5`）。
- `math_pow_log.ms`（正向）：`sqrt(2)`、`pow(2, 10)`、`pow(2.0, 0.5)`、`pow(-8, 2)`、`exp(0) == 1.0`、`log(e) == 1.0`、`log(8, 2) == 3.0`、`log2(1024) == 10.0`、`log10(1000) == 3.0`、`pow(0, 0) == 1.0`、`exp` 大参数返回 `inf`（`math.isInf(math.exp(1000.0))`）。
- `math_trig.ms`（正向）：`sin(0)`、`cos(0)`、`sin(pi/2)`、`tan(pi/4)`、`asin(1) == pi/2`、`acos(-1) == pi`、`atan(1) == pi/4`、`atan2(1, 1) == pi/4`、`atan2(0, 0) == 0`，全部 epsilon 比较。
- `math_predicates.ms`（正向）：`isNaN`/`isInf`/`isFinite` 对 int、普通 float、`math.inf`、`-math.inf`、`math.nan` 的完整输入矩阵。
- `math_int_funcs.ms`（正向）：`gcd(12, 18) == 6`、`gcd(-12, 18) == 6`、`gcd(0, 5) == 5`、`gcd(0, 0) == 0`、`lcm(4, 6) == 12`、`lcm(0, 7) == 0`、`lcm(-4, 6) == 12`、`factorial(0) == 1`、`factorial(1) == 1`、`factorial(20) == 2432902008176640000`。
- `math_error_type.ms`（负向，预期退出码 1）：类型错误——`math.sqrt("x")`、`math.floor(true)`、`math.gcd(1.5, 2)`、`math.pow(2)`（参数个数）。
- `math_error_domain.ms`（负向，预期退出码 1）：定义域错误——脚本顺序触发其一即失败，按拆分为逐文件用例或首条断言触发：`math.sqrt(-1)`、`math.log(0)`、`math.log2(-3)`、`math.asin(2)`、`math.pow(-2, 0.5)`、`math.pow(0, -1)`、`math.log(8, 1)`、`math.factorial(-1)`。实现时每个负向情形一个独立小脚本（`math_err_sqrt.ms` 等），避免单脚本只覆盖首条。
- `math_error_overflow.ms`（负向，预期退出码 1）：`math.factorial(21)` 溢出报错；`math.lcm` 大数溢出、`math.abs` 的 `INT64_MIN` 情形视 int64 字面量表达能力单独成小脚本。

覆盖点对照：23 个函数每个至少一条正向断言；每条「定义域错误」列至少一条负向用例；int/float 参数约定（bool 拒绝、float 不隐式转 int、「保持数值种类」）均有专项断言。

## 验收标准

- [ ] `stdlib/ms_std_math.h` / `ms_std_math.c` 存在，guard 为 `MSLANG_STDLIB_MS_STD_MATH_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、唯一导出符号 `msStdMathRegister`）。
- [ ] 常量 `pi`/`e`/`inf`/`nan` 与 07-stdlib §4 的 23 个函数全部可用，脚本名逐字一致（含 `isNaN`/`isInf`/`isFinite` 小驼峰）；常量不依赖非标准的 `M_PI`/`M_E`。
- [ ] int/float 参数约定落地：数值函数接受 int 与 float（int 提升为 double），bool/其他类型报 `TypeError`；`gcd`/`lcm`/`factorial` 只接受 int，不隐式截断 float；`floor`/`ceil`/`trunc`/`abs` 保持入参数值种类。
- [ ] 定义域错误（`sqrt(-1)`、`log(0)`、`asin(2)`、`pow(0, -1)`、`pow(负数, 非整数)`、`factorial(负数)`、`log` 非法 base）全部在调用 C 库前显式检查并报 `ValueError`；实现不读 `errno`；`exp` 溢出返回 `inf` 而非报错。
- [ ] `gcd`/`lcm` 基于 uint64 幅度计算，`INT64_MIN` 输入无未定义行为；`factorial(20)` 精确，`factorial(21)` 与 `lcm` 溢出按约定报错。
- [ ] 模块经任务 18 的注册机制挂接，`import "math"` 可用且内建注册表优先级高于搜索路径；注册失败路径返回非 `MS_OK`。
- [ ] `tests/ms/stdlib/` 下「测试方案」清单的正/负向脚本全数经 `run_tests.py` 通过；`ctest --test-dir build` 包含本层。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建无 ASAN/内存错误报告；模块自身无堆分配（分配统计只来自结果对象，由 GC/状态回收）。
- [ ] 无 TBD/TODO 占位；与任务 18 的接口假定（注册表查询、stdlib 启动入口）在实现时已对齐。
