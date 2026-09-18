# 20 标准库：strings

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [18 C API 基础](18-c-api-foundation.md) |

## 任务目标

交付 C 内建标准库模块 `strings`（`stdlib/strings/ms_mod_strings.h` / `stdlib/strings/ms_mod_strings.c`），完整实现 `docs/language/07-stdlib.md` §2 列出的全部 22 个名字（21 个函数 + `builder` 构造器）：大小写转换（`toUpper`/`toLower`/`title`）、切分与连接（`split`/`splitN`/`fields`/`join`）、查找（`contains`/`hasPrefix`/`hasSuffix`/`indexOf`/`lastIndexOf`/`count`）、`replace`、修剪（`trimSpace`/`trim`/`trimPrefix`/`trimSuffix`）、`repeat`、`padLeft`/`padRight`，以及高效拼接对象 Builder（`write`/`toString`）。

全部函数遵守 `docs/language/02-types.md` §4 的 str 语义：字符串是不可变 UTF-8 序列，一切索引、宽度、计数按 **Unicode 码点**口径；任何函数都不原地修改输入串，一律返回新串。为此本任务同时交付一层与内建 str 方法共享的纯 C 字符串算法层（`src/object/ms_str_op.{c,h}`），消除模块函数与 `s.toUpper()` 等方法之间的重复实现。完成后，ms 脚本可直接以 `strings.toUpper("abc")` 形式使用全部函数（v0.1 尚无 import，挂接方式见「详细设计」第 6 节），并经 `tests/ms/stdlib/strings/` 下的脚本测试验证。

## 设计依据

