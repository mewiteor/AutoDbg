# CLI、`--json` 契约与 raw TCP 帧

> 事实来源：`src/sys/pack/launcher.cpp`（选项）、`src/scripting/lua/main.cpp`（信封）、
> `src/common/frame.hpp` + `src/scripting/lua/stream.hpp`（帧）、`src/scripting/lua/tcp_server.cpp`（服务端）、
> `src/scripting/lua/client.cpp`（`--remote`）。
> ❗T5（D41/D42）：**HTTP 与 MCP 已删除**，对外只剩「本地 CLI」与「raw TCP 帧」两形态；
> `--server=http` 会直接报错（退出码 2）。

## 1. 两种运行形态

```bat
:: ① 本地（脚本与目标在同一台机器）
AutoDbg_64.exe [选项] script.lua [脚本参数...]

:: ② 远端：目标机上起 server，另一台机器跑客户端
AutoDbg_64.exe --server=tcp --server-port=8642 --server-no-echo
AutoDbg_64.exe --remote=127.0.0.1:8642 [--remote-token=<hex>] [选项] script.lua
```

- 一次调用 = 一次 Lua 执行，严格串行。
- 远端模式下文件 I/O（`open_file`）在**客户端**那台机器上。
- 服务端一次只跑一个脚本；第二个执行请求会拿到 `error{busy}`。

## 2. 启动器选项

| 选项 | 说明 |
| --- | --- |
| `-h` / `--help` | 用法，退出 0 |
| `--version` | 版本 / ABI / 架构，退出 0（带 `--remote` 时改为**查 server 版本**） |
| `--flavor=lua\|luajit` | 解释器（默认 lua 5.5）；脚本首行 `--!lua` / `--!luajit` 也能选 |
| `--variant=modern\|legacy` | 变体（仅排障；正常靠平台探测 + 映射失败回退） |
| `--list` | 列出载荷里的模块 |
| `--module-dir=<dir>` | 调试：从磁盘取同名 DLL（仍走内存映射） |
| `--quiet` | 关错误弹窗（headless / CI 必带） |
| `--selftest` | 纯函数自测 |
| `--repl` | 本地 REPL：从 **stdin** 读合法 Lua 语句（**不需要脚本**；与 `--json`/`--out`/`--remote` 互斥，可与 `--server` 并存） |
| `--json` / `--out=<path>` | 机器可读结果信封（与 `--server`、`--repl` 互斥） |
| `--server[=tcp]` | 起 raw TCP 服务端 |
| `--server-addr=<ip:port>` / `--server-port=` / `--server-token=<hex\|auto\|none>` | 监听地址 / 端口 / 认证（D21：loopback 免 token） |
| `--server-max-body=<bytes>` / `--server-queue=` / `--server-timeout=<sec>` / `--server-log=` / `--server-no-echo` | 帧上限 / 队列 / 单次执行超时 / 日志 / 不回显 |
| `--remote=<host:port>` | 客户端：连 server 跑脚本（缺省端口 8642） |
| `--remote-token=<hex>` | 握手 token（非 loopback 必给） |
| `--pid=<n>` | 显式目标 pid（D48 解析链的「显式」一档） |
| `--read-timeout=<ms>` | 单次文件操作 / 等回包的上限（默认 5000） |
| `--max-frame=<bytes>` | 客户端能接受的帧上限（默认 8 MB） |
| `--cancel` / `--kill` / `--detach` / `--health` | 客户端控制命令（必须与 `--remote` 一起用） |

**退出码**：`0` 成功；脚本不存在 → `-1`；脚本语法错误 → `3`（`LUA_ERRSYNTAX`）；
运行时错误 → `2`（`LUA_ERRRUN`）；启动器自身错误 → `2`。

## 2.1 `--repl`：stdin 喂 Lua 语句

```bat
:: 交互：stdout 是真控制台时才打提示符（autodbg> /    ...>）
.out\AutoDbg_64.exe --repl

:: 管道（可以，管道下**零提示符**）：
echo print(1+1) | .out\AutoDbg_86.exe --repl
```

- 每条 stdin 行 = **一条合法 Lua 语句**（表达式要 `=` 简写：`=1+1` ⇒ `return 1+1`）。
- **多行续行**：块没写完就继续读下一行（`for` / `body` / `end`）；判据是语法错误里出现 **`<eof>` 标记**。
- **退出码 = 最后一条命令的码**（`0` / `2` 运行时 / `3` 语法）；干净的 EOF → `0`。
- `--repl` 与 `--json`/`--out`、`--remote` **互斥**（启动器报错，退出码 `2`）。
- **`--repl --server` 可并存**：同一条连接内 `lua_State` **跨命令存活**（`x = 41 print(x)` 之后
  `=x + 1` 能看到 41），**一帧 `script` = 一行 / 一个完整块**（不跨帧累积：没输完在那里就是语法错 `3`）。
  归因：`out` 帧 `id` = 命令号、`result` 帧 `id` = **该行的 Lua 错误码**（`0`/`2`/`3`）。
