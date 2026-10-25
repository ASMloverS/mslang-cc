# 任务 08 VM 执行核心实现设计

## 目标与范围

依据 `docs/tasks/08-vm-core.md` 交付栈式字节码 VM，并以 `tests/c/test_vm.c`
独立验收。实现每协程独立的可增长调用栈与求值栈、单一 switch 分派循环、39 条核心指令、
精确数值比较、脚本/C 函数调用和统一错误恢复。

当前仓库尚无 `MsState`、错误槽、全局命名空间、函数对象和慢路径运算，本任务必须补齐其最小
形态。不提前实现任务 09 的求值入口和内建注册、任务 13 的默认/关键字/可变参数、任务 14 的
闭包指令、任务 16 的容器或任务 18 的完整公开 C API。

## 方案选择

### 采用：分层补齐最小运行时

VM 只负责执行上下文和指令语义；状态、函数对象、慢路径运算分别放在独立模块。该结构直接
承接后续任务，全局表暂以动态数组实现，避免为了复用 dict 而提前交付任务 16。

### 未采用：所有支持逻辑并入 `ms_vm.c`

这样能减少文件数，但会把格式化错误、全局表、对象构造、字符串拼接和函数对象所有权耦合进
分派文件，后续任务仍需拆分，也削弱独立测试能力。

### 未采用：提前建设完整运行时

完整公开 C API、dict 全局表和完整参数绑定会侵入任务 09、13、16、18 的范围，不采用。

## 文件与模块边界

- `src/vm/ms_vm.h` / `ms_vm.c`：`MsCallFrame`、`MsCoroutine`、栈/帧管理、`msVmRun`、
  `msVmPushFrame`、`msCallProto`。
- `include/mslang/state.h`：仅公开不透明 `MsState`、`msNewState`、`msCloseState`。
- `src/vm/ms_state_internal.h` / `ms_state.c`：最小 `MsState`、线性全局表、错误槽和
  `msNewNil`/`msNewBool`/`msNewInt`/`msNewFloat` 内部包装。任务 09 在此扩展求值入口与内建。
- `src/vm/ms_function.h` / `ms_function.c`：最小 `MsFunction` 和 C 函数对象、构造器及访问器。
  Proto、名称和 upvalue 均为借用引用；任务 13/14 在同一结构上扩展，不重复定义函数类型。
- `src/object/ms_operator.h` / `ms_operator.c`：`msObjectBinaryOp`/`msObjectUnaryOp`，承载
  溢出慢路径、字符串运算、浮点 `div`/`mod` 和类型错误；未来魔术方法也在此接入。
- `tests/c/test_vm.c`：真实对象、状态、Proto builder 和 VM 的 C 单元测试。
- `CMakeLists.txt`：注册上述源文件和 `test_vm`。

内部头文件不 typedef 结构体。`MsCFunction` 仅是公开 API 允许的函数指针 typedef，对应对象
结构命名为 `struct MsCFunctionObject`，避免 C 普通标识符命名空间冲突。

## MsState、全局表与错误槽

`MsState` 内嵌 `MsHeap` 和主 `MsCoroutine`，并持有全局项数组、当前错误字符串和错误结果码。
`msNewState` 分配并清零状态、初始化 heap 和主协程；全局项数组惰性分配。`msCloseState` 按
协程、全局项数组、heap、状态本体的顺序销毁，最终分配统计归零。本任务不注册内建。

全局项为 `{struct MsString* name; struct MsObject* value;}`，名字和值均借用，由 heap 持有。
`msStateGetGlobal` 按字符串内容线性查找，未命中返回 `NULL`；`msStateSetGlobal` 更新已有项或
倍增追加新项，分配失败返回 `MS_ERROR_OOM`。不依赖指针驻留，长名字也可作为全局名。

错误槽保存 `MsString*` 和对应的 `MsResult`。`msRaiseRuntimeError` 与最小 `msRaiseTypeError`
使用 `va_list` 格式化，经临时字符缓冲区创建错误字符串；内部只读查询供 VM 和测试使用。新的
错误替换旧错误。格式化或分配失败时错误码为 `MS_ERROR_OOM`，不保留悬空或部分消息。

VM 产生的错误以 `sourceFile:line: message` 记录；缺源文件时使用 `<unknown>`，缺行号时为 0。
慢路径或 C 函数已经写入错误时，VM 在返回前为没有位置的信息补上当前指令位置，且不覆盖原始
错误类别。OOM 始终返回 `MS_ERROR_OOM`，其他执行失败返回 `MS_ERROR_RUNTIME`。

## 协程、帧与栈不变量