- `docs/language/07-stdlib.md` §0（模块分两类，`strings` 为 C 内建模块、置于 `stdlib/` 目录；函数命名小驼峰）、§2（strings 函数全集，本任务的范围唯一来源）。
- `docs/language/02-types.md` §4（str：不可变、UTF-8、按码点索引；内建方法清单 `s.len() s.toUpper() s.toLower() s.split(sep) s.join(xs) s.contains(sub) s.hasPrefix(p) s.hasSuffix(p) s.replace(old, new) s.trimSpace() s.indexOf(sub) s.format(...)`——与 strings 模块重叠的操作语义必须逐字一致，见「详细设计」第 7 节）。
- `docs/language/05-modules.md` §2/§7（内建 C 模块经内建注册表解析、优先级高于搜索路径；import 语句本身由任务 24 实现，v0.1 期间模块对象经全局命名空间暴露，见「详细设计」第 6 节）。
- `docs/language/09-c-api.md` §3（GC 根栈纪律：参数自动是根，跨分配存活的局部 `MsObject*` 必须 `msRootPush`/`msRootPop`）、§5（`msNewStringN`/`msAsCString`/`msStringLen` 等值构造与转换；`msAsCString` 缓冲区「下一次分配前有效」）、§6（`msLen`/`msListGet`/`msListAppend`）、§8（C 函数出错时设置错误并返回 `NULL`，VM 转为脚本异常；v0.1 异常系统未落地，错误以运行时错误形式呈现）、§9（`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule` 与 `mslangInit_<name>` 约定）、§10（`MsTypeDef`/`msDefineType`/`msCInstanceData`，Builder 类型的实现基础）。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- `docs/language/11-project-layout.md` §1（`stdlib/` 为 C 标准库模块目录、`src/object/` 为类型实现目录、`tests/ms/` 为脚本测试目录）、§4（三层测试策略中的脚本测试层）。
- `docs/tasks/README.md` 测试约定：本任务编号 ≥ 09 一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`，负向用例以 `<name>.exit` 同伴文件声明预期退出码（约定见 [09-minimal-interpreter.md](09-minimal-interpreter.md) 测试方案第 2 节），由仓库根 `run_tests.py` 驱动。
- 任务 06（对象模型基础）假定提供：str 类型对象（`MS_TYPE_STR`）、UTF-8 码点原语（解码/编码/码点计数，见「详细设计」第 3 节的假定接口）。
- 任务 18（C API 基础）假定提供：`msRegisterModule`、`msDefineType`、`msRootPush`/`msRootPop`、`msRaiseTypeError`/`msRaiseValueError` 等公开头文件（09-c-api §5/§8/§9/§10）的可调用实现，以及 v0.1 期间 C 标准库模块经 `msNewState` 初始化挂接的机制（任务 19 fmt 先行建立同一模式）。
- 任务 06、18 的设计文档本文撰写时尚不存在，上述接口名均为假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件布局与分层

```
stdlib/strings/
├── ms_mod_strings.h     # 模块对外声明：注册入口
└── ms_mod_strings.c     # 21 个 MsCFunction 包装、Builder 类型、MsModuleDef 注册表
src/object/
├── ms_str_op.h          # 共享算法层声明（内建 str 方法与 strings 模块共用）
├── ms_str_op.c          # 算法层实现
└── ms_case_table.c      # 生成的 Unicode 简单大小写映射范围表（static，仅被 ms_str_op.c 包含）
tools/
└── gen_unicode_tables.py    # 从 UnicodeData.txt 离线生成 ms_case_table.c；产物入库，构建不依赖网络
```

分层职责：

- **算法层**（`src/object/ms_str_op.*`）：纯 C 函数，只操作 `const char* + size_t` 字节切片与自备增长缓冲 `struct MsStrBuf`，不触碰 `MsObject`、`MsState` 与脚本层概念。strings 模块函数与内建 str 方法都是它的薄封装。
- **模块层**（`stdlib/strings/ms_mod_strings.c`）：`MsCFunction` 包装——参数个数/类型校验、从 `MsObject` 取出字节切片、调用算法层、把结果装箱为脚本对象。全部 `static`，唯一导出符号是注册入口。
- **数据表**（`ms_case_table.c`）：生成产物，头注释标明生成命令与 Unicode 版本，禁止手改。

### 2. 函数语义总表（唯一权威）

下表逐函数固定语义；凡 07-stdlib §2 未说明的边界行为，以本表为准（「码点」列指该函数的索引/宽度/计数口径）：

| 函数 | 语义 | 码点口径与边界 |
|---|---|---|
| `toUpper(s)` / `toLower(s)` | Unicode **简单**大小写映射（1:1），返回新串 | 不含多字符展开（`ß` 不变大写为 `SS`，其 Upper 映射为自身）；不含 locale 规则（土耳其语 i 不特殊处理） |
| `title(s)` | 按 Unicode 空白分词，每词首码点转大写、其余转小写 | 简化规则：标点不触发新词（与 Python `str.title` 的标点分词不同，有意从简）；`"héllo wORLD"` → `"Héllo World"` |
| `split(s, sep)` | 按分隔子串切分，返回 `list[str]` | `sep == ""` 时按码点切分（每码点一个元素，对齐 Go）；`s` 不含 `sep` 时返回单元素列表 `[s]` |
| `splitN(s, sep, n)` | 最多切出 `n` 段，末段含剩余原文 | `n < 0` 不限（等价 `split`）；`n == 0` 返回 `[]`；`n == 1` 返回 `[s]` |
| `fields(s)` | 按 Unicode 空白连续段切分，忽略首尾空白 | 全空白或空串返回 `[]` |
| `join(xs, sep)` | 以 `sep` 连接 `xs` 的元素 | `xs` 必须是 list 且元素全为 str，否则 TypeError；空列表返回 `""` |
| `contains(s, sub)` / `hasPrefix(s, p)` / `hasSuffix(s, p)` | 子串/前缀/后缀判断，返回 bool | 字节级比较即正确（UTF-8 子串匹配不会错切码点）；空子串恒真 |
| `indexOf(s, sub)` | 首个匹配位置，未找到返回 `-1` | 返回**码点索引**（与 `s[i]` 的码点索引一致）；`sub == ""` 返回 `0` |
| `lastIndexOf(s, sub)` | 末个匹配位置，未找到返回 `-1` | 码点索引；`sub == ""` 返回 `s` 的码点数 |
| `count(s, sub)` | **非重叠**匹配次数 | `sub == ""` 返回码点数 + 1（对齐 Go/Python） |
| `replace(s, old, new, n=-1)` | 非重叠替换，最多 `n` 处 | `n < 0` 替换全部；`n == 0` 原样返回；`old == ""` 时在码点边界处插入 `new`（共码点数 + 1 处，受 `n` 限制，对齐 Go） |
| `trimSpace(s)` | 去除两端 Unicode 空白 | 空白集为 Unicode White_Space 属性 |
| `trim(s, cutset)` | 去除两端属于 `cutset` 的码点 | `cutset` 按**码点集合**解释（非字节集）；`cutset == ""` 原样返回 |
| `trimPrefix(s, p)` / `trimSuffix(s, p)` | 去除一个匹配的前缀/后缀 | 不匹配时原样返回 |
| `repeat(s, n)` | 重复 `n` 次 | `n < 0` 报 ValueError；`n == 0` 返回 `""` |
| `padLeft(s, n, pad=" ")` / `padRight(s, n, pad=" ")` | 左侧/右侧以 `pad` 填充至码点宽度 `n` | `n` 为**码点**宽度；`s` 已达 `n` 码点时原样返回；`pad` 必须恰好 1 个码点，否则 ValueError |
| `builder()` | 构造 Builder 对象 | 方法：`write(s)` 追加并返回 Builder 自身（支持链式）；`toString()` 返回累积串，**不清空**内部缓冲，可继续写入 |

通用约定：

- 所有函数的首参数（被操作串）必须是 str，参数个数不符或类型不符报 TypeError；上表标注 ValueError 的情形报 ValueError。v0.1 异常类型尚未落地（任务 23），统一以 `msRaiseTypeError`/`msRaiseValueError` 置错误并返回 `NULL`，脚本侧表现为运行时错误、进程退出码 1。
- 输入串均为合法 UTF-8（str 类型构造时已校验，见任务 06）；算法层对非法序列不重复校验，内部不变量用 `MS_ASSERT` 防御。
- 多码点参数（`sep`/`sub`/`old`/`new`/`pad` 等）本身也是 str，天然是合法 UTF-8，字节级子串算法即可保证码点边界不错切。

### 3. UTF-8 码点原语（假定由任务 06 提供）

算法层依赖以下原语，假定声明于 `src/object/ms_str.h`（随 str 类型交付；本任务不重复实现，缺失时先在任务 06 的模块内补齐）：

```c
// Decodes the first rune in [p, p+len); returns bytes consumed, or 0 on
// invalid input (callers guarantee valid UTF-8; MS_ASSERT in debug).
int msUtf8DecodeRune(const char* p, size_t len, uint32_t* runeOut);

