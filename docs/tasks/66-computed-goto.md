# 66 computed goto 分派

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v1.0 | ⬜ | [64 性能基准套件](64-benchmarks.md) |

## 任务目标

为 VM 主分派循环提供 GCC/Clang 的 computed goto（`&&label` / `goto *p`）实现，
作为首版 `switch` 分派之外的编译期可选优化（08-vm-internals §2.2、§7 路线图第 3
项）。两种分派各自独占一个实现源文件，由 CMake 构建选项在配置期二选一编译进
`mslang` target，VM 代码中不出现任何 `#ifdef` 分派分支（10-c-style §9）。MSVC
等不支持该扩展的编译器自动回退到 `switch` 实现，行为与正确性在两套分派下完全
一致。完成后，`cmake -B build -DMSLANG_VM_DISPATCH=GOTO` 产出 goto 版解释器，
默认 `AUTO` 按编译器能力自动选择；两套构建均通过全部既有测试套件，且 goto 版在
任务 64 的基准套件上达到本文规定的性能门槛，方可默认启用。

## 设计依据

- `docs/language/08-vm-internals.md`
  - §2.2 分派方式：首版 `switch` 分派；computed goto 作为非 MSVC 平台的编译期可选
    优化，经 CMake 构建选项选择分派实现源文件，不在 VM 代码中散布 `#ifdef`。
  - §4 VM 执行核心：每个协程持有调用栈（`MsCallFrame` 帧数组）与求值栈；算术指令
    内联 int/float 快路径——分派循环的上下文（帧、pc、栈基址）与快路径分布。
  - §7 性能路线图：computed goto 分派为第 3 项。
- `docs/language/11-project-layout.md`
  - §2 CMake 构建：选项以 `option(...)`/`CACHE STRING` 形式集中在根
    `CMakeLists.txt`，编译产物只落 `build/`。
  - §5 路线图 v1.0：性能优化项"按基准测试选取"——本任务默认策略须以任务 64 的
    基准数据为依据，故依赖任务 64。
- `docs/language/10-c-style.md`：§2 格式化、§3 命名、§4 C 特性规则（内部结构体
  不 typedef、优先 `static inline`）、§8 断言、§9 平台相关代码不散布 `#ifdef`
  的精神同样适用于编译器扩展。
- 任务 64 性能基准套件（`64-benchmarks.md`，尚不存在）：提供基准脚本集、运行入
  口与对比口径；本文假定的基准入口名（`benchmarks/` 目录与 `run_benchmarks.py`
  对比脚本）实现时以对应任务文档定名为准。
- 任务 08 VM 执行核心（`08-vm-core.md`，尚不存在）：提供 `msVmRun`、
  `struct MsCallFrame`、`struct MsCoroutine` 与指令译码宏；本文对既有 VM 接口的
  引用（`msVmRun`、`MS_OP_GET_OPCODE` 等）为假定命名，实现时以对应任务文档定名
  为准。

## 详细设计

### 总体方案：同符号、双实现文件

分派循环从任务 08 的 `src/vm/ms_vm.c` 中剥离，形成三个新文件与一个共享头：

| 文件 | 职责 |
|---|---|
| `src/vm/ms_vm_dispatch.h` | 分派入口与诊断查询的唯一声明，两个实现文件共用 |
| `src/vm/ms_vm_ops.h` | 每条字节码指令的语义处理函数（`static inline`），两个实现文件共享，保证语义单点维护 |
| `src/vm/ms_vm_dispatch_switch.c` | `switch` 分派循环（任务 08 原循环平移而来） |
| `src/vm/ms_vm_dispatch_goto.c` | computed goto 分派循环（本任务新增） |

两个 `.c` 实现同一组导出符号，CMake 配置期只把其中一个加入 `mslang` target 的
源文件列表，因此不存在重定义冲突，也不需要 `#ifdef`。`src/vm/ms_vm.c` 保留 VM
状态管理、帧操作、调用约定等公共逻辑，调用 `msVmDispatchRun` 进入分派循环。

### 分派头文件

`src/vm/ms_vm_dispatch.h`，include guard `MSLANG_SRC_VM_MS_VM_DISPATCH_H_`，
自包含：

```c
// Runs the dispatch loop for coroutine co until it returns, yields, or
// raises. Called by msVmRun (ms_vm.c); exactly one of the two dispatch
// implementation files provides this symbol, selected at configure time.
MsResult msVmDispatchRun(MsState* L, struct MsCoroutine* co);

// Returns "switch" or "computed-goto": the implementation compiled into
// this binary. Used by --version output and build-plumbing tests.
const char* msVmDispatchName(void);
```

### 共享指令语义头 `src/vm/ms_vm_ops.h`

