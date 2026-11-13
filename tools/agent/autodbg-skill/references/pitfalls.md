# 写 AutoDbg 脚本时最容易踩的坑

> 全部来自实测（详见仓库 `PITFALLS.md`）。按「会让你白白浪费一轮」的程度排序。

## 1. 调试逻辑只能写在回调里

`Run{...}` / `Attach{...}` **阻塞**到目标退出或 `Detach()`。回调外面的代码只在
「目标已经没了」之后才执行。想看某个函数被调用时的状态，就 `SetBreakpoint` + 回调。

```lua
Run({ imagepath = path, callback = function()   -- 入口断点
  SetBreakpoint(base + 0x1234, function(ctx)     -- 回调里下断点也可以
    print("hit", ctx.Cip)
    return true                                   -- 返回 false 表示不继续（停住目标）
  end)
  return true                                     -- 入口回调返回 true = 继续跑
end })
```

## 2. `.val` 是**字符串**、`.rawval` 才是数字；寄存器里常常是指针

```lua
local c = GetContext(tid)
print(c.Ccx.hex)       -- "000000003978614"  十六进制串（打印/日志用）
print(c.Ccx.val)       -- "60229140"         **字符串**，打印 OK，拿去算会变成拼接
print(c.Ccx.rawval)    -- 60229140           数字（要算就用它）
```

「参数在 ECX 里」通常是**两段信息**，先看清是哪种：

```lua
local arg = V(c.Ccx):to("i").val   -- Ccx 里的值 = 地址 → 读 [ECX] 处的 int32（最常见）
local raw = c.Ccx.rawval           -- Ccx 里的值本身就是参数（整数/句柄/长度）
print(arg, raw)
```

- `V(x)` 的 `x` 可以是数字、`U32()`，也可以是**寄存器（LuaVariant）**——它的值会被当地址。
- ❗`V(...)` 只在**调试回调**里可用：顶层 / REPL 里它返回 `nil, msg`（REPL 里 `V` 干脆是 `nil`）。
  顶层读指针链用会话绑定的 **`d:v(addr)`**（`d:v(base)[0x10]:to('I')`），详见
  `references/lua-api.md` 的「V 与 d:v：指针链走查」；代理（`dbg.remote`）上两者都没有。
- 目标位数决定寄存器宽度：x64 目标上 `Ccx` 就是 64 位的 `RCX`（`Csp` = `RSP`）；
  AutoDbg server 自己是 x64，**不影响** `GetContext` 返回的是目标的上下文。
- 别把 64 位地址塞进 `U32`（会被截断）。

## 3. `ReadMemory` 返回的是 table，不是字符串拼接器

```lua
local buf, err = ReadMemory(V(addr), U32(16))  -- ❗返回 byte table（1..n）；不可读 → nil, err
                                               -- ❗两个参数都必填（少一个会直接抛错）
local v32 = V(addr):to("I")                 -- 读 uint32（Var:to 用**单字母**；回调里，顶层换 d:v）
WriteMemory(V(addr), { 0x90, 0x90 })        -- byte table
```

`V(addr)` 返回 **Var**，用 `:to("I")` / `:tos("d", n)` 等（**单字母**类型字母：
`c b h H i I q Q z f d`）；LuaVariant（`GetContext` 的字段、`U32()` 的返回值）才用全名
`:to("u32")` / `.rawval`。构造值用 `U32(x)` / `I64(x)` / `F64(x)`。
LuaJIT 下没有位运算元方法，`V()` 的 `& | ~ << >>` 不可用。

## 4. `open_file` 才是文件 API

`read_file` / `write_file` 已删除（D44）。写大块数据、或**在回调里**读写文件，都用文件对象：

```lua
local f = open_file(client_path, "wb")   -- remote 模式下这是**客户端**的路径
if not f then return nil end
f:write(ReadMemory(addr, 0x1000))
f:close()
```

- 回调里 `f:read()` 是阻塞 RPC：超时 → `nil, "timeout"`，被取消 → `nil, "cancelled"`，
  两种都不会打断调试会话。
- `f:write` 不 yield、不落盘（remote 模式直接进 socket），内存占用 = 单次那一块。

## 5. 取消 / 杀 / 分离是三件不同的事

| 命令 | 效果 | 作用域 |
| --- | --- | --- |
| `--cancel` | 打断**脚本**（`lua_sethook` + 等待循环） | 整个脚本 |
| `--kill` | 结束**当前** `Attach`/`Run`（目标若由我们启动则终止） | 当前这次 run，脚本继续 |
| `--detach` | 真 detach，目标**存活**、pid 不变 | 同上 |

- 取消在「引擎阻塞」与「纯 Lua 死循环」两条路上由不同机制注入，但结论一致：脚本被打断。
- 在 `Run` 里被 cancel 时，`Run` 会返回、脚本**继续往下执行**（除非随后踩到 hook）。
  要「立刻停」就在 `Run` 之后自己判 `canceled`（`--json` 的 `canceled` 字段）。

## 6. 输出与 `--json`

- `--json` 下 stdout **只有一行 JSON**；`print` 进 `stdout` 字段，**不要**靠解析终端输出。
- 被调试进程自己的 stdout/stderr **不进信封**（被指到本进程 stderr）；要看目标输出就用
  `on_output_debug_string`（`Run{ on_output_debug_string = function(e) ... end }`）。
- 要交回结构化结果，`return { ... }`；`images` 会列出你写过盘的文件。

## 7. 目标机 / pid

- `open_file`、`gui.*`、`Attach` 的 pid 全部在**目标机**上解析。
- 不知道 pid 时在目标机上跑 `gui.pick()`：按鼠标位置取窗口所属进程
  （`local w = gui.pick(); if not w then ... end`），或命令行 `--pid=pick` 走同一条链。
- `Attach` 一个已有进程后也要 `Detach()`，否则脚本会一直等它退出。

## 8. Win9x / legacy 变体

- 老系统上没有 `DebugActiveProcessStop`：`--detach` 会降级成 kill（`--json` 的 `detached=false`）。
- `DASM`/位运算、`SendInput` 等新 API 在 legacy 变体上走降级实现 —— 行为一致但不保证全部特性。
