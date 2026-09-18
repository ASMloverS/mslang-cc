# 30 切片与下标完善

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [16 容器 list 与 dict](16-containers-list-dict.md) |

## 任务目标

交付 mslang 的切片语义与下标体系的收尾工作：

- 编译器接线：`MS_AST_SLICE`（parser 已在任务 04 产出，三分量均可空）编译为 `MS_OP_SLICE`，缺省分量编译为 `nil` 占位；
- 通用切片归一化原语 `src/object/ms_slice.{c,h}`：把 `(start, stop, step)` 三分量按 Python 语义归一化为确定的下标区间与结果长度，处理负下标、缺省值、越界钳制与 `step == 0` 错误；
- 内建序列类型的切片落地：`str`（按 Unicode 码点口径，返回新 str）与 `list`（返回新 list 浅拷贝）；`bytes`/`tuple` 的切片在任务 32 接入同一原语，本任务只提供可复用机制；
- 下标语义完善：统一负索引与越界规则（单下标越界抛 `IndexError`，切片永不越界只做钳制）；`str` 单下标（码点索引、负索引归一化）若前序任务未接通则由本任务补齐；
- list 切片赋值与切片删除：`xs[i:j] = ys`、`xs[i:j:k] = ys`（要求等长）、`del xs[i:j]`、`del xs[i:j:k]`（规范未明文，本任务按 Python 语义补齐，见「设计依据」歧义说明）。

完成后，脚本能写 `xs[1:3]`、`xs[::2]`、`"héllo"[1:3]`、`xs[::-1]`、`xs[:]`（浅拷贝）、`xs[1:3] = [9, 9]`、`del xs[::2]`，全部边界行为与 Python 对齐，并通过 `tests/ms/slicing/` 的 ms 脚本验证。本任务完成后任务 16/32 中 `MS_OP_SLICE` 的"not subscriptable with slice"占位分支被替换为真实语义。

## 设计依据

- [03-syntax.md](../language/03-syntax.md)
  - §6 优先级表：下标 `a[i]`、切片 `a[i:j:k]` 为第 2 级后缀运算（左结合）。
  - §6.1 下标与切片：`a[i]` 负索引从尾部计数；`a[start:stop]` 半开区间；`a[start:stop:step]`；`a[:]` 为浅拷贝。
  - §3 `assignStmt` 与 `delStmt`：`targetList` 含下标目标；切片是否可作赋值/`del` 目标规范未明文，本任务按 Python 语义补齐为**仅 list 支持**（设计决策，见下）。
- [02-types.md](../language/02-types.md)
  - §4 str：不可变 UTF-8 序列，`s[i]` 返回第 i 个 **Unicode 码点** 组成的单字符字符串——故 str 的一切切片索引同为码点口径。
  - §5.1 list：`xs[1:3]` 切片返回新 list、`xs[::2]` 带步长切片。
  - §5.2 tuple 语义（切片属本任务的机制复用方任务 32）；§5.3/§5.4：dict/set 不可下标为序列，切片一律抛 `TypeError`。
