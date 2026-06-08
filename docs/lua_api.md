# Lua API 参考

## 模块概览

| 模块 | 加载方式 | 说明 |
|------|---------|------|
| 全局内建 | 无需 require | `now` / `sleep` / `setTimeout` / `clearTimeout` / `await` |
| `http` | `require('http')` | HTTP 客户端 + WebSocket |
| `blackboard` | `require('blackboard')` | 黑板键值存储（线程安全） |
| `bt` | `require('bt')` | 行为树（见 [bt_node_config.md](bt_node_config.md)） |

---

## 1. 全局内建函数

### now()

获取当前时间戳（毫秒）。

```lua
local t = now()
-- t: 自 epoch 以来的毫秒数 (int64)
```

**返回：** `integer` — 毫秒时间戳

---

### sleep(ms)

挂起当前协程指定毫秒。不会阻塞线程。

```lua
sleep(1000)  -- 等待 1 秒
```

| 参数 | 类型 | 说明 |
|------|------|------|
| ms | integer | 挂起时间（毫秒） |

**注意：** 只能在协程上下文中使用（`RunScript` / `bt.run` 内部均可）。

---

### setTimeout(ms, fn)

在指定毫秒后调用函数。类似浏览器 `setTimeout`。

```lua
local handle = setTimeout(500, function()
    print("500ms later")
end)
```

| 参数 | 类型 | 说明 |
|------|------|------|
| ms | integer | 延迟时间（毫秒） |
| fn | function | 回调函数（无参数） |

**返回：** `integer` — 定时器句柄，用于 `clearTimeout` 取消

---

### clearTimeout(handle)

取消由 `setTimeout` 创建的定时器。

```lua
local handle = setTimeout(1000, function() print("hi") end)
clearTimeout(handle)  -- 取消，回调不会执行
```

| 参数 | 类型 | 说明 |
|------|------|------|
| handle | integer | `setTimeout` 返回的句柄 |

---

### await(fn)

将回调式异步转为协程同步等待。类似 JavaScript 的 `Promise`。

调用后当前协程挂起，直到 `resolve` 或 `reject` 被调用时自动恢复。

```lua
local value = await(function(resolve)
    setTimeout(100, function()
        resolve(42)
    end)
end)
print(value)  -- 42
```

| 参数 | 类型 | 说明 |
|------|------|------|
| fn | function | 接收 `resolve` 和 `reject` 两个回调的函数 |

**fn 参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| resolve | function | 成功回调，参数作为 `await` 的返回值 |
| reject | function | 失败回调，参数作为错误信息 |

**返回值（resolve 时）：** 传入 `resolve` 的参数

**返回值（reject 时）：** `nil, string` — nil + 错误信息

**特性：**
- `resolve` 和 `reject` 只能调用一次，后续调用会被忽略
- 如果 `fn` 执行过程中抛出错误，会自动 `reject`
- 只能在协程上下文中使用

**示例：**

```lua
-- 等待 setTimeout 回调
local value = await(function(resolve)
    setTimeout(100, function()
        resolve(42)
    end)
end)
print(value)  -- 42

-- 带错误处理
local result, err = await(function(resolve, reject)
    setTimeout(100, function()
        if math.random() > 0.5 then
            resolve("ok")
        else
            reject("timeout")
        end
    end)
end)
if err then
    print("failed:", err)
else
    print("got:", result)
end
```

---

## 2. http 模块

通过 `require('http')` 加载。

```lua
local http = require('http')
```

### HTTP 请求

所有 HTTP 函数都是**协程异步**的——调用时会 yield 挂起，请求完成后自动恢复。

统一返回值：`status, body, err`

| 返回值 | 类型 | 说明 |
|--------|------|------|
| status | integer | HTTP 状态码（200, 404 等），失败时为 0 |
| body | string/nil | 响应体，失败时为 nil |
| err | string/nil | 错误信息，成功时为 nil |

#### http.get(url [, headers])

```lua
local status, body, err = http.get("https://example.com/api")
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| url | string | 是 | 请求 URL |
| headers | table | 否 | 请求头 `{["Key"] = "Value"}` |

#### http.post(url, body [, content_type [, headers]])

```lua
local status, body, err = http.post(
    "https://example.com/api",
    '{"name":"test"}',
    "json",
    {["Authorization"] = "Bearer xxx"}
)
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| url | string | 是 | 请求 URL |
| body | string | 否 | 请求体（默认 `""`） |
| content_type | string | 否 | 内容类型（见下表） |
| headers | table | 否 | 请求头 |

