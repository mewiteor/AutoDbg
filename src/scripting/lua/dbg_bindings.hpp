#ifndef AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP
#define AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP
/* =============================================================================
 * src/scripting/lua/dbg_bindings.hpp —— `dbg` 表 + 会话对象（T7.3 / 切片 C1a）
 *
 * 来源：新写。形态照 `gui` / `open_file`：注册**一个全局表** `dbg`（AGENTS.md §5 第 6 条），
 *   29 个契约名一个都不动。
 *
 * 形状（切片 C1a：D55 / D57；C1b 把数据面接上对象；**C2a 放开多会话 + 进程级事件路由**）：
 *   · `dbg.create{...}` / `dbg.attach(pid)` → **会话对象**（full userdata + metatable `AutoDbgSession`）；
 *   · 对象方法：pump / cont / step / eip / arm / hits / dropped / setcap / stopped / paused / caps /
 *     help / detach / kill（`detach` 只在平台有 `DebugActiveProcessStop` 时装，D51）；
 *   · **数据面（C1b）**：read / read_u8 / read_u16 / read_u32 / read_u64 / read_str / write /
 *     context（读 + 部分更新）/ stack / mods / threads —— 一律 `nil, err` 报错（D55 第 4 条），
 *     键名与帧字段跟 `GetContext` / `StackTrace` 同源；
 *   · **C1d**：`v(addr)`（会话绑定的 `Var`，解引用一律走本会话的引擎）+ `context()` **全变体**
 *     （与 `GetContext` 逐字段同型，见 dbg_bindings.cpp 的类型口径块）；
 *   · **C2b（进程树）**：`proc(pid)` → **普通表**（`pid` 字段 + read / read_u8..u64 / read_str /
 *     write / v / context / stack / mods，都作用在**那个进程**上）、`procs()` → `{ pid, ... }`（根在第一个）、`current_pid()`（回调里 =
 *     事件 pid，顶层 = `root_pid`）。会话上的同名方法仍是**根进程**的视角；`d:v` 绑根进程、
 *     `p:v` 绑那个进程（`Var` 载荷的 pid 字段）；
 *   · 对象字段：`stop`（当前未应答的事件表，与 `pump` 返回的是**同一张表**）/ `root_pid` / `kind` /
 *     `state`（`running` / `stopped` / `exited`）；
 *   · 模块级保留 create / attach / pump / cont / step / eip / arm / hits / dropped / setcap / stopped /
 *     paused / detach / kill / caps / help —— 作用对象是**默认会话**（最近一次 create/attach 的那个），
 *     `dbg.detach(pid)` / `dbg.kill(pid)` 按 root pid 指名会话；
 *   · registry 查询：`dbg.sessions()` / `dbg.session(pid)` / `dbg.last()`（D54）；
 *   · `dbg.pump` 是**唯一的「等」**，且是**进程级**的：读一条事件 → 按 `ev.dwProcessId` 找归属会话
 *     → 在归属会话上分派 → 返回事件表（带 `session` 字段）。`d:pump` 是它的按会话过滤版（C2a）。
 *   · `dbg.code()` 已删：事件表带 `code`（异常码）与 `cont_code`（core 算好的默认应答码）。
 *
 * 回调参数形状（新接口一律普通数字/字符串，不再用 LuaVariant）：`{ pid, tid, ea, code, modname, rva }`，
 *   与 `dbg.hits()` 的记录同名同义。回调键在**两个入口共用**：
 *   `callback` / `exception_callback` / `on_exception` / `on_breakpoint` / `on_single_step` / `on_exit_process`
 *   （`attach` 不给选项表 ⇒ 不要回调，只记 `hits()`）。
 *
 * 两条固定文案（门禁逐字断）定在本层：
 *   "pump cannot be called from a callback"      —— 回调里调状态机
 *   "cannot run while a dbg session exists (detach/kill it first)" —— **存在** dbg 会话时调 `Run`/`Attach`
 * （后者被 bindings.cpp 的 `lua_run` / `lua_attach` 用：两条路径都独占整个进程的事件流，不能与 `dbg` 共存。）
 *
 * 切片 C1d 的两处对外增量：
 *   · **回调上下文**（`CallbackScope` + `cb_active/cb_session/cb_pid`）：引擎正在调用户回调这件事
 *     跨 `bindings.cpp`（`Run`/`Attach` 那条）与本文件（`dbg` 那条）两层共享 —— `V()` 靠它找会话；
 *   · **`d:v(addr)`**：会话绑定的 `Var`（`Var` 的载荷因此带 `DebuggerSession*`，见 bindings.cpp）。
 * ========================================================================== */
