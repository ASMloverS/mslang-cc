# 07 标准库

> 模块路径采用 Go 风格（如 `encoding/json`），函数与方法命名采用 Java 规范的小驼峰（如 `json.dumps`、`strings.toUpper`）。模块按实现方式分两类：**C 内建模块**（`stdlib/` 目录，性能关键）与**纯脚本模块**（`lib/` 目录，用 .ms 编写）。

## 0. 模块清单与实现方式

| 模块 | 实现 | 说明 |
|---|---|---|
| `fmt` | C | 格式化输出（printf/sprintf） |
| `strings` | C | 字符串工具 |
| `strconv` | C | 字符串↔数字转换 |
| `math` | C | 数学函数与常量 |
| `sort` | .ms | 排序与二分 |
| `os` | C | 操作系统接口 |
| `io` | C | Reader/Writer 抽象 |
| `path/filepath` | C | 路径操作 |
| `time` | C | 时间与睡眠 |
| `datetime` | .ms | 日期时间对象（基于 time） |
| `errors` | .ms | 错误包装工具 |
| `log` | .ms | 日志 |
| `encoding/json` | C | JSON 编解码 |
| `encoding/base64` | C | Base64 编解码 |
| `crypto/md5` `crypto/sha256` | C | 哈希 |
| `regexp` | C | 正则（自研引擎，见 §12） |
| `net` | C | TCP/UDP（协程感知） |
| `net/http` | C + .ms | HTTP 客户端与服务器 |
| `sync` | C | Mutex/WaitGroup/Once |
| `testing` | .ms | 单元测试框架 |
| `collections` | .ms | deque/heap/Counter/defaultdict |
| `random` | .ms | 伪随机数（xorshift128+） |
| `itertools` | .ms | 迭代器工具 |
| `functools` | .ms | 高阶函数工具 |
| `importlib` | C | 动态导入 |

## 1. fmt

职责定位：**仅负责格式化输出**。常规输出用内建 `print`（见 [03-syntax.md](03-syntax.md) §9.1）；需要宽度、精度、进制、对齐等格式控制时才用 fmt。简单插值优先用 f-string 而非 `fmt.sprintf`。

```go
fmt.printf(format, ...)             // 格式化输出到 stdout
fmt.sprintf(format, ...)            // 返回格式化字符串
fmt.fprintf(writer, format, ...)    // 格式化输出到 io.Writer
```

格式动词对齐 Go：`%v %s %d %f %.2f %x %o %b %q %t %p %%`。不提供 `fmt.print`/`fmt.println`——与内建 `print` 功能重叠，刻意删除（"一种事一种做法"）。

## 2. strings

```go
strings.toUpper(s)  strings.toLower(s)  strings.title(s)
strings.split(s, sep)  strings.splitN(s, sep, n)  strings.fields(s)
strings.join(xs, sep)
strings.contains(s, sub)  strings.hasPrefix(s, p)  strings.hasSuffix(s, p)
strings.indexOf(s, sub)  strings.lastIndexOf(s, sub)  strings.count(s, sub)
strings.replace(s, old, new, n=-1)
strings.trimSpace(s)  strings.trim(s, cutset)  strings.trimPrefix(s, p)  strings.trimSuffix(s, p)
strings.repeat(s, n)  strings.padLeft(s, n, pad=" ")  strings.padRight(s, n, pad=" ")
strings.builder()                    // Builder 对象：write(s)/toString()，高效拼接
```

## 3. strconv

```go
strconv.parseInt(s, base=10)         // 失败抛 ValueError
strconv.parseFloat(s)
strconv.formatInt(i, base=10)
strconv.formatFloat(f, prec=-1)      // prec=-1 最短往返表示
strconv.quote(s)  strconv.unquote(s)
```

## 4. math

常量：`math.pi math.e math.inf math.nan`

```go
math.floor(x) math.ceil(x) math.trunc(x) math.abs(x)
math.sqrt(x) math.pow(x, y) math.exp(x) math.log(x, base=math.e) math.log2(x) math.log10(x)
math.sin(x) math.cos(x) math.tan(x) math.asin(x) math.acos(x) math.atan(x) math.atan2(y, x)
math.isNaN(x) math.isInf(x) math.isFinite(x)
math.gcd(a, b) math.lcm(a, b) math.factorial(n)
```