结构和容量常量遵循任务 08：

```c
struct MsCallFrame {
  struct MsProto* proto;
  int pc;
  int baseIndex;
  struct MsUpvalue** upvalues;
  int upvalueCount;
};

struct MsCoroutine {
  struct MsCallFrame* frames;
  int frameCount;
  int framesCap;
  struct MsObject** stack;
  int stackTop;
  int stackCap;
};
```

`msCoroutineInit` 产生全零空上下文，数组首次使用时分配；`msCoroutineDestroy` 只释放两个数组，
不释放栈中对象或借用的 Proto/upvalue。两数组均倍增增长，求值栈硬上限为 `1 << 20`，帧数上限
为 1000；溢出和 OOM 分别报运行时错误与 `MS_ERROR_OOM`。

每个帧窗口统一为：

```text
... | callee | arg0 ... | nil-filled locals ... | scratch ...
             ^baseIndex
```

公开 `msVmPushFrame` 在参数前压入 nil 作为直接 Proto 调用的 callee 哨兵，因此顶层调用与脚本
`CALL` 共用同一返回路径。内部脚本 `CALL` 复用已经位于栈上的函数对象，不另加哨兵。
`baseIndex` 指向首个参数槽，`stackTop` 在执行期间指向暂存区顶端。

压帧时校验精确位置参数个数、拒绝 varargs/kwargs 标志，确保 `localCount >= paramCount`，并
一次性预留 callee、局部窗口和 `stackSize`。实参复制后，其余局部槽填 nil。帧切换、栈增长或
任何可能分配的操作之后都重新从 `co->stack + baseIndex` 推导窗口，不跨这些操作缓存栈指针。

`RETURN` 取栈顶返回值，将栈截到 `baseIndex - 1`，弹帧，再把返回值写回原 callee 槽。到达
`stopAt` 时 `msVmRun` 成功返回；否则恢复调用者继续分派。`msCallProto` 组合压帧与执行，成功时
把结果写到 `out`，失败时保持 `out == NULL`。

`msVmRun` 记录入口栈顶；任何错误都丢弃 `stopAt` 以上的帧并恢复该栈顶。这样嵌套调用失败不会
留下半帧，同一协程可在清除错误后再次执行。`stopAt` 必须有效且不高于进入时帧数。

## 分派与基础指令

`msVmRun` 是唯一取指/译码/switch 外壳，不引入 computed goto 或平台 `#ifdef`。所有操作数经
`msOpDecode*` 取得，跳转目标为取指后的 `pc + sAx`。回边和 `CALL` 仅保留 safepoint 注释。

实现任务 08 指定的 39 条指令：常量/移动 5、局部 2、全局 2、算术 15、比较 8、跳转 4、
`CALL`/`RETURN`、`NOP`。局部和常量下标在 debug 下断言编译器不变量，release 下以运行时错误
防止越界访问。其余操作码统一报告 `opcode not implemented`；debug 按规格断言，Release 测试
其错误恢复路径。

`LOAD_GLOBAL` 未定义时报 `undefined name '<name>'`；`STORE_GLOBAL` 弹栈写表。
`JUMP_IF_FALSE`/`JUMP_IF_TRUE`/`JUMP_IF_NIL` 弹条件；真值统一走 `msObjectIsTruthy`，当前覆盖
标量和字符串，容器真值日后仍在该对象入口扩展。

## 算术与慢路径

双机器字 int 的 `ADD`/`SUB`/`MUL`/`NEG` 先做边界检查，确认无溢出后才执行；`MUL` 不依赖可能
溢出的中间有符号值。`FLOORDIV`/`MOD` 特判除零和 `INT64_MIN / -1`，其余按向负无穷取整及
除数符号调整余数。`DIV` 的 int/int 也生成 float。`POW` 对非负 int 指数做受检平方乘；负指数
转为 double；快路径无法保持 int64 时交慢路径，任务 31 前慢路径统一报 `integer overflow`。

位运算只接受 int。移位计数小于 0 或大于等于 64 报运行时错误。左移通过受检乘法或专门的
63 位边界路径实现，不对负数或溢出值做有符号左移；右移用无符号位操作组合出算术右移，避免
依赖实现定义的有符号右移。

int/float 混合和双 float 的 `ADD`/`SUB`/`MUL`/`DIV`/`POW`/`NEG` 直接以 double 计算；
`FLOORDIV`/`MOD` 在慢路径以 `floor`/`fmod` 实现并返回 float，除数为零同样报错。float 位运算
和其他不支持组合报类型错误。字符串 `ADD` 按字节拼接；序比较按 UTF-8 字节字典序，值相等沿用
`msObjectValueEquals`。慢路径返回 `NULL` 时必须已写错误槽，VM 原样传播其结果码。

