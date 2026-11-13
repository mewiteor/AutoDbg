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

**落地状态（T7.3）**：`dbg` 表在 `src/scripting/lua/dbg_bindings.cpp`（照 `gui` 的做法，注册一个全局表；29 个契约名不动）。
入口**两个**（不再用 `params.pid != 0` 隐式分派）：`create{...}` = `Run` 的原语版（`CreateProcess`，选项键与
`Run` 同名），`attach(pid [, opts])` = `Attach` 的原语版（`DebugActiveProcess`，**只认显式 pid** ——
「沿用上次 pid / 鼠标选进程」属 D48 解析链，不进原语层；Win95/98 没附加能力 ⇒ `nil, err`）。
方法共 15 个：`create / attach / pump / cont / close / step / eip / arm / hits / dropped / setcap / code /
stopped / paused`（`end` 改叫 `close` —— `end` 是 Lua 关键字，`dbg.end` 写不出来）。回调参数统一是普通表
`{pid,tid,ea,code,modname,rva}`（新接口不再用 LuaVariant）。`hits()` 是 64 格环形队列 + `dropped` 计数
+ `setcap(1..64)`；记录只在 `on_breakpoint / on_exception / on_user_exception / on_single_step` 四个通道产生。

`close()` 的**归属口径**：附加来的目标（`attach`）只分离不杀 —— `to_detach()` 为真时 core 的 `end` 做
真 detach（恢复原字节 + `DebugActiveProcessStop`），目标继续活着、pid 不变；自己 `create` 的目标仍随
`close` 终止（= `Run` 的老口径）。返回值：0 = 正常；**1 = 要 detach 但系统没 `DebugActiveProcessStop`
（Win95/98）⇒ 已降级成 kill**（D47 的能力上报）。`run()` 的旧路径（`to_detach` 恒为假）行为一字不变。

> ❗**下一步（设计已拍板，见 §11.1 / D51）**：`close` 将被拆成 `d:detach()` / `d:kill()`，且 **9x 上
> `dbg.detach` 根本不存在**（能力缺席而不是降级）。上一段描述的「`close` + 返回 1 降级」是 T7.3 的
> 临时口径，它去掉前不要往新文档/示例里引用。

同轮改的 core：`pump` 的等待按 **≤250 ms 切片**，每片没等到事件就问一次 `on_wait_timeout`
（`run()` 传 100 ms ⇒ 正好一片 ⇒ 行为逐字节不变）；`m_last_code` + `last_code()` 让绑定层拿到
「本来该用的 code」（`dbg.cont()` 无参时用它）；两条固定文案 `"pump cannot be called from a callback"`
与 `"cannot run while an event is unhandled"` 定在 `dbg_bindings.hpp`，`bindings.cpp` 的
`lua_run` / `lua_attach` 在 `dbg` 停着未应答事件时报后者。

同期把 T7.1 记录 ① 结清：`decide_console_action` 从 `scripting/lua/console_input.*` 搬进
`common/console_action.*`（纯函数、只带 `windows.h`），`common → scripting` 的反向依赖消失；
自测仍是 70 条。

**T7.3 实测口径（数字已钉死，改门禁前先看这里）**：
- 「显式放行」路线：`dbg.cont(nil, nil, 0x00010002)`（= DBG_CONTINUE）逐次放行 ⇒ 9 条记录
  （ntdll 初始 int3 1 条 + site 的 int3 8 条）、窗口 2 ⇒ `dropped = 7`、目标**干净退出 42**。
- `autocont` 路线：fixture 的 int3 不是 core 管的软件断点 ⇒ core 算出来的是 NOT_HANDLED，目标在
  第二次机会后终止；同一次 NOT_HANDLED 会在同一 EA 上**报两遍**（first / second chance），
  所以那一段的固定数字是 `hits = 3`（不是 2）。要区分 first/second chance 得等 T7.4（§2.2 第 5 条）。
- ❗**别把 `dbg.arm` 打在 fixture 的 int3 上**：那条指令本身就是 `CC`，core 命中后会把它当
  「原始字节」还原、退回 EIP、置 TF 单步 —— 单步执行的又是 `CC` ⇒ 再撞一次断点，实测只到
  3 条记录就被 NOT_HANDLED 终止。`arm` 适用于**普通指令地址**，不是已经写着 `int 3` 的地址。
