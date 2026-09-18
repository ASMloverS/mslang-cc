# 62 标准库：net

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [42 平台抽象层](42-platform-layer.md) |

## 任务目标

交付 C 内建标准库模块 `net`（`stdlib/net/ms_net.h` / `stdlib/net/ms_net.c` / `stdlib/net/ms_net_module.c`），实现 `docs/language/07-stdlib.md` §16 规定的脚本接口并补足其 UDP 侧面（§0 模块表标注 net 为「TCP/UDP（协程感知）」）：

- **TCP**：`net.dial("tcp", address)` 建立连接得 Conn（`read` / `write` / `close` / `setDeadline` / `localAddr` / `remoteAddr`）；`net.listen("tcp", address)` 得 Listener（`accept` / `close` / `addr`）。
- **UDP**：`net.dial("udp", address)` 得已连接 UdpConn；`net.listenUdp(address)` 得绑定本地地址的 UdpConn，具数据报方法 `readFrom` / `writeTo`（07-stdlib §16 原文只列 TCP 形态，UDP 服务端入口为本文档对 §0「TCP/UDP」标注的补足，规则见「详细设计」第 1 节）。
- **协程感知**：`dial` / `accept` / `read` / `write` / `readFrom` / `writeTo` 在不能立即完成时**挂起当前协程并让出调度权**，不空转、不长期占用工作线程（06-concurrency §4 让出点清单、07-stdlib §25 协程感知 API 标注）。
- **平台层复用与扩展**：socket 操作全部经任务 42 的 `msSocket*` 接口；UDP 原语与对端地址查询是任务 42 明确划出 v0.3 范围的部分，由本任务按其既定约定补入平台层（接口名沿用其命名风格，见「详细设计」第 3 节）。

Conn 满足任务 37 既定的 io.Reader/Writer 鸭子协议核心（`read` / `write` / `flush` / `close`），可被 `io.copy` 直接消费；net/http（任务 63）以 Conn 为传输底座。本任务通过 `tests/ms/net/` 下的 ms 脚本测试（testing 模块，全部用例仅走 127.0.0.1 本地回环）独立验证。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0：net 为 C 内建模块（置于 `stdlib/`），职责「TCP/UDP（协程感知）」；函数与方法命名小驼峰。
  - §16：`net.dial("tcp", "example.com:80")`、`net.listen("tcp", ":8080")`、`conn.read(n)` / `conn.write(data)` / `conn.close()` / `conn.setDeadline(seconds)`、`listener.accept()`——本任务脚本 API 的唯一来源；UDP 服务端入口在该节缺位，本文档以最小补足处理（见第 1 节决策）。
  - §25：`net.*` 全部属协程感知 API——「阻塞时挂起协程而非 OS 线程」。
- `docs/language/06-concurrency.md`
  - §4：net 模块中标注协程感知的阻塞式 IO 是让出点；阻塞在 OS 调用上的操作占用工作线程，调度器检测到线程耗尽时临时扩容——这是本任务对 DNS 解析保持阻塞语义的规范依据（见第 5 节）。
  - §6：协程让出点是 GC safepoint，挂起中的协程栈是 GC 根——等待循环不得在裸 C 指针上持有脚本对象跨越让出点（见第 9 节）。
