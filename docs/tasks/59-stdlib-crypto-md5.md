# 59 标准库：crypto/md5

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [18 C API 基础与嵌入示例](18-c-api-foundation.md) |

## 任务目标

交付 C 内建标准库模块 `crypto/md5`（`stdlib/crypto/ms_md5.h` / `stdlib/crypto/ms_md5.c` / `stdlib/crypto/ms_md5_module.c`）：MD5 算法在仓库内自实现（不引入任何外部密码学库），对上暴露 07-stdlib §15 规定的脚本接口——一次性摘要 `md5.sum(data)` 返回 16 字节 bytes，以及增量哈希对象 `md5.new()`（Hasher：`write(data)` / `digest()` / `hexDigest()`）。算法层为纯 C、零分配、零全局状态的可重入实现，供绑定层与未来其他模块（如 HTTP 摘要认证）复用。本任务通过 `tests/ms/crypto_md5_test.ms` 脚本测试（testing 模块 + RFC 1321 测试向量）独立验证。

## 设计依据

- `docs/language/07-stdlib.md` §15：
  - `md5.sum(data)` → bytes（digest）；`h := md5.new()` → Hasher：`write(data)` / `digest()` / `hexDigest()`。
  - §0：`crypto/md5` 为 C 内建模块，位于 `stdlib/` 目录。
  - §7：`io.Writer` 协议规定 `write(data)` 返回 int，Hasher 的 `write` 遵循同一约定。
- RFC 1321（The MD5 Message-Digest Algorithm）：算法本体、四轮 64 步压缩函数、padding 规则与小端长度附加、附录 A.5 的官方测试向量（本任务测试数据来源）。
- `docs/language/09-c-api.md`：
  - §5 值构造：`msNewBytes` / `msNewStringN`；类型判断 `msTypeOf`。
  - §8 错误处理：参数非法时 `msRaiseTypeError` 并返回 `NULL`。
  - §9 扩展模块：`MsModuleDef` / `MsMethodDef` / `MsCFunction` 签名与模块注册。
  - §10 C 自定义类型：`MsTypeDef` / `msDefineType` / `msCInstanceData`，Hasher 以此实现（该 API 的实现在任务 33，编号先于本任务，属顺序前提，不需额外依赖边）。
  - §3 GC 根纪律：C 局部 `MsObject*` 跨分配必须入根。
- `docs/language/10-c-style.md`：§1 include guard 与文件组织、§2 格式化、§3 命名、§4 内部结构体不 typedef、§6 堆分配只经 `msAlloc/msRealloc/msFree`（本模块堆分配全部经脚本对象构造器，无直接 `msAlloc` 调用）。
- `docs/language/11-project-layout.md` §1：C 标准库模块位于 `stdlib/`；§4：脚本测试位于 `tests/ms/`，经仓库根 `run_tests.py` 驱动 mslang CLI 执行。
- 任务 18（C API 基础）假定提供内建 C 模块的注册入口，本文记为 `msRegisterModule` 与内建模块登记表；其确切定名以任务 18 文档为准。bytes 类型的对象访问器（本文记为 `msBytesData` / `msBytesLen`）与类型标签 `MS_TYPE_BYTES` 假定由任务 32（bytes/tuple/set）提供；`MsCFunction` 形式的方法约定 `argv[0]` 为实例自身（self），假定与任务 33 的实现对齐。**以上接口名均为假定命名，实现时以对应任务文档定名为准。**

## 详细设计

### 文件与分层

模块分两层，算法层与解释器完全解耦：

- `stdlib/crypto/ms_md5.h`（include guard `MSLANG_STDLIB_CRYPTO_MS_MD5_H_`，自包含，仅 include `<stddef.h>` `<stdint.h>`）：算法层公开头 + 模块注册函数声明。
- `stdlib/crypto/ms_md5.c`：MD5 算法实现，不 include 任何 mslang 头文件，不做堆分配，不使用平台相关代码。
- `stdlib/crypto/ms_md5_module.c`：脚本绑定层——Hasher C 自定义类型、模块函数表与注册逻辑；include `<mslang/mslang.h>` 与本模块头文件。

分层理由：MD5 是公开定长算法，实现短小且无授权负担，自实现可避免外部依赖、保证全平台行为一致（07-stdlib §0 将 `crypto/*` 归为性能关键的 C 模块）；算法层独立成纯 C 单元后可在无 `MsState` 的环境下复用与移植。

### 算法层接口

