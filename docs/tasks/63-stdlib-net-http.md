# 63 标准库：net/http

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [62 标准库：net](62-stdlib-net.md) |

## 任务目标

交付标准库模块 `net/http`（HTTP/1.1 客户端与服务器），完整覆盖 `docs/language/07-stdlib.md` §17 的脚本接口：

- **客户端**：`http.get(url, headers=nil, timeout=30)`、`http.post(url, body, headers=nil)`、`http.request(method, url, body=nil, headers=nil)`，返回 Response 对象（`resp.status` / `resp.headers` / `resp.body` / `resp.json()`）。
- **服务器**：`http.server(addr, handler)` 得到 Server 对象，`srv.serve()` 按「每连接一个协程」服务；handler 返回 `http.response(status, body, headers=nil)` 构造的响应，或直接返回 str / dict（自动 JSON）。

实现按 §0 的定为 **C + .ms 混合**分两层：

- **C 引擎层**（`src/net/ms_http.{c,h}`）：纯字节缓冲上的 HTTP/1.1 报文解析器（增量状态机，请求/响应两种模式）、URL 解析器、报文序列化器。不触碰 `MsObject`、不做任何 socket IO。
- **C 绑定层**（`stdlib/net/ms_std_http.{c,h}`）：把引擎包装为内部 C 内建模块 `_http`（Parser 自定义类型 + 解析/序列化函数），唯一导出符号为注册入口。
- **脚本层**（`lib/net/http.ms`）：公开 API 全貌。socket 的收发泵循环（feed → 取报文 → 再读）位于本层，调用任务 62 的协程感知 `conn.read`/`conn.write` 自然挂起协程，实现 §25「`http.*` 全部协程感知」的约定。

首版范围（§17 明示与本文补充裁定）：仅 HTTP/1.1、无 TLS（`https://` 报 ValueError）、无 chunked 传输编码、无压缩、不重定向跟随；客户端每请求一条连接（`Connection: close`），服务器支持同连接 keep-alive。本任务经 `tests/ms/stdlib/net_http/` 的 ms 脚本测试（testing 模块，本地回环服务用例）独立验收。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`net/http` 为「C + .ms」混合实现——本任务两层划分的依据。
  - §17 net/http：客户端/服务器 API 清单与「首版仅 HTTP/1.1，不含 TLS」——本任务功能边界的唯一来源。
  - §16 net：`net.dial("tcp", addr)` → conn（`read(n)`/`write(data)`/`close()`/`setDeadline(seconds)`）、`net.listen("tcp", addr)` → listener（`accept()`）——脚本层传输的唯一底座。
  - §7 io：Reader 鸭子协议「`read` 返回空数据即 EOF」——泵循环的 EOF 判定依据。
  - §13 encoding/json：`resp.json()` 与 dict 响应自动 JSON 的实现依赖（`json.dumps`/`json.loads`，任务 57 已定稿）。
  - §19 testing：测试文件 `xxx_test.ms`、`test` 前缀、`testing.run()` 约定。
  - §25：`http.*` 全部 API 协程感知（阻塞时挂起协程而非 OS 线程）。
- `docs/language/06-concurrency.md` §1（`async func` 调用即派生协程、句柄可显式丢弃并注释理由）与 §4（阻塞 IO 为让出点）。服务器「每连接一个协程」即以 `async func` 派生实现。
- `docs/language/04-exceptions.md` §4 异常层级：`OSError`（含子类 `TimeoutError`）、`ValueError`（含子类 `UnicodeError`）、`TypeError` 的既有类名。
- `docs/language/02-types.md` §4/§1：str 为不可变合法 UTF-8 序列、`bytes.toString()` 的编码约定——响应体解码的依据。
- `docs/language/09-c-api.md` §3（GC 根栈纪律）、§5（`msNewStringN`/`msNewBytes`/`msNewDict`）、§6（`msDictSet`）、§8（`msRaiseTypeError`/`msRaiseValueError`/`msRaiseOSError`）、§9（`MsModuleDef`/`MsMethodDef`/`msRegisterModule`）、§10（`MsTypeDef`/`msDefineType`/`msCInstanceData`，finalize 纪律：数据区不持有脚本对象）。
- `docs/language/10-c-style.md`：§1 include guard、§2 格式化、§3 命名、§4 内部结构体不 typedef、§6 堆分配只经 `msAlloc/msRealloc/msFree`、§9 平台相关代码只在 `src/platform/`（本任务两层均不直接做 socket 调用，天然满足）。
- `docs/language/12-ms-style.md`：脚本层规范（4 空格缩进、行宽 120、内部函数 `_` 前缀、公开函数英文前置文档注释、模块级状态原则禁止）。
- `docs/language/11-project-layout.md` §1（`src/`、`stdlib/`、`lib/`、`tests/ms/` 位置约定）。
- RFC 9112（HTTP/1.1 消息语法：start line、header field、CRLF 终止、`Content-Length` 定界）与 RFC 9110 §15（状态码与 reason phrase）——报文格式的权威参照。
- [43 协程与 async/await](43-coroutines.md)：协程派生/挂起/唤醒语义已定稿；调度器在主协程 DEAD 后即返回——测试脚本主协程结束时遗留的服务器协程随之终止，回环测试无需显式关闭服务器即可退出。
- [42 平台抽象层](42-platform-layer.md)：socket 原语与非阻塞/poll 语义（任务 62 的 conn 即建于其上）；本任务不直接消费。
- 任务 62（net）的设计文档本文撰写时**尚不存在**：本文引用的 net 接口（`net.dial`/`net.listen`、`conn.read` 返回 bytes、`conn.write` 接受 str/bytes、`conn.setDeadline(seconds)` 到期抛 `TimeoutError`、`listener.accept()`/`listener.close()`、`listener.addr()` 返回实际绑定地址字符串、临时端口 `":0"`）均为假定命名与假定语义，实现时以对应任务文档定名为准。
- 任务 18（C API 基础）的 stdlib 启动注册链为假定机制，本文只交付模块自身注册入口，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件布局与分层

