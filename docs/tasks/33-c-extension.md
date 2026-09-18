# 33 C 扩展与自定义类型

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [18 C API 基础](18-c-api-foundation.md) |

## 任务目标

交付 mslang C API 的扩展半边：`include/mslang/module.h`（扩展模块注册与动态库加载）与 `include/mslang/ctype.h`（C 自定义类型）两个公开头文件及其内部实现。完成后：

- 嵌入方可以用 `msRegisterModule` 把 `MsModuleDef` 描述的 C 函数表静态注册为脚本可 `import` 的模块；
- 解释器在模块解析阶段能按约定加载共享库（导出 `mslangInit_<name>` 函数），把第三方 C 扩展动态接入模块注册表；
- C 扩展可以用 `msDefineType` 定义脚本可见的新类型（构造、方法、`str()` 表示、GC 终结），脚本侧像普通 class 实例一样使用；
- 一个示例扩展模块（`ctestext`：函数 + C 自定义类型 `Counter`）同时作为测试夹具与用法范例。

本任务自身通过 C 集成测试（`tests/c/test_c_extension.c`）与 ms 脚本测试（`tests/ms/c_extension/`）验证，不改动 VM 分派循环以外的既有模块边界。

## 设计依据

- `docs/language/09-c-api.md`
  - §2 头文件布局：`module.h`（扩展模块注册）与 `ctype.h`（C 自定义类型）是公开伞头文件的组成。
  - §3 对象模型与 GC 根：C 侧持有的 `MsObject*` 须入显式根栈；本任务的 C 函数实现范例与测试必须示范该纪律。
  - §8 错误处理：C 扩展函数出错时设置错误并返回 `NULL`，VM 转为脚本异常；`msRaiseTypeError` 等。
  - §9 扩展模块：`MsCFunction` / `MsMethodDef` / `MsModuleDef` / `msRegisterModule` / `mslangInit_<name>` 动态库约定，含 `fastmath` 示例。
  - §10 C 自定义类型：`MsTypeDef` / `msDefineType` / `msCInstanceData`；`finalize` 仅释放 C 侧资源，不得访问其他脚本对象（回收顺序未定义）。
  - §13 版本与兼容：首版不承诺 ABI 稳定，扩展按头文件版本重新编译。
- `docs/language/05-modules.md` §2 模块解析顺序与 §7：C 扩展模块与脚本模块共用同一注册表；已注册（内建）模块优先级高于搜索路径，标准库 C 模块不可被同名脚本遮蔽。
- `docs/language/08-vm-internals.md` §3 对象模型：`MsTypeTag` 含 `MS_TYPE_C_FUNCTION` 与 `MS_TYPE_C_TYPE`（C 扩展类型的实例）；`struct MsObjectHeader` 布局。§5 GC：标记-清除；根集合含模块注册表与 C API 显式根。
- `docs/language/10-c-style.md`：§1 include guard 与自包含头文件、§3 命名、§4 typedef 规则（`MsMethodDef`/`MsModuleDef`/`MsTypeDef`/`MsCFunction` 属允许 typedef 的公开纯数据结构与函数指针；内部结构体不 typedef）、§5 错误处理、§6 内存纪律、§9 平台抽象（平台相关代码只允许出现在 `src/platform/`）。
- `docs/language/11-project-layout.md` §1 目录结构（`include/mslang/`、`src/platform/`、`tests/c/`、`tests/ms/`、`tests/fixtures/`）与 §2 构建目标；§4 测试策略（C 单元/集成测试 + 脚本测试两层在本任务并用）。
- 任务 18 C API 基础提供：`MsState` 生命周期、`msRootPush/msRootPop`、`msNew*` 构造族、`msAs*` 转换族、`msRaise*` 族、公开伞头文件 `include/mslang/mslang.h` 的聚合方式。任务 18 文档尚不存在，本文假定其接口名，实现时以对应任务文档定名为准。
- 任务 24 模块系统与 import 提供：模块注册表（假定内部接口 `msModuleRegistryFind` / `msModuleRegistryAdd`，键为模块名或解析后路径）与解析流程中「未命中时的动态库候选」挂接点。任务 24 文档尚不存在，接口名为假定命名，实现时以对应任务文档定名为准。
- 任务 42 平台抽象层将统一接管 `dlopen`。本任务早于任务 42，按 10-c-style §9 的位置约束在 `src/platform/` 内先落一个**最小动态加载实现**（见「详细设计」），接口刻意收窄，任务 42 落地时并入平台层统一命名，此处显式承认该过渡安排。

