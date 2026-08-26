# http 模块

通过 `require('http')` 加载。

```lua
local http = require('http')
```

## HTTP 请求

`http.request` 是**协程异步**的——调用时会 yield 挂起，请求完成后自动恢复。

### http.request(options)

发送一次 HTTP 请求，所有参数通过 `options` 表传入。

```lua
local status, body, err = http.request({
    url = "https://example.com/api",
    method = "POST",            -- 可选，默认 "GET"
    body = '{"name":"test"}',   -- 可选，默认 ""
    headers = {                 -- 可选
        ["Authorization"] = "Bearer xxx",
    },
    content_type = "json",      -- 可选
    timeout = 5000,             -- 可选，毫秒，不传或 <=0 用默认 60s
    proxy = "http://127.0.0.1:7890",  -- 可选，经代理服务器访问
})
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| url | string | 是 | 请求 URL |
| method | string | 否 | HTTP 方法，大小写不敏感；支持 `"GET"` / `"POST"` / `"PUT"` / `"DELETE"`，默认 `"GET"`，未知值回退为 GET |
| body | string | 否 | 请求体（默认 `""`） |
| headers | table | 否 | 请求头 `{["Key"] = "Value"}` |
| content_type | string | 否 | 内容类型（见下表） |
| timeout | integer | 否 | 请求超时（毫秒），超时后返回 `err`；不传或 `<= 0` 时使用默认 60s |
| proxy | string | 否 | HTTP 代理服务器，格式 `"http://[user:pass@]host[:port]"`（也可省略 `http://` 前缀）；端口缺省 `8080`；仅支持 HTTP 代理（不支持 socks） |

**返回值：** `status, body, err`

| 返回值 | 类型 | 说明 |
|--------|------|------|
| status | integer | HTTP 状态码（200, 404 等），失败时为 0 |
| body | string/nil | 响应体，失败时为 nil |
| err | string/nil | 错误信息，成功时为 nil |

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
local status, body, err = http.request({url = "https://httpbin.org/get"})
if err then
    print("error:", err)
else
    print("status:", status, "body:", body)
end

-- POST JSON
local s, b, e = http.request({
    url = "https://httpbin.org/post",
    method = "POST",
    body = '{"key":"value"}',
    content_type = "json",
})
print(s, b)

-- 带自定义 header
local s, b, e = http.request({
    url = "https://api.example.com/data",
    headers = {
        ["Accept"] = "application/json",
        ["X-Token"] = "abc123",
    },
})

-- 经代理服务器访问
local s, b, e = http.request({
    url = "https://httpbin.org/get",
    proxy = "http://127.0.0.1:7890",
})

-- 经需要 Basic 认证的代理访问
local s, b, e = http.request({
    url = "http://httpbin.org/get",
    proxy = "http://user:password@127.0.0.1:7890",
})
```

**代理说明：**

- `proxy` 仅对该次请求生效（无全局默认），不传则直连
- 明文 `http://` 目标：请求以绝对形式 URI 发给代理
- `https://` 目标：自动先与代理建立 CONNECT 隧道，再进行 TLS 握手
- URL 中内嵌 `user:pass@` 会作为代理的 Basic 认证（`Proxy-Authorization` 头）
- 仅支持 HTTP 正向代理；`socks5://` 等其他 scheme 会抛出 Lua 错误

---

## WebSocket

### http.ws_create(url [, options])

创建 WebSocket 连接对象。

```lua
local ws = http.ws_create("ws://localhost:8080/ws")
local ws = http.ws_create("ws://localhost:8080/ws", {proxy = "http://127.0.0.1:7890"})
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| url | string | 是 | WebSocket URL（`ws://` 或 `wss://`） |
| options | table | 否 | `{proxy = "http://[user:pass@]host[:port]"}` — 经 HTTP 代理建立 WebSocket 连接（格式同 `http.request` 的 `proxy` 字段；`wss://` 目标走 CONNECT 隧道） |

**返回：** WebSocket 对象

### ws:connect()

连接到 WebSocket 服务器。协程异步，连接完成后恢复。

```lua
local ok, err = ws:connect()
if not ok then
    print("connect failed:", err)
end
```

**返回：** `boolean, string|nil` — 是否成功 + 错误信息

### ws:send(msg [, mode])

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

### ws:close()

关闭 WebSocket 连接。协程异步。

```lua
ws:close()
```

**返回：** `boolean, string|nil`

### 回调属性

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