```
src/net/
├── ms_http.h            # 引擎层公开头（guard MSLANG_SRC_NET_MS_HTTP_H_）
└── ms_http.c            # 引擎实现：增量解析状态机、URL 解析、序列化；纯 C，无 MsObject
stdlib/net/
├── ms_std_http.h        # 绑定层对外声明（guard MSLANG_STDLIB_NET_MS_STD_HTTP_H_）
└── ms_std_http.c        # "_http" 模块：HttpParser C 类型、模块函数表、注册入口
lib/net/
└── http.ms              # 公开 API：request/get/post/response/server 与 Response/Request/Server
```

分层职责（沿用任务 35/37/57 的「引擎层 + 模块层」模式）：

- **引擎层**：输入输出都是字节切片与 C 结构体，唯一外部依赖是任务 02 的分配接口（`msAlloc/msRealloc/msFree`）。不含 socket 调用（§9 的平台纪律因此天然满足），可在无解释器环境下独立移植复用。
- **绑定层**：`MsCFunction` 包装——参数校验、dict/list 与 C 结构体互转、错误转脚本异常。全部函数 `static`，唯一导出符号 `msStdHttpRegister`。
- **脚本层**：唯一的 IO 场所。`_http` 是内部实现细节，不作为公开 API 承诺（以下划线前缀名标示）。

**「C 层负责协议解析/传输」的落地解释**（关键设计决策）：mslang 协程是 VM 级结构，所有原生 C 函数共享同一条 C 栈，无法在一次 C 调用中途挂起协程；只有 `conn.read`/`conn.write` 这类**单步原子原语**（任务 62）能以「未就绪即让出、唤醒后重入」的方式协程感知。因此复合 IO 循环（反复 read 直到收齐一个完整报文）在构造上不可能放进单个 C 函数。本任务据此划分：C 层负责**协议解析与线上格式的编解码**（报文解析、URL 解析、报文序列化），**实际 socket 收发泵**由脚本层承担——脚本层的 `conn.read` 调用点天然是协程让出点，§25 的协程感知约定由任务 62 的原语保证。此解释是对 §0「C + .ms」分工的显式裁定。

**模块注册名与解析**：C 绑定层注册为内建模块 `"_http"`；公开模块 `net/http` 是纯脚本模块 `lib/net/http.ms`，经任务 24 的解析档（安装目录 `lib/` + 路径 `net/http` → `lib/net/http.ms`）命中，文件内 `import "_http"` 与 `import "net"`。若 C 层直接注册 `"net/http"`，按 §2 的解析顺序（注册表优先于文件候选）会永久遮蔽脚本层，故内部名必须不同——这与 CPython `_socket`/`socket.py` 的分层同理。

### 2. 引擎层接口（src/net/ms_http.h）

include guard `MSLANG_SRC_NET_MS_HTTP_H_`，自包含（`<stdbool.h>` `<stddef.h>` `<stdint.h>` 及任务 02 的 `"core/ms_result.h"`）。内部结构体不 typedef；解析器对调用者以不完整类型暴露：

