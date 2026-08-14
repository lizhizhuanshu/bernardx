-- Records its Enter `params.target` (which may be a `$key` blackboard reference
-- resolved at Enter) back to the blackboard as "got_target", so tests can
-- verify the value the runtime injected. Works both as an action Script
-- (returns "success") and as a condition (a non-empty string is truthy).
local M = {}
function M:Enter(params)
  self.target = params.target
end
function M:Tick()
  local bb = require('blackboard')
  bb.set("got_target", self.target)
  return "success"
end
return M