## 详细设计

### 文件与头文件骨架

- 公开头文件 `include/mslang/module.h`，include guard `MSLANG_INCLUDE_MSLANG_MODULE_H_`；`include/mslang/ctype.h`，guard `MSLANG_INCLUDE_MSLANG_CTYPE_H_`。两者自包含（`<stddef.h>` `<stdint.h>` 及 `"state.h"`/`"object.h"` 的前向声明），并由伞头文件 `mslang.h` 聚合（任务 18 已建）。
- 公开头文件只放声明与允许 typedef 的纯数据结构；结构体定义放内部头文件：
  - `src/module/ms_native_module.h` / `ms_native_module.c`：静态注册与动态加载（guard `MSLANG_SRC_MODULE_MS_NATIVE_MODULE_H_`）。
  - `src/object/ms_ctype.h` / `ms_ctype.c`：C 自定义类型对象与实例（guard `MSLANG_SRC_OBJECT_MS_CTYPE_H_`）。
  - `src/platform/ms_dynload.h` / `ms_dynload.c`：最小动态加载（guard `MSLANG_SRC_PLATFORM_MS_DYNLOAD_H_`；任务 42 时并入平台层）。

### 公开接口：module.h

按 09-c-api §9 原样落地，补充文档注释（功能/参数/返回值/错误行为/根纪律）：

```c
typedef MsObject* (*MsCFunction)(MsState* L, int64_t argc, MsObject** argv);

typedef struct {
  const char* name;     // "factorial"
  MsCFunction func;
  const char* doc;      // docstring, may be NULL
} MsMethodDef;

typedef struct {
  const char*        name;     // module name: "fastmath"
  const char*        doc;
  const MsMethodDef* methods;  // array terminated by {NULL, NULL, NULL}
} MsModuleDef;

// Registers the module described by def under def->name, making it
// importable. The module object and its function objects are owned by the
// state; def itself is only borrowed for the duration of the call.
// Returns MS_ERROR_RUNTIME (and sets the error state) when the name is
// already registered or def is malformed; MS_ERROR_OOM on allocation
// failure.
MsResult msRegisterModule(MsState* L, const MsModuleDef* def);
```

注册语义：

- 校验：`def->name` 非空且为合法模块名（首段规则同脚本模块名）；`methods` 若非 `NULL` 必须以 `{NULL, NULL, NULL}` 哨兵结尾、每项 `name`/`func` 非空。
- 重名：注册表已含同名模块时报错返回 `MS_ERROR_RUNTIME`，不覆盖（防止静态注册遮蔽标准库 C 模块，对齐 05-modules §7 的安全意图）。
- 注册产物：新建模块对象（`MS_TYPE_MODULE`），把每个 `MsMethodDef` 包装为 C 函数对象（`MS_TYPE_C_FUNCTION`，见下）设为模块属性；`doc` 存为模块的 `__doc__`。模块对象进入注册表后即成为 GC 根（注册表本身是根，08-vm-internals §5）。

### 公开接口：ctype.h

按 09-c-api §10 原样落地：

