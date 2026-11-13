# plan.md —— 剩余工作

> 本文件是 `plan_p7_win9x.md`、`plan_server.md`、`plan_cli.md` 的合并版。
> 只留**剩余工作 / 待确认 / 已作废留档**：决策在 `AGENTS.md` §13（D1–D50），坑在 `PITFALLS.md`，
> 门禁口径在 `AGENTS.md` §4，对外契约文档在 `tools/agent/autodbg-skill/`。本文件不重抄这些。
> 时间线：`79b3de2`（项目骨架）里落地 `plan_p7_win9x.md` + `plan_server.md` → `25a9295` 定稿 CLI + raw TCP
> 路线（D41–D50，即 `plan_cli.md`）→ T1–T6 落地（merge `5e938d3`，分支 `feat/cli-tcp`）→ 收尾 fix
> （`4695594` stale `read_resp`、`941ddd5` 网卡横幅、`7522e00`/`f240a9a` nvim 子模块、`d700f5c` 绑定退码）。
> 合并时删掉的（不再恢复）：已执行的删除清单与新增/改写清单、逐条 T1–T6 过程记录、帧全表（已在
> `tools/agent/autodbg-skill/references/cli.md`）、`plan_server.md` §1 的代码落点（列的文件已不存在）、
> `plan_cli.md` §8 的换机器命令（已在 `AGENTS.md` §1/§4）、pi 侧的 MCP 实测事实（只作为 D41 的依据，
> 那个 stash 已按计划丢弃，`git stash list` 为空）。

---

## 0. 基线（已落地，不要重复审）

| 任务 | 落点 | 门禁 |
| --- | --- | --- |
| T1 `--json` / `--out`（D50） | `launcher.cpp` → `boot`（ABI 3）→ `main.cpp` / `output.cpp` | `e2e_json.ps1`（32 条 Check） |
| T2 传输无关的帧核 + `--server=tcp`（D42） | `src/common/frame.*` + `src/scripting/lua/stream.*` + `tcp_server.cpp`；执行期两个 poll 点（`on_wait_timeout` / `lua_sethook` → `exec_guard.cpp:133` `stream_poll_and_dispatch`） | `e2e_tcp.ps1`（48 条） |
| T3 `--remote` 客户端（ABI 4） | `src/scripting/lua/client.cpp`；服务端 accept 线程 + worker（控制连接在执行期间也接得住） | `e2e_tcp.ps1` 的 `client.*` 段 |
| T4 `open_file` 读向 + 阻塞读 + `gui.pick()`（D45/D49/D48） | `file_stream.cpp`（4096 缓冲、逻辑 `tell`、往返 `seek`、`lines`、`stream_wait_frame` 切片 25 ms + stale 丢弃 + `cancel_notify`）；`core/gui.cpp:747 window_pick` + `gui_bindings.cpp:832` | `e2e_tcp.ps1` 的 `open.*` / `read.timeout-*` / `callback.read-*` 段 |
| T5 删除 HTTP / MCP / effects / base64 / 协程执行器 / 会话表（D41/D43/D44） | `--server=http` → 报错退 2；`registry.cpp` 不再有 `read_file`/`write_file`；`lualib` 只剩 `inspect`/`jsonx`/`moses` | `--selftest` 101 → 67；门禁脚本只剩 `e2e_tcp` / `e2e_json` / `e2e_gui` / `nvim_smoke` / `check_skill` |
| T6 agent 技能包 | `tools/agent/autodbg-skill/`（`SKILL.md` + `references/{cli,lua-api,pitfalls}.md` + 5 个 recipe） | `check_skill.ps1`（14 条 = 6 静态 + 8 个含 lua 块的 md；39 个片段） |
| nvim 客户端（独立仓 `AutoDbg.nvim`） | 远程改成 raw TCP（`nvim/lua/autodbg/tcp.lua`），`curl` 依赖消失（子模块 `04bbae4`）；`:AutoDbgKill` / `:AutoDbgDetach` 已加 | `nvim_smoke.ps1`（恢复后的等价门禁，35 条 Check（含 `repl.*` 三条）） |

