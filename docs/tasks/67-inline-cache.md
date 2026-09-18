# 67 属性内联缓存

| 阶段 | 状态 | 依赖任务 |
|---|---|---|
| v1.0 | ⬜ | [64 性能基准套件](64-benchmarks.md) |

## 任务目标

为 mslang VM 的属性访问接入内联缓存（inline cache），落地 [08-vm-internals.md](../language/08-vm-internals.md) §7 性能路线图第 2 项的「属性访问内联缓存」部分（隐藏类不在本任务范围）。首版的属性访问是「实例属性表哈希查找 + 类方法表查找 + MRO 线性查找」（§4、任务 15 的 `msObjGetAttr` 查找顺序），每次访问全量执行；本任务在每个属性访问指令旁挂接单态/多态缓存站点，使类型稳定的热点访问以「一次指针比较 + 一次数组读」完成。

交付内容：

- 新模块 `src/vm/ms_ic.h` / `src/vm/ms_ic.c`：缓存站点与条目结构、单态→多态→超多态的状态机、GET_ATTR / SET_ATTR / LOAD_METHOD 三条指令的命中与未命中路径；
- `struct MsProto` 扩展站点数组，编译器为属性指令分配站点下标（复用未使用的 `A` 操作数，不改字节码格式）；
- 类型变更失效机制：实例属性表变更经「槽位键核验」天然失效，类方法表变更经版本号失效；
- GC 对接：缓存持有的函数对象纳入 Proto 的标记遍历；
- 以任务 64 基准套件（经任务 65/66 更新后的最新基线）为对照的验收门槛。

完成后，属性密集的脚本（方法调用循环、字段读写循环）达到本文规定的提速门槛，全部既有测试套件回归通过，且缓存关闭时（未命中/超多态）语义与无缓存路径逐字节一致。`DEL_ATTR` 与 `CALL_METHOD` 本身不接缓存（前者是冷路径，后者的解析已由配对的 `LOAD_METHOD` 承载）。

## 设计依据

- [08-vm-internals.md](../language/08-vm-internals.md)
  - §2.1：指令定长 4 字节（`opcode(8) | A(8) | Bx(16)`）——指令内没有空余位嵌入缓存，这是「站点旁挂于 Proto」决策的直接依据。
  - §2.2：属性类指令 `MS_OP_GET_ATTR` / `MS_OP_SET_ATTR` / `MS_OP_DEL_ATTR` 与方法调用对 `MS_OP_LOAD_METHOD` / `MS_OP_CALL_METHOD`。
  - §4：「属性访问首版为 dict 查找 + 类 MRO 线性查找；内联缓存列入性能路线图」——本任务即该条目落地。
  - §5：GC 根集合与标记遍历——缓存条目持有函数对象引用，必须纳入标记，否则函数会被误回收。
  - §7 路线图第 2 项：「属性访问内联缓存 + 隐藏类（hidden class）」。本任务只实现内联缓存；隐藏类要求实例属性从哈希表改为槽位数组布局，属对象模型重构，不随本任务进行（见「详细设计·不采用隐藏类方案的理由」）。
- [10-c-style.md](../language/10-c-style.md)：§1 文件组织与 include guard、§2 格式化、§3 命名、§4 typedef 规则（内部结构体不 typedef、枚举允许 typedef）、§6 内存纪律（堆分配只经 `msAlloc/msRealloc/msFree`）、§8 断言。
- [11-project-layout.md](../language/11-project-layout.md)：§1 `src/vm/` 目录位置；§5 路线图 v1.0 性能项「按基准测试选取」——门槛未达成时以数据收尾而非强行合入。
- [15 class 基础](15-class-basics.md)（已定稿，本文直接引用其定名）：
  - 统一属性协议入口 `msObjGetAttr` / `msObjSetAttr` / `msObjDelAttr` 与实例查找顺序（实例 `attrs` → 类 `methods` → 类 `statics`）；
  - `struct MsInstance{header, klass, attrs}`、`struct MsClass{header, name, methods, statics}`、开放定址的 `struct MsNameTable`（`entries`/`mask`/`used`/`fill`，键为 `struct MsString*`，哈希缓存，冲突线性探测）；
  - `LOAD_METHOD`/`CALL_METHOD` 拆成两条指令即「为任务 67 的内联缓存留位」，本任务在 `LOAD_METHOD` 上兑现该预留；
  - 任务 15 已定 `GET_ATTR`/`SET_ATTR`/`LOAD_METHOD` 为 ABC 格式、`Bx` = 名字常量池下标，`A` 操作数语义未定——本文定稿 `A` = 缓存站点下标（见「详细设计·站点挂接」）。
