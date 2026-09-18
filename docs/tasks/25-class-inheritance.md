# 25 class 继承与魔术方法

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v0.2 | ⬜ | [15 class 基础（无继承）](15-class-basics.md) |

## 任务目标

在任务 15 的 class 对象模型上交付单继承与魔术方法协议全集：

- **单继承**：`class Dog < Animal { ... }` 的编译与运行时装配；`struct MsClass` 增基类链，方法/静态方法/属性查找改为沿 MRO（基类链）线性查找；`isinstance`/`issubclass`/`msIsInstance` 的子类语义。
- **`super` 显式调用**：`super.method(args)`（非 `super()` 零参形式，见 [03-syntax.md](../language/03-syntax.md) §5），经新指令 `MS_OP_LOAD_SUPER` 与方法的 `homeClass` 指针实现。
- **静态方法继承**：`static func` 沿基类链解析，经子类名或子类实例均可调用。
- **魔术方法协议全集**（[02-types.md](../language/02-types.md) §8）：构造/表示（`__init__` 继承、`__str__`/`__repr__`/`__bool__`/`__int__`/`__float__`）、容器（`__len__`/`__iter__`/`__next__`/`__getitem__`/`__setitem__`/`__delitem__`/`__contains__`）、比较/哈希（`__eq__`/`__ne__`/`__lt__`/`__le__`/`__gt__`/`__ge__`/`__hash__`）、算术（`__add__`…`__pow__`、`__neg__`/`__pos__`）、位运算（`__and__`…`__rshift__`、`__invert__`）、`__call__`；`__enter__`/`__exit__` 与 `__iter__`/`__next__` 的查找约定由本任务定稿、指令级消费属任务 29/26。**无反射运算**（`__radd__` 等，规范明确首版不支持）。
- **脚本异常子类**：`class MyError < Exception` 接入任务 23 的异常体系（实例化、匹配、`raise`）。
- **方法变更传播接口** `msClassNoteMethodChanged`：类方法表变更沿子类链递归递增版本号，供任务 67 内联缓存失效消费。

完成后，`tests/ms/inheritance/` 与 `tests/ms/magic/` 的 ms 脚本验证全部行为；迭代协议（任务 26）、with 语句（任务 29）、内联缓存（任务 67）可直接消费本任务的查找约定与接口。

## 设计依据

- [03-syntax.md](../language/03-syntax.md) §5：`classDecl = "class" identifier [ "<" expr ] block`；单继承用 `<`；`super.method(...)` 显式调用父类方法（非零参形式）；`static func` 无 `self`；无多继承、无元类。§9：`isinstance(x, T)`、`issubclass(A, B)` 内建。
- [02-types.md](../language/02-types.md)：§2 真值——`__bool__` 覆盖、未定义时回落 `__len__`；§6 相等与哈希——实例默认按身份，「定义 `__eq__` 时必须同时定义 `__hash__`，否则哈希回落为身份哈希并产生一次警告」；§8 魔术方法协议全集（六类，本任务除 `__enter__`/`__exit__` 的指令消费外全部接线），反射方法首版不支持；§9 `isinstance` 含子类、`issubclass`。
- [04-exceptions.md](../language/04-exceptions.md) §1/§2：`except` 匹配类及其子类；`raise` 的对象必须是 `BaseException` 的实例或子类——脚本异常子类由此成为继承的自然推论。
- [08-vm-internals.md](../language/08-vm-internals.md) §2.1（指令定长 4 字节 `opcode|A|Bx`）、§2.2（指令清单为代表性列举、目标 ≤ 80 条，本任务新增 `MS_OP_LOAD_SUPER` 一条）、§4（「属性访问首版为 dict 查找 + 类 MRO 线性查找」——MRO 线性查找是规范既定的首版方案）。
- [09-c-api.md](../language/09-c-api.md) §5：`bool msIsInstance(MsState* L, MsObject* obj, MsObject* classObj)` 签名（任务 18 已声明「依赖继承语义定稿，随任务 25 加入」）。
- [15 class 基础](15-class-basics.md)（已定稿，本文直接沿用其定名）：`struct MsClass`/`struct MsInstance`/`struct MsBoundMethod`/`struct MsNameTable`；`msNewClass`/`msClassAddMethod`/`msClassAddStatic`/`msClassFindMethod`/`msClassFindStatic`/`msNewInstance`/`msClassInstantiate`；属性协议 `msObjGetAttr`/`msObjSetAttr`/`msObjDelAttr`；`LOAD_METHOD`/`CALL_METHOD` 的 `[recvSlot, callable]` 免分配约定；`MAKE_CLASS` 栈布局；统一调用入口假定名 `msVmCallValue`；其「设计依据」留下的评估项（`__init__` 返回值、绑定方法相等）由本文裁定（见下「歧义处理」第 8 条）。
- [06 对象模型基础](06-object-model.md)（已定稿）：`struct MsType { header, name }` 为最小定义，其文档明示「方法与继承语义由任务 15/25 在此结构上扩字段」——本文的 `base`/`flags` 扩展即该扩展点；`msObjectValueEquals`/`msObjectHash`/`msObjectIsTruthy` 是不取 `MsState` 的纯函数，不能调用脚本代码，故魔术方法经本文新增的有状态入口接入（见「详细设计」第 7 节）。
- [23 异常系统](23-exceptions.md)（已定稿）：内建异常层级为 `MsType` 对象 + `MsType.base` 基类链（其文档已按此字段存在消费，本文定稿该字段）；`msExceptionIsA` 沿基类链匹配；`struct MsException` 固定字段与 0/1 参构造约定；`msVmRaise`/`msVmRaiseFmt` 抛出入口。本任务起，类/实例相关错误点统一改抛脚本异常（`TypeError`/`AttributeError` 等），与任务 23 的统一改造对齐。
- [67 属性内联缓存](67-inline-cache.md)（已定稿）对本文的接口约定：`struct MsClass` 含 `uint32_t methodVersion`；`msClassAddMethod`/`msClassAddStatic` 内递增；`void msClassNoteMethodChanged(struct MsClass*)` 沿子类链递归递增祖先修改波及的版本——本文按此定名落地。
- [26 迭代协议](26-iteration-protocol.md)、[29 with 语句](29-with-statement.md)（已定稿）：两者均假定「魔术方法查找沿类 MRO、实例属性是否遮蔽以任务 25 为准」，本文第 5 节的统一查找约定即其消费口径。
- 任务 07（编译器，文档尚不存在）：基类子句发射、`super` 编译等挂接点为假定描述，实现时以对应任务文档定名为准。任务 08/09 假定的 `msObjectBinaryOp`/`msObjectUnaryOp` 慢路径归属、任务 10 假定的 `msObjectStr`/`msObjectRepr` 名，本文沿用，实现时对齐。

