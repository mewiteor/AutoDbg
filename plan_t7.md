# plan_t7.md —— 低级引擎 API（`begin` / `pump` / `cont` / `end`）+ REPL

> 分支：`feat/t7-low-level-api`。本文件是 T7 的独立计划（`plan.md` §1.1 第 6 条是它的出处，`plan.md` 不动）。
> 决策仍归 `AGENTS.md` §13；坑归 `PITFALLS.md`；对外契约文档归 `tools/agent/autodbg-skill/`。
> 门禁基线（本轮实测）：import 12 产物全过 / selftest 67 / e2e_tcp 48 / e2e_json 32 / e2e_gui 21（26）/ nvim_smoke 32 / check_skill 14（39 片段）。
> 本文件里所有「已核实的落点」都带文件:行号，便于改之前先验证。

---

## 0. 为什么要做，边界在哪

`Run`/`Attach` 阻塞到目标退出或 `Detach()`，所以「断点命中 → 停下来逐条操作 → 再放行」做不到。
D45 之后 T7 不再是文件读的前提，但它是三件事的唯一前提：

1. **停下来看**：命中后读上下文 / 栈回溯 / 关键内存，再决定放行还是吞异常。
2. **AI 的节奏**：AI 比目标慢几个数量级，不能停在断点上等 AI ⇒ 必须有「记录并放行」这一档（见 §2.2）。
3. **可证伪的数据面**：AI 与 IDA 的联动需要稳定的键与记录形状（见 §7）。

边界：core 只暴露**状态机 + 数据面原语**；一切「策略」（条件断点、命中后做什么、输出格式）归绑定层 / Lua。

---

## 1. 不可动的硬约束

| 约束 | 出处 |
| --- | --- |
| `Run` / `Attach` 等 29 个契约名可观察行为逐字节不变 | `AGENTS.md` §5 第 6 条 |
| `DebugCallbacks` 布局与 `WaitTimeoutAction` 枚举值不可动；`on_wait_timeout` 签名保留 | §7、`src/core/types.hpp:226-276` |
| `src/core/**` 不感知 Lua，也不感知 socket（只认 `windows.h`） | §5 第 5 条 |
| 映射模块里禁止 `thread_local` / 函数内非平凡构造的 static → pump 之间的状态**必须是会话成员** | §11.5、PITFALLS M10 |
| 执行串行（并发 1）；执行连接唯一 reader 是脚本线程；取消靠 `on_wait_timeout`(100 ms) + `lua_sethook`(100 万指令) | D32 / D43 / S13 |
| 新能力只能落在**新全局表**（`dbg`）或新名字；不许平铺全局函数 | §5 第 6 条 |
| 结构体布局与 `#pragma pack(1)` 不可动（`BreakpointInfo.modname[1]` 是变长尾数组） | §7、`types.hpp:56-66` |
| 新 CRT / 系统符号先查导入表；NT-only 的调用一律 dynapi + 降级 | §5 第 1/3 条 |

---

## 2. 低级 API 面

### 2.1 状态机（T7 的核心）

`debug_loop.cpp` 现在是一个 `while (cont())`：`WaitForDebugEvent(&e, 100)` → 派发 → 行尾 `ContinueDebugEvent`（755）。拆成：

```text
begin(params, &err)              Attach / CreateProcess；不循环
pump(timeout_ms, autocont, &err) 一次 wait + 派发；autocont = false ⇒ ❗不 continue
pump_now()                       pump(0, false)：REPL 与回调里唯一能用的形态
cont(pid, tid, code)             ContinueDebugEvent；code 显式（含「原样交给程序」）
end(&err)                        run_shutdown（761）+ detach；D47 的降级由返回值上报
```

`run()` 保留原语义 = `begin` + `while` + `pump(…, autocont = true)` + `cont` 的封装 ⇒ `Run` 契约名一个字不改。

必须从循环栈帧搬进会话成员（这是 T7.1 的真正工作量，缺一个就是跨 pump 状态丢失）：

