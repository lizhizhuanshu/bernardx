# 复合节点 (Composite)

有子节点，通过 `"children"` 数组配置。

---

## Selector - 选择节点

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

---

## Sequence - 顺序节点

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

---

## Parallel - 并行节点

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

## RandomSelector - 随机选择节点

与 Selector 相同（OR 逻辑），但每次 Reset 后重新随机排列子节点执行顺序，使 AI 行为更加多样化。

```json
{
  "type": "RandomSelector",
  "children": [
    {"type": "Script", "path": "a.lua"},
    {"type": "Script", "path": "b.lua"}
  ]
}
```

---

## RandomSequence - 随机顺序节点

与 Sequence 相同（AND 逻辑），但每次 Reset 后重新随机排列子节点执行顺序。

```json
{
  "type": "RandomSequence",
  "children": [
    {"type": "Script", "path": "a.lua"},
    {"type": "Script", "path": "b.lua"}
  ]
}
```
