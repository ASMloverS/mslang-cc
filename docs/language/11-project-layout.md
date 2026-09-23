# 11 仓库结构、构建与路线图

## 1. 仓库结构

```
mslang-cc/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── language/                # 本设计文档集
├── include/
│   └── mslang/                  # 公开 C API 头文件（见 09-c-api.md）
├── src/
│   ├── core/                    # 核心基础设施（内存封装、错误码、通用宏、诊断收集器）
│   ├── lexer/                   # 词法分析
│   │   ├── ms_lexer.c
│   │   └── ms_lexer.h
│   ├── parser/                  # 语法分析（递归下降 + Pratt）
│   ├── compiler/                # AST → 字节码
│   ├── vm/                      # 执行核心：分派循环、帧、内建函数
│   ├── object/                  # 对象模型与各类型实现
│   ├── gc/                      # 标记-清除 GC
│   ├── sched/                   # 协程与 M:N 调度器
│   ├── platform/                # 平台抽象层（线程/原子/时钟/socket/dlopen）
│   └── cli/                     # mslang 可执行文件：REPL + 脚本入口
├── stdlib/                      # C 实现的标准库模块（fmt/strings/json/...）
├── lib/                         # 纯 .ms 实现的标准库模块（sort/log/testing/...）
├── tests/
│   ├── c/                       # C 单元测试（自研轻量断言框架）
│   ├── ms/                      # 脚本级测试（testing 模块驱动）
│   └── fixtures/                # 测试数据
└── examples/                    # 嵌入示例、脚本示例
```

## 2. 构建（CMake）

要求：CMake ≥ 3.20，编译器支持 C11（MSVC 2019+ / GCC 10+ / Clang 12+）。

目标（targets）：

| Target | 类型 | 说明 |
|---|---|---|
| `mslang` | 静态/动态库 | 解释器核心 + C 标准库模块 |
| `mslang-cli` | 可执行文件 | REPL 与脚本入口，链接 `mslang` |
| `mslang-tests` | 可执行文件 | C 单元测试 |
| `embed-example` | 可执行文件 | 嵌入示例（examples/） |

构建选项：

```cmake
option(MSLANG_BUILD_TESTS "Build tests" ON)
option(MSLANG_BUILD_SHARED "Build shared library" OFF)
option(MSLANG_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MSLANG_STRICT_WARNINGS "Treat warnings as errors" ON)
```

常用命令：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build            # C 测试 + 脚本测试（以 CLI 驱动 tests/ms/）
```

## 3. CLI 行为

```bash
mslang                    # 进入 REPL
mslang script.ms arg1     # 直接顺序执行脚本顶层语句（无入口函数概念），args 经 os.args 暴露
mslang test ./...         # 运行测试（发现 *_test.ms）
mslang -e "print(1+1)"    # 执行代码片段
mslang --version
```

REPL 特性：多行输入（花括号未闭合时续行）、历史记录、上次结果绑定 `_`。

## 4. 测试策略

三层：

1. **C 单元测试**（`tests/c/`）：lexer/parser/compiler/VM/GC/channel 各模块独立测试。自研极简断言头文件（`ms_test.h`，约 100 行：宏 `MS_TEST` / `MS_ASSERT_EQ` / 自动注册），不引入外部测试框架依赖。
2. **脚本测试**（`tests/ms/`）：用 `testing` 标准库模块编写的语言级测试，验证语法语义、标准库行为、异常、并发。每个语言特性至少一个脚本测试。
3. **嵌入测试**：`examples/embed_demo.c` 同时作为 C API 的集成测试，CI 中编译并运行。

CI 矩阵（GitHub Actions）：{windows-latest, ubuntu-latest, macos-latest} × {Debug, Release}，Debug 构建启用 ASAN（MSVC 除外，用 `/RTC` + Dr. Memory 备选）。

## 5. 路线图

### v0.1（最小可用解释器）

- Lexer / Parser / Compiler / 栈式 VM（仅同步语义）
- 核心类型：nil/bool/int（仅 int64，大整数后续）/float/str/list/dict
- 控制流、函数与闭包、class（无继承）
- C API：嵌入（eval + 调用脚本函数）
- 标准库：fmt、strings、math
- CLI + REPL

### v0.2（语言补全）

- 异常系统、模块系统、import
- class 继承与魔术方法协议、推导式、f-string、with
- 大整数、bytes、tuple、set
- C API：扩展模块、C 自定义类型
- 标准库：os、io、filepath、time、strconv、testing、collections

### v0.3（并发）

- async/await、协程、channel、select
- M:N 调度器（先单线程协作式跑通语义，再接入多线程）
- sync 模块、GC 的 STW safepoint 改造

### v0.4（生态模块）

- encoding/json、encoding/base64、crypto/*、regexp
- net、net/http、log、errors、random、datetime、itertools、functools

### v1.0（固化）

- 语言语义冻结、文档完备、C API 评审
- 性能优化项从 [08-vm-internals.md](08-vm-internals.md) §7 中按基准测试选取

### 路线图候选（未承诺）

生成器（`yield`）、装饰器语法、match 语句、反射增强、包管理器、TLS、IDE 工具链。
