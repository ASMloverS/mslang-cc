# 38 标准库：path/filepath

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [18 C API 基础](18-c-api-foundation.md) |

## 任务目标

交付 C 内建标准库模块 `path/filepath`（`stdlib/filepath/ms_mod_filepath.h` / `stdlib/filepath/ms_mod_filepath.c`），完整实现 `docs/language/07-stdlib.md` §8 列出的全部 10 个函数：`join` / `split` / `dir` / `base` / `ext` / `abs` / `clean` / `isAbs` / `match` / `glob`。

本任务同时把「平台相关的路径知识」从模块层剥离、下沉为平台原语层（`src/platform/ms_path*.{c,h}`，Windows / POSIX 各一份实现文件，编译期择一）：分隔符集合、卷名（drive letter / UNC）解析、绝对路径判定、当前目录获取、目录枚举、绝对路径合成，全部经原语函数提供；`clean` / `split` / `join` / `match` / `glob` 等核心算法只写一遍、平台无关，经原语参数化。该层是 [11-project-layout.md](../language/11-project-layout.md) §1 预留的 `src/platform/` 目录的首个入住者，任务 42（平台抽象层，v0.3）落地时将其吸收对齐。

完成后，ms 脚本可以 `filepath.join("a", "b")`、`d, n := filepath.split(p)`、`filepath.glob("src/**/*.ms".replace("**", "*"))` 等形式使用全部函数（挂接方式见「详细设计」第 6 节），并经 `tests/ms/stdlib/filepath/` 下的脚本测试验证。

## 设计依据

