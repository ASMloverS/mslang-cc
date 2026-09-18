# 65 NaN-boxing 值表示

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v1.0 | ⬜ | [64 性能基准套件](64-benchmarks.md) |

## 任务目标

把 mslang 的值表示从「所有值都是 `MsObject*` 装箱对象」（`docs/language/08-vm-internals.md` §3 的首版决策）改造为 64 位 NaN-boxing 立即数表示：nil/bool/小整数/double 直接编码进一个机器字，不再触发堆分配；只有字符串、容器、函数等堆对象仍走指针。改造以新模块 `src/core/ms_value.h` / `src/core/ms_value.c` 为唯一抽象边界，VM、GC、容器、C API 全部经该模块的内联访问器读写值，并通过 CMake 选项 `MS_NAN_BOXING` 保留装箱旧实现作为回退构建。完成后，数值密集代码的堆分配降为零（大整数溢出除外），以任务 64 记录的装箱基线为对照达到既定提速门槛；全部既有测试在 `MS_NAN_BOXING=ON/OFF` 两种构建下回归通过。

## 设计依据

- `docs/language/08-vm-internals.md`
  - §3 对象模型：首版「所有值都是 `MsObject*` 装箱对象」，小整数（-256..4095）与短字符串驻留缓存；`int` 机器字内直存、溢出转堆上大整数；`MsTypeTag` 枚举与 `struct MsObjectHeader` 布局。本任务替换其中「一律装箱」这一决策，对象头与类型枚举保持不变。
  - §7 性能路线图第 1 条：「NaN-boxing / tagged pointer 消除装箱」，本任务即该条目落地。
  - §4：算术指令内联快路径——改造后快路径直接命中立即数，不再先解引用对象头。
  - §5：GC 根集合（各协程调用栈/求值栈、模块注册表、内建类型表、C API 显式根）——这些集合的元素类型全部由 `MsObject*` 变为 `MsValue`，标记入口需先判别立即数。
- `docs/language/09-c-api.md` §3/§5/§9：公开 API 以 `MsObject*` 为值句柄；§13 明确「主版本号变化允许破坏性修改」，v1.0 是切换公开值句柄类型的唯一合法窗口。
- `docs/language/10-c-style.md`：§2 格式化、§3 命名、§4 typedef 规则（`MsValue` 按不透明句柄处理，允许 typedef）、§6 内存纪律、§8 断言、§9 平台抽象（本任务的值表示模块是 §9 精神的同构应用：表示相关的条件编译集中在 `ms_value.h` 一处，不散布业务代码，见「详细设计」对回退开关的说明）。
- 任务 02 核心基础设施：`msAlloc/msRealloc/msFree`、`MS_ASSERT`。
- 任务 06 对象模型基础、任务 17 GC 标记-清除、任务 31 大整数、任务 43 协程：本文引用其职责边界作为改造范围盘点依据；这些任务文档中的接口名（如大整数构造函数 `msIntNewFromInt64`、GC 标记入口 `msGcMarkValue`）为本文假定命名，实现时以对应任务文档定名为准。
- 任务 64 性能基准套件提供基线数据与对比入口。任务 64 文档当前尚不存在，本文假定为 `bench/run_benchmarks.py` 驱动、产出 `bench/results/<label>.json` 报告、并以 `bench/baselines/v1.0-boxed.json` 保存装箱基线；基准项名称（如 `fib`、`nbody`）同为假定，实现时以任务 64 文档定名为准。

## 详细设计

### 位布局方案

目标平台为 64 位（x86-64、AArch64），用户态指针保证落在低 48 位（规范地址）。值字 `MsValue` 为 64 位无符号整数：

