-- =============================================================================
-- lualib/autodbg_api.lua —— 给 AI（MCP 的 `lua_api` 工具）看的 AutoDbg Lua API 速查表
--
-- 为什么要它：MCP 只有 `lua_eval` 能真正干活，AI 必须知道「该怎么写这段 Lua」。
--   工具的 description 只能给一句话，这里给的是**可执行的最小示例 + 签名 + 地雷**。
-- 数据来源（改绑定层时同步这里）：`src/scripting/lua/registry.cpp` 的 29 个全局名、
--   `bindings.cpp` 的参数形状、`variant.cpp` 的 LuaVariant 方法、
--   `nvim/addons/autodbg/library/*.lua`（LuaLS 注解）与 `AGENTS.md` §12。
-- 用法（在 mcp.lua 里）：`autodbg_api.topics()` → 目录；`autodbg_api.get(name)` → 正文。
-- =============================================================================

local M = {}

-- ---------------------------------------------------------------------------
-- 目录（顺序就是推荐阅读顺序）
-- ---------------------------------------------------------------------------
local ORDER = {
  "model", "run", "attach", "breakpoints", "hwbp", "memory", "variant",
  "context", "symbols", "scan", "events", "gui", "files", "pitfalls", "recipes",
}

local SUMMARY = {
  model       = "执行模型：一次调用 = 一个脚本；Run/Attach 阻塞；一切都是回调",
  run         = "Run{...}：启动并调试一个新进程",
  attach      = "Attach{pid=...}：附加到已在运行的进程",
  breakpoints = "SetBreakpoint / SetBreakpoints / Del|Enable|DisableBreakpoint",
  hwbp        = "SetHardwareBreakpoint（DRx 硬件断点）",
  memory      = "ReadMemory / WriteMemory（注意 ReadMemory 返回 table）",
  variant     = "V(addr) / Var 短类型字母 / LuaVariant 与 U32()… 构造",
  context     = "GetContext / SetContext（Cax..Cip，x64 另有 R8-R15/Xmm）",
  symbols     = "GetModBase / FindFuncRva / Ea2ModnameRva / StackTrace / LoadStr …",
  scan        = "FindMemory（特征扫描）/ GetMemoryRegion（内存布局）",
  events      = "Run 的 on_* 事件回调（异常、模块加载、OutputDebugString…）",
  gui         = "gui.* 表：键鼠、窗口、控件、截图",
  files       = "open_file（流式，回调里可用）/ write_file（一次性）",
  pitfalls    = "已知地雷（务必读）",
  recipes     = "典型场景模板（下断点抓参数 / trace / dump / GUI）",
}

-- ---------------------------------------------------------------------------
-- 正文
-- ---------------------------------------------------------------------------
local BODY = {}

BODY.model = [==[
# 执行模型（先读这段）

- 一次 `lua_eval` = 执行一段 Lua（**串行**，同一时刻只有一个脚本在跑）。
- `Run{...}` / `Attach{...}` 是**阻塞**的：它们创建/附加目标并进入调试循环，
  一直跑到「目标退出」或脚本里调用 `Detach()`。所以**调试逻辑全部写在回调里**。
- 没有「暂停后逐条读内存」的交互式接口（没有 step/pause/continue）。想「跑几秒看看」，
  就在回调里用 `gui.sleep(ms)` 或直接用 `Run{...}` 跑到目标退出。
- 后台没有任何常驻调试会话：上一条 `lua_eval` 里 `Detach()` 之后进程就干净结束了。

最小骨架：

```lua
Run{                                  -- 或 Attach{ pid = 1234, ... }
  imagepath = [[C:\path\target.exe]],
  callback = function(pid, tid)        -- 入口断点（目标刚创建、还没跑）
    SetBreakpoint{
      modname  = "kernel32.dll",
      rva      = FindFuncRva("kernel32.dll", "CreateFileW"),
      callback = function(info)        -- 命中回调
        local c = GetContext(info.tid)
        print("called! rcx =", c.Ccx.hex)
      end,
    }
  end,
}
```

返回值/输出怎么回给工具调用方：
- `print(...)` 的输出会被捕获并放进工具结果（推荐用它输出结论）。
- 脚本的**第一个返回值**也会以 `tostring` 附带返回；结构化数据请
  `print(jsonx.encode(v))`（`jsonx` 是内置库，encode/decode）。
- 出错 → 工具结果 `isError = true`，带 `@文件名:行号` 的 Lua 错误。
]==]

