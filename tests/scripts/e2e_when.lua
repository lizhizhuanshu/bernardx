-- Condition: blackboard[key] == value. Used to simulate "am I on page X?" /
-- "is flag Y set?" checks. Returns true (met) / nil (not met).
local M = {}
function M:Enter(p)
  self.key = p.key
  self.value = p.value
end
function M:Tick()
  return require('blackboard').get(self.key) == self.value and true or nil
end
return M
