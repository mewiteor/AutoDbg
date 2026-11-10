-- sleep.lua —— 并发可行性探针：跑 15 秒，写开始/结束标记
local out = ...
local f = io.open(out, 'w')
if not f then return end
f:write('start\n')
f:close()
local t0 = os.clock()
while os.clock() - t0 < 15 do end
f = io.open(out, 'a')
f:write('end\n')
f:close()