- [04-exceptions.md](../language/04-exceptions.md) §4 异常层级：`IndexError`（下标越界，`LookupError` 子类）、`ValueError`（`step == 0`、扩展切片赋值长度不符）、`TypeError`（分量类型错误、不可切片类型、对不可变序列切片赋值）。异常系统已在任务 23 落地，本任务全部错误点抛出真实异常对象。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.2：容器指令含 `MS_OP_SLICE`；[任务 05](05-bytecode-proto.md) 已定义该指令的枚举与 ABC 格式，语义细节由本任务定稿（任务 05 的既定分工）。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R、指针星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`、include guard）。
- [11-project-layout.md](../language/11-project-layout.md) §1：`src/object/` 与 `tests/ms/` 的目录位置。
- 任务 04（语法分析器）已产出 `MS_AST_SLICE`（`target/start/stop/step` 四字段，三分量可空）并解析全部 `a[:]`/`a[::2]`/`a[1:]` 形态；任务 16（容器 list 与 dict）提供 `struct MsList`、负索引归一化先例（`msListNormalizeIndex`）、`MS_OP_INDEX` 等指令的既有分派与切片占位分支；任务 23（异常系统）提供异常抛出 API；任务 26（for-in 迭代协议）提供迭代原语，供切片赋值接受任意可迭代右值。
- 任务 07（编译器）文档尚未存在，`MS_AST_SLICE` 的代码生成点与其内部接口名（发射函数、寄存器分配约定）为本文假定命名，实现时以对应任务文档定名为准。同理，str 单下标的既有归属（任务 06/09/10 之一）与 str 的 UTF-8 码点原语（假定声明于 `src/object/ms_str.h`：`msStrRuneCount`、`msStrByteToRuneIndex`、`msStrRuneToByteIndex`）以对应任务文档定名为准。

规范歧义的处理（实现时按本文口径）：

- **切片赋值/删除**：03-syntax 未提及 `xs[i:j] = ys` 与 `del xs[i:j]`。本任务按 Python 语义补齐：仅可变序列 list 支持；`step == 1`（含缺省）允许任意长度替换（原地伸缩），`step != 1` 要求右值元素个数与切片长度严格相等，否则抛 `ValueError`；`del` 切片对两种步长都合法。右值接受任意可迭代对象（任务 26 协议），先完整收集为临时 list 再拼接，保证 `xs[:] = xs` 这类自赋值正确。
- **str 的 `s[:]`**：浅拷贝对不可变类型无意义，`s[:]`（及任何覆盖全串的切片）返回**原对象**而非新对象；这与 Python 一致，且 02-types §6 已声明驻留是实现细节，脚本不得依赖 `is`。
- **实例切片**：02-types §8 定义 `__getitem__`/`__setitem__`/`__delitem__`，但语言没有 slice 值类型，无法把三分量打包传入魔术方法。v0.2 决策：`MS_OP_SLICE` 作用于 class 实例一律抛 `TypeError`（"not subscriptable with slice"）；实例切片协议（slice 对象或三参形式）列入路线图，由后续版本评估。
- **`step` 归一化的"下界之外"哨兵**：负步长的缺省 `stop` 语义为"起点之前"（含下标 0），无法用合法下标表示；实现以钳制后的 `-1`（序列内）表达，算法描述见「详细设计」第 2 节。

## 详细设计

### 1. 文件与模块边界

- `src/object/ms_slice.h`（guard `MSLANG_SRC_OBJECT_MS_SLICE_H_`）与 `ms_slice.c`：通用切片归一化原语 `msSliceUnpack` 与各序列类型的切片操作函数。头文件自包含，包含 `<stdbool.h>`/`<stdint.h>` 与任务 06 的对象模型头文件（假定 `"object/ms_object.h"`）。
- VM 侧：填充任务 08 分派循环中 `MS_OP_SLICE` 的各类型分支，并为切片赋值/删除新增两条指令（见第 4 节）；既有 `MS_OP_INDEX`/`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX` 的分支不改编码，只做下标语义完善（str 补齐，见第 5 节）。
- 编译器侧（任务 07 的模块内）：`MS_AST_SLICE` 的代码生成；赋值/`del` 目标的可赋值性检查若任务 04/07 未接纳 `MS_AST_SLICE`，本任务补齐（最小改动：可赋值目标种类 + 代码生成各一处）。
- 替换占位：任务 16 在 `MS_OP_SLICE` 上对 list/dict 的占位错误分支、任务 32 对 bytes/tuple 的占位分支——本任务落地 list/dict 分支；bytes/tuple 分支由任务 32 调用本任务的 `msSliceUnpack` 接入（任务 32 文档的"任务 30 落地后替换"即指此机制）。

### 2. 切片归一化原语

```c
// Normalized slice over a sequence of length len. All fields are in
// element units (list slots / str runes / bytes / tuple slots).
struct MsSliceRange {
  int64_t start;    // first selected index; may be -1 or len when length == 0
  int64_t stop;     // exclusive bound after clamping (step > 0) or the
                    // "before first" sentinel -1 (step < 0)
  int64_t step;     // never 0
  int64_t length;   // exact number of selected elements, >= 0
};

// Resolves nil-missing components and negative/clamped bounds per Python
// semantics. Raises TypeError for a non-nil non-int component and
// ValueError for step == 0; returns false on either (out is undefined).
bool msSliceUnpack(MsState* L, int64_t len, MsObject* start, MsObject* stop, MsObject* step,
    struct MsSliceRange* out);