BODY.run = [==[
# Run{ ... }：启动并调试新进程

必需：`imagepath`（或把完整命令行放进 `cmdline`）。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `imagepath` | string | 程序路径 |
| `cmdline` | string? | 命令行（含 imagepath 时可省略 imagepath） |
| `cwd` | string? | 工作目录 |
| `env` | table? | 环境变量 |
| `is_new_console` | boolean? | 是否新开控制台 |
| `use_scylla_hide` | boolean? | 是否启用 ScyllaHide（只有 modern 变体有） |
| `callback` | fun(pid, tid)? | **入口断点**回调（下断点、改内存都在这里做） |
| `exception_callback` | fun(info)? | 异常回调（info.type/address/…） |
| `on_*` | fun(opt)? | 其它事件，见 `lua_api` topic=`events` |

```lua
Run{
  cmdline = [[C:\Windows\System32\notepad.exe]],
  callback = function(pid, tid)
    print("pid", pid.val, "tid", tid.val)
    Detach()            -- 不想继续调就直接分离（目标会继续跑）
  end,
}
```

返回：目标退出/分离后返回，不返回值。
]==]

BODY.attach = [==[
# Attach{ ... }：附加到已运行的进程

```lua
Attach{ pid = 1234, callback = function()
  -- 附加成功后的第一站；在这里下断点
  SetBreakpoint{ modname = "kernel32.dll", rva = FindFuncRva("kernel32.dll", "CreateFileW"),
                 callback = function(info) print("hit"); Detach() end }
end }
```

- 老式写法 `Attach(function() ... end)` 也支持（只有回调）。
- 目标进程会**暂停**在附加点；`callback` 里做完事情后，正常返回 = 继续运行。
- Win9x 不支持 attach（只有 Run），这是系统限制。
]==]

BODY.breakpoints = [==[
# 断点（软件断点 = INT3）

```lua
-- 单个
SetBreakpoint{
  modname = "my.dll",
  rva     = FindFuncRva("my.dll", "SomeFunc"),   -- 或 U32("0x1234")
  is_single = true,                              -- 可选：命中一次后自动删除
  callback = function(info)
    -- info.ea / info.modname / info.rva / info.pid / info.tid
  end,
}

-- 批量（按模块 -> RVA -> 回调）
SetBreakpoints{
  ["my.dll"] = {
    ["0x1234"] = function(info) end,
    ["0x5678"] = function(info) end,
  },
}

DelBreakpoint("my.dll", U32("0x1234"))   -- 或 DelBreakpoint(ea)
EnableBreakpoint("my.dll", U32("0x1234"))
DisableBreakpoint("my.dll", U32("0x1234"))
```

- `rva` 必须是 **LuaVariant**：用 `FindFuncRva(...)` / `U32("0x1234")` / `U64(...)`，
  直接写 `0x1234`（裸 number）会被拒绝。
- 回调是**同步**执行在调试循环里：别在这里做长耗时的事（会卡住目标）。
]==]

BODY.hwbp = [==[
# 硬件断点（DRx）

```lua
SetHardwareBreakpoint{
  ea       = 0x401000,        -- 绝对地址（LuaVariant/V())
  index    = 0,               -- 可选：DR0..DR3
  type     = "e",             -- e=执行 / w=写入 / r=读写
  size     = 4,               -- 1/2/4（x86）；x64 还能 8
  callback = function(info) end,
}
DelHardwareBreakpoint(0)      -- 按 index 删除
```

硬件断点不需要改目标内存（对付自校验/CRC 的程序有用），但同时最多 4 个。
]==]

BODY.memory = [==[
# 内存读写

```lua
local bytes = ReadMemory(V(addr), U32(64))   -- ❗返回 **table of bytes**（1..n），不是字符串
print("first byte", bytes[1])
print(#bytes)

WriteMemory(V(addr), { 0x90, 0x90, 0x90 })   -- table of bytes，写回目标
```

- `ReadMemory(addr, size)`：`addr`/`size` 都是 **LuaVariant**（`V(x)` 或 `U32(x)`）。
- 要字符串：`local s = string.char(table.unpack(bytes))`；要 hex：`inspect(bytes)` 或自己格式化。
- 大块读取建议分片（每次 ≤ 一张页更稳）：`for i = 0, n-1, 0x1000 do ... end`。
- 内存布局：`GetMemoryRegion()` 返回 `{ BaseAddress, AllocationBase, RegionSize, State, Protect, Type }` 列表。
]==]

