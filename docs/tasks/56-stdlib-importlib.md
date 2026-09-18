# 56 标准库：importlib

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.4 | ⬜ | [24 模块系统与 import](24-modules-import.md) |

## 任务目标

交付 C 内建标准库模块 importlib（`stdlib/importlib/ms_importlib.h` / `stdlib/importlib/ms_importlib.c`），向脚本暴露两个函数：

- `importlib.importModule(path)`：动态导入，等价于运行期执行的 `import "path"`，返回模块对象；
- `importlib.reload(module)`：就地重新执行模块顶层代码（面向 REPL 调试场景），返回同一模块对象。

本任务不实现模块解析、缓存注册表或模块执行器——这些全部由任务 24 提供；importlib 只是模块机制的**运行期入口**，负责参数校验、相对路径限制、reload 的就地重执行编排与缓存失效语义（见「详细设计」）。完成后，脚本可在运行期按字符串名导入任意模块，并在修改源码后无需重启解释器即刷新模块内容。

## 设计依据

- `docs/language/07-stdlib.md`
  - §0 模块清单：`importlib` 为 **C 内建模块**（`stdlib/` 目录）。
  - §24 importlib：仅 `importModule(path)` 与 `reload(module)` 两个函数；`reload` 注释为 "re-executes the module code (for REPL debugging)"。
- `docs/language/05-modules.md`
  - §2 模块解析：解析顺序（内建/已注册模块 → 相对路径 → `MS_PATH` → 安装目录 `lib/`）；`a/b/c` 依次尝试 `c.ms` 与 `c/__init__.ms`。
  - §4 执行语义：模块首次导入执行一次，模块对象缓存于 `modules` 注册表，键为解析后的绝对路径（或内建模块名），重复导入返回同一对象；循环导入时后导入方拿到部分初始化的模块对象。
  - §6 动态导入与内省：`importlib.importModule("encoding/json")` 返回模块对象，可配合内建 `dir` 使用。
  - §7 与 C 扩展的关系：C 内建模块在注册表中优先级高于搜索路径，不可被同名脚本遮蔽。
- `docs/language/04-exceptions.md` §4 异常层级：`ImportError`、`TypeError`、`ValueError`、`AttributeError` 均为标准异常类。
- `docs/language/09-c-api.md`：`MsModuleDef` / `MsMethodDef` / `MsCFunction`（签名 `MsObject* fn(MsState* L, int64_t argc, MsObject** argv)`）与 `msRegisterModule` 注册流程；§3 根栈纪律（`msRootPush` / `msRootPop`）。
- `docs/language/10-c-style.md`：§1 文件组织与 include guard、§2 格式化（2 空格缩进、120 列、K&R）、§3 命名、§4 typedef 规则、§5 错误处理、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）。
- `docs/language/11-project-layout.md`：C 标准库模块位于 `stdlib/`；脚本测试位于 `tests/ms/`，测试数据位于 `tests/fixtures/`。
- 任务 24 模块系统与 import：提供模块注册表（`modules`）、解析器与模块执行器，importlib 的全部功能都建立在它的导出接口之上。任务 24 的文档（`24-modules-import.md`）尚不存在，本文出现的任务 24 侧接口名（`msModulesImportPath`、`msModulesLookupByName`、`msModulesExecFile`、`struct MsModule` 的字段名等）均为假定命名，实现时以对应任务文档定名为准。同理，任务 40 的 `testing` / `testing/assert` 模块接口以 `40-stdlib-testing.md` 定名为准。

## 详细设计

### 职责边界

importlib 自身**无状态**：不持有堆内存、不维护任何缓存。模块缓存注册表、路径解析、源码编译与顶层执行全部委托任务 24。本模块只做三件事：校验脚本侧参数、把字符串路径交给任务 24 的导入原语、为 reload 编排"查找注册键 → 取源文件 → 就地重执行"的流程。