- 退出前会把没关的 `dbg` 会话逐个收尾，往 stderr 打一行
  `repl: N unclosed session(s) - detaching`（**不改退出码**）。
- 本地 REPL **不提供取消**：`dbg` 的 `on_wait_timeout` 有意不读控制台 ⇒ 长 `pump` 在本地 REPL 里
  打不断（这是决定，不是 bug）；server 形态下 `--cancel` 照常生效。

## 3. `--json` 信封（D50）

stdout **只有一行**、纯 ASCII：

```json
{"ok":true,"exit":0,"canceled":false,"detached":false,"pid":0,
 "stdout":"...","stderr":"","images":[],"result":null}
```

- `pid` 是**被调试进程**（未附加为 0）。
- `detached` 只在目标真的存活时为 true（真 detach / keep-alive）；被 kill → false。
- `images` = 本次写盘过的路径（`open_file` 的 w/a、`gui.screenshot_to`），去重、上限 64，空也输出 `[]`。
- `result` = 脚本返回值：1 个 → 那个值；0 个 → `null`；多个 → 数组；编不了（function / 稀疏表 / NaN / 循环引用）→ `null`（`ok` 仍可能为 true）。
  ❗地址类值是 LuaVariant（`dbg` 新面：`ev.ea` / `mods().base` / `eip` / `read_u64` / `d:context()` 的
  **所有字段**（C1d 起全变体）…）⇒ 走 `jsonx` 的 `userdata` 分支吐 `.val`（**精确十进制**），不会被
  `%.14g` 截断（裸 number 会被）。M128（`Xmm*`）是 `{ Low, High }` 表，两个半部同样走 `.val`。
- `--out=<path>` 写同一份 JSON 到文件，stdout 留空。
- JSON 模式下被调试进程的 stdout/stderr 被指到本进程的 stderr —— 不会污染那一行。

## 4. raw TCP 帧（自己实现客户端时才需要读）

❗**协议版本 = 2**（T7 切片 D2a）：相对于 1，只多了 `event` 帧（16）与 `out` 帧的**命令号归因**。
版本不符时**不要**猜；客户端与模块必须同版本（握手帧是唯一协商手段，所以只 bump）。

分帧：`u32 payload_len | u8 type | u8 flags | u16 id | u32 data_len | [json] | [data]`（小端）。
`json_len = payload_len - 8 - data_len`。

| type | 名字 | 方向 | 用途 |
| --- | --- | --- | --- |
| 1 | `open` | S→C | `flags` = 0 读 / 1 写 / 2 追加；`data` = 路径；`id` = 句柄 |
| 2 | `data` | S→C | 写模式的原始字节（`id` = 句柄） |
| 3 | `close` | S→C | 关句柄 |
| 4 | `out` | S→C | `flags` 0 = stdout / 1 = stderr；❗`id` = **命令号**（0 = 无归属） |
| 5 | `result` | S→C | ❗`id` = **退出码**（不是命令号）；`flags` bit0 ok / bit1 canceled / bit2 timed_out / bit3 truncated；json = 结果 |
| 6 | `error` | S→C | json `{error}` |
| 7 | `read_req` | S→C | `flags` = op \| (whence<<4)；`data` = `{u32 arg}` |
| 8 | `ack` | S→C | `flags` bit0 = applied；json `{cmd, applied, reason, pid}` |
| 9 | `info` | S→C | health / version 应答（json） |
| 10 | `handshake` | C→S | `id` = **协议版本（当前 2）**；`data` = token |
| 11 | `script` | C→S | `data` = Lua 源码（不落盘）；`json` 段 = **chunkname 原文**（不是 JSON，服务端给它加 `@`，错误信息里就是 `你的文件:12: …`） |
| 12 | `control` | C→S | `flags` = 命令：0 cancel / 1 kill / 2 detach / 3 health / 4 version / 5 shutdown |
| 13 | `read_resp` | C→S | `flags` bit0 EOF / bit1 错误；`data` = 字节或 `{u32 pos}` |
| 14 | `end` | 双向 | 一段脚本/连接的收尾 |
| 15 | `cancel_notify` | S→C | json `{id}`：那一轮本地 I/O 作废，别回包 |
| 16 | `event` | S→C | 一条命中记录：json `{type, pid, tid, ea, modname, rva, code, first_chance, dropped}`（与 `hits()` 记录 / `dbg` 事件表同形，多一个 `dropped`）；❗`id` = **产生它的命令号**（0 = 无归属） |

- 握手失败（版本不符 / token 不符 / 首帧不是握手）→ `error` 帧 + 关连接。
- `control` 可以在脚本**跑着的时候**从另一条连接发（accept 线程处理）——这就是 `--cancel` 的实现。
- **命令号归因**：`out` / `event` 帧的 `id` = 最近一次 `script` 帧的 `id`（跑完清 0）。
  客户端用自己的命令号就能把输出/事件分到下一条命令上（`result` 帧的 `id` **不是**命令号）。
- `event` 帧**只**是诊断：`--json` 的信封里没有它，客户端可以不理。
