-- 动作: 黑板算术写回(`set count = @count + 1` 的编译目标,DSL 编译器发射)。
-- params: {key=目标黑板键, expr=槽名算式(n0/n1/.. 引用 params 槽), n0="$键" 运行期引用…}
-- @键 槽由引擎 ScriptNode 参数机制在 Enter 注入(含异步提供器);缺键槽缺失 →
-- 算术错误 → 脚本运行时错误 → 该步诚实失败(树里先 `set count = 0` 初始化)。
-- expr 是编译器全括号下发的 Lua 算式串,此处 load 后求值,结果 bb.set 写回。
local M = {}
local bb = require "blackboard"

function M:Enter(params)
  if type(params.key) ~= "string" or type(params.expr) ~= "string" then
    error("bb_calc 需要 key/expr 参数")
  end
  self.key = params.key
  local chunk, err = load("return " .. params.expr, "bb_calc", "t", params)
  if not chunk then
    error("bb_calc 表达式编译失败: " .. tostring(err))
  end
  self.calc = chunk
end

function M:Tick()
  local ok, v = pcall(self.calc)
  if not ok then
    error("bb_calc 求值失败(" .. tostring(self.key) .. "): " .. tostring(v))
  end
  bb.set(self.key, v)
  return "success"
end

return M
