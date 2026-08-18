# blackboard 模块

黑板键值存储，线程安全。行为树节点与 Lua 脚本通过同一个黑板实例共享数据。

```lua
local bb = require('blackboard')
```

| 函数 | 说明 |
|------|------|
| `bb.set(key, value)` | 设置值（会替换该 key 上已安装的 value 提供器） |
| `bb.set_provider(key, fn)` | 为 key 安装 **value 提供器**：此后每次读取该 key 都调用 `fn()`，用其返回值作为 value；点号路径读取时后续段作为**字符串参数**传入 `fn`（见下） |
| `bb.get(key)` | 获取值，不存在返回 `nil`；有提供器时返回 `fn()` 的实时结果；key 含 `.` 时支持下级索引（见[点号路径取表内字段](#点号路径取表内字段)） |
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

**点号路径读取时，后续段作为参数传给 `fn`**：`bb.get("cfg.net.ip")`（`cfg` 装了提供器）
调用的是 `fn("net", "ip")`，返回值**直接作为结果**，引擎不再下钻。`fn` 自己决定怎么
处理路径（返回 nil 即该子路径不存在）。

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

-- 点号路径：后续段作为参数进入提供器（varargs）
bb.set_provider("env", function(...)
  local cur = bb.get("store")   -- 任意数据源
  for _, seg in ipairs({...}) do
    if type(cur) ~= "table" then return nil end
    cur = cur[seg]
  end
  return cur
end)
bb.get("env")        -- fn()           -> store 整体
bb.get("env.net.ip") -- fn("net","ip") -> 按路径自取
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

## 点号路径取表内字段

**读取**一个 key 含 `.` 且没有对应字面键时，`.` 表示 **table 的下级索引**：
`bb.get("proxy.ip")` 取 `blackboard["proxy"]`（一个 table）的 `ip` 字段，任意深度均可
（`"cfg.net.proxy.proto"` 逐层下钻）。所有走 `Blackboard::Get` 的消费端统一生效：

- Lua 侧 `bb.get("proxy.ip")`
- Script/条件 `params` 的 `"$proxy.ip"`（Enter 时解析）
- `Blackboard` 条件的 `params.key` / `params.key2`
- Pipeline `*timeout` / `*retry` 的 `"$key"` 边参数
- `Set` 节点的 `"$src"` 引用形式

```lua
local bb = require('blackboard')

bb.set("proxy", {ip = "10.0.0.1", port = 8100, net = {mtu = 1500}})
bb.get("proxy.ip")        -- "10.0.0.1"
bb.get("proxy.net.mtu")   -- 1500（多级下钻）
bb.get("proxy.host")      -- nil（字段不存在）
bb.get("ghost.ip")        -- nil（根键不存在）
```

规则：

- **字面键优先**：先查整个 key 的字面条目；存在（静态值或提供器）直接返回，不再下钻。
  `bb.set("proxy.ip", "x")` 会**遮蔽** `proxy` 表里的 `ip` 字段；`bb.remove("proxy.ip")`
  后恢复下钻。
- **根段是提供器时路径传参**：`bb.set_provider("cfg", fn)` 后，`bb.get("cfg.host")`
  调用 `fn("host")`、`bb.get("cfg.net.ip")` 调用 `fn("net","ip")`，返回值**直接作为
  结果**（引擎不再自动下钻）；需要下钻语义就在 `fn` 里自己遍历（见
  [value 提供器](#value-提供器-computed-value)示例）。
- **下钻只认数据表**：逐级 `rawget`，不触发 `__index` 元方法；中途遇到非 table
  （标量/nil）、字段不存在、空段（`"a..b"`、尾点）一律按**键不存在**处理（Lua 侧即 `nil`）。
- **只作用于读**：`bb.set` / `bb.has` / `bb.remove` 仍按完整字符串字面键操作，
  不会按 `.` 拆分（`bb.has("proxy.ip")` 只看字面键）。
- 命中中间层 table 时返回的是**活 table 引用**，可在 Lua 侧继续索引。