```c
#define MS_HTTP_MAX_START_LINE 8192              // request/status line 字节上限
#define MS_HTTP_MAX_HEADER_BYTES 65536           // 整个头部块（含 CRLF）上限
#define MS_HTTP_MAX_HEADERS 200                  // 头部字段数上限
#define MS_HTTP_MAX_BODY_BYTES (16 * 1024 * 1024)  // 报文体上限
#define MS_HTTP_MAX_METHOD_LEN 32

typedef enum {
  MS_HTTP_REQUEST,
  MS_HTTP_RESPONSE
} MsHttpKind;

typedef enum {
  MS_HTTP_PARSE_INCOMPLETE,   // 数据不足，继续 feed
  MS_HTTP_PARSE_OK,           // 产出一个完整报文（*out 归调用者，msHttpMessageFree 释放）
  MS_HTTP_PARSE_ERROR,        // 协议错误；errBuf 含原因（粘滞：此后一切调用即报错）
  MS_HTTP_PARSE_OOM
} MsHttpParseStatus;

struct MsHttpHeader {
  char* name;    // msAlloc'd，已归一为小写
  char* value;   // msAlloc'd，已剥离首尾 OWS
};

struct MsHttpMessage {
  MsHttpKind kind;
  // request 专有（response 时 method/target 为 NULL）
  char* method;
  char* target;        // 原始 request-target（path + optional "?" query），未解码
  // response 专有（request 时 status 为 0、reason 为 NULL）
  int status;
  char* reason;
  // 公共
  int versionMinor;              // HTTP/1.x 的 x；首版只接受 0/1
  struct MsHttpHeader* headers;  // msAlloc'd 数组
  size_t headerCount;
  uint8_t* body;                 // msAlloc'd；bodyLen == 0 时为 NULL
  size_t bodyLen;
};

// Creates an incremental parser. NULL on OOM.
struct MsHttpParser* msHttpParserNew(MsHttpKind kind);
void msHttpParserFree(struct MsHttpParser* p);

// Appends [data, len) to the internal buffer (copies; caller keeps its own).
// MS_ERROR_RUNTIME when a size limit is exceeded or the parser is finished.
MsResult msHttpParserFeed(struct MsHttpParser* p, const char* data, size_t len);

// Attempts to parse one complete message from buffered data, consuming it.
// *out is set only on MS_HTTP_PARSE_OK. Multiple messages may be extracted
// from one buffer across calls (keep-alive / pipelining).
MsHttpParseStatus msHttpParserNext(struct MsHttpParser* p, struct MsHttpMessage** out,
    char* errBuf, size_t errBufLen);

// Signals end of stream. Succeeds with a message only when the pending
// message is EOF-delimited (response without Content-Length); a pending
// Content-Length body is MS_HTTP_PARSE_ERROR ("unexpected EOF"). After this
// call the parser accepts no further input.
MsHttpParseStatus msHttpParserFinish(struct MsHttpParser* p, struct MsHttpMessage** out,
    char* errBuf, size_t errBufLen);

void msHttpMessageFree(struct MsHttpMessage* msg);
```

URL 解析与序列化：

```c
struct MsHttpUrl {
  char* scheme;   // msAlloc'd；仅 "http" 合法（其余在解析期即拒绝）
  char* host;     // msAlloc'd；非空
  uint16_t port;  // 缺省 80
  char* path;     // msAlloc'd；缺省 "/"，恒以 '/' 开头
  char* query;    // msAlloc'd 或 NULL（无 '?' 时）
};

// Parses an absolute http URL. Rejects (MS_ERROR_RUNTIME, errBuf holds the
// reason): non-http scheme, missing/empty host, userinfo ("u@h"), IPv6
// literals, port out of range or non-numeric. Percent-decoding is NOT done.
MsResult msHttpUrlParse(const char* url, size_t urlLen, struct MsHttpUrl* out,
    char* errBuf, size_t errBufLen);
void msHttpUrlClear(struct MsHttpUrl* u);

// Serializes a request/response into a newly msAlloc'd buffer
// (*out/*outLen; caller frees with msFree). The serializer always writes a
// computed Content-Length and silently skips any caller-supplied
// content-length header (case-insensitive). Transfer-Encoding is never
// emitted (no chunked). Reason phrase comes from msHttpStatusText.
MsResult msHttpWriteRequest(char** out, size_t* outLen, const char* method, const char* target,
    const struct MsHttpHeader* headers, size_t headerCount, const uint8_t* body, size_t bodyLen);
MsResult msHttpWriteResponse(char** out, size_t* outLen, int status,
    const struct MsHttpHeader* headers, size_t headerCount, const uint8_t* body, size_t bodyLen);

// Static table for common codes (200/201/204/301/302/304/400/401/403/404/
// 405/500/501/502/503); returns "Unknown" otherwise.
const char* msHttpStatusText(int status);
```

### 3. 解析器算法与报文定界规则

`struct MsHttpParser`（定义于 `ms_http.c` 内部）持有：累积缓冲（`char* buf/bufLen/bufCap`，`feed` 经 `msRealloc` 倍增增长）、阶段标志（`headersDone`/`eof`/`failed`）、头部期解析结果暂存（定界信息：`int64_t contentLength`、`bool bodyUntilEof`）与待完成的半成品报文。

`msHttpParserNext` 的两阶段流程：

1. **头部期**（`headersDone == false`）：在缓冲中查找 `"\r\n\r\n"`；未找到则按上限检查（缓冲 > `MS_HTTP_MAX_HEADER_BYTES` 报错）后返回 `MS_HTTP_PARSE_INCOMPLETE`。找到后切出头部块，逐行解析：
   - 首行：请求行 `METHOD SP target SP HTTP/1.x`（method 为 token 字符、长度 ≤ 32；target 非空）或状态行 `HTTP/1.x SP 3DIGIT [SP reason]`；格式不符报 `MS_HTTP_PARSE_ERROR`（errBuf 英文消息含情形描述，如 `"malformed request line"`）。
   - 头部行：`name ":" OWS value OWS`，name 为 token 字符并归一小写；拒绝：无冒号、非法 name、obs-fold（以 SP/HTAB 开头的续行）、裸 CR/LF；字段数超 `MS_HTTP_MAX_HEADERS` 报错。
   - 定界判定（头部解析完成后立即计算）：出现 `Transfer-Encoding`（任何值，含 chunked）→ 报错 `"transfer-encoding not supported"`（首版只支持 `Content-Length` 定界与 EOF 定界）；`Content-Length` 出现多次且值不一致 → 报错，一致 → 取该值（非十进制数字或超 `MS_HTTP_MAX_BODY_BYTES` 报错）；无 `Content-Length`：请求 → 报文体为空，头部结束即报文完成；响应 → 状态码为 1xx/204/304 时报文体为空，否则 `bodyUntilEof = true`（读到连接关闭为止）。
