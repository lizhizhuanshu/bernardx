-- Verifies that an object-valued param arrives in Enter as a real Lua table
-- (nested field access). Used by the table-param passthrough tests.
local M = {}
function M:Enter(params)
  self.cfg = params.config
end
function M:Tick()
  local bb = require('blackboard')
  bb.set("t_name", self.cfg.name)
  bb.set("t_hp", self.cfg.hp)
  return "success"
end
return M