- ❗门禁脚本层面两个坑：① PowerShell 双引号 here-string 里 `$site.rva` **不是**属性访问（要
  `$($site.rva)`），写错时症状是「脚本一句输出都没有」（整段 Lua 变成 `System.Collections.Hashtable + 5`）；
  ② 服务端把空闲 2 s 的控制连接关掉（`tcp_server.cpp` 的 `k_conn_idle_ms = 2000`）⇒ 中间跑过几轮
  `Run-Client` 之后必须**新开**连接，否则第一次写就是 `WSAECONNABORTED`。

**落地状态（T7.1 = `8e35348` + `ca3bde1`）**：本机全部门禁跑过且绿（导入表 12 产物、json 32、gui 21、tcp 48、skill 14、nvim_smoke 32、w9x gate），除自测 `67` → `70` 外断言数未变。两点记录：
① `src/common/selftest.cpp` 为此 include 了 `src/scripting/lua/console_input.hpp`（那个头只带 `windows.h`，§5 第 5 条没被破坏），但依赖方向是 `common → scripting`，与分层相反；理顺只需把纯函数 `decide_console_action` 挪进 `src/common/`（它本来不做 I/O、不碰 Lua），留待 T7.3 一起定。
② `pump` 的返回值只表达事件码（`0` = 没等到事件），「该不该停」仍靠会话标志，所以绑定层要用无参的 `cont()` 判断循环是否已终止 —— 「没等到事件」与「已 detach 退出」在返回值上不可区分；`cont()`（无参标志）与 `cont(pid, tid, code)`（显式应答）是同名重载，保留不改，但绑定层容易误用。

### 2.2 「人工不一定好用，但不能没有」的数据面补口

这一批按「缺了就只能靠猜」来选，不按好不好用：

| # | 原语 | 为什么不能少 | 现在的状态 |
| --- | --- | --- | --- |
| 1 | `list_modules()`（带 `machine` 与 pid） | AI 与 IDA 对齐的键只有 `(modname, rva)`；运行时 base、size、entry、region 类型是判断「这个 EA 属于哪个副本」的唯一依据 | `peinfos()` 是内部链表，Lua 侧只有 `GetModBase(name)`。❗`ea_owner(ea)` 不需要单独实现——`Ea2ModnameRva`（契约名，返回形状不可动）已经覆盖它；缺的是**反向**：同名多实例时 `GetModBase(name)` 只取第一个 |
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

**fixture（T7 与 Win9x case 34 的共同阻塞项）**：建议新增 `tests/fixtures/bin/x86_loop.c`（一个会命中断点的 x86 小程序，用项目自己的 `cc` + `ld`，零 CRT；x86 产物照 D12 走 `pever` 打 4.0），ninja 加一条 `fixture` 边，产物落 `.out/fixtures/`。❗`scan_dir.py` 扫 `.out` 下全部产物（含子目录，否则 fixture 不被计数）并自动选口径 ⇒ 已拍板：fixture 走 **w95**（不排除，见 §9 第 4 条）⇒ fixture 源码里禁止 NT-only 调用。

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
| **T7.3 `dbg` 表** | `src/scripting/lua/dbg_bindings.cpp`（新文件、新全局表）：`create`/`attach`/`pump`/`cont`/`close`/`step`/`eip`/`arm`/`hits`/`dropped`/`setcap`/`code`/`stopped`/`paused` + `pump` 的 250 ms 切片 + `m_last_code`/`last_code()` + `end()` 的真 detach（D47 能力上报）+ `decide_console_action` 搬进 `common/` | 约 750 行（含门禁与注释） | `pump.*` 八条（tcp 48 → 56，已在 86 modern/legacy 上真过；x64 整段 SKIP） |
| **T7.4 数据面补口** | §2.2 的 1/2/3/4/5/6/7/12（其余按需要补） | 约 200 行 | `repl.snapshot-restore`、`pump.partial-read-length`、`repl.writes-log` |
| **T7.5 协议 + `--repl`** | `event` 帧 + `out.id` 归因 + proto 2；launcher `--repl` → ABI 5；模块侧 REPL loop（ReadFile + 切片 + guard） | `stream.*` 约 +40 行、launcher 约 40 行、模块约 120 行 | `repl.*` 五条 + 三处断言数字更新 |
| **T7.6 Lua sugar** | `lualib/` 里新名字（例如 `dbglib`），只跑在 `dbg` 上；契约名不动 | 约 100 行 | `check_skill`（新 markdown 的 lua 片段）+ 一条 sugar 对照断言 |
| **T7.7 文档与 nvim** | `AGENTS.md` §7 架构行 + §12 口径、skill 包 `references/lua-api.md` 加 `dbg`、新 recipe `stop-and-inspect.md`、PITFALLS 新增 S14（pump 不可从回调重入 / 归因靠命令号）；nvim 的 `:AutoDbgRepl`（独立仓单独提交） | — | `check_skill` 14 → 会随新 markdown 增加 |

