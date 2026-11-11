-- =============================================================================
-- lualib/mcp.lua —— MCP 的 JSON-RPC 2.0 分发（**在会话的 Lua 状态里运行**）
--
-- 来源：新写（plan_server.md S6 / §8.2，决策 D28）。
-- 为什么放在 Lua 层：`jsonx` 已内置 encode+decode（C 侧不写 JSON 解析器），
--   而且工具实现直接调现有 Lua API（`gui.*`、`Run`、`ReadMemory`…），
--   于是「AI 能做的事」与「用户脚本能做的事」天然是同一条路径。
-- 调用方式：C 侧（`session_mcp_dispatch`）取全局 `mcp`，调 `mcp.dispatch(body)`：
--   返回 JSON 字符串 = 要回给客户端的响应；返回 nil = 这条是通知（无需响应）。
-- 协议：MCP 2024-11-05 的 streamable HTTP 子集（initialize / tools/list / tools/call /
--   ping / notifications）。会话由 C 侧的 `Mcp-Session-Id` 头负责（§8.3）。
--
-- ❗工具集口径（2026 修订）：**一次 tools/call = 一次 Lua 执行（串行）**。
--   `Run`/`Attach` 会阻塞到目标退出/Detach，所以**不提供**“单独读内存/下断点”的交互式工具
--   （曾经的 dbg_run / dbg_mem_read / dbg_breakpoint_set 因为拿不到活会话、 且参数形状不对，
--   实际是死工具，已删除）。调试逻辑请写在 `lua_eval` 的场景脚本里，在 Attach/Run 的回调内做。
--   现工具：lua_eval、dbg_status、gui_screenshot/windows/find/click/type/press/text/set_text/post。
-- =============================================================================

local M = {}

local json = jsonx

-- 协议版本：客户端要求什么就回什么（本实现只用与版本无关的 tools 能力）
local DEFAULT_PROTOCOL = "2024-11-05"
local SERVER_NAME      = "AutoDbg"
-- 版本号来自 C 侧 `_autodbg_info()`（构建期 git describe），不再写死
local SERVER_VERSION   = tostring((type(_autodbg_info) == "function" and _autodbg_info().version) or "unknown")