```

算法（`len >= 0` 为元素口径长度）：

1. `step`：`nil` → 1；非 int → `TypeError`（"slice indices must be integers or nil"）；`0` → `ValueError`（"slice step cannot be zero"）。
2. `step > 0`：缺省 `start = 0`、`stop = len`。负 `start`/`stop` 加 `len`；随后各自钳制到 `[0, len]`。`length = stop > start ? (stop - start + step - 1) / step : 0`。
3. `step < 0`：缺省 `start = len - 1`、`stop` 为"下标 -1 之前"哨兵。负 `start`/`stop` 加 `len`；`start` 钳制到 `[-1, len - 1]`（上界 `len - 1`，下界 `-1`），`stop` 钳制到 `[-1, len - 1]`，哨兵用 `-1` 表示"含下标 0 在内的全程"。`length = stop < start ? (start - stop - step - 1) / (-step) : 0`（`stop` 为哨兵时按 `-1` 参与运算，自然覆盖下标 0）。
4. 空序列（`len == 0`）经上述流程自然得到 `length == 0`，无特判。
5. 一致性不变量（debug 下 `MS_ASSERT`）：`length == 0` 或 `start`/`stop`/`step` 驱动的逐次下标全部落在 `[0, len)`。

此函数是**唯一**的切片语义权威：VM 分支、list/str 切片函数、任务 32 的 bytes/tuple 切片、以及将来任何序列类型都经它归一化，禁止在别处复制边界逻辑。

### 3. 各内建类型的切片操作

全部声明于 `src/object/ms_slice.h`；`r` 参数必须是 `msSliceUnpack` 的产物（编程错误由 `MS_ASSERT` 防御）。

```c
// --- list (struct MsList from task 16) ---
// Returns a new list with the selected elements (shallow: elements shared).
MsObject* msListSlice(MsState* L, struct MsList* list, const struct MsSliceRange* r);
// Replaces the selected range with src's items. src is any iterable (task 26
// protocol); it is fully collected into a temp list first. step != 1 requires
// an exact length match (ValueError otherwise).
MsResult  msListSetSlice(MsState* L, struct MsList* list, const struct MsSliceRange* r, MsObject* src);
// Deletes the selected range in place.
MsResult  msListDelSlice(MsState* L, struct MsList* list, const struct MsSliceRange* r);