- `docs/language/07-stdlib.md` §0（模块分两类，`path/filepath` 为 C 内建模块、置于 `stdlib/` 目录；函数命名小驼峰）、§8（filepath 函数全集——10 个函数，本任务范围的唯一来源；§8 未说明的边界语义一律对齐 Go `path/filepath`，理由：模块名直接取自 Go，且 mslang 整体语法为 Go 风格，「规范沉默处取 Go 语义」与任务 20 对 strings 的处理一致）。
- `docs/language/05-modules.md` §1（`import "path/filepath"` 绑定最后一段名 `filepath`）、§2（内建 C 模块经注册表解析，优先级高于搜索路径；`MS_PATH` 的目录列表分隔符 Windows 为 `;`、其余为 `:`——与本模块的平台分隔符知识同源，但本任务不消费 `MS_PATH`）。
- `docs/language/02-types.md` §4（str 为不可变 UTF-8 序列——路径串一律以 UTF-8 持有，Windows 侧只在系统调用边界做 UTF-8↔UTF-16 转换）、§5.2（tuple 为不可变定长序列，`split` 的返回类型）。
- `docs/language/03-syntax.md` §2（解包赋值 `a, b := pair` 作用于可迭代对象——`split` 返回 tuple 后脚本侧自然写作 `d, n := filepath.split(p)`）。
- `docs/language/09-c-api.md` §3（GC 根栈纪律：参数自动是根，跨分配存活的局部 `MsObject*` 必须 `msRootPush`/`msRootPop`）、§5（`msNewStringN`/`msNewBool`/`msAsCString`/`msStringLen` 等值构造与转换；`msAsCString` 缓冲区「下一次分配前有效」）、§6（`msNewList`/`msListAppend`）、§8（C 函数出错置错误并返回 `NULL`；`msRaiseTypeError`/`msRaiseValueError`/`msRaiseRuntimeError`）、§9（`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule` 与 `mslangInit_<name>` 约定）。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- `docs/language/11-project-layout.md` §1（`stdlib/` 为 C 标准库模块目录、`src/platform/` 为平台抽象层目录、`tests/ms/` 与 `tests/fixtures/` 的测试位置）。
- `docs/tasks/README.md` 测试约定：本任务编号 ≥ 09 一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`，负向用例以 `<name>.exit` 同伴文件声明预期退出码，由仓库根 `run_tests.py` 驱动。
- 任务 18（C API 基础）提供：公开头文件族中 `msRegisterModule`、`msNewStringN`、`msNewBool`、`msNewList`/`msListAppend`、`msRootPush`/`msRootPop`、`msRaise*` 的可调用实现，以及 C 标准库模块在 `msNewState` 初始化链上的注册机制（任务 19/20 已先行建立同模式：注册 + 过渡性全局预绑定）。
- 依赖边界声明：本任务**不**依赖任务 36（os 模块）与任务 42（平台抽象层）——`abs` 所需的当前目录与 `glob` 所需的目录枚举均由本任务的平台原语层直接实现（POSIX `getcwd`/`opendir`、Windows `GetCurrentDirectoryW`/`FindFirstFileW`）；任务 36 落地 `os.cwd`/`os.listDir` 时应复用本层原语（去重方向：os 调本层，而非反之），任务 42 落地时将本层并入其平台抽象。
- 假定接口（实现时以对应任务文档定名为准）：任务 06 经任务 20 确立的 UTF-8 码点原语 `msUtf8DecodeRune`（`match` 的字符类按码点判定所需）；任务 20 的可增长字节缓冲 `struct MsStrBuf` / `msStrBufInit` / `msStrBufPut` / `msStrBufFree` 与切片 `struct MsStrSlice`（`src/object/ms_str_op.h`——若本任务实现次序早于任务 20，则在本层自带等价定义，任务 20 落地后去重对齐）；任务 32 的 `msNewTuple` / tuple 元素写入接口（`split` 返回值所需；若早于任务 32，`split` 暂以 `msNewList` 返回 `list[str]` 两元素替代，任务 32 落地后切换为 tuple，解包语义不变）。

## 详细设计

### 1. 文件布局与分层

```
src/platform/
├── ms_path.h           # 平台原语 + 平台无关算法的声明（唯一头文件）
├── ms_path.c           # 平台无关算法：clean/split/dir/base/ext/join/match/glob
├── ms_path_win.c       # Windows 平台原语实现（编译期 #ifdef _WIN32 择入）
└── ms_path_posix.c     # POSIX 平台原语实现（#else 择入）
stdlib/filepath/
├── ms_mod_filepath.h   # 模块对外声明：注册入口
└── ms_mod_filepath.c   # 10 个 MsCFunction 包装 + MsModuleDef 注册表
tests/fixtures/
└── filepath_glob/      # glob 测试夹具（固定目录树，随实现步骤创建）
```

分层职责：

- **平台原语层**（`ms_path_win.c` / `ms_path_posix.c`）：唯一允许出现 `#include <windows.h>` / `<unistd.h>` / `<dirent.h>` 的地方；对外只暴露 UTF-8 接口，Windows 侧的 UTF-16 转换、宽字符 API、卷名解析全部封装在内。
- **算法层**（`ms_path.c`）：纯 C 函数，操作 `const char* + size_t` 字节切片与 `struct MsStrBuf`，不触碰 `MsObject`/`MsState`；一切平台差异经原语函数查询（分隔符、卷名长度等），代码本身一份两用。
- **模块层**（`ms_mod_filepath.c`）：`MsCFunction` 薄封装——参数校验、取切片、调算法层、装箱。全部 `static`，唯一导出符号是注册入口（模式与任务 20 的 strings 模块一致）。

### 2. 函数语义总表（唯一权威）

