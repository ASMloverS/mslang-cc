# 24 模块系统与 import

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [23 异常系统](23-exceptions.md) |

## 任务目标

交付 mslang 的模块系统（`src/module/ms_module.h` / `src/module/ms_module.c`）：模块对象类型（`MS_TYPE_MODULE`）、模块注册表（`modules`）、导入路径解析器与模块执行器，并把任务 04 已解析的 import 三种 AST 形式接线到编译器与 VM（`MS_OP_IMPORT` / `MS_OP_IMPORT_FROM`）。语义完整覆盖 `docs/language/05-modules.md`：

- `import "a/b/c"`（绑定末段名 `c`）、`import "m" as x`、`from "m" import a, b`（含括号多名形式）；
- 解析顺序：内建/已注册模块 → 相对路径（以导入方文件目录为基准）→ `MS_PATH` 目录列表 → 解释器安装目录 `lib/`；路径候选依次为 `x.ms` 与 `x/__init__.ms`（包）；
- 首次导入执行一次并缓存（键为规范化绝对路径或内建模块名），重复导入返回同一对象；循环导入拿到部分初始化的模块对象；
- 每个模块有内建变量 `__name__`（入口脚本为 `"__main__"`，被导入时为路径形式名）与 `__file__`；
- C 内建/扩展模块与脚本模块共用同一注册表，已注册模块优先级高于搜索路径，不可被同名脚本文件遮蔽。

完成后，任务 33（C 扩展）获得动态库候选的解析挂接点与注册表原语，任务 56（importlib）获得运行期导入/重执行原语，标准库脚本模块（`lib/`）与多文件用户项目可用。本任务通过 `tests/ms/modules/` 的 ms 脚本测试与 `tests/fixtures/modules/` 的多文件夹具验证。

## 设计依据

- `docs/language/05-modules.md`（本任务的范围唯一来源）
  - §1 导入形式：三种语句形式与绑定规则（末段名、别名、from 绑定模块内名字）；不支持 `from "m" import *`（刻意删减）。
  - §2 模块解析：四级解析顺序；`MS_PATH` 的平台分隔符（Windows `;`，其余 `:`）；`a/b/c` 依次尝试 `c.ms` 与 `c/__init__.ms`。
  - §3 包：`__init__.ms` 即包；`import "a/b/c"` 优先匹配子模块文件；包内相对导入 `./sibling`、`../other`。
  - §4 执行语义：首次导入执行一次并缓存；模块级变量即模块属性；循环导入给出部分初始化模块对象，顶层立即访问未定义名字抛 `AttributeError`。
  - §5 主模块判定：`__name__ == "__main__"` 惯用法。
  - §6 动态导入与内省：`importlib.importModule` 与 `dir(m)`（前者由任务 56 实现，本任务提供原语；`dir` 对模块枚举属性由任务 10 的内建函数自然支持，本任务保证模块属性可枚举）。
  - §7 与 C 扩展的关系：共用注册表与解析顺序；C 模块在内建注册表中优先级高于搜索路径，不可被同名脚本遮蔽（安全考虑）。
