# 行为树 (bt 模块)

UE4/5 风格行为树：**Lua table 描述结构**、Lua 脚本承载逻辑、黑板做节点间数据共享、传感器异步感知、装饰器做条件中止。

```lua
local bt = require('bt')
```

| 函数 | 说明 |
|------|------|
| `bt.run(opts)` | 加载并运行行为树直到完成或超时（协程异步，yield 挂起、完成后恢复） |
| `bt.notify(event, data)` | 发送事件到事件队列（下个 tick 写入黑板 `_event_{name}`） |
| `bt.get_status()` | 状态：`"idle"` / `"running"` / `"success"` / `"failure"` |

## 核心机制一览

先用一个微型示例把全部核心件一次串起来：**树拓扑**（`tree`，Lua table）描述结构、**Script**（叶子）用 `Tick` 返回状态驱动流转、**Sensor**（传感器）把感知结果写黑板、**BlackboardCondition**（装饰器）读黑板做条件门。

场景：敌人进入 10 米内则攻击，否则巡逻。

```lua
local bt = require('bt')

bt.run({
    tree = {
        type = 'Selector',                 -- ① 根节点（复合 OR）：依次尝试，首个成功即停
        sensors = { 'enemy_dist' },        -- ② 传感器挂在根上 → 根总在活跃路径，传感器常驻
        children = {
            {   type = 'Script',           -- ④ 动作 A：攻击
                path = 'scripts/attack.lua',
                decorators = {             -- ③ 条件门：bb.enemy_dist < 10 才放行
                    { type = 'BlackboardCondition', key = 'enemy_dist',
                      operator = 'less_than', value = 10 },
                },
            },
            { type = 'Script', path = 'scripts/patrol.lua' },  -- 动作 B：否则巡逻
        },
    },
    sensors = {
        enemy_dist = { interval = 100, path = 'sensors/enemy_dist.lua' },
    },
})
```

Sensor 脚本——`Tick` 返回值写入黑板：

```lua
-- sensors/enemy_dist.lua
local M = {}
function M:Tick()
    return distance_to_nearest()           -- 数值 → bb.enemy_dist（每 100ms 刷新）
end
return M
```

Script 脚本——`Tick` 返回状态字符串驱动节点：

```lua
-- scripts/attack.lua
local M = {}
function M:Tick()
    swing()
    return "success"                       -- "success" / "failure" / "running"
end
return M
```

**每个 tick 的数据流**：① Sensor 测距 → 写 `bb.enemy_dist`；② Selector 试 `attack`，`BlackboardCondition` 读 `bb.enemy_dist`——`< 10` 放行 `attack`，否则**跳过** `attack`、落到 `patrol`。

> **核心区别**：Script 与 Sensor 的脚本是**同一种 table 回调**（`return M` + `Enter/Tick/Exit` 冒号语法）。差别只在 `Tick` 的产出——**Script 返回状态字符串**（`"success"`/`"failure"`/`"running"`）控制树如何流转；**Sensor 返回任意值**写入黑板供条件读取。后续各节是对这些件的展开。

## bt.run(opts)

一站式入口，依次完成：从 Lua table 构建 Node 树 → 初始化 Script → 初始化 Sensor → 激活初始路径 Sensor → 进入 tick 循环（直到完成、达到 `max_step` 或 `timeout`）。重复调用会先停止并清理之前的树。

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `tree` | `table` | **是** | 根节点定义（行为树拓扑，见下） |
| `subtrees` | `table` | 否 | 子树定义：`{名字 = 节点定义, ...}` |
| `sensors` | `table` | 否 | 传感器配置：`{名字 = {interval, args, path}, ...}` |
| `max_step` | `integer` | 否 | 最大 tick 步数，未设置则无限 |
| `timeout` | `integer` | 否 | 超时（毫秒），未设置则不超时 |
| `interval` | `integer` | 否 | tick 间隔（毫秒），未设置则连续 |

返回 `status, err`：`status` 为 `"success"` / `"failure"` / `"timeout"`，出错为 `nil`；`err` 为错误信息，成功为 `nil`。

