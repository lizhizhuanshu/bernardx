# BT DSL (.bt)

`.bt` 是行为树的**文本 DSL**：缩进定层级、任务导向语法（目标态 / 位置主参 / 双 sigil）。
bernardx 在加载时把它编译成与 JSON 定义**完全等价**的节点树（结构及键序一致——参照实现为
bernard-agent2 的 `bt_dsl/dsl.py`，两侧靠 golden 差分测试保持一致），引擎语义零新增。

```lua
bt.init({ root = "res://bt/tasks/demo.bt" })   -- 按扩展名识别 DSL
bt.exec({ interval = 50 })
```

## 语法

缩进定层级（任意一致宽度，同级必须一致；tab 拒绝）。行结构：

```
tree_name[step_retry=@common_action_retry, step_timeout=@common_action_timeout]: # 根描述
let url = "https://www.baidu.com/"          # 常量（仅根级；引用写 $url）

step_a: until in_app "com.x" open_app "com.x"   # 任务导向：until=目标态(*target)
step_b: when not see "**/Ad" until see "**/B" click "**/B"
step_c: until count "**/List" >= 3 swipe_on_node "**/List" order="up"
group_c: until (see "**/A" and see "**/B"):  # 容器步：子行缩进 → Pipeline
  inner: until see "**/C" click
settle: wait [200,400]ms                     # Wait
mark: set page = "home"                      # Set（黑板写）
copy: set dst = @src                         # Set 黑板拷贝（@键 = 运行期引用）
login: use login_flow(user="alice")          # Subtree(bt/subtrees/login_flow.json)
scroll_find: repeat until see "**/List" max 5 interval [800,1200]ms:
  swipe: swipe_on_node "**/List" order="up"
dispatch: choose:
  when see "**/DialogA":
    close: click "**/DialogA"
  otherwise:
    skip: wait 100ms
```

`name[mods]: [when cexpr] [until cexpr] <content> [# 注释]`

- **when** = guard（不成立则该步失败）：纯 bb 条件 → 引擎原生 `condition`；其余 → 可等待条件前缀 Sequence。
- **until** = 目标态（`*target`；缺省 = 动作完成即目标）。容器行另有 `until` 时挂 Pipeline 级 `*target`。
- **mods** = `[retry=N|[lo,hi]|@key, timeout=N|[lo,hi]ms|@key]`（顺序可交换）：动作/装饰行 → 步级
  `*retry`/`*timeout`；容器行 → Pipeline 级缺省 `params.retry/timeout`。另有 `step_retry`/`step_timeout`
  （仅根行/容器行）= **继承域缺省**：编译期级联进子树里每个未自带 `retry`/`timeout` 的 Pipeline 的
  `params`（本级自带优先；同级 `retry`/`step_retry` 互斥）。
- **mods 的 `@key`** = 运行期黑板引用（编译为引擎 `"$key"` 惰性解析：值可为 int 或 `{lo,hi}` 表，
  每轮重摇）——步级自愈数值由用户黑板控制（约定预设键 `common_action_retry`/`common_action_timeout`
  = 常规 UI 步、`common_network_retry`/`common_network_timeout` = launch_app/页面切换），树里只留引用：
  `task_x[step_retry=@common_action_retry, step_timeout=@common_action_timeout]: # 目标`。
- 行尾 `# 注释` → 节点 `description`。

### 条件表达式（cexpr）

`in_app "包名"` | `see "定位器" [k == "v"]` | `attr "定位器" k == "v"`（attr ≡ 必带 k/v 的 see）
| `count "定位器" >= 3`（匹配数比较，op ∈ ==/!=/>/</>=/<=）| `bb key op value|exists`
| `not` / `and` / `or` / 括号。定位器一律结构化下发（by/desc/key/value），不在编译期拼接谓词。

### 值引用双 sigil