- [06 对象模型基础](06-object-model.md)（已定稿）：`MsObjectHeader{tag, type, ...}`；实例的 `header.type` 是共享的内建 `instance` 类型对象，**不能**用作实例的缓存判别键（判别键必须是 `inst->klass`）；短字符串驻留（≤ 32 字节同内容同指针）是缓存键指针核验的成立条件。
- 任务 25（class 继承与魔术方法，文档尚不存在）：MRO 线性查找的起点与类的方法版本维护（方法增删波及子类）。本文假定 `struct MsClass` 含基类链与方法版本字段 `methodVersion`、以及版本传播入口 `msClassNoteMethodChanged(struct MsClass*)`（祖先修改递归波及全部子类），实现时以对应任务文档定名为准。
- 任务 64（性能基准套件，文档尚不存在）：基准运行入口与结果落盘约定沿用任务 65/66 的假定口径——`bench/run_benchmarks.py` 驱动、结果落盘 `bench/results/<label>.json`、同机三轮取中位数；本文的对比基线为任务 65/66 落地后更新的最新结果文件（假定 `bench/results/v1.0-latest.json` 语义），基准项名称同为假定，实现时以任务 64 文档定名为准。
- 任务 05/07（编译器侧站点下标分配）与任务 17（GC 标记入口 `msGcMarkValue`/Proto 遍历挂接点）的相关接口为假定命名，实现时以对应任务文档定名为准。

对规范歧义与空白的显式处理（实现与评审时以此为据）：

1. **缓存挂接在 Proto 侧，不挂接在指令侧。** 指令定长 4 字节、运行期只读（字节码可在协程间共享），无法内嵌缓存指针；常见替代「运行期改写指令流」与只读代码、GC 稳定性冲突。决定：`struct MsProto` 增加并行站点数组，属性指令的 `A` 操作数携带站点下标（编译期分配，非运行期改写），取站点是一次数组下标访问，零查找开销。
2. **实例属性槽位缓存不加表版本号，靠键指针核验。** 实例属性表（`MsNameTable`）在增删/扩容 rehash 后槽位布局变化，直觉方案是给表加版本号、缓存条目存版本；但键核验已足够：命中要求「缓存槽位上的键指针 == 本站点的名字符串指针」，满足时读出的就是该属性的当前值——无论表是否 rehash（rehash 后键恰好落回同槽位的「巧合命中」同样是正确命中）。这省掉每实例一个版本字段与全部写入路径的维护点。删除产生的墓碑键 != 名字指针，天然未命中。
3. **方法缓存必须加类版本号。** 类方法表条目缓存的是「名字 → 函数对象」的解析结果，键核验不可行（缓存里不存方法表槽位，存的是解析产物）；类指针不变而方法表内容可变（`msClassAddMethod` 覆盖同名方法），故条目记录填充时的 `methodVersion`，命中要求版本一致。
4. **不做负缓存（negative cache）。** 属性缺失是运行时错误路径（08-vm-internals §4 的语义），不缓存「缺失」结论——动态属性随时可能被写入，负缓存的失效面大于收益。
5. **单实例单态缓存的局限明示。** 实例属性条目以类为判别键、槽位为载荷：同类实例若属性插入顺序不同（条件分支里建属性），槽位不同，同站点服务它们会单态抖动并升级多态；这是无隐藏类方案的固有代价，由多态条目（上限 4）吸收，超出后落超多态慢路径。隐藏类消除该问题的评估列入任务 68 之后的路线图，不在本任务承诺。

