-- w3_cmdtest.lua —— 判断「command.com 不执行」是 vmrun 的问题还是 command.com 本身的问题：
-- 用 AutoDbg 的 Run（CreateProcess）起 command.com，看它能不能执行 /c 命令。
local out = ...
local f = io.open(out, 'w')
if not f then return end
f:write('before run\n'); f:flush()

Run{
    imagepath = 'C:\\WINDOWS\\command.com',
    cmdline   = 'C:\\WINDOWS\\command.com /c echo hello-from-cmd>C:\\WINDOWS\\TEMP\\cmdtest.txt',
    callback  = function(pid, tid)
        f:write('callback pid=' .. tostring(pid) .. '\n'); f:flush()
        Detach()
    end
}

f:write('run returned\n'); f:flush()
f:close()
