-- Condition whose truthiness comes from a `$flag` blackboard reference
-- resolved at Enter. Set the blackboard key "flag" before the tree runs:
-- the condition is met (Success) when that value is truthy, not met (Failure)
-- otherwise. Proves ScriptCondition resolves `$key` params from the blackboard.
local M = {}
function M:Enter(params)
  self.flag = params.flag
end
function M:Tick()
  return self.flag
end
return M