- `docs/language/03-syntax.md` §7：import 三种形式的 EBNF；`from` 形式无 `as` 别名（语法层即无此产生式）；import 路径必须是字符串字面量（任务 04 parser 已强制）。
- `docs/language/04-exceptions.md` §4：异常层级含 `ImportError` 与 `AttributeError`；§5：顶层未捕获异常打印 traceback 并以非零码退出（负向测试的退出码依据）。
- `docs/language/08-vm-internals.md` §2.1：`struct MsProto` 已有 `sourceFile` 字段（相对导入的基准目录来源）；§2.2：指令集已列 `MS_OP_IMPORT` / `MS_OP_IMPORT_FROM`；§3：`MsTypeTag` 含 `MS_TYPE_MODULE`、`struct MsObjectHeader` 布局；§5：GC 根集合含模块注册表。
- `docs/language/09-c-api.md` §3：GC 根栈纪律；§8：错误处理约定（C 侧置错误状态返回失败，VM 转为脚本异常）；§9：`MsModuleDef` / `msRegisterModule`（注册产物即 `MS_TYPE_MODULE` 模块对象，其创建由任务 33 的 `msRegisterModule` 实现，模块对象的内部布局由本任务定义）。
- `docs/language/10-c-style.md`：§1 include guard、§2 格式化、§3 命名、§4 内部结构体不 typedef、§6 堆分配只经 `msAlloc/msRealloc/msFree`、§9 平台相关代码只允许出现在 `src/platform/`。
- `docs/language/11-project-layout.md` §1：`tests/fixtures/` 为测试数据目录；§4：脚本测试层约定。
- [33-c-extension.md](33-c-extension.md)：假定本任务提供注册表原语 `msModuleRegistryFind` / `msModuleRegistryAdd` 与解析流程的「动态库候选」挂接点；[56-stdlib-importlib.md](56-stdlib-importlib.md)：假定本任务提供 `msModulesImportPath` / `msModulesLookupByName` / `msModulesExecFile` 与模块对象的 `__name__` / `__file__` 属性。本文**定名即采用这些假定名**，两份文档的假定由此确认，不再变更。
- [20-stdlib-strings.md](20-stdlib-strings.md) §6：v0.1 期间 C 标准库模块经全局命名空间过渡暴露，「任务 24 落地后是否保留该全局预绑定，以任务 24 文档定稿为准」——本文「详细设计」第 6 节定稿为**移除**。
- [22-cli-polish.md](22-cli-polish.md)：`run_tests.py` 的 `.env` / `.cliargs` 同伴文件机制（本任务测试用以注入 `MS_PATH`）。
- [23-exceptions.md](23-exceptions.md)：提供统一抛出入口 `msVmRaise` 与按内建异常 id 构造并抛出的 `msVmRaiseFmt(L, id, fmt, ...)`；本任务的 `ImportError`/`AttributeError` 抛出点使用其 `MsExceptionId` 枚举值 `MS_EXC_IMPORT_ERROR` / `MS_EXC_ATTRIBUTE_ERROR`。任务 07/08 的编译入口与 VM 执行入口（本文记为 `msCompileFile` / `msVmRunModule`）、任务 06 的 dict/字符串内部构造接口均为假定命名，实现时以对应任务文档定名为准。

## 详细设计

### 1. 文件布局

```
src/module/
├── ms_module.h          # 模块对象布局、注册表与导入原语（guard MSLANG_SRC_MODULE_MS_MODULE_H_）
└── ms_module.c          # 注册表、解析器、模块执行器；搜索目录枚举与候选拼接为文件内 static
src/platform/
├── ms_path.h            # 最小路径原语（guard MSLANG_SRC_PLATFORM_MS_PATH_H_；任务 42 并入平台层）
└── ms_path.c            # Windows/POSIX 两路实现
```

平台相关代码只允许出现在 `src/platform/`（10-c-style §9），路径拼接/规范化/可执行文件定位等最小原语落在 `src/platform/ms_path.*`，属任务 42 前的过渡实现，与任务 33 的 `ms_dynload.*` 同款安排，接口刻意收窄，任务 42 落地时并入平台层统一命名。

任务 33 的 `src/module/ms_native_module.*`（动态库加载）与本任务的 `ms_module.*` 同目录、单向依赖：动态库候选挂接在本任务解析流程的脚本候选之后（见第 3 节）。

### 2. 模块对象与命名空间

内部结构体不 typedef（10-c-style §4）：

```c
// src/module/ms_module.h
struct MsModule {
  struct MsObjectHeader header;   // tag MS_TYPE_MODULE
  struct MsDict* attrs;           // 模块命名空间：顶层环境即模块属性表
};
```

- 模块顶层语句在 `attrs` 命名空间内执行：编译器把模块级变量解析为该 dict 上的全局槽（`MS_OP_LOAD_GLOBAL` / `MS_OP_STORE_GLOBAL` 的作用对象从「单一全局表」改为「当前帧所属模块的 `attrs`」，VM 帧需记录所属模块指针——对任务 08 帧结构的扩展属本任务范围，字段假定名 `module`）。
- 元数据存于 `attrs` 中的普通字符串键，而非结构体字段（脚本可读写，与 Python 一致）：
  - `__name__`：模块名。入口脚本为 `"__main__"`；经搜索路径/内建导入的为导入路径原文（如 `"strings"`、`"encoding/json"`）。
  - `__file__`：源文件的规范化绝对路径；内建 C 模块为 nil。
  - `__doc__`：模块文档串；内建 C 模块取 `MsModuleDef.doc`，脚本模块默认 nil（首行注释提取列入路线图，不在本任务）。