// Encodes rune as UTF-8 into out (>= 4 bytes); returns bytes written.
int msUtf8EncodeRune(uint32_t rune, char* out);

// Counts runes in [s, s+len).
size_t msUtf8RuneCount(const char* s, size_t len);

// Converts a byte offset known to be on a rune boundary into a rune index.
size_t msUtf8ByteToRuneIndex(const char* s, size_t len, size_t byteOff);
```

接口名为假定命名，实现时以任务 06 文档定名为准。

### 4. Unicode 数据表与生成脚本

- 大小写映射：`tools/gen_unicode_tables.py`（遵守仓库 Python 规范：PEP 8、全量类型标注、`X | Y` 联合语法）解析指定版本 UnicodeData.txt 的 Simple Uppercase/Lowercase Mapping 字段，合并连续区间，生成 `src/object/ms_case_table.c`：两张 `static const` 升序范围表（每条目 `{起始码点, 结束码点, 差值}` 或交错 ±1 的区间标记），运行时二分查找。表注释固定写入 Unicode 版本号与生成命令；产物入库，正常构建不运行该脚本。
- 空白集：White_Space 属性码点仅约 25 个，作为升序 `static const uint32_t` 数组内嵌 `ms_str_op.c`，二分判定；同由生成脚本输出到 `ms_case_table.c` 亦可，实现时二选一并在该文件注释说明。

判定接口（`ms_str_op.h` 声明，`ms_case_table.c`/`ms_str_op.c` 实现）：

```c
uint32_t msUnicodeToUpperRune(uint32_t r);  // identity when no simple mapping exists
uint32_t msUnicodeToLowerRune(uint32_t r);
bool     msUnicodeIsSpace(uint32_t r);
```

### 5. 共享算法层（src/object/ms_str_op.h）

include guard `MSLANG_SRC_OBJECT_MS_STR_OP_H_`，自包含（`<stdbool.h>`/`<stddef.h>`/`<stdint.h>` + 任务 02 的分配头）。内部结构体不 typedef：

```c
struct MsStrBuf {           // growable byte buffer; all growth via msRealloc
  char* data;               // msAlloc'd, owned by the buffer
  size_t len;
  size_t cap;
};