| 现在（行号） | 目标 |
| --- | --- |
| `FirstBPX` (182) | `m_first_bp` |
| `reset_bpx {modname,rva,ea}` (186-191) | `m_reset_bpx` —— ❗D32 的显式初始化必须留在构造函数/`begin`（否则「武装后首次访问必定漏掉」复发） |
| `reset_hwbpx` (196) | `m_reset_hw_bp` |
| `first_create_process` (184) | `m_first_create_process` |
| `DebugActiveProcessStop_` (200-201，循环内 `GetProcAddress`) | `m_debug_active_process_stop`，在 `begin` 里解析；取不到 → D47 降级成 kill |
| `cont()` / `term()` / `to_detach()` | 已是成员（`core.hpp:58-60`），不动 |

`on_wait_timeout` 的语义不变：pump 的空闲路径照旧问宿主，那正是执行期唯一的 control poll 点（`exec_guard.cpp:133` → `stream_poll_and_dispatch`）。

### 2.2 「人工不一定好用，但不能没有」的数据面补口

这一批按「缺了就只能靠猜」来选，不按好不好用：

| # | 原语 | 为什么不能少 | 现在的状态 |
| --- | --- | --- | --- |
| 1 | `list_modules()` / `ea_owner(ea)` | AI 与 IDA 对齐的键只有 `(modname, rva)`；运行时 base、size、entry、region 类型是判断「这个 EA 属于哪个副本」的唯一依据 | `peinfos()` 是内部链表，Lua 侧只有 `GetModBase(name)` |
| 2 | `list_threads()` + `tid → handle` | 栈回溯 / `GetContext` 都要 tid；现在只有内部链表 | `types.hpp:44-51`，无 Lua 面 |
| 3 | `list_breakpoints()` / `list_hw_breakpoints()` | 客户端不知道「我 armed 了什么、哪条是 single、原始字节是什么」就无法推理 | 只有 set/del/enable/disable |
| 4 | **命中计数与命中记录**（per bp：hits、pid/tid、时间） | AI 问「命中了几次 / 被哪个线程命中」；没有它只能靠 `print` 散文 | `BreakpointInfo` 无计数字段（56-66） |
| 5 | **完整 exception record**（first chance、param count、params） | 「吞还是交给程序」必须知道 first chance；现在只有 `code` + `addr` | `types.hpp:137-141` |
| 6 | `snapshot(region)` / `restore(snap)` | AI 打补丁必须能回滚；`update_memory` 有 old data 但没有回滚 | 无 |
| 7 | **写入日志**（addr / old / new / 时刻）+ `dry_run` | 远端代码执行需要「先看会改成什么，再决定改不改」；且 AI 的输出要可审计（与 `images` 同级，进信封 `patches[]`） | `images` 只登记落盘路径 |
| 8 | `scan(pattern, mask)` 带通配符 + `scan_string(charset)` | AI 找字符串与代码模式；现在的 `FindMemory` 只有定长 pattern 的 BM；GBK/UTF-8 口径必须显式 | `types.hpp` 的 `FindMemoryContext` |
| 9 | `pump` 的 **record-and-continue** 模式 + `hits()` 有界队列 + `dropped` 计数 | AI 慢 ⇒ 不能停在断点上等 AI；「记录并放行」这一档必须有，且必须诚实报告丢了多少 | 无 |
| 10 | `cont(pid, tid, code)` 的显式 code（swallow / pass） | 反反调试与异常策略的最小件 | 现在 code 是循环内部算出来的 |
| 11 | `step(tid)`（置 TF → 下一个 `SINGLE_STEP`） | 单步是「停下来逐条」的另一半；不需要新 core 代码，绑定层用 `SetContext` + `cont` 即可，但 re-arm 逻辑（409-418）必须在 pump 边界仍正确 | 无独立原语 |
| 12 | **真实读取长度**（partial read 的字节数） | `ReadMemory` 现在吞掉部分读；AI 拿脏数据会全错 | `core::read` 返回 bool |
| 13 | `alloc` / `free` in target（注入桩、scratch） | 需要时不能没有；但 `VirtualAllocEx` 是 NT-only（9x 没有）→ dynapi + **能力上报**（照 D47 的「能力上报」写法，不是「许可」） | 无 |
| 14 | `update_all_threads_context(mutate)` 的 Lua 面 | 已是 core 成员（`core.hpp:90`），但参数是 C 函数指针，Lua 用不了 | 无 Lua 面 |
| 15 | **一切低层原语都必须有有界变体**（timeout / cap） | 取消模型（D32/S13）依赖有界；不可有界的调用 = 打断不了 = 门禁测不了 | 设计规则，不是某个 API |

