# plan_cli.md —— CLI + raw TCP 路线（已拍板）

> 状态：**已拍板**，待落地分支 `feat/cli-tcp`。对外契约只剩 **CLI + raw TCP 帧**：MCP 与 HTTP 代码整体删除。
> 决策 D41–D50 已登记进 `AGENTS.md` §13；`AGENTS.md` §12.3（MCP）与 §12.4（效果协议）已在原地标注被本文件取代。
> 前置说明：pi 的 MCP 地址热切换扩展**不入库**（留在 `git stash@{0}`），本文件不依赖它；§6 的 pi 实测事实保留，是 D41 的依据。
> 已踩的坑见 `PITFALLS.md`（S 节 = server / 帧协议）。`plan_server.md` 的 §2.1 / §2.2 / §2.6 / §5.2 随本方案作废。

---

## 0. 已拍板的决定（D41–D50）

| # | 决定 | 一句话 |
| --- | --- | --- |
| D41 | CLI 是对外主契约；MCP 与 HTTP 代码整体删除 | 执行粒度已经定成「一次调用 = 一整段脚本」，MCP 唯一的结构性优势（细粒度逐次调用）在当前引擎里兑现不了（`PITFALLS.md` S12） |
| D42 | 传输 = raw TCP 帧：单端口、握手帧带版本、**不做 HTTP 嗅探、不留残留报错** | `--health` / `--version` 改由 CLI 经 TCP 提供 |
| D43 | 每次调用无状态 | 会话表、`Mcp-Session-Id`、≤8 上限、30 min 空闲回收、503 全删；跨调用唯一的状态是**被调试进程本身**，靠 pid 显式指认 |
| D44 | 文件 I/O 只有 `open_file` 文件对象（io 风格 API） | `read_file` / `write_file` + 效果协议 + base64 + 协程执行器一起删 |
| D45 | `f:read()` 在调试回调里**可以阻塞** | 超时返回 `nil, "timeout"` → 回调返回、断点继续；取消立即生效（规则见 §2.3） |
| D46 | 控制三档：`cancel`（硬）/ `kill`（软）/ `detach`（软） | 硬 = `lua_sethook`；软 = `on_wait_timeout`（等价 local mode 的 q/Q） |
| D47 | `--detach` 无条件 | NT+ 走 `DebugActiveProcessStop`——**即使目标是我们 `CreateProcess` 起的也是真 detach**，pid 不变，可用于下一次 `Attach`。Win95/98 没有该导出 → 降级成 kill，由 `ack` 帧报 reason（`tests/golden/w9x/w3_detach_quick.txt` 的口径不变） |
| D48 | pid 解析链（禁止隐形状态） | 显式（`--pid=` / 握手帧 / 头标记 `pid=`）> `gui.pick()`（目标机上 `GetCursorPos` → `WindowFromPoint` → `GetWindowThreadProcessId`）> 脚本字面量 |
| D49 | `open_file` 默认 buffer = 4096；`f:tell()` 是逻辑位置；帧内字节一律精确 | `f:seek()` 是往返且清空 buffer；不再做 CRLF 归一 |
| D50 | `--json` 契约含 `pid` / `canceled` / `detached`，`result` 暴露脚本返回值 | 机器可读结果，agent 不必解析 `print` 散文 |

---

## 1. 落地前仍然成立的不变量

