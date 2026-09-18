# 61 标准库：regexp

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [18 C API 基础与嵌入示例](18-c-api-foundation.md) |

## 任务目标

交付 C 内建标准库模块 `regexp`（`stdlib/regexp/ms_regexp.h` / `stdlib/regexp/ms_regexp.c` / `stdlib/regexp/ms_regexp_module.c`），实现 `docs/language/07-stdlib.md` §12 规定的脚本接口：编译式对象 API（`regexp.compile(pattern)` 得到 Regexp，具 `match` / `search` / `findAll` / `replaceAll` / `split` 五个方法，Match 对象具 `group` / `start` / `end` / `groups` 四个方法）与免编译便捷形式 `regexp.match(pattern, text)`。

正则引擎为仓库内自研：递归下降解析器把正则语法子集编译为 Thompson NFA 指令程序，执行采用 Pike VM 式多线程模拟（含优先级排序与捕获槽），对任意合法模式与输入保证 O(文本长度 × 程序长度) 时间，**不存在回溯爆炸**（指数级最坏情形在构造上被排除）。免编译形式经挂在 `MsState` 上的编译缓存复用已编译程序。模块同时交付跨步骤复用的原则：引擎层（解析 + 编译 + 模拟）为不依赖 `MsState` 的纯 C 单元，绑定层只做参数校验与对象装箱。本任务通过 `tests/ms/regexp_test.ms` 等脚本测试（testing 模块）独立验证。

## 设计依据

- `docs/language/07-stdlib.md` §12（regexp 节：语法子集为「字符类、量词、分组、捕获、锚点、交替」；Thompson NFA 保证线性时间；API 清单——本任务范围唯一来源）、§0（`regexp` 为 C 内建模块，置于 `stdlib/` 目录；函数与方法命名小驼峰）。
- `docs/language/02-types.md` §4（str 为不可变 UTF-8 序列，索引按码点口径）：本任务的文本匹配按 **Unicode 码点**逐字符进行，`Match.start()` / `Match.end()` 返回**码点索引**（与 `s[i]`、`strings.indexOf` 口径一致）；正则匹配的字符类（`\w` 等）同样按码点判定。
- `docs/language/09-c-api.md` §3（GC 根栈纪律）、§5（`msNewStringN` / `msAsCString` / `msStringLen` / `msNewList` 等值构造）、§6（`msListAppend`）、§8（C 函数出错置错误并返回 `NULL`）、§9（`MsModuleDef` / `MsMethodDef` / `MsCFunction` 与模块注册）、§10（`MsTypeDef` / `msDefineType` / `msCInstanceData`，Regexp 与 Match 两个 C 自定义类型的实现基础；其落地在任务 33，编号先于本任务，属顺序前提）。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、禁止非 const 可变全局变量——编译缓存因此挂在 `MsState` 上、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- `docs/language/11-project-layout.md` §1（`stdlib/` 为 C 标准库模块目录）、§4（脚本测试经仓库根 `run_tests.py` 驱动 mslang CLI 执行）。
- `docs/tasks/README.md` 测试约定：本任务晚于任务 40，测试一律用 ms 脚本 + testing 模块（`import "testing"` / `import "testing/assert"`，测试函数以 `test` 开头，末尾 `testing.run()`）。
- 任务 18（C API 基础）假定提供：`msRegisterModule`、错误接口 `msRaiseTypeError` / `msRaiseValueError`、根栈 `msRootPush` / `msRootPop`，以及把可变模块状态挂到 `MsState` 的机制（编译缓存的宿主，本文记为「标准库扩展槽」）。任务 20（strings）的共享算法层 `src/object/ms_str_op.h` 假定提供 `struct MsStrBuf` / `msStrBufInit` / `msStrBufPut` / `msStrBufFree`（`replaceAll` 的输出缓冲复用）；UTF-8 码点原语 `msUtf8DecodeRune` / `msUtf8ByteToRuneIndex`（任务 06/20 已假定定名）。**以上接口名均为假定命名，实现时以对应任务文档定名为准。**
- 算法参考：Thompson 构造与 Pike VM 模拟按 Russ Cox《Regular Expression Matching Can Be Simple And Fast》及其 pikevm 变体（SAVE 指令支持捕获组）；实现注释中注明出处。

