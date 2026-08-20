local M = {}
local errc = require('errc')
function M:Enter()
    errc.raise()
end
function M:Tick()
    return "running"
end
return M
