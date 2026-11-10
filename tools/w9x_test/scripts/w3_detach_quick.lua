-- w3_detach_quick.lua —— W3 探针的最小版：被调试目标立刻退出，回调里 Detach，看进程能不能正常退出
local out = ...
local f = io.open(out, 'w')
if not f then return end
f:write('before run\n'); f:flush()

Run{
    imagepath = 'C:\\WINDOWS\\TEMP\\w9x_exe.exe',
    cmdline   = 'C:\\WINDOWS\\TEMP\\w9x_exe.exe C:\\WINDOWS\\TEMP\\quick.lua C:\\WINDOWS\\TEMP\\quick_child.txt',
    callback  = function(pid, tid)
        local frames = 0
        for _ in StackTrace(tid) do frames = frames + 1 end
        f:write('frames=' .. frames .. '\n'); f:flush()
        Detach()
    end
}

f:write('run returned\n'); f:flush()
f:close()
