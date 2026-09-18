# 39 标准库：time

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [18 C API 基础](18-c-api-foundation.md) |

## 任务目标

交付 C 内建标准库模块 `time`（`stdlib/ms_std_time.h` / `stdlib/ms_std_time.c`），完整实现 `docs/language/07-stdlib.md` §9 的 `time` 部分共 5 个函数：时钟（`now` / `monotonic`）、睡眠（`sleep`）、时间戳与文本互转（`format` / `parse`）。`format`/`parse` 采用 Go 参考时刻风格的 layout 记号，时间戳一律按 UTC 解释（本地时区与对象化 API 属 `datetime` 模块，任务 52 的 v0.4 范围）。

本任务顺带把时钟与睡眠的系统调用下沉为平台原语层（`src/platform/ms_clock.h` + `ms_clock_posix.c` / `ms_clock_win.c`，编译期择一）：单调钟、Unix 墙钟、纳秒睡眠三个原语，是 `src/platform/` 继任务 38（`ms_path*`）与任务 36（`ms_os*`）之后的又一组入住者。原语命名直接采用任务 42 文档预定的 `msClockMonotonicNs` / `msClockUnixEpochNs`，使任务 42（平台抽象层，v0.3）落地时的「吸收对齐」退化为文件移动与头文件合并，不发生改名迁移；线程/原子/socket/dlopen 原语不在本任务，仍由任务 42 补齐。

完成后，ms 脚本 `import "time"` 即可使用全部功能，并经 `tests/ms/stdlib/time/` 下的脚本测试验证。

## 设计依据

- [07-stdlib.md](../language/07-stdlib.md)
  - §0：`time` 属 **C 内建模块**，源码位于 `stdlib/` 目录；函数命名小驼峰。
  - §9：`time` 函数清单（本任务的功能边界，逐条对应，不增不减）；`time.now` 返回 float Unix 秒（含小数）；`time.monotonic` 为单调钟；`time.format(ts, layout)` / `time.parse(layout, text)` 只有签名无 layout 语义——layout 记号集由本任务固定（见「详细设计」第 4 节，取 Go 参考时刻风格，与模块的 Go 命名渊源一致）。
  - §25：`time.sleep` 要求协程感知（挂起协程而非 OS 线程）——协程属 v0.3（任务 43/46），本任务先交付阻塞当前线程的实现，差距在「详细设计」第 7 节显式承认。
- [02-types.md](../language/02-types.md) §3.1/§3.2（int 为 int64、float 为 IEEE 754 双精度；时间戳以 float 秒与 int 纳秒两种口径出现）、§3.3（`int op float` → float，不存在 float → int 隐式转换——数值参数提取约定与任务 21 math 模块一致）、§4（str 为不可变 UTF-8 序列，layout 与文本一律 UTF-8 持有）。
- [04-exceptions.md](../language/04-exceptions.md) §4 内建异常层级：参数类型错误抛 `TypeError`，数值域/格式错误抛 `ValueError`；任务 23 已落地异常系统，本任务抛真实异常实例，ms 测试用 `try/except` 断言。
- [05-modules.md](../language/05-modules.md) §2/§7：内建 C 模块经内建注册表解析，优先级高于搜索路径，不可被同名脚本遮蔽；`import "time"` 由任务 24 提供。
- [09-c-api.md](../language/09-c-api.md) §3（GC 根栈纪律；`msAsCString` 缓冲区「下一次分配前有效」）、§5（`msNewFloat`/`msNewNil`/`msNewStringN`/`msTypeOf`/`msAsInt`/`msAsFloat`/`msAsCString`/`msStringLen`）、§8（`msRaiseTypeError`/`msRaiseValueError` 错误约定，C 函数出错返回 `NULL`）、§9（`MsCFunction`/`MsMethodDef`/`MsModuleDef`/`msRegisterModule` 注册机制）。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）；§5「禁止 errno 跨层传播」（`nanosleep` 的 `EINTR` 等在原语层内消化）；§9 平台抽象（本任务的 `ms_clock_*` 即该规则的落地场所——`clock_gettime`/`QueryPerformanceCounter`/`GetSystemTimePreciseAsFileTime`/`Sleep` 只允许出现在 `src/platform/`）。
- [11-project-layout.md](../language/11-project-layout.md) §1（`stdlib/`、`src/platform/`、`tests/ms/` 的目录位置）。
- [README.md](README.md) 测试约定：本任务编号 ≥ 09 一律用 ms 脚本测试；任务 40（testing 模块）之前用内建 `assert` + `print`，由仓库根 `run_tests.py` 驱动。
- 任务 18（C API 基础）提供：公开头文件族与标准库启动注册链（`msNewState` 内统一调用各 stdlib 模块注册函数，任务 19/20/21/35/36 已建立同模式）。
- 任务 20（strings）提供：可增长字节缓冲 `struct MsStrBuf` / `msStrBufInit` / `msStrBufPut` / `msStrBufFree`（`src/object/ms_str_op.h`，`format` 的输出组装所需）。
- [42 平台抽象层](42-platform-layer.md)：其文档已预定时钟原语名 `msClockMonotonicNs` / `msClockUnixEpochNs` 并声明「任务 39 的最小时钟封装并入 `msClock*`」；本任务直接以该命名落地（睡眠原语 `msClockSleepNs` 为本文新增命名，任务 42 落地时与其 `msThreadSleepMs` 的关系一并归并，见「详细设计」第 2 节）。
- 假定接口（实现时以对应任务文档定名为准）：任务 02 的 `msAlloc`/`msRealloc`/`msFree` 与 `MS_ASSERT`；任务 20 的 `struct MsStrBuf` 系列；任务 23 的异常类挂载（`msRaiseValueError` 等公开 API 签名已由任务 18 定稿，不受异常系统内部接口影响）。

