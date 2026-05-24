local M = {}
function M:Enter()
  local bb = require('blackboard')
  bb.set("sensor_b_entered", true)
  bb.set("sensor_b_exited", false)
end
function M:Tick()
  return 2
end
function M:Exit()
  local bb = require('blackboard')
  bb.set("sensor_b_exited", true)
end
return M