## 5. sort

```go
sort.sort(xs)                        // 原地排序
sort.sorted(xs)                      // 返回新列表
sort.sortBy(xs, key)                 // key 为函数
sort.reverse(xs)
sort.binarySearch(xs, v)             // 返回插入位（bisect 语义）
sort.isSorted(xs)
```

## 6. os

```go
os.args                              // list，命令行参数
os.getEnv(key, default=nil)  os.setEnv(key, value)  os.environ()    // dict
os.cwd()  os.chdir(path)
os.exit(code=0)                      // 抛 SystemExit
os.stat(path)                        // StatInfo：size/mode/modTime/isDir/isFile
os.listDir(path)                     // list[str]
os.mkdir(path)  os.mkdirAll(path)  os.remove(path)  os.removeAll(path)  os.rename(old, new)
os.exists(path)
os.exec(cmd, args=[])                // 执行外部命令，返回 (exitCode, stdout, stderr)
os.platform                          // "windows" | "linux" | "darwin"
```

## 7. io

接口协议（鸭子类型）：

- **Reader**：`read(n=-1)` → bytes/str；`readAll()`；`readLine()`；`close()`
- **Writer**：`write(data)` → int；`flush()`；`close()`

```go
io.copy(dst, src)                    // 返回拷贝字节数
io.readAll(reader)
io.tee(reader, writer)
```

`open(path, mode)` 返回的 File 同时实现两者；mode：`"r" "w" "a" "rb" "wb" "r+"`。File 额外方法：`seek(offset, whence)` `tell()`，且支持 `with`。

## 8. path/filepath

```go
filepath.join(p1, p2, ...)  filepath.split(path)     // → (dir, name)
filepath.dir(path)  filepath.base(path)  filepath.ext(path)
filepath.abs(path)  filepath.clean(path)  filepath.isAbs(path)
filepath.match(pattern, name)            // glob 匹配
filepath.glob(pattern)                   // 返回 list
```

## 9. time 与 datetime

```go
time.now()                           // float，Unix 秒（含小数）
time.sleep(seconds)                  // 协程感知：挂起协程而非线程
time.monotonic()                     // 单调时钟
time.format(ts, layout)  time.parse(layout, text)
```

`datetime` 提供对象化 API：

```go
dt := datetime.now()                 // DateTime 对象
dt.year dt.month dt.day dt.hour dt.minute dt.second
dt + datetime.timedelta(days=1)      // 运算
dt.strftime("%Y-%m-%d")
datetime.strptime(text, fmt)
datetime.date(2025, 1, 1)  datetime.time(12, 30)
```

## 10. errors

```go
errors.new(message)                  // 构造 RuntimeError 实例
errors.wrap(err, message)            // 包装：raise X from err 的便捷形式
errors.isInstance(err, type)         // isinstance 的语义别名（沿 __cause__ 链查找）
errors.unwrap(err)                   // 返回 __cause__
```

## 11. log

```go
log.debug(...) log.info(...) log.warn(...) log.error(...)
log.setLevel(log.INFO)               // DEBUG < INFO < WARN < ERROR
log.setOutput(writer)                // 默认 stderr
log.setFormat("{time} [{level}] {message}")
```

## 12. regexp

自研正则引擎（实现子集：字符类、量词、分组、捕获、锚点、交替；**不支持回溯灾难构造**，采用 Thompson NFA 保证线性时间）：

```go
re := regexp.compile(`\d+`)
re.match(text)                       // 从开头匹配 → Match 或 nil
re.search(text)                      // 任意位置
re.findAll(text)                     // list[Match]
re.replaceAll(text, repl)
re.split(text)
m.group(0) m.group(1) m.start() m.end() m.groups()
regexp.match(pattern, text)          // 免编译便捷版
```

## 13. encoding/json

```go
json.dumps(value, indent=0)          // mslang 值 → JSON 字符串
json.loads(text)                     // JSON → mslang 值（dict/list/str/float/int/bool/nil）
json.dump(value, writer)  json.load(reader)
```

映射：object↔dict、array↔list、number↔int/float、true/false/nil 直译。class 实例通过定义 `__json__()` 方法定制序列化。

## 14. encoding/base64