- `docs/language/04-exceptions.md` §4：异常层级中 `OSError`（含子类 `TimeoutError`）是 IO 失败的既定类型；net 模块不新增异常类。
- `docs/language/09-c-api.md` §3（GC 根栈纪律）、§5（`msNewStringN` / `msNewBytes` / `msAsCString` / `msStringLen`）、§8（`msRaiseTypeError` / `msRaiseValueError` / `msRaiseOSError` 等错误接口，C 函数出错置错误槽并返回 `NULL`）、§9（`MsModuleDef` / `MsMethodDef` / `MsCFunction` 与 `msRegisterModule`）、§10（`MsTypeDef` / `msDefineType` / `msCInstanceData`）。
- `docs/language/10-c-style.md`：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R 括号、指针星号贴类型、`ms`/`Ms`/`MS_` 命名、内部结构体不 typedef、include guard 按相对路径大写蛇形、禁止非 const 可变全局变量、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）；§9：平台相关代码只允许出现在 `src/platform/`——本任务的 UDP 增补因此落在平台层两后端，而非 net 模块内。
- `docs/language/11-project-layout.md` §1（`stdlib/` 目录）、§4（脚本测试经仓库根 `run_tests.py` 驱动 mslang CLI）。
- `docs/tasks/README.md` 测试约定：本任务晚于任务 40，测试一律用 ms 脚本 + testing 模块（`import "testing"` / `import "testing/assert"`，测试函数以 `test` 开头，末尾 `testing.run()`）。
- 任务 42（平台抽象层）提供既定 socket 接口：`struct MsSocket`、`MsSocketStatus`（`MS_SOCKET_STATUS_OK` / `MS_SOCKET_STATUS_ERROR` / `MS_SOCKET_STATUS_WOULD_BLOCK`）、`MS_SOCKET_POLL_READABLE` / `MS_SOCKET_POLL_WRITABLE`、`msSocketGlobalInit` / `msSocketGlobalCleanup`、`msSocketTcpOpen` / `msSocketSetNonBlocking` / `msSocketBind` / `msSocketListen` / `msSocketAccept` / `msSocketConnect` / `msSocketSend` / `msSocketRecv` / `msSocketPoll` / `msSocketLocalAddr` / `msSocketClose` / `msSocketLastErrorMessage`，及单调时钟 `msClockMonotonicNs`。任务 42 明确「UDP 与 Unix domain socket 不在 v0.3 范围」，并把「协程感知 DNS」列入本任务考虑范围——两者都在本文档第 3、5 节落定。
- 任务 43（协程）提供：`struct MsCoroutine`、`msSchedEnqueue`、`msSchedRun`、协程让出/恢复机制。任务 46（M:N 调度器，文档尚不存在）将提供 netpoller；本任务对调度侧唯一的新假定是 C 可调的协作让出原语 `msSchedYieldCurrent`（语义见第 5 节），**该接口名为假定命名，实现时以任务 46 文档定名为准**；若任务 46 提供真正的 netpoller 挂起点，本任务的 `msNetWaitReady` 整体换芯、net 模块其余代码不变。
- 任务 37（io）既定约定：Reader/Writer 鸭子协议方法集；「`read` 返回空数据即 EOF」；对已关闭对象再操作抛 `ValueError`；`msRaiseOSError` 假定名。任务 32 提供 `msNewBytes(MsState* L, const uint8_t* data, size_t len)` 既定签名。任务 33 提供 `msDefineType` / `msCInstanceData` 及其硬性限制：**C 类型数据区不得持有脚本对象**——Conn/Listener/UdpConn 的数据区只放 C 资源（socket、deadline、地址字符串副本），地址串经 `msAlloc` 复制、finalize 时 `msFree`。**以上假定接口名实现时以对应任务文档定名为准。**

## 详细设计

### 1. 范围与脚本 API 总览

07-stdlib §16 的原文接口逐条落地；§16 未覆盖而 §0 标注要求的 UDP 以最小面补足。脚本可见 API 全集（除此表外不新增任何模块函数或方法）：

```ms
import "net"

// TCP 客户端 / 服务端（07-stdlib §16 原文）
conn := net.dial("tcp", "127.0.0.1:8080")       // 协程感知；失败抛 OSError
n := conn.write(b"ping")                         // 返回写入字节数；str 参数按 UTF-8 发送
data := conn.read(1024)                          // 至多 1024 字节的 bytes；对端有序关闭返回空 bytes（EOF）
conn.setDeadline(2.5)                            // 后续阻塞操作 2.5 秒超时，超时抛 TimeoutError
conn.localAddr()  conn.remoteAddr()              // "ip:port" 形式的 str
conn.close()

listener := net.listen("tcp", ":8080")           // ":port" = 通配地址
conn := listener.accept()                        // 协程感知
listener.addr()  listener.close()

// UDP（本文档对 §0「TCP/UDP」标注的最小补足）
u := net.dial("udp", "127.0.0.1:5353")           // 已连接数据报 socket：read/write 可用
srv := net.listenUdp("127.0.0.1:5353")           // 绑定本地地址的数据报 socket
data, from := srv.readFrom(2048)                 // → (bytes, "ip:port")
srv.writeTo(b"pong", from)
```

明确不做（不在本任务范围）：Unix domain socket、TLS、`net/http`（任务 63）、`tcp4`/`tcp6`/`udp4`/`udp6` 等地址族限定网络名（任务 42 的 `msSocketTcpOpen` 把 AF_INET/AF_INET6 的抉择放在 bind/connect 内部，网络名无需分族）、异步 DNS（第 5 节说明）、原始 socket、多播选项。