## 详细设计

### 1. 文件布局与分层

```
stdlib/regexp/
├── ms_regexp.h          # 引擎层公开头 + 模块注册入口声明
├── ms_regexp.c          # 解析器、Thompson 编译器、Pike VM 模拟器（纯 C，不 include mslang 头文件以外的解释器内部头）
└── ms_regexp_module.c   # 脚本绑定层：Regexp/Match 自定义类型、模块函数表、编译缓存
```

分层职责：

- **引擎层**（`ms_regexp.c`）：输入 `const char* + size_t` 字节切片，不触碰 `MsObject` 与脚本概念；唯一外部依赖是任务 02 的分配接口（`msAlloc`/`msRealloc`/`msFree`）与码点解码原语。编译产物 `struct MsReProg` 与解释器完全解耦，可在无 `MsState` 环境下移植复用。
- **绑定层**（`ms_regexp_module.c`）：`MsCFunction` 包装（参数校验、装箱）、`Regexp` 与 `Match` 两个 C 自定义类型、编译缓存。全部函数 `static`，唯一导出符号是注册入口。

### 2. 正则语法子集（唯一权威定义）

07-stdlib §12 只列出特性类别，本节固定其确切语法与语义。模式按码点解析；不支持内联标志，匹配恒区分大小写。

**支持**：

| 类别 | 语法 | 语义 |
|---|---|---|
| 字面量 | 普通字符 | 自匹配（按码点比较） |
| 转义 | `\n \t \r \0 \xHH \uHHHH`，及 `\\ \. \* \+ \? \( \) \[ \] \{ \} \| \^ \$` 等元字符转义 | `\xHH` 为单字节值（0x00–0xFF 码点）、`\uHHHH` 为 BMP 码点；代理区与超 U+FFFF 码点不支持（报编译错误） |
| 字符类预定义 | `\d \D \w \W \s \S` | `\d`=[0-9]；`\w`=[0-9A-Za-z_]；`\s`=Unicode White_Space 集（复用任务 20 的 `msUnicodeIsSpace`）；大写形式为补集。**ASCII 口径**，不做 Unicode 字母类别扩展（有意从简，测试锁定） |
| 字符类 | `[...]`、`[^...]`、范围 `a-z`、内部允许 `\d \w \s` 等转义与 `\xHH \uHHHH` | `]` 紧跟 `[` 或 `[^` 时为字面量；`-` 位于首尾时为字面量；范围端点按码点比较，起点 > 终点报编译错误 |
| 任意字符 | `.` | 匹配除 `\n` 外的任意码点 |
| 量词 | `*`、`+`、`?`、`{n}`、`{n,m}`、`{n,}`，均可后随 `?` 构成非贪婪形式 | 贪婪/非贪婪通过 SPLIT 优先级体现（见第 4 节）；`{n,m}` 要求 `0 <= n <= m <= 1000`（上限约束程序体积，超限报编译错误） |
| 分组 | `(...)` 捕获组、`(?:...)` 非捕获组 | 捕获组上限 `MS_REGEXP_MAX_CAPTURES - 1`（15 个），组号按左括号出现顺序从 1 编号 |
| 交替 | `a|b|c` | 优先级从左到右（左侧分支优先） |
| 锚点 | `^`、`$` | 匹配整个文本的绝对起点/终点；**无多行模式**（无 `(?m)`），`^`/`$` 不匹配 `\n` 前后位置 |

