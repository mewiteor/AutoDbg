# plan_server.md —— server / MCP / GUI / client 文件 I/O（剩余工作）

> 状态：**主体已实现**（HTTP server、取消/超时、neovim 插件、GUI 自动化、MCP、client 文件 I/O），
> 下面只列**还没做**的部分（§2）、**已知缺口**（§3）与**待确认**（§4）。
> 现行接口约定（选项 / 端点 / `gui` 表 / MCP 工具 / `read_file`·`write_file` / 插件行为）见 `AGENTS.md` §12；
> 决策见 `AGENTS.md` §13 的 D20–D38；已踩的坑见 `PITFALLS.md` 的 **S 节**（server）与 **N 节**（nvim）。
> 门禁：`tools/server_test/*.ps1`（HTTP / GUI / MCP / client I/O / nvim smoke），全部不需要 VM。

---

## 1. 代码落点（改之前先知道文件在哪）

| 层 | 文件 |
| --- | --- |
| 传输 | `src/common/net.*`（winsock 动态载入）、`src/common/http.*`（HTTP/1.1 子集，不含 socket） |
| 序列化 | `src/common/json_writer.*`、`src/common/base64.*`、`src/common/png_writer.*`、`src/common/dbl2str.c`（Ryu） |
| 服务端 | `src/scripting/lua/server.*`（accept/worker、路由、认证、`/effects`）、`exec.*`（协程执行器）、`exec_session.*`（持久会话 + 挂起槽）、`exec_guard.*`（取消/超时）、`output.*`（输出收集 + `print` 覆写） |
| Lua API | `src/scripting/lua/gui_bindings.*`（`gui` 表）、`file_io.*`（`read_file`/`write_file`）、`registry.cpp`（注册入口） |
| 烘焙库 | `lualib/mcp.lua`（JSON-RPC 分发）、`lualib/effects.lua`（客户端结果 → Lua 值） |
| 客户端 | `nvim/lua/autodbg/*.lua`（另一仓库：`AutoDbg.nvim`） |
| 测试 | `tools/server_test/{e2e,e2e_gui,e2e_mcp,e2e_io,nvim_smoke}.{ps1,lua}`、`gui_target.ps1`（WinForms 靶子） |

---

## 2. 剩余工作

### 2.1 增量流式输出（D36 后半）

| 项 | 现状 | 要做的 | 验收 |
| --- | --- | --- | --- |
| `/run` 实时输出 | 跑完一次性回（`begin/write/end`） | 响应写入器改成持有 socket 的增量接口，`output_sink` 每攒够一帧就 flush；`Connection: close` 语义不变 | 长脚本在 200 ms 内能看到第一批输出 |
| SSE 增量帧 | 单事件（`event: message` + 完整响应） | `notifications/message` 语义的增量帧（D36 预留位） | `Accept: text/event-stream` 下能收到 ≥2 帧 |
| 断开处理 | 未见明确验证 | `send` 返回 `WSAECONNRESET` → 置取消标志（`exec_guard` 已有状态） | 客户端中途断开后脚本在下一次 `on_wait_timeout` 内退出 |

### 2.2 `GET /screenshot` 端点（S5 剩余）

- 现状：只能用 MCP 的 `gui_screenshot` 或 `/run` 里调 `gui.screenshot`。
- 要做的：`GET /screenshot[?x=&y=&w=&h=&scale=&format=png]` 直接回 `image/png` 原始字节（约 20 行，复用 `png_writer` 与 `gui` 同一实现）。
- 理由：这是「避免 base64 膨胀」最便宜的做法（比 BSON 便宜 1/20，见 §5）。

### 2.3 neovim 插件：纯 `vim.loop` 传输（无 `curl` 依赖）

- 现状：`vim.system` + `curl`（脚本走 stdin，不落盘）；Windows 10+ 与主流 Linux 都有 `curl`。
- 要做的：`nvim/lua/autodbg/http.lua` 增加一条基于 `vim.loop`（`uv.tcp`）的实现，`setup{ transport = "curl"|"uv" }` 二选一，约 80 行。
- 验收：`tools/server_test/nvim_smoke.ps1` 在 `-Transport uv` 下同样全绿。

### 2.4 性能与资源实测回填

| 项 | 记录内容 |
| --- | --- |
| 延迟 | `/health` 往返、`/run` 空脚本、`/run` 带 100 KB 输出、`screenshot`（PNG-stored）耗时 |
| 内存 | 空载工作集、单请求峰值、输出缓冲上限（4 MB/流）触发点 |
| 9x | server 模式在 Win98 能否起来（Winsock 2 可用）；Win95 退 `wsock32` 的行为（若可测） |

