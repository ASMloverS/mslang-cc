# 08 VM 内部实现

> 编译管线、字节码、对象模型、GC、协程与调度器的实现设计。编码规范见 [10-c-style.md](10-c-style.md)。

## 1. 编译管线

```
.ms 源码
  → Lexer（词法分析，含分号自动插入）
  → Parser（递归下降 + Pratt 表达式解析）
  → AST
  → Compiler（作用域解析、闭包转换、字节码生成）
  → ByteCode（Proto 对象：指令流 + 常量池 + 行号表 + 异常表）
  → VM 执行
```

- **无独立优化 pass**（首版）：窥孔优化只做常量折叠与死跳转消除，编译器一次遍历生成。
- 每个函数（含模块顶层）编译为一个 `MsProto`；嵌套函数是外层 Proto 常量池中的子 Proto。
- 编译错误收集模式：单文件最多报告 20 个错误后中止，错误含文件/行/列与错误码。

## 2. 字节码设计

### 2.1 格式

- 栈式 VM，指令定长 4 字节：`opcode(8) | A(8) | Bx(16)` 或 `opcode(8) | sAx(24, 有符号)`。
- 定长指令：译码简单、跳转偏移计算便宜；代价是指令密度低于变长编码（可接受）。
- `MsProto` 布局：

```c
struct MsProto {
    MsObjectHeader header;
    uint32_t     *code;         // 指令流
    MsObject    **consts;       // 常量池（数字、字符串、子 Proto）
    int           codeLen, constsLen;
    int           paramCount;   // 参数个数
    int           localCount;   // 寄存器窗口大小（局部变量槽位数）
    int           stackSize;    // 求值栈最大深度（编译期算出）
    MsTryBlock   *tryBlocks;    // 异常表：{pcStart, pcEnd, handlerPc, finallyPc}
    MsLineEntry  *lines;        // pc → 行号映射
    MsString     *name;
    MsString     *sourceFile;
    bool          isAsync;
    bool          hasVarArgs, hasKwArgs;
};
```

### 2.2 指令集（按类别，代表性列举）

| 类别 | 指令 |
|---|---|
| 常量/移动 | `OP_LOAD_CONST` `OP_LOAD_NIL` `OP_LOAD_TRUE` `OP_LOAD_FALSE` `OP_MOVE` |
| 局部/闭包 | `OP_LOAD_LOCAL` `OP_STORE_LOCAL` `OP_LOAD_UPVAL` `OP_STORE_UPVAL` `OP_CLOSE_UPVALS` |
| 全局/模块 | `OP_LOAD_GLOBAL` `OP_STORE_GLOBAL` `OP_IMPORT` `OP_IMPORT_FROM` |
| 算术 | `OP_ADD` `OP_SUB` `OP_MUL` `OP_DIV` `OP_FLOORDIV` `OP_MOD` `OP_POW` `OP_NEG` `OP_NOT` `OP_BITAND/OR/XOR/SHL/SHR/INVERT` |
| 比较 | `OP_EQ` `OP_NE` `OP_LT` `OP_LE` `OP_GT` `OP_GE` `OP_IS` `OP_IN` `OP_CMP_CHAIN` |
| 跳转 | `OP_JUMP` `OP_JUMP_IF_FALSE` `OP_JUMP_IF_TRUE` `OP_JUMP_IF_NIL` |
| 容器 | `OP_BUILD_LIST` `OP_BUILD_TUPLE` `OP_BUILD_DICT` `OP_BUILD_SET` `OP_INDEX` `OP_SET_INDEX` `OP_DEL_INDEX` `OP_SLICE` `OP_APPEND` |
| 调用 | `OP_CALL` `OP_CALL_KW` `OP_TAIL_CALL` `OP_RETURN` `OP_LOAD_METHOD` `OP_CALL_METHOD` |
| 函数/类 | `OP_MAKE_FUNCTION` `OP_MAKE_CLASS` `OP_MAKE_LAMBDA` |
| 属性 | `OP_GET_ATTR` `OP_SET_ATTR` `OP_DEL_ATTR` |
| 迭代 | `OP_GET_ITER` `OP_ITER_NEXT`（失败时跳转，避免 StopIteration 异常开销） `OP_UNPACK` |
| 异常 | `OP_SETUP_TRY` `OP_POP_TRY` `OP_RAISE` `OP_RERAISE` |
| 并发 | `OP_SPAWN`（async 调用） `OP_AWAIT` `OP_CHAN_NEW` `OP_CHAN_SEND` `OP_CHAN_RECV` `OP_CHAN_TRY_RECV` `OP_SELECT_BEGIN/ADD_RECV/ADD_SEND/EXEC` |
| 其他 | `OP_PRINT_EXPR`（REPL 用） `OP_NOP` |

目标指令数控制在 80 条以内。分派方式：首版 `switch` 分派；`computed goto`（GCC/Clang 扩展）作为非 MSVC 平台的编译期可选优化。

## 3. 对象模型

