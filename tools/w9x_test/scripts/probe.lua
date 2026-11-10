-- probe.lua —— W1 最小往返探针（BOM-free、LF；harness 约定见 tools/w9x_test/w9x.ps1）
-- 约定：脚本的**第一个 vararg** = 结果文件路径（harness 传入）。
--       ❗AutoDbg 里没有 `arg` 全局：脚本参数是主 chunk 的 vararg（state.cpp 把 argv[2..]
--       压栈后 `lua_pcall(L, argc-2, ...)`）→ 用 `local out = ...`，不是 `arg[2]`。
-- 结果一律写文件：guest 里没有控制台，`WriteConsole` 路径不可用（§2.4 分流后重定向能抓到，
-- 但 vmrun 抓不到）。
local out = ...
if not out then return end
local f = io.open(out, 'w')
if not f then return end
f:write('flavor=' .. (jit and 'luajit' or 'lua55') .. '\n')
f:write('globals=' .. (inspect and 'inspect,jsonx,moses' or 'none') .. '\n')
f:write('ok\n')
f:close()
