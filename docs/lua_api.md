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
| `bt.run(json_or_path)` | 运行行为树（协程），接受 JSON 字符串或目录路径，返回 `"success"` / `"failure"` / `"stopped"` |
| `bt.stop()` | 停止行为树 |
| `bt.pause()` | 暂停行为树 |
| `bt.resume()` | 恢复行为树 |
| `bt.notify(event, data)` | 发送事件到事件队列 |
| `bt.get_status()` | 获取状态 (`"running"` / `"paused"` / `"stopped"`) |
| `bt.get_current_node()` | 获取当前执行的节点名称 |

### bt.set_project_path(path)

设置行为树的项目根目录。设置后，`bt.run()` 中的目录路径会相对于此路径解析，Script/Sensor 节点的脚本路径也基于此路径查找。同时，BT 的 `require()` 搜索路径也会基于此路径。

```lua
bt.set_project_path("/path/to/bt_project")
local status = bt.run("trees/ai_main")
```

**BT 代码搜索路径**（设置项目路径后）：

1. `{project_path}/scripts/`
2. `{project_path}/sensors/`
3. `{project_path}/`
4. `{主项目}/libs/`（主项目的共享库）

**注意：** 如果未设置项目路径，`bt.run()` 的行为与之前相同——使用主运行时的 CodeProvider。

---

### bt.run(json_or_path)

协程异步——调用时 yield 挂起，行为树执行完成后自动恢复。

**两种调用方式：**

```lua
-- 方式1: JSON 字符串（以 { 或 [ 开头）
local ok, err = bt.run('{"root": {"type": "Selector", "children": [...]}}')

-- 方式2: 目录路径
local ok, err = bt.run("path/to/tree_dir")

-- 方式3: 结合项目路径使用（推荐）
bt.set_project_path("/path/to/bt_project")
local ok = bt.run("trees/ai_main")  -- 解析为 /path/to/bt_project/trees/ai_main
```

设置了项目路径时，目录路径会相对于项目根目录解析；未设置时，路径相对于当前工作目录。

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

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| `ok` | `boolean` | `true` = 正常完成，`false` = 出错 |
| `status` | `string` | 正常时为 `"success"` / `"stopped"`；出错时为错误信息 |

```lua
local ok, status = bt.run(json)
if ok then
    print("result: " .. status)   -- "success" or "stopped"
else
    print("failed: " .. status)   -- 错误信息
end
```
