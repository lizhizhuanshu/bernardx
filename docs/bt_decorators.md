# 装饰器 (Decorators)

附加在任意节点上，通过 `"decorators"` 数组配置。装饰器在节点 tick 前评估条件。

```json
{
  "type": "Script",
  "path": "a.lua",
  "decorators": [
    { "type": "BlackboardCondition", ... },
    { "type": "Inverter" }
  ]
}
```

---

## BlackboardCondition - 黑板条件

根据黑板数据决定节点是否可执行。

```json
{
  "type": "BlackboardCondition",
  "key": "hp",
  "operator": "greater_than",
  "value": 50,
  "abort": "Self"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `key` | **是** | 黑板键名 |
| `operator` | 否 | 比较运算符，默认 `"is_set"` |
| `value` | 否 | 期望值（部分运算符需要） |
| `abort` | 否 | 中断模式，默认 `"None"` |

**operator 可选值：**

| 值 | 说明 | 需要 value |
|---|------|-----------|
| `"is_set"` | 键存在即通过 | 否 |
| `"is_not_set"` | 键不存在即通过 | 否 |
| `"equals"` | 值相等 | 是 |
| `"not_equals"` | 值不相等 | 是 |
| `"greater_than"` | 值大于 | 是 |
| `"less_than"` | 值小于 | 是 |

**value 支持类型：** `bool`、`int`（int64）、`double`、`string`。类型不匹配时条件为 false。

**abort 可选值（UE4/5 风格观察者中止）：**

| 值 | 说明 |
|---|------|
| `"None"` | 不中断（默认） |
| `"Self"` | 条件变化时中断自身正在执行的子树 |
| `"LowerPriority"` | 条件变化时中断右侧低优先级节点 |
| `"Both"` | 同时中断自身和低优先级 |

---

## Inverter - 取反装饰器

反转被装饰节点的成功/失败结果。

```json
{"type": "Inverter", "abort": "None"}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `abort` | 否 | 中断模式，默认 `"None"` |

---

## ForceSuccess - 强制成功装饰器

无论被装饰节点结果如何，始终返回成功。

```json
{"type": "ForceSuccess"}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `abort` | 否 | 中断模式，默认 `"None"` |

---

## ForceFailure - 强制失败装饰器

无论被装饰节点结果如何，始终返回失败。

```json
{"type": "ForceFailure"}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `abort` | 否 | 中断模式，默认 `"None"` |
