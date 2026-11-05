# 任务 10：内建函数设计

## 背景与目标

任务 09 已提供最小 `print`/`assert` 和脚本执行链路。任务 10 将内建模块扩展为表驱动的正式注册机制，并交付 v0.1 所需的 12 个内建：`print`、`assert`、`len`、`type`、`int`、`float`、`str`、`bool`、`range`、`abs`、`min`、`max`。

当前代码只有 list/dict 类型标签，没有容器对象或字面量编译；任务 10 的测试和 `range` 又依赖这些能力。经确认，本任务补齐最小容器对象、字面量构建和内建所需的只读访问，完整容器语义仍属任务 16。

## 方案选择

采用“表驱动内建 + 独立最小容器模块”。内建模块负责统一参数检查、表示、转换和比较；`src/object/` 中的 list/dict 模块负责存储和基础访问；编译器与 VM 只增加字面量构建路径。

不将容器实现私藏在 `ms_builtin.c`，避免任务 16 拆出重做；也不在对象层集中实现全部容器语义，避免扩大本任务边界。

## 模块设计

### 内建注册

`src/vm/ms_builtin.h` 只公开 `void msBuiltinRegisterAll(MsState* L)`。`msNewState` 在堆和全局命名空间初始化后调用一次，取代 `msBuiltinRegisterMinimal`。

`ms_builtin.c` 使用文件级 `static const struct MsBuiltinEntry` 表保存名称与 `MsCFunction`。注册时依次创建名称字符串和 C 函数对象，并写入全局命名空间。任一步骤失败即停止，由既有 `msNewState` 错误路径关闭状态并返回失败。

脚本内建仍保存在全局命名空间，因此同名全局声明可遮蔽内建，不增加第二套名字解析机制。

### 最小 list/dict

新增 `src/object/ms_list.{h,c}` 和 `src/object/ms_dict.{h,c}`。

list 使用动态数组，记录元素指针、长度和容量。提供构造、追加、长度和只读元素访问；`range` 可预分配后逐项追加。

dict 使用保持插入序的条目数组和线性查找。提供构造、按键插入或替换、长度及按插入序遍历。条目保存键和值；替换已有键不改变位置。线性查找是最小实现的刻意取舍，任务 16 再替换为完整哈希表。

本任务不提供容器方法、脚本下标读写、删除、切片、成员测试或迭代协议。编译器和 VM 仅接通 `MS_AST_LIST_LIT`/`MS_AST_DICT_LIT` 到 `MS_OP_BUILD_LIST`/`MS_OP_BUILD_DICT` 的路径。

对象释放路径按类型释放 list/dict 内部缓冲区，然后释放对象本身；正常 `msCloseState` 和构造失败回滚均不得遗留缓冲。

## 公共辅助与表示层

内建模块内部提供参数个数检查、类型检查、真值判定和动态字符串构建，这些辅助函数均为文件内 `static`。对象运算层提供共享 `msObjectCompare`，统一 VM 与内建的精确数值及字符串排序；内建只补充 TypeError 包装。

`msObjectIsTruthy` 扩展 list/dict 的空值判定，使 `bool`、`assert` 和 `flush` 共用一致的真值语义。

`str` 与 `print` 共用同一表示函数：

| 值 | 表示 |
|---|---|
| nil | `nil` |
| bool | `true` / `false` |
| int | 十进制 |
| float | 最短往返表示；整数值补 `.0` |
| str | 字符串自身 |
| list | `[e0, e1, ...]` |
| dict | `{"k": v, ...}`，保持插入序 |
| C 函数 | `<builtin function print>` 等形式 |
| 类型对象 | `<type 'int'>` 等形式 |

容器元素和 dict 键值使用 repr 风格：字符串带双引号，并转义反斜杠、引号、换行、回车、制表及其他控制字符；其他值复用普通表示。字符串自身可包含任意 UTF-8 字节，输出和构造使用显式长度，不依赖 NUL 终止。

float 格式化从精度 1 到 17 使用 `%.*g`，选取首个经 `strtod` 往返后位级相等的候选；没有小数点或指数标记时追加 `.0`。保留 `-0.0`、`inf` 和 `nan` 的可识别表示。

## 内建行为

### print 与 assert

`print(*values, sep=" ", end="\n", file=nil, flush=false)` 将值依次转为 str 表示，以 `sep` 连接并追加 `end`，返回 nil。`sep`/`end` 必须是字符串；`file` 非 nil 报 TypeError；`flush` 按真值决定是否调用 `fflush(stdout)`。所有写入和冲刷失败均设置运行时错误。

