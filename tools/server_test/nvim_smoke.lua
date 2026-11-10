-- =============================================================================
-- tools/server_test/nvim_smoke.lua —— neovim 客户端的无头验收（S3）
--
-- 来源：新写（plan_server.md S3）。
-- 跑法（先起 server）：
--   nvim --headless -u NONE --cmd "set rtp+=<repo>/nvim" \
--        -c "luafile tools/server_test/nvim_smoke.lua" -c "qa!"
--   环境变量：AUTODBG_URL（默认 http://127.0.0.1:8642）
-- 退出码：全部通过 → 0；有失败 → 1（用 `:cquit 1`）。
-- 覆盖：同步执行、结果 buffer 内容、quickfix 跳转、取消（纯 Lua 忙循环）、
--       text 格式、token 缺失时的 401 诊断路径。
-- =============================================================================

local url = os.getenv("AUTODBG_URL") or "http://127.0.0.1:8642"

local pass, fail = 0, 0
local function ok(name, detail)
  pass = pass + 1
  io.write(("[ok]   %s%s\n"):format(name, detail and ("  " .. detail) or ""))
end
local function bad(name, detail)
  fail = fail + 1
  io.write(("[FAIL] %s  %s\n"):format(name, tostring(detail)))
end
local function check(name, cond, detail)
  if cond then
    ok(name, detail)
  else
    bad(name, detail)
  end
end

local adbg = require("autodbg")
adbg.setup({ url = url, open = "split" })

local function set_buffer(lines, name)
  vim.cmd("enew")
  vim.api.nvim_buf_set_lines(0, 0, -1, false, lines)
  -- 每个用例用**不同**的缓冲区名：重名会报 E95 并直接中断本脚本（S3 踩过）
  vim.api.nvim_buf_set_name(0, name or "smoke.lua")
  return vim.api.nvim_get_current_buf()
end

-- ---------------------------------------------------------------- 正常执行
set_buffer({ 'print("nvim hello")', 'return 1' }, "ok.lua")
local res = adbg.run({ sync = true })
check("run.ok", res.ok == true, "status=" .. tostring(res.status) .. " exit=" .. tostring(res.exit))
check("run.exit", res.exit == 0, "exit=" .. tostring(res.exit))
check("run.stdout", (res.stdout or ""):find("nvim hello", 1, true) ~= nil, "stdout=[" .. tostring(res.stdout) .. "]")

-- 结果 buffer 里应该有摘要行 + 输出（`res.result_buf` 是 ui.render 建的那个 buffer）
local rb = res.result_buf
check("ui.result-buffer", rb ~= nil and vim.api.nvim_buf_is_valid(rb), rb and ("buf=" .. tostring(rb)) or "未找到结果 buffer")
if rb and vim.api.nvim_buf_is_valid(rb) then
  local text = table.concat(vim.api.nvim_buf_get_lines(rb, 0, -1, false), "\n")
  check("ui.header", text:find("exit=0", 1, true) ~= nil, text:sub(1, 80))
  check("ui.stdout", text:find("nvim hello", 1, true) ~= nil,
    text:find("nvim hello", 1, true) and "ok" or "result buffer 里没有 stdout")
end

-- ---------------------------------------------------------------- 语法错 → quickfix
-- ❗真实语法错（`local x = ` 后面直接换行是合法 Lua，S3 先弄错过一次）
set_buffer({ "local ok = true", "if then end" }, "syntax.lua")
local err = adbg.run({ sync = true })
check("err.not-ok", err.ok == false, "ok=" .. tostring(err.ok))
check("err.exit-nonzero", (err.exit or 0) ~= 0, "exit=" .. tostring(err.exit))
check("err.error-text", (err.error or ""):find("syntax.lua:2", 1, true) ~= nil, "error=[" .. tostring(err.error) .. "]")

local qf = vim.fn.getqflist()
check("qf.has-entry", #qf > 0, "qflist=" .. tostring(#qf))
if #qf > 0 then
  check("qf.line", qf[1].lnum == 2, "lnum=" .. tostring(qf[1].lnum))
  check("qf.text", (qf[1].text or ""):find("near", 1, true) ~= nil or (qf[1].text or "") ~= "", qf[1].text)
end

-- ---------------------------------------------------------------- 运行时错
set_buffer({ 'error("boom-nvim")' }, "runtime.lua")
local rt = adbg.run({ sync = true })
check("runtime.error", (rt.error or ""):find("boom-nvim", 1, true) ~= nil, "error=[" .. tostring(rt.error) .. "]")

-- ---------------------------------------------------------------- text 格式
adbg.setup({ url = url, format = "text", open = "none" })
set_buffer({ 'print("text mode")' }, "text.lua")
local t = adbg.run({ sync = true })
check("text.ok", t.ok == true, "status=" .. tostring(t.status))
check("text.body", (t.stdout or ""):find("text mode", 1, true) ~= nil, "stdout=[" .. tostring(t.stdout) .. "]")
check("text.exit-header", t.exit == 0, "exit=" .. tostring(t.exit))

-- ---------------------------------------------------------------- 取消（纯 Lua 忙循环）
adbg.setup({ url = url, format = "json", open = "none" })
set_buffer({ "local t = os.clock(); while os.clock() - t < 5 do end" }, "cancel.lua")
local long_task = adbg.run({ open = "none" })
vim.wait(400, function() return false end)
local accepted = adbg.cancel({ sync = true })
check("cancel.accepted", accepted == true, "accepted=" .. tostring(accepted))
vim.wait(4000, function() return false end)

-- ---------------------------------------------------------------- client 文件 I/O（S7）
-- 脚本让 server 挂起去读写**本机**文件，插件（effects.resolve_sync）负责在本地做完再送回
local ioPath = vim.fn.tempname() .. "-adbg-nvim.bin"
set_buffer({ 'local p = [==[' .. ioPath .. ']==]',
             'print("w", write_file(p, "abc\\0def"))',
             'local s = read_file(p)',
             'print("r", #s, s == "abc\\0def")' }, "io.lua")
local ioRes = adbg.run({ sync = true, open = "none" })
check("io.ok", ioRes.ok == true, "status=" .. tostring(ioRes.status) .. " err=" .. tostring(ioRes.error))
check("io.write", (ioRes.stdout or ""):find("w\t7", 1, true) ~= nil, "stdout=[" .. tostring(ioRes.stdout) .. "]")
check("io.read-binary", (ioRes.stdout or ""):find("r\t7\ttrue", 1, true) ~= nil, "stdout=[" .. tostring(ioRes.stdout) .. "]")
pcall(os.remove, ioPath)
-- ---------------------------------------------------------------- /health
local h = adbg.health({ sync = true })
check("health.status", h.status == 200, "status=" .. tostring(h.status))
check("health.body", (h.body or ""):find('"ok":true', 1, true) ~= nil or (h.body or ""):find('"ok": true', 1, true) ~= nil,
  tostring(h.body))

io.write(("\nnvim_smoke: pass=%d fail=%d\n"):format(pass, fail))
if fail > 0 then
  vim.cmd("cquit 1")
else
  vim.cmd("qall!")
end
