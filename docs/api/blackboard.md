# blackboard 模块

黑板键值存储，线程安全。行为树节点与 Lua 脚本通过同一个黑板实例共享数据。

```lua
local bb = require('blackboard')
```

| 函数 | 说明 |
|------|------|
| `bb.set(key, value)` | 设置值：平键直接存储（替换该 key 上的提供器）；点号键按[点号路径写入](#点号路径取表内字段)路由（提供器 `set` / 表内 `rawset`）。目标是异步提供器时**自动挂起**，直到 `set` 完成 |
| `bb.set_provider(key, {get = fn, set = fn2})` | 为 key 安装**提供器**：读走 `get(...)`（每次实时调用），点号写走 `set(value, ...)`；至少提供其一；**两者都可作为协程挂起**（见下） |
| `bb.get(key)` | 获取值，不存在返回 `nil`；有提供器时返回 `get(...)` 的实时结果——异步提供器会**自动挂起**；key 含 `.` 时支持下级索引（见[点号路径取表内字段](#点号路径取表内字段)） |
| `bb.has(key)` | 检查键是否存在（静态值或提供器均算存在），返回 `boolean` |
| `bb.remove(key)` | 删除键（同时移除提供器） |
| `bb.clear()` | 清空所有键值（含提供器） |
| `bb.to_table()` | 返回整个黑板为 table（提供器键做**同步**调用取快照；会挂起的提供器按 nil 快照并记错误） |

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

## value 提供器（可异步）

`bb.set_provider(key, {get = fn, set = fn2})` 把 key 交给一个**提供器**（引擎持有该
table 的引用）。第二个参数必须是 table，`get` / `set` 至少提供其一，字段值必须是函数：

- **`get(...)`** —— 该 key 的读值：之后**每一次**读取——Lua 侧 `bb.get(key)`，引擎侧
  （Script/条件 params 的 `$key` 在 Enter 时解析、`Blackboard` 条件比较、Pipeline 边参数）
  ——都会**实时调用一次**。点号读取时后续段作为**字符串参数**传入：`bb.get("cfg.net.ip")`
  调用 `get("net","ip")`，返回值**直接作为结果**（引擎不再下钻），返回 nil 即该子路径
  不存在。未提供 `get` 时该 key 读值为 nil。
- **`set(value, ...)`** —— 该 key 的**点号写入**：`bb.set("cfg.net.ip", v)` 调用
  `set(v, "net", "ip")`（先值后路径段）。未提供 `set` 时点号写告警并放弃。

**`get` / `set` 都以协程运行，可以挂起**（`sleep`/`await`/`http.request`，甚至在
provider 里再 `bb.get` 另一个提供器 key）。挂起期间：

- Lua 侧的 `bb.get` / `bb.set` **自动挂起**——对脚本就像一次同步调用，提供器完成后
  以其结果返回（同步完成的提供器不挂起，直接返回）。
- 引擎侧消费方（节点 Enter 参数解析、`Blackboard` 条件、`Set` 节点、Pipeline 边参数）
  维持 Running/挂起直到拿到值——见 [bt 文档](../bt.md)。
- `bb.to_table()` 例外：只做**同步尝试**，挂起的提供器被取消、按 nil 快照（记错误日志）。

```lua
local bb = require('blackboard')

-- 计算值：只读 get
local n = 0
bb.set_provider("attempt", {get = function() n = n + 1; return n end})
bb.get("attempt")   -- 1（每次调用 get）
bb.get("attempt")   -- 2

-- 异步数据源：get/set 都可以 sleep / await
bb.set_provider("dev", {
  get = function(seg)
    local resp = http.request({url = "http://device.local/" .. seg})  -- 可挂起
    return resp.body
  end,
  set = function(v, seg)
    http.request({url = "http://device.local/" .. seg, method = "PUT", body = v})
  end,
})
bb.get("dev.status")   -- 脚本在此挂起，直到 HTTP 返回

-- 外部数据源（纯 Lua 存储）：get/set 都接路径，读写自己的 store
local store = {net = {ip = "10.0.0.1"}}
local function dive(path)
  local cur = store
  for _, seg in ipairs(path) do
    if type(cur) ~= "table" then return nil end
    cur = cur[seg]
  end
  return cur
end
bb.set_provider("cfg", {
  get = function(...) return dive({...}) end,
  set = function(v, ...)
    local path = {...}
    local last = table.remove(path)
    local cur = dive(path)
    if type(cur) ~= "table" then return end
    cur[last] = v
  end,
})
bb.get("cfg.net.ip")   -- "10.0.0.1"
bb.set("cfg.net.ip", "10.0.0.9")
bb.get("cfg.net.ip")   -- "10.0.0.9"（写进了 store）
```

要点：

- **互斥、后写胜出**：一个 key 要么是静态值、要么是提供器。平键 `bb.set(key, v)` 覆盖
  提供器，`bb.set_provider` 覆盖静态值；`bb.remove` / `bb.clear` 一并移除（释放对
  table 的引用）。
- **运行时报错**记日志：`get` 按返回 nil 处理、`set` 的写被放弃，不会中断调用方。
- **返回 nil** 是合法值（该 key 存在、值为 nil）；`bb.has` 对提供器 key 恒为 `true`。
- **递归保护**：提供器链（含 get/set 内部再读写黑板）同步段深度上限 8，超限记错误并
  按缺失处理（跨挂起不累计——每次恢复都是新栈）。
- 引擎侧消费即"每次求值都拿最新值"：`$key` 在**每个节点 Enter 时**各自调用一次提供器；
  `Blackboard` 条件每次轮询调用一次。

```lua
-- 与行为树配合：params 用 $attempt，两个节点各自 Enter 时取到 1 和 2
bb.set_provider("attempt", {get = function() n = n + 1; return n end})
bt.init({root = "res://tree.json"})   -- tree 内节点 params: {"target": "$attempt"}
```

## 点号路径取表内字段

**读写**一个 key 含 `.` 且没有对应字面键时，`.` 表示 **table 的下级索引**：
`bb.get("proxy.ip")` 取 `blackboard["proxy"]`（一个 table）的 `ip` 字段，任意深度均可
（`"cfg.net.proxy.proto"` 逐层下钻）；**写对称**——`bb.set("proxy.port", 9000)` 写进
同一张表（`rawset`，Lua 侧持有的同一 table 对象立即可见）。所有走
`Blackboard::Get/Set` 的消费端统一生效：

- Lua 侧 `bb.get("proxy.ip")` / `bb.set("proxy.port", 9000)`
- Script/条件 `params` 的 `"$proxy.ip"`（Enter 时解析）
- `Blackboard` 条件的 `params.key` / `params.key2`
- Pipeline `*timeout` / `*retry` 的 `"$key"` 边参数
- `Set` 节点的 `"$src"` 引用形式；`Set` 节点的 `params.key` 本身也可带 `.`（写入路由）

```lua
local bb = require('blackboard')

bb.set("proxy", {ip = "10.0.0.1", port = 8100, net = {mtu = 1500}})
bb.get("proxy.ip")        -- "10.0.0.1"
bb.get("proxy.net.mtu")   -- 1500（多级下钻）
bb.get("proxy.host")      -- nil（字段不存在）
bb.get("ghost.ip")        -- nil（根键不存在）

bb.set("proxy.port", 9000)     -- 写进 proxy 表
bb.set("proxy.net.mtu", 1400)  -- 多级写入
```

**写入路由**（`bb.set("a.b", v)`）：

- **字面键优先**：整个 key 已有字面条目（静态值或提供器）→ 直接覆盖字面条目，
  不路由（与读取的遮蔽规则一致）。
- **根段是提供器** → 调用提供器的 `set(v, "b")`（见
  [value 提供器](#value-提供器可异步)）；未提供 `set` 时告警并放弃。
- **根段是静态 table** → 逐级 `rawset` 写入该表（Lua 侧可见，不触发 `__newindex`）。
  **不自动建表**：中途字段缺失、中途非 table、空段（`"a..b"`、尾点）→ 告警并放弃本次写。
- **根段缺失或非 table** → 按完整字符串字面键存储（旧行为）。

**读取规则**：

- **字面键优先**：先查整个 key 的字面条目；存在（静态值或提供器）直接返回，不再下钻。
  `bb.remove("proxy.ip")` 移除字面遮蔽后恢复下钻。
- **根段是提供器时路径传参**：`bb.get("cfg.host")` 调用 `get("host")`，返回值直接作为
  结果（引擎不再自动下钻）；需要下钻语义就在 `get` 里自己遍历（见
  [value 提供器](#value-提供器可异步)示例）。
- **下钻只认数据表**：逐级 `rawget`，不触发 `__index` 元方法；中途遇到非 table
  （标量/nil）、字段不存在、空段一律按**键不存在**处理（Lua 侧即 `nil`）。
- `bb.has` / `bb.remove` 仍按完整字符串字面键操作，不按 `.` 拆分
  （`bb.has("proxy.ip")` 只看字面键；探测点号路径请用 `bb.get(...) ~= nil`）。
- 命中中间层 table 时返回的是**活 table 引用**，可在 Lua 侧继续索引。
