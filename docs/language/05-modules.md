# 05 模块系统

> Python 的 import 语义 + Go 的模块路径外观。模块是 `.ms` 文件或标准库模块，首次导入时执行并缓存。

## 1. 导入形式

```go
import "fmt"                          // 标准库模块
import "encoding/json"                // 带子路径；绑定名为末段 json
import "encoding/json" as encjson     // 别名
import "./utils/helper"               // 相对路径（以当前文件目录为基准）
import "github.com/user/lib"          // 预留：外部包路径（首版不实现下载）

from "strings" import toUpper, split
from "os" import (getEnv, listDir)    // 多行形式
```

绑定规则：

- `import "a/b/c"` → 在当前作用域绑定名字 `c`。
- `from "a/b" import x` → 直接绑定模块内名字 `x`。
- `*` 导入（`from "m" import *`）**不支持**（刻意删减，保持命名空间显式）。

## 2. 模块解析

解析顺序（首个命中者胜）：

1. **内建/已注册模块**：标准库 C 模块与 `lib/` 纯脚本模块（见 [07-stdlib.md](07-stdlib.md)）。
2. **相对路径**：以 `./` 或 `../` 开头，相对于**当前导入方文件**的目录。
3. **搜索路径**（按序）：
   - 环境变量 `MS_PATH` 中的目录列表（平台路径分隔符：Windows `;`，其余 `:`）
   - 解释器安装目录下的 `lib/`

路径 `a/b/c` 依次尝试：

- `<dir>/a/b/c.ms`（模块）
- `<dir>/a/b/c/__init__.ms`（包）

## 3. 包

目录含 `__init__.ms` 即为包。`import "a/b"` 加载 `a/b/__init__.ms`；`import "a/b/c"` 优先匹配子模块文件。包内相对导入：

```go
import "./sibling"          // 同包兄弟模块
import "../other"           // 父包
```

## 4. 执行语义

- 模块代码在**首次导入**时执行一次，模块对象（顶层环境）缓存于 `modules` 注册表，键为解析后的绝对路径（或内建模块名）。重复导入返回同一对象。
- 模块顶层语句顺序执行；模块级变量即模块属性，可用 `m.x` 访问。
- **循环导入**：A 导入 B、B 导入 A 时，B 拿到的是 A 的**部分初始化模块对象**（已注册、顶层未执行完）。B 若在模块顶层立即访问 A 尚未定义的名字，抛 `AttributeError`。文档建议：循环依赖中改用函数内延迟导入。

## 5. 主模块判定

每个模块有内建变量 `__name__`：

- 作为入口脚本直接运行时：`__name__ == "__main__"`
- 被导入时：`__name__` 为模块名（路径形式，如 `"encoding/json"`）

脚本顶层语句总是顺序执行，无需入口函数。`__name__` 惯用法用于**保护不希望被导入时执行的顶层代码**：

```go
// tool.ms：既可直接运行，也可被导入复用函数
import "lib"

func doWork() { ... }

if __name__ == "__main__" {
    doWork()        // 仅直接运行时执行；被导入时不执行
}
```

不写该判断时，顶层代码在导入与直接运行时都会执行——纯库文件通常不需要这个判断。

## 6. 动态导入与内省

```go
m := importlib.importModule("encoding/json")   // 动态导入（importlib 标准库）
dir(m)                                          // 模块内名字列表
```

## 7. 与 C 扩展的关系

C 扩展模块与脚本模块共用同一注册表与解析顺序——C 模块在内建注册表中的优先级高于搜索路径，这意味着标准库 C 模块不可被路径上的同名脚本遮蔽（安全考虑）。自定义 C 扩展的注册见 [09-c-api.md](09-c-api.md)。