顺序不能调的地方：T7.1 必须单独一个提交（纯重构 + 既有门禁全绿）；T7.3 依赖 T7.1；T7.5 的协议变更必须与客户端（nvim/独立仓）同步，否则归因会在两边分叉（PITFALLS 里已有同类教训）。

---

## 9. 待确认

> 本节只剩「还没拍」的条目；**已拍板的在 §11（D51–D55）与 `AGENTS.md` §13**，不要在这里重复。
> 第 6、7 条（session 上限/回收、`default-parity`）的形状由 §11.5 / §11.7 定，具体数值仍待拍。

1. `pump` 的默认 `autocont`：false（capture）还是 true（record）？我推荐 false，因为「停下来看」是 T7 的意义，但 REPL 里可以按模式翻。
2. `hits()` 队列上限与 drop 策略（丢最旧 / 丢最新 / 计数后停）—— 我推荐 64 + 丢最旧 + `dropped` 计数，因为诚实性优先。
3. proto 到底 bump 不 bump（§5）：如果 AI 客户端只有 nvim 一个，可以先只在 T7 里 bump；如果第三方 agent 已经在连，必须 bump。
4. ~~fixture 落在 `scan_dir.py` 的哪个口径（w95 还是排除）~~ —— **已拍板：w95（不排除）**。后果：fixture 源码里禁止任何 XP+ 才有的调用，实际导入共三条、全部由 `src/sys/main.c`（零 CRT 的 CRT 兼容层）引入而非由 fixture 自己产生 —— `msvcrt!_ftol`、`msvcrt!__getmainargs`、`kernel32!ExitProcess`（三条都在 `datas.bin` 的 w95 段里，前两条 `msvcrt.def` 已显式导出）。实测：`uv --directory tools/api_check run main.py .out/fixtures/x86_loop.exe w95` 无输出 = 通过。`scan_dir.py` 对 `.out/fixtures/` 下的产物自动选 w95（既有「Legacy / 启动器 → w95、其余 x86 → wxp、x64 → w7」判定不变），并且必须递归扫子目录（门禁口径 12 → 13）。fixture 的固定量是契约（T7.3 的 `pump.*` 断言直接依赖）：8 轮循环、命中 8 次、退出码 42、不打印不落盘；断点位置靠扫代码里 6 字节模式 `B8 44 42 47 50 CC`（`mov eax, 0x50474244` —— 立即数按小端存成 `44 42 47 50` —— 紧跟 `int 3`；`bp_site` 用 `#pragma optimize("", off)` 关掉优化，因为 `/O1` 下把 static 函数内联进循环会把模式复制多次，唯一性就破了），该模式在整个 exe 里必须只出现一次，命中点 = 模式命中地址 + 5（所以匹配整条指令编码，不是只匹配立即数）。
5. ~~`ReadConsoleW` 要先查导入表~~ —— 已查（`tools/api_check/datas.bin` 按 OS 段）：`ReadConsoleW` 全 w95 可 ⇒ 不需要 dynapi。剩下的只是「先 `ReadFile`（管道可跑，门禁靠它）再回退 `ReadConsoleW`」这个顺序。
6. session 上限与 idle 回收的数值（`hits()` 队列上限照 64 已推荐，但「几个 session」「空闲多久回收」还没数）。
7. `session.default-parity` 这条门禁的落点：契约名走默认 session、`sess:xxx` 走 session 对象，两者必须逐字节一致（§10 第 6 条）。
8. **T7.2 已落地，fixture 的契约数字由 T7.3 钉死**：无武装断点时 `on_exception` 只触发 3 次、
   `on_exit_process` 报的是 `0x4000001F`（不是 42）；8 次命中与退出码 42 只有在每次 `int 3` 都被
   **显式** `DBG_CONTINUE` 之后才成立（`m_first_bp` 只放行第一个）。T7.3 的门禁现在就是这样写的：
   `dbg.cont(nil, nil, 0x00010002)` 逐次放行 ⇒ 9 条记录（窗口 2 ⇒ dropped 7）、`exit=42`。
   单跑 fixture（无调试器）在第一个 `int 3` 就终止，退出码 `0x80000003` ⇒ 代码路径本身没问题。
