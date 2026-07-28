# 全局内建函数

无需 `require`，在所有协程上下文中直接可用。

---

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

**注意：** 只能在协程上下文中使用（`RunScript` / `bt.ready` / `bt.exec` 内部均可）。

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