```lua
local status, err = bt.run({
    tree = {
        type = 'Sequence',
        children = {
            { type = 'Script', path = 'scripts/attack.lua', args = { target = 'enemy' } },
            { type = 'Subtree', subtree = 'combat' },
        },
        sensors = { 'hp' },
    },
    subtrees = { combat = { type = 'Sequence', children = { ... } } },
    sensors = { hp = { interval = 100, path = 'sensors/hp.lua' } },
    max_step = 100, timeout = 5000, interval = 100,  -- 均可选
})
if not status then error("bt.run failed: " .. err)
elseif status == "timeout" then print("timed out")
else print("finished:", status) end
```

## 脚本加载

Script 节点和传感器的 **Lua 脚本仍由 `path` 指定**，由运行时 `CodeProvider` 加载（与主脚本 `require()` / `loadfile()` 共用同一加载器）。`path` 相对 `CodeProvider` 的搜索路径解析——请将 `scripts/`、`sensors/` 等脚本目录纳入 `CodeProvider` 搜索路径。

**`bt.run` 不再有 `project_path`/目录加载**：树拓扑由 `tree`/`subtrees` table 直接提供，不经文件或 JSON。

## 节点速查表

节点用 Lua table 表达。每个节点必须有 `type`，通用可选字段：`name`、`decorators`（装饰器数组）、`sensors`（传感器名数组）。

| 类型 | 分类 | 必填字段 | 子节点 | 说明 |
|------|------|---------|--------|------|
| `Selector` | 复合 | `type` | `children` | OR 逻辑，任一成功即成功 |
| `Sequence` | 复合 | `type` | `children` | AND 逻辑，全部成功才成功 |
| `Parallel` | 复合 | `type` | `children` | 并行，策略控制结果 |
| `RandomSelector` | 复合 | `type` | `children` | 随机顺序 OR |
| `RandomSequence` | 复合 | `type` | `children` | 随机顺序 AND |
| `Script` | 叶子 | `type`,`path` | 无 | 执行 Lua 脚本，`args` 传参 |
| `Subtree` | 叶子 | `type`,`subtree` | 无 | 引用 subtrees 子树 |
| `Wait` | 叶子 | `type` | 无 | 等待指定毫秒数 |
| `Repeat` | 包装 | `type` | `children[1]` | 重复执行子节点 |
| `RetryUntilSuccessful` | 包装 | `type` | `children[1]` | 失败时重试 |
| `BlackboardCondition` | 装饰器 | `type`,`key` | — | 黑板条件判断 |
| `Inverter` | 装饰器 | `type` | — | 反转结果 |
| `ForceSuccess` | 装饰器 | `type` | — | 强制成功 |
| `ForceFailure` | 装饰器 | `type` | — | 强制失败 |

## 运行机制（每个 tick）

1. **处理事件** — `bt.notify()` 推入的事件写入黑板（`_event_{name}`）
2. **执行传感器** — 到期且激活的 Sensor 运行，结果写黑板
3. **评估装饰器** — 条件从满足变为不满足时触发节点中止（`OnAborted`）
4. **Tick 树** — 从根节点执行行为树
5. **更新传感器激活** — 按活跃路径激活/停用 Sensor
6. **树完成时重置** — 返回 `success`/`failure` 时停用所有 Sensor 并重置树

## 节点详解

### 复合节点 (Composite)

通过 `children` 数组配置，Running 时记忆当前位置。

| 节点 | 逻辑 |
|------|------|
| `Selector` | 依次 tick，首个 Success 即 Success，全 Failure 才 Failure |
| `Sequence` | 依次 tick，首个 Failure 即 Failure，全 Success 才 Success |
| `RandomSelector` | 同 Selector，每次 Reset 后随机排列子节点顺序 |
| `RandomSequence` | 同 Sequence，每次 Reset 后随机排列子节点顺序 |

`Parallel` 同时执行所有子节点，策略控制判定：

| 字段 | 可选值 | 默认 | 含义 |
|------|--------|------|------|
| `success_policy` | `RequireAll` / `RequireOne` | `RequireAll` | 全部成功才成功 / 任一成功即成功 |
| `failure_policy` | `RequireAll` / `RequireOne` | `RequireOne` | 全部失败才失败 / 任一失败即失败 |

```lua
{ type = 'Selector', name = '可选名称', children = { ... } }
```

### 叶子节点 (Leaf)

#### Script — 脚本节点

