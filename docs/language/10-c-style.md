# 10 C 编码规范

> 以 [Google C++ 风格指南](https://google.github.io/styleguide/cppguide.html)为基础，裁剪其中不适用 C 的部分；命名约定遵循 [Google Java 风格指南](https://google.github.io/styleguide/javaguide.html) 并映射到 C。冲突时以本文档为准。

## 1. 源文件组织

- 文件扩展名：`.c` / `.h`；文件名小写蛇形：`ms_lexer.c`、`ms_object.h`。
- 每文件一个明确的职责；单文件超过 ~1500 行是拆分信号。
- 头文件使用 include guard（不用 `#pragma once`，对齐 Google 风格）：

```c
#ifndef MSLANG_SRC_MS_LEXER_H_
#define MSLANG_SRC_MS_LEXER_H_
/* ... */
#endif  /* MSLANG_SRC_MS_LEXER_H_ */
```

- guard 宏 = 项目名 + 相对路径大写蛇形。
- include 顺序（组间空行）：对应头文件 → C 系统头 → 项目内头文件。组内按字母序。
- 头文件中只做声明；公开头文件（`include/mslang/`）仅前向声明不透明类型，结构体定义放 `src/` 内部头文件。

## 2. 格式化

对齐 Google 风格（C 子集）：

- **缩进 2 空格**，禁止 Tab。
- **行宽 80 列**；超长表达式按语义换行，续行缩进 4 空格。
- 大括号挂行尾（K&R 附着式），`else` 与前 `}` 同行：

```c
if (condition) {
  doSomething();
} else {
  doOther();
}

while (msLexerPeek(lexer) != '\0') {
  msLexerAdvance(lexer);
}
```

- 单语句块也必须带大括号。
- 指针星号靠左：`MsObject *obj`（Google 允许两种，本项目固定靠左）。
- `switch` 每个 `case` 要么以 `break`/`return` 结尾，要么标注 `/* fallthrough */`。
- 二元运算符换行时运算符在行首（Java 风格）：

```c
bool ok = msTypeOf(obj) == MS_TYPE_INT
    && msAsInt(state, obj) > 0;
```

## 3. 命名（Java 规范 → C 映射）

| Java 概念 | C 映射 | 示例 |
|---|---|---|
| package | 库前缀 | `ms` / `MS_` |
| class（类型） | `Ms` + UpperCamelCase | `MsState` `MsObject` `MsLexer` |
| method / function | `ms` + lowerCamelCase | `msNewState()` `msLexerAdvance()` |
| 局部变量、参数 | lowerCamelCase | `sourceLen` `chunkName` |
| 结构体字段 | lowerCamelCase | `proto->codeLen` |
| 常量（static final） | `MS_` + UPPER_SNAKE | `MS_MAX_STACK_DEPTH` |
| 枚举类型 | `Ms` + UpperCamelCase | `MsResult` |
| 枚举值 | `MS_` + UPPER_SNAKE | `MS_ERROR_SYNTAX` |
| 宏 | `MS_` + UPPER_SNAKE | `MS_ARRAY_LEN(x)` |
| 文件内 static 函数 | 同函数规则 | `static bool matchKeyword(...)` |
| 测试函数 | `test` + UpperCamelCase 主题 | `testLexerSkipsComments` |

模块内聚命名：同一模块的函数共享语义前缀，如 lexer 模块 `msLexerInit/msLexerNext/msLexerPeek`。

## 4. C 语言特性使用规则

- 标准：**C11**（`threads.h` 不可移植，线程/原子操作走 `src/platform/` 抽象层，见 [11-project-layout.md](11-project-layout.md)）。
- 允许：定长数组、`restrict`、`_Static_assert`、匿名 union/struct（内部）。
- 禁止：VLA（变长数组）、`alloca`、递归宏、K&R 函数定义、`gets` 类危险函数。
- 整数：对外接口用定宽类型（`int64_t`、`size_t`）；循环下标可用 `int`。
- 不定义 `bool` 之外的单字母缩写类型别名；`typedef` 仅用于不透明类型与枚举，结构体一律 `struct MsLexer`（Google 风格：类型名即结构体名，不加 `_t` 后缀）。

## 5. 错误处理与资源管理

- 无 setjmp 异常：内部函数用 `MsResult` 或 `NULL` + 状态错误位报告失败。
- 资源获取即初始化（C 版）：函数内申请的资源在函数内释放，单一出口模式：

```c
MsResult msCompileFile(MsState *L, const char *path, MsProto **out) {
  MsLexer lexer;
  MsResult result = msLexerInit(&lexer, path);
  if (result != MS_OK) {
    return result;
  }
  result = msParserRun(&lexer, out);
  msLexerDestroy(&lexer);
  return result;
}
```

- 禁止 `errno` 跨层传播：底层错误立即转换为 `MsResult`/脚本异常并附消息。

## 6. 内存与 GC 纪律

- 所有堆分配经 `msAlloc/msRealloc/msFree`（带状态统计，OOM 走统一失败路径），禁止直接 `malloc`。
- 每对分配/释放在代码评审时必须有明确的所有者。
- C 局部持有的 `MsObject*` 遵守根栈纪律（见 [09-c-api.md](09-c-api.md) 第 3 节）；评审清单固定包含此项。

## 7. 注释与文档

- 公开 API（`include/mslang/`）每个函数必须有文档注释：功能、参数语义、返回值、错误行为、根纪律要求。
- 注释用英文（代码国际化惯例），设计文档用中文。
- 注释说明"为什么"，不复述代码；非显然的算法附参考（如标记-清除的伪代码出处）。

```c
/*
 * Pushes obj onto the GC root stack so it survives allocation-triggered
 * collections. Must be paired with msRootPop in LIFO order.
 */
void msRootPush(MsState *L, MsObject *obj);
```

## 8. 断言与防御

- 内部不变量用 `MS_ASSERT`（debug 构建启用，release 编译为空）。
- 公开 API 对参数做完整校验并返回错误，不依赖断言。
- `MS_UNREACHABLE()` 标注不可达分支，release 下展开为优化提示。

## 9. 平台抽象

- 平台相关代码只允许出现在 `src/platform/`：线程（pthread/Win32）、原子、动态库加载、时钟、socket。
- 其余代码只包含 `ms_platform.h`，不出现 `#ifdef _WIN32`。
