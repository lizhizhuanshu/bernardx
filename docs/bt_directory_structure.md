# 行为树目录结构规范

## 推荐项目结构

```
project/
├── trees/                    # 行为树 JSON 配置
│   └── ai_main/              # 每个目录 = 一棵行为树
│       ├── root.json         #   根树定义（必需）
│       ├── combat.json       #   子树 "combat"
│       └── patrol.json       #   子树 "patrol"
├── scripts/                  # Script 节点脚本
│   ├── combat/               #   按功能域组织
│   │   ├── aim.lua
│   │   └── attack.lua
│   ├── patrol/
│   │   ├── find_point.lua
│   │   └── move_to.lua
│   └── common/
│       └── idle.lua
└── sensors/                  # 传感器脚本
    ├── element_visible.lua
    ├── nearby.lua
    └── check_hp.lua
```

---

## trees/ — 行为树配置

每个子目录对应一棵行为树，通过 `bt.run("trees/ai_main")` 加载。

```
trees/ai_main/
├── root.json       # 根节点定义（文件名固定，必需）
├── combat.json     # 子树，文件名 → 子树名 "combat"
└── patrol.json     # 子树，文件名 → 子树名 "patrol"
```

| 文件 | 说明 |
|------|------|
| `root.json` | 根树定义，内容为节点 JSON 对象（无外层 `{ "root": ... }` 包裹） |
| `<name>.json` | 子树定义，文件名即子树名，通过 `{"type": "Subtree", "subtree": "<name>"}` 引用 |
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

传感器脚本平铺在 `sensors/` 目录下，通过 `"path"` 引用：

```
sensors/
├── element_visible.lua    # UI 元素可见性检测
├── nearby.lua             # 附近目标检测
└── check_hp.lua           # 血量检测
```

---

## 完整示例

```
project/
├── trees/
│   └── ai_main/
│       ├── root.json
│       ├── combat.json
│       ├── patrol.json
│       └── flee.json
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
