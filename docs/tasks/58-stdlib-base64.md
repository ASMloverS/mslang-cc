# 58 标准库：encoding/base64

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [18 C API 基础与嵌入示例](18-c-api-foundation.md) |

## 任务目标

交付 C 内建标准库模块 `encoding/base64`（`stdlib/encoding/ms_base64.h` / `stdlib/encoding/ms_base64.c` / `stdlib/encoding/ms_base64_module.c`）：按 RFC 4648 实现 Base64 编解码，对上暴露 `docs/language/07-stdlib.md` §14 规定的脚本接口——`base64.encode(data)`（str/bytes → str，标准字母表）、`base64.decode(text)`（str → bytes）、`base64.urlEncode(data)` / `base64.urlDecode(text)`（URL 安全字母表 `-_`）。算法层为纯 C、零分配、零全局状态的可重入实现，同时提供**整块（one-shot）**与**流式（增量 update/final）**两组 C API：脚本侧的四个函数全部构建在流式 API 之上（整块函数即流式函数的一次性驱动），保证两条路径共享同一份编码逻辑；独立的流式接口供未来基于 io 模块的流式编解码复用。解码采用严格模式：输入长度必须为 4 的倍数、填充 `=` 只允许出现在末尾、非字母表字符与规范外的填充位一律拒绝（ValueError）。本任务通过 `tests/ms/stdlib/encoding_base64_test.ms` 脚本测试（testing 模块 + RFC 4648 §10 测试向量）独立验证。

## 设计依据

- `docs/language/07-stdlib.md`
  - §14 encoding/base64：函数集 `encode(data)` / `decode(text)` / `urlEncode(data)` / `urlDecode(text)`；`encode` 输入 str/bytes、返回 str；`decode` 返回 bytes。
  - §0：`encoding/base64` 为 C 内建模块，位于 `stdlib/` 目录。
  - §19 testing：测试文件 `xxx_test.ms`、测试函数 `test` 前缀、`testing.run()` 约定。
- RFC 4648（The Base16, Base32, and Base64 Data Encodings）：
  - §4：标准字母表（`A–Z a–z 0–9 + /`）、3 字节 → 4 字符分组、尾部 `=` 填充规则（余 1 字节补 `==`，余 2 字节补 `=`）。
  - §5：URL 与文件名安全字母表（`+`→`-`、`/`→`_`），填充字符仍为 `=`。
  - §3.5：规范编码要求未使用的填充位（pad bits）必须为 0；严格解码器可拒绝非零填充位（本实现选择拒绝）。
  - §10：官方测试向量（本任务测试数据来源）。
- `docs/language/09-c-api.md`：
  - §5 值构造：`msNewBytes` / `msNewStringN` / `msAsCString` / `msStringLen`；类型判断 `msTypeOf`。
  - §8 错误处理：参数非法时 `msRaiseTypeError`、解码失败时 `msRaiseValueError`，出错返回 `NULL` 由 VM 转为脚本异常。
  - §9 扩展模块：`MsModuleDef` / `MsMethodDef` / `MsCFunction` 签名与 `msRegisterModule` 注册。
- `docs/language/10-c-style.md`：§1 include guard 与文件组织、§2 格式化、§3 命名、§4 内部结构体不 typedef 与禁止非 const 可变全局变量、§6 堆分配只经 `msAlloc/msRealloc/msFree`（本模块堆分配全部经脚本对象构造器，无直接 `msAlloc` 调用）。
- `docs/language/11-project-layout.md` §1：C 标准库模块位于 `stdlib/`；§4：脚本测试位于 `tests/ms/`，经仓库根 `run_tests.py` 驱动 mslang CLI 执行。
- 任务 18（C API 基础）假定提供内建 C 模块的注册入口，本文记为 `msRegisterModule` 与内建模块登记表；bytes 类型的对象访问器（本文记为 `msBytesData` / `msBytesLen`）与类型标签 `MS_TYPE_BYTES` 假定由任务 32（bytes/tuple/set）提供，编号先于本任务，属顺序前提。**以上接口名均为假定命名，实现时以对应任务文档定名为准。**

## 详细设计

### 文件与分层

模块分两层，算法层与解释器完全解耦（与任务 59 `crypto/md5` 同一组织方式）：