- 属性访问 `m.x` 即 `attrs` 的 dict 查找，未命中抛 `AttributeError`（任务 06/08 的属性分派按 `MS_TYPE_MODULE` 落到该 dict；若任务 08 尚未实现模块分支，在本任务补齐）。
- `MsModule.attrs` 在 GC 标记阶段作为子引用遍历；注册表本身是根（08-vm-internals §5），注册表内模块无需额外入根。

### 3. 模块注册表

每个 `MsState` 持有一个注册表 dict（`L->modules`，键为 str、值为模块对象；假定任务 06 的 dict 内部接口，定名以任务 06 文档为准）。键空间分两类：

- **名键**：内建/已注册模块以注册名（`MsModuleDef.name`，如 `"strings"`）为键；文件模块额外以**导入路径原文**（如 `"encoding/json"`，相对导入含 `./`、`../` 前缀的原文）为名键。
- **路径键**：文件模块以源文件的规范化绝对路径为路径键（去重与循环导入判定的语义键）。

文件模块注册时同时写入路径键与名键，两者指向同一模块对象。名键冲突（同一导入路径原文经不同入口解析到不同文件，仅可能发生于相对导入）时保留先注册者——路径键才是语义键，名键仅供按名查找与任务 56 的 reload 身份核验，此简化在本文显式注明。

```c
// ---- 注册表原语（供本模块、任务 33 的 msRegisterModule、任务 56 使用） ----

// Looks up key (a registered name, an import path as written, or a
// canonical absolute path) in the state's module registry. Returns NULL
// when absent; never sets the error state.
MsObject* msModuleRegistryFind(MsState* L, const char* key);

// Inserts key -> module. Returns MS_ERROR_RUNTIME with the error state set
// when key is already bound to a different module; re-adding the same
// module under an additional key is a no-op success.
MsResult msModuleRegistryAdd(MsState* L, const char* key, MsObject* module);

// Removes key when present (used to roll back a failed first import).
void msModuleRegistryRemove(MsState* L, const char* key);

// Alias of msModuleRegistryFind restricted to name keys; provided under
// the name assumed by task 56 (reload identity check).
MsObject* msModulesLookupByName(MsState* L, const char* name);
```

`msRegisterModule`（任务 33）经 `msModuleRegistryAdd` 写入名键；其重名拒绝语义（05-modules §7 的防遮蔽意图）由 `msModuleRegistryAdd` 的「异对象冲突报错」自然支撑。

### 4. 路径解析器

解析入口（文件内 `static`）：给定导入路径原文与导入方目录，产出 `{命中文件的规范化绝对路径, 解析所用的搜索目录类别}`，或报告未命中。

**路径分类**（按原文前缀判定，`/` 与 `\` 均接受，先归一为 `/` 再判定）：

1. 相对路径：以 `./` 或 `../` 开头。基准目录为**导入方文件的目录**（见第 5 节的导入方判定）；基准缺失（REPL 片段、`-e` 片段，其 chunk 名非文件路径）时报 `ImportError`。
2. 其余一律为搜索路径形式：含内建名（`"strings"`）、子路径（`"encoding/json"`）、保留的外部包路径（`"github.com/user/lib"`——v1 不下载，仅按普通子路径在搜索目录中查找，未命中即 `ImportError`）。

**解析顺序**（05-modules §2，首个命中者胜）：

```
resolve(path, importerDir):
  if 相对路径:
      candidates = [ normalize(join(importerDir, path)) ]
  else:
      1. msModuleRegistryFind(path) —— 内建/已注册模块，命中即返回（不产生产文件路径）
      2. candidates = [ join(dir, path) for dir in searchDirs ]
         其中 searchDirs = MS_PATH 目录列表 ++ [ 安装目录 lib/ ]
  for cand in candidates:
      try  cand + ".ms"                // 模块文件（优先，05-modules §3）
      try  cand + "/__init__.ms"       // 包
  // 任务 33 挂接点：以上全部未命中后，对同一组搜索目录尝试动态库候选
  // <dir>/<path>.<dll|so|dylib>（本任务不实现，仅保留调用点与注释）
  全部未命中 → ImportError