下表逐函数固定语义；例子以 POSIX（`/`）为主，Windows 差异见第 3 节。「分隔符」在 Windows 上指 `\` 与 `/`（输入均接受），输出一律规整为平台首选分隔符。

| 函数 | 语义 | 边界行为（对齐 Go） |
|---|---|---|
| `join(p1, p2, ...)` | 以平台分隔符连接任意个（≥1）路径元素，结果为 `clean` 后的串 | 空串元素跳过；全部为空返回 `""`；参数个数为 0 或任一元素非 str 报 TypeError |
| `split(path)` | 在最后一个分隔符处切开，返回 tuple `(dir, name)` | `dir` 为「卷名 + clean（末分隔符及其前）」，`name` 为末分隔符之后原文；`split("a/b/c")` → `("a/b", "c")`；`split("a/b/")` → `("a/b", "")`；`split("c")` → `("", "c")`；`split("")` → `("", "")`；`split("/")` → `("/", "")` |
| `dir(path)` | 路径去掉最后一个元素后的目录部分 | 恒等于 `split(path)` 的 `dir`；`dir("a")` → `"."`；`dir("")` → `"."`；`dir("/")` → `"/"` |
| `base(path)` | 路径的最后一个元素 | 先删除尾部全部分隔符再取末段；`base("a/b/")` → `"b"`；`base("")` → `"."`；`base("/")` → `"/"` |
| `ext(path)` | 末元素中最后一个 `.` 起的后缀（含点） | 无点返回 `""`；`ext(".bashrc")` → `".bashrc"`（对齐 Go，与 Python 的 `""` 不同，有意为之并在此固定）；`ext("a.b/c")` → `""`（点不在末元素） |
| `abs(path)` | 返回绝对路径 | 已绝对：`clean(path)`；相对：POSIX 为 `clean(join(getcwd(), path))`，Windows 委托 `GetFullPathNameW`（正确处理 `C:foo` 卷相对形式）；获取失败报 RuntimeError |
| `clean(path)` | 纯词法规整，不触碰文件系统 | 规则见第 5 节；`clean("")` → `"."` |
| `isAbs(path)` | 是否绝对路径 | POSIX：以 `/` 开头；Windows：`卷名 + 分隔符`（`C:\x`）或 UNC（`\\host\share`）为绝对；`C:foo` 与 `\foo`（无卷）均非绝对 |
| `match(pattern, name)` | 对单个名字做 glob 模式匹配，返回 bool | 语法：`*`（任意非分隔符序列）、`?`（单个非分隔符码点）、`[...]`（字符类，`[^...]` 取反，支持 `a-z` 区间）；两端锚定（全串匹配）；模式非法（未闭合 `[` 等）报 ValueError；**大小写敏感**（含 Windows，对齐 Go，见第 3 节说明）；`\` 转义仅 POSIX 有效（Windows 上 `\` 是分隔符，无转义，对齐 Go） |
| `glob(pattern)` | 按模式搜索文件系统，返回 `list[str]` | `*`/`?`/`[...]` 不跨分隔符；不支持 `**`（v0.2 无递归通配，对齐 Go）；结果按字节序升序排序；无任何匹配返回 `[]`（非错误）；模式非法报 ValueError；遍历中不可读的目录静默跳过（对齐 Go Glob 忽略 IO 错误）；相对模式返回相对路径（不附加 `./` 前缀），绝对模式返回绝对路径 |

通用约定：

- 除 `join` 外所有函数恰好 1 个 str 参数（`match`/`glob` 的参数个数按签名），个数或类型不符报 TypeError；上表标注 ValueError 的情形报 ValueError。v0.2 异常系统（任务 23）若已落地则为对应异常类，否则表现为运行时错误（退出码 1），与任务 20 的过渡约定一致。
- 所有路径入参均为合法 UTF-8（str 构造时已校验）；算法层不重复校验，`match` 的字符类判定按码点解码，内部不变量以 `MS_ASSERT` 防御。
- 路径不保证存在：`clean`/`dir`/`base`/`ext`/`join`/`split`/`isAbs`/`match` 为纯词法操作，不触碰文件系统；只有 `abs`（取当前目录）与 `glob`（枚举目录）有系统调用。

### 3. 平台差异与下沉策略

平台知识全部集中于平台原语层（`ms_path_win.c` / `ms_path_posix.c`），算法层只经下列原语感知差异：

```c
// src/platform/ms_path.h（include guard MSLANG_SRC_PLATFORM_MS_PATH_H_；自包含）

// ---- 平台原语（每平台恰好一份实现） ----

// Preferred separator: '\\' on Windows, '/' elsewhere. Static, never fails.
char msPathPlatSeparator(void);

// Is c a separator on this platform? Windows accepts both '/' and '\\'.
bool msPathPlatIsSeparator(char c);

// Length of the volume name prefix ("C:", "\\host\share") in [path, path+len);
// always 0 on POSIX.
size_t msPathPlatVolumeNameLen(const char* path, size_t len);

// Platform absolute-path test (semantics per the §2 table).
bool msPathPlatIsAbs(const char* path, size_t len);

// Absolute form of path (see §2 abs row). Appends UTF-8 to out.
// Fails with MS_ERROR_RUNTIME when the cwd is unavailable, MS_ERROR_OOM on OOM.
MsResult msPathPlatAbs(const char* path, size_t len, struct MsStrBuf* out);

// Lists the entries of dir (UTF-8 names, no "." / ".."). *namesOut is an
// msAlloc'd array of msAlloc'd NUL-terminated strings; the caller frees each
// element and the array with msFree. Unreadable/unopenable dir is NOT an
// error: returns MS_OK with *countOut == 0 (glob semantics, §2).
// MS_ERROR_OOM is the only failure.
MsResult msPathPlatReadDir(const char* dir, size_t dirLen, char*** namesOut, size_t* countOut);

// True when path exists on the filesystem (any kind). Never fails.
bool msPathPlatExists(const char* path, size_t len);
```

Windows 与 POSIX 的具体差异及其归置：

| 差异点 | POSIX | Windows | 归置 |
|---|---|---|---|
| 首选分隔符 | `/` | `\` | `msPathPlatSeparator`；算法层输出一律用它 |
| 输入分隔符集合 | 仅 `/` | `/` 与 `\` 均接受 | `msPathPlatIsSeparator`；`clean` 输出把 `/` 规整为 `\` |
| 卷名 | 无（恒返回 0） | drive letter（`C:`，含大小写）、UNC（`\\host\share`） | `msPathPlatVolumeNameLen` |
| 绝对路径判定 | 首字符 `/` | 卷名后接分隔符，或 UNC；`C:foo`、`\foo` 非绝对 | `msPathPlatIsAbs` |
| 绝对路径合成 | `getcwd` + join + clean | `GetFullPathNameW`（内建处理卷相对与 `.`/`..`） | `msPathPlatAbs` |
| 目录枚举 | `opendir`/`readdir`（跳过 `.`/`..`） | `FindFirstFileW`/`FindNextFileW`（跳过 `.`/`..`） | `msPathPlatReadDir` |
| 存在性检查 | `stat` | `GetFileAttributesW` | `msPathPlatExists` |
| 字符编码 | 原生 UTF-8 | 系统调用边界 UTF-8↔UTF-16（`MultiByteToWideChar`/`WideCharToMultiByte`，`CP_UTF8`） | 原语层内部，不外泄 |
| 路径列表分隔符（`MS_PATH`） | `:` | `;` | 本任务不消费；原语层预留常量注释，供任务 24/42 使用 |
| 匹配大小写 | 敏感 | 敏感（刻意不随文件系统——对齐 Go `filepath.Match`，保证同一脚本跨平台行为一致；大小写不敏感匹配列入路线图评估） | 算法层统一，无平台分支 |

Windows 长路径说明：`MAX_PATH`（260）限制不做 `\\?\` 前缀绕过，v0.2 承认该限制并在 `ms_path_win.c` 文件注释记录；超长路径下 `GetFullPathNameW`/枚举失败按上表错误约定处理。

### 4. 算法层接口（src/platform/ms_path.h 续）

```c
// ---- 平台无关算法（ms_path.c 单份实现，经原语参数化） ----

// Lexical cleanup per §5; appends the result to out. MS_ERROR_OOM only.
MsResult msPathClean(const char* path, size_t len, struct MsStrBuf* out);

// Joins count elements with the platform separator and cleans the result
// (§2 join row). Empty elements are skipped.
MsResult msPathJoin(const char* const* parts, const size_t* lens, size_t count, struct MsStrBuf* out);

// Splits at the final separator (§2 split row). Both outputs are non-owning
// slices into path or into a caller-provided scratch buffer:
// dirOut may reference scratch when cleaning was necessary.
void msPathSplit(const char* path, size_t len, struct MsStrBuf* scratch,
    struct MsStrSlice* dirOut, struct MsStrSlice* nameOut);

// dir/base/ext per §2. dir appends to out; base/ext return slices into
// scratch (base may need to strip trailing separators; ext never allocates).
MsResult          msPathDir(const char* path, size_t len, struct MsStrBuf* out);
struct MsStrSlice msPathBase(const char* path, size_t len, struct MsStrBuf* scratch);
struct MsStrSlice msPathExt(const char* path, size_t len);

// Result of a match attempt; bad pattern is distinct from non-match so the
// module layer can raise ValueError.
typedef enum {
  MS_PATH_MATCH_OK = 0,    // matched
  MS_PATH_MATCH_NO,        // did not match
  MS_PATH_MATCH_BAD_PATTERN,
  MS_PATH_MATCH_OOM
} MsPathMatchResult;

// Whole-name glob match (§2 match row); rune-wise for character classes.
MsPathMatchResult msPathMatch(const char* pattern, size_t patLen, const char* name, size_t nameLen);

// Filesystem glob (§2 glob row). *out receives an msAlloc'd array of
// msAlloc'd UTF-8 strings, sorted ascending by byte order; the caller frees
// each element and the array. MS_ERROR_SYNTAX marks a bad pattern (the
// module layer maps it to ValueError); MS_ERROR_OOM on OOM.
MsResult msPathGlob(const char* pattern, size_t patLen, char*** out, size_t* outCount);
```

### 5. 核心算法

**clean**（对齐 Go `Clean`，纯词法）：

1. 空输入 → 输出 `.`。
2. 跳过卷名前缀（原样保留）；记录根性：卷名后（或开头）紧跟分隔符则为有根。
3. 用输出缓冲模拟栈：逐段读取分隔符之间的元素——空元素与 `.` 丢弃；`..` 在栈非空且栈顶非 `..` 时弹栈，否则有根时丢弃（根之上无上级）、无根时保留入栈。
4. 有根结果以根分隔符开头，元素间以单个平台首选分隔符连接；结果为空时有根输出根分隔符、无根输出 `.`。

**split**：从末尾反向扫描最后一个分隔符（下界为卷名长度）；`name` 为其后原文切片；`dir` 为「卷名 + clean（卷名之后到末分隔符（含）的前缀）」，`clean` 结果写入 `scratch`。`dir(path)` 直接取 `split` 的 `dir`。

**base**：先截掉尾部连续分隔符（卷名+根分隔符的纯根输入例外，直接返回根分隔符）；空结果 → `.`；再从尾部找最后一个分隔符，返回其后切片。

**ext**：在末分隔符之后的末元素中找最后一个 `.`；找不到或末元素为空 → 空切片。

**match**（对齐 Go `Match`，两端锚定）：对 `pattern`/`name` 按码点双指针递归推进；`*` 依次尝试匹配 0..n 个非分隔符码点（回溯）；`?` 消费一个非分隔符码点；`[...]` 解析字符类（可选 `^` 取反、`-` 区间、POSIX 下 `\` 转义），对当前码点判定成员资格；其余字符逐字相等（Windows 上模式与名字中的分隔符互相匹配——`/` 与 `\` 视为同一字符）。`[` 未闭合、类内区间倒挂（`[z-a]`）、悬裸转义 → `MS_PATH_MATCH_BAD_PATTERN`。

**glob**：

1. 模式非法（经 `msPathMatch` 同款解析器预检）→ `MS_ERROR_SYNTAX`。
2. 按分隔符切分模式为组件序列；绝对模式以根（卷名+根分隔符）为起点，相对模式以空串为起点（保证结果不附加 `./`）。
3. 逐组件维护候选目录列表：组件不含魔法字符（`*?[`）时，候选 = 各父候选 join 该组件后仍存在（`msPathPlatExists`）者；含魔法字符时，对各父候选 `msPathPlatReadDir`，用 `msPathMatch` 过滤名字，join 入新候选。根组件（`.`/`..` 已在模式内按字面处理，不做 clean——与 Go 一致，glob 不规整模式）。
4. 末组件处理后的候选即结果；按字节序（`strcmp`）升序排序输出。UTF-8 的字节序与码点序一致，排序结果跨平台确定。

### 6. 模块封装层（stdlib/filepath/ms_mod_filepath.c）

- include guard `MSLANG_STDLIB_FILEPATH_MS_MOD_FILEPATH_H_`。
- 10 个包装函数均为 `static MsObject* filepathXxx(MsState* L, int64_t argc, MsObject** argv)`，统一骨架（以 `split` 为例，含 tuple 装箱与根纪律）：

```c
static MsObject* filepathSplit(MsState* L, int64_t argc, MsObject** argv) {
  if (argc != 1 || msTypeOf(argv[0]) != MS_TYPE_STR) {
    msRaiseTypeError(L, "filepath.split() requires exactly one str");
    return NULL;
  }
  const char* path = msAsCString(L, argv[0]);
  size_t pathLen = msStringLen(argv[0]);

  struct MsStrBuf scratch;
  msStrBufInit(&scratch);
  struct MsStrSlice dir, name;
  msPathSplit(path, pathLen, &scratch, &dir, &name);   // no allocation besides scratch

  // argv are roots; tuple must be rooted while boxing its elements.
  MsObject* pair = msNewTuple(L, 2);                    // task 32; see 设计依据 fallback
  if (pair == NULL) {
    msStrBufFree(&scratch);
    return NULL;
  }
  msRootPush(L, pair);
  MsObject* dirObj = msNewStringN(L, dir.data, dir.len);
  MsObject* nameObj = msNewStringN(L, name.data, name.len);
  msStrBufFree(&scratch);
  if (dirObj == NULL || nameObj == NULL) {
    msRootPop(L);
    return NULL;
  }
  msTupleSet(L, pair, 0, dirObj);                       // element write per task 32's API
  msTupleSet(L, pair, 1, nameObj);
  msRootPop(L);
  return pair;
}
```

纪律要点：`msAsCString` 指针窗口内不调用任何其他 C API（先取切片、算法层跑完、再装箱——与任务 20 第 6 节同纪律）；`msPathSplit`/`msPathBase` 的输出切片只引用入参或本函数持有的 `scratch`，`scratch` 在装箱完成后释放；所有失败路径按获取逆序释放资源并返回 `NULL`。

- `join` 为变参：先校验 `argc >= 1` 且全为 str，把各参数的 `{指针, 长度}` 收集到栈上小数组（≤8 元素）或 `msAlloc` 数组（>8），调 `msPathJoin` 后装箱。
- `abs` 直接转发 `msPathPlatAbs` 并把 `MS_ERROR_RUNTIME` 映射为 `msRaiseRuntimeError(L, "filepath.abs(): cannot determine working directory")`。
- `match`/`glob` 把 `MS_PATH_MATCH_BAD_PATTERN` / `MS_ERROR_SYNTAX` 映射为 `msRaiseValueError(L, "filepath.match(): malformed pattern")`（glob 同形）。
- `glob` 装箱：先 `msNewList` + `msRootPush`，逐元素 `msNewStringN` + `msListAppend`，结束后 `msRootPop` 并释放算法层字符串数组（逐元素 `msFree` + 数组 `msFree`）。

模块表与注册：

```c
static const MsMethodDef filepathMethods[] = {
  {"abs", filepathAbs, "abs(path) -> str"},
  // ... 10 entries, alphabetical: abs base clean dir ext glob isAbs join match split ...
  {NULL, NULL, NULL},
};

static const MsModuleDef filepathModuleDef = {
  "path/filepath", "file path manipulation (platform-aware)", filepathMethods,
};

// 09-c-api §9 naming convention applied to the last path segment.
const MsModuleDef* mslangInit_filepath(void);

// Called by the stdlib registrar during msNewState (mechanism established by
// task 18/19/20): registers the module under "path/filepath" and, until the
// import statement lands (task 24), also binds the module object under the
// global name "filepath" (05-modules §1 last-segment rule).
MsResult msFilepathRegister(MsState* L);
```

挂接方式与任务 20 的过渡策略一致：import（任务 24）落地前脚本直接以 `filepath.join(...)` 调用；任务 24 落地后 `import "path/filepath"` 绑定同一模块对象，全局预绑定是否保留以任务 24 文档定稿为准。

### 7. Windows 实现要点（ms_path_win.c）

- `msPathPlatVolumeNameLen`：`[A-Za-z]:` → 2；`\\` 或 `//` 开头且能解析出 `host\share` 两段 → UNC 前缀长度（`\\host\share`）；其余 → 0。
- `msPathPlatAbs`：UTF-8→UTF-16（`MultiByteToWideChar`，两段式求长），`GetFullPathNameW`，UTF-16→UTF-8 追加到 `out`；返回值 0 或转换失败 → `MS_ERROR_RUNTIME`。
- `msPathPlatGetCwd`（`abs` 的 POSIX 对照路径不需要单独导出——Windows 由 `GetFullPathNameW` 一并处理）：POSIX 侧 `getcwd(NULL, 0)` 不允许（非 C11、且分配不经 `msAlloc`），用定长栈缓冲 4 KiB 起步、`errno == ERANGE` 时 `msRealloc` 倍增重试。
- `msPathPlatReadDir`：拼接 `dir + "\*"` 的 UTF-16 形式交给 `FindFirstFileW`；逐条 `WideCharToMultiByte` 为 UTF-8，跳过 `.`/`..`；句柄 `FindClose` 在任何退出路径上配对。
- 全部 `msAlloc`/`msRealloc`/`msFree`，失败路径逆序释放（10-c-style §5/§6）。

## 实现步骤

1. 建 `src/platform/ms_path.h` 骨架与两份平台原语实现文件的空壳（`msPathPlatSeparator`/`msPathPlatIsSeparator`/`msPathPlatVolumeNameLen`/`msPathPlatIsAbs`/`msPathPlatExists`），接入 CMake（按 `_WIN32` 择一编译，目标并入 `mslang` 库）。验证：三平台编译链接通过；卷名与分隔符原语在两平台的行为符合第 3 节表格（临时 C 侧冒烟或经后续脚本断言）。
2. 实现算法层 `msPathClean` 与 `msPathJoin`（第 5 节栈式算法）。验证：`clean("")` → `"."`、`..` 弹栈与根处丢弃、重复分隔符合并、Windows 下 `/`→`\` 规整。
3. 实现 `msPathSplit`/`msPathDir`/`msPathBase`/`msPathExt`（共享反向扫描）。验证：第 2 节总表的全部示例逐条命中，含 `ext(".bashrc")` → `".bashrc"`、`ext("a.b/c")` → `""`。
4. 实现 `msPathMatch`（码点级、字符类、`*` 回溯、Windows 无转义）。验证：`*`/`?`/`[..]`/`[^..]`/区间/非法模式各情形；Windows 上模式 `/` 匹配名字 `\`。
5. 实现平台原语的系统调用部分：`msPathPlatAbs`（Windows `GetFullPathNameW` / POSIX `getcwd` 倍增重试）、`msPathPlatReadDir`（两平台）。验证：`abs(".")` 非空且 `isAbs` 为真；枚举固定夹具目录得到预期名单。
6. 实现 `msPathGlob`（组件切分、候选列表、魔法组件过滤、排序）。验证：对夹具目录树的 `*`/`?`/`[..]`/多层模式与无匹配 `[]`、非法模式 `MS_ERROR_SYNTAX`。
7. 建 `stdlib/filepath/ms_mod_filepath.{c,h}`：10 个 `MsCFunction` 包装、方法表、`filepathModuleDef`、`mslangInit_filepath`、`msFilepathRegister`，接入 `msNewState` 注册链与过渡性全局预绑定。验证：脚本 `print(filepath.join("a", "b"))` 输出平台正确结果。
8. 实现 `split` 的 tuple 装箱（任务 32 已落地则用 `msNewTuple`；否则按「设计依据」的 fallback 用两元素 list 并在代码注释标明切换点）。验证：`d, n := filepath.split("a/b/c")` 解包正确。
9. 创建 `tests/fixtures/filepath_glob/` 夹具目录树与 `tests/ms/stdlib/filepath/` 全部测试脚本（见测试方案），`python run_tests.py` 全绿。
10. Win/Linux/macOS × Debug/Release 构建验证；Debug（ASAN / `/RTC`）下跑全部 filepath 测试无内存错误与泄漏（含 `glob` 大结果集与 `msPathPlatReadDir` 失败路径）。

## 测试方案

本任务只交付本设计文档；测试脚本随实现编写。一律使用 ms 脚本测试（`tests/ms/stdlib/filepath/`，内建 `assert` + `print`——任务 40 之前不用 testing 模块），成功脚本末尾 `print("<用例名> ok")`，负向用例配 `<name>.exit`（内容 `1`），由仓库根 `run_tests.py` 驱动（假定其以仓库根为 cwd 调用 CLI；glob 测试的相对模式依赖此约定，若 `run_tests.py` 的 cwd 约定不同，测试改用 `filepath.abs` 先取绝对模式再断言后缀）。

跨平台断言策略：测试用运行时探针 `isWindows := filepath.isAbs("C:/")`（POSIX 恒假、Windows 恒真）派生平台期望值（首选分隔符、卷名行为），同一脚本两平台可跑；纯词法函数（`clean`/`dir`/`base`/`ext`/`split`/`match`）的语义在两平台除分隔符外一致，用探针参数化期望分隔符即可。

测试文件清单与覆盖点：

- `clean.ms`：空串 → `"."`；`.`/`..` 弹栈各情形（`a/./b`、`a/../b`、根处 `..` 丢弃、相对路径前导 `..` 保留）；重复分隔符合并；尾部分隔符去除；Windows 分支下 `/` 规整为 `\`、卷名保留（`C:/a/../b` → `C:\b`）。
- `split_dir_base_ext.ms`：第 2 节总表 `split`/`dir`/`base`/`ext` 的全部示例逐条断言（含 `split("a/b/")` 的 `name == ""`、`dir("a") == "."`、`base("/")` 为根分隔符、`ext(".bashrc") == ".bashrc"`、`ext("a.b/c") == ""`）；`split` 解包赋值 `d, n := filepath.split(p)`；不变量性质断言：`filepath.join(d, n) == filepath.clean(p)`（`n != ""` 时）。
- `join_isabs.ms`：`join` 的多元素、空元素跳过、单元素、结果已 clean；`isAbs` 的平台分支（POSIX："/x" 真、"x" 假；Windows：`C:\x`、`\\host\share\x` 真，`C:foo`、`\foo`、`x` 假）。
- `abs.ms`：`abs("")`/`abs(".")` 结果为绝对路径（`isAbs` 断言）；`abs("x")` 以 `abs(".")` 为前缀（经 `hasPrefix` 交叉断言，内建 str 方法已可用）；`abs(abs(p)) == abs(p)` 幂等。
- `match.ms`：`*`、`?`、`[abc]`、`[^abc]`、`[a-z]`、两端锚定（`match("a*", "ab")` 真、`match("a", "ab")` 假）、`*` 不跨分隔符、多字节码点名的 `?` 恰好一个码点；大小写敏感（`match("A", "a")` 假，两平台同）；Windows 分支下模式 `/` 与名字 `\` 互匹配。
- `glob.ms`：对 `tests/fixtures/filepath_glob/`（固定内容：`a.txt`、`b.txt`、`c.log`、`sub/d.txt`、`.hidden`）断言——`*.txt` 单层匹配集合与排序、`sub/*.txt`、`?.txt`、`[ab].txt`、不跨层的 `*.txt` 不含 `sub/d.txt`、无匹配返回 `[]`（`len == 0`）、相对模式结果不带 `./` 前缀；`.` 开头的隐藏文件行为固定为「仅当模式显式以 `.` 开头才匹配」（对齐 Go：`.` 是普通字符，`*` 可匹配——实现时以 Go 实际语义为准并在此锁定）。
- `errors_type.ms`（配 `errors_type.exit`）：参数个数错误、非 str 参数（`filepath.join("a", 1)`、`filepath.clean(42)`）；按首触发点拆分为 `errors_type_*.ms` 若干小文件（约定同任务 20）。
- `errors_pattern.ms`（配 `errors_pattern.exit`）：`match("[abc", "x")`、`glob("[z-a]*")` 等非法模式报 ValueError → 退出码 1；同上按需拆分。

## 验收标准

- [ ] `stdlib/filepath/ms_mod_filepath.{c,h}`、`src/platform/ms_path.{h,c}`、`src/platform/ms_path_win.c`、`src/platform/ms_path_posix.c` 存在；guard 分别为 `MSLANG_STDLIB_FILEPATH_MS_MOD_FILEPATH_H_` 与 `MSLANG_SRC_PLATFORM_MS_PATH_H_`；代码通过 [10-c-style.md](../language/10-c-style.md) 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef）。
- [ ] 07-stdlib §8 的 10 个函数全部实现并可经 `filepath.f(...)` 调用；语义与「详细设计」第 2 节总表逐条一致（含 `ext(".bashrc")`、match 大小写敏感、glob 无 `**`、无匹配返回 `[]` 等已固定边界）。
- [ ] 平台差异全部经 `msPathPlat*` 原语下沉：算法层（`ms_path.c`）无 `#ifdef _WIN32`，Windows 特有的头文件与 API 只出现在 `ms_path_win.c`；UTF-16 转换不外泄。
- [ ] `split` 返回二元组（任务 32 的 tuple，或其落地前的 list 过渡形式并带切换点注释），脚本侧 `d, n := filepath.split(p)` 解包正确。
- [ ] `glob` 结果按字节序升序、相对模式不附加 `./` 前缀、目录枚举失败静默跳过、非法模式报 ValueError；`match` 字符类按码点判定。
- [ ] 模块层遵守 GC 根纪律（tuple/list 装箱期间入根、`msAsCString` 指针窗口内不调用其他 API）与失败路径逆序释放；堆分配全部经 `msAlloc`/`msRealloc`/`msFree`。
- [ ] `tests/ms/stdlib/filepath/` 与 `tests/fixtures/filepath_glob/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿（含负向用例的预期退出码 1）；`ctest --test-dir build` 并入通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告。
- [ ] 无 TBD/TODO 占位；对任务 06/20/32 的接口假定（UTF-8 码点原语、`MsStrBuf`、`msNewTuple`/`msTupleSet`）与过渡性全局预绑定机制在实现时已对齐定名；任务 36/42 落地时按「设计依据」的依赖边界声明完成去重与吸收。