## 详细设计

### 1. 文件布局与分层

```
src/platform/
├── ms_clock.h         # 时钟/睡眠原语声明（唯一头文件，无平台条件编译）
├── ms_clock_posix.c   # POSIX 实现（clock_gettime / nanosleep）
└── ms_clock_win.c     # Win32 实现（QPC / FILETIME / Sleep）
stdlib/
├── ms_std_time.h      # 模块对外声明：注册入口
└── ms_std_time.c      # 5 个 MsCFunction 包装 + layout 引擎 + MsModuleDef 注册表
```

分层职责（与任务 36/38 的「平台原语层 + 模块层」模式一致）：

- **平台原语层**（`ms_clock_posix.c` / `ms_clock_win.c`，CMake 按 `WIN32` 择一编译）：唯一允许包含 `<time.h>` 的 POSIX 部分与 `<windows.h>` 的地方；`EINTR` 重试、QPC 频率换算、FILETIME 纪元差全部封装在内。原语层**零堆分配**、**不触碰 `MsState`**，接口只用定宽整数。
- **模块层**（`stdlib/ms_std_time.c`）：`MsCFunction` 薄封装——参数个数/类型校验、纳秒↔float 秒换算、layout 记号引擎（平台无关纯算术）、装箱为脚本对象、错误映射。全部 `static`，唯一导出符号是注册入口。民用历法换算（civil calendar）是纯算术，不放平台层，以文件内 `static` 函数实现。

### 2. 平台原语层（src/platform/ms_clock.h）

include guard `MSLANG_SRC_PLATFORM_MS_CLOCK_H_`，自包含（`<stdint.h>` + 任务 02 的 `"core/ms_result.h"`，后者仅为将来扩展保留——本组原语全部不失败，不返回 `MsResult`）。命名与任务 42 文档预定的目标名逐字一致：

```c
// src/platform/ms_clock.h

// Monotonic clock, nanoseconds from an unspecified epoch; never moves
// backward within measurement error. Backs time.monotonic and, later,
// the scheduler timers (task 46). Only meaningful for differences.
uint64_t msClockMonotonicNs(void);

// Wall clock, nanoseconds since the Unix epoch (may jump on NTP/manual
// adjustment). Backs time.now.
int64_t msClockUnixEpochNs(void);

// Blocks the current thread for at least ns nanoseconds (OS scheduling
// jitter may extend it). ns == 0 is a no-op. Task 42 folds this primitive
// together with its msThreadSleepMs into the unified platform layer.
void msClockSleepNs(uint64_t ns);
```

后端实现要点：

