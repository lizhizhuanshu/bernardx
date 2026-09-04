-- Target condition that reads the blackboard FRESH every Tick (not Enter-cached),
-- mirroring the worker's page-recognition predicates which poll current UI state.
-- Returns met only when `done` is set.
local M = {}
function M:Tick()
  local bb = require('blackboard')
  return bb.get("done") == true
end
return M