### 文件与头文件骨架

- 头文件 `stdlib/importlib/ms_importlib.h`，include guard `MSLANG_SRC_IMPORTLIB_MS_IMPORTLIB_H_`，自包含（自行 include `<mslang/mslang.h>`）。
- 实现文件 `stdlib/importlib/ms_importlib.c`；两个 C 函数实现均为文件内 `static`。
- 头文件只暴露模块定义入口，供解释器启动时装配：

```c
// Returns the static module definition for the built-in "importlib" module.
// The interpreter registers it at startup via msRegisterModule; the returned
// pointer refers to static storage and must not be freed.
const MsModuleDef* msImportlibModuleDef(void);
```

- 模块定义与函数表（文件级 `static const`）：

```c
static const MsMethodDef importlibMethods[] = {
  {"importModule", importlibImportModule, "importModule(path) -> module"},
  {"reload", importlibReload, "reload(module) -> module"},
  {NULL, NULL, NULL},
};

static const MsModuleDef importlibModule = {
  "importlib", "dynamic import and module reload", importlibMethods,
};
```

### 脚本侧接口与错误约定

```ms
import "importlib"

m := importlib.importModule("encoding/json")   // 动态导入，返回模块对象
importlib.reload(m)                            // 就地重执行，返回同一模块对象
```

| 情形 | 行为 |
|---|---|
| `importModule` 参数不是 str | 抛 `TypeError` |
| `importModule` 参数为空串 | 抛 `ValueError` |
| `importModule` 参数以 `./` 或 `../` 开头 | 抛 `ValueError`（见下「相对路径限制」） |
| 路径无法解析（无对应内建模块、搜索路径无命中） | 抛 `ImportError` |
| 模块顶层代码执行时抛异常 | 原样传播（不做包装） |
| `reload` 参数不是模块对象 | 抛 `TypeError` |
| `reload` 的模块不在注册表（如 `__main__` 入口模块） | 抛 `ImportError` |
| `reload` 内建 C 模块（含 importlib 自身） | 抛 `ImportError` |
| `reload` 时源文件已删除 | 抛 `ImportError` |
| `reload` 重执行中抛异常 | 原样传播，模块以部分更新状态留在注册表 |

### 相对路径限制（规范空白的处理）

05-modules §2 的相对路径解析基准是"当前导入方文件的目录"，这是**编译期**概念；`importModule` 在运行期被调用，调用栈顶可能是任意模块甚至是 REPL 片段，"导入方"界定模糊且易误用。v0.4 刻意限制：`importModule` 不接受以 `./` / `../` 开头的相对路径（抛 `ValueError`），只接受内建模块名与搜索路径形式（`"strings"`、`"encoding/json"`、`"github.com/user/lib"` 等）。需要动态导入本地文件时，调用方先把目录加入 `MS_PATH`（经 `os.setEnv`）再按包路径导入。此限制在模块 doc 与函数 doc 字符串中写明。

### C 函数签名与实现要点

```c
// importlib.importModule(path): dynamically imports the module named by the
// string argv[0] and returns the module object. Relative paths ("./",
// "../") are rejected with ValueError; unresolvable paths raise ImportError.
// Exceptions raised by the module's top-level code propagate unchanged.
static MsObject* importlibImportModule(MsState* L, int64_t argc, MsObject** argv);

// importlib.reload(module): re-executes the module's top-level code in place
// (same module object, same namespace dict) and returns the module.
// Raises TypeError for non-modules, ImportError for modules not present in
// the registry, built-in modules, or missing source files.
static MsObject* importlibReload(MsState* L, int64_t argc, MsObject** argv);
```

实现要点：

- 参数个数校验走内建函数的统一 argc 检查（任务 10/18 的约定）；类型检查用任务 06 的对象模型谓词（字符串判定、模块类型判定）。
- C 局部持有的 `MsObject*`（模块对象、字符串）遵守根栈纪律：跨越可能触发 GC 的调用（编译、执行）前 `msRootPush`，返回前按 LIFO `msRootPop`。
- 本模块不调用 `msAlloc`：所有分配都在任务 24 的原语内部完成。