- **POSIX**（`ms_clock_posix.c`）：`clock_gettime(CLOCK_MONOTONIC)` 与 `clock_gettime(CLOCK_REALTIME)`，秒×10⁹+纳秒合成（乘加均在 64 位内，当前纪元纳秒值 ≈1.7×10¹⁸ < 2⁶³，无溢出）；`msClockSleepNs` 用 `nanosleep`，`EINTR` 时以剩余时间重试直至睡满（10-c-style §5：errno 不出本层）。macOS 无 `CLOCK_MONOTONIC_RAW` 之类的差异不引入——统一 `CLOCK_MONOTONIC`。
- **Win32**（`ms_clock_win.c`）：单调钟用 `QueryPerformanceCounter` + `QueryPerformanceFrequency`，换算用「`ticks / freq` 与 `ticks % freq` 分治」的 64 位溢出安全算法（任务 42 文档已固定该手法，本任务先行落地同一写法）；频率在首次调用时经 `static` 局部初始化缓存（C11 不保证线程安全初始化——v0.2 单线程解释器下安全，任务 42 落地线程后改为一次性初始化原语，差异在此显式承认）。墙钟用 `GetSystemTimePreciseAsFileTime`（不可得的旧系统回退 `GetSystemTimeAsFileTime`），FILETIME（1601 纪元、100ns 刻度）减 11644473600 秒偏移后换算为 Unix 纳秒。睡眠用 `Sleep(ms)`，毫秒数向上取整保证「至少睡满」语义；不做 `timeBeginPeriod` 进程级计时分辨率提升（全局副作用，列入路线图评估）。
- 精度声明（写入头文件注释）：单调钟用于差值测量，分辨率 ≥1ms 保证（实际 POSIX 纳秒级、Windows 为 QPC 频率级）；睡眠粒度受 OS 调度限制，Windows 默认约 15.6ms 档。测试断言一律用容忍区间（见「测试方案」）。

**与任务 42 的归并计划**：任务 42 落地时把本头文件的三个声明并入 `src/platform/ms_platform.h`、两份后端并入 `ms_platform_posix.c` / `ms_platform_win32.c`，删除 `ms_clock.*`；因命名已一致，`stdlib/ms_std_time.c` 的调用点只需改 include 行。任务 42 另定义 `msThreadSleepMs`（毫秒粒度线程睡眠）：`msClockSleepNs` 语义更细且为 time 模块所需，归并时保留 `msClockSleepNs` 并令 `msThreadSleepMs` 以其为底或并存，由任务 42 定稿，本任务不预设。

### 3. 脚本函数语义总表（唯一权威）

07-stdlib §9 只给出签名与一行注释；下表逐函数固定语义，规范未写明处以本表为准：

| 函数 | 语义 | 参数与返回 | 失败 |
|---|---|---|---|
| `time.now()` | 当前 Unix 墙钟时刻 | 无参；返回 float 秒（含小数，纪元 1970-01-01T00:00:00Z） | 参数个数不符抛 TypeError |
| `time.monotonic()` | 单调钟读数，仅差值有意义 | 无参；返回 float 秒（含小数），起点未指定 | 参数个数不符抛 TypeError |
| `time.sleep(seconds)` | 睡眠至少 `seconds` 秒，返回 nil | `seconds` 为 int 或 float；int 按秒、float 按小数秒 | 负数、NaN、±inf、换算后超出 uint64 纳秒抛 ValueError；bool/str 等其他类型抛 TypeError |
| `time.format(ts, layout)` | 把时间戳按 layout 渲染为 UTC 文本 | `ts` 为 int 或 float（Unix 秒），`layout` 为 str；返回 str | `ts` 为 NaN/±inf、或其 UTC 民用日期年份超出 [1, 9999] 抛 ValueError；类型不符抛 TypeError |
| `time.parse(layout, text)` | 按 layout 解析文本为 Unix 秒 | 两者均为 str；返回 float 秒（含毫秒小数，若 layout 含 `.000`） | 文本与 layout 不匹配、字段越界（月/日/时/分/秒非法）抛 ValueError；类型不符抛 TypeError |

通用约定：