| 不变量 | 出处 |
| --- | --- |
| 一次调用 = 一次 Lua 执行，严格串行（并发 1）；`Run`/`Attach` 阻塞到目标退出或 `Detach()` | `AGENTS.md` §12.1、`src/scripting/lua/server.cpp` 的 T1/T2 分工注释 |
| 交付物是**单个 exe**（`.out/AutoDbg_<arch>.exe`），模块内嵌 + 内存映射，无安装 | `AGENTS.md` D4 |
| 每次启动付一次解包 + 映射：x86 ≈19.5 ms / 2.3 MB，x64 ≈17 ms / 1.5 MB | `AGENTS.md` §11.2 |
| `--remote` 的落点在**模块**里（launcher 的链接清单没有 `net`/`http`） | `make.bat` `:buildlauncher` |
| `src/common/net.cpp`（531 行）保留：winsock 必须动态载入（Win95 没有 `ws2_32`，只有 `wsock32`） | `src/common/net.hpp` 头注释、`PITFALLS.md` S9 |
| `json_writer`（319 行，只写不解析）与 `png_writer`（264 行）保留；`jsonx`（326 行）保留（`--json` 的 `result` 是 Lua 值，必须在 Lua 侧 encode） | `AGENTS.md` D25 / D28 |
| 29 个契约全局名不可改（含 `BsonEncode` / `BsonDecode`，后者仍是桩） | `AGENTS.md` §5 第 6 条、`registry.cpp` |
| `read_file` / `write_file` 是 `lua_setglobal` 加的名字，**不在 29 个契约名内** → 可以删 | `src/scripting/lua/file_io.cpp` |
| `DebugCallbacks` 布局与 `WaitTimeoutAction` 枚举值不可动（`on_wait_timeout` 的签名保留） | `AGENTS.md` §7 |
| 手工映射模块里禁止 `thread_local` / 非平凡构造的函数内 static | `AGENTS.md` §11.5、`PITFALLS.md` M10 |
| 帧格式本身已经与传输无关（`u8 type \| u8 flags \| u16 id \| u32 data_len \| [json] \| [data]`） | `src/scripting/lua/stream.hpp` |

---

## 2. 协议定稿

### 2.1 帧

布局不变，方向补齐。

出向（server → client）：

| type | 名字 | json / data |
| --- | --- | --- |
| 1 | `open` | json `{path, mode}` |
| 2 | `data` | 原始字节（不编码、不转义） |
| 3 | `close` | — |
| 4 | `out` | flags `0`=stdout / `1`=stderr |
| 5 | `result` | `--json` 的同一份信封（含 `pid` / `canceled` / `detached` / `result`） |
| 6 | `error` | json |
| 7 | `read_req` | **新**：json `{id, format \| bytes, whence, offset}` |
| 8 | `ack` | **新**：json `{applied, reason, pid}` |
| 9 | `info` | **新**：json（`--health` / `--version` 的应答，替代 `GET /health` `/version`） |

入向（client → server）：

| type | 名字 | 内容 |
| --- | --- | --- |
| 10 | `handshake` | json `{version, pid, filename, token, read_timeout, timeout_sec, max_frame}` |
| 11 | `script` | data = Lua 源码（D26「全程不落盘」不变） |
| 12 | `control` | json `{cmd: "cancel" \| "kill" \| "detach" \| "health" \| "version"}` |
| 13 | `read_resp` | data（或 json `{error}`）；`data_len == 0` 且 json 为 `null` = EOF |
| 14 | `end` | 无 body（替代 chunked 终止块 / `Connection: close`） |

`u16 id` 继续是**句柄号**，不是请求相关性 id：执行串行 + reader 唯一 ⇒ 同一句柄最多一个在途请求，不需要相关性表。

> ❗**已落地的口径（T2，2025 年实现时定的）**：帧头保持 **8 字节不变**，`[json]` 的长度**由传输层的 payload 总长反推**：
> `json_len = payload_len - 8 - data_len`。传输层两种分帧：chunked（`<hex>\r\n` + payload + `\r\n`，现行 HTTP）
> 与 raw（`u32` 小端 = payload 总长，D42）。好处：现有客户端（`nvim/lua/autodbg/stream.lua` 的 8 字节解析器）
> 不用跟着改，T5 删 HTTP 时只换分帧、不碰帧布局。实现：`src/common/frame.*`（纯函数，可被 `--selftest` 断言）
> + `src/scripting/lua/stream.*`（帧类型/协议版本/传输钩子/读一帧）。

