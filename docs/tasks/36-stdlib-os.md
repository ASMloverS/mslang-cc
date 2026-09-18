# 36 标准库：os

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [18 C API 基础](18-c-api-foundation.md) |

## 任务目标

交付 C 内建标准库模块 `os`（`stdlib/ms_std_os.h` / `stdlib/ms_std_os.c`），完整实现 `docs/language/07-stdlib.md` §6 列出的全部 15 个函数与 2 个数据属性：命令行参数（`args`）、环境变量（`getEnv`/`setEnv`/`environ`）、工作目录（`cwd`/`chdir`）、进程退出（`exit`）、文件系统操作（`stat`/`listDir`/`mkdir`/`mkdirAll`/`remove`/`removeAll`/`rename`/`exists`）、外部命令执行（`exec`）、平台标识（`platform`）。

为此本任务同时交付 `src/platform/` 下的 OS 原语层（`ms_os.h` / `ms_os_posix.c` / `ms_os_win.c`）：环境变量、文件状态、目录创建/删除/枚举、进程创建与管道捕获等系统调用的平台差异全部下沉到该层，模块层只面对统一的 UTF-8 接口（[10-c-style.md](../language/10-c-style.md) §9 的硬约束——`src/platform/` 之外不出现 `#ifdef _WIN32`）。该层是继任务 38 的 `ms_path*` 之后 `src/platform/` 的第二位入住者，任务 42（平台抽象层，v0.3）落地时将其吸收对齐。

`os.args` 接替任务 22 的占位实现：移除 `src/cli/main.c` 中的占位 `os` dict 全局，改为由本模块提供正式的 `os` 模块对象，CLI 在求值前经新增的 `msStdOsSetArgs` 入口把命令行参数注入模块的 `args` 属性；任务 22 留下的 `os["args"]` 形式测试随之切换为 `os.args`。

完成后，ms 脚本 `import "os"` 即可使用全部功能，并经 `tests/ms/stdlib/os/` 下的脚本测试验证。

## 设计依据

- [07-stdlib.md](../language/07-stdlib.md)
  - §0：`os` 属 **C 内建模块**，源码位于 `stdlib/` 目录；函数命名小驼峰。
  - §6：函数与属性清单（本任务的功能边界，逐条对应，不增不减）；`os.exit` 抛 `SystemExit`；`os.stat` 返回 `StatInfo`（`size`/`mode`/`modTime`/`isDir`/`isFile`）；`os.platform` 取值为 `"windows" | "linux" | "darwin"`。
  - §25：`os.exec` 等待子进程时挂起协程而非 OS 线程——协程属 v0.3（任务 43/46），本任务先交付阻塞实现，差距在「详细设计」第 6 节显式承认。