9. `SetBreakpoint` 的 `rva` 只接受 LuaVariant（数字与普通字符串都报 "LuaVariant expected"），且 LuaVariant 算术有坑：实测 `GetModBase('x86_loop.exe') + 0x1028` 传进来的 rva 变成 `0x28`（`VirtualProtectEx` 报 487）。
   主模块在 `GetModBase` 里的名字带扩展名（`x86_loop.exe`），且它返回的是 LuaVariant 而**不是**普通数字
   （`tostring()` 才得到 `0x00550000` 那种串），所以门禁里不要拿它做算术。
   T7.3 的 `dbg.arm(modname, rva, cb)` 走**普通数字**并先判 `get_base`：模块没加载时直接返回
   `nil, "dbg.arm: module not loaded (pump to its load event first)"`，而不是留给 core 在命中时报
   "tmp->has_orignal_byte is zero!"。
10. 多 session 的 pump 口径：「事件属于别的 session」时是立刻 dispatch 还是入 queue（§10 第 5 条建议入 queue + 返回归属）。
11. Lua sugar 的落点：`lualib/`（按文件名设全局，不能设 `Run`）还是 `dbg` 表的方法（C 注册）——我推荐 sugar 挂 `dbg` 表，lualib 只放纯 Lua 的策略函数。转义符口径（已讨论）：文件名去扩展名后，`<转义符><字母>` → 大写，其余原样；转义符选 `-`（`_` 不行，它是标识符字符，会跟「按文件名」约定打架）。只转 `name` 不转 `path`（`path` 是 chunkname，`nvim_smoke` 的 `err.error-text` 靠它）；`gen_lua_lib.lua` 要在构建期校验「结果是合法标识符」。
12. **`attach` 的两条实测口径**（T7.3 / `pump.attach-*`）：
   · 位数必须配对：**32 位调试器附 64 位目标**时 `DebugActiveProcess` 返回 ERROR_NOT_SUPPORTED（50），
     不是权限错也不是「目标已退出」——门禁里必须起一个 32 位靶子（`SysWOW64\cmd.exe`，
     32 位主机上才能用 `system32\cmd.exe`）。
   · `attach` + `close()` 必须**只分离**：自己 `create` 的目标随 close 终止（= `Run` 老口径），
     附加来的目标 `to_detach()` → 恢复原字节 + `DebugActiveProcessStop` 后继续活着（pid 不变，可再 Attach）。
     降级路径（9x 无 `DebugActiveProcessStop`）由 `dbg.close()` 返回值 **1** 上报。
   · 门禁不能复用「跑过几轮客户端」的那条共享连接（空闲 2 s 被服务端关，见 §2.1 实测口径），
     且断言「目标还活着」要在 `Run-Client` 返回后 `$tgt.Refresh()` 再读 `HasExited`。

---

## 10. 多 session（进程树路由）

这是 §2.2 的自然延伸，也是 A/B 对比用例（同一脚本跑两个实例、比较内存）唯一能成立的形式——两个实例同架构，所以不需要任何进程间通信。