- 数值参数（`sleep` 的 `seconds`、`format` 的 `ts`）接受 int 与 float（int 提升为 double，与任务 21 的混合算术约定一致）；**bool 不是数值参数**，`time.sleep(true)` 抛 TypeError（与任务 21 同约定）。
- `now`/`monotonic` 的 float 精度：纳秒值先转 double 再除以 10⁹。当前纪元纳秒 ≈1.7×10¹⁸ 已超 2⁵³，double 尾数只保约 0.1–1µs 有效分辨率——对脚本侧足够（日志时间戳、耗时测量），此固有精度损失在头文件与本节显式承认，不做整数对（秒+纳秒）返回形式（规范定死了 float 秒口径）。
- UTC 口径：`format`/`parse` 不涉本地时区，`time.now()` 的纪元本身无时区概念；`datetime` 模块（任务 52，纯 .ms）在本模块之上提供时区与对象化 API。此边界与 07-stdlib §9 的分层一致。
- 模块层返回值构造失败（OOM）时 `msNew*` 返回 `NULL` 且错误状态已置，直接向上传播 `NULL`。

### 4. layout 记号集（Go 参考时刻风格）

`format`/`parse` 共用的 layout 引擎：layout 是普通 str，引擎从左到右做**最长匹配**扫描，命中下表记号则展开/消费对应字段，未命中字符为字面量，逐字节相等（format 原样输出、parse 要求文本逐字节相同）。记号表（取自 Go 参考时刻 `2006-01-02 15:04:05.000` 的固定子集，v0.2 不支持 12 小时制、时区名、星期与月名）：

| 记号 | 含义 | format 行为 | parse 行为 |
|---|---|---|---|
| `2006` | 年 | 4 位零填充（`0001`–`9999`） | 恰好 4 位数字，范围 [1, 9999] |
| `01` | 月 | 2 位零填充 | 恰好 2 位数字，范围 [1, 12] |
| `02` | 日 | 2 位零填充 | 恰好 2 位数字，按年月合法性校验（含闰年） |
| `15` | 时（24 小时制） | 2 位零填充 | 恰好 2 位数字，范围 [0, 23] |
| `04` | 分 | 2 位零填充 | 恰好 2 位数字，范围 [0, 59] |
| `05` | 秒 | 2 位零填充 | 恰好 2 位数字，范围 [0, 59]（不接纳闰秒 60） |
| `.000` | 毫秒 | 小数部分**截断**到毫秒后 3 位零填充（对齐 Go `.000` 的截断语义） | 恰好 3 位数字，解释为毫秒 |

扫描规则与边界：

- 匹配优先级按记号长度：`2006`（4 字符）先于两位记号（`01`/`02`/`15`/`04`/`05`），`.000` 以 `.` 起始单独识别；两位记号的判定是「当前位置起的两个字符恰为表中记号」。layout 中连续数字串如 `20060102` 依最长匹配切为 `2006` `01` `02`，无歧义。
- 字面量与记号的冲突：layout 中任何与记号前缀相同的字面文本都会被识别为记号（如字面量 `"01"` 无法表达）。这是 Go 风格 layout 的固有性质，文档化即可；需要纯字面输出的场景由脚本自行拼字符串。
- 同一记号在 layout 中出现多次：format 多次展开同一字段；parse 以**最后一次**出现为准（Go 允许重复字段，行为同样以后写覆盖，与之对齐）。
- 解析缺省值：layout 未包含的字段取纪元基线——年 1970、月 1、日 1、时/分/秒 0、毫秒 0（Go 的缺省年是 0 年，超出本模块的 [1, 9999] 合法域，故取 1970，差异显式固定为模块契约）。
- 严格匹配：parse 要求 text 全长消费完毕（首尾无多余字符），字段宽度固定不可伸缩（`"1"` 不匹配 `01` 记号）；任一违例抛 ValueError，消息含 layout 与 text 摘要。

### 5. 民用历法换算（Unix 秒 ↔ UTC 年月日时分秒）

平台无关纯算术，文件内 `static` 函数，采用 Howard Hinnant 的无分支历法算法（`days_from_civil` / `civil_from_days`，命题 proven、无查表），proleptic Gregorian 历：