对规范歧义与空白的显式处理（实现与评审时以此为据）：

1. **魔术方法隐式触发只在类型上查找。** 运算符、内建函数、协议入口对魔术方法的查找只走类 MRO 的实例方法表：实例属性不遮蔽（`obj.__str__ = f` 不影响 `str(obj)`），静态方法表不参与。显式 `obj.__str__()` 仍走任务 15 的常规属性协议（实例属性可遮蔽）。此约定对齐 Python，任务 29 的 `__enter__`/`__exit__`、任务 26 的 `__iter__`/`__next__` 随之继承。
2. **`super` 的使用限定。** 规范只给出 `super.method(...)` 调用形式。决定：`super` 只允许出现在实例方法体（含其内层块）中、且必须直接构成 `super.name(args)` 调用；`super` 单独作值、`super.name` 不调用、嵌套函数/lambda 内、静态方法内、方法外使用，均为编译错误（退出码 2）。嵌套函数内支持（`__class__` 单元式捕获）列入路线图。
3. **`struct MsType` 增 `base`/`flags` 两字段**（任务 06 明示的扩展点，任务 23 已按 `base` 消费）；`struct MsClass` 改为内嵌 `struct MsType` 为首成员（`header`/`name` 偏移与任务 15 原文一致，方法表字段顺延，纯布局重排）。
4. **脚本异常子类的实例布局。** 基类链含 `BaseException` 的类（创建时打 `MS_TYPE_FLAG_EXCEPTION`）实例化时分配任务 23 的 `struct MsException`（而非 `MsInstance`），`header.type` 指向脚本类对象；无自定义 `__init__` 时走任务 23 的 0/1 参 `message` 默认协议。`msExceptionIsA`/`msVmRaise` 因基类链统一而零改动接入。
5. **继承内建非异常类型被拒绝。** `class X < int/str/list/...`：内建类型实例是固定 C 布局，脚本类实例是 `MsInstance`，二者不可兼容，v0.2 在 `MAKE_CLASS` 处报运行时 `TypeError`；继承内建异常类型允许（第 4 条）。开放内建类型继承列入路线图。
6. **无反射运算与比较的回退规则。** 算术/位运算只查左操作数类的魔术方法，未命中即 `TypeError`（不查右操作数，规范 §8 无 `__radd__`）。`==`/`!=` 例外地对称：先左 `__eq__`、未定义再查右 `__eq__`；`__ne__` 未定义时取 `__eq__` 结果的反（Python 3 语义）。序比较（`<` 等）只查左操作数。不设 `NotImplemented` 协议。
7. **魔术方法返回值校验。** `__str__`/`__repr__` 必须返回 str，`__bool__`/比较方法/`__contains__` 必须返回 bool，`__len__` 必须返回非负 int（负值报 `ValueError`），`__hash__` 必须返回 int，`__int__`/`__float__` 必须返回对应类型；违反抛 `TypeError`。`__init__` 返回值维持任务 15 的忽略语义（评估结论：保持向后兼容，不收紧）。
8. **绑定方法相等维持身份比较**（任务 15 留下的评估项）：不做「同 receiver 同 callable」值化，避免每次 `obj.m` 取值的相等语义牵连方法表快照问题。
9. **`__eq__` 无 `__hash__` 的警告时机。** 类创建（`MAKE_CLASS` 装配完成）时沿 MRO 检查一次：命中 `__eq__` 而未命中 `__hash__` 即向 stderr 打一条警告（每类定义执行一次，天然「一次」），哈希保持身份哈希。运行期经 C API 补加 `__eq__` 的边角不重复警告。
10. **`isinstance` 的元组形式**（`isinstance(x, (int, float))`）依赖 tuple 类型（任务 32），本任务只实现单类型参数形式，元组形式随任务 32 对齐。

