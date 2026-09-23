# 31 任意精度整数（大整数）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [06 对象模型基础](06-object-model.md) |

## 任务目标

交付 mslang 的任意精度整数：在任务 06 的机器字（int64）`MsInt` 之上落地 `docs/language/08-vm-internals.md` §3 承诺的「符号 + `uint32_t` 数字数组」堆上大整数表示，并保持「两者统一走 `MsInt` 接口」——脚本作者完全无感知，小整数永远走机器字，溢出自动提升（upgrade）、结果回落自动降级（demote）。

具体交付：

- 新增 `src/object/ms_bigint.{h,c}`：`struct MsBigInt` 堆上表示（符号-幅度、32 位 limb、柔性数组单次分配）、统一 int 接口（查询/比较/哈希/转 double）、全部四则与位运算、移位、幂、解析与进制格式化。
- 扩展任务 06 的 `src/object/ms_object.{h,c}`：`struct MsInt` 增加表示判别字段；`msObjectValueEquals`/`msObjectHash`/`msObjectIsTruthy` 的 int 分支改经统一接口。
- 接入既有慢路径：任务 08 的 `msObjectBinaryOp`/`msObjectUnaryOp` 对 int×int 组合改调本任务接口（替换 v0.1 的溢出占位行为）；任务 07 编译器的整数字面量求值改经本任务解析入口；任务 10 的 `str(int)`/`hex`/`oct`/`bin`/`abs`/`pow`/`int(float)`/`int(str)` 走统一接口。
- 落地 09-c-api §5 的公开 API `msNewIntFromString`（任务 18 已明确推迟至本任务），供嵌入方与任务 35（strconv）使用。

完成后脚本可透明使用任意精度：`2 ** 100`、`50` 的阶乘、百位级十进制字面量、大整数的位运算与哈希（作 dict 键）均按值语义工作。本任务只交付设计文档；实现与测试代码随实现任务编写。

## 设计依据

- `docs/language/02-types.md`
  - §1/§3.1：`int` 为任意精度有符号整数，机器字优化存储、溢出自动提升大整数，对脚本作者透明。
  - §3.3：`int op float` → `float`；`int / int` → `float`；`int div int` → `int`（向下取整）；显式转换 `int("42")`、`int(3.9)` 截断；无 float → int 隐式转换。
  - §6：`==` 按值比较；`int` 可哈希；驻留（含小整数缓存）是实现细节。
- `docs/language/08-vm-internals.md`
  - §3：「`int` 内部表示：机器字内直存（不溢出时）；溢出后转为堆上大整数（符号 + `uint32_t` 数字数组），两者统一走 `MsInt` 接口」——本任务的表示与接口边界。
  - §4：算术指令内联快路径只处理「双操作数均为机器字 int」的情形，其余走类型分派——本任务不改快路径，只接管慢路径的 int 分支。
- `docs/language/09-c-api.md` §5：`msNewIntFromString(MsState* L, const char* text, int64_t base)` 已定名（本任务落地）；§8：返回 `MsObject*` 的 API 失败返回 `NULL` 且设置错误状态。
- `docs/language/10-c-style.md`：§1 include guard、§2 格式化、§3 命名、§4（枚举允许 typedef、内部结构体不 typedef）、§5 错误处理、§6 堆分配只经 `msAlloc/msRealloc/msFree`、§8 `MS_ASSERT`。
- `docs/language/11-project-layout.md` §1：`src/object/` 为类型实现目录；`tests/ms/` 脚本测试约定。
- 任务 06（已定稿）提供：`struct MsInt {header; int64_t value;}`、`msObjectAlloc`/`msObjectFree`、`msIntNew`、`msObjectValueEquals`/`msObjectHash`/`msObjectIsTruthy`、小整数驻留缓存。本任务在此之上扩展，不重定义对象头。
- 任务 08（已定稿）提供/假定：快路径溢出检出后转慢路径 `msObjectBinaryOp`；`msIntIsWord`/`msIntWordValue` 为其假定的判别/取值名，本文定稿为 `msIntIsWord`/`msIntAsInt64`（取值函数沿用任务 06 的 `msIntValue` 于具体结构体形态），实现时由任务 08 侧对齐。
- 任务 23（异常系统）提供 `ZeroDivisionError`/`ValueError`/`OverflowError` 异常类与 `msVmRaiseFmt`；本任务的运行时错误一律抛脚本异常。
- 任务 20（已定稿）提供共享增长缓冲 `struct MsStrBuf`（`src/object/ms_str_op.h`），本任务的格式化输出复用它。
- 任务 35（strconv，已定稿但实现晚于本任务）假定本任务提供 `bool msIntIsBig(MsObject*)`、`int64_t msIntAsInt64(MsObject*)`、`MsResult msBigintFormat(MsObject*, int base, struct MsStrBuf* out)` 与 `msNewIntFromString`——本文**定稿**这四个接口（前三个的形参按本仓库 `const` 纪律加 `const`，语义不变），任务 35 实现时按本文对齐。