为避免两份分派文件各自复制指令语义，每条指令的处理体抽成一个 `static inline`
函数，集中在本头中（仅被两个分派 `.c` 包含，不进任何公开头）。分派上下文为内部
结构体（不 typedef，10-c-style §4）：

```c
struct MsVmDispatchCtx {
  MsState* L;                 // interpreter state
  struct MsCoroutine* co;     // current coroutine
  struct MsCallFrame* frame;  // top call frame (reloaded after calls/returns)
  const uint32_t* pc;         // next instruction (MsProto.code)
  MsObject** base;            // eval-stack base of current frame
  MsObject** stackTop;        // current eval-stack top
};

// Representative signatures; one function per opcode in MS_OP_*.
static inline MsResult msVmOpLoadConst(struct MsVmDispatchCtx* ctx, uint32_t insn);
static inline MsResult msVmOpAdd(struct MsVmDispatchCtx* ctx, uint32_t insn);
static inline MsResult msVmOpCall(struct MsVmDispatchCtx* ctx, uint32_t insn);
static inline MsResult msVmOpReturn(struct MsVmDispatchCtx* ctx, uint32_t insn);
```

约束：

- 译码宏（`MS_OP_GET_OPCODE(insn)`、`MS_OP_GET_A`、`MS_OP_GET_Bx`、
  `MS_OP_GET_SAX`，任务 05/08 提供）由本头统一引用，两个分派文件不各自定义。
- 处理函数只表达指令语义；帧切换、协程让出、异常展开等会离开分派循环的情形
  返回相应 `MsResult`（或任务 08 约定的内部码），由循环骨架统一处理，保证两个
  实现的控制流等价。
- 算术快路径（08-vm-internals §4：双操作数均为机器字 int/float 直接计算）内联
  在各算术处理函数开头，慢路径调用 `ms_vm.c` 的类型分派。
- `MS_ASSERT` 校验不变量（如 `pc` 不越界、`stackTop >= base`），debug 构建生效。

### switch 实现文件骨架

`ms_vm_dispatch_switch.c` 为任务 08 原循环的机械平移，仅把指令体替换为对
`ms_vm_ops.h` 处理函数的调用：

```c
MsResult msVmDispatchRun(MsState* L, struct MsCoroutine* co) {
  struct MsVmDispatchCtx ctx;
  msVmDispatchCtxInit(&ctx, L, co);
  for (;;) {
    const uint32_t insn = *ctx.pc++;
    switch (MS_OP_GET_OPCODE(insn)) {
      case MS_OP_LOAD_CONST: {
        MsResult r = msVmOpLoadConst(&ctx, insn);
        if (r != MS_OK) { /* unified exit handling */ }
        break;
      }
      // ... one case per opcode ...
      default:
        MS_UNREACHABLE();
    }
  }
}
```

### computed goto 实现文件骨架

`ms_vm_dispatch_goto.c` 使用标签地址表 + 间接跳转，循环骨架：

```c
MsResult msVmDispatchRun(MsState* L, struct MsCoroutine* co) {
  struct MsVmDispatchCtx ctx;
  msVmDispatchCtxInit(&ctx, L, co);

  static const void* const kDispatchTable[] = {
    [MS_OP_LOAD_CONST] = &&opLoadConst,
    [MS_OP_ADD]        = &&opAdd,
    // ... every MS_OP_* exactly once ...
  };
  _Static_assert(MS_OP_COUNT <= 256, "opcode fits dispatch table");

  uint32_t insn;
#define MS_VM_NEXT() \
  do { \
    insn = *ctx.pc++; \
    goto *kDispatchTable[MS_OP_GET_OPCODE(insn)]; \
  } while (0)

  MS_VM_NEXT();

opLoadConst: {
    MsResult r = msVmOpLoadConst(&ctx, insn);
    if (r != MS_OK) { goto opExit; }
    MS_VM_NEXT();
  }
opAdd: {
    MsResult r = msVmOpAdd(&ctx, insn);
    if (r != MS_OK) { goto opExit; }
    MS_VM_NEXT();
  }
// ... one label per opcode, same order and same handler calls as the
// switch version ...
opExit:
  return msVmDispatchHandleExit(&ctx);
}
```

要点：

- `kDispatchTable` 用指定初始化器按枚举值索引，编译期静态保证每个
  `MS_OP_*` 恰有一项；遗漏即编译错误（越界索引诊断 + 严格警告即错误）。
- 每个标签体调用与 switch 版**完全相同**的 `ms_vm_ops.h` 处理函数，语义不可能
  分叉；两个文件的差异只允许存在于取指/跳转骨架。
- 指令体末尾直接 `MS_VM_NEXT()`（尾部分派），消除循环回跳，这是 computed goto
  相对 `switch` 的主要收益来源（更好的分支预测局部性）。
- `MS_VM_NEXT` 宏仅在本文件内定义/使用，不进入共享头——switch 版无对应概念，
  避免为抽象而抽象。