## 详细设计

### 1. 文件与模块边界

- `src/vm/ms_ic.h`（guard `MSLANG_SRC_VM_MS_IC_H_`）：站点/条目结构、状态枚举、全部公开函数声明；自包含，include `<stdbool.h>` `<stdint.h>` 与任务 06 的 `"object/ms_object.h"`，前向声明 `struct MsString` `struct MsClass` `struct MsInstance` `struct MsProto` 与 `MsState`。
- `src/vm/ms_ic.c`：命中判定、未命中更新、状态迁移、GC 标记钩子的实现；辅助函数一律文件内 `static`。
- 改动面：`src/vm/ms_proto.h`（任务 05，`MsProto` 增字段）、`src/vm/ms_vm.c` 或任务 66 之后的 `src/vm/ms_vm_ops.h`（三条指令的语义处理函数接入缓存）、`src/object/ms_class.h`（`struct MsClass` 增 `methodVersion`，`msClassAddMethod`/`msClassAddStatic` 内递增）、编译器（任务 07，站点计数与 `A` 操作数分配）、GC 的 Proto 遍历（任务 17，调用标记钩子）。
- 模块堆分配仅两处：Proto 的站点数组（`icCount > 0` 时经 `msAlloc`，随 Proto 释放）、多态条目的 `poly` 数组（升级多态时 `msAlloc` 固定 4 项，站点销毁时 `msFree`）。站点数组的 `msFree` 收口在 Proto 的销毁路径（任务 05 的释放函数），无第二处释放点。

### 2. 站点挂接：Proto 侧并行数组 + A 操作数下标

`struct MsProto` 扩展（任务 05 的结构体上增量）：

```c
struct MsProto {
  // ... task 05 fields unchanged ...
  struct MsIcSite* icSites;   // cache sites, msAlloc'd when icCount > 0; owned by the proto
  int icCount;                // number of attribute-access instructions in this proto
};
```

- 编译器（任务 07 挂接点）在生成 `GET_ATTR`/`SET_ATTR`/`LOAD_METHOD` 时维护每 Proto 计数器，把站点下标写入指令的 `A` 操作数（0..icCount-1），`Bx` 照旧为名字常量池下标。这是对任务 15 未定语义的 `A` 的定稿，字节码格式与译码宏不变。
- `A` 为 8 位：保留 `0xFF` 为「无站点」哨兵，单 Proto 可用站点上限 255；超出时编译器对该指令发射 `A = MS_IC_SITE_NONE`，VM 走无缓存慢路径（正确性不受影响）。`LOAD_METHOD` 之外的 `CALL_METHOD` 的 `A` 仍是实参个数（任务 15 约定），不接站点。
- Proto 创建时若 `icCount > 0` 经 `msAlloc` 分配站点数组并逐项 `msIcSiteInit`（状态 UNUSED，零值即可）；`icCount == 0` 时 `icSites == NULL`，零开销。
- VM 取站点：`site = proto->icSites[A]`（debug 构建 `MS_ASSERT(A < proto->icCount)`）。指令流保持只读，缓存不随协程/帧复制——同一 Proto 的并发执行共享站点（v0.3 调度器下多工作线程执行同一 Proto 的情形见「并发与内存序」）。

### 3. 缓存条目与状态机