## 详细设计

### 1. 文件与模块边界

- 扩展 `src/object/ms_class.h` / `ms_class.c`（任务 15 既有文件，guard `MSLANG_SRC_OBJECT_MS_CLASS_H_` 不变）：继承装配、MRO 查找、`msClassNoteMethodChanged`、魔术方法查找与全部有状态协议入口。
- 扩展 `src/object/ms_object.h` / `ms_object.c`（任务 06）：`struct MsType` 增字段；`msHeapInit` 为 20 个内建类型对象补 `base = NULL`、`flags = 0` 初始化。
- 扩展任务 13 的 `struct MsFunction`：增 `homeClass` 字段（构造函数时置 NULL）。
- VM 侧（任务 08 的分派循环）：`EQ`/`NE` 慢路径改调 `msObjectEq`；真值判定收口 `vmIsTruthy` 改调 `msObjectTruthy`（签名改为可失败）；`INDEX`/`SET_INDEX`/`DEL_INDEX`/`IN` 增实例分派；`CALL` 增 `MS_TYPE_INSTANCE` 的 `__call__` 分支；新增 `MS_OP_LOAD_SUPER` 分派；`msObjectBinaryOp`/`msObjectUnaryOp` 慢路径增实例分派。
- 编译器侧（任务 07 挂接点）：基类子句的表达式发射、`super` 编译规则、`MAKE_CLASS` 栈布局更新。
- 魔术方法名的驻留字符串集中缓存于 `MsState`（假定字段 `magicNames[MS_MAGIC_COUNT]`，初始化时一次驻留，避免每次协议触发重新驻留；`MsState` 字段名以任务 08/09 实现对齐为准）。

### 2. 类型对象与类对象的继承扩展

```c
// ms_object.h（任务 06 结构扩展；任务 23 已按 base 字段消费）
#define MS_TYPE_FLAG_SCRIPT_CLASS 0x1u   // 对象实为 struct MsClass（含方法表等扩展字段）
#define MS_TYPE_FLAG_EXCEPTION    0x2u   // 基类链含 BaseException；实例按 struct MsException 分配

struct MsType {
  struct MsObjectHeader header;
  struct MsString* name;
  struct MsType* base;        // NULL = 无基类；内建类型恒 NULL，异常层级见任务 23
  uint32_t flags;             // MS_TYPE_FLAG_*
};

// ms_class.h（任务 15 结构重排：内嵌 MsType，header/name 偏移不变）
struct MsClass {
  struct MsType type;                 // base 指向基类对象（MsClass 或 MsType）；flags 含 SCRIPT_CLASS
  struct MsNameTable methods;
  struct MsNameTable statics;
  uint32_t methodVersion;             // 方法表版本，任务 67 消费；变更经 msClassNoteMethodChanged
  struct MsClass** subclasses;        // 直接子类表，msAlloc/msRealloc 动态数组；版本传播用
  int subclassCount;
  int subclassCap;
};

// 任务 13 结构扩展
struct MsFunction {
  // ... 任务 13/14 既有字段 ...
  struct MsClass* homeClass;          // super 解析起点（定义该方法的类）；非方法函数为 NULL
};
```

- `MS_TYPE_FLAG_SCRIPT_CLASS` 是 `MS_TYPE_CLASS` 标签下区分 `MsClass` 与纯 `MsType`（内建类型、任务 23 异常类型）的判别位：两者 `header.tag` 同为 `MS_TYPE_CLASS`、`header.type` 同指共享 `type` 类型对象（任务 06 既定），无法靠头部区分。
- `MS_TYPE_FLAG_EXCEPTION` 在类创建时计算（沿基类链上溯命中任务 23 的 `BaseException` 类型对象即置位）；它同时充当 `MS_TYPE_INSTANCE` 标签下 `MsException` 布局与 `MsInstance` 布局的判别位（`header.type->flags & MS_TYPE_FLAG_EXCEPTION`）。
- 基类链无环是构造不变式：基类表达式求值时本类名尚未绑定，脚本无法成环；C API 调用方有维持无环的义务（`msClassNoteMethodChanged` 的递归依赖此式）。

### 3. 类定义编译与 MAKE_CLASS 更新

