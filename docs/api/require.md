# require / loadfile

运行时提供了自定义的 `require` 和 `loadfile`，支持通过 `CodeProvider` 异步加载模块。

## require(module_name)

模块查找顺序：
1. `package.loaded` 缓存
2. C 模块（通过 `LuaRuntime::Builder::Register` 注册）
3. LuaLibrary（通过 `LuaRuntime::Builder::RegisterLibrary` 注册）
4. CodeProvider 异步加载

```lua
local http = require('http')
local bt = require('bt')
local my_module = require('my_module')  -- 通过 CodeProvider 加载
```

## loadfile(filename)

- 绝对路径（以 `/` 开头）：直接加载文件
- 相对路径：通过 CodeProvider 异步加载

```lua
local chunk = loadfile("scripts/helper.lua")
if chunk then
    chunk()
end
```