```c
#define MS_IC_SITE_NONE 0xFF   // A-operand sentinel: no cache site for this instruction
#define MS_IC_POLY_MAX 4       // polymorphic entries per site before going megamorphic

typedef enum {
  MS_IC_STATE_UNUSED,          // never executed
  MS_IC_STATE_MONO,            // one inline entry
  MS_IC_STATE_POLY,            // poly[0..polyCount), polyCount <= MS_IC_POLY_MAX
  MS_IC_STATE_MEGA             // gave up caching; always slow path
} MsIcState;

typedef enum {
  MS_IC_KIND_NONE,
  MS_IC_KIND_INST_ATTR,        // instance attribute at a cached slot of the receiver's own table
  MS_IC_KIND_METHOD,           // instance method resolved via class/MRO; LOAD_METHOD binds receiver
  MS_IC_KIND_STATIC            // static method resolved via class/MRO; no receiver binding
} MsIcKind;

struct MsIcEntry {
  MsTypeTag expectedTag;       // receiver tag at fill time
  const void* expectedShape;   // instance: inst->klass; class object: the class itself; other tags:
                               // obj->header.type
  uint32_t classVersion;       // MsClass.methodVersion at fill time (METHOD/STATIC only)
  int32_t slotIndex;           // cached slot in the receiver's attrs table (INST_ATTR only)
  struct MsObject* callable;   // resolved function (METHOD/STATIC); traced by GC via msIcMarkProto
  MsIcKind kind;
};

struct MsIcSite {
  MsIcState state;
  int polyCount;               // valid when state == MS_IC_STATE_POLY
  struct MsIcEntry mono;       // valid when state == MS_IC_STATE_MONO
  struct MsIcEntry* poly;      // msAlloc'd MS_IC_POLY_MAX entries; NULL unless POLY
};
```

判别键 `expectedShape` 的取法（文件内 `static const void* icShapeOf(const struct MsObject* obj)`）：

- `MS_TYPE_INSTANCE`：`inst->klass`（实例的 `header.type` 全类共享，不可用——06 已定稿）；
- `MS_TYPE_CLASS`：类对象指针自身（类对象的 `header.type` 同为共享的 `type` 类型对象）；
- 其余标签（module、str 等内建类型的方法访问）：`obj->header.type`。

状态迁移（只升不降，无降级路径）：

```
UNUSED --首次未命中--> MONO（填入单态条目）
MONO   --异键未命中--> POLY（msAlloc 4 项数组，迁入旧条目 + 新条目）
POLY   --异键未命中且未满--> 追加条目；已满 --> MEGA（msFree poly 数组，此后恒走慢路径）
```

MEGA 不再缓存：属性访问的超高多态站点（如异构容器遍历）继续缓存只会浪费内存与填充时间，慢路径行为与无缓存完全一致。条目查找在 POLY 下为 ≤4 次的线性扫描（tag + shape 两次指针比较每项）。

### 4. 命中判定与失效语义

**INST_ATTR 条目**（GET_ATTR / SET_ATTR 共用），命中当且仅当：

1. `obj->header.tag == entry->expectedTag`（即 `MS_TYPE_INSTANCE`）；
2. `((struct MsInstance*)obj)->klass == entry->expectedShape`；
3. `entry->slotIndex <= inst->attrs.mask`（表可能已缩容/重建，防越界）；
4. `inst->attrs.entries[entry->slotIndex].key == name`——指针比较。

第 4 条是全部失效机制的支点（设计依据第 2 条）：属性删除（墓碑）、rehash 搬移、异布局同类的键不同，全部表现为键指针不等 → 未命中 → 慢路径重解析并更新条目。`name` 是本站点的常量池名字符串（编译期固定）；属性名是短标识符，经任务 06 驻留后同内容同指针，写入路径存入表中的正是该常量池对象，指针比较因此充分。超长（> 32 字节）不驻留的属性名永远指针不等 → 恒未命中走慢路径，语义正确、仅无加速，属可接受边角（动态属性名几乎恒为短名）。

**METHOD / STATIC 条目**，命中当且仅当：

1. tag 与 shape 同上匹配；
2. `klass->methodVersion == entry->classVersion`（`klass` 为实例的类或类对象自身；MRO 场景的版本覆盖见「类型变更失效」）。

命中产物：`callable`（已解析的函数对象）。`GET_ATTR` 命中 METHOD 条目仍需按任务 15 语义创建 `MsBoundMethod`（缓存省掉的是查找，不是绑定分配）；`LOAD_METHOD` 命中时直接产出 `[recvSlot=obj, callable]`（METHOD）或 `[recvSlot=nil, callable]`（STATIC），免分配路径不变。