### importModule 流程

```
importlibImportModule(L, argc, argv)
  1. 校验 argc == 1、argv[0] 是 str，否则 TypeError
  2. 取 C 字符串 path；长度为 0 → ValueError
  3. path 以 "./" 或 "../" 开头 → ValueError
  4. 调用任务 24 的导入原语：
       MsResult msModulesImportPath(MsState* L, const char* path, MsObject** outModule);
     该原语内部完成：内建注册表查找 → 搜索路径解析 → 缓存命中直接返回 →
     未命中则加载、注册、执行顶层（首次导入语义，05-modules §4）
  5. 结果为 MS_ERROR_IMPORT → 抛 ImportError（消息含原路径与失败原因）
     结果为模块执行异常 → 异常已在 L 上，直接返回 NULL 传播
  6. 成功：返回 *outModule
```

与静态 `import "path"` 的等价性是核心保证：两者走**同一注册表与同一解析顺序**，因此对同一路径，`importModule` 返回的模块对象与静态导入得到的对象是同一个（脚本可用 `is` 断言恒等），内建 C 模块同样不可被搜索路径上的同名脚本遮蔽（05-modules §7）。

### reload 流程与缓存失效语义

**语义定义**（07-stdlib §24 只有一句话，以下为对齐 Python `importlib.reload` 的明确化决策，须在模块 doc 中写明）：

1. **就地重执行**：复用同一模块对象与同一命名空间 dict，**不清空**命名空间——重执行前模块内的旧名字全部保留，新源码重新定义的名字覆盖旧值，新源码删除的名字仍然残留。持有该模块引用的代码立即看到更新后的属性。
2. **缓存不失效**：注册表中的键 → 模块对象映射**不替换**，重执行期间模块始终处于已注册状态。因此 reload 期间若触发循环导入，导入方拿到的是部分更新的同一模块对象——与 05-modules §4 的循环导入部分初始化语义一致。reload 之后的静态/动态导入返回刷新后的同一对象。
3. **旧绑定不追溯**：此前经 `from "m" import x` 绑定的名字是值拷贝语义，reload 后调用方的 `x` 仍指向旧对象；只有 `m.x` 形式的属性访问看到新值。
4. **失败语义**：重执行中任何异常原样传播；模块以部分更新状态留在注册表（**不**回滚、**不**摘除），与 Python 一致。下次 `import` 或 `importModule` 仍返回该部分更新对象；再次 `reload` 可重试。
5. **元数据保留**：`__name__`（注册键）、`__file__`（源文件绝对路径）在 reload 前后不变，不受新源码顶层赋值影响（重执行结束后由 importlib 强制恢复）。

**流程**：

```
importlibReload(L, argc, argv)
  1. 校验 argc == 1、argv[0] 是模块对象，否则 TypeError
  2. 取模块 __name__，在任务 24 注册表中按键查找：
       MsObject* msModulesLookupByName(MsState* L, const char* name);
     未命中，或注册值与 argv[0] 不是同一对象（同名被替换的孤儿模块）
       → ImportError("module not in registry")；__main__ 天然落入此分支
  3. 取模块 __file__：
     为 nil（内建 C 模块，含 importlib 自身）→ ImportError("cannot reload built-in module")
     文件已不存在 → ImportError（消息含路径）
  4. msRootPush 模块对象
  5. 调用任务 24 的就地执行原语：
       MsResult msModulesExecFile(MsState* L, MsObject* module, const char* filePath);
     编译 filePath → 在 module 的既有命名空间内执行顶层字节码；
     期间模块保持注册（满足语义 2）
  6. 恢复 __name__ / __file__（语义 5）
  7. msRootPop；失败则异常已在 L 上，返回 NULL 传播；成功返回模块对象
```

