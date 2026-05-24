local data = require("sensor_helper")

local M = {}
function M:Tick()
  return data.value
end
return M