BODY.variant = [==[
# LuaVariant 与 Var（读目标内存的两种口味）

## 1) Var：`V(addr)` —— 按「地址 + 类型字母」读

```lua
V(addr)[0x10]            -- 把 addr+0x10 处的**指针**取出来，返回新的 Var
V(addr)[0x10]:to("i")    -- 在该指针处读 int32
V(addr)[0x10]:tos("d", 130)  -- 在该指针处读 130 个 double（返回 LuaVariant 数组）
V(addr):to("I", 0x20)    -- 在 addr+0x20 处读 uint32
V(addr):sto("i", 4, 0x20)-- 在 addr+0x20 处读 4 个 int32 的字符串表示
V(addr):write(U32(1), 0x20)   -- 写 uint32
V(addr):writes({1,2,3}, 0x20) -- 批量写
V(addr):swrite("abc", 0x20)   -- 写字节串
```

单字母类型（`Var:*` 用）：

| 字母 | 类型 | 字母 | 类型 |
| --- | --- | --- | --- |
| `c` | int8 | `b` | uint8 |
| `h` | int16 | `H` | uint16 |
| `i` | int32 | `I` | uint32 |
| `q` | int64 | `Q` | uint64 |
| `z` | size_t | `f` | float |
| `d` | double | | |

Var 还有只读字段：`hex` / `val` / `rawval` / `type` / `bin`。

## 2) LuaVariant：`U32(x)` / `I64(x)` / `F64(x)` … —— 值本身

- 构造：`U8 I8 U16 I16 U32 I32 U64 I64 F32 F64`（也接受 `"0x1234"` 字符串）。
- 方法：`:to("u32")` / `:as("i64")`（**全名**：i8/u8/i16/u16/i32/u32/i64/u64/f32/f64）。
- 字段：`.hex` / `.val` / `.rawval` / `.type` / `.bin`；算术/比较元方法都有（`+ - * / % // ^ == < > <= >= .. #`）。
- 注意：LuaJIT **没有位运算元方法**——`& | ~ << >>` 在 LuaJIT 下不可用。
]==]

BODY.context = [==[
# 线程上下文

```lua
local c = GetContext(tid)     -- tid 来自 info.tid / callback 参数
print(c.Cip.hex, c.Cax.val)   -- 通用寄存器（x86/x64 同名：Cax Cbx Ccx Cdx Csi Cdi Csp Cbp Cip）
print(c.EFlags.val, c.Dr7.hex)

c.Cax = U64(0)                -- 修改后写回
SetContext(tid, c)
```

- 字段（x86 23 项）：`ContextFlags SegGs SegFs SegEs SegDs SegCs SegSs EFlags Dr0..Dr3 Dr6 Dr7`
  + `Cax..Cip`。
- x64 另有 37 项：`P1Home..P6Home MxCsr R8..R15 Xmm0..Xmm15 VectorControl DebugControl
  LastBranchToRip LastBranchFromRip LastExceptionToRip LastExceptionFromRip`。
- 取到的每个字段都是 **LuaVariant**（`.val` 是数字，`.hex` 是十六进制串）。
]==]

BODY.symbols = [==[
# 模块 / 符号 / 栈

```lua
local base = GetModBase("kernel32.dll")        -- LuaVariant?（模块未加载 -> nil）
local rva  = FindFuncRva("kernel32.dll", "CreateFileW")  -- 导出 RVA（LuaVariant?）
local mod, rva2 = Ea2ModnameRva(0x7FFE1234)    -- 地址 -> 模块名 + RVA
print(LoadStr(addr))                          -- 读宽/窄字符串（自动判断，返回 string?）
print(GetCppClassName(addr))                  -- C++ 对象 -> 类名（读 RTTI，string?）
Hide()                                        -- PEB/Heap 反反调试修补（modern 变体含 ScyllaHide）

for i, fr in ipairs({ StackTrace(tid) }) do    -- 栈帧列表：ea / modname / rva
  print(i, fr.modname, fr.rva)
end
```

- `StackTrace` 依赖 dbghelp；Win9x/无 dbghelp 时退化（`frames = 0`），不报错。
]==]