1. **session 的键是一棵树**（root pid + 由它派生的 pid 集），不是单个 pid。`debug_loop.cpp:512` 的 `params->pid == event.pid` 只在第一个 `CREATE_PROCESS` 时匹配，本质是「假设只有一个进程」的隐式状态。
2. **事件与模块/线程表已经是全树口径**：每个 `CREATE_PROCESS` 都建 `PEInfo` 节点（节点带 `dwProcessId`，`types.hpp:115`）、调 `update_breakpoints(info)`、调 `scylla_hide_on_create_process(DBGEvent.dwProcessId, ...)`。
3. **但内存与断点补丁只认 root**：`memory.cpp` 的 `read`/`write`/`VirtualProtectEx` 全用 `proc().hProcess`，且 `read(size_t addr, void*, size_t)` 签名里没有 pid；`apply_breakpoints_for_module` → `write()` → `proc().hProcess` ⇒ 断点记录有、补丁打到错的进程。需要按 pid 的句柄表（`OpenProcess` 全 w95；9x 不认 `PROCESS_QUERY_INFORMATION` 的降级已在 `util.cpp:690`）。
4. **断点的进程维度只能落在新记录层**：`BreakpointInfo` 的 key 是 `(modname, rva)`，没有 pid，且布局不可动（§5 第 7 条）。线程句柄反而没问题——`CREATE_THREAD`/`CREATE_PROCESS` 给的 `hThread` 是调试器拥有的有效句柄。
5. **pump 的多 session 口径**：`pump(sess, timeout)` = 先看 sess 的 `hits()` queue；空则读一条事件；事件属于 sess → dispatch；属于别的 session → 入它自己的 queue 并返回归属（❗不在名义上 pump A 的时候跑 B 的 callback）。被忽略的 session 会停在事件未应答状态（`cont()` 的 `DBG_EVENT_UNHANDLED` 就是为此存在）⇒ 脚本必须显式 drain 每个 session，不能靠引擎自动继续。
6. **契约名继续作用于默认 session**（29 个名字一个字不改），多 session 走 session 对象的方法（新名字，§5 第 6 条允许）⇒ 门禁必须有一条逐字节对照：`session.default-parity`。
7. **一棵树只能有一个调试器**（子进程自动归同一个）⇒ session 不能跨进程；要拆必须显式 `DebugActiveProcessStop`（NT+；9x 没有，D47 降级）⇒ 分派在 `Run`/`Attach` 时定，不可迁移。9x 上还有另一条硬后果：不能 attach 已运行进程（case 30）⇒ 98 上 session 只能由 CreateProcess 建立，session 生命周期 = 执行生命周期，不能跨调用恢复。
8. 回调里不能 pump（§2.3）⇒ 跨 session 比较只能在两边都停住时做；跑着的 session 只能读内存，不能读 context（`SuspendThread` 明确不提供）。

新断言（`e2e_tcp.ps1`）：

```text
route.child-process            被调试进程创建的子进程 → 事件归属同一 session
route.child-handle             读子进程必须用该进程的句柄，不是 root 的
route.same-name-two-processes  list_modules() 能区分同名多实例
route.other-session-queued     B 的事件入 queue；A 仍能读内存；B 停在未应答状态
pump.reentry-rejected          回调里 pump（已有）
session.tree-key               pid-set 归属
session.one-exec-connection    一执行连接一 session（其它连接只能走 control）
session.idle-reclaim / session.cap
session.default-parity         契约名与 sess: 方法逐字节一致
```
---

## 11. 对象化 API、命名、Help 与 session 生命周期（设计已拍板，D51–D55）

> 本节是 **T7.3 之后的方向**：`dbg` 从「一个全局表 + 一堆函数」变成「会话对象 + 普通事件表」，
> 并顺带定下命名、帮助、能力探测与 session 归属。代码落地在 §11.7 的切片 A→D 里；
> 已定的决策进 `AGENTS.md` §13（D51–D55）。

### 11.1 `close` 拆成 `detach` / `kill`（D51）

现状（T7.3 已提交的 `dbg.close()`）是「按目标来源决定杀不杀」，并返回 `1` 表示「要 detach 但系统
没有 `DebugActiveProcessStop` ⇒ 已降级成 kill」。三个问题：策略藏在实现里、降级是**静默改变目标
命运**（调用方不查返回值就会杀掉目标）、`close` 同时暗示两件事。

改成：

| 原语 | 语义 | 9x |
| --- | --- | --- |
| `d:detach()` / `dbg.detach(pid)` | 恢复断点原字节 → `DebugActiveProcessStop` → 关句柄；目标继续活着、pid 不变 | **key 不存在**（`dbg.detach == nil`） |
| `d:kill()` / `dbg.kill(pid)` | `TerminateProcess` + 关句柄 | 有（`TerminateProcess` 全 w95） |

- **能力缺席而不是降级**：`DebugActiveProcessStop` 的可用性是**平台属性**，在 `register_lua_api()`
  时用 dynapi 探一次（core 的 `begin` 已经在探同一件事：`debug_loop.cpp` 的
  `m_debug_active_process_stop`），探不到就不装这个 key。这样「`nil` = 这台机器没有」是唯一判据，
  不会出现「存在但一调就失败」的第三种状态。
- **detach 前必须先应答未决事件**：`paused` 为真时先 `ContinueDebugEvent(pid, tid, DBG_CONTINUE)`，
  否则目标停在被系统挂起的线程上（`detach.pending-event` 门禁）。
- 两个动词都做**完整收尾**（`unref` 回调、复位 `started`/`paused`、清 `params`），之后可以再
  `create`/`attach`。
