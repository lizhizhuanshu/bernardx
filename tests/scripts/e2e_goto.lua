-- Action: simulate navigating to a page. Sets blackboard["page"] = `to`.
-- With `flaky` = N, the first N attempts "don't take" (leave the page
-- unchanged) so a Pipeline step *retry re-run has something to recover from.
-- Attempt count is kept on the blackboard (per target) so it survives Reset.
local M = {}
function M:Enter(p)
  self.to = p.to
  self.flaky = p.flaky or 0
end
function M:Tick()
  local bb = require('blackboard')
  local key = "goto_" .. tostring(self.to)
  local n = (bb.get(key) or 0) + 1
  bb.set(key, n)
  if n > self.flaky then
    bb.set("page", self.to)  -- navigation succeeded
  end
  -- flaky attempt: leave the page unchanged
  return "success"
end
return M
