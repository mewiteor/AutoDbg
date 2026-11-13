/* =============================================================================
 * src/scripting/lua/dbg_bindings.cpp —— `dbg` 表与会话对象（T7.3 + 切片 C1a）
 *
 * 来源：新写（plan_t7.md §2.1 / §11.4 / §11.9.1）。形态照 `gui` / `open_file`：注册**一个全局表**
 *   `dbg`（AGENTS.md §5 第 6 条），29 个契约名一个都不动；便捷层（until / on_hit / 条件断点）是
 *   Lua sugar，归 T7.6，不在这里。
 *
 * 切片 C1a（D55 / D57，**仍限单会话**）改了两件事：
 *   ① `dbg.create` / `dbg.attach` 返回**会话对象**（full userdata + metatable `AutoDbgSession`）：
 *      方法面 = pump / cont / step / eip / arm / hits / dropped / setcap / stopped / paused / caps /
 *      help / detach / kill。原来的单份 `DbgState g` 拆成 `SessionState` 并进 C 侧 registry
 *      （模块级侵入式单链表，作用域 = **一次执行**，D54/D43）。
 *   ② `pump` 的返回值从「事件码」变成**普通事件表**（无 metatable）：`d:pump()` 返回的就是缓存进
 *      `d.stop` 的那**同一张表** ⇒ 判「还是不是同一个」用表身份 `e == d.stop`，没有 seq、没有代次。
 *
 * 切片 C1b（数据面接上对象）加了一组 `d:` 方法：`read` / `read_u8|u16|u32|u64` / `read_str` /
 *   `write` / `context` / `stack` / `mods` / `threads`。形状与对应的契约名同形（`ReadMemory` 的字节、
 *   `GetContext` 的键、`StackTrace` 的帧字段）；「真实读取长度」靠 core 新增的 `read_some`（§2.2 第 12 条）。
 *   ❗断点面（`bp` / `bp_hw` / `bps`）、`snapshot`/`restore`、`mem_regions` 不在本切片；代理在 C-remote。
 *
 * 切片 C1c（类型统一）：新面里**地址类的值一律 LuaVariant**（`LV_UZ`）、小整数仍是 number。
 *   反转了 C1b/T7.3 的「§2.1 第 3 条：新接口不用 LuaVariant」—— 完整口径、三条动机与比较陷阱
 *   写在下面「类型统一（T7 切片 C1c）」那个注释块里（它是唯一真相源）。落点概览：
 *   输出侧 `ev.ea` / `hits()[i].ea` / 用户回调的 `ea` / `mods()[i].base` / `stack()[i].ea` /
 *   `d:context()` 的 `UPTR` 字段 / `d:eip()` / `d:read_u64()`；输入侧 `d:read*` / `read_str` /
 *   `write` 的 `addr` 与 `d:context(tid, t)` 的字段值都**同时收** number 与 LuaVariant。
 *   `d:arm` 的 rva 与 `ev.rva` **仍是普通 number**（D57：门禁大量依赖 `ev.rva == 0x1028`）。
 *
 * 切片 C2a（多会话 + **进程级事件路由**，plan_t7.md §10 / §11.9.1 / D57）：
 *   ① **每个会话自带引擎会话**（`new core::DebuggerSession`；`release_state` 释放）—— 之前所有会话共用
 *      宿主挂给 core 的那一份，多会话会互相踩状态。
 *   ② `dbg.pump` = **唯一的「等」且是进程级的**：拿任意会话的 `wait_one` 读一条事件 → 按
 *      `ev.dwProcessId` 在 registry 里找归属会话（`root_pid` 精确匹配）→ 在归属会话上 `dispatch` →
 *      返回事件表（新增 `session` 字段 = 归属会话的句柄）。找不到归属 → `nil, err`，**不静默丢事件**。
 *   ③ `d:pump` = 缓存优先 + **过滤**：循环 `dbg.pump` 直到出现属于自己的事件；别人的事件已经在自己
 *      的会话里**分派完了**（D57 的「分派不能推迟」），只是不返回给本会话的调用方。
 *   ④ 允许同时存在多个会话（删掉 C1a 的「一次执行一个会话」拒绝）；新增 `dbg.detach(pid)` /
 *      `dbg.kill(pid)`（与 `d:detach()` / `d:kill()` 同义）。`dbg_paused()` = 任一会话停着事件；
 *      新增 `dbg_busy()`（registry 非空）作为 `Run`/`Attach` 的拒给判据。
 *   ⑤ 待应答槽的**诚实口径**：本切片只保证「一个会话 = 一棵单进程树」⇒ 每会话最多一个未应答事件
 *      （OS 挂起该进程的线程直到 `ContinueDebugEvent`）。因此 `d.stop` 仍是单槽；同一会话的第二个
 *      事件（只可能来自 C2b 的进程树）不覆盖 `d.stop`，而是诚实地报错（见 `kErrPending`）。
 *
 * pump 的两层（D57，C2a 起才是真的两层：有多个会话可路由）：
 *   · `dbg.pump([timeout[, autocont]])`（模块级）= **唯一的「等」**：读一条事件 → 分派（记录 + 回调 +
 *     引擎记账）→ 返回事件表；`nil` = 没等到事件；`nil, err` = 引擎错 / 回调里出错 / 被取消。
 *   · `d:pump(...)` = **缓存优先**：`d.stop` 非空就立即返回它（不阻塞、不调 core），否则走上面那条。
 *   ❗「停住」就是 `d.stop` 非空（= `d:paused()`），不引入额外 flag；`pump` 返回 nil **不清** `d.stop`，
 *     `d:cont()` 成功后才清 —— 没等到事件 ≠ 事件已被应答。
 *
 * 与旧实现（T7.3 的表函数）的差异：
 *   · 事件表字段：`type pid tid ea modname rva code first_chance cont_code`（与 `hits()` 记录、
 *     协议 `event` 帧同形，可直接 `jsonx.encode`）。**`e.code` 是异常码，`e.cont_code` 是 core 算好的
 *     默认应答码** —— 因此 T7.3 的 `dbg.code()` 已删（它只是为了补「pump 不携带 code」的缺口）。
 *   · `arm` 允许**延迟武装**（D57）：模块未加载时 core 的 `set_breakpoint` 只登记，`CREATE_PROCESS` /
 *     `LOAD_DLL` 时由 `apply_breakpoints_for_module` 补 `0xCC`。T7.3 的「base == 0 就直接失败」已删
 *     —— 否则 `create -> arm -> pump` 这个最自然的顺序会被拒。「模块始终没加载」留给收尾/bps() 报告。
 *   · 收尾仍是**两个动词**（D51）：`detach`（恢复原字节 + `DebugActiveProcessStop`，目标继续活着）与
 *     `kill`（先恢复原字节/释放回调，再 TerminateProcess）。**9x 上 `detach` 这个 key 根本不存在**
 *     （建表时 dynapi 探 `DebugActiveProcessStop`）；`kill` 永远有。`__gc` **不做副作用**（D54）：
 *     只解绑 Lua 侧句柄，绝不 detach/kill。
 *   · 默认会话转发：`dbg.create/attach/pump/cont/step/eip/arm/hits/dropped/setcap/stopped/paused/
 *     detach/kill/caps/help` 全部保留，作用对象 = **最近一次 create/attach 的那个会话**（`g_default`），
 *     错误文案逐字不变；新增 `dbg.sessions() / dbg.session(pid) / dbg.last()` 三个取回入口。
 *   · 一次执行可以同时有多个会话（每棵树一个）；`dbg.*` 作用在默认会话上，`dbg.detach(pid)` /
 *     `dbg.kill(pid)` 指名会话（C2a）。
 *
 * 切片 D2a（proto 2）只加了一件向外的事：每条命中记录除了进窗口（`hits()`）与用户回调，
 *   还向**当前执行连接**发一帧 `event`（`emit_event_frame`：json 与 `hits()` 记录同形，
 *   多一个 `dropped` 累计值；`id` = 产生它的命令号）。没有执行连接时它一个分支就返回，
 *   所以本地模式与 `--server` 的回显零副作用；发送失败只报 stderr，**绝不** `lua_error`。
 *
 * 命中记录缓冲在**宿主层**（core 不认识队列）：环形、满则丢最旧、`dropped()` 可数、`setcap(1..64)` 可调。
 *   记录只在四个事件通道产生（`on_breakpoint` / `on_exception` / `on_user_exception` / `on_single_step`）；
 *   断点**自带**的回调（`on_breakpoint_callback`）只转交用户，不另记 —— 否则同一个命中会被记两次。
 *   例外：断点的 `on_exception` 通道不记（同一命中已经由 `on_breakpoint` 记过），但仍会调用户回调。
 *   `cont_code` 只能等 core 在 `pump` 末尾算完才知道 ⇒ 泵完再把「这一轮新产生的记录」补上。
 *
 * 切片 D2b 对外只多了两个 C 接口：`dbg_open_session_count()` / `dbg_release_unclosed_sessions()`
 *   （REPL 退出前的安全网，D54）。Lua 面**零改动** —— 它们不是给脚本用的，是给 `repl.cpp` 的收尾
 *   用的；而 `d:detach()` / `d:kill()` 的引擎侧动作被抽成 `end_session_by_detach` /
 *   `end_session_by_kill`，三处（两个 Lua 面 + 安全网）共用同一份，行为逐字节不变。
 *
 * 回调：core 的回调签名带 `void *ctx`，这里传的是 **`SessionState*`**（不是 `lua_State*`）——
 *   每会话一份（C2a 的多会话路由就靠它）；`lua_State` 从 `st->L` 取。回调里出错**不**长跳穿 C++ 帧
 *   （D18）：`lua_pcall` 兜住，消息存起来，下一次 `pump` 把它作为第二返回值。
 *   回调里调状态机（pump/cont/step/arm/detach/kill/create/attach）→ `nil, "pump cannot be called from
 *   a callback"`（固定文案）；**存在** dbg 会话时调 `Run`/`Attach` →
 *   `"cannot run while a dbg session exists (detach/kill it first)"`（`dbg_busy()` 查 registry，C2a）。
 *
 * `current_session()` 的一致性（C2a 的回归修正）：每会话自带 `DebuggerSession`（`SessionState::sess`），
 *   而 29 个契约名走 `current_session()` —— 所以「当前会话」必须**持久**跟随 `dbg.last()`：
 *   `dbg.create` / `dbg.attach` 成功后 `sync_current_session()` 把 `st->sess` 挂上，
 *   `detach` / `kill` 收尾时回退到 registry 里剩下的最近一个会话、最后退回宿主会话
 *   （`g_host_sess` = `main.cpp` / `repl.cpp` / `exec.cpp` 挂的那一份）。
 *   `SessionScope` 只在会话方法调用期间强制切换到 `st->sess`，析构回到同一份 `sync_current_session()`
 *   （而不是「进入前的那个」），所以嵌套调用（会话方法里再调 29 名）也指向同一个引擎。
 *   core 调用点直接用 `st->sess->`（成员函数，作用对象明确是 this），仍按「当前会话」工作的
 *   内部路径（`stack_trace.cpp` / `hide.cpp`）则因此能取到正确的会话。
 *
 * 状态是模块级（不是 thread_local、也不是函数内非平凡 static —— §11.5 / PITFALLS M10）：执行严格串行
 *   （并发 1），与 `exec_guard` 同一口径。跨 pump 的引擎状态在会话成员里，这里只存「宿主侧」的那部分。
 * ========================================================================== */
#include "dbg_bindings.hpp"

#include <windows.h>
#include <intrin.h>   /* ZeroMem 宏（__stosb）；与 core/process.cpp 同一做法 */
#include <lua.hpp>
#include <string.h>   /* strcmp（`__index` 的字段分派） */

#include "common/allocator.hpp"   /* free_from_tchar → common::release（UNICODE 下才真释放） */
#include "common/dynapi.hpp"      /* get_proc_as：DebugActiveProcessStop 的能力探测（D51） */
#include "common/json_writer.hpp" /* `event` 帧的 json（T7 D2a） */
#include "common/platform.hpp"    /* is_win9x / arch_name / variant_name / flavor_name（caps） */
#include "core/context.hpp"       /* context_fields / ctx_kind：`d:context` 的键与 `GetContext` 同源 */
#include "core/core.hpp"
#include "core/types.hpp"         /* RunParams / DebugCallbacks / 事件载荷 / callback_ref_t */
#include "core/util.hpp"          /* TCHAR 宏、tchar 转换、ZeroMem */

#include "bindings.hpp"           /* lua_stack_trace：`d:stack` 复用同一个栈回溯迭代器（不写第二套） */
#include "client.hpp"             /* T7 C-remote：远端代理复用 `--remote` 的连接/发帧/收帧（不写第二套 TCP） */
#include "exec_guard.hpp"         /* D32：pump 空闲片里的那一个 poll 点 */
#include "help_bindings.hpp"      /* `dbg.help()` = `help('dbg')`（同一张文本表，不是第二真相源） */
#include "stream.hpp"             /* T7 D2a：`event` 帧发给当前执行连接 */
#include "variant.hpp"            /* lv_push / lv_check：契约名的参数形状是 LuaVariant（`StackTrace` 要它） */