`network` 参数合法值仅 `"tcp"`（`dial` / `listen`）与 `"udp"`（`dial` / `listenUdp`）；其余值抛 `ValueError`。`listen("udp", ...)` 属错用，抛 `ValueError` 并在消息中指向 `net.listenUdp`。

### 2. 文件布局与分层

```
stdlib/net/
├── ms_net.h          # 引擎层公开头：地址解析、等待原语、对象内部结构声明；guard MSLANG_STDLIB_NET_MS_NET_H_
├── ms_net.c          # 引擎层：地址解析、协程感知等待循环、读写/连接/接受的非阻塞操作包装
└── ms_net_module.c   # 绑定层：Conn/Listener/UdpConn 三个 C 自定义类型、模块函数表、注册入口
```

分层职责（对齐任务 61 的引擎/绑定分层原则）：

- **引擎层**（`ms_net.c`）：输入 socket 与字节切片，完成「非阻塞尝试 → 不可立即完成则等待 → 重试」的核心流程；不触碰 `MsTypeDef`/方法表。对 `MsState` 的唯一依赖是等待让出（`msSchedYieldCurrent`）与错误消息缓冲。
- **绑定层**（`ms_net_module.c`）：参数校验、bytes/str 装箱、异常抛出、类型注册。全部函数 `static`，唯一导出符号是注册入口 `msNetModuleRegister`。

### 3. 平台 socket 层扩展（任务 42 增补）

任务 42 把 UDP 与对端地址查询划出其 v0.3 范围；net 模块需要的最小增补作为本任务的一部分落进 `src/platform/ms_platform.h` 与两后端，**完全沿用任务 42 的约定**（零堆分配、`MsSocketStatus` 返回、`errno`/`WSAGetLastError` 不出层、线程本地错误消息）：

```c
// Datagram socket; AF_INET/AF_INET6 decided at bind/connect, same as TCP.
MsSocketStatus msSocketUdpOpen(struct MsSocket* outSock);

// Sends one datagram to host:port (host resolved internally, per task 42).
// *outSent receives the datagram size on success; partial datagram writes do
// not exist at this layer (sendto is all-or-nothing).
MsSocketStatus msSocketSendTo(struct MsSocket* sock, const void* data, size_t len,
    const char* host, uint16_t port, size_t* outSent);

// Receives one datagram; the sender address is written to hostBuf/outPort
// (hostBuf may be NULL when only the payload is wanted). No data on a
// non-blocking socket returns MS_SOCKET_STATUS_WOULD_BLOCK.
MsSocketStatus msSocketRecvFrom(struct MsSocket* sock, void* buf, size_t len, size_t* outRead,
    char* hostBuf, size_t hostBufLen, uint16_t* outPort);

// Peer address of a connected socket (getpeername); backs Conn.remoteAddr
// for accepted connections. hostBuf may be NULL when only the port is wanted.
MsSocketStatus msSocketRemoteAddr(const struct MsSocket* sock, char* hostBuf, size_t hostBufLen,
    uint16_t* outPort);
```

另对任务 42 已定接口补充一条语义对齐要求（实现任务 42 时若未覆盖，由本任务在两后端补齐，语义不变更）：**非阻塞 `connect` 的可重入归一**——`msSocketConnect` 在连接进行中再次被调用时，POSIX 的 `EALREADY`/`EINPROGRESS` 归一为 `MS_SOCKET_STATUS_WOULD_BLOCK`，`EISCONN` 归一为 `MS_SOCKET_STATUS_OK`，Win32 对应 `WSAEALREADY`/`WSAEINPROGRESS`/`WSAEISCONN` 同理。这是第 5 节等待-重试循环的正确性前提。

### 4. 地址解析（"host:port"）

脚本地址是单一字符串（Go 风格），平台层接口要 host/port 分离，故引擎层提供解析器：

```c
// Splits "host:port", ":port" (wildcard host), or "[v6literal]:port" into a
// newly msAlloc'd host string (NULL for wildcard) and a numeric port.
// Port must be decimal 0..65535; a malformed address returns MS_ERROR_VALUE
// with a human-readable reason in errBuf. Caller frees *hostOut with msFree.
MsResult msNetSplitAddr(const char* addr, size_t addrLen, char** hostOut, uint16_t* portOut,
    char* errBuf, size_t errBufLen);
```