- **CLI 的 `--detach` 保持 D47 的「降级 + `ack.reason` 上报」**：那是**显式上报**的回退（客户端要
  统一接口面）；Lua 原语层是「诚实缺席」。两者的差别要写进 skill，不要当成不一致。
- `close` 直接删（`dbg` 还没进 skill，对外零成本；只有 e2e 与本节引用它）。

### 11.2 命名：新面一律小写，29 个 PascalCase 冻结（D52）

事实：仓库的**新面全是小写**（`gui.*`、`dbg.*`、`open_file`、`_autodbg_info`、烘焙的
`inspect`/`jsonx`/`moses`、`Var` 的方法 `to/tos/sto/write/off`）；PascalCase 只存在于 29 个历史契约名
（`registry.cpp` 的 `lua_register` 列表）。所以：

| 项 | 决定 |
| --- | --- |
| 帮助入口 | **`help()`**（小写），外加 `dbg.help()` 打印 `dbg` 那一节（同一张文本表，不是第二真相源） |
| 29 个契约名 | **冻结**：不改名、不删、不加新参数；新文档/示例一律用对象 API。等 §11.7 的 C/D 落地且 gate 与 recipe 迁移完，再用新 D 条目退役 |
| `V` / `U32` / `I32` / `U64` / `I64` / `F64` | **不出现在新面上**。已被实测咬过两次（§9.9 的 `GetModBase(...) + 0x1028` → `0x28`；`SetBreakpoint` 的 rva 只收 LuaVariant）。新面一律**普通整数 + 字符串**，宽度靠函数名表达（`d:read_u32`） |
| 为什么保留而不是现在删 | `Run`/`Attach` 身上还挂着对象 API 没有的东西：鼠标选进程 + 上次 pid 持久化（D48 链）、控制台 q/Shift+Q 策略、`--json` 信封的 `images`/`result` 收尾。删之前这些必须先在 `dbglib`（T7.6）与 REPL 里落地，否则是功能回退，不是清理 |

### 11.3 Help / caps / skill 的三方分工（D53）

| 问题 | 归属 | 形态 |
| --- | --- | --- |
| 「这个 API 什么意思、前置条件、坑」 | **skill**（`tools/agent/autodbg-skill/references/*.md`） | markdown，`check_skill` 已做门禁 |
| 「**这一台机器/这个 build** 实际有什么」 | **`dbg.caps()`** | 固定形状的普通表：`{ arch, variant, flavor, detach, attach, step, hits_cap, ... }`；按表成员生成，不引入第二真相源 |
| 「我现在该敲什么」 | **`help([topic])`** | 人类散文，打印到输出 sink |

`help` 的输出规则：

- **只有真控制台才上色**：`GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)` 成功 ⇒ 用
  `SetConsoleTextAttribute` + `WriteConsoleW`（9x 控制台支持；**不要**用 ANSI /
  `ENABLE_VIRTUAL_TERMINAL_PROCESSING`，9x 没有）。管道/文件/重定向 ⇒ 纯文本，零属性字节。
- 落点是 core 的 **`print_styled(attr, utf8)`**（core 不碰 Lua；`Print*` 已经自己选
  `WriteConsoleW`/`WriteFile`），不是绑定层自己写句柄。
- 尊重 `--quiet`、`NO_COLOR` 环境变量；**`--json` 下 help 走 stderr 且不上色**（stdout 只许那一行
  JSON，D50）。
- 门禁：把 help 的输出走管道 ⇒ 断言「无 0x1B、无属性字节、前后行数稳定」。

AI 侧的能力探测（写进 skill）：

1. `dbg.detach ~= nil` —— **权威判据**（表成员就是能力）；
2. `inspect(dbg)` —— 给人看（键名一排），**明确标注「给人看，机器别解析」**；
3. `dbg.caps()` —— 一次问全，形状稳定，可门禁。

### 11.4 对象化：Session 对象 + **事件就是普通表**（D55，已按此修订：删掉 Stop 对象）

形态照 `open_file` 的文件对象（D44/D49）——本仓已有先例。但 **`pump` 的返回值不做对象**：
它是一张**普通表**（无 metatable），会话把「当前未应答的那个」缓存在 `d.stop`。