对规范未写明处的显式处理（实现与评审时以此为据）：

1. **表示判别字段落在 `MsInt`/`MsBigInt` 共有前缀**。规范只给出对象头三字段（任务 06 已补 `tag`），int 的两种表示共用 `MS_TYPE_INT` 标签，必须另有判别位。本任务在任务 06 的 `struct MsInt` 中插入 `MsIntRepr repr` 字段，`struct MsBigInt` 以相同偏移的同名字段开头，O(1) 判别；这是对任务 06 结构体的显式修改（向后兼容：仅追加字段）。
2. **右移负数为算术移位（floor 语义）**。03-syntax §6 只列优先级未定移位语义；任务 08 的机器字快路径按 C 语义。任意精度语义必须与 Python 一致（`>>` 等价于对 2ⁿ 向下取整除法），否则 `div` 与 `>>` 在大整数上出现不一致。本文定为：右移恒为算术移位。
3. **移位计数上界放开**。任务 08 曾暂定「移位计数 ≥ 64 报运行时错误」——那是无大整数时的占位；本任务起 `1 << 100` 等合法并提升为大整数，仅负计数报错（`ValueError`，对齐 Python）。任务 08 的快路径行为（≥64 转慢路径）不变，语义变化发生在慢路径。
4. **`int(inf)` / `int(nan)`**：02-types 未规定。对齐 Python：`int(inf)` 抛 `OverflowError`、`int(nan)` 抛 `ValueError`。
5. **不设位数上限**。CPython 的 4300 位十进制转换上限与本语言规范均无对应条款；v0.2 不设人为上限，耗尽内存走统一 OOM 路径（`MS_ERROR_OOM`）。DoS 防护列入路线图。

## 详细设计

### 文件与模块边界

- 新增 `src/object/ms_bigint.h`（guard `MSLANG_SRC_OBJECT_MS_BIGINT_H_`）：`struct MsBigInt`、统一 int 接口与解析/格式化声明；自包含，include `<stdbool.h>`/`<stddef.h>`/`<stdint.h>`、任务 06 的 `"object/ms_object.h"` 与任务 20 的 `"object/ms_str_op.h"`；前向声明 `MsState`。
- 新增 `src/object/ms_bigint.c`：全部实现；幅度（magnitude）级辅助函数一律文件内 `static`，操作裸 limb 数组、不触碰 `MsObject`。
- 修改 `src/object/ms_object.h`：`MsIntRepr` 枚举与 `struct MsInt` 的 `repr` 字段（见下）；`ms_object.c` 的三个值语义函数 int 分支改经统一接口。
- 修改 `src/vm/ms_vm.c`（任务 08 文件）之外不改快路径：慢路径 `msObjectBinaryOp`/`msObjectUnaryOp`（其归属以实现时任务 06/08 的对齐为准）的 int×int、int×float 分支改调本任务接口。
- 修改任务 07 编译器的整数字面量求值点与任务 10 的相关内建函数（集成点清单见末节）。
- 内存纪律：大整数对象为**单次分配**（对象头 + limb 柔性数组，`msObjectAlloc(heap, MS_TYPE_INT, sizeof(struct MsBigInt) + cap * sizeof(uint32_t))`），`msObjectFree` 无需改动；对象无出向引用，任务 17 的标记阶段对其零开销。全部 limb 级临时计算直接写进新分配的结果对象，无额外堆缓冲（Knuth 除法的归一化副本除外，见「除法」）。

### 表示：机器字与大整数

