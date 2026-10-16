# 任务 07 编译器实现设计

## 目标与范围

依据 `docs/tasks/07-compiler.md` 交付单趟 AST → 字节码编译器，新增
`src/compiler/ms_compiler.h` 与 `src/compiler/ms_compiler.c`，并以
`tests/c/test_compiler.c` 独立验收。实现字面量、名字与块级作用域、基础表达式、
位置参数调用、单目标声明/赋值、零/单值返回、基础 `func`/`lambda`、REPL 最后表达式
输出、常量折叠和死跳转消除。后续任务拥有的语法继续走 E307 占位路径。

当前 `MsProto` 不能安全进入 `MsObject*` 常量池：它没有对象头，而
`msProtoAddConst` 会读取对象头。实施编译器前必须补齐子 Proto 的表示和所有权，
但不在本任务提前实现 GC。

## 方案选择

### 采用：内部 Proto 标签与父 Proto 所有权

为 `MsProto` 增加对象头和仅内部使用的 `MS_TYPE_PROTO`，新增子 Proto 专用的常量池
接口。Proto 不挂入 `MsHeap`，仍由 `msProtoFree` 释放；父 Proto 递归拥有子 Proto，
builder 负责清理尚未定稿的子 Proto。该方案保持现有 `msProtoBuild`、
`msCompilerCompile` 和顶层 `msProtoFree` 的所有权模型。

### 未采用：立即完整 GC 对象化

经 `msObjectAlloc` 创建 Proto 并同步改造堆销毁、GC 扫描和释放接口，会提前承担任务
17 的工作，还会引入 `msProtoFree` 与 `MsHeap` 的双重所有权，范围过大。

### 未采用：常量池改为带标签联合体

该方案能清楚区分标量对象和子 Proto，但会改变 `MsProto.consts` 布局及所有现有和
后续消费者，超出任务 07 的合理范围。

## Proto 表示与所有权

`MS_TYPE_PROTO` 是内部常量标签，不代表脚本可见类型：

- `MS_TYPE_COUNT` 保持 20，`heap->types` 不增加 Proto 类型项，`msHeapInit` 行为不变。
- `MS_TYPE_PROTO` 位于有效堆对象标签范围之外；`msObjectAlloc` 仍只接受普通的
  20 个堆对象标签。
- `MsProto` 的对象头以其为标签，`header.type == NULL`、`gcNext == NULL`。对象头注释
  明确：非空的 `header.type` 不变量只适用于挂入堆的对象。
- `msTypeTagName` 能识别该内部标签；普通堆对象的标签数量及既有测试语义不变。

新增接口：

```c
int msProtoAddProto(struct MsProtoBuilder* b, struct MsProto* proto);
const struct MsProto* msProtoConstProto(const struct MsProto* proto, int index);
```

规则如下：

- `msProtoAddProto` 不做值去重；成功后把子 Proto 所有权移交给 builder，失败时所有权
  仍归调用方。普通 `msProtoAddConst` 不接受带 Proto 标签的值。
- `msProtoConstProto` 只在目标常量确为子 Proto 时返回其只读指针，供测试和后续 VM
  使用，避免把任意常量强转为 `MsProto*`。
- `msProtoBuilderInit` 初始化内部 Proto 标签；`msProtoBuild` 成功时把整棵子 Proto 树
  随产物移交调用方。
- `msProtoBuilderDestroy` 和 `msProtoFree` 递归释放各自拥有的子 Proto 及其四条数组，
  但不释放数字、字符串等由 `MsHeap` 持有的常量，也不释放 `name`/`sourceFile`。
- 构建中途失败时，数组和已加入的子 Proto 仍归 builder，由统一 destroy 路径回收；
  尚未成功加入父 builder 的子 Proto 由当前编译函数上下文回收。

## 编译器结构与单趟生成

公开入口保持任务 07 定义：

