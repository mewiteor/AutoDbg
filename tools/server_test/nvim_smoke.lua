-- =============================================================================
-- tools/server_test/nvim_smoke.lua —— neovim 客户端的无头验收（T5：raw TCP）
--
-- 由 `nvim_smoke.ps1` 用 `nvim --headless -u NONE --cmd "set rtp+=<repo>/nvim"` 跑。
-- 环境变量（PS 侧给）：AUTODBG_HOST / AUTODBG_PORT / AUTODBG_EXE / AUTODBG_TOKEN_PORT /
--   AUTODBG_TOKEN / AUTODBG_REPL_PORT / AUTODBG_EVENT_TARGET。
-- 覆盖：
--   ① 远程执行（`script` 帧 → `out` → `result` → `end`）+ 结果 buffer
--   ② 异步路径不爆 `E5560`（uv fast event context）
--   ③ 语法错误 → `error` 文本 + quickfix 定位到行
--   ④ `M.cancel`（控制帧，另一条连接）能打断正在跑的脚本
--   ⑤ `M.health` / `M.version`（`info` 帧）
--   ⑥ 文件 I/O 走帧：`open_file` 写 → 读回（`open`/`data`/`close`/`read_req`）
--   ⑨ T7 §12.3 ② 的 REPL（连第三台 server）：跨命令 `lua_State` 存活 + `out.id` 归因
--      + `result.id` = 该行的 Lua 错误码 + `event` 帧（16）渲染进 scratch buffer
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
check("health.proto", type(h.info) == "table" and h.info.proto == 2, "proto=" .. tostring(h.info and h.info.proto))

local v = ad.version({ sync = true })
check("version.ok", v.ok == true, "error=" .. tostring(v.error))
check("version.abi", type(v.info) == "table" and v.info.abi == 5, "abi=" .. tostring(v.info and v.info.abi))

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

-- ------------------------------------------------ ⑧ 默认配置（sync_timeout_ms = 0）+ token
-- ❗`vim.wait(0, ...)` 是「不等待」而不是「一直等」：默认配置曾让每条控制命令都 1 ms 后报
--   timeout。这里用没有任何超时设置的 setup 复刻默认情况。
set_buffer({ 'print("default-cfg")' }, "smoke_default.lua")
ad.setup({
  mode = "remote",
  host = host,
  port = port,
  -- 故意不给 sync_timeout_ms（默认 0）
  open = "none",
  focus = false,
  lsp = { enable = false },
  snippets = { enable = false },
})
local dh = ad.health({ sync = true })
check("default.health-no-timeout", dh.ok == true, "body=" .. tostring(dh.body):sub(1, 80))
local dv = ad.version({ sync = true })
check("default.version-no-timeout", dv.ok == true, "error=" .. tostring(dv.error))
local dr = ad.run({ sync = true })
check("default.run-sync", dr.ok == true and dr.exit == 0, "exit=" .. tostring(dr.exit) .. " err=" .. tostring(dr.error))

-- token 转发：用一台**要求认证**的 server（PS 侧在 port+1 起的）
local tport = tonumber(os.getenv("AUTODBG_TOKEN_PORT") or "0")
if tport and tport > 0 then
  ad.setup({
    mode = "remote",
    host = host,
    port = tport,
    token = os.getenv("AUTODBG_TOKEN") or "",
    open = "none",
    focus = false,
    lsp = { enable = false },
    snippets = { enable = false },
  })
  local tv = ad.version({ sync = true })
  check("token.accepted", tv.ok == true, "error=" .. tostring(tv.error))

  ad.setup({
    mode = "remote",
    host = host,
    port = tport,
    token = "wrong-token-0000",
    open = "none",
    focus = false,
    lsp = { enable = false },
    snippets = { enable = false },
  })
  local tb = ad.version({ sync = true })
  check("token.rejected", tb.ok == false and tostring(tb.error):find("unauthorized", 1, true) ~= nil,
    "error=" .. tostring(tb.error))
else
  -- 没有第二台 server（带 token）：跳过 token 用例
  print("[skip] token.*（PS 侧没起带 token 的 server）")
end