仍然成立的不变量（D43 之后）：执行串行（并发 1）；`Run`/`Attach` 阻塞到目标退出或 `Detach()`；没有「已停止的目标」这个可对外暴露的状态（`PITFALLS.md` S12）；跨调用唯一的状态是**被调试进程本身，靠 pid 显式指认**（D48）；执行连接的唯一 reader 是脚本线程（三个消费点：`f:read()` 等待循环、`on_wait_timeout` 的非阻塞 poll、`lua_sethook` tick）；`f:write` 在回调里可用是因为它只是一次带超时的 `net_send_all`。

已经验证过、不必再验的事实：Win9x 侧见 `PITFALLS.md` W 节 + 本文件 §1.3；`SymFunctionTableAccess`/`UnDecorateSymbolName` 的退化路径（golden `w3_dbghelp.txt`）；UTF-8 BOM 脚本在 98 上 `exit=3`；LuaJIT 在 Win98 不可用（记 xfail）。

---

## 1. 剩余工作

### 1.1 CLI / 帧协议

| # | 项 | 落点 | 验收 |
| --- | --- | --- | --- |
| 1 | `decide_console_action` 的断言 | `src/common/selftest.cpp`（新增一段） | `--selftest` 里断言三档：`SHIFT_PRESSED` 且能不终止 → `2`；非 Shift 或不能不终止 → `1`；无匹配输入记录 → `0`。目前只有代码路径（`console_input.cpp:9`），没有断言 |
| 2 | 握手帧的 `pid` / `filename` 被服务端消费 | `tcp_server.cpp`：`pid` → 默认 Attach 目标；`filename` → chunkname（D26 的 `@<文件名>`） | `e2e_tcp.ps1` 加 `handshake.pid-defaults-attach` 一类用例。❗`max_frame`（`tcp_server.cpp:133` 帧上限 + `info` 回显）与 `read_timeout`（`file_stream.cpp` 的 `g_read_timeout_ms`）**已消费**，只剩这两个字段 |
| 3 | `--pid` 影响 Attach | 与 2 同批（模块侧，不在 launcher） | 显式 `--pid=` 才是默认目标；禁止隐形状态（「记住的 pid」只存在脚本头标记里，D48） |
| 4 | CLI 侧 `--pid=pick` | `src/sys/pack/launcher.cpp` 解析 → 传到模块 | pick 必须在**目标机**上解析（`core/gui.cpp`，launcher 没有 core/gui）；取不到就报错而不是猜。Lua 侧 `gui.pick()` 已实现 |
| 5 | `:AutoDbgPick`（nvim，独立仓单独提交） | `nvim/lua/autodbg/init.lua`（`modeline.lua` 已支持 `pid=`） | 在目标机跑一次 pick，把结果写回头标记，不落内存 |
| 6 | T7（可选，独立立项）：引擎 `begin/pump/continue` + `--repl` | `src/core/debug_loop.cpp` 的模型变更 | D45 之后它不再是文件读的前提，但「不靠回调、看着目标跑」仍然需要它；做之前先单独设计一轮 |
| 7 | `BsonDecode` 仍是桩（`bson.cpp:308`） | 不为它单独实现 | 它是 29 个契约名之一（§5 第 6 条）→ 保留名字与桩；BSON 的结论见 §3 |

### 1.2 server / GUI 侧

- **实测回填**（原 `plan_server.md` §2.4，仍是空的）：
  - 延迟：`control health` 往返、空脚本一次执行、带 100 KB 输出的执行、`gui.screenshot`（PNG stored deflate）耗时；
  - 内存：空载工作集、单次执行峰值、输出缓冲上限（4 MB/流）的触发点；
  - 9x：server 模式在 Win98 能否起来（Winsock 2 可用）、Win95 退 `wsock32` 的行为（若可测）。