```c
#define MS_MD5_BLOCK_LEN 64   // 分组长度（字节）
#define MS_MD5_DIGEST_LEN 16  // 摘要长度（字节）
#define MS_MD5_HEX_LEN 32     // 小写 hex 字符串长度（不含 NUL）

struct MsMd5Ctx {
  uint32_t state[4];      // 链接变量 A B C D
  uint64_t totalLen;      // 已处理消息总长度（字节，用于 padding 时换算 bit 数）
  uint8_t buffer[MS_MD5_BLOCK_LEN];  // 未满一个分组的残余数据
  size_t bufferLen;       // buffer 中有效字节数，恒 < MS_MD5_BLOCK_LEN
};

// Initializes ctx to the RFC 1321 initial state. ctx may live on the stack.
void msMd5Init(struct MsMd5Ctx* ctx);

// Absorbs [data, dataLen) into ctx. data may be NULL when dataLen is 0.
void msMd5Update(struct MsMd5Ctx* ctx, const uint8_t* data, size_t dataLen);

// Applies padding and writes the 16-byte digest to out. ctx is left in an
// unspecified state and must not be reused without msMd5Init.
void msMd5Final(struct MsMd5Ctx* ctx, uint8_t out[MS_MD5_DIGEST_LEN]);

// One-shot convenience: digest of [data, dataLen) into out.
void msMd5Sum(const uint8_t* data, size_t dataLen, uint8_t out[MS_MD5_DIGEST_LEN]);

// Renders a 16-byte digest as 32 lowercase hex chars into out
// (exactly MS_MD5_HEX_LEN bytes; no NUL terminator).
void msMd5Hex(const uint8_t digest[MS_MD5_DIGEST_LEN], char out[MS_MD5_HEX_LEN]);
```

- 全程零堆分配；上下文由调用者持有（栈上或 C 实例数据区），所有权清晰。
- 无端序假设：分组内 32 位字按 RFC 的小端约定逐字节拼装，输出摘要同样逐字节小端写出，代码在大/小端主机上行为一致。

### 核心算法流程（RFC 1321）

- **压缩函数**：每 64 字节分组执行 64 步，分四轮（F/G/H/I 各 16 步）。两个文件级 `static const` 表：
  - `md5ShiftTable[64]`：各步循环左移位数（RFC §3.4）；
  - `md5KTable[64]`：`K[i] = floor(2^32 * abs(sin(i+1)))`，以预计算常量表形式落地（RFC 附录参考实现同款数值），不在运行期调用 `sin`。
- **update**：先把输入拼入 `buffer` 凑满 64 字节则压缩；随后对输入中完整的分组直接逐组压缩（不经过 `buffer` 拷贝）；尾部残余存入 `buffer`。`totalLen` 累加输入字节数。
- **padding（final）**：先补 `0x80`，再补 `0x00` 至当前分组长度 ≡ 56 (mod 64)（不足时压缩一次换新分组），最后附加 `totalLen * 8` 的 64 位小端整数，压缩最后一个分组；`state[0..3]` 逐字小端拆为 16 字节即摘要。消息长度按 RFC 取模 2^64，`totalLen` 用 `uint64_t` 自然回绕即可，无需特判。
- **hex 渲染**：静态字符串 `"0123456789abcdef"` 查表，每字节两字符，固定小写。

### 绑定层：Hasher 自定义类型

经 09-c-api §10 的 `msDefineType` 定义脚本可见类型，`instanceSize` 即上下文大小，每个实例的数据区直接内嵌一份 `struct MsMd5Ctx`，无额外堆分配：

```c
static const MsTypeDef md5HasherType = {
  "Md5Hasher",                  // 脚本侧不直接暴露类名，仅经 md5.new() 获得实例
  sizeof(struct MsMd5Ctx),
  md5HasherInit,                // init：对 msCInstanceData 区域调用 msMd5Init
  NULL,                         // finalize：无 C 侧外部资源，不需要
  NULL,                         // toString：暂不提供，走默认表示
  md5HasherMethods,             // write / digest / hexDigest
};
```

方法实现要点（`MsCFunction` 约定 `argv[0]` 为 self）：

```c
static MsObject* md5HasherWrite(MsState* L, int64_t argc, MsObject** argv);
static MsObject* md5HasherDigest(MsState* L, int64_t argc, MsObject** argv);
static MsObject* md5HasherHexDigest(MsState* L, int64_t argc, MsObject** argv);
```