```lua
{ type = 'Script', path = 'scripts/attack.lua', name = '可选', args = { target = 'enemy', damage = 100 } }
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `path` | **是** | Lua 脚本路径（相对 `CodeProvider` 搜索路径） |
| `name` | 否 | 节点名，默认等于 path |
| `args` | 否 | 传给 `Enter` 的参数对象（值支持 `bool`/`int`/`double`/`string`） |

**脚本格式**：必须 **return 一个 table**，用冒号语法定义回调，`self` 是该 table（存跨 tick 状态）：

```lua
-- scripts/attack.lua
local M = {}
function M:Enter(args) self.target = args.target end   -- 进入活跃时一次
function M:Tick()                                       -- 必需，协程内每次 tick
  return self.target and "success" or "failure"
end
function M:Exit(reason) end   -- reason: success|failure|aborted|reset
function M:Abort() end        -- 被中止时，在 Exit 之前
return M
```

| 回调 | 签名 | 必需 | 可 yield | 说明 |
|------|------|------|---------|------|
| `Enter` | `self:Enter(args)` | 否 | 否 | 进入活跃状态时一次 |
| `Tick` | `self:Tick()` | **是** | **是** | 每次 tick，返回状态字符串 |
| `Exit` | `self:Exit(reason)` | 否 | 否 | 离开活跃状态时 |
| `Abort` | `self:Abort()` | 否 | 否 | 强制中止时（Exit 之前） |

未返回 table 或缺 `Tick` → 加载失败，tick 时返回 Failure。**Tick 返回**：`"success"` / `"failure"`（进 Exit）/ `"running"`（保持活跃，下次 tick 直接 Tick，不再 Enter）；返回 nil 或无法识别的字符串视为 Failure。`Tick` 内可用所有异步 API（`sleep`/`await`/`http.get` 等），异步期间节点挂起 Running，完成后自动恢复。

**生命周期**：首次 tick → `Enter(args)` → `Tick()` →（success/failure: `Exit(reason)` 完成 / running: 下次 tick 直接 `Tick()`）；被装饰器中止 → `Abort()` → `Exit("aborted")`；被重置 → `Exit("reset")`。

**黑板访问**：`require('blackboard')` 后 `bb.get(key)` / `bb.set(key, value)`，值支持 `nil`/`boolean`/`integer`(int64)/`double`/`string`。

#### Subtree — 子树节点

```lua
{ type = 'Subtree', subtree = 'combat', name = '可选' }
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `subtree` | **是** | `subtrees` table 中定义的子树名 |
| `name` | 否 | 默认等于 subtree 名 |

支持 `decorators`/`sensors`，子树定义内可引用其他子树（可嵌套）。

#### Wait — 等待节点

```lua
{ type = 'Wait', ms = 500 }   -- ms 默认 1000
```

首次 tick 记起始时间，到达前返回 Running，到达后返回 Success。

### 包装节点 (Wrapper)

`children` 数组，仅取第一个子节点；子节点 Running 时返回 Running。

| 节点 | 字段 | 行为 |
|------|------|------|
| `Repeat` | `count`（默认 `-1` 无限） | 有限：子成功计数+1，达 count→Success，子失败→Failure；无限：子成功重置继续，子失败→Failure |
| `RetryUntilSuccessful` | `attempts`（默认 `-1` 无限） | 子成功→Success；失败重置重试，超 attempts→Failure |

### 装饰器 (Decorators)

`decorators` 数组，附加在任意节点上，节点 tick 前评估条件。

**BlackboardCondition** — 根据黑板值决定节点是否执行：

| 字段 | 必填 | 说明 |
|------|------|------|
| `key` | **是** | 黑板键名 |
| `operator` | 否 | 默认 `is_set` |
| `value` | 否 | 期望值（部分 operator 需要） |
| `abort` | 否 | 中断模式，默认 `None` |

`operator`：`is_set` / `is_not_set`（无需 value）/ `equals` / `not_equals` / `greater_than` / `less_than`（需 value）。`value` 支持 `bool`/`int64`/`double`/`string`，类型不匹配即 false。

`abort`（UE4/5 观察者中止）：`None`（默认）/ `Self`（中断自身执行中的子树）/ `LowerPriority`（中断右侧低优先级节点）/ `Both`。