- **`io.write` / `puts` 不进输出**（fd 级 `_dup2` 重定向）：实验项，约 120 行 + 2 个新 msvcrt 导入，且手工映射下 `_iob` 初始化时机不可控 → 失败就保持现状。要不要试一次 = §2 第 5 条。
- **纯 Lua 死循环的抢占粒度**（100 万条指令一次）：可接受，不调小。
- **请求级切换 flavor/variant**：不做（要新增 launcher→module 派发契约 = ABI 变更）。
- **并发**：执行串行（并发 1）是设计如此 —— 单 operator 的远程调试后端，不做多租户。
- **引擎阻塞路的取消端到端**：已由 `e2e_tcp.ps1` 的 `cancel/kill/detach.engine-wait-*` 六条覆盖（原 `e2e.ps1` 的 `-EngineCancel` 随 HTTP 一起删除）→ 这条已闭环。

### 1.3 Win9x 矩阵（原 `plan_p7_win9x.md`）

口径与前提不变：被测产物是 **`AutoDbg_86.exe`**（`98` 架构已退役，D1），在 Win98 上由平台探测自动选 **legacy 变体**；脚本跑在 host（PowerShell 7），产物跑在 guest（中文 Win98 第二版，CP936），全部走 `vmrun`；退出码 `0 = PASS / 1 = FAIL / 2 = SKIP`（无 VM / 凭据 / Tools 时 SKIP，不算失败）。harness 入口 `pwsh tools/w9x_test/w9x.ps1 -Action check|run|record|revert|gate`。

> ❗本机现状：`tools/w9x_test/config.local.ps1` **不存在**（只有 `config.example.ps1`），所以这一条门禁在本机器上是 SKIP。换机器要重配的都在 `config.local.ps1`（`$VMRUN` / `$VMX` / `$SNAP` / `$GU` / `$GP` / `$GUEST_DIR`）。

**2.1 加载与变体（脚本侧可自动化）**

| # | case | 期望 | 状态 |
| --- | --- | --- | --- |
| 01 | 平台探测 → legacy | 98 上映射的是 `AutoDbgLegacy_lua_86.dll`（`--list` 或诊断行确认） | 待做 |
| 02 | legacy + lua | 跑通，结果与 golden 一致 | 待做 |
| 03 | legacy + luajit | **xfail**（Win98 上 LuaJIT 不可用）：记录实测，不进硬门禁 | 待做（记录型） |
| 04 | `--variant=modern`（显式） | 干净失败 + 回退到 legacy；stderr 可捕获 | 待做 |
| 05 | `--list` | 4 个 entry；当前生效变体 = legacy | 待做 |
| 06 | `--module-dir` | 从 guest 磁盘读 DLL 仍走内存映射 | 待做 |
| 07 | 无参（文件对话框） | 不自动化，只人工确认 9x 上 A 版降级不崩 | 人工 |

**2.2 dynapi 降级点** —— 自动化部分 = 每轮开头的静态门禁（`-Action gate` → `tools/api_check/scan_dir.py`），它保证 x86 产物导入表里不会出现这些符号；「降级路径真的走到」只能人工在 98 上确认一次，**填不上就保持待确认，不能因为门禁绿就当作通过**。

| # | 符号 | 位置 | 98 上要确认什么 | 结果 |
| --- | --- | --- | --- | --- |
| 10 | `IsProcessorFeaturePresent` | `main.cpp`（`__isa_available_init`） | 不崩，走 x87（`flt.asm`） | 待确认 |
| 11 | `GetAncestor` | `core/util.cpp` `LjGetRootWindow` | 窗口选取走 `GetParent` 循环 | 待确认 |
| 12 | `FlashWindowEx` | `core/util.cpp` `LjFlashWindow` | 窗口只闪一次（`FlashWindow`） | 待确认 |
| 13 | `SymFunctionTableAccess` / `UnDecorateSymbolName` | `core/stack_trace.cpp` | `frames=0` 的退化路径生效 | ✅（golden `w3_dbghelp.txt`） |
| 14 | `GetModuleHandleExA` | `src/sys/include/lj.h` 垫片 | 98 上 LuaJIT 不可用 → 代码审阅 + 在能跑 LuaJIT 的机器上确认垫片生效 | 待确认 |
| 15 | `strtoull` | `src/sys/strtoull.c` | `tonumber(s, base)` 用**非常量**输入跑一次 | 待确认 |
| 16 | `__DestructExceptionObject` | `src/sys/destruct_eh.c` | 转发失败时降级「不析构」、不崩 | 待确认 |
| 17 | `__chkstk` | legacy 链 `ntdll.lib` | 构造 >4 KB 栈帧的探针，看是否崩（已知风险项） | 待确认 |