```
double    : 位模式不满足「标签窗口」判定的一切 64 位模式（含 ±Inf、±0、规范化 NaN）
标签窗口  : (v & MS_VALUE_QNAN) == MS_VALUE_QNAN，即高 16 位为 0x7FFC 起的保留区
  指针    : MS_VALUE_QNAN | 0<<48 | ptr48        （ptr48 = 指针低 48 位）
  小整数  : MS_VALUE_QNAN | 1<<48 | int48        （int48 = 48 位二进制补码）
  nil     : MS_VALUE_QNAN | 2<<48 | 0
  bool    : MS_VALUE_QNAN | 3<<48 | (0/1)
```

常量（`ms_value.h` 内 `#define`，编译期常量）：

```c
#define MS_VALUE_QNAN       0x7FFC000000000000ULL  // 标签窗口基址：指数全 1 + quiet 位
#define MS_VALUE_TAG_MASK   0x0003000000000000ULL  // 位 49..48
#define MS_VALUE_TAG_PTR    0x0000000000000000ULL
#define MS_VALUE_TAG_INT    0x0001000000000000ULL
#define MS_VALUE_TAG_NIL    0x0002000000000000ULL
#define MS_VALUE_TAG_BOOL   0x0003000000000000ULL
#define MS_VALUE_PAYLOAD    0x0000FFFFFFFFFFFFULL  // 低 48 位载荷
#define MS_VALUE_CANON_NAN  0x7FF8000000000000ULL  // 规范化 quiet NaN，判定结果仍是 double

#define MS_VALUE_INT_MIN    (-0x800000000000LL)        // -2^47
#define MS_VALUE_INT_MAX    0x7FFFFFFFFFFFLL           //  2^47 - 1
```

布局决策与约束：

- `MS_VALUE_QNAN` 本身（标签窗口内指针标签 + 零载荷，即 NULL 指针的编码）保留为 `MS_VALUE_INVALID` 哨兵，只用于调试期「未初始化槽位」标注，绝不作为合法值出现；指针编码时对 `NULL` 用 `MS_ASSERT` 拦截。
- 规范化 NaN：`0x7FF8_0000_0000_0000` 与标签窗口按位与结果为 `0x7FF8…`，不等于 `MS_VALUE_QNAN`，故仍判定为 double。**所有产生浮点结果的运算必须经 `msValueFromDouble` 出口**：若结果为任何 NaN（含负 NaN、带载荷 NaN），一律改写为 `MS_VALUE_CANON_NAN`。否则用户计算出与标签窗口同位模式的 NaN 会被误解码为指针，GC 随即崩溃。这是本方案的第一安全不变量。
- 指针编码：`MS_ASSERT(((uint64_t)ptr & ~MS_VALUE_PAYLOAD) == 0)`（debug 构建），解码取低 48 位，不做符号扩展。CMake 在配置期做编译探测：若目标平台用户态地址超出 48 位（如启用 5 级分页且进程用到高地址），`MS_NAN_BOXING=ON` 直接配置失败并提示改用回退构建，不做运行期兜底。
- 小整数范围收窄为 48 位有符号（-2^47 .. 2^47-1）。语言层面 `int` 是任意精度（08-vm-internals §3、任务 31），机器字范围只是内部表示边界，溢出照旧转堆上大整数，脚本语义不可观测，仅影响溢出发生的阈值。
- `-0.0` 原样存储（其位模式不在标签窗口内）；`0.0 == -0.0` 的比较语义由数值比较路径保证，与装箱版一致。
- 装箱时代的小整数缓存（-256..4095，08-vm-internals §3）整体删除——整数已是立即数；短字符串驻留缓存保留不变。

### MsValue 类型与双构建抽象边界

`MsValue` 按 10-c-style §4 的「不透明类型」处理，允许 typedef。两种构建共用同一套访问器 API，业务代码零条件编译：

```c
// NaN-boxing 构建（MS_NAN_BOXING=ON）
typedef uint64_t MsValue;
// 回退装箱构建（MS_NAN_BOXING=OFF）
typedef struct MsObject* MsValue;
```