- `stdlib/encoding/ms_base64.h`（include guard `MSLANG_STDLIB_ENCODING_MS_BASE64_H_`，自包含，仅 include `<stdbool.h>` `<stddef.h>` `<stdint.h>`）：算法层公开头 + 模块注册函数声明。
- `stdlib/encoding/ms_base64.c`：Base64 算法实现，不 include 任何 mslang 头文件，不做堆分配，不使用平台相关代码。
- `stdlib/encoding/ms_base64_module.c`：脚本绑定层——模块函数表、参数校验、对象构造与注册逻辑；include `<mslang/mslang.h>` 与本模块头文件。

分层理由：Base64 是公开定长算法，实现短小且无授权负担；算法层独立成纯 C 单元后可在无 `MsState` 的环境下复用（如未来 net/http 的 Basic 认证头、流式编解码器）。

### 算法层接口

```c
typedef enum {
  MS_BASE64_STD,  // RFC 4648 §4 标准字母表，带 = 填充
  MS_BASE64_URL   // RFC 4648 §5 URL 安全字母表（- 和 _），带 = 填充
} MsBase64Variant;

// ---- 长度计算（恒为精确值或上界） ----

// Exact encoded length in bytes for dataLen input bytes (including padding).
size_t msBase64EncodedLen(size_t dataLen);

// Maximum decoded length in bytes for a padded textLen (multiple of 4).
size_t msBase64DecodedMaxLen(size_t textLen);

// ---- 整块（one-shot）API：构建在流式 API 之上 ----

// Encodes [data, dataLen) into out (capacity >= msBase64EncodedLen(dataLen));
// returns the exact byte count written. data may be NULL when dataLen is 0.
size_t msBase64Encode(MsBase64Variant variant, const uint8_t* data, size_t dataLen, char* out);

// Strictly decodes [text, textLen) into out (capacity >=
// msBase64DecodedMaxLen(textLen)); on success returns true and stores the
// exact byte count in *outLen; on invalid input returns false and stores the
// byte offset of the offending character in *outLen.
bool msBase64Decode(MsBase64Variant variant, const char* text, size_t textLen, uint8_t* out,
    size_t* outLen);

// ---- 流式（增量）API ----

struct MsBase64Encoder {
  MsBase64Variant variant;
  uint8_t pending[2];   // 不足 3 字节的残余输入
  size_t pendingLen;    // 0..2
};

struct MsBase64Decoder {
  MsBase64Variant variant;
  uint8_t pending[4];   // 不足 4 字符的残余分组（已映射为 6 位值）
  size_t pendingLen;    // 0..3
  bool finished;        // 已见填充分组，此后任何输入均非法
};

void msBase64EncoderInit(struct MsBase64Encoder* enc, MsBase64Variant variant);

// Encodes the next chunk; returns bytes written. Caller provides capacity >=
// 4 * ((pendingLen + dataLen) / 3).
size_t msBase64EncoderUpdate(struct MsBase64Encoder* enc, const uint8_t* data, size_t dataLen,
    char* out);

// Emits the final partial group plus padding; returns bytes written (0 or 4).
// The encoder must not be reused without msBase64EncoderInit.
size_t msBase64EncoderFinal(struct MsBase64Encoder* enc, char* out);

void msBase64DecoderInit(struct MsBase64Decoder* dec, MsBase64Variant variant);

// Decodes the next chunk; semantics match msBase64Decode (false + offending
// offset relative to this chunk on invalid input).
bool msBase64DecoderUpdate(struct MsBase64Decoder* dec, const char* text, size_t textLen,
    uint8_t* out, size_t* outLen);

// Validates that the stream ended on a group boundary (pendingLen == 0).
bool msBase64DecoderFinal(struct MsBase64Decoder* dec);
```

要点：

- 全程零堆分配；上下文由调用者持有（栈上），所有权清晰；字母表与解码表为文件级 `static const`（编码字母表两份各 64 字节，解码表两份各 256 项的 `int8_t` 映射，非法字符为 -1，与任务 59 的常量表做法一致）。
- `msBase64EncodedLen`：`4 * ((dataLen + 2) / 3)`；`msBase64DecodedMaxLen`：`3 * (textLen / 4)`（调用方已校验 `textLen % 4 == 0`）。注意 `size_t` 溢出保护：`dataLen > (SIZE_MAX / 4) * 3` 之类的病态输入在绑定层先按 `int64` 长度上限拒绝，算法层不重复检查（绑定层是唯一入口）。
- 流式编码：`pending` 缓冲凑满 3 字节即输出 4 字符；`Final` 把残余 1/2 字节编码为 `XX==` / `XXX=`。
- 流式解码：`pending` 缓冲凑满 4 字符即输出 1–3 字节（视 `=` 个数）；含 `=` 的分组之后置 `finished`，后续任何非空输入报错。