规则：以**最后一个** `:` 为 host/port 分界；`[...]` 包裹的 IPv6 字面量先剥括号再按上述规则分界（`[::1]:80`）；端口必须纯十进制且在 0–65535；host 为空串（`":8080"`）映射为 `NULL`（通配，对应任务 42 `msSocketBind` 的 `host NULL = wildcard`）。主机名的 DNS 解析不在此处做，交给平台层 `msSocketBind`/`msSocketConnect`/`msSocketSendTo` 内部的 `getaddrinfo`（任务 42 既定）。

地址格式化（`localAddr`/`remoteAddr`/`readFrom` 的第二返回值）统一为 `host:port`——IPv6 字面量加方括号（`[::1]:80`），经 `msNewStringN` 装箱。

### 5. 协程感知等待（核心设计）

**总原则**：net 模块创建的每个 socket 立即 `msSocketSetNonBlocking(sock, true)`；任何脚本级阻塞操作都是「尝试 → `WOULD_BLOCK` 则等待就绪 → 重试」的引擎层循环。等待不空转、不无限占用工作线程。

**等待原语**（引擎层，`static` 于 `ms_net.c`）：

```c
typedef enum {
  MS_NET_WAIT_READY,    // at least one requested event is ready
  MS_NET_WAIT_TIMEOUT,  // deadlineNs reached first
  MS_NET_WAIT_ERROR     // socket error; msSocketLastErrorMessage has details
} MsNetWaitResult;

// Waits until sock signals any of events (MS_SOCKET_POLL_* mask), or the
// absolute monotonic deadline deadlineNs (0 = wait forever). Between poll
// slices the CURRENT COROUTINE yields to the scheduler; other coroutines
// run while this one waits. Must be called from a coroutine context.
static MsNetWaitResult msNetWaitReady(MsState* L, struct MsSocket* sock, uint32_t events,
    int64_t deadlineNs);
```

循环结构：

1. 计算剩余等待预算：无 deadline 时取固定切片 `MS_NET_POLL_SLICE_MS`（10ms），否则取 `min(剩余, 10ms)`；剩余 ≤ 0 直接返回 `MS_NET_WAIT_TIMEOUT`。
2. `msSocketPoll(sock, events, sliceMs, &ready)`：就绪返回 `MS_NET_WAIT_READY`；错误返回 `MS_NET_WAIT_ERROR`。
3. 未就绪：`msSchedYieldCurrent(L)` 让出当前协程，恢复后回步骤 1。

**`msSchedYieldCurrent`（假定命名，实现时以任务 46 文档定名为准）**：C 可调用的协作让出——把当前协程重新入就绪队列尾（READY），然后重入调度循环运行其他就绪协程，直到当前协程再次被调度后本函数返回（调用者的 C 栈在等待期间保持存活）。单线程调度器（任务 43）上它是一个最小新增原语；M:N 调度器（任务 46）上它是工作线程的普通让出。这一「嵌套泵」形态使等待中的协程不占用调度资源以外的任何预算，且不需要 VM 为 C 内建函数引入可挂起帧机制。

- **死锁交互**：等待中的协程处于 RUNNING（泵内）而非 SUSPENDED，任务 43 的死锁检测（就绪耗尽且仍有存活协程）不会误判；极端情形「所有协程都在等网络」退化为 10ms 切片的轮询泵，行为正确、代价有界。
- **演进点**：任务 46 落地真正的 netpoller 后，`msNetWaitReady` 的步骤 3 可换成「把 fd 与 deadline 注册进 netpoller 并挂起协程」，C 栈随挂起释放；net 模块其余代码与脚本语义完全不变。
- **safepoint**：让出点即 GC safepoint（06-concurrency §6），等待循环跨越让出点时不得持有未入根的脚本对象指针（第 9 节）。

**deadline 语义**：`conn.setDeadline(seconds)` 把 deadline 记录为**调用时刻起算的绝对单调时间**（`msClockMonotonicNs() + seconds × 10⁹`，`seconds <= 0` 清除为 0 = 永不超时），作用于其后一切阻塞操作（read/write/accept/readFrom/writeTo；dial 由 `net.dial` 的可选第三参数 `timeout` 控制，见第 7 节）。等待超时统一抛 `TimeoutError`（04-exceptions §4 既定层级，OSError 子类，消息含接口名与秒数）。`write` 在超时前已写出的部分字节不回滚、不重发——超时异常携带已写入计数不可得，脚本层把超时连接视为不可继续（文档与测试锁定此语义：写超时后连接应由调用方关闭）。

