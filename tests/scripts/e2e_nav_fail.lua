-- Action: navigate to page `to` and THEN FAIL — an action whose side effects
-- break the previous step's *target (the page it navigated away from) before
-- erroring. Persistent across retries (no state, always the same outcome),
-- used to verify the precondition-redo bounce is bounded by *retry.
local M = {}
function M:Enter(p)
  self.to = p.to
end
function M:Tick()
  require('blackboard').set("page", self.to)
  return "failure"
end
return M