### 核心算法流程（RFC 4648）

- **编码**：每 3 字节 `(a, b, c)` 展开为 4 个 6 位值 `a>>2`、`((a<<4)|(b>>4))&0x3F`、`((b<<2)|(c>>2))&0x3F`、`c&0x3F`，查字母表写出；尾组余 1 字节时写 `s0 s1 ==`，余 2 字节时写 `s0 s1 s2 =`。
- **解码**（严格模式）：每 4 字符一组，经查表映射为 6 位值后逆运算重组 3 字节。逐组校验规则：
  1. 非字母表字符（含空白、换行、其他符号）报错——本实现**不接受** MIME 风格的换行内嵌（07-stdlib §14 未规定，取 RFC 4648 §3.3「非字母表字符须拒绝」的严格解释，与 Go `base64.StdEncoding` 一致）；
  2. `=` 只允许出现在末组末尾，且连续 1 或 2 个（`XX==` 或 `XXX=`），出现在其他位置、或末组之后还有数据均报错；
  3. 填充位必须为 0（RFC 4648 §3.5 规范形式）：`XX==` 组的第二字符低 4 位、`XXX=` 组的第三字符低 2 位非零时报「non-canonical padding」错误（如 `Zh==` 非法、`Zg==` 合法）；
  4. 输入总长度必须是 4 的倍数——严格模式不省略填充（`Zg` 报错，07-stdlib §14 未提供 raw 变体函数，url 系列同样带填充；与 RFC 4648 §5「填充可选」的宽松形态刻意区分，保证 encode/decode 严格互逆）。
- 错误定位：解码失败时把**出错字符在输入中的字节偏移**经 `outLen` 传出，供绑定层拼入错误消息。

### 绑定层：模块函数

```ms
base64.encode(data)      // data: str | bytes → str（标准字母表，带填充）
base64.decode(text)      // text: str → bytes
base64.urlEncode(data)   // data: str | bytes → str（URL 安全字母表，带填充）
base64.urlDecode(text)   // text: str → bytes
```

- 四个 `MsCFunction` 实现均为文件内 `static`，参数校验：`argc != 1` 报 TypeError；`encode`/`urlEncode` 接受 str 或 bytes，其余类型报 TypeError；`decode`/`urlDecode` 只接受 str（07-stdlib §14 的 `text` 参数），bytes 或其他类型报 TypeError。
- 字节视图提取收敛为一个文件内 `static` 辅助函数：str 用 `msAsCString` + `msStringLen`（按 UTF-8 字节原样编码，不做任何转码）；bytes 用假定接口 `msBytesData` / `msBytesLen`。空 str/空 bytes 合法（编码结果为空串，解码空串得空 bytes）。
- 编码路径：取字节视图 → `msBase64EncodedLen` 定长 → 栈上小缓冲（≤ 64 字节输入）或临时缓冲（较大输入经 `msAlloc`，用后 `msFree`；实现自选，亦可直接构造未定长缓冲再 `msNewStringN` 拷贝）→ `msBase64Encode` → `msNewStringN` 返回 str。
- 解码路径：先校验 `textLen % 4 == 0`（否则 ValueError，消息含「length is not a multiple of 4」）→ `msBase64DecodedMaxLen` 分配输出缓冲 → `msBase64Decode`；失败时 `msRaiseValueError(L, "encoding/base64: invalid character at byte offset %zu", offset)`（填充位错误与填充位置错误分别给出对应消息，消息模板为文件内 `static const`）→ 成功时 `msNewBytes(L, out, outLen)` 返回 bytes 并释放临时缓冲。
- GC 根纪律：以上路径中局部 `MsObject*` 均为「构造后立即返回」，无跨分配存活对象，无需 `msRootPush`；若实现中引入中间对象，须按 09-c-api §3 入根。
- 模块注册表与注册入口（09-c-api §9 约定，`{NULL, NULL, NULL}` 结尾）：