**DNS 语义**（任务 42 留给本任务的考虑项，决策如下）：v0.4 保持任务 42 既定——主机名解析在平台层 `bind`/`connect`/`sendto` 内部经 `getaddrinfo` **同步完成**。后果显式承认：解析期间占用当前工作线程，M:N 模式下由调度器的线程耗尽临时扩容兜底（06-concurrency §4），单线程模式下短暂阻塞全部协程。异步 DNS（线程池或 `getaddrinfo_a`）列入路线图，不在 v0.4 范围。

### 6. Conn / Listener / UdpConn 类型与方法语义

三个 C 自定义类型经任务 33 的 `msDefineType` 注册，实例数据区（任务 33 限制：不得持有脚本对象）只放 C 资源：

```c
typedef enum {
  MS_NET_KIND_TCP,
  MS_NET_KIND_UDP
} MsNetKind;

struct MsNetConnData {           // backs Conn (TCP) and UdpConn (UDP)
  struct MsSocket sock;
  MsNetKind kind;
  bool connected;                // UDP: dial-connected (write allowed); TCP: always true
  bool closed;
  int64_t deadlineNs;            // monotonic; 0 = no deadline
  char* remote;                  // msAlloc'd "host:port" of the dial target, or NULL (accepted)
};

struct MsNetListenerData {       // backs Listener (TCP only)
  struct MsSocket sock;
  bool closed;
  int64_t deadlineNs;            // applies to accept()
};
```

**Conn（TCP）方法表**：

| 方法 | 语义 |
|---|---|
| `read(n=-1)` | `n > 0`：等待至多 n 字节，有多少返回多少（流语义，不凑满），返回 bytes；`n == -1`：循环读到 EOF（对端有序关闭）返回全部字节；`n == 0` 返回空 bytes；`n < -1` 抛 ValueError。EOF（`msSocketRecv` 返回 OK 且 `*outRead == 0`）返回**空 bytes**——对齐任务 37「`read` 返回空数据即 EOF」的协议约定。已关闭抛 ValueError，超时抛 TimeoutError，其余失败抛 OSError |
| `write(data)` | data 为 bytes 原样发送、为 str 按其 UTF-8 字节发送，其余类型抛 TypeError；循环 `msSocketSend` 直到全部写出（`WOULD_BLOCK` 经第 5 节等待），返回总字节数（int）；空 data 返回 0。失败/超时/已关闭同 `read` |
| `flush()` | 无操作，返回 nil——为满足 io.Writer 鸭子协议（任务 37 的 `write`/`flush`/`close` 三件套），使 `io.copy(dst=conn, ...)` 等协议消费方可用 |
| `setDeadline(seconds)` | seconds 为 int/float，其余类型抛 TypeError；语义见第 5 节；返回 nil |
| `localAddr()` / `remoteAddr()` | `"ip:port"` str。local 经 `msSocketLocalAddr`；remote 对已拨号连接返回缓存的 `remote` 副本，对 accept 所得连接经 `msSocketRemoteAddr` 惰性查询 |
| `close()` | 幂等：首次 `msSocketClose` 并置 `closed`，再次调用为 no-op；返回 nil。GC finalize 对未关闭句柄同样只 `msSocketClose`（脚本忘了 close 不漏 fd） |

**Listener 方法表**：`accept()`（等待 `MS_SOCKET_POLL_READABLE` 后 `msSocketAccept`；返回新 Conn，`deadlineNs` 初始为 0、`remote = NULL`）、`addr()`（`msSocketLocalAddr` 格式化，端口 0 拨号场景的实测端口来源）、`close()`（同 Conn 幂等语义）、`setDeadline(seconds)`（作用于 `accept`）。

**UdpConn 方法表**：`read(n=65535)`（已连接：单个数据报截断至 n 字节）、`readFrom(n=65535)`（→ `(bytes, "ip:port")`，未连接也可用）、`write(data)`（仅已连接，未连接抛 OSError「destination address required」）、`writeTo(data, address)`（地址经 `msNetSplitAddr`）、`setDeadline` / `localAddr` / `remoteAddr` / `close` / `flush`（语义同 Conn）。数据报语义：一次 `read`/`readFrom` 对应一个数据报，超出 n 的部分按 UDP 语义丢弃（不缓存、不拼接）；`write`/`writeTo` 的返回值恒等于数据报长度（sendto 全有或全无）。

