# 传感器 (Sensors)

传感器是按需运行的异步感知模块，声明在节点上。当节点处于活跃执行路径时，传感器自动激活并周期性执行；节点离开活跃路径时自动停用。

---

## 何时使用传感器

当条件判断依赖异步数据（如 DOM 查询、网络请求）时，用传感器定期将结果写入黑板，`BlackboardCondition` 装饰器同步读取黑板缓存值。

---

## 配置方式

传感器有两种配置方式：**内联定义**和**全局定义 + 名称引用**。

### 方式一：内联定义

在节点的 `"sensors"` 数组中直接写完整配置：

```json
{
  "type": "Sequence",
  "sensors": [
    {
      "name": "login_btn",
      "path": "sensors/element_visible.lua",
      "interval": 100,
      "args": {"selector": "#login-btn"}
    }
  ],
  "children": [...]
}
```

### 方式二：全局定义 + 名称引用

在树目录的 `sensors.json` 中定义传感器，节点通过名称字符串引用：

**sensors.json：**
```json
{
  "nearby": {
    "description": "附近目标检测",
    "interval": 200,
    "args": {"range": 50}
  },
  "combat.target_check": {
    "interval": 100
  }
}
```

**root.json 中引用：**
```json
{
  "type": "Script",
  "path": "scripts/attack.lua",
  "sensors": ["nearby", "combat.target_check"]
}
```

名称引用时，`sensors.json` 中的定义提供 `interval`、`path`、`args` 等参数。如果全局定义中没有指定 `path`，则根据名称自动推导（见下方路径推导规则）。

### 混合使用

`"sensors"` 数组支持混合使用字符串引用和内联对象：

```json
{
  "sensors": [
    "nearby",
    {"name": "custom", "path": "sensors/custom.lua", "interval": 50}
  ]
}
```

---

## 字段参考

### 节点 `sensors` 数组中的条目

| 条目类型 | 格式 | 说明 |
|---------|------|------|
| 字符串 | `"nearby"` | 引用 `sensors.json` 中定义的传感器 |
| 对象 | `{"name": "...", ...}` | 内联定义传感器配置 |

### 传感器配置字段

| 字段 | 必填 | 类型 | 说明 |
|------|------|------|------|
| `name` | **是** | string | 传感器名称，同时作为黑板键名（Tick 返回值写入 `bb[name]`） |
| `path` | 否 | string | Lua 脚本路径。未指定时根据 name 自动推导 |
| `interval` | 否 | integer | Tick 间隔（毫秒），默认 `100` |
| `description` | 否 | string | 传感器描述 |
| `args` | 否 | object | 传递给 `Enter` 回调的参数对象 |

`args` 值支持类型：`bool`、`int`（int64）、`double`、`string`。

### 路径自动推导

当未显式指定 `path` 时，根据 `name` 自动推导脚本路径：
- 名称中的 `.` 替换为 `/`
- 添加 `sensors/` 前缀和 `.lua` 后缀

| name | 推导路径 |
|------|---------|
| `"nearby"` | `sensors/nearby.lua` |
| `"combat.target_check"` | `sensors/combat/target_check.lua` |
| `"ui.login.visible"` | `sensors/ui/login/visible.lua` |

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
  "sensors": [
    "login_visible"
  ],
  "children": [
    {
      "type": "Script",
      "path": "scripts/click_login.lua",
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
