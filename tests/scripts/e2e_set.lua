-- Action: set blackboard[key] = value, then succeed. Used to flip simulated
-- page/flag state (e.g. "task done", "alert cleared").
local M = {}
function M:Enter(p)
  self.key = p.key
  self.value = p.value
end
function M:Tick()
  require('blackboard').set(self.key, self.value)
  return "success"
end
return M
