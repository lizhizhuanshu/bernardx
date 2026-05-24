# 叶子节点 (Leaf)

无子节点，执行具体逻辑。

---

## Script - 脚本节点

执行 Lua 脚本文件。

```json
{
  "type": "Script",
  "path": "scripts/attack.lua",
  "name": "可选名称（默认为 path）",
  "args": {
    "target": "enemy",
    "damage": 100
  }
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `path` | **是** | Lua 脚本文件路径（相对路径基于项目根目录解析） |
| `name` | 否 | 节点名称，默认等于 path |
| `args` | 否 | 传递给 `Enter` 回调的参数对象 |

`args` 值支持类型：`bool`、`int`（int64）、`double`、`string`。

### 脚本格式

脚本文件必须 **return 一个 table**，使用冒号语法定义回调函数。`self` 是脚本 table 本身，用于存储跨 tick 的用户状态：

```lua
-- scripts/attack.lua

local M = {}

function M:Enter(args)
  -- args = JSON 中的 "args" 对象
  -- self = 脚本 table，可自由存储状态
  self.target = args.target
  self.damage = args.damage
end

function M:Tick()
  -- 每次 tick 调用（协程，可 yield）
  -- 这是**必需**的回调
  if self.target then
    return "success"
  end
  return "failure"
end

function M:Exit(reason)
  -- reason = "success" | "failure" | "aborted" | "reset"
  print("exit:", reason)
end

function M:Abort()
  print("aborted")
end

return M
```

| 回调 | 签名 | 必需 | 可否 yield | 说明 |
|------|------|------|-----------|------|
| `Enter` | `self:Enter(args)` | 否 | 否 | 节点进入活跃状态时调用一次 |
| `Tick` | `self:Tick()` | **是** | **是** | 每次 tick 调用，返回状态字符串 |
| `Exit` | `self:Exit(reason)` | 否 | 否 | 节点离开活跃状态时调用 |
| `Abort` | `self:Abort()` | 否 | 否 | 节点被强制中止时调用（在 Exit 之前） |

**注意：** 脚本必须返回 **table**。如果脚本未返回 table 或缺少 `Tick` 函数，节点将加载失败，tick 时直接返回 Failure。

### self — 脚本状态

`self` 是脚本返回的 table 本身，可自由存储跨 tick 的状态：

```lua
local M = {}
function M:Enter(args)
  self.counter = 0
end
function M:Tick()
  self.counter = self.counter + 1
  if self.counter >= 3 then
    return "success"
  end
  return "running"
end
return M
```

### 黑板访问

通过 `blackboard` 模块读写黑板：

```lua
local bb = require('blackboard')

local M = {}
function M:Tick()
  local hp = bb.get("hp")
  if hp and hp > 50 then
    bb.set("last_attack_time", os.time())
    return "success"
  end
  return "failure"
end
return M
```

黑板值支持类型：`nil`、`boolean`、`integer`（int64）、`double`、`string`。

### Tick 返回值

`Tick` 必须返回以下字符串之一：

| 返回值 | 说明 |
|--------|------|
| `"success"` | 节点成功完成，进入 Exit 回调 |
| `"failure"` | 节点失败，进入 Exit 回调 |
| `"running"` | 节点仍在执行，下次 tick 继续调用 Tick（不会触发 Enter/Exit） |

**异常情况：** 如果 Tick 未返回值、返回 nil、或返回无法识别的字符串，节点视为 Failure。

### 异步操作

`Tick` 在协程中执行，可以使用所有异步 API（`sleep`、`await`、`http.get` 等）：

```lua
local M = {}
function M:Tick()
  sleep(500)

  local status, body, err = http.get("https://api.example.com/target")
  if err then
    return "failure"
  end

  bb.set("target", body)
  return "success"
end
return M
```

当 Tick 中执行异步操作时，节点返回 `Running` 状态挂起，异步操作完成后自动恢复 Tick 执行。

### 生命周期

1. **首次 Tick** → 调用 `self:Enter(args)` → 调用 `self:Tick()` → 根据返回值：
   - `"success"` / `"failure"` → 调用 `self:Exit(reason)` → 节点完成
   - `"running"` → 保持活跃，下次 tick 直接调用 `Tick`（不再调 Enter）
2. **后续 Tick**（仍为活跃状态）→ 直接调用 `self:Tick()`
3. **被中止**（如 BlackboardCondition 中断）→ 调用 `self:Abort()` → 调用 `self:Exit("aborted")`
4. **被重置**（Reset）→ 调用 `self:Exit("reset")` → 节点回到非活跃状态

---

## Subtree - 子树节点

引用 `"subtrees"` 中定义的子树。子树可复用、可嵌套。

```json
{
  "subtrees": {
    "combat": {
      "type": "Sequence",
      "children": [
        {"type": "Script", "path": "aim.lua"},
        {"type": "Script", "path": "attack.lua"}
      ]
    }
  },
  "root": {
    "type": "Subtree",
    "subtree": "combat",
    "name": "可选名称"
  }
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `subtree` | **是** | `"subtrees"` 中定义的子树名称 |
| `name` | 否 | 节点名称，默认等于 subtree 名 |

Subtree 节点支持 `decorators` 和 `sensors`，与普通节点一致。子树定义内也可以引用其他子树（支持嵌套）。

---

## Wait - 等待节点

等待指定时间后返回成功。

```json
{
  "type": "Wait",
  "ms": 500
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `ms` | 否 | 等待时间（毫秒），默认 `1000` |

**行为：** 首次 tick 记录起始时间，在等待时间到达前持续返回 Running，时间到达后返回 Success。