条件编译只允许出现在两处：`src/core/ms_value.h` 顶部一个 `#ifdef MS_NAN_BOXING` 区块（选择 typedef 与全部 `static inline` 访问器的实现），以及 CMake 生成的公开配置头 `include/mslang/config.h`（向扩展模块导出同一个宏定义，保证扩展与解释器值表示一致）。这是对 10-c-style §9 的同构应用——值表示模块本身就是抽象层，与「平台代码集中在 `src/platform/`」同理；业务代码（VM、GC、容器、编译器）中禁止再出现 `MS_NAN_BOXING`。

公开访问器（全部为 `static inline`，回退构建下是恒等/解引用的平凡实现）：

```c
// Predicates.
bool msValueIsNil(MsValue v);
bool msValueIsBool(MsValue v);
bool msValueIsInt(MsValue v);
bool msValueIsFloat(MsValue v);
bool msValueIsNumber(MsValue v);    // int 或 float
bool msValueIsPtr(MsValue v);       // 堆对象

// Constructors (no allocation, cannot fail).
MsValue msValueNil(void);
MsValue msValueBool(bool b);
MsValue msValueInt(int64_t v);      // MS_ASSERT(v 在 48 位范围内)
MsValue msValueDouble(double d);    // NaN 一律规范化为 MS_VALUE_CANON_NAN
MsValue msValuePtr(MsObject* obj);  // MS_ASSERT(obj != NULL 且地址 < 2^48)

// Extractors (debug builds assert the predicate).
bool        msValueToBool(MsValue v);
int64_t     msValueToInt(MsValue v);
double      msValueToDouble(MsValue v);
MsObject*   msValueToPtr(MsValue v);

// Needs MsState: int64 溢出时经任务 31 的接口装箱为大整数，可 OOM（返回 MS_ERROR_OOM）。
MsResult msValueFromInt64(MsState* L, int64_t v, MsValue* out);

// Full type tag, same enum as 08-vm-internals §3; pointer path reads the object header.
MsTypeTag msValueType(MsState* L, MsValue v);

// Structural equality and hash for dict/set keys; bitwise-equal values always equal,
// numeric path falls back to value comparison (1 == 1.0 keeps boxed semantics).
bool     msValueEquals(MsState* L, MsValue a, MsValue b);
uint64_t msValueHash(MsState* L, MsValue v);
```

对象模型侧的配套改造：`struct MsObjectHeader`（`type` / `markColor` / `gcNext`）**保持不变**——它仍是堆对象的头部，与值表示解耦。GC 标记入口改为接受值：

```c
// Marks v if it is a heap pointer; immediates are a no-op.
void msGcMarkValue(MsState* L, MsValue v);
```

### 对象模型改造范围盘点

字段级原则：**装「任意脚本值」的字段一律改为 `MsValue`；类型上必是对象的字段保持 `MsObject*`**。按任务归属逐项盘点：