### 2.3 明确不提供（写进注释，避免以后又被加回来）

- `SuspendThread` / `ResumeThread`：调试器里挂起线程会死锁；事件处理期间系统本来就挂起全部线程，pump 的 pause 模型已经够用。
- 反汇编 / 解码：静态语义归 IDA MCP（xref、decompiler、类型）。AutoDbg 只提供运行时真值 + `(modname, rva)` 这个键。自写 decoder 是重复实现且会误导 AI。
- 跨调用的持久状态（D43）：AI 的记忆在 agent 侧和 IDB 里，不在 AutoDbg 里；`state.cpp` 不写文件这条不变。
- REPL 里 `yield`：LuaJIT 的 `pcall` 不能跨 yield（PITFALLS L6）。
- 回调里再 `pump`（重入）：明确拒绝，并有断言。

---

## 3. `Run` / `Attach` 能不能用 Lua 写

**结论：策略可以搬进 Lua，契约名不搬。**

成立的事实：`create_state_with_libs()` 是「注册 API → 再烘焙」（`state.cpp:295` → `297`），所以 lualib 文件在顶层**能**看到 C 注册的表（`dbg`）。L4 那条坑只针对 lualib 之间的顺序，不是这条的障碍。

不搬的三个理由：

1. **按文件名设全局**（§2 / PITFALLS L4）：`run.lua` 只能设全局 `run`；要设 `Run` 就得在文件里显式赋值，等于把「按文件名」这个约定打烂——而整个烘焙机制就是按文件名跑的。
2. **逐字节口径没有安全网**：`Run`/`Attach` 是 29 契约名，改绑定层必须逐字节验证输出。现在 `tests/golden/` 只有 w9x 探针（`probe/bom/w3_*`），没有 `Run` 的黄金输出 ⇒ 搬之前必须先补一份「同一脚本在 C 实现与 Lua 实现下逐字节对照」的门禁，否则 §5 第 6 条无法执行。
3. **取消与清理路径会变**：`on_engine_error` 的旧行为是 `lua_pushstring + lua_error`（长跳、跳过清理），迁移时已改成「走完清理 + 返回错误码由绑定层 `lua_error`」（`types.hpp:262-266`）。如果 `Run` 是 Lua 循环，`lua_sethook` 的取消会长跳跳出 Lua 循环，此时 engine 停在「未 continue 的事件」上 —— 必须由 `pcall` + `__gc` 兜底才能干净 detach。这是新增风险，不是零风险。

建议的落法：

- C 侧：`dbg.begin/pump/cont/end`（状态机原语，不可少）。
- Lua 侧：一份**新名字**的 sugar（例如 `lualib/dbglib.lua` → 全局 `dbglib`，按文件名，符合 §5 第 6 条），它只跑在 `dbg` 原语上：`until`、`on_hit`、`record`、条件断点、超时。人和 AI 的便捷面是这层；被需要时不能没有的最小件是 C 原语。
- 如果将来一定要把 `Run` 搬进 Lua，先决条件是 §3 第 2 条的黄金门禁先存在。

---

## 4. 要拍板的语义（推荐值先写在这）

