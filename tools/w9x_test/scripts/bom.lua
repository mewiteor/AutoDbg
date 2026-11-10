�local out = ...
local f = io.open(out, 'w')
if not f then return end
f:write('ran\n')
f:close()