### CMake 选项与编译器探测

根 `CMakeLists.txt` 新增三态选项（字符串 cache 变量，便于表达 AUTO）：

```cmake
set(MSLANG_VM_DISPATCH "AUTO" CACHE STRING "VM dispatch implementation: AUTO | SWITCH | GOTO")
set_property(CACHE MSLANG_VM_DISPATCH PROPERTY STRINGS AUTO SWITCH GOTO)
```

解析逻辑（配置期执行一次，结果存入普通变量）：

1. `SWITCH`：直接使用 `ms_vm_dispatch_switch.c`。
2. `GOTO`：先做能力探测，失败则 `message(FATAL_ERROR ...)`（显式请求必须显式
   失败，不静默回退）。
3. `AUTO`（默认）：能力探测成功用 goto 版，否则用 switch 版。

能力探测用 `check_c_source_compiles`，测试片段同时覆盖本实现用到的全部扩展点
（`&&` 取标签地址、标签数组的指定初始化器、`goto *` 间接跳转）：

```cmake
include(CheckCSourceCompiles)
check_c_source_compiles("
  int main(void) {
    static const void* const t[] = { [0] = &&lab };
    goto *t[0];
  lab:
    return 0;
  }
" MSLANG_HAVE_COMPUTED_GOTO)
```

探测通过即等价于"当前编译器接受本实现的语法"；MSVC 天然探测失败。随后把选中
文件加入 target 并向 `mslang` target 追加编译定义供诊断输出（仅用于
`--version` 文案与测试断言，不驱动任何 VM 代码分支）：

```cmake
if(dispatch_use_goto)
  target_sources(mslang PRIVATE src/vm/ms_vm_dispatch_goto.c)
  target_compile_definitions(mslang PRIVATE MS_VM_DISPATCH_IS_GOTO=1)
else()
  target_sources(mslang PRIVATE src/vm/ms_vm_dispatch_switch.c)
endif()
```

### MSVC 与不支持平台的回退

- `AUTO` 下 MSVC 探测必然失败 → 自动选择 switch 版，配置期打印一行
  `STATUS` 说明（如 `VM dispatch: switch (compiler lacks computed goto)`）。
- 显式 `GOTO` + MSVC → `FATAL_ERROR`，提示改用 `AUTO` 或 `SWITCH`。
- switch 版是纯 C11，无扩展语法，在所有受支持编译器（MSVC 2019+ / GCC 10+ /
  Clang 12+）上零警告通过 `MSLANG_STRICT_WARNINGS`。
- 三套 CI 平台（11-project-layout §4 矩阵）均跑 `AUTO`；Linux/macOS 实际落到
  goto 版，Windows/MSVC 落到 switch 版，天然形成双实现覆盖。

### 诊断报告

`mslang --version` 输出追加分派实现标识，形如
`mslang 1.0.0 (dispatch=computed-goto)`，数据来自 `msVmDispatchName()`，供基准
对比与 CI 断言当前二进制实际使用的实现，防止"以为在测 goto 版"的误判。

## 实现步骤

1. 重构拆分：从 `src/vm/ms_vm.c` 抽出分派循环为 `ms_vm_dispatch_switch.c`，指令
   语义抽为 `ms_vm_ops.h` 的 `static inline` 处理函数，新增
   `ms_vm_dispatch.h`；`mslang` target 固定编译 switch 版。验证：纯重构、无行为
   变化——全部既有 C 测试与 ms 脚本测试在改动前后同样全绿。
2. 新增 `src/vm/ms_vm_dispatch_goto.c`：标签地址表 + 逐指令标签体，复用
   `ms_vm_ops.h`；临时在构建系统里无条件改用 goto 版做本机验证。验证：
   GCC/Clang 下编译零警告，全部测试全绿。
3. 接入 CMake 三态选项与能力探测，恢复默认 `AUTO`；显式 `GOTO`/`SWITCH` 各配置
   一次验证选源正确；MSVC（或探测失败的模拟环境）验证 `AUTO` 回退与显式 `GOTO`
   的 `FATAL_ERROR`。验证：配置输出与 `msVmDispatchName()` 返回值一致。
4. `--version` 追加 dispatch 标识。验证：两种构建下输出分别含
   `dispatch=switch` / `dispatch=computed-goto`。
5. 双构建全量回归：同一源码分别配置 `-DMSLANG_VM_DISPATCH=SWITCH` 与
   `-DMSLANG_VM_DISPATCH=GOTO`（构建目录 `build/` 内分子目录，产物不出
   `build/`），各跑 `ctest` + `python run_tests.py`。验证：两轮结果逐项一致。