- 任务 05/07（字节码与编译器）：`MsProto.consts` 由 `MsObject**` 改为 `MsValue*`——常量池里的数字常量不再独立装箱，常量池内存占用与加载时分配数同步下降。序列化/调试输出路径同步适配。
- 任务 08（VM 执行核心）：求值栈、调用帧的局部变量槽、寄存器窗口全部由 `MsObject*` 数组改为 `MsValue` 数组；算术/比较指令（`MS_OP_ADD` 等）快路径改写为「双操作数均非指针」时直接对立即数计算（int 路径先按 48 位加法做溢出检测，溢出转 `msValueFromInt64`；float 路径出口经 `msValueDouble` 规范化 NaN），仅指针操作数走魔术方法分派。
- 任务 06（对象模型）：`MsList` 元素数组、`MsDict`/`MsSet` 的键值槽、`MsTuple` 元素、upvalue 的指向值、模块全局表、实例属性字典的值槽全部改 `MsValue`；`header.type`、`header.gcNext`、`MsString` 内部字节缓冲等必然是对象的字段不动。
- 任务 17（GC）：根集合扫描（调用栈、求值栈、模块注册表、内建类型表、C 根栈）与容器遍历的标记入口统一换成 `msGcMarkValue`；全对象链表与清除阶段不变。C 显式根栈（09-c-api §3）的元素类型改为 `MsValue`，立即数入根是合法空操作——这消除了嵌入代码为临时整数入根的必要，但 API 行为保持兼容（入根立即数无害）。
- 任务 31（大整数）：`int` 的统一接口边界从「机器字 63 位」改为「机器字 48 位」；大整数对象本身、其运算实现不变，仅收窄直存阈值与提升/降级转换点。
- 任务 43/46（协程与调度器）：`struct MsCoroutine` 的 `frames` 内槽位、`stack`、`result`、`waiters` 队列元素改 `MsValue`；调度器逻辑无语义变化。
- 任务 18/33（C API 与 C 扩展）：见下「兼容性风险」。
- 标准库各模块（任务 19–63）：不直接触碰值布局，仅因 C API 签名变化随编译适配，无独立改造项。

### 兼容性风险与回退开关

风险清单与对策：

1. **公开 C API 破坏**（最大风险）：`msGetGlobal`、`msNewInt`、`msCallObject`、`MsCFunction` 签名等以 `MsObject*` 为值句柄的 API 全部改收/返 `MsValue`。依据 09-c-api §13，v1.0 主版本窗口允许此破坏；`include/mslang/object.h` 等头文件统一切换，嵌入示例与扩展范例同步改写。`MsValue` 在两种构建下定义不同，故 `include/mslang/config.h` 导出 `MS_NAN_BOXING` 宏，扩展模块与解释器必须用同一构建选项编译——这沿用「扩展按头文件版本重新编译」的既有约定，不新增 ABI 承诺。
2. **NaN 位模式混淆**：见位布局小节的第一安全不变量；所有浮点出口强制经 `msValueDouble`，并在 debug 构建提供 `msValueCheckValid`（断言标签窗口内的值必是合法标签 + 合法载荷），VM 主循环在 debug 下对每条指令的栈顶抽样校验。
3. **48 位地址假设**：配置期探测 + 编码断言双保险；超限平台只能使用回退构建。
4. **指针身份语义**：`is` 运算符对堆对象比较指针（编码后比较值字，等价）；对立即数比较值字，与装箱时代的单例/缓存语义一致（nil/bool 全局唯一，小整数按值相等即身份相等——脚本无法区分，装箱时代的小整数缓存本就制造了同样的观测）。
5. **性能回退风险**：若个别基准（如纯对象图遍历）因标签判别劣化超过豁免阈值，凭回退开关兜底。

回退开关：CMake 选项 `MS_NAN_BOXING`（v1.0 起默认 `ON`）。`OFF` 时 `ms_value.h` 的全部访问器退化为装箱实现（`MsValue = MsObject*`，`msValueInt` 走原小整数缓存路径——该缓存在共享源码中保留，仅 NaN-boxing 构建不再使用），整条代码路径与 v0.4 等价。CI 对两种构建各跑一遍完整测试与基准，保证回退路径常绿；不提供运行期开关（运行期双表示会抵消全部收益）。

### 与任务 64 的对接

任务 64 的基准套件在本任务前已记录装箱基线（`bench/baselines/v1.0-boxed.json`，假定命名）。本任务的性能验收以该基线为分母，对比同一台机器、同一编译选项（除 `MS_NAN_BOXING`）下的 `bench/results/v1.0-nanbox.json`（假定命名）：