**2.3 编码与文件系统**

| # | case | 期望 | 状态 |
| --- | --- | --- | --- |
| 20 | GBK 脚本（CP936 执行字符集） | guest 里抓不到 stdout → 人工确认 | 人工 |
| 21 | UTF-8 BOM 脚本 | 98 上 Lua 5.5 直接 `exit=3` | ✅（golden `bom.txt`） |
| 22 | 中文路径 + 长路径 | 能打开、能跑；`GetModuleFileName(NULL)` 仍是主 exe（§11.5） | 待做 |
| 23 | 只读 / 不可写目录 | 模块不再写脚本目录（编辑器提示已移到 nvim 插件），只读目录下应能正常跑完 | 待做 |

**2.4 调试 API 在 9x 上的行为差异（记录型，允许 xfail）**

| # | case | 期望 | 状态 |
| --- | --- | --- | --- |
| 30 | attach 到已运行进程 | Win9x Debug API 不支持 attach → 干净报错，不崩 | 待做 |
| 31 | `GetThreadContext` / `SetThreadContext` | 32 位 CONTEXT 正常；记录实际行为 | 待做 |
| 32 | 硬件断点（DRx） | 9x 上可能不生效 → 记 xfail | 待做 |
| 33 | 栈回溯 | 走 2.2 第 13 条的降级路径，输出修饰名 | 部分（golden 有基线） |
| 34 | 被调试目标 | 需要 x86 fixture（架构必须与产物一致） | **阻塞项：缺 fixture**（本仓库暂无 `tests/fixtures/bin`） |

**2.5 观察项（不设硬门禁，只记录）**：启动解压 2.3 MB 在 98 VM 上的实际耗时（host ≈19.5 ms，98 上大概率 100–300 ms）；峰值内存 ≈2.9 MB 在 98 上的表现（`VirtualAlloc` 粒度与提交页行为不同）；映射器逐节 `VirtualProtect` 在 98 上是否全部成功；`C:\WINDOWS\TEMP` 之外再试一个中文路径目录（与 case 22 合并做）。

**2.6 harness 自身的待补**：`$VMRUN` / `$VMX` / `$SNAP` / `$GUEST_DIR` 目前是机器相关默认值 + dot-source 顺序生效 → 建议改成「显式从 config 读，缺省才用内置值」并打印实际生效值；`-Action check` 建议再加一行「实际生效的 guest 工作目录」；只跑 01–04 + 10–16 的瘦模式（`--quick`）尚未实现（`tools/w9x_test/scripts/quick.lua` 已有，但 `-Test` 没串起来）。

已在 98 上验证过、不必再摸索的事实（原 §1.2）：`runProgramInGuest` 会传回真实退出码，但 guest 里的调试会话不退出 → 探针只能靠超时结束，判定以结果文件内容为准；guest 里没有可用的命令执行通道（`command.com /c` 经 vmrun 不执行）→ 没有重定向、没有 mkdir、没有删文件；测试脚本一律用 `io.open` 写结果文件并逐条 `flush`；golden 统一 LF，比较按「CRLF→LF + 去末尾空行」归一化（D15）。现有 golden：`probe.txt`、`bom.txt`、`w3_cmdtest.txt`、`w3_dbghelp.txt`、`w3_detach_quick.txt`。

---

## 2. 待确认（开工前要定）

