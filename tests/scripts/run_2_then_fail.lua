local M = {}
function M:Enter()
  self.count = 0
end
function M:Tick()
  self.count = (self.count or 0) + 1
  if self.count >= 2 then
    return "failure"
  end
  return "running"
end
return M