struct MsStrSlice {         // non-owning view into caller-held memory
  const char* data;
  size_t len;
};
```

缓冲操作：

```c
void     msStrBufInit(struct MsStrBuf* buf);
void     msStrBufFree(struct MsStrBuf* buf);
MsResult msStrBufPut(struct MsStrBuf* buf, const char* data, size_t len);   // append bytes
MsResult msStrBufPutRune(struct MsStrBuf* buf, uint32_t rune);              // append one rune
```

算法例程（输入均为合法 UTF-8；返回 `MsResult` 者仅可能失败于 `MS_ERROR_OOM`；输出语义见第 2 节总表）：

```c
// Case mapping and title-casing; append the result to out.
MsResult msStrOpToUpper(const char* s, size_t len, struct MsStrBuf* out);
MsResult msStrOpToLower(const char* s, size_t len, struct MsStrBuf* out);
MsResult msStrOpTitle(const char* s, size_t len, struct MsStrBuf* out);

// Byte-substring search. Returns the byte offset of the first/last match,
// or -1. Empty sub matches at 0 / len respectively.
int64_t msStrOpIndex(const char* s, size_t sLen, const char* sub, size_t subLen);
int64_t msStrOpLastIndex(const char* s, size_t sLen, const char* sub, size_t subLen);

// Non-overlapping match count; empty sub yields rune count + 1.
size_t msStrOpCount(const char* s, size_t sLen, const char* sub, size_t subLen);

// Split into an msAlloc'd array of slices (caller frees with msFree).
// maxPieces < 0: unlimited; 0: empty result; sepLen == 0: split into runes.
MsResult msStrOpSplit(const char* s, size_t sLen, const char* sep, size_t sepLen,
    int64_t maxPieces, struct MsStrSlice** out, size_t* outCount);

// Split on runs of Unicode whitespace; same ownership as msStrOpSplit.
MsResult msStrOpFields(const char* s, size_t sLen, struct MsStrSlice** out, size_t* outCount);

// Replace up to n non-overlapping occurrences (n < 0: all); appends to out.
MsResult msStrOpReplace(const char* s, size_t sLen, const char* oldS, size_t oldLen,
    const char* newS, size_t newLen, int64_t n, struct MsStrBuf* out);

// Non-owning trimmed views (no allocation).
struct MsStrSlice msStrOpTrimSpace(const char* s, size_t len);
struct MsStrSlice msStrOpTrimCutset(const char* s, size_t len, const char* cutset, size_t cutsetLen);

// Append n copies of s to out; n must be >= 0 (validated at the module layer).
MsResult msStrOpRepeat(const char* s, size_t len, int64_t n, struct MsStrBuf* out);