```c
// In ms_object.h (extends task 06):
typedef enum {
  MS_INT_WORD,               // machine-word inline value (task 06 behavior)
  MS_INT_BIG                 // heap bigint; object is struct MsBigInt
} MsIntRepr;

struct MsInt {               // word form (repr == MS_INT_WORD)
  struct MsObjectHeader header;
  MsIntRepr repr;
  int64_t value;             // unchanged from task 06
};

// In ms_bigint.h:
struct MsBigInt {            // big form (repr == MS_INT_BIG)
  struct MsObjectHeader header;
  MsIntRepr repr;
  int sign;                  // +1 or -1; zero is never big (canonical form)
  size_t len;                // used limb count
  size_t cap;                // allocated limb capacity (construction slack)
  uint32_t limbs[];          // little-endian magnitude, base 2^32
};
```

不变量（`MS_ASSERT` 在 debug 构建守住）：

- `repr` 在两个结构体中同偏移（`_Static_assert` 校验），判别只读共有前缀。
- 规范化（canonicalization）：**任何能装进 int64 的值一律为 word 表示**——每个产生新值的出口（运算、解析、移位）统一经内部 `msBigintNormalize` 检查，回落即转为 `msIntNew` 的 word 对象（含驻留缓存）。推论：big 值的 `|v| >= 2^63`、`len >= 2`、最高 limb 非零、`sign != 0`；`0` 恒为 word。
- 大整数对象对脚本不可变（`int` 不可变，02-types §1）；`cap` 只为构造期就地增长服务，对象一经从构造函数返回即视为冻结，此后只读。

统一判别与取值（内联，供 VM 快路径与 strconv 复用）：

```c
// obj must have tag MS_TYPE_INT (MS_ASSERT in debug).
static inline bool msIntIsWord(const struct MsObject* obj);   // repr == MS_INT_WORD
static inline bool msIntIsBig(const struct MsObject* obj);    // !msIntIsWord
// Word form only (MS_ASSERT). Supersedes nothing: task 06's
// msIntValue(const struct MsInt*) stays for the concrete-struct form.
static inline int64_t msIntAsInt64(const struct MsObject* obj);
```

### 统一 int 接口：查询、比较、转换

```c
// Three-way compare of two int objects (any representation mix).
int msIntCompare(const struct MsObject* a, const struct MsObject* b);

// Exact compare of an int against a double (no precision loss; NaN handled
// per IEEE: every order comparison with NaN is false). Returns -1/0/1;
// *cmp is undefined when b is NaN and the function returns false via
// *isOrdered == false.
bool msIntCompareDouble(const struct MsObject* a, double b, int* cmp);

// Value hash: h(v) = mix64((uint64_t)(v mod 2^64)) truncated to 32 bits,
// where mix64 is task 06's integer finalizer. Identical rule for word and
// big forms (big: low two limbs, sign applied mod 2^64), so hash consistency
// across representations and with integral floats (task 06's rule extended:
// floats reduce their exact value mod 2^64 the same way) holds by
// construction. See 「相等与哈希」.
uint32_t msIntHash(const struct MsObject* obj);

// Exact-ish conversions. msIntToDouble rounds to nearest (ties to even);
// sets *overflow and returns +-HUGE_VAL when |v| exceeds double range.
double msIntToDouble(const struct MsObject* obj, bool* overflow);

// Explicit conversion float -> int (builtin int(float), 02-types §3.3):
// truncates toward zero into an int of any precision. Raises OverflowError
// for +-inf, ValueError for NaN; returns NULL with the error set.
MsResult msIntFromDouble(MsState* L, double v, struct MsObject** out);

// C API support: checked narrowing for msAsInt (09-c-api §5). Returns false
// (without raising) when a big value does not fit int64; the caller raises
// OverflowError.
bool msIntToInt64Checked(const struct MsObject* obj, int64_t* out);
```

`msIntCompareDouble` 的精确性：不做 `double` 化的模糊比较。处理顺序——NaN（无序）、±inf、符号不同即定；同号时按「指数（值域）→ 53 位尾数对齐比较 → 剩余低位只影响严格大于/小于」分解，与 CPython 的 long/float 比较同思路，保证 `2**53 + 1 > float(2**53)` 之类边界正确。

### 四则运算与幂

对外接口（`a`、`b` 均为 int 对象，任意表示组合；返回新 int 对象或 `NULL` 带错误状态）：

