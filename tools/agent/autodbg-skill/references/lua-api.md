# AutoDbg Lua API 参考

> 本文件取代旧 MCP 工具 `lua_api(topic)` 的正文（D41 之后对外只剩 CLI + raw TCP 帧）。
> 事实来源（改绑定层时同步这里）：`src/scripting/lua/registry.cpp` 的 29 个契约全局名、
> `bindings.cpp` 的参数形状、`variant.cpp` 的 LuaVariant、`nvim/addons/autodbg/library/*.lua`。
> ❗**T5（D44）删除了 `read_file` / `write_file`**：文件 I/O 只有 `open_file` 的文件对象；下面 `files` 段已按新口径改写。

## 执行模型（先读这段）

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

## Run{ ... }：启动并调试新进程

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

## Attach{ ... }：附加到已运行的进程

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

## 断点（软件断点 = INT3）

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

## 硬件断点（DRx）

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

## 内存读写

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

## LuaVariant 与 Var（读目标内存的两种口味）

### 1) Var：`V(addr)` —— 按「地址 + 类型字母」读

```lua
local v   = V(addr)              -- Var：地址 + 类型字母的读窗口
local ptr = v[0x10]              -- 把 addr+0x10 处的**指针**取出来（返回新的 Var）
local i   = v[0x10]:to("i")      -- 在该指针处读 int32
local ds  = v[0x10]:tos("d", 130)-- 读 130 个 double（LuaVariant 数组）
local u32 = v:to("I", 0x20)      -- 在 addr+0x20 处读 uint32
local s   = v:sto("i", 4, 0x20)  -- 4 个 int32 的字符串表示
v:write(U32(1), 0x20)            -- 写 uint32
v:writes({1,2,3}, 0x20)          -- 批量写
v:swrite("abc", 0x20)            -- 写字节串
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

### 2) LuaVariant：`U32(x)` / `I64(x)` / `F64(x)` … —— 值本身

- 构造：`U8 I8 U16 I16 U32 I32 U64 I64 F32 F64`（也接受 `"0x1234"` 字符串）。
- 方法：`:to("u32")` / `:as("i64")`（**全名**：i8/u8/i16/u16/i32/u32/i64/u64/f32/f64）。
- 字段：`.hex` / `.val` / `.rawval` / `.type` / `.bin`；算术/比较元方法都有（`+ - * / % // ^ == < > <= >= .. #`）。
- 注意：LuaJIT **没有位运算元方法**——`& | ~ << >>` 在 LuaJIT 下不可用。

## 线程上下文

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

## 模块 / 符号 / 栈

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

## 特征扫描与内存布局

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

## 其它调试事件（都在 Run{...} 的选项里）

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

## gui.* —— GUI 自动化（不需要调试会话也能用）

```lua
local w, h    = gui.size()                          -- 屏幕尺寸
local r, g, b = gui.pixel(0, 0)                     -- 单像素颜色
local shot    = gui.screenshot{ format = "png" }    -- { w=, h=, format=, data=<二进制> }
local small   = gui.screenshot{ hwnd = hwnd, scale = 0.5 }
gui.screenshot_to("C:/tmp/shot.png")                -- 落盘（会进 --json 的 images）

local wins = gui.windows()                          -- { {hwnd=,pid=,title=,class=,x=,y=,w=,h=,visible=} ... }
local hwnd = gui.find{ title = "记事本" }            -- hwnd | nil
gui.active(); gui.activate(hwnd); gui.focus(hwnd)
gui.rect(hwnd); gui.set_rect(hwnd, 0, 0, 800, 600); gui.show(hwnd, "max")

local kids = gui.children(hwnd)                     -- { {hwnd=,class=,text=,id=,x=,y=,w=,h=} ... }
gui.text(hwnd); gui.set_text(hwnd, "x")
gui.post(hwnd, 0x0010, 0, 0)                        -- PostMessage（异步，不阻塞目标）
gui.send(hwnd, 0x000F, 0, 0)                        -- SendMessage（拿返回值）
gui.click_control(hwnd)                             -- 控件中心真实点击

gui.click(100, 200); gui.move(100, 200); gui.write("abc"); gui.press("enter")
gui.hotkey("ctrl", "shift", "esc"); gui.sleep(500)
```

- 坐标是**屏幕绝对像素**；失败返回 `nil, errmsg`（不抛异常）。
- 截图给 AI 看：`print(#shot.data)` 后由 MCP 的 `gui_screenshot` 工具直接回 image/png
  （比在 `lua_eval` 里 base64 更省事）。
- 目标被调试器挂起时窗口不响应输入：先在脚本里让它继续跑，再操作 GUI。

## files —— `open_file` 的文件对象（唯一文件 API，D44/D49）

```text
local f, err = open_file(path [, mode])     -- mode: "r"/"rb"（默认读）/ "w"/"wb" / "a"/"ab"
f:read([fmt])          -- 默认 "l"；"a" / "l" / "n" / 整数 N（读 N 字节）
f:read_line()          -- = f:read("l")
f:lines()              -- for line in f:lines() do ... end
f:seek([whence][,off]) -- "set" / "cur"（默认）/ "end"；返回新位置
f:tell()               -- 逻辑位置
f:setbuf(n)            -- 服务端缓冲（默认 4096，上限 8192）
f:write(...)   f:flush()   f:close()
```

- **本地模式**：等价 `io.open`。
- **remote 模式**（`--remote`）：文件在**客户端**那台机器上；读是阻塞 RPC，回调里也能用。
- 语义（D49）：`"l"` 去行尾、`f:read(N)` 精确 N 字节、EOF → `nil`、错误 → `nil, err`、
  字节不做 CRLF 归一；`f:tell()` 是逻辑位置、`f:seek()` 往返且清缓冲。
- **回调里**：等回包超时 → `nil, "timeout"`（回调返回、断点继续）；被 `--cancel` → `nil, "cancelled"`。

## 已知地雷（务必读）

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

## 典型场景模板

### A. 抓函数参数（IAT/thunk 处下断点）

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

### B. 统计某函数被调用多少次 / 每次的计数

```lua
local n = 0
Run{ cmdline = "target.exe", callback = function()
  SetBreakpoint{ modname = "my.dll", rva = FindFuncRva("my.dll", "F"),
    callback = function() n = n + 1 end }
end }
print("hits =", n)
```

### C. 把大块内存 dump 到客户端文件（流式，内存恒定）

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

### D. 看界面 + 点按钮（AI 操作的典型循环）

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