- 提速门槛：数值密集基准项（算术循环、递归调用类，如 `fib`/`nbody`）墙钟时间下降 ≥ 30%。
- 回归豁免线：其余所有基准项劣化 ≤ 5% 视为噪声；超出者必须定位原因并修复，或经评审记入任务文档的已知让步清单。
- 分配计数：数值密集基准的每轮堆分配次数（经任务 02 的分配统计读取）下降 ≥ 80%；理想情形下小整数/浮点运算路径零分配（大整数溢出与字符串/容器构造除外）。
- 峰值内存：全部基准的峰值 RSS 不得高于基线（值字 8 字节不变，但常量池与栈槽位中立即数不再伴随堆对象，预期下降）。

## 实现步骤

1. 引入抽象层（行为中立重构）：建 `src/core/ms_value.h` / `ms_value.c`，先只实现装箱版访问器（`MsValue = MsObject*` 下的平凡内联）；加 CMake 选项 `MS_NAN_BOXING`（默认 `ON`，本步强制 `OFF` 验证）与公开配置头 `include/mslang/config.h`。验证：`MS_NAN_BOXING=OFF` 构建通过，全量测试无变化。
2. 全仓值字段迁移：按「对象模型改造范围盘点」逐项把 `MsObject*` 值字段/数组改为 `MsValue`，读写点全部切换为访问器；VM 快路径、GC 标记、常量池、协程栈、C 根栈分批提交，每批跑全量测试。验证：每批在 `OFF` 构建下测试全绿（此时访问器是恒等实现，等价于纯重命名重构）。
3. 实现 NaN-boxing 版访问器：位布局常量、构造/判别/提取内联、NaN 规范化、编码断言、`msValueCheckValid` 调试校验、配置期 48 位地址探测。验证：`ON` 构建编译通过；debug 构建在 `msNewState` 初始化时跑编码/解码往返自检（`MS_ASSERT` 级别，不新增测试文件）。
4. 整数语义切换：`msValueFromInt64` 溢出转大整数、VM 算术快路径的 48 位溢出检测、删除小整数缓存的使用点（`OFF` 构建保留）。验证：`ON` 构建下 ±2^47 边界与溢出脚本行为与大整数路径一致。
5. GC 适配：根集合与容器遍历改走 `msGcMarkValue`，立即数标记为空操作。验证：`ON` 构建跑 GC 压力脚本（大量立即数与堆对象混合分配后强制回收），无悬挂指针、无泄漏（任务 02 分配统计归零）。
6. C API 迁移：公开头文件签名切换 `MsValue`、嵌入示例与扩展范例改写、`config.h` 宏导出核对。验证：C API 既有测试与嵌入示例在 `ON`/`OFF` 两种构建下均通过；用错误宏组合编译扩展时得到明确的编译期错误而非静默 ABI 错配。
7. 基准对比与调优：按「与任务 64 的对接」跑对比，未达门槛的基准项做剖析（重点：快路径分支布局、`msValueType` 内联失败点），迭代至达标。验证：对比报告落盘，门槛全部满足；结果追加为 `bench/baselines/v1.0-nanbox.json`（假定命名）供任务 66–68 复用。
8. 文档同步：更新 `docs/language/08-vm-internals.md` §3（值表示改为 NaN-boxing、删除「首版不做」表述、标注回退开关）与 §7 路线图第 1 条；更新 `docs/language/09-c-api.md` 的值句柄类型说明。验证：文档与实现一致，无残留「一律装箱」描述。

## 测试方案

本任务在任务 09 之后、任务 40 之后，新增测试一律为 ms 脚本（`tests/ms/`）并使用 `testing` 模块，由仓库根 `run_tests.py` 驱动（本任务只交付本设计文档，测试脚本随实现任务编写）。位布局正确性属 C 内部，不新增 C 测试文件，由 debug 构建的 `msValueCheckValid` 抽样校验与初始化自检覆盖。

新增测试文件与覆盖点：