2. **报文体期**：`bodyUntilEof == false` 时缓冲满 `contentLength` 字节即产出报文（消费头部 + 定长体，缓冲余量 `memmove` 前移留给下一报文）；否则等待 `msHttpParserFinish` 把全部剩余缓冲作为报文体产出。`feed` 在任一期使缓冲超过上限即返回 `MS_ERROR_RUNTIME`。

错误为粘滞态（`failed`）：一旦 `MS_HTTP_PARSE_ERROR`，后续 `feed`/`next`/`finish` 不再推进——连接级协议错误后该连接的唯一出路是关闭，与 HTTP/1.1 实践一致。

### 4. URL 解析

单遍扫描，形如 `http://host[:port][/path][?query]`：scheme 必须字面 `http://`（大小写不敏感比较，其余含 `https://` 直接拒绝）；host 到 `:`/`/`/`?`/串尾为止，非空；端口 1–5 位数字、范围 1–65535；path 缺省或空时归一为 `/`；query 为 `?` 后原文（允许空串）。不做百分号解码、不处理 fragment（`#` 出现即拒绝——绝对请求 URI 不应含 fragment）、不支持 userinfo 与 IPv6 字面量（显式报错，列入路线图）。回环测试走 `http://127.0.0.1:port/...`，不受影响。

### 5. 绑定层：内部模块 `_http`（stdlib/net/ms_std_http.c）

头文件 `ms_std_http.h`（guard `MSLANG_STDLIB_NET_MS_STD_HTTP_H_`，自包含 `<mslang/mslang.h>`）只导出：

```c
// Registers the internal "_http" builtin module (the HttpParser C type and
// the parse/serialize helper functions backing lib/net/http.ms). Called
// once during interpreter startup by the stdlib bootstrap.
MsResult msStdHttpRegister(MsState* L);
```

**HttpParser 类型**（`MsTypeDef`，`name = "HttpParser"`）：实例数据区只持有一个 `struct MsHttpParser*`（无脚本对象引用，满足任务 33「C 类型数据区不得持有脚本对象」的约束）；`init` 接受 `("request" | "response")` 一个 str 参数并建引擎解析器；`finalize` 仅 `msHttpParserFree`。方法（均 `static MsCFunction`，`argv[0]` 为 self）：

| 方法 | 语义 | 失败 |
|---|---|---|
| `feed(data)` | data 为 str 或 bytes，追加到内部缓冲，返回 nil | 类型不符 TypeError；超限/已 finish 抛 ValueError |
| `next()` | 尝试取一个完整报文；成功返回报文 dict（见下），不足返回 nil | 协议错误抛 ValueError（消息前缀 `http:`） |
| `finish()` | 通知 EOF；EOF 定界报文返回报文 dict，否则 nil（无待完成报文）或抛错（定长体未收齐） | ValueError |

**报文 dict 形状**（`next`/`finish` 的返回值，头部名已小写、值为 str、报文体为 bytes）：

- 请求：`{"method": str, "target": str, "path": str, "query": str|nil, "version": "1.1", "headers": dict, "body": bytes}`；`path`/`query` 由绑定层在 `target` 首个 `?` 处拆分（无 `?` 时 `query` 为 nil）。
- 响应：`{"status": int, "reason": str, "version": "1.1", "headers": dict, "body": bytes}`。

**模块函数表**（`{NULL, NULL, NULL}` 结尾）：

```c
static const MsMethodDef msHttpMethods[] = {
  {"newParser",     msHttpNewParser,     "newParser(kind) -> HttpParser"},
  {"parseUrl",      msHttpParseUrlFn,    "parseUrl(url) -> dict"},
  {"buildRequest",  msHttpBuildRequest,  "buildRequest(method, target, headers, body) -> bytes"},
  {"buildResponse", msHttpBuildResponse, "buildResponse(status, headers, body) -> bytes"},
  {"statusText",    msHttpStatusTextFn,  "statusText(status) -> str"},
  {NULL, NULL, NULL},
};

static const MsModuleDef msHttpModule = {
  "_http", "HTTP/1.1 wire-format engine (internal; use net/http)", msHttpMethods,
};
```

