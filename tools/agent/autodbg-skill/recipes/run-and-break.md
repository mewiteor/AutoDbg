# Recipe：启动目标 + 在函数上断点，抓参数

场景：目标由你启动，你想看某个 API 被调用时的参数。

```lua
-- break_on_createfile.lua
local hits = 0

Run({
  imagepath = [[C:\Windows\System32\notepad.exe]],

  callback = function(pid, tid)
    -- 入口断点：目标刚创建、还没执行。在这里下断点。
    print("pid", pid.val, "tid", tid.val)

    SetBreakpoint{
      modname = "kernel32.dll",
      rva     = FindFuncRva("kernel32.dll", "CreateFileW"),   -- ❗rva 必须是 LuaVariant
      callback = function(info)
        hits = hits + 1

        -- Windows x64：前四个参数在 RCX/RDX/R8/R9；x86：走栈。
        -- ❗`GetContext` 的每个字段都是 **LuaVariant**：直接取 `.val`（`.hex` 是十六进制串）。
        local ctx = GetContext(info.tid)
        print(string.format("hit#%d rip=%s", hits, tostring(ctx.Cip.hex)))

        -- 把第一个参数当宽字符串读出来（失败返回 nil）
        local p = U64(ctx.Rcx.val)                  -- x64；x86 用 V(ctx.Csp.val)[1]
        local s = LoadStr(p)
        if s then print("CreateFileW:", s) end

        if hits >= 5 then Detach() end           -- 抓够了就分离，Run 会返回
        return true                              -- true = 继续跑；false = 让目标停住
      end,
    }

    return true                                  -- 入口回调返回 true = 让目标开始执行
  end,
})

print("done, hits =", hits)
```

要点：

- **断点必须在回调里下**：`Run` 一返回目标就结束了。
- `rva` 不接受裸 number：`FindFuncRva(...)` / `U32("0x1234")` / `U64(...)`。
- 参数位置按架构不同：x64 走寄存器（`ctx.Rcx/Rdx/R8/R9`，取 `.val`），x86 走栈
  （`V(ctx.Csp.val)[1]` 起）。`Var:to` 用**单字母**（`q`/`Q`/`i`/`I`…），
  `LuaVariant:to` 用**全名**（`u64`/`i32`…）——别搞反。
- 回调里别做长耗时的事（同步跑在调试循环里）；要写大文件请用 `open_file`（见
  `recipes/remote-agent.md`）。
- 想「抓一次就停」用 `is_single = true`。

## 小技巧

- 归一化模块 RVA：`Ea2ModnameRva(ea)` 给出 `modname` + `rva`，方便拿它去下断点。
- 只在特定调用上落盘：

```lua
local f = open_file("C:/tmp/calls.txt", "ab")
SetBreakpoint{ modname = "kernel32.dll", rva = FindFuncRva("kernel32.dll", "CreateFileW"),
  callback = function(info)
    local name = LoadStr(U64(GetContext(info.tid).Rcx.val))   -- x64 第一个参数
    if f and name then f:write(name, "\n") end
    return true
  end }
```

记得在脚本末尾 `f:close()`（或者靠 `__gc` 的尽力关闭 + 警告）。