- `tests/ms/nanbox/int_boundaries.ms`：±2^47-1 / ±2^47 边界值的算术、比较、位运算；边界处溢出转大整数后继续运算的正确性（`2^47 - 1 + 1` 的类型与值、大整数回减到边界内）；`//`、`%`、移位在边界两侧的一致性。
- `tests/ms/nanbox/float_nan.ms`：`0.0 / 0.0` 等路径产生的 NaN 是 float 类型、`nan != nan`、NaN 作 dict 键的行为与装箱版一致（哈希稳定、按身份/相等规则命中）、`-0.0` 与 `0.0` 的比较及字典键等价性、±Inf 与超大 double（接近标签窗口位模式的合法 double 值，如 `1.7976931348623157e308`）不被误判。
- `tests/ms/nanbox/value_identity.ms`：`is` 对 nil/bool/小整数/驻留字符串/普通堆对象的语义与装箱版逐项一致；`true is true`、`nil is nil`、跨作用域小整数恒等。
- `tests/ms/nanbox/gc_stress.ms`：构造立即数与堆对象混合的深容器图，循环分配触发多次 GC 后校验容器内容完整；显式根栈压入立即数（经内建路径间接触发）无害。
- `tests/ms/nanbox/const_pool.ms`：函数常量池中的数字/字符串常量在多次调用、闭包捕获、GC 后保持正确。

回归与基准：

- 既有 `tests/ms/` 全部脚本与 `tests/c/` 全部 C 单元测试在 `MS_NAN_BOXING=ON` 与 `OFF` 两种构建下各回归一遍，两套结果都必须全绿——`OFF` 构建同时验证回退路径没有随重构腐化。
- 基准对比：任务 64 的基准套件（假定入口 `bench/run_benchmarks.py`）在两种构建下各跑完整一轮，产出 `bench/results/v1.0-boxed.json` 与 `bench/results/v1.0-nanbox.json`（假定命名），按「与任务 64 的对接」的门槛逐项核对。

## 验收标准

- [ ] `src/core/ms_value.h` / `ms_value.c` 存在，guard 为 `MSLANG_SRC_CORE_MS_VALUE_H_`，头文件自包含；位布局常量、标签分配与本文「位布局方案」一致；`MS_NAN_BOXING` 条件编译只出现在该头文件与 `include/mslang/config.h`，业务源码零 `#ifdef MS_NAN_BOXING`。
- [ ] 全部浮点产生点经 `msValueDouble` 出口，NaN 规范化生效；debug 构建 `msValueCheckValid` 与初始化自检启用；指针编码带 48 位断言，配置期探测拒绝超 48 位用户态地址的平台。
- [ ] 「对象模型改造范围盘点」中列出的值字段全部迁移为 `MsValue`，`struct MsObjectHeader` 与 `MsTypeTag` 未改动；小整数缓存在 `ON` 构建下无使用点。
- [ ] 公开 C API 值句柄切换为 `MsValue`，嵌入示例与扩展范例同步更新，`include/mslang/config.h` 导出构建宏，09-c-api §13 的版本约定得到遵守。
- [ ] CMake 选项 `MS_NAN_BOXING`（默认 `ON`）生效；`OFF` 构建语义与 v0.4 装箱版等价。
- [ ] 「测试方案」列出的新增 ms 测试全部存在并通过；既有 `tests/ms/` 与 `tests/c/` 在 `ON`/`OFF` 两种构建下回归全绿。
- [ ] 基准对比达标：数值密集基准墙钟下降 ≥ 30%，其余基准劣化 ≤ 5%，数值密集基准每轮堆分配计数下降 ≥ 80%，全部基准峰值 RSS 不高于任务 64 装箱基线；对比结果落盘为基线文件。
- [ ] `docs/language/08-vm-internals.md` §3/§7 与 `docs/language/09-c-api.md` 已同步为新值表示，无过时描述。
- [ ] 构建产物只落在 `build/`；无 TBD/TODO 占位；对任务 02/31/64 等的接口假定在实现时已按对应任务文档对齐。