### 5. 公开接口

```c
// Site lifecycle. Init zeroes (state UNUSED); Destroy frees the poly array only
// (the sites array itself is freed by the proto's destroy path, task 05).
void msIcSiteInit(struct MsIcSite* site);
void msIcSiteDestroy(MsState* L, struct MsIcSite* site);

// GET_ATTR. Try: returns true on hit with *out set (borrowed; for METHOD hits the
// bound method is freshly allocated and follows the usual transfer discipline).
// Miss: full lookup via msObjGetAttr, then updates the site; NULL = error slot set.
bool msIcTryGetAttr(struct MsIcSite* site, struct MsObject* obj, const struct MsString* name,
    struct MsObject** out);
struct MsObject* msIcGetAttrMiss(MsState* L, struct MsIcSite* site, struct MsObject* obj,
    struct MsString* name);

// SET_ATTR. Try: returns true on hit (existing slot overwritten in place).
// Miss: full msObjSetAttr (including dynamic creation), then updates the site.
bool msIcTrySetAttr(struct MsIcSite* site, struct MsObject* obj, const struct MsString* name,
    struct MsObject* value);
MsResult msIcSetAttrMiss(MsState* L, struct MsIcSite* site, struct MsObject* obj,
    struct MsString* name, struct MsObject* value);

// LOAD_METHOD: produces the [recvSlot, callable] pair consumed by CALL_METHOD
// (task 15's allocation-free convention).
bool msIcTryLoadMethod(struct MsIcSite* site, struct MsObject* obj, struct MsObject** recvSlot,
    struct MsObject** callable);
MsResult msIcLoadMethodMiss(MsState* L, struct MsIcSite* site, struct MsObject* obj,
    struct MsString* name, struct MsObject** recvSlot, struct MsObject** callable);

// GC hook (task 17): marks the cached callables of every site of proto. Called from
// the proto traversal in the mark phase; INST_ATTR entries carry no object reference.
void msIcMarkProto(MsState* L, struct MsProto* proto);
```

VM 指令处理（任务 66 落地后在 `ms_vm_ops.h` 的对应处理函数内，此前在 `ms_vm.c` 的 case 体内）统一形态：

```
A = msOpDecodeA(insn)
if (A != MS_IC_SITE_NONE && msIcTry*(site, ...)) 命中收尾
else 走 msIc*Miss（内部 = 慢路径 + 站点更新）
```

`DEL_ATTR` 不接缓存，维持任务 15 的 `msObjDelAttr` 直调。

### 6. 未命中时的条目填充规则

`msIc*Miss` 在慢路径成功后按解析结果决定填充内容：

- 解析落在实例 `attrs`：填 `INST_ATTR` 条目（slotIndex = 命中槽位）；该实例随后动态增删属性不影响条目正确性（键核验兜底，第 4 节）。
- 解析落在类 `methods`：填 `METHOD` 条目（callable = 函数对象，classVersion = 当前版本）。
- 解析落在类 `statics`：填 `STATIC` 条目。
- 解析落在其他类型（module 属性、内建类型方法等）：填 `METHOD`/`INST_ATTR` 的对应形态，shape 取 `header.type`；该路径的表结构由各类型任务文档定，填充逻辑集中在本模块，未覆盖的标签不填充（恒慢路径）。
- SET_ATTR 动态**创建**属性：慢路径写入后填 INST_ATTR 条目（slotIndex = 新槽位）；同站点下一次同形状写入即命中。

同键重复未命中（如键核验失败的常驻情形）不迁移状态——状态只在「shape 不同的新键」出现时迁移，避免抖动站点被误判为多态。

### 7. 类型变更失效