```

要点：

- **C 模块不可遮蔽**：注册表查找位于一切文件候选之前，搜索路径上的同名 `strings.ms` 永远不会被读取（§7 安全语义）。规范 §2 第 1 步的措辞把「`lib/` 纯脚本模块」也归入「内建/已注册」，与同节第 3 步把安装目录 `lib/` 列在 `MS_PATH` 之后存在张力；本任务取**字面顺序**：`lib/` 脚本模块经第 3 步解析、可被 `MS_PATH` 同名模块遮蔽，只有 C 注册模块不可遮蔽（§7 只声明 C 模块的安全性质）。此歧义处理在此显式注明。
- **`MS_PATH` 每次导入时重新读取**（不缓存）：按平台分隔符切分（Windows `;`，其余 `:`），空段跳过。任务 56 的测试依赖运行期经 `os.setEnv` 修改 `MS_PATH` 生效，故不做启动期快照。
- **安装目录 `lib/`**：取可执行文件所在目录的上一级下的 `lib/`（标准布局 `bin/mslang` + `lib/`），经 `msPathExeDir` 定位；定位失败（嵌入场景无标准布局）时该搜索目录静默缺席，不视为错误。
- **规范化**：候选命中后统一经 `msPathNormalize` 转为规范化绝对路径作为路径键——折叠 `.`/`..`、统一分隔符为 `/`、相对路径基于 cwd 绝对化；Windows 下键比较按大小写不敏感折叠（盘符与路径段统一小写后作键）。**不解析符号链接**（v0.2 简化：同一文件经符号链接双路径导入会被加载两次，列为已知限制）。

### 5. 平台路径原语（过渡实现）

```c
// src/platform/ms_path.h — minimal shim, folded into the platform layer
// proper by task 42 (naming subject to that task's document).
// All returned strings are msAlloc'd; the caller frees them with msFree.

bool   msPathIsAbsolute(const char* path);
bool   msPathIsFile(const char* path);          // regular file exists and is readable
char*  msPathJoin(const char* dir, const char* name);
char*  msPathDirName(const char* path);
char*  msPathNormalize(const char* path);       // absolute, "." / ".." folded, '/' separators
char*  msPathExeDir(void);                      // directory of the running executable, NULL on failure
```

### 6. 导入执行与缓存

公开原语：

```c
// Imports the module named by path, resolving relative paths against
// importerDir (NULL for non-file chunks such as REPL / -e snippets).
// On success *outModule receives the (possibly partially initialized)
// module object and MS_OK is returned. Unresolvable paths raise
// ImportError; exceptions raised by the module's top-level code propagate
// unchanged with the registry rolled back.
MsResult msModulesImportPathFrom(MsState* L, const char* path, const char* importerDir,
    MsObject** outModule);

// importlib.importModule entry: equivalent to msModulesImportPathFrom with
// importerDir == NULL (task 56 rejects relative paths itself).
MsResult msModulesImportPath(MsState* L, const char* path, MsObject** outModule);

// Re-executes filePath's top-level code in module's existing namespace
// (importlib.reload primitive); the module stays registered throughout.
MsResult msModulesExecFile(MsState* L, MsObject* module, const char* filePath);

// Creates the entry module for a directly-run script: __name__ "__main__",
// __file__ the canonical script path, NOT inserted into the registry.
MsObject* msModulesNewMain(MsState* L, const char* scriptPath);
```

首次导入流程（`msModulesImportPathFrom` 的核心，失败路径均为置错误状态返回非 `MS_OK`）：

```
1. path 为空串 → ImportError
2. resolve(path, importerDir)（第 4 节）：
   - 命中注册表（内建/已注册/缓存名键）→ 直接返回该模块对象
   - 命中文件 → 得 absPath；msModuleRegistryFind(absPath) 命中 → 返回缓存对象
     （含循环导入的部分初始化对象：注册在先、执行未完的同一对象）
