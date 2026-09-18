# 37 标准库：io

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [36 标准库：os](36-stdlib-os.md) |

## 任务目标

交付 C 内建标准库模块 `io`（`stdlib/ms_std_io.h` / `stdlib/ms_std_io.c`），完整覆盖 `docs/language/07-stdlib.md` §7：

- **Reader/Writer 鸭子类型协议**：Reader 为 `read(n=-1)` / `readAll()` / `readLine()` / `close()`，Writer 为 `write(data)` / `flush()` / `close()`；协议不对应任何 C 基类或接口结构体，仅靠方法名约定（07-stdlib §7「接口协议（鸭子类型）」），脚本 class 实例与 C 自定义类型实例只要带齐方法即被 `io.copy` 等函数接受。
- **模块函数**：`io.copy(dst, src)`（返回拷贝字节数）、`io.readAll(reader)`、`io.tee(reader, writer)`。
- **File 类型**：内建函数 `open(path, mode="r")` 返回的 C 自定义类型实例，同时实现 Reader 与 Writer，另有 `seek(offset, whence)` / `tell()`，支持 `with`（任务 29 的 `__enter__`/`__exit__` 协议）；mode 为 `"r" "w" "a" "rb" "wb" "r+"` 六种，无多不少（07-stdlib §7 原文列举，本任务不擅自扩充 `"w+"` 等形式）。
- **标准流**：模块属性 `io.stdin` / `io.stdout` / `io.stderr`，为包裹 C 标准流的 File 实例，供 `print(..., file=io.stderr)`（03-syntax §9.1）与 `log.setOutput`（任务 51）等消费 Writer 协议的场合使用。
- **内建函数补全**：`open` 与 `input`（03-syntax §9 清单；任务 10 明确把两者推迟至「任务 36/37」，本任务承接）。

文件读写引擎下沉为纯 C 层（`src/io/ms_file.{c,h}`），只操作 `FILE*` 与字节切片、不触碰 `MsObject`，供 io 模块与后续需要文件读写的模块（如任务 51 log、任务 57 json 的 `dump`/`load`）复用。文件 IO 按 07-stdlib §25 的约定为**阻塞式**：占用当前工作线程，不做协程感知（协程调度属 v0.3，文件 IO 异步化列入路线图）。完成后脚本可 `import "io"` 使用全部功能，并经 `tests/ms/stdlib/io/` 下的脚本测试验证（测试夹具在 `tests/fixtures/io/`）。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0：`io` 属 C 内建模块，源码位于 `stdlib/`；函数命名小驼峰。
  - §7：Reader/Writer 协议方法集、`io.copy`/`io.readAll`/`io.tee` 三个模块函数、`open(path, mode)` 的 mode 全集与 File 额外方法（`seek`/`tell`/`with`）——本任务功能边界的唯一来源。
  - §25：文件读写首版阻塞占用工作线程，不挂起协程。