- **实例属性表变更**（增/删/扩容 rehash）：无需主动失效，第 4 节的槽位键核验逐条拦截；无版本字段、无失效广播。
- **类方法表变更**：`struct MsClass` 增 `uint32_t methodVersion`，`msClassAddMethod`/`msClassAddStatic`（含覆盖同名）内递增。MRO（任务 25）下缓存条目记录的是解析起点类的版本，祖先类的方法变更必须波及全部后代类版本——假定任务 25 提供 `msClassNoteMethodChanged(struct MsClass*)` 沿子类链递归递增，本模块只消费版本号，不维护传播（实现时以任务 25 文档定名为准）。版本回绕（uint32）理论存在，2^32 次方法热修改才触发一次假命中窗口，不予处理并注释说明。
- **SET_ATTR 命中的写值路径**：集中收口在文件内 `static void icSlotWrite(struct MsInstance* inst, int32_t slot, struct MsObject* value)` 一处；任务 68（增量 GC）接入写屏障时只改这一处，VM 指令体不直接写字段。
- **Proto 共享与并发**：站点数组每 Proto 一份，多协程/多工作线程并发执行同一 Proto 时并发读写站点。v0.3（任务 46/47）前的单线程执行无竞争；调度器落地后，站点迁移（MONO→POLY 的 `msAlloc`）需与读者竞态——届时以「读者读到旧状态顶多多走一次慢路径、写者经原子发布指针」的无锁方案收口，本任务在结构上保证条目填充是「先完整构造、末位指针写入」的可发布形态，并在 `msIcMarkProto` 的 GC 遍历处注释该约定。并发最终方案属任务 47 范围，本文不展开。

### 8. 与任务 65/66 的兼容

- **NaN-boxing（任务 65）**：值表示切换不改 `MsObjectHeader` 与 `MsString` 布局；命中判定只触碰对象头与表条目，经 `msValueToPtr` 取得对象指针后逻辑不变。缓存接口的参数类型随全仓值迁移从 `MsObject*` 变为 `MsValue`（本文以装箱形态书写，实现时随当时代码库形态对齐）。
- **computed goto（任务 66）**：缓存逻辑全部在 `ms_vm_ops.h` 的指令处理函数内，与分派骨架正交，两套分派构建共享同一份缓存实现，无额外适配。
- 三者落地顺序为 65 → 66 → 67，本任务的基准对比以当时最新的落盘基线为分母，不重复扣除前两项收益。

### 9. 统计与诊断

`MsState` 内嵌诊断计数（假定字段名 `icStats`，含 `hits`/`misses`/`monoToPoly`/`polyToMega` 四个 uint64，递增成本可忽略，全构建启用）：环境变量 `MS_IC_STATS=1` 时进程退出前把汇总打印到 stderr，供基准调优与人工抽查「缓存确实接战」；不进 `--version`，不加 CLI 选项。脚本测试不断言计数（行为不经此暴露），门槛验收以基准数据为准。

### 10. 与任务 64 的对接（验收门槛）

对比口径沿用任务 65/66 的假定：Release 构建、同一机器同负载、`bench/run_benchmarks.py` 各跑不少于 3 轮取中位数，产出 `bench/results/v1.0-ic.json`（假定命名），分母为任务 65/66 落地后的最新结果文件。

- **提速门槛**：属性密集基准项（方法调用循环、实例字段读写循环类，如 `method_call`/`attr_access`/`oop_traverse`，名称为假定）墙钟时间下降 ≥ 25%。
- **全套件门槛**：几何平均提速 ≥ 5%。
- **回归豁免线**：无单项劣化 > 3%；超出者定位修复，或经评审记入本文的已知让步清单（超多态站点集中的人工基准是已知风险点，落入此清单需附命中率数据）。
- 门槛未达时按 11-project-layout §5 的原则记录数据与结论收尾，缓存默认保留（正确性已由回归保证，且不存在「关闭开关」——缓存是挂接式优化，无构建选项）。

## 实现步骤

