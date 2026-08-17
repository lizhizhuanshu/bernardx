-- Fails the first time it runs (global run counter via blackboard), succeeds
-- on every later run — Retry's Reset+re-run after the interval wait reaches
-- the success branch. State lives on the blackboard so it survives the
-- per-run Enter/Reset cycle.
local M = {}
function M:Tick()
  local bb = require('blackboard')
  local runs = bb.get("fail_then_ok_runs") or 0
  bb.set("fail_then_ok_runs", runs + 1)
  if runs == 0 then
    return "failure"
  end
  return "success"
end
return M