**明确不支持**（出现即编译错误，错误消息含码点位置与原因）：反向引用 `\1`、前向/后向断言 `(?=...)`、`(?!...)`、`(?<=...)`、`(?<!...)`、命名捕获 `(?P<...)`、内联标志 `(?i)` 等、占有量词 `*+`、原子组 `(?>`、`\b` 等边界断言、`{n,m}` 之外的畸形重复说明。这些构造或要求回溯语义、或超出本子集定位，排除它们是「无回溯灾难」承诺的一部分。

**匹配语义**：左优先（leftmost-first，与 Perl/Go 一致）：`search` 从最小起始位置找匹配，同一起始位置下按分支优先级取最先到达 MATCH 的线程的捕获结果；非贪婪量词只改变分支优先级，不改变「左优先」起点规则。空模式合法，匹配起点处的空串。`^` 出现在非组首、`$` 出现在非组尾时按字面量处理过于反直觉，故规定：`^`/`$` 在任何位置都是零宽断言（与 Go 一致），`a^b` 永不匹配。

### 3. 引擎层接口（ms_regexp.h）

include guard `MSLANG_STDLIB_REGEXP_MS_REGEXP_H_`，自包含（`<stdbool.h>` `<stddef.h>` `<stdint.h>` + 任务 02 分配头）。内部结构体不 typedef；编译程序对调用者以不完整类型暴露：

```c
#define MS_REGEXP_MAX_CAPTURES 16   // 含第 0 组（整体匹配）；脚本可见捕获组至多 15 个
#define MS_REGEXP_MAX_REPEAT 1000   // {n,m} 的 m 上限

typedef enum {
  MS_RE_OK = 0,
  MS_RE_ERROR_SYNTAX,   // 模式非法（errBuf 含位置与原因）
  MS_RE_ERROR_OOM
} MsReResult;

struct MsReProg;   // 编译产物，定义在 ms_regexp.c 内部

// Compiles [pattern, patternLen) into a Thompson NFA program.
// On success *out is a newly allocated program owned by the caller
// (free with msReProgFree). On MS_RE_ERROR_SYNTAX, errBuf receives a
// NUL-terminated message "pos <runeIndex>: <reason>" (truncated to fit).
MsReResult msReCompile(const char* pattern, size_t patternLen,
    struct MsReProg** out, char* errBuf, size_t errBufLen);

void msReProgFree(struct MsReProg* prog);

// Number of capture groups including group 0; span arrays passed to
// msReSearch must have room for 2 * msReProgGroupCount(prog) entries.
size_t msReProgGroupCount(const struct MsReProg* prog);

// True when the program begins with a ^ assertion (the binding layer uses
// this to reject re.search on start-anchored patterns only via early exit;
// matching itself handles the assertion).
bool msReProgIsAnchored(const struct MsReProg* prog);

// Searches [text, textLen) for a match starting at or after byte offset
// startOff (must be a rune boundary). anchored requires the match to begin
// exactly at startOff. On a match, spans[2*i] / spans[2*i+1] receive the
// half-open byte range of group i, or (SIZE_MAX, SIZE_MAX) for a group that
// did not participate; returns true. scratch is caller-owned working storage
// obtained from msReScratchNew, allowing reuse across calls; may be NULL,
// in which case storage is allocated and freed per call.
bool msReSearch(const struct MsReProg* prog, const char* text, size_t textLen,
    size_t startOff, bool anchored, size_t* spans, struct MsReScratch* scratch);

struct MsReScratch* msReScratchNew(const struct MsReProg* prog);
void msReScratchFree(struct MsReScratch* scratch);
```

要点：

- 全部索引内部按**字节偏移**运算；码点索引的换算（`Match.start()` 的返回值）只在绑定层命中后做一次（`msUtf8ByteToRuneIndex`），模拟器内部不逐码点维护索引表。
- `struct MsReScratch`（线程列表等工作存储，见第 5 节）与 `MsReProg` 解耦分配：一个编译程序可被并发地用于多个搜索（各自持 scratch），符合「禁止可变全局变量」并天然可重入。
- 未参与匹配的可选组（如 `(a)?` 未命中）以 `(SIZE_MAX, SIZE_MAX)` 哨兵表示，绑定层据此返回 nil。

