local M = {}
function M:Enter(args)
  self.has_flag = args.enabled
end
function M:Tick()
  if self.has_flag == true then
    return "success"
  end
  return "failure"
end
return M