```c
// stdlib/ms_std_time.c 内部（static，不 typedef）

struct MsCivilTime {        // broken-down UTC civil time
  int64_t year;             // [1, 9999] enforced by callers
  int month;                // 1..12
  int day;                  // 1..31 (validated against month/year)
  int hour;                 // 0..23
  int minute;               // 0..59
  int second;               // 0..59
  int millis;               // 0..999
};

// Splits unixSecs into days-since-epoch and seconds-of-day using floor
// division (correct for negative inputs), then converts days to
// year/month/day via Hinnant's civil_from_days. Returns false when the
// resulting year is outside [1, 9999].
static bool timeCivilFromUnix(int64_t unixSecs, struct MsCivilTime* out);

// Inverse of timeCivilFromUnix. Fields must already be validated.
static int64_t timeUnixFromCivil(const struct MsCivilTime* civil);

// Proleptic Gregorian leap-year test.
static bool timeIsLeapYear(int64_t year);
```

要点：

- 天数与日内秒的分拆用 floor 除法（负数时间戳正确落位，如 `-1` 秒 = 1969-12-31 23:59:59）；年份合法性由 `timeCivilFromUnix` 返回值与 parse 的字段校验两侧把守。
- 闰年规则（能被 4 整除且不能被 100 整除，或能被 400 整除）只服务 parse 的日校验（2 月 29 日）与 Hinnant 算法内部的月长表。
- 纪元→天数→年月的常数（719468 等）照 Hinnant 原文落地，实现注释附出处（10-c-style §7「非显然算法附参考」）。

### 6. format / parse 引擎流程

**format**（`ts` → str）：

1. `ts` 为 float 时拒绝 NaN/±inf（ValueError）；向负无穷取整得 `int64_t secs` 与非负小数 `frac ∈ [0, 1)`；`millis = (int)(frac * 1000.0)`（截断，见第 4 节）。
2. `timeCivilFromUnix(secs, &civil)`，年份越界抛 ValueError（`timestamp out of range`）。
3. 初始化 `struct MsStrBuf out`（任务 20），扫描 layout：命中记号把对应字段按宽度零填充写入（手写定宽数字输出，不经过 `snprintf`——避免区域设置依赖与格式串开销），字面量逐字节 `msStrBufPut`。
4. `msNewStringN(L, buf, len)` 装箱后 `msStrBufFree`；OOM 路径先释放缓冲再返回 `NULL`。

**parse**（layout + text → float 秒）：

1. 双指针同步扫描：layout 指针按第 4 节最长匹配取记号，text 指针按记号的定宽消费数字（`01` 消费 2 位、`2006` 消费 4 位）；字面量要求逐字节相等。任一失配、宽度不足、非数字、text 提前结束或扫描后 text 有剩余，抛 ValueError。
2. 数字字段直接按位累积（`v = v * 10 + digit`），字段量少、宽度 ≤4，无溢出风险；收集进 `struct MsCivilTime`（初值为第 4 节缺省基线）。
3. 字段合法性校验：月 [1,12]、日 [1, 月长（含闰年 2 月）]、时 [0,23]、分/秒 [0,59]、毫秒 [0,999]（宽度保证）；违例抛 ValueError。
4. `timeUnixFromCivil` 得整数秒，返回 `msNewFloat(L, secs + millis / 1000.0)`。

### 7. 模块封装层（stdlib/ms_std_time.c）

- 头文件 `stdlib/ms_std_time.h`，include guard `MSLANG_STDLIB_MS_STD_TIME_H_`，自包含（`<mslang/mslang.h>`），与任务 21/36 的 `stdlib/` 根布局同形：

```c
// stdlib/ms_std_time.h

// Registers the "time" builtin module (now/monotonic/sleep/format/parse)
// into the builtin module registry. Called once during interpreter
// startup (msNewState's stdlib wiring, task 18).
MsResult msStdTimeRegister(MsState* L);
```

- 5 个包装函数均为 `static MsObject* timeXxx(MsState* L, int64_t argc, MsObject** argv)`。统一骨架（以 `format` 为例）：

```c
static MsObject* timeFormat(MsState* L, int64_t argc, MsObject** argv) {
  if (argc != 2) {
    msRaiseTypeError(L, "time.format() takes exactly 2 arguments");
    return NULL;
  }
  double ts;
  if (!timeArgAsDouble(L, argv[0], &ts)) {        // static helper; rejects bool/non-numeric
    return NULL;
  }
  if (msTypeOf(argv[1]) != MS_TYPE_STR) {
    msRaiseTypeError(L, "time.format() layout must be str");
    return NULL;
  }
  const char* layout = msAsCString(L, argv[1]);   // argv are roots; pointer window below
  size_t layoutLen = msStringLen(argv[1]);
  // floor + civil conversion + MsStrBuf render run before any allocating call
  // ...
  return msNewStringN(L, outData, outLen);
}
```