```c
struct MsObject* msIntAdd(MsState* L, struct MsObject* a, struct MsObject* b);
struct MsObject* msIntSub(MsState* L, struct MsObject* a, struct MsObject* b);
struct MsObject* msIntMul(MsState* L, struct MsObject* a, struct MsObject* b);
struct MsObject* msIntFloorDiv(MsState* L, struct MsObject* a, struct MsObject* b);
struct MsObject* msIntMod(MsState* L, struct MsObject* a, struct MsObject* b);
// Single-pass division producing both results (divmod builtin routes here).
MsResult msIntDivMod(MsState* L, struct MsObject* a, struct MsObject* b,
    struct MsObject** outQ, struct MsObject** outR);
// a ** b for int operands: b < 0 yields a float (1.0 / (a ** |b|), task 08's
// rule); b >= 0 exact. 0 ** 0 == 1. A big exponent with |a| > 1 raises
// OverflowError ("exponent too large") instead of attempting allocation.
struct MsObject* msIntPow(MsState* L, struct MsObject* a, struct MsObject* b);
struct MsObject* msIntNeg(MsState* L, struct MsObject* a);   // -a; abs(int) reuses via sign flip
```

实现要点（接口级，非完整代码）：

- **分派**：每个入口先按 `msIntIsWord` 组合分派——双 word 走带溢出检出的 int64 路径（与任务 08 快路径同规则，溢出才继续）；其余组合把 word 操作数升格为临时幅度视图（`{sign, len<=2, limbs[2]}` 栈上结构，不分配），进入幅度级算法。
- **幅度级原语**（`static`，操作 `const uint32_t* + len`）：`magCompare`/`magAdd`/`magSub`（课本算法，`uint64_t` 进位）、`magMul`（O(n²) 课本乘法；Karatsuba/Toom 列入性能路线图）、`magShiftLeft`/`magShiftRight`（按 limb + 位两级）、`magDivMod`（见下）。结果先按「最大可能 limb 数」分配 `cap`，计算后写 `len` 并 `msBigintNormalize`。
- **除法**：Knuth《TAOCP》卷二 Algorithm D，基数 2^32、`uint64_t` 中间量；归一化（左移使除数最高位置位）需要可写副本时，被除数/除数的副本是函数内 `msAlloc` 的裸 limb 缓冲，函数内释放（所有权明确，非 GC 对象）。单 limb 除数走 `magDivModSmall` 快路径（格式化解析也复用）。截断商/余后再按符号调整为 Python 的 floor 语义：商向 -inf 取整、余数取除数符号（`(r != 0) && ((a < 0) != (b < 0))` 时 `q -= 1, r += b`）。除数为零抛 `ZeroDivisionError`（含 `0 // 0`、`0 % 0`）。
- **幂**：平方-累乘；底数 `0`/`±1` 特判；指数先求值——big 指数仅当底数 ∈ {-1, 0, 1} 时可判（否则 `OverflowError`）；中间结果每步 `msBigintNormalize`，耗尽内存走 OOM。

### 位运算与移位

语义锚定**无限二进制补码**（与 Python 一致）：`~x == -x - 1`；`a >> n` 恒为算术移位（floor 语义）；负移位计数抛 `ValueError`（"negative shift count"）。

```c
struct MsObject* msIntBitAnd(MsState* L, struct MsObject* a, struct MsObject* b);
struct MsObject* msIntBitOr(MsState* L, struct MsObject* a, struct MsObject* b);
struct MsObject* msIntBitXor(MsState* L, struct MsObject* a, struct MsObject* b);
struct MsObject* msIntInvert(MsState* L, struct MsObject* a);
struct MsObject* msIntShl(MsState* L, struct MsObject* a, struct MsObject* count);
struct MsObject* msIntShr(MsState* L, struct MsObject* a, struct MsObject* count);
```

实现要点：