-- ------------------------------------------------ ⑨ REPL（T7 §12.3 ②：连已经在跑的 `--repl --server`）
-- 第三台 server（PS 侧起的）。命令号从 1 开始递增；这里把「语法错」那条排在第 5 条，
-- 这样 `result.id`（= 错误码 3）与命令号（= 5）能分开，断言才不是在赌巧合。
local rport = tonumber(os.getenv("AUTODBG_REPL_PORT") or "0")
if rport and rport > 0 then
  ad.setup({
    mode = "remote",
    host = host,
    port = rport,
    open = "none",
    focus = false,
    sync_timeout_ms = 30000,
    lsp = { enable = false },
    snippets = { enable = false },
    repl = { read_timeout_ms = 30000 },   -- `d:pump` 可能跑几秒，用默认 5000 会把它断掉
  })

  local ropen = ad.repl_open({ sync = true })
  print(("[info] repl.open  buf=%s  %s:%s  ok=%s"):format(
    tostring(ropen and ropen.buf), tostring(ropen and ropen.host), tostring(ropen and ropen.port),
    tostring(ropen and ropen.ok)))

  local function detail(r)
    if not r then
      return "nil"
    end
    return ("cmd=%d code=%s out=[%s]"):format(r.cmd, tostring(r.code), tostring(r.out))
  end

  local c1 = ad.repl_eval("x = 41", { sync = true })
  local c2 = ad.repl_eval("= x + 1", { sync = true })
  check("repl.cross-command-state",
    ropen ~= nil and ropen.ok == true and c1 ~= nil and c2 ~= nil
      and c1.cmd == 1 and c2.cmd == 2 and c2.ok and c2.code == 0
      and (c2.out or ""):find("42", 1, true) ~= nil
      and c2.outs and #c2.outs > 0 and c2.outs[1].id == c2.cmd,
    ("open=[%s] "):format(tostring(ropen and ropen.error)) .. detail(c1) .. " | " .. detail(c2) ..
    " out.id=" .. tostring(c2 and c2.outs and c2.outs[1] and c2.outs[1].id))

  -- `event` 帧：`dbg.create` 的初始 int 3（与 e2e_tcp.ps1 的 `proto.event-frame` 同一个源）
  local target = os.getenv("AUTODBG_EVENT_TARGET") or ""
  local c3 = ad.repl_eval(string.format("d = dbg.create{ imagepath = %q, cmdline = '/c exit' }", target), { sync = true })
  local c4 = ad.repl_eval("for i = 1, 12 do local ev = d:pump(2000); if ev == nil then break end; d:cont(); if #d:hits() > 0 then break end end; print('hits=' .. #d:hits())", { sync = true })
  local ev = (c4 and c4.events and c4.events[1]) or nil
  local btext = ""
  if ropen and ropen.buf then
    btext = table.concat(vim.api.nvim_buf_get_lines(ropen.buf, 0, -1, false), "\n")
  end
  local evline = nil
  if c4 then
    for _, l in ipairs(vim.split(btext, "\n", { plain = true })) do
      if l:find(("[event] cmd=%d type=1"):format(c4.cmd), 1, true) then
        evline = l
        break
      end
    end
  end
  check("repl.event-render",
    ev ~= nil and ev.id == c4.cmd and ev.type == 1 and evline ~= nil
      and evline:find("pid=" .. tostring(ev.pid), 1, true) ~= nil
      and evline:find("tid=" .. tostring(ev.tid), 1, true) ~= nil
      and evline:find("rva=" .. tostring(ev.rva), 1, true) ~= nil,
    "target=[" .. target .. "] create=[" .. tostring(c3 and (c3.out or c3.error)) ..
    "] pump=[" .. tostring(c4 and (c4.out or c4.error)) .. "] line=[" .. tostring(evline) .. "]")

  local c5 = ad.repl_eval("local y = ", { sync = true })
  check("repl.error-code",
    c5 ~= nil and c5.ok == false and c5.code == 3 and c5.cmd ~= 3,
    "cmd=" .. tostring(c5 and c5.cmd) .. " result.id=" .. tostring(c5 and c5.code) ..
    " err=[" .. tostring(c5 and c5.err) .. "]")

  ad.repl_eval("d:kill()", { sync = true })   -- 收尾：会话按归属 detach/kill（D54 安全网）
  ad.repl_close()
else
  print("[skip] repl.*（PS 侧没起 `--repl --server`）")
end

print(("nvim_smoke: pass=%d fail=%d"):format(pass, fail))
vim.cmd(fail > 0 and "cq" or "qa!")
