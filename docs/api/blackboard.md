# blackboard 模块

黑板键值存储，线程安全。行为树节点与 Lua 脚本通过同一个黑板实例共享数据。

```lua
local bb = require('blackboard')
```

| 函数 | 说明 |
|------|------|
| `bb.set(key, value)` | 设置值（会替换该 key 上已安装的 value 提供器） |
| `bb.set_provider(key, fn)` | 为 key 安装 **value 提供器**：此后每次读取该 key 都调用 `fn()`，用其返回值作为 value（见下） |
| `bb.get(key)` | 获取值，不存在返回 `nil`；有提供器时返回 `fn()` 的实时结果 |
| `bb.has(key)` | 检查键是否存在（静态值或提供器均算存在），返回 `boolean` |
| `bb.remove(key)` | 删除键（同时移除提供器） |
| `bb.clear()` | 清空所有键值（含提供器） |
| `bb.to_table()` | 返回整个黑板为 table（提供器键会调用一次取快照值） |

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

## value 提供器（computed value）

`bb.set_provider(key, fn)` 把 key 变成**计算值**：之后**每一次**读取——无论是 Lua 侧的
`bb.get(key)`，还是引擎侧对该 key 的消费（Script/条件 params 里的 `$key` 在 Enter 时解析、
`Blackboard` 条件比较、`bb.to_table()`）——都会**实时调用 `fn()` 一次**，把返回值作为 value。
适合"每次要最新值"的来源：计数器、由其它键推导的值、随时间/环境变化的量。

```lua
local bb = require('blackboard')

local n = 0
bb.set_provider("attempt", function() n = n + 1; return n end)

bb.get("attempt")   -- 1（每次调用 fn）
bb.get("attempt")   -- 2

bb.set("base", 10)
bb.set_provider("double_base", function()
  return bb.get("base") * 2   -- 提供器内部可以再读黑板（读自身 key 有递归保护）
end)
bb.get("double_base")  -- 20；改 base 后再读即最新值
```

要点：

- **互斥、后写胜出**：一个 key 要么是静态值、要么是提供器。`bb.set` 覆盖提供器，
  `bb.set_provider` 覆盖静态值；`bb.remove` / `bb.clear` 一并移除。
- **同步要求**：`fn` 必须同步返回（不能 `yield`/`sleep`/`await`）；运行时报错会记录日志并
  按返回 `nil` 处理，不会中断调用方。
- **返回 nil** 是合法值（该 key 存在、值为 nil）；`bb.has` 对提供器 key 恒为 `true`。
- **递归保护**：提供器链（含读自身 key）深度上限 8，超限记错误并按缺失处理。
- 引擎侧消费即"每次求值都拿最新值"：`$key` 在**每个节点 Enter 时**各自调用一次提供器；
  `Blackboard` 条件每次轮询调用一次。

```lua
-- 与行为树配合：params 用 $attempt，两个节点各自 Enter 时取到 1 和 2
bb.set_provider("attempt", function() n = n + 1; return n end)
bt.init({root = "res://tree.json"})   -- tree 内节点 params: {"target": "$attempt"}
```