namespace autodbg::host
{
using autodbg::core::utf8_to_utf16;   /* util.hpp 的 TCHAR 宏展开成这两个名字（所以要在本 TU 里可见） */
using autodbg::core::utf16_to_utf8;
using autodbg::common::release;       /* free_from_tchar 宏（UNICODE 下才真释放） */

namespace
{

const int kHitCapMax = 64;

/* 四条固定文案（门禁逐字断 / 默认会话转发必须逐字节保持，plan_t7.md §2.1 第 4 条）： */
const char *const kErrReentry = "pump cannot be called from a callback";
const char *const kErrPaused  = "cannot run while an event is unhandled";
const char *const kErrNoSess  = "dbg: no session (call dbg.create or dbg.attach first)";
/* C2a：**存在** dbg 会话时 `Run`/`Attach` 一律拒（两条路径都独占整个进程的事件流）。 */
const char *const kErrBusy = "cannot run while a dbg session exists (detach/kill it first)";
/* C2a：同一会话的第二个未应答事件 —— 只有 C2b 的进程树才会走到；❗不能覆盖 `d.stop`，
 * 覆盖会让先停住的进程永远挂起（那个进程的线程在等 `ContinueDebugEvent`）。 */
const char *const kErrPending = "dbg.pump: session already has a pending event (process trees land in C2b)";
/* 进程级路由找不到归属会话：诚实地报错，**不**静默丢事件（事件已经被 OS 取走了）。 */
const char *const kErrUnknownPid = "dbg.pump: event for unknown pid ";

/* 会话对象（full userdata）的元表名、方法表名、以及「状态 → 活着的 userdata」弱值表。
 * 弱值表让 `d = nil` 真的能丢句柄（`lua_bound` 因此有意义），会话本体仍在 C 侧 registry 里。 */
const char *const kSessionMeta    = "AutoDbgSession";
const char *const kSessionMethods = "AutoDbgSession.methods";
const char *const kSessionHandles = "AutoDbgSession.handles";

/* `DebugActiveProcessStop` 的能力（D51）：**平台属性**，在建表时（`register_dbg_api`）探一次。
 * 探不到（Win95/98）⇒ `dbg.detach` **不安装** —— 能力缺席，而不是「存在但一调就降级成 kill」。 */
bool g_has_detach = false;

typedef BOOL(WINAPI * debug_active_process_stop_fn)(DWORD);

debug_active_process_stop_fn resolve_detach() noexcept
{
    return autodbg::common::get_proc_as<debug_active_process_stop_fn>("kernel32.dll",
                                                                    "DebugActiveProcessStop");
}

enum HitType
{
    HIT_BREAKPOINT  = 1,
    HIT_EXCEPTION   = 2,
    HIT_SINGLE_STEP = 3
};

struct HitRecord
{
    int    seq;
    int    type;
    DWORD  pid;
    DWORD  tid;
    size_t ea;
    char   modname[40];    /* 定长拷贝：不能指向会话里的节点（下一次 pump 之后节点可能就没了） */
    DWORD  rva;
    DWORD  code;           /* 异常码（断点 = EXCEPTION_BREAKPOINT） */
    DWORD  cont_code;      /* 该事件的默认应答码（core 在 pump 末尾算出来，见 fill_cont_code） */
    int    first_chance;   /* -1 = core 没报（见文件头） */
};

enum SessionKind
{
    SESSION_CREATE = 1,
    SESSION_ATTACH = 2
};

/* 一个会话的**宿主侧**状态（原来的 `DbgState g`）。引擎侧在 `core::DebuggerSession` 里，
 * 这里只存「绑定层自己要知道」的那部分：回调 ref、命中窗口、参数串、缓存的事件表。 */
struct SessionState
{
    SessionState *next;      /* 侵入式链表首成员（registry 用） */

    lua_State *L;            /* 本执行的 Lua 状态（回调 ref、事件表缓存都挂这里） */

    int ref_entry;           /* `callback`（入口断点，一次性） */
    int ref_exception_cb;    /* `exception_callback`（一次性） */
    int ref_on_exception;
    int ref_on_breakpoint;
    int ref_on_single_step;
    int ref_on_exit_process;

    int stop_ref;            /* `d.stop` 那张事件表的 registry ref（LUA_NOREF = 没有未应答事件） */

    bool started;            /* begin 已跑、detach/kill 未跑 */
    bool registered;         /* 还在 C 侧 registry 里（收尾时删） */
    bool lua_bound;          /* Lua 侧还有一个 userdata 指着本状态（`__gc` 清掉） */

    int   kind;              /* SessionKind */
    DWORD root_pid;          /* 会话的键 = 进程树的根（C1a = 目标进程本身）；事件的 pid 另有一份 */
    DWORD last_pid;          /* 最近一条事件的 pid/tid（`cont()` 不带参数时用这一对） */
    DWORD last_tid;

    HitRecord ring[kHitCapMax];
    int head;
    int count;
    int cap;
    int dropped;
    int seq_next;

    char cb_error[160];      /* 回调里 lua_pcall 的错误（不长跳穿 C++ 帧，D18） */

    core::RunParams      params;
    core::DebugCallbacks cb;
    LPXSTR s_imagepath;      /* 本层分配，收尾时释放 */
    LPXSTR s_cmdline;
    LPXSTR s_cwd;

    core::DebuggerSession *sess;   /* 引擎会话（**本会话独有**，C2a；`release_state` 释放） */
};

/* C 侧 registry（D54）：作用域 = **一次执行**。`register_dbg_api`（每次建 Lua 状态）清空，
 * `create`/`attach` 加、`detach`/`kill` 删。容器是侵入式单链表线性扫 —— 会话数是个位数量级。
 * ❗C2a 起可以同时有**多个**会话（每会话自带引擎会话，但仍是单进程、同一个事件流）；
 *   事件归属由 `root_pid` 精确匹配（见 `pump_once`）。
 * ❗不是静态对象而是裸指针：手工映射的模块里别给静态对象找动态初始化的麻烦（§11.5）。 */
SessionState *g_sessions = nullptr;

/* 默认会话 = 最近一次 `create`/`attach` 的那个：29 个契约名与 `dbg.*` 的转发都作用在它上面。
 * 它被收尾（detach/kill）后置空，并回退到 registry 里剩下的最近一个会话（见 `sync_current_session`）；
 * `dbg.pump` 是进程级的，它在 `g_default` 为空时拿 registry 的头一个去等。 */
SessionState *g_default = nullptr;

/* 宿主**原来的**会话（`main.cpp` / `repl.cpp` / `exec.cpp` 挂给 `core::set_current_session()` 的那一份）：
 * dbg 会话全部收尾后，29 个契约名必须回到它身上（否则回退成 nullptr 就一调就踩空）。
 * ❗取两个时点：`register_dbg_api`（一次执行的开头，可能还没挂 —— `--server` 口径下 `exec.cpp`
 *   是 `create_state_with_libs()` **之后**才 `set_current_session`）与 `start_session`（那时脚本正在跑，
 *   宿主会话一定已挂；只在 registry 为空时补抓，免得把上一个 dbg 会话当成宿主会话）。 */
core::DebuggerSession *g_host_sess = nullptr;

/* 把「当前会话」（29 个契约名走的那一份）对准默认会话，并作为 dbg 会话收尾后的回退链：
 *   g_default → registry 里剩下的最近一个会话 → 宿主会话。
 * 调用点：`start_session`（成功后持久挂上）、`registry_remove`（收尾）、`registry_clear`（一次执行的开头）、
 *   `SessionScope` 析构（会话方法返回后 —— 不回到「进入前的那个」，所以嵌套调用也指向同一个引擎）。 */
static void sync_current_session()
{
    SessionState          *st   = g_default ? g_default : g_sessions;
    core::DebuggerSession *sess = (st != nullptr) ? st->sess : g_host_sess;
    if (sess != nullptr) core::set_current_session(sess);   /* 两边都空 ⇒ 宿主根本没挂，保持原样 */
}

/* 引擎此刻是否正在执行用户回调（单线程串行 ⇒ 这是一个全局事实，与具体会话无关）：
 * 回调里再调状态机一律拒（plan_t7.md §2.3）。 */
bool g_in_callback = false;

static int fail(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

static int reentry(lua_State *L) { return fail(L, kErrReentry); }

/* ---------------- registry ---------------- */

static void registry_add(SessionState *st)
{
    st->next  = g_sessions;
    g_sessions = st;
    st->registered = true;
}

static void registry_remove(SessionState *st)
{
    SessionState **pp = &g_sessions;
    while (*pp)
    {
        if (*pp == st)
        {
            *pp = st->next;
            break;
        }
        pp = &(*pp)->next;
    }
    st->next       = nullptr;
    st->registered = false;
    if (g_default == st) g_default = nullptr;
    sync_current_session();   /* 收尾的正是默认会话 ⇒ 回退到剩下的最近一个 / 宿主会话 */
}

static SessionState *registry_find(DWORD root_pid)
{
    for (SessionState *st = g_sessions; st; st = st->next)
    {
        if (st->root_pid == root_pid) return st;
    }
    return nullptr;
}

/* 一次执行的收尾（或下一次执行的开头）：registry 里的状态全部释放。
 * ❗这里**不碰 Lua**（`register_dbg_api` 时上一条执行的状态已经 close 了），只放 C 侧内存。 */
static void release_state(SessionState *st);
static void free_strings(SessionState *st);

static void registry_clear()
{
    SessionState *st = g_sessions;
    while (st)
    {
        SessionState *next = st->next;
        release_state(st);
        st = next;
    }
    g_sessions = nullptr;
    g_default  = nullptr;
    if (g_host_sess != nullptr) core::set_current_session(g_host_sess);   /* 释放掉的会话不能再当「当前」 */
}

/* ---------------- 小工具 ---------------- */

static void free_strings(SessionState *st)
{
    if (st->s_imagepath) free_from_tchar(st->s_imagepath);
    if (st->s_cmdline)   free_from_tchar(st->s_cmdline);
    if (st->s_cwd)       free_from_tchar(st->s_cwd);
    st->s_imagepath = nullptr;
    st->s_cmdline   = nullptr;
    st->s_cwd       = nullptr;
}

/* 释放一个尚未进 registry（或 registry 已经把它摘掉）的状态。
 * ❗C2a：每个会话自带一个 `DebuggerSession`，在这里一并释放（它的析构会解 PE/线程/断点三张链表）。
 *   未收尾（detach/kill）就丢弃的会话不在这里做任何引擎动作 —— `__gc` / 执行结束都不做事是 D54 的
 *   口径；引擎侧的「放行 + 关句柄」仍然只由 `end()`（`d:detach` / `d:kill`）触发。 */
static void release_state(SessionState *st)
{
    free_strings(st);
    delete st->sess;
    st->sess = nullptr;
    delete st;
}

static SessionState *new_session(lua_State *L)
{
    SessionState *st = new SessionState;
    if (!st) return nullptr;
    ZeroMem(*st);
    st->L        = L;
    st->cap      = kHitCapMax;
    st->seq_next = 1;
    /* C2a：**每个会话一个引擎会话**（之前共用宿主挂给 core 的那一份）。这样两棵树各自的
     * `proc()` / `peinfos()` / `cont()` / `to_detach()` 互不干扰 —— 多会话的全部前提。 */
    st->sess     = new core::DebuggerSession();
    /* `ZeroMem` 给的是 0，而 LUA_NOREF 是 -2 —— ref 必须逐个显式置哨兵 */
    st->stop_ref           = LUA_NOREF;
    st->ref_entry          = LUA_NOREF;
    st->ref_exception_cb   = LUA_NOREF;
    st->ref_on_exception   = LUA_NOREF;
    st->ref_on_breakpoint  = LUA_NOREF;
    st->ref_on_single_step = LUA_NOREF;
    st->ref_on_exit_process = LUA_NOREF;
    return st;
}

static int store_ref(lua_State *L, int n, const char *field)
{
    lua_getfield(L, n, field);
    int ref = LUA_NOREF;
    if (lua_isfunction(L, -1)) ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    return ref;
}

static void unref(lua_State *L, int ref)
{
    if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

/* 清掉「当前未应答事件」的缓存（`pump` 拿到新事件时、`cont` 成功后）。
 * ❗`pump` 没等到事件时**不能**调它（D55 第 3 条）。 */
static void clear_stop(SessionState *st)
{
    if (st->stop_ref != LUA_NOREF)
    {
        luaL_unref(st->L, LUA_REGISTRYINDEX, st->stop_ref);
        st->stop_ref = LUA_NOREF;
    }
}

static const char *kind_name(int kind)
{
    return (kind == SESSION_ATTACH) ? "attach" : "create";
}

/* `state` 字段（D55）：`stopped` = 停着一个未应答事件；`exited` = 目标没了（或已收尾）。 */
static const char *state_name(SessionState *st)
{
    if (!st->started) return "exited";
    if (st->stop_ref != LUA_NOREF) return "stopped";
    if (!st->sess->cont()) return "exited";   /* 循环出口已置：目标退出 / 被终止 */
    return "running";
}

static void copy_name(char *out, size_t cap, LPCTSTR name)
{
    out[0] = 0;                     /* 转换失败（极少见的分配失败）时留空串，不是上一轮的垃圾 */
    /* UNICODE：tchar_to_utf8 是分配来的，用完要释放；ANSI（legacy 变体）：宏就是原样返回，不能 release */
#ifdef _UNICODE
    LPSTR u = utf16_to_utf8(name, NULL);
    if (!u) return;
#else
    LPCSTR u = name;
#endif
    size_t i = 0;
    for (; u[i] && i < cap - 1; i++) out[i] = u[i];
    out[i] = 0;
#ifdef _UNICODE
    release(u);
#endif
}

/* ---------------- 类型统一（T7 切片 C1c）：地址类值 = LuaVariant，小整数 = number ----------------
 *
 * 口径（写一次，下面各处的注释引用这里）：
 *   地址 / 指针 / 指针宽的寄存器值 ⇒ **LuaVariant**（`LV_UZ` = x86 的 U32 / x64 的 U64）：
 *     `ev.ea`、`hits()[i].ea`、`mods()[i].base`、`stack()[i].ea`、`d:context()` 里
 *     `ctx_kind::UPTR` 的字段（`Cax…Cip` / `Dr0-Dr7` / x64 的 `R8-R15` 与 home 槽）、`d:eip()`；
 *   **小整数 ⇒ 普通 number**：`pid/tid/type/code/cont_code/first_chance/rva/hits/dropped/size/index`、
 *     `d:read_u8|u16|u32`、`ctx_kind::U32` 的字段（`EFlags/ContextFlags/Seg*`）与 `U64` 的字段
 *     （`VectorControl`/`DebugControl`）、`dbg.sessions()` 的 `root_pid`、`dbg.caps()`、`d:arm` 的 rva；
 *   `d:read_u64()` 是 **`LV_U64`**（宽度由函数名给出；x86 上 `LV_UZ` 是 4 字节，装不下 64 位读值）。
 *
 * 为什么不是「一律普通整数」（§2.1 第 3 条的旧口径就此反转，三条理由）：
 *   ① 与 29 个契约名天然互通：`ReadMemory(ev.ea, 4)` / `Ea2ModnameRva(ev.ea)` 本来就吃 LuaVariant，
 *      新面与 `GetModBase` 同型就省掉一层转换；
 *   ② `jsonx.encode` 的 userdata 分支直接吐 `.val`（**十进制串、精确**）⇒ `--json` 的 `result`
 *      与 `dbglib` 的远端过线对 x64 地址不再「静默丢低位」（`jsonx` 的 number 分支是 `%.14g`）；
 *   ③ 远端代理的 `{"__i":…}` 兜底不再是地址类值的唯一出路（编码侧改走 `__u64`，解码兼容旧对端）。
 *
 * ❗比较的陷阱（门禁与脚本都要知道）：`__eq` 只在**两边 variant 类型相同**时才比（`variant.cpp`
 *   的 `lua_variant_eq`）；`__lt`/`__le` 会跨类型。x86 上 `ev.ea` 是 `LV_U32` 而 `d:read_u64`
 *   是 `LV_U64`，`ev.ea + 1` 经 `lv_simple` 还可能被**窄化**成 U16 之类 ⇒ 凡可能跨宽度/窄化的比较
 *   一律用 `.val`（十进制串）或 `tostring`（hex），不要赌 `==`。
 * ------------------------------------------------------------------------------------------- */

/* userdata 是不是 LuaVariant：比较元表，**不**用 `luaL_checkudata`（它会 `lua_error` 长跳，
 * 而这里要的是「判假后由调用方当值报错」；会话句柄 / 连接句柄也是 userdata）。 */
static bool is_lua_variant(lua_State *L, int idx)
{
    if (!lua_getmetatable(L, idx)) return false;
    luaL_getmetatable(L, "LuaVariant");
    bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same;
}

/* 地址类的值怎么推给 Lua：`LV_UZ` = 指针宽（x86 = U32 / x64 = U64）。`size_t` 与 `val.uz`
 * 在两种架构上宽度一致 ⇒ 直接赋值，不需要按 `_WIN64` 分叉。 */
static void push_ea(lua_State *L, size_t v)
{
    lv_push(L, LV_UZ)->val.uz = v;
}

/* 地址类实参：number 与 LuaVariant **都收**（C1c）。
 * ❗`lv_check` 本身不动 —— 29 个契约名依赖它的严格语义（`ReadMemory` 只吃 UZ）。
 * number 一侧仍用 `lua_isnumber`（与改前一致：数字串也照旧收）。 */
static bool arg_num_or_lv(lua_State *L, int idx, uint64_t *out)
{
    if (lua_type(L, idx) == LUA_TUSERDATA)
    {
        if (!is_lua_variant(L, idx)) return false;
        *out = (uint64_t)lv_check(L, idx, LV_ANY)->val.uz;   /* `uz` 随架构展开成 u32/u64 */
        return true;
    }
    if (!lua_isnumber(L, idx)) return false;
    *out = (uint64_t)lua_tointeger(L, idx);
    return true;
}

/* ---------------- 命中窗口 ---------------- */

/* T7 切片 D2a：把一条命中记录作为 `event` 帧（16）发给**当前执行连接**。
 * 字段与 `hits()` 的记录同形（AI 侧只学一套字段），另多一个 `dropped` = 记录时的累计丢弃数
 * （告诉客户端「你看的是一个窗口」，plan_t7.md §7）。
 * 三条口径：
 *   ① **只在有执行连接时发**：`stream_active()` 为假（本地模式、`--server` 的命令行回显）就
 *      直接返回 —— 不分配、不加锁、零副作用（这是回调路径，绝不能乱花时间）；
 *   ② 发送失败**不中断调试**：只往 stderr 报一句（不能 `lua_error`，否则一次网络抖动就会毁掉
 *      整次调试会话）；
 *   ③ 不进 `hits()` 的形状（历史窗口按原样），也不进 `--json` 的信封。 */
static void emit_event_frame(const HitRecord &r, int dropped)
{
    if (!stream_active())
        return;

    autodbg::common::json_writer w;
    w.begin_object();
    w.key("type");         w.number((long long)r.type);
    w.key("pid");          w.number((long long)r.pid);
    w.key("tid");          w.number((long long)r.tid);
    w.key("ea");           w.number((unsigned long long)r.ea);
    w.key("modname");      w.string(r.modname);
    w.key("rva");          w.number((long long)r.rva);
    w.key("code");         w.number((long long)r.code);
    w.key("first_chance");
    if (r.first_chance < 0) w.null_value();
    else                    w.boolean(r.first_chance != 0);
    w.key("dropped");      w.number((long long)dropped);
    w.end_object();

    if (w.data() == nullptr)
        return;   /* 分配失败：不发（诊断信息，不打断调试） */
    if (!stream_write_frame(stream_frame_event, stream_command_id(), 0, w.data(), w.size(), nullptr, 0))
        autodbg::core::ErrorS("dbg: event frame send failed\n");
}

static void record_hit(SessionState *st, int type, DWORD pid, DWORD tid, size_t ea, DWORD code,
                       int first_chance, LPCTSTR modname, DWORD rva)
{
    HitRecord *r;
    if (st->count < st->cap)
    {
        r = &st->ring[(st->head + st->count) % kHitCapMax];
        st->count++;                        /* 窗口里还剩格子 */
    }
    else
    {
        st->dropped++;                          /* 满则丢最旧（可断） */
        st->head = (st->head + 1) % kHitCapMax; /* 窗口前移：最旧那条格子腾出来 */
        r = &st->ring[(st->head + st->count - 1) % kHitCapMax];
    }
    r->seq          = st->seq_next++;
    r->type         = type;
    r->pid          = pid;
    r->tid          = tid;
    r->ea           = ea;
    r->code         = code;
    r->rva          = rva;
    r->first_chance = first_chance;
    r->cont_code    = 0;      /* core 在 pump 末尾才算得出来（fill_cont_code 补） */
    copy_name(r->modname, sizeof(r->modname), modname);

    /* T7 D2a：同一条记录也发一帧 `event` 给执行连接（`hits()` 的形状不动）。 */
    emit_event_frame(*r, st->dropped);
}

/* `pump` 之后：给这一轮（`seq >= from_seq`）新产生的记录补上默认应答码。
 * ❗core 把 `m_last_code` 写在 `pump` 的**末尾**，所以记录时刻拿不到；泵完回填是唯一正确的时点。 */
static void fill_cont_code(SessionState *st, int from_seq)
{
    DWORD code = st->sess->last_code();
    for (int i = 0; i < st->count; i++)
    {
        HitRecord &r = st->ring[(st->head + i) % kHitCapMax];
        if (r.seq >= from_seq) r.cont_code = code;
    }
}

/* 这一泵里最新的一条命中记录（没有 ⇒ nullptr）。事件表用它填 ea/modname/rva/code/first_chance。 */
static const HitRecord *newest_hit(SessionState *st, int from_seq)
{
    const HitRecord *found = nullptr;
    for (int i = 0; i < st->count; i++)
    {
        const HitRecord &r = st->ring[(st->head + i) % kHitCapMax];
        if (r.seq >= from_seq && (found == nullptr || r.seq > found->seq)) found = &r;
    }
    return found;
}

/* 在 `ea` 处记录一条命中，并把同一份 modname/rva 交给用户回调（字段名与 `hits()` 记录一致） */
static void record_hit_by_ea(SessionState *st, int type, DWORD pid, DWORD tid, size_t ea, DWORD code,
                             int first_chance, int ref);

/* 会话句柄（定义在下面的「会话对象」段：`push_event_table` 要往事件表里塞 `session`）。 */
static void push_handle(lua_State *L, SessionState *st);

static void push_hit_table(lua_State *L, SessionState *st)
{
    for (int i = 0; i < st->count; i++)
    {
        const HitRecord &r = st->ring[(st->head + i) % kHitCapMax];
        lua_createtable(L, 0, 10);
        lua_pushinteger(L, (lua_Integer)r.seq);          lua_setfield(L, -2, "seq");
        lua_pushinteger(L, (lua_Integer)r.type);         lua_setfield(L, -2, "type");
        lua_pushinteger(L, (lua_Integer)r.pid);          lua_setfield(L, -2, "pid");
        lua_pushinteger(L, (lua_Integer)r.tid);          lua_setfield(L, -2, "tid");
        push_ea(L, r.ea);                                lua_setfield(L, -2, "ea");
        lua_pushstring(L, r.modname);                    lua_setfield(L, -2, "modname");
        lua_pushinteger(L, (lua_Integer)r.rva);          lua_setfield(L, -2, "rva");
        lua_pushinteger(L, (lua_Integer)r.code);         lua_setfield(L, -2, "code");
        lua_pushinteger(L, (lua_Integer)r.cont_code);    lua_setfield(L, -2, "cont_code");
        if (r.first_chance < 0) lua_pushnil(L);
        else                    lua_pushinteger(L, r.first_chance ? 1 : 0);
        lua_setfield(L, -2, "first_chance");
        lua_rawseti(L, -2, i + 1);
    }
}

/* `pump` 的返回值 = **普通表**（无 metatable，D55）：字段与 `hits()` 记录、协议 `event` 帧同形。
 * `type` 是 `DEBUG_EVENT` 码，`code` 是异常码（没有异常的事件 = 0），`cont_code` 是默认应答码。
 * `session`（C2a）= 本事件**归属的会话**：`d:pump` 靠它过滤（D57：不返回别人的事件），
 *   脚本侧也靠它给事件归因（而不是拿 ev.pid 去猜）。 */
static void push_event_table(lua_State *L, SessionState *st, int type, int from_seq)
{
    const HitRecord *hit = newest_hit(st, from_seq);
    lua_createtable(L, 0, 10);
    lua_pushinteger(L, (lua_Integer)type);                            lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)st->last_pid);                    lua_setfield(L, -2, "pid");
    lua_pushinteger(L, (lua_Integer)st->last_tid);                    lua_setfield(L, -2, "tid");
    push_ea(L, hit ? hit->ea : 0);                                    lua_setfield(L, -2, "ea");
    lua_pushstring(L, hit ? hit->modname : "");                       lua_setfield(L, -2, "modname");
    lua_pushinteger(L, (lua_Integer)(hit ? hit->rva : 0));            lua_setfield(L, -2, "rva");
    lua_pushinteger(L, (lua_Integer)(hit ? hit->code : 0));           lua_setfield(L, -2, "code");
    if (hit == nullptr || hit->first_chance < 0) lua_pushnil(L);
    else                                         lua_pushinteger(L, hit->first_chance ? 1 : 0);
    lua_setfield(L, -2, "first_chance");
    lua_pushinteger(L, (lua_Integer)st->sess->last_code());           lua_setfield(L, -2, "cont_code");
    push_handle(L, st);                                               lua_setfield(L, -2, "session");
}

/* 用户回调的参数形状：`{ pid, tid, ea, code, modname, rva }` —— 与 `hits()` 的记录同名同义
 * （AI 侧只学一套字段）⇒ 同一个字段名在两个通道里**必须同型**，所以 `ea` 也是 LuaVariant
 * （C1c 的类型口径见上面的注释块；`rva`/`code`/`pid`/`tid` 仍是小整数）。
 * `ref == LUA_NOREF` ⇒ 没有回调：**仍然记录**（记录与回调是两条独立通道）。 */
static void call_user(SessionState *st, int ref, DWORD pid, DWORD tid, size_t ea, DWORD code,
                      LPCTSTR modname, DWORD rva)
{
    if (ref == LUA_NOREF) return;
    lua_State *L = st->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }

    char modbuf[40];
    copy_name(modbuf, sizeof(modbuf), modname ? modname : TEXT(""));

    lua_createtable(L, 0, 6);
    lua_pushinteger(L, (lua_Integer)pid);  lua_setfield(L, -2, "pid");
    lua_pushinteger(L, (lua_Integer)tid);  lua_setfield(L, -2, "tid");
    push_ea(L, ea);                        lua_setfield(L, -2, "ea");
    lua_pushinteger(L, (lua_Integer)code); lua_setfield(L, -2, "code");
    lua_pushstring(L, modbuf);             lua_setfield(L, -2, "modname");
    lua_pushinteger(L, (lua_Integer)rva);  lua_setfield(L, -2, "rva");

    g_in_callback = true;
    if (lua_pcall(L, 1, 0, 0) != 0)
    {
        const char *msg = lua_tostring(L, -1);
        size_t i = 0;
        if (msg)
        {
            for (; msg[i] && i < sizeof(st->cb_error) - 1; i++) st->cb_error[i] = msg[i];
        }
        st->cb_error[i] = 0;
        lua_pop(L, 1);
    }
    g_in_callback = false;
}

static void record_hit_by_ea(SessionState *st, int type, DWORD pid, DWORD tid, size_t ea, DWORD code,
                             int first_chance, int ref)
{
    core::PEInfo *pe      = st->sess->get_peinfo_by_ea(ea);
    LPCTSTR       modname = pe ? (pe->Path + pe->name_offset) : TEXT("");
    DWORD         rva     = pe ? (DWORD)(ea - pe->base) : 0;
    record_hit(st, type, pid, tid, ea, code, first_chance, modname, rva);
    call_user(st, ref, pid, tid, ea, code, modname, rva);
}

/* ---------------- C 回调（签名与 DebugCallbacks 逐字对应；ctx = SessionState*） ---------------- */

static void h_any_event(void *ctx, const core::DebugEvent &ev)
{
    SessionState *st = (SessionState *)ctx;
    st->last_pid = ev.pid;    /* `cont()` 不带参数时用的就是这一对 */
    st->last_tid = ev.tid;
}

static void h_exception(void *ctx, const core::ExceptionEvent &ev)
{
    SessionState *st = (SessionState *)ctx;
    /* 断点由 `on_breakpoint` 记录（同一个命中只记一次）；这里记其余异常。
     * ❗用户回调**每条异常都调**（含断点）——与 `Run` 的 `exception_callback` 口径一致。 */
    if (ev.code != EXCEPTION_BREAKPOINT)
        record_hit_by_ea(st, HIT_EXCEPTION, ev.pid, ev.tid, ev.addr, ev.code, -1, LUA_NOREF);
    core::PEInfo *pe = st->sess->get_peinfo_by_ea(ev.addr);
    call_user(st, st->ref_on_exception, ev.pid, ev.tid, ev.addr, ev.code,
              pe ? (pe->Path + pe->name_offset) : TEXT(""), pe ? (DWORD)(ev.addr - pe->base) : 0);
}

static void h_user_exception(void *ctx, const core::UserExceptionEvent &ev)
{
    SessionState *st = (SessionState *)ctx;
    /* 这条通道带真 first_chance（`on_exception` 通道 core 不传 → 那边记 -1） */
    record_hit_by_ea(st, HIT_EXCEPTION, ev.pid, ev.tid, (size_t)ev.record->ExceptionAddress,
                     ev.record->ExceptionCode, ev.first_chance ? 1 : 0, st->ref_exception_cb);
}

static void h_breakpoint(void *ctx, const core::BreakpointEvent &ev)
{
    SessionState *st = (SessionState *)ctx;
    record_hit_by_ea(st, HIT_BREAKPOINT, ev.pid, ev.tid, ev.addr, EXCEPTION_BREAKPOINT, -1,
                     st->ref_on_breakpoint);
}

static void h_single_step(void *ctx, const core::StepEvent &ev)
{
    SessionState *st = (SessionState *)ctx;
    record_hit_by_ea(st, HIT_SINGLE_STEP, ev.pid, ev.tid, ev.addr, EXCEPTION_SINGLE_STEP, -1,
                     st->ref_on_single_step);
}

static void h_first_breakpoint(void *ctx, const core::FirstBreakpointEvent &ev)
{
    SessionState *st = (SessionState *)ctx;
    call_user(st, st->ref_entry, ev.pid, ev.tid, 0, 0, TEXT(""), 0);
}

static void h_exit_process(void *ctx, const core::ExitEvent &ev)
{
    SessionState *st = (SessionState *)ctx;
    call_user(st, st->ref_on_exit_process, ev.pid, ev.tid, 0, ev.code, TEXT(""), 0);
}

static void h_bp_callback(void *ctx, const core::BreakpointCallbackEvent &ev)
{
    SessionState *st = (SessionState *)ctx;
    /* 该通道不带首次命中的记录（`on_breakpoint` 已经记过），只转交用户回调 */
    call_user(st, (int)ev.callback.token.i, ev.pid, ev.tid, ev.ea, EXCEPTION_BREAKPOINT,
              ev.modname, ev.rva);
}

static void h_bp_destroyed(void *ctx, callback_ref_t callback)
{
    if (!callback.valid) return;
    SessionState *st = (SessionState *)ctx;
    luaL_unref(st->L, LUA_REGISTRYINDEX, (int)callback.token.i);
}

static int h_wait_timeout(void *ctx, bool can_detach_without_term)
{
    (void)ctx;
    if (guard_exec_active())
    {
        guard_poll_control();                                   /* D32 第①个 poll 点 */
        int soft = guard_take_soft();
        if (soft == guard_soft_kill) return core::WAIT_TIMEOUT_DETACH_AND_TERMINATE;
        if (soft == guard_soft_detach)
        {
            if (can_detach_without_term)
            {
                guard_mark_detached(true);
                return core::WAIT_TIMEOUT_DETACH_KEEP_ALIVE;
            }
            return core::WAIT_TIMEOUT_DETACH_AND_TERMINATE;
        }
        if (!guard_should_stop()) return core::WAIT_TIMEOUT_CONTINUE;
        if (can_detach_without_term)
        {
            guard_mark_detached(true);
            return core::WAIT_TIMEOUT_DETACH_KEEP_ALIVE;
        }
        return core::WAIT_TIMEOUT_DETACH_AND_TERMINATE;
    }
    /* 本地模式：q / Shift+Q 的策略仍只属 `Run` 路径（`CheckConsoleInput`）。`dbg` 在这里不读控制台。 */
    return core::WAIT_TIMEOUT_CONTINUE;
}

/* ---------------- current_session 的交换 ---------------- */

/* 会话方法调 core 前把**本会话的引擎会话**挂成「当前会话」（§11.4 的接缝 / D55 / C2a）。
 * ❗C2a 起每个会话自带 `DebuggerSession`（与宿主那一份不是同一个）⇒ 交换不再是恒等，
 *   而析构**不回到「进入前的那个」**：它恢复到 `g_default`（没有则 registry 剩下的最近一个，
 *   再没有则宿主会话）—— 因为默认会话就是「当前会话」的持久定义（`sync_current_session`）。
 *   这样嵌套调用（会话方法里再调 29 个契约名）也指向同一个引擎。
 * ❗不是 thread_local：手工映射的模块禁用 TLS（§11.5），这里只是栈上的 RAII。 */
class SessionScope
{
public:
    explicit SessionScope(SessionState *st)
    {
        core::set_current_session(st->sess);
    }
    ~SessionScope() { sync_current_session(); }