- `parseUrl(url)`：`url` 必须是 str；成功返回 `{"scheme": str, "host": str, "port": int, "path": str, "query": str|nil}`；失败抛 ValueError。
- `buildRequest(method, target, headers, body)`：`method`/`target` 为 str；`headers` 为 dict（str→str）或 nil；`body` 为 str（按 UTF-8 字节）或 bytes 或 nil。dict 先转为临时 `struct MsHttpHeader` 数组（`msAlloc`，调用后 `msFree`，键不做大小写归一——序列化器原样写出，头部名规范由脚本层负责）。返回 bytes。
- `buildResponse(status, headers, body)`：`status` 为 int 且 `100 <= status <= 999`，否则 ValueError；其余同上。
- `statusText(status)`：静态表查询，未知码返回 `"Unknown"`。
- GC 纪律：报文 dict 的构造顺序为「引擎报文 → 逐项装箱 str/bytes/dict → `msHttpMessageFree`」；构造期间半成品 dict `msRootPush`/`msRootPop` 保护；`msAsCString` 的指针窗口内不调用任何会分配的 C API（先把各段长度取出，再逐段装箱）。
- 注册流程：`msDefineType` 建 HttpParser 类型（不导出类型对象，脚本只经 `newParser` 拿实例）→ `msRegisterModule(L, &msHttpModule)`。

### 6. 脚本层公开 API（lib/net/http.ms）

文件头注释一句话说明职责；import 组：`net`、`_http`、`encoding/json`、`strings`（大小写归一用）。无模块级可变状态。全部公开函数/类带英文前置文档注释，内部函数 `_` 前缀（12-ms-style）。

**Response 与 Request 类**（脚本 class）：

```ms
// Response is an HTTP response: status (int), headers (dict, lowercase
// names), body (str decoded per Content-Type).
class Response {
    func __init__(self, status, headers, body) { ... }

    // json parses body as JSON; shorthand for json.loads(self.body).
    func json(self) {
        return json.loads(self.body)
    }
}

// Request is a server-side HTTP request: method, path, query (str|nil),
// headers (dict, lowercase names), body (str).
class Request {
    func __init__(self, method, path, query, headers, body) { ... }
}
```

**客户端**：

```ms
// request performs one HTTP/1.1 request over a fresh connection and returns
// a Response. Raises ValueError for a bad URL or unsupported scheme,
// OSError (or its TimeoutError subclass) for transport failures.
func request(method, url, body = nil, headers = nil, timeout = 30)

// get/post are convenience wrappers of request.
func get(url, headers = nil, timeout = 30)
func post(url, body, headers = nil)
```

`request` 流程（`_pump` 为公共泵循环，见第 7 节）：

1. `u := _http.parseUrl(url)`（https 等非法即在此抛 ValueError）。
2. `conn := net.dial("tcp", f"{u["host"]}:{u["port"]}")`；`conn.setDeadline(timeout)`。
3. 组装头部 dict：先置 `"Host"`（含非默认端口）、`"Connection": "close"`，再并入调用者 `headers`（调用者可覆盖 `Host`，不可覆盖 `Connection`——后者在并入后再强制写回 `"close"`，保证 EOF 定界语义成立）。
4. `wire := _http.buildRequest(method, target, hdrs, body)`，`conn.write(wire)`（target 为 `path` + 可选 `"?" + query`）。
5. `msg := _pump(conn, _http.newParser("response"))`；`conn.close()`。
6. 报文 dict → Response：`body` bytes 经 `_decodeBody(headers, rawBody)` 转 str。

**`http.response`（服务器侧构造器）**：

```ms
// response builds a Response value for handler returns; body may be str,
// bytes, or dict/list (auto JSON with Content-Type application/json).
func response(status, body, headers = nil)
```

**服务器**：

```ms
// server binds addr (e.g. ":8080", "127.0.0.1:0") and returns a Server
// whose addr attribute holds the actual bound address. The handler is
// called once per request with a Request and may return a Response
// (http.response), a str (200, text/plain), or a dict/list (200,
// application/json). A raised handler produces 500 and closes the
// connection.
class Server {
    // serve accepts connections until close(), one coroutine per
    // connection. Raises OSError when the listener fails.
    func serve(self) { ... }

    // close stops the listener; serve() then returns nil.
    func close(self) { ... }
}

// server creates a Server (the listener is bound eagerly, so bind errors
// raise here, not in serve()).
func server(addr, handler)
```

`_serveConn`（模块级 `async func`，每连接一个协程）流程：

1. `parser := _http.newParser("request")`；`try { ... } finally { conn.close() }` 保证关闭。
2. 循环：`msg := _pump(conn, parser)`；`msg is nil`（对端在报文边界前关闭）→ 退出循环。
3. 报文 dict → Request（body 按同一 `_decodeBody` 规则转 str）；调用 `handler(req)`。
4. 返回值归一（`_coerceResponse`）：Response → 原样；str → `response(200, v, {"Content-Type": "text/plain; charset=utf-8"})`；dict/list → `response(200, json.dumps(v), {"Content-Type": "application/json"})`；其他类型 → `raise TypeError`（视为 handler 错误，走 500 路径）。
5. handler 抛任何异常 → 捕获并替换为 `response(500, "Internal Server Error")`，响应后关闭连接退出（异常吞掉是有意的：单连接 handler 错误不得击垮服务器；此处置注释说明理由，符合 12-ms-style §5.4）。
6. `conn.write(_http.buildResponse(resp.status, resp.headers, respBodyBytes))`；请求头 `connection: close`（大小写归一后比较）或解析器粘滞错误 → 写完退出，否则回到第 2 步处理同连接下一请求（keep-alive）。

`serve()` 流程：

```ms
func serve(self) {
    for {
        conn := self._listener.accept()
        handle := _serveConn(self._handler, conn)
        // The coroutine handle is intentionally dropped: the per-connection
        // coroutine ends by itself when the connection closes.
    }
}
```