### 2.5 「引擎阻塞路」的取消端到端验证

- 现状：`/cancel` 的**纯 Lua 路**（`lua_sethook`）已实测 515 ms 生效；**引擎阻塞路**（`on_wait_timeout`）只有代码路径，没有 fixture 做端到端。
- 要做的：借一个会长时间 `Run()` 的 x86 fixture（与 `plan_p7_win9x.md` 2.4 case 34 的 fixture 需求可以合并），验证 `/cancel` 与 `--server-timeout` 在引擎阻塞期间真的打断。
- 验收：`tools/server_test/e2e.ps1` 增一条 `-EngineCancel` 用例。

### 2.6 可选：命令行 client（`tools/adbgcli.ps1`）

- 现状：`curl` 能跑，但脚本一旦 `read_file`/`write_file` 就要手工做 `/effects` 多轮循环。
- 建议（未做）：写一个 **PowerShell** 封装（约 150 行），复用 `tools/server_test/e2e_io.ps1` 里那套效果循环，提供「从文件或 stdin 读脚本 → 打到远端 → 回显输出与退出码」。
- 不做专用 C++ 客户端（D29）。
- 只有 CI 需要「与平台无关的一条命令」时，才考虑升级成 C++ 宿主工具。

### 2.7 文档

- `AGENTS.md` §12 与实现保持一致（改接口/字段时同步）。
- `nvim/README.md`（另一仓库）与插件行为保持一致。

---

## 3. 已知缺口（记录备查，不假装没有）

| 缺口 | 影响 | 现状/态度 |
| --- | --- | --- |
| `io.write` / `puts("done")` 不进响应 | 脚本里 `io.write` 的输出只在真 stdout 上 | 已知；fd 级 `_dup2` 重定向（需 2 个新 msvcrt 导入，且手工映射下 `_iob` 初始化时机不可控）作为实验项，失败就保持现状 |
| 纯 Lua 死循环的抢占粒度 | 100 万条指令一次检查 | 可接受；调小会影响性能 |
| 单请求执行超时 | 只能在 `on_wait_timeout` 注入，非 `Run()` 的长脚本靠 `lua_sethook` | 已覆盖两条路，语义写进 §12 |
| 请求级切换 flavor/variant | server 的 flavor 由「映射到的模块」决定 | 不做（要新增 launcher→module 派发契约 = ABI 变更） |
| 多租户并发 | 执行严格串行（并发 1），目标进程是全局唯一资源 | **设计如此**：这是「单 operator 的远程调试后端」 |
| `BsonDecode` | 仍是桩（`bson.cpp` 报 `not implement BsonDecode yet!`） | 不为此单独实现，见 §5 |

---

## 4. 待确认

1. 增量流式（§2.1）现在做还是等有真实需求（长脚本的实时反馈）时再做。
2. `tools/adbgcli.ps1`（§2.6）要不要做；要做就按 PowerShell 版落。
3. nvim 的 `uv` 传输要不要设为默认（去掉 `curl` 依赖）还是保持 `curl` 优先。
4. fd 级输出重定向（覆盖 `io.write`）值不值得试一次（成本约 120 行 + 2 个 msvcrt 导入）。

---

## 5. 两项已评估、结论为「不做」的需求（留档，避免重复讨论）

### 5.1 专用命令行 client

`curl` / `Invoke-WebRequest` / nvim 插件已覆盖「从文件或 stdin 读脚本、把输出打到 stdout」的使用场景；
专用 C++ 客户端只会多一份需要同步维护的参数解析与输出格式（约 250–350 行）。→ **D29：不做**。真需要时按 §2.6 的 PowerShell 版起步。

### 5.2 BSON-RPC

- 收益点很窄：现状唯一的大二进制负载是 `gui_screenshot`（PNG ≈ 521 KB → base64 后 ≈ 695 KB）。
- 代价很大：`BsonDecode` 是**桩**（要先把 200–300 行实现补上）+ C 侧 Content-Type 分流 + Lua 侧分发/序列化 + 客户端 BSON 编解码 + 测试矩阵翻倍（`/run`、`/effects`、`/mcp` × JSON/BSON × 4 个模块变体）；而且 MCP 规范只有 JSON-RPC，**BSON 对 AI/MCP 生态零收益**。
- 更便宜的替代：`GET /screenshot` 直接回原始字节（§2.2）。
- 若将来仍要做：**只做 `/run` 一条路径**（nvim 专用），不碰 MCP、不碰 `/effects`，并先把 `BsonDecode` 实现掉。
- 已留的缝：序列化/分发都在 Lua 层（`mcp.lua` / `effects.lua`），真要加时 C 侧几乎不用动。