版本必须在握手帧里（硬切之后没有别的协商手段；版本不符 → `error` 帧 + 关连接）。

### 2.2 唯一 reader

> **执行连接的唯一 reader 是脚本线程**（跑 Lua 与调试循环的是同一条线程，`Lua 状态不跨线程`）。只有三个点能读它：
> ① `f:read()` 的等待循环（按 ≤250 ms 切片 `select`，每片检查 guard 标志）；
> ② `on_wait_timeout`（100 ms 空闲轮询）里的非阻塞 poll；
> ③ `lua_sethook` tick（100 万条指令一次）里的非阻塞 poll。
>
> 其它连接一律是**控制连接**，由 accept 线程处理；accept 线程永远不读执行连接。

后果：因为执行严格串行，同一时刻只有一个在途执行 ⇒ 控制命令不需要 id / session key，直接作用于「当前正在跑的那次执行」。这是 D43 带来的最干净的简化。

### 2.3 控制三档与立即取消

| 命令 | 消费点 | 效果 | 作用域 |
| --- | --- | --- | --- |
| `cancel` | `lua_sethook` | 打断脚本 | 整个脚本（跨多个 `Attach`/`Run`） |
| `kill` | `on_wait_timeout` → `WAIT_TIMEOUT_DETACH_AND_TERMINATE` | 结束**当前** `Attach`/`Run`；我们 `CreateProcess` 起的目标由 `run_shutdown` 终止 | 当前这一次 run；脚本继续，下一个 `Attach` 照常开始 |
| `detach` | `on_wait_timeout` → `WAIT_TIMEOUT_DETACH_KEEP_ALIVE` | 真 detach，目标存活，pid 不变，下一次 `Attach` 直接可用 | 同上 |

`WaitTimeoutAction` 枚举值与 `DebugCallbacks` 布局不动；`can_detach_without_term` 从「许可」改成「能力上报」：宿主一律返回 2，引擎按 `DebugActiveProcessStop` 的可用性执行（Win95/98 取不到该导出 ⇒ 实际是 kill）。`console_input.hpp` 的注释同步；`decide_console_action` 目前没有门禁覆盖（`tests/` 下只有 golden），借这次补一条 `--selftest`。

每条控制命令都要回 `ack` 帧（控制命令是 fire-and-forget，nvim 和 AI 都需要知道实际执行了哪一条、有没有降级）：`{applied, reason, pid}`。

立即取消的四条规则：

1. 同一句柄最多一个在途请求；
2. `f:read()` 在发 `read_req` **之前**先查 guard 标志；切片等待中读到控制帧 → 置 `cancelled`，**立刻**返回 `nil, "cancelled"`；
3. 被取消的请求标 `stale`；之后任何 `read_resp` 若其请求已被取消 → **丢弃该帧**，不当数据用（否则协议串位）；
4. 返回前给 client 发一个 `cancel_notify`（json `{id}`），让 client 丢掉那一轮的本地 I/O、不要回包。客户端必须实现这条丢弃，否则两边各自兜底、行为会分叉。

CLI / nvim 的调用面：

```bat
AutoDbg_86.exe --remote h:p --cancel        :: 硬 cancel
AutoDbg_86.exe --remote h:p --kill          :: q/Q 的等价（action 1）
AutoDbg_86.exe --remote h:p --detach        :: Shift+Q 的等价（action 2）
AutoDbg_86.exe --remote h:p --health        :: 替代 GET /health
AutoDbg_86.exe --remote h:p --version       :: 替代 GET /version
AutoDbg_86.exe --remote h:p --json script.lua
```

nvim：`:AutoDbgCancel`（硬，保留现名）+ 新增 `:AutoDbgKill` / `:AutoDbgDetach` / `:AutoDbgPick`。

### 2.4 `open_file` 文件对象（唯一文件 API）

