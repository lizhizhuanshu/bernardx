local M = {}
function M:Enter(params)
  self.target = params.target
  self.damage = params.damage
end
function M:Tick()
  if self.target and self.damage then
    return "success"
  end
  return "failure"
end
return M