// --- str (rune-based; primitives from task 06's ms_str.h) ---
// Returns the sliced substring. A slice covering the whole string returns
// the original object (immutable; no copy needed).
MsObject* msStrSlice(MsState* L, MsObject* s, const struct MsSliceRange* r);
```

- `msListSlice`：以 `r->length` 为容量 `msNewList`；`step == 1` 时 `memcpy` 连续段，`step != 1` 逐槽拷贝；`length == 0` 返回新空 list（切片不返回原 list，可变类型必须拷贝，`xs[:]` 的浅拷贝语义由此保证）。
- `msListSetSlice`：
  1. 右值经任务 26 的迭代原语收集为临时 list `items`（`msRootPush` 入根），收集期间不触碰目标 list；
  2. `step == 1`：设旧段长 `k = r->length`、新段长 `n = items->len`。`n != k` 时先按差值 `memmove` 中段尾部（扩张前确保容量，收缩不缩容——沿用任务 16 的"不归还缓冲区"约定），再 `memcpy` 写入新元素，更新 `len`；
  3. `step != 1`：`n != r->length` 抛 `ValueError`（"attempt to assign sequence of size N to extended slice of size M"）；相等则按下标序列逐槽覆写，结构尺寸不变；
  4. `xs[:] = xs` 类自赋值因先收集后拼接而天然正确；`xs[:] = []` 等价清空（`len = 0`，保留容量）。
- `msListDelSlice`：`step == 1` 时 `memmove` 左移覆盖；`step != 1` 时按下标序列把存活元素压实（读写下标同向前进，单遍 O(len)）；只减 `len` 不缩容。元素对象不释放（所有权归 GC，同任务 16 约定）。
- `msStrSlice`：输入的 `r` 已是**码点**口径。实现分两遍：第一遍按 `r` 的步进把每个选中码点的字节偏移与字节数记入临时数组（`msAlloc`，长度 `r->length`），同时累加结果字节数；第二遍逐段 `memcpy` 装入 `msNewStringN` 的产物。快路径：`step == 1` 时只需求 `start` 与 `start + length` 两个码点的字节偏移（经假定的 `msStrRuneToByteIndex`），中间一段连续 `memcpy`。全串覆盖（`start == 0 && length == runeCount`）直接返回原对象。临时偏移数组用完即 `msFree`，失败路径逆序释放。

### 4. 指令与编译接线

`MS_OP_SLICE` 已在任务 05 定义为 ABC 无操作数格式。本任务定稿其栈约定与语义，并追加两条指令：

- `MS_OP_SLICE`：栈布局（自底向上）`target start stop step`，缺省分量由编译器压 `nil`。VM 弹 4 值，按 `target` 类型分派：
  - `MS_TYPE_LIST`：`msSliceUnpack(L, list->len, ...)` → `msListSlice`，压入结果；
  - `MS_TYPE_STR`：先求码点数（假定 `msStrRuneCount`），`msSliceUnpack` → `msStrSlice`，压入结果；
  - `MS_TYPE_DICT` 及其他一切类型（含 class 实例）：抛 `TypeError`（"not subscriptable with slice"），替换任务 16 的占位错误；
  - `MS_TYPE_BYTES`/`MS_TYPE_TUPLE`：任务 32 存在后由其填充分支（复用 `msSliceUnpack`），本任务时期这两个类型尚不存在。
- `MS_OP_SET_SLICE`（新增，ABC 无操作数）：栈布局 `value target start stop step`（RHS 先求值，与"右侧先整体求值"的赋值总则一致）。仅 `MS_TYPE_LIST` 合法：`msSliceUnpack` → `msListSetSlice`；其余类型抛 `TypeError`（不可变序列或不可切片）。
- `MS_OP_DEL_SLICE`（新增，ABC 无操作数）：栈布局 `target start stop step`。仅 list 合法（`msListDelSlice`），其余类型抛 `TypeError`。
- 两条新指令追加在任务 05 枚举的容器组内（`MS_OP_SLICE` 之后）；字节码无存档格式，重编号无兼容负担，任务 05 的元数据表（操作数格式、栈效应注释）同步补两行——该表的唯一权威是任务 05 文档，本任务实现时对齐其定名。
- 编译器（任务 07 模块内）对 `MS_AST_SLICE` 的生成：读上下文依序编译 `target`、三个分量（`NULL` → `MS_OP_LOAD_NIL`），发 `MS_OP_SLICE`；赋值目标上下文编译 RHS 后发 `MS_OP_SET_SLICE`；`del` 目标上下文发 `MS_OP_DEL_SLICE`。`a[i:j]` 在表达式与目标两种语境共用 parser 的同一 AST 节点，由编译器按语境分派。
- 可赋值性：任务 04/07 若未把 `MS_AST_SLICE` 列为合法赋值/`del` 目标，本任务在其可赋值目标检查中补入（复合赋值如 `xs[i:j] += ys` 不合法——切片结果无稳定左值语义，报既有"非法赋值目标"错误码；此限制在实现时与任务 04/07 的错误码体系对齐）。

### 5. 下标语义完善（`MS_OP_INDEX` 家族）

本任务统一并补齐以下规则（已有行为不改，只补缺漏）：

- 负索引：一切序列类型的单下标与切片分量在归一化层处理（单下标 `i < 0` 加 `len`，即任务 16 `msListNormalizeIndex` 的同款规则；切片分量见第 2 节）。
- 越界规则：**单下标**越界（归一化后仍在 `[0, len)` 外）抛 `IndexError`（list 既有行为；str 补齐同规则）；**切片**永不抛 `IndexError`，越界分量一律钳制（含 `xs[99:]`、`xs[-99:99]`、空序列切片），结果可为空序列。
- str 单下标：`s[i]` 返回第 i 个**码点**的单字符 str（02-types §4），负索引归一化，越界抛 `IndexError`。若前序任务（06/09/10，以定名为准）未接通 str 的 `MS_OP_INDEX` 分支或仍按字节口径，本任务补齐/修正：经 `msStrRuneToByteIndex` 定位字节偏移，截出该码点的 UTF-8 字节段装箱为新 str。
- 不可变序列（str，及任务 32 的 bytes/tuple）的 `MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX` 维持既有 `TypeError`；本任务不引入"切片赋值绕过不可变性"的任何路径（`MS_OP_SET_SLICE`/`MS_OP_DEL_SLICE` 对 str 同样抛 `TypeError`）。

### 6. 错误消息与 GC 纪律

- 错误消息沿用任务 16/23 的既定风格，固定文本：`"slice indices must be integers or nil"`（TypeError）、`"slice step cannot be zero"`（ValueError）、`"not subscriptable with slice"`（TypeError，dict/实例/其他类型）、扩展切片长度不符的 ValueError 消息含两侧尺寸。
- 根纪律（09-c-api §3）：`msListSetSlice` 收集右值期间的临时 list、迭代产生的中间元素，凡跨分配存活的局部 `MsObject*` 一律 `msRootPush`/`msRootPop`（LIFO）；`msListSlice`/`msStrSlice` 的结果对象在压栈前视为调用方新持有的引用，遵循 09-c-api 的转移语义。`msSliceUnpack` 自身不分配内存（纯整数运算 + 错误路径），无根义务。
- `msStrSlice` 的临时偏移数组经 `msAlloc`/`msFree`，不属 GC 堆对象，失败路径先释放再返回。

## 实现步骤

1. 建 `src/object/ms_slice.h` / `ms_slice.c` 骨架：`struct MsSliceRange` 与 `msSliceUnpack` 全量归一化算法（正/负步长、缺省、负分量、钳制、长度公式、`step == 0` 与非 int 错误）。验证：暂以 VM 分支接通前的最小脚手架（或临时调试入口）驱动；正/负步长、空序列、全越界组合的归一化结果与 Python 参照逐例比对。
2. 实现 `msListSlice`（step 1 快路径 + 步进拷贝）并接通 `MS_OP_SLICE` 的 list 分支，替换任务 16 占位。验证：ms 脚本 `xs[1:3]`、`xs[:]`、`xs[::2]`、`xs[::-1]`、空结果与越界钳制断言。
3. 实现 `msStrSlice`（两遍字节偏移收集 + step 1 快路径 + 全串返回原对象）并接通 `MS_OP_SLICE` 的 str 分支；dict/其他类型抛 `TypeError`。验证：ASCII 与多字节串（`"héllo"[1:3] == "él"`）、负步长反转、`s[:]` 内容相等断言。
4. 编译器接线：`MS_AST_SLICE` 表达式语境的代码生成（缺省分量压 `nil`）；若任务 04/07 未接纳切片目标则补可赋值性检查。验证：`a[i:j]` 全形态（`[:]`/`[::2]`/`[1:]`/`[:j]`）在脚本中求值正确。
5. 追加 `MS_OP_SET_SLICE`/`MS_OP_DEL_SLICE` 枚举与元数据（对齐任务 05 定名），实现 `msListSetSlice`（收集右值 → 伸缩/等长覆写两路径）与 `msListDelSlice`（连续段 memmove + 步进压实），接通 VM 分支。验证：`xs[1:3] = [9]`（伸缩）、`xs[::2] = ys`（等长）、长度不符 `ValueError`、`del xs[1:3]`、`del xs[::-1]`、`xs[:] = xs` 自赋值的脚本断言。
6. 下标完善：检查并补齐 str 的 `MS_OP_INDEX` 码点口径分支（含负索引与 `IndexError`）；核对 list 既有行为不变。验证：`"héllo"[-1] == "o"`、越界抛 `IndexError`（`try/except` 断言）。
7. 根纪律复查与内存收尾：切片赋值/构建中跨分配局部对象入根核对；Debug 构建（ASAN / `/RTC`）跑全部本任务测试无报告，`msCloseState` 后分配计数归零。
8. 全平台（Win/Linux/macOS）× Debug/Release 构建验证，`python run_tests.py` 全绿。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试；异常系统已在任务 23 落地，负向用例用 `try/except` 捕获并断言异常类型（`except` 块断言类型、应抛未抛路径显式 `assert(false)`），沿用任务 32 的约定；`testing` 模块（任务 40）尚未存在，正向断言用内建 `assert` + `print`。本任务只交付设计文档，脚本随实现编写。

测试文件清单（`tests/ms/slicing/`）与覆盖点：

- `list_basic.ms`：`xs[1:3]` 半开区间、`xs[:]` 浅拷贝（内容相等、`xs[:] is not xs`）、`xs[1:]`/`xs[:2]`/全缺省、空结果（`xs[2:2]`、空 list 切片）、越界钳制（`xs[99:]`、`xs[-99:99]`）不抛异常；末尾 `print("list basic ok")`。
- `list_negative_step.ms`：`xs[::-1]` 反转、`xs[3:0:-1]`、`xs[:2:-1]`（缺省 start 为末元素）、`xs[-1:-4:-1]`、负步长空结果（`xs[0:3:-1]`）、`xs[::2]` 与 `xs[1::2]`。
- `str_slice.ms`：ASCII 与多字节码点口径（`"héllo"[1:3] == "él"`、`"héllo"[1] == "é"` 的单下标码点断言）、负索引（`"héllo"[-1] == "o"`）、负步长反转多字节串、`s[:]` 内容相等、空串切片、越界钳制不抛异常。
- `slice_assign.ms`：`xs[i:j] = ys` 的缩短/等长/加长三向替换、`xs[2:2] = [9]` 中段插入、`xs[:] = []` 清空、`xs[:] = xs` 自赋值、右值为非 list 可迭代对象（以任务 26 既有可迭代类型为准，如 `range`）；`del xs[i:j]`、`del xs[:]`、`del xs[::2]`。
- `slice_assign_step.ms`：`xs[::2] = ys` 等长覆写逐值断言；长度不符抛 `ValueError`（长/短两向）；`xs[::-1] = ys` 逆序覆写。
- `errors.ms`：`try/except` 断言——`step == 0` 抛 `ValueError`；分量非 int（`xs[1.5:]`、`xs["a":2]`）抛 `TypeError`；dict 切片（`{"a": 1}[1:2]`）抛 `TypeError`；str 切片赋值/删除（`s[0:1] = "x"`、`del s[0:1]`）抛 `TypeError`；list/str 单下标越界抛 `IndexError`；切片越界**不**抛（对照断言正常返回）。
- `compile_forms.ms`：编译形态回归——切片作为表达式、赋值目标、`del` 目标、嵌套后缀链（`m["k"][1:3]`、`f()[::2]`）、切片分量本身是复杂表达式（`xs[f():g(x):-1]`）。

每个文件末尾以 `print("<name> ok")` 收尾；错误消息文本不纳入断言（`run_tests.py` 不比对 stderr），实现时人工抽查一次各 `TypeError`/`ValueError`/`IndexError` 消息内容。

## 验收标准

- [ ] `src/object/ms_slice.{c,h}` 存在，guard 为 `MSLANG_SRC_OBJECT_MS_SLICE_H_`，头文件自包含；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsSliceRange` 不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] `msSliceUnpack` 是唯一切片归一化权威：缺省分量、负分量、越界钳制、正/负步长、长度公式与 Python 语义逐例一致；`step == 0` 抛 `ValueError`、非 int 分量抛 `TypeError`；函数自身零分配。
- [ ] `MS_OP_SLICE` 语义定稿并接通：list 返回新浅拷贝 list、str 按码点口径返回新 str（全串切片返回原对象）、dict 及其他类型抛 `TypeError`；任务 16 的占位分支被替换。
- [ ] 切片赋值与删除：`xs[i:j] = ys` 支持任意长度替换（先收集右值，自赋值正确）、`xs[i:j:k] = ys`（`k != 1`）要求等长否则 `ValueError`、`del xs[i:j]` 与 `del xs[i:j:k]` 正确；仅 list 支持，其余类型抛 `TypeError`；`MS_OP_SET_SLICE`/`MS_OP_DEL_SLICE` 的枚举与元数据对齐任务 05 定名。
- [ ] 下标完善：str 单下标按码点口径、负索引归一化、越界抛 `IndexError`；切片对一切越界分量只做钳制、永不抛 `IndexError`；list 既有下标行为无回归（任务 16 测试保持绿色）。
- [ ] `MS_AST_SLICE` 在表达式/赋值/`del` 三种语境均正确编译，缺省分量以 `nil` 占位；复合赋值不接受切片目标（报既有"非法赋值目标"错误）。
- [ ] 跨分配存活的局部 `MsObject*` 全部遵守根纪律；`msCloseState` 后无残余分配计数；Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告；构建产物只落在 `build/`。
- [ ] `tests/ms/slicing/` 下「测试方案」全部清单项实现并全数通过，`python run_tests.py` 退出码为 0。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过。
- [ ] 无 TBD/TODO 占位；与任务 04/05/06/07/23/26 的接口假定（`MS_AST_SLICE` 字段、指令枚举与元数据、str 码点原语、编译器发射函数、异常抛出 API、迭代原语）在实现时已对齐对应任务文档的定名。