```lua
local f = open_file(path [, mode])          -- 本地 = 真文件；远程 = 帧
f:read([format])        -- "a"/"*a" | "l"/"*l" | "n"/"*n" | 整数（读 N 字节）
f:read_line()           -- 便捷式，等价 f:read("l")
f:lines([format])       -- 返回迭代器：for line in f:lines() do
f:seek([{whence}] [, offset])               -- "set" / "cur" / "end"
f:tell()                -- = f:seek("cur")
f:setbuf(n)             -- 默认 4096；0 = 不缓冲（每次调用一次往返）
f:flush()  f:close()
```

必须钉死的语义（否则 Lua 5.5 与 LuaJIT 会分叉）：

| 语义 | 口径 | 理由 |
| --- | --- | --- |
| 默认 format | `"l"`（含行尾 `\n`） | Lua 5.1 / LuaJIT / 5.4 一致；`*` 前缀两者都接受（LuaJIT `li_io.c` 会 `if (mode[0]=='*') mode++`） |
| `f:read(整数)` | 读 N 字节 | 与 `io` 一致：只有字符串 format 才是「读一个数」 |
| `f:lines()` | **自己实现迭代器，不委托 `io.lines`** | LuaJIT 的 `File:lines()` 不接受 format 参数 |
| EOF vs 错误 | EOF → `nil`；错误 → `nil, err` | `io` 的口径；`nil, err` 与 `gui` 表（D23）一致 |
| 字节 | 帧里传**原始字节**，不做 CRLF 归一 | 现有 `to_crlf` / `translate_read` 在 `*l` 口径下自相矛盾；mode 字符串只转交给 client 的 `io.open` |
| buffer | 每句柄一个服务端缓冲，默认 4096 | 不缓冲则 `read("l")` / `lines()` 是每行一次往返；缓冲后是每次 refill 一次（stdio 模型） |
| `f:tell()` | **逻辑位置** = client 位置 − 服务端未消费的缓冲 | `f:seek()` 是往返且清空 buffer，否则两者互相打脸 |

一个干净的对称性：`open_file` 只有**一个实现 + 两个后端**（真文件 / 帧）。`--remote` 的 client 直接复用现成的 `local_read_file` / `local_write_file` 当自己的后端 —— 不需要新写文件 I/O 代码。

### 2.5 pid 的解析链

1. **显式**：CLI `--pid=<pid>` / 握手帧 `pid` / nvim 头标记 `pid=`（`modeline.lua` 已解析 `url/token/host/port/exe/arch`，加一个 `pid=`）。
2. **鼠标选取**：新增 `gui.pick()`（新名字，符合 §5 第 6 条）= `GetCursorPos` + `WindowFromPoint` + `GetWindowThreadProcessId` → `pid` + `hwnd`。CLI 侧 `--pid=pick`。
   ❗必须在**目标机器**上解析（落在模块 / core，不能落启动器：launcher 没有 core/gui）。无交互桌面时降级 `gui.foreground_pid()`，取不到就报错而不是猜。`WindowFromPoint` 与 `GetWindowThreadProcessId` 都是 Win95 就有的 user32，不需要 dynapi、不新增导入。
3. 脚本自己的 `Attach(pid)`（`Attach` 是契约名，不改）。

D43 说「每次调用无状态」，所以**不能有隐形状态**：「记住的 pid」只能存在脚本头标记里（人工改一行）。`state.cpp` 不写任何文件这条不变（§3）。nvim 加 `:AutoDbgPick`：在目标机跑一次 pick，把结果**写回头标记**，而不是存在内存里。

于是这个模型成立：**跨调用唯一可存活的状态是被调试进程本身，靠 pid 显式指认。** 这条要写进 `AGENTS.md` §12。

---

## 3. 删除清单（实数）