通用约定：一切方法的参数个数/类型不符抛 TypeError；对已关闭对象调用除 `close`/`flush` 外任何方法抛 ValueError（对齐任务 37 的 File 约定）；`n` 超 int 范围或非 int 抛 TypeError。

### 7. 模块函数

```c
static const MsMethodDef netModuleMethods[] = {
  {"dial",      netDial,      "dial(network, address, timeout=0) -> Conn or UdpConn"},
  {"listen",    netListen,    "listen(network, address) -> Listener"},
  {"listenUdp", netListenUdp, "listenUdp(address) -> UdpConn"},
  {NULL, NULL, NULL},
};
```

- `net.dial(network, address, timeout=0)`：`network` 校验（`"tcp"`/`"udp"`，其余 ValueError）→ `msNetSplitAddr`（失败 ValueError）→ `msSocketTcpOpen`/`msSocketUdpOpen` + 立即非阻塞 → UDP 直接 `connect`（UDP connect 只记录对端，不发起 IO，失败仅限地址解析）→ TCP 走 `msSocketConnect` 的 `WOULD_BLOCK` 循环：`msNetWaitReady(WRITABLE)` 后**重调** `msSocketConnect`（第 3 节的可重入归一使 `EISCONN` 归一为 OK，进行中的连接错误在重调时以 `ERROR` 浮出）。`timeout > 0` 为连接专用 deadline（秒，相对调用时刻），超时抛 TimeoutError；`timeout == 0` 永不超时。成功建 Conn/UdpConn 对象（`remote` 存地址串副本）；任何失败路径先 `msSocketClose` 再抛 OSError/TimeoutError。
- `net.listen(network, address)`：`network != "tcp"` 抛 ValueError；`bind` + `msSocketListen(sock, MS_NET_LISTEN_BACKLOG)`（backlog 取 128，定值常量）；`SO_REUSEADDR` 不在任务 42 接口面内，v0.4 不设（测试一律用端口 0 规避 TIME_WAIT 冲突，见测试方案）。
- `net.listenUdp(address)`：`msSocketUdpOpen` + `msSocketBind`；返回未连接 UdpConn。
- 注册入口 `MsResult msNetModuleRegister(MsState* L)`：`msSocketGlobalInit()`（任务 42 的引用计数生命周期，可重复调用安全）→ `msDefineType` 注册三个类型（`"Conn"` / `"Listener"` / `"UdpConn"`，finalize 只关 socket、`msFree(remote)`，不分配、不抛错）→ `msRegisterModule(L, "net", ...)`。重复注册幂等。

### 8. 错误映射与异常类型

| 情形 | 异常 |
|---|---|
| `network` 非法、地址格式非法、端口越界、`n < -1`、对已关闭对象操作 | `ValueError` |
| 参数个数/类型不符（含 `write(123)`、`setDeadline("x")`） | `TypeError` |
| 任何阻塞操作超 deadline / dial 超 timeout | `TimeoutError`（OSError 子类） |
| 连接拒绝、对端复位、网络不可达、未连接 UDP 的 `write` 等 OS 层失败 | `OSError`，消息 = `"net.<op>: " + msSocketLastErrorMessage()` |

错误消息统一带接口名前缀（`net.dial` / `net.read` 等），与任务 37 的消息风格一致。引擎层 `MS_NET_WAIT_ERROR` 的转换只在绑定层出口做一次（`msRaiseOSError`），中间路径不置错误槽。

### 9. 内存与 GC 纪律

- 引擎层分配两处：`msNetSplitAddr` 的 host 副本（调用者 `msFree`）、绑定层 `MsNetConnData.remote`（finalize `msFree`）。失败路径按获取逆序释放。
- 读路径的接收缓冲是引擎层栈上定长数组（`MS_NET_READ_BUF_SIZE` = 64 KiB）配 `msNetWaitReady` 循环；装箱（`msNewBytes`）只发生在拿到字节之后、下一次等待之前——**`msNewBytes` 可能触发 GC，因此拿到缓冲内容后先入根/装箱，再进入下一轮让出**；跨让出点不持有任何裸 `MsObject*`。
- `read(-1)` 的累积经任务 20 的 `struct MsStrBuf`（字节模式使用，假定名以任务 20 定名为准），每块读到的 bytes 立即 `msStrBufPut`，中途让出点前缓冲对象入根。
- 类型数据区遵守任务 33「不得持有脚本对象」：地址串是 `msAlloc` 的 C 副本，非脚本 str 引用。
- finalize 纪律（三类型同）：只 `msSocketClose` + `msFree(remote)`，不访问脚本对象、不调 `msRaise*`、不分配。