1. 建 `src/vm/ms_ic.h` / `ms_ic.c` 骨架：状态/种类枚举、`struct MsIcEntry`/`struct MsIcSite`、`msIcSiteInit`/`msIcSiteDestroy`、`icShapeOf`。验证：编译通过，风格检查（guard、2 空格、星号贴类型、结构体不 typedef）过。
2. `struct MsProto` 增 `icSites`/`icCount` 与创建/销毁路径接挂（分配、逐项 init、随 Proto 释放）；`struct MsClass` 增 `methodVersion`，两个 Add 函数内递增。验证：全量既有测试在「编译器尚不分配站点（`A` 恒为 `MS_IC_SITE_NONE` 的临时约定）」下全绿——纯结构扩展、零行为变化。
3. 编译器接通站点分配：属性指令计数、`A` 写下标、超 255 回退哨兵；Proto 的 `icCount` 落位。验证：含属性访问的脚本执行结果不变（缓存逻辑未接，`A` 仅被忽略）；debug 断言站点数与指令数一致。
4. GET_ATTR 缓存路径：`msIcTryGetAttr` 三类条目的命中判定 + `msIcGetAttrMiss` 的填充。验证：单态/多态/超多态脚本（见测试方案）结果与无缓存一致；`MS_IC_STATS=1` 人工抽查命中率随循环迭代爬升。
5. SET_ATTR 与 LOAD_METHOD 缓存路径（含 `icSlotWrite` 收口、METHOD 命中的免分配绑定对）。验证：方法调用循环、字段写循环脚本全绿；实例属性遮蔽方法、静态方法两路径行为不变。
6. GC 对接：`msIcMarkProto` 挂入任务 17 的 Proto 标记遍历。验证：构造「方法仅被缓存引用」的压力脚本（类定义后丢弃其他引用、强制 GC），缓存命中仍返回存活函数；ASAN 构建无报告。
7. 失效语义用例落地：实例增删属性、rehash（大量属性触发扩容）、方法覆盖（`msClassAddMethod` 重名路径，经内建或 C 扩展间接触发，视任务 25 落地形态）、继承链版本传播。验证：测试方案中 `invalidation.ms` 全绿。
8. 全量回归 + 基准对比：`ctest` + `python run_tests.py` + `mslang test ./...` 全绿；按「与任务 64 的对接」跑基准并落盘对比结果；未达门槛项剖析迭代（重点：键核验分支布局、POLY 线性扫描顺序按最近命中前置）。验证：门槛全部满足或按原则记录收尾。

## 测试方案

本任务晚于任务 40，新增测试一律为 ms 脚本（`tests/ms/inline_cache/`）并使用 `testing` 模块，由仓库根 `run_tests.py` 驱动；缓存内部状态不经脚本断言（行为等价性 + 基准门槛 + `MS_IC_STATS` 人工抽查三层覆盖）。本任务只交付设计文档，脚本随实现编写。

测试文件清单与覆盖点：

- `monomorphic.ms`：单类单实例的字段读/写与方法调用循环（≥ 1000 次迭代，确保站点稳定于 MONO）；循环内结果逐次断言——缓存命中绝不能改变读出值。
- `polymorphic.ms`：同一访问站点服务 2 个、4 个不同类的实例（鸭子类型：同名方法/同名字段、不同类），断言各类结果正确（覆盖 MONO→POLY 迁移与 POLY 内线性扫描）；5 个不同类轮转访问（越过 `MS_IC_POLY_MAX`，站点落 MEGA），断言结果仍正确——超多态只是慢，不是错。
- `shape_diversity.ms`：同类不同属性布局——`__init__` 内按条件分支以不同顺序创建属性、部分实例缺某属性；断言每个实例读出自己的值（键核验拦截错误槽位命中）；一个实例先读后动态新增属性再读，新旧值各自正确。
- `invalidation.ms`：属性 `del` 后重读报错（负例，`.exit` 同伴文件）与删后重建读回新值；大量属性（触发 `MsNameTable` 扩容 rehash）写入后逐字段复读断言；实例属性遮蔽同名方法后再 `del` 遮蔽属性、方法恢复可达。
- `method_bind.ms`：`obj.m(args)` 循环（LOAD_METHOD 命中路径）；`f := obj.m; f()`（GET_ATTR 的 METHOD 命中仍需正确绑定）；字段持有函数遮蔽方法名（`recvSlot = nil` 路径）；静态方法经类名与实例两种调用。
- `inheritance.ms`（依赖任务 25 已落地）：继承链上的方法解析缓存命中；子类覆盖后同名调用命中新实现；`super` 调用路径不受缓存影响。
- `gc_stress.ms`：缓存持有唯一方法引用的场景（见实现步骤 6）+ 属性密集循环中强制多轮 GC，断言结果完整、无悬挂引用。