| 删 | 现在 | 备注 |
| --- | --- | --- |
| `src/common/http.cpp` + `http.hpp` | 642 + 119 行 | `Content-Length` 重复（请求走私防御）、`Expect: 100-continue`、`Accept`、`Mcp-Session-Id`、`X-AutoDbg-*` 头、411/413/501 分支全消失 |
| `lualib/mcp.lua` | 361 行 / 16.9 KB | 烘焙集合从 5 变 4（`effects` 也删 → 3）；`autodbg_api.lua`（397 行）内容搬进 skill markdown |
| `lualib/effects.lua` + `nvim/lua/autodbg/effects.lua` | 86 + 193 行 | 效果协议整条路（含 16 轮客户端循环、8 MB `k_file_effect_max`、`token=sid:gen` 与重放判定）随 D44 消失 |
| `src/common/base64.*` | 120 行 + 5 条断言 | 帧直接带原始字节 |
| `src/scripting/lua/exec_session.cpp` | 358 行 | D43：会话表、挂起槽、`Mcp-Session-Id`、≤8、30 min 扫描、503 全删 |
| `exec.cpp` 的协程路径 | 345 → 约 120 行 | 不需要 yield ⇒ 不需要协程 |
| `file_io.cpp` 的 `lua_read_file` / `lua_write_file` / `yield_blocker` / `translate_*` | 488 行里约 200 行 | `read_file` / `write_file` 不在 29 契约名内 ⇒ 可删 |
| `stream.cpp` 的 chunked 半边（`make_chunk_head` + `k_head` + `stream_finish` 终止块） | ~40 行 | 这是「传输无关」口号里唯一真正 entangled 的部分 |
| `nvim/lua/autodbg/http.lua` + `stream.lua` 的 chunked 半边 + `init.lua` 的 `stream = false` 回退路径 | 117 + 约一半 229 | `curl` 依赖整块消失 |
| `tools/server_test/e2e_mcp.ps1` | 12.1 KB | 全删 |
| `plan_cli.md` 旧 §3（relay）与 `plan_server.md` §2.1 / §2.2 / §2.6 / §5.2 | — | 随 D41/D42/D44 作废；D44 不需要编号 |
| `PITFALLS.md` S1 / S5 / S8 / S10 / S11 | — | 与 HTTP / 效果协议绑定，分支落地后作废（保留备查） |

不能删：`net.cpp`(531) · `json_writer`(319) · `jsonx`(326) · `png_writer`(264) · `exec_guard.cpp`(106) · `gui.cpp`/`gui_bindings.cpp`(808) · `registry.cpp` 的 29 契约名（含 `BsonEncode`/`BsonDecode`）。

---

## 4. 新增 / 改写

| 项 | 落点 | 说明 |
| --- | --- | --- |
| 双向帧 + 握手 | `src/scripting/lua/stream.hpp/.cpp` | `stream.*` 从 130 行涨到约 200 行（不是净减）：`read_req` / `ack` / `info` / `end` + `control` |
| 控制命令在两个点的 poll | `bindings.cpp` 的 `lua_on_wait_timeout` + `lua_sethook` 钩 | 100 ms 空闲轮询里加一次非阻塞 `select`；这是「引擎空转期间还能收到控制帧」的唯一途径 |
| `f:read()` 的阻塞读 + buffer | `file_stream.cpp` | 切片 `select`（≤250 ms）、每片查 `exec_guard`、stale 帧丢弃、`cancel_notify` |
| `--json` | `launcher.cpp`（选项 → boot，ABI **2 → 3**：`json_mode` / `json_out`，T1 已用）→ `main.cpp` → `output.cpp` | 契约：`{"ok","exit","canceled","detached","pid","stdout","stderr","images","result"}`；`--out=<path>` 写同一份 JSON。❗legacy 变体是 GBK 执行字符集 ⇒ 一律 UTF-8 + `\uXXXX` 转义非 ASCII（`AGENTS.md` §12.6） |
| `--remote` / `--pid` / `--pid=pick` | `boot.h`（ABI **3 → 4**：`remote_*`、`pid`、`read_timeout`、`max_frame`；T1 已把 ABI 用到 3）→ `launcher.cpp` → 模块新增 `client.cpp` | 落点在模块（launcher 不链 winsock）；client 用现成的 local 文件后端 |
| `--server-*` 口径 | `AGENTS.md` §12.1 | `--server-read-timeout`（默认 **5000 ms**，单次文件操作）与 `--server-timeout`（整次执行 wall-clock）分开；`--server-max-body` → `--server-max-frame`；`--server-queue` 保留（执行连接队列）；D21 的认证默认不变，token 走握手帧；前缀保留 `--server-`，client 侧新增 `--remote` |
| `gui.pick()` | `src/core/gui.cpp` + `gui_bindings.cpp` | 新名字，不动 29 契约名 |
| nvim（独立仓库，单独提交） | `nvim/lua/autodbg/*` | `http.lua`、`effects.lua`、chunked 半边删；`modeline.lua` 加 `pid=`；`:AutoDbgKill` / `:AutoDbgDetach` / `:AutoDbgPick` 新增；`addons/.../file_io.lua` 与 `lualib.lua`（`mcp` / `effects` 全局声明）同步；`snippets.lua` 的 `adfile` 片段重写 |

