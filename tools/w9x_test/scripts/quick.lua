-- quick.lua —— 子实例用：立刻写文件并退出（W3 探针的被调试目标）
local out = ...
local f = io.open(out, 'w')
if not f then return end
f:write('child ok\n')
f:close()
