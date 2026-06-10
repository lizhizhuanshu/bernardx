# 行为树目录结构规范

## 推荐项目结构

行为树项目可以独立于主 Lua 项目，拥有自己的目录结构。通过 `bt.run({project_path = path, ...})` 指定项目根目录后，树的加载、脚本查找和 `require()` 都基于该路径。

```
bt_project/                   ← project_path 指向这里
├── trees/                    # 行为树 JSON 配置
│   └── ai_main/              # 每个目录 = 一棵行为树
│       ├── root.json         #   根树定义（必需）
│       ├── sensors.json      #   全局传感器定义（可选）
│       └── subtrees/         #   子树目录
│           ├── combat.json   #     子树 "combat"
│           └── patrol.json   #     子树 "patrol"
├── scripts/                  # Script 节点脚本
│   ├── combat/               #   按功能域组织
│   │   ├── aim.lua
│   │   └── attack.lua
│   ├── patrol/
│   │   ├── find_point.lua
│   │   └── move_to.lua
│   └── common/
│       └── idle.lua
└── sensors/                  # 传感器脚本（按 name 自动映射路径）
    ├── element_visible.lua
    ├── nearby.lua
    └── check_hp.lua
```

### 使用方式

```lua
local status = bt.run({
    path = "trees/ai_main",
    project_path = "/path/to/bt_project"
})
```

### 代码查找路径

设置项目路径后，BT 的 `require()` 按以下顺序查找模块：

1. `{project_path}/scripts/` — 脚本节点目录
2. `{project_path}/sensors/` — 传感器脚本目录
3. `{project_path}/` — 项目根目录
4. `{主项目}/libs/` — 主项目的共享库目录

Script 和 Sensor 节点的 `path` 字段相对于项目根目录解析。例如 `"path": "scripts/combat/attack.lua"` 解析为 `{project_path}/scripts/combat/attack.lua`。

---

## trees/ — 行为树配置

每个子目录对应一棵行为树。设置了项目路径时，通过 `bt.run({path = "trees/ai_main"})` 加载（路径相对于项目根目录）；未设置时，相对于当前工作目录。

```
trees/ai_main/
├── root.json         # 根节点定义（文件名固定，必需）
├── sensors.json      # 全局传感器定义（可选）
└── subtrees/         # 子树目录（可选）
    ├── combat.json   #   子树 "combat"
    └── patrol.json   #   子树 "patrol"
```

| 文件 | 说明 |
|------|------|
| `root.json` | 根树定义，内容为节点 JSON 对象（无外层 `{ "root": ... }` 包裹） |
| `sensors.json` | 全局传感器定义，JSON 对象，键为传感器名称，值为配置（`interval`、`path`、`args`） |
| `subtrees/<name>.json` | 子树定义，文件名即子树名，通过 `{"type": "Subtree", "subtree": "<name>"}` 引用 |
| 非 `.json` 文件 | 自动忽略 |

---

## scripts/ — 脚本节点

Script 节点通过 `"path"` 字段引用 Lua 脚本，推荐按功能域分子目录：

```
scripts/
├── combat/          # 战斗
│   ├── aim.lua
│   └── attack.lua
├── patrol/          # 巡逻
│   ├── find_point.lua
│   └── move_to.lua
└── common/          # 通用
    └── idle.lua
```

---

## sensors/ — 传感器脚本

传感器脚本放在 `sensors/` 目录下，通过传感器名称自动映射路径（`.` 替换为 `/`）。传感器有两种配置方式：

1. **全局定义**（`sensors.json`）：在树目录中定义，节点通过名称字符串引用
2. **内联定义**：直接在节点的 `"sensors"` 数组中写完整配置

详见 [传感器文档](bt_sensors.md)。

```
sensors/
├── element_visible.lua    # UI 元素可见性检测
├── nearby.lua             # 附近目标检测
└── combat/
    └── target_check.lua   # 传感器名 "combat.target_check"
```

---

## 完整示例

```lua
-- main.lua
local status = bt.run({
    path = "trees/ai_main",
    project_path = "bt_project"
})
print("tree finished:", status)
```

```
bt_project/
├── trees/
│   └── ai_main/
│       ├── root.json
│       ├── sensors.json
│       └── subtrees/
│           ├── combat.json
│           ├── patrol.json
│           └── flee.json
├── scripts/
│   ├── combat/
│   │   ├── aim.lua
│   │   └── attack.lua
│   ├── patrol/
│   │   ├── find_point.lua
│   │   └── move_to.lua
│   ├── flee/
│   │   └── run_away.lua
│   └── common/
│       └── idle.lua
└── sensors/
    ├── has_target.lua
    ├── low_hp.lua
    └── nearby.lua
```