- **编译期**（任务 07 挂接点）：解除任务 15 对基类子句的拒绝；先发射基类表达式的求值代码（栈底压入基类对象，无 `<` 子句时压 `nil` 占位），再按任务 15 顺序发射方法名/函数对与 `INT(s)`，最后 `MAKE_CLASS A=m Bx=类名常量下标`。类体规则（仅 `func`/`static func`/`pass`、实例方法首参 `self`）不变。
- **栈布局**（对任务 15 布局的修订，实现时与任务 05/08 文档对齐）：自底向上为 `baseOrNil, name_0, fn_0, …, sname_0, sfn_0, …, INT(s)`。VM 弹栈顺序同任务 15，多弹一个栈底基类槽。
- **运行时装配**（`MAKE_CLASS` 分支）：
  1. 基类槽为 `nil` → 无基类；为 `MS_TYPE_CLASS` 对象 → 基类成立，但纯 `MsType`（无 `SCRIPT_CLASS`）且非异常类型（无 `EXCEPTION`）时报 `TypeError`（歧义处理第 5 条）；其余值报 `TypeError`（"base must be a class"）。
  2. `msNewClass` 建类；置 `type.base`、`type.flags`（`SCRIPT_CLASS` 恒置位；沿基类链命中 `BaseException` 加置 `EXCEPTION`）。
  3. 基类是 `MsClass` 时把新类追加进其 `subclasses`（`msRealloc` 扩容，GC 标记范围内）。
  4. 方法/静态方法入表（`msClassAddMethod`/`msClassAddStatic` 内部已含版本传播，见第 6 节；装配期无缓存消费者，多次递增无害）。
  5. `__eq__`/`__hash__` 警告检查（歧义处理第 9 条）。

### 4. MRO 线性查找与属性协议更新

- `msClassFindMethod` / `msClassFindStatic` 改为沿基类链线性查找：从 `klass` 起，每级若是 `MsClass`（`SCRIPT_CLASS`）查对应表，命中即返回；沿 `type.base` 上溯至 NULL。纯 `MsType` 级无脚本方法表，直接跳过。单继承下「线性化」即基类链本身，无需 C3。
- 实例 `GET_ATTR` 查找顺序扩展为：实例 `attrs` → MRO `methods`（命中建 `MsBoundMethod`）→ MRO `statics`。类对象 `GET_ATTR`：逐级 `statics` → `methods`，再上行。`LOAD_METHOD` 同步扩展（`recvSlot` 规则不变）。
- 子类覆盖同名方法即 MRO 先命中子类条目，遮蔽语义自然成立；实例属性遮蔽方法（任务 15 既定）优先级仍高于一切类级条目。

### 5. super 显式调用

新增指令 `MS_OP_LOAD_SUPER Bx`（`Bx` = 方法名常量池下标；`A` 保留不用，为任务 67 的缓存站点留位，与 `LOAD_METHOD` 同约定）：

- 编译器对 `super.name(args)` 发射：`LOAD_LOCAL 0`（`self`，任务 15 既定首槽位）→ `LOAD_SUPER Bx(name)` → 实参 → `CALL_METHOD A`。
- 运行时：栈顶为 `self`；取当前帧函数的 `homeClass`（编译期已保证非 NULL），从 `homeClass->type.base` 起沿 MRO 查 `methods` 表（不查 `statics`）；命中则把栈形变换为 `[recvSlot=self, callable]`（与 `LOAD_METHOD` 同约定）；基类缺失或全程未命中报 `AttributeError`。
- 继承场景中 `homeClass` 是**定义**该方法的类而非实例的运行时类：`Dog` 实例执行继承来的 `Animal.__init__` 内的 `super.__init__(...)` 时，解析起点是 `Animal` 的基类——与 Python 的 `__class__` 单元语义一致。
- `msClassAddMethod` 内存放的是脚本函数（`MS_TYPE_FUNCTION`）时顺带写入 `homeClass = klass`；同一函数对象经 C API 加入多个类时后者覆盖（边角情形，注释说明）。`msClassAddStatic` 不写（静态方法内 `super` 是编译错误）。

### 6. 方法变更传播（任务 67 既定接口）

```c
// Bumps klass->methodVersion and recurses into every subclass. Called from
// msClassAddMethod/msClassAddStatic (including same-name overwrite) and from
// any future C API that mutates a class's method tables. The class hierarchy
// is acyclic by construction (see 详细设计 §2), so recursion terminates.
void msClassNoteMethodChanged(struct MsClass* klass);
```

- 实现：`klass->methodVersion++` 后对 `subclasses[0..subclassCount)` 逐项递归。uint32 回绕的理论假命中窗口按任务 67 的既有裁定不予处理，注释说明。
- `MsClass` 之外（纯 `MsType` 基类级）无脚本方法表、不可变，不需要版本。

### 7. 魔术方法：统一查找与协议入口

魔术方法名枚举与统一查找（`ms_class.h`）：

