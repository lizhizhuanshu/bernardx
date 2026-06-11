# async 模块

基于 asio 的异步文件 I/O 和进程管理。通过 `require('async')` 加载。

> **注意：** 需要在构建 `LuaRuntime` 时注册 `AsyncIOLibrary` 并传入 `asio::io_context`。
> 所有 read/write 操作均为**协程异步**——调用时 yield 挂起，I/O 完成后自动恢复。

```lua
local async = require('async')
```

## async.open(path [, mode])

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

## async.exec(cmd)

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

## FileHandle

由 `async.open` 返回的文件句柄对象。

### f:read([fmt])

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

### f:write(data)

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

### f:close()

关闭文件句柄。同步操作。重复调用安全。

```lua
f:close()
```

**返回值：** `boolean` — 始终返回 `true`

---

## ProcessHandle

由 `async.exec` 返回的进程句柄对象。

### p:read([fmt])

从进程 stdout 读取。协程异步。格式与 `FileHandle:read` 相同。

```lua
local all  = p:read("*a")   -- 读取全部输出
local line = p:read("*l")   -- 读取一行
local chunk = p:read(256)   -- 读取 256 字节
```

参数和返回值同 `f:read`。

### p:write(data)

向进程 stdin 写入。协程异步。

```lua
local ok, err = p:write("some input\n")
```

参数和返回值同 `f:write`。

### p:shutdown()

关闭进程 stdin（发送 EOF）。同步操作。通常用于通知进程输入结束。

```lua
p:write("data\n")
p:shutdown()  -- 通知进程输入完毕
local output = p:read("*a")  -- 读取全部输出
```

**返回值：** `boolean` — 始终返回 `true`

### p:kill([signal])

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

### p:close()

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

## 完整示例

### 文件读写

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

### 进程管道

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

### 错误处理

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
