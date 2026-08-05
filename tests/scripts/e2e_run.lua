-- Action: a long-running simulated operation. Returns "running" for `ticks`
-- ticks (0 = forever) then "success". Optionally writes blackboard[set_key]=
-- set_val on tick `set_at` (e.g. a worker that triggers a popup mid-run), and
-- records its Exit reason to blackboard[exit_key] (to assert it was aborted).
local M = {}
function M:Enter(p)
  self.ticks = p.ticks or 0
  self.set_at = p.set_at
  self.set_key = p.set_key
  self.set_val = p.set_val
  self.exit_key = p.exit_key
  self.n = 0
end
function M:Tick()
  self.n = self.n + 1
  local bb = require('blackboard')
  if self.set_key and self.n == self.set_at then
    bb.set(self.set_key, self.set_val)
  end
  if self.ticks > 0 and self.n >= self.ticks then return "success" end
  return "running"
end
function M:Exit(reason)
  if self.exit_key then
    require('blackboard').set(self.exit_key, reason)
  end
end
return M
