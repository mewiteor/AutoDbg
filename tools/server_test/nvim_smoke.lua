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

-- ---------------------------------------------------------------- client 文件 I/O
-- ① 流式（默认 `stream = true`）：`open_file` + `f:write` 边跑边写到**本机**文件
--    （不再靠 /effects 轮次回调，所以回调里也能用；这里先在主 chunk 里验证字节与长度）
local ioPath = vim.fn.tempname() .. "-adbg-stream.bin"
set_buffer({ 'local p = [==[' .. ioPath .. ']==]',
             'local f = open_file(p, "wb")',
             'print("open", f ~= nil)',
             'f:write("abc", "\\0", "def")',
             'f:write(string.rep("Q", 200000))',
             'f:flush()',
             'f:close()',
             'print("done-write")' }, "stream_io.lua")
local ioRes = adbg.run({ sync = true, open = "none" })
check("io.stream.ok", ioRes.ok == true and ioRes.stream == true,
  "status=" .. tostring(ioRes.status) .. " err=" .. tostring(ioRes.error) .. " stream=" .. tostring(ioRes.stream))
check("io.stream.stdout", (ioRes.stdout or ""):find("open\ttrue", 1, true) ~= nil, "stdout=[" .. tostring(ioRes.stdout) .. "]")
local fh = io.open(ioPath, "rb")
local head, fsz = "", vim.fn.getfsize(ioPath)
if fh then
  head = fh:read(7) or ""
  fh:close()
end
check("io.stream.size", fsz == 200007, "size=" .. tostring(fsz))
check("io.stream.content", head == "abc\0def", "head=[" .. tostring((head:gsub("%z", "\\0"))) .. "]")
pcall(os.remove, ioPath)

-- ② 非流式（`stream = false`）：老的一次性 `write_file` / `read_file`（effects 协议）
adbg.setup({ url = url, format = "json", open = "none", stream = false })
local ioPath2 = vim.fn.tempname() .. "-adbg-nvim.bin"
set_buffer({ 'local p = [==[' .. ioPath2 .. ']==]',
             'print("w", write_file(p, "abc\\0def"))',
             'local s = read_file(p)',
             'print("r", #s, s == "abc\\0def")' }, "io.lua")
local ioRes2 = adbg.run({ sync = true, open = "none" })
check("io.ok", ioRes2.ok == true, "status=" .. tostring(ioRes2.status) .. " err=" .. tostring(ioRes2.error))
check("io.write", (ioRes2.stdout or ""):find("w\t7", 1, true) ~= nil, "stdout=[" .. tostring(ioRes2.stdout) .. "]")
check("io.read-binary", (ioRes2.stdout or ""):find("r\t7\ttrue", 1, true) ~= nil, "stdout=[" .. tostring(ioRes2.stdout) .. "]")
pcall(os.remove, ioPath2)
adbg.setup({ url = url, format = "json", open = "none" }) -- 回到默认（流式）

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
check("modeline.run-url", mres.url == "http://127.0.0.1:9" and mres.url_mode == "modeline" and mres.mode == "remote",
  tostring(mres.url) .. " url_mode=" .. tostring(mres.url_mode) .. " mode=" .. tostring(mres.mode))
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
-- ---------------------------------------------------------------- 本地 / 远程模式（modeline exe / arch）
-- 规则：能拿到 exe → 本地跑；拿不到（或标记的 exe 不存在）→ 回退远程。
local exe = os.getenv("AUTODBG_EXE")
local have_exe = exe ~= nil and exe ~= "" and vim.fn.filereadable(exe) == 1

local mlexe = modeline.from_text("--!autodbg exe=C:/x/AutoDbg_64.exe arch=64\n")
check("modeline.exe", mlexe.found == true and tostring(mlexe.exe):lower():find("autodbg_64%.exe") ~= nil, tostring(mlexe.exe))
check("modeline.arch-alias", mlexe.arch == "x64", tostring(mlexe.arch))
check("modeline.arch-x86", modeline.from_text("--!autodbg arch=i386\n").arch == "x86")
check("modeline.arch-from-exe", modeline.arch_from_exe("C:/x/AutoDbg_86.exe") == "x86")

local local_exe = require("autodbg.local")
local mode, _ = adbg.resolve_mode({ url = url, exe = "C:/definitely/not/here/AutoDbg_64.exe", mode = "auto" })
check("mode.missing-exe-falls-back", mode == "remote", tostring(mode))
check("mode.forced-remote", (adbg.resolve_mode({ url = url, exe = exe, mode = "remote" })) == "remote")
if have_exe then
  local m2, found = adbg.resolve_mode({ url = url, exe = exe, mode = "auto" })
  check("mode.local-exists", m2 == "local" and vim.fs.normalize(found or "") == vim.fs.normalize(exe), tostring(m2))
else
  ok("mode.local-exists", "skip（没有 AUTODBG_EXE）")
end