---

## 5. 任务顺序

1. **T1 `--json`**（与传输无关，是 `structuredContent` 的唯一替代；先做它）。
   ✅ **已完成**（分支 `feat/cli-tcp`，ABI 2 → 3；门禁 `e2e_json.ps1` 32 条，legacy 必过）。
2. **T2 双向帧 + 握手**（`read_req` / `ack` / `info` / `end` / `control`）。这一轮先把 `stream.*` 做成传输无关的核。
   ✅ **已完成**：`common/frame.*`（8 字节头 + json 长度反推 + raw u32 前缀，`--selftest` +20 条）+
   `stream.*`（帧类型 1–15、协议版本、传输钩子、`stream_begin_ex`、`stream_read_frame`、
   执行期非阻塞的 `stream_poll_frame` / `stream_poll_and_dispatch`）+
   `--server=tcp` 的 `tcp_server.cpp`（监听 / 握手 / `script`→`out`+`result`+`end` / `control`→`ack`·`info`）+
   **执行期的两个 poll 点**（`on_wait_timeout` 的 100 ms + `lua_sethook`，`kill`/`detach` 的软动作也接了）；
   门禁 `tools/server_test/e2e_tcp.ps1`（26 条，64 / 86 / 86 legacy 全绿）。
   ❗还没做的：`read_req` / `read_resp`（回调里阻塞读）属于 T4；握手 json 里的 `pid` / `filename` /
   `read_timeout` / `max_frame` 尚未消费（客户端字段齐全后才用得上）。
3. **T3 `--remote`**（一次 ABI 变更把 `remote_*` / `pid` / `read_timeout` / `max_frame` 一起加进去，不要分两次；ABI **3 → 4**，因为 T1 已经用 3）。
   ✅ **已完成**：`boot` 加 6 个字段（ABI 4）；launcher 解析 `--remote[=host:port]` / `--remote-token=` /
   `--pid=` / `--read-timeout=` / `--max-frame=` / `--cancel|--kill|--detach|--health`（带 `--remote` 时
   `--version` = 查 server 版本）；模块新增 `client.cpp`（握手 → `script`/`control` → `result` 帧的
   `id` 当退出码，D28 不解 JSON）；服务端改成 **accept 线程 + worker**：控制连接在执行期间也接得住
   （所以 `AutoDbg --remote h:p --cancel` 能从**另一个进程**打断正在跑的脚本），一个执行连接可发多段 `script`。
   门禁 `e2e_tcp.ps1` 26 → 35 条。
   ❗尚未做：握手 json 里的 `pid` / `filename` / `read_timeout` / `max_frame` 还没被服务端消费（`--pid` 也还没
   影响 Attach）；`read_req`/`read_resp` 属 T4。
