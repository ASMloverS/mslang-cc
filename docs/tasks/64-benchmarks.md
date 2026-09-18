# 64 性能基准套件

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v1.0 | ⬜ | [09 最小可运行解释器](09-minimal-interpreter.md) |

## 任务目标

交付 mslang 的性能基准套件：独立于 `tests/` 的顶层 `benchmarks/` 目录，内含一组
ms 基准脚本与一个 Python 驱动 `benchmarks/run_benchmarks.py`，负责计时、统计、
产出 JSON 报告、归档基线并按任务门槛做对比判定。套件覆盖七类热点：算术、字符串、
容器、函数调用、属性访问、GC 压力、协程切换，为 v1.0 的性能优化任务
（[65 NaN-boxing](65-nan-boxing.md)、[66 computed goto](66-computed-goto.md)、
67 属性内联缓存、68 增量 GC）提供统一的基线数据与量化验收口径，落实
[11-project-layout.md](../language/11-project-layout.md) §5「性能优化项从
[08-vm-internals.md](../language/08-vm-internals.md) §7 中按基准测试选取」。

依赖仅声明任务 09（可执行脚本的 `mslang` CLI）：套件骨架与算术/调用类用例在任务
09 之后即可运行；但字符串插值（任务 28）、GC（任务 17）、协程（任务 43/44）等
用例依赖后续特性，驱动按特性分档自动跳过。**实际全量执行建议在 v0.4 完成后进行**
（此时 `time` 标准库模块可用，脚本内计时路径完整），v1.0 优化任务启动前以该时点
冻结首份基线。

## 设计依据

- [08-vm-internals.md](../language/08-vm-internals.md)
  - §7 性能路线图：NaN-boxing（1）、内联缓存（2）、computed goto（3）、增量/并发
    GC（4）——任务 65/67/66/68 的优化对象，基准组按这四项的受益路径设计。
  - §4：算术指令内联快路径、属性访问首版为 dict + MRO 线性查找——`arith_*` 与
    `attr_access` 用例的测量目标。
  - §5：STW 标记-清除、阈值自适应——`gc_*` 用例的测量目标（暂停与吞吐）。
  - §6：协程与调度器——`coro_switch` 用例的测量目标（切换/调度开销）。
- [11-project-layout.md](../language/11-project-layout.md)
  - §1 仓库结构：顶层目录清单无 `benchmarks/`。本任务新增该目录（与 `tests/`、
    `examples/` 平级），属有意识的目录增补，§1 的目录树在后续文档同步时更新。
  - §2：构建产物只落 `build/`；基准为 Release 构建的外部测量，不产生新构建目标。
  - §5 路线图 v1.0：性能优化项「按基准测试选取」——本任务即该条款的执行设施。
- [10-c-style.md](../language/10-c-style.md)：本任务唯一的 C 改动（CLI 统计输出，
  见「详细设计」）遵循其全部规范。
- 仓库 Python 规范（AGENTS.md）：PEP 8、全量类型标注、`X | Y` 联合语法。
- 任务 02 核心基础设施：`struct MsMemStats` 与 `msMemGetStats`（分配统计：
  `allocCount` / `totalAllocatedBytes` / `peakBytes` 等）。
- 任务 17 GC 标记-清除：`struct MsGcStats` 与
  `msGcGetStats(const MsState* L, struct MsGcStats* out)`（收集轮数等）。
- 任务 09 最小可运行解释器：CLI 退出码约定（`MS_EXIT_OK`/`MS_EXIT_SYNTAX` 等）、
  `run_tests.py` 的二进制定位约定（命令行参数或 `MSLANG_BIN` 环境变量）——驱动
  沿用同一定位方式，保证与测试设施一致的调用体验。
- 任务 43 协程与 async/await：`async func` 句柄与 `await` 语义（`coro_switch`
  用例的语义基础）；任务 44 channel（乒乓用例的另一实现路径）。
- [07-stdlib.md](../language/07-stdlib.md)：`time.monotonic()` 单调时钟（脚本内
  计时）；其实现任务（39）文档尚未定稿，接口名实现时以对应任务文档为准。任务
  67/68 文档尚不存在，本文为其给出的门槛为建议初值，定稿以对应任务文档为准。
- 任务 65/66 已定稿文档对本任务的假定：65 假定 `bench/run_benchmarks.py` 与
  `bench/baselines/v1.0-boxed.json`，66 假定 `benchmarks/` 目录与
  `run_benchmarks.py`——两处均注明以实现时对齐本任务定名为准。本文定名采用
  `benchmarks/`（与 66 一致），65 中的 `bench/` 前缀在其实现时按本文对齐。