- `write(data)`：`data` 须为 str 或 bytes（取字节视图见下），否则 `msRaiseTypeError` 返回 `NULL`。对实例上下文调用 `msMd5Update`，返回写入字节数（`msNewInt`，对齐 `io.Writer` 协议）。**不改动哈希语义状态以外的任何东西**，可任意次交错调用。
- `digest()`：在栈上复制一份 `struct MsMd5Ctx` 副本，对副本调用 `msMd5Final`，结果经 `msNewBytes(L, out, MS_MD5_DIGEST_LEN)` 返回。**副本求值保证 `digest()` 不终结、不重置实例**（对齐 Go `hash.Hash.Sum` 语义），之后仍可继续 `write`。
- `hexDigest()`：同上取副本摘要，经 `msMd5Hex` 渲染后用 `msNewStringN(L, hex, MS_MD5_HEX_LEN)` 返回 32 字符小写 hex 字符串；同样不重置实例。
- GC 根纪律：以上路径中局部 `MsObject*` 均为「构造后立即返回」，无跨分配存活对象，无需 `msRootPush`；若实现中引入中间对象，须按 09-c-api §3 入根。

### 绑定层：模块函数与字节视图

```ms
md5.sum(data)    // data: str | bytes → bytes（16 字节摘要）
md5.new()        // → Hasher 实例
```

- 字节视图提取收敛为一个文件内 `static` 辅助函数：str 用 `msAsCString` + `msStringLen`（按 UTF-8 字节原样哈希，不做任何转码）；bytes 用假定接口 `msBytesData` / `msBytesLen`。其余类型报 TypeError。空 str/空 bytes 合法（`dataLen == 0`）。
- `md5.sum(data)`：取字节视图后 `msMd5Sum` 一次性求值，返回 bytes。参数个数非 1 或类型不符时报 TypeError。
- `md5.new()`：经类型对象构造实例（init 回调已完成 `msMd5Init`），返回 Hasher。

### 模块注册

- 模块名 `"crypto/md5"`，函数表：

```c
static const MsMethodDef md5ModuleMethods[] = {
  {"sum", md5Sum, "sum(data) -> bytes"},
  {"new", md5New, "new() -> Hasher"},
  {NULL, NULL, NULL},
};
```

- 注册入口 `MsResult msMd5ModuleRegister(MsState* L)`（头文件中声明，供内建模块登记表调用）：先 `msDefineType` 注册 `Md5Hasher` 类型（类型对象存入模块命名空间或 VM 类型注册表，具体挂接点以任务 18/33 的实现为准），再以 `"crypto/md5"` 为名经 `msRegisterModule` 登记模块。重复注册按幂等处理（已注册则直接返回 `MS_OK`）。
- 整个绑定层不持有跨调用的可变 C 状态（常量表均为 `static const`），符合 10-c-style §4「禁止非 const 可变全局变量」。

## 实现步骤

1. 建 `stdlib/crypto/ms_md5.h` 头文件骨架：常量宏、`struct MsMd5Ctx`、五个算法层函数声明与 `msMd5ModuleRegister` 声明，guard 为 `MSLANG_STDLIB_CRYPTO_MS_MD5_H_`。验证：头文件可独立编译（自包含检查）。
2. 在 `ms_md5.c` 实现压缩函数：F/G/H/I 轮函数（`static inline`）、`md5ShiftTable` / `md5KTable` 常量表、单分组处理例程，字的小端拼装逐字节进行。验证：随第 3 步一并验证。
3. 实现 `msMd5Init` / `msMd5Update` / `msMd5Final`（含 padding 与小端长度附加）与 `msMd5Sum` / `msMd5Hex`。验证：临时用 `mslang -e` 不可行（尚无绑定），以第 6 步完成后的 ms 脚本对 RFC 1321 全部 7 个向量一次性核验；padding 边界（55/56/64/119 字节输入）在测试方案中单列。
4. 建 `stdlib/crypto/ms_md5_module.c`：字节视图辅助函数（str/bytes 分派、TypeError 路径）、`md5Sum` 与 `md5New` 模块函数。验证：`md5.sum("")` 等单向量冒烟脚本通过。
5. 实现 `Md5Hasher` 类型：`MsTypeDef` 定义、init 回调、`write` / `digest` / `hexDigest` 三个方法（副本求值语义）。验证：增量写入分块与一次性求值结果一致的脚本断言通过。
6. 实现 `msMd5ModuleRegister` 并接入内建模块登记表（CMake 将新源文件加入 `mslang` target）。验证：`import "crypto/md5"` 在脚本中可用。
7. 编写 `tests/ms/crypto_md5_test.ms`（testing 模块），接入 `run_tests.py` 既有的 `tests/ms/` 发现机制。验证：`python run_tests.py` 全绿。