`accept` 在 `close()` 后抛 `OSError`（任务 62 假定语义），`serve` 捕获该特定路径返回 nil，其余错误原样传播。

**服务器启动与调度器的关系**：`srv.serve()` 是协程感知的永久循环，调用方协程挂起在 `accept` 上。测试与典型用法把 `serve()` 放在顶层直接调用（主协程挂起、accept 循环常驻），或先派生客户端协程再调 `serve()`；主协程结束时任务 43 的调度器直接返回，遗留的服务器协程不阻塞进程退出。

### 7. 泵循环与协程整合

```ms
// _pump reads from conn until parser yields one complete message, feeding
// every chunk. Returns the message dict, or nil on a clean EOF at a message
// boundary. conn.read is coroutine-aware (task 62): the coroutine suspends
// here instead of blocking the OS thread.
func _pump(conn, parser) {
    for {
        msg := parser.next()
        if msg is not nil {
            return msg
        }
        chunk := conn.read(65536)
        if len(chunk) == 0 {
            return parser.finish()
        }
        parser.feed(chunk)
    }
}
```

- EOF 判定用 §7 io 的「空数据即 EOF」哨兵；`finish()` 处理 EOF 定界响应体与「边界前干净关闭」（返回 nil）。
- 协程整合的全部要点即此：`net.dial`、`conn.read`、`conn.write`、`listener.accept` 的挂起由任务 62 实现，本层只是顺序调用；服务器并发度来自 `_serveConn` 的 `async` 派生，客户端并发来自调用方自行 `async` 包装 `http.get`（06-concurrency §1 的示例即此形态）。本模块自身不创建 channel、不使用 select。
- 超时：客户端经 `conn.setDeadline(timeout)` 一次性设置（假定语义：每次阻塞操作独立计时的相对超时，到期抛 `TimeoutError`）；服务器侧首版不设读写超时（慢连接防护列入路线图）。

### 8. 语义与边界约定（规范歧义的显式裁定）

- **`http.request` 的返回值**：§17 示例把结果赋给 `req`，但规范全篇没有任何消费「Request 对象」的客户端 API（无 `send(req)` 之类）。裁定为：与 Python `requests.request` 一致，`http.request` **执行请求并返回 Response**，是 `get`/`post` 的底层通用形式；示例中的变量名视为笔误。若后续规范补充请求对象语义，本裁定需回看。
- **`http.post` 无 `timeout` 参数**：按 §17 签名原样保留；内部委托 `request(..., timeout = 30)`（默认超时仍生效）。
- **`srv.close()` 与 `srv.addr`**：§17 未列。`close()` 是监听器资源释放与测试有序拆卸的最小补充；`addr` 属性是回环测试可取实际端口（`":0"` 临时端口）的必要补充。两者均为规范沉默处的最小扩展，在此显式注明。
- **头部名大小写**：解析侧一律归一小写（dict 键小写，查找方无需关心对端写法）；序列化侧按 dict 键原样写出。同一报文中重复头部名按「后值覆盖前值」处理——`Set-Cookie` 多值语义因此不支持，列入路线图。
- **body 解码（`resp.body` 恒为 str 的落实）**：取 `Content-Type` 的 `charset` 参数（大小写不敏感）；`utf-8` 或缺省 → UTF-8 解码（bytes `toString` 语义），非法 UTF-8 抛 `UnicodeError`；其他 charset → `ValueError("http: unsupported charset ...")`（首版只有 UTF-8，二进制体场景列入路线图，届时再评估暴露 bytes 的 API）。
- **错误分类**：本地参数/URL/构造错误 → `TypeError`/`ValueError`；对端协议违规（畸形报文、定长体未收齐、不支持的 Transfer-Encoding）→ `OSError`（消息前缀 `http:`）——这是对端造成的传输失败而非本地值错误；传输与超时错误沿用 net 层抛出的 `OSError`/`TimeoutError`，不包装。
- **Connection 语义**：客户端恒发 `Connection: close`，一请求一连接（无连接池，路线图）；服务器解析 `connection` 头决定是否 keep-alive，对自产客户端自然一请求一连接，对第三方客户端可同连接多请求。
- **解析器粘滞错误**：脚本层在 `_pump` 抛出后不再复用该连接（finally 关闭），与第 3 节一致。

### 9. 内存与 GC 纪律清单

- 引擎层分配：解析器内部累积缓冲（`msHttpParserFree` 释放）、`MsHttpMessage` 及其全部字段（`msHttpMessageFree` 统一释放，所有权随 `MS_HTTP_PARSE_OK` 移交调用者）、序列化输出缓冲（调用者 `msFree`）、`struct MsHttpUrl` 字段（`msHttpUrlClear`）。失败路径按获取逆序释放。
- 绑定层：headers dict ↔ `MsHttpHeader` 数组的临时转换缓冲在函数内成对 `msAlloc`/`msFree`；报文 dict 构造期间半成品容器入根；HttpParser 数据区无脚本对象，finalize 只调 `msHttpParserFree`、不调任何脚本 API。
- 脚本层无 C 侧纪律问题；连接对象的关闭路径由 `_serveConn` 的 `finally` 与客户端的显式 `conn.close()` 覆盖（含异常路径）。

