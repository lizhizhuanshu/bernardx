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
    "decorators": [...],
    "sensors": [...]
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

## 节点类型汇总

| 类型 | 分类 | 必填字段 | 子节点 | 说明 |
|------|------|---------|--------|------|
| `Selector` | [复合](bt_composite_nodes.md) | `type` | `children` | OR 逻辑，任一成功即成功 |
| `Sequence` | [复合](bt_composite_nodes.md) | `type` | `children` | AND 逻辑，全部成功才成功 |
| `Parallel` | [复合](bt_composite_nodes.md) | `type` | `children` | 并行执行，策略控制结果 |
| `RandomSelector` | [复合](bt_composite_nodes.md) | `type` | `children` | 随机顺序 OR 逻辑 |
| `RandomSequence` | [复合](bt_composite_nodes.md) | `type` | `children` | 随机顺序 AND 逻辑 |
| `Script` | [叶子](bt_leaf_nodes.md) | `type`, `path` | 无 | 执行 Lua 脚本，支持 `args` 传参 |
| `Subtree` | [叶子](bt_leaf_nodes.md) | `type`, `subtree` | 无 | 引用 subtrees 中定义的子树 |
| `Wait` | [叶子](bt_leaf_nodes.md) | `type` | 无 | 等待指定毫秒数 |
| `Repeat` | [包装](bt_wrapper_nodes.md) | `type` | `children[0]` | 重复执行子节点 |
| `RetryUntilSuccessful` | [包装](bt_wrapper_nodes.md) | `type` | `children[0]` | 失败时重试子节点 |
| `BlackboardCondition` | [装饰器](bt_decorators.md) | `type`, `key` | N/A | 黑板条件判断 |
| `Inverter` | [装饰器](bt_decorators.md) | `type` | N/A | 反转结果 |
| `ForceSuccess` | [装饰器](bt_decorators.md) | `type` | N/A | 强制成功 |
| `ForceFailure` | [装饰器](bt_decorators.md) | `type` | N/A | 强制失败 |

---

## 详细文档

- [复合节点](bt_composite_nodes.md) — Selector, Sequence, Parallel, RandomSelector, RandomSequence
- [叶子节点](bt_leaf_nodes.md) — Script, Subtree, Wait
- [包装节点](bt_wrapper_nodes.md) — Repeat, RetryUntilSuccessful
- [装饰器](bt_decorators.md) — BlackboardCondition, Inverter, ForceSuccess, ForceFailure
- [传感器](bt_sensors.md) — 异步感知模块

---

## 完整示例

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