```c
typedef enum {
  MS_MAGIC_INIT, MS_MAGIC_STR, MS_MAGIC_REPR, MS_MAGIC_BOOL, MS_MAGIC_INT, MS_MAGIC_FLOAT,
  MS_MAGIC_LEN, MS_MAGIC_ITER, MS_MAGIC_NEXT,
  MS_MAGIC_GETITEM, MS_MAGIC_SETITEM, MS_MAGIC_DELITEM, MS_MAGIC_CONTAINS,
  MS_MAGIC_EQ, MS_MAGIC_NE, MS_MAGIC_LT, MS_MAGIC_LE, MS_MAGIC_GT, MS_MAGIC_GE, MS_MAGIC_HASH,
  MS_MAGIC_ADD, MS_MAGIC_SUB, MS_MAGIC_MUL, MS_MAGIC_TRUEDIV, MS_MAGIC_FLOORDIV, MS_MAGIC_MOD,
  MS_MAGIC_POW, MS_MAGIC_NEG, MS_MAGIC_POS,
  MS_MAGIC_AND, MS_MAGIC_OR, MS_MAGIC_XOR, MS_MAGIC_INVERT, MS_MAGIC_LSHIFT, MS_MAGIC_RSHIFT,
  MS_MAGIC_CALL, MS_MAGIC_ENTER, MS_MAGIC_EXIT,
  MS_MAGIC_COUNT
} MsMagicId;

// Looks up a magic method for obj following the implicit-invocation rules
// (设计依据 歧义 1): instance method tables along the class MRO only; never
// instance attributes, never statics. Returns NULL when absent (never raises).
// Works for MsInstance and for MsException instances of script classes.
struct MsObject* msMagicFind(struct MsObject* obj, MsMagicId id);
```

有状态协议入口（失败经任务 23 的异常通道传播；任务 06 的三个纯函数保持不变，仅服务非实例标签与编译期常量去重）：

```c
// str/repr protocol: __str__ -> __repr__ -> default (task 15 缺位表示).
// Magic results must be str; otherwise TypeError. NULL = exception in flight.
struct MsObject* msObjectStr(MsState* L, struct MsObject* obj);
struct MsObject* msObjectRepr(MsState* L, struct MsObject* obj);

// Truthiness: __bool__ (must return bool) -> __len__ (0 is falsy) -> default
// (non-instances delegate to task 06 的 msObjectIsTruthy).
MsResult msObjectTruthy(MsState* L, struct MsObject* obj, bool* out);

// Equality: pointer shortcut, then __eq__ left-then-right (歧义 6); result
// must be bool. __ne__ falls back to negated __eq__. VM EQ/NE 慢路径与容器
// 键比较共用此入口；任务 06 的 msObjectValueEquals 不再承担实例比较。
MsResult msObjectEq(MsState* L, struct MsObject* a, struct MsObject* b, bool* out);

// Hash: __hash__ (must return int; mixed with the same 64-bit finalizer as
// task 06 int hashing) -> identity fallback (task 15). dict 键路径与 hash()
// 内建共用；任务 06 的 msObjectHash 不再承担实例哈希。
MsResult msObjectHashValue(MsState* L, struct MsObject* obj, uint32_t* out);
```

各指令/内建的接线点：

- **算术与位运算**：`msObjectBinaryOp`/`msObjectUnaryOp` 慢路径在既有内建类型分派之前增实例分支：左操作数（一元为唯一操作数）为实例时 `msMagicFind` 对应 id，命中即经 `msVmCallValue` 以 `[self, rhs]` 调用、结果原样返回（不设返回类型校验）；未命中落既有 `TypeError` 路径。无反射（歧义处理第 6 条）。`pow(a, b)` 内建与 `MS_OP_POW` 同路径。复合赋值无 `__iadd__` 系列，`a += b` 恒为 `a = a + b`。
- **序比较**：`LT`/`LE`/`GT`/`GE` 慢路径（含 `CMP_CHAIN` 的比较助手与任务 10 假定名 `msObjectCompare`，实现时收口为同一入口）查左操作数的 `__lt__` 等，结果必须 bool；未命中报 `TypeError`。
- **容器协议**：`MS_OP_INDEX`/`MS_OP_SET_INDEX`/`MS_OP_DEL_INDEX` 对实例分派 `__getitem__(key)`/`__setitem__(key, v)`/`__delitem__(key)`，未定义报 `TypeError`（"object is not subscriptable"）；`MS_OP_IN` 对实例查 `__contains__`，结果必须 bool；`len(x)`（09-c-api §6 的 `msLen`）增 `__len__` 分支。下标的负索引/切片语义由类自行实现（内建容器的切片属任务 30）。
- **真值**：`vmIsTruthy`（任务 08 预留的单一收口）改调 `msObjectTruthy`；`bool(x)` 内建同。`and`/`or`/条件跳转经同一入口，魔术方法抛错正常传播。
- **转换**：`int(x)`/`float(x)` 对实例查 `__int__`/`__float__`；`str(x)`/`print`/`repr(x)` 经 `msObjectStr`/`msObjectRepr`；`hash(x)` 经 `msObjectHashValue`；`callable(x)` 对含 `__call__` 的实例返真（任务 10 把 `repr`/`hash`/`callable` 排期「后续版本」，随本协议一并接线，属对该排期的显式提前，已在歧义清单对应条注明范畴）。
- **`__call__`**：`MS_OP_CALL` 增 `MS_TYPE_INSTANCE` 分支：`msMagicFind(obj, MS_MAGIC_CALL)` 命中则以 `self` 为 `argv[0]` 调用；未命中报 `TypeError`（"object is not callable"）。
- **迭代与上下文**：`__iter__`/`__next__` 的查找约定即 `msMagicFind`（任务 26 消费）；`__enter__`/`__exit__` 同约定（任务 29 消费，其实例属性遮蔽问题由歧义处理第 1 条闭合）。本任务只在测试中经显式调用验证这两个方法的查找行为本身。
- **静态方法不得充当魔术方法**：`msMagicFind` 不查 `statics` 表；`static func __str__` 之类的定义被协议忽略（不报错）。

