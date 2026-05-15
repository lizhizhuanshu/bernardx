# Lua API 参考

## 模块概览

| 模块 | 加载方式 | 说明 |
|------|---------|------|
| 全局内建 | 无需 require | `now` / `sleep` / `setTimeout` / `clearTimeout` |
| `http` | `require('http')` | HTTP 客户端 + WebSocket |
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

## 4. bt 模块

行为树模块，详见 [bt_node_config.md](bt_node_config.md)。

```lua
local bt = require('bt')
```

| 函数 | 说明 |
|------|------|
| `bt.run(json_or_path)` | 运行行为树（协程），接受 JSON 字符串或目录路径，返回 `"success"` / `"failure"` / `"stopped"` |
| `bt.stop()` | 停止行为树 |
| `bt.pause()` | 暂停行为树 |
| `bt.resume()` | 恢复行为树 |
| `bt.set(key, value)` | 设置黑板值 |
| `bt.get(key)` | 获取黑板值 |
| `bt.get_blackboard()` | 返回整个黑板为 table |
| `bt.notify(event, data)` | 发送事件到事件队列 |
| `bt.get_status()` | 获取状态 (`"running"` / `"paused"` / `"stopped"`) |
| `bt.get_current_node()` | 获取当前执行的节点名称 |

### bt.run(json_or_path)

协程异步——调用时 yield 挂起，行为树执行完成后自动恢复。

**两种调用方式：**

```lua
-- 方式1: JSON 字符串（以 { 或 [ 开头）
local status = bt.run('{"root": {"type": "Selector", "children": [...]}}')

-- 方式2: 目录路径
local status = bt.run("path/to/tree_dir")
```

**目录模式：** 指定一个包含行为树定义文件的目录：

```
tree_dir/
├── root.json       # 根树定义（必需）
├── combat.json     # 子树 "combat"（可选）
└── patrol.json     # 子树 "patrol"（可选）
```

- `root.json`：根节点定义
- 其他 `.json` 文件：文件名（去掉扩展名）作为子树名称，可在 root 中通过 `{"type": "Subtree", "subtree": "combat"}` 引用
- 非 `.json` 文件会被忽略

**返回值：** `string` — `"success"` / `"failure"` / `"stopped"`