- 双 word 且结果不溢出（含 `<<` 的预检）走 int64 路径；其余进入补码算法。
- 补码算法：取 `w = max(bitLen(a), bitLen(b)) + 1` 位工作宽度，把两个符号-幅度操作数各自转为 `w` 位的二进制补码 limb 序列（负数：`2^w - |v|`），按位运算后再转回符号-幅度并规范化。工作缓冲是结果对象本身或函数内 `msAlloc` 的临时 limb 数组。该「扩展-运算-收缩」方案以一次额外转换换取三种运算共用一条路径，正确性优先。
- 移位：`count` 必须是 int 且非 big（big 计数对 `<<` 无实际意义，抛 `OverflowError`；对 `>>` 可直接按「超过 bit 长度」处理为 `0`/`-1`，实现取统一判错，行为以本文为准：big 计数一律先判——`>>` 时若计数 ≥ 被移值 bit 长度，结果为 `0`（非负）或 `-1`（负），不报错的宽松方案**不采用**，保持单一路径）。`a >> n` 负数按 `-ceil(|a| / 2^n)` 实现（`( |a| + 2^n - 1 ) >> n` 取负）。
- `<<` 的结果规模随计数线性增长，不设人为上限，OOM 兜底（「设计依据」第 5 条）。

### 解析与格式化（与 strconv 的边界）

```c
// Parses a pre-validated digit slice (no sign, no prefix, no underscores;
// every digit < base) into an int object. This is the workhorse shared by
// the compiler's integer-literal evaluation and by strconv (task 35).
// Never fails on value magnitude (arbitrary precision); MS_ERROR_OOM only.
MsResult msBigintParseDigits(MsState* L, const char* digits, size_t len,
    int base, bool negative, struct MsObject** out);

// Public C API (09-c-api §5, deferred by task 18 to this task). text is a
// NUL-terminated C string with an optional leading '-' followed by digits in
// base (2..36, ASCII letters case-insensitive); no base prefix, no
// underscores, no whitespace. Syntax errors raise ValueError and return
// NULL; OOM returns NULL with the OOM state set.
struct MsObject* msNewIntFromString(MsState* L, const char* text, int64_t base);

// Formats any int object in base (2..36, lowercase digits, '-' for
// negatives) into out. Ratifies task 35's assumed signature.
MsResult msBigintFormat(const struct MsObject* obj, int base, struct MsStrBuf* out);
```

边界划分（与任务 35 §5 的约定一致，本文为其定稿侧）：

- **语法校验不归本任务**。词法层（字面量）由 lexer 校验形式、编译器调用 `msBigintParseDigits`；数据层（运行时字符串）由 `msNewIntFromString`（C API 契约：无前缀、无下划线）或 strconv 的 `msNumConvScanInt`（base=0 前缀检测、下划线规则）校验后传入规范化数字切片。本任务只认「纯数字切片 + 进制 + 符号」，永不因数值大小失败。
- **解析算法**：按「每次能装进 `uint32_t` 的最大进制幂」分块累加——十进制每 9 位一块（10^9 < 2^32），经 `magMulAddSmall(mag, chunkBase, chunk)` 原地乘加；2/8/16 等 2 的幂进制直接按位打包进 limb，O(n) 无乘法。符号最后作用于规范化结果。
- **格式化算法**：word 表示走内部 uint64 取余快路径（语义同任务 35 的 `msNumConvFormatInt64`，此处自实现以避免反向依赖）；big 表示反复 `magDivModSmall` 除以最大进制幂（十进制 10^9）得逆序块再逐块补零输出；2 的幂进制直接按位提取。`str(int)`、`hex/oct/bin` 内建统一经 `msBigintFormat`（前缀与大小写排版归调用方）。

### 相等、哈希与真值

- **相等**：`msObjectValueEquals`（任务 06）的 int 分支改经 `msIntCompare(a, b) == 0`；int/float 跨标签分支改经 `msIntCompareDouble`（big int 与 float 的相等由此精确化，`2**70 == float(2**70)` 为真、`2**53 + 1 == float(2**53)` 为假）。指针捷径（驻留小整数）保留在最前。
- **哈希**：`msObjectHash` 的 int 分支改经 `msIntHash`。一致性规则（定稿，扩展任务 06 的既有约定）：任意整数值 `v`（word 或 big）与任何等于 `v` 的 float 都哈希到同一 `uint32_t`——规则为「精确值 mod 2^64 后经任务 06 的 64 位混合取低 32 位」，float 一侧从其精确二进制展开（尾数 × 2^指数）计算 mod 2^64。dict 键查找在数值键上的正确性依赖此不变量，测试专项锁定。
- **真值**：`msObjectIsTruthy` 的 int 分支——word 判 `value != 0`；big 由不变量知恒非零，恒真。