### 8. 类型判定与脚本异常子类

```c
// True when derived is base or has base on its base chain. Both must be
// MS_TYPE_CLASS objects (MsClass or MsType); identity counts as subclass.
bool msClassIsSubclass(const struct MsObject* derived, const struct MsObject* base);

// 09-c-api §5 既定签名（任务 18  deferred 至本任务）：obj 的类取法——
// MsInstance 取 klass；MsException 实例取 header.type；其余取 header.type。
// classObj 非类对象时报 TypeError 并返 false。
bool msIsInstance(MsState* L, MsObject* obj, MsObject* classObj);
```

- 内建接线：`isinstance(x, T)` 改经 `msIsInstance`（单类型形式；元组形式见歧义处理第 10 条）；`issubclass(A, B)` 经 `msClassIsSubclass`，参数非类对象报 `TypeError`；`type(x)` 行为不变。
- **异常子类实例化**：`msClassInstantiate` 首步按 `klass->type.flags & MS_TYPE_FLAG_EXCEPTION` 分流——置位时经任务 23 的分配路径建 `MsException`（`header.type = (struct MsType*)klass`），否则 `msNewInstance`。随后统一走 `__init__` 协议：MRO 查 `__init__`（此时起继承基类的 `__init__`）；异常类无自定义 `__init__` 时套用任务 23 的 0/1 参 `message` 默认协议；普通类无 `__init__` 的行为维持任务 15 裁定（带参报 `TypeError`）。
- `raise` 与 `except` 匹配零改动：脚本异常子类实例即 `MsException`，`msExceptionIsA` 沿统一基类链上溯命中。
- 异常实例的属性协议：任务 23 的四个固定属性优先，其余动态属性走任务 23 的实例 dict 通道（若其 `struct MsException` 实现未含通用属性表，本任务对齐时在既有字段后补 `struct MsNameTable attrs`），之后沿 MRO 查方法表——与 `MsInstance` 的查找顺序同构。

### 9. 内存与 GC 纪律

- 新增标记面（任务 17 的遍历挂接点同步扩展）：`MsType.base`、`MsClass.subclasses` 数组全部元素、`MsFunction.homeClass`。类 ↔ 方法（`homeClass` 回指）与基类 ↔ 子类互指形成的环由标记-清除天然处理。
- `subclasses` 是强引用：父类存活则子类不被回收（类对象通常全局存活，接受此简化；弱引用子类表列入路线图）。
- `MAKE_CLASS` 装配期的根纪律沿用任务 15（方法名/函数对入表前跨分配入根），基类槽同理；`msClassInstantiate` 的实例入根纪律不变，异常布局分流不改根时序。
- 全部新增堆分配（`subclasses` 数组扩容）经 `msAlloc`/`msRealloc`/`msFree`；`msClassNoteMethodChanged` 不分配。

## 实现步骤

1. `struct MsType` 增 `base`/`flags` 与内建类型初始化补齐；`struct MsClass` 重排为内嵌 `MsType` 并增 `methodVersion`/`subclasses`；`struct MsFunction` 增 `homeClass`；`msHeapInit`/`msNewClass`/构造函数置初值。验证：任务 15 全部既有测试零改动通过（纯结构扩展）。
2. `msClassFindMethod`/`msClassFindStatic` 改 MRO 线性查找；`msObjGetAttr` 与 `LOAD_METHOD` 的查找顺序扩展。验证：仍无继承语法，行为与任务 15 逐点一致。
3. `msClassNoteMethodChanged` 与 `subclasses` 注册、两个 Add 函数内嵌调用、`homeClass` 写入。验证：经临时 C 驱动或后续步骤脚本间接断言；版本递增由任务 67 的消费测试兜底。
4. 编译器接通基类子句与 `MAKE_CLASS` 新栈布局、运行时基类校验与 flags 计算、`__eq__`/`__hash__` 警告。验证：继承定义、方法继承/覆盖、多层链、负例（非类基类、内建非异常类型基类）脚本。
5. `MS_OP_LOAD_SUPER` 与 `super` 编译规则。验证：`super.__init__` 链、覆盖方法经 super 调用、继承来的方法内 super 的解析起点、全部编译负例（嵌套函数内、静态方法内、非调用形式）。
6. `msMagicFind` 与 `MsState` 魔术名缓存；`msObjectStr`/`msObjectRepr`/`msObjectTruthy`/`msObjectEq`/`msObjectHashValue` 五个入口及 VM/内建接线（EQ/NE、真值、str/repr/print、len、int/float、hash、callable）。验证：`magic_str.ms`/`magic_bool_len.ms`/`magic_convert.ms`/`magic_compare.ms`。
7. 算术/位运算/序比较的慢路径实例分派与容器协议（`INDEX` 族、`IN`、`__call__`）接线。验证：`magic_arith.ms`/`magic_container.ms`/`magic_call.ms` 及返回值校验负例。
8. `msIsInstance`/`msClassIsSubclass` 与 `isinstance`/`issubclass` 内建接线；脚本异常子类实例化分流与 `MsException` 属性通道对齐。验证：`type_and_isinstance.ms`、`exception_subclass.ms`（raise/except 匹配、message 默认协议、`__str__` 覆盖）。
9. 任务 15 遗留错误点随任务 23 对齐为脚本异常（属性缺失 `AttributeError`、类对象写属性 `TypeError` 等）；GC 标记遍历扩展复查；Debug（ASAN/`/RTC`）全量无报告；`msCloseState` 分配计数归零；三平台 Debug/Release 构建与 `run_tests.py` 全绿。