```c
typedef struct {
  const char* name;     // "Matrix"
  size_t instanceSize;  // per-instance extra data size
  MsCFunction init;     // constructor, may be NULL
  // Called before the GC frees the instance; may be NULL.
  void (*finalize)(MsObject* obj);
  MsObject* (*toString)(MsState* L, MsObject* obj);
  const MsMethodDef* methods;
} MsTypeDef;

// Defines a new script-visible type; returns the type object (owned by the
// state, already rooted). Returns NULL and sets the error state on failure.
MsObject* msDefineType(MsState* L, const MsTypeDef* def);

// Returns the extra data area of a C type instance, instanceSize bytes,
// zero-initialized at construction. obj must be an instance of a type
// created by msDefineType; anything else is a programming error
// (MS_ASSERT in debug builds).
void* msCInstanceData(MsObject* instance);
```

构造约定（对 09-c-api §10 未明言处的补全）：脚本调用 `Counter(10)` 时，VM 先分配实例（数据区清零），再以 `argv[0]` 为新实例、其余元素为构造实参调用 `init`；`init` 返回 `NULL` 表示构造失败（错误状态已设置，实例随之成为垃圾），返回非 `NULL` 值被忽略（约定返回 `argv[0]` 或 nil）。`init` 为 `NULL` 时类型不接受构造实参（`Counter()` 之外的调用由 VM 抛 `TypeError`）。

### 内部结构体

内部结构体一律 `struct MsFoo` 不 typedef（10-c-style §4）：

```c
// src/object/ms_cfunc.h（若任务 18 尚未建立 C 函数对象，则在本任务落地；已建立则复用并对齐命名）
struct MsCFunc {
  struct MsObjectHeader header;  // tag MS_TYPE_C_FUNCTION
  MsCFunction func;
  const char* name;              // owned copy, for diagnostics and repr
  const char* doc;               // owned copy, may be NULL
};

// src/object/ms_ctype.h
struct MsCType {
  struct MsObjectHeader header;  // script-visible type object
  struct MsString* name;         // GC-managed copy of def->name
  size_t instanceSize;
  MsCFunction init;              // may be NULL
  void (*finalize)(MsObject* obj);
  MsObject* (*toString)(MsState* L, MsObject* obj);
  struct MsDict* methods;        // name -> struct MsCFunc, for attribute lookup
};

struct MsCInstance {
  struct MsObjectHeader header;  // tag MS_TYPE_C_TYPE, header.type points at the MsCType
  // instanceSize bytes of extra data follow inline (single allocation).
};
```

- `struct MsCInstance` 与其数据区**单次分配**：`msAlloc(sizeof(struct MsCInstance) + def->instanceSize)`，数据区清零；`msCInstanceData` 返回头后地址。这样 GC 只需一次 `msFree`，无嵌套释放问题。
- 类型对象由 `msDefineType` 创建后挂入 `MsState` 的内建类型表（成为根），扩展随后通常把它设为自家模块的属性（`msSetAttr` 或注册进 `MsModuleDef` 之外的模块对象），脚本经 `m.Counter` 取得。

### 静态注册流程（msRegisterModule）

1. 校验 `def`（上文），失败设错误状态返回 `MS_ERROR_RUNTIME`。
2. 查注册表，重名报错。
3. 创建模块对象，遍历 `methods` 逐项包装 `struct MsCFunc` 并设为模块属性；写入 `__doc__`。
4. `msModuleRegistryAdd(L, def->name, module)`，返回 `MS_OK`。

### 动态库加载流程

挂接在任务 24 的模块解析流程中：内建/已注册与脚本路径候选（`.ms` / `__init__.ms`）全部未命中后，对同一组搜索目录依次尝试动态库候选 `<dir>/<path>.<ext>`，其中 `<ext>` 按平台为 `dll`（Windows）/ `so`（Linux 等）/ `dylib`（macOS），`<path>` 为导入路径原文（子路径如 `a/b/c` 对应 `<dir>/a/b/c.so`）。命中后：