#include <windows.h>   /* DWORD（`CallbackScope` / `dbg_session_alive` 的形参）*/
#include <stdint.h>    /* uint64_t（`arg_num_or_lv`）*/
#include <lua.hpp>

namespace autodbg::core
{
class DebuggerSession;
}

namespace autodbg::host
{
void register_dbg_api(lua_State *L);

/* ---------------- 回调上下文（T7 切片 C1d） ----------------
 *
 * 「引擎此刻正在执行用户回调」是一个**跨两层的全局事实**（单线程串行 ⇒ 不需要 TLS，也不许用
 * `thread_local`：手工映射的模块不分配 TLS 槽，AGENTS.md §11.5）。置位点有两处：
 *   · `dbg_bindings.cpp` 的 `call_user`（`dbg` 会话的回调派发，会话与 pid 都是现成的）；
 *   · `bindings.cpp` 的每个 `lua_on_*` 适配器（`Run`/`Attach` 那条，会话 = 当前会话）。
 * 两处都用栈上 RAII（`CallbackScope`）包住，回调的**可见签名一个字不变**。
 *
 * 用途：`V()` / `Var:<method>` 要「用哪个引擎去读写」—— 回调里只有唯一答案（引擎正在跑这个脚本的
 * 那个会话），顶层/REPL 里**没有**答案。之前那种「猜 `current_session()`」正是本切片要消灭的歧义
 * （顶层猜到的可能是宿主会话，也可能是一个随手 `create` 出来的会话）。 */
class CallbackScope
{
public:
    CallbackScope(core::DebuggerSession *sess, DWORD pid) noexcept;
    ~CallbackScope() noexcept;
    CallbackScope(const CallbackScope &)            = delete;
    CallbackScope &operator=(const CallbackScope &) = delete;

private:
    core::DebuggerSession *prev_sess;
    DWORD                  prev_pid;
};

/* 现在有没有用户回调在跑；在跑时它的会话与事件 pid（没有则是 nullptr / 0）。 */
bool                   cb_active() noexcept;
core::DebuggerSession *cb_session() noexcept;
DWORD                  cb_pid() noexcept;

/* 地址类实参的两种收法（number 与 LuaVariant）—— 会话方法与 **`V()`** 共用同一份语义。
 * ❗`lv_check` 自己的严格语义不动：29 个契约名依赖它。 */
bool arg_num_or_lv(lua_State *L, int idx, uint64_t *out);

/* `d:v()` 绑定的引擎会话是否还活着（收尾 detach/kill 之后会话会从 registry 摘掉、之后被释放）。
 * 只**比较**指针、不解引用 ⇒ 对着已释放的会话调 `Var` 方法是干净的错误而不是 UAF。 */
bool dbg_session_alive(const core::DebuggerSession *sess) noexcept;

/* `dbg_paused()` = **任一**会话停着一个未应答事件（C2a：不再只是默认会话；没有会话 = 没停着事件）。 */
bool        dbg_paused() noexcept;
/* `dbg_busy()` = registry 非空（**存在** dbg 会话）。`Run` / `Attach` 在它为真时拒绝 —— 这是比
 * `dbg_paused()` 更严的那个判据（a live session 就是与契约名不可混用的状态）。 */
bool        dbg_busy() noexcept;

/* ---- REPL 退出时的会话报告（T7 切片 D2b，D54 的安全网） ----
 * `dbg_open_session_count()`：registry 里还开着的会话数。
 * `dbg_release_unclosed_sessions()`：把还开着的会话逐个收尾（有 `DebugActiveProcessStop` 就
 *   detach —— 目标继续活着；没有（Win95/98）就退 kill，与 `--detach` 的降级口径一致），单个失败
 *   只打 stderr、继续；返回收尾的个数。落地见 `repl.cpp` 的 `repl_unclosed_session_report`
 *   （本地 REPL 与服务端 REPL 都在 `lua_close` **之前**调它）。 */
int         dbg_open_session_count() noexcept;
int         dbg_release_unclosed_sessions();

const char *dbg_error_reentry() noexcept;
const char *dbg_error_paused()  noexcept;
const char *dbg_error_busy()    noexcept;

} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_DBG_BINDINGS_HPP */
