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
--       text 格式、token 缺失时的 401 诊断路径、client 文件 I/O，
--       脚本头标记（modeline）、LuaLS 注解库与 LuaSnip 片段（后三项不需要 server）。
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

-- ---------------------------------------------------------------- 脚本头标记（不需要 server）
-- 远程地址：setup 的 url 是默认值，脚本头部的 `--!autodbg ...` 覆盖它（换机器只改那一行）
local modeline = require("autodbg.modeline")
local ml = modeline.from_text("--!autodbg url=http://192.168.1.10:8642 token=abc\nprint(1)\n")
check("modeline.marker", ml.found == true)
check("modeline.url", ml.url == "http://192.168.1.10:8642", tostring(ml.url))
check("modeline.token", ml.token == "abc", tostring(ml.token))
local ml2 = modeline.from_text("-- header\n-- autodbg: host=10.0.0.5 port=9000\n")
check("modeline.host-port", ml2.found == true and ml2.host == "10.0.0.5" and ml2.port == "9000",
  tostring(ml2.host) .. ":" .. tostring(ml2.port))
check("modeline.merge-url", modeline.merge_url("http://127.0.0.1:8642", ml) == "http://192.168.1.10:8642")
check("modeline.merge-host-port", modeline.merge_url("http://127.0.0.1:8642", ml2) == "http://10.0.0.5:9000")
check("modeline.none", modeline.from_text("-- just a comment\nprint(1)\n").found == false)
check("modeline.not-comment", modeline.from_text("local s = 'autodbg: url=x'\n").found == false)

-- 无名 buffer 也要能判定（插件不依赖文件名）
local ubuf = vim.api.nvim_create_buf(false, true)
vim.api.nvim_buf_set_lines(ubuf, 0, -1, false, { "--!autodbg port=4321", "Run({})" })
local ub = modeline.from_buf(ubuf)
check("modeline.unnamed-buffer", ub.found == true and ub.port == "4321", tostring(ub.port))

