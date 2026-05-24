# 传感器 (Sensors)

传感器是按需运行的异步感知模块，声明在节点上。当节点处于活跃执行路径时，传感器自动激活并周期性执行；节点离开活跃路径时自动停用。

---

## 何时使用传感器

当条件判断依赖异步数据（如 DOM 查询、网络请求）时，用传感器定期将结果写入黑板，`BlackboardCondition` 装饰器同步读取黑板缓存值。

---

## JSON 配置

在任意节点（复合节点或叶子节点）上通过 `"sensors"` 数组声明：

```json
{
  "type": "Sequence",
  "sensors": [
    {"name": "login_btn", "path": "sensors/element_visible.lua", "interval": 100}
  ],
  "children": [...]
}
```

| 字段 | 必填 | 类型 | 说明 |
|------|------|------|------|
| `name` | **是** | string | 传感器名称，同时作为黑板键名（Tick 返回值写入 `bb[name]`） |
| `path` | **是** | string | Lua 脚本路径 |
| `interval` | **是** | integer | Tick 间隔（毫秒） |
| `args` | 否 | object | 传递给 `Enter` 回调的参数对象 |

`args` 值支持类型：`bool`、`int`（int64）、`double`、`string`。

---

## 传感器脚本格式

脚本文件必须 **return 一个 table**，使用冒号语法定义回调函数。`self` 是脚本 table 本身，用于存储跨 tick 的用户状态：

```lua
-- sensors/element_visible.lua

local M = {}

function M:Enter(args)
  -- args = JSON 中的 "args" 对象（可选）
  -- self = 脚本 table，可自由存储状态
  print("sensor activated")
end

function M:Tick()
  -- 按 interval 周期调用（协程，可 yield）
  local found = coroutine.yield(async_query("#login-btn"))
  return found ~= nil  -- 返回值 → 黑板[传感器名]
end

function M:Exit()
  -- 停用时调用一次（同步，不可 yield）
  print("sensor deactivated")
end

return M
```

| 回调 | 签名 | 必需 | 可否 yield | 说明 |
|------|------|------|-----------|------|
| `Enter` | `self:Enter(args)` | 否 | 否 | 激活时调用一次，args 为 JSON 中的 `"args"` 对象 |
| `Tick` | `self:Tick()` | **是** | **是** | 按 interval 周期调用，返回值写入黑板 |
| `Exit` | `self:Exit()` | 否 | 否 | 停用时调用一次 |

`self` 是脚本返回的 table，可自由存储跨 tick 的状态。黑板通过 `blackboard` 模块（`bb.get`/`bb.set`）访问。

---

## 传感器与装饰器配合

传感器写入黑板，`BlackboardCondition` 读取黑板：

```json
{
  "type": "Selector",
  "children": [
    {
      "type": "Script",
      "path": "scripts/click_login.lua",
      "sensors": [
        {"name": "login_visible", "path": "sensors/element_visible.lua", "interval": 100}
      ],
      "decorators": [
        {
          "type": "BlackboardCondition",
          "key": "login_visible",
          "operator": "is_set",
          "abort": "Self"
        }
      ]
    }
  ]
}
```

工作流程：
1. 传感器 `login_visible` 每 100ms 查询 DOM，结果写入 `bb.login_visible`
2. `BlackboardCondition` 每次 tick 前同步检查 `bb.login_visible`
3. 条件满足 → 执行脚本；条件不满足 → 中止执行

---

## 生命周期

- **激活**：节点进入活跃路径（root → 当前 child → ... → 当前 leaf）时，其声明的传感器被激活
- **Tick**：按 `interval` 间隔执行 Tick 函数，支持协程 yield
- **停用**：节点离开活跃路径时，其传感器被停用（如果无其他活跃节点共享同一传感器）