1. `msDynloadOpen` 打开共享库，失败记录原因继续下一候选；全部候选失败转 `ImportError`（错误消息汇总各候选原因）。
2. 取导入路径最后一段（`a/b/c` → `c`）拼出符号名 `mslangInit_c`，`msDynloadSym` 解析；缺符号报 `ImportError`。
3. 调用入口函数取得 `const MsModuleDef*`；校验 `def->name` 与模块全名一致（子路径模块以路径形式命名，如 `a/b/c`），不一致报 `ImportError`，防止张冠李戴。
4. 走 `msRegisterModule` 的 3–4 步完成注册；此后重复导入命中注册表缓存，不再触碰共享库。
5. 已打开的库句柄挂入 `MsState` 的已加载库链表，**v0.2 不卸载**：库中的 `MsCFunc`/`struct MsCType` 代码地址可能被存活对象引用，`dlclose` 时机无法安全界定（与 CPython 同款取舍）；句柄随进程退出由 OS 回收。此处显式注明为已知限制。

### 最小动态加载（任务 42 前的过渡实现）

平台相关代码只允许在 `src/platform/`（10-c-style §9），故本任务先落三函数最小面，Windows 用 `LoadLibrary`/`GetProcAddress`，其余平台用 `dlopen`/`dlsym`：

```c
// src/platform/ms_dynload.h — minimal shim, folded into the platform
// layer proper by task 42 (naming subject to that task's document).
void* msDynloadOpen(const char* path);        // NULL on failure
void* msDynloadSym(void* handle, const char* symbol);  // NULL on failure
const char* msDynloadError(void);             // static buffer, last error text
```

### C 函数调用约定

VM 调用 `MS_TYPE_C_FUNCTION` 对象时：收集实参为连续 `MsObject*` 数组（实参自动是根，09-c-api §3），调用 `func(L, argc, argv)`；返回 `NULL` 且错误状态已设置时 VM 转为脚本异常抛出，返回 `NULL` 而无错误状态属扩展编程错误（debug 构建 `MS_ASSERT`）；非 `NULL` 返回值压回求值栈。`struct MsCType` 的方法调用把实例作为 `argv[0]`（self），与脚本 class 的 `self` 约定一致。

### C 自定义类型与 GC 的集成

- **标记**：`struct MsCInstance` 的数据区对 GC 不透明，标记阶段无子引用可遍历。由此引出显式限制：v0.2 的 C 类型**不得在数据区持有脚本对象**（无 traverse 回调）；确需持有时必须改用 `msRootPush` 全程钉住，文档与评审清单固定包含此项。扩展数据区持有的 `MsObject*` 若在 GC 扫描时不可达，将被回收形成悬垂指针。
- **清除**：sweep 遇到 `MS_TYPE_C_TYPE` 实例时，先取其 `header.type` 取得 `struct MsCType`，`finalize` 非 `NULL` 则以实例指针调用之，随后 `msFree` 整块分配。
- **finalize 纪律**（09-c-api §10，逐条落实为评审项）：仅释放 C 侧资源（关闭 fd、`free` 外部内存、递减外部引用计数）；不得访问任何其他脚本对象（回收顺序未定义）；不得分配 GC 对象（sweep 进行中）；不得调用任何 `msRaise*`；不得阻塞。`finalize` 内可用 `msCInstanceData(obj)` 取数据区——这是唯一允许对 `obj` 做的操作。
- **toString**：脚本 `str()`/`print` 命中 C 类型实例时，若类型提供 `toString` 则调用之（可正常分配与抛错，此时不在 GC 内），未提供则回退默认表示 `<Counter object>`。

### 示例扩展模块（测试夹具）

`tests/fixtures/ctestext/ctestext.c`，CMake 构建为共享库目标 `ctestext`（产物只落 `build/`），模块名 `ctestext`，内容：

