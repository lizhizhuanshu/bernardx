-- Filler action for Pipeline *target LowerPriority preemption: on its FIRST
-- run it flips the page away from cart and stays Running (mid-work); the
-- regression preempts it back to step 0. On the re-run (after step 0 redoes
-- its navigation) "flipped" is already set — the blackboard counter survives
-- the Reset — so it succeeds immediately and the flow completes.
local M = {}
function M:Tick()
  local bb = require('blackboard')
  if not bb.get("flipped") then
    bb.set("flipped", true)
    bb.set("page", "login")
    return "running"
  end
  return "success"
end
return M