### 4. 解析与 Thompson 编译

**解析器**：单遍递归下降（模式长度天然有限，递归深度受 `{n,m}` 展开与括号嵌套限制，嵌套深度上限 64，超限报编译错误），文法：

```
alt    := concat ('|' concat)*
concat := repeat*
repeat := atom ('*' | '+' | '?' | '{n,m}') '?'?
atom   := '(' alt ')' | '(?:' alt ')' | '[' class ']' | '.' | '^' | '$' | escape | literal
```

解析直接产出抽象语法树节点 `struct MsReNode`（kind 枚举 + 子指针 + 量词边界 + 捕获组号），节点池经 `msAlloc` 一次性分配、编译结束统一 `msFree`（编译期临时结构，不暴露）。

**指令集**（`struct MsReInst`，定长指令便于布线与补丁回填）：

```c
typedef enum {
  MS_RE_OP_RUNE,    // match one specific rune; operand: rune, next
  MS_RE_OP_CLASS,   // match one rune in class; operand: class index, next
  MS_RE_OP_ANY,     // match one rune except '\n'
  MS_RE_OP_SPLIT,   // try x first (priority), then y
  MS_RE_OP_JMP,     // unconditional jump
  MS_RE_OP_SAVE,    // record current position into capture slot n
  MS_RE_OP_BOL,     // assert position == 0
  MS_RE_OP_EOL,     // assert position == textLen
  MS_RE_OP_MATCH    // accept
} MsReOpcode;
```

- 编译按 Russ Cox 的片段（fragment）+ 悬空指针补丁（patch list）技术单遍完成；`{n,m}` 以子图复制展开（`{2,4}` → 两份必选 + 两份各包一层 `?`），由 `MS_REGEXP_MAX_REPEAT` 限制展开上限；字符类编译为排序后的码点区间表（`struct MsReClass`：`{uint32_t lo, hi}` 数组 + 取反标志），运行时二分判定。
- 贪婪量词的 SPLIT 以「继续循环」为优先分支，非贪婪以「跳出循环」为优先分支——优先级即线程顺序，无需任何回溯。
- 程序头部固定 `SAVE 0`、尾部 `SAVE 1` + `MATCH`，第 0 组即整体匹配。

### 5. Pike VM 模拟执行

执行器为输入的每个码点位置维护「当前线程表」与「下一位置线程表」两个集合（`struct MsReScratch` 内两块定长数组，容量 = 程序指令数，每线程携带 `2 * groupCount` 个捕获槽的副本）：

1. `startOff` 处向当前表加入初始线程（`pc = 0`，捕获槽全置 `SIZE_MAX`）；非 anchored 且当前表在每一步未含 MATCH 时，于该位置再注入一个新初始线程（追加在表尾，保证更左的起点优先级更高）。
2. **加线程**（闭包）：沿 `JMP`/`SPLIT`/`SAVE`/`BOL`/`EOL` 递归展开到消费型指令（`RUNE`/`CLASS`/`ANY`/`MATCH`），以 `(pc, 捕获槽)` 入表；同一 `pc` 去重（先到者保留，后到者丢弃——去重是线性时间的关键）。SPLIT 先递归 x 分支再 y 分支，天然实现优先级。`SAVE` 在展开的捕获槽副本上记录当前字节偏移。递归展开转迭代 + 显式栈，深度不受调用栈限制；visited 标记用程序级「代数戳」（每位置递增的 generation 计数，无需清零数组）。
3. 对当前表按序逐线程：消费型指令匹配当前码点则把后继加入下一表（携带该线程的捕获槽）；遇 `MATCH` 即返回该线程的捕获槽（左优先语义下第一个到达者即答案），并终止本轮搜索。
4. 码点推进（`msUtf8DecodeRune`），交换两表，重复至文本结束；表变空（无存活线程）提前失败。