6. 基准对比：两套构建跑任务 64 基准套件，产出对比表（每项 switch 均值 / goto
   均值 / 比值）。验证：达到「验收标准」的性能门槛；未达标则按 11-project-layout
   §5 的原则将默认 `AUTO` 中 goto 的启用结论记录为"保留 switch 默认"，本任务以
   回退结论收尾也算完成。
7. CI 矩阵增补：Linux 与 macOS 任务各加一次显式 `-DMSLANG_VM_DISPATCH=GOTO`
   的构建+测试，Windows 保持 `AUTO`（=switch）。验证：CI 全绿。

## 测试方案

本任务是性能优化，不引入语言新语义；正确性保障以**两套分派构建下的全量回归**
为核心，辅以构建管线检查与基准对比。新增脚本测试遵循任务 40 之后的约定，使用
`testing` 模块；本任务只交付设计文档，测试代码随实现编写。

- 全量回归（两套构建各一遍）：
  - `ctest --test-dir build`：既有全部 C 单元测试；
  - `python run_tests.py`：`tests/ms/` 全量脚本测试；
  - `mslang test ./...`（任务 34）：testing 模块驱动的测试。
  判定标准：两套构建的通过/失败清单逐项相同，且与改动前基线一致。
- 新增冒烟脚本 `tests/ms/vm_dispatch_smoke_test.ms`（testing 模块）：以最短路径
  各触发一类指令——算术/比较快路径、跳转与循环、函数调用与返回、闭包、容器构
  建与下标、属性访问、异常 try/raise、协程 spawn/await——用于切换分派实现后的
  一分钟级快速确认。覆盖点不在深度（全量套件负责深度），而在指令类别无遗漏。
- 构建管线验证（CI/手工）：
  - `mslang --version` 的 dispatch 标识与配置选项一致（SWITCH→`switch`，
    GOTO→`computed-goto`，Linux/macOS 的 `AUTO`→`computed-goto`，MSVC 的
    `AUTO`→`switch`）；
  - 显式 `GOTO` + MSVC 配置期报错；
  - goto 版在 `MSLANG_STRICT_WARNINGS=ON` 与 `MSLANG_ENABLE_ASAN=ON`（非 MSVC）
    下构建零警告、测试无 ASAN 报告。
- 基准对比（依赖任务 64 套件）：
  - Release 构建，同一机器、同负载条件下两套构建各跑基准套件不少于 3 轮取中位
    数；
  - 产出逐项对比表（switch / goto / 比值）存档于基准结果目录；
  - 门槛见「验收标准」。

## 验收标准

- [ ] `src/vm/ms_vm_dispatch.h` / `ms_vm_ops.h` / `ms_vm_dispatch_switch.c` /
  `ms_vm_dispatch_goto.c` 齐备；dispatch 头 guard 为
  `MSLANG_SRC_VM_MS_VM_DISPATCH_H_`；两实现文件导出且仅导出
  `msVmDispatchRun` 与 `msVmDispatchName` 两个符号。
- [ ] VM 代码（含两个分派文件与 `ms_vm_ops.h`）中无任何 `#ifdef` 分派分支；
  指令语义只在 `ms_vm_ops.h` 单点维护，两个分派文件差异仅限取指/跳转骨架。
- [ ] CMake 选项 `MSLANG_VM_DISPATCH`（AUTO/SWITCH/GOTO，默认 AUTO）生效：
  配置期能力探测正确，`AUTO` 在 GCC/Clang 选 goto 版、在 MSVC 回退 switch 版，
  显式 `GOTO` 在不支持的编译器上配置期 `FATAL_ERROR`。
- [ ] `mslang --version` 输出含与配置一致的 `dispatch=` 标识。
- [ ] SWITCH 与 GOTO 两套构建下 `ctest` + `run_tests.py` + `mslang test ./...`
  全量回归结果与改动前基线逐项一致；`tests/ms/vm_dispatch_smoke_test.ms` 在两
  套构建下均通过。
- [ ] goto 版在 GCC 与 Clang 下 Release/Debug 均零警告通过
  `MSLANG_STRICT_WARNINGS`，ASAN 构建测试无报告。
- [ ] 基准门槛（Release、同机三轮中位数）：分派密集型基准（算术循环、递归函
  数调用类）goto 版相对 switch 版提升 ≥ 8%；全套件几何平均提升 ≥ 3%；无单项
  劣化 > 3%。达标则将 `AUTO` 的 goto 启用记录为正式结论；未达标则在任务文档中
  记录数据并保留 switch 为 `AUTO` 默认。
- [ ] CI 含至少一个显式 `MSLANG_VM_DISPATCH=GOTO` 的构建+测试任务且全绿；构建
  产物全部位于 `build/`。
- [ ] 无 TBD/TODO 占位；对任务 08/64 的接口假定（`msVmRun`、译码宏、基准入口
  等）在实现时已对齐对应任务文档。