---------------------------------------------------------------------
-- 小工具
---------------------------------------------------------------------
local function b64(s)
  local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local out = {}
  local n = #s
  local i = 1
  while i <= n do
    local a = s:byte(i) or 0
    local b = s:byte(i + 1)
    local c = s:byte(i + 2)
    out[#out + 1] = alphabet:sub(math.floor(a / 4) + 1, math.floor(a / 4) + 1)
    out[#out + 1] = alphabet:sub((a % 4) * 16 + (b and math.floor(b / 16) or 0) + 1,
                                (a % 4) * 16 + (b and math.floor(b / 16) or 0) + 1)
    if b then
      out[#out + 1] = alphabet:sub((b % 16) * 4 + (c and math.floor(c / 64) or 0) + 1,
                                  (b % 16) * 4 + (c and math.floor(c / 64) or 0) + 1)
    else
      out[#out + 1] = "="
    end
    if c then
      out[#out + 1] = alphabet:sub(c % 64 + 1, c % 64 + 1)
    else
      out[#out + 1] = "="
    end
    i = i + 3
  end
  return table.concat(out)
end

local function text(s)
  return { { type = "text", text = tostring(s) } }
end

-- 宿主身份（C 侧注册的 `_autodbg_info()`）；拿不到就降级成 unknown
local INFO = (type(_autodbg_info) == "function") and _autodbg_info() or {}

-- ---------------------------------------------------------------------------
-- 协程「yield 透传」（S7）：read_file/write_file 会在 Lua 里 yield 一个效果表，
-- 需要一路冒泡到 C 侧的协程（宿主把它转成 JSON 发给客户端）。因此工具调用
-- **不能**用 pcall 包（LuaJIT 下 yield 穿不过 pcall），改用 coroutine.resume 循环。
-- ---------------------------------------------------------------------------
local pack = function(...) return { n = select("#", ...), ... } end
local unpack = table.unpack or unpack

local function call_yielding(fn, ...)
  local co = coroutine.create(fn)
  local r = pack(coroutine.resume(co, ...))
  while coroutine.status(co) == "suspended" do
    -- 内层 yield 出来的效果原样向上 yield；宿主的回复回填给内层
    local reply = pack(coroutine.yield(unpack(r, 2, r.n)))
    r = pack(coroutine.resume(co, unpack(reply, 1, reply.n)))
  end
  return unpack(r, 1, r.n)
end
-- 捕获 print 输出（工具里 `print` 很自然，但它的输出走不到 JSON-RPC 响应里）
local function with_capture(fn, ...)
  local buf = {}
  local old = print
  print = function(...)
    local parts = {}
    for i = 1, select("#", ...) do
      parts[#parts + 1] = tostring((select(i, ...)))
    end
    buf[#buf + 1] = table.concat(parts, "\t")
  end
  local ok, err = pcall(fn, ...)
  print = old
  return ok, err, table.concat(buf, "\n")
end

---------------------------------------------------------------------
-- 工具表
---------------------------------------------------------------------
local tools = {}
local order = {}

local function define(name, description, schema, handler)
  tools[name] = { name = name, description = description, inputSchema = schema, handler = handler }
  order[#order + 1] = name
end

local function schema(props, required)
  return { type = "object", properties = props or {}, required = required or {} }
end

---------------------------------------------------------------------
-- 工具：表达式求值 / 调试
---------------------------------------------------------------------
define("lua_eval",
  "在 AutoDbg 会话里执行一段 Lua 代码（返回值和 print 输出都会带回）",
  schema({ code = { type = "string", description = "Lua 源码" },
           filename = { type = "string", description = "仅用于错误信息里的文件名" } },
         { "code" }),
  function(args)
    local filename = args.filename or "mcp_eval.lua"
    local chunk, load_err = load(args.code, "@" .. filename)
    if not chunk then
      error(load_err, 0)
    end

    -- print 捕获（不用 pcall：yield 要能穿过去，见 call_yielding 的说明）
    local buf = {}
    local old_print = print
    print = function(...)
      local parts = {}
      for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring((select(i, ...)))
      end
      buf[#buf + 1] = table.concat(parts, "\t")
    end

    local r = pack(call_yielding(chunk, unpack(args.args or {})))
    print = old_print

    local parts = {}
    local printed = table.concat(buf, "\n")
    if printed ~= "" then parts[#parts + 1] = printed end
    if r[1] ~= true then
      parts[#parts + 1] = "error: " .. tostring(r[2])
    elseif r.n >= 2 and r[2] ~= nil then
      parts[#parts + 1] = tostring(r[2])
    end
    return text(table.concat(parts, "\n"))
  end)

define("dbg_status",
  "报告宿主身份与调试模型：版本/ABI/flavor/variant/pid/client_io，以及“本 MCP 不提供交互式暂停”的说明",
  schema({}, {}),
  function()
    local info = (type(_autodbg_info) == "function") and _autodbg_info() or {}
    local lines = {
      "version   = " .. tostring(info.version or SERVER_VERSION),
      "source    = " .. tostring(info.version_source or "unknown"),
      "abi       = " .. tostring(info.abi or "?"),
      "flavor    = " .. tostring(info.flavor or _VERSION),
      "variant   = " .. tostring(info.variant or "?"),
      "runtime   = " .. tostring(_VERSION) .. (jit and (" (" .. tostring(jit.version) .. ")") or ""),
      "server_pid= " .. tostring(info.pid or "?"),
      "client_io = " .. tostring(info.client_io == true),
      "",
      "模型：一次 tools/call = 一次 Lua 执行（串行）。调试必须在脚本里用 Attach/Run 的回调做，",
      "      Run/Attach 会阻塞到目标退出或 Detach；**没有**“暂停后逐条读内存/下断点”的交互式接口。",
      "写文件：流式模式下用 open_file + f:write（回调里也能用）。",
    }
    return text(table.concat(lines, "\n"))
  end)

---------------------------------------------------------------------
-- 工具：GUI（D23 的 `gui` 表）
---------------------------------------------------------------------
define("gui_screenshot",
  "截屏（默认整屏；也可指定区域或窗口），返回 PNG 图片内容",
  schema({ x = { type = "integer" }, y = { type = "integer" },
           w = { type = "integer" }, h = { type = "integer" },
           hwnd = { type = "integer" }, scale = { type = "number" } }, {}),
  function(a)
    local shot = gui.screenshot({ x = a.x, y = a.y, w = a.w, h = a.h, hwnd = a.hwnd,
                                  scale = a.scale or 1, format = "png" })
    if not shot then error("screenshot failed") end
    return { { type = "image", data = b64(shot.data), mimeType = "image/png" },
             { type = "text", text = string.format("%dx%d png", shot.w, shot.h) } }
  end)

define("gui_windows",
  "枚举可见窗口（hwnd/pid/标题/类名/矩形）",
  schema({}, {}),
  function()
    local out = {}
    for _, w in ipairs(gui.windows()) do
      if w.visible then
        out[#out + 1] = string.format("%d  pid=%d  %dx%d @%d,%d  [%s]  %s",
          w.hwnd, w.pid, w.w, w.h, w.x, w.y, tostring(w.class), tostring(w.title))
      end
    end
    return text(table.concat(out, "\n"))
  end)

define("gui_find",
  "按标题/类名/pid 找窗口，返回 hwnd",
  schema({ title = { type = "string" }, ["class"] = { type = "string" },
           pid = { type = "integer" } }, {}),
  function(a)
    local h = gui.find({ title = a.title, ["class"] = a["class"], pid = a.pid })
    if not h then error("window not found") end
    return text(string.format("hwnd=%d  title=%s", h, gui.text(h)))
  end)

define("gui_click",
  "鼠标点击（可指定坐标）",
  schema({ x = { type = "integer" }, y = { type = "integer" },
           button = { type = "string" }, times = { type = "integer" } }, {}),
  function(a)
    local ok
    if a.x and a.y then
      ok = gui.click(a.x, a.y, a.button or "left", a.times or 1)
    else
      ok = gui.click(a.button or "left", a.times or 1)
    end
    if not ok then error("click failed") end
    return text("clicked")
  end)

define("gui_type",
  "键盘输入一段文本（ASCII 走真实键路径；非 ASCII 走 Unicode 注入）",
  schema({ text = { type = "string" }, interval = { type = "integer" } }, { "text" }),
  function(a)
    if not gui.write(a.text, a.interval or 0) then error("type failed") end
    return text("typed " .. #a.text .. " bytes")
  end)

define("gui_press",
  "按键（支持 \"enter\"/\"f5\"/\"ctrl+c\" 这类写法）",
  schema({ key = { type = "string" }, times = { type = "integer" } }, { "key" }),
  function(a)
    if not gui.press(a.key, a.times or 1) then error("press failed") end
    return text("pressed " .. a.key)
  end)

define("gui_text",
  "读窗口/控件的文本",
  schema({ hwnd = { type = "integer" } }, { "hwnd" }),
  function(a) return text(gui.text(a.hwnd)) end)

define("gui_set_text",
  "设置窗口/控件的文本",
  schema({ hwnd = { type = "integer" }, text = { type = "string" } }, { "hwnd", "text" }),
  function(a)
    if not gui.set_text(a.hwnd, a.text) then error("set_text failed") end
    return text("ok")
  end)

define("gui_post",
  "向窗口 PostMessage（异步，不阻塞）",
  schema({ hwnd = { type = "integer" }, msg = { type = "integer" },
           wparam = { type = "integer" }, lparam = { type = "integer" } }, { "hwnd", "msg" }),
  function(a)
    if not gui.post(a.hwnd, a.msg, a.wparam or 0, a.lparam or 0) then error("post failed") end
    return text("posted")
  end)

---------------------------------------------------------------------
-- JSON-RPC
---------------------------------------------------------------------
local function error_obj(code, message, data)
  local e = { code = code, message = message }
  if data ~= nil then e.data = data end
  return e
end

local function respond(id, result)
  return json.encode({ jsonrpc = "2.0", id = id, result = result })
end

local function respond_error(id, code, message, data)
  return json.encode({ jsonrpc = "2.0", id = id, error = error_obj(code, message, data) })
end

local function handle_tools_call(id, params)
  local name = params and params.name
  local tool = name and tools[name]
  if not tool then
    return respond_error(id, -32602, "unknown tool: " .. tostring(name))
  end
  local args = params.arguments or {}
  -- 用 call_yielding 而不是 pcall：工具内部可能要 yield（client 文件 I/O）
  local r = pack(call_yielding(tool.handler, args))
  if r[1] ~= true then
    -- 工具执行失败按 MCP 约定放在 result.isError 里（而不是 JSON-RPC error）
    return respond(id, { content = text("tool error: " .. tostring(r[2])), isError = true })
  end
  return respond(id, { content = r[2] })
end

local function handle(msg)
  local id = msg.id
  local method = msg.method
  local params = msg.params

  -- 通知（无 id）一律不响应
  if id == nil then
    return nil
  end

  if method == "initialize" then
    local pv = (params and params.protocolVersion) or DEFAULT_PROTOCOL
    return respond(id, {
      protocolVersion = pv,
      capabilities = { tools = { listChanged = false } },
      serverInfo = { name = SERVER_NAME, version = SERVER_VERSION },
    })
  elseif method == "ping" then
    return respond(id, {})
  elseif method == "tools/list" then
    local list = {}
    for _, n in ipairs(order) do
      local t = tools[n]
      list[#list + 1] = { name = t.name, description = t.description, inputSchema = t.inputSchema }
    end
    return respond(id, { tools = list })
  elseif method == "tools/call" then
    return handle_tools_call(id, params)
  end

  return respond_error(id, -32601, "method not found: " .. tostring(method))
end

---------------------------------------------------------------------
-- 入口
---------------------------------------------------------------------
--- 处理一条 JSON-RPC 消息（字符串）。
-- @return JSON 字符串（响应）或 nil（通知/空体）
function M.dispatch(body)
  if type(body) ~= "string" or body == "" then
    return respond_error(json.null, -32700, "parse error: empty body")
  end

  local ok, msg = pcall(json.decode, body)
  if not ok then
    return respond_error(json.null, -32700, "parse error: " .. tostring(msg))
  end

  -- 批量请求（JSON 数组）
  if type(msg) == "table" and #msg > 0 then
    local out = {}
    for _, one in ipairs(msg) do
      local r = handle(one)
      if r then out[#out + 1] = r end
    end
    if #out == 0 then return nil end
    return "[" .. table.concat(out, ",") .. "]"
  end

  if type(msg) ~= "table" then
    return respond_error(json.null, -32600, "invalid request")
  end
  return handle(msg)
end

--- 工具名列表（给 `/mcp` 的调试/自检用）
function M.tool_names()
  return order
end

return M