## 详细设计

### 目录布局

```
benchmarks/
├── run_benchmarks.py     # 唯一入口：run / compare / archive / selftest 子命令
├── cases/                # 基准用例（ms 脚本，非测试，不被 run_tests.py 发现）
│   ├── _probe_startup.ms # 启动开销探针（空操作脚本，下划线前缀不做基准）
│   ├── arith_loop.ms
│   ├── fib.ms
│   ├── nbody.ms
│   ├── string_concat.ms
│   ├── string_interp.ms
│   ├── list_ops.ms
│   ├── dict_ops.ms
│   ├── call_overhead.ms
│   ├── method_call.ms
│   ├── attr_access.ms
│   ├── gc_churn.ms
│   ├── gc_graph.ms
│   └── coro_switch.ms
├── results/              # 运行产物 <label>.json（不入库，加入 .gitignore）
└── baselines/            # 归档基线 <label>.json（入库，供跨任务对比）
```

### 用例清单与分组

每个用例是一个自包含 ms 脚本，文件顶部集中一个 `SCALE` 规模常量，目标为 Release
构建下单轮内核耗时落在 100ms–2s（噪声可控且全套件分钟级跑完）。**每个用例必须
先对计算结果做 `assert` 自检再计时**——防止优化把语义改错后"更快但错误"的结果
被当作提速。分组是驱动内的逻辑标签（一个用例可属多组），门槛判定按组应用：

| 用例 | 组 | 测量热点 | 主要消费任务 |
|---|---|---|---|
| `arith_loop` | numeric, dispatch | int/float 混合算术循环、快路径分支 | 65, 66 |
| `fib` | numeric, dispatch, call | 朴素递归 `fib(27)`（自检 == 196418），调用+分派密集 | 65, 66 |
| `nbody` | numeric | float 密集数值积分（benchmark game 简化版） | 65 |
| `string_concat` | string | 循环 `+=` 拼接与驻留/哈希路径 | 65（回归豁免） |
| `string_interp` | string | f-string 插值格式化（v0.2，任务 28） | 65（回归豁免） |
| `list_ops` | container | append/下标读写/for-in 迭代 | 65, 67（回归豁免） |
| `dict_ops` | container | 插入/命中查找/删除，字符串与整数键 | 65（回归豁免） |
| `call_overhead` | call, dispatch | 空体函数与多参函数的高频调用/返回 | 66 |
| `method_call` | call, attr | 方法绑定与调用（`MS_OP_CALL_METHOD` 路径） | 67 |
| `attr_access` | attr | 实例属性读写循环（dict + MRO 查找路径） | 67 |
| `gc_churn` | gc | 短命对象高速分配，触发频率与吞吐 | 65, 68 |
| `gc_graph` | gc | 长命大对象图 + 周期性新分配，标记成本与暂停 | 68 |
| `coro_switch` | coroutine | 两协程经 channel 乒乓 N 次（v0.3，任务 43/44） | 68（回归豁免） |

特性分档：驱动内置 `CASE_MIN_STAGE` 表（如 `string_interp → v0.2`、
`coro_switch → v0.3`）。执行某用例时若解释器以 `MS_EXIT_SYNTAX`（退出码 2）
失败，判定为特性缺失，该用例记 `skipped` 而非失败；其余非零退出码（含自检断言
失败的 `MS_EXIT_RUNTIME`）记失败并使 `run` 子命令最终退出码非零。

### 计时协议：脚本内 + 脚本外双轨

- **内核时间（kernel_ms，主对比指标）**：脚本用 `time.monotonic()` 包住被测
  循环，完成后向 stdout 输出唯一一行协议行：

  ```
  BENCH kernel_ms=123.456
  ```

  该路径在 v0.4（`time` 模块）后完整可用；此前执行时脚本省略协议行，驱动回退
  为纯外部计时，`kernel_ms` 记 `null`，对比改用 `wall_ms` 并在报告中标注降级。
- **墙钟时间（wall_ms）**：驱动用 `time.perf_counter_ns()` 包住整个子进程。
  驱动先跑 `_probe_startup.ms` 测得解释器启动基线并单列于报告元数据，供判读
  wall/kernel 差值；对比判定一律用 `kernel_ms`（可得时），wall 仅作参考。
- **解释器统计（alloc / gc）**：驱动以环境变量 `MS_BENCH_STATS=1` 运行
  `mslang`；CLI 在 `msCloseState` 之前向 stderr 输出单行 JSON（见下「CLI
  统计输出」），驱动按行解析合并进该轮结果。

### CLI 统计输出（本任务唯一的 C 改动）