复杂度：每位置至多「程序长度」个线程，每线程推进代价 O(捕获组数)（槽副本），总代价 O(文本码点数 × 程序长度 × 组数)——对固定模式为文本长度的线性函数，任何输入都不会指数爆炸。

### 6. 编译缓存

`regexp.match(pattern, text)` 等免编译形式不应每次重编译。缓存挂在 `MsState` 的标准库扩展槽（假定机制，见「设计依据」），模块注册时创建：

```c
#define MS_REGEXP_CACHE_CAPACITY 32

struct MsReCacheEntry {
  MsObject* pattern;    // str key, rooted by the cache itself
  MsObject* regexpObj;  // compiled Regexp object (owns the MsReProg)
  uint64_t lastUsed;    // logical clock for LRU eviction
};

struct MsReCache {
  struct MsReCacheEntry entries[MS_REGEXP_CACHE_CAPACITY];
  uint64_t clock;
};
```

- 查找按模式串内容比较；命中更新 `lastUsed`，未命中编译新程序、包装为 Regexp 对象后按 LRU 替换。缓存持有的对象经 GC 根纪律注册为内部根（不占用户根栈），随 `msCloseState` 一并释放。
- 容量固定 32、不扩容：缓存只是便捷形式的加速，非正确性机制；`regexp.compile` 的显式对象不经缓存，生命周期完全归脚本（GC）管理。
- 语法错误的模式**不缓存**（错误消息直接抛出，下次调用重新编译重新报错，行为可预期）。

### 7. 绑定层：Regexp 与 Match 类型

**Regexp**（`MsTypeDef`，`name = "Regexp"`）：实例数据区内嵌 `{ struct MsReProg* prog; MsObject* pattern; size_t groupCount; }`；`finalize` 调 `msReProgFree`。五个方法（`MsCFunction`，`argv[0]` 为 self）：

| 方法 | 语义 |
|---|---|
| `match(text)` | `msReSearch(..., startOff=0, anchored=true)`；无匹配返回 nil，有匹配返回 Match 对象 |
| `search(text)` | 同上 `anchored=false`；等价于 `^` 前缀模式的 `match` 行为由断言自然处理，无需特判 |
| `findAll(text)` | 从位置 0 起循环 `search`：每次命中记录 Match，下一次从 `max(end, start + 1 码点)` 继续（**空匹配强制推进一个码点**，防止死循环；对齐 Go `FindAll` 语义）；返回 `list[Match]`，无匹配返回 `[]` |
| `replaceAll(text, repl)` | 按 `findAll` 的同一迭代把命中区间替换为 `repl` 的展开结果（见下），经 `struct MsStrBuf` 累积后装箱为新 str；无匹配返回原串的副本（str 不可变，可直接返回原对象） |
| `split(text)` | 按命中区间切分，返回命中之间（含首尾）的 `list[str]`；空匹配按上述推进规则参与切分（每码点切开）；无匹配返回 `[text]` |

`repl` 展开规则：`$$` → 字面 `$`；`$0`…`$9` → 对应组文本（未参与组展开为空串）；`$` 后非数字非 `$` 时报 ValueError（宁可严格，避免静默吞错）。不支持命名组引用（语法子集无命名组）。

**Match**（`MsTypeDef`，`name = "RegexpMatch"`）：实例数据区持有 `{ MsObject* subject; size_t* spans; size_t groupCount; }`——`spans` 为命中时捕获槽的 `msAlloc` 副本，`subject` 持有原串对象使切片永远有效（实例与方法表中对象经根纪律保护，finalize 只 `msFree(spans)`）。方法：

