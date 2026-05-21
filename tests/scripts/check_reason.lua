local M = {}
function M:Tick()
  return "success"
end
function M:Exit(reason)
  self.last_reason = reason
end
return M