## 测试方案

本任务晚于任务 09，一律用 ms 脚本测试（内建 `assert` + `print`；负例配 `<name>.exit` 同伴文件，运行时错误退出码 1、编译错误退出码 2，设施见任务 09）。本任务只交付设计文档，脚本随实现编写。可用设施边界：list/dict（任务 16）与 try/except（任务 23）可用；**不得使用** for-in（任务 26）、f-string（任务 28）、切片（任务 30）、tuple/bytes（任务 32），循环用 while 或三部式 for，字符串拼信用 `+`。

测试文件清单与覆盖点：

- `tests/ms/inheritance/basic.ms`：方法继承与覆盖、三层链 `A → B → C` 的逐层命中、子类无 `__init__` 时继承基类构造、实例属性在继承下的动态创建与遮蔽、基类表达式是非常量表达式（如函数返回值）。
- `tests/ms/inheritance/super_calls.ms`：`super.__init__(...)` 显式链式构造；覆盖方法内经 `super.m()` 调基类实现；多层继承中 super 的逐级上溯；继承来的方法内 super 以定义类为起点（`Dog` 实例跑 `Animal` 的方法时 super 从 `Animal` 的基类查）；super 调用中 `self` 传递正确（基类方法读写子类实例属性）。
- `tests/ms/inheritance/static_inherit.ms`：静态方法经子类名调用命中基类定义；子类覆盖静态方法后两条路径各自命中；静态方法与实例方法同名在继承下的优先级。
- `tests/ms/inheritance/type_and_isinstance.ms`：`type(sub) == Sub`；`isinstance(sub, Base)` 真、`isinstance(base, Sub)` 假、`isinstance` 对非类第二参报 `TypeError`（负例）；`issubclass(Sub, Base)`/`issubclass(A, A)` 真、非类参数负例；`==`/`is` 身份语义在继承下不变。
- `tests/ms/inheritance/exception_subclass.ms`：`class MyError < Exception` 的构造（0/1 参 message 默认协议）、`raise MyError("x")` 被 `except Exception` 与 `except MyError` 捕获、不被 `except ValueError` 捕获；自定义 `__init__` 写附加属性并读取；覆盖 `__str__` 后 `print(e)` 走覆盖版本；多层异常子类链。
- `tests/ms/magic/str_repr.ms`：`__str__` 优先、`__repr__` 回落、缺省表示不变；继承的 `__str__` 对子类实例生效；`print`/`str()`/`repr()` 三路径；实例属性 `obj.__str__ = ...` 不影响 `str(obj)`（隐式查找不看实例属性）。
- `tests/ms/magic/bool_len.ms`：`__bool__` 覆盖真值；未定义时 `__len__` 回落（0 为假）；两者皆无恒真；`if`/`while`/`not`/`and`/`or`/`bool()` 全入口一致。
- `tests/ms/magic/convert.ms`：`__int__`/`__float__` 经 `int()`/`float()` 生效；返回值类型校验负例。
- `tests/ms/magic/compare.ms`：`__eq__` 值相等与 dict 键查找（配合 `__hash__`）；`__ne__` 缺省取反；对称回退（仅右操作数定义 `__eq__`）；`__lt__` 等序比较与链式比较；未定义序比较报 `TypeError`（负例）；无 `__hash__` 时身份哈希回落（`__eq__`+无 `__hash__` 的警告只人工抽查 stderr，脚本不断言）。
- `tests/ms/magic/arith.ms`：`__add__`/`__sub__`/`__mul__`/`__truediv__`/`__floordiv__`/`__mod__`/`__pow__` 全量；`__neg__`/`__pos__`；位运算六个；复合赋值 `+=` 等价于 `a = a + b`；继承来的算术方法对子类生效；负例：`1 + obj`（无反射，左操作数非实例）报 `TypeError`。
- `tests/ms/magic/container.ms`：`__len__`（含 `len()`）、`__getitem__`/`__setitem__`/`__delitem__`、`__contains__`（`in`/`not in`）；未定义下标报 `TypeError`（负例）；显式调用 `obj.__iter__()`/`obj.__enter__()` 验证查找约定（协议指令消费属任务 26/29）。
- `tests/ms/magic/call.ms`：`__call__` 使实例可调用、参数转发、返回值；`callable()` 判定；继承的 `__call__`。
- 编译负例（各配 `.exit`，退出码 2）：`err_super_toplevel.ms`（方法外 super）、`err_super_nested.ms`（方法内嵌套函数/lambda 内 super）、`err_super_static.ms`（静态方法内 super）、`err_super_not_call.ms`（`super.m` 不调用、`x := super`）。
- 运行时负例（各配 `.exit`，退出码 1）：`err_base_not_class.ms`（`class A < 1`）、`err_base_builtin.ms`（`class A < int`）、`err_super_no_base.ms`（无基类方法的 `super.m()`）、`err_super_missing.ms`（基类链无该方法）、`err_magic_ret_str.ms`（`__str__` 返回非 str）、`err_magic_ret_bool.ms`（`__bool__` 返回非 bool）、`err_magic_ret_len.ms`（`__len__` 返回负数）、`err_magic_ret_hash.ms`（`__hash__` 返回非 int）、`err_no_radd.ms`（`1 + 实例`）、`err_call_noncallable.ms`（无 `__call__` 的实例被调用）。