| 方法 | 语义 |
|---|---|
| `group(i)` | `0 <= i < groupCount`，否则 IndexError；组未参与返回 nil；否则返回 `subject[spans[2i], spans[2i+1])` 切片装箱的新 str |
| `groups()` | 组 1..n 的 `list`（元素语义同 `group(i)`，未参与为 nil）；不含第 0 组 |
| `start(i=0)` / `end(i=0)` | 组 `i` 的起/止**码点索引**（字节偏移经 `msUtf8ByteToRuneIndex` 换算）；组未参与返回 `-1` |

模块函数表（`regexpModuleMethods`）仅一项便捷形式：

```c
static const MsMethodDef regexpModuleMethods[] = {
  {"compile", regexpCompile, "compile(pattern) -> Regexp"},
  {"match",   regexpMatch,   "match(pattern, text) -> Match or nil"},
  {NULL, NULL, NULL},
};
```

- `regexp.compile(pattern)`：`pattern` 必须是 str；编译失败经 `msRaiseValueError`（消息含错误位置）返回 `NULL`；成功返回 Regexp 对象。
- `regexp.match(pattern, text)`：走编译缓存（第 6 节），等价于缓存命中对象的 `match(text)`；模式非法同样 ValueError。
- 注册入口 `MsResult msRegexpModuleRegister(MsState* L)`：`msDefineType` 注册两个类型（挂入模块命名空间或 VM 类型注册表，以任务 33 定稿为准）、创建缓存并挂到 `MsState` 扩展槽、`msRegisterModule("regexp", ...)`；重复注册幂等。
- 通用约定：一切 `text`/`pattern`/`repl` 参数必须是 str，类型或参数个数不符报 TypeError；匹配输入恒为合法 UTF-8（str 构造时已校验），引擎内部用 `MS_ASSERT` 防御。

### 8. 内存与 GC 纪律清单

- 引擎层分配四处：AST 节点池（编译期，统一释放）、`struct MsReProg`（指令数组 + 字符类表，`msReProgFree` 释放）、`struct MsReScratch`（调用者成对释放）、无其他。失败路径按获取逆序释放。
- 绑定层：`findAll`/`split` 的 list 在循环装箱前 `msRootPush`、结束 `msRootPop`；Match 构造先建对象再复制 spans（对象已入根或由调用约定保护）；`msAsCString` 指针窗口内不调用其他会触发分配的 C API（先取指针与长度、引擎跑完、再装箱）。
- 缓存持有的 `pattern`/`regexpObj` 注册为内部 GC 根；淘汰时先除根再替换。

## 实现步骤

