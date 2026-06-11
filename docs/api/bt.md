# bt 模块

行为树模块，详见 [bt_node_config.md](../bt_node_config.md)。

```lua
local bt = require('bt')
```

| 函数 | 说明 |
|------|------|
| `bt.run(opts)` | 加载并运行行为树直到完成或超时（协程异步） |
| `bt.notify(event, data)` | 发送事件到事件队列 |
| `bt.get_status()` | 获取状态 (`"idle"` / `"running"` / `"success"` / `"failure"`) |

## bt.run(opts)

协程异步——调用时 yield 挂起，行为树运行完成后自动恢复。

`bt.run()` 是行为树的一站式入口，完成加载、初始化、tick 循环的完整流程：

1. 解析 JSON 或从目录加载树结构
2. 创建对应的 `CodeProvider`（自动）
3. 初始化所有 Script 节点（加载脚本）
4. 初始化所有 Sensor（加载脚本）
5. 激活初始路径上的 Sensor
6. 进入 tick 循环，直到树完成、达到最大步数或超时

**参数：** `opts` table

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `path` | `string` | `path` 和 `json` 至少一个 | 树文件/目录路径 |
| `json` | `string` | `path` 和 `json` 至少一个 | 树 JSON 字符串 |
| `project_path` | `string` | 否 | 行为树项目根目录 |
| `max_step` | `integer` | 否 | 最大 tick 步数，未设置则无限 |
| `timeout` | `integer` | 否 | 超时时间（毫秒），未设置则不超时 |
| `interval` | `integer` | 否 | tick 间隔（毫秒），未设置则连续 tick |

**`project_path` 说明：**

路径以 `@` 开头表示远程资源路径（`@remote_base`），此时脚本通过 `ResourceProvider` 加载；否则为本地文件系统路径。

如果未提供 `project_path`，则使用 C++ 端通过 `SetProjectPath()` 设置的路径。

**BT 代码搜索路径**（本地模式，设置项目路径后）：

1. `{project_path}/scripts/`
2. `{project_path}/sensors/`
3. `{project_path}/`
4. `{主项目}/libs/`（主项目的共享库，通过 `SetMainLibsPath` 设置）

**BT 代码搜索路径**（远程模式，`@` 前缀）：

1. `{remote_base}/scripts/`
2. `{remote_base}/sensors/`
3. `{remote_base}/`

**返回值：**

| 返回值 | 类型 | 说明 |
|--------|------|------|
| `status` | `string` / `nil` | `"success"` / `"failure"` / `"timeout"`，出错时为 `nil` |
| `err` | `string` / `nil` | 出错时为错误信息，成功时为 `nil` |

**使用示例：**

```lua
local bt = require('bt')

-- 方式1: JSON 字符串
local status, err = bt.run({
    json = '{"root": {"type": "Selector", "children": [...]}}',
    project_path = "/path/to/bt_project"
})

-- 方式2: 目录路径
local status, err = bt.run({
    path = "trees/ai_main",
    project_path = "/path/to/bt_project"
})

-- 方式3: 带超时和步数限制
local status, err = bt.run({
    path = "trees/ai_main",
    project_path = "/path/to/bt_project",
    max_step = 100,
    timeout = 5000,
    interval = 100
})

-- 检查结果
if not status then
    error("bt.run failed: " .. err)
elseif status == "timeout" then
    print("tree timed out")
else
    print("tree finished:", status)
end
```

**每次 tick 内部依次执行：**

1. **处理事件** — 将 `bt.notify()` 推入的事件从队列取出，写入黑板（`_event_{name}`）
2. **执行传感器** — 运行所有到期且处于激活状态的 Sensor，将结果写入黑板
3. **评估装饰器** — 检查装饰器条件变化，若条件从满足变为不满足则触发对应节点的中止（`OnAborted`）
4. **Tick 树** — 从根节点开始执行行为树
5. **更新传感器激活状态** — 根据当前活跃路径激活/停用 Sensor
6. **树完成时自动重置** — 若返回 `success` 或 `failure`，停用所有 Sensor 并重置树

重复调用 `bt.run()` 会先停止并清理之前的树（递增 generation 以使旧的异步操作失效）。

`bt.run()` 会自动根据项目路径创建 `CodeProvider` 并设置到运行时，确保 Script/Sensor 节点能正确加载脚本。