## 实现步骤

1. 平台层增补：两后端实现 `msSocketUdpOpen` / `msSocketSendTo` / `msSocketRecvFrom` / `msSocketRemoteAddr` 与 `msSocketConnect` 的可重入归一（`EALREADY`/`EINPROGRESS` → `WOULD_BLOCK`、`EISCONN` → OK）。验证：任务 42 既有 `tests/c/test_platform.c` 全部通过；本任务落地后在脚本层覆盖新原语（平台层 C 单测可选补一条 UDP loopback 收发用例，风格同任务 42 的 echo 用例）。
2. 建 `stdlib/net/ms_net.h` 骨架（guard `MSLANG_STDLIB_NET_MS_NET_H_`）：`MsNetKind`、`struct MsNetConnData` / `struct MsNetListenerData`、`msNetSplitAddr` 声明、`msNetModuleRegister` 声明。验证：头文件自包含编译通过。
3. 实现 `msNetSplitAddr` 与地址格式化辅助（含 IPv6 方括号、端口边界、全部负例消息）。验证：随绑定层落地后经脚本断言（非法地址抛 ValueError）。
4. 实现 `msNetWaitReady`：poll 切片 + deadline 计算 + `msSchedYieldCurrent` 让出；若调度侧尚无该原语，在 `src/sched/` 按第 5 节语义以最小改动落地之（当前协程入就绪队尾 + 重入调度循环）。验证：任务 43 的全部既有协程测试回归通过。
5. 绑定层骨架：`msNetModuleRegister`、三个类型的 `msDefineType` 与 finalize、空方法表注册 `"net"` 模块，CMake 加入新源文件。验证：`import "net"` 冒烟脚本通过。
6. TCP 客户端路径：`net.dial`（含 timeout 参数、connect 可重入循环）与 Conn 的 `read` / `write` / `close` / `flush` / `setDeadline` / `localAddr` / `remoteAddr`。验证：回环 echo 脚本（listener 用固定高端口的临时方案）收发字节一致。
7. TCP 服务端路径：`net.listen` + Listener 的 `accept` / `addr` / `close` / `setDeadline`。验证：`listen("tcp", "127.0.0.1:0")` 后经 `addr()` 取实测端口、accept/echo 全双工。
8. UDP 路径：`net.dial("udp", ...)`、`net.listenUdp`、`read` / `readFrom` / `write` / `writeTo`。验证：回环数据报收发、`readFrom` 的对端地址可用于 `writeTo` 回包、未连接 `write` 抛 OSError。
9. 错误与边界：第 8 节错误映射全表、close 幂等、关闭后操作 ValueError、`read(-1)` 读到 EOF、写超时语义。验证：负向脚本用例全绿。
10. 编写 `tests/ms/net/` 五个测试脚本（见测试方案），接入 `run_tests.py` 发现机制；Win/Linux/macOS × Debug/Release 构建，Debug（ASAN / `/RTC`）下 net 测试无内存错误，`msCloseState` 后分配计数归零（含未 close 连接被 GC 回收的路径）。

## 测试方案

本任务晚于任务 40，测试一律用 ms 脚本 + testing 模块，由仓库根 `run_tests.py` 驱动 mslang CLI 执行。**全部网络用例只走 127.0.0.1 本地回环**，监听一律 `bind` 端口 0 后经 `addr()` 取实测端口，杜绝端口冲突与外部环境依赖；涉及时间的断言一律用容忍区间（下界略小于名义值、上界放宽一个数量级），避免 CI 抖动误判。测试文件清单（本任务只交付本设计文档，测试代码随实现编写）：

