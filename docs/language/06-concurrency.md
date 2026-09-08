# 06 并发模型

> Go 的协程 + channel 语义，`async`/`await` 关键字表达；channel 操作为方法调用（`send`/`recv`）。底层是 M:N 调度器：M 个协程调度到 N 个 OS 线程。

## 1. async 函数与协程句柄

```go
async func fetch(url) {
    resp := http.get(url)
    return resp.body
}

h := fetch("https://example.com")    // 调用 async 函数 = 派生协程，立即返回句柄
body := await h                      // 挂起当前协程，直到目标协程完成
```

语义要点：

- `async func` 定义**协程函数**。调用它不执行函数体，而是创建并调度一个新协程，返回协程句柄（类型 `coroutine`）。
- `await h` 挂起**当前协程**（不阻塞 OS 线程），直到 `h` 对应协程结束：
  - 正常结束 → `await` 表达式得到其 `return` 值。
  - 未捕获异常 → 在 `await` 点**重新抛出**（traceback 含目标协程栈）。
- `await` 可在任意函数中使用（不限于 async 函数内）；在顶层使用时等价于同步等待。
- 同一个句柄可被多个协程 `await`，都获得同一结果。
- 句柄方法：`h.done()` → bool；`h.cancel()` → 在目标协程的下一个让出点注入 `CancelledError`（`RuntimeError` 子类）；`h.result()` → 非阻塞，未完成抛 `RuntimeError`。

## 2. channel

```go
c := chan()            // 无缓冲：send 阻塞到有 recv（会合语义）
c := chan(16)          // 带缓冲：容量 16

c.send(v)              // 发送；缓冲满/无接收者时让出调度
v := c.recv()          // 接收；缓冲空时让出调度；已关闭且排空时抛 ChannelClosedError
v, ok := c.tryRecv()   // 非阻塞；无值时 ok 为 false
ok := c.trySend(v)     // 非阻塞发送，缓冲满返回 false
c.close()              // 关闭；再 send 抛 ChannelClosedError
n := c.len()           // 当前缓冲内元素数
m := cap(c)            // 缓冲容量（内建函数）
```

- channel 是一等值，可作为参数/返回值/字典值传递。
- channel 的收发操作是协程让出点（yield point）：阻塞时协程被挂起，OS 线程转而运行其他就绪协程。
- `for v in c` 迭代 channel：持续 `recv` 直到 channel 关闭且排空（`ChannelClosedError` 被循环内部消化）。

## 3. select

```go
select {
case v := c1.recv():
    print("from c1:", v)
case c2.send(x):
    print("sent to c2")
default:
    print("no op ready")
}
```

语法规则：

```ebnf
selectStmt = "select" "{" { selectCase } "}"
selectCase = "case" ( recvStmt | sendStmt ) ":" block
           | "default" ":" block
recvStmt   = [ identList ( ":=" | "=" ) ] expr "." "recv" "(" ")"
sendStmt   = expr "." "send" "(" expr ")"
```

- 多个 case 同时就绪时**伪随机**选一个（不保证顺序公平，避免饥饿由实现保证）。
- 无 `default` 且全部阻塞时，协程挂起直到任一 case 就绪。
- `select {}`（空 select）永久挂起当前协程。
- 等待协程句柄用 `await`，不进入 `select` 的 case（首版决策；`select` + 句柄列入路线图）。

## 4. 调度器（M:N）

- M 个协程由调度器映射到 N 个 OS 工作线程，N 默认为 CPU 核数，可用环境变量 `MS_THREADS` 或 `sync.setMaxThreads(n)` 调整。
- 工作线程从全局/本地就绪队列取协程运行，支持**工作窃取**（work stealing）。
- 让出点：channel 操作、`await`、`select` 阻塞、`time.sleep`、阻塞式 IO（net/os 模块中标注为"协程感知"的 API）。
- **抢占**：纯计算型长循环不会主动让出。VM 在每个回边（loop back-edge）与函数调用处检查抢占标记，超时（默认 10ms）强制让出，防止单个协程饿死调度线程。
- 阻塞在 OS 调用上的操作（如未封装的 C 扩展）会占用其工作线程；调度器检测到线程耗尽时**临时扩容**工作线程数。

## 5. 内存模型

借鉴 Go 的 happens-before 规则：

1. `c.send(v)` 中的写入 happens-before 对应的 `c.recv()` 返回——经 channel 传递的对象对接收方可见。
2. 协程派生（调用 async 函数）前的写入 happens-before 协程体内的读取。
3. 协程的 `return` happens-before 对其句柄 `await` 的返回。
4. `sync` 模块的 `Mutex`/`WaitGroup` 建立各自的 happens-before 边。

**不在上述边内的共享可变状态是数据竞争，行为未定义**。mslang 不提供 volatile/原子语义的脚本级类型；需要底层同步时用 `sync.Mutex`：

```go
import "sync"

mu := sync.mutex()
mu.lock()
shared.count += 1
mu.unlock()
// 或
with mu { shared.count += 1 }     // Mutex 实现 __enter__/__exit__
```

## 6. 与 GC 的协作

- GC 为 STW 标记-清除：触发时所有工作线程在最近 safepoint 停住（safepoint = 让出点 + 函数调用 + 回边），标记-清除完成后恢复。
- 协程的 VM 栈、寄存器窗口、句柄表都是 GC 根。
- 详细实现见 [08-vm-internals.md](08-vm-internals.md)。

## 7. 完整示例：生产者-消费者

```go
async func producer(c, n) {
    for i := 0; i < n; i++ {
        c.send(i * i)
    }
    c.close()
}

async func consumer(c, out) {
    total := 0
    for v in c {            // 迭代至 channel 关闭
        total += v
    }
    out.send(total)
}

jobs := chan(8)
results := chan(1)
p := producer(jobs, 100)
consumer(jobs, results)
await p                                // 等待生产完成（可选，recv 本身会等）
print(results.recv())                  // 328350
```