```lua
local d = dbg.create{ imagepath = 'x86_loop.exe' }   -- 或 dbg.attach(pid)
d.root_pid, d.kind('create'|'attach'), d.state       -- stopped | running | exited
d.stop                        -- 当前未应答事件（就是 pump 返回的那个表）；nil = 没有
d:pump([msec])                -- → 事件表；nil = 没等到事件（不是错误）；nil,err = 引擎错
d:cont([code])                -- 缺省 = d.stop.cont_code；可传 dbg.continue / dbg.not_handled
d:step([tid])
d:bp(modname, rva, cb) / d:bp_hw(ea, reg, type, size, cb) / d:bps()
d:read(addr, n) → string / d:read_u8|u16|u32|u64 / d:read_str
d:write(addr, data [, {dry_run=true}]) → {written=, old=}
d:context([tid[, t]]) / d:stack([tid[, depth]]) / d:trace([tid[, ea]])
d:mods() / d:threads() / d:mem_regions()
d:snapshot(addr, n) / d:restore(snap | snap_id)
d:detach() / d:kill()         -- detach 在 9x 上不存在（D51）
```

事件表形状（与 `hits()` 的记录、协议的 `event` 帧 **同一形状**，见下面第 3 条）：

```text
type  pid  tid  ea  modname  rva  code  first_chance  cont_code
```

为什么要删掉上一版草案里的 Stop 对象（四条，按重要性排）：

1. **单会话交互里 pid/tid 没有第二个来源**：pump 之后就是「最近一次事件」，缓存在会话里就够了。
   真要看另一个线程就**显式给 tid**（`d:context(tid)` / `d:stack(tid)`）—— 这本来就是必须有的入口。
2. **代次问题自动消失**：`d:cont()` 应答的永远是「当前那个」，旧表只是数据。想判「还是不是那一个」
   用**表身份**：`pump` 返回的就是存进 `d.stop` 的同一个表 ⇒ `e == d.stop` 就够了，不需要 `seq` 字段、
   也不需要「过期 Stop」的报错路径。
3. **形状统一就好进 JSON**：`hits()` 记录 / `pump()` 返回值 / `event` 帧的 json 同形 ⇒
   `jsonx.encode(d.stop)` 直接可用，不用给 userdata 写序列化（S12 的教训：形状不稳每个 agent 都要重猜）。
   userdata 对象在这里是最贵的东西，而它带来的只是 `s:` 这几个方法。
4. **少一套名字**：`s:cont/context/trace` 全变成会话方法，人/AI 的记忆负担减半。

必须写清的规则（替代上一版的「三条硬规则」）：

1. **`d.root_pid` ≠ `e.pid`**：session 的键是一棵进程树（§10），`root_pid` 是树根，`e.pid` 是事件的
   真实进程。字段名必须分开，否则 AI 会把子进程事件当成根进程。
2. **`e.code`（异常码）与 `e.cont_code`（该回的应答码）是两个字段**：前者告诉 AI「这是什么异常」，
   后者是 core 算好的默认应答（BREAKPOINT → `DBG_CONTINUE`，其余 → `DBG_EXCEPTION_NOT_HANDLED`）。
   有了它 **`dbg.code()` 取消**（T7.3 里那个访问器只是为了补 pump 返回值不携带 code 的缺口）。
3. **`d:pump()` 返回 nil 不清 `d.stop`**（没等到事件 ≠ 事件已被应答）；`d:cont()` 之后 `d.stop = nil`；
   `hits()` 是历史窗口的**副本** ⇒ `hits()[n] ~= d.stop` 即便它们是同一个事件（要「当前」就读 `d.stop`）。
4. **`context()` 的 setter 必须返回值**：无参/带 tid → 表；`d:context(tid, t)` → **部分更新**（只改
   给出的键），返回 `true` / `nil, err`。理由是 `types.hpp` 的既定口径「错误一律当值返回、不长跳」；
   一个悄悄失败/悄悄写错寄存器的 setter 是这类 API 最贵的 bug。**返回值不影响「像语句一样用」**。
5. **不要假设「其它线程没意义」**：fixture 是单线程，但真实目标里别的线程可能正停在有意义的地方
   （等锁、在系统调用里、持有堆锁）。缺省用 `d.stop.tid` 只是**便利**，`d:threads()` + 显式 tid 才是
   看别处的入口。

`d:pump()` 返回 `nil`（不再是现在的 `0`）让 `while d:pump(100) do ... end` 读起来自然；
「没等到事件」与「引擎出错」靠第二返回值区分。`first_chance` 的真值仍要等 T7.4（§2.2 第 5 条）
由 core 补上（现在通用 `on_exception` 通道不传）。