- 函数 `add(a, b)`：两 int 相加，演示参数校验 + `msRaiseTypeError`；
- C 类型 `Counter`：`instanceSize` 存一个 `int64_t` 计数；`init(start)` 演示构造约定；方法 `incr(n)`、`value()`；`toString` 返回 `f"Counter({v})"` 等价字符串；`finalize` 把一个进程级计数器加一并写入环境变量指定的文件（供 C 集成测试断言 finalize 真的被调用）；
- 导出 `mslangInit_ctestext` 返回 `MsModuleDef`。

该夹具同时是 09-c-api §9 示例风格的完整可编译参照，供扩展作者模仿。

## 实现步骤

1. 落地 `include/mslang/module.h` 与 `include/mslang/ctype.h` 公开声明（含文档注释），挂入伞头文件 `mslang.h`。验证：头文件自包含编译通过，guard 与命名通过 10-c-style 检查。
2. 实现 `struct MsCFunc`（若任务 18 未建）与 VM 对 `MS_TYPE_C_FUNCTION` 的调用约定。验证：C 集成测试中手工注册的函数可被脚本调用并返回值。
3. 实现 `msRegisterModule`：校验、重名拒绝、模块对象创建、方法包装、注册表挂载。验证：静态注册后脚本 `import "m"` 命中；重复注册返回 `MS_ERROR_RUNTIME`。
4. 实现 `src/platform/ms_dynload.{c,h}` 三函数最小面（Windows/POSIX 两路）。验证：C 集成测试打开夹具库、解析符号、读取错误文本。
5. 把动态库候选挂进模块解析流程（任务 24 的挂接点）：候选枚举、`mslangInit_<name>` 约定、def->name 一致性校验、句柄链表。验证：脚本 `import "ctestext"` 经 `MS_PATH` 命中共享库，重复导入走缓存。
6. 实现 `msDefineType` / `msCInstanceData`：类型对象与实例布局、单次分配、`init` 构造约定、方法表属性查找、`toString` 接入 `str()`。验证：脚本侧 `Counter(10)`、`c.incr(5)`、`str(c)` 全通。
7. GC 集成：sweep 的 `MS_TYPE_C_TYPE` 分支调 `finalize` 后整块 `msFree`；标记侧确认无子引用。验证：C 集成测试构造垃圾实例后 `msGCCollect`，断言 finalize 计数递增且分配统计无泄漏。
8. 完成 `tests/fixtures/ctestext/ctestext.c` 夹具与 CMake 共享库目标，配通 `run_tests.py`/`ctest` 的 `MS_PATH` 指向夹具产物目录。验证：全部 ms 脚本测试经统一驱动通过。

## 测试方案

本任务采用 C 集成测试 + ms 脚本测试双层（语言级行为已可用脚本断言，注册/GC 等内部行为仍需 C 层驱动）。任务 40 的 `testing` 模块尚未就位，ms 脚本一律用内建 `assert` + `print`。

### C 集成测试：`tests/c/test_c_extension.c`

用任务 02 的 `ms_test.h`（`MS_TEST` / `MS_ASSERT_EQ`），每个用例自建 `MsState`：

- 静态注册：合法 `MsModuleDef` 注册成功后 `msGetGlobal`/`msEvalString("import ...")` 可取到模块；缺 `name`、方法表缺哨兵、`func` 为 `NULL` 各负例返回 `MS_ERROR_RUNTIME`；重名注册拒绝。
- C 函数调用约定：argc/argv 透传、返回 `NULL`+错误状态转脚本异常、参数自动是根（函数体内触发分配后参数仍有效）。
- `msDefineType`：类型对象非空且已入根；实例数据区清零且大小正确；`msCInstanceData` 对非 C 类型实例在 debug 构建触发 `MS_ASSERT`。
- GC/finalize：创建若干实例、丢弃引用、`msGCCollect` 后断言夹具/桩类型的 finalize 计数与释放次数精确相等；`msCloseState` 路径上存活实例的 finalize 同样被调用；分配统计全程无泄漏。
- 动态加载：`msDynloadOpen` 打开夹具库成功、`msDynloadSym("mslangInit_ctestext")` 非空、打开不存在路径返回 `NULL` 且 `msDynloadError` 非空。

