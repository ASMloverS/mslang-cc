# 15 class 基础（无继承）

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.1 | ⬜ | [13 函数与调用](13-functions-calls.md) |

## 任务目标

交付 mslang 的 class 基础能力（无继承）：

- 对象模块 `src/object/ms_class.{c,h}`：类对象（`struct MsClass`，`MS_TYPE_CLASS`）、实例对象（`struct MsInstance`，`MS_TYPE_INSTANCE`）、绑定方法对象（`struct MsBoundMethod`，`MS_TYPE_BOUND_METHOD`）三个类型的存储结构与操作接口；
- 类定义的编译与运行时构建：`class Name { func m(self) {...} static func s(...) {...} }` 经 `MS_OP_MAKE_CLASS` 在运行时装配为类对象，并以类名绑定到全局命名空间；
- 属性访问语义接线：`MS_OP_GET_ATTR` / `MS_OP_SET_ATTR` / `MS_OP_DEL_ATTR` 对实例与类对象的分派，属性经 `self.x = ...` 或 `obj.x = ...` 动态创建；
- 方法绑定与调用：`MS_OP_LOAD_METHOD` / `MS_OP_CALL_METHOD` 的免分配快路径（`obj.m(args)`），以及把方法取作一等值时产生 `MsBoundMethod`（`f := obj.m; f()`）；
- `__init__` 构造协议：调用类对象 `Point(1, 2)` 分配实例并调用 `__init__`；
- 静态方法：`static func` 无 `self`，经类名或实例调用均不绑定接收者。

完成后，脚本能定义类、构造实例、经显式 `self` 读写动态属性、调用实例方法与静态方法、把方法当作值传递，并通过 `tests/ms/class/` 的 ms 脚本验证。继承（`<` 基类、`super`）、魔术方法协议（`__str__`/`__eq__` 等，`__init__` 之外的全部魔术方法）、类对象的可调用定制（`__call__`）均不在本任务范围，属任务 25。

## 设计依据

- [03-syntax.md](../language/03-syntax.md) §5（class 语法）：
  - `classDecl = "class" identifier [ "<" expr ] block`；花括号类体，单继承用 `<`（本任务不实现继承，见「详细设计·类定义编译」的拒绝路径）。
  - 方法第一个参数必须是 `self`（关键字，调用时隐式传入实例）。
  - `static func` 定义静态方法，无 `self`，通过类名调用。
  - 属性在 `__init__` 中通过 `self.x = ...` 动态创建；无访问控制关键字，`_` 前缀仅是约定。
  - 无多继承、无元类。
- [03-syntax.md](../language/03-syntax.md) §6：属性 `a.b` 与调用 `f(x)` 同为优先级 2、左结合，`a.b(c)` 的解析形状由此确定。
- [02-types.md](../language/02-types.md)：
  - §1：类（`type`）与实例两个类型；实例可变，类型即其类。
  - §2：真值规则——实例不在假值清单中，故恒为真（`__bool__`/`__len__` 覆盖属任务 25）。
  - §6：实例 `==` 默认按身份；`is` 比较对象身份；实例哈希默认按身份（「定义 `__eq__` 时必须同时定义 `__hash__`，否则哈希回落为身份哈希」一条蕴含无 `__eq__` 的实例按身份哈希）。
  - §8：`__init__` 属构造/表示类魔术方法，是 v0.1 唯一接线的魔术方法。
  - §9：`type(x)` 返回类型对象；`isinstance(x, T)` 无继承时退化为类相等判定。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.2：`MS_OP_MAKE_CLASS` / `MS_OP_GET_ATTR` / `MS_OP_SET_ATTR` / `MS_OP_DEL_ATTR` / `MS_OP_LOAD_METHOD` / `MS_OP_CALL_METHOD` 指令清单；§3：`MsTypeTag` 已含 `MS_TYPE_CLASS` / `MS_TYPE_INSTANCE` / `MS_TYPE_BOUND_METHOD`，所有值为装箱 `MsObject*`，短字符串驻留；§4：属性访问首版为 dict 查找 + 类线性查找（无继承即单表查找），内联缓存留任务 67。
