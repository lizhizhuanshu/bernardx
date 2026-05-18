# 行为树节点 JSON 配置参考

## 顶层结构

JSON 必须包含 `"root"` 字段，所有节点必须包含 `"type"` 字段。可选的 `"subtrees"` 字段定义可复用的子树。

### 内联 JSON 模式

通过 `bt.run(json_string)` 直接传入完整 JSON：

```json
{
  "subtrees": {
    "子树名": { "type": "...", "children": [...] }
  },
  "root": {
    "type": "节点类型",
    "name": "可选名称",
    "children": [...],
    "decorators": [...]
  }
}
```

### 目录模式

通过 `bt.run(directory_path)` 从目录加载树定义。如果设置了项目路径（`bt.set_project_path`），路径相对于项目根目录解析：

```
tree_dir/
├── root.json       # 根树定义（必需）
├── combat.json     # 子树 "combat"
└── patrol.json     # 子树 "patrol"
```

- `root.json`：根节点定义（与内联模式的 `"root"` 字段内容格式相同）
- 其他 `.json` 文件：文件名（去掉扩展名）作为子树名称，等价于内联模式 `"subtrees"` 中的一个条目

```lua
local bt = require('bt')

-- 不使用项目路径（相对当前工作目录）
local status = bt.run("trees/ai_main")

-- 使用项目路径（推荐）
bt.set_project_path("/path/to/bt_project")
local status = bt.run("trees/ai_main")  -- 解析为 /path/to/bt_project/trees/ai_main
```

---

## 1. 复合节点 (Composite)

有子节点，通过 `"children"` 数组配置。

### Selector - 选择节点

依次执行子节点，任一成功即返回成功（OR 逻辑）。

```json
{
  "root": {
    "type": "Selector",
    "name": "可选名称",
    "children": [...]
  }
}
```

**行为：** 从左到右依次 tick 子节点，第一个返回 Success 的子节点即返回 Success；全部 Failure 才返回 Failure。Running 时记住当前位置，下次从该子节点继续。

### Sequence - 顺序节点

依次执行子节点，全部成功才返回成功（AND 逻辑）。

```json
{
  "root": {
    "type": "Sequence",
    "name": "可选名称",
    "children": [...]
  }
}
```

**行为：** 从左到右依次 tick 子节点，第一个返回 Failure 的子节点即返回 Failure；全部 Success 才返回 Success。Running 时记住当前位置。

### Parallel - 并行节点

同时执行所有子节点，通过策略控制成功/失败判定。

```json
{
  "root": {
    "type": "Parallel",
    "name": "可选名称",
    "success_policy": "RequireAll",
    "failure_policy": "RequireOne",
    "children": [...]
  }
}
```

| 字段 | 可选值 | 默认值 | 说明 |
|------|--------|--------|------|
| `success_policy` | `"RequireAll"` / `"RequireOne"` | `"RequireAll"` | 全部成功才算成功 / 任一成功即成功 |
| `failure_policy` | `"RequireAll"` / `"RequireOne"` | `"RequireOne"` | 全部失败才算失败 / 任一失败即失败 |

---

## 2. 叶子节点 (Leaf)

无子节点，执行具体逻辑。

### Script - 脚本节点

执行 Lua 脚本文件。