- `docs/language/03-syntax.md` §3.2（with 语句语义：`__enter__()` 绑定 as 目标，块退出调 `__exit__(excType, excValue, traceback)`，返回真值决定吞否异常）、§9（内建函数清单含 `open(path, mode="r")` 与 `input(prompt="")`）、§9.1（`print` 的 `file` 参数「需实现 io.Writer 协议」，示例 `print("err msg", file=os.stderr)`）。
- `docs/language/02-types.md` §4（str 为不可变合法 UTF-8 序列——文本模式读出的 str 必须经 UTF-8 校验的依据；`str.toBytes()`/`bytes.toString()` 的编码约定）与 §1（bytes 不可变原始字节序列，二进制模式的读出类型）。
- `docs/language/04-exceptions.md` §4：`OSError`（含子类 `FileNotFoundError`/`PermissionError`）、`EOFError`、`ValueError`（含子类 `UnicodeError`）、`TypeError` 的层级与类名。
- `docs/language/09-c-api.md` §3（GC 根栈纪律）、§5（`msNewStringN`/`msNewBytes`/`msNewInt`/`msAsCString`/`msStringLen`；`msAsCString` 缓冲区「下一次分配前有效」）、§7（`msCallMethod`/`msHasAttr`/`msGetAttr`——鸭子协议调用的全部设施）、§8（`msRaiseTypeError`/`msRaiseValueError`/`msRaiseOSError`，`sysErrno` 直接对应 `errno`）、§9（`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule`）、§10（`MsTypeDef`/`msDefineType`/`msCInstanceData`，finalize 纪律）。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）；§5「禁止 errno 跨层传播」——引擎层把 `errno` 立即转换为返回值语义，模块层在调用点经 `msRaiseOSError` 转为脚本异常；§9（平台相关代码只在 `src/platform/`——本任务全部走 C11 `stdio`，无平台分支，不需平台层新原语）。
- `docs/language/11-project-layout.md` §1（`stdlib/`、`src/` 子目录、`tests/ms/`、`tests/fixtures/` 的位置约定）。
- `docs/tasks/README.md` 测试约定：任务编号 ≥ 09 一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`；由仓库根 `run_tests.py` 驱动。
- [35 标准库：strconv](35-stdlib-strconv.md) 确立的本阶段 stdlib 任务模式：算法层 + 模块层分层、模块唯一导出符号为注册入口、负向用例用 `try/except` 在脚本内断言（任务 23 异常系统已落地）、`import "io"` 经内建注册表解析（任务 24）。
- 任务 33（C 扩展与自定义类型）提供 `msDefineType`/`msCInstanceData`/`MsTypeDef`（File 与 TeeReader 的类型载体），及其硬性限制：**C 类型数据区不得持有脚本对象**（无 traverse 回调）——`io.tee` 的 TeeReader 需要持有 reader/writer 两个脚本对象，本任务以「模块级引用表 + 槽位索引」规避（见「详细设计」第 6 节），不修改任务 33 的既有约束。
- 任务 32（bytes/tuple/set）提供 bytes 类型与 `msNewBytes`（09-c-api §5 既定签名），二进制模式读出的装箱类型。
- 任务 29（with 语句）定义 `__enter__`/`__exit__` 调用协议；本任务只在 File 上实现两侧魔术方法，不改动 with 语义本身。
- 任务 20（strings）的可增长缓冲 `struct MsStrBuf`（`src/object/ms_str_op.h`：`msStrBufInit`/`msStrBufPut`/`msStrBufFree`，`readAll`/`io.readAll` 的累积缓冲）；任务 06 的 UTF-8 原语（假定名 `msUtf8Validate` / `msUtf8DecodeRune`，文本模式校验与码点计数所需；若任务 06/20 定名不同，实现时以对应任务文档为准）。
- 任务 10（内建函数）建立内建注册表（`src/vm/ms_builtin.c` 的 `msBuiltinTable`），`open`/`input` 两项由本任务追加到该表，实现函数由 io 模块导出（见「详细设计」第 7 节）。
- 依赖任务 36（os）的设计文档本文撰写时**尚不存在**：本任务对 os 的消费仅限测试脚本用 `os.mkdirAll`/`os.remove`/`os.cwd` 做临时文件管理（07-stdlib §6 既定签名）；io 与 os 的功能边界见「详细设计」第 8 节。凡本文引用的任务 36 接口名，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件布局与分层

```
src/io/
├── ms_file.h          # 文件引擎层声明（纯 C，无 MsObject）
└── ms_file.c          # 引擎实现：fopen/fread/fwrite/fseek/ftell 的语义封装
stdlib/
├── ms_std_io.h        # 模块对外声明：注册入口 + 内建 open/input 实现函数
└── ms_std_io.c        # File/TeeReader 类型定义、模块函数、注册逻辑
tests/fixtures/io/     # 只读测试夹具（固定内容文件，见测试方案）
```

分层职责（与任务 35 的「算法层 + 模块层」模式一致）：

- **引擎层**（`src/io/ms_file.*`）：纯 C11 `stdio` 封装。负责 mode 解析、`FILE*` 生命周期、按字节/按码点读取、行读取、写入、`fseek`/`ftell`、更新模式（`r+`）的读写方向切换处理、`errno` 的就地捕获。不 include 任何项目头文件（除 `<stdbool.h>` 等系统头与任务 02 的 `core/ms_alloc.h`），不触碰 `MsObject`/`MsState`。
- **模块层**（`stdlib/ms_std_io.c`）：`MsCFunction` 包装——参数校验、经引擎层读写、把字节切片装箱为 str（文本模式，含 UTF-8 校验）或 bytes（二进制模式）、鸭子协议调用（`msCallMethod`）、错误转异常。File/TeeReader 两个 C 自定义类型经 `msDefineType` 创建。唯一导出符号：`msStdIoRegister`、`msStdIoOpenBuiltin`、`msStdIoInputBuiltin`。

### 2. 模块面总表（唯一权威）

07-stdlib §7 只给出签名；下表逐条固定语义，规范未写明处以本表为准：

| 接口 | 语义 | 边界与失败 |
|---|---|---|
| `open(path, mode="r")` | 打开文件，返回 File | mode 六种之外抛 ValueError；path 非 str 抛 TypeError；打开失败按 `errno` 抛 OSError 子类（ENOENT → FileNotFoundError、EACCES/EPERM → PermissionError、其余 → OSError） |
| `f.read(n=-1)` | 读至多 n 个单位；`n == -1`（或省略）读全部剩余 | 文本模式单位为**码点**、返回 str；二进制模式单位为**字节**、返回 bytes；`n < -1` 抛 ValueError；EOF 返回空 str/bytes；已关闭抛 ValueError |
| `f.readAll()` | 读全部剩余，等价 `read(-1)` | 同 `read(-1)` |
| `f.readLine()` | 读一行（含结尾 `\n`），EOF 前的末尾无换行行原样返回 | 类型随模式（str/bytes）；流上无任何剩余数据时返回空 str/bytes（**不抛 EOFError**）；已关闭抛 ValueError |
| `f.write(data)` | 写入并返回**写入的字节数**（int） | 文本模式只接受 str（按其 UTF-8 字节写入）、二进制模式只接受 bytes，错型抛 TypeError；以「读」模式打开的文件抛 OSError；返回字节数对齐 Go 的 Writer 契约（`io.copy` 需要字节计数） |
| `f.flush()` | `fflush`，返回 nil | 对只读文件为无操作；底层失败抛 OSError |
| `f.close()` | 关闭文件，幂等，返回 nil | 重复 close 无操作；标准流（stdin/stdout/stderr）的 close 只做逻辑关闭（`fflush` + 置 closed 标志），不 `fclose` 底层 C 流 |
| `f.seek(offset, whence=0)` | 定位；返回新的绝对位置（int） | whence 仅 0（SEEK_SET）/1（SEEK_CUR）/2（SEEK_END），其余抛 ValueError；文本模式的 offset 为**字节偏移**（与 Python 的「不透明 cookie」不同，本文固定为字节语义并显式承认差异）；定位失败（如负位置）抛 OSError |
| `f.tell()` | 当前字节位置（int） | 已关闭抛 ValueError |
| `f.__enter__()` | 返回 File 自身 | 已关闭抛 ValueError |
| `f.__exit__(excType, excValue, tb)` | 关闭文件，恒返回 `false`（不吞异常） | 关不失败路径与 `close` 一致 |
| `io.copy(dst, src)` | 从 src 读、向 dst 写直到 EOF，返回拷贝总字节数 | 两者经鸭子协议调用；src 无 `read` 或 dst 无 `write` 抛 TypeError；`write` 返回非 int 或短写（返回值 ≠ 数据字节数）抛 OSError |
| `io.readAll(reader)` | 读全部剩余 | reader 有 `readAll` 方法则直接调用之，否则以 32 KiB 块循环 `read` 累积；块类型混用（str 与 bytes 混杂）抛 TypeError |
| `io.tee(reader, writer)` | 返回 TeeReader：从 reader 读到的内容同时写入 writer | TeeReader 只实现 Reader（`read`/`readAll`/`readLine`/`close`）；`close` 只逻辑关闭自身并关闭底层 reader，**不关闭 writer**（writer 生命周期归调用方） |
| `io.stdin` / `io.stdout` / `io.stderr` | 预建 File 实例，文本模式；stdin 只读、stdout/stderr 只写 | 模块属性，进程级共享 |
| `input(prompt="")` | 向 stdout 写 prompt（无换行）并冲刷，从 stdin 读一行，剥离结尾 `\n`（及其前的 `\r`）后返回 str | 首字符即 EOF 抛 EOFError（04-exceptions 层级中 EOFError 的既定用途）；读到部分行后 EOF 返回该部分行 |

通用约定：

- 除 `open` 的模式校验外，参数个数/类型不符一律抛 `TypeError`；上表标注 ValueError/OSError 的情形抛对应异常类。错误消息含接口名，形如 `io.copy: dst does not implement Writer protocol (missing write)`。
- **不做换行转换**：引擎层一律以二进制模式（`"rb"`/`"wb"` 等）调 `fopen`，读写字节原样往返（对齐 Go；文本/二进制的区别只在「读出装箱为 str（含 UTF-8 校验）还是 bytes」与「`read(n)` 按码点还是字节计数」）。CRLF 透传，`readLine` 以 `\n` 切分、行尾保留 `\r\n` 原文（剥离是调用方职责，用 `strings.trimSuffix`）。
- 文本模式读出的字节必须构成合法 UTF-8，否则抛 `UnicodeError`（ValueError 子类，04-exceptions §4）。
- 对已关闭 File 的一切读写/定位操作抛 `ValueError`（"I/O operation on closed file"，对齐 Python 语义）。

### 3. 引擎层接口（src/io/ms_file.h）

include guard `MSLANG_SRC_IO_MS_FILE_H_`，自包含（`<stdbool.h>`/`<stddef.h>`/`<stdint.h>`/`<stdio.h>`）。内部结构体不 typedef：

```c
struct MsFileIo {
  FILE* fp;           // owned unless stdStream is true
  bool readable;
  bool writable;
  bool binary;        // false: text mode (read counts runes, module layer boxes str)
  bool closed;        // logical close; for std streams fp stays open
  bool stdStream;     // wraps C stdin/stdout/stderr; never fclose'd
  bool lastWasWrite;  // update-mode ("r+") direction switch tracking
};