- [05 字节码格式与 MsProto](05-bytecode-proto.md)：上述六条指令的枚举与格式（均为 ABC 格式）；`MAKE_CLASS Bx` = 类名常量池下标、`LOAD_METHOD Bx` / `GET_ATTR` 等 `Bx` = 名字常量池下标的既定约定。`MAKE_CLASS` 的 `A` 操作数与「方法表自栈弹出」的栈布局在任务 05 未定稿，由本文补齐（见「详细设计·类定义编译」），实现时与任务 05/08 文档对齐。
- [09-c-api.md](../language/09-c-api.md) §3：GC 根纪律（跨分配存活的局部 `MsObject*` 必须 `msRootPush`/`msRootPop`）；§7：`msGetAttr`/`msSetAttr`/`msHasAttr` 公开 API 形状（其实现对本任务的对象类型分派到本模块）；§8：错误处理约定（返回 `NULL` + 错误槽，`msRaiseTypeError` 等）。
- [10-c-style.md](../language/10-c-style.md)：全部 C 接口遵循其规范（2 空格缩进、120 列行宽、K&R、指针星号贴类型、内部结构体不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`、include guard）。
- [11-project-layout.md](../language/11-project-layout.md) §1：`src/object/` 目录位置；§5 路线图 v0.1：class（无继承）。
- [09 最小可运行解释器](09-minimal-interpreter.md)：错误槽 → `MS_ERROR_RUNTIME` → CLI 退出码 1、编译错误退出码 2 的链路；`tests/ms/` + `run_tests.py`（含 `<name>.exit` 同伴文件）测试设施；内建 `assert`/`print`。
- 任务 06（对象模型基础，已定名）提供：`MsObjectHeader`、`struct MsString`（含缓存哈希）、字符串比较 `msStringEquals`、统一分配入口 `msObjectAlloc`、值相等 `msObjectValueEquals` / 哈希 `msObjectHash` / 真值 `msObjectIsTruthy` 分派入口（头文件 `"object/ms_object.h"`）；任务 08（VM 执行核心，已定名）提供：分派主循环 `msVmRun`、帧原语 `msVmPushFrame`、错误槽报告 `msRaiseRuntimeError`，其「指令实现范围」表把本任务的六条指令明确归属于此；任务 13（函数与调用，已定名）提供：函数对象 `struct MsFunction`、`MS_OP_CALL` 的实参装配与参数绑定算法，但统一的「以值调用」入口未定名，本文假定 `msVmCallValue`（见「详细设计」第 5 节）；任务 14（闭包与 upvalue，已定名）使方法可以捕获外层变量。任务 07（编译器）文档尚不存在：本文对编译器挂接点（class 声明编译段及其发射助手）的描述为假定，实现时以对应任务文档定名为准。

对规范歧义与空白的显式处理（实现与评审时以此为据）：

1. **`static` 不是关键字。** 01-lexical §4 的 37 个关键字清单（任务 03 已定稿为 `MS_TOKEN_KW_*` 枚举）不含 `static`，而 03-syntax §5 使用 `static func`。处理：parser 在 class 体内把紧跟 `func` 之前的标识符 `static` 作上下文关键字识别（class 体外的 `static` 仍是普通标识符）；此解析规则属任务 04 范围，本文仅声明依赖。
2. **`__init__` 的返回值。** 规范未规定。决定：v0.1 忽略 `__init__` 的返回值，构造表达式恒产出新实例（与 Python「返回非 None 报错」不同）；是否收紧由任务 25 评估，届时保持向后兼容优先。
3. **无 `__init__` 的类带参构造。** 规范未规定。决定：报运行时错误（"`<Name>() takes no arguments`"），与 Python 的 `object.__init__` 行为对齐。
4. **类对象的属性写入。** 规范只承诺实例属性动态创建。决定：v0.1 对类对象执行 `SET_ATTR`/`DEL_ATTR` 报运行时错误（"cannot set attribute on class object"），类级可变属性待需求明确后再开放。
5. **实例哈希与任务 16 文档的出入。** 02-types §6 蕴含实例默认身份哈希（可哈希），任务 16 文档的 dict 键契约把「未定义 `__hash__` 的实例」列为不可哈希——两处表述冲突。以语言规范 §6 为准：本任务注册身份哈希，实例可作 dict 键；任务 16/25 实现时与此对齐。
6. **class 体内容。** 规范未列举。决定：v0.1 类体内只允许实例方法（`func`）、静态方法（`static func`）与 `pass`，其他语句为编译错误（类体非常规语句执行位，避免「类体即作用域」的语义负担）。

## 详细设计

### 1. 文件与模块边界

- `src/object/ms_class.h`（guard `MSLANG_SRC_OBJECT_MS_CLASS_H_`）与 `src/object/ms_class.c`：`struct MsClass` / `struct MsInstance` / `struct MsBoundMethod`、内部名表 `struct MsNameTable`、全部构造与查找/读写接口。头文件自包含，include `<stdbool.h>` `<stdint.h>` 与任务 06 的 `"object/ms_object.h"`。
- VM 侧：本任务在任务 08 的分派循环中填充 `MS_OP_MAKE_CLASS` / `MS_OP_GET_ATTR` / `MS_OP_SET_ATTR` / `MS_OP_DEL_ATTR` 分支，并新增 `MS_OP_LOAD_METHOD` / `MS_OP_CALL_METHOD` 分支；`MS_OP_CALL` 增加 `MS_TYPE_CLASS`（实例化）与 `MS_TYPE_BOUND_METHOD`（插入接收者）两个被调对象分支。其他对象类型（str/list 等）的属性分派不在本任务范围。
- 编译器侧（任务 07 的代码内新增一个 class 声明编译段）：类体方法的子 Proto 生成沿用函数编译通路（任务 13），本任务定义其语义检查与 `MAKE_CLASS` 发射序列。
- **不用 `MsDict` 作存储**：dict 属任务 16，按 docs/tasks/README.md 的编号依赖规则本任务不得依赖它。方法表与实例属性表因此用本模块私有的字符串键哈希表 `struct MsNameTable`（见下）；任务 16 落地后两者并存（`MsNameTable` 键限定为字符串、无插入序承诺，接口更窄，不造成重复实现负担）。

### 2. 对象模型

```c
// String-keyed open-addressing table; keys are MsString (hash cached by the
// object model). No insertion-order guarantee. Embedded by value.
struct MsNameEntry {
  struct MsString* key;    // NULL = empty slot; MS_NAMETABLE_TOMBSTONE = deleted
  MsObject* value;
  uint32_t hash;           // cached key hash
};