回归与基准：

- 既有 `tests/ms/` 全部脚本、`tests/c/` 全部 C 单元测试、`mslang test ./...` 全量回归一遍，结果与改动前基线逐项一致；Release 与 Debug（ASAN）两种构建各跑一轮。
- 基准对比按「与任务 64 的对接」执行：同机 ≥ 3 轮中位数，产出对比表（基线 / 本任务 / 比值）并落盘结果文件；三项门槛（属性密集 ≥ 25%、几何平均 ≥ 5%、无劣化 > 3%）逐项核对。
- `MS_IC_STATS=1` 下人工抽查一次代表性脚本：确认命中率随循环爬升、polymorphic.ms 的站点状态按预期迁移（MONO→POLY→MEGA 计数递增），抽查结论记入实现提交说明。

## 验收标准

- [ ] `src/vm/ms_ic.h` / `ms_ic.c` 存在，guard 为 `MSLANG_SRC_VM_MS_IC_H_`，头文件自包含，风格通过 10-c-style 检查（2 空格缩进、120 列、K&R、星号贴类型、内部结构体不 typedef、枚举值 `MS_IC_` 大写蛇形、堆分配只经 `msAlloc/msFree`）。
- [ ] 缓存站点旁挂于 `struct MsProto`（`icSites`/`icCount`），指令流保持定长 4 字节且运行期只读；`GET_ATTR`/`SET_ATTR`/`LOAD_METHOD` 的 `A` 操作数 = 站点下标、`0xFF` = 无站点哨兵的约定按本文定稿并与任务 05/07 文档对齐；单 Proto 站点超 255 时回退慢路径。
- [ ] 单态/多态（≤ 4 项）/超多态状态机与本文一致；条目判别键为 `tag + shape`（实例取 `klass`、类对象取自身、其余取 `header.type`）。
- [ ] 失效语义：实例属性条目以「槽位界内 + 键指针核验」判定，表增删/rehash/异布局全部正确未命中；方法条目以 `methodVersion` 判定，类方法表变更（含 MRO 祖先变更经版本传播）后旧条目失效；无负缓存。
- [ ] 语义等价：缓存命中路径与 `msObjGetAttr`/`msObjSetAttr`/`LOAD_METHOD` 慢路径产出逐项一致（含 METHOD 命中时 GET_ATTR 仍创建绑定方法、LOAD_METHOD 仍免分配）；`DEL_ATTR` 与 `CALL_METHOD` 行为不变。
- [ ] `msIcMarkProto` 挂入 GC 的 Proto 标记遍历，缓存持有的函数对象不被误回收；SET_ATTR 写值收口于单点助手，为任务 68 写屏障留口。
- [ ] 「测试方案」`tests/ms/inline_cache/` 全部清单项实现并通过；既有 C 与 ms 测试全量回归（Debug + Release）结果与改动前基线逐项一致；构建产物只落在 `build/`。
- [ ] 基准门槛达成（同机 ≥ 3 轮中位数）：属性密集基准项墙钟下降 ≥ 25%，全套件几何平均提速 ≥ 5%，无单项劣化 > 3%；对比结果落盘 `bench/results/`；未达项已定位修复或经评审记入让步清单。
- [ ] 无 TBD/TODO 占位；对任务 05/07/17/25/64 的接口假定（站点字段落位、编译器挂接点、GC 标记入口、`msClassNoteMethodChanged`、基准入口与结果文件命名）在实现时已按对应任务文档对齐。
