local M = {}
function M:Enter()
    self.count = 0
end
function M:Tick()
    self.count = (self.count or 0) + 1
    if self.count >= 2 then
        local bb = require('blackboard')
        bb.set("go", false)
    end
    if self.count >= 5 then
        return "success"
    end
    return "running"
end
return M