// Pad s on the left/right with padRune to a total rune width of width;
// appends s unchanged when already wide enough.
MsResult msStrOpPadLeft(const char* s, size_t len, int64_t width, uint32_t padRune, struct MsStrBuf* out);
MsResult msStrOpPadRight(const char* s, size_t len, int64_t width, uint32_t padRune, struct MsStrBuf* out);
```

核心算法要点：

- **子串查找**：朴素两遍扫描（memcmp 逐位置）+ 首字节快速跳过；v0.1 不引入 KMP/BM（串长以脚本场景为主，常数更重要；若基准测试（任务 64）证明显著再优化）。`msStrOpIndex`/`LastIndex` 返回字节偏移，模块层经 `msUtf8ByteToRuneIndex` 换算为码点索引——只在命中时换算一次，避免全程按码点扫描。
- **切分**：先一遍计数确定片数，再 `msAlloc` 切片数组并第二遍填充；切片指向输入串（模块层随即装箱为新 str 对象），算法层本身零字符串拷贝。
- **大小写转换**：单遍解码码点 → 查表 → 重新编码追加到 `out`；ASCII 快路径（`< 0x80` 时内联 `'a'-'z'` 判定，不查表）。
- **`trim`**：`cutset` 解码为码点序列后，对两端逐码点线性判定（cutset 通常极短；超过 32 码点时排序后二分，实现细节）。

### 6. 模块封装层（stdlib/strings/ms_mod_strings.c）

- include guard 形如 `MSLANG_STDLIB_STRINGS_MS_MOD_STRINGS_H_`（guard = 项目名 + 相对路径大写蛇形，`stdlib/` 不在 `src/` 下，故前缀为 `MSLANG_STDLIB_`）。
- 21 个包装函数均为 `static MsObject* stringsXxx(MsState* L, int64_t argc, MsObject** argv)`，统一骨架：

```c
static MsObject* stringsToUpper(MsState* L, int64_t argc, MsObject** argv) {
  if (argc != 1 || msTypeOf(argv[0]) != MS_TYPE_STR) {
    msRaiseTypeError(L, "strings.toUpper() requires exactly one str");
    return NULL;
  }
  const char* s = msAsCString(L, argv[0]);
  size_t sLen = msStringLen(argv[0]);
  struct MsStrBuf out;
  msStrBufInit(&out);
  MsResult result = msStrOpToUpper(s, sLen, &out);   // argv are roots; safe across allocation
  if (result != MS_OK) {
    msStrBufFree(&out);
    msRaiseRuntimeError(L, "strings.toUpper(): out of memory");
    return NULL;
  }
  MsObject* ret = msNewStringN(L, out.data, out.len);
  msStrBufFree(&out);
  return ret;
}
```

注意 `msAsCString` 的缓冲区「下一次分配前有效」（09-c-api §5）：`msStrOpToUpper` 内部的 `msRealloc` 不触碰 str 对象存储（对象内嵌缓冲，非该指针），但包装层一律「先取指针与长度、算法层跑完、再装箱」，中途不再调用其他 C API，使该窗口内无失效风险；实现时在每处取指针处评审此纪律。

- 模块表与注册：

```c
static const MsMethodDef stringsMethods[] = {
  {"toUpper", stringsToUpper, "toUpper(s) -> str"},
  // ... 21 entries, alphabetical, each with a one-line docstring ...
  {NULL, NULL, NULL},
};

static const MsModuleDef stringsModuleDef = {
  "strings", "string utilities", stringsMethods,
};

// Follows the 09-c-api §9 dynamic-loading naming convention.
const MsModuleDef* mslangInit_strings(void);