### 11.5 session 生命周期与 registry（D54）

- **`__gc` 绝不做副作用**（不 detach、不 kill）。`__gc` 可能在 `lua_close` 期间、在回调内部、在任意
  步骤触发；它一旦动目标就是「随手 `d = nil` 把目标弄死」＋不可门禁的时序（同 PITFALLS M10 的取向）。
  `__gc` 只做「解绑 Lua 侧句柄」。
- **session 进 C 侧 registry**，作用域 = **一次执行**（不是进程级）：
  - 于是 `d = nil` 只是丢句柄，会话仍在 registry 里，可以找回来；
  - **D43 不破**：执行结束 registry 清空，跨调用仍然无状态（想再连就 `dbg.attach(pid)`）；
    REPL 恰好就是「一次执行」，人的体验不受影响；
  - API：`dbg.sessions()` → `{ {root_pid, kind, state, lua_bound} }`、`dbg.session(pid)`、
    `dbg.last()`（最近一个会话）、`dbg.detach(pid)` / `dbg.kill(pid)` —— 「不记得 pid」这件事直接消失；
  - REPL 退出时打印「N 个会话未关闭」并逐个收尾（NT+ detach、9x kill），这是给人的安全网。
- **容器：侵入式链表线性扫，先不上 RBTree**。会话数是个位数量级（§9.6 的上限还没拍，但 8 这个量级）；
  零 CRT 下自己写红黑树是 ~150 行 + 自测，收益为零。真到几十个会话，需要的其实是
  `root_pid → session` 的哈希，也不是有序树。§10 的树路由只需「事件属于哪个 root」，线性足够。
- 与 §10 的关系：**对象 = 一棵被调试进程树**（root pid 为键）；子进程的 `CREATE_PROCESS` 路由进
  root 的会话；按 pid 的内存/断点句柄表就在会话对象里（§10 第 3 条要求的正是它）。

### 11.6 REPL 与人的体验（T7.5 的口径补充）

§4.3–4.5 已定：REPL 的输入是**合法 Lua 语句**（`dbg.*` / 对象方法），没有 gdb 式单字母命令。
对 AI 这是最优形态（skill 已按 Lua 写、一行可发一批动作、失败是值）；对人的缺口是四件小事：

1. `=expr` 简写（等价 `return expr`）；
2. 多行续行（`luaL_loadbuffer` 的 "end of input expected" 视为未完）；
3. `help()`（§11.3）；
4. **本地 REPL 的取消**：`dbg` 的 `on_wait_timeout` 有意不读控制台，所以本地 REPL 里长 `pump`
   目前无法 Ctrl+C。要么把 `decide_console_action`（已在 `common/`）接进 REPL 循环，要么明确写
   「本地 REPL 不提供中断」。**必须现在定，否则以后会被当 bug 报。**

明确不提供：历史记录 / 箭头键 / Tab 补全（Win9x 控制台 + 零 CRT + 无 readline，代价远大于收益）。

### 11.7 切片与顺序（覆盖 §8 的 T7.4–T7.6 部分）

| 切片 | 内容 | 量 | 门禁 |
| --- | --- | --- | --- |
| **A** | `close` → `detach`/`kill`（9x 无 `detach`）；`dbg.caps()`；`help()` 纯文本表 | ~200 行 | `detach.*`、`caps.*`、`help.plain-when-piped` |
| **B** | help 上色（`core::print_styled` + 探测 + `NO_COLOR` + `--json` 口径） | ~80 行 | `help.no-color-when-piped` |
| **C** | Session 对象（事件是**普通表**，无 Stop 对象）+ registry + `dbg.sessions/session/last/detach(pid)/kill(pid)`；29 名转薄转发；`Var` 退出新面 | ~550 行 | `session.*`（含 §10 那组 `route.*`）、`session.default-parity` |
| **D** | REPL 本体（T7.5：`event` 帧 / `out.id` 归因 / proto 2 / ABI 5 / `=` 简写 / 多行 / 退出收尾报告） | 按 §8 | `repl.*` |

顺序硬约束：**A 必须早于 C**（`detach`/`kill` 就是 Session 对象的方法）；**C 必须早于 D**（REPL 里
人/AI 都按对象写）。29 个旧名在 C 期间转成对象 API 的薄转发（行为逐字节不变，`session.default-parity`
守门），退役留给之后的 D 条目。