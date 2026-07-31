# 行为树 (bt 模块)

UE4/5 风格行为树：**JSON 描述结构**、Lua 脚本承载逻辑、黑板做节点间数据共享、传感器异步感知、装饰器做条件中止。

```lua
local bt = require('bt')
```

## API

| 函数 | 说明 |
|------|------|
| `bt.init(opts)` | 加载 JSON 定义、构建树、初始化脚本/传感器、激活（协程 yield）。返回 `"ready"` 或 `nil, err` |
| `bt.exec(opts)` | 启动后台 tick 并**阻塞**到本次 run 结束（协程 yield）；返回 `"success"`/`"failure"`/`"timeout"`/`"stopped"` 或 `nil, err` |
| `bt.goto_path(names)` | 跳到指定节点名路径（如 `{"combat","attack"}`），重置树让叶子重新 `Enter` |
| `bt.stop()` | 停止并清理当前树 |
| `bt.notify(event, data)` | 发事件到队列（下个 tick 写入黑板 `_event_{name}`） |
| `bt.get_status()` | `"idle"`/`"ready"`/`"running"`/`"paused"`/`"success"`/`"failure"` |
| `bt.dump_paths()` / `bt.path_report()` | 路径数据/报告（见[路径记录](#路径记录)） |

状态机：`idle →(init) ready →(exec) running ↔ paused → success/failure →(stop) idle`。`init` 在 running/paused 时报错（先 `stop`）；终态再 `exec` 报错（重新 `init`）；`ready`/`paused` 之外的状态调 `exec` 也报错。

> **暂停/恢复**是宿主级（网络 `kPause`/`kResume` → `BernardXEngine::Pause/Resume`），**不由 Lua 调用**。暂停时 bt 后台 tick 与 `sleep` 停推进（树状态保留），`http`/`async_io` 照常；恢复后从原位置继续。

### bt.init / bt.exec 参数

`bt.init`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `root` | string | **是** | 根节点 JSON 文件路径（见[路径解析](#路径解析)） |
| `sensor_defs` | string | 否 | 传感器定义 JSON 文件路径 |
| `trace_paths` | bool | 否 | 采集路径数据（默认 `true`） |

`bt.exec`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `interval` | integer | **是** | tick 间隔（毫秒），> 0 |
| `max_step` | integer | 否 | 最大 tick 步数，未设则无限 |
| `timeout` | integer | 否 | 超时（毫秒），未设则不超时 |

## 路径解析

`root`、`sensor_defs`、Subtree 节点的 `path` 都是字符串路径，统一解析：

- `@<相对路径>` → **项目资源**：`ResourceProvider` 在资源根目录（`<项目>/res`）下加载。
- `<绝对路径>` → 直接读文件系统。

Script 与传感器的 **Lua 脚本**仍由 `path` 指定，经 `CodeProvider` 加载（与主脚本 `require()` 共用加载器）——请把 `scripts/`、`sensors/` 等目录纳入 `CodeProvider` 搜索路径。两套加载器各管一摊：JSON 拓扑走 `ResourceProvider`，脚本走 `CodeProvider`。

## 节点结构

所有节点都是 JSON 对象，`type` 必填，其余字段按节点类型选用：

```json
{
  "type": "Sequence",
  "name": "可选",
  "children": [],
  "decorators": [],
  "sensors": ["名字"],
  "params": {},
  "path": ""
}
```

| 字段 | 适用 | 说明 |
|------|------|------|
| `type` | 全部 | 节点类型（必填） |
| `name` | 全部 | 节点名；Script/Subtree 默认等于 `path` |
| `children` | 复合/包装 | 子节点数组（包装类仅取第一个） |
| `decorators` | 全部 | 装饰器数组（见[装饰器](#装饰器)） |
| `sensors` | 全部 | 传感器名数组（见[传感器](#传感器)） |
| `params` | 各类型 | 类型相关参数：Script→`Enter` 参数、Wait→`ms`、Repeat→`count`、Retry→`attempts`、Parallel→`success_policy`/`failure_policy` |
| `path` | Script / Subtree | Script 的 Lua 脚本路径 / Subtree 的子树 JSON 路径 |
| `description` | 全部 | 备注（仅文档/调试） |

### 节点类型

| 类型 | 分类 | 特有字段 | 说明 |
|------|------|---------|------|
| `Selector` | 复合 | — | 依次 tick，首个 Success 即 Success（OR） |
| `Sequence` | 复合 | — | 依次 tick，首个 Failure 即 Failure（AND） |
| `ResumeSequence` | 复合 | — | 首次/恢复时找首个 decorator 成立的子节点作为入口，之后按序推进（见下） |
| `Parallel` | 复合 | `params` | 并行所有子节点，策略控制结果 |
| `RandomSelector` / `RandomSequence` | 复合 | — | 同 Selector/Sequence，每次 Reset 后随机排列子节点顺序 |
| `Script` | 叶子 | `path`, `params` | 执行 Lua 脚本 |
| `Subtree` | 叶子 | `path` | 加载 `path` 指向的子树 JSON（递归） |
| `Wait` | 叶子 | `params` | 等待 `params.ms` 毫秒（默认 1000） |
| `Repeat` | 包装 | `params` | 重复执行子节点（`params.count`，默认 -1 无限） |
| `RetryUntilSuccessful` | 包装 | `params` | 失败重试（`params.attempts`，默认 -1 无限） |

**Parallel 策略**（`params.success_policy` / `params.failure_policy`，均 `RequireAll`/`RequireOne`）：默认 `RequireAll` / `RequireOne`。

**ResumeSequence** —— 首次进入（或被上层打断后恢复）从上到下找首个 decorator 成立的子节点作为入口，之后按 Sequence 记忆位置推进；只有 `Reset`/被中止才重新找入口。适合"按步骤执行、被打断后自动回到当前步骤"——已完成的步骤因描述不再成立被跳过：

```json
{ "type":"ResumeSequence", "children":[
  { "type":"Script", "path":"step1.lua",
    "decorators":[{"type":"BlackboardCondition","key":"step","operator":"less_than","value":1}] },
  { "type":"Script", "path":"step2.lua",
    "decorators":[{"type":"BlackboardCondition","key":"step","operator":"less_than","value":2}] }
]}
```

## 脚本 (Script)

`path` 指向的 Lua 脚本必须 **return 一个 table**，用冒号语法定义回调，`self` 就是该 table（可存跨 tick 状态）：

```lua
-- scripts/attack.lua
local M = {}
function M:Enter(params) self.target = params.target end          -- 进入活跃时一次
function M:Tick() return self.target and "success" or "failure" end -- 必需，每次 tick
function M:Exit(reason) end    -- reason: success|failure|aborted|reset
function M:Abort() end         -- 被中止时（Exit 之前）
return M
```

| 回调 | 必需 | 可 yield | 说明 |
|------|------|---------|------|
| `Enter(params)` | 否 | 否 | 进入活跃时一次，`params` 为节点 `params` |
| `Tick()` | **是** | **是** | 每次 tick，返回 `"success"`/`"failure"`/`"running"`（nil/未知字符串 = Failure） |
| `Exit(reason)` | 否 | 否 | 离开活跃 |
| `Abort()` | 否 | 否 | 被中止时（Exit 之前） |

`Tick` 内可用所有异步 API（`sleep`/`await`/`http.request` 等），异步期间节点挂起 Running、完成后自动恢复。黑板访问：`require('blackboard')` 后 `bb.get(key)` / `bb.set(key, value)`，值支持 `nil`/`bool`/`int64`/`double`/`string`。

## 装饰器

`decorators` 数组，附加在任意节点上，节点 tick 前评估条件。

**BlackboardCondition** —— 读黑板值决定节点是否执行：

| 字段 | 必填 | 说明 |
|------|------|------|
| `key` | 是 | 黑板键名 |
| `operator` | 否 | 默认 `is_set`；`equals`/`not_equals`/`greater_than`/`less_than`/`greater_equal`/`less_equal` 需配 `value`（后四个仅数值） |
| `value` | 否 | 期望值（`bool`/`int`/`double`/`string`，类型不匹配即 false） |
| `abort` | 否 | 观察者中止：`None`（默认）/`Self`/`LowerPriority`/`Both` |

**结果修饰**（均接受可选 `abort`）：`Inverter`（反转成功/失败）、`ForceSuccess`、`ForceFailure`。

```json
"decorators":[
  {"type":"BlackboardCondition","key":"hp","operator":"greater_than","value":50,"abort":"Self"},
  {"type":"Inverter"}
]
```

## 传感器

异步感知模块。节点进入活跃路径时激活、按 `interval` 周期执行、离开路径时停用。**用途**：把异步数据（DOM 查询、网络请求）定期写入黑板，`BlackboardCondition` 同步读取缓存值。

定义集中在 `sensor_defs` JSON 文件（`bt.init` 的 `sensor_defs` 参数）：`{"名字":{"interval","path","params"}}`；节点通过 `"sensors":["名字"]` 按名引用。**传感器名同时是黑板键名**（`Tick` 返回值写入 `bb[名字]`）。

```json
{
  "hp":     { "interval": 50,  "path": "sensors/hp.lua" },
  "threat": { "interval": 100, "path": "sensors/threat.lua", "params": { "range": 50 } }
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `path` | 是 | Lua 脚本路径（经 `CodeProvider`） |
| `interval` | 否 | tick 间隔毫秒，默认 100 |
| `params` | 否 | 传给 `Enter` 的参数 |

脚本格式与 Script 相同（return table + 冒号回调），差别只在 `Tick` 的第一个返回值写入黑板：

```lua
-- sensors/nearby_enemies.lua
local M = {}
function M:Enter(params) self.range = params.range end     -- 可选，同步不可 yield
function M:Tick() return #scan_enemies(self.range) end      -- 必需，可 yield，→ bb.nearby_enemies
function M:Exit() end                                       -- 可选，同步不可 yield
return M
```

> **`is_set` 的坑**：传感器 `Tick` 返回值会**无条件**写黑板（返回 `false` 也会把键置上），而 `is_set` 只看键是否存在——布尔条件一旦首次满足就会一直满足。布尔场景改用 `operator:"equals","value":true`，或让传感器不满足时 `return nil`（写 nil 清键，`is_set` 随之变假）。**数值类型要一致**：比较/相等运算符要求两侧类型相同（`int`↔`int`、`double`↔`double`），否则 type mismatch 直接返回 false。

## 完整示例

`bt.init({ root = "@bt/ai_root.json" })`，`@bt/ai_root.json`（引用两份子树 JSON）：

```json
{
  "type": "Selector", "name": "ai_root",
  "decorators": [ {"type":"BlackboardCondition","key":"alive","operator":"is_set","abort":"Self"} ],
  "children": [
    { "type": "Subtree", "path": "@bt/combat.json" },
    { "type": "Subtree", "path": "@bt/patrol.json" },
    { "type": "Script", "path": "scripts/idle.lua", "decorators": [ {"type":"ForceSuccess"} ] }
  ]
}
```

`@bt/combat.json`（子树 JSON 即该子树的根节点对象，可再引用其它子树，递归）：

```json
{
  "type": "Sequence", "name": "combat",
  "decorators": [ {"type":"BlackboardCondition","key":"has_target","operator":"is_set"} ],
  "children": [
    { "type": "Script", "path": "scripts/aim.lua", "name": "aim" },
    { "type": "Script", "path": "scripts/attack.lua", "name": "attack" }
  ]
}
```

## 路径记录

每个 tick 记录**活跃路径**（root → 当前叶子），相同路径合并计数；事后回溯"树走了哪条路、多久、为何切换"，定位卡死或异常切换。`trace_paths` 默认 `true`（在 `bt.init` 关闭则返回空）。报告**不自动打印**，由你决定何时输出。

- **`bt.dump_paths()`** — 返回 table：`total_ticks`、`path_occurrences`、`terminal`、`has_terminal`、`terminal_note`、`tracing`、`paths[]`（`sig_ids`/`names`/`count`/`first_tick`/`last_tick`/`leaf_status`/`root_status`/`is_terminal`）、`nodes[]`、`switches[]`、`dec_flips[]`。
- **`bt.path_report()`** — 三视图报告字符串：**A 路径热度**（卡在哪、多久）、**B 树形热力图**（哪个分支热）、**C 切换时间线**（何时、为何切换）。

末态（成功/失败/超时/异常）自动标记；`paths` 中 `is_terminal=true` 的路径就是树最终停留处——排查卡死优先看它。`Parallel` 一个 tick 产出 N 条路径（每子节点一条），故 `∑路径count = path_occurrences ≥ total_ticks`，A 列表占比分母用 `path_occurrences`。

## 运行机制（每个 tick）

1. **处理事件** — `bt.notify()` 推入的事件写入黑板（`_event_{name}`）
2. **执行传感器** — 到期且激活的 Sensor 运行，结果写黑板
3. **评估装饰器** — 条件从满足变为不满足时触发节点中止（`OnAborted`）
4. **Tick 树** — 从根节点执行
5. **更新传感器激活** — 按活跃路径激活/停用 Sensor
6. **树完成时重置** — 返回 success/failure 时停用所有 Sensor 并重置树