BODY.scan = [==[
# 特征扫描与内存布局

```lua
-- 在目标内存里找字节特征（返回 addr, hexstr ... ; 具体形态见 GetMemoryRegion）
for addr, s in FindMemory("48 8B 05 ?? ?? ?? ??") do
  print(addr.hex, s)
end

for _, r in ipairs(GetMemoryRegion()) do
  print(r.BaseAddress.hex, r.RegionSize.val, r.Protect.val)
end
```

- 扫描是**逐区域**的，大进程会慢；先用 `GetMemoryRegion` 过滤（只看 `Protect` 可读的区域）。
]==]

BODY.events = [==[
# 其它调试事件（都在 Run{...} 的选项里）

| 字段 | 回调参数 | 含义 |
| --- | --- | --- |
| `exception_callback` | `info` | 异常（`type/address/nt_status/obj_*`，含 C++ EH 对象） |
| `on_any_debug_event` | `opt` | 任意调试事件（`code` 等） |
| `on_single_step` | `opt` | 单步 |
| `on_create_thread` / `on_exit_thread` | `opt` | 线程创建/退出 |
| `on_create_process` / `on_exit_process` | `opt` | 进程创建/退出 |
| `on_load_dll` / `on_unload_dll` | `opt` | 模块加载/卸载（`on_load_dll` 可用来下“还没加载的模块”的断点） |
| `on_output_debug_string` | `opt` | 目标的 `OutputDebugString` |
| `on_rip` | `opt` | RIP 事件 |
| `on_console_event` | `opt` | 控制台事件 |

```lua
Run{ cmdline = "target.exe", on_load_dll = function(e) print("load", e.modname) end,
     exception_callback = function(e) print("exc", e.type.hex, e.address.hex) end }
```
]==]

BODY.gui = [==[
# gui.* —— GUI 自动化（不需要调试会话也能用）

```lua
gui.size()                          -- w, h
gui.pixel(x, y)                     -- r, g, b
gui.screenshot{ format = "png" }    -- { w=, h=, format=, data=<二进制> }
gui.screenshot{ hwnd = h, scale = 0.5 }
gui.screenshot_to("C:/tmp/shot.png")

gui.windows()                       -- { {hwnd=, pid=, title=, class=, x=, y=, w=, h=, visible=} ... }
gui.find{ title = "记事本" }        -- hwnd | nil
gui.active() / gui.activate(h) / gui.focus(h)
gui.rect(h) / gui.set_rect(h, x, y, w, h) / gui.show(h, "max")

gui.children(h)                     -- { {hwnd=, class=, text=, id=, x=, y=, w=, h=} ... }
gui.text(h) / gui.set_text(h, "x")
gui.post(h, msg, wparam, lparam)    -- PostMessage（异步）
gui.send(h, msg, wparam, lparam)    -- SendMessage（拿返回值）
gui.click_control(h)                -- 控件中心真实点击

gui.click(x, y) / gui.move(x, y) / gui.write("abc") / gui.press("enter")
gui.hotkey("ctrl", "shift", "esc") / gui.sleep(500)
```

- 坐标是**屏幕绝对像素**；失败返回 `nil, errmsg`（不抛异常）。
- 截图给 AI 看：`print(#shot.data)` 后由 MCP 的 `gui_screenshot` 工具直接回 image/png
  （比在 `lua_eval` 里 base64 更省事）。
- 目标被调试器挂起时窗口不响应输入：先在脚本里让它继续跑，再操作 GUI。
]==]

BODY.files = [==[
# 写文件到「客户端那台机器」

## 流式（推荐；**调试回调里也能用**）

```lua
local f, e = open_file("Z:/out.bin", "wb")
if not f then print("ERROR: " .. tostring(e)) else
  Run{ cmdline = "target.exe", callback = function()
    SetBreakpoint{ modname = "my.dll", rva = FindFuncRva("my.dll", "F"),
      callback = function(info) f:write("hit\n") end }   -- 回调里直接写，不占内存
  end }
  f:close()          -- 必须调（在回调之外）
end
```

## 一次性（小数据）

```lua
write_file("Z:/a.bin", "data")        -- -> bytes | nil, err
local s = read_file("Z:/a.bin")       -- -> string | nil, err
```

- 流式要求客户端是流式的（`X-AutoDbg-Stream: 1`，nvim 插件默认打开）；
  流式请求里 `write_file`/`read_file` 会被拒绝（它们靠挂起协程）。
- 写**服务器本机**文件用标准 `io.open`（不受上述限制）。
]==]