### 错误语义与内存/GC 纪律

| 情形 | 行为 |
|---|---|
| `//`、`%`、divmod 除数为零 | 抛 `ZeroDivisionError` |
| 移位计数为负 | 抛 `ValueError` |
| 移位计数为 big / 幂的 big 指数无法判定 | 抛 `OverflowError` |
| `int(inf)` / `int(-inf)` | 抛 `OverflowError` |
| `int(nan)` | 抛 `ValueError` |
| `msNewIntFromString` 语法错误 / base 越界 | 抛 `ValueError` |
| 任何分配失败 | `NULL` + `MS_ERROR_OOM` 错误状态 |

- 所有对外函数遵循 09-c-api §3 根纪律：参数自动是根；构造期跨分配点存活的中间对象经 `msRootPush/msRootPop`（任务 18 机制）保护——主要是「先建部分结果、再分配最终结果」的幂与补码路径。
- 裸 limb 临时缓冲（除法归一化副本、补码工作区）经 `msAlloc/msFree`、函数内释放，与 GC 对象严格区分。
- big 对象无出向引用，任务 17 标记-清除无需改动；`msObjectFree` 单次 `msFree` 即完整回收。

### 对既有任务的集成点清单

1. **任务 06（ms_object）**：`struct MsInt` 加 `repr` 字段；`msIntNew` 填 `MS_INT_WORD`；`msObjectValueEquals`/`msObjectHash`/`msObjectIsTruthy` 的 int 分支改经统一接口。
2. **任务 08（ms_vm）**：快路径不变；慢路径 int×int 全运算（含比较序运算）改调 `msInt*` 接口，int×float 经 `msIntToDouble` 提升；「移位 ≥64 报错」的占位语义随慢路径替换而废止（见「设计依据」第 3 条）。
3. **任务 07（编译器）**：整数字面量常量折叠——词素（已去下划线判定由 lexer 保证形式合法）按进制前缀调 `msBigintParseDigits`；v0.1 的「字面量溢出报编译错误」占位废止，任意精度字面量直接入常量池（常量池去重经 `msObjectValueEquals`，自动兼容）。
4. **任务 10（内建函数）**：`str(int)`/`repr(int)` 经 `msBigintFormat` 十进制；`hex/oct/bin` 经其 16/8/2 进制；`abs` 经 `msIntNeg` 的符号翻转路径；`pow(a, b)`/`pow(a, b, m)` 中两参形式经 `msIntPow`（三参模幂列入路线图，v0.2 对 big 操作数报 `TypeError` 之外的语义不在本任务承诺——两参形式外的既有行为不变）；`int(float)` 经 `msIntFromDouble`、`int(str)` 经 `msNewIntFromString`；`hash(int)` 经 `msIntHash`。
5. **任务 18/35（C API 与 strconv）**：`msNewIntFromString`、`msIntIsBig`、`msIntAsInt64`、`msBigintFormat` 按本文定稿落地；任务 35 假定名与本文不一致处在实现时以本文为准。

## 实现步骤