#### http.put(url, body [, content_type [, headers]])

参数同 `http.post`，发送 HTTP PUT 请求。

#### http.del(url [, headers])

参数同 `http.get`，发送 HTTP DELETE 请求。

**content_type 可选值：**

| 值 | 说明 |
|---|------|
| `"json"` | application/json |
| `"text"` | text/plain |
| `"html"` | text/html |
| `"xml"` | application/xml |
| `"form"` | application/x-www-form-urlencoded |
| `"octet"` | application/octet-stream |
| 不传 / 其他 | 不设置 Content-Type |

**示例：**

```lua
local http = require('http')

-- GET 请求
local status, body, err = http.get("https://httpbin.org/get")
if err then
    print("error:", err)
else
    print("status:", status, "body:", body)
end

-- POST JSON
local s, b, e = http.post("https://httpbin.org/post", '{"key":"value"}', "json")
print(s, b)

-- 带自定义 header
local s, b, e = http.get("https://api.example.com/data", {
    ["Accept"] = "application/json",
    ["X-Token"] = "abc123"
})
```

---

### WebSocket

#### http.ws_create(url)

创建 WebSocket 连接对象。

```lua
local ws = http.ws_create("ws://localhost:8080/ws")
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| url | string | 是 | WebSocket URL（`ws://` 或 `wss://`） |

**返回：** WebSocket 对象

#### ws:connect()

连接到 WebSocket 服务器。协程异步，连接完成后恢复。

```lua
local ok, err = ws:connect()
if not ok then
    print("connect failed:", err)
end
```

**返回：** `boolean, string|nil` — 是否成功 + 错误信息

#### ws:send(msg [, mode])

发送消息。协程异步。

```lua
local ok, err = ws:send("hello")
local ok, err = ws:send(binary_data, "binary")
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| msg | string | 是 | 消息内容 |
| mode | string | 否 | `"text"`（默认）或 `"binary"` |

**返回：** `boolean, string|nil` — 是否成功 + 错误信息

#### ws:close()

关闭 WebSocket 连接。协程异步。

```lua
ws:close()
```

**返回：** `boolean, string|nil`

#### 回调属性

通过赋值设置回调函数：

```lua
ws.onmessage = function(msg)
    print("received:", msg)
end

ws.onclose = function()
    print("connection closed")
end

ws.onerror = function(err)
    print("error:", err)
end
```

| 属性 | 类型 | 说明 |
|------|------|------|
| `onmessage` | function(msg: string) | 收到消息时触发 |
| `onclose` | function() | 连接关闭时触发 |
| `onerror` | function(err: string) | 发生错误时触发 |

**完整 WebSocket 示例：**

```lua
local http = require('http')

local ws = http.ws_create("ws://localhost:8080/echo")

ws.onmessage = function(msg)
    print("echo:", msg)
end

ws.onclose = function()
    print("disconnected")
end

local ok, err = ws:connect()
if not ok then
    print("connect failed:", err)
    return
end

ws:send("hello world")
sleep(1000)
ws:close()
```

---

## 3. require / loadfile

运行时提供了自定义的 `require` 和 `loadfile`，支持通过 `CodeProvider` 异步加载模块。

### require(module_name)

模块查找顺序：
1. `package.loaded` 缓存
2. C 模块（通过 `LuaRuntime::Builder::Register` 注册）
3. LuaLibrary（通过 `LuaRuntime::Builder::RegisterLibrary` 注册）
4. CodeProvider 异步加载

```lua
local http = require('http')
local bt = require('bt')
local my_module = require('my_module')  -- 通过 CodeProvider 加载
```

### loadfile(filename)

- 绝对路径（以 `/` 开头）：直接加载文件
- 相对路径：通过 CodeProvider 异步加载

```lua
local chunk = loadfile("scripts/helper.lua")
if chunk then
    chunk()