typedef enum {
  MS_FILE_OK = 0,
  MS_FILE_EOF,          // clean end of stream (not an error)
  MS_FILE_BAD_MODE,     // mode string outside the six allowed forms
  MS_FILE_SYS,          // OS error; *sysErrno carries errno
  MS_FILE_OOM
} MsFileStatus;

// Parses mode ("r" "w" "a" "rb" "wb" "r+") and opens path. All modes are
// passed to fopen in binary form ("rb"/"wb"/"ab"/"r+b"): no newline
// translation. On MS_FILE_SYS, *sysErrno carries the fopen errno.
MsFileStatus msFileIoOpen(struct MsFileIo* f, const char* path, const char* mode, int* sysErrno);

// Initializes f around an existing C stream (std streams). Never fails.
void msFileIoInitStd(struct MsFileIo* f, FILE* fp, bool readable, bool writable);

// Closes f. Idempotent. stdStream instances only fflush and set closed.
// Returns MS_FILE_SYS (with *sysErrno) when fclose/fflush fails.
MsFileStatus msFileIoClose(struct MsFileIo* f, int* sysErrno);

// Reads up to maxBytes raw bytes into buf. *outLen is the byte count;
// *outLen == 0 with MS_FILE_EOF means end of stream. A short read at EOF
// returns MS_FILE_OK with the partial count (next call reports EOF).
MsFileStatus msFileIoRead(struct MsFileIo* f, uint8_t* buf, size_t maxBytes,
    size_t* outLen, int* sysErrno);

// Text-mode read of up to maxRunes runes, appended to out (grows via the
// caller's MsStrBuf discipline). Reads whole UTF-8 sequences only (never
// splits a sequence). MS_FILE_EOF semantics as above. Does NOT validate
// UTF-8 beyond sequence framing; full validation is the module layer's job.
MsFileStatus msFileIoReadText(struct MsFileIo* f, struct MsStrBuf* out, int64_t maxRunes,
    int64_t* outRunes, int* sysErrno);

// Reads one line including the trailing '\n' (kept verbatim), appended to
// out. EOF before any byte yields MS_FILE_EOF and an empty append; EOF after
// a partial line yields MS_FILE_OK with the partial line.
MsFileStatus msFileIoReadLine(struct MsFileIo* f, struct MsStrBuf* out, int* sysErrno);