1. **pump 默认不 continue**（T7 的全部意义）；`pump` 返回事件码，`0` = 无事件，并把「本来该用的 code」存进会话 ⇒ `cont()` 无参时用它，保证 `run()` 的 1:1。
2. **两种 pump**：`autocont = false`（capture，停在事件处）与 `autocont = true`（record-and-continue，给 AI 用）。`hits()` 队列默认上限照 `--server-queue` 的 8？建议 64，且必须带 `dropped` 计数。
3. **REPL 会话 = 一次执行**（D43 不破）：一次执行里读命令、派发、回 `result`；跨执行仍无状态。这条要写进 `AGENTS.md` §12。
4. **REPL 的输入不能阻塞**：stdin 读取必须切片（≤250 ms）+ 每片查 guard，否则 S13 复发。核心不变：core 只拿 timeout，不知道 stdin 与 socket。
5. **REPL 事件停住后默认不放行**（用户要的是停住能看）；REPL 空闲时 `pump_now()` + 控制帧 poll。
6. **错误一律当值返回，不长跳**（`types.hpp` 的既定口径）；重入（回调里 pump、paused 时调 `Run`/`Attach`）固定文案报错。
7. **退出码口径不变**：0 / -1 / Lua 的 3、2；REPL 的码 = 最后一条命令的码；EOF → 0。`--repl` 与 `--json`/`--out` 互斥（那一行独占 stdout）→ launcher 报错退 2；`--repl` 与 `--server` 不互斥。
8. **ABI 单跳**：`boot` 加 `repl_mode`（+ 可选 `repl_poll_ms`）→ ABI 4 → 5。
9. **REPL 的 stdin 读取落点**：先 `ReadFile(GetStdHandle(STD_INPUT_HANDLE))`（管道可跑，门禁靠它），失败回退 `ReadConsoleW` —— `ReadConsoleW` 的导入表要先用 `tools/api_check/main.py ReadConsoleW` 查（`ReadConsoleInput` 已在用，95 可）。
10. **能力上报而不是许可**：`alloc in target`、`step`、`hits()` 这类「NT-only / 可能失败」的原语，一律照 D47 的写法——返回值带 reason，客户端能知道实际降级了什么。

---

## 5. 协议（REPL 需要的最小加法）

现有帧表（`stream.hpp:42-66`）里 `u16 id` 是**句柄号**，不是相关性 id —— 前提是执行串行 ⇒ 同一句柄最多一个在途请求。REPL 会打破这个前提：

- 命令 A 的 `out` / `result` 与「事件在 A 之后、A 的 result 之前由回调产生」的 `out` 会串味：客户端无法归因。
- `record-and-continue` 模式下命中是**异步**的，没有「属于哪条命令」的归属。

建议（这是 T7 唯一真正的协议变更）：

| 项 | 建议 |
| --- | --- |
| 新帧类型 | `event = 16`：json = 命中记录（`{type, pid, tid, ea, modname, rva, code, first_chance, dropped, seq}`），`id` = 产生它的命令号（0 = 无命令） |
| `out` 的 `id` | 改成「产生它的命令号」；0 = 无命令（无归属的输出仍可发，客户端知道是 engine 自己打的） |
| `result` 的 `id` | ❗保持 = 退出码（u16 二补数）——这条已经定死并有门禁；退出码不能挪进 json，否则 `client.cpp` 与门禁都改 |
| 命令号来源 | `script` 帧的 `id` = 客户端递增的命令号（现在 `script` 的 id 未被使用 ⇒ 正好腾出来） |
| 版本 | 因为客户端必须实现归因 ⇒ **proto 1 → 2**（握手帧是唯一协商手段，没有别的办法）。要改的断言：`proto.info-fields`（`proto`）、`handshake.bad-version-values`（`want`）、nvim 的 `health.proto` |
| 如果不想 bump | 唯一不 bump 的方案是「REPL 里禁止回调 print + 只做同步命令」——不可接受，所以写清楚：bump 是代价，不是选择 |

---

## 6. 门禁

**fixture（T7 与 Win9x case 34 的共同阻塞项）**：建议新增 `tests/fixtures/bin/x86_loop.c`（一个会命中断点的 x86 小程序，用项目自己的 `cc` + `ld`，零 CRT），ninja 加一条 `fixture` 边，产物落 `.out/fixtures/`。❗`scan_dir.py` 扫 `.out` 下全部产物并自动选口径 ⇒ fixture 必须满足 w95 口径，或在 `scan_dir.py` 里加排除规则（这条要拍板）。

新断言（`e2e_repl.ps1` 用管道喂 stdin；`e2e_tcp.ps1` 加 `repl.*` 段）：