```c
static const MsMethodDef msBase64Methods[] = {
  {"encode",    msBase64EncodeFn,   "encode(data) -> str"},
  {"decode",    msBase64DecodeFn,   "decode(text) -> bytes"},
  {"urlEncode", msBase64UrlEncodeFn,"urlEncode(data) -> str"},
  {"urlDecode", msBase64UrlDecodeFn,"urlDecode(text) -> bytes"},
  {NULL, NULL, NULL},
};

static const MsModuleDef msBase64Module = {
  "encoding/base64", "Base64 encoding and decoding (RFC 4648)", msBase64Methods,
};

// Registers the "encoding/base64" builtin module into L. Called once during
// interpreter startup by the stdlib bootstrap.
MsResult msBase64Register(MsState* L);
```

- 模块函数名与 C 实现函数名冲突处理：C 侧 `static` 实现统一加 `Fn` 后缀（上表），避免与算法层 `msBase64Encode` / `msBase64Decode` 撞名。
- 整个绑定层不持有跨调用的可变 C 状态（常量表均为 `static const`），符合 10-c-style §4；重复注册按幂等处理（已注册则直接返回 `MS_OK`）。

## 实现步骤

1. 建 `stdlib/encoding/ms_base64.h` 头文件骨架：`MsBase64Variant` 枚举、长度函数、整块与流式函数声明、`struct MsBase64Encoder` / `struct MsBase64Decoder`（不 typedef）、`msBase64Register` 声明，guard 为 `MSLANG_STDLIB_ENCODING_MS_BASE64_H_`。验证：头文件可独立编译（自包含检查）。
2. 在 `ms_base64.c` 落地两份编码字母表与两份 256 项解码表（`static const`，含 -1 非法标记）、长度函数。验证：随第 3 步一并验证。
3. 实现流式编码器（`msBase64EncoderInit` / `Update` / `Final`）与整块 `msBase64Encode`（构建在流式 API 上）。验证：第 7 步脚本测试的 RFC 4648 §10 向量与尾组 1/2 字节两种填充形态。
4. 实现流式解码器（`msBase64DecoderInit` / `Update` / `Final`）与整块 `msBase64Decode`：四字符分组重组、逐组校验（非法字符、`=` 位置、填充位、长度倍数）、错误偏移传出。验证：第 7 步脚本测试的全部负例。
5. 建 `stdlib/encoding/ms_base64_module.c`：字节视图辅助函数（str/bytes 分派、TypeError 路径）、四个模块函数、`msBase64Register`。验证：`base64.encode("")` 冒烟脚本通过。
6. 注册接线与构建集成：`msBase64Register` 挂入内建模块登记表，CMake 将新源文件加入 `mslang` target。验证：脚本中 `import "encoding/base64"` 可用，四个函数名存在。
7. 编写 `tests/ms/stdlib/encoding_base64_test.ms`（testing 模块），接入 `run_tests.py` 既有的 `tests/ms/` 发现机制。验证：`python run_tests.py` 全绿。

## 测试方案

本任务晚于任务 40（testing 模块），测试一律用 ms 脚本。测试文件 `tests/ms/stdlib/encoding_base64_test.ms`（本任务只交付本设计文档，测试代码随实现编写），`import "testing"` 与 `import "testing/assert"`，测试函数以 `test` 开头，末尾 `testing.run()`；由仓库根 `run_tests.py` 经 mslang CLI 驱动。

覆盖清单：

- **RFC 4648 §10 全量向量**（`encode`/`decode` 往返断言；url 系列断言相同解码结果与 URL 字母表编码结果——注意 RFC 表中 base64url 向量省略填充，本实现带填充，期望值取补全填充后的形式）：

| 输入 | 标准编码 | URL 安全编码 |
|---|---|---|
| `""` | `""` | `""` |
| `"f"` | `"Zg=="` | `"Zg=="` |
| `"fo"` | `"Zm8="` | `"Zm8="` |
| `"foo"` | `"Zm9v"` | `"Zm9v"` |
| `"foob"` | `"Zm9vYg=="` | `"Zm9vYg=="` |
| `"fooba"` | `"Zm9vYmE="` | `"Zm9vYmE="` |
| `"foobar"` | `"Zm9vYmFy"` | `"Zm9vYmFy"` |