// Writes [data, len). *outWritten is the byte count accepted; a short write
// is MS_FILE_SYS with errno (or EIO when fwrite reports short without errno).
MsFileStatus msFileIoWrite(struct MsFileIo* f, const uint8_t* data, size_t len,
    size_t* outWritten, int* sysErrno);

// fflush. MS_FILE_OK on read-only streams (no-op).
MsFileStatus msFileIoFlush(struct MsFileIo* f, int* sysErrno);

// whence: 0 SEEK_SET / 1 SEEK_CUR / 2 SEEK_END (module layer validates).
// Returns the new absolute position via *outPos.
MsFileStatus msFileIoSeek(struct MsFileIo* f, int64_t offset, int whence,
    int64_t* outPos, int* sysErrno);

// ftell. Fails on closed streams (module layer pre-checks, so this is
// defensive).
MsFileStatus msFileIoTell(struct MsFileIo* f, int64_t* outPos, int* sysErrno);
```

引擎层要点：

- **更新模式方向切换**（`r+`）：C11 要求读→写或写→读之间必须有 `fflush`/`fseek` 等定位调用。引擎层在 `msFileIoWrite`/`msFileIoRead*` 入口检查 `f->lastWasWrite` 与本次方向的异同，方向翻转时先 `fseek(f->fp, 0, SEEK_CUR)`（读转写先 `fflush`），随后更新标志；`msFileIoSeek`/`msFileIoFlush` 把标志复位为「无方向」。
- **追加模式**（`"a"`）：`fopen("ab")` 的 append 语义由 C 运行时保证（写恒在末尾）；`seek` 允许调用但只影响后续读位置与 `tell`，不改变写行为，此语义在模块层文档注释固定。
- **`errno` 纪律**：每个引擎函数在 `stdio` 调用失败点立即 `*sysErrno = errno` 并返回 `MS_FILE_SYS`，不让 `errno` 跨层（10-c-style §5）；模块层随即 `msRaiseOSError(L, sysErrno, ...)`。
- 引擎层不含任何 `msAlloc` 之外的分配；`struct MsFileIo` 本身由模块层内嵌于 C 类型实例数据区（见第 5 节），引擎只借不拥有。

### 4. 文本/二进制语义与 EOF 约定

- **读出装箱**（模块层）：二进制模式 → `msNewBytes`；文本模式 → 先以任务 06 的 UTF-8 校验原语（假定名 `msUtf8Validate`）全量校验，合法则 `msNewStringN`，非法抛 `UnicodeError`（`"io: invalid UTF-8 in text-mode read"`）。`read(n)`（n ≥ 0）的文本路径经 `msFileIoReadText` 按码点上限读取，读到的字节必为整序列，校验后装箱；`read(-1)`/`readAll`/`readLine` 经 `msFileIoRead`/`msFileIoReadLine` 读字节到 `struct MsStrBuf` 后统一校验装箱。
- **EOF 哨兵**：`read(n)`（n > 0）与 `readLine` 在流耗尽时返回**空 str/bytes**——这是 `io.copy`/`io.readAll` 的循环终止条件，也是对一切 Reader 实现者的协议要求（鸭子协议的一部分，随模块文档固定）：「`read` 返回空数据即 EOF」。`read(-1)`/`readAll` 在空流上返回空 str/bytes。
- **写入计数**：`write` 返回字节数（两种模式一致）；文本模式写入 str 时即其 UTF-8 字节长度（`msStringLen` 的字节语义）。

### 5. File 类型接线（msDefineType）

- File 是 `MsTypeDef` 描述的 C 自定义类型（任务 33），`instanceSize = sizeof(struct MsFileIo)`，实例数据区即引擎结构体（`msCInstanceData` 取得，构造时清零）。类型对象由 `msStdIoRegister` 创建后由任务 33 的内建类型表持有（已是根），**不作为模块属性导出**——脚本只能经 `open`/`io.stdin` 等获得实例，`type(f)` 仍可正常显示 `<type 'File'>`。
- `init` 回调支持两种构造形态（第二种不对脚本可达，因类型对象不导出）：
  1. `(path: str, mode: str)`——`open` 内建的用户路径；经 `msFileIoOpen`，失败按 errno 抛 OSError 子类（构造失败返回 `NULL`，实例随之成为垃圾，任务 33 构造约定）。
  2. `(kind: int)`——kind ∈ {0, 1, 2}，内部构造标准流包装（`msFileIoInitStd`），供 `msStdIoRegister` 创建 `io.stdin/stdout/stderr`；因类型对象脚本不可达，该形态无脚本入口。
- 方法表（`MsMethodDef`，全部为 `static MsObject* ioFileXxx(MsState* L, int64_t argc, MsObject** argv)`，`argv[0]` 为实例）：`read`/`readAll`/`readLine`/`write`/`flush`/`close`/`seek`/`tell`/`__enter__`/`__exit__`。统一前置检查：取数据区 → `closed` 则 `msRaiseValueError(L, "I/O operation on closed file")` 返回 `NULL`；方向检查（如只读文件调 `write`）抛 `OSError`。
- `toString` 回调：`<file 'a.txt' mode 'r'>`（标准流为 `<file '<stdin>' mode 'r'>`）；`path` 不入数据区——File 数据区不持有脚本对象，`toString` 所需的路径显示名以 `char*` 形式挂在数据区尾部扩展字段（`msAlloc` 拷贝，`close`/finalize 释放；标准流为静态字符串字面量，不释放）。
- `finalize` 回调（GC 回收前，任务 33 纪律：只释放 C 侧资源）：未关闭且非标准流则 `fclose`；释放路径缓冲；不访问任何脚本对象、不调 `msRaise*`。
- `__enter__` 返回 `argv[0]`（自身）；`__exit__(excType, excValue, traceback)` 忽略三参（个数校验放宽为 `argc >= 1`，with 协议恒传三参），调 `close` 逻辑后返回 `msNewBool(L, false)`（不吞异常，03-syntax §3.2）。
- GC 纪律：File 数据区只含 `FILE*`/`char*`/标志位，无脚本对象引用，标记阶段无子引用（满足任务 33 的无 traverse 限制）。

### 6. 鸭子协议函数与 TeeReader

**`io.copy(dst, src)`**（拷贝缓冲 32 KiB，常量 `MS_IO_COPY_BUF_SIZE`）：

1. 协议预检：`msHasAttr(L, src, "read")` 与 `msHasAttr(L, dst, "write")`，缺失抛 `TypeError`（消息指明缺哪个方法）。
2. 构造 int 参数 `msNewInt(L, MS_IO_COPY_BUF_SIZE)` 并入根；循环：`chunk = msCallMethod(L, src, "read", 1, &nObj)`（`NULL` 则传播脚本异常）——chunk 必须是 str 或 bytes，否则 `TypeError`；长度为 0 → EOF 退出循环。
3. `written = msCallMethod(L, dst, "write", 1, &chunk)`：返回值必须是 int（否则 TypeError）；`written != len(chunk)` 为短写，抛 `OSError`（`"io.copy: short write"`，对齐 Go 的 `ErrShortWrite`）。
4. 字节计数：str 取 `msStringLen`（UTF-8 字节数）、bytes 取其长度，累加为 int64 返回。
5. 根纪律：`src`/`dst` 是 `argv` 根；`chunk` 在调用 `write` 前 `msRootPush`、调用后 `msRootPop`；循环内每次分配的临时对象在下一次分配前消费完毕。

**`io.readAll(reader)`**：`msHasAttr(L, reader, "readAll")` 为真则直接 `msCallMethod(L, reader, "readAll", 0, NULL)` 并原样返回（类型须为 str/bytes，否则 TypeError）；否则以 32 KiB 循环 `read`：首块类型决定结果类型，后续块类型不一致抛 `TypeError`；str 块经 `msStrConcat`、bytes 块经 `msNewBytes` 拼接（中间结果入根），EOF（空块）结束。

**`io.tee(reader, writer)` 与 TeeReader 的引用表**：

- 任务 33 禁止 C 类型数据区持有脚本对象。TeeReader 改为持有**模块级引用表**的槽位索引：注册时在 io 模块对象上挂一个隐藏属性 `__teeRefs`（list，模块对象是 GC 根，故表及其元素对 GC 可达），每个 TeeReader 占一个槽位，槽位为两元素 list `[reader, writer]`。
- TeeReader 的 `MsTypeDef.instanceSize = sizeof(struct MsIoTeeData)`：

```c
struct MsIoTeeData {
  int64_t slot;   // index into the module-owned __teeRefs list; -1 when closed
};
```

- 构造（`io.tee` 内联完成，TeeReader 类型对象同样不导出）：协议预检 reader 有 `read`、writer 有 `write`（缺失抛 TypeError）；扫描 `__teeRefs` 找首个空槽（元素为 nil）否则 `msListAppend` 新槽；建两元素 list 装入 `[reader, writer]` 写入槽位；创建实例并记 `slot`。
- 方法 `read(n=-1)`：按 `slot` 取回 `[reader, writer]`（`msListGet` 两次，writer 先入根再调 read），调 `reader.read(n)`，非空则调 `writer.write(chunk)` 并做与 `io.copy` 相同的类型/短写检查，返回 chunk；`readAll`/`readLine` 同构（基于 `reader` 的同名方法，缺失时 `readAll` 退化为循环 `read`、`readLine` 抛 `TypeError` 指明底层 reader 无该方法）。`close`：把槽位置 nil（归还槽位）、置 `slot = -1`、调用底层 reader 的 `close`（如有；无则跳过），不触碰 writer。已关闭（`slot < 0`）再调任何方法抛 `ValueError`。
- `finalize`：`slot >= 0` 时把槽位置 nil（**不调用底层 reader 的 `close`**——finalize 不得调用脚本对象方法，任务 33 纪律；底层 reader 自身有其 finalize/`with` 管理）。
- 该机制是任务 33「无 traverse 回调」限制下的既定规避方案；若后续版本为 `MsTypeDef` 增补 traverse 槽，TeeReader 可改为直接持有对象指针并删除引用表，脚本语义不变（此处显式记为演进点，不在本任务实施）。

### 7. 内建 open/input 与标准流注册

- `ms_std_io.h`（guard `MSLANG_STDLIB_MS_STD_IO_H_`，自包含 `<mslang/mslang.h>`）导出三符号：

```c
// Registers the "io" builtin module (functions copy/readAll/tee, attributes
// stdin/stdout/stderr, and the File/TeeReader C types). Called once during
// interpreter startup (msNewState's stdlib wiring, task 18's chain).
MsResult msStdIoRegister(MsState* L);