```c
void msCompilerInit(struct MsCompiler* c, struct MsHeap* heap, struct MsDiagList* diags,
    const char* sourceFile, uint32_t flags);
void msCompilerDestroy(struct MsCompiler* c);
MsResult msCompilerCompile(struct MsCompiler* c, const struct MsAst* root, struct MsProto** out);
```

`msCompilerCompileStmt` / `msCompilerCompileExpr` 及作用域、发射、回填助手均为
文件内 `static`。生成只经过 `MsProtoBuilder`，不引入 IR 或独立优化 pass。

每个模块和函数对应一个函数上下文及一个 `MsProto`：

- 模块名为 `<main>`，参数数为 0；lambda 名为 `<lambda>`。
- 普通参数依次占槽 `0..paramCount-1`；默认参数、`*args`、`**kwargs` 报 E307。
- 函数体最外层块直接逐句编译，不额外 push scope；只有嵌套块引入新作用域。
- 子 Proto 经 `msProtoAddProto` 加入外层常量池，定义点发射 `MAKE_FUNCTION` 或
  `MAKE_LAMBDA`，再按声明规则绑定名字；`isAsync` 照传到子 Proto。
- 隐式返回统一补 `LOAD_NIL; RETURN`，末尾已有 `RETURN` 时不重复补齐。

## 作用域、名字与隐藏槽

名字解析顺序为当前函数局部、外层函数局部、全局/内建：

- 当前函数局部按最内层优先解析，支持遮蔽和兄弟块槽位复用。
- 命中外层函数局部时报 E310；upvalue 发射属于任务 14。
- 未命中局部时生成 `LOAD_GLOBAL`/`STORE_GLOBAL`，未定义名留待运行时报错。
- 局部名按字符串内容比较，不依赖驻留后的指针身份，因此超过 32 字节的名字也能
  正确声明、读取和遮蔽。
- 模块深度 0 的 `:=` 不声明局部，直接生成 `STORE_GLOBAL`；模块块内及函数内声明
  使用局部槽。右值先于目标声明生成，保留 `x := x` 读取外层旧值的语义。

隐藏槽按生命周期分两类：

1. 表达式语句丢弃槽在每个作用域首次需要时惰性分配，在该作用域内复用，pop scope
   后失效。作用域记录局部槽基数和自己的丢弃槽，防止内层块首次使用后遗留失效槽号。
2. `and`/`or` 短路临时槽在表达式内部分配，生成结束后立即恢复 `localCount`；高水位
   `localMax` 仍保留其实际需求。

函数上下文维护作用域栈、当前/最大局部槽数、当前/最大求值栈深。所有路径在汇合点
保持相同的编译期栈深。

## 表达式、字面量与窥孔优化

二元、一元、逻辑、条件和单段比较按任务 07 的操作码映射生成；位置调用按被调表达式、
从左到右实参、`CALL A=n` 的顺序生成。`not in` 与 `is not` 分别展开为 `IN; NOT` 和
`IS; NOT`。链式比较段数大于 1、关键字实参和其余未支持节点报 E307。

字面量处理遵循 AST 切片不保证 NUL 结尾的事实：

- 数字先复制切片、删除下划线并补 NUL；整数显式识别 `0x`/`0o`/`0b`/十进制及对应
  基数，浮点使用清理后的缓冲区转换，检查完整消费和范围错误。
- 一元负号直接作用于十进制整数字面量时允许幅值 `9223372036854775808`，从而正确
  支持 `INT64_MIN`；同一正字面量不在该语境下仍报 E308。
- 普通字符串去掉引号后经 `msLexerUnescape` 按长度解码，再以明确长度调用
  `msStringNew`；解码缓冲区无论成功与否都释放。raw string 去掉反引号后直接按长度
  构造。不能用依赖 NUL 终止的字符串构造方式。
- `true`、`false`、`nil` 使用专用指令，不进入常量池。

