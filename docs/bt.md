# 行为树 (bt 模块)

UE4/5 风格行为树：**JSON 描述结构**、Lua 脚本承载逻辑、黑板做节点间数据共享、**轮询式条件（`NodeCondition`）做判断**、`Pipeline` 做扫描定起点的顺序自动化流水线。

```lua
local bt = require('bt')
```

## API

| 函数 | 说明 |
|------|------|
| `bt.init(opts)` | 加载根 JSON、构建树、初始化脚本与条件、就绪（协程 yield）。返回 `"ready"` 或 `nil, err` |
| `bt.exec(opts)` | 启动后台 tick 并**阻塞**到本次 run 结束（协程 yield）；返回 `"success"`/`"failure"`/`"timeout"`/`"stopped"` 或 `nil, err` |
| `bt.goto_path(names)` | 跳到指定节点名路径（如 `{"combat","attack"}`），重置树让叶子重新 `Enter` |
| `bt.stop()` | 停止并清理当前树 |
| `bt.notify(event, data)` | 发事件到队列（下个 tick 写入黑板 `_event_{name}`） |
| `bt.get_status()` | `"idle"`/`"ready"`/`"running"`/`"paused"`/`"success"`/`"failure"` |
| `bt.dump_paths()` / `bt.path_report()` | 路径数据/报告（见[路径记录](#路径记录)） |

状态机：`idle →(init) ready →(exec) running ↔ paused → success/failure →(stop) idle`。`init` 在 running/paused 时报错（先 `stop`）；终态再 `exec` 报错（重新 `init`）；`ready`/`paused` 之外的状态调 `exec` 也报错。

> **暂停/恢复**是宿主级（`LuaRuntime::Pause/Resume`，由宿主调用），**不由 Lua 调用**。暂停时 bt 后台 tick 与 `sleep` 停推进（树状态保留），`http`/`async` 照常；恢复后从原位置继续。

### bt.init / bt.exec 参数

`bt.init`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `root` | string | **是** | 根节点 JSON 文件路径（见[路径解析](#路径解析)） |
| `trace_paths` | bool | 否 | 采集路径数据（默认 `true`） |

`bt.exec`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `interval` | integer | 否 | tick 间隔（毫秒），默认 50，需 > 0 |
| `max_step` | integer | 否 | 最大 tick 步数，未设则无限 |
| `timeout` | integer | 否 | 超时（毫秒），未设则不超时 |

## 路径解析

`root` 与 Subtree 节点的 `source` 都是字符串路径，统一解析：

- `res://<相对路径>` → **项目资源**：`ResourceProvider` 在资源根目录（`<项目>/res`）下加载。`<相对路径>` 原样传入（含 `.json` 扩展名，如 `res://bt/ai_root.json` → 加载 `bt/ai_root.json`）。
- `<绝对路径>` → 直接读文件系统。

Script 与条件的 **Lua 脚本**仍由 `source` 指定，经 `CodeProvider` 加载（与主脚本 `require()` 共用加载器）——请把 `scripts/` 等目录纳入 `CodeProvider` 搜索路径。两套加载器各管一摊：JSON 拓扑走 `ResourceProvider`，脚本走 `CodeProvider`。

## 节点结构

所有节点都是 JSON 对象，`type` 必填，其余字段按节点类型选用：

```json
{
  "type": "Sequence",
  "name": "可选",
  "children": [],
  "condition": {},
  "params": {},
  "source": ""
}
```

| 字段 | 适用 | 说明 |
|------|------|------|
| `type` | 全部 | 节点类型（必填） |
| `name` | 全部 | 节点名；Script/Subtree 默认等于 `source` |
| `children` | 复合 | 子节点数组 |
| `child` | 包装 | 单个子节点（直接对象，非数组） |
| `condition` | 全部 | 可选的守卫条件（见[条件](#条件)）。**所有节点通用**：复合节点 tick 子节点前先门控（Failure 则跳过/失败）；条件对象上的 `abort` 字段（默认 `None`）再开启 UE4/5 风格的响应式中断（`Self`/`LowerPriority`/`Both`）。`Pipeline` 还额外在首次扫描时用它定起点。`Subtree` 的 `condition` 还可取特殊字符串 `"@child_condition"`——透传引用所嵌子树**根节点**自身的 condition（共享同一对象，让父复合在子树边界处门控一个定义在子树文件内的条件；`abort` 随子树根条件一起带过来；子树根无 condition 时为 no-op 并告警）。 |
| `params` | 各类型 | 类型相关参数：Script→`Enter` 参数、Wait→`ms`、Repeat→`count`、Retry→`attempts`、Parallel→`success_policy`/`failure_policy`、Subtree 透传给子树（见[子树参数透传](#子树参数透传)） |
| `source` | Script / Subtree | Script 的 Lua 脚本路径 / Subtree 的子树 JSON 路径 |
| `description` | 全部 | 备注（仅文档/调试） |

### 节点类型

| 类型 | 分类 | 特有字段 | 说明 |
|------|------|---------|------|
| `Selector` | 复合 | — | 依次 tick，首个 Success 即 Success（OR） |
| `Sequence` | 复合 | — | 依次 tick，首个 Failure 即 Failure（AND） |
| `Pipeline` | 复合 | 子节点 `condition`/`$timeout`/`$retry` | 扫描续传 + 每步等待超时/回退重试的自动化流水线（见下） |
| `Parallel` | 复合 | `params` | 并行所有子节点，策略控制结果 |
| `RandomSelector` / `RandomSequence` | 复合 | — | 同 Selector/Sequence，每次 Reset 后随机排列子节点顺序 |
| `Script` | 叶子 | `source`, `params` | 执行 Lua 脚本 |
| `Subtree` | 叶子 | `source`, `params` | 加载 `source` 指向的子树 JSON（递归）；`params` 透传给子树内节点（见[子树参数透传](#子树参数透传)）；`condition` 支持 `"@child_condition"` 透传子树根的条件（见上[节点结构](#节点结构)） |
| `Wait` | 叶子 | `params` | 等待 `params.ms` 毫秒（默认 1000） |
| `Repeat` | 包装 | `params`, `child` | 重复执行子节点（`params.count`，默认 -1 无限） |
| `RetryUntilSuccessful` | 包装 | `params`, `child` | 失败重试（`params.attempts`，默认 -1 无限） |
| `ForceSuccess` | 包装 | `child` | 执行子节点，结束态强制为 Success（Running 透传） |
| `ForceFailure` | 包装 | `child` | 执行子节点，结束态强制为 Failure（Running 透传） |
| `Inverter` | 包装 | `child` | 反转子节点结束态 Success↔Failure（Running 透传） |

**Parallel 策略**（`params.success_policy` / `params.failure_policy`，均 `RequireAll`/`RequireOne`）：默认 `RequireAll` / `RequireOne`。

### Pipeline —— 扫描定起点 + 顺序执行

为软件自动化的"判断页面 → 动作 → 判断是否到达目标页 → 动作"流程设计。**纯 tick 驱动的状态机**——内部不 sleep、不看墙钟，只记录状态（当前步、相位、已等待 tick 数、已用重试），每个 tick 根据状态决定动作。`children` 是有序步骤，每个子节点可带一个 `condition`（判断"我现在处于哪一步"）和两个 **Pipeline 边参数** `$timeout` / `$retry`（`$` 前缀标记，表明它们是管线边参数、节点自身忽略）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `condition` | object | 该步的守卫条件（见[条件](#条件)）；可选，无则视为已成立 |
| `$timeout` | int 或 `[lo,hi]` (tick) | 等待**本步** condition 成立的 tick 预算；数组形式表示闭区间内**均匀随机**（每次运行、每个步摇一个值，该步本次运行内固定）；0/缺省 = 无限等 |
| `$retry` | int 或 `[lo,hi]` | 等待本步 condition 超时后，回退重跑上一步动作的最大次数；同样支持 `[lo,hi]` 范围随机；0/缺省 = 不重试 |

执行：

- **首次 tick 扫描（断点续传）** — 自上而下找**首个** `condition` 成立（Success）或无 `condition` 的子节点作为起点；扫到 Running 则整条流水线 Running、下 tick 续扫；扫到 Failure 跳过该步；**全部不成立 → 从 step0 起等 condition0**（用 `$timeout`0）。
- **跑动作** — 起点选定（condition 已成立）后 tick 其动作；动作 Success → 进入下一步的等待；动作 Failure → Pipeline failure。
- **等下一步 condition（带超时+重试）** — 动作完成后，每 tick 求值下一步 condition：成立 → 跑该步动作；不成立 → 消耗一个等待 tick，达到 `$timeout` 即超时。超时后：若 `$retry` 还有余额，**回退一步**重新求值上一步 condition，成立则 Reset+重跑上一步动作、随后重新等下一步 condition（重置 tick 预算）；若上一步 condition 不成立 或 `$retry` 耗尽 → Pipeline failure。

当每个 `condition` 表示一个独立的页面/状态时，"首个成立"= 当前所处步骤——正好对应"落在中间页就从该步接着做"的断点续传；超时+回退重跑则覆盖"上一步动作再触发一次，好让下一步页面出现"的常见自动化重试。

```json
{ "type":"Pipeline", "children":[
  { "type":"Script", "source":"scripts/login.lua",
    "condition":{ "type":"Script", "source":"conds/on_login_page.lua" } },
  { "type":"Script", "source":"scripts/open_settings.lua",
    "condition":{ "type":"And", "children":[
        { "type":"Script", "source":"conds/logged_in.lua" },
        { "type":"Not", "child":{ "type":"Script", "source":"conds/on_login_page.lua" } } ] },
    "$timeout": 50, "$retry": 2 },
  { "type":"Script", "source":"scripts/toggle_switch.lua",
    "condition":{ "type":"Script", "source":"conds/on_settings_page.lua" },
    "$timeout": 50, "$retry": 2 }
]}
```

**范围随机**：`$timeout` / `$retry` 既可写整数（固定值），也可写两元素数组 `[lo, hi]`——表示在该闭区间内**均匀随机**取一个值。Pipeline 在**每次运行**（Reset 后）为每个步各摇一个值，该步本次运行内固定（等价于"随机出的固定值"，不会每次 tick 重新摇）。常用于避免多次运行/多实例的等待与重试完全雷同：

```json
{ "type":"Pipeline", "children":[
  { "type":"Script", "source":"scripts/login.lua",
    "condition":{ "type":"Script", "source":"conds/on_login_page.lua" } },
  { "type":"Script", "source":"scripts/open_settings.lua",
    "condition":{ "type":"Script", "source":"conds/on_settings_page.lua" },
    "$timeout": [40, 80], "$retry": [1, 3] }
]}
```

> 范围内若含 0，0 仍保留"无限等/不重试"语义；要表达"有界随机"建议下界 ≥1。`$timeout`/`$retry` 单元素数组 `[v]` 等价于标量 `v`，乱序数组 `[b,a]`(b>a) 会被归一化为 `[a,b]`。
>
> `$timeout` 单位是 **tick 数**，实际墙钟时长 = tick 数 × `bt.exec` 的 `interval_ms`。`$timeout`/`$retry` 是边参数（描述进入该步的转移），节点自身（Script 等）不读取它们，Pipeline 在解析时单独取。纯顺序无起点判断/无需等待重试用 `Sequence`。

## 条件

`condition` 是一个 JSON 对象（本身也是一棵小树），挂在任意节点上，在被需要时**同步轮询** `Tick` 得出三态：**Success=成立 / Failure=不成立 / Running=判断中（异步 yield）**。

| 类型 | 字段 | 说明 |
|------|------|------|
| `Script` | `source`, `params` | Lua 脚本判断（见下） |
| `And` | `children` | 全部成立才成立，短路到首个 Failure/Running |
| `Or` | `children` | 任一成立即成立，短路到首个 Success/Running |
| `Not` | `child` | 反转 Success/Failure，Running 透传 |

**Script 条件**的脚本格式与 Script 节点相同（return table + 冒号回调），`Tick` 返回值双模解析：

- 返回状态串 `"success"`/`"failure"`/`"running"` → 对应三态；
- 返回其它值按 Lua 真值：**仅 `false`/`nil` 为 Failure，其余（含 `0`、空串）为 Success**。

```lua
-- conds/on_login_page.lua
local M = {}
function M:Tick()
  return find_element("login_button") ~= nil   -- 找到=true(成立)，没找到=nil(不成立)
end
return M
```

`Enter`（首次求值前一次，可 yield）、`Exit`（`Reset` 时）可选，便于复用带状态/异步的识别脚本。条件对象支持 `coroutine.yield`/`sleep`/`await`——返回 Running 期间挂起，下次扫描继续。

### 守卫、中断与 abort（UE4/5 风格）

节点的 `condition` 同时是**入口门控**和**响应式中断**源，行为由条件对象上的 `abort` 字段选择（默认 `None`）：

| `abort` | 入口门控 | 响应式中断 |
|---------|---------|-----------|
| `None`（默认） | 是：复合节点 tick 子节点前先求值 condition，Failure 则跳过（Selector）/失败（Sequence）/计失败（Parallel） | 否 |
| `Self` | 是 | **是**：节点运行期间持续监视；condition 转 Failure → `OnAborted` 中断自身、返回 Failure（父节点按其语义处理） |
| `LowerPriority` | 是 | **是**：condition 由 false 翻 true 时，抢占当前正在运行的**低优先级兄弟**分支（父复合中止该分支、回退到本节点） |
| `Both` | 是 | Self + LowerPriority |

> **异步条件的 stale-while-running**：condition 返回 `Running`（评估中，如 yield 中的图像查找）时，其结果取**上一次的终态**（首次求值前视为 Failure）。因此异步 condition 评估中途不会误判为失败、不会错误中断其下级动作；只有真正转为 Failure 才触发 Self 中断，只有真正 false→true 才触发 LowerPriority 抢占。

```json
{ "type":"Script", "source":"scripts/attack.lua",
  "condition":{ "type":"Script", "source":"conds/has_target.lua", "abort":"Self" } }
```

> `abort` 写在**顶层** condition 对象上（节点直接挂的那个）；组合条件（And/Or/Not）的子条件不带 `abort`。`Pipeline` 步骤的 condition 同样适用——`Self` 即"动作运行中页面丢失就中断该步"。

## 脚本 (Script)

`source` 指向的 Lua 脚本必须 **return 一个 table**，用冒号语法定义回调，`self` 就是该 table（可存跨 tick 状态）：

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

**`params` 支持标量和 table**：`params` 的值除 `nil`/`bool`/`int`/`double`/`string` 外，也可是 object 或 array——在 `Enter(params)` 里收到的是**真正的 Lua table**，可嵌套访问（`params.config.hp`）；数组按 Lua 惯例从 1 开始。table 在 `bt.init` 阶段于 `lua_State` 上构建并以 `LuaRef` 持有，生命周期随节点。

## 子树参数透传

`Subtree` 节点可带 `params`，其值通过 `{{key}}` 占位符透传给**子树 JSON 内任意字符串字段**——`params`、`source`、`name`、`condition`（脚本路径/参数）、嵌套 `Subtree` 节点等都可模板化。无需透传时不写 `params` 即可，子树按原样加载。

- **整值占位**（值刚好是 `"{{key}}"`）：用参数的**原始 JSON 值**替换，**类型保留**（数字仍是数字、布尔仍是布尔、对象仍是对象/table）。
- **片段占位**（`"hello {{name}}"`、`"scripts/{{role}}.lua"`）：按文本插值，结果为字符串。
- 未知 key：保留 `{{key}}` 字面量并打印一次 warning。

```json
// 父树引用子树并传入参数
{ "type":"Subtree", "source":"res://bt/greet.json",
  "params":{ "name":"lizhi", "age":18, "active":true, "role":"combat", "threshold":50 } }
```

```json
// res://bt/greet.json —— 任意字段都能用 {{key}}
{ "type":"Sequence", "children":[
  { "type":"Script", "source":"scripts/{{role}}.lua", "name":"{{role}}_node",
    "params":{ "name":"{{name}}", "age":"{{age}}", "active":"{{active}}", "msg":"hello {{name}}" } }
]}
```

上例：Script 脚本路径变成 `scripts/combat.lua`、节点名 `combat_node`；Script 收到 `name="lizhi"`(string)、`age=18`(number)、`active=true`(bool)、`msg="hello lizhi"`(string)。

**多层 `Subtree` 嵌套透传**：外层 `params` 先替换本层 JSON 的所有字段——包括内层 `Subtree` 节点的 `source` 和 `params`；内层 `Subtree` 加载自己的 JSON 时，再用它（已被外层填充）的 `params` 替换内层 JSON，逐层穿透。

## 完整示例

`bt.init({ root = "res://bt/ai_root.json" })`，`res://bt/ai_root.json`：

```json
{
  "type": "Pipeline", "name": "checkout_flow",
  "children": [
    { "type": "Script", "source": "scripts/goto_cart.lua",
      "condition": { "type":"Script", "source":"conds/on_home.lua" } },
    { "type": "Script", "source": "scripts/submit_order.lua",
      "condition": { "type":"Script", "source":"conds/on_cart.lua" } },
    { "type": "Script", "source": "scripts/confirm.lua",
      "condition": { "type":"Script", "source":"conds/on_checkout.lua" } }
  ]
}
```

落在购物车页 → 跳过 `goto_cart`、从 `submit_order` 开始；依次推进到 `confirm` 完成。需要分支决策（多选一）用 `Selector`/`Sequence`；公共子流程抽成 `Subtree`。

## 路径记录

每个 tick 记录**活跃路径**（root → 当前叶子），相同路径合并计数；事后回溯"树走了哪条路、多久、为何切换"，定位卡死或异常切换。`trace_paths` 默认 `true`（在 `bt.init` 关闭则返回空）。报告**不自动打印**，由你决定何时输出。

- **`bt.dump_paths()`** — 返回 table：`total_ticks`、`path_occurrences`、`terminal`、`has_terminal`、`terminal_note`、`tracing`、`paths[]`（`sig_ids`/`names`/`count`/`first_tick`/`last_tick`/`leaf_status`/`root_status`/`is_terminal`）、`nodes[]`、`switches[]`。
- **`bt.path_report()`** — 三视图报告字符串：**A 路径热度**（卡在哪、多久）、**B 树形热力图**（哪个分支热）、**C 切换时间线**（何时切换）。

末态（成功/失败/超时/异常）自动标记；`paths` 中 `is_terminal=true` 的路径就是树最终停留处——排查卡死优先看它。`Parallel` 一个 tick 产出 N 条路径（每子节点一条），故 `∑路径count = path_occurrences ≥ total_ticks`，A 列表占比分母用 `path_occurrences`。

## 运行机制（每个 tick）

1. **处理事件** — `bt.notify()` 推入的事件写入黑板（`_event_{name}`）
2. **响应式中断评估** — 扫描各 condition（`LowerPriority`/`Both`），false→true 翻转时抢占正在运行的低优先级兄弟分支（`Self` 中断在 tick 树时由各节点自行处理）
3. **Tick 树** — 从根节点执行；复合节点对每个子节点先门控（condition），再 tick。`Pipeline` 是 tick 驱动状态机：首次扫描定起点，之后每步按 `$timeout` 等待下一步 condition、超时按 `$retry` 回退重跑上一步动作
4. **树完成时重置** — 返回 success/failure 时重置树（清空复合节点游标、`Pipeline` 扫描状态、各节点条件状态、中断监视缓存）