### 与任务 24 的接口约定汇总

本任务假定任务 24 导出以下能力（定名以任务 24 文档为准）：

| 假定接口 | 用途 |
|---|---|
| `msModulesImportPath(L, path, outModule)` | importModule 的解析 + 缓存 + 首次执行 |
| `msModulesLookupByName(L, name)` | reload 的注册表身份核验 |
| `msModulesExecFile(L, module, filePath)` | reload 的就地重执行 |
| 模块对象的 `__name__` / `__file__` 属性读写 | 注册键与源文件定位；`__file__` 为 nil 表示内建模块 |

若任务 24 未提供 `msModulesExecFile` 形式的原语，实现时在任务 24 的模块中补充（属任务 24 范围内的增量），importlib 不自行拼装"编译 + 执行"细节。

## 实现步骤

1. 建 `stdlib/importlib/ms_importlib.h` / `ms_importlib.c` 骨架：`MsModuleDef` 静态定义、`msImportlibModuleDef` 入口、两个 `static` 空实现（先返回 nil），并接入解释器启动时的内建模块注册表。验证：脚本 `import "importlib"` 成功，`dir(importlib)` 列出 `importModule` 与 `reload`。
2. 实现 `importlibImportModule` 的参数校验：argc、str 类型（TypeError）、空串（ValueError）、`./`/`../` 前缀（ValueError）。验证：各非法参数抛出对应异常类，消息含 offending 值。
3. 接通 `msModulesImportPath`：内建模块、搜索路径模块、缓存命中、ImportError 与执行异常传播。验证：动态导入与静态导入返回同一对象（`is` 断言）；重复调用返回同一对象；不存在路径抛 ImportError。
4. 实现 `importlibReload` 的校验链：非模块 TypeError；不在注册表 ImportError（`__main__` 用例）；内建模块 ImportError；源文件缺失 ImportError。验证：逐条异常断言。
5. 实现就地重执行：根栈保护、`msModulesExecFile` 调用、`__name__`/`__file__` 恢复。验证：修改 fixture 源文件后 reload，经同一模块引用看到新属性；旧 `from import` 绑定不变；被删除的名字残留。
6. 实现失败语义：重执行中语法错误/运行时异常原样传播，模块以部分更新状态留在注册表，再次 reload 可恢复。验证：对应脚本测试全部通过；ASAN/Debug 构建下无泄漏与双重释放（配合任务 02 的分配统计）。

## 测试方案

本任务晚于任务 40（testing 模块），脚本测试统一使用 `testing` + `testing/assert`，文件遵循 `xxx_test.ms` 约定（测试函数以 `test` 开头，文件末尾在 `if __name__ == "__main__"` 中调用 `testing.run()`），由仓库根 `run_tests.py` 驱动 mslang CLI 执行。

fixture（多文件模块，本任务只交付设计文档，fixture 随实现编写）：

- `tests/fixtures/importlib/dynmod.ms`：顶层定义 `value = 1`、函数 `greet(name)`、`onlyInV1 = "old"`，并在顶层向共享记录模块 `dynlog` 的列表追加一次（用于计数顶层执行次数）。
- `tests/fixtures/importlib/dynlog.ms`：顶层 `entries = []`，供 dynmod 记录执行次数。
- `tests/fixtures/importlib/dyncycle_a.ms` / `dyncycle_b.ms`：相互导入的一对模块（B 在函数内延迟访问 A 的名字），验证 reload 期间循环导入拿到部分更新的同一对象。

测试文件与覆盖点：