1. 建 `stdlib/regexp/ms_regexp.h` 骨架：常量宏、`MsReResult`、不透明 `struct MsReProg` / `struct MsReScratch`、五个引擎函数声明与 `msRegexpModuleRegister` 声明，guard `MSLANG_STDLIB_REGEXP_MS_REGEXP_H_`。验证：头文件自包含编译通过。
2. 实现解析器（`ms_regexp.c` 前半）：文法递归下降、AST 节点池、转义解码（`\xHH`/`\uHHHH`）、字符类解析（范围、取反、内嵌 `\d\w\s`）、全部不支持构造的报错路径（消息含码点位置）。验证：随第 3 步一并经脚本测试核验；本步以编译错误消息的内容断言为主。
3. 实现 Thompson 编译器：AST → 指令程序的片段/补丁布线、`{n,m}` 展开、SPLIT 优先级（贪婪/非贪婪）、字符类区间表排序合并、`SAVE 0`/`SAVE 1` 包裹。验证：编译产物指令数与结构符合预期（可用调试期临时 dump 函数人工核对，不留在交付代码中）。
4. 实现 Pike VM 模拟器：加线程闭包（显式栈 + 代数戳去重）、消费/推进主循环、左优先 MATCH 判定、anchored 模式、捕获槽记录与 `(SIZE_MAX, SIZE_MAX)` 哨兵。验证：`msReSearch` 对构造用例返回正确的 spans（经第 5 步绑定后用脚本断言）。
5. 建 `stdlib/regexp/ms_regexp_module.c`：Regexp 类型（`msDefineType`、finalize）、`match`/`search`/`findAll` 三个只读方法、`regexp.compile` 模块函数。验证：`regexp.match("\\d+", "abc123")` 冒烟脚本命中 `123`。
6. 实现 `replaceAll`（`$` 展开 + `MsStrBuf` 累积）与 `split`（空匹配推进规则）、Match 类型的四个方法。验证：`findAll` 空匹配、replaceAll 组引用、split 边界（首/尾命中产生空片）的脚本断言。
7. 实现编译缓存：`MsReCache`、LRU 替换、内部根注册、`regexp.match` 接入；`msRegexpModuleRegister` 完成类型注册与 `"regexp"` 模块登记，CMake 加入新源文件。验证：同一模式重复 `regexp.match` 结果正确（缓存命中与未命中路径行为一致）；GC 压力脚本（循环触发分配）下缓存对象不失效。
8. 编写 `tests/ms/regexp_test.ms` 与 `tests/ms/regexp_syntax_test.ms`（见测试方案），接入 `run_tests.py` 发现机制。验证：`python run_tests.py` 全绿。
9. Win/Linux/macOS × Debug/Release 构建验证；Debug（ASAN / `/RTC`）下全部 regexp 测试无内存错误，`msCloseState` 后分配计数归零（含缓存条目）。

## 测试方案

本任务晚于任务 40，测试一律用 ms 脚本 + testing 模块。测试文件 `tests/ms/regexp_test.ms`（API 与匹配语义）与 `tests/ms/regexp_syntax_test.ms`（编译错误），均 `import "testing"`、`import "testing/assert"`、`import "regexp"`，测试函数以 `test` 开头，末尾 `testing.run()`；由仓库根 `run_tests.py` 驱动。本任务只交付本设计文档，测试代码随实现编写。

`tests/ms/regexp_test.ms` 覆盖清单：

- **基本匹配**：字面量、`.`（含不匹配 `\n`）、`^`/`$` 锚点（含「`a^b` 永不匹配」）、`match` 的从头锚定与 `search` 的任意起点、`regexp.match` 便捷形式与 `compile().match` 结果一致。
- **量词**：`*`/`+`/`?`/`{n}`/`{n,}`/`{n,m}` 的命中长度；贪婪 vs 非贪婪（`a.*b` vs `a.*?b` 对 `"aXbYb"` 的 group(0)）；`{n,m}` 上限边界 `{0,1000}` 可编译。
- **字符类**：`[abc]`、范围 `[a-z]`、取反 `[^0-9]`、内部转义 `[.\]]`、`]` 首位字面、`-` 首尾字面、`\d\w\s` 及其补集（含多字节文本中 `\w` 不匹配中文的 ASCII 口径断言）。
- **分组与捕获**：`(\d+)-(\d+)` 的 group(1)/group(2)、嵌套组编号顺序、`(?:...)` 不占组号、可选组未参与时 `group(i)` 为 nil 且 `start(i)`/`end(i)` 为 `-1`、`groups()` 列表形态、交替优先级 `a|ab` 对 `"ab"` 命中 `"a"`。
- **索引口径**：多字节文本（如 `"héllo 123"`）中 `m.start()`/`m.end()` 返回码点索引并与 `strings.indexOf` 结果一致；`group(i)` 切片内容正确。
- **findAll**：多命中序列、`(\d+)` 各 Match 的组 1、空匹配模式（如 `a*` 对 `"aab"`）的推进不死循环且结果符合「空匹配推进一码点」规则、无匹配返回 `[]`。
- **replaceAll**：纯字面替换、`$0`/`$1` 组引用、`$$` 字面美元、未参与组展开为空、无匹配返回等值原串、`$` 后接字母经 `assert.raises(ValueError, ...)` 校验。
- **split**：常规切分、首/尾命中产生空片、无匹配返回 `[text]`、空匹配模式逐码点切分。
- **类型错误**：`compile`/`match`/`search`/`findAll`/`replaceAll`/`split` 的首参或 text 参数非 str、`group(-1)` / `group(99)` 越界（IndexError）、参数个数错误——均经 `assert.raises` 校验。
- **线性时间（冒烟级）**：`(a?){30}a{30}` 类灾难模式对 30 个 `a` 的输入——该模式按子集语法编译（`{n}` 展开后等价构造）并在脚本层断言「能在合理时间返回正确结果」（以测试套件整体不超时不做硬性计时断言）；对照：真正的指数构造如反向引用已在语法层拒绝。