## 实现步骤

1. 建 `src/net/ms_http.h` / `ms_http.c` 骨架：常量宏、`MsHttpKind`/`MsHttpParseStatus` 枚举、`struct MsHttpHeader`/`struct MsHttpMessage`/`struct MsHttpUrl`、全部函数声明；接入 CMake（并入 `mslang` 库目标）。验证：全平台编译链接通过。
2. 实现 URL 解析器与 `msHttpStatusText`。验证：经步骤 6 绑定后以脚本断言正例（全字段/缺省端口/缺省 path/query 有无）与负例（https、空 host、userinfo、IPv6、端口越界、含 `#`）。
3. 实现解析器头部期：累积缓冲、`"\r\n\r\n"` 查找、请求行/状态行解析、头部行解析与小写归一、全部上限与格式错误路径、定界判定（Content-Length/EOF/空体/拒绝 Transfer-Encoding）。验证：脚本侧逐字节 feed 的增量解析断言（见测试方案）。
4. 实现报文体期与 `finish`：定长体消费、EOF 定界、缓冲余量前移（一缓冲多报文）、粘滞错误。验证：keep-alive 双报文、EOF 定界、定长不足 EOF 报错的脚本断言。
5. 实现 `msHttpWriteRequest`/`msHttpWriteResponse`：start line、头部遍历、Content-Length 计算与覆盖调用者同名字段、CRLF 终止。验证：序列化输出经解析器回读（round-trip）字段一致。
6. 建 `stdlib/net/ms_std_http.{c,h}`：HttpParser 类型（init/finalize/feed/next/finish）、五个模块函数、`msStdHttpRegister`；接入 stdlib 启动注册链。验证：`import "_http"` 成功，五个名字与 Parser 方法可达；`_http.newParser("request")` 冒烟。
7. 建 `lib/net/http.ms` 骨架与 `_pump`/`_decodeBody`/`_coerceResponse` 内部函数；实现 `response` 构造器与 Response/Request 类。验证：导入成功；`_decodeBody` 的 charset 分支与 `_coerceResponse` 的四类返回值经脚本断言。
8. 实现客户端 `request`/`get`/`post`。验证：对步骤 9 的服务器完成 get/post 回环；https URL 抛 ValueError；连接拒绝端口抛 OSError。
9. 实现服务器 `server`/`Server.serve`/`Server.close` 与 `_serveConn`。验证：回环服务 str/dict/Response 三种返回形态、keep-alive 双请求、handler 抛错回 500 且服务器存活。
10. 编写 `tests/ms/stdlib/net_http/` 全部测试（见测试方案），`python run_tests.py` 全绿；Win/Linux/macOS × Debug/Release 构建，Debug（ASAN / `/RTC`）下无内存错误，配合任务 02 分配统计确认无泄漏（含解析错误路径与 GC 压力下的 HttpParser 回收）。

## 测试方案

本任务晚于任务 40（testing 模块），一律用 ms 脚本测试（`import "testing"` / `import "testing/assert"`，测试函数 `test` 前缀，末尾 `testing.run()`），由仓库根 `run_tests.py` 驱动 mslang CLI 执行。全部网络用例只走本地回环（`127.0.0.1` + `":0"` 临时端口），不访问外部网络，保证 CI 离线可运行；服务器在测试函数内启动、用毕 `close()`（遗留协程随主协程结束由调度器回收，双重保障）。时间相关断言一律用容忍区间，超时用例的超时值取 1 秒量级、睡眠值取其二倍，避免 CI 抖动误判。本任务只交付本设计文档，测试脚本随实现编写。

测试文件清单与覆盖点：

- `tests/ms/stdlib/net_http/http_parse_url_test.ms`：`parseUrl` 正例——完整 URL、缺省端口 80、缺省 path 归一 `/`、query 有无与空 query、host 大小写保留；负例（`assert.raises(ValueError, ...)`）——`https://`、无 scheme、空 host、`user@host`、`http://[::1]/`、端口非数字/越界/为 0、含 `#` 片段。
- `tests/ms/stdlib/net_http/http_parser_test.ms`（直接驱动内部模块 `_http`，属有意的白盒覆盖）：
  - 请求解析：最小 GET（无体）、POST + Content-Length 体、头部小写归一与 OWS 剥离、`path`/`query` 拆分、`version` 字段。
  - 响应解析：状态行（含空 reason）、定长体、1xx/204/304 空体、无 Content-Length 时 `finish()` 的 EOF 定界体。
  - 增量性：逐字节 feed 后 `next()` 先持续返回 nil、末字节后产出完整报文；一次 feed 含两个完整请求时两次 `next()` 依次产出（keep-alive 解析）。
  - 负例（ValueError）：畸形请求行/状态行、无冒号头部行、obs-fold 续行、头部块超 64 KiB、字段数超 200、`Content-Length` 非数字/两值不一致/超 16 MiB、任何 `Transfer-Encoding`、定长体未收齐时 `finish()`；粘滞性——报错后再次 `next()` 仍报错。
  - `finish()` 在无待完成报文时返回 nil；`feed` 非 str/bytes 抛 TypeError。