新数字或字符串由状态内嵌 heap 构造。返回值立即写回栈；在当前无 GC 的阶段由 heap 统一回收，
不增加根栈或提前实现 GC。

## 精确比较、身份与链式比较

同类型 int 直接比较，同类型 float 使用 IEEE 语义；EQ/NE 的其他组合先走指针捷径，再走
`msObjectValueEquals`。`IS` 只比较指针身份。

混合 int/float 的六种比较不得先把 int 粗暴转为 double。实现一个返回小于/等于/大于/无序的
数值比较助手：处理 NaN 和无穷；float 位于 int64 可转换范围时取得其向零截断的整数部分，
先与 int 比较，再依据小数部分和符号决定相等之外的次序；范围外由 float 所在侧直接决定次序。
由此保证 `2^53+1` 与 `2^53.0` 的相等和顺序均正确。NaN 使 EQ 为假、NE 为真，其余序比较为假。

`CMP_CHAIN` 的 A 限定为 `MS_OP_EQ` 到 `MS_OP_GE`，Bx 是无符号前向偏移。弹 right/left；比较为
真时压回 right 并继续，为假时压 false 并跳到取指后的 `pc + Bx`。无效 A 报运行时错误。普通
比较复用同一比较助手，确保链式与单段语义一致。

## 函数对象与调用

`MsFunction` 当前只有对象头、借用的 Proto、空 upvalue 指针及计数；`msNewFunction` 仅包装已有
Proto，不接管其所有权。C 函数对象保存借用名称和 `MsCFunction` 回调。构造均经 `msObjectAlloc`，
由 heap 销毁。`msFunctionProto`/`msAsCFunction` 仅对正确标签返回对应内容。

脚本 `CALL` 从栈形 `[... callee args ...]` 计算 `baseIndex`，校验参数个数及 Proto 标志，把函数
对象的空 upvalue 视图写入新帧。C `CALL` 直接借用连续 argv 调用回调，不压帧；成功后以单个
返回值替代 callee 和实参，`NULL` 则传播错误。其他标签报 `object is not callable`。帧增长、
嵌套返回和递归都复用同一个内部压帧入口。

本任务提供最小函数对象只是为了使任务 08 的 `CALL` 可被独立测试；`MAKE_FUNCTION` 和
`MAKE_LAMBDA` 仍属后续任务，不在本任务实现。

## 测试设计

所有用例使用 `msNewState`、独立 `MsCoroutine` 和 `MsProtoBuilder` 构造真实输入；拆卸后关闭
OOM 注入、释放 Proto/协程/状态，并断言 `liveBlocks == 0`。浮点用值或位模式断言，指针用
`MS_ASSERT_TRUE`，不套用仅适合整数的 `MS_ASSERT_EQ`。

覆盖以下组别：

1. 生命周期、空协程、nil 返回、直接 Proto 的 callee 哨兵和输出约定。
2. 栈/帧首次分配与倍增、深窗口重定位、两级硬上限、参数及局部窗口布局。
3. 常量、移动、局部、全局往返、长全局名和未定义名。
4. 前向/回边跳转、各标量与字符串真值、循环累加。
5. 全部算术和一元指令、int/float 混合、Python 取整符号、除零、溢出、负指数、全部移位边界。
6. 六种数值比较、NaN、字符串比较、身份、`2^53` 附近精确次序。
7. 链式比较全真、首段/中段失败和后续操作数短路。
8. 脚本多层调用、递归增长与深度错误、C 函数成功/失败、参数不匹配和不可调用对象。
9. 源文件/行号诊断、错误后帧与栈恢复、清除错误后的再次成功执行。
10. OOM 注入覆盖栈、帧、全局表、结果对象、错误格式化和函数对象分配；Release 覆盖未实现指令。

先新增失败测试并观察预期编译或断言失败，再实现对应小组；每组完成后运行 `test_vm` 和既有
全测。最终在独立 worktree 内执行严格警告的 Debug/Release 构建、完整 `ctest` 和
`git diff --check`，所有产物仅位于该 worktree 的 `build/`。

## 文档与验收

实现过程中若最终接口与任务 08 的假定名不同，同步修正 `docs/tasks/08-vm-core.md`；同时更新
任务 06/09/13 中受影响的最小状态、函数对象边界说明，避免文档重复承诺同一交付物。只有全部
测试和验收项确实通过后，才勾选任务 08 验收项并把 `docs/tasks/README.md` 的状态改为 ✅。