- 参数提取辅助 `timeArgAsDouble`（文件内 `static`，接受 int/float、拒绝 bool 与其他类型，失败已置 TypeError）与任务 21 的 `mathArgAsDouble` 同约定；`msAsCString` 指针窗口纪律与任务 20/35/36/38 相同：先取指针、纯 C 计算跑完、再装箱。
- `sleep` 换算：`seconds`（double）经显式检查后换算 `ns = (uint64_t)(seconds * 1e9 + 0.5)`（四舍五入到纳秒）；int 参数走 int64 乘法并检查 `> UINT64_MAX / 10⁹` 的溢出，溢出抛 ValueError。`ns == 0` 直接返回 nil 不调原语。
- **协程感知差距**：07-stdlib §25 要求 `time.sleep` 挂起协程而非 OS 线程；协程属 v0.3（任务 43/46），本任务交付**阻塞当前线程**的实现（`msClockSleepNs`），脚本可见语义（返回值、异常、至少睡满）与终态一致，仅阻塞粒度不同。任务 46 的调度器落地后把睡眠改为协程定时器挂起，接口与测试不变。此差距在此显式承认（与任务 36 对 `os.exec` 的处理同例）。
- 方法表与注册：

```c
static const MsMethodDef timeMethods[] = {
  {"format",    timeFormat,    "format(ts, layout) -> str"},
  {"monotonic", timeMonotonic, "monotonic() -> float"},
  {"now",       timeNow,       "now() -> float"},
  {"parse",     timeParse,     "parse(layout, text) -> float"},
  {"sleep",     timeSleep,     "sleep(seconds)"},
  {NULL, NULL, NULL},
};

static const MsModuleDef timeModuleDef = {
  "time", "time, clocks and sleeping", timeMethods,
};
```

  `msStdTimeRegister` 调 `msRegisterModule(L, &timeModuleDef)`（任务 33 落地实现），挂入任务 18 建立的标准库注册链。模块无常量与数据属性，注册后无需再写属性。脚本统一 `import "time"`（任务 24），不做全局名预绑定（与任务 36 的现行约定一致）。

### 8. 内存与 GC 纪律清单

- 平台原语层零堆分配、零 `MsState` 依赖（与任务 42 的层约定一致）。
- 模块层唯一 C 侧缓冲是 `format` 的 `struct MsStrBuf`（`msRealloc` 增长，`msStrBufFree` 在所有退出路径释放）；结果对象经 `msNewFloat`/`msNewNil`/`msNewStringN` 由 `MsState` 分配。
- 参数自动是根（09-c-api §3）；本模块无跨分配存活的中间 `MsObject*`（format 的装箱发生在全部 C 侧计算之后），无需 `msRootPush`。
- 无可变全局新增：time 模块不持有模块级 C 状态；Win32 后端的 QPC 频率缓存是原语层内的只写一次 static（第 2 节已声明其线程安全边界）。

## 实现步骤