- `tests/ms/stdlib/net_http/http_build_test.ms`：`buildRequest`/`buildResponse` 输出经解析器 round-trip 一致（方法/target/状态码/头部/体逐字段）；调用者给 `Content-Length` 头被计算值覆盖；body 为 str（UTF-8 多字节）与 bytes 的字节级一致；`buildResponse(99, ...)` 抛 ValueError；`statusText(200) == "OK"`、未知码返回 `"Unknown"`。
- `tests/ms/stdlib/net_http/http_server_test.ms`：回环服务器（`127.0.0.1:0`，经 `srv.addr` 取端口）——handler 返回 str（200 + `text/plain` 头）、dict（200 + `application/json` 且体为合法 JSON）、`http.response(201, ...)` 自定义状态与自定义头；Request 字段断言（method/path/query/headers 小写键/body）；同连接 keep-alive 连发两请求（直接用任务 62 的 conn 手工组报文）；handler 抛异常 → 客户端收到 500、服务器随后请求仍正常；`srv.close()` 后 `serve()` 返回。
- `tests/ms/stdlib/net_http/http_client_test.ms`：对回环服务器的 `http.get`/`http.post` 往返（status/headers/body 三字段）；`resp.json()` 对 dict 响应；调用者 headers 透传与 `Host` 自动设置；`timeout` 用例——handler `time.sleep` 长于客户端 timeout，客户端抛 `TimeoutError`（假定 net 层语义，若任务 62 定名不同以实现时对齐为准）；连接未监听端口抛 `OSError`；`http.request("DELETE", ...)` 的通用形式；`https://` URL 抛 ValueError。
- `tests/ms/stdlib/net_http/http_concurrent_test.ms`：16 个客户端协程（`async` 派生 + `await` 收集）并发 get 同一回环服务器（handler 内 `time.sleep(0.05)` 制造挂起窗口），全部成功且响应体含按请求区分的回显标记——验证「每连接一个协程」真正并发而非串行（总耗时显著小于串行下界的宽松断言，如 < 串行耗时的 1/2）。

## 验收标准

- [ ] `src/net/ms_http.{c,h}`（guard `MSLANG_SRC_NET_MS_HTTP_H_`）、`stdlib/net/ms_std_http.{c,h}`（guard `MSLANG_STDLIB_NET_MS_STD_HTTP_H_`）、`lib/net/http.ms` 存在；头文件自包含；C 代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc/msRealloc/msFree`、无平台条件编译）；ms 代码通过 12-ms-style 检查（4 空格缩进、内部函数 `_` 前缀、公开函数英文前置文档注释）。
- [ ] 模块分层落实：C 内建模块注册名为 `"_http"`（HttpParser 类型 + `newParser`/`parseUrl`/`buildRequest`/`buildResponse`/`statusText`），公开模块 `net/http` 为 `lib/net/http.ms` 纯脚本层；`import "net/http"` 命中脚本层、`import "_http"` 命中 C 层，互不遮蔽。
- [ ] 引擎层为纯缓冲解析/序列化，无任何 socket 调用与 `MsObject` 依赖；解析器为增量状态机，支持一缓冲多报文与逐字节 feed；上限（首行 8 KiB、头部 64 KiB、字段 200、体 16 MiB）与错误粘滞语义与本文一致。
- [ ] 报文定界只支持 Content-Length 与 EOF 两种；任何 `Transfer-Encoding` 报协议错误；响应 1xx/204/304 无体；`Content-Length` 重复不一致报错。
- [ ] 脚本 API 完整覆盖 §17：`http.get`/`http.post`/`http.request` 返回 Response（`status`/`headers`/`body` str/`json()`）；`http.response` 与 handler 的 str/dict/Response 三种返回形态；`http.server` + `serve()` 每连接一协程；扩展项 `srv.close()` 与 `srv.addr` 按本文裁定实现。
- [ ] 协程感知落实：全部 IO 经任务 62 的 `conn`/`listener` 原语，泵循环内无阻塞 OS 线程的调用；并发测试验证多连接真并发。
- [ ] 错误分类与本文第 8 节一致：URL/参数错误 ValueError/TypeError、对端协议错误 OSError、超时 TimeoutError、非法 UTF-8 体 UnicodeError、非 UTF-8 charset ValueError；`https://` 拒绝。
- [ ] 客户端恒发 `Connection: close` 且不可被调用者 headers 覆盖；`Host` 自动设置可覆盖；服务器按 `connection` 头实现 keep-alive；handler 异常回 500 且连接关闭、服务器存活。
- [ ] 绑定层 GC 纪律：报文 dict 构造期间半成品入根、HttpParser 数据区无脚本对象、finalize 只释放引擎资源；ASAN/分配统计下无泄漏与悬垂。
- [ ] `tests/ms/stdlib/net_http/` 六个测试文件覆盖「测试方案」全部清单项，`python run_tests.py` 全绿；测试只走回环、不依赖外部网络；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 62（`net.dial`/`net.listen`/`conn.*`/`listener.*`/`TimeoutError`）与任务 18（stdlib 启动注册链）的接口假定在实现时已按对应任务文档对齐定名。