```json
{
  "type": "Script",
  "path": "scripts/attack.lua",
  "name": "可选名称（默认为 path）"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `path` | **是** | Lua 脚本文件路径（相对路径基于项目根目录解析） |
| `name` | 否 | 节点名称，默认等于 path |

#### 脚本格式

脚本文件必须 **return 一个 table**，包含回调函数：

```lua
-- scripts/attack.lua
return {
    Enter = function(bb)
        -- 节点首次被 tick 前调用（同步，不可 yield）
        print("enter attack")
    end,

    Tick = function(bb)
        -- 每次 tick 调用（协程，可 yield）
        -- 这是**必需**的回调
        if bb.target then
            return "success"
        end
        return "failure"
    end,

    Exit = function(bb, reason)
        -- 节点完成/中止时调用（同步，不可 yield）
        -- reason: "success" | "failure" | "aborted" | "reset"
        print("exit:", reason)
    end,

    Abort = function()
        -- 节点被强制中止时调用，在 Exit 之前（同步，不可 yield）
        -- 无参数
        print("aborted")
    end
}
```

| 回调 | 参数 | 必需 | 可否 yield | 说明 |
|------|------|------|-----------|------|
| `Enter` | `bb` (table) | 否 | 否 | 节点进入活跃状态时调用一次 |
| `Tick` | `bb` (table) | **是** | **是** | 每次 tick 调用，返回状态字符串 |
| `Exit` | `bb` (table), `reason` (string) | 否 | 否 | 节点离开活跃状态时调用 |
| `Abort` | 无 | 否 | 否 | 节点被强制中止时调用（在 Exit 之前） |

**注意：** 脚本必须返回 **table**。如果脚本未返回 table 或缺少 `Tick` 函数，节点将加载失败，tick 时直接返回 Failure。

#### bb 参数（黑板）

所有回调的第一个参数 `bb` 是黑板 table，可直接读写：

```lua
return {
    Tick = function(bb)
        -- 读取黑板
        local hp = bb.hp
        local name = bb.name

        -- 写入黑板（可被其他节点的 BlackboardCondition 装饰器感知）
        bb.last_attack_time = now()

        return "success"
    end
}
```

黑板值支持类型：`nil`、`boolean`、`integer`（int64）、`double`、`string`。

#### Tick 返回值

`Tick` 必须返回以下字符串之一：

| 返回值 | 说明 |
|--------|------|
| `"success"` | 节点成功完成，进入 Exit 回调 |
| `"failure"` | 节点失败，进入 Exit 回调 |
| `"running"` | 节点仍在执行，下次 tick 继续调用 Tick（不会触发 Enter/Exit） |

**异常情况：** 如果 Tick 未返回值、返回 nil、或返回无法识别的字符串，节点视为 Failure。

#### 异步操作

`Tick` 在协程中执行，可以使用所有异步 API（`sleep`、`await`、`http.get` 等）：

```lua
return {
    Tick = function(bb)
        -- 等待 500ms
        sleep(500)

        -- 异步 HTTP 请求
        local status, body, err = http.get("https://api.example.com/target")
        if err then
            return "failure"
        end

        bb.target = body
        return "success"
    end
}
```

当 Tick 中执行异步操作时，节点返回 `Running` 状态挂起，异步操作完成后自动恢复 Tick 执行。

#### 生命周期

1. **首次 Tick** → 调用 `Enter(bb)` → 调用 `Tick(bb)` → 根据返回值：
   - `"success"` / `"failure"` → 调用 `Exit(bb, reason)` → 节点完成
   - `"running"` → 保持活跃，下次 tick 直接调用 `Tick`（不再调 Enter）
2. **后续 Tick**（仍为活跃状态）→ 直接调用 `Tick(bb)`
3. **被中止**（如 BlackboardCondition 中断）→ 调用 `Abort()` → 调用 `Exit(bb, "aborted")`
4. **被重置**（Reset）→ 调用 `Exit(bb, "reset")` → 节点回到非活跃状态

### Subtree - 子树节点

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

## 3. 装饰器 (Decorators)

附加在任意节点上，通过 `"decorators"` 数组配置。装饰器在节点 tick 前评估条件。

```json
{
  "type": "Script",
  "path": "a.lua",
  "decorators": [
    { "type": "BlackboardCondition", ... },
    { "type": "Inverter" }
  ]
}
```

### BlackboardCondition - 黑板条件

根据黑板数据决定节点是否可执行。

```json
{
  "type": "BlackboardCondition",
  "key": "hp",
  "operator": "greater_than",
  "value": 50,
  "abort": "Self"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `key` | **是** | 黑板键名 |
| `operator` | 否 | 比较运算符，默认 `"is_set"` |
| `value` | 否 | 期望值（部分运算符需要） |
| `abort` | 否 | 中断模式，默认 `"None"` |

**operator 可选值：**

| 值 | 说明 | 需要 value |
|---|------|-----------|
| `"is_set"` | 键存在即通过 | 否 |
| `"is_not_set"` | 键不存在即通过 | 否 |
| `"equals"` | 值相等 | 是 |
| `"not_equals"` | 值不相等 | 是 |
| `"greater_than"` | 值大于 | 是 |
| `"less_than"` | 值小于 | 是 |

**value 支持类型：** `bool`、`int`（int64）、`double`、`string`。类型不匹配时条件为 false。

**abort 可选值（UE4/5 风格观察者中止）：**

| 值 | 说明 |
|---|------|
| `"None"` | 不中断（默认） |
| `"Self"` | 条件变化时中断自身正在执行的子树 |
| `"LowerPriority"` | 条件变化时中断右侧低优先级节点 |
| `"Both"` | 同时中断自身和低优先级 |

### Inverter - 取反装饰器

反转被装饰节点的成功/失败结果。

```json
{"type": "Inverter", "abort": "None"}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `abort` | 否 | 中断模式，默认 `"None"` |

### ForceSuccess - 强制成功装饰器

无论被装饰节点结果如何，始终返回成功。

```json
{"type": "ForceSuccess"}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `abort` | 否 | 中断模式，默认 `"None"` |

---

## 4. 传感器 (Sensors)

传感器是按需运行的异步感知模块，声明在节点上。当节点处于活跃执行路径时，传感器自动激活并周期性执行；节点离开活跃路径时自动停用。

### 何时使用传感器

当条件判断依赖异步数据（如 DOM 查询、网络请求）时，用传感器定期将结果写入黑板，`BlackboardCondition` 装饰器同步读取黑板缓存值。

### JSON 配置

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

### 传感器脚本格式

脚本 `return` 一个 **table** 或 **function**：

#### Table 格式（推荐）

```lua
-- sensors/element_visible.lua
return {
    Enter = function(bb)
        -- 节点进入活跃路径时调用（同步，不可 yield）
        print("sensor activated")
    end,

    Tick = function(bb)
        -- 周期性调用（协程，可 yield）
        local found = coroutine.yield(async_query("#login-btn"))
        return found ~= nil  -- 返回值 → 黑板[传感器名]
    end,

    Exit = function(bb)
        -- 节点离开活跃路径时调用（同步，不可 yield）
        print("sensor deactivated")
    end
}
```

| 回调 | 参数 | 可否 yield | 说明 |
|------|------|-----------|------|
| `Enter` | `bb` (table) | 否 | 激活时调用一次 |
| `Tick` | `bb` (table) | **是** | 按 interval 周期调用，返回值写入黑板 |
| `Exit` | `bb` (table) | 否 | 停用时调用一次 |

`Enter` 和 `Exit` 是可选的，`Tick` 是必需的。

#### Function 格式（简化）

```lua
-- sensors/check_hp.lua
return function(bb)
    return bb.hp and bb.hp > 0
end
```

返回单个函数时，等同于只有 `Tick`。

### 传感器与装饰器配合

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

### 生命周期

- **激活**：节点进入活跃路径（root → 当前 child → ... → 当前 leaf）时，其声明的传感器被激活
- **Tick**：按 `interval` 间隔执行 Tick 函数，支持协程 yield
- **停用**：节点离开活跃路径时，其传感器被停用（如果无其他活跃节点共享同一传感器）

---

## 5. 完整示例

### 内联 JSON 示例

```json
{
  "root": {
    "type": "Selector",
    "name": "ai_root",
    "decorators": [
      {
        "type": "BlackboardCondition",
        "key": "alive",
        "operator": "is_set",
        "abort": "Self"
      }
    ],
    "children": [
      {
        "type": "Sequence",
        "name": "combat",
        "decorators": [
          {
            "type": "BlackboardCondition",
            "key": "has_target",
            "operator": "is_set"
          }
        ],
        "children": [
          {"type": "Script", "path": "scripts/aim.lua", "name": "aim"},
          {"type": "Script", "path": "scripts/attack.lua", "name": "attack"}
        ]
      },
      {
        "type": "Sequence",
        "name": "patrol",
        "children": [
          {"type": "Script", "path": "scripts/find_point.lua"},
          {"type": "Script", "path": "scripts/move_to.lua"}
        ]
      },
      {
        "type": "Script",
        "path": "scripts/idle.lua",
        "decorators": [
          {"type": "ForceSuccess"}
        ]
      }
    ]
  }
}
```

### 目录模式示例

将上面的 JSON 拆分为目录文件，效果等价：

```lua
local status = bt.run("trees/ai_main")
```

```
trees/ai_main/
├── root.json
├── combat.json
└── patrol.json
```

**root.json:**
```json
{
  "type": "Selector",
  "name": "ai_root",
  "decorators": [
    {"type": "BlackboardCondition", "key": "alive", "operator": "is_set", "abort": "Self"}
  ],
  "children": [
    {"type": "Subtree", "subtree": "combat"},
    {"type": "Subtree", "subtree": "patrol"},
    {
      "type": "Script",
      "path": "scripts/idle.lua",
      "decorators": [{"type": "ForceSuccess"}]
    }
  ]
}
```

**combat.json:**
```json
{
  "type": "Sequence",
  "name": "combat",
  "decorators": [
    {"type": "BlackboardCondition", "key": "has_target", "operator": "is_set"}
  ],
  "children": [
    {"type": "Script", "path": "scripts/aim.lua", "name": "aim"},
    {"type": "Script", "path": "scripts/attack.lua", "name": "attack"}
  ]
}
```

**patrol.json:**
```json
{
  "type": "Sequence",
  "name": "patrol",
  "children": [
    {"type": "Script", "path": "scripts/find_point.lua"},
    {"type": "Script", "path": "scripts/move_to.lua"}
  ]
}
```

---

## 6. 节点类型汇总

| 类型 | 分类 | 必填字段 | 子节点 | 说明 |
|------|------|---------|--------|------|
| `Selector` | 复合 | `type` | `children` | OR 逻辑，任一成功即成功 |
| `Sequence` | 复合 | `type` | `children` | AND 逻辑，全部成功才成功 |
| `Parallel` | 复合 | `type` | `children` | 并行执行，策略控制结果 |
| `Script` | 叶子 | `type`, `path` | 无 | 执行 Lua 脚本 |
| `Subtree` | 叶子 | `type`, `subtree` | 无 | 引用 subtrees 中定义的子树 |
| `BlackboardCondition` | 装饰器 | `type`, `key` | N/A | 黑板条件判断 |
| `Inverter` | 装饰器 | `type` | N/A | 反转结果 |
| `ForceSuccess` | 装饰器 | `type` | N/A | 强制成功 |