struct MsNameTable {
  struct MsNameEntry* entries;  // msAlloc'd, power-of-two slots; NULL when empty
  int64_t mask;                 // slot count - 1; -1 when entries == NULL
  int64_t used;                 // live entries
  int64_t fill;                 // used + tombstones
};

struct MsClass {
  MsObjectHeader header;
  struct MsString* name;
  struct MsNameTable methods;   // instance methods: name -> function object
  struct MsNameTable statics;   // static methods:   name -> function object
};

struct MsInstance {
  MsObjectHeader header;
  struct MsClass* klass;
  struct MsNameTable attrs;     // dynamic attributes, created on first write
};

struct MsBoundMethod {
  MsObjectHeader header;
  MsObject* receiver;           // the instance (argv[0] at call time)
  MsObject* callable;           // function or C function object
};
```

- `MsNameTable` 算法：开放定址，`slot = hash & mask`，冲突线性探测；墓碑用 `MS_NAMETABLE_TOMBSTONE`（`(struct MsString*)-1` 哨兵）标记；负载因子 `fill * 3 >= (mask + 1) * 2` 时扩容并压实（丢弃墓碑、以缓存 `hash` 重建）；键比较用任务 06 已定名的 `msStringEquals`（驻留字符串走指针快速路径）。空表零分配（`entries == NULL`），首次插入分配 8 槽。
- 三个对象类型经任务 06 的统一分配入口 `msObjectAlloc` 创建（经 `MsState` 内嵌的 `MsHeap`，内嵌约定见任务 06/08 文档），类型标签分别为 `MS_TYPE_CLASS` / `MS_TYPE_INSTANCE` / `MS_TYPE_BOUND_METHOD`。
- 方法表存入的值是函数对象（任务 13 产物，含闭包）。`MsBoundMethod.callable` 允许为 C 函数（内建方法通路，任务 06/10 的方法表机制与本类型共用）。

模块接口（`MsObject*` 参数均为调用方持有的根；返回的新对象遵循 09-c-api §3 转移语义——立即消费或入根）：

```c
// Name table (also used by MsInstance.attrs).
void      msNameTableInit(struct MsNameTable* table);
void      msNameTableDestroy(MsState* L, struct MsNameTable* table);  // frees the buffer only
MsObject* msNameTableGet(const struct MsNameTable* table, const struct MsString* key);  // NULL when absent
MsResult  msNameTableSet(MsState* L, struct MsNameTable* table, struct MsString* key, MsObject* value);
bool      msNameTableDelete(struct MsNameTable* table, const struct MsString* key);     // false when absent

