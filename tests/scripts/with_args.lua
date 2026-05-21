local M = {}
function M:Enter(args)
  self.target = args.target
  self.damage = args.damage
end
function M:Tick()
  if self.target and self.damage then
    return "success"
  end
  return "failure"
end
return M
