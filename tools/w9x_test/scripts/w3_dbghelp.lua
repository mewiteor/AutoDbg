-- w3_dbghelp.lua —— dynapi 降级探针（P7 W3 / plan_p7_win9x.md §4.2 case 13）
-- 目标：dbghelp.dll 不是 Win9x 自带（Win95/98/ME 没有）→ `SymFunctionTableAccess` /
--       `UnDecorateSymbolName` / `StackWalk*` 走 dynapi 动态解析，取不到必须**优雅退化**
--       （不是硬失败）。用 Run 起一个被调试目标（第二个 AutoDbg 实例，跑 sleep.lua 撑 15 秒），
--       在入口断点里对目标线程做 StackTrace：
--         · 取不到 dbghelp → 0 帧（退化路径）
--         · 取到 dbghelp   → 帧数 > 0
-- ❗guest 里 `CheckConsoleInput`：`GetNumberOfConsoleInputEvents` 成功但事件数为 0 →
--   返回 CONTINUE → 调试循环永远不结束（实测卡死）→ 必须显式 `Detach()`。
-- ❗Lua 的 io 是缓冲写：不 flush 的话 host 取回的是空文件 → 每步都 flush。
local out = ...
local f = io.open(out, 'w')
if not f then return end
f:write('before run\n'); f:flush()

Run{
    imagepath = 'C:\\WINDOWS\\TEMP\\w9x_exe.exe',
    cmdline   = 'C:\\WINDOWS\\TEMP\\w9x_exe.exe C:\\WINDOWS\\TEMP\\sleep.lua C:\\WINDOWS\\TEMP\\sleep_child.txt',
    callback  = function(pid, tid)
        local frames = 0
        for _ in StackTrace(tid) do frames = frames + 1 end
        f:write('frames=' .. frames .. '\n'); f:flush()
        f:write('has_dbghelp=' .. (frames > 0 and 'yes' or 'no') .. '\n'); f:flush()
        Detach()
        f:write('detached flag set\n'); f:flush()
    end
}

f:write('run returned\n'); f:flush()
f:close()