```text
pump.no-auto-continue        事件停住后 Eip 未推进（GetContext 对照）
pump.default-code-parity     pump 存下的 code 与旧 run() 用的码一致（BREAKPOINT → DBG_CONTINUE，其余 → NOT_HANDLED）
pump.step                    置 TF 后 cont() → SINGLE_STEP，且断点被 re-arm（D32 那条）
pump.reentry-rejected        回调里 pump / paused 时调 Run → 固定错误文案
pump.record-and-continue     autocont 下目标继续跑 + hits() 拿到记录 + dropped 计数正确
pump.cancel-while-idle       pump 空闲时 control cancel 生效 + ack（S13 的 guard 路径）
pump.partial-read-length     跨页读取时真实长度 < 请求长度（现在被吞）
repl.line-eval / repl.stdin-piped / repl.exit-code-eof / repl.json-exclusive
repl.snapshot-restore        写 → snapshot → restore → 内存回到原值
repl.event-attribution       event 帧的 id = 发出命令的号；无归属输出 id = 0
repl.state-member-parity     run() 重构后既有门禁全绿（逐字节验证输出的那条）
```

要跟着改的数字：ABI 4 → 5（`e2e_tcp.ps1` 的 `client.version-abi`、`nvim_smoke.lua` 的 `version.abi`）；proto 1 → 2（上面 §5 列的三条）。顺带把 `plan.md` §1.1 第 1 条欠着的 `decide_console_action` 断言补上（3 条：SHIFT+Q → 2、非 Shift → 1、无记录 → 0），它正是 REPL 输入判定的那一半，同一个提交最合理 → selftest 67 → 70。

---

## 7. AI + IDA MCP：AI 会怎么用，缺什么

典型回路（AI 侧实际会跑的形状）：

1. IDA MCP 给候选函数 / RVA / xref / decompile（静态假设）。
2. AutoDbg 跑起目标 → `list_modules()` 拿运行时 base、size、entry、文件路径。
3. 用 `(modname, rva)` 把 IDA 的 RVA 映射成运行时 EA（`Ea2ModnameRva` 是反向，`GetModBase(name)` + `FindFuncRva` 是正向）——这是两个系统之间**唯一**的键。
4. `SetBreakpoint(modname, rva)` → `pump`（或 record-and-continue）→ 命中。
5. 取运行时真值：context、栈回溯、关键内存、字符串、exception record。
6. 与第 1 步的假设对照：不一致 → 判断是 ASLR/副本/自修改代码（靠 `ea_owner` + region 类型 + `snapshot`）。
7. 补丁或观察行为 → `restore` 回滚 → 把发现写回 IDA（rename/comment 由 agent 调 IDA MCP，AutoDbg 不参与）。

由「AI 的节奏」推出的缺口（这些是真正的缺，不是锦上添花）：

| 缺口 | 为什么 AI 特别需要 |
| --- | --- |
| `hits()` 队列 + `dropped` 计数 | AI 慢几个数量级 ⇒ 不能停在断点上等 AI；必须诚实报告「你看的是一个窗口，不是全部事实」 |
| 命令归因（§5 的 `event` 帧 + `out.id`） | 一次调用一个信封，信封必须能对应到它自己产生的输出；否则 agent 会把事件的输出当成命令的输出 |
| 稳定的记录形状（字段名写死） | `result` 是 Lua 值 ⇒ 形状不稳定的话每个 agent 都要重新猜字段（S12 的复发）。落点：skill 包 `references/lua-api.md` + `check_skill` 的门禁 |
| 可证伪的最小件：模块身份（base/size/checksum/timestamp）、region 类型、真实读取长度 | AI 会瞎猜「这个 EA 属于哪个副本」；没有这三样就无法证伪 |
| `dry_run` + `patches[]` 写入日志 + `snapshot`/`restore` | agent 会写坏目标；必须能「先看会改成什么」和「回滚」 |
| `list_breakpoints` / `list_threads` / 命中计数 | agent 会忘了自己 arm 了什么（它没有跨调用记忆，D43） |
| 机器可读的 API schema（`--schema`：API + 载荷形状 + 哪些是契约名） | 现在 markdown 是唯一的契约文档，但没有可被程序消费的机器形状；`check_skill` 只能做语法检查 |
| 字符串 / 通配符扫描（GBK/UTF-8 显式口径） | AI 的「找线索」90% 是找字符串与模式；定长 BM 不够 |