```lua
decorators = {
    { type = 'BlackboardCondition', key = 'hp', operator = 'greater_than', value = 50, abort = 'Self' },
    { type = 'Inverter' },
}
```

**结果修饰**（均接受可选 `abort`）：`Inverter` 反转成功/失败；`ForceSuccess` 始终成功；`ForceFailure` 始终失败。

## 传感器 (Sensors)

按需运行的异步感知模块，声明在节点上。节点进入活跃路径时激活、按 `interval` 周期执行、离开路径时停用。**用途**：条件依赖异步数据（DOM 查询、网络请求）时，由传感器定期把结果写入黑板，`BlackboardCondition` 同步读取缓存值。

### 配置方式

传感器的**脚本与调度配置集中在 `bt.run` 的 `sensors` 参数**（一个 `{名字 = 配置}` 的 table）；节点通过 `sensors = {'名字'}` 数组按名引用。

```lua
bt.run({
    tree = {
        type = 'Script', path = 'scripts/click.lua',
        sensors = { 'login_visible' },          -- 引用名字
    },
    sensors = {
        login_visible = { interval = 100, path = 'sensors/element_visible.lua', args = { selector = '#login' } },
        hp = { interval = 50, path = 'sensors/hp.lua' },
    },
})
```

### 配置字段

| 字段 | 必填 | 类型 | 说明 |
|------|------|------|------|
| `path` | **是** | string | Lua 脚本路径（相对 `CodeProvider` 搜索路径） |
| `interval` | 否 | integer | tick 间隔（毫秒），默认 `100` |
| `args` | 否 | object | 传给 `Enter` 的参数（值支持 `bool`/`int`/`double`/`string`） |

传感器名同时作为**黑板键名**（Tick 返回值写入 `bb[名字]`）。

### 脚本格式

脚本 return 一个 table，用冒号语法定义回调；`self` 就是这个 table，可自由存放跨 tick 的状态：

```lua
-- sensors/element_visible.lua
local M = {}
function M:Enter(args) end                  -- 激活时一次（同步，不可 yield）
function M:Tick()                           -- 必需，按 interval 周期调用（协程，可 yield）
  return find("#login-btn") ~= nil          -- 第一个返回值 → bb[名字]
end
function M:Exit() end                       -- 停用时一次（同步，不可 yield）
return M
```

| 回调 | 必需 | 可 yield | 说明 |
|------|------|---------|------|
| `Enter(args)` | 否 | 否 | 激活时一次，`args` 为配置中的 `args` table |
| `Tick()` | **是** | **是** | 按 interval 周期，第一个返回值写入黑板 |
| `Exit()` | 否 | 否 | 停用时一次 |

脚本里可用 `require('blackboard')` 直接读写黑板：`bb.get(key)` 取值、`bb.set(key, value)` 写值。

#### 示例 1：`self` 累计跨 tick 状态

`self` 在传感器存活期间持久存在，可缓存计数、上一次的结果等：

```lua
-- sensors/nearby_count.lua
local M = {}
function M:Enter()
  self.count = 0                    -- 初始化（仅激活时一次）
end
function M:Tick()
  self.count = (self.count or 0) + 1
  return self.count                 -- → bb.nearby_count
end
return M
```

#### 示例 2：`blackboard` 模块 + Enter/Exit 生命周期

常在 Enter/Exit 里打生命周期标记，供树中其它节点读取：

```lua
-- sensors/radar.lua
local M = {}
function M:Enter()
  local bb = require('blackboard')
  bb.set('radar_active', true)      -- 激活 → 置位
end
function M:Tick()
  return detect_targets()           -- → bb.radar
end
function M:Exit()
  local bb = require('blackboard')
  bb.set('radar_active', false)     -- 停用 → 清位
end
return M
```

#### 示例 3：`args` 从配置传入 Enter

配置里的 `args` 会原样作为参数传给 `Enter`：

```lua
-- bt.run 配置：
--   sensors = { threat = { interval = 100, path = 'sensors/threat.lua',
--                          args = { range = 50, faction = 'enemy' } } }
local M = {}
function M:Enter(args)
  self.range   = args.range         -- 50
  self.faction = args.faction       -- 'enemy'
end
function M:Tick()
  return count_units(self.faction, self.range)   -- → bb.threat
end
return M
```

