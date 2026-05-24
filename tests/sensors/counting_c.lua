local M = {}
function M:Enter()
  self.count = 0
end
function M:Tick()
  self.count = (self.count or 0) + 1
  return self.count
end
function M:Exit()
  local bb = require('blackboard')
  bb.set("sensor_c_final_count", self.count or 0)
end
return M