```c
typedef enum {
    MS_TYPE_NIL, MS_TYPE_BOOL, MS_TYPE_INT, MS_TYPE_FLOAT,
    MS_TYPE_STRING, MS_TYPE_BYTES, MS_TYPE_LIST, MS_TYPE_TUPLE,
    MS_TYPE_DICT, MS_TYPE_SET, MS_TYPE_FUNCTION, MS_TYPE_CLASS,
    MS_TYPE_INSTANCE, MS_TYPE_MODULE, MS_TYPE_CHANNEL, MS_TYPE_COROUTINE,
    MS_TYPE_ITERATOR, MS_TYPE_BOUND_METHOD, MS_TYPE_C_FUNCTION,
    MS_TYPE_C_TYPE,        /* C 扩展定义的类型的实例 */
    MS_TYPE_COUNT
} MsTypeTag;

struct MsObjectHeader {
    MsType     *type;        /* 类型对象 */
    uint8_t     markColor;   /* GC 标记位 */
    MsObject   *gcNext;      /* 全对象链表 */
};
```

- 所有值都是 `MsObject*` 装箱对象（首版不做 NaN-boxing/指针打包；列入性能路线图）。
- 小整数（-256..4095）与短字符串驻留（interning）缓存，减少分配。
- `int` 内部表示：机器字内直存（不溢出时）；溢出后转为堆上大整数（符号+`uint32_t` 数字数组），两者统一走 `MsInt` 接口。
- `MsString`：长度 + 哈希缓存 + UTF-8 字节；码点索引 O(n)（与 CPython 的 latin-1/2/4 分层表示相比更简单，性能代价列入路线图评估）。

## 4. VM 执行核心

- 每个协程拥有独立的**调用栈**（帧数组）与**求值栈**，堆上分配，可伸缩。
- 帧（`MsCallFrame`）：`proto`、返回地址、求值栈基址、upvalue 数组。
- 算术指令内联快路径：双操作数均为机器字 int / float 时直接计算，否则走类型分派（魔术方法查找）。
- 属性访问首版为 dict 查找 + 类 MRO 线性查找；**内联缓存（inline cache）列入性能路线图**。

## 5. GC：STW 标记-清除

- **触发**：分配计数超过阈值（初始 1MB 等值对象数，按存活率自适应调整，Go 的 GOGC 思路）。
- **标记**：从根集合出发三色标记（实际是黑白两色 + 标记栈）。根集合：
  - 所有协程的调用栈/求值栈
  - 模块注册表、内建类型表
  - C API 显式根（`msRootPush` 压入的对象，见 [09-c-api.md](09-c-api.md)）
- **清除**：遍历全对象链表，回收未标记对象。
- **STW 协作**：GC 线程设置全局标记 → 各工作线程在最近 safepoint（让出点/函数调用/回边）自旋等待 → 标记清除 → 恢复。safepoint 检查是读取一个原子标志，开销可忽略。
- **终结器**：首版不支持 `__del__`（GC 语言终结器的坑不值得踩），列入路线图候选并附警示。
- 演进路径：增量三色标记（需写屏障）→ 并发标记。首版架构上把"标记"实现为可中断的步骤函数，为增量改造留口。

## 6. 协程与调度器

### 6.1 协程对象

```c
struct MsCoroutine {
    MsObjectHeader header;
    MsCallFrame  *frames;       /* 调用栈 */
    MsObject    **stack;        /* 求值栈 */
    int           state;        /* READY / RUNNING / SUSPENDED / DEAD */
    MsObject     *result;       /* return 值或未捕获异常 */
    MsObject     *waiters;      /* await 此协程的等待者队列 */
    MsWaitQueue  *blockedOn;    /* 阻塞在哪个 channel/句柄上 */
    ...
};
```

### 6.2 调度器

- 结构：全局就绪队列 + 每工作线程本地队列 + 工作窃取（偷本地队列尾部一半）。
- 工作线程数 = `MS_THREADS` 环境变量或 CPU 核数。
- channel 的 `send`/`recv` 阻塞：协程挂入 channel 的等待队列（双向：send 等待队列 + recv 等待队列），配对时双方（或单方）转 READY。
- 抢占：工作线程每 10ms 设置"抢占标志"，VM 在回边/调用指令检查并让出。让出的协程回到就绪队列尾部。
- `sync.Mutex` 等原语直接操作协程状态（挂起/唤醒），不依赖 OS 互斥锁（同调度器内）。

### 6.3 与 C API 的线程规则

- 一个 `MsState` 可在任意 OS 线程中使用，但**同一时刻只能由一个线程**操作；跨线程使用需调用方自行加锁（首版约束）。
- 调度器的工作线程是 VM 内部实现，与嵌入方线程隔离。

## 7. 性能路线图（非首版承诺）

1. NaN-boxing / tagged pointer 消除装箱
2. 属性访问内联缓存 + 隐藏类（hidden class）
3. computed goto 分派
4. 增量/并发 GC
5. 字符串分层表示（latin-1/UCS-2/UCS-4）
6. 基于 tracing 的方法内联