3. 新建模块对象（msRootPush 保护），attrs 预置 __name__（名键，即导入路径原文）、
   __file__（absPath）、__doc__（nil）
4. 先注册后执行：msModuleRegistryAdd(absPath) + msModuleRegistryAdd(名键)
5. 编译并执行顶层：msCompileFile(absPath) 得顶层 MsProto，msVmRunModule(module, proto)
   在 module->attrs 命名空间内顺序执行
6. 执行失败（语法错误/运行时异常）→ msModuleRegistryRemove 两键回滚，异常原样传播，
   下次 import 重新尝试完整加载（对齐 Python 的失败不缓存语义）
7. 成功 → msRootPop，返回模块对象
```

- **循环导入**：A 顶层执行中导入 B、B 顶层导入 A 时，第 2 步命中 A 的路径键缓存，B 拿到部分初始化的 A；B 顶层立即访问 A 尚未定义的名字抛 `AttributeError` 并沿导入链传播（A 的首次导入随之失败回滚）。规范建议的「函数内延迟访问」模式天然可用：函数体执行时 A 已初始化完毕。
- **导入方判定**：VM 执行 `MS_OP_IMPORT` 时，导入方目录取当前帧 `proto->sourceFile` 的 `msPathDirName`；包内 `__init__.ms` 的目录名即包目录，`./sibling`、`../other` 因此自然正确（05-modules §3）。REPL/`-e` 的 chunk 名（`"<repl>"` / `"-e"`）非文件路径，相对导入报 `ImportError`。
- **入口模块**：CLI（任务 09/22）直接运行的脚本改为经 `msModulesNewMain` 创建 `__main__` 模块并在其命名空间内执行；`__main__` **不写入注册表**（任务 56 据此对 `reload(__main__)` 报 `ImportError`）。被导入模块的 `__name__` 为导入路径原文——规范 §5 只示例了搜索路径形式，本任务明确化：相对导入的 `__name__` 亦为导入路径原文（含 `./`、`../` 前缀），路径键（绝对路径）不受命名形式影响。
- **v0.1 全局预绑定的移除**（对任务 20 §6 待定项的定稿）：本任务落地后，`msStringsRegister` 等 v0.1 注册入口不再把模块对象写入全局命名空间，标准库统一经 `import` 取得；任务 19/20/21 的既有脚本测试在文件头部补相应 `import`（属本任务的连带修改，逐文件仅加导入行）。
- **GC 纪律**：注册表 dict 是 GC 根；第 3–5 步中模块对象跨编译/执行存活，须 `msRootPush`/`msRootPop`；解析过程的 C 字符串（候选路径、规范化结果）全部 `msAlloc`/`msFree` 配对，失败路径按获取逆序释放。

### 7. 编译器与 VM 接线

import 语句的 AST（任务 04 的 `MS_AST_IMPORT` / `MS_AST_FROM_IMPORT`）由任务 07 的编译器扩展编译（任务 07 文档尚不存在，以下为其必须满足的契约，实现时以任务 07 文档对齐）：

- `import "a/b/c"`：编译期取路径末段名（最后一个 `/` 之后），发射 `MS_OP_IMPORT`（A = 路径字符串常量索引），随后按当前作用域发射对该名字的 store（模块级 → `MS_OP_STORE_GLOBAL`，函数内 → 局部槽）。
- `import "a/b/c" as x`：同上，绑定名为别名 `x`。
- `from "a/b" import n1, n2`（含括号形式与尾逗号）：发射 `MS_OP_IMPORT_FROM`（A = 路径常量索引，Bx = 名字元组常量索引），随后依次发射各名字的 store。`from` 形式无别名语法（03-syntax §7）。
- `from "m" import *` 无产生式，parser 已拒绝（05-modules §1 的刻意删减在语法层落地）。

VM 指令语义（任务 08 的 VM 扩展，契约同上）：

| 指令 | 操作数 | 语义 |
|---|---|---|
| `MS_OP_IMPORT` | A = 路径常量索引 | 以当前帧 `proto->sourceFile` 的目录为 `importerDir` 调 `msModulesImportPathFrom`；成功把模块对象压栈；`ImportError` 或模块顶层异常按任务 23 的机制抛出 |
| `MS_OP_IMPORT_FROM` | A = 路径常量索引，Bx = 名字（str）元组常量索引 | 同上导入模块，再对元组中每个名字做模块属性查找并把属性值**按序压栈**；名字缺失抛 `AttributeError`（消息含模块名与属性名） |

两条指令均不改变栈上的模块对象本身（`MS_OP_IMPORT_FROM` 只压属性值），绑定动作完全由后续 store 指令完成，编译器可见的栈效应在任务 05 的字节码文档中补记。

### 8. 错误语义汇总

| 情形 | 行为 |
|---|---|
| 导入路径为空串 | `ImportError` |
| 路径无法解析（注册表与全部候选未命中） | `ImportError`，消息含路径原文与各尝试候选 |
| REPL/`-e` 中的相对导入 | `ImportError`（无导入方文件目录） |
| `from "m" import x` 而 `x` 不存在 | `AttributeError` |
| 循环导入中顶层访问未定义名字 | `AttributeError`（沿导入链传播，相关模块回滚出注册表） |
| 被导入模块语法错误 / 顶层抛异常 | 诊断/异常原样传播，注册表回滚，不缓存失败模块 |
| 重复导入 | 返回同一模块对象（`is` 恒等），不重复执行 |

## 实现步骤

1. 建 `src/platform/ms_path.{c,h}` 六个最小路径原语（Windows/POSIX 两路）。验证：CMake 三平台编译通过；规范化对 `a/./b/../c`、混合分隔符、Windows 盘符大小写的用例正确（可先在 `tests/c/` 落少量 C 用例，或经后续脚本测试间接覆盖）。
2. 建 `src/module/ms_module.{c,h}`：`struct MsModule`、模块对象构造（`__name__`/`__file__`/`__doc__` 预置）、GC 标记遍历、属性分派接入（若任务 08 尚无 `MS_TYPE_MODULE` 分支）。验证：嵌入层手工构造模块并 `msSetAttr`/`msGetAttr` 往返。
3. 实现注册表：`MsState.modules` dict、四个注册表原语、双键插入与冲突规则。验证：同一模块双键查找命中、异对象同键报 `MS_ERROR_RUNTIME`。
4. 实现路径解析器：路径分类、相对基准、注册表优先、`MS_PATH` 逐次读取与平台分隔符切分、安装目录 `lib/`、`x.ms` 优先于 `x/__init__.ms`、候选规范化。验证：脚本经 `.env` 注入 `MS_PATH` 后各搜索目录命中与优先级用例。
5. 实现 `msModulesImportPathFrom` 首次导入流程：缓存命中、先注册后执行、失败回滚、`msModulesImportPath` / `msModulesNewMain` 包装。验证：重复导入 `is` 恒等、顶层只执行一次、失败后可重新导入。
6. VM 与编译器接线：帧记录所属模块、`MS_OP_LOAD_GLOBAL`/`MS_OP_STORE_GLOBAL` 改指模块 `attrs`、两条 IMPORT 指令、import 三形式的字节码生成。验证：`import "strings"` + `strings.toUpper`、别名、from 绑定、函数内局部导入。
7. 入口改造：CLI 脚本模式经 `msModulesNewMain` 执行；移除任务 19/20/21 的全局预绑定并为其既有测试补 `import` 行。验证：`__name__ == "__main__"` 用例、任务 19/20/21 测试全部保持绿色。
8. 循环导入与 `AttributeError` 语义、任务 33 动态库挂接点（空实现 + 注释）。验证：循环夹具与顶层立即访问负例。
9. 编写 `tests/ms/modules/` 全部测试与 `tests/fixtures/modules/` 夹具，`python run_tests.py` 全绿；三平台 × Debug/Release 构建，Debug（ASAN / `/RTC`）下无泄漏（配合任务 02 分配统计）。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试；早于任务 40（testing 模块），脚本用内建 `assert` + `print`，成功脚本末尾 `print("<用例名> ok")`；无法捕获的负向用例以 `<name>.exit` 同伴文件声明预期退出码 `1`。`MS_PATH` 经任务 22 的 `.env` 同伴文件注入（内容为 `MS_PATH=<fixtures 绝对路径>`，驱动器负责把相对路径展开为绝对路径），`-e` 用例经 `.cliargs` 同伴文件注入。本任务只交付本设计文档，测试与夹具随实现编写。

夹具（`tests/fixtures/modules/`）：

- `basic/greeter.ms`：顶层向 `shared_log.entries` 追加一次（计数顶层执行次数），定义常量与函数。
- `basic/shared_log.ms`：顶层 `entries = []`，供各夹具记录执行痕迹。
- `basic/pkg/__init__.ms`、`basic/pkg/sibling.ms`、`basic/pkg/sub.ms`：包结构；`__init__` 内 `import "./sibling"`，`sub` 内 `import "../shared_log"`（父级相对导入）。
- `basic/dual/both.ms` 与 `basic/dual/both/__init__.ms`：同名文件与包并存（验证 `.ms` 优先）。
- `cycle/cycle_a.ms` / `cycle/cycle_b.ms`：相互导入；B 顶层读取 A 已定义名字、函数内延迟读取后定义名字。
- `cycle_hard/hard_a.ms` / `cycle_hard/hard_b.ms`：B 顶层立即访问 A 尚未定义的名字（`AttributeError` 负例）。
- `shadow/strings.ms`：与内建同名的脚本，顶层向 `shared_log` 追加、定义行为不同的 `toUpper`（验证不可遮蔽）。
- `namecheck/named.ms`：顶层把自身 `__name__` 追加到 `shared_log`。
- `guarded/tool.ms`：含 `if __name__ == "__main__"` 保护块（块内向 `shared_log` 追加）。
- `bad/syntax_err.ms`、`bad/raises.ms`：语法错误与顶层抛异常模块（失败回滚用例）。

测试文件（`tests/ms/modules/`）与覆盖点：

- `import_forms.ms`：`import "strings"` 绑定末段名并可调用；`import "strings" as s` 别名；`from "strings" import toUpper, split` 直接绑定；`from "os" import (...)` 括号多名形式留待任务 36 后补充，本任务先用 `strings` 两名验证括号与尾逗号语法；函数内 import 绑定为局部。
- `import_paths.ms`（配 `.env`）：`MS_PATH` 命中（`import "basic/greeter"`）；子路径末段名绑定（`import "basic/pkg"` 绑定 `pkg`）；包 `__init__.ms` 加载；包内 `./sibling` 与 `../shared_log` 相对导入；`dual/both` 的文件优先于包。
- `import_cache.ms`（配 `.env`）：同一脚本内两次 `import "basic/greeter"` 返回同一对象（`is`）；`shared_log.entries` 计数为 1（首次导入只执行一次）；静态 import 与 `from` 导入混用不重复执行。
- `import_builtin_priority.ms`（配 `.env` 指向含 `shadow/` 的目录）：`import "strings"` 得到内建模块（`strings.toUpper("a") == "A"`），`shadow/strings.ms` 未被执行（`shared_log` 无其痕迹）——C 模块不可被搜索路径同名脚本遮蔽。
- `import_circular.ms`（配 `.env`）：导入 `cycle/cycle_a` 成功；B 顶层读到 A 的部分初始化状态、函数内延迟访问成功；导入 `cycle_hard/hard_a` 抛 `AttributeError`（try/except 捕获断言），且失败后 `hard_a`/`hard_b` 均已回滚（再次导入重新触发同样错误而非返回残留对象）。
- `import_main.ms`（配 `.env`）：本脚本自身 `__name__ == "__main__"`；`import "namecheck/named"` 后其记录的 `__name__` 为 `"namecheck/named"`；`import "guarded/tool"` 时保护块未执行（`shared_log` 无痕迹）。
- `import_errors.ms`（配 `.env`）：`import ""`、`import "no/such/module"`、相对导入基准缺失以外的解析失败均抛 `ImportError`（try/except 断言异常类型）；`from "basic/greeter" import noSuchName` 抛 `AttributeError`；`import "bad/raises.ms"` 对应模块的异常原样传播（类型与消息不变）；失败后再次导入同一模块重试执行。
- `import_errors_exit.ms`（配 `.env` 与 `import_errors_exit.exit`，内容 `1`）：不捕获地 `import "no/such/module"`，断言进程以退出码 1 结束。
- `import_repl_relative.ms`（配 `.cliargs` 注入 `-e 'import "./x"'` 与 `.exit` 内容 `1`）：`-e` 片段中的相对导入报 `ImportError` 并以退出码 1 结束。
- `import_failure_retry.ms`（配 `.env`）：`import "bad/syntax_err"` 报语法诊断失败；两次尝试均失败（不缓存失败模块）。

## 验收标准

- [ ] `src/module/ms_module.{c,h}` 与 `src/platform/ms_path.{c,h}` 存在，guard 分别为 `MSLANG_SRC_MODULE_MS_MODULE_H_` / `MSLANG_SRC_PLATFORM_MS_PATH_H_`，头文件自包含，代码风格通过 10-c-style 检查（2 空格缩、120 列、K&R、星号贴类型、`struct MsModule` 不 typedef、堆分配只经 `msAlloc/msRealloc/msFree`、平台代码仅在 `src/platform/`）。
- [ ] 模块对象（`MS_TYPE_MODULE`）以 `attrs` dict 为命名空间，预置 `__name__`/`__file__`/`__doc__`；模块级变量即模块属性，`m.x` 访问与 `dir(m)` 枚举可用；注册表是 GC 根且 `attrs` 参与标记遍历。
- [ ] 注册表双键（路径键 + 名键）语义符合「详细设计」第 3 节；`msModuleRegistryFind`/`msModuleRegistryAdd`/`msModuleRegistryRemove`/`msModulesLookupByName` 与任务 33/56 的假定定名一致。
- [ ] 解析顺序完整实现：注册表优先（C 内建模块不可被同名脚本遮蔽）→ 相对路径（导入方文件目录基准）→ `MS_PATH`（平台分隔符、每次导入重新读取）→ 安装目录 `lib/`；`x.ms` 优先于 `x/__init__.ms`；动态库候选挂接点预留并注释。
- [ ] 首次导入执行一次并缓存（重复导入 `is` 恒等）；先注册后执行支持循环导入的部分初始化语义；执行失败回滚注册表、异常原样传播、重试可行。
- [ ] `__name__` 规则实现：入口脚本 `"__main__"`（不入注册表）、被导入模块为导入路径原文；`__file__` 为规范化绝对路径（内建 C 模块为 nil）。
- [ ] import 三形式（末段名绑定、`as` 别名、`from` 绑定含括号形式）经 `MS_OP_IMPORT`/`MS_OP_IMPORT_FROM` 工作，模块级与函数内绑定作用域正确；`from` 导入缺失名字抛 `AttributeError`。
- [ ] v0.1 标准库全局预绑定已移除，任务 19/20/21 既有测试补 `import` 后全部保持绿色。
- [ ] `tests/ms/modules/` 与 `tests/fixtures/modules/` 覆盖「测试方案」全部清单项，`python run_tests.py` 全绿（含 `.exit` 负向用例），`ctest --test-dir build` 并入通过；构建产物只落在 `build/`。
- [ ] 三平台 Debug/Release 构建通过；Debug（ASAN / `/RTC`）与任务 02 分配统计下无泄漏（解析路径串与失败回滚路径全数释放）。
- [ ] 无 TBD/TODO 占位；对任务 06/07/08 的接口假定（dict 内部接口、`msCompileFile`/`msVmRunModule`、帧 `module` 字段）在实现时已按对应任务文档对齐命名；`ImportError`/`AttributeError` 抛出点使用任务 23 的 `msVmRaiseFmt` 与 `MS_EXC_IMPORT_ERROR`/`MS_EXC_ATTRIBUTE_ERROR`。