1. 建 `src/platform/ms_clock.h` 全量声明骨架（guard、三个原语签名与英文文档注释）与两份后端空壳；CMake 按 `WIN32` 择一编译并入 `mslang` 库。验证：三平台编译链接通过。
2. 实现 POSIX 后端：`clock_gettime` 双钟 + `nanosleep` 的 `EINTR` 重试。验证：临时冒烟（或经后续脚本断言）——单调钟连续调用非递减、`msClockSleepNs(20ms)` 后流逝 ≥ 15ms、墙钟与 `time(NULL)` 差 < 5s。
3. 实现 Win32 后端：QPC 分治换算 + 频率缓存、FILETIME 纪元偏移换算、`Sleep` 向上取整。验证：同第 2 步的断言在 Windows 下通过。
4. 建 `stdlib/ms_std_time.{c,h}` 骨架与 `timeArgAsDouble` 辅助，实现 `now`/`monotonic`/`sleep` 三个包装（纳秒↔float 换算、sleep 的域检查与溢出检查）。验证：脚本断言三者返回类型与 sleep 的 nil 返回。
5. 实现民用历法换算三函数（Hinnant 算法、floor 除法分拆、闰年判定）。验证：C 侧或脚本侧抽查已知纪元（`0` → 1970-01-01 00:00:00、`-1` → 1969-12-31 23:59:59、`951782400` → 2000-02-29 00:00:00）。
6. 实现 layout 扫描与 `format`（定宽零填充输出、`.000` 截断、范围检查）。验证：第 4 节表格的全部记号与字面量混排。
7. 实现 `parse`（双指针定宽消费、缺省基线、字段校验、严格全长匹配）。验证：format/parse 往返一致、全部负例抛 ValueError。
8. 方法表、`timeModuleDef`、`msStdTimeRegister` 挂入 `msNewState` 注册链。验证：`import "time"` 冒烟脚本通过，内建注册表优先于搜索路径。
9. 编写 `tests/ms/stdlib/time/` 全部测试脚本（见测试方案），`python run_tests.py` 全绿。
10. Win/Linux/macOS × Debug/Release 构建验证；Debug（ASAN / `/RTC`）下跑全部 time 测试无内存错误与泄漏；grep 断言 `src/platform/` 之外无 `#ifdef _WIN32`、`<windows.h>` 只出现在平台层。

## 测试方案

本任务只交付本设计文档；测试脚本随实现编写。一律使用 ms 脚本测试（`tests/ms/stdlib/time/`，内建 `assert` + `print`——任务 40 之前不用 testing 模块），成功脚本末尾 `print("<用例名> ok")`，由仓库根 `run_tests.py` 驱动。异常系统（任务 23）已落地，负向用例用 `try/except TypeError`/`except ValueError` 在正向脚本内断言（捕获后校验，未捕获即 `assert(false)`），与任务 36 的约定一致。

涉时断言统一用容忍区间（下界略小于名义值、上界放宽一个数量级），避免 CI 抖动误判；float 比较用 epsilon 容差（脚本内自定义辅助函数）。

测试文件清单与覆盖点：

- `now_monotonic.ms`：`time.now()` 返回 float 且大于已知下界常量（如 `1.7e9`，2023 年之后）；两次 `now()` 差值在 [0, 60s) 内；`time.monotonic()` 返回 float、循环 1000 次取值非递减；两次 `monotonic()` 差值非负且微小；`now`/`monotonic` 带参调用抛 TypeError。
- `sleep.ms`：`sleep(0)` 与 `sleep(0.0)` 立即返回 nil；`sleep(0.05)` 前后取 `monotonic()` 差值 ∈ [0.04, 5.0]（下界略低于名义值容忍提前唤醒误差，上界放宽）；`sleep(1)`（int 参数）差值 ∈ [0.9, 10.0]；负向：`sleep(-1)`、`sleep(-0.5)`、`sleep(math.nan 等价物——用 `0.0/0.0` 不可行时以 `parse` 之外的合法途径构造 NaN，实现时选用语言已有的 NaN 来源，如 math 模块的 `math.nan`)`、超大 `seconds`（如 `1e30`）抛 ValueError，`sleep("1")`、`sleep(true)` 抛 TypeError。
- `format.ms`：`format(0, "2006-01-02 15:04:05") == "1970-01-01 00:00:00"`；已知时间戳 `format(1700000000, "2006-01-02 15:04:05") == "2023-11-14 22:13:20"`；零填充（`format(978307200, ...)` 的月份 `01`）；字面量混排（`format(0, "year 2006!") == "year 1970!"`）；`.000` 截断（`format(0.1234, "15:04:05.000") == "00:00:00.123"`、`format(0.9999, ...)` 得 `.999` 不进位）；负时间戳（`format(-1, "2006-01-02 15:04:05") == "1969-12-31 23:59:59"`）；重复记号多次展开（`"2006/2006"` → `"1970/1970"`）；负向：年份越界的 `ts`（如 `253402300800`，10000-01-01 起）抛 ValueError、`ts` 为 bool/str 抛 TypeError。
- `parse.ms`：`parse("2006-01-02 15:04:05", "1970-01-01 00:00:00") == 0.0`；与 format 往返一致（`parse(layout, format(ts, layout))` 在毫秒精度内等于 `ts`，覆盖含 `.000` 与不含的 layout）；缺省基线（`parse("15:04", "12:30")` 为 1970-01-01 12:30:00）；毫秒解析（`.000` → `0.123`）；闰年：`parse("2006-01-02", "2000-02-29")` 合法、`parse("2006-01-02", "1900-02-29")` 抛 ValueError（1900 非闰年）、`parse("2006-01-02", "2001-02-29")` 抛 ValueError；字段越界（月 `13`、日 `32`、时 `24`、分 `60`）逐一抛 ValueError；严格匹配（text 长度不符、宽度不足 `"1970-1-1"`、字面量失配、尾随多余字符）抛 ValueError；重复记号后者覆盖前者（`"2006 2006"` 解析 `"1970 2001"` 得 2001 年）；类型错误（非 str 参数）抛 TypeError。
- `errors_type.ms`：参数个数错误与类型错误的批量断言（`time.now(1)`、`time.sleep()`、`time.format(0)`、`time.parse("2006")`、`time.format("0", "2006")` 等），全部经 `try/except TypeError` 在脚本内断言。