## 验收标准

- [ ] `struct MsType` 增 `base`/`flags`（任务 23 的 `MsType.base` 消费口径与本文一致）；`struct MsClass` 内嵌 `struct MsType` 并含 `methodVersion`/`subclasses`；`struct MsFunction` 含 `homeClass`；全部结构体不 typedef、代码通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、堆分配只经 `msAlloc`/`msRealloc`/`msFree`）。
- [ ] 单继承：`class X < expr` 编译与装配正确；基类校验（nil/类对象/内建非异常类型拒绝/其他拒绝）与 `MS_TYPE_FLAG_SCRIPT_CLASS`/`MS_TYPE_FLAG_EXCEPTION` 计算正确；基类为 `MsClass` 时注册进 `subclasses`。
- [ ] MRO 线性查找：`msClassFindMethod`/`msClassFindStatic` 沿基类链；实例属性 → MRO 实例方法 → MRO 静态方法的查找顺序；子类覆盖遮蔽基类同名方法；`LOAD_METHOD`/`GET_ATTR` 一致。
- [ ] `super`：`MS_OP_LOAD_SUPER` 经 `homeClass->type.base` 起查、复用 `[recvSlot, callable]` 约定；继承来的方法内 super 以定义类为起点；五类误用（方法外/嵌套函数/静态方法/非调用形式）全部编译错误，退出码 2。
- [ ] `msClassNoteMethodChanged` 按任务 67 口径实现：自身版本递增并沿 `subclasses` 递归；`msClassAddMethod`/`msClassAddStatic`（含覆盖）内调用；`methodVersion` 字段位置与任务 67 假定一致。
- [ ] 魔术方法协议：`msMagicFind` 只查类 MRO 实例方法表（实例属性不遮蔽、静态方法不参与）；`__str__`/`__repr__`/`__bool__`/`__int__`/`__float__`/`__len__`/`__getitem__`/`__setitem__`/`__delitem__`/`__contains__`/`__eq__`/`__ne__`/`__lt__`/`__le__`/`__gt__`/`__ge__`/`__hash__`、九个算术、三个一元（`__neg__`/`__pos__`/`__invert__`）、六个位运算、`__call__` 全部接线；返回值类型校验与错误类型（`TypeError`/负 `__len__` 为 `ValueError`）符合本文。
- [ ] 无反射运算：算术/位运算只查左操作数；`__eq__` 对称回退、`__ne__` 缺省取反、序比较只查左操作数；`__eq__` 无 `__hash__` 时类创建打一次 stderr 警告且身份哈希回落。
- [ ] 类型判定：`msIsInstance`（09-c-api §5 签名）与 `isinstance`/`issubclass` 的子类语义；脚本异常子类实例化为 `MsException`、`raise`/`except` 匹配、message 默认协议与 `__str__` 覆盖正确。
- [ ] 既有行为不回退：任务 15 全部测试零改动通过（缺省表示、属性协议、`__init__` 忽略返回值、绑定方法身份相等等裁定维持）；任务 06 的三个纯函数签名不变。
- [ ] GC 与内存：`MsType.base`/`subclasses`/`homeClass` 纳入标记遍历；`msCloseState` 后分配计数归零；Debug 构建（ASAN/`/RTC`）无报告；构建产物只落在 `build/`；Win/Linux/macOS 三平台 Debug/Release 构建通过。
- [ ] `tests/ms/inheritance/` 与 `tests/ms/magic/` 下「测试方案」全部清单项实现并全数通过（不含 for-in/f-string/切片/tuple/bytes 用法），`python run_tests.py` 退出码为 0；负例脚本以各自预期退出码（1 或 2）失败。
- [ ] 无 TBD/TODO 占位；对任务 05/07/08/10/13/23 的接口假定（`MAKE_CLASS` 栈布局对齐、`super` 编译挂接点、`msObjectBinaryOp` 慢路径归属、`msObjectStr`/`msObjectRepr` 定名、`msVmCallValue`、`MsException` 属性通道）在实现时已按对应任务文档对齐；任务 67 的 `msClassNoteMethodChanged`/`methodVersion` 消费约定可直接对接。