- `tests/ms/net/tcp_echo_test.ms`：`net.listen("tcp", "127.0.0.1:0")` 的 `addr()` 端口非 0；async 协程内 `accept` + echo 循环（`read` 到空 bytes 即退出）；`dial` 后 `write(b"...")` 返回全长、`read(n)` 字节级一致；str 直写按 UTF-8 字节往返；`localAddr()`/`remoteAddr()` 端口互指；客户端 `close()` 后服务端 `read` 得空 bytes（EOF）；`close()` 幂等。
- `tests/ms/net/tcp_deadline_test.ms`：已建立连接上对端不发数据，`setDeadline(0.2)` 后 `read(1)` 抛 `TimeoutError`（`assert.raises`）且耗时下界达标；`setDeadline(0)` 清除后阻塞读恢复可用（由另一协程补写数据后读到）；`accept` 无连接时同样超时；dial 一个监听中但不 accept 的回环端口 + `timeout=0.2` 的连接在 backlog 内立即成功（不构造依赖外部网络的不可达用例）。
- `tests/ms/net/udp_test.ms`：`listenUdp` + `dial("udp")` 数据报往返；`readFrom` 返回值是 `(bytes, "ip:port")` 且地址可直接喂给 `writeTo`；未连接 UdpConn 的 `write` 抛 OSError；单数据报语义——两个数据报分两次 `read` 各得其一、超长数据报截断到 `n`；`setDeadline` 超时抛 TimeoutError。
- `tests/ms/net/tcp_concurrent_test.ms`（协程感知核心证据）：服务端协程对两条连接分别 echo；客户端协程 A `read` 阻塞期间，协程 B 的 `write`/`read` 正常完成（经共享 list 记录事件顺序，断言 B 的完成事件先于 A 的）；十个客户端协程并发 echo 各 100 轮，汇总字节数精确。另验证协程内未捕获的 OSError（如对端 reset）经 `await` 重抛、类型保持。
- `tests/ms/net/net_errors_test.ms`：`dial("sctp", ...)` / `listen("udp", ...)` 抛 ValueError；地址负例（无端口、端口非数字、端口 65536、空地址）逐一 ValueError；`dial` 到未监听回环端口抛 OSError（连接拒绝）；`conn.write(123)`、`setDeadline("x")`、`read(-2)` 抛 TypeError/ValueError 按第 8 节表；关闭后的 `read`/`write`/`accept` 抛 ValueError、`close`/`flush` 不抛。
- 回归：任务 42 的 `tests/c/test_platform.c`、任务 43–48 的全部既有脚本测试不变通过。

## 验收标准

- [ ] `stdlib/net/ms_net.h` / `ms_net.c` / `ms_net_module.c` 存在，头文件 guard 为 `MSLANG_STDLIB_NET_MS_NET_H_` 且自包含；代码风格符合 10-c-style（2 空格缩进、120 列、K&R、星号贴类型、`struct MsNetConnData` / `struct MsNetListenerData` 不 typedef、无可变全局变量、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] 平台层增补（`msSocketUdpOpen`/`msSocketSendTo`/`msSocketRecvFrom`/`msSocketRemoteAddr` 与 `msSocketConnect` 可重入归一）落在 `src/platform/` 两后端，沿用任务 42 约定；`src/platform/` 之外无新增平台条件编译；任务 42 既有测试全部通过。
- [ ] 脚本 API 与「详细设计」第 1 节逐条一致：`net.dial` / `net.listen` / `net.listenUdp` 三函数与 Conn / Listener / UdpConn 的方法表完整，此外无新增模块函数或方法。
- [ ] 协程感知：一切阻塞操作经非阻塞 socket + `msNetWaitReady` 实现，等待期间当前协程让出、其他协程照常推进（`tcp_concurrent_test.ms` 的顺序断言通过）；单线程与 M:N 调度下行为一致；不产生死锁误报。
- [ ] `setDeadline` / dial `timeout` 语义与第 5 节一致：超时抛 `TimeoutError`、清除后恢复、写超时后连接交由调用方关闭。
- [ ] 错误映射与第 8 节全表一致；EOF 返回空 bytes（io.Reader 协议约定）；Conn 满足 `read`/`write`/`flush`/`close`，可被 `io.copy` 消费。
- [ ] UDP 单数据报语义：一报一读、超长截断、未连接 `write` 抛 OSError、`readFrom`/`writeTo` 地址往返可用。
- [ ] GC/finalize 纪律：类型数据区不持有脚本对象；未 `close` 的连接经 GC 回收时句柄不泄漏；Debug 构建（ASAN / `/RTC`）下 net 测试无内存错误，`msCloseState` 后分配计数归零。
- [ ] `tests/ms/net/` 五个测试脚本覆盖「测试方案」全部清单项且仅用本地回环，`python run_tests.py` 全部通过；Win/Linux/macOS × Debug/Release 构建通过；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；假定接口（`msSchedYieldCurrent`、`msRaiseOSError`、`msDefineType`/`msCInstanceData`、`struct MsStrBuf`）在实现时已与任务 46/37/33/20 的实际定名对齐；若任务 46 提供 netpoller，`msNetWaitReady` 已按第 5 节演进点换芯或留有等价适配。
