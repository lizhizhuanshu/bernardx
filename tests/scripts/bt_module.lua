local M = {}
function M:Enter(params)
  self.checked = true
end
function M:Tick()
  if self.checked then
    return "success"
  end
  return "failure"
end
return M