1. Win9x 快照策略：继续复用既有快照 `1`（关机态 → 每轮 `start` + 等 Tools），还是建一个专门的 clean-w9x 快照？
2. case 03（LuaJIT on 98）的期望值：保持 xfail 只记录，还是先定位 `cannot create state: not enough memory`？
3. case 34 的 fixture 从哪来：新建 `tests/fixtures/bin`，还是由 `tools/server_test/gui_target.ps1` 那类脚本现场生成？（与 §1.2 的「引擎阻塞路」用例可以合并 —— 那条现在靠 `e2e_tcp` 的空转断言，真 fixture 才能测「长时间 `Run()` 中被 cancel」）
4. Win9x 这些 case 要不要进默认 `-Action run`（会显著拉长单轮时间），还是继续按 `-Test <name>` 逐条跑。
5. fd 级输出重定向（覆盖 `io.write`）值不值得试一次（约 120 行 + 2 个 msvcrt 导入）。

---

## 3. 已作废 / 结论为「不做」（留档，避免重复讨论）

| 项 | 结论 | 出处 |
| --- | --- | --- |
| HTTP 全套（含 `POST /run`、`/health`、`/version`、SSE 增量帧、`GET /screenshot`、`Content-Length` 走私防御、411/413/501） | 已删（T5） | D41 |
| MCP / JSON-RPC 分发 / 会话表 / `Mcp-Session-Id` / ≤8 上限 / 30 min 回收 / 503 | 已删（T5） | D41 / D43（D35 作废） |
| `read_file` / `write_file` + 效果协议 + base64 + 协程执行器 + `yield_blocker` | 已删（T5）；文件 I/O 只有 `open_file` 文件对象 | D44（D37 作废） |
| 专用命令行 client（`tools/adbgcli.ps1`） | 不做：`--remote` 客户端就是 client | D29（原 §2.6 作废） |
| BSON-RPC（`BsonDecode` 200–300 行 + Content-Type 分流 + 测试矩阵翻倍） | 不做；`BsonDecode` 保持桩 | 原 `plan_server.md` §5.2（作废，留一句结论） |
| nvim 的 `curl` 传输与 `setup{ transport = "curl" }` 二选一 | 已完成即作废：插件只剩 raw TCP（`tcp.lua`），`curl` 依赖消失 | 子模块 `04bbae4` |
| `98` 架构 | 退役，legacy 变体取代；`make.bat a 98` 报错 | D1 |
| Win95 RTM/gold（只有 `crldll.dll`）、x64 on 98、modern 变体在 9x 的功能、GUI 自动化（文件对话框 / `--quiet` 弹窗）、往 guest 装任何运行环境 | 明确不做（只人工确认） | 原 `plan_p7_win9x.md` §4 |
| 行为类缺陷（如调试会话不退出） | **只记录不修**，等 Win9x 环境就绪 | `AGENTS.md` §4 |
| 请求级 flavor/variant、并发 >1 | 不做 / 设计如此 | `AGENTS.md` §12.1 |

---

## 4. 本次合并顺带修掉的事实（原三处文档漂移）

- `AGENTS.md` §2 / §7 的烘焙库清单：`effects`、`mcp` 已随 T5 删除 → 实际是 `inspect`、`jsonx`、`moses`。
- `AGENTS.md` §7 `registry.cpp` 行仍写 `+ read_file/write_file`（T5 已删，只剩 29 契约名 + Var 元表 + `gui` 表）。
- `AGENTS.md` §12.5 仍描述 `nvim/lua/autodbg/effects.lua`（插件里已无该文件）与 §12.1 的「`--server=http` 仍是现行 HTTP（待删）」。
- `AGENTS.md` §4 的断言数按脚本内 `Check` 调用点重算：`check_skill` 14 条（6 静态 + 8 个含 lua 块的 md）/ 片段 36 → **39**；`nvim_smoke` 24 → **31**；`e2e_gui` 21 是不带 `-AllowInput` 的口径（带则 26）。
- `PITFALLS.md` 第 5 行与 S10 的 `plan_server.md` 引用改指本文件；文档导航（`AGENTS.md` §0.1 / §9、两个 README）指向 `plan.md`。