-- 标记放文件末尾也算（与 vim 的 modeline 同口径：前 5 行 / 后 5 行）
local tbuf = vim.api.nvim_create_buf(false, true)
local tlines = {}
for _ = 1, 30 do
  tlines[#tlines + 1] = "print(1)"
end
tlines[#tlines + 1] = "--!autodbg port=4444"
vim.api.nvim_buf_set_lines(tbuf, 0, -1, false, tlines)
check("modeline.tail", modeline.from_buf(tbuf).port == "4444")

-- `:AutoDbgHeader` 的实现（没装 LuaSnip 时的替代品）
local hbuf = vim.api.nvim_create_buf(false, true)
adbg.insert_header({ bufnr = hbuf })
local hline = vim.api.nvim_buf_get_lines(hbuf, 0, 1, false)[1] or ""
check("header.insert", hline == "--!autodbg url=" .. url, hline)
check("header.idempotent", adbg.insert_header({ bufnr = hbuf }) == false)

-- run() 会用标记里的地址（指向一个必然连不上的端口 → 只验证优先级与 res.url）
set_buffer({ "--!autodbg url=http://127.0.0.1:9", 'print("never")' }, "marker-url.lua")
local mres = adbg.run({ sync = true, open = "none" })
check("modeline.run-url", mres.url == "http://127.0.0.1:9" and mres.mode == "modeline",
  tostring(mres.url) .. " mode=" .. tostring(mres.mode))
check("modeline.run-unreachable", mres.status == 0 and mres.ok == false, "status=" .. tostring(mres.status))

-- ---------------------------------------------------------------- LuaLS 注解库（不需要 server）
-- 取代模块侧的 generate_templete：插件自带 addons/autodbg/{config.json,library/*.lua}
local lsp = require("autodbg.lsp")
local addon = lsp.addon_dir()
check("lsp.addon-dir", vim.fn.isdirectory(addon) == 1, addon)
check("lsp.addon-config", vim.fn.filereadable(addon .. "/config.json") == 1)
for _, f in ipairs({ "lualib", "variant", "callbacks", "debugger", "gui", "file_io" }) do
  local p = addon .. "/library/" .. f .. ".lua"
  local head = vim.fn.filereadable(p) == 1 and (vim.fn.readfile(p, "", 1)[1] or "") or ""
  check("lsp.meta." .. f, head == "---@meta", head)
end

-- ---------------------------------------------------------------- LuaSnip 片段（不需要 server）
local snippets = require("autodbg.snippets")
package.loaded["luasnip"] = nil
package.loaded["autodbg.snippets"] = nil
snippets = require("autodbg.snippets")
check("snippets.no-luasnip", snippets.get() == nil and snippets.add("lua") == false)

-- 假 LuaSnip：只实现片段构造函数，用来证明片段能构造、触发词齐全、展开文本是合法 Lua
local built, added = {}, {}
package.loaded["luasnip"] = {
  snippet = function(o, nodes)
    built[#built + 1] = { trig = o.trig, nodes = nodes }
    return { trig = o.trig }
  end,
  text_node = function(t)
    return { t = t }
  end,
  insert_node = function(pos, v)
    return { i = pos, v = v }
  end,
  choice_node = function(pos, choices)
    return { c = pos, choices = choices }
  end,
  add_snippets = function(ft, list, opts)
    added.ft, added.count, added.key = ft, #list, opts and opts.key
    return true
  end,
}
package.loaded["autodbg.snippets"] = nil
snippets = require("autodbg.snippets")
local slist = snippets.get()
check("snippets.count", type(slist) == "table" and #slist >= 10, tostring(slist and #slist))
check("snippets.add", snippets.add("lua") == true and added.ft == "lua" and added.key == "autodbg",
  tostring(added.ft) .. "/" .. tostring(added.key))

local seen, uniq = {}, {}
for _, s in ipairs(built) do
  if not seen[s.trig] then
    seen[s.trig] = true
    uniq[#uniq + 1] = s
  end
end
for _, want in ipairs({ "adbg", "adrun", "adbp", "adrunbp", "adhw", "adctx", "admem", "adstack",
  "adexcept", "adgui", "adclick", "adfile" }) do
  check("snippets.trig." .. want, seen[want] == true)
end

-- 渲染成文本再 load()：片段里的引号/括号写错会在这里变红（曾把 "png" 写坏过一次）
local function render(nodes)
  local out = {}
  for _, n in ipairs(nodes or {}) do
    if n.t ~= nil then
      -- LuaSnip 的 t{"a","b"} 是**两行**（元素之间自动换行，末尾不再加）
      out[#out + 1] = type(n.t) == "table" and table.concat(n.t, "\n") or n.t
    elseif n.i ~= nil then
      out[#out + 1] = tostring(n.v or "")
    elseif n.c ~= nil then
      local first = n.choices and n.choices[1]
      out[#out + 1] = first and tostring(first.t or "") or ""
    end
  end
  return table.concat(out)
end
for _, s in ipairs(uniq) do
  local chunk, err = load(render(s.nodes), "snippet:" .. s.trig)
  check("snippets.valid-lua." .. s.trig, chunk ~= nil, err and tostring(err) or "")
end
package.loaded["luasnip"] = nil
package.loaded["autodbg.snippets"] = nil
-- ---------------------------------------------------------------- /health
-- 注意：health/cancel 也尊重**当前 buffer** 的标记，所以先换回一个没有标记的 buffer
-- （上一个用例留了一个指向 127.0.0.1:9 的标记）。
set_buffer({ "print(1)" }, "health.lua")
local h = adbg.health({ sync = true })
check("health.mode", h.mode == "setup", tostring(h.mode))
check("health.status", h.status == 200, "status=" .. tostring(h.status))
check("health.body", (h.body or ""):find('"ok":true', 1, true) ~= nil or (h.body or ""):find('"ok": true', 1, true) ~= nil,
  tostring(h.body))

io.write(("\nnvim_smoke: pass=%d fail=%d\n"):format(pass, fail))
if fail > 0 then
  vim.cmd("cquit 1")
else
  vim.cmd("qall!")
end