### ms 脚本测试：`tests/ms/c_extension/`

由仓库根 `run_tests.py` 调用 `mslang` CLI 驱动，运行环境把夹具库产物目录加入 `MS_PATH`：

- `test_import_native.ms`：`import "ctestext"` 与 `import "ctestext" as ce`；`add(2, 3) == 5`；`add("a", 1)` 抛 `TypeError`（try/except 断言，异常系统任务 23 已就位）；重复 `import` 返回同一模块对象（函数身份相等）；导入不存在的模块抛 `ImportError`。
- `test_ctype_counter.ms`：`Counter(10)` 构造、`incr(5)` 后 `value() == 15`、`str(c)` 内容；无参构造走默认（`init` 对缺省 `start` 的处理）；`Counter(1, 2)` 多实参抛 `TypeError`；错误类型实参抛 `TypeError`；实例方法经属性访问取得并可调用。
- `test_ctype_gc.ms`：循环创建大量短命 `Counter` 实例并穿插分配以越过 GC 阈值，断言进程不崩溃、行为正确（finalize 计数的精确断言由 C 集成测试负责，脚本侧只验证正确性存活）。

### 覆盖点核对

静态注册正反例、动态加载正反例与符号缺失、构造约定（缺省/多参/错型）、方法调用与 self 约定、`toString` 有无两路、finalize 触发与纪律、GC 无泄漏、模块缓存。覆盖不到项：`dlclose`（v0.2 明确不卸载，见「详细设计」）。

## 验收标准

- [ ] `include/mslang/module.h` / `include/mslang/ctype.h` 存在，guard 分别为 `MSLANG_INCLUDE_MSLANG_MODULE_H_` / `MSLANG_INCLUDE_MSLANG_CTYPE_H_`，自包含且被伞头文件聚合，文档注释覆盖功能/参数/返回值/错误行为/根纪律。
- [ ] `MsCFunction` / `MsMethodDef` / `MsModuleDef` / `MsTypeDef` 与 `msRegisterModule` / `msDefineType` / `msCInstanceData` 签名与 09-c-api §9/§10 一致；代码风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体 `struct MsCFunc`/`struct MsCType`/`struct MsCInstance` 不 typedef）。
- [ ] 静态注册成功路径与全部校验负例（缺 name、缺哨兵、重名）行为符合「详细设计」；已注册模块不可被搜索路径上的同名文件遮蔽。
- [ ] 动态库加载按 `<dir>/<path>.<dll|so|dylib>` + `mslangInit_<name>` 约定工作，符号缺失与 def->name 不符均报 `ImportError`；句柄挂入 `MsState` 链表且不卸载（已知限制已在文档注明）；`src/platform/ms_dynload.*` 为任务 42 前的最小过渡实现并注明。
- [ ] C 自定义类型单次分配、数据区清零、`init`/`finalize`/`toString` 三回调语义与「详细设计」一致；`finalize` 仅释放 C 侧资源的纪律写入评审项。
- [ ] GC 集成：sweep 对 `MS_TYPE_C_TYPE` 实例先 `finalize` 后整块 `msFree`；「数据区不得持有脚本对象」限制在文档与示例注释中显式声明。
- [ ] 示例夹具 `tests/fixtures/ctestext/ctestext.c` 可编译为共享库（产物只落 `build/`），函数、C 类型、finalize 计数齐备。
- [ ] `tests/c/test_c_extension.c` 与 `tests/ms/c_extension/`（3 个脚本）覆盖「测试方案」全部清单项并全部通过；ms 脚本经 `run_tests.py` 驱动、`MS_PATH` 配通。
- [ ] 无 TBD/TODO 占位；对任务 18/24/42 的接口假定在实现时已按对应任务文档对齐命名。