- [04-exceptions.md](../language/04-exceptions.md) §4 内建异常层级：`SystemExit`（`BaseException` 直属，裸 `except` 不捕获）、`OSError` 及其子类 `FileNotFoundError`/`PermissionError`——本模块全部 OS 失败的异常归属。
- [02-types.md](../language/02-types.md) §4（str 为不可变 UTF-8 序列——路径、环境变量、进程输出一律以 UTF-8 持有，Windows 侧只在系统调用边界做 UTF-8↔UTF-16 转换）、§5.2（tuple 为不可变定长序列，`os.exec` 的返回类型）。
- [05-modules.md](../language/05-modules.md) §2/§7：内建 C 模块经内建注册表解析，优先级高于搜索路径，不可被同名脚本遮蔽。
- [09-c-api.md](../language/09-c-api.md) §3（GC 根栈纪律）、§5/§6（值构造与容器操作；`msAsCString` 缓冲区「下一次分配前有效」）、§8（`msRaiseTypeError`/`msRaiseValueError`/`msRaiseOSError` 错误约定，C 函数出错返回 `NULL`）、§9（`MsCFunction`/`MsMethodDef`/`MsModuleDef` 注册机制）。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）；§5「禁止 errno 跨层传播」——`errno`/`GetLastError` 在平台层内立即转换为规范化错误种类与消息，不外泄；§9 平台抽象（本任务的原语层即该规则的落地场所）。
- [11-project-layout.md](../language/11-project-layout.md) §1（`stdlib/`、`src/platform/`、`tests/ms/` 的目录位置）、§3（脚本参数经 `os.args` 暴露）。
- [README.md](README.md) 测试约定：本任务编号 ≥ 09 一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`，由仓库根 `run_tests.py` 驱动。
- 任务 18（C API 基础）提供：公开头文件族（`object.h`/`container.h`/`error.h`/`gc.h`）与标准库启动注册链（`msNewState` 内统一调用各 stdlib 模块注册函数，任务 35 已建立同模式）。
- 任务 22（CLI 完善）提供：`src/cli/main.c` 的参数解析与三模式分派、`os` 占位 dict 全局（`msCliExposeArgs`，本任务移除并替换）、`.cliargs`/`.exit` 同伴文件测试设施。
- 任务 23（异常系统）提供：28 个内建异常类与 `msVmRaiseFmt(L, MsExceptionId, fmt, ...)`、`msExceptionNew` 等内部接口——`os.exit` 的 `SystemExit` 与文件系统失败的 `OSError` 系列经此抛出。
- 任务 24（模块系统）提供：`import "os"` 解析、模块对象的 `attrs` 命名空间（`args`/`platform` 数据属性的写入目标）；假定其提供 `msModulesLookupByName` 供注册后按名取回模块对象。
- 任务 32（bytes/tuple/set）提供 `msNewTuple` / 元素写入接口（`os.exec` 三元组返回值所需）。
- [38 标准库：path/filepath](38-stdlib-path-filepath.md)：其文档声明「任务 36 落地 `os.cwd`/`os.listDir` 时应复用本层原语」。对齐结果见「详细设计」第 4 节——`cwd` 复用方向成立，`listDir` 因错误语义不同（glob 静默跳过 vs os 报错）不复用，理由与去重计划在该节注明。
- 任务 23/24/32 的设计文档若定名与本文假定（`msVmRaiseFmt`、`MS_EXC_*` 枚举值、`msModulesLookupByName`、`msNewTuple`/`msTupleSet`）不同，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件布局与分层

```
stdlib/
├── ms_std_os.h        # 模块对外声明：注册入口 + os.args 注入入口
└── ms_std_os.c        # 15 个 MsCFunction 包装、数据属性装配、MsModuleDef 注册表
src/platform/
├── ms_os.h            # OS 原语声明（唯一头文件，无平台条件编译）
├── ms_os_posix.c      # POSIX 实现（getenv/setenv/stat/opendir/fork/exec/pipe/poll/waitpid）
└── ms_os_win.c        # Win32 实现（宽字符 API + UTF-8↔UTF-16 转换 + CreateProcess）
```

分层职责（与任务 20/35/38 的「算法层 + 模块层」模式一致）：

- **平台原语层**（`ms_os_posix.c` / `ms_os_win.c`，编译期经 CMake 按平台择一）：唯一允许包含 `<unistd.h>` / `<windows.h>` 等系统头的地方；对外只暴露 UTF-8 接口，Windows 的宽字符转换、`GetLastError` 语义、句柄管理全部封装在内。
- **模块层**（`stdlib/ms_std_os.c`）：`MsCFunction` 薄封装——参数个数/类型校验、取切片、调原语层、装箱为脚本对象、把原语层的失败映射为脚本异常。全部 `static`，导出符号仅注册入口与 args 注入入口两个。

### 2. 函数语义总表（唯一权威）

07-stdlib §6 只给出签名与一行注释；下表逐函数固定语义，规范未写明处以本表为准（「对齐 Go」处指 Go `os` 包同名语义）：

| 函数/属性 | 语义 | 边界与失败 |
|---|---|---|
| `os.args` | 命令行参数 `list[str]`，`args[0]` 为脚本路径（`-e` 模式为 `"-e"`，REPL 为 `""`） | 普通 list，脚本可读写（对齐 Python `sys.argv`）；嵌入方不注入时为空 list |
| `os.platform` | 平台标识 str | 恰好为 `"windows"` / `"linux"` / `"darwin"` 之一（其他 POSIX 归为 `"linux"` 之外的 uname 小写名——首版三平台 CI 只覆盖这三者，其余平台取值不承诺） |
| `getEnv(key, default=nil)` | 读环境变量 | 不存在返回 `default`（缺省 nil）；`key` 含 `"="` 或为空抛 ValueError |
| `setEnv(key, value)` | 设置环境变量（覆盖语义），返回 nil | `key` 含 `"="` 或为空抛 ValueError；OS 失败抛 OSError；不提供 `unsetEnv`（规范无此名，清空用 `setEnv(key, "")`） |
| `environ()` | 全部环境变量的快照 dict（str→str） | 是拷贝而非活视图：返回后 `setEnv` 不改写它；Windows 上以 `=` 开头的隐藏条目（驱动器 cwd）跳过 |
| `cwd()` | 当前工作目录绝对路径 | 失败抛 OSError |
| `chdir(path)` | 切换工作目录，返回 nil | 目标不存在抛 FileNotFoundError、非目录或权限不足抛 OSError（按「错误映射」节） |
| `exit(code=0)` | 抛 `SystemExit`，`message` 为 int 退出码 | 顶层行为由任务 23 定稿（int → 退出码、不打印 traceback）；可被 `except SystemExit` 捕获后继续执行；`code` 非 int 抛 TypeError |
| `stat(path)` | 文件状态 dict：`{"size": int, "mode": int, "modTime": float, "isDir": bool, "isFile": bool}` | 不存在抛 FileNotFoundError；`modTime` 为 Unix 秒（含小数，与 `time.now` 口径一致）；`mode` 见下；`isDir`/`isFile` 至多一真（符号链接按目标解析，对齐 `stat` 而非 `lstat`） |
| `listDir(path)` | 目录项名 `list[str]`（仅名字，不含路径前缀） | 按字节序升序排序（与任务 38 glob 排序口径一致，跨平台确定）；不含 `"."`/`".."`；路径不存在抛 FileNotFoundError、非目录抛 OSError |
| `mkdir(path)` | 创建单层目录 | 已存在（任何形式）抛 OSError、父目录不存在抛 FileNotFoundError |
| `mkdirAll(path)` | 递归创建目录 | 已存在且为目录：成功返回 nil（对齐 Go `MkdirAll`）；已存在且非目录、或任一层失败抛 OSError |
| `remove(path)` | 删除文件或**空**目录 | 不存在抛 FileNotFoundError；非空目录抛 OSError |
| `removeAll(path)` | 递归删除整棵目录树 | 路径不存在**不报错**（对齐 Go `RemoveAll`）；删除中途失败抛 OSError |
| `rename(old, new)` | 重命名/移动 | 源不存在抛 FileNotFoundError；Windows 上目标已存在时失败（`MoveFileW` 不带 REPLACE 语义，与 POSIX `rename` 覆盖语义不同——差异显式承认，统一语义列入路线图）；跨盘移动失败抛 OSError |
| `exists(path)` | 路径是否存在（任何类型），返回 bool | 永不抛异常：权限不足等不可判定情形返回 `false`（对齐 Go 惯例 `os.path.exists` 的宽松语义） |
| `exec(cmd, args=nil)` | 运行外部命令，等待结束，返回 tuple `(exitCode, stdout, stderr)` | `cmd` 经 PATH 查找；找不到抛 FileNotFoundError；启动失败抛 OSError；`exitCode` 为 int（信号致死时见第 6 节）；`stdout`/`stderr` 为 str（字节原样，不做编码转换） |

通用约定：

- 路径、键、值参数必须是 str，个数或类型不符抛 TypeError；上表标注 ValueError/OSError/FileNotFoundError 的情形抛对应异常类（任务 23 已落地，抛真实异常实例，ms 测试可用 `try/except` 断言）。
- `stat` 的返回类型定为 **dict** 而非自定义 StatInfo 类型：`StatInfo` 在规范中只是字段清单，dict 与 `environ()` 的返回形态一致，且无需引入 C 自定义类型机制；`size`/`mode`/`modTime`/`isDir`/`isFile` 五个键名逐字取自规范。
- `mode` 口径：POSIX 为 `st_mode` 的低 12 位（权限位 + setuid/setgid/sticky）；Windows 由 `_wstat` 结果合成同口径近似值（只读文件 `0444`、可写 `0666`，目录另或 `040000` 位——`isDir`/`isFile` 已由独立布尔键承担，mode 的目录位仅为兼容习惯）。文件类型位不承诺跨平台一致，脚本判定类型一律用 `isDir`/`isFile`。
- 所有路径入参均为合法 UTF-8（str 构造时已校验，任务 06）；原语层对非法序列不重复校验，内部不变量用 `MS_ASSERT` 防御。

### 3. os.args：接替任务 22 的占位实现

任务 22 在 `src/cli/main.c` 以 `msCliExposeArgs` 把 `{"args": [...]}` dict 写入全局命名空间。本任务做三处切换：

1. **删除占位**：`src/cli/main.c` 移除 `msCliExposeArgs` 及其调用；任务 22 的 `tests/ms/cli/*args*.ms` 用例从 `os["args"]` 改为 `import "os"` + `os.args`（断言的三种模式 `args[0]` 语义不变）。
2. **模块侧属性**：`msStdOsRegister` 注册模块后，立即创建空 list 写入模块 `args` 属性、创建 `os.platform` 字符串写入 `platform` 属性。嵌入方不注入参数时 `os.args == []`。
3. **CLI 注入入口**（`stdlib/ms_std_os.h` 的第二个导出符号）：

```c
// stdlib/ms_std_os.h（guard MSLANG_STDLIB_MS_STD_OS_H_，自包含 <mslang/mslang.h>）

// Registers the "os" builtin module into the builtin module registry.
// Called once during interpreter startup (msNewState's stdlib wiring).
// Creates os.args as an empty list and os.platform as the platform string.
MsResult msStdOsRegister(MsState* L);

// Replaces the contents of os.args with argv[0..argc). Called by the CLI
// after msNewState and before evaluation; embedders may skip it. argv
// strings must be UTF-8 (see msOsUtf8Argv below) and outlive the call
// (they are copied into new str objects).
MsResult msStdOsSetArgs(MsState* L, int argc, const char* const* argv);
```

- `msStdOsSetArgs` 实现：经 `msModulesLookupByName(L, "os")`（任务 24 假定接口）取回模块对象，新建 list 逐元素 `msNewString` + `msListAppend`（list 先入根），再以 `msSetAttr(L, module, "args", list)` 整体替换属性（替换而非原地清空，避免持有旧 list 引用的脚本看到中途状态）。
- **Windows 的 argv 编码**：MSVC `main(int, char**)` 收到的窄 argv 是系统 ANSI 代码页而非 UTF-8。平台层提供统一入口，CLI 的 `main` 一律改经它取参：

```c
// src/platform/ms_os.h

// Provides argc/argv as UTF-8. Windows: converted from GetCommandLineW via
// CommandLineToArgvW (fresh msAlloc'd strings). POSIX: passthrough aliases
// of the caller's argv (already UTF-8 by convention). Pair with
// msOsUtf8ArgvFree (frees the copies on Windows; no-op on POSIX).
void msOsUtf8Argv(int* argc, char*** argv);
void msOsUtf8ArgvFree(int argc, char** argv);
```

### 4. 平台原语层（src/platform/ms_os.h）

include guard `MSLANG_SRC_PLATFORM_MS_OS_H_`，自包含（`<stdbool.h>`/`<stddef.h>`/`<stdint.h>` + 任务 02 的 `"core/ms_result.h"` + 任务 20 的 `"object/ms_str_op.h"` 取 `struct MsStrBuf`）。错误规范化枚举：

```c
// Normalized OS error kinds; errno/GetLastError never escape this layer
// (10-c-style §5). The module layer maps kinds to exception classes:
// NOT_FOUND -> FileNotFoundError, PERMISSION -> PermissionError, else OSError.
typedef enum {
  MS_OS_ERR_NONE = 0,
  MS_OS_ERR_NOT_FOUND,      // ENOENT / ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
  MS_OS_ERR_PERMISSION,     // EACCES / EPERM / ERROR_ACCESS_DENIED
  MS_OS_ERR_EXISTS,         // EEXIST / ERROR_FILE_EXISTS / ERROR_ALREADY_EXISTS
  MS_OS_ERR_NOT_EMPTY,      // ENOTEMPTY / ERROR_DIR_NOT_EMPTY
  MS_OS_ERR_NOT_DIR,        // ENOTDIR / ERROR_DIRECTORY
  MS_OS_ERR_OTHER
} MsOsErrKind;

// Converts the last OS error of the calling thread into a kind and a UTF-8
// message appended to msgBuf (message includes the OS text, e.g.
// FormatMessageW / strerror output). Call immediately after a failed
// primitive, before any other OS call.
MsOsErrKind msOsLastError(struct MsStrBuf* msgBuf);
```

原语函数（全部 UTF-8 入参/出参；返回 `bool` 者失败时经 `msOsLastError` 取详情；堆分配只经 `msAlloc`/`msRealloc`/`msFree`）：

```c
// ---- 平台标识 ----

// "windows" | "linux" | "darwin" | other lowercase uname name. Static
// storage, never fails. Backs os.platform. Task 42 folds this into
// msPlatformName.
const char* msOsPlatformName(void);

// ---- 环境变量 ----

// Looks up key. Returns true and appends the value to out when found;
// returns false (out untouched) when absent. OOM is impossible to report
// here: callers preflight with a large-enough buffer policy (the primitive
// grows via msRealloc and MS_ASSERTs on OOM in debug; see 实现步骤).
bool msOsGetEnv(const char* key, struct MsStrBuf* out);

// Sets key=value (overwrite). false on OS failure (msOsLastError).
bool msOsSetEnv(const char* key, const char* value);

// Snapshot of all environment variables. *keysOut/*valuesOut receive
// msAlloc'd arrays of msAlloc'd NUL-terminated strings; the caller frees
// each element and both arrays. MS_ERROR_OOM is the only failure.
MsResult msOsEnviron(char*** keysOut, char*** valuesOut, size_t* countOut);

// ---- 工作目录 ----

// Appends the absolute cwd to out. false on OS failure.
bool msOsGetCwd(struct MsStrBuf* out);
bool msOsChdir(const char* path);

// ---- 文件系统 ----

struct MsOsStat {           // filled by msOsStat; no heap ownership
  int64_t size;
  uint32_t mode;            // per §2: low 12 permission bits (+ dir bit on Windows)
  double modTime;           // Unix seconds with fraction
  bool isDir;
  bool isFile;
};

bool msOsStat(const char* path, struct MsOsStat* out);

// Directory entry names (UTF-8, no "." / ".."), msAlloc'd array of msAlloc'd
// strings, caller frees. Unlike msPathPlatReadDir (task 38, glob semantics:
// unreadable dir silently yields empty), failure here is reported via
// msOsLastError so the module layer can raise.
bool msOsListDir(const char* path, char*** namesOut, size_t* countOut);

bool msOsMkdir(const char* path);
bool msOsRemove(const char* path);       // file or empty dir
bool msOsRename(const char* oldPath, const char* newPath);

// Existence probe that never fails (backs os.exists).
bool msOsExists(const char* path);
```

- **`mkdirAll`/`removeAll` 不在原语层**：它们是纯词法分解 + 原语循环的组合逻辑（按分隔符逐层 `msOsMkdir`、先 `msOsListDir` 递归后 `msOsRemove` 自底向上），平台无关，放在**模块层**以 `static` 函数实现，复用任务 38 的分隔符原语 `msPathPlatIsSeparator`/`msPathPlatSeparator`（`src/platform/ms_path.h`，同层引用合法）。这是任务 38「os 调本层」声明的落实点之一。
- **`cwd`**：POSIX 侧 `getcwd(NULL, 0)` 不可用（非 C11 且分配不经 `msAlloc`），用定长栈缓冲起步、`errno == ERANGE` 时 `msRealloc` 倍增重试（与任务 38 的 `msPathPlatAbs` POSIX 分支同手法）；Windows 侧 `GetCurrentDirectoryW` + UTF-8 转换。任务 38 的注释已约定该手法，两处实现由任务 42 吸收时合并。
- **`listDir` 与任务 38 的去重**：任务 38 的 `msPathPlatReadDir` 是 glob 语义（目录不可读静默返回空），`os.listDir` 必须区分「不存在/非目录/权限不足」并抛对应异常，二者错误契约不同，故本任务在 `ms_os_*` 中实现独立的 `msOsListDir`（POSIX `opendir`/`readdir`、Windows `FindFirstFileW`/`FindNextFileW`，跳过 `.`/`..`）。两份目录枚举代码由任务 42 吸收平台层时统一（抽出带错误报告的公共内核，glob 语义作为其调用选项）。此偏离任务 38 文档字面声明之处在此显式记录并注明理由。
- **`msOsStat`**：POSIX `stat(2)`（`modTime` 取 `st_mtim` 秒 + 纳秒合成 double，老系统回退 `st_mtime`）；Windows `_wstat64`（UTF-8→UTF-16 先转）。`isDir`/`isFile` 由 `S_ISDIR`/`S_ISREG` 或 `_S_IFDIR`/`_S_IFREG` 判定。

### 5. 环境变量原语的实现要点

- `msOsGetEnv`：POSIX `getenv` 返回指针指向 `environ` 内部存储，**立即拷贝**到 `out`（拷贝前不做任何可能触碰环境的调用）。Windows `GetEnvironmentVariableW` 两段式（先求长再取）。
- `msOsSetEnv`：POSIX `setenv(key, value, 1)`；Windows `SetEnvironmentVariableW`（空值语义：传非空串，清空由模块层约定为 `setEnv(key, "")` 即设空串，**不**做 Windows 的 NULL 删除语义——保持跨平台一致）。
- `msOsEnviron`：POSIX 遍历 `extern char** environ`（在 `ms_os_posix.c` 内声明，不外泄），按首个 `=` 切分键值（无 `=` 的条目跳过）；Windows `GetEnvironmentStringsW` 返回双 NUL 结尾块，逐条解析（跳过以 `=` 开头的隐藏驱动器条目），`FreeEnvironmentStringsW` 在拷贝完成后配对调用。
- 线程安全：v0.2 解释器单线程（协程 v0.3），`getenv`/`setenv` 的非线程安全在此显式承认；任务 42 落地时评估加锁。

### 6. os.exec：进程创建与输出捕获

原语层接口：

```c
// src/platform/ms_os.h（续）

struct MsOsExecResult {     // filled by msOsExec; caller releases via msOsExecResultFree
  int64_t exitCode;         // process exit code; killed by signal: 128 + signo (POSIX convention)
  char* stdoutData;         // msAlloc'd, NUL-terminated for convenience (may embed NUL)
  size_t stdoutLen;
  char* stderrData;         // msAlloc'd, NUL-terminated
  size_t stderrLen;
};

// Runs cmd (PATH search applied) with args (may be NULL when argCount == 0),
// capturing stdout and stderr in full, waiting for termination.
// Returns MS_ERROR_RUNTIME when the process cannot be started
// (msOsLastError distinguishes NOT_FOUND); MS_ERROR_OOM on OOM.
MsResult msOsExec(const char* cmd, const char* const* args, size_t argCount, struct MsOsExecResult* out);

// Frees out's buffers; safe on a zeroed/partially filled result.
void msOsExecResultFree(struct MsOsExecResult* out);
```

实现要点：

- **参数传递**：POSIX `posix_spawnp`（首选，免 fork 的地址空间顾虑；不可用时回退 `fork` + `execvp`）；argv 数组按 `[cmd, args..., NULL]` 现场拼装（`msAlloc`）。Windows 无 argv 数组约定，需按 `CreateProcessW` 的命令行规则自行 quote（含空格/引号的参数加双引号并转义内嵌引号——对齐 `CommandLineToArgvW` 的逆运算），拼成单条 UTF-16 命令行。
- **管道捕获与防死锁**：子进程 stdout/stderr 各接一条匿名管道。两路必须并发排空，否则子进程写满 stderr 而父进程在读 stdout 时互锁。方案（不依赖任务 42 的线程原语）：
  - POSIX：`poll(2)` 双 fd 循环，就绪即读、EOF 关闭对应 fd，两路均 EOF 后 `waitpid` 取退出码；信号致死映射 `128 + WTERMSIG`。
  - Windows：管道的读端句柄配合 `PeekNamedPipe` 非阻塞探测 + `ReadFile` 排空，主循环内交替排空两路并以 `WaitForSingleObject(processHandle, 10ms)` 轮询进程结束；进程退出后再做最后一次排空（管道内残余数据）。两路缓冲经 `struct MsStrBuf` 增长。
- **协程感知差距**：07-stdlib §25 要求 `os.exec` 等待子进程时挂起协程；协程属 v0.3（任务 43/46），本任务交付**阻塞当前线程**的实现，语义（返回值、异常）与终态一致，仅阻塞粒度不同。任务 46 的调度器落地后把等待环节改为协程挂起，接口与测试不变。此差距在此显式承认。
- 模块层包装：`cmd` 必须是 str；`args` 缺省/为 nil 时无参数，否则必须是元素全为 str 的 list（非 str 元素抛 TypeError）。成功后装箱三元组：先 `msNewTuple(L, 3)` + `msRootPush`，再依次装箱 `exitCode`（int）、`stdout`/`stderr`（`msNewStringN`，字节原样——子进程输出不保证是合法 UTF-8 时按原字节保留，解码是调用方职责），元素写入后 `msRootPop`。

### 7. 模块封装层（stdlib/ms_std_os.c）

- include guard `MSLANG_STDLIB_MS_STD_OS_H_`（`stdlib/` 不在 `src/` 下，前缀为 `MSLANG_STDLIB_`，与任务 35 同形）；头文件自包含（`<mslang/mslang.h>`）。
- 15 个包装函数均为 `static MsObject* osXxx(MsState* L, int64_t argc, MsObject** argv)`，统一骨架（以 `stat` 为例，含 dict 装箱与根纪律）：

```c
static MsObject* osStat(MsState* L, int64_t argc, MsObject** argv) {
  if (argc != 1 || msTypeOf(argv[0]) != MS_TYPE_STR) {
    msRaiseTypeError(L, "os.stat() requires exactly one str path");
    return NULL;
  }
  const char* path = msAsCString(L, argv[0]);   // argv are roots; no API call until boxing
  struct MsOsStat st;
  if (!msOsStat(path, &st)) {
    osRaiseLastError(L, "os.stat", path);       // static helper: kind -> exception class
    return NULL;
  }
  MsObject* info = msNewDict(L);
  if (info == NULL) {
    return NULL;
  }
  msRootPush(L, info);
  // box size/mode/modTime/isDir/isFile via msNewInt/msNewFloat/msNewBool + msDictSet
  // ...
  msRootPop(L);
  return info;
}
```

- **错误映射辅助**（模块层 `static`）：`osRaiseLastError(L, funcName, path)` 调 `msOsLastError` 取种类与消息，按 `MS_OS_ERR_NOT_FOUND → FileNotFoundError`、`MS_OS_ERR_PERMISSION → PermissionError`、其余 `→ OSError` 经 `msVmRaiseFmt`（任务 23 假定接口）抛出，消息形如 `os.stat: '/x/y': No such file or directory`。参数类错误（TypeError/ValueError）仍用公开 API `msRaiseTypeError`/`msRaiseValueError`。
- **`os.exit`**：`argc ∈ [0, 1]`，给定时必须为 int（否则 TypeError）；构造 `SystemExit` 异常实例（`msExceptionNew(L, msExceptionTypeOf(L, MS_EXC_SYSTEM_EXIT), msNewInt(L, code))`，任务 23 假定接口）并 `msRaise(L, exc)` 后返回 `NULL`。顶层退出码语义由任务 23 的未捕获处理保证。
- **`os.environ`**：调 `msOsEnviron` 后逐对装箱进 dict（dict 先入根）；释放原语层字符串数组（逐元素 `msFree` + 两数组 `msFree`）。
- **`os.listDir`**：调 `msOsListDir` 后对名字数组按 `strcmp` 升序排序（排序放原语层出口或模块层入口皆可，定为原语层出口，保证任一调用方拿到有序结果），装箱为 list（先入根）。
- **`mkdirAll`**（模块层 `static`）：路径已 `msOsExists` 且 `msOsStat` 为目录 → 直接成功；否则按分隔符逐层前缀 `msOsMkdir`，`MS_OS_ERR_EXISTS` 且该层为目录时继续，其余失败经 `osRaiseLastError` 抛出。
- **`removeAll`**（模块层 `static`）：`msOsStat` 探测——不存在直接成功（Go 语义）；文件/空目录直接 `msOsRemove`；目录则 `msOsListDir` 逐名字拼子路径（分隔符经 `msPathPlatSeparator`）递归删除后删自身。递归深度以路径分隔符数为界，不做人为上限。
- **`msAsCString` 指针窗口纪律**与任务 20/35 相同：「先取指针、原语层跑完、再装箱」，窗口内不调用其他可能分配的 C API。

模块表与注册：

```c
static const MsMethodDef osMethods[] = {
  {"chdir",    osChdir,    "chdir(path)"},
  {"cwd",      osCwd,      "cwd() -> str"},
  {"environ",  osEnviron,  "environ() -> dict"},
  {"exec",     osExec,     "exec(cmd, args=nil) -> (int, str, str)"},
  {"exists",   osExists,   "exists(path) -> bool"},
  {"exit",     osExit,     "exit(code=0)"},
  {"getEnv",   osGetEnv,   "getEnv(key, default=nil)"},
  {"listDir",  osListDir,  "listDir(path) -> list[str]"},
  {"mkdir",    osMkdir,    "mkdir(path)"},
  {"mkdirAll", osMkdirAll, "mkdirAll(path)"},
  {"remove",   osRemove,   "remove(path)"},
  {"removeAll", osRemoveAll, "removeAll(path)"},
  {"rename",   osRename,   "rename(old, new)"},
  {"setEnv",   osSetEnv,   "setEnv(key, value)"},
  {"stat",     osStat,     "stat(path) -> dict"},
  {NULL, NULL, NULL},
};

static const MsModuleDef osModuleDef = {
  "os", "operating system interface", osMethods,
};
```

`msStdOsRegister` 调 `msRegisterModule(L, &osModuleDef)`（任务 33 落地实现），随后经 `msModulesLookupByName` 取回模块对象，写入 `args`（空 list）与 `platform`（`msOsPlatformName()` 装箱）两个数据属性；挂入任务 18 建立的标准库注册链。本任务**不做全局名预绑定**（v0.1 过渡策略已由任务 24 收尾移除），脚本统一 `import "os"`。

### 8. 内存与 GC 纪律清单

- 原语层分配：`MsStrBuf` 增长（`msRealloc`）、`msOsEnviron`/`msOsListDir` 的字符串数组（`msAlloc` 数组 + 逐元素 `msAlloc`）、`msOsExecResult` 的两个输出缓冲、Windows 侧 UTF-16 临时缓冲与 argv 拷贝（`msAlloc`）；每对分配/释放的所有者在函数文档注释写明，失败路径按获取逆序释放。
- 模块层：参数自动是根（09-c-api §3）；跨分配存活的中间对象为装箱中的 dict/list/tuple（`stat`/`environ`/`listDir`/`exec`），一律先 `msRootPush`、完成后 `msRootPop`；`msOsEnviron`/`msOsListDir`/`msOsExecResult` 的 C 侧缓冲不涉 GC，但必须在装箱结束或失败路径释放。
- `struct MsState` 无可变全局新增：os 模块不持有模块级可变 C 状态（`args` 是模块属性上的普通脚本对象）。

## 实现步骤

1. 建 `src/platform/ms_os.h` 全量声明骨架（guard、`MsOsErrKind`、`struct MsOsStat`、`struct MsOsExecResult`、全部原语签名与英文文档注释）与两份后端空壳；CMake 按 `WIN32` 择一编译并入 `mslang` 库。验证：三平台编译链接通过；`msOsPlatformName` 返回值属于合法集合。
2. 实现平台标识与环境变量原语（`msOsGetEnv`/`msOsSetEnv`/`msOsEnviron`）与 `msOsLastError`。验证：经后续模块层脚本断言 setEnv→getEnv 往返、environ 快照含注入变量。
3. 实现 `msOsGetCwd`/`msOsChdir` 与 `msOsStat`/`msOsExists`。验证：cwd 非空且为绝对路径；stat 的 size/isDir/isFile 在已知文件上正确；Windows 的 UTF-16 边界（含非 ASCII 路径）人工验证。
4. 实现 `msOsListDir`（两后端，跳过 `.`/`..`，出口排序）、`msOsMkdir`/`msOsRemove`/`msOsRename`。验证：对自建目录树的枚举与增删改名行为。
5. 实现 `msOsUtf8Argv`/`msOsUtf8ArgvFree`；CLI `main` 改经它取参。验证：Windows 下带非 ASCII 参数的脚本 `os.args` 内容正确；POSIX 直通无拷贝泄漏。
6. 实现 `msOsExec`/`msOsExecResultFree`：两后端的进程创建、双管道防死锁排空、退出码与信号映射、Windows 命令行 quoting。验证：捕获大量 stdout 与大量 stderr 同时产生的用例不挂死、内容完整。
7. 建 `stdlib/ms_std_os.{c,h}`：15 个 `MsCFunction` 包装（含 `osRaiseLastError` 错误映射、`mkdirAll`/`removeAll` 组合逻辑）、方法表、`osModuleDef`、`msStdOsRegister`（含 `args`/`platform` 属性装配）、`msStdOsSetArgs`，挂入 `msNewState` 注册链。验证：脚本 `import "os"` 后 `os.cwd()`、`os.platform` 可用。
8. `os.args` 切换：删除任务 22 的 `msCliExposeArgs` 占位，CLI 在 `msNewState` 之后、求值之前调 `msStdOsSetArgs`；任务 22 的 `tests/ms/cli/*args*.ms` 用例改写为 `import "os"` + `os.args`。验证：三种模式（脚本/`-e`/REPL）的 `args[0]` 与尾随参数断言通过，任务 22 其余用例无回归。
9. 编写 `tests/ms/stdlib/os/` 全部测试脚本（见测试方案），`python run_tests.py` 全绿。
10. Win/Linux/macOS × Debug/Release 构建验证；Debug（ASAN / `/RTC`）下跑全部 os 测试无内存错误与泄漏（含 `exec` 管道缓冲与 `environ`/`listDir` 数组的失败路径）；grep 断言 `src/platform/` 之外无 `#ifdef _WIN32`。

## 测试方案

本任务只交付本设计文档；测试脚本随实现编写。一律使用 ms 脚本测试（`tests/ms/stdlib/os/`，内建 `assert` + `print`——任务 40 之前不用 testing 模块），由仓库根 `run_tests.py` 驱动；异常系统（任务 23）已落地，负向用例用 `try/except FileNotFoundError`/`except OSError` 等在正向脚本内断言（捕获后校验类型，未捕获即 `assert(false)`），与任务 35 的约定一致；`os.exit` 的进程退出码用 `.exit` 同伴文件断言（任务 22 设施）。

文件系统用例统一自建刮削目录：脚本开头 `os.mkdirAll("build/os_test_scratch_<用例名>")`（构建产物目录已存在，`run_tests.py` 以仓库根为 cwd 调用 CLI——同任务 38 的约定），结尾 `os.removeAll` 清理；刮削目录落在 `build/` 内，不污染源码树。跨平台断言经 `os.platform` 派生期望值（如路径分隔符相关的拼接一律用变量而非字面量）。

测试文件清单与覆盖点：

- `platform_args.ms`：`os.platform` ∈ {"windows", "linux", "darwin"}；`os.args` 为 list、`args[0]` 以本脚本文件名结尾、无尾随参数时 `len(os.args) == 1`；`os.args` 可读写（赋值后读回，验证它是普通 list）。
- `env.ms`：`setEnv` → `getEnv` 往返（含空串值与 UTF-8 值）；`getEnv` 缺失键返回 nil 与自定义 `default`；`environ()` 返回 dict、含刚 setEnv 的键、`environ()` 后再 `setEnv` 不改写已得快照（拷贝语义）；负向：`getEnv("")`、`setEnv("A=B", "x")` 抛 ValueError。注：用 `.env` 同伴文件（任务 22 设施）注入一个已知变量断言读取，或仅以 setEnv 自举。
- `cwd_chdir.ms`：`os.cwd()` 非空且 `filepath.isAbs` 为真（任务 38 模块可作辅助）；`chdir` 到刮削目录后 `cwd()` 变化、`chdir` 回原处还原；负向：`chdir` 到不存在路径抛 FileNotFoundError、`chdir` 到文件抛 OSError。
- `stat_exists.ms`：对刮削目录内已知文件断言 `exists` 真、`stat` 的 `size` 等于实际写入长度（经 io 未落地前的替代：用 `os.exec` 或固定内容文件——实现时选用任务 37 之前的可行手段，如直接对测试脚本自身 stat）、`isFile` 真 `isDir` 假、`mode` 低 9 位非零、`modTime` 为正 float；对目录 `isDir` 真 `isFile` 假；`exists` 对不存在路径返回 false 且不抛异常；负向：`stat` 不存在路径抛 FileNotFoundError。
- `fs_ops.ms`：`mkdir` 单层、`mkdir` 已存在抛 OSError、`mkdir` 父缺失抛 FileNotFoundError；`mkdirAll("a/b/c")` 递归创建、重复调用幂等（目录已存在不报错）、已存在同名文件时报错；`rename` 文件与目录；`remove` 文件、`remove` 空目录、`remove` 非空目录抛 OSError、`remove` 不存在抛 FileNotFoundError；`removeAll` 删除整棵树、对不存在路径不报错。
- `listdir.ms`：刮削目录内建已知文件集（含子目录），`listDir` 结果按字节序升序、仅名字不含前缀、不含 `"."`/`".."`；空目录返回 `[]`；负向：不存在路径抛 FileNotFoundError、对文件调用抛 OSError。
- `exec.ms`：按 `os.platform` 分支选取命令——POSIX 用 `os.exec("/bin/sh", ["-c", "echo hello"])` 断言 `(0, "hello\n", "")`、非零退出（`exit 3`）断言 exitCode、写 stderr 的命令断言捕获；Windows 用 `os.exec("cmd", ["/c", "echo hello"])` 等同形断言；**双路大量输出**（各数万字节）同时产生时不挂死且长度完整（防死锁回归）；负向：不存在的命令名抛 FileNotFoundError、`args` 含非 str 元素抛 TypeError。
- `exit_code.ms`（配 `exit_code.exit` 内容 `3`）：顶层 `os.exit(3)`，断言进程退出码 3；另 `exit_default.ms`（配 `.exit` 内容 `0`）顶层 `os.exit()`。
- `exit_catch.ms`：`try { os.exit(7) } except SystemExit as e { assert(e.message == 7) }` 捕获后继续执行；裸 `except` 不捕获 `SystemExit`（内层裸 except、外层 `except SystemExit` 接住，对齐任务 23）；`os.exit("x")` 抛 TypeError。
- `errors_type.ms`：参数个数错误与类型错误的批量断言（`os.stat(42)`、`os.getcwd` 类误用名不存在抛 AttributeError 的断言不做——只覆盖本文定义的接口面），全部经 `try/except TypeError` 在脚本内断言。

## 验收标准

- [ ] `stdlib/ms_std_os.{c,h}` 与 `src/platform/ms_os.{h}`、`ms_os_posix.c`、`ms_os_win.c` 存在；guard 分别为 `MSLANG_STDLIB_MS_STD_OS_H_` 与 `MSLANG_SRC_PLATFORM_MS_OS_H_`；头文件自包含且 `ms_os.h` 不含任何平台条件编译；代码通过 [10-c-style.md](../language/10-c-style.md) 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef）。
- [ ] 07-stdlib §6 的 15 个函数与 2 个数据属性全部实现并经 `import "os"` 可用；语义与「详细设计」第 2 节总表逐条一致（`stat` 返回五键 dict、`listDir` 升序排序、`mkdirAll`/`removeAll` 的幂等与容错语义、`exists` 永不抛异常、`getEnv` 的 default 语义、`environ` 的快照语义）。
- [ ] 平台差异全部下沉 `src/platform/`：`stdlib/` 与其余 `src/` 代码无 `#ifdef _WIN32`（grep 可验证）；Windows 的 UTF-16 转换、`GetLastError`、`CreateProcess` 命令行 quoting 不外泄；`errno`/`GetLastError` 经 `MsOsErrKind` 规范化后映射为 `FileNotFoundError`/`PermissionError`/`OSError`。
- [ ] `os.args` 接替完成：`src/cli/main.c` 的占位 dict 全局已删除，CLI 经 `msOsUtf8Argv` 取参、经 `msStdOsSetArgs` 注入；三种模式（脚本/`-e`/REPL）的 `args[0]` 语义与任务 22 一致；任务 22 的 args 用例已迁移到 `import "os"` 且其余 CLI 用例无回归；嵌入方不注入时 `os.args == []`。
- [ ] `os.exit(code=0)` 抛 `SystemExit`（int 消息），可被 `except SystemExit` 捕获、不被裸 `except` 捕获；顶层未捕获时进程退出码与 code 一致（`.exit` 同伴断言）。
- [ ] `os.exec` 返回 `(exitCode, stdout, stderr)` 三元组，双管道并发排空无死锁（双路大量输出用例通过），信号致死映射 `128 + signo`；阻塞而非协程感知的差距已在文档显式承认。
- [ ] 模块层遵守 GC 根纪律（dict/list/tuple 装箱期间入根、`msAsCString` 指针窗口内不调用其他 API）与失败路径逆序释放；堆分配全部经 `msAlloc`/`msRealloc`/`msFree`；os 模块不持有模块级可变 C 状态。
- [ ] `tests/ms/stdlib/os/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿（含 `.exit` 用例），`ctest --test-dir build` 并入通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告。
- [ ] 无 TBD/TODO 占位；对任务 23（`msVmRaiseFmt`/`MS_EXC_*`/`msExceptionNew`/`msExceptionTypeOf`）、任务 24（`msModulesLookupByName` 与模块属性写入机制）、任务 32（`msNewTuple`/元素写入）、任务 20（`MsStrBuf`）与任务 38（`msPathPlatSeparator`/`msPathPlatIsSeparator`）的接口假定在实现时已对齐定名；`listDir` 不复用 `msPathPlatReadDir` 的理由已记录，任务 42 吸收平台层时完成目录枚举代码的统一。
