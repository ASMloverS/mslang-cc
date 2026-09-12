# 01 词法结构

> mslang 词法规范。整体采用 Go 的词法外观：花括号、行注释、分号自动插入。

## 1. 源文件

- 后缀：`.ms`
- 编码：UTF-8（无 BOM；带 BOM 的文件视为非法）
- 换行：LF 或 CRLF 均可，词法分析统一为 LF

## 2. 注释

```ms
// single-line comment

/*
   Block comments
   do not nest.
*/
```

## 3. 标识符

```
identifier = letter { letter | digit | "_" }
letter     = "a".."z" | "A".."Z" | "_" | unicodeLetter
```

标识符区分大小写。命名约定（变量/函数用小驼峰、类型用大驼峰等）见 [12-ms-style.md](12-ms-style.md) §4。

## 4. 关键字

```
and      as       async    await    break    case     class    continue
default  del      else     except   false    finally  for      from
func     global   if       import   in       is       lambda   nil
not      or       pass     raise    return   select   self     super
true     try      while    with
```

说明：

- `chan` 不是关键字，是内建构造函数 `chan(capacity)`。
- `self`/`super` 是关键字，仅在 class 方法体内可用。
- 以下名字为内建函数而非关键字，可被遮蔽（不建议）：`len cap str int float bool bytes list tuple dict set range enumerate zip map filter sorted reversed type isinstance issubclass hasattr getattr setattr delattr repr chr ord hex oct bin abs min max sum round divmod pow open input print iter next id hash vars globals locals dir callable chan`。

## 5. 字面量

### 5.1 整数（任意精度）

```ms
42
1_000_000        // underscores are visual separators only
0x1F             // hex
0o755            // octal
0b1010           // binary
```

### 5.2 浮点（float64）

```ms
3.14
1e-9
2.5e+4
.5               // legal
5.               // illegal: a digit must follow the dot (avoids ambiguity with attribute access)
```

### 5.3 字符串（不可变，UTF-8）

```ms
"hello\n"            // regular string with escapes
`raw \n string`      // raw string in backticks; escapes are not processed (Go style)
f"x = {x + 1}"       // f-string: braces hold any expression
```

转义序列：`\n \t \r \\ \" \' \0 \x41 \u4e2d \U0001F600`。

f-string 内 `{expr}` 可带格式说明：`f"{pi:.2f}"`、`f"{n:08d}"`，格式微语言对齐 Python。

### 5.4 字节串

```ms
b"\x00\x01"          // bytes type, immutable
```

### 5.5 布尔与空

```ms
true  false  nil
```

## 6. 运算符与定界符

```
算术:   +  -  *  /  //  %  **
比较:   ==  !=  <  <=  >  >=
位运算: &  |  ^  ~  <<  >>
赋值:   :=  =  +=  -=  *=  /=  //=  %=  **=  &=  |=  ^=  <<=  >>=
其他:   (  )  [  ]  {  }  ,  :  .  ;  ...
```

关键字形式的逻辑运算：`and or not`；身份比较 `is` / `is not`；成员测试 `in` / `not in`。

## 7. 分号自动插入

与 Go 相同的规则：词法分析器在行尾遇到以下 token 之一时自动插入分号——

- 标识符、字面量（含 `true false nil`）
- `return break continue pass raise`
- `)  ]  }`
- 自增自减（仅 `for` 子句中的 `i++` / `i--` 语句形式）

推论：

- 续行：行尾是运算符、`,`、`(`、`[`、`{`、`.` 时不插入分号，自然续行。
- 分号总是可以手写，同一行多条语句用 `;` 分隔。
- 空语句不合法（`pass` 代替）。

## 8. 程序结构

一个 `.ms` 文件 = 顶层语句序列。**顶层语句即程序**：直接执行脚本时从第一条顶层语句开始顺序执行，无需也无"入口函数"概念——`main` 只是普通标识符，解释器不特殊对待。

模块被导入时同样顺序执行顶层语句（即模块初始化代码）。区分"直接运行 / 被导入"用内建变量 `__name__`，见 [05-modules.md](05-modules.md) §5。
