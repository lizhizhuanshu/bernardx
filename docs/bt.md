# 行为树 (bt 模块)

UE4/5 风格行为树：**JSON 描述结构**、Lua 脚本承载逻辑、黑板做节点间数据共享、**轮询式条件（`NodeCondition`）做判断**、`Pipeline` 做跳过已完成步骤的顺序自动化流水线。

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
| `bt.get_status()` | `"idle"`/`"ready"`/`"running"`/`"paused"`/`"success"`/`"failure"` |
| `bt.dump_paths()` / `bt.path_report()` | 路径数据/报告（见[路径记录](#路径记录)） |

状态机：`idle →(init) ready →(exec) running ↔ paused → success/failure →(stop) idle`。`init` 在 running/paused 时报错（先 `stop`）；终态再 `exec` 报错（重新 `init`）；`ready`/`paused` 之外的状态调 `exec` 也报错。

> **暂停/恢复**是宿主级（`LuaRuntime::Pause/Resume`，由宿主调用），**不由 Lua 调用**。暂停时 bt 后台 tick 与 `sleep` 停推进（树状态保留），`http`/`async` 照常；恢复后从原位置继续。

### bt.init / bt.exec 参数

`bt.init`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `root` | string | **是** | 根节点 JSON 文件路径（见[路径解析](#路径解析)） |
| `params` | table | 否 | 根模板参数：把根 JSON 里任意字符串字段的 `{{key}}` 占位符按值替换（规则同[子树参数透传](#子树参数透传)：整值占位保留类型、片段占位按文本插值、未知 key 留字面量并告警）。不传则根按原样加载 |
| `trace_paths` | bool | 否 | 采集路径数据（默认 `true`） |

`bt.exec`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `interval` | integer | 否 | tick 间隔（毫秒），默认 50，需 > 0 |
| `max_step` | integer | 否 | 最大 tick 步数，未设则无限 |
| `timeout` | integer | 否 | 超时（毫秒），未设则不超时 |

### 根参数模板化

`bt.init` 的 `params` 把**根 JSON 当作子树**处理：根里任意字符串字段的 `{{key}}` 占位符都会被 `params` 的值替换（规则与[子树参数透传](#子树参数透传)完全一致——整值占位 `"{{age}}"` 保留原始类型，片段占位 `"hello {{name}}"` 按文本插值，未知 key 留字面量并告警）。同一份 JSON 可被不同参数复用：

```lua
bt.init({ root = "res://open_app.json",
          params = { package_name = "com.example.app",
                     timeout = { 500, 2000 } } })
bt.exec({ interval = 50 })
```

```json
// res://open_app.json —— 任意字段都能用 {{key}}
{ "type":"Selector", "children":[
  { "type":"Script", "source":"scripts/in_app.lua",
    "condition":{ "type":"Script", "source":"conds/in_app.lua",
                  "params":{ "package_name":"{{package_name}}" } } },
  { "type":"Wait", "params":{ "timeout":"{{timeout}}" } }
]}
```

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
| `condition` | 全部 | 可选的守卫条件（见[条件](#条件)）。**所有节点通用**：复合节点 tick 子节点前先门控（Failure 则跳过/失败）；条件对象上的 `abort` 字段（默认 `None`）再开启 UE4/5 风格的响应式中断（`Self`/`LowerPriority`/`Both`）。（`Pipeline` 的步骤完成判据用独立的 `*target` 边参数，不用 `condition`，见下。）`Subtree` 的 `condition` 还可取特殊字符串 `"child_condition"`——透传引用所嵌子树**根节点**自身的 condition（共享同一对象，让父复合在子树边界处门控一个定义在子树文件内的条件；`abort` 随子树根条件一起带过来；子树根无 condition 时为 no-op 并告警）。 |
| `params` | 各类型 | 类型相关参数：Script→`Enter` 参数、Wait→`timeout`、Repeat→`count`、Retry→`max_count`/`interval`、Parallel→`success_policy`/`failure_policy`、Subtree 透传给子树（见[子树参数透传](#子树参数透传)） |
| `source` | Script / Subtree | Script 的 Lua 脚本路径 / Subtree 的子树 JSON 路径 |
| `description` | 全部 | 备注（仅文档/调试） |

### 节点类型

| 类型 | 分类 | 特有字段 | 说明 |
|------|------|---------|------|
| `Selector` | 复合 | — | 依次 tick，首个 Success 即 Success（OR） |
| `Sequence` | 复合 | — | 依次 tick，首个 Failure 即 Failure（AND） |
| `Pipeline` | 复合 | 子节点 `*target`/`*timeout`/`*retry` | 跳过已完成步骤 + 每步"动作→等目标→超时重跑"的自动化流水线（见下） |
| `Parallel` | 复合 | `params` | 并行所有子节点，策略控制结果 |
| `RandomSelector` / `RandomSequence` | 复合 | — | 同 Selector/Sequence，每次 Reset 后随机排列子节点顺序 |
| `Script` | 叶子 | `source`, `params` | 执行 Lua 脚本 |
| `Set` | 叶子 | `params` | **内置黑板写 Action**（零脚本）：`params.key`（必填）写入目标键；`params.value`（标量，缺省写 nil）写入值。字符串值以 `$` 开头为黑板引用——`"$src"` 在**每次 Tick** 实时读 `blackboard[src]` 拷入（缺键写 nil 并告警），`"$$x"` 转义为字面 `"$x"`。写入后立即 Success，常与 `Blackboard` 条件配合组成纯声明式树 |
| `Subtree` | 叶子 | `source`, `params` | 加载 `source` 指向的子树 JSON（递归）；`params` 透传给子树内节点（见[子树参数透传](#子树参数透传)）；`condition` 支持 `"child_condition"` 透传子树根的条件（见上[节点结构](#节点结构)） |
| `Wait` | 叶子 | `params` | 等待若干毫秒后 Success：`params.timeout` 为**数值**（固定等待，默认 1000）或**两元素数组 `[lo, hi]`**（闭区间均匀随机，每次运行重摇；`[v]` 等价固定 v）。`0` 立即 Success。旧 `min_timeout`/`max_timeout` 已移除，出现即解析报错 |
| `Success` | 叶子 | — | 恒 Success（常用于挂 `condition` 的"已达成→成功"分支，如 Selector 第一支：条件成立即短路） |
| `Failure` | 叶子 | — | 恒 Failure |
| `Repeat` | 包装 | `params`, `child` | 重复执行子节点：`params.count` 为**数值**（默认 -1 无限）或 **`[lo,hi]` 数组**（每次运行摇一次、本次运行内固定） |
| `Retry` | 包装 | `params`, `child` | 失败重试：`params.max_count` 同上（数值或 `[lo,hi]`，默认 -1 无限）；`params.interval`（**毫秒**，数值或 `[lo,hi]`，缺省 0）为**两次尝试之间的等待**——失败后先等 interval 毫秒再重跑子节点（同样每次运行摇一次），首次尝试不等待。常用于避免高频死循环重试（如等弹窗消失、页面就绪） |
| `Inverter` | 包装 | `child` | 反转子节点结束态 Success↔Failure（Running 透传） |

**Parallel 策略**（`params.success_policy` / `params.failure_policy`，均 `RequireAll`/`RequireOne`）：默认 `RequireAll` / `RequireOne`。

### Pipeline —— 跳过已完成步骤 + 顺序执行

**Pipeline 级默认**：Pipeline 节点自己的 `params.timeout` / `params.retry`（同样的 int / `[lo,hi]` / `"$key"` 形式）作为**未声明边参数步骤的默认值**——步骤自身的 `*timeout`/`*retry` 优先。适合一整条流水线统一超时/重试策略：

```json
{ "type":"Pipeline", "params":{ "timeout":5000, "retry":2 },
  "children":[ { "type":"Script", "source":"scripts/a.lua",
                 "*target":{ "type":"Script", "source":"conds/ok.lua" } } ] }
```

为软件自动化的"每步一个目标状态 + 达成目标的动作"流程设计：每步声明自己的 `*target`（目标状态），**目标已成立的步骤直接跳过**，未成立的才执行动作。**tick 驱动的状态机**——内部不 sleep，只记录状态（当前步、相位、等待起点、已用重试），每个 tick 根据状态决定动作；唯一用到的墙钟是 `*timeout` 的毫秒预算。`children` 是有序步骤，每个子节点带三个 **Pipeline 边参数** `*target` / `*timeout` / `*retry`（`*` 前缀标记，表明它们是管线边参数、节点自身忽略）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `*target` | object（条件） | 该步的**目标状态**（见[条件](#条件)）："这步做完时应当成立"。**已成立 → 该步跳过**（工作已完成）；不成立 → 执行本步动作。可选，缺省 = 动作完成即目标（该步不会被跳过，动作跑完即过）。条件对象上可写 `abort` 字段（`Self`/`LowerPriority`/`Both`）开启**响应式中断**——guard 的镜像语义（对 target，"**成立**"才是事件）：`Self` = 动作运行期间 target 成立 → 中断动作、跳过剩余工作立即前进；`LowerPriority` = 后续步运行期间**更早**步的 target 成立→不成立翻转（前置回退，如中途被登出）→ 打断当前工作回跳重做该步；`Both` = 两者 |
| `*timeout` | int、`[lo,hi]` 或 `"$key"` (**毫秒**) | 本步动作跑完后等待 `*target` 成立的墙钟预算；数组形式表示闭区间内**均匀随机**；0/缺省 = 无限等（缺省时若 Pipeline 定义了 `params.timeout` 则用该默认值）。**分配时机是本步每次进入等待窗口时**（Reset 后的每轮运行重新分配）：`[lo,hi]` 重摇、`"$key"` 黑板引用活读（provider 活调，值可为整数或 `{lo,hi}` 表）。引用解析失败（缺键/类型错）记日志并按 0 处理（= 无限等），不报解析错误 |
| `*retry` | int、`[lo,hi]` 或 `"$key"` | 等待本步 `*target` 超时后，**重跑本步动作**的最大次数；同样支持 `[lo,hi]` 范围随机与 `"$key"` 黑板引用，缺省时回退 `params.retry`，分配时机同上 |

执行：

- **跳过已完成（断点续传）** — 自上而下求值各步 `*target`：成立即跳过该步（动作不跑）；扫到 Running 则整条流水线 Running、下 tick 续扫；停在**首个不成立**的步并执行其动作。**全部成立 → 整条流水线立即 Success**。缺省 `*target` 的步不会被跳过（无从判断已完成），动作跑完即视为达成。
- **跑动作** — tick 当前步动作（可跨多个 tick）；动作 Success → 进入本步的等待；动作 Failure → Pipeline failure。
- **等本步 target（带超时+重跑）** — 动作完成后每 tick 求值本步 `*target`：成立 → 前进到下一步（再次跳过已完成的后续步）；不成立 → 记为等待，墙钟时长达到 `*timeout` 毫秒即超时。超时后：若 `*retry` 还有余额，**Reset + 重跑本步动作**、随后重新等待（重置毫秒预算）；`*retry` 耗尽仍不成立 → Pipeline failure。

当每个 `*target` 表示一个独立的页面/状态时，"跳过已成立"= 断点续传——落在中间页就从下一个未完成步接着做；超时+重跑本步动作则覆盖"动作再触发一次，好让目标页面出现"的常见自动化重试。

```json
{ "type":"Pipeline", "children":[
  { "type":"Script", "source":"scripts/login.lua",
    "*target":{ "type":"Script", "source":"conds/logged_in.lua" } },
  { "type":"Script", "source":"scripts/open_settings.lua",
    "*target":{ "type":"Script", "source":"conds/on_settings_page.lua" },
    "*timeout": 500, "*retry": 2 },
  { "type":"Script", "source":"scripts/toggle_switch.lua",
    "*target":{ "type":"Script", "source":"conds/switch_on.lua" },
    "*timeout": 500, "*retry": 2 }
]}
```

**范围随机**：`*timeout` / `*retry` 既可写整数（固定值），也可写两元素数组 `[lo, hi]`——表示在该闭区间内**均匀随机**取一个值。Pipeline 在**每次运行**（Reset 后）为每个步各摇一个值，该步本次运行内固定（等价于"随机出的固定值"，不会每次 tick 重新摇）。常用于避免多次运行/多实例的等待与重试完全雷同：

```json
{ "type":"Pipeline", "children":[
  { "type":"Script", "source":"scripts/login.lua",
    "*target":{ "type":"Script", "source":"conds/logged_in.lua" } },
  { "type":"Script", "source":"scripts/open_settings.lua",
    "*target":{ "type":"Script", "source":"conds/on_settings_page.lua" },
    "*timeout": [400, 800], "*retry": [1, 3] }
]}
```

> 范围内若含 0，0 仍保留"无限等/不重跑"语义；要表达"有界随机"建议下界 ≥1（毫秒）。`*timeout`/`*retry` 单元素数组 `[v]` 等价于标量 `v`，乱序数组 `[b,a]`(b>a) 会被归一化为 `[a,b]`。
>
> `Wait` 的 `timeout` 与 Pipeline 的 `*timeout` 单位一致，均为**毫秒**。`*target`/`*timeout`/`*retry` 是边参数（描述该步的完成判据与转移），节点自身（Script 等）不读取它们，Pipeline 在解析时单独取。子节点仍可带普通 `condition`（节点守卫，见[条件](#条件)），与 `*target` 互不相干。纯顺序无需目标判断/等待重试用 `Sequence`。

## 条件

`condition` 是一个 JSON 对象（本身也是一棵小树），挂在任意节点上，在被需要时**同步轮询** `Tick` 得出三态：**Success=成立 / Failure=不成立 / Running=判断中（异步 yield）**。

| 类型 | 字段 | 说明 |
|------|------|------|
| `Script` | `source`, `params` | Lua 脚本判断（见下） |
| `Blackboard` | `key`, `op`, `value` | 内置黑板值比较，无需脚本（见下） |
| `And` | `children` | 全部成立才成立，短路到首个 Failure/Running |
| `Or` | `children` | 任一成立即成立，短路到首个 Success/Running |
| `Not` | `child` | 反转 Success/Failure，Running 透传 |

**Blackboard 条件**直接比较黑板值与一个字面量或另一个黑板 key 的值，同步、零脚本，适合简单的状态门控：

```json
"condition": { "type":"Blackboard", "key":"page", "op":"==", "value":"home" }   // key 对字面量
"condition": { "type":"Blackboard", "key":"hp",   "op":">",  "key2":"shield" } // key 对 key（实时）
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `key` | string | **必填**，要读取的黑板 key（比较左值） |
| `op` | string | 可选，默认 `"=="`；取值 `==`、`!=`、`>`、`>=`、`<`、`<=`、`exists` |
| `value` | 标量 | 期望字面量（string/number/bool/null）；与 `key2` 互斥 |
| `key2` | string | 右值改为读黑板 `key2` 的值；**每次求值都实时重读**；与 `value` 互斥，`exists` 不可用 |

语义要点：

- **key 不存在 → 一律不成立**（包括 `!=`，`key2` 形式任一侧缺失同理）；要判断存在性用 `op:"exists"`。
- `==`/`!=` 按类型感知比较：数值跨 int/float 互通，string/bool 同型比较，`null` 匹配 nil；类型不匹配（如数字 vs 字符串）视为不相等。
- `>`/`>=`/`<`/`<=` 要求两侧同为数值或同为字符串（字典序）；否则不成立并记录 last_error。
- `key2` 是**活比较**：守卫每次轮询都重读两侧黑板值，适合"当 hp > shield 时…"这类随运行变化的门控；与 params 的 `$key`（Enter 时快照注入）语义不同，按需选用。
- 解析期即校验：缺 `key`、未知 `op`、`value` 非标量、排序 op 配 bool 值、`value` 与 `key2` 同给、`exists` 配 `key2` 都会直接报解析错误。

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

> `abort` 写在**顶层** condition 对象上（节点直接挂的那个）；组合条件（And/Or/Not）的子条件不带 `abort`。`Pipeline` 子节点的 condition 守卫同样适用——`Self` 即"动作运行中页面丢失就中断该步"（注意与步骤的 `*target` 无关）。

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

0. **盖时间戳** — 引擎在 tick 开始读一次单调时钟缓存进 per-tick 上下文；节点（Wait、Pipeline 的 `*timeout` 预算）统一读该缓存时间，同 tick 内看到同一时刻、不再各自调时钟
1. **响应式中断评估** — 扫描各 condition（`LowerPriority`/`Both`），false→true 翻转时抢占正在运行的低优先级兄弟分支（`Self` 中断在 tick 树时由各节点自行处理）
2. **Tick 树** — 从根节点执行；复合节点对每个子节点先门控（condition），再 tick。`Pipeline` 是 tick 驱动状态机：先跳过 `*target` 已成立的步骤，之后每步"跑动作 → 按 `*timeout` 等 `*target` 成立、超时按 `*retry` 重跑本步动作"
3. **树完成时重置** — 返回 success/failure 时重置树（清空复合节点游标、`Pipeline` 扫描状态、各节点条件状态、中断监视缓存）
