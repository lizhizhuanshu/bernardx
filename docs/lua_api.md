# Lua API 参考

## 模块概览

| 模块 | 加载方式 | 说明 |
|------|---------|------|
| 全局内建 | 无需 require | `now` / `sleep` / `setTimeout` / `clearTimeout` / `await` |
| `http` | `require('http')` | HTTP 客户端 + WebSocket |
| `blackboard` | `require('blackboard')` | 黑板键值存储（线程安全） |
| `bt` | `require('bt')` | 行为树（见 [bt_node_config.md](bt_node_config.md)） |
| `async` | `require('async')` | 异步文件 I/O 和进程管理 |

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
| `bt.run(opts)` | 加载并运行行为树直到完成或超时（协程异步） |
| `bt.notify(event, data)` | 发送事件到事件队列 |
| `bt.get_status()` | 获取状态 (`"idle"` / `"running"` / `"success"` / `"failure"`) |

### bt.run(opts)

协程异步——调用时 yield 挂起，行为树运行完成后自动恢复。

`bt.run()` 是行为树的一站式入口，完成加载、初始化、tick 循环的完整流程：

1. 解析 JSON 或从目录加载树结构
2. 创建对应的 `CodeProvider`（自动）
3. 初始化所有 Script 节点（加载脚本）
4. 初始化所有 Sensor（加载脚本）
5. 激活初始路径上的 Sensor
6. 进入 tick 循环，直到树完成、达到最大步数或超时

**参数：** `opts` table

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `path` | `string` | `path` 和 `json` 至少一个 | 树文件/目录路径 |
| `json` | `string` | `path` 和 `json` 至少一个 | 树 JSON 字符串 |
| `project_path` | `string` | 否 | 行为树项目根目录 |
| `max_step` | `integer` | 否 | 最大 tick 步数，未设置则无限 |
| `timeout` | `integer` | 否 | 超时时间（毫秒），未设置则不超时 |
| `interval` | `integer` | 否 | tick 间隔（毫秒），未设置则连续 tick |

**`project_path` 说明：**

路径以 `@` 开头表示远程资源路径（`@remote_base`），此时脚本通过 `ResourceProvider` 加载；否则为本地文件系统路径。

如果未提供 `project_path`，则使用 C++ 端通过 `SetProjectPath()` 设置的路径。

**BT 代码搜索路径**（本地模式，设置项目路径后）：

1. `{project_path}/scripts/`
2. `{project_path}/sensors/`
3. `{project_path}/`
4. `{主项目}/libs/`（主项目的共享库，通过 `SetMainLibsPath` 设置）

**BT 代码搜索路径**（远程模式，`@` 前缀）：

1. `{remote_base}/scripts/`
2. `{remote_base}/sensors/`
3. `{remote_base}/`

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| `status` | `string` / `nil` | `"success"` / `"failure"` / `"timeout"`，出错时为 `nil` |
| `err` | `string` / `nil` | 出错时为错误信息，成功时为 `nil` |

**使用示例：**

```lua
local bt = require('bt')

-- 方式1: JSON 字符串
local status, err = bt.run({
    json = '{"root": {"type": "Selector", "children": [...]}}',
    project_path = "/path/to/bt_project"
})

-- 方式2: 目录路径
local status, err = bt.run({
    path = "trees/ai_main",
    project_path = "/path/to/bt_project"
})

-- 方式3: 带超时和步数限制
local status, err = bt.run({
    path = "trees/ai_main",
    project_path = "/path/to/bt_project",
    max_step = 100,
    timeout = 5000,
    interval = 100
})

-- 检查结果
if not status then
    error("bt.run failed: " .. err)
elseif status == "timeout" then
    print("tree timed out")
else
    print("tree finished:", status)
end
```

**每次 tick 内部依次执行：**

1. **处理事件** — 将 `bt.notify()` 推入的事件从队列取出，写入黑板（`_event_{name}`）
2. **执行传感器** — 运行所有到期且处于激活状态的 Sensor，将结果写入黑板
3. **评估装饰器** — 检查装饰器条件变化，若条件从满足变为不满足则触发对应节点的中止（`OnAborted`）
4. **Tick 树** — 从根节点开始执行行为树
5. **更新传感器激活状态** — 根据当前活跃路径激活/停用 Sensor
6. **树完成时自动重置** — 若返回 `success` 或 `failure`，停用所有 Sensor 并重置树

重复调用 `bt.run()` 会先停止并清理之前的树（递增 generation 以使旧的异步操作失效）。

`bt.run()` 会自动根据项目路径创建 `CodeProvider` 并设置到运行时，确保 Script/Sensor 节点能正确加载脚本。

---

## 6. async 模块

基于 asio 的异步文件 I/O 和进程管理。通过 `require('async')` 加载。

> **注意：** 需要在构建 `LuaRuntime` 时注册 `AsyncIOLibrary` 并传入 `asio::io_context`。
> 所有 read/write 操作均为**协程异步**——调用时 yield 挂起，I/O 完成后自动恢复。

```lua
local async = require('async')
```

### async.open(path [, mode])

打开文件，返回 `FileHandle` 对象。同步操作，不挂起协程。