```go
base64.encode(data)                  // str/bytes → str
base64.decode(text)                  // → bytes
base64.urlEncode(data)  base64.urlDecode(text)
```

## 15. crypto/md5 与 crypto/sha256

```go
md5.sum(data)                        // → bytes（摘要）
sha256.sum(data)
h := sha256.new()                    // Hasher：write(data)/digest()/hexDigest()
```

## 16. net

```go
conn := net.dial("tcp", "example.com:80")    // 协程感知
conn.read(n) conn.write(data) conn.close()
conn.setDeadline(seconds)

listener := net.listen("tcp", ":8080")
conn := listener.accept()                    // 协程感知
```

## 17. net/http

客户端：

```go
resp := http.get(url, headers=nil, timeout=30)
resp := http.post(url, body, headers=nil)
resp.status resp.headers resp.body           // body 为 str（按 Content-Type 解码）
resp.json()                                  // 便捷：json.loads(body)
req := http.request(method, url, body=nil, headers=nil)
```

服务器：

```go
func handler(req) {
    return http.response(200, "ok")          // 或返回 str/dict（自动 JSON）
}

srv := http.server(":8080", handler)
srv.serve()                                  // 每个连接一个协程
```

首版仅 HTTP/1.1，不含 TLS（TLS 依赖第三方库，列入路线图评估）。

## 18. sync

```go
mu := sync.mutex()                   // lock() unlock()，支持 with
rw := sync.rwMutex()                 // rlock() runlock() lock() unlock()
wg := sync.waitGroup()               // add(n) done() wait()（协程感知）
once := sync.once()                  // once.do(f)
sync.setMaxThreads(n)                // 调整调度器工作线程数
```

## 19. testing

约定：测试文件 `xxx_test.ms`，测试函数名以 `test` 开头。

```go
import "testing"
import "testing/assert"

func testAdd() {
    assert.equal(1 + 1, 2)
    assert.true(len([]) == 0)
    assert.raises(ValueError, func() { strconv.parseInt("abc") })
}

testing.run()                        // 发现并运行当前模块的 test* 函数
```

CLI 支持 `mslang test ./...`（发现 `*_test.ms` 并汇总报告）。

## 20. collections

```go
collections.deque(xs=[])             // append/appendLeft/pop/popLeft/rotate
collections.heap()                   // push/pop/peek/pushPop（最小堆）
collections.counter(xs)              // 计数 dict，mostCommon(n)
collections.defaultdict(factory)     // 缺键自动调用 factory
collections.orderedDict()            // dict 已有插入序，此为显式语义别名
```

## 21. random

```go
random.seed(n)
random.random()                      // [0, 1)
random.randInt(a, b)                 // [a, b] 闭区间
random.choice(xs)  random.shuffle(xs)  random.sample(xs, k)
random.gauss(mu, sigma)
```

## 22. itertools

```go
itertools.count(start=0, step=1)  itertools.cycle(xs)  itertools.repeat(v, n=-1)
itertools.chain(a, b, ...)  itertools.islice(it, start, stop, step=1)
itertools.zipLongest(a, b, fill=nil)
itertools.groupby(xs, key)
itertools.permutations(xs, r)  itertools.combinations(xs, r)
itertools.accumulate(xs, f=nil)  itertools.takewhile(pred, xs)  itertools.dropwhile(pred, xs)
```

## 23. functools

```go
functools.reduce(f, xs, init)  // init 可选
functools.partial(f, *args, **kwargs)
functools.lruCache(maxSize=128)      // 装饰器用法：见下
functools.compose(f, g)              // compose(f, g)(x) == f(g(x))
```

装饰器语法（`@decorator`）列入路线图；首版用显式包装：

```go
fib = functools.lruCache()(fib)
```

## 24. importlib

```go
importlib.importModule(path)         // 动态导入，返回模块对象
importlib.reload(module)             // 重新执行模块代码（REPL 调试用）
```

## 25. 协程感知 API 标注

以下 API 在阻塞时挂起协程而非 OS 线程：`time.sleep`、`net.*` 全部、`http.*` 全部、`sync.waitGroup.wait`、`os.exec`（等待子进程时）。其余 IO（文件读写）首版占用工作线程（文件 IO 难以跨平台异步化，列入路线图）。