`tests/ms/regexp_syntax_test.ms` 覆盖清单（全部经 `assert.raises(ValueError, lambda: regexp.compile(p))`，并抽查错误消息含位置信息）：

- 未闭合的 `(`、`[`；`{n,m}` 的 `n > m`、`m > 1000`、非数字内容；孤立 `*`/`+`（无前置原子）；范围端点倒置 `[z-a]`；`\x`/`\u` 位数不足或非法；`\u` 代理区码点。
- 不支持构造逐一：`\1`、`(?=x)`、`(?!x)`、`(?<=x)`、`(?P<n>x)`、`(?i)x`、`a*+`、`(?>x)`、`\b`——各自报「不支持」类编译错误。

## 验收标准

- [ ] `stdlib/regexp/ms_regexp.h` / `ms_regexp.c` / `ms_regexp_module.c` 存在，头文件 guard 为 `MSLANG_STDLIB_REGEXP_MS_REGEXP_H_` 且自包含；代码风格符合 10-c-style（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、无可变全局变量、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] 引擎为正则语法的自研实现，无外部正则库依赖；语法子集与「详细设计」第 2 节逐条一致，不支持构造（反向引用、断言、命名组、内联标志、占有量词、`\b` 等）全部报带位置的编译错误。
- [ ] 匹配执行基于 Thompson NFA 的多线程模拟（Pike VM），SPLIT 优先级实现贪婪/非贪婪与左优先语义；对任意合法模式与输入不存在指数级路径（测试方案的灾难模式冒烟用例通过）。
- [ ] 脚本接口完整：`regexp.compile` / `regexp.match` 与 Regexp 的 `match` / `search` / `findAll` / `replaceAll` / `split`、Match 的 `group` / `start` / `end` / `groups`；`start()`/`end()` 返回码点索引，未参与组返回 nil / `-1`。
- [ ] `findAll`/`split` 的空匹配按「推进一个码点」规则处理，无死循环；`replaceAll` 的 `$` 展开规则（`$$`、`$0`–`$9`、非法 `$` 报 ValueError）与本文一致。
- [ ] 编译缓存容量 32、LRU、挂在 `MsState` 上（非全局变量），命中与未命中行为一致，语法错误不缓存，缓存对象经内部根注册、随状态关闭无泄漏。
- [ ] 绑定层遵守 GC 根纪律（findAll/split 的 list 入根、`msAsCString` 指针窗口内不分配）与失败路径逆序释放。
- [ ] `tests/ms/regexp_test.ms` 与 `tests/ms/regexp_syntax_test.ms` 覆盖「测试方案」全部清单项，`python run_tests.py` 全部通过；构建产物只落在 `build/`。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过，Debug 构建（ASAN / `/RTC`）下 regexp 测试无内存错误与泄漏报告。
- [ ] 无 TBD/TODO 占位；假定接口（`msRegisterModule` / `msDefineType` / `MsState` 标准库扩展槽 / `struct MsStrBuf` / `msUtf8DecodeRune` / `msUtf8ByteToRuneIndex`）在实现时已与任务 18/33/20/06 的实际定名对齐。