4. **T4 `open_file():read` 的阻塞语义** + buffer + stale 帧 + `cancel_notify` + `gui.pick()`；跑通「回调里阻塞读 → 超时返回 `nil,"timeout"` → 断点继续」。
   ✅ **已完成**：`file_stream.cpp` 的读向（r/rb + 4096 缓冲 + 逻辑 `tell` + 往返 `seek` + `lines`），
   `stream_wait_frame`（切片 25 ms + 片间查取消 + stale 帧丢弃 + `cancel_notify`），
   `client.cpp` 的本地文件后端（`open`/`data`/`close`/`read_req` → `read_resp`，全二进制，D28），
   `gui.pick()`（`GetCursorPos` → `WindowFromPoint` → `GetWindowThreadProcessId`，降级前台窗口）。
   门禁 `e2e_tcp.ps1` 35 → 41 条（含「假客户端不回 read_req」的超时用例）。
   ⏳ **剩下**：回调里（`Attach`/`Run` 回包内）的真阻塞读只做了单元级验证（`--read-timeout` 路径已通），
   还没盖「断点继续」那一条；`read_req` 目前只发 bytes/line/seek/tell 四种。
5. **T5 删除**：`http.*` / `mcp.lua` / `effects.lua` / base64 / 协程执行器 / 会话表 / `yield_blocker` / nvim 的 `http.lua` + chunked 半边。**必须排在 T2/T4 之后**，否则 `open_file` 在回调里写/读的能力会先失去。
   ✅ **已完成（本仓）**：`server.cpp`（HTTP）/ `server.hpp` / `http.*` / `base64.*` / `exec_session.*` /
   `exec.cpp` 的协程段 / `file_io.cpp` 的效果协议（只剩 client_io 开关）/ `lualib/{mcp,effects,autodbg_api}.lua` /
   `registry.cpp` 的 `read_file`/`write_file` 全删；配置解析搬到 `server_config.*`；
   **`--server=http` 改为报案（退码 2），`--server` / `--server=tcp` 都是 raw TCP**；
   `--selftest` 101 → 67（http/base64 两段随依赖一起走）；门禁脚本删 `e2e.ps1` /
   `e2e_mcp.ps1` / `e2e_io.ps1` / `nvim_smoke.*`，`e2e_gui.ps1` 改成本地 CLI（21 条）。
   顺手修了一个真缺陷：本地模式下脚本的 `...` 第一个参数竟然是脚本路径（命令行未对齐）。
   ⏳ **剩下**：`nvim/`（子模块、独立仓）的 `http.lua` / `effects.lua` / chunked 半边 ——
   随插件改到 raw TCP 客户端时在它自己的仓里删，并恢复一份 `nvim_smoke` 等价门禁。
6. **T6 agent 文档**：`tools/agent/autodbg-skill/SKILL.md`（Agent Skills 格式）+ 三五个 recipe。在 CLI 口径下这份 markdown 是**唯一契约文档**，不是锦上添花 —— `autodbg_api.lua` 的内容搬过去，否则模型只能靠训练数据猜参数形状（就是 S12 复发）。
   ✅ **已完成**：`tools/agent/autodbg-skill/`（`SKILL.md` + `references/{cli,lua-api,pitfalls}.md` +
   `recipes/{run-and-break,attach-existing,scan-and-patch,gui-automation,remote-agent}.md` + `README.md`）。
   `lua-api.md` 的正文从 `09174da:lualib/autodbg_api.lua` 用脚本按 topic 搬出来（不是手抄），
   并按 T5 的新口径改写了 `files` 段（`read_file`/`write_file` → `open_file` 文件对象）。
7. **T7（可选，独立立项）**引擎 `begin/pump/continue` + `--repl`。D45 之后它不再是文件读的前提，但「不靠回调、看着目标跑」仍然需要它。

---

## 6. 已验证事实（留档，不要重复试验）

pi 侧（这些是 D41 的依据；扩展代码在 `git stash@{0}`，未入库）：