- `tests/ms/stdlib/importlib_test.ms`
  - `testImportModuleBuiltin`：动态导入 `"strings"` 与静态 `import "strings"` 返回同一对象（`is`）；动态导入 `"importlib"` 自身成功。
  - `testImportModuleCache`：两次 `importModule("...")` 返回同一对象；`dynlog.entries` 长度为 1（首次导入只执行一次）。
  - `testImportModuleName`：动态导入模块的 `__name__` 为路径形式名。
  - `testImportModuleErrors`：非 str 参数 → TypeError；空串 → ValueError；`"./dynmod"` → ValueError；不存在的 `"no/such/module"` → ImportError；顶层代码抛异常的模块 → 异常原样传播（类型与消息不变）。
  - `testReloadPicksUpChanges`：测试运行前把 `dynmod.ms` 复制到临时目录、改写 `value = 2` 并删除 `onlyInV1`（经 `os`/`io` 写文件，`MS_PATH` 指向临时目录），`importModule` 后 `reload`：同一对象（`is`）、`m.value == 2`、`m.onlyInV1` 仍残留（语义 1）、`dynlog.entries` 长度 +1。
  - `testReloadStaleFromBinding`：reload 前 `from "..." import value`，reload 后旧绑定仍是 `1`，`m.value` 是 `2`（语义 3）。
  - `testReloadErrors`：非模块参数 → TypeError；`__main__` 模块 → ImportError；内建模块（`strings`、importlib 自身）→ ImportError；源文件删除后 → ImportError。
  - `testReloadFailurePartial`：把 fixture 改写为含运行时错误的版本，reload 抛原异常；模块仍在注册表（`importModule` 返回同一对象），已执行部分的更新可见；修复文件后再次 reload 恢复正常。
  - `testReloadCircular`：对 `dyncycle_a` reload，期间 `dyncycle_b` 的延迟导入拿到同一部分更新对象，不抛 AttributeError（延迟到函数内访问，符合 05-modules §4 建议）。
- 测试运行环境约定：`run_tests.py`（或测试脚本开头经 `os.setEnv`）把 `tests/fixtures` 与测试临时目录加入 `MS_PATH`，fixture 以 `"importlib/dynmod"` 形式的包路径导入；临时改写文件只落在 `build/` 下，不修改 `tests/fixtures/` 原件。

## 验收标准

- [ ] `stdlib/importlib/ms_importlib.h` / `ms_importlib.c` 存在，guard 为 `MSLANG_SRC_IMPORTLIB_MS_IMPORTLIB_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、无多余 typedef）。
- [ ] `import "importlib"` 可用，模块导出且仅导出 `importModule` 与 `reload` 两个函数，doc 字符串写明相对路径限制与 reload 语义。
- [ ] `importModule` 与静态 `import` 走同一注册表与解析顺序：同路径返回同一对象（`is` 恒等），首次导入只执行一次，内建模块不可被同名脚本遮蔽。
- [ ] `importModule` 的错误行为符合「脚本侧接口与错误约定」表：TypeError / ValueError（空串、相对路径）/ ImportError / 模块异常原样传播。
- [ ] `reload` 实现就地重执行语义：同一模块对象、命名空间不清空（残留旧名）、注册表映射不变、旧 `from import` 绑定不追溯、`__name__`/`__file__` 保留。
- [ ] `reload` 的校验链完整：非模块 TypeError；`__main__`、内建模块、源文件缺失均 ImportError；重执行异常原样传播且模块以部分更新状态留在注册表，可再次 reload 恢复。
- [ ] C 侧遵守根栈纪律，本模块零堆分配；ASAN/Debug 构建与任务 02 分配统计下无泄漏。
- [ ] `tests/ms/stdlib/importlib_test.ms` 与 `tests/fixtures/importlib/` 各 fixture 覆盖「测试方案」全部清单项，`run_tests.py` 全绿；构建产物只落在 `build/`。
- [ ] 无 TBD/TODO 占位；与任务 24 的接口假定（`msModulesImportPath`、`msModulesLookupByName`、`msModulesExecFile`、`__name__`/`__file__` 字段）在实现时已对齐任务 24 文档定名。