#### 异步 Tick

`Tick` 运行在协程中，可 `coroutine.yield()` 等待异步操作（`http`、`async_io` 等）完成后再 return。这正是传感器适合"条件依赖异步数据"的原因——把慢查询挪出主 tick 路径、按 `interval` 刷新缓存值，`BlackboardCondition` 始终同步读取缓存。

### 生命周期与 BlackboardCondition 配合

激活（节点进入活跃路径 root→…→leaf）→ 按 `interval` tick（可 yield）→ 停用（节点离开活跃路径，且无其他活跃节点共享该传感器）。典型链路：传感器周期写黑板，`BlackboardCondition` 在每次 tick 树前同步读黑板、决定是否放行或中止。

完整示例——附近有敌人时进攻。传感器返回**整数**数量，条件用比较运算符判断：

```lua
bt.run({
    tree = {
        type = 'Sequence', name = 'engage',
        sensors = { 'nearby_enemies' },          -- 节点活跃时该传感器周期运行
        children = {
            { type = 'Script', path = 'scripts/attack.lua',
              decorators = {
                  { type = 'BlackboardCondition', key = 'nearby_enemies',
                    operator = 'greater_than', value = 0, abort = 'Self' },
              },
            },
        },
    },
    sensors = {
        nearby_enemies = { interval = 100, path = 'sensors/nearby_enemies.lua' },
    },
})
```

```lua
-- sensors/nearby_enemies.lua
local M = {}
function M:Tick()
    return #scan_enemies()                       -- 整数数量 → bb.nearby_enemies
end
return M
```

工作流：① `engage` 进入活跃路径 → `nearby_enemies` 激活，每 100ms 把敌人数量写入 `bb.nearby_enemies`；② `BlackboardCondition` 每次 tick 前读该键，`> 0` 则放行 `attack.lua`、否则中止。

> **`is_set` 的坑**：`is_set` 只判断键**是否存在**，不看值真假；而传感器 `Tick` 的返回值会**无条件**写入黑板（返回 `false` 也会把键置上）。所以布尔型条件不要用 `is_set` 配合返回 `true/false` 的传感器——条件一旦首次满足就会一直满足。正确写法二选一：
> - **数值 + 比较运算符**（推荐，见上例）：`operator = 'greater_than', value = 0`
> - **布尔 + `equals`**：`operator = 'equals', value = true`
>
> 若一定要用 `is_set`，可让传感器在不满足时显式 `return nil`——写 nil 会清除该键，`is_set` 随之变假。
>
> **数值类型要一致**：比较/相等运算符要求两侧类型相同（`int64` ↔ `int64` 或 `double` ↔ `double`），否则判定为 type mismatch 直接返回 false。即 `value` 写整数字面量（`0`、`30`）则传感器须返回整数；写小数（`30.0`）则须返回浮点。
>
> 可用运算符：`equals`、`not_equals`、`greater_than`、`less_than`、`greater_equal`、`less_equal`（后四个仅适用于数值）、`is_set`、`is_not_set`。比较/相等运算符通过 `value` 字段提供期望值。

## 完整示例

```lua
local bt = require('bt')

local status, err = bt.run({
    tree = {
        type = 'Selector', name = 'ai_root',
        decorators = {
            { type = 'BlackboardCondition', key = 'alive', operator = 'is_set', abort = 'Self' },
        },
        children = {
            { type = 'Subtree', subtree = 'combat' },
            { type = 'Subtree', subtree = 'patrol' },
            { type = 'Script', path = 'scripts/idle.lua', decorators = { { type = 'ForceSuccess' } } },
        },
    },
    subtrees = {
        combat = {
            type = 'Sequence', name = 'combat',
            decorators = { { type = 'BlackboardCondition', key = 'has_target', operator = 'is_set' } },
            children = {
                { type = 'Script', path = 'scripts/aim.lua', name = 'aim' },
                { type = 'Script', path = 'scripts/attack.lua', name = 'attack' },
            },
        },
        patrol = {
            type = 'Sequence', name = 'patrol',
            children = {
                { type = 'Script', path = 'scripts/find_point.lua' },
                { type = 'Script', path = 'scripts/move_to.lua' },
            },
        },
    },
})
```
