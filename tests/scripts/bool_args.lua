local M = {}
function M:Enter(params)
  self.has_flag = params.enabled
end
function M:Tick()
  if self.has_flag == true then
    return "success"
  end
  return "failure"
end
return M
