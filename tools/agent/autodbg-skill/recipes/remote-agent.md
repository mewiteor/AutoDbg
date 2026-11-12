# Recipe：代理循环（`--remote` + `--json` + `--cancel`）

场景：目标在另一台机器（或另一个终端）上，代理在本地反复「跑脚本 → 拿结构化结果」。

## 1. 目标机上起一次 server

```bat
:: 只绑 loopback 时免 token；绑 0.0.0.0 必须给 token（D21）
AutoDbg_64.exe --server=tcp --server-port=8642 --server-no-echo
::   非 loopback 就显式给：
AutoDbg_64.exe --server=tcp --server-addr=0.0.0.0:8642 --server-token=<hex> --server-no-echo
```

服务端一次只跑一个脚本（严格串行）。第二个执行请求会收到 `error{busy}`。

## 2. 代理侧：一次调用 = 一行 JSON

```bat
AutoDbg_64.exe --remote=127.0.0.1:8642 --json step1.lua
:: → {"ok":true,"exit":0,"canceled":false,"detached":false,"pid":0,
::     "stdout":"...","stderr":"","images":[],"result":{"found":3}}
```

脚本要交回结构化结果就 `return { ... }`：

```lua
-- step1.lua：找一个模块、把它的范围交回去
local base = GetModBase("target.exe")
if not base then return { error = "module-not-loaded" } end
return { base = tostring(base) }   -- 目标 pid 看 --json 信封的 `pid` 字段（未附加为 0）
```

**不要**让代理去解析 `print` 散文；`result` 才是契约。

## 3. 长跑 + 外部中断

`Run` / `Attach` 会一直阻塞。需要「跑一会儿就停」时，从**另一个进程**发控制命令：

```bat
:: 终端 A：长跑
AutoDbg_64.exe --remote=127.0.0.1:8642 long_run.lua

:: 终端 B：
AutoDbg_64.exe --remote=127.0.0.1:8642 --health      :: {"ok":true,"busy":true,...}
AutoDbg_64.exe --remote=127.0.0.1:8642 --cancel      :: {"cmd":0,"applied":true,"reason":"cancel-requested"}
AutoDbg_64.exe --remote=127.0.0.1:8642 --detach      :: 只结束当前 Attach，目标继续活着
```

- `--cancel` → 打断**脚本**（引擎阻塞与纯 Lua 死循环都能打断）；
- `--kill` → 只结束**当前** `Attach`/`Run`（我们启动的目标会被终止），脚本继续；
- `--detach` → 真 detach：目标存活、pid 不变，下一次还能 `Attach`（NT+；Win9x 降级成 kill）。

客户端退出码：`0` = 成功，`1` = 服务端说 `applied=false`，`2` = 连接/协议错误。

## 4. 大块数据 / 回调里读写文件

文件 I/O 在**客户端**那台机器上，`open_file` 的读是阻塞 RPC（回调里也能用）：

```lua
-- dump_on_hit.lua
local out  = "C:/tmp/dump.bin"
local f, err = open_file(out, "wb")
if not f then return { error = "open-failed", detail = tostring(err) } end

Run({
  imagepath = [[C:\target\target.exe]],
  callback = function()
    SetBreakpoint{
      modname = "target.exe",
      rva     = FindFuncRva("target.exe", "Encrypt"),
      is_single = true,
      callback = function(info)
        local ctx = GetContext(info.tid)
        local p   = ctx.Rcx.val                       -- x64 第一个参数（LuaVariant.val）
        local buf = ReadMemory(U64(p), U32(0x10000))
        -- 注意：是 table of bytes，转成字节串再写
        local bytes = {}
        for i = 1, #buf do bytes[i] = string.char(buf[i]) end
        f:write(table.concat(bytes))
        return true
      end,
    }
    return true
  end,
})

f:close()
return { dumped = out, bytes = 0x10000, images = out }
```

要点：

- `f:write` 不 yield、不落盘（remote 模式直接进 socket），内存 = 单次那一块。
- 回调里 `f:read()` 等回包：超时 → `nil, "timeout"`（回调返回、断点继续）；`--cancel` →
  `nil, "cancelled"`。两种都不会把调试会话搞崩。
- `open_file` 写入的路径会自动进 `--json` 的 `images` 列表（去重、上限 64）。

## 5. 建议的代理编排

1. `--health` 探测（`{"busy":false}` 才能接活）；
2. `--json step.lua` 跑一步，读 `result` / `stderr` / `images`；
3. 超时或跑飞了：`--cancel`（软停）→ 不行再 `--kill`；
4. 需要留下目标继续观察：脚本里 `Detach()`，`--json` 里 `detached=true`、`pid` 就是下次可用的 pid。
