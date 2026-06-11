# blackboard 模块

黑板键值存储，线程安全。行为树节点、传感器和 Lua 脚本通过同一个黑板实例共享数据。

```lua
local bb = require('blackboard')
```

| 函数 | 说明 |
|------|------|
| `bb.set(key, value)` | 设置值 |
| `bb.get(key)` | 获取值，不存在返回 `nil` |
| `bb.has(key)` | 检查键是否存在，返回 `boolean` |
| `bb.remove(key)` | 删除键 |
| `bb.clear()` | 清空所有键值 |
| `bb.to_table()` | 返回整个黑板为 table |

**示例：**

```lua
local bb = require('blackboard')

bb.set("hp", 100)
bb.set("name", "hero")
bb.set("alive", true)

local hp = bb.get("hp")       -- 100
local missing = bb.get("x")   -- nil
local has_name = bb.has("name") -- true

bb.remove("alive")
bb.clear()
```