常量折叠严格限制为任务 07 白名单：数值 `+ - *`、一元 `+ - ~` 和 `not`。整数先检出
溢出，溢出时保留原指令；除法、取余、幂、比较和字符串运算不折叠。折叠递归与正常
AST 编译共用节点深度预算。

常量条件只生成实际选取的分支；回填时，目标为下一条指令的无条件 `JUMP` 改写为
`NOP`，条件跳转不改写。常量池沿用同类型值去重，但浮点零只有在符号位也相同时才可
合并，避免 `+0.0` 与 `-0.0` 因语言层 `==` 而共享错误常量；NaN 的既有不去重行为不变。

逐指令跟踪求值栈深和高水位。条件表达式在编译 else 分支前恢复 else 入口深度，并在
汇合点断言平衡。空模块生成 `LOAD_NIL; RETURN`，所以 `localCount == 0` 且
`stackSize == 1`；任务 07 文档中“空模块 stackSize 为 0”的旧表述同步修正。

## 诊断与资源失败

编译器区分可恢复诊断和致命 sticky 错误：

- OOM、builder 失败、E309、E311 及局部槽/作用域等结构性超限进入 sticky 并中止。
- E307、E308、E310、E312 等可恢复节点错误记录诊断后继续，以便单文件收集最多
  20 条。失败的表达式统一发射 `LOAD_NIL` 维持栈形；失败的语句跳过该节点剩余生成。
- 每次诊断后检查 `msDiagListIsFull`，第 20 条后立即停止。诊断收集期间产生的 Proto
  不会作为成功结果返回。
- OOM 诊断沿用 parser 的 `code = 0`、消息 `out of memory` 约定。OOM 优先返回
  `MS_ERROR_OOM`；本次编译新增任一语法诊断则返回 `MS_ERROR_SYNTAX`。
- 所有失败路径保持 `*out == NULL`。函数上下文、数字/字符串临时缓冲区、builder 和
  子 Proto 按各自所有权对称清理；OOM 注入结束后显式关闭故障注入再检查泄漏。

## 测试设计

新增 `tests/c/test_compiler.c`，测试助手使用真实 lexer → parser → compiler 管线，
通过指令解码、常量对象检查、Proto 元数据、行号表、必要的 `msProtoDump` 黄金文本及
诊断字段验证结果。拆卸顺序为 compiler、顶层 Proto、AST arena、heap、diag；所有路径
最终断言分配统计归零。

覆盖任务 07 测试清单，并加入以下针对当前实现的回归用例：

- 超过 32 字节的局部名声明、读取和遮蔽。
- 带下划线的浮点数和各进制整数，以及 `INT64_MIN` 语境。
- 同一常量池中的 `0.0` 与 `-0.0` 保留各自符号位。
- 首次表达式丢弃发生在内层块时，出块后声明和使用的局部不被覆盖。
- 空模块 `stackSize == 1`。
- 嵌套 Proto 在成功、构建失败和 OOM 路径上的所有权与无泄漏。
- 第 20 条编译诊断后停止，以及常量折叠路径触发 E311。

Proto 辅助接口在 `tests/c/test_proto.c` 增加成功移交、失败保留所有权、递归释放和
非法常量类型防护测试；对象标签测试继续断言脚本可见类型数为 20。

## 构建、文档与验收

- 在 `CMakeLists.txt` 将编译器加入 `mslang`，通过现有
  `mslang_add_c_test(test_compiler)` 注册测试。
- 在严格警告配置下执行完整构建和 `ctest`，并进行 Release 验证；所有构建产物只放在
  `build/` 下。
- 同步 `docs/tasks/05-bytecode-proto.md`、`06-object-model.md` 和
  `07-compiler.md` 中 Proto 所有权、内部标签、隐藏槽和空模块栈深的描述。
- 全部测试通过且文档一致后，才把任务 07 的验收项和状态标记为完成。