1. 表示落地：`MsIntRepr`、`struct MsInt` 加字段、`struct MsBigInt`、判别内联函数、不变量 `_Static_assert`/`MS_ASSERT`；`msIntNew` 与任务 06 既有测试适配。验证：既有 int 行为全部保持（任务 06/08 测试不回退）。
2. 幅度级原语：`magCompare`/`magAdd`/`magSub`/`magMul`/`magShift*` 与 `msBigintNormalize`（big→word 降级）。验证：经脚本断言 `2**63 - 1`、`2**63`、`-2**63 - 1` 边界两侧的值与表示透明性（值相等、可继续运算）。
3. 统一接口查询/转换：`msIntCompare`/`msIntCompareDouble`/`msIntHash`/`msIntToDouble`/`msIntToInt64Checked`，接入 `msObjectValueEquals`/`msObjectHash`/`msObjectIsTruthy`。验证：跨表示相等、与 float 的精确比较、dict 数值键一致性脚本用例。
4. 加减乘与取负：`msIntAdd/Sub/Mul/Neg` 含分派与降级。验证：已知值（`2**100`、`50!` 经连乘构造比对常量）、符号矩阵、结果回落 word（如 `(2**70) - (2**70 - 5) == 5`）。
5. 除法：`magDivModSmall`、Knuth Algorithm D、floor 语义调整、`msIntFloorDiv/Mod/DivMod`。验证：符号矩阵（`(-7) // 2 == -4` 等在大整数上的对应值）、恒等式 `a == (a // b) * b + a % b` 与 `0 <= a % b < b`（b > 0）的随机化脚本断言、除零异常。
6. 位运算与移位：补码扩展算法、五个接口、移位计数规则。验证：`~x == -x - 1`、负数的 `& | ^` 与 `<<`/`>>` 已知值、`>>` 大计数得 `0`/`-1`、负计数 `ValueError`。
7. 幂：`msIntPow`（特判、负指数转 float、big 指数 OverflowError）。验证：`2**100`、 `(-2)**101`、`0**0 == 1`、`2 ** -3 == 0.125`。
8. 解析与格式化：`msBigintParseDigits`（分块累加 + 2 幂进制打包）、`msNewIntFromString`、`msBigintFormat`（含 word 快路径）；接入编译器字面量与 `str/hex/oct/bin/int()`。验证：百位级十进制字面量、各进制往返 `int(str) / format → parse`、任务 35 假定接口的可用性。
9. VM 慢路径与内建函数集成（集成点清单第 2、4 条）：`msObjectBinaryOp` int 分支、int×float 提升、`msIntFromDouble`、`msAsInt` 窄化 OverflowError。验证：溢出运算无报错的端到端脚本、`int(1e300)` 精确截断、`int(inf)`/`int(nan)` 异常。
10. 全量回归与平台验证：编写「测试方案」全部脚本，`python run_tests.py` 全绿；任务 06/08/10 既有测试不回退；Win/Linux/macOS × Debug/Release 构建通过，Debug（ASAN / `/RTC`）无内存错误与泄漏；构建产物只在 `build/`。

## 测试方案

本任务编号 ≥ 09，一律用 ms 脚本测试（`tests/ms/bigint/`），由仓库根 `run_tests.py` 驱动。任务 40（testing 模块）之前断言用内建 `assert` + `print`，成功脚本末尾 `print("ok: <用例名>")`；负向用例经 `try/except`（任务 23 已落地）在正向脚本内断言异常类型。已知值以预先计算的十进制/十六进制常量字符串经 `str()`/`hex()` 比对锁定。本任务只交付方案与清单，测试实体随实现步骤编写。

测试文件清单与覆盖点：