    SessionScope(const SessionScope &)            = delete;
    SessionScope &operator=(const SessionScope &) = delete;
};

/* ---------------- 会话对象（userdata + metatable） ---------------- */

struct SessionUserdata
{
    SessionState *state;   /* `__gc` 之后为 nullptr（那一刻之后不再接受任何方法调用） */
};

static void push_new_userdata(lua_State *L, SessionState *st)
{
    SessionUserdata *ud = (SessionUserdata *)lua_newuserdata(L, sizeof(SessionUserdata));
    ud->state = st;
    luaL_getmetatable(L, kSessionMeta);
    lua_setmetatable(L, -2);
    st->lua_bound = true;
}

/* 「同一个会话 → 同一个 userdata」：句柄放在**弱值**表里（`__mode = "v"`）。
 * 弱值 ⇒ `d = nil` 之后句柄真的会被回收（`__gc` 解绑、`lua_bound` 变 false），
 * 而会话本体仍在 C 侧 registry 里 —— 再 `dbg.session(pid)` 取回来的是同一个会话（新句柄）。
 * 强引用会反过来让 `__gc` 在 `lua_close` 之前永远跑不到，`lua_bound` 也就失去意义（D54）。 */
static void push_handle(lua_State *L, SessionState *st)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kSessionHandles);
    lua_pushlightuserdata(L, st);
    lua_rawget(L, -2);
    if (lua_type(L, -1) == LUA_TUSERDATA)
    {
        lua_remove(L, -2);   /* 留下 userdata */
        return;
    }
    lua_pop(L, 1);                       /* nil → 表还在栈顶 */
    push_new_userdata(L, st);            /* [table, ud] */
    lua_pushlightuserdata(L, st);        /* [table, ud, key] */
    lua_pushvalue(L, -2);                /* [table, ud, key, ud] */
    lua_rawset(L, -4);                   /* table[key] = ud（弱值） */
    lua_remove(L, -2);                   /* 留下 ud */
}

/* `__index`：先认会话的**动态字段**（`stop` / `root_pid` / `kind` / `state`），其余落到方法表。 */
static int session_index(lua_State *L)
{
    SessionUserdata *ud = (SessionUserdata *)lua_touserdata(L, 1);
    SessionState    *st = ud ? ud->state : nullptr;
    if (st && lua_type(L, 2) == LUA_TSTRING)
    {
        const char *key = lua_tostring(L, 2);
        if (strcmp(key, "stop") == 0)
        {
            if (st->stop_ref != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, st->stop_ref);
            else                           lua_pushnil(L);
            return 1;
        }
        if (strcmp(key, "root_pid") == 0) { lua_pushinteger(L, (lua_Integer)st->root_pid); return 1; }
        if (strcmp(key, "kind") == 0)     { lua_pushstring(L, kind_name(st->kind));      return 1; }
        if (strcmp(key, "state") == 0)    { lua_pushstring(L, state_name(st));           return 1; }
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kSessionMethods);
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
    return 1;
}

/* `__gc`：**只解绑 Lua 侧句柄**，不 detach、不 kill（D54）。
 * `__gc` 可能在 `lua_close` 期间、在任意步骤触发；它一旦动目标就是「随手 `d = nil` 把目标弄死」
 * 加不可门禁的时序。已收尾（detach/kill）的会话把 C 侧状态一并释放，未收尾的留给 registry 清。 */
static int session_gc(lua_State *L)
{
    SessionUserdata *ud = (SessionUserdata *)lua_touserdata(L, 1);
    if (ud && ud->state)
    {
        SessionState *st = ud->state;
        ud->state = nullptr;
        st->lua_bound = false;
        if (!st->registered) release_state(st);
    }
    return 0;
}

static SessionState *self(lua_State *L)
{
    SessionUserdata *ud = (SessionUserdata *)lua_touserdata(L, 1);
    return ud ? ud->state : nullptr;
}

/* ---------------- 数据面（切片 C1b：plan_t7.md §2.2 第 12 条 + §11.4） ----------------
 * 形状与 29 个契约名里的对应项**同形**，只按 D52 把「LuaVariant → 普通整数」换掉：
 *   · `d:context()` 的键来自 core 的 `context_fields()` —— 那就是 `lua_get_context` 自己用的表，
 *     所以 x86 的 `Cax/Cbx/…/Cip`（= 通用寄存器，`C` 前缀是旧 X-macro 的口径）与 x64 多出的
 *     `R8-R15/Xmm0-15/P1Home…` 都由那张表的 `#ifdef _WIN64` 决定，这里**不另起一套键名**；
 *   · `d:stack()` 的帧字段 `index/ea/modname/rva` 就是 `StackTrace` 迭代器一次返回的那四个值
 *     （因此直接复用 `lua_stack_trace`，不写第二套 StackWalk 调用）。
 * 错误一律当值返回（D55 第 4 条：`nil, err`），这些方法从不 `lua_error`；**没有会话**时与
 * `pump` 同口径（`kErrNoSess`）—— 收尾（detach/kill）之后数据面就没有引擎可问了。 */

/* 缺省线程：`d.stop` 的那个（= 当前停住的线程）；没有未应答事件时用最近一条事件的 tid。
 * `d.stop` 是普通表（D55），所以这里只能从缓存里取 `tid` 字段，没有第二处可依。 */
static DWORD current_tid(lua_State *L, SessionState *st)
{
    if (st->stop_ref != LUA_NOREF)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, st->stop_ref);
        lua_getfield(L, -1, "tid");
        DWORD tid = (DWORD)lua_tointeger(L, -1);
        lua_pop(L, 2);
        if (tid) return tid;
    }
    return st->last_tid;
}

/* `d:context(nil, t)` / `d:stack()` 的缺省 tid：显式数字优先，否则走上面那条。 */
static DWORD arg_tid(lua_State *L, SessionState *st, int base)
{
    if (lua_type(L, base) == LUA_TNUMBER) return (DWORD)lua_tointeger(L, base);
    return current_tid(L, st);
}

/* 64 位值怎么推给 Lua：Lua 5.5（含 x86 构建）与 x64 的 LuaJIT 的 `lua_Integer` 都是 64 位 ⇒ 整数；
 * x86 的 LuaJIT 是 32 位 ptrdiff_t ⇒ 走 double（2^53 以内精确，读内存的值实际都在这个范围内）。
 * 判据与 `variant.cpp` 的 `set_lv_integer` 同一条（D52：新面只用普通整数）。 */
static void push_u64(lua_State *L, uint64_t v)
{
    if (sizeof(lua_Integer) >= 8) lua_pushinteger(L, (lua_Integer)v);
    else                          lua_pushnumber(L, (lua_Number)v);
}

/* `fail` 的两段拼接版（零 CRT 下没有 snprintf；只用于「指名是哪个键」的错误文案） */
static int fail_two(lua_State *L, const char *a, const char *b)
{
    char   buf[96];
    size_t n = 0;
    for (size_t i = 0; a[i] && n < sizeof buf - 1; i++) buf[n++] = a[i];
    for (size_t i = 0; b[i] && n < sizeof buf - 1; i++) buf[n++] = b[i];
    buf[n] = 0;
    return fail(L, buf);
}

/* `d:read(addr, n)` → **实际读到的字节**（Lua string，可能短于 n）。0 字节 ⇒ `nil, err`。
 * 与契约名 `ReadMemory` 的差别只有形状：那边返回字节数组（且部分读被吞成整体失败），
 * 这边返回字节串 + 真实长度（plan_t7.md §2.2 第 12 条）。 */
static int do_read(lua_State *L, SessionState *st, int base)
{
    if (!st->started) return fail(L, kErrNoSess);

    uint64_t a64;
    if (!arg_num_or_lv(L, base, &a64)) return fail(L, "d:read: addr (number or LuaVariant) expected");
    if (!lua_isnumber(L, base + 1)) return fail(L, "d:read: size (number) expected");

    size_t addr = (size_t)a64;
    size_t size = (size_t)lua_tointeger(L, base + 1);
    if (!addr || !size) return fail(L, "d:read: addr and size must be positive");

    void  *buf = alloc_(size);
    size_t got;
    {
        SessionScope scope(st);
        got = st->sess->read_some(addr, buf, size);
    }
    if (!got)
    {
        free_(buf);
        return fail(L, "d:read: unreadable address");
    }
    lua_pushlstring(L, (const char *)buf, got);
    free_(buf);
    return 1;
}

/* `d:read_u8/u16/u32(addr)` → 普通 number；`d:read_u64(addr)` → `LV_U64` variant
 * （宽度由函数名给出，D52；x86 上 `LV_UZ` 是 4 字节，装不下 64 位读值 ⇒ u64 必须走 LV_U64）。
 * 读不到 ⇒ `nil, err`。 */
static int read_int(lua_State *L, SessionState *st, int base, size_t nbytes, bool as_variant)
{
    if (!st->started) return fail(L, kErrNoSess);

    uint64_t a64;
    if (!arg_num_or_lv(L, base, &a64)) return fail(L, "d:read_*: addr (number or LuaVariant) expected");

    size_t addr = (size_t)a64;
    BYTE   buf[8];
    if (!addr) return fail(L, "d:read_*: addr must be positive");

    size_t got;
    {
        SessionScope scope(st);
        got = st->sess->read_some(addr, buf, nbytes);
    }
    if (got != nbytes) return fail(L, "d:read_*: unreadable address");

    uint64_t v = 0; /* 显式按字节拼：目标地址可能不对齐，不能直接解引用成 uint32_t/uint64_t */
    for (size_t i = 0; i < nbytes; i++) v |= (uint64_t)buf[i] << (8 * i);
    if (as_variant) lv_push(L, LV_U64)->val.u64 = v;
    else            push_u64(L, v);
    return 1;
}

static int do_read_u8(lua_State *L, SessionState *st, int base)  { return read_int(L, st, base, 1, false); }
static int do_read_u16(lua_State *L, SessionState *st, int base) { return read_int(L, st, base, 2, false); }
static int do_read_u32(lua_State *L, SessionState *st, int base) { return read_int(L, st, base, 4, false); }
static int do_read_u64(lua_State *L, SessionState *st, int base) { return read_int(L, st, base, 8, true); }

/* `d:read_str(addr)` → 以 NUL 结尾的字节串。复用 core 的 `loadstr`（`LoadStr` 用的同一个函数），
 * 所以语义逐字一致：每 16 字节一块、不含结尾 NUL、**字节原样**（不转码 —— UNICODE 变体也不转，
 * 与 `LoadStr` 相同；目标里的串是 UTF-8 还是 GBK 由目标的编译器决定）。 */
static int do_read_str(lua_State *L, SessionState *st, int base)
{
    if (!st->started) return fail(L, kErrNoSess);

    uint64_t a64;
    if (!arg_num_or_lv(L, base, &a64)) return fail(L, "d:read_str: addr (number or LuaVariant) expected");

    size_t addr = (size_t)a64;
    if (!addr) return fail(L, "d:read_str: addr must be positive");

    char  *s   = nullptr;
    size_t len = 0;
    bool   ok;
    {
        SessionScope scope(st);
        ok = st->sess->loadstr(addr, &s, &len);
    }
    if (!ok)
    {
        if (s) free_(s);
        return fail(L, "d:read_str: unreadable address");
    }
    if (s) lua_pushlstring(L, s, len);
    else   lua_pushliteral(L, "");   /* 首字节就是 NUL ⇒ `loadstr` 不给缓冲，但这不是错误 */
    if (s) free_(s);
    return 1;
}

/* `d:write(addr, data)` → `true` / `nil, err`。`data` 是字节串或整数：
 *   · 字节串：原样写（长度 = 串长）；
 *   · 整数：**u32 小端 4 字节**（宽度靠字节串表达 —— 新面不加 `d:write_u16/_u64`，D52 的
 *     「宽度靠函数名」在这里体现为「要别的宽度就给字节串」）。
 * 走 core 的 `write(..., up_intrin = true)`（写代码页时要刷指令缓存）。 */
static int do_write(lua_State *L, SessionState *st, int base)
{
    if (!st->started) return fail(L, kErrNoSess);

    uint64_t a64;
    if (!arg_num_or_lv(L, base, &a64)) return fail(L, "d:write: addr (number or LuaVariant) expected");

    size_t addr = (size_t)a64;
    if (!addr) return fail(L, "d:write: addr must be positive");

    const void *data = nullptr;
    size_t      size = 0;
    BYTE        word[4];
    int         type = lua_type(L, base + 1);
    if (type == LUA_TSTRING)
    {
        data = lua_tolstring(L, base + 1, &size);
    }
    else if (type == LUA_TNUMBER)
    {
        uint64_t v = (uint64_t)lua_tointeger(L, base + 1);
        for (size_t i = 0; i < sizeof word; i++) word[i] = (BYTE)(v >> (8 * i));
        data = word;
        size = sizeof word;
    }
    else
    {
        return fail(L, "d:write: string or number expected");
    }
    if (!size) return fail(L, "d:write: empty data");

    bool ok;
    {
        SessionScope scope(st);
        ok = st->sess->write(addr, data, size, true);
    }
    if (!ok) return fail(L, "d:write: cannot write");
    lua_pushboolean(L, 1);
    return 1;
}