`src/cli/ms_bench.h` / `src/cli/ms_bench.c`，include guard
`MSLANG_SRC_CLI_MS_BENCH_H_`，自包含：

```c
// When the MS_BENCH_STATS environment variable is set, writes one line of
// JSON with allocation and GC statistics to out. Called by main() right
// before msCloseState; a no-op when the variable is unset. Never fails:
// statistics are diagnostic data, dump errors are ignored.
void msBenchDumpStats(const MsState* L, FILE* out);
```

输出字段（单行、键序固定，便于驱动按行解析）：

```json
{"allocCount": 12345, "totalAllocatedBytes": 987654, "peakBytes": 123456, "gcCollectCount": 7}
```

- 分配三字段取自任务 02 的 `msMemGetStats`；`gcCollectCount` 取自任务 17 的
  `msGcGetStats`。本任务属 v1.0 阶段，实施时两者均已存在；若提前实施（任务 17
  之前），GC 字段省略，驱动对缺失键记 `null`。
- 该开关不改变默认 CLI 行为（未设环境变量时零输出、零开销），不影响任务 22 的
  CLI 参数面，属基准设施的一部分。
- 任务 68 需要 GC 暂停时间字段时，扩展 `msGcGetStats` 的统计结构并在本行追加
  `gcTotalPauseMs` / `gcMaxPauseMs` 键（驱动按缺失即 `null` 兼容旧二进制）。

### run_benchmarks.py 驱动

单文件、仅标准库（`argparse` / `json` / `statistics` / `subprocess` / `time` /
`platform` / `os` / `sys`；RSS 采集经 `resource`（POSIX）或 `ctypes` + psapi
（Windows），尽力而为，失败记 `null`）。遵守 PEP 8 与全量类型标注（`X | Y`）。
解释器二进制定位沿用任务 09 约定：`--bin` 参数或 `MSLANG_BIN` 环境变量，默认取
`build/` 下的构建产物。

子命令：

```bash
python benchmarks/run_benchmarks.py run --label v1.0-boxed [--runs N] [--filter glob] [--quick]
python benchmarks/run_benchmarks.py compare benchmarks/baselines/v1.0-boxed.json benchmarks/results/x.json [--task 65]
python benchmarks/run_benchmarks.py archive --label v1.0-boxed   # results → baselines 入库
python benchmarks/run_benchmarks.py selftest                     # 套件自检（见测试方案）
```

`run` 的执行规程：

1. 发现 `cases/*.ms`（排除 `_` 前缀），按 `--filter` 过滤。
2. 跑 `_probe_startup.ms` 3 次取中位，记入元数据 `startupMs`。
3. 每用例：1 轮预热（丢弃）+ N 轮计时（默认 N=5，`--quick` 为 3 且提示用例
   作者可把 `SCALE` 调小档；任务 66 要求的"不少于 3 轮"即 `--quick` 下限）。
   每轮独立子进程，捕获退出码、stdout 协议行、stderr 统计行与子进程峰值 RSS。
4. 聚合：每用例报告 `median` / `min` / `stdev`；对比判定用中位数。
5. 任一用例失败（非跳过）则最终退出码非零；报告始终落盘
   `benchmarks/results/<label>.json`。

结果 JSON 骨架（schema 的字段全集，缺失数据记 `null`）：

```json
{
  "label": "v1.0-boxed",
  "timestamp": "2026-01-01T00:00:00+00:00",
  "platform": {"os": "windows", "arch": "x86_64", "cpu": "...", "python": "3.12"},
  "build": {"mslangVersion": "...", "buildType": "Release", "commit": "<git rev>"},
  "config": {"runs": 5, "warmup": 1, "startupMs": 12.3},
  "cases": {
    "fib": {
      "groups": ["numeric", "dispatch", "call"],
      "status": "ok",
      "kernelMs": {"median": 812.4, "min": 805.1, "stdev": 4.2},
      "wallMs": {"median": 830.0, "min": 821.0, "stdev": 5.0},
      "peakRssKb": 15240,
      "alloc": {"allocCount": 5900000, "totalAllocatedBytes": 188800000},
      "gc": {"gcCollectCount": 41}
    }
  }
}
```

`compare` 的判定规则：

- 逐项计算比值 `candidate / baseline`（< 1 为提速），输出表格：用例、基线中位、
  候选中位、比值，并给出全套件与各组的几何平均比值。
- 任一侧数据为 `null` 或 `skipped` 的项不参与判定，表格标 `N/A` 并告警。
- 不带 `--task` 时纯展示，退出码恒 0；带 `--task NN` 时应用下表门槛，任一硬性
  门槛不满足则退出码 1（供 CI 卡点）。

