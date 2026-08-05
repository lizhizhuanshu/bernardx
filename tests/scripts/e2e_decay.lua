-- Condition: a guard that "holds" for the first `hold` ticks then fails.
-- Simulates a page that disappears after a while (to exercise Self abort
-- mid-action). Returns true (met) while self.n <= hold, else nil.
local M = {}
function M:Enter(p)
  self.hold = p.hold or 0
  self.n = 0
end
function M:Tick()
  self.n = self.n + 1
  return self.n <= self.hold and true or nil
end
return M