/* `d:context([tid[, t]])`：一个函数两个方向（与 `GetContext` / `SetContext` 的分工一致）——
 *   · 不给表 ⇒ 返回整张寄存器表；
 *   · 给表 ⇒ **部分更新**（只写表里出现的键），返回 `true` / `nil, err`（D55 第 4 条：
 *     setter 必须返回值，静默失败/静默写错寄存器是这类 API 最贵的 bug）。 */
static int do_context(lua_State *L, SessionState *st, int base)
{
    if (!st->started) return fail(L, kErrNoSess);

    DWORD tid = arg_tid(L, st, base);
    if (!tid) return fail(L, "d:context: tid expected");

    bool   ok;
    HANDLE h = nullptr;
    CONTEXT ctx;
    ZeroMem(ctx);
    {
        SessionScope scope(st);
        h = st->sess->get_thread_by_tid(tid);
        ok = (h != nullptr) && st->sess->get_context(h, CONTEXT_ALL, &ctx);
    }
    if (!h)  return fail(L, "d:context: thread not found");
    if (!ok) return fail(L, "d:context: cannot get thread context");

    size_t                 count  = 0;
    const core::ctx_field *fields = core::context_fields(&count);

    if (!lua_istable(L, base + 1))
    {
        lua_createtable(L, 0, (int)count);
        for (size_t i = 0; i < count; i++)
        {
            const BYTE *p = (const BYTE *)&ctx + fields[i].offset;
            switch (fields[i].kind)
            {
            case core::ctx_kind::U32 : lua_pushinteger(L, (lua_Integer)(*(const uint32_t *)p)); break;
            case core::ctx_kind::U64 : push_u64(L, *(const uint64_t *)p); break;
            case core::ctx_kind::UPTR: push_ea(L, *(const size_t *)p); break;   /* C1c：指针宽 ⇒ LuaVariant */
            case core::ctx_kind::M128:
            {
                /* M128A = { Low, High }（与 `GetContext` 暴露的两个 64 位半部同名） */
                const uint64_t *q = (const uint64_t *)p;
                lua_createtable(L, 0, 2);
                push_u64(L, q[0]); lua_setfield(L, -2, "Low");
                push_u64(L, q[1]); lua_setfield(L, -2, "High");
                break;
            }
            }
            lua_setfield(L, -2, fields[i].name);
        }
        return 1;
    }

    /* setter：只写表里出现的键（`nil` = 不改；**未知键按 `SetContext` 的既有口径忽略**）；
     * 值不是数字/地址（M128 不是 {Low, High}）⇒ 指名报错，**一个字节都不写**（不写半套）。
     * ❗读与写用**同一套**列：UPTR 收 number 也收 LuaVariant（`arg_num_or_lv`），其余只收数字。 */
    for (size_t i = 0; i < count; i++)
    {
        lua_getfield(L, base + 1, fields[i].name);
        int t = lua_type(L, -1);
        if (t != LUA_TNIL && t != LUA_TNONE)
        {
            BYTE *p = (BYTE *)&ctx + fields[i].offset;
            if (fields[i].kind == core::ctx_kind::M128)
            {
                if (t != LUA_TTABLE) { lua_pop(L, 1); return fail_two(L, "d:context: bad value for ", fields[i].name); }
                lua_getfield(L, -1, "Low");
                lua_getfield(L, -2, "High");
                uint64_t lo = 0;
                uint64_t hi = 0;
                if (!arg_num_or_lv(L, -2, &lo) || !arg_num_or_lv(L, -1, &hi))
                {
                    lua_pop(L, 3);
                    return fail_two(L, "d:context: bad value for ", fields[i].name);
                }
                uint64_t *q = (uint64_t *)p;
                q[0]        = lo;
                q[1]        = hi;
                lua_pop(L, 2);
            }
            else
            {
                uint64_t v = 0;
                if (!arg_num_or_lv(L, -1, &v)) { lua_pop(L, 1); return fail_two(L, "d:context: bad value for ", fields[i].name); }
                switch (fields[i].kind)
                {
                case core::ctx_kind::U32 : *(uint32_t *)p = (uint32_t)v; break;
                case core::ctx_kind::U64 : *(uint64_t *)p = v; break;
                case core::ctx_kind::UPTR: *(size_t *)p = (size_t)v; break;
                case core::ctx_kind::M128: break; /* 上面已处理 */
                }
            }
        }
        lua_pop(L, 1);
    }

    bool wrote;
    {
        SessionScope scope(st);
        wrote = st->sess->set_context(h, CONTEXT_ALL, &ctx);
    }
    if (!wrote) return fail(L, "d:context: cannot set thread context");
    lua_pushboolean(L, 1);
    return 1;
}

/* 迭代器的一次返回值**不保证**四个都在：`lua_stack_trace_iter` 在「这一帧不属于任何已知模块」时
 * 只返回 `(index, ea)` 两个值（`bindings.cpp` 的原样行为，`get_peinfo_by_ea` 失败就 `return 2`）⇒
 * 缺的位置按 0 / "" 补。这里不能直接把 nil 交给 `lv_check`：那会 `lua_error` 长跳出去，
 * 而 D55 的口径是「错误是值」。 */
static lua_Integer frame_value(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TUSERDATA) return 0;
    return (lua_Integer)lv_check(L, idx, LV_ANY)->val.uz;   /* 迭代器给的变体类型是 UZ/U32（同一个 union） */
}

/* `d:stack([tid[, depth]])` → 帧数组（`{ index, ea, modname, rva }`，与 `StackTrace` 同形）。
 * 帧来源是**契约名** `StackTrace(tid)` 建出来的那个迭代器：`d:stack` 只负责把它逐帧收进数组
 * （再加一个帧数上限 —— §2.2 第 15 条：低层原语必须有界）。不复制 StackWalk 的 x86/x64 两条
 * 回调路径（那是 `stack_trace.cpp` 与绑定层已有的实现）。
 * ❗`lua_stack_trace` 按位置 1 取 LuaVariant、出错走 `lua_error` ⇒ 两处都用 `lua_pcall` 隔开：
 *   同时保证它的长跳不会穿过 `SessionScope` 的 C++ 帧（D18）。 */
static int do_stack(lua_State *L, SessionState *st, int base)
{
    if (!st->started) return fail(L, kErrNoSess);

    DWORD tid = arg_tid(L, st, base);
    if (!tid) return fail(L, "d:stack: tid expected");

    int depth = 64;   /* 缺省帧数上限；`StackTrace` 迭代器本身会自然结束，这里只是兜底 */
    if (lua_type(L, base + 1) == LUA_TNUMBER)
    {
        depth = (int)lua_tointeger(L, base + 1);
        if (depth < 1) depth = 1;
        if (depth > 512) depth = 512;
    }

    int top = lua_gettop(L);
    lua_pushcfunction(L, lua_stack_trace);
    lv_push(L, LV_U32)->val.u32 = tid;
    {
        SessionScope scope(st);
        if (lua_pcall(L, 1, 2, 0) != 0)   /* 成功 ⇒ (迭代器, nil)；失败 ⇒ (nil, 文案) */
        {
            char   buf[96];
            size_t n = 0;
            const char *msg = lua_tostring(L, -1);   /* ❗先拷出：`lua_settop` 之后这个指针就悬空了 */
            for (size_t i = 0; msg && msg[i] && n < sizeof buf - 1; i++) buf[n++] = msg[i];
            buf[n] = 0;
            lua_settop(L, top);
            return fail(L, n ? buf : "d:stack: cannot walk the stack");
        }
    }
    if (!lua_isfunction(L, -2))
    {
        const char *msg = lua_tostring(L, -1);
        char        buf[96];
        size_t      n = 0;
        for (size_t i = 0; msg && msg[i] && n < sizeof buf - 1; i++) buf[n++] = msg[i];
        buf[n] = 0;
        lua_settop(L, top);
        return fail(L, n ? buf : "d:stack: cannot walk the stack");
    }
    lua_pop(L, 1);                    /* 成功时第二个返回值是 nil */
    int iter = top + 1;               /* 迭代器固定在这个下标 */
    lua_createtable(L, 0, 8);         /* 帧数组 */
    int arr = iter + 1;

    for (int i = 0; i < depth; i++)
    {
        lua_pushvalue(L, iter);
        {
            SessionScope scope(st);
            if (lua_pcall(L, 0, 4, 0) != 0)
            {
                lua_settop(L, arr);       /* 迭代里出错：保留已收到的帧（错误当值报给上层没意义） */
                break;
            }
        }
        if (lua_isnil(L, iter + 2))
        {
            lua_settop(L, arr);           /* 迭代结束（0 个返回值 ⇒ 补出 4 个 nil） */
            break;
        }

        lua_createtable(L, 0, 4);         /* 帧表：字段名与迭代器的返回值一一对应 */
        lua_pushinteger(L, frame_value(L, iter + 2));
        lua_setfield(L, -2, "index");
        push_ea(L, (size_t)frame_value(L, iter + 3));   /* C1c：帧地址是地址类值 ⇒ LuaVariant */
        lua_setfield(L, -2, "ea");
        if (lua_type(L, iter + 4) == LUA_TSTRING) lua_pushvalue(L, iter + 4);
        else                                     lua_pushliteral(L, "");
        lua_setfield(L, -2, "modname");
        lua_pushinteger(L, frame_value(L, iter + 5));
        lua_setfield(L, -2, "rva");
        lua_rawseti(L, arr, i + 1);
        lua_settop(L, arr);               /* 丢掉这一帧的四个返回值 */
    }

    lua_pushvalue(L, arr);
    lua_remove(L, iter);                  /* 只返回帧数组 */
    return 1;
}

/* `d:mods()` → `{ {name, base, size, path}, ... }`：**纯投影**（`peinfos()` 的只读视图）。
 * 不在这里解析 PE、也不过滤 pid —— 同名多实例的区分交给 `(name, base)` 这两个字段（§2.2 第 1 条）。 */
