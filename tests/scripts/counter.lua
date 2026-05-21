local M = {}
function M:Enter(args)
  self.counter = 0
end
function M:Tick()
  self.counter = self.counter + 1
  if self.counter >= 2 then
    return "success"
  end
  return "running"
end
return M