## 测试方案

本任务晚于任务 40（testing 模块），测试一律用 ms 脚本。测试文件 `tests/ms/crypto_md5_test.ms`（本任务只交付本设计文档，测试代码随实现编写），`import "testing"` 与 `import "testing/assert"`，测试函数以 `test` 开头，末尾 `testing.run()`；由仓库根 `run_tests.py` 经 mslang CLI 驱动。

覆盖清单：

- **RFC 1321 附录 A.5 全量向量**（`md5.sum` 与 `md5.new().hexDigest()` 各跑一遍）：

| 输入 | 期望 hexDigest |
|---|---|
| `""` | `d41d8cd98f00b204e9800998ecf8427e` |
| `"a"` | `0cc175b9c0f1b6a831c399e269772661` |
| `"abc"` | `900150983cd24fb0d6963f7d28e17f72` |
| `"message digest"` | `f96b697d7cb7938d525a2f31aaf161d0` |
| `"abcdefghijklmnopqrstuvwxyz"` | `c3fcd3d76192e4007dfb496cca67e13b` |
| `"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"` | `d174ab98d277d9f5a5611c2c9f419d9f` |
| `"12345678901234567890123456789012345678901234567890123456789012345678901234567890"` | `57edf4a22be3c955ac49da2e2107b67a` |

- **padding 边界**：长度 55、56、63、64、65、119、128 字节的输入，与已知正确的 MD5 值比对（实现时用权威工具预生成期望值，硬编码进脚本）；重点覆盖「残余 55→补到 120」「残余 56→换分组」两条 padding 分支。
- **增量语义**：同一消息按 1/3/7/64 字节等多种分块 `write`，`hexDigest()` 与一次性 `md5.sum` 的 hex 形式一致；`write` 返回写入字节数（int）。
- **非终结求值**：`digest()` / `hexDigest()` 调用后继续 `write` 仍能正确累积（先求中途摘要、再补数据、终值与一次性结果一致）；多次 `digest()` 返回相同内容。
- **输出形态**：`md5.sum("abc")` 返回 bytes、长度 16；`hexDigest()` 返回 str、长度 32、全小写；`digest()` 返回 bytes、长度 16 且与 `md5.sum` 一致。
- **输入类型**：bytes 输入（如 `b"abc"`）与 str 输入结果一致；含多字节 UTF-8 字符的 str 按 UTF-8 字节哈希（与对同等字节序列的 bytes 输入结果一致）；int/list/nil 等非法类型经 `assert.raises(TypeError, ...)` 校验，`sum` 与 `write` 都覆盖。
- **参数个数**：`md5.sum()`（0 参）、`md5.sum("a", "b")`（2 参）报 TypeError。

## 验收标准

- [ ] `stdlib/crypto/ms_md5.h` / `ms_md5.c` / `ms_md5_module.c` 存在，头文件 guard 为 `MSLANG_STDLIB_CRYPTO_MS_MD5_H_` 且自包含；代码风格符合 10-c-style（2 空格缩进、120 列、K&R、星号贴类型、`struct MsMd5Ctx` 不 typedef、无常变全局变量）。
- [ ] MD5 为仓库内自实现：无任何外部密码学库依赖；算法层不 include mslang 头文件、不做堆分配、无端序假设。
- [ ] 算法层接口（`msMd5Init` / `msMd5Update` / `msMd5Final` / `msMd5Sum` / `msMd5Hex`）与常量宏（`MS_MD5_BLOCK_LEN` / `MS_MD5_DIGEST_LEN` / `MS_MD5_HEX_LEN`）与本文一致。
- [ ] 脚本接口完整：`md5.sum(data)` → bytes（16 字节）；`md5.new()` → Hasher，具 `write(data)` → int、`digest()` → bytes、`hexDigest()` → 32 字符小写 hex str；str/bytes 输入均接受，其余类型与参数个数错误报 TypeError。
- [ ] 增量语义正确：任意分块 `write` 与一次性求值结果一致；`digest()` / `hexDigest()` 为副本求值，不重置实例状态。
- [ ] `tests/ms/crypto_md5_test.ms` 覆盖「测试方案」全部清单项（RFC 1321 全 7 向量、padding 边界、增量、类型错误），`python run_tests.py` 全部通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；假定接口（内建模块注册入口、`msBytesData` / `msBytesLen`、`MS_TYPE_BYTES`、方法 self 约定）在实现时已与任务 18/32/33 的实际定名对齐。
