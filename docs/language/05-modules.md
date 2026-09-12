# 05 模块系统

> Python 的 import 语义 + Go 的模块路径外观。模块是 `.ms` 文件或标准库模块，首次导入时执行并缓存。

## 1. 导入形式

```ms
import "fmt"                          // standard library module
import "encoding/json"                // subpath; binds the last segment, json
import "encoding/json" as encjson     // alias
import "./utils/helper"               // relative path (resolved against the importing file's directory)
import "github.com/user/lib"          // reserved: external package path (no downloading in v1)

from "strings" import toUpper, split
from "os" import (getEnv, listDir)    // parenthesized multi-name form
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

```ms
import "./sibling"          // sibling module in the same package
import "../other"           // parent package
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

```ms
// tool.ms: runnable directly, or imported to reuse its functions
import "lib"

func doWork() { ... }

if __name__ == "__main__" {
    doWork()        // runs only when executed directly, not when imported
}
```

不写该判断时，顶层代码在导入与直接运行时都会执行——纯库文件通常不需要这个判断。

## 6. 动态导入与内省

```ms
m := importlib.importModule("encoding/json")   // dynamic import (the importlib stdlib module)
dir(m)                                         // lists names in the module
```

## 7. 与 C 扩展的关系

C 扩展模块与脚本模块共用同一注册表与解析顺序——C 模块在内建注册表中的优先级高于搜索路径，这意味着标准库 C 模块不可被路径上的同名脚本遮蔽（安全考虑）。自定义 C 扩展的注册见 [09-c-api.md](09-c-api.md)。