// Class construction and lookup (lookups return NULL when absent, never raise).
MsObject* msNewClass(MsState* L, struct MsString* name);
MsResult  msClassAddMethod(MsState* L, struct MsClass* klass, struct MsString* name, MsObject* fn);
MsResult  msClassAddStatic(MsState* L, struct MsClass* klass, struct MsString* name, MsObject* fn);
MsObject* msClassFindMethod(struct MsClass* klass, const struct MsString* name);
MsObject* msClassFindStatic(struct MsClass* klass, const struct MsString* name);

// Instances.
MsObject* msNewInstance(MsState* L, struct MsClass* klass);

// Bound methods.
MsObject* msNewBoundMethod(MsState* L, MsObject* receiver, MsObject* callable);
```

- 重名处理：同一张表内 `msClassAddMethod` 重名即覆盖（后者胜），实例方法与静态方法分属两表、允许同名（经实例访问时实例方法优先，经类名访问时静态方法优先，见第 4 节）；`__init__` 只是 `methods` 表中的普通条目，无特殊存储。
- 实例类型判定：`instanceof` 关系退化为指针比较 `inst->klass == klass`（无继承）；`isinstance` 的元组形式与 `issubclass` 的完整语义属任务 10/25，本任务只保证单类参数路径正确。

### 3. 类定义编译与 MS_OP_MAKE_CLASS

编译器（任务 07 挂接点）对 `class Name { ... }` 的处理：

- **编译期检查**（失败即编译错误，CLI 退出码 2）：
  - 每个实例方法的首个参数必须是 `self`（关键字参数名，parser 已放行）；`static func` 不得有 `self` 首参之外的隐式接收者（静态方法参数表原样）。
  - 类体内只允许 `func` / `static func` / `pass`（设计依据第 6 条）。
  - 遇到基类子句（`<` expr）：parser 已能解析，编译器报编译错误 "inheritance is not supported yet (task 25)"，明确指向后续任务。
- **发射序列**：按类体声明顺序，对每个实例方法发射 `LOAD_CONST <方法名>` + `MAKE_FUNCTION`（子 Proto 与 upvalue 处理同任务 13/14）；然后对每个静态方法同样发射其名与函数对；最后发射 `LOAD_CONST <int 静态方法数 s>` 与 `MAKE_CLASS A=m Bx=<类名常量下标>`（m = 实例方法数），类对象留在栈顶，随后照常 `STORE_GLOBAL Name`（顶层）或写入相应作用域。
- **栈布局约定**（补齐任务 05 未定稿的 `MAKE_CLASS` 操作数细节，实现时与任务 05/08 文档对齐）：自底向上为 `name_0, fn_0, …, name_{m-1}, fn_{m-1}, sname_0, sfn_0, …, sname_{s-1}, sfn_{s-1}, INT(s)`。VM 执行 `MAKE_CLASS`：弹栈顶 int 得 `s`；弹 2s 个值填入 `statics` 表；再弹 2m 个值填入 `methods` 表（`A` 给出 m）；以 `consts[Bx]` 的类名经 `msNewClass` 建类对象并压栈。全部条目入表经 `msNameTableSet`；OOM 走统一失败路径。
- 方法子 Proto 约定：`paramCount` 含 `self`（首槽位）；`isAsync` 首版恒 `false`（async 方法随任务 43 自然支持，无需特殊处理）；其余元数据同普通函数。

### 4. 属性访问协议（GET_ATTR / SET_ATTR / DEL_ATTR）

统一入口为按对象类型分派的三个语义函数，VM 指令分支与 09-c-api §7 的公开 API（`msGetAttr`/`msSetAttr`/`msHasAttr`）共用：

```c
// Generic attribute protocol (dispatches on object type). Get returns NULL
// with the error slot set when the attribute is missing or obj supports no
// attributes; Set/Del report errors via the error slot and return the result.
MsObject* msObjGetAttr(MsState* L, MsObject* obj, struct MsString* name);
MsResult  msObjSetAttr(MsState* L, MsObject* obj, struct MsString* name, MsObject* value);
MsResult  msObjDelAttr(MsState* L, MsObject* obj, struct MsString* name);
```

分派规则（本任务只定义 `MS_TYPE_INSTANCE` 与 `MS_TYPE_CLASS` 两行，其余类型落到任务 06/10/16 的分支，未命中时报 "`<type>` object has no attribute '<name>'"）：

| 操作 | 实例 | 类对象 |
|---|---|---|
| GET | ① `attrs` 命中 → 其值（实例属性遮蔽同名方法）；② `methods` 命中 → 新建 `MsBoundMethod{receiver: obj, callable: fn}`（方法是一等值）；③ `statics` 命中 → 函数本身（不绑定）；④ 未命中 → 运行时错误 | ① `statics` 命中 → 函数本身（`Animal.create` 经类名取得）；② `methods` 命中 → 未绑定函数（`Animal.speak(a)` 显式传实例可用）；③ 未命中 → 运行时错误 |
| SET | 写 `attrs`，不存在即动态创建（`self.x = v` 与 `obj.x = v` 同路径）；OOM 传播 | 运行时错误（设计依据第 4 条） |
| DEL | `attrs` 删除；缺失报运行时错误；不影响 `methods`/`statics`（`del obj.m` 当 `m` 是方法时报缺失错误） | 运行时错误 |

### 5. 方法绑定与调用（LOAD_METHOD / CALL_METHOD / CALL）

`obj.m(args)` 是热点路径，采用免分配的双指令序列（不为每次调用创建 `MsBoundMethod`；拆出独立 `CALL_METHOD` 而非复用 `CALL`，为任务 67 的内联缓存留位）：

- `LOAD_METHOD Bx`：栈顶为接收者 `obj`，名字取 `consts[Bx]`。按第 4 节 GET 的查找顺序解析，但**不创建对象**，把栈形统一变换为 `[.., recvSlot, callable]`：
  - 实例属性命中（值为任意可调用对象）→ `recvSlot = nil`，`callable = 该值`；
  - 实例方法命中 → `recvSlot = obj`，`callable = fn`；
  - 静态方法命中（经实例或类名）→ `recvSlot = nil`，`callable = fn`；
  - 未命中 → 运行时错误。
- `CALL_METHOD A`：栈形 `[.., recvSlot, callable, arg_1..arg_A]`。`recvSlot` 非 nil 时以 `recvSlot` 为 `argv[0]`、共 `A+1` 个实参调用 `callable`；为 nil 时退化为普通 `A` 参调用（字段存函数、静态方法、`Animal.speak(a)` 均走此路径）。`nil` 作哨兵是内部约定：字段值为 nil 时本就会在「不可调用」检查处报错，不产生歧义。
- `MS_OP_CALL` 新增两个被调对象分支：
  - `MS_TYPE_BOUND_METHOD`：把 `receiver` 插入为 `argv[0]` 后走任务 13 的常规调用装配（等价于 `CALL_METHOD` 的绑定路径，但接收者来自对象字段而非栈）。
  - `MS_TYPE_CLASS`：实例化，见第 6 节。
- 调用经统一调用入口（任务 13 未定名，本文假定 `MsObject* msVmCallValue(MsState* L, MsObject* callable, int64_t argc, MsObject** argv)`，实现时以任务 13 实现对齐为准）；参数个数不匹配等错误由该入口报告。
- `self` 的运行时身份即 `argv[0]`：方法体内 `self.x` 编译为对首槽位的 `GET_ATTR/SET_ATTR`，无额外机制。方法经 `f := obj.m` 取作值后经 `MS_OP_CALL` 调用，绑定语义不变。

### 6. `__init__` 构造协议

`MS_OP_CALL`（或 `CALL_METHOD`，静态方法经类名调用时同路径）遇到 `MS_TYPE_CLASS` 被调对象时执行实例化：

```c
// Instantiation protocol behind CALL on a class object. Always returns the
// new instance on success; returns NULL with the error slot set on failure.
MsObject* msClassInstantiate(MsState* L, struct MsClass* klass, int64_t argc, MsObject** argv);
```

流程：

1. `msNewInstance(L, klass)` 分配实例（`attrs` 为空表，零分配初始化）；实例立即入根（后续调用可能触发 GC/分配）。
2. `msClassFindMethod(klass, "__init__")`（名字符串在编译期入常量池，VM 侧缓存于 `MsState`，避免每次构造重新驻留）：
   - 命中 → 以新实例为 `argv[0]`、后接 `argc` 个构造实参，经 `msVmCallValue` 调用；`__init__` 抛错则原样传播（实例随 GC 回收）；**返回值被丢弃**（设计依据第 2 条），构造表达式产出实例。
   - 未命中 → `argc != 0` 时报运行时错误 "`<Name>() takes no arguments`"（设计依据第 3 条）；`argc == 0` 直接产出空白实例。
3. 弹出临时根，返回实例。

- `__init__` 体内的 `self.name = name` 即第 4 节 SET 路径的属性动态创建；构造期间实例已完全可用（可把 `self` 传给其他函数）。
- 嵌套构造（`__init__` 内 `return Other(...)` 或字段赋新实例）无特殊状态，自然成立。

### 7. 相等、哈希、真值与内建接线

- **相等**：`MS_OP_EQ`/`MS_OP_NE` 对实例、类对象、绑定方法均按身份（指针）比较（02-types §6）；绑定方法不做「同 receiver 同 callable 即相等」的值化（v0.1 简化，与 Python 不同，列入任务 25 评估）。`MS_OP_IS` 同指针判定，不涉及本任务新逻辑。
- **哈希**：任务 06 的 `msObjectHash` 分派为 `MS_TYPE_INSTANCE` 注册身份哈希（以指针值混淆而成，实例在 GC 下不移动，地址稳定；移动式 GC 列入路线图时再改句柄方案）；类对象同按身份。设计依据第 5 条记录了与任务 16 文档表述的对齐义务。
- **真值**：实例、类对象、绑定方法恒为真（02-types §2 的假值清单不含实例；`__bool__`/`__len__` 回落属任务 25）。
- **默认表示**：`str(inst)`/`print(inst)` 在魔术方法缺位时产出 `"<<Name> instance>"`，`str(klass)` 产出 `"<class '<Name>'>"`；`__str__`/`__repr__` 协议属任务 25，本任务保证缺位表示不含地址（测试可比对、输出确定）。
- **内建分派补齐**（任务 10 的内建函数体内增加分支，本任务接线）：`type(inst)` 返回其类对象、`type(klass)` 返回内建 `type` 类型对象（任务 06 产物）；`isinstance(x, T)` 对实例做单类指针比较（`T` 非类对象时报 TypeError 式错误）；`hasattr`/`getattr`/`setattr`/`delattr` 落到第 4 节的 `msObjGetAttr` 等统一入口（`getattr` 的默认值参数形式：缺属性时返回默认值而非报错，仅此一处吞掉「缺失」类错误）。

### 8. 内存与 GC 纪律

- 全部堆分配（三个对象本体、`MsNameTable` 的条目缓冲区扩容）经 `msAlloc`/`msRealloc`/`msFree`；`msCloseState` 的全量回收路径（任务 09 约定，正式 GC 属任务 17）按逆序释放条目缓冲区与对象本体。
- 根纪律（09-c-api §3）：`msClassInstantiate` 中新实例跨 `__init__` 调用必须入根；`MAKE_CLASS` 装配期间弹出的方法名/函数对在入表前若跨分配（表扩容）需暂存入根；`msNewBoundMethod` 的 `receiver`/`callable` 由调用约定保证是根（求值栈上的值天然是根）。
- 为任务 17 留口：三个类型的子对象遍历集中为文件内 `static` 辅助函数（类：name + 两表的 value；实例：klass + attrs 的 value；绑定方法：receiver + callable），GC 标记阶段复用，不在 VM 中散落字段访问。

## 实现步骤

1. 建 `src/object/ms_class.h` / `ms_class.c` 骨架：`struct MsNameTable` 五个接口（init/destroy/get/set/delete、开放定址 + 墓碑 + 压实扩容）。验证：编译器暂不可用时以临时 C 驱动或经后续步骤的脚本间接验证；表的增长/删除/重查行为由步骤 4 起的脚本断言覆盖（本任务不写 C 单元测试，见测试方案）。
2. 实现三个对象类型与构造函数（`msNewClass`/`msClassAddMethod`/`msClassAddStatic`/查找/`msNewInstance`/`msNewBoundMethod`），注册类型标签与默认表示（第 7 节的 `str` 缺位输出）。验证：脚本 `class A { pass }`、`print(A)` 输出 `<class 'A'>`。
3. 编译器接通 class 声明：类体语法检查（self 首参、静态方法、仅 func/pass）、继承子句拒绝、方法子 Proto 生成与第 3 节发射序列。验证：语法负例脚本退出码 2（缺 `self`、类体内赋值语句、`<` 基类）。
4. 实现 `MS_OP_MAKE_CLASS` 分支：栈布局弹出、两表装配、`STORE_GLOBAL` 后类名可用。验证：空类与多方法类的定义脚本；类名可作为值传递、赋给变量。
5. 实现 `msObjGetAttr`/`msObjSetAttr`/`msObjDelAttr` 的实例与类对象分派，接通 `MS_OP_GET_ATTR`/`MS_OP_SET_ATTR`/`MS_OP_DEL_ATTR` 与 09-c-api §7 公开 API。验证：属性动态创建/覆盖/删除、缺属性报错、类对象写属性报错的脚本断言。
6. 实现 `MS_OP_LOAD_METHOD`/`MS_OP_CALL_METHOD` 双指令与 `MS_OP_CALL` 的 `BOUND_METHOD` 分支。验证：`obj.m(args)` 直调、`f := obj.m; f()` 一等值绑定、实例属性遮蔽方法、静态方法经类名与实例两种调用。
7. 实现 `msClassInstantiate` 与 `MS_OP_CALL` 的 `CLASS` 分支（`__init__` 协议、无 `__init__` 的零参/带参两路径、返回值丢弃）。验证：构造协议全清单脚本断言。
8. 内建接线：`type`/`isinstance`/`hasattr`/`getattr`（含默认值）/`setattr`/`delattr` 的类与实例分支；相等/真值/身份哈希注册。验证：对应脚本断言与实例作身份比较（`is`、`==`）用例。
9. 根纪律复查与内存收尾；Debug 构建（ASAN / `/RTC`）跑全部 class 测试无报告；`msCloseState` 后分配计数归零；全平台（Win/Linux/macOS）× Debug/Release 构建验证，`run_tests.py` 全绿。

## 测试方案

本任务晚于任务 09，一律使用 ms 脚本测试（`testing` 模块在任务 40 才存在，本阶段用内建 `assert` + `print` 自断言；负向用例以 `<name>.exit` 同伴文件声明预期退出码——运行时错误为 1、编译错误为 2，由 `run_tests.py` 驱动，设施约定见任务 09）。本任务只交付设计文档，脚本随实现编写。注意任务 16（list/dict）晚于本任务，测试脚本**不得使用** list/dict 字面量与方法，多值场景用多个变量替代。

测试文件清单（`tests/ms/class/`）与覆盖点：

- `class_basic.ms`：空类（仅 `pass`）与多方法类的定义；类名绑定到全局、类作为值赋给变量/传入函数；`print` 类的缺位表示 `<class 'A'>`；末尾 `print("class basic ok")`。
- `instantiate.ms`：`__init__` 带参构造并写入属性（`self.name = name`）；两个实例属性互不影响；无 `__init__` 的零参构造产生空白实例；实例的缺位表示 `<A instance>`。
- `init_protocol.ms`：`__init__` 返回值被忽略（显式 `return 42` 仍产出实例）；`__init__` 内调用同一实例的其他方法；`__init__` 内构造另一个类的实例（嵌套构造）；构造实参个数与 `__init__` 形参不匹配的错误由任务 13 的装配报告（并入负例）。
- `attributes.ms`：方法外动态创建（`p.age = 3` 在顶层直接写）；覆盖已有属性；实例属性遮蔽同名方法（`p.speak = ...` 后 `p.speak` 取到属性值）；`del p.age` 后再读报错（负例独立脚本）；`hasattr`/`getattr`（含默认值）/`setattr`/`delattr` 内建路径。
- `methods.ms`：`obj.m(args)` 经显式 `self` 读写属性；方法返回值；方法体内局部变量与 `self` 属性互不混淆；`f := obj.m; f()` 与把绑定方法传入函数再调用（一等值）；`obj.m` 绑定在属性赋值之后仍指向原方法（绑定时机语义快照）。
- `static_methods.ms`：`static func` 经类名调用（`Animal.create("x")` 返回新实例）；经实例调用同一静态方法（不绑定接收者）；静态方法与实例方法同名时两条访问路径各自命中（第 4 节优先级）。
- `type_and_identity.ms`：`type(p) == Point`、`type(Point)` 为内建 `type`；`isinstance(p, Point)` 真、`isinstance(p, Other)` 假；`p is p`、`p1 is p2` 假；`==`/`!=` 按身份；实例恒为真（`assert(p)`、`if p { ... }`）。
- 负例脚本（各配 `<name>.exit`；编译错误退出码 2，运行时错误退出码 1）：
  - `err_missing_self.exit=2`：实例方法首参非 `self`；静态方法首参为 `self` 不报错（静态无隐式接收者，首参名为 self 视为普通参数——此用例只覆盖实例方法）。
  - `err_class_body_stmt.exit=2`：类体内出现赋值/表达式语句。
  - `err_inheritance.exit=2`：`class B < A { ... }` 报「未支持」编译错误（任务 25 落地后此脚本移除）。
  - `err_no_such_attr.exit=1`：读不存在的实例属性、类属性。
  - `err_set_class_attr.exit=1`：`A.x = 1`、`del A.x`。
  - `err_init_args.exit=1`：无 `__init__` 的类带参构造（"takes no arguments"）。
  - `err_call_noncallable_attr.exit=1`：调用值为非函数的属性（`p.age = 3; p.age()`）。
  - `err_del_missing_attr.exit=1`：`del` 不存在的实例属性；`del` 方法名（方法不在 `attrs`）。

错误消息文本不纳入断言（`run_tests.py` 不比对 stderr），实现时人工抽查一次「未支持继承」「takes no arguments」等关键消息内容。

## 验收标准

- [ ] `src/object/ms_class.{c,h}` 存在，guard 为 `MSLANG_SRC_OBJECT_MS_CLASS_H_`，头文件自包含；代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、`struct MsClass`/`struct MsInstance`/`struct MsBoundMethod`/`struct MsNameTable` 不 typedef、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] 方法表与属性表用本模块私有 `struct MsNameTable`（不依赖任务 16 的 `MsDict`）；空表零分配，扩容压实正确。
- [ ] 类定义编译：类体仅允许 `func`/`static func`/`pass`；实例方法首参必须为 `self`（编译错误，退出码 2）；继承子句报明确的「未支持」编译错误；`MAKE_CLASS` 的 `A`=实例方法数、栈顶 int=静态方法数的栈布局约定与任务 05/08 文档对齐落实。
- [ ] 属性协议：实例属性动态创建/覆盖/删除，查找顺序为实例属性 → 实例方法 → 静态方法；实例属性遮蔽同名方法；类对象的 `SET_ATTR`/`DEL_ATTR` 报运行时错误。
- [ ] 方法绑定：`obj.m(args)` 走 `LOAD_METHOD`/`CALL_METHOD` 免分配路径且语义与「先 GET 再 CALL」一致；`f := obj.m; f()` 经 `MsBoundMethod` 正确绑定；静态方法经类名与实例调用均不绑定接收者。
- [ ] `__init__` 协议：带参构造转发实参、返回值被忽略恒产出实例；无 `__init__` 时零参构造成功、带参构造报运行时错误；构造期间实例已可用（`__init__` 内可调用本实例方法）。
- [ ] 内建接线：`type`/`isinstance`/`hasattr`/`getattr`（含默认值）/`setattr`/`delattr` 对类与实例行为如本文；实例 `==`/`is` 按身份、恒为真、注册身份哈希；缺位表示为 `<ClassName instance>` / `<class 'Name'>`（不含地址）。
- [ ] 跨分配存活的局部 `MsObject*` 全部遵守根纪律；`msCloseState` 后无残余分配计数；Debug 构建（ASAN / `/RTC`）无内存错误与泄漏报告；构建产物只落在 `build/`。
- [ ] `tests/ms/class/` 下「测试方案」全部清单项实现并全数通过（不含 list/dict 用法），`python run_tests.py` 退出码为 0；负例脚本以各自预期退出码（1 或 2）失败。
- [ ] Win/Linux/macOS 三平台 Debug/Release 构建通过。
- [ ] 无 TBD/TODO 占位；与任务 05/07/13 的接口假定（`msVmCallValue`、编译器挂接点、`MAKE_CLASS` 的 `A` 操作数与栈布局等）在实现时已按对应任务文档对齐；继承与 `__init__` 之外的魔术方法仍属任务 25 范围。