static int do_mods(lua_State *L, SessionState *st)
{
    if (!st->started) return fail(L, kErrNoSess);

    int n = 0;
    for (core::PEInfo *pe = st->sess->peinfos().head(); pe; pe = pe->next) n++;
    lua_createtable(L, n, 0);

    int i = 0;
    for (core::PEInfo *pe = st->sess->peinfos().head(); pe; pe = pe->next)
    {
        LPCTSTR name = pe->Path + pe->name_offset;
        char    namebuf[64];
        copy_name(namebuf, sizeof namebuf, name);   /* 与事件表的 `modname` 同一口径（截断到 63 字节） */

        lua_createtable(L, 0, 4);
        lua_pushstring(L, namebuf);                          lua_setfield(L, -2, "name");
        push_ea(L, (size_t)pe->base);                        lua_setfield(L, -2, "base");   /* C1c：地址 ⇒ LuaVariant（`size` 仍是 number） */
        lua_pushinteger(L, (lua_Integer)pe->size);           lua_setfield(L, -2, "size");
        /* 完整路径转 UTF-8（UNICODE 变体下 `utf16_to_utf8` 是分配来的，用完释放） */
#ifdef _UNICODE
        LPSTR path = utf16_to_utf8(pe->Path, NULL);
        if (path) { lua_pushstring(L, path); release(path); }
        else        lua_pushliteral(L, "");
#else
        lua_pushstring(L, pe->Path);
#endif
        lua_setfield(L, -2, "path");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

/* `d:threads()` → `{ {tid}, ... }`：`threads()` 的只读投影。
 * ❗缺省 tid 只对「当前停住的线程」方便；看别的线程必须显式给 tid（D55 第 5 条）。 */
static int do_threads(lua_State *L, SessionState *st)
{
    if (!st->started) return fail(L, kErrNoSess);

    int n = 0;
    for (core::ThreadInfo *t = st->sess->threads().head(); t; t = t->next) n++;
    lua_createtable(L, n, 0);

    int i = 0;
    for (core::ThreadInfo *t = st->sess->threads().head(); t; t = t->next)
    {
        lua_createtable(L, 0, 1);
        lua_pushinteger(L, (lua_Integer)t->dwThreadId);
        lua_setfield(L, -2, "tid");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

/* ---------------- worker（模块级与对象方法共用；base = 第一个真实参数的下标） ---------------- */

static int do_pump(lua_State *L, SessionState *st, int base);

static void read_common_opts(lua_State *L, SessionState *st, int n)
{
    lua_getfield(L, n, "use_scylla_hide");
    st->params.use_scylla_hide = lua_toboolean(L, -1) ? true : false;
    lua_pop(L, 1);

    st->ref_entry           = store_ref(L, n, "callback");
    st->ref_exception_cb    = store_ref(L, n, "exception_callback");
    st->ref_on_exception    = store_ref(L, n, "on_exception");
    st->ref_on_breakpoint   = store_ref(L, n, "on_breakpoint");
    st->ref_on_single_step  = store_ref(L, n, "on_single_step");
    st->ref_on_exit_process = store_ref(L, n, "on_exit_process");
}

/* 装好 `DebugCallbacks` 调 core 的 `begin`，成功后把会话进 registry 并返回 userdata。 */
static int start_session(lua_State *L, SessionState *st, int kind)
{
    /* 宿主会话的兜底抓取：`register_dbg_api` 时可能还没挂（`--server` 口径，见 `g_host_sess` 的注释）。
     * ❗只在 registry 为空时抓：那时 `current_session()` 一定还是宿主那一份，不会是别的 dbg 会话。 */
    if (g_host_sess == nullptr && g_sessions == nullptr)
        g_host_sess = core::current_session_or_null();

    st->cb.context                 = st;    /* ❗ctx = SessionState*（不是 lua_State） */
    st->cb.on_any_debug_event      = h_any_event;
    st->cb.on_exception            = h_exception;
    st->cb.on_breakpoint           = h_breakpoint;
    st->cb.on_single_step          = h_single_step;
    st->cb.on_exit_process         = h_exit_process;
    st->cb.on_first_breakpoint     = h_first_breakpoint;
    st->cb.on_user_exception       = h_user_exception;
    st->cb.on_breakpoint_callback  = h_bp_callback;
    st->cb.on_breakpoint_destroyed = h_bp_destroyed;
    st->cb.on_wait_timeout         = h_wait_timeout;

    SessionScope scope(st);
    const char  *err = nullptr;
    int          r   = st->sess->begin(&st->params, &st->cb, &err);
    if (r == -2)
    {
        /* 1:1：这条路径不跑收尾（§4.1）。`-2` 只来自 `DebugActiveProcess` 失败
         * （Win95/98 根本没有附加能力 ⇒ legacy 变体上 `dbg.attach` 就是这句）。 */
        release_state(st);
        return fail(L, "attach failed (DebugActiveProcess) — Win95/98 has no attach");
    }
    if (r < 0)
    {
        release_state(st);                          /* -1 的清理已在 core 内跑过（D18） */
        return fail(L, err ? err : "engine error");
    }
    st->started  = true;
    st->kind     = kind;
    st->root_pid = (DWORD)st->sess->proc().dwProcessId;

    /* userdata 现在建：`begin` 成功后本路径没有第二个失败点了 */
    push_handle(L, st);
    registry_add(st);
    g_default = st;
    sync_current_session();   /* 持久挂上：29 个契约名从此刻起就作用在本会话的引擎上（切片 C2a 的回归修正） */
    return 1;
}

/* `dbg.create{ imagepath=…, cmdline=…, cwd=…, is_new_console=…, use_scylla_hide=…, <六个回调> }`
 * = `Run` 的原语版：`CreateProcess` + `DEBUG_PROCESS`，**不循环**；成功返回会话对象。 */
static int dbg_create(lua_State *L)
{
    if (g_in_callback)  return reentry(L);
    if (!lua_istable(L, 1)) return fail(L, "dbg.create: table expected");

    SessionState *st = new_session(L);
    if (!st) return fail(L, "dbg.create: out of memory");

    lua_getfield(L, 1, "imagepath");
    if (lua_isstring(L, -1))
    {
        st->s_imagepath      = utf8_to_tchar(lua_tostring(L, -1));
        st->params.imagepath = st->s_imagepath;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "cmdline");
    if (lua_isstring(L, -1))
    {
        st->s_cmdline      = utf8_to_tchar(lua_tostring(L, -1));
        st->params.cmdline = st->s_cmdline;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "cwd");
    if (lua_isstring(L, -1))
    {
        st->s_cwd      = utf8_to_tchar(lua_tostring(L, -1));
        st->params.cwd = st->s_cwd;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "is_new_console");
    st->params.is_new_console = lua_toboolean(L, -1) ? true : false;
    lua_pop(L, 1);

    /* 必填项显式判：core 会拿 NULL 去 `CreateProcess`，那只会换来一句 CreateProcess 失败 */
    if (!st->params.imagepath)
    {
        release_state(st);
        return fail(L, "dbg.create: imagepath expected");
    }

    read_common_opts(L, st, 1);
    return start_session(L, st, SESSION_CREATE);
}

/* `dbg.attach(pid [, { use_scylla_hide=…, <六个回调> }])` = `Attach` 的原语版。
 * ❗只认**显式** pid：`Attach{ }` 的「沿用上次 pid / 鼠标选进程」魔法属 D48 的解析链，
 *   在状态机原语层里是隐形状态（脚本该自己 `gui.pick()` 并把结果写进脚本头标记）。 */
static int dbg_attach(lua_State *L)
{
    if (g_in_callback) return reentry(L);
    if (!lua_isnumber(L, 1)) return fail(L, "dbg.attach: pid (number) expected");

    DWORD pid = (DWORD)lua_tointeger(L, 1);
    if (!pid) return fail(L, "dbg.attach: pid expected");

    bool has_opts = lua_istable(L, 2);
    if (!has_opts && lua_gettop(L) >= 2) return fail(L, "dbg.attach: options table expected");

    SessionState *st = new_session(L);
    if (!st) return fail(L, "dbg.attach: out of memory");

    st->params.pid = pid;                    /* 非 0 ⇒ core 走 `DebugActiveProcess` 而不是 CreateProcess */
    if (has_opts) read_common_opts(L, st, 2); /* 不给表 = 只要命中记录，不要回调 */
    return start_session(L, st, SESSION_ATTACH);
}

/* 引擎内部错误（D18：core 已自行跑过收尾）⇒ 宿主侧只收自己的那一半并把它从 registry 摘掉。
 * ❗不 `delete st`：会话对象还可能被一个 userdata 指着（之后的方法按「没有会话」失败），
 *   释放留给 `__gc`（那时 `!registered` ⇒ 由它收尾）。 */
static int finish_engine_error(lua_State *L, SessionState *st, const char *err)
{
    free_strings(st);
    clear_stop(st);
    st->started = false;
    registry_remove(st);
    return fail(L, err ? err : "engine error");
}

/* `fail` 的「前缀 + pid」版（零 CRT 没有 snprintf；只用于「指名是哪个 pid」的文案） */
static int fail_pid(lua_State *L, const char *prefix, DWORD pid)
{
    char   buf[96];
    size_t n = 0;
    for (size_t i = 0; prefix[i] && n < sizeof buf - 1; i++) buf[n++] = prefix[i];

    char digits[12];
    int  d = 0;
    do
    {
        digits[d++] = (char)('0' + (pid % 10));
        pid /= 10;
    } while (pid && d < (int)sizeof digits);
    while (d > 0 && n < sizeof buf - 1) buf[n++] = digits[--d];
    buf[n] = 0;
    return fail(L, buf);
}

/* 回调里 `lua_pcall` 的错误（D18：不长跳）→ `nil, msg`，并清掉缓冲（只报一次）。 */
static int take_cb_error(lua_State *L, SessionState *st)
{
    char   msg[170];
    size_t i = 0;
    for (; st->cb_error[i] && i < sizeof(msg) - 1; i++) msg[i] = st->cb_error[i];
    msg[i]          = 0;
    st->cb_error[0] = 0;
    return fail(L, msg);
}

/* 一条事件的「等 + 路由 + 分派 + 组装事件表」—— 进程级 `dbg.pump` 的**唯一**实现（D57 / C2a）。
 * 返回值：1 = 栈顶一个值（事件表或 `nil`）；2 = `nil, err`。
 * `*owner` = 事件归哪张会话（返回 `nil` 时不动它）；`d:pump` 靠它过滤。
 *
 * 路由：`WaitForDebugEvent` 是**进程级**的（一个调试器进程只有一个事件流），所以「谁来等」不影响
 * 「事件归谁」—— 拿默认会话（没有就 registry 的头一个）去等，再按 `ev.dwProcessId` 在 registry 里
 * 找 `root_pid` 精确匹配的会话。找不到 ⇒ `nil, err`（**不静默丢**：事件已经从 OS 取走了）。 */
static int pump_once(lua_State *L, DWORD timeout_ms, bool autocont, SessionState **owner)
{
    /* 回调里不能重入状态机（plan_t7.md §2.3：回调正在被 `pump_once` 派发）。模块级与对象方法
     * 两个入口共用本函数，所以这条判必须在这里（C2a 把 `dbg.pump` 从 `do_pump` 改成直走本函数）。 */
    if (g_in_callback) return reentry(L);

    SessionState *waiter = g_default ? g_default : g_sessions;
    if (!waiter) return fail(L, kErrNoSess);

    DEBUG_EVENT ev;
    const char *err = nullptr;
    int         type;
    {
        SessionScope scope(waiter);
        type = waiter->sess->wait_one(timeout_ms, &ev, &err);
    }
    if (type < 0) return finish_engine_error(L, waiter, err);

    /* ❗没等到事件（0）⇒ `nil`，且**不动**任何会话的 `d.stop`：上一轮那个未应答的事件仍然停在那儿
     *（目标被系统挂起），清掉就等于放 `Run` 进来。
     * ❗但回调错误与取消守卫**必须先问**（与拆分前同序）：`--cancel` 在空闲期插话就是靠这条
     *（`pump.cancel-while-idle` 的 `err=canceled by server`）。 */
    if (type == 0)
    {
        if (waiter->cb_error[0]) return take_cb_error(L, waiter);
        if (guard_exec_active() && guard_should_stop()) return fail(L, guard_stop_message());
        lua_pushnil(L);
        return 1;
    }

    SessionState *st = registry_find(ev.dwProcessId);
    if (!st) return fail_pid(L, kErrUnknownPid, ev.dwProcessId);

    /* 待应答槽（D57 的不变量 2）：一个会话最多一个未应答事件 —— OS 会挂起该进程的线程直到
     * `ContinueDebugEvent`。C2a 只保证「一个会话 = 一棵单进程树」，所以这第二个事件只可能来自
     * C2b 的进程树（那时才会发生）。❗分派**仍然要做**（事件已经从 OS 取走，不分派就等于 EIP--/
     * re-arm/记账/回调全丢），但**不覆盖** `d.stop`（覆盖会让先停住的进程永远挂起），而是诚实报错。 */
    bool pending = (st->stop_ref != LUA_NOREF);

    int before = st->seq_next;
    int r;
    {
        SessionScope scope(st);
        r = st->sess->dispatch(ev, autocont);
    }
    if (r < 0) return finish_engine_error(L, st, st->sess->last_error());
    fill_cont_code(st, before);   /* core 在 dispatch 末尾才算得出默认应答码 */

    /* 回调里出错 / 取消：与 T7.3 同序（先报回调错误，再问守卫） */
    if (st->cb_error[0]) return take_cb_error(L, st);
    if (guard_exec_active() && guard_should_stop()) return fail(L, guard_stop_message());
    if (pending) return fail(L, kErrPending);

    if (owner) *owner = st;

    if (autocont)
    {
        /* record-and-continue：引擎已经应答过，没有「停住」这回事 ⇒ 不缓存 */
        push_event_table(L, st, type, before);
        return 1;
    }

    clear_stop(st);                                   /* 旧表不再代表「当前」 */
    push_event_table(L, st, type, before);
    st->stop_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, LUA_REGISTRYINDEX, st->stop_ref);  /* 返回的就是缓存里那一张（同一个对象） */
    return 1;
}

/* `d:pump([timeout_ms [, autocont]])`（C1a 的「缓存优先」+ C2a 的「按会话过滤」）。
 * ① `d.stop` 非空 ⇒ 立即返回它（不阻塞、不调 core）；
 * ② 否则循环 `pump_once`，直到出现**属于本会话**的事件 —— 别人的事件已经在它自己的会话里分派完了
 *    （D57 的不变量 1：分派不能推迟），这里只是不把它返回给本会话的调用方；
 * ③ 超时（同一个预算用尽）⇒ `nil`。 */
static int do_pump(lua_State *L, SessionState *st, int base)
{
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

    if (st->stop_ref != LUA_NOREF)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, st->stop_ref);
        return 1;
    }

    int   top      = lua_gettop(L);
    DWORD timeout  = (top >= base && lua_isnumber(L, base)) ? (DWORD)lua_tointeger(L, base) : 100;
    bool  autocont = (top >= base + 1 && lua_toboolean(L, base + 1)) ? true : false;

    DWORD deadline = GetTickCount() + timeout;
    for (;;)
    {
        /* 同一个 timeout 预算：每轮只把**剩余**时间交给下一次等待（`GetTickCount` 回绕靠有符号差值） */
        LONG left = (LONG)(deadline - GetTickCount());
        if (left < 0) left = 0;

        SessionState *owner = nullptr;
        int           n     = pump_once(L, (DWORD)left, autocont, &owner);
        if (n == 2) return n;
        if (lua_isnil(L, -1)) return 1;        /* 预算用尽 / 没等到事件 */
        if (owner == st) return 1;             /* 自己的事件 */

        lua_pop(L, 1);                         /* 别人的事件：已分派，继续等自己的 */
    }
}

/* `d:cont([code])` / `dbg.cont([pid[, tid[, code]]])`。
 * `raw` = 模块级的旧形态（门禁与老脚本按位置传 pid/tid，行为一字不改）；对象方法只收 code，
 * 缺省值就是 `d.stop.cont_code`（core 在 pump 末尾把它存进会话，之后没变过）。 */
static int do_cont(lua_State *L, SessionState *st, int base, bool raw)
{
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

    int   top = lua_gettop(L);
    DWORD pid, tid, code;
    if (raw)
    {
        pid  = (top >= base && lua_isnumber(L, base)) ? (DWORD)lua_tointeger(L, base) : st->last_pid;
        tid  = (top >= base + 1 && lua_isnumber(L, base + 1))
                   ? (DWORD)lua_tointeger(L, base + 1)
                   : st->last_tid;
        code = (top >= base + 2 && lua_isnumber(L, base + 2))
                   ? (DWORD)lua_tointeger(L, base + 2)
                   : st->sess->last_code();
    }
    else
    {
        pid  = st->last_pid;
        tid  = st->last_tid;
        code = (top >= base && lua_isnumber(L, base)) ? (DWORD)lua_tointeger(L, base)
                                                     : st->sess->last_code();
    }

    bool ok;
    {
        SessionScope scope(st);
        ok = st->sess->cont(pid, tid, code);
    }
    if (ok) clear_stop(st);   /* 应答成功才清（D55 第 3 条） */
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static HANDLE step_thread(SessionState *st, DWORD tid)
{
    HANDLE h = st->sess->get_thread_by_tid(tid);
    if (!h) return nullptr;
    CONTEXT ctx;
    ZeroMem(ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!st->sess->get_context(h, CONTEXT_CONTROL, &ctx)) return nullptr;
    ctx.EFlags |= 0x100;      /* TF（§2.1 第 6 条） */
    if (!st->sess->set_context(h, CONTEXT_CONTROL, &ctx)) return nullptr;
    return h;
}

static int do_step(lua_State *L, SessionState *st, int base)
{
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

    int   top = lua_gettop(L);
    DWORD tid = (top >= base && lua_isnumber(L, base)) ? (DWORD)lua_tointeger(L, base) : st->last_tid;
    bool  ok;
    {
        SessionScope scope(st);
        ok = (step_thread(st, tid) != nullptr);
    }
    if (!ok) return fail(L, "dbg.step: thread not found or context unavailable");
    lua_pushboolean(L, 1);
    return 1;
}

static int do_eip(lua_State *L, SessionState *st, int base)
{
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

    int   top = lua_gettop(L);
    DWORD tid = (top >= base && lua_isnumber(L, base)) ? (DWORD)lua_tointeger(L, base) : st->last_tid;
    SessionScope scope(st);
    HANDLE h = st->sess->get_thread_by_tid(tid);
    if (!h) return fail(L, "dbg.eip: thread not found");
    CONTEXT ctx;
    ZeroMem(ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!st->sess->get_context(h, CONTEXT_CONTROL, &ctx)) return fail(L, "dbg.eip: GetThreadContext failed");
#ifdef _WIN64
    push_ea(L, (size_t)ctx.Rip);
#else
    push_ea(L, (size_t)ctx.Eip);
#endif
    return 1;
}

static int do_arm(lua_State *L, SessionState *st, int base)
{
    if (g_in_callback)   return reentry(L);
    if (!st->started)    return fail(L, kErrNoSess);
    if (!lua_isstring(L, base))   return fail(L, "dbg.arm: modname (string) expected");
    if (!lua_isnumber(L, base + 1)) return fail(L, "dbg.arm: rva (number) expected");   /* 普通数字，不经 LuaVariant（§2.1 第 3 条） */
    if (!lua_isfunction(L, base + 2)) return fail(L, "dbg.arm: callback expected");

    /* ❗D57：允许**延迟武装** —— 模块没加载时 core 的 `set_breakpoint` 只登记节点（`has_orignal_byte = 0`），
     * `CREATE_PROCESS` / `LOAD_DLL` 时由 `apply_breakpoints_for_module` 补 `0xCC`。
     * T7.3 的「base == 0 就失败」已删：`create -> arm -> pump` 是最自然的顺序。 */
    LPXSTR modname = utf8_to_tchar(lua_tostring(L, base));   /* UNICODE 下是分配来的；ANSI 下宏就是原样返回 */

    lua_pushvalue(L, base + 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* 令牌由 core 在断点销毁时经 `on_breakpoint_destroyed` 还回来 */

    callback_ref_t tok;
    tok.token.i = (intptr_t)ref;
    tok.valid   = 1;

    {
        SessionScope scope(st);
        st->sess->set_breakpoint(modname, (DWORD)lua_tointeger(L, base + 1), false, tok);
    }
#ifdef _UNICODE
    release(modname);
#endif
    lua_pushboolean(L, 1);
    return 1;
}

static int do_hits(lua_State *L, SessionState *st)
{
    lua_createtable(L, st->count, 0);
    push_hit_table(L, st);
    return 1;
}

static int do_dropped(lua_State *L, SessionState *st)
{
    lua_pushinteger(L, (lua_Integer)st->dropped);
    return 1;
}

static int do_setcap(lua_State *L, SessionState *st, int base)
{
    int n = (lua_isnumber(L, base)) ? (int)lua_tointeger(L, base) : st->cap;
    if (n < 1) n = 1;
    if (n > kHitCapMax) n = kHitCapMax;
    if (n < st->count)
    {
        st->dropped += st->count - n;                       /* 收窄时丢的是最旧那几条 */
        st->head = (st->head + (st->count - n)) % kHitCapMax;
        st->count = n;
    }
    st->cap = n;
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int do_stopped(lua_State *L, SessionState *st)
{
    if (!st->started) return fail(L, kErrNoSess);
    lua_pushboolean(L, st->sess->cont() ? 0 : 1);   /* 「这一轮该停」= run 的 while 出口（§2.1 记录 ②） */
    return 1;
}

static int do_paused(lua_State *L, SessionState *st)
{
    lua_pushboolean(L, st->stop_ref != LUA_NOREF ? 1 : 0);
    return 1;
}

static int do_caps(lua_State *L, SessionState *st)
{
    (void)st;   /* 能力是**这台机器 / 这个 build** 的属性，与会话无关（D53） */
    lua_createtable(L, 0, 7);
    lua_pushstring(L, autodbg::common::arch_name());        lua_setfield(L, -2, "arch");
    lua_pushstring(L, autodbg::common::variant_name());     lua_setfield(L, -2, "variant");
    lua_pushstring(L, autodbg::common::flavor_name());      lua_setfield(L, -2, "flavor");
    lua_pushboolean(L, autodbg::common::is_win9x() ? 1 : 0); lua_setfield(L, -2, "win9x");
    /* `attach` 在 9x 上不可用（plan.md §1.3 case 30）⇒ 能力上报，不是许可 */
    lua_pushboolean(L, autodbg::common::is_nt() ? 1 : 0);   lua_setfield(L, -2, "attach");
    lua_pushboolean(L, g_has_detach ? 1 : 0);               lua_setfield(L, -2, "detach");
    lua_pushinteger(L, (lua_Integer)kHitCapMax);            lua_setfield(L, -2, "hits_cap");
    return 1;
}

/* `help()` 的文本表只有一份（help_bindings.cpp）；这里只转发，没有第二真相源。 */
static int do_help(lua_State *L, SessionState *st)
{
    (void)st;
    if (!help_print_topic("dbg")) return fail(L, "dbg.help: no help topic");
    lua_pushboolean(L, 1);
    return 1;
}

/* 会话收尾的**宿主侧**那一半（core 的 `end()` 管引擎侧）：释放回调 ref、释放 TCHAR 串、
 * 丢掉缓存的事件表、从 registry 摘掉 —— 之后可以再 `create` / `attach`。 */
static void finish_session(SessionState *st)
{
    unref(st->L, st->ref_entry);
    unref(st->L, st->ref_exception_cb);
    unref(st->L, st->ref_on_exception);
    unref(st->L, st->ref_on_breakpoint);
    unref(st->L, st->ref_on_single_step);
    unref(st->L, st->ref_on_exit_process);
    st->ref_entry           = LUA_NOREF;
    st->ref_exception_cb    = LUA_NOREF;
    st->ref_on_exception    = LUA_NOREF;
    st->ref_on_breakpoint   = LUA_NOREF;
    st->ref_on_single_step  = LUA_NOREF;
    st->ref_on_exit_process = LUA_NOREF;

    free_strings(st);
    clear_stop(st);
    st->started     = false;
    st->cb_error[0] = 0;
    st->params.pid  = 0;
    registry_remove(st);
}

/* 会话收尾的**引擎侧动作**（`d:detach()` / `d:kill()` 与 REPL 的收尾报告共用同一份，T7 D2b）：
 *   · `end_session_by_detach`：恢复断点原字节 + `DebugActiveProcessStop`，**目标继续活着**（D51）。
 *     返回 false = 平台没有 `DebugActiveProcessStop`（detach 降级成 kill，会话**同样**已经关掉）。
 *   · `end_session_by_kill`：终止目标（先恢复原字节 + 通知销毁，顺带把断点回调的 ref 还回来）。
 * 两者都会 `finish_session`（释放 ref / 从 registry 摘掉）—— 调用方只管检查返回值。 */
static bool end_session_by_detach(SessionState *st, const char **reason)
{
    int r;
    {
        SessionScope scope(st);
        /* ❗未应答的事件必须先放行：否则 `DebugActiveProcessStop` 之后目标停在被系统挂起的
         * 线程上（DBG_CONTINUE = 这个断点「已处理」，然后我们撒手 —— 此处唯一合理的选择）。 */
        if (st->stop_ref != LUA_NOREF)
        {
            st->sess->cont(st->last_pid, st->last_tid, DBG_CONTINUE);
            clear_stop(st);
        }
        st->sess->to_detach() = true;   /* core 的 `end()` 做实事：恢复原字节 + 通知销毁 + 分离 */
        const char *err = nullptr;
        r = st->sess->end(&err);
        (void)err;
    }
    finish_session(st);

    if (r != 0)
    {
        if (reason != nullptr) *reason = "detach not supported on this platform";
        return false;
    }
    return true;
}

static void end_session_by_kill(SessionState *st)
{
    {
        SessionScope scope(st);
        st->sess->detach_process(&st->cb); /* `end()` 在 to_detach 为假时不做这一步，所以这里显式做 */
        st->sess->to_detach() = false;
        st->sess->term()      = true;      /* `run_shutdown` 会 TerminateProcess / TerminateThread */
        const char *err = nullptr;
        st->sess->end(&err);               /* 返回值只有「detach 降级」一种含义 ⇒ kill 路径必然 0 */
        (void)err;
    }
    finish_session(st);
}

/* `d:detach()`：恢复断点原字节 + `DebugActiveProcessStop`，**目标继续活着**（D51）。
 * ❗只有平台有这个导出时才会注册这个 key（`register_dbg_api` 里按能力装）。 */
static int do_detach(lua_State *L, SessionState *st, int base)
{
    (void)base;
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

    const char *reason = nullptr;
    if (!end_session_by_detach(st, &reason)) return fail(L, reason);
    lua_pushboolean(L, 1);
    return 1;
}

/* `d:kill()`：终止目标（先恢复原字节 + 通知销毁，顺带把断点回调的 ref 还回来）。 */
static int do_kill(lua_State *L, SessionState *st, int base)
{
    (void)base;
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

    end_session_by_kill(st);
    lua_pushboolean(L, 1);
    return 1;
}

/* ---------------- registry 查询（D54：`dbg.sessions/session/last`） ---------------- */

static int dbg_sessions(lua_State *L)
{
    int n = 0;
    for (SessionState *st = g_sessions; st; st = st->next) n++;
    lua_createtable(L, n, 0);
    int i = 0;
    for (SessionState *st = g_sessions; st; st = st->next)
    {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, (lua_Integer)st->root_pid);        lua_setfield(L, -2, "root_pid");
        lua_pushstring(L, kind_name(st->kind));               lua_setfield(L, -2, "kind");
        lua_pushstring(L, state_name(st));                    lua_setfield(L, -2, "state");
        lua_pushboolean(L, st->lua_bound ? 1 : 0);            lua_setfield(L, -2, "lua_bound");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

static int dbg_session(lua_State *L)
{
    if (!lua_isnumber(L, 1)) return fail(L, "dbg.session: pid (number) expected");
    SessionState *st = registry_find((DWORD)lua_tointeger(L, 1));
    if (!st) lua_pushnil(L);
    else     push_handle(L, st);
    return 1;
}

static int dbg_last(lua_State *L)
{
    if (!g_default) lua_pushnil(L);
    else            push_handle(L, g_default);
    return 1;
}

/* ---------------- 模块级转发（默认会话；错误文案逐字节不变） ---------------- */

/* 模块级 `dbg.pump([timeout_ms [, autocont]])` = **唯一的「等」，而且是进程级的**（C2a / D57）：
 * 读一条事件 → 按归属分派 → 返回事件表（带 `session` 字段）。没有会话 ⇒ `nil, kErrNoSess`。 */
static int dbg_pump(lua_State *L)
{
    int   top      = lua_gettop(L);
    DWORD timeout  = (top >= 1 && lua_isnumber(L, 1)) ? (DWORD)lua_tointeger(L, 1) : 100;
    bool  autocont = (top >= 2 && lua_toboolean(L, 2)) ? true : false;
    return pump_once(L, timeout, autocont, nullptr);
}

static int dbg_cont(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_cont(L, g_default, 1, true);    /* 旧形态：cont([pid[, tid[, code]]]) */
}

static int dbg_step(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_step(L, g_default, 1);
}

static int dbg_eip(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_eip(L, g_default, 1);
}

static int dbg_arm(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_arm(L, g_default, 1);
}

static int dbg_hits(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_hits(L, g_default);
}

static int dbg_dropped(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_dropped(L, g_default);
}

static int dbg_setcap(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_setcap(L, g_default, 1);
}

static int dbg_stopped(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_stopped(L, g_default);
}

/* 没有会话 ⇒ 也没有未应答事件（T7.3 的口径：paused 从不报错）。
 * C2a：看**任一**会话 —— 「有事件停着」是多会话下的进程级事实。 */
static int dbg_paused_method(lua_State *L)
{
    lua_pushboolean(L, dbg_paused() ? 1 : 0);
    return 1;
}

static int dbg_caps(lua_State *L) { return do_caps(L, g_default); }
static int dbg_help(lua_State *L) { return do_help(L, g_default); }

/* 模块级收尾的目标：显式 `pid`（按 root_pid 找）优先，否则默认会话。
 * `dbg.detach(pid)` / `dbg.kill(pid)` 与 `d:detach()` / `d:kill()` 同义（C2a），
 * 多会话时代就是「按 pid 指名收尾哪一个」。 */
static bool pick_finish_target(lua_State *L, const char *fn, SessionState **out)
{
    *out = nullptr;
    int t = lua_type(L, 1);
    if (t == LUA_TNUMBER)
    {
        SessionState *st = registry_find((DWORD)lua_tointeger(L, 1));
        if (!st)
        {
            fail_two(L, fn, ": no session with that pid");
            return false;
        }
        *out = st;
        return true;
    }
    if (t != LUA_TNONE && t != LUA_TNIL)
    {
        fail_two(L, fn, ": pid (number) expected");
        return false;
    }
    if (!g_default)
    {
        fail(L, kErrNoSess);
        return false;
    }
    *out = g_default;
    return true;
}

static int dbg_detach(lua_State *L)
{
    SessionState *st;
    if (!pick_finish_target(L, "dbg.detach", &st)) return 2;
    return do_detach(L, st, 1);
}

static int dbg_kill(lua_State *L)
{
    SessionState *st;
    if (!pick_finish_target(L, "dbg.kill", &st)) return 2;
    return do_kill(L, st, 1);
}

/* ---------------- 会话方法（self 在栈 1，第一个实参在栈 2） ---------------- */

static int session_pump(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_pump(L, st, 2);
}

static int session_cont(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_cont(L, st, 2, false);
}

static int session_step(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_step(L, st, 2);
}

static int session_eip(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_eip(L, st, 2);
}

static int session_arm(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_arm(L, st, 2);
}

static int session_hits(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_hits(L, st);
}

static int session_dropped(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_dropped(L, st);
}

static int session_setcap(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_setcap(L, st, 2);
}

static int session_stopped(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_stopped(L, st);
}

static int session_paused(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_paused(L, st);
}

static int session_caps(lua_State *L) { return do_caps(L, self(L)); }
static int session_help(lua_State *L) { return do_help(L, self(L)); }

/* 数据面（C1b）：全部是 `d:` 调用，没有模块级转发（它们天然属于某一个会话） */
static int session_read(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_read(L, st, 2);
}

static int session_read_u8(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_read_u8(L, st, 2);
}

static int session_read_u16(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_read_u16(L, st, 2);
}

static int session_read_u32(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_read_u32(L, st, 2);
}

static int session_read_u64(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_read_u64(L, st, 2);
}

static int session_read_str(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_read_str(L, st, 2);
}

static int session_write(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_write(L, st, 2);
}

static int session_context(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_context(L, st, 2);
}

static int session_stack(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_stack(L, st, 2);
}

static int session_mods(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_mods(L, st);
}

static int session_threads(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_threads(L, st);
}

static int session_detach(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_detach(L, st, 2);
}

static int session_kill(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_kill(L, st, 2);
}

/* 每个执行建一份的方法表（`__index` 落到它上面；`detach` 按平台能力装） */
static void push_methods(lua_State *L, bool has_detach)
{
    lua_createtable(L, 0, 24);
    lua_pushcfunction(L, session_pump);    lua_setfield(L, -2, "pump");
    lua_pushcfunction(L, session_cont);    lua_setfield(L, -2, "cont");
    lua_pushcfunction(L, session_step);    lua_setfield(L, -2, "step");
    lua_pushcfunction(L, session_eip);     lua_setfield(L, -2, "eip");
    lua_pushcfunction(L, session_arm);     lua_setfield(L, -2, "arm");
    lua_pushcfunction(L, session_hits);    lua_setfield(L, -2, "hits");
    lua_pushcfunction(L, session_dropped); lua_setfield(L, -2, "dropped");
    lua_pushcfunction(L, session_setcap);  lua_setfield(L, -2, "setcap");
    lua_pushcfunction(L, session_stopped); lua_setfield(L, -2, "stopped");
    lua_pushcfunction(L, session_paused);  lua_setfield(L, -2, "paused");
    lua_pushcfunction(L, session_caps);    lua_setfield(L, -2, "caps");
    lua_pushcfunction(L, session_help);    lua_setfield(L, -2, "help");
    /* 数据面（C1b） */
    lua_pushcfunction(L, session_read);     lua_setfield(L, -2, "read");
    lua_pushcfunction(L, session_read_u8);  lua_setfield(L, -2, "read_u8");
    lua_pushcfunction(L, session_read_u16); lua_setfield(L, -2, "read_u16");
    lua_pushcfunction(L, session_read_u32); lua_setfield(L, -2, "read_u32");
    lua_pushcfunction(L, session_read_u64); lua_setfield(L, -2, "read_u64");
    lua_pushcfunction(L, session_read_str); lua_setfield(L, -2, "read_str");
    lua_pushcfunction(L, session_write);    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, session_context);  lua_setfield(L, -2, "context");
    lua_pushcfunction(L, session_stack);    lua_setfield(L, -2, "stack");
    lua_pushcfunction(L, session_mods);     lua_setfield(L, -2, "mods");
    lua_pushcfunction(L, session_threads);  lua_setfield(L, -2, "threads");
    if (has_detach) { lua_pushcfunction(L, session_detach); lua_setfield(L, -2, "detach"); }
    lua_pushcfunction(L, session_kill);    lua_setfield(L, -2, "kill");
}

/* 状态 → 活着的 userdata 的**弱值**表（见 `push_handle`） */
static void push_handle_table(lua_State *L)
{
    lua_newtable(L);
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
}

} /* anonymous namespace */

const char *dbg_error_reentry() noexcept { return kErrReentry; }
const char *dbg_error_paused()  noexcept { return kErrPaused; }
const char *dbg_error_busy()    noexcept { return kErrBusy; }

/* 任一会话停着一个未应答事件（C2a：不再只看默认会话）。 */
bool dbg_paused() noexcept
{
    for (SessionState *st = g_sessions; st; st = st->next)
    {
        if (st->stop_ref != LUA_NOREF) return true;
    }
    return false;
}

/* registry 非空 = 存在 dbg 会话（`Run`/`Attach` 的拒给判据）。 */
bool dbg_busy() noexcept
{
    return g_sessions != nullptr;
}

/* registry 里还开着的会话数（REPL 退出报告用，T7 切片 D2b）。 */
int dbg_open_session_count() noexcept
{
    int n = 0;
    for (SessionState *st = g_sessions; st; st = st->next) n++;
    return n;
}

/* REPL 退出前的安全网（D54 / T7 D2b）：registry 里**所有**还开着的会话逐个收尾。
 * 有 `DebugActiveProcessStop` 就 detach（目标继续活着），没有（Win95/98）就退 kill
 * —— 与 `--detach` 的降级口径一致。单个失败只打一行 stderr、继续收下一个。
 * ❗调用点必须在 `lua_close` **之前**：会话的 callback ref / 事件表 ref 都挂在那份 state 上。 */
int dbg_release_unclosed_sessions()
{
    int n = 0;

    /* ❗每收尾一个就把它从 registry 摘掉（`finish_session`），所以每次都从表头取。 */
    while (g_sessions != nullptr)
    {
        SessionState *st  = g_sessions;
        DWORD         pid = st->root_pid;
        n++;

        const char *reason = nullptr;
        bool        ok;
        if (g_has_detach)
        {
            ok = end_session_by_detach(st, &reason);
        }
        else
        {
            end_session_by_kill(st);   /* 9x：能力缺席 ⇒ 退 kill（与 CLI 的 `--detach` 同一降级） */
            ok = true;
        }

        if (!ok)
        {
            autodbg::core::ErrorS("repl: session ");
            autodbg::core::ErrorF(TEXT("%lu"), (unsigned long)pid);
            autodbg::core::ErrorS(" detach failed: ");
            autodbg::core::ErrorS((reason != nullptr) ? reason : "unknown");
            autodbg::core::ErrorS("\n");
        }
    }
    return n;
}

/* =============================================================================
 * 远端会话代理的传输原语（T7 切片 C-remote，plan_t7.md §10.1 / D56）
 *
 * 分工：**对象面与 JSON 编解码在 `lualib/dbglib.lua`**（hex、表遍历、`jsonx` 都是 Lua 的强项，
 *   D28 也要求 C 侧**不解析** JSON）；**连接与帧必须复用 `--remote` 客户端的实现**
 *   （`client.cpp` 的 `remote_connect` / `remote_run_script` / `remote_close`）—— 不写第二套 TCP。
 *   于是 C 侧只有三个**内部管道**（脚本用的面是 `dbg.remote(addr[, opts])`）：
 *     dbg.remote_open(host, port[, token[, read_timeout_ms[, max_frame]]]) -> conn | nil, err
 *     dbg.remote_xfer(conn, src[, timeout_ms]) -> `result` 帧的 json 原文 | nil, err
 *     dbg.remote_close(conn) -> true
 *   `dbg.remote_xfer` 把 `result` 帧的 json **原文**交回 Lua（`jsonx.decode` 在那边）：
 *   `dbg.remote_call` 的回复就是这个帧的 `json.value`（协议面唯一的增量，见 `tcp_server.cpp`）。
 *
 * `conn` 是 full userdata（metatable `AutoDbgRemoteConn`）+ `__gc` 关 socket：不显式 `close`
 *   也不会漏。❗它是**纯本地资源**（不是调试会话）⇒ 不受「`__gc` 不做副作用」（D54）约束；
 *   但显式 `close` 仍是推荐用法（远端会话可能还停着事件，先 detach/kill 再 close）。
 * ========================================================================== */
constexpr const char *kRemoteConnMeta = "AutoDbgRemoteConn";

struct RemoteConnBox
{
    autodbg::host::remote_conn *c;
};

static RemoteConnBox *check_remote_conn(lua_State *L, int idx)
{
    return (RemoteConnBox *)luaL_checkudata(L, idx, kRemoteConnMeta);
}

static int remote_conn_gc(lua_State *L)
{
    RemoteConnBox *b = check_remote_conn(L, 1);
    if (b->c != nullptr)
    {
        autodbg::host::remote_close(b->c);
        b->c = nullptr;
    }
    return 0;
}

/* `dbg.remote_open(host, port[, token[, read_timeout_ms[, max_frame]]])` */
static int dbg_remote_open(lua_State *L)
{
    if (!lua_isstring(L, 1)) return fail(L, "dbg.remote_open: host (string) expected");
    if (!lua_isnumber(L, 2)) return fail(L, "dbg.remote_open: port (number) expected");

    const char *host  = lua_tostring(L, 1);
    unsigned    port  = (unsigned)lua_tointeger(L, 2);
    const char *token = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr;
    unsigned    rtmo  = lua_isnumber(L, 4) ? (unsigned)lua_tointeger(L, 4) : 0;
    unsigned    mfrm  = lua_isnumber(L, 5) ? (unsigned)lua_tointeger(L, 5) : 0;

    char err[192];
    err[0] = '\0';
    autodbg::host::remote_conn *c = autodbg::host::remote_connect(host, port, token, rtmo, mfrm, err, sizeof err);
    if (c == nullptr)
    {
        lua_pushnil(L);
        lua_pushstring(L, (err[0] != '\0') ? err : "connect failed");
        return 2;
    }

    RemoteConnBox *b = (RemoteConnBox *)lua_newuserdata(L, sizeof(RemoteConnBox));
    b->c = c;
    luaL_getmetatable(L, kRemoteConnMeta);
    lua_setmetatable(L, -2);
    return 1;
}

/* `dbg.remote_xfer(conn, src[, timeout_ms])` → `result` 帧的 json 原文
 * ❗`src` 是一条 **Lua 语句**（通常是 `return dbglib.remote_call(...)`）——远端是 `--repl --server`，
 *   一帧 = 一条命令 / 一个完整块；`timeout_ms` = 整次调用的上限（0 = 不设）。 */
static int dbg_remote_xfer(lua_State *L)
{
    RemoteConnBox *b = check_remote_conn(L, 1);
    if (b->c == nullptr) return fail(L, "dbg.remote_xfer: connection closed");
    if (!lua_isstring(L, 2)) return fail(L, "dbg.remote_xfer: src (string) expected");

    size_t      len = 0;
    const char *src = lua_tolstring(L, 2, &len);
    unsigned    tmo = lua_isnumber(L, 3) ? (unsigned)lua_tointeger(L, 3) : 0;

    char        err[192];
    const char *rj = autodbg::host::remote_run_script(b->c, src, len, "remote", tmo, err, sizeof err);
    if (rj == nullptr)
    {
        lua_pushnil(L);
        lua_pushstring(L, (err[0] != '\0') ? err : "remote call failed");
        return 2;
    }

    lua_pushstring(L, rj);
    autodbg::common::release((void *)rj);   /* `remote_run_script` 交回的是 alloc 缓冲 */
    return 1;
}

/* `dbg.remote_close(conn)` */
static int dbg_remote_close(lua_State *L)
{
    RemoteConnBox *b = check_remote_conn(L, 1);
    if (b->c != nullptr)
    {
        autodbg::host::remote_close(b->c);
        b->c = nullptr;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* `dbg.remote(addr[, opts])` → RemoteHost / RemoteSession（实现全在 `lualib/dbglib.lua`）。
 * 这里只是**转发**：`dbglib` 在**调用时**取（PITFALLS.md L4 —— 库按文件名烘焙，dbglib 先于 jsonx）。
 * 这么做的理由：代理要做的事（参数序列化、`__hex`、表遍历、`jsonx.decode`）全是 Lua 的强项，
 * 而 D28 明确要求 C 侧不碰 JSON。
 * ❗失败一律**当值**返回 `nil, err`（不向调用方抛）—— `pcall` 包住；走到 error 分支就是
 *   `dbglib.remote` 的 bug，照样当值报。 */
static int dbg_remote(lua_State *L)
{
    int nargs = lua_gettop(L);

    lua_getglobal(L, "dbglib");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return fail(L, "dbg.remote: dbglib is not loaded");
    }
    lua_getfield(L, -1, "remote");
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 2);
        return fail(L, "dbg.remote: dbglib.remote is not a function");
    }
    lua_remove(L, -2);    /* 去掉 dbglib 表，只留函数 */
    lua_insert(L, 1);     /* 函数挪到参数之前 */
    if (lua_pcall(L, nargs, LUA_MULTRET, 0) != 0)
    {
        lua_pushnil(L);   /* [err] -> [err, nil] -> [nil, err] */
        lua_insert(L, -2);
        return 2;
    }
    return lua_gettop(L);
}

void register_dbg_api(lua_State *L)
{
    /* 宿主会话先记下来（可能还是空的，见 `g_host_sess` 的注释），再清一次执行的 registry ——
     * 否则 `registry_clear` 里释放掉的会话还可能被当成「当前会话」。 */
    g_host_sess   = core::current_session_or_null();
    g_in_callback = false;
    /* 一次执行一个 registry（D43/D54）：上一条执行留下的 C 侧状态在这里统一释放 */
    registry_clear();
    g_has_detach  = (resolve_detach() != nullptr);   /* D51：平台能力，建表时定一次 */

    /* 远端代理的连接对象（D56）：`__gc` 关 socket —— 它是**纯本地资源**（不是调试会话），
     * 所以不受「`__gc` 不做副作用」那条（D54）的约束：不 close 就该漏，`__gc` 必须收尾。 */
    luaL_newmetatable(L, kRemoteConnMeta);
    lua_pushcfunction(L, remote_conn_gc); lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    push_methods(L, g_has_detach);
    lua_setfield(L, LUA_REGISTRYINDEX, kSessionMethods);

    push_handle_table(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kSessionHandles);

    /* 会话对象的元表：`__index` 先认动态字段再落到方法表；`__gc` **只解绑**（D54） */
    luaL_newmetatable(L, kSessionMeta);
    lua_pushcfunction(L, session_index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, session_gc);    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_createtable(L, 0, 26);
    lua_pushcfunction(L, dbg_create);   lua_setfield(L, -2, "create");   /* = Run 的原语版（CreateProcess） */
    lua_pushcfunction(L, dbg_attach);   lua_setfield(L, -2, "attach");   /* = Attach 的原语版（DebugActiveProcess；Win95/98 没有） */
    lua_pushcfunction(L, dbg_pump);     lua_setfield(L, -2, "pump");
    lua_pushcfunction(L, dbg_cont);     lua_setfield(L, -2, "cont");
    /* ❗D51：没有 `close`。收尾是两个语义不同的动词，且 `detach` 在没有
     * `DebugActiveProcessStop` 的平台上**根本不存在**（下面按能力装） */
    if (g_has_detach)
    {
        lua_pushcfunction(L, dbg_detach); lua_setfield(L, -2, "detach");
    }
    lua_pushcfunction(L, dbg_kill);    lua_setfield(L, -2, "kill");
    lua_pushcfunction(L, dbg_caps);    lua_setfield(L, -2, "caps");     /* 机器可读：这台机器有什么 */
    lua_pushcfunction(L, dbg_help);    lua_setfield(L, -2, "help");     /* = help('dbg') */
    lua_pushcfunction(L, dbg_step);    lua_setfield(L, -2, "step");
    lua_pushcfunction(L, dbg_eip);     lua_setfield(L, -2, "eip");
    lua_pushcfunction(L, dbg_arm);     lua_setfield(L, -2, "arm");
    lua_pushcfunction(L, dbg_hits);    lua_setfield(L, -2, "hits");
    lua_pushcfunction(L, dbg_dropped); lua_setfield(L, -2, "dropped");
    lua_pushcfunction(L, dbg_setcap);  lua_setfield(L, -2, "setcap");
    lua_pushcfunction(L, dbg_sessions); lua_setfield(L, -2, "sessions"); /* D54：registry 里的会话（跨 d=nil 存活） */
    lua_pushcfunction(L, dbg_session);  lua_setfield(L, -2, "session");  /* 取回同一个 userdata */
    lua_pushcfunction(L, dbg_last);     lua_setfield(L, -2, "last");     /* 最近一次 create/attach 的那个 */
    lua_pushcfunction(L, dbg_stopped); lua_setfield(L, -2, "stopped");
    lua_pushcfunction(L, dbg_paused_method); lua_setfield(L, -2, "paused");
    /* T7 切片 C-remote（D56）：代理的对外面只有 `dbg.remote`（实现在 `lualib/dbglib.lua`），
     * 其余三个是它用的**传输原语**（不写第二套 TCP：都落到 `client.cpp`）。 */
    lua_pushcfunction(L, dbg_remote);       lua_setfield(L, -2, "remote");
    lua_pushcfunction(L, dbg_remote_open);  lua_setfield(L, -2, "remote_open");
    lua_pushcfunction(L, dbg_remote_xfer);  lua_setfield(L, -2, "remote_xfer");
    lua_pushcfunction(L, dbg_remote_close); lua_setfield(L, -2, "remote_close");
    lua_setglobal(L, "dbg");
}

} /* namespace autodbg::host */