end
```

---

## 4. blackboard 模块

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

---

## 5. bt 模块

行为树模块，详见 [bt_node_config.md](bt_node_config.md)。

```lua
local bt = require('bt')
```

| 函数 | 说明 |
|------|------|
| `bt.set_project_path(path)` | 设置行为树项目根目录 |
| `bt.load(json_or_path)` | 加载并初始化行为树（协程异步） |
| `bt.tick()` | 执行一次 tick（协程异步），返回 `"running"` / `"success"` / `"failure"` |
| `bt.notify(event, data)` | 发送事件到事件队列 |
| `bt.get_status()` | 获取状态 (`"idle"` / `"running"` / `"success"` / `"failure"`) |

### bt.set_project_path(path)

设置行为树的项目根目录。路径以 `@` 开头表示远程资源路径（`@remote_base`），此时脚本通过 `ResourceProvider` 加载；否则为本地文件系统路径。

设置后，`bt.load()` 中的目录路径会相对于此路径解析，Script/Sensor 节点的脚本路径也基于此路径查找。

```lua
-- 本地路径
bt.set_project_path("/path/to/bt_project")
local ok = bt.load("trees/ai_main")

-- 远程路径（通过 ResourceProvider 加载脚本）
bt.set_project_path("@my_remote_project")
local ok = bt.load("trees/ai_main")
```

**BT 代码搜索路径**（本地模式，设置项目路径后）：

1. `{project_path}/scripts/`
2. `{project_path}/sensors/`
3. `{project_path}/`
4. `{主项目}/libs/`（主项目的共享库，通过 `SetMainLibsPath` 设置）

**BT 代码搜索路径**（远程模式，`@` 前缀）：

1. `{remote_base}/scripts/`
2. `{remote_base}/sensors/`
3. `{remote_base}/`

---

### bt.load(json_or_path)

协程异步——调用时 yield 挂起，初始化完成后自动恢复。

加载流程：
1. 解析 JSON 或从目录加载树结构
2. 创建对应的 `CodeProvider`（自动）
3. 初始化所有 Script 节点（加载脚本）
4. 初始化所有 Sensor（加载脚本）
5. 激活初始路径上的 Sensor

`bt.load()` 会自动根据项目路径创建 `CodeProvider` 并设置到运行时，确保 Script/Sensor 节点能正确加载脚本。如果运行时尚未配置 `CodeProvider`，`bt.load()` 创建的 `CodeProvider` 会自动成为运行时的默认 `CodeProvider`，后续 `require()` / `loadfile()` 也会使用它。

重复调用 `bt.load()` 会先停止并清理之前的树（递增 generation 以使旧的异步操作失效）。

**三种调用方式：**

```lua
-- 方式1: JSON 字符串（以 { 或 [ 开头）
local ok, err = bt.load('{"root": {"type": "Selector", "children": [...]}}')

-- 方式2: 目录路径
local ok, err = bt.load("path/to/tree_dir")

-- 方式3: 结合项目路径使用（推荐）
bt.set_project_path("/path/to/bt_project")
local ok, err = bt.load("trees/ai_main")
```

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| `ok` | `boolean` | `true` = 加载成功，`false` = 出错 |
| `err` | `string` | 出错时为错误信息，成功时为 `nil` |

---

### bt.tick()

协程异步——调用时 yield 挂起，执行一次 tick 后自动恢复。

每次 tick 内部依次执行：

1. **处理事件** — 将 `bt.notify()` 推入的事件从队列取出，写入黑板（`_event_{name}`）
2. **执行传感器** — 运行所有到期且处于激活状态的 Sensor，将结果写入黑板
3. **评估装饰器** — 检查装饰器条件变化，若条件从满足变为不满足则触发对应节点的中止（`OnAborted`）
4. **Tick 树** — 从根节点开始执行行为树
5. **更新传感器激活状态** — 根据当前活跃路径激活/停用 Sensor
6. **树完成时自动重置** — 若返回 `success` 或 `failure`，停用所有 Sensor 并重置树，下次 `bt.tick()` 会重新开始

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| `status` | `string` | `"running"` / `"success"` / `"failure"` |

如果未加载树，返回 `nil, "no tree loaded"`。

**使用示例：**

```lua
local bt = require('bt')
bt.set_project_path("/path/to/bt_project")

-- 加载树
local ok, err = bt.load("trees/ai_main")
if not ok then error(err) end

-- Lua 驱动 tick 循环
while true do
    local status = bt.tick()
    if status ~= "running" then
        print("tree finished:", status)
        break
    end
    coroutine.yield()  -- 让出控制权给事件循环
end
```
