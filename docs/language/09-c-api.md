# 09 C API 规范

> 对标 Python C API 的嵌入/扩展双向能力。命名遵循 Java 规范到 C 的映射（见 [10-c-style.md](10-c-style.md) 第 3 节）。

## 1. 命名约定速查

| 元素 | 约定 | 示例 |
|---|---|---|
| 公开类型 | `Ms` + 大驼峰 | `MsState` `MsObject` `MsModuleDef` |
| 公开函数 | `ms` + 小驼峰 | `msNewState()` `msCallObject()` |
| 常量/宏/枚举值 | `MS_` + 大写蛇形 | `MS_OK` `MS_TYPE_INT` `MS_VERSION_MAJOR` |
| 内部函数（不导出） | `ms` + 小驼峰 + `Impl` 后缀或 static | `msMarkRootsImpl()` |

## 2. 头文件布局

```
include/mslang/
├── mslang.h        // 伞头文件：包含以下全部
├── state.h         // 解释器生命周期、执行
├── object.h        // 对象模型、类型判断、转换
├── container.h     // list/dict/str 等容器操作
├── call.h          // 调用与属性访问
├── error.h         // 错误状态
├── gc.h            // 根管理与 GC 控制
├── module.h        // 扩展模块注册
├── ctype.h         // C 自定义类型
└── version.h       // 版本宏
```

所有公开 API 仅使用 C 基本类型与不透明指针；**不暴露结构体布局**（`MsState`/`MsObject` 均为不透明类型，仅前向声明），为内部演进留空间。

## 3. 对象模型与 GC 根

- 所有脚本值统一为 `MsObject*`（句柄）。
- GC 为标记-清除，**无引用计数**。C 代码持有的对象在 GC 扫描时必须可达——通过**显式根栈**：

```c
msRootPush(L, obj);        // obj 入根栈，GC 不会回收
// ... 使用 obj ...
msRootPop(L);              // 弹出（LIFO 顺序）
```

- 规则：任何可能触发分配（任何 `msNew*` / 脚本调用）的两个 API 调用之间存活的局部 `MsObject*` 都必须入根。
- 函数的**参数**自动是根（调用约定保证）；返回值在调用者的下一次分配前有效（转移所有权语义：要么入根，要么立即消费）。
- `msGCDisable(L)` / `msGCEnable(L)` / `msGCCollect(L)` 提供手动控制。

## 4. 状态与执行（state.h）

```c
#define MS_VERSION_MAJOR 0
#define MS_VERSION_MINOR 1
#define MS_VERSION_PATCH 0

MsState *msNewState(void);
MsState *msNewStateWithConfig(const MsConfig *config);  // 线程数、GC 阈值等
void     msCloseState(MsState *L);

MsResult msEvalString(MsState *L, const char *source);
MsResult msEvalFile(MsState *L, const char *path);
MsResult msEvalStringAs(MsState *L, const char *source, const char *chunkName);

MsObject *msGetGlobal(MsState *L, const char *name);
void      msSetGlobal(MsState *L, const char *name, MsObject *value);
```

`MsResult` 为枚举：`MS_OK` / `MS_ERROR_RUNTIME` / `MS_ERROR_SYNTAX` / `MS_ERROR_OOM`。

## 5. 值构造与转换（object.h）

```c
// 构造
MsObject *msNewNil(MsState *L);
MsObject *msNewBool(MsState *L, bool v);
MsObject *msNewInt(MsState *L, int64_t v);
MsObject *msNewIntFromString(MsState *L, const char *text, int base);
MsObject *msNewFloat(MsState *L, double v);
MsObject *msNewString(MsState *L, const char *utf8);
MsObject *msNewStringN(MsState *L, const char *data, size_t len);
MsObject *msNewBytes(MsState *L, const uint8_t *data, size_t len);
MsObject *msNewList(MsState *L, int capacity);
MsObject *msNewDict(MsState *L);
MsObject *msNewTuple(MsState *L, int len);

// 类型判断与转换
MsTypeTag msTypeOf(MsObject *obj);
bool      msIsNil(MsObject *obj);
bool      msIsInstance(MsState *L, MsObject *obj, MsObject *classObj);
bool      msAsBool(MsState *L, MsObject *obj);
int64_t   msAsInt(MsState *L, MsObject *obj);          // 不可转抛 TypeError
double    msAsFloat(MsState *L, MsObject *obj);
const char *msAsCString(MsState *L, MsObject *obj);    // 内部缓冲区，下次分配前有效
size_t    msStringLen(MsObject *strObj);
```

## 6. 容器操作（container.h）

```c
int64_t   msLen(MsState *L, MsObject *container);

MsObject *msListGet(MsState *L, MsObject *list, int64_t index);
void      msListSet(MsState *L, MsObject *list, int64_t index, MsObject *v);
void      msListAppend(MsState *L, MsObject *list, MsObject *v);

MsObject *msDictGet(MsState *L, MsObject *dict, MsObject *key);   // 无键返回 nil
void      msDictSet(MsState *L, MsObject *dict, MsObject *key, MsObject *v);
bool      msDictContains(MsState *L, MsObject *dict, MsObject *key);
void      msDictDelete(MsState *L, MsObject *dict, MsObject *key);

MsObject *msStrConcat(MsState *L, MsObject *a, MsObject *b);
```

## 7. 调用与属性（call.h）