| 事实 | 依据 |
| --- | --- |
| MCP 的 `mcp.json` `url` **不做 `${VAR}` 展开**（原样透传，连接失败）；只有 `env` / `headers` 的值支持 `${VAR}` 与 `!command` | 实测 |
| MCP 单请求超时默认 **60 s** → `lua_eval` 跑 `Run` 必须显式放大 | pi `docs/mcp.md` |
| `mcp.json` 里同名服务器**盖住**扩展注册的同名服务器；扩展注册**不落盘** | pi `docs/extensions.md` |
| MCP 工具默认 `exposure: codemode`（不声明给模型，让 codemode 脚本去调）—— 等于把 MCP 当脚本里的一行调用 | pi `docs/mcp.md` |
| 脚本化测试 pi 不需要模型：`pi --mode rpc --no-session` + stdin 写 JSON | pi `docs/rpc-commands.md` + 实测 |

AutoDbg 侧仍然成立的事实：执行串行（并发 1）；`Run`/`Attach` 阻塞到目标退出或 `Detach()`；没有「已停止的目标」这个可对外暴露的状态（S12）；`f:write` 在回调里可用是因为它只是一次带超时的 `net_send_all`（不需要 yield）。

---

## 7. 门禁与文档

新增 `tools/server_test/e2e_tcp.ps1`（`e2e.ps1` 现有的裸 `TcpClient` 逐帧校验段是底子），至少覆盖：

- 握手版本不匹配 → `error` 帧；非 loopback 无 token → 拒；
- 回调里 `f:read()` 阻塞 + 超时返回 `nil,"timeout"` 且断点继续；
- 读取期间 `--cancel` 立即生效 + stale 应答被丢弃（`cancel_notify` 路径）；
- `--kill` 只结束当前 `Attach`（脚本里跑两个 `Attach` 的用例）；
- `--detach` 在 NT+ 真 detach 且 pid 可复用；在 Win9x 口径（走 golden 那条）；
- 客户端中途断开 → 置取消标志；
- `f:read("l")` / `lines()` / `f:seek()` / `f:tell()` 的二进制逐字节往返与 buffer 语义。

`--selftest`：http 段约 29 条（70 条里的最大块）随删除消失，要补帧编解码 + `decide_console_action` 的断言才配得上「改完必跑」。

`AGENTS.md`：§12.3（MCP）整节删除；§12.4 改写成「帧协议 + `open_file` 文件对象」；§12.1 的端点改成握手帧 + `--health`/`--version`；§4 门禁表的 `e2e_mcp.ps1` 换成 `e2e_tcp.ps1`；§9 文档导航加 `plan_cli.md`。
`PITFALLS.md`：新增 S13（回调里阻塞读时 `lua_sethook` 不会触发 ⇒ 取消只能靠等待循环里的 guard 检查；被取消的请求可能已有 stale 应答，必须丢弃）；S1 / S5 / S8 / S10 / S11 标注作废。S12 的教训（每个对外接口都要有门禁断言）在帧命令面上继续适用。

---

## 8. 换机器继续

```bat
:: 分支（代码工作从这里开始；文档已在 main 上落地）
git checkout -b feat/cli-tcp

:: 构建（控制台代码页必须是 936）
cmd /c "chcp 936 && make.bat a 64"
cmd /c "chcp 936 && make.bat a 86"

:: 门禁
uv --directory tools/api_check run scan_dir.py
.out\AutoDbg_64.exe --selftest
pwsh tools/server_test/e2e.ps1 -Render 64
pwsh tools/server_test/e2e_tcp.ps1 -Render 64        :: 新增
pwsh tools/server_test/e2e_gui.ps1 -Render 64
pwsh tools/server_test/e2e_io.ps1 -Render 64          :: 重写后
pwsh tools/server_test/nvim_smoke.ps1 -Render 64
```

未入库的 pi 扩展在 `git stash@{0}`（`git stash pop` 取回）。它不会被提交，也不被本方案依赖。