- `$name` = **let 常量**（编译期内联；未定义 → 编译错误）。
- `@name` = **运行期黑板键**（动作参数 / set 值 / 位置主参可用，编译为引擎 `"$name"` 注入契约；
  条件参数不接受——运行期注入回字符串会破坏数值比较）。

### 数据流（auto_bind）

动作的定位参数（`desc`）与包名（`package_name`）由本行 when/until 条件自动绑定——唯一定位原子
（see/attr/count，不在 or/not 极性下）/ 唯一 `in_app`；显式 `k=v` 优先；绑定 desc 后缺省
`by="class_chain"`（显式 `by=` 优先）。动作可写**位置主参数**（`click "**/X"` / `open_app "com.x"`，
按注册表 `primary` 落参，与 k=v 同义）。

## 词表（registry）

动词 → Lua 脚本（`actions/*.lua`）与条件 → 双形态脚本（`conds/*.lua`）的映射由 registry 给出：

- **内嵌默认**：`src/bt/bt_dsl.cc` 中的 `DefaultRegistryText()`——bernard-agent2
  `src/bernard_agent2/bt_dsl/registry.json` 的原文拷贝（两侧契约变更时须同步）。
- **显式替换**：`bt.init({ root = "x.bt", registry = "res://bt/registry.json" })`——给出即
  **整体替换**内嵌默认（非合并）。工作区扩展词表（agent2 侧 libs 自动发现的动词）由部署侧
  导出合并后的 registry 文件传入。

registry JSON 结构（与 agent2 相同）：

```json
{
  "actions": { "click": { "source": "actions/click.lua", "primary": "desc",
                          "auto_bind": ["desc"] },
               "ime_input": { "source": "actions/ime_action.lua",
                              "fixed": { "action": "input" }, "requires": ["value"] } },
  "conds":   { "see": { "cond_source": "conds/see.lua", "waitable_source": "conds/see.lua" },
               "bb":  { "engine_cond": "Blackboard" } },
  "subtree_dir": "bt/subtrees"
}
```

字段：`source` 动作脚本；`primary` 位置主参落参名；`fixed` 固定参数；`requires` 必填参数；
`auto_bind` 自动绑定参数（desc/package_name）；conds 的 `cond_source`（布尔契约）/`waitable_source`
（字符串契约，repeat/choose/when 前缀位用）；`subtree_dir` 决定 `use x` 的源路径
`{subtree_dir}/x.json`。

> `registry` 选项只作用于**根**；Subtree/Template 的 `.bt` 兜底用内嵌默认词表。

## 编译时序与子树兜底

DSL 编译产物落在原 `json::parse` 的位置，其后 `params` 的 `{{key}}` 模板替换、数据引用解析、
`ParseNode` 原样运行——`.bt` 字符串里仍可写 `{{key}}` 占位符由 `bt.init` 的 `params` 填充。

`use x` 编译产出 `bt/subtrees/x.json`（与 Python 编译器一致）。加载时若该 `.json` 不存在而同名
`.bt` 存在，自动改用 `.bt`（内嵌默认 registry 编译）——子树可直接以 `.bt` 编写。

## 错误格式

编译错误带行号，`bt.init` 返回 `nil, "failed to compile root bt dsl '<path>': 第 N 行: ..."`。
与 Python 侧一致的报错点（未知动词、未定义 `$常量`、位置参数无 primary/重复、`requires` 缺参、
auto_bind 缺失/歧义、缩进非法……）。Python 侧的一个崩溃路径（`bb` 条件用于可等待位——registry
未登记 `waitable_source`）在此为干净编译错误。

## 测试与 golden 再生

- `tests/bt_dsl_test.cc`：golden 差分（`tests/data/bt_dsl/*.bt` ↔ `*.golden.json`，键序+内容
  一致）+ 单元/错误路径 + `bt.init` e2e。
- golden 再生（改语法时先改 agent2 侧 `dsl.py`，再同步 C++）：

```bash
python3 tools/gen_bt_dsl_goldens.py --agent2 /path/to/bernard-agent2
```