### 量化验收门槛（供任务 65–68）

门槛表以数据形式内置于驱动（`TASK_GATES` 字典），此处为规范表述。
"提升/下降"均以基线中位数为分母的 `kernel_ms` 比值表述：

| 任务 | 适用对象 | 门槛 |
|---|---|---|
| 65 NaN-boxing | numeric 组 | 每用例 kernel_ms 中位下降 ≥ 30%（比值 ≤ 0.70） |
| 65 | 其余全部用例 | 劣化 ≤ 5%（比值 ≤ 1.05，超出须修复或评审记录让步） |
| 65 | numeric 组 | `alloc.allocCount` 中位下降 ≥ 80%（比值 ≤ 0.20） |
| 65 | 全部用例 | `peakRssKb` 不高于基线（比值 ≤ 1.00） |
| 66 computed goto | dispatch 组 | 提升 ≥ 8%（比值 ≤ 0.92） |
| 66 | 全套件 | 几何平均提升 ≥ 3%（比值 ≤ 0.97）；无单项劣化 > 3% |
| 67 内联缓存 | attr 组 | 每用例下降 ≥ 25%（比值 ≤ 0.75）；全套件几何平均 ≤ 0.95；无单项 > 1.05（建议初值，以任务 67 文档定稿为准） |
| 68 增量 GC | gc 组 | `gc.gcMaxPauseMs` 中位下降 ≥ 50%（比值 ≤ 0.50，字段由任务 68 扩展）；kernel_ms 吞吐劣化 ≤ 10%（建议初值，以任务 68 文档定稿为准） |

65/66 两行与其已定稿文档的「验收标准」一致，此处为唯一定义点（对比口径、
分组归属、字段名）；执行环境要求同步：Release 构建、同一机器、相近负载、
两轮运行间不更换编译选项以外的任何条件。

### 基线管理

- 基线文件 `benchmarks/baselines/<label>.json` 入库，是跨任务对比的锚点；
  `benchmarks/results/` 为本地产物，实施时加入 `.gitignore`。
- 基线序列：`v1.0-boxed.json`（v0.4 完成后、任务 65 开工前冻结的装箱基线，
  即任务 65 验收的分母）→ 65 完成后追加 `v1.0-nanbox.json` → 66/67/68 依次
  累加。每个优化任务以上一份基线为分母，`compare --task` 判定后把新结果
  `archive` 入库，形成可追溯的性能演进链。
- 基线只在「同机同人」原则下有效：更换测量机器时整套基线作废重录，报告中
  `platform` / `build` 元数据即为此审计而设。

## 实现步骤

1. 建 `benchmarks/` 目录与 `run_benchmarks.py` 骨架：子命令框架、用例发现、
   子进程执行、协议行解析、`_probe_startup.ms` 探针、首个用例 `arith_loop.ms`
   （含自检断言）。验证：`selftest` 雏形能跑通单用例并产出合法 JSON。
2. CLI 统计输出：新增 `src/cli/ms_bench.{h,c}` 的 `msBenchDumpStats`，接入
   `main.c`（`msCloseState` 前调用，读取 `MS_BENCH_STATS`）。验证：
   `MS_BENCH_STATS=1 mslang <script>` 的 stderr 恰一行 JSON 且字段正确；未设
   变量时无任何额外输出；任务 09 既有测试不受影响。
3. 补齐 numeric / string / container / call / attr / gc 各组用例（`SCALE` 常量、
   自检断言、`BENCH` 协议行）。验证：每个用例单独运行退出码 0、协议行格式
   正确、篡改自检值后以退出码 1 失败。
4. 聚合与报告：预热 + N 轮、median/min/stdev、RSS 采集（POSIX/Windows）、
   结果 JSON 落盘。验证：schema 字段齐全，缺失数据（如无 `time` 模块时的
   kernel_ms）正确记 `null`。
5. `compare` 与门槛表：逐项比值、组/全套件几何平均、`TASK_GATES` 数据表、
   `--task` 判定与退出码。验证：同一文件自对比全部比值为 1.00 且 `--task`
   判定通过；构造劣化样本验证退出码 1。
6. 特性分档与 `coro_switch`：`CASE_MIN_STAGE` 表与退出码 2 跳过逻辑；v0.3
   落地后补 `coro_switch.ms` 用例。验证：在缺失特性的解释器上该用例记
   `skipped` 而非失败。
7. `archive` 与基线约定：results → baselines 复制、`.gitignore` 增补
   `benchmarks/results/`。验证：归档文件入库后 `compare` 可直接引用。