- `word_boundary.ms`：int64 边界邻域（`9223372036854775807 + 1`、`-9223372036854775808 - 1`、`INT64_MIN * -1`、`INT64_MIN // -1`）；提升后与字面值相等（`9223372036854775807 + 1 == 9223372036854775808`——右侧字面量本身即 big）；降级（大数运算结果回落后与 word 字面相等且可作 `for` 计数等普通用途）；驻留缓存边界（-256/4095）不受 big 引入影响。
- `arithmetic.ms`：`2**100`、`2**64`、50 连乘阶乘、`(2**70 + 3) * (2**65 - 7)` 等对常量字符串；`+ - *` 符号矩阵；`//`/`%` 的 Python floor 语义在大整数上的符号矩阵（`(-7) // 2 == -4`、`7 % -3 == -2` 的 big 对应值）；恒等式 `a == (a // b) * b + a % b` 与 `0 <= a % b < b`（正除数）对一批确定性构造的大操作数成立；`divmod`（若 tuple 尚不可用则跳过，tuple 属任务 32，本用例在该集成前以 `//`+`%` 双算替代）；**`/`** 仍得 float（`(2**70) / 2 == 2.0**69`）。
- `bitwise_shift.ms`：`~x == -x - 1` 对 word/big/负数成立；`& | ^` 在负数与 big 上的已知值（如 `(2**100 + 0xFF) & 0xFF == 0xFF`、`-1 & (2**100) == 2**100`）；`1 << 100 == 2**100`；big 左移/右移互逆（`(a << 70) >> 70 == a`）；负数右移 floor 语义（`-7 >> 1 == -4`、`-(2**100) >> 99 == -2`）；`>>` 超过 bit 长度得 `0`/`-1`；负计数抛 `ValueError`。
- `compare_hash.ms`：跨表示六运算比较（word vs big、big vs big，含符号）；与 float 的精确比较（`2**53 + 1 > float(2**53)`、`2**70 == float(2**70)`、`nan` 不参与相等）；dict 键一致性——`{2**70: "big"}` 可用 `float(2**70)` 与同值 big 字面量命中、`hash(2**70) == hash(float(2**70))`；word 与 float 的既有哈希一致性不回退（`hash(42) == hash(42.0)`）。
- `parse_format.ms`：各进制字面量（`0x`/`0o`/`0b`）超出 int64 的解析；百位级十进制字面量；`str(big)` 对常量字符串精确比对；`hex/oct/bin` 大整数输出（如 `hex(2**100)` 为 `"0x1"` 后随 25 个 `0`）；`int(str(big))` 与 `int("0x...", 16)` 风格不可用时以 `strconv` 之外的既有入口（`int(s)` 十进制）做往返；负值格式化带 `-`。
- `error_semantics.ms`：big 操作数除零/模零抛 `ZeroDivisionError`；负移位抛 `ValueError`；`int(inf)`/`int(-inf)` 抛 `OverflowError`、`int(nan)` 抛 `ValueError`（`math.inf`/`math.nan` 可用，任务 21）；`2 ** (2**63)` 抛 `OverflowError`；`int("12x")` 抛 `ValueError`（经任务 10 内建路径）。
- `float_mix.ms`：`int op float` 提升为 float（`(2**70) + 0.5`、`(2**70) * 1.0`）；big int 转 double 的舍入与上溢（`(2**2000) * 1.0 == math.inf`）；`int(1e300)` 精确截断回 big（与 `10**300 // (10**0)` 构造值比较其数量级与末位特征——精确黄金值以常量字符串经 `str()` 比对）。

## 验收标准

- [ ] `src/object/ms_bigint.{h,c}` 存在，guard 为 `MSLANG_SRC_OBJECT_MS_BIGINT_H_`，头文件自包含；对 `src/object/ms_object.h` 的修改仅限 `MsIntRepr` 与 `struct MsInt` 追加字段；全部代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsBigInt` 不 typedef、枚举值 `MS_` 大写蛇形、堆分配只经 `msAlloc/msRealloc/msFree`）。
- [ ] 表示不变量成立：word/big 共用 `MS_TYPE_INT` 标签、`repr` 同偏移判别、任何可容纳 int64 的值恒为 word（每个运算出口规范化）、`0` 恒为 word、big 最高 limb 非零；小整数驻留缓存行为不回退。
- [ ] 四则/位运算/移位/幂覆盖任意精度且语义正确：`//`/`%` 为 floor 语义、位运算为无限补码语义、`>>` 算术移位、负指数幂得 float；双 word 不溢出路径无大整数分配（任务 02 分配统计可证）。
- [ ] 机器字 ↔ 大整数自动升降级对脚本透明：溢出无错误、回落无残留表示差异（值相等、哈希一致、可比较）；int64 全部边界值（`INT64_MIN`/`INT64_MAX` 邻域）行为正确。
- [ ] 解析与格式化按本文边界落地：编译器字面量任意精度、`msNewIntFromString` 符合 09-c-api §5 契约、`msBigintFormat` 覆盖 2–36 进制；任务 35 假定的 `msIntIsBig`/`msIntAsInt64`/`msBigintFormat`/`msNewIntFromString` 四接口以本文定稿形态存在。
- [ ] 相等/比较/哈希满足一致性不变量：跨表示、跨 int/float（含超出 int64 的精确比较与哈希一致，`hash(2**70) == hash(float(2**70))`），dict 数值键行为正确。
- [ ] 错误语义表逐项落实（`ZeroDivisionError`/`ValueError`/`OverflowError` 各情形）；OOM 路径返回 `NULL` + 错误状态，无部分构造对象泄漏。
- [ ] `tests/ms/bigint/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿；任务 06/08/10 既有测试不回退；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告。
- [ ] 无 TBD/TODO 占位；对任务 06/07/08/10/17/18/20/23 的接口引用在实现时已按对应任务文档对齐定名；「设计依据」五条显式处理（repr 字段、算术右移、移位计数放开、`int(inf/nan)`、无位数上限）在实现与评审记录中可查。