不需要 AutoDbg 做的事（避免和 IDA MCP 重复）：反汇编、xref、类型、命名、IDB 写入。这些归 IDA MCP；AutoDbg 只负责运行时真值 + 那个键。

还有一条要写进 `AGENTS.md` §12 的口径：**跨调用唯一可存活的状态是被调试进程本身**（D48）。所以 agent 的「记忆」只能有三处：它自己的上下文、IDA 的 IDB、以及脚本头标记。AutoDbg 不提供第四处。

---

## 8. 落地顺序与工作量

| 切片 | 内容 | 量 | 门禁 |
| --- | --- | --- | --- |
| **T7.1 纯重构** | 循环态 → 会话成员；`run()` = `begin`+`pump`+`cont`+`end` 封装；可观察行为不变 | `debug_loop.cpp` +80 行、`core.hpp` +6 声明 | 既有门禁全绿 + `decide_console_action` 3 条 |
| **T7.2 fixture** | `tests/fixtures/bin/x86_loop.c` + `fixture` ninja 边 + `scan_dir.py` 口径 | 约 60 行 | fixture 过导入表 |
| **T7.3 `dbg` 表** | `src/scripting/lua/dbg_bindings.cpp`（新文件、新全局表）：`begin/pump/cont/end/step/hits/list_*` | 约 150 行 | `pump.*` 七条 |
| **T7.4 数据面补口** | §2.2 的 1/2/3/4/5/6/7/12（其余按需要补） | 约 200 行 | `repl.snapshot-restore`、`pump.partial-read-length`、`repl.writes-log` |
| **T7.5 协议 + `--repl`** | `event` 帧 + `out.id` 归因 + proto 2；launcher `--repl` → ABI 5；模块侧 REPL loop（ReadFile + 切片 + guard） | `stream.*` 约 +40 行、launcher 约 40 行、模块约 120 行 | `repl.*` 五条 + 三处断言数字更新 |
| **T7.6 Lua sugar** | `lualib/` 里新名字（例如 `dbglib`），只跑在 `dbg` 上；契约名不动 | 约 100 行 | `check_skill`（新 markdown 的 lua 片段）+ 一条 sugar 对照断言 |
| **T7.7 文档与 nvim** | `AGENTS.md` §7 架构行 + §12 口径、skill 包 `references/lua-api.md` 加 `dbg`、新 recipe `stop-and-inspect.md`、PITFALLS 新增 S14（pump 不可从回调重入 / 归因靠命令号）；nvim 的 `:AutoDbgRepl`（独立仓单独提交） | — | `check_skill` 14 → 会随新 markdown 增加 |

顺序不能调的地方：T7.1 必须单独一个提交（纯重构 + 既有门禁全绿）；T7.3 依赖 T7.1；T7.5 的协议变更必须与客户端（nvim/独立仓）同步，否则归因会在两边分叉（PITFALLS 里已有同类教训）。

---

## 9. 待确认

1. `pump` 的默认 `autocont`：false（capture）还是 true（record）？我推荐 false，因为「停下来看」是 T7 的意义，但 REPL 里可以按模式翻。
2. `hits()` 队列上限与 drop 策略（丢最旧 / 丢最新 / 计数后停）—— 我推荐 64 + 丢最旧 + `dropped` 计数，因为诚实性优先。
3. proto 到底 bump 不 bump（§5）：如果 AI 客户端只有 nvim 一个，可以先只在 T7 里 bump；如果第三方 agent 已经在连，必须 bump。
4. fixture 落在 `scan_dir.py` 的哪个口径（w95 还是排除）——这条决定了 fixture 能不能用 NT-only 的调用。
5. `--repl` 的 stdin 在真控制台上走 `ReadConsoleW` 还是 `ReadConsoleInput`（后者已在用且 95 可，前者要先查导入表）。
6. Lua sugar 的落点：`lualib/`（按文件名设全局，不能设 `Run`）还是 `dbg` 表的方法（C 注册）——我推荐 sugar 挂 `dbg` 表，lualib 只放纯 Lua 的策略函数。