8. v0.4 完成后全量执行并冻结 `baselines/v1.0-boxed.json`。验证：全套件绿、
   报告元数据完整，该基线可供任务 65 作为分母使用。

## 测试方案

本任务在任务 09 之后，但其交付物是基准设施而非语言特性：基准脚本本身即
"可运行的验证"，`benchmarks/cases/` **故意不放进 `tests/ms/`**（避免
`run_tests.py` 每次执行分钟级耗时基准），这是与「脚本测试一律入 `tests/ms/`」
约定的显式偏离，偏离范围仅限本目录。套件自验证以 `selftest` 子命令为核心：

- `selftest`（快速、CI 可跑）：
  - 以 `--quick`（3 轮）跑全部当前解释器支持的用例，断言每用例 `status == ok`
    或 `skipped`（特性分档正确），失败用例使退出码非零；
  - 校验结果 JSON schema：必填键齐全、类型正确、`null` 语义正确；
  - 协议健壮性：临时构造一个输出畸形 `BENCH` 行 / 无协议行 / 非零退出的
    用例（selftest 内置临时目录，不污染 `cases/`），断言驱动分别报协议错误、
    `null` 降级与失败；
  - `compare` 自校验：同一份结果文件自对比，全部比值为 1.00，`--task 65/66`
    判定通过、退出码 0；注入 10% 劣化的样本在 `--task 66` 下退出码为 1。
- 用例自检：每个基准脚本内建 `assert` 校验计算结果（如 `fib(27) == 196418`），
  保证「通过基准」蕴含「语义正确」；该断言每轮都执行，优化任务引入的语义回归
  会以用例失败（而非虚假提速）暴露。
- CLI 统计开关：stderr 是驱动私有通道，`run_tests.py` 只校验退出码与 stdout，
  故 `MS_BENCH_STATS` 的正确性由 `selftest` 覆盖（设变量跑探针用例，断言
  stderr 单行 JSON 的键与值域；不设变量断言 stderr 为空）。
- 回归：本任务不改语言语义，验收时跑既有 `ctest --test-dir build` 与
  `python run_tests.py` 全量回归，确认 CLI 改动零影响。

本文档只描述测试方案；驱动、用例与 selftest 的实体文件随实现步骤编写。

## 验收标准

- [ ] `benchmarks/` 目录按「目录布局」建立；`benchmarks/results/` 已加入
  `.gitignore`；`benchmarks/baselines/` 入库。
- [ ] 「用例清单」13 个用例齐备：每个含集中 `SCALE` 常量、计算结果 `assert`
  自检、`BENCH kernel_ms=...` 协议行（`time` 模块可用时）；`coro_switch` 等
  依赖后续特性的用例在缺失特性时由驱动记 `skipped`。
- [ ] `run_benchmarks.py` 四个子命令（run / compare / archive / selftest）
  可用；仅标准库；PEP 8 + 全量类型标注（`X | Y`）；二进制定位支持 `--bin`
  与 `MSLANG_BIN`，默认取 `build/` 产物。
- [ ] `run` 规程为 1 轮预热 + N 轮（默认 5，`--quick` 为 3）取中位数，报告
  median/min/stdev、墙钟、峰值 RSS（尽力而为）与 alloc/gc 统计，JSON schema
  与本文一致并落盘 `results/<label>.json`。
- [ ] `MS_BENCH_STATS=1` 时 CLI 在 `msCloseState` 前向 stderr 输出单行 JSON
  （含 `allocCount` / `totalAllocatedBytes` / `peakBytes` / `gcCollectCount`），
  未设变量时零输出；`src/cli/ms_bench.h` guard 为 `MSLANG_SRC_CLI_MS_BENCH_H_`，
  C 代码符合 10-c-style。
- [ ] `compare` 输出逐项比值与组/全套件几何平均；`--task 65` / `--task 66`
  按本文门槛表判定并以退出码报告结果；67/68 门槛以建议初值内置并标注待对应
  任务文档定稿。
- [ ] `selftest` 在 CI 常规时限内通过，覆盖协议畸形/缺失/失败、schema 校验、
  自对比与劣化注入判定。
- [ ] v0.4 完成后冻结 `benchmarks/baselines/v1.0-boxed.json`，任务 65 可直
  接以之为分母执行其验收（其文档中的 `bench/` 假定路径已按本文 `benchmarks/`
  对齐）。
- [ ] 既有 `ctest` 与 `run_tests.py` 全量回归不受影响；构建产物只落在
  `build/`。
- [ ] 无 TBD/TODO 占位；对任务 02/17/39/43/44 的接口引用（`msMemGetStats`、
  `msGcGetStats`、`time.monotonic()` 等）在实现时已按对应任务文档对齐。
