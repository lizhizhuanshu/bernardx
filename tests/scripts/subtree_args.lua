-- Records its Enter params to the blackboard so tests can verify that a
-- parent Subtree's params were templated through (with types preserved).
local M = {}
function M:Enter(params)
  self.name = params.name
  self.age = params.age
  self.active = params.active
end
function M:Tick()
  local bb = require('blackboard')
  bb.set("sub_name", self.name)
  bb.set("sub_age", self.age)
  bb.set("sub_active", self.active)
  return "success"
end
return M
