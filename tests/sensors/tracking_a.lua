local M = {}
function M:Enter()
  local bb = require('blackboard')
  bb.set("sensor_a_entered", true)
  bb.set("sensor_a_exited", false)
end
function M:Tick()
  return 1
end
function M:Exit()
  local bb = require('blackboard')
  bb.set("sensor_a_exited", true)
end
return M