-- PATH 发现：把 exe 所在目录塞进 PATH，不写 exe= 也应走本地
if have_exe then
  local oldpath = vim.env.PATH
  vim.env.PATH = vim.fs.dirname(exe) .. ";" .. tostring(oldpath)
  local m3, f3, why3 = adbg.resolve_mode({ url = url, mode = "auto" })
  vim.env.PATH = oldpath
  check("mode.path-discovery", m3 == "local" and tostring(why3) == "PATH", tostring(m3) .. "/" .. tostring(why3))
else
  ok("mode.path-discovery", "skip（没有 AUTODBG_EXE）")
end

-- 真实本地执行：带 `--!autodbg` 标记的 buffer 直接跑 exe（launcher 会跳过这个非 flavor 标记）
if have_exe then
  set_buffer({ "--!autodbg exe=" .. exe .. " arch=x64", 'print("local-mode-ok")' }, "local.lua")
  local lres = adbg.run({ sync = true, open = "none" })
  check("local.mode", lres.mode == "local", tostring(lres.mode))
  check("local.ok", lres.ok == true and lres.exit == 0, "exit=" .. tostring(lres.exit) .. " err=" .. tostring(lres.stderr))
  check("local.stdout", (lres.stdout or ""):find("local-mode-ok", 1, true) ~= nil, "stdout=[" .. tostring(lres.stdout) .. "]")
  check("local.arch", lres.arch == "x64", tostring(lres.arch))
  local hres = adbg.health({ sync = true })
  check("local.health", hres.mode == "local" and tostring(hres.body):find("exe=", 1, true) ~= nil, tostring(hres.body))
else
  ok("local.mode", "skip（没有 AUTODBG_EXE）")
  ok("local.ok", "skip")
  ok("local.stdout", "skip")
  ok("local.arch", "skip")
  ok("local.health", "skip")
end

-- 标记里的 exe 不存在 → 回退远程（这里真的打到正在跑的 server）
set_buffer({ "--!autodbg exe=C:/nope/AutoDbg_64.exe", 'print("remote-fallback")' }, "fallback.lua")
local fres = adbg.run({ sync = true, open = "none" })
check("mode.fallback-remote", fres.mode == "remote" and fres.ok == true, tostring(fres.mode) .. "/" .. tostring(fres.exit))
check("mode.fallback-stdout", (fres.stdout or ""):find("remote-fallback", 1, true) ~= nil, tostring(fres.stdout))

-- LuaLS 注解库的架构目录（x86 没有 Xmm，x64 有）
check("lsp.arch.x86-dir", vim.fn.filereadable(lsp.library_dirs("x86")[2] .. "/context.lua") == 1)
check("lsp.arch.x64-dir", vim.fn.filereadable(lsp.library_dirs("x64")[2] .. "/context.lua") == 1)
local x86_ctx = table.concat(vim.fn.readfile(lsp.library_dirs("x86")[2] .. "/context.lua"), "\n")
local x64_ctx = table.concat(vim.fn.readfile(lsp.library_dirs("x64")[2] .. "/context.lua"), "\n")
check("lsp.arch.x86-fields", x86_ctx:find("---@field Cip", 1, true) ~= nil and x86_ctx:find("---@field Xmm0", 1, true) == nil)
check("lsp.arch.x64-fields", x64_ctx:find("---@field Xmm15", 1, true) ~= nil and x64_ctx:find("---@field VectorControl", 1, true) ~= nil)
local abuf = vim.api.nvim_create_buf(false, true)
vim.api.nvim_buf_set_lines(abuf, 0, -1, false, { "--!autodbg arch=x86" })
check("lsp.arch.resolve-x86", lsp.resolve_arch(abuf) == "x86")
local abuf2 = vim.api.nvim_create_buf(false, true)
vim.api.nvim_buf_set_lines(abuf2, 0, -1, false, { "--!autodbg" })
check("lsp.arch.resolve-default", lsp.resolve_arch(abuf2) == "x64")

-- 片段：本地标记 adbgl（触发词 + 展开文本是合法 Lua）
local lsnip = seen["adbgl"]
check("snippets.trig.adbgl", lsnip ~= nil)
if lsnip then
  local text = render(lsnip.nodes)
  check("snippets.valid-lua.adbgl", load(text) ~= nil, text:gsub("\n", " | "))
end

-- ---------------------------------------------------------------- /health
-- 注意：health/cancel 也尊重**当前 buffer** 的标记，所以先换回一个没有标记的 buffer
-- （上一个用例留了一个指向 127.0.0.1:9 的标记）。
set_buffer({ "print(1)" }, "health.lua")
local h = adbg.health({ sync = true })
check("health.mode", h.mode == "remote" and h.url_mode == "setup", tostring(h.mode) .. "/" .. tostring(h.url_mode))
check("health.status", h.status == 200, "status=" .. tostring(h.status))
check("health.body", (h.body or ""):find('"ok":true', 1, true) ~= nil or (h.body or ""):find('"ok": true', 1, true) ~= nil,
  tostring(h.body))

io.write(("\nnvim_smoke: pass=%d fail=%d\n"):format(pass, fail))
if fail > 0 then
  vim.cmd("cquit 1")
else
  vim.cmd("qall!")
end