- **字母表差异**：`b"\xfb\xff"` 标准编码为 `"+/8="`、URL 编码为 `"-_8="`；两组各含 `-`/`_` 或 `+`/`/` 的串只能用对应解码函数解码，交叉解码报 ValueError。
- **输出形态**：`encode`/`urlEncode` 返回 str；`decode`/`urlDecode` 返回 bytes 且内容与原始输入逐字节一致；空输入编码得 `""`、解码 `""` 得空 bytes（`len == 0`）。
- **str 与 bytes 输入等价**：`encode("foobar")` 与 `encode(b"foobar")` 结果一致；含多字节 UTF-8 字符的 str（如 `"你好"`）按其 UTF-8 字节编码，与对同等字节序列的 bytes 输入结果一致，解码回 bytes 后与原字节一致。
- **往返一致性**：随机或构造的多组数据（长度覆盖 mod 3 余 0/1/2 三种）经 `decode(encode(x)) == x` 断言；`urlDecode(urlEncode(x)) == x` 同理。
- **解码负例**（均经 `assert.raises(ValueError, ...)` 校验，`decode` 与 `urlDecode` 各跑一遍适用项）：
  - 长度非 4 的倍数：`"Zg"`、`"Zm9"`；
  - 非法字符：`"Zm9v!"`、`"Zm 9v"`（内嵌空白）、`"Zm9v\n"`（内嵌换行）；
  - 填充位置错误：`"=m9v"`、`"Z==v"`、`"Zm9vZg=="`（填充不在末尾后还有数据）、`"Zm9v===="`（填充超过 2 个）；
  - 非规范填充位（RFC 4648 §3.5）：`"Zh=="`（第二字符低 4 位非零）、`"Zm9="` 合法而 `"Zm/="`（第三字符低 2 位非零）非法；
  - 字母表交叉：标准串含 `+`/`/` 交给 `urlDecode`、URL 串含 `-`/`_` 交给 `decode`。
- **参数类型与个数**：`encode(42)`、`encode(nil)`、`encode([1])` 报 TypeError；`decode(b"Zm9v")`、`decode(42)` 报 TypeError；`encode()`（0 参）、`encode("a", "b")`（2 参）报 TypeError。

## 验收标准

- [ ] `stdlib/encoding/ms_base64.h` / `ms_base64.c` / `ms_base64_module.c` 存在，头文件 guard 为 `MSLANG_STDLIB_ENCODING_MS_BASE64_H_` 且自包含；代码风格符合 10-c-style（2 空格缩进、120 列、K&R、星号贴类型、`struct MsBase64Encoder` / `struct MsBase64Decoder` 不 typedef、无常变全局变量）。
- [ ] Base64 为仓库内自实现：算法层不 include mslang 头文件、不做堆分配、无端序与平台假设；堆分配只经 `msAlloc/msRealloc/msFree` 或脚本对象构造器。
- [ ] 算法层接口与本文一致：`MsBase64Variant`、`msBase64EncodedLen` / `msBase64DecodedMaxLen`、整块 `msBase64Encode` / `msBase64Decode`、流式 `msBase64EncoderInit/Update/Final` 与 `msBase64DecoderInit/Update/Final`；整块 API 构建在流式 API 之上，两条路径共享同一编码逻辑。
- [ ] 脚本接口完整：`base64.encode` / `base64.decode` / `base64.urlEncode` / `base64.urlDecode` 四函数可用；`encode` 接受 str/bytes 返回 str（UTF-8 字节原样编码），`decode` 接受 str 返回 bytes；参数类型与个数错误报 TypeError。
- [ ] 编码严格符合 RFC 4648：标准与 URL 安全两份字母表、尾组 `==`/`=` 填充、编码输出恒为 4 的倍数。
- [ ] 解码为严格模式：长度必须 4 的倍数、拒绝一切非字母表字符（含空白换行）、`=` 仅允许末尾 1–2 个、非零填充位拒绝；所有解码失败报 ValueError 且消息含出错字节偏移。
- [ ] `tests/ms/stdlib/encoding_base64_test.ms` 覆盖「测试方案」全部清单项（RFC 4648 §10 全 7 向量、字母表差异、填充位规范、负例与类型错误），`python run_tests.py` 全部通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；假定接口（`msRegisterModule` 内建模块注册入口、`msNewBytes`、`msBytesData` / `msBytesLen`、`MS_TYPE_BYTES`）在实现时已与任务 18/32 的实际定名对齐。