// Bodies of the builtin functions open(path, mode="r") and input(prompt="").
// Linked into the builtin table (src/vm/ms_builtin.c, task 10) by this task.
MsObject* msStdIoOpenBuiltin(MsState* L, int64_t argc, MsObject** argv);
MsObject* msStdIoInputBuiltin(MsState* L, int64_t argc, MsObject** argv);
```

- `msStdIoOpenBuiltin`：解析 `(path: str, mode: str = "r")`，取 File 类型对象（模块静态持有，已是根）经 `msCallObject(L, fileType, 2, args)` 构造（任务 33 构造约定），异常原样传播。
- `msStdIoInputBuiltin`：解析 `(prompt: str = "")`；把 prompt 写入 `io.stdout` 的引擎实例并 `msFileIoFlush`；随后从 `io.stdin` 的引擎实例经 `msFileIoReadLine` 读一行：首字节即 EOF → `msRaise` 一个 `EOFError` 实例（假定任务 23 提供按类名构造异常的接口，定名以任务 23 文档为准；退化路径为 `msRaiseRuntimeError` 并在测试中按实际类断言）；剥离结尾 `\n` 及可选 `\r`，UTF-8 校验（非法抛 `UnicodeError`）后 `msNewStringN` 返回。`input` 走引擎实例而非脚本层 `io.stdin.readLine()`，避免依赖脚本侧对象状态（用户可能 `io.stdin.close()`——逻辑关闭后 `input` 仍可用，语义对齐 Python 的独立 stdin 读取）。
- 内建表接线：在任务 10 的 `src/vm/ms_builtin.c` 的 `msBuiltinTable` 追加 `{"open", msStdIoOpenBuiltin}` 与 `{"input", msStdIoInputBuiltin}` 两行并 include `"stdlib/ms_std_io.h"`（对任务 10 文件的最小增量改动，属本任务范围；03-syntax §8 的名字解析顺序使两者可被用户全局变量遮蔽，与既有内建一致）。
- `msStdIoRegister` 流程：`msDefineType` 建 File 与 TeeReader 两个类型 → `msCallObject` 建三个标准流实例入根 → `msRegisterModule(L, &ioModuleDef)` 注册 `{copy, readAll, tee}` 三函数 → 经任务 24/33 的注册表查询接口（假定名 `msModuleRegistryFind`，定名以对应任务文档为准）取回模块对象，`msSetAttr` 挂 `stdin`/`stdout`/`stderr` 与隐藏属性 `__teeRefs`（`msNewList`）→ 出根返回 `MS_OK`。

### 8. 与 os（任务 36）的边界

- **os 管「路径与元数据」，io 管「字节流」**：`os.stat`/`os.listDir`/`os.mkdir`/`os.remove`/`os.rename`/`os.exists`/`os.environ`/`os.exec` 等一律不产生也不消费 File/Reader/Writer；`open` 是打开文件的唯一入口，os 不提供 `os.open`/`os.fdopen` 之类（07-stdlib §6 无此函数，刻意不增）。
- **标准流的唯一权威是 io**：`io.stdin`/`io.stdout`/`io.stderr` 由本任务创建。03-syntax §9.1 的示例写作 `os.stderr`——该别名（若保留）应由任务 36 以「引用 io 模块的同一对象」实现，**不得**另建包装实例（避免两份 closed 状态漂移）；任务 36 文档撰写/实现时对齐此约定（其文档尚不存在，定名以任务 36 为准）。
- `os.exec` 返回的 `(exitCode, stdout, stderr)` 是 str 而非 Reader——子进程输出一次性收齐，不经流抽象（07-stdlib §6 既定签名），本任务不为其提供 Reader 适配（如有需要由后续任务评估）。
- `fmt.fprintf(writer, ...)`（任务 19）与 `print(..., file=w)`（任务 10）只依赖 Writer 鸭子协议（`write`/`flush`），本任务保证 File 满足协议即可，无需改动这两个任务的实现。

### 9. 内存与 GC 纪律清单

- 引擎层：无堆分配（`struct MsStrBuf` 增长经 `msRealloc`，所有权归调用者）；`errno` 就地转换。
- 模块层堆分配：File 路径显示名（`msAlloc` 拷贝，`close`/finalize 时 `msFree`，标准流用静态字面量不释放）；`readAll`/`readLine`/`io.readAll` 的 `struct MsStrBuf`（函数内 `msStrBufFree`，失败路径先释放再返回 `NULL`）。
- 根纪律：`argv` 自动是根；跨 `msCallMethod`/分配存活的局部对象（`io.copy` 的 chunk、`io.readAll` 的累积串、tee 构造中的槽位 list、`msStdIoRegister` 中的标准流实例）一律 `msRootPush`/`msRootPop` LIFO 配对。
- `msAsCString` 指针窗口：取 path/mode 指针后在任何可能分配的调用前完成引擎调用（先 `msFileIoOpen` 后装箱），与任务 35/20 同纪律。
- finalize 纪律（File 与 TeeReader 同）：只释放 C 资源、只清引用表槽位，不访问脚本对象、不调 `msRaise*`、不分配 GC 对象。

## 实现步骤

1. 建 `src/io/ms_file.h` / `ms_file.c` 骨架：guard、`struct MsFileIo`、`MsFileStatus`、全部函数空实现；接入构建（并入 `mslang` 库目标）。验证：全平台编译通过。
2. 实现 `msFileIoOpen`（六种 mode 解析 → `fopen` 二进制形式）与 `msFileIoClose`（幂等、标准流逻辑关闭）。验证：经步骤 6 的模块层脚本断言六种 mode 的打开/关闭与非法 mode 的 ValueError。
3. 实现 `msFileIoRead`/`msFileIoReadText`（码点整序列聚合）/`msFileIoReadLine`/`msFileIoWrite`/`msFileIoFlush`，含 `r+` 方向切换处理与 `errno` 就地捕获。验证：读/写/行读/追加/更新模式的脚本行为断言（步骤 6 后）。
4. 实现 `msFileIoSeek`/`msFileIoTell`。验证：三种 whence、负位置报错、seek 后 tell/读的一致性。
5. 建 `stdlib/ms_std_io.{c,h}`：`msDefineType` 落地 File 类型（init 两形态、十个方法、toString、finalize），`msStdIoRegister` 骨架（类型 + 空模块注册）。验证：C 侧注册成功；脚本尚不可达，行为验证待下一步。
6. 实现 File 各方法的模块层包装（closed 检查、方向检查、文本 UTF-8 校验、str/bytes 装箱）与 `open` 内建（含内建表接线）；创建 `io.stdin/stdout/stderr` 并挂为模块属性。验证：`f := open(p); print(f.readAll())` 读通夹具文件；`print("x", file=io.stdout)` 输出正确。
7. 实现 `input` 内建。验证：空 stdin 下 `input()` 抛 EOFError 的 `try/except` 断言；prompt 写 stdout 无换行。
8. 实现 `io.copy` 与 `io.readAll`（协议预检、32 KiB 循环、短写检查、类型一致性）。验证：File→File 拷贝字节数与内容一致；脚本 class 实现的假 Reader/Writer 同样被接受（鸭子协议回归）。
9. 实现 `io.tee` 与 TeeReader（引用表、四个方法、close/finalize 槽位归还）。验证：tee 读取的旁路写入内容正确；大量短命 TeeReader 在 GC 后无泄漏、槽位复用。
10. 编写 `tests/ms/stdlib/io/` 全部测试脚本与 `tests/fixtures/io/` 夹具（见测试方案），`python run_tests.py` 全绿；Win/Linux/macOS × Debug/Release 构建验证，Debug（ASAN / `/RTC`）无内存错误与泄漏。

## 测试方案

本任务编号 ≥ 09，一律用 ms 脚本测试（`tests/ms/stdlib/io/`），由仓库根 `run_tests.py` 驱动；任务 40（testing 模块）尚不存在，断言用内建 `assert` + `print`，成功脚本末尾 `print("<用例名> ok")`；负向用例用 `try/except`（任务 23 已落地）在脚本内断言异常类型，未捕获即 `assert(false)`。本任务只交付设计文档，测试实体随实现步骤编写。

夹具与临时文件约定：

- 只读夹具 `tests/fixtures/io/`（随实现创建，内容固定）：`hello.txt`（已知 ASCII 多行、末行有换行）、`utf8.txt`（含多字节码点如 `中`/`é` 的已知文本）、`no_eol.txt`（末行无换行）、`empty.txt`（0 字节）、`binary.bin`（字节 0–255 各一）、`crlf.txt`（CRLF 行尾，验证不转换语义）。
- 夹具路径以仓库根为 cwd 的相对路径（`tests/fixtures/io/...`，同任务 38 的 cwd 约定假定；若 `run_tests.py` 的 cwd 约定不同，改用任务 36 的 `os.cwd()` 推导——以任务 36 文档定名为准）。
- **写入类用例只写 `build/test-tmp/io/` 下**（构建产物只落 `build/` 的纪律延伸）；每个写测试脚本开头 `os.mkdirAll("build/test-tmp/io")`、结尾 `os.remove` 清理自建文件（os 接口名以任务 36 文档为准）。

测试文件清单与覆盖点：

- `open_modes.ms`：六种 mode 逐一——`"r"` 读夹具、`"w"` 建文件写后复读、`"a"` 追加（原有内容保留、新内容在尾）、`"rb"`/`"wb"` 二进制往返（bytes 相等）、`"r+"` 读写混合（读后写、写后 seek 回读）；`open` 缺省 mode 为 `"r"`。负向：非法 mode（`"x"`/`"w+"`/`""`）抛 ValueError；`open(42)` 抛 TypeError；不存在路径 `"r"` 抛 FileNotFoundError；只读文件 `write` 抛 OSError；只写文件 `read` 抛 OSError。
- `read_text.ms`：文本模式 `read(n)` 按码点计数（多字节夹具逐码点切片断言）、`read(0)` 得空串、EOF 后 `read(1)` 得 `""`、`read(-1)` 与 `readAll()` 等价且为空流返回 `""`、连续 `read` 的位置推进；`no_eol.txt` 的 `readLine` 末行无换行原文返回、再读得 `""`；CRLF 行尾原样保留（不转换）；非法 UTF-8 内容（用 `binary.bin` 以文本模式读）抛 UnicodeError。
- `read_write_binary.ms`：`binary.bin` 的 `read(n)` 逐段字节断言（bytes 相等）、`readAll` 全长 256；`"wb"` 写 bytes 后字节级复读一致；`write` 返回字节数；负向：二进制模式 `write("str")` 与文本模式 `write(b"x")` 抛 TypeError。
- `seek_tell.ms`：`tell()` 初始 0；读 n 后位置推进；`seek(0, 0)` 回头复读一致；`seek(-2, 2)` 相对末尾；`seek(3, 1)` 相对当前；`seek` 返回新位置；负向：whence 为 3/-1 抛 ValueError；`seek(-1, 0)` 抛 OSError；关闭后 `seek`/`tell`/`read` 抛 ValueError。
- `with_close.ms`：`with open(p) as f` 块内可读、块后文件已关闭（再 `read` 抛 ValueError）；`__exit__` 不吞异常（with 块内 `raise`，`try/except` 在外层捕获且文件已关闭）；显式 `close()` 幂等（连续两次不抛错）；`f.__enter__()` 返回 `f` 自身（`is` 断言）。
- `io_copy.ms`：File→File 拷贝夹具，返回字节数与文件大小一致、目标内容字节级一致；空文件拷贝返回 0；**鸭子协议**：脚本 `class FakeReader { func read(self, n) { ... } func close(self) {} }`（分块喂数据后返回 `""`）与 `class FakeWriter`（收集写入）被 `io.copy` 接受；负向：src 缺 `read` 或 dst 缺 `write` 抛 TypeError；FakeReader 返回 int 抛 TypeError；FakeWriter 返回短计数抛 OSError；reader 内部 `raise` 的异常原样传播。
- `io_readall.ms`：`io.readAll(f)` 与 `f.readAll()` 结果一致；对有 `readAll` 方法的 FakeReader 走直调路径、对只有 `read` 的走循环路径，结果一致；负向：块类型混用（FakeReader 交替返回 str 与 bytes）抛 TypeError。
- `io_tee.ms`：`t := io.tee(reader, writer)` 后 `t.read(n)` 返回值与 writer 收集内容一致；`t.readAll()` 全量旁路；`t.close()` 关闭底层 reader（其再读抛 ValueError）但 writer 仍可写；负向：tee 缺协议方法抛 TypeError；tee 的 reader 数据经 writer 短写抛 OSError；TeeReader 关闭后再读抛 ValueError。
- `std_streams.ms`：`io.stdout.write("...")` 与 `io.stderr.write("...")` 返回字节数（不比对 stdout 全文，避免驱动依赖）；`io.stdout.flush()` 正常；`io.stdin` 只读（`write` 抛 OSError）、`io.stdout` 只写（`read` 抛 OSError）；标准流 `close()` 幂等且不崩溃（逻辑关闭）；`print("msg", file=io.stderr)` 不抛错（任务 10 的 `file` 协议对接回归）。
- `input_builtin.ms`：`run_tests.py` 以空 stdin 驱动脚本——`input()` 抛 EOFError 的 `try/except` 断言；`input("q> ")` 的 prompt 输出到 stdout（配 `input_builtin.out` 同伴文件全文比对，若 `run_tests.py` 支持 `.out` 比对则启用，否则仅断言异常路径）。交互式输入（管道喂入行）依赖驱动能力，本阶段不测，记入已知未覆盖项。
- `errors_type.ms`：参数个数/类型矩阵——`f.read("1")`、`f.seek(0, "x")`、`io.copy(f)`、`io.tee(f)`、`io.readAll(42)`、`f.write()` 空参等抛 TypeError；`f.read(-2)` 抛 ValueError；各用例按首触发点拆分为若干小脚本（约定同任务 20/38）。
- `gc_stress.ms`：循环建弃大量 File（不 close，靠 finalize 回收）与 TeeReader，穿插分配越过 GC 阈值，断言进程不崩溃、行为正确、引用表槽位复用（建弃 N 个后 `len` 不随 N 线性增长的间接行为断言：循环尾部仍可正常 tee）。

## 验收标准

- [ ] `src/io/ms_file.{c,h}` 与 `stdlib/ms_std_io.{c,h}` 存在；guard 分别为 `MSLANG_SRC_IO_MS_FILE_H_` 与 `MSLANG_STDLIB_MS_STD_IO_H_`；头文件自包含；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsFileIo`/`struct MsIoTeeData` 不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] 07-stdlib §7 的全部接口落地：Reader/Writer 鸭子协议方法集、`io.copy`/`io.readAll`/`io.tee`、File 的 `read/readAll/readLine/write/flush/close/seek/tell/__enter__/__exit__`；mode 恰为 `"r" "w" "a" "rb" "wb" "r+"` 六种；语义与「详细设计」第 2 节总表逐条一致（EOF 空数据哨兵、write 返回字节数、不做换行转换、文本模式 UTF-8 校验抛 UnicodeError、关闭后操作抛 ValueError）。
- [ ] File/TeeReader 经 `msDefineType` 创建，类型对象不作为模块属性导出；File 数据区不持有脚本对象；TeeReader 经 `__teeRefs` 引用表持有 reader/writer，finalize 只清槽位不调脚本方法（任务 33 纪律）；标准流 `fclose` 永不发生。
- [ ] `open`/`input` 进入内建表（`src/vm/ms_builtin.c` 增量两行），`open` 默认 `mode="r"`；`input` 空 stdin 抛 EOFError、剥离行尾换行；`io.stdin/stdout/stderr` 可作为 `print` 的 `file` 目标（协议回归）。
- [ ] 引擎层无平台分支（纯 C11 stdio）、`errno` 不跨层（调用点立即转 OSError 子类）；`r+` 读写方向切换符合 C11 定位要求。
- [ ] `io.copy`/`io.readAll`/`io.tee` 对脚本 class 实现的假 Reader/Writer 成立（鸭子协议非名义类型）；短写抛 OSError；reader 内异常原样传播。
- [ ] 与 os 的边界落实：本任务不产生 os 侧改动；测试仅用 os 的目录/删除函数管理 `build/test-tmp/io/`；`os.stderr` 别名约定已在第 8 节写明待任务 36 对齐。
- [ ] 模块层遵守 GC 根纪律与 `msAsCString` 指针窗口约定；失败路径逆序释放资源；`tests/fixtures/io/` 夹具只读、写测试产物只落 `build/test-tmp/io/`。
- [ ] `tests/ms/stdlib/io/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿；`ctest --test-dir build` 并入通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告（含未 close 文件的 finalize 路径与 TeeReader 引用表）。
- [ ] 无 TBD/TODO 占位；对任务 06/10/20/23/24/33/36 的接口假定（UTF-8 原语、`struct MsStrBuf`、`msModuleRegistryFind`、EOFError 构造、os 测试辅助函数名）在实现时已按对应任务文档对齐定名。
