# Recipe：附加到已经在跑的目标（`--pid` / `gui.pick()`）

场景：目标不是你启动的（GUI 程序、别人开的服务），你只有「它现在在屏幕上」。

## 1. 拿 pid（三种方式，优先级从高到低）

```bat
:: ① 显式给
AutoDbg_64.exe --pid=1234 --json attach_and_go.lua

:: ② 目标机上按鼠标位置取（D48）：脚本里调 gui.pick()
```

```lua
-- pick.lua：在**目标机**上跑一次，把 pid 打出来（然后人工/代理写回头标记）
local w, err = gui.pick()
if not w then error("no window under cursor: " .. tostring(err)) end
print("pid=" .. tostring(w.pid))
print("hwnd=" .. tostring(w.hwnd))
```

```bat
:: ③ 让脚本自己挑一个（按标题）
```

```lua
local h = gui.find{ title = "Untitled - Notepad" }
if not h then error("target window not found") end
local wins = gui.windows()          -- 想自己筛 class/visible 时用这个
```

> `gui.pick()` 是「鼠标位置 → 窗口 → 进程」：`GetCursorPos` → `WindowFromPoint` →
> `GetWindowThreadProcessId`；没有交互桌面时降级到前台窗口，取不到就报错（不猜）。

## 2. 附加 + 下断点 + 分离

```lua
-- attach_and_go.lua
local pid = tonumber(os.getenv("TARGET_PID") or "") or 0
if pid == 0 then
  local w = gui.pick()
  if not w then error("pass --pid=<n> or point the cursor at the target") end
  pid = w.pid
end

local hits = 0
Attach{ pid = pid, callback = function()
  -- 附加成功；目标**暂停**在这里
  print("attached", pid)

  SetBreakpoint{
    modname  = "user32.dll",
    rva      = FindFuncRva("user32.dll", "MessageBoxW"),
    is_single = true,
    callback = function(info)
      hits = hits + 1
      local ctx = GetContext(info.tid)
      local text = LoadStr(U64(ctx.Rdx.val))            -- x64 第二个参数（LuaVariant.val）
      print("MessageBoxW text:", tostring(text))
      return true
    end,
  }

  -- 附加一个「已经在跑」的进程时，通常要自己安排收尾：
  -- 这里靠断点命中后 Detach()；也可以直接 return 让它继续跑、由外部再 cancel。
end }

print("done, hits =", hits)
```

要点：

- `Attach` 也是**阻塞**的：目标退出或 `Detach()` 才返回。
- 目标暂停在附加点，回调里做完事 `return`/结束回调 = 继续运行。
- **Win9x 不支持 attach**（系统限制，只有 `Run`）。
- 附加上之后想「放着不管」：`Detach()`（NT+ 是真 detach，pid 不变，下次还能再 `Attach`）。

## 3. 从另一个进程控制长会话

```bat
:: 目标机上：起 server，脚本在那边长跑
AutoDbg_64.exe --server=tcp --server-port=8642 --server-no-echo
AutoDbg_64.exe --remote=127.0.0.1:8642 long_attach.lua     :: 会一直占着（阻塞）

:: 另一台机器 / 另一个终端：
AutoDbg_64.exe --remote=127.0.0.1:8642 --health
AutoDbg_64.exe --remote=127.0.0.1:8642 --cancel     :: 打断脚本
AutoDbg_64.exe --remote=127.0.0.1:8642 --detach     :: 只结束当前 Attach，目标存活
```

`--cancel` 从**另一个进程**也能生效（服务端 accept 线程接控制连接），
细节见 `references/cli.md` 与 `recipes/remote-agent.md`。
