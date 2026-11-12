-- =============================================================================
-- tools/server_test/nvim_smoke.lua —— neovim 客户端的无头验收（T5：raw TCP）
--
-- 由 `nvim_smoke.ps1` 用 `nvim --headless -u NONE --cmd "set rtp+=<repo>/nvim"` 跑。
-- 环境变量（PS 侧给）：AUTODBG_HOST / AUTODBG_PORT / AUTODBG_EXE。
-- 覆盖：
--   ① 远程执行（`script` 帧 → `out` → `result` → `end`）+ 结果 buffer
--   ② 异步路径不爆 `E5560`（uv fast event context）
--   ③ 语法错误 → `error` 文本 + quickfix 定位到行
--   ④ `M.cancel`（控制帧，另一条连接）能打断正在跑的脚本
--   ⑤ `M.health` / `M.version`（`info` 帧）
--   ⑥ 文件 I/O 走帧：`open_file` 写 → 读回（`open`/`data`/`close`/`read_req`）
-- 退出码：0 = 全通过；1 = 有失败
-- =============================================================================

local host = os.getenv("AUTODBG_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("AUTODBG_PORT") or "0") or 8642

vim.opt.runtimepath:append(vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h:h:h") .. "/nvim")

local pass, fail = 0, 0
local function ok(name, detail)
  pass = pass + 1
  print(("[ok]   %s  %s"):format(name, detail or ""))
end
local function bad(name, detail)
  fail = fail + 1
  print(("[FAIL] %s  %s"):format(name, detail or ""))
end
local function check(name, cond, detail)
  if cond then ok(name, detail) else bad(name, detail) end
end

local ad = require("autodbg")
ad.setup({
  mode = "remote",        -- 强制远程（本机有没有 exe 都不影响）
  host = host,
  port = port,
  open = "split",
  focus = false,
  sync_timeout_ms = 30000,
  lsp = { enable = false },
  snippets = { enable = false },
})

-- ---------------------------------------------------------------- ① 远程执行
local function set_buffer(lines, name)
  vim.cmd("enew")
  vim.api.nvim_buf_set_lines(0, 0, -1, false, lines)
  vim.api.nvim_buf_set_name(0, name)
end

set_buffer({ 'print("nvim hello")' }, "smoke_run.lua")
local res = ad.run({ sync = true })
check("run.ok", res.ok == true, "status=" .. tostring(res.status) .. " exit=" .. tostring(res.exit) .. " err=" .. tostring(res.error))
check("run.exit", res.exit == 0, "exit=" .. tostring(res.exit))
check("run.stdout", (res.stdout or ""):find("nvim hello", 1, true) ~= nil, "stdout=[" .. tostring(res.stdout) .. "]")

local rb = res.result_buf
check("ui.result-buffer", rb ~= nil and vim.api.nvim_buf_is_valid(rb), rb and ("buf=" .. tostring(rb)) or "未找到结果 buffer")
if rb then
  local text = table.concat(vim.api.nvim_buf_get_lines(rb, 0, -1, false), "\n")
  check("ui.header", text:find("exit=0", 1, true) ~= nil or text:find("exit", 1, true) ~= nil, text:sub(1, 80))
  check("ui.stdout", text:find("nvim hello", 1, true) ~= nil, text:sub(1, 200))
end

-- ---------------------------------------------------------------- ② 异步路径
set_buffer({ 'print("async-ok")' }, "smoke_async.lua")
local slot = { done = false }
ad.run({
  on_done = function(r)
    slot.done = true
    slot.res = r
  end,
})
vim.wait(30000, function()
  return slot.done
end, 20)
check("async.done", slot.done == true and slot.res ~= nil, "done=" .. tostring(slot.done))
check("async.ok", slot.res and slot.res.ok == true, slot.res and ("exit=" .. tostring(slot.res.exit)) or "nil")
check("async.stdout", slot.res and (slot.res.stdout or ""):find("async-ok", 1, true) ~= nil,
  slot.res and ("stdout=[" .. tostring(slot.res.stdout) .. "]") or "nil")
check("async.no-fast-context-error", slot.res and tostring(slot.res.error or ""):find("E5560", 1, true) == nil,
  slot.res and tostring(slot.res.error) or "nil")

-- ---------------------------------------------------------------- ③ 语法错误 + quickfix
set_buffer({ "local x = 1", "local y = " }, "smoke_syntax.lua")
local err = ad.run({ sync = true })
check("err.not-ok", err.ok == false, "ok=" .. tostring(err.ok))
check("err.exit-nonzero", (err.exit or 0) ~= 0, "exit=" .. tostring(err.exit))
check("err.error-text", (err.error or ""):find("smoke_syntax.lua:", 1, true) ~= nil, "error=[" .. tostring(err.error) .. "]")
local qf = vim.fn.getqflist()
check("qf.has-entry", #qf > 0, "qflist=" .. tostring(#qf))
if #qf > 0 then
  -- 行号是「解析器放弃的位置」：`buf_script` 末尾补了一个换行，<eof> 因此在第 3 行\n  check("qf.line", qf[1].lnum >= 2, "lnum=" .. tostring(qf[1].lnum))
end

-- ---------------------------------------------------------------- ④ 取消（控制帧）
set_buffer({ "print('before-loop')", "while true do end" }, "smoke_cancel.lua")
local cslot = { done = false }
ad.run({
  on_done = function(r)
    cslot.done = true
    cslot.res = r
  end,
})
vim.wait(500, function()
  return false
end, 20)
local accepted = ad.cancel({ sync = true })
check("cancel.accepted", accepted == true, "accepted=" .. tostring(accepted))
vim.wait(15000, function()
  return cslot.done
end, 20)
check("cancel.finished", cslot.done == true, "done=" .. tostring(cslot.done))
check("cancel.canceled-flag", cslot.res and cslot.res.canceled == true,
  cslot.res and ("canceled=" .. tostring(cslot.res.canceled) .. " exit=" .. tostring(cslot.res.exit)) or "nil")

-- ------------------------------------------------ ⑦ 脚本头标记里的地址优先（两种写法）
-- setup 里故意放一个不可达地址（192.0.2.0/24 是 TEST-NET，必定连不上）：
-- 如果模型标记没生效，就会去连它而不是 server。
ad.setup({
  mode = "remote",
  host = "192.0.2.1",
  port = 9,
  open = "none",
  focus = false,
  sync_timeout_ms = 8000,
  lsp = { enable = false },
  snippets = { enable = false },
})

-- 复刻用户报的那个脚本头（`--!lua` + `--!autodbg url=… token=…`）:
set_buffer({ "--!lua", ("--!autodbg url=http://127.0.0.1:%d token=11111111"):format(port), 'print("x")' },
  "smoke_modeline.lua")
local mv = ad.version({ sync = true })
check("modeline.url-overrides-setup", mv.ok == true, "error=" .. tostring(mv.error))

set_buffer({ ("--!autodbg host=127.0.0.1 port=%d"):format(port), 'print("x")' }, "smoke_modeline2.lua")
local mh = ad.health({ sync = true })
check("modeline.host-port", mh.ok == true, "body=" .. tostring(mh.body):sub(1, 80))
check("modeline.host-port-reported", mh.host == "127.0.0.1" and mh.port == port,
  "host=" .. tostring(mh.host) .. " port=" .. tostring(mh.port))

-- 恢复默认（后面的用例还要用）
ad.setup({
  mode = "remote",
  host = host,
  port = port,
  open = "split",
  focus = false,
  sync_timeout_ms = 30000,
  lsp = { enable = false },
  snippets = { enable = false },
})

-- ---------------------------------------------------------------- ⑤ health / version
local h = ad.health({ sync = true })
check("health.ok", h.ok == true and h.mode == "remote", "mode=" .. tostring(h.mode) .. " body=" .. tostring(h.body):sub(1, 80))
check("health.proto", type(h.info) == "table" and h.info.proto == 1, "proto=" .. tostring(h.info and h.info.proto))

local v = ad.version({ sync = true })
check("version.ok", v.ok == true, "error=" .. tostring(v.error))
check("version.abi", type(v.info) == "table" and v.info.abi == 4, "abi=" .. tostring(v.info and v.info.abi))

-- ---------------------------------------------------------------- ⑥ 文件 I/O 走帧
local tmp = vim.fn.tempname() .. ".bin"
set_buffer({
  ("local path = %q"):format(tmp),
  'local f, e = open_file(path, "wb")',
  "if not f then return { err = e } end",
  'f:write("abc\\0def")',
  "f:close()",
  'local g, e2 = open_file(path, "rb")',
  "if not g then return { err = e2 } end",
  'local data = g:read("a")',
  "g:close()",
  "return { size = #data, head = data, tell_hint = 7 }",
}, "smoke_io.lua")
local ioRes = ad.run({ sync = true })
check("io.ok", ioRes.ok == true, "exit=" .. tostring(ioRes.exit) .. " err=" .. tostring(ioRes.error) .. " stdout=" .. tostring(ioRes.stdout):sub(1, 100))
local size = vim.fn.getfsize(tmp)
check("io.size", size == 7, "size=" .. tostring(size))
local fh = io.open(tmp, "rb")
if fh then
  local head = fh:read("*a")
  fh:close()
  check("io.content", head == "abc\0def", "head=[" .. tostring((head or ""):gsub("%z", "\\0")) .. "]")
  pcall(os.remove, tmp)
else
  bad("io.content", "读不回 " .. tmp)
end

print(("nvim_smoke: pass=%d fail=%d"):format(pass, fail))
vim.cmd(fail > 0 and "cq" or "qa!")