实现任务 10/13 文档约定的显式 C 函数关键字通道：公开 `MsCFunction` 签名不变，内部 `MsState.cKeywordCount` 记录关键字个数 `k`，名字字符串和值仍交错追加在 `argv` 尾部。普通 `CALL` 时 `k=0`；`print` 从位置参数末尾读取恰好 `k` 对名字和值，不猜测普通字符串是否为关键字。识别 `sep`、`end`、`file`、`flush`，拒绝未知名、重复名和位置/关键字重复绑定。任务 13 接通 `CALL_KW` 前，脚本测试只覆盖位置参数和默认行为，不提前实现关键字调用指令。

`assert(cond, msg=nil)` 使用统一真值入口。条件为假时产生 `AssertionError:`；有消息时附加其 str 表示，无消息时使用固定断言失败文本。条件为真返回 nil。

### len、type 与转换

`len(x)` 对合法 UTF-8 str 逐序列计 Unicode 码点，对 list/dict 返回元素数，其他类型报 TypeError。非法 UTF-8 的验证不属于本任务。

`type(x)` 返回 `x->header.type`，类型对象在堆中唯一，可按对象身份比较。

`int(x, base=10)`：

- int 原值返回，bool 转为 0/1，float 向零截断。
- float 为 nan、inf 或超出 int64 可表示范围时报 ValueError。
- str 按 base 2/8/10/16 完整解析，允许正负号，base 16 额外允许 `0x`/`0X` 前缀；非法输入和 int64 溢出报 ValueError。
- 非字符串配合显式 base，以及其他不支持类型，报 TypeError。

`float(x)` 支持 int/bool/float/str。字符串使用十进制浮点解析并完整消费，支持 `nan`/`inf`，显式拒绝 `strtod` 额外接受的十六进制浮点字符串；非法输入和溢出报 ValueError。

`str(x)` 返回表示层构造的字符串对象，只有 OOM 可失败。`bool(x)` 返回统一真值结果。

### range、abs、min、max

`range(start, stop=nil, step=1)` 只接受 int，不接受 bool。1 参为 `[0, start)`，2 参为 `[start, stop)`，3 参使用 step；step 为零报 ValueError。正负 step 与空区间均受支持。

本任务按任务文档明确返回物化 int list，而非规范最终要求的惰性对象。元素数使用无符号差值和除法计算，避免有符号减法溢出；无法由当前 list 表示或内存不足时走统一错误路径。任务 26 后再以惰性 range 替换实现。

`abs(x)` 支持 int/float；`INT64_MIN` 报 ValueError。

`min(...)`/`max(...)` 接受至少一个位置参数。单参为 list 时取元素，空 list 报 ValueError；其他单参即候选值本身。int/float 可混合比较，str 仅与 str 比较，其他组合报 TypeError。返回获胜的原对象，不复制。

## 错误与内存约定

补充内部 `msRaiseValueError` 和 `msRaiseAssertionError`，与既有 `msRaiseTypeError` 共用格式化和错误槽路径。异常对象仍属任务 23；本任务错误均为消息前缀加 `MS_ERROR_RUNTIME`，CLI 退出码 1。

所有堆缓冲只经 `msAlloc`/`msRealloc`/`msFree`。动态字符串和容器扩容失败时保留原有对象状态并设置 OOM。当前没有执行中的 GC 或根栈 API，本任务不提前实现任务 17；所有权以参数借用、返回值立即消费和状态关闭统一回收为准。

## 测试设计

新增 `tests/ms/builtin/`：

- `print.ms` + `print.out`：空参、多值、返回 nil、标量与容器表示、float 往返形式。
- `len.ms`：ASCII、多码点 UTF-8、list/dict 和空值长度。
- `type.ms` + `type.out`：全部当前可构造值及 C 函数的类型输出和类型对象唯一性。
- `convert.ms`：int/float/str 转换矩阵及完整 bool 真值表。
- `range.ms`：1/2/3 参、正负 step、空区间和元素个数。
- `minmax.ms`：abs、多数值、单 list、混合数值和字符串。
- 负例覆盖 len 类型、int 解析与溢出、float 解析、非有限 float 转 int、range 零 step、空 min、混合比较和 assert 消息。

每个负例使用 `.exit` 声明预期退出码，并使用 `.err` 声明 stderr 必须包含的错误片段。`run_tests.py` 增加可选 `.err` 检查；无该文件时保持现有行为。

既有 `tests/c/test_vm.c` 适配全局容量增长与 `BUILD_LIST` 执行支持，不新增独立 C 行为测试。任务 09 冒烟脚本必须保持通过。

最终验证包括完整 Debug/Release 构建、对应 14 项以上的 CTest、全部 MS 脚本、直接 CLI stdout/stderr/退出码检查、`git diff --check` 和产物检查。当前仅验证 Windows，不声称 Linux/macOS 已通过。

## 文档收尾

完成时将任务 10 标记为已完成并勾选本机已验证项。更新任务 16 的模块起点，说明任务 10 已提供最小 list/dict 对象和字面量路径，任务 16 在此基础上交付完整容器语义。设计、计划和执行偏差保存在 `docs/superpowers/`。