// Called by the stdlib registrar during msNewState (mechanism established by
// task 18/19): registers the module and, in v0.1 (before import, task 24),
// also binds the module object under the global name "strings".
MsResult msStringsRegister(MsState* L);
```

- v0.1 挂接方式：import 语句由任务 24 实现，本任务阶段 `msStringsRegister` 除调用 `msRegisterModule` 外，把模块对象以 `"strings"` 为名写入全局命名空间（与任务 19 对 `fmt` 的过渡策略保持一致），脚本直接以 `strings.toUpper(...)` 调用；任务 24 落地后是否保留该全局预绑定，以任务 24 文档定稿为准。

### 7. Builder 类型（C 自定义类型）

Builder 经 `msDefineType`（09-c-api §10）定义为脚本可见 C 类型；实例附加数据持有算法层缓冲：

```c
struct MsStrBuilder {       // instance data, obtained via msCInstanceData
  struct MsStrBuf buf;
};
```

- `MsTypeDef`：`name = "StringBuilder"`，`instanceSize = sizeof(struct MsStrBuilder)`，`init = NULL`（实例只由 `strings.builder()` 创建，创建时 `msStrBufInit`）；`finalize` 调 `msStrBufFree`（只释放 C 侧缓冲，不访问其他脚本对象，遵守 §10 约束）；`toString` 回调返回当前累积串（供 `print(b)` 与 `str(b)`）。
- 方法表（挂于 `MsTypeDef.methods`）：`write`（1 个 str 参数，追加后返回实例自身以支持链式）、`toString`（0 参，装箱 `buf` 内容为新 str，不清空）。两者均为普通 `MsCFunction`，接收者经约定的首参/当前实例机制取得（以任务 18 定名的 C 方法绑定机制为准）。
- 拼接效率语义：N 次 `write` 的总代价为 O（总字节数）（倍增扩容均摊），显著优于不可变 str 的 N 次 `+`（O(N²)）；这是 Builder 存在的理由，测试中以千次级拼接验证正确性（不做硬性计时断言）。

### 8. 与内建 str 方法的边界划分

- **归属边界**：02-types §4 的 12 个内建方法是 str 类型的方法（`s.method()` 形式，接收者语义），由对象模型任务（任务 06）交付；07-stdlib §2 的 22 个名字是 `strings` 模块的函数式接口（`strings.f(s, ...)`），由本任务交付。两侧都不是对方的别名转发——都是算法层的独立薄封装。
- **重叠操作**（10 个）：`toUpper`/`toLower`/`split`/`join`/`contains`/`hasPrefix`/`hasSuffix`/`replace`/`trimSpace`/`indexOf` 的语义必须与第 2 节总表逐字一致。参数映射差异：内建 `s.replace(old, new)` 等价 `strings.replace(s, old, new, -1)`；内建 `s.join(xs)` 的接收者是分隔符，等价 `strings.join(xs, s)`；内建 `s.split(sep)` 等价 `strings.split(s, sep)`。
- **代码复用方向**：共享算法层在 `src/object/`（任务 06 的地界），模块层在 `stdlib/strings/`（编号更靠后，单向依赖，无环）。任务 06 先行期间其内建方法若已内联实现重叠操作，本任务实现时将其重构为 `msStrOp*` 调用（行为不变，由任务 06 既有测试守住）；`s.len()`、`s.format(...)` 不属于重叠集（前者是对象模型基本操作，后者属任务 19 fmt 领域），不在本任务触碰。
- **模块独有**：`title`/`splitN`/`fields`/`lastIndexOf`/`count`/`trim`/`trimPrefix`/`trimSuffix`/`repeat`/`padLeft`/`padRight`/`builder` 只以模块函数存在，不回填为内建方法（02-types §4 的清单是封闭集）。

### 9. 内存与 GC 纪律清单

- 算法层分配：仅 `MsStrBuf` 增长（`msRealloc`）与切片的 `msAlloc` 数组两处；所有者的归属在每个函数文档注释写明（缓冲由调用者 `msStrBufFree`，切片数组由调用者 `msFree`）。
- 模块层：包装函数参数自动是根（09-c-api §3）；跨分配存活的中间对象仅出现在 `split`/`fields`（先建 list、循环装箱元素）与 `join`（取元素指针）中——list 在循环前 `msRootPush`、结束后 `msRootPop`；`join`/`split` 的元素指针窗口遵守第 6 节的「取指针后不调用其他 API」纪律。
- 装箱完成后立即 `msStrBufFree`；所有失败路径先释放算法层缓冲再置错误返回 `NULL`，资源按获取逆序释放。

## 实现步骤

1. 编写 `tools/gen_unicode_tables.py` 并离线生成 `src/object/ms_case_table.c`（简单大小写映射范围表 + White_Space 表）。验证：抽查若干已知映射（`'a'→'A'`、`'é'→'É'`、`'ß'` 无展开）与空白集成员（`U+0020`/`U+00A0`/`U+3000`）。
2. 建 `src/object/ms_str_op.h` 骨架：`struct MsStrBuf`/`struct MsStrSlice`、缓冲四件套、`msUnicodeToUpperRune`/`msUnicodeToLowerRune`/`msUnicodeIsSpace`。验证：任务 06 若已提供码点原语则直接引用，否则先按第 3 节补齐并对齐定名。
3. 实现大小写转换三例程（含 ASCII 快路径）。验证：算法层可被 str 内建方法链接调用（若任务 06 已有内建 `toUpper`，本步即完成重构为薄封装）。
4. 实现查找三例程（`msStrOpIndex`/`LastIndex`/`Count`）。验证：空子串、多字节串、非重叠计数的算法层行为符合总表。
5. 实现切分两例程（`msStrOpSplit`/`msStrOpFields`）。验证：片数计数两遍扫描的正确性、空 sep 按码点切分、`maxPieces` 三档。
6. 实现 `msStrOpReplace`/`msStrOpRepeat`/`msStrOpPadLeft`/`msStrOpPadRight`/两个 trim 例程。验证：总表中各边界情形（空 old 插入、n=0、宽度已达、单码点 pad）。
7. 建 `stdlib/strings/ms_mod_strings.{c,h}`：21 个 `MsCFunction` 包装、方法表、`stringsModuleDef`、`mslangInit_strings`、`msStringsRegister`，接入 `msNewState` 的标准库注册链（任务 18/19 的机制）与全局预绑定。验证：脚本 `print(strings.toUpper("abc"))` 输出 `ABC`。
8. 实现 Builder：实例数据结构、`msDefineType` 注册、`write`/`toString`/`finalize`。验证：链式 `strings.builder().write("a").write("b").toString() == "ab"`。
9. 编写 `tests/ms/stdlib/strings/` 全部测试脚本（见测试方案），`python run_tests.py` 全绿。
10. Win/Linux/macOS × Debug/Release 构建验证；Debug（ASAN / `/RTC`）下跑全部 strings 测试无内存错误、无泄漏（含 Builder 实例回收路径）。

## 测试方案

本任务只交付本设计文档；测试脚本随实现编写。一律使用 ms 脚本测试（`tests/ms/stdlib/strings/`，内建 `assert` + `print`——任务 40 之前不用 testing 模块），成功脚本末尾 `print("<用例名> ok")`，负向用例配 `<name>.exit`（内容为预期退出码 `1`）由 `run_tests.py` 驱动。测试内可直接使用内建 str 方法做交叉断言（任务 06/16 已交付）。

测试文件清单与覆盖点：

- `case.ms`：`toUpper`/`toLower`/`title` 的 ASCII、多字节（`"héllo"`、`"Grüße"` 验证 `ß` 不展开）、无大小写脚本（中文原样）、空串；`title` 的空白分词与连续空白。
- `search.ms`：`contains`/`hasPrefix`/`hasSuffix` 的正反例与空子串恒真；`indexOf`/`lastIndexOf` 在多字节串上返回**码点索引**（如 `strings.indexOf("héllo", "llo") == 2`）、未找到 `-1`、空子串返回 `0`/码点数；`count` 非重叠（`count("aaa", "aa") == 1`）、空子串为码点数 + 1。
- `split_join.ms`：`split` 常规/无匹配单元素/空 sep 按码点切分/首尾 sep 产生空片；`splitN` 的 `n>0`/`n==0`/`n<0`/`n==1`；`fields` 的多空白折叠、全空白返回 `[]`、Unicode 空白；`join` 的多元素/单元素/空列表/空 sep；与内建 `s.split(sep)`、`sep.join(xs)` 结果相等。
- `replace.ms`：`n=-1` 全部替换、`n` 限定次数、`n==0` 原样、无匹配原样、空 `old` 的码点边界插入（含 `n` 截断）、多字节 `old`/`new`；与内建 `s.replace(old, new)` 相等。
- `trim.ms`：`trimSpace` 含 Unicode 空白（`U+3000` 等）；`trim` 的码点集合语义（cutset 含多字节码点）、空 cutset 原样；`trimPrefix`/`trimSuffix` 的匹配与不匹配。
- `pad_repeat.ms`：`repeat` 的 `n=0`/`n=3`/多字节串；`padLeft`/`padRight` 的宽度按码点（多字节 `s` 与多字节 `pad`）、宽度已达原样、默认空格 pad。
- `builder.ms`：空 Builder `toString() == ""`；`write` 链式与返回值即自身；`toString()` 不清空可继续写；千次级 `write` 循环拼接结果与预期串相等（对照 `repeat` 产物）。
- `parity.ms`：总表 10 个重叠操作逐一对内建方法与模块函数做同输入等值断言（含多字节与空串边界），锁定第 8 节的一致性约定。
- `errors_type.ms`（配 `errors_type.exit`）：首参数非 str、参数个数错误、`join` 的列表含非 str 元素——任一即置错误退出码 1（脚本在首个断言前触发，文件逐一覆盖时拆为多个小脚本或在注释标明首触发点；实现时按首触发点拆分为 `errors_type_*.ms` 若干文件）。
- `errors_value.ms`（配 `errors_value.exit`）：`repeat(s, -1)`、`padLeft(s, 5, "ab")`（pad 非单码点）——同上分文件拆分。

## 验收标准

- [ ] `stdlib/strings/ms_mod_strings.{c,h}`、`src/object/ms_str_op.{c,h}`、`src/object/ms_case_table.c`、`tools/gen_unicode_tables.py` 存在；guard 分别为 `MSLANG_STDLIB_STRINGS_MS_MOD_STRINGS_H_` 与 `MSLANG_SRC_OBJECT_MS_STR_OP_H_`；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef）。
- [ ] 07-stdlib §2 的 22 个名字全部实现并可经 `strings.f(...)` 调用；语义与「详细设计」第 2 节总表逐条一致（码点索引、非重叠计数、空子串/空 sep/空 old 边界、`n` 各档位、pad 单码点约束）。
- [ ] 大小写转换为 Unicode 简单映射（1:1、无 `ß` 展开、无 locale 规则），映射表由 `tools/gen_unicode_tables.py` 生成并标注 Unicode 版本；空白判定用 White_Space 属性。
- [ ] 10 个重叠操作与内建 str 方法共享 `msStrOp*` 算法层且结果相等（`parity.ms` 通过）；内建方法侧的重构不改变行为，任务 06 既有测试保持绿色。
- [ ] Builder 经 C 自定义类型实现，`write` 返回自身支持链式，`toString` 不清空，`finalize` 释放内部缓冲且无泄漏。
- [ ] 模块层遵守 GC 根纪律（split/fields/join 的 list 入根、`msAsCString` 指针窗口内不调用其他 API）与失败路径逆序释放；堆分配全部经 `msAlloc`/`msRealloc`/`msFree`。
- [ ] `tests/ms/stdlib/strings/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿（含两个负向用例的预期退出码 1），`ctest --test-dir build` 并入通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告。
- [ ] 无 TBD/TODO 占位；对任务 06/18/19 的接口假定（码点原语、`msRegisterModule`/`msDefineType`、注册链与全局预绑定机制）在实现时已对齐定名。