```lua
local f, err = async.open("/tmp/data.txt", "w")
if not f then
    error("open failed: " .. tostring(err))
end
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| path | string | 是 | 文件路径 |
| mode | string | 否 | 打开模式（默认 `"r"`） |

**mode 可选值：**

| 值 | 说明 |
|---|------|
| `"r"` | 只读（默认） |
| `"w"` | 只写，创建或截断 |
| `"a"` | 追加，创建或定位到末尾 |
| `"r+"` | 读写 |
| `"w+"` | 读写，创建或截断 |

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| handle | FileHandle | 成功时返回文件句柄 |
| err | string / nil | 失败时返回错误信息 |

---

### async.exec(cmd)

创建子进程，返回 `ProcessHandle` 对象。子进程的 stderr 合并到 stdout。同步操作，不挂起协程。

```lua
local p = async.exec("echo hello")
local data = p:read("*a")
local code = p:close()
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| cmd | string | 是 | 通过 `/bin/sh -c` 执行的命令 |

**返回值：** `ProcessHandle` — 进程句柄

---

### FileHandle

由 `async.open` 返回的文件句柄对象。

#### f:read([fmt])

读取文件内容。协程异步。

```lua
local all  = f:read("*a")   -- 读取全部
local line = f:read("*l")   -- 读取一行
local chunk = f:read(1024)  -- 读取 1024 字节
local line  = f:read()      -- 默认等价于 "*l"
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| fmt | string / integer | 否 | `"*a"` 全部、`"*l"` 一行（默认）、数字表示字节数 |

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| data | string / nil | 读取到的内容，EOF 时返回 `nil` |
| err | string / nil | 文件已关闭或出错时返回错误信息 |

**`"*l"` 模式：** 读取到 `\n` 为止（不含换行符），EOF 时返回剩余内容或 `nil`。

#### f:write(data)

向文件写入字符串。协程异步。

```lua
local ok, err = f:write("hello world")
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| data | string | 是 | 要写入的内容 |

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| ok | boolean | 成功返回 `true` |
| err | string / nil | 失败时返回错误信息 |

#### f:close()

关闭文件句柄。同步操作。重复调用安全。

```lua
f:close()
```

**返回值：** `boolean` — 始终返回 `true`

---

### ProcessHandle

由 `async.exec` 返回的进程句柄对象。

#### p:read([fmt])

从进程 stdout 读取。协程异步。格式与 `FileHandle:read` 相同。

```lua
local all  = p:read("*a")   -- 读取全部输出
local line = p:read("*l")   -- 读取一行
local chunk = p:read(256)   -- 读取 256 字节
```

参数和返回值同 `f:read`。

#### p:write(data)

向进程 stdin 写入。协程异步。

```lua
local ok, err = p:write("some input\n")
```

参数和返回值同 `f:write`。

#### p:shutdown()

关闭进程 stdin（发送 EOF）。同步操作。通常用于通知进程输入结束。

```lua
p:write("data\n")
p:shutdown()  -- 通知进程输入完毕
local output = p:read("*a")  -- 读取全部输出
```

**返回值：** `boolean` — 始终返回 `true`

#### p:kill([signal])

向进程发送信号。同步操作，不挂起协程。

```lua
p:kill()           -- 发送 SIGTERM（默认）
p:kill(9)          -- 发送 SIGKILL
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| signal | integer | 否 | 信号编号（默认 `15`，即 SIGTERM） |

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| ok | boolean | 成功返回 `true` |
| err | string / nil | 进程已关闭或 kill 失败时返回错误信息 |

**注意：** `kill` 只发送信号，不关闭句柄。之后仍需调用 `close()` 回收进程资源。被信号终止的进程，`close()` 返回信号编号的负值（如 SIGTERM 返回 `-15`）。

#### p:close()

等待进程退出并关闭句柄。协程异步。

```lua
local exit_code = p:close()
```

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| code | integer | 进程退出码（正常退出）或信号的负值（被信号终止） |
| err | string / nil | `waitpid` 失败时返回错误信息 |

---

### 完整示例

#### 文件读写

```lua
local async = require('async')

-- 写入文件
local f = async.open("/tmp/test.txt", "w")
f:write("line one\n")
f:write("line two\n")
f:close()

-- 逐行读取
local f2 = async.open("/tmp/test.txt", "r")
while true do
    local line = f2:read("*l")
    if not line then break end
    print(line)
end
f2:close()

-- 追加模式
local f3 = async.open("/tmp/test.txt", "a")
f3:write("line three\n")
f3:close()
```

#### 进程管道

```lua
local async = require('async')

-- 执行命令并获取输出
local p = async.exec("ls -la /tmp")
local output = p:read("*a")
local code = p:close()
print("exit:", code, "output:", output)

-- 与子进程交互
local p2 = async.exec("cat")
p2:write("hello\n")
p2:write("world\n")
p2:shutdown()
local l1 = p2:read("*l")  -- "hello"
local l2 = p2:read("*l")  -- "world"
local code = p2:close()
assert(l1 == "hello")
assert(l2 == "world")
assert(code == 0)

-- 进程输出写入文件
local p3 = async.exec("echo generated_data")
local data = p3:read("*a")
p3:close()
local f = async.open("/tmp/out.txt", "w")
f:write(data)
f:close()

-- 终止长时间运行的进程
local p4 = async.exec("sleep 60")
p4:kill()               -- 发送 SIGTERM
local code = p4:close()  -- 回收进程，code == -15
```

#### 错误处理

```lua
local async = require('async')

-- 文件不存在
local f, err = async.open("/nonexistent/file.txt", "r")
assert(f == nil)
assert(err ~= nil)

-- 关闭后操作
local f2 = async.open("/tmp/test.txt", "w")
f2:close()
local data, err2 = f2:read("*a")
assert(data == nil)
assert(err2 ~= nil)
```
