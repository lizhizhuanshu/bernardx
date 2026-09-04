-- Action that only achieves its target on the SECOND run (survives Reset via
-- a blackboard counter). Run 1 returns success but does NOT set `done` -> the
-- step's *timeout waits, then OnWaitTimeout retries. Run 2 (after Reset)
-- sets `done=true`, so the retried action succeeds AND the *target must be
-- RE-TICKED to see `done` and advance. State lives on the blackboard because
-- Reset() clears the node's `self`.
local M = {}
function M:Tick()
  local bb = require('blackboard')
  local n = bb.get("retry_achieve_count") or 0
  bb.set("retry_achieve_count", n + 1)
  if n == 0 then
    return "success"          -- first run: no side effect -> target unmet -> timeout -> retry
  end
  bb.set("done", true)        -- second run: achieve the target
  return "success"
end
return M