BODY.pitfalls = [==[
# 已知地雷（务必读）

1. **回调里不能 yield**：`Run`/`Attach` 的回调是引擎经 C 调用进来的，`write_file`/`read_file`
   （以及任何需要挂起的 API）在那里会失败 → 用 `open_file` 流式写。
2. **`ReadMemory` 返回 table（不是字符串）**，`addr`/`size` 必须是 LuaVariant（`V()`/`U32()`）。
3. **`SetBreakpoint` 传 table**（`modname`/`rva`/`callback`），不是两个位置参数。
4. **`Var:*` 用单字母类型**（`i`/`I`/`d`/`q`…），**`LuaVariant:*` 用全名**（`u32`/`f64`…）。
5. **LuaJIT 没有位运算元方法**（`& | ~ << >>`）与 `string.unpack`/整数除法 `//`。
6. `BsonDecode` 是**桩**（会报 `not implement BsonDecode yet!`），只用 `BsonEncode`。
7. 脚本文件带 UTF-8 BOM 时 Lua 5.5 会直接失败（`lua_eval` 的 body 不受影响）。
8. 会话是**串行**的：一个 `Run`/`Attach` 没结束前，其它工具调用会排队（满了 503）。
   想提前收工：`/cancel`（或脚本里 `Detach()`）。
9. `GetContext` 的字段名 x86/x64 同名（`Cax..Cip`），但 x64 才有 `R8..R15`/`Xmm*`。
]==]

BODY.recipes = [==[
# 典型场景模板

## A. 抓函数参数（IAT/thunk 处下断点）

```lua
Run{ cmdline = [[C:\path\target.exe]],
  callback = function()
    SetBreakpoint{ modname = "kernel32.dll", rva = FindFuncRva("kernel32.dll", "CreateFileW"),
      callback = function(info)
        local c = GetContext(info.tid)
        print("path =", LoadStr(c.Ccx.val))        -- x64 第 1 参数 RCX（x86 是栈）
        Detach()
      end }
  end }
```

## B. 统计某函数被调用多少次 / 每次的计数

```lua
local n = 0
Run{ cmdline = "target.exe", callback = function()
  SetBreakpoint{ modname = "my.dll", rva = FindFuncRva("my.dll", "F"),
    callback = function() n = n + 1 end }
end }
print("hits =", n)
```

## C. 把大块内存 dump 到客户端文件（流式，内存恒定）

```lua
local f = open_file("Z:/dump.bin", "wb")
Run{ cmdline = "target.exe", callback = function()
  SetBreakpoint{ modname = "my.dll", rva = FindFuncRva("my.dll", "F"),
    callback = function(info)
      local c = GetContext(info.tid)
      local n = V(c.Ccx.val):to("I")
      f:write(BsonEncode{ vols = V(GetModBase("my.dll"))[0x56C5C0]:tos("d", 130 * (n + 1)) })
      Detach()
    end }
end }
f:close()
```

## D. 看界面 + 点按钮（AI 操作的典型循环）

```lua
local h = gui.find{ title = "Target" }
print("hwnd", h and h.val)
if h then
  gui.activate(h)
  local shot = gui.screenshot{ hwnd = h, format = "png" }
  print("shot bytes", #shot.data)          -- 或直接用 MCP 工具 gui_screenshot
  gui.click(400, 300)                      -- 屏幕上该控件的绝对坐标
  gui.sleep(300)
end
```
]==]

-- ---------------------------------------------------------------------------
-- 对外接口
-- ---------------------------------------------------------------------------
function M.topics()
  local list = {}
  for _, name in ipairs(ORDER) do
    list[#list + 1] = { name = name, summary = SUMMARY[name] or "" }
  end
  return list
end

function M.index()
  local lines = {
    "# AutoDbg Lua API（topic 目录）",
    "",
    "用 `lua_api(topic=...)` 取某一节的完整说明（含可执行示例）。",
    "",
  }
  for _, name in ipairs(ORDER) do
    lines[#lines + 1] = string.format("- %-12s %s", name, SUMMARY[name] or "")
  end
  lines[#lines + 1] = ""
  lines[#lines + 1] = "不确定怎么写的就先读 `model`（执行模型）+ `pitfalls`（地雷）。"
  return table.concat(lines, "\n")
end

function M.get(name)
  if name == nil or name == "" or name == "index" then
    return M.index()
  end
  local body = BODY[name]
  if body == nil then
    return nil, "unknown topic '" .. tostring(name) .. "'；可用：" .. table.concat(ORDER, ", ")
  end
  return body
end

return M