```c
MsObject *msCallObject(MsState *L, MsObject *callable, int argc, MsObject **argv);
MsObject *msCallMethod(MsState *L, MsObject *obj, const char *method,
                       int argc, MsObject **argv);

MsObject *msGetAttr(MsState *L, MsObject *obj, const char *name);
void      msSetAttr(MsState *L, MsObject *obj, const char *name, MsObject *v);
bool      msHasAttr(MsState *L, MsObject *obj, const char *name);
```

## 8. 错误处理（error.h）

```c
bool        msErrorOccurred(MsState *L);
MsObject   *msErrorGet(MsState *L);            // 取走并清除当前异常对象
void        msErrorClear(MsState *L);
void        msRaise(MsState *L, MsObject *excObj);
void        msRaiseTypeError(MsState *L, const char *fmt, ...);
void        msRaiseValueError(MsState *L, const char *fmt, ...);
void        msRaiseRuntimeError(MsState *L, const char *fmt, ...);
void        msRaiseOSError(MsState *L, int sysErrno, const char *fmt, ...);
```

约定：所有返回 `MsObject*` 的 API 失败时返回 `NULL` 且设置错误状态；返回 `MsResult` 的 API 用枚举区分。C 扩展函数出错时设置错误并返回 `NULL`，VM 将其转为脚本异常。

## 9. 扩展模块（module.h）

```c
typedef MsObject *(*MsCFunction)(MsState *L, int argc, MsObject **argv);

typedef struct {
  const char  *name;             // "factorial"
  MsCFunction  func;
  const char  *doc;              // 文档字符串，可为 NULL
} MsMethodDef;

typedef struct {
  const char         *name;      // 模块名："fastmath"
  const char         *doc;
  const MsMethodDef  *methods;   // 以 {NULL, NULL, NULL} 结尾的数组
} MsModuleDef;

// 嵌入方静态注册：
int msRegisterModule(MsState *L, const MsModuleDef *def);

// 动态加载约定：共享库导出名为 mslangInit_<name> 的函数
MsModuleDef *mslangInit_fastmath(void);
```

C 函数实现示例：

```c
static MsObject *fastmathFactorial(MsState *L, int argc, MsObject **argv) {
  if (argc != 1 || msTypeOf(argv[0]) != MS_TYPE_INT) {
    msRaiseTypeError(L, "factorial() requires exactly one int");
    return NULL;
  }
  int64_t n = msAsInt(L, argv[0]);
  // ...
  return msNewInt(L, result);
}

static const MsMethodDef fastmathMethods[] = {
  {"factorial", fastmathFactorial, "factorial(n) -> int"},
  {NULL, NULL, NULL},
};

static const MsModuleDef fastmathModule = {
  "fastmath", "fast math routines", fastmathMethods,
};

MsModuleDef *mslangInit_fastmath(void) { return (MsModuleDef *)&fastmathModule; }
```

脚本侧直接 `import "fastmath"`。

## 10. C 自定义类型（ctype.h）

C 扩展可定义脚本可见的新类型（如高性能矩阵、数据库连接）：

```c
typedef struct {
  const char *name;                       // "Matrix"
  size_t      instanceSize;               // 实例附加数据大小
  MsCFunction init;                       // 构造，可为 NULL
  void      (*finalize)(MsObject *obj);   // GC 回收前调用，可为 NULL
  MsObject  (*toString)(MsState *L, MsObject *obj);
  const MsMethodDef *methods;
} MsTypeDef;

MsObject *msDefineType(MsState *L, const MsTypeDef *def);      // 返回类型对象
void     *msCInstanceData(MsObject *instance);                 // 取附加数据区
```

`finalize` 仅用于释放 C 侧资源（关闭 fd、free 外部内存），不得在其中访问其他脚本对象（回收顺序未定义）。

## 11. 嵌入完整示例

```c
#include <mslang/mslang.h>
#include <stdio.h>

int main(void) {
  MsState *L = msNewState();
  if (L == NULL) { return 1; }

  if (msEvalFile(L, "scripts/main.ms") != MS_OK) {
    MsObject *err = msErrorGet(L);
    fprintf(stderr, "mslang error: %s\n", msAsCString(L, err));
    msCloseState(L);
    return 1;
  }

  // 调用脚本函数 fib(30)
  MsObject *fib = msGetGlobal(L, "fib");
  MsObject *arg = msNewInt(L, 30);
  msRootPush(L, arg);
  MsObject *result = msCallObject(L, fib, 1, &arg);
  msRootPop(L);

  if (result == NULL) {
    fprintf(stderr, "call failed\n");
  } else {
    printf("fib(30) = %lld\n", (long long)msAsInt(L, result));
  }

  msCloseState(L);
  return 0;
}
```

## 12. 线程规则

1. 一个 `MsState` 同一时刻只能被一个 OS 线程操作；跨线程移交所有权前必须无并发访问（调用方用互斥锁保证）。
2. VM 内部调度线程对嵌入方透明，不计入上述约束。
3. 多解释器实例（多个 `MsState`）完全隔离，可安全地分属不同线程。

## 13. 版本与兼容

- `msVersionString()` / `MS_VERSION_*` 宏供编译期与运行期检查。
- 首版**不承诺 ABI 稳定**：次版本号变化可能重排/新增 API，主版本号变化允许破坏性修改。扩展模块按头文件版本重新编译即可。