覆盖点对照：5 个函数每个至少一条正向断言；第 3 节总表每条「失败」列至少一条负向断言；平台原语的单调性/睡眠下界/墙钟 sanity 经脚本侧断言间接覆盖（脚本层无平台差异分支，全部用例三平台同跑）。

## 验收标准

- [ ] `stdlib/ms_std_time.{c,h}` 与 `src/platform/ms_clock.{h}`、`ms_clock_posix.c`、`ms_clock_win.c` 存在；guard 分别为 `MSLANG_STDLIB_MS_STD_TIME_H_` 与 `MSLANG_SRC_PLATFORM_MS_CLOCK_H_`；头文件自包含且 `ms_clock.h` 不含任何平台条件编译；代码通过 [10-c-style.md](../language/10-c-style.md) 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef）。
- [ ] 07-stdlib §9 的 5 个 `time` 函数全部实现并经 `import "time"` 可用；语义与「详细设计」第 3 节总表逐条一致（float 秒口径、sleep 至少睡满、format/parse UTC 口径、数值参数 int/float 双收且拒绝 bool）。
- [ ] 平台差异全部下沉 `src/platform/ms_clock_*`：`stdlib/` 与其余 `src/` 代码无 `#ifdef _WIN32`（grep 可验证）；`clock_gettime`/QPC/FILETIME/`Sleep` 只出现在平台层；`errno`（`EINTR`）不外泄；原语命名 `msClockMonotonicNs`/`msClockUnixEpochNs` 与任务 42 文档预定名逐字一致。
- [ ] layout 记号集与「详细设计」第 4 节一致（7 个记号、最长匹配、`.000` 截断、parse 缺省基线 1970-01-01、严格全长匹配、重复记号后者覆盖）；民用历法换算对负时间戳正确（floor 除法），年份域 [1, 9999]，闰年规则含 1900/2000 判别。
- [ ] `sleep` 的负数/NaN/inf/溢出抛 ValueError，`format` 的越界时间戳抛 ValueError，`parse` 的全部失配与字段越界抛 ValueError，类型错误抛 TypeError；失败路径返回 `NULL` 且错误状态已置。
- [ ] 模块层遵守 GC 根纪律（`msAsCString` 指针窗口内不调用其他 API）与失败路径释放 `MsStrBuf`；堆分配全部经 `msAlloc`/`msRealloc`/`msFree`；平台原语层零堆分配；time 模块不持有模块级可变 C 状态。
- [ ] `time.sleep` 的「阻塞线程而非协程感知」差距已在文档显式承认；Win32 QPC 频率缓存的线程安全边界已声明并标注任务 42 的归并点。
- [ ] `tests/ms/stdlib/time/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿；`ctest --test-dir build` 并入通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）下 time 测试无内存错误与泄漏报告。
- [ ] 无 TBD/TODO 占位；对任务 02（`msAlloc` 系列、`MS_ASSERT`）、任务 20（`struct MsStrBuf` 系列）与任务 42（时钟原语命名与归并计划）的接口假定在实现时已对齐定名。
