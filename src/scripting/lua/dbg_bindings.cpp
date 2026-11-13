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
 * pump 的两层（D57，C1a 已按此落地，但只有一个会话可路由）：
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
 *   · 一次执行只允许一个会话：第二次 `create`/`attach` → `nil, kErrOneSess`（多会话与进程树路由是 C2）。
 *
 * 命中记录缓冲在**宿主层**（core 不认识队列）：环形、满则丢最旧、`dropped()` 可数、`setcap(1..64)` 可调。
 *   记录只在四个事件通道产生（`on_breakpoint` / `on_exception` / `on_user_exception` / `on_single_step`）；
 *   断点**自带**的回调（`on_breakpoint_callback`）只转交用户，不另记 —— 否则同一个命中会被记两次。
 *   例外：断点的 `on_exception` 通道不记（同一命中已经由 `on_breakpoint` 记过），但仍会调用户回调。
 *   `cont_code` 只能等 core 在 `pump` 末尾算完才知道 ⇒ 泵完再把「这一轮新产生的记录」补上。
 *
 * 回调：core 的回调签名带 `void *ctx`，这里传的是 **`SessionState*`**（不是 `lua_State*`）——
 *   每会话一份，C2 的进程树路由直接可用；`lua_State` 从 `st->L` 取。回调里出错**不**长跳穿 C++ 帧
 *   （D18）：`lua_pcall` 兜住，消息存起来，下一次 `pump` 把它作为第二返回值。
 *   回调里调状态机（pump/cont/step/arm/detach/kill/create/attach）→ `nil, "pump cannot be called from
 *   a callback"`（固定文案）；`dbg` 停着未应答事件时调 `Run`/`Attach` → `"cannot run while an event is
 *   unhandled"`（`dbg_paused()` 查默认会话的 `d.stop`）。
 *
 * `current_session()` 的一致性：会话方法在调 core 前用 `SessionScope` 把 `st->sess` 挂成当前会话、
 *   返回后还原。C1a 里两者本来就是同一份（宿主 `main.cpp`/`exec.cpp` 挂给 core 的那个），交换是恒等；
 *   写成保存/还原是为了 C2 的多会话（那时每个 session 自带引擎会话）。core 调用点直接用 `st->sess->`
 *   （成员函数，作用对象明确是 this），只有仍按「当前会话」工作的内部路径才依赖 `current_session()`。
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
#include "common/platform.hpp"    /* is_win9x / arch_name / variant_name / flavor_name（caps） */
#include "core/core.hpp"
#include "core/types.hpp"         /* RunParams / DebugCallbacks / 事件载荷 / callback_ref_t */
#include "core/util.hpp"          /* TCHAR 宏、tchar 转换、ZeroMem */

#include "exec_guard.hpp"         /* D32：pump 空闲片里的那一个 poll 点 */
#include "help_bindings.hpp"      /* `dbg.help()` = `help('dbg')`（同一张文本表，不是第二真相源） */

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
/* 一次执行只允许一个会话（C1a）。多会话与进程树路由是 C2（plan_t7.md §10 / D56）。 */
const char *const kErrOneSess = "dbg: only one session per execution (multi-session lands in C2)";

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
    SessionState *next;      /* 侵入式链表首成员（registry 用；C1a 至多一个节点） */

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

    core::DebuggerSession *sess;   /* 引擎会话（宿主挂给 core 的那一份；C1a 只有一个） */
};

/* C 侧 registry（D54）：作用域 = **一次执行**。`register_dbg_api`（每次建 Lua 状态）清空，
 * `create`/`attach` 加、`detach`/`kill` 删。容器是侵入式单链表线性扫 —— 会话数是个位数量级。
 * ❗不是静态对象而是裸指针：手工映射的模块里别给静态对象找动态初始化的麻烦（§11.5）。 */
SessionState *g_sessions = nullptr;

/* 默认会话 = 最近一次 `create`/`attach` 的那个：29 个契约名与 `dbg.*` 的转发都作用在它上面。
 * 收尾（detach/kill）后置空 —— C1a 没有「上一个会话」可回退。 */
SessionState *g_default = nullptr;

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

/* 释放一个尚未进 registry（或 registry 已经把它摘掉）的状态。 */
static void release_state(SessionState *st)
{
    free_strings(st);
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
    st->sess     = &core::current_session();
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

/* ---------------- 命中窗口 ---------------- */

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
        lua_pushinteger(L, (lua_Integer)r.ea);           lua_setfield(L, -2, "ea");
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
 * `type` 是 `DEBUG_EVENT` 码，`code` 是异常码（没有异常的事件 = 0），`cont_code` 是默认应答码。 */
static void push_event_table(lua_State *L, SessionState *st, int type, int from_seq)
{
    const HitRecord *hit = newest_hit(st, from_seq);
    lua_createtable(L, 0, 9);
    lua_pushinteger(L, (lua_Integer)type);                            lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)st->last_pid);                    lua_setfield(L, -2, "pid");
    lua_pushinteger(L, (lua_Integer)st->last_tid);                    lua_setfield(L, -2, "tid");
    lua_pushinteger(L, (lua_Integer)(hit ? hit->ea : 0));             lua_setfield(L, -2, "ea");
    lua_pushstring(L, hit ? hit->modname : "");                       lua_setfield(L, -2, "modname");
    lua_pushinteger(L, (lua_Integer)(hit ? hit->rva : 0));            lua_setfield(L, -2, "rva");
    lua_pushinteger(L, (lua_Integer)(hit ? hit->code : 0));           lua_setfield(L, -2, "code");
    if (hit == nullptr || hit->first_chance < 0) lua_pushnil(L);
    else                                         lua_pushinteger(L, hit->first_chance ? 1 : 0);
    lua_setfield(L, -2, "first_chance");
    lua_pushinteger(L, (lua_Integer)st->sess->last_code());           lua_setfield(L, -2, "cont_code");
}

/* 用户回调的参数形状（§2.1 第 3 条：新接口一律普通数字/字符串，不再用 LuaVariant）：
 *   { pid, tid, ea, code, modname, rva } —— 与 `hits()` 的记录同名同义（AI 侧只学一套字段）。
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
    lua_pushinteger(L, (lua_Integer)ea);   lua_setfield(L, -2, "ea");
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

/* 会话方法调 core 前把**本会话的引擎会话**挂成「当前会话」，返回后还原（§11.4 的接缝 / D55）。
 * C1a 只有一个会话、且它就是宿主挂给 core 的那一份 ⇒ 交换是恒等；写成保存/还原是为了 C2 的多会话
 * （那时每个 session 自带 `DebuggerSession`，`current_session()` 必须对准本会话）。
 * ❗不是 thread_local：手工映射的模块禁用 TLS（§11.5），这里只是栈上的 RAII。 */
class SessionScope
{
public:
    explicit SessionScope(SessionState *st) : m_prev(&core::current_session())
    {
        core::set_current_session(st->sess);
    }
    ~SessionScope() { core::set_current_session(m_prev); }

    SessionScope(const SessionScope &)            = delete;
    SessionScope &operator=(const SessionScope &) = delete;

private:
    core::DebuggerSession *m_prev;
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

/* ---------------- worker（模块级与对象方法共用；base = 第一个真实参数的下标） ---------------- */

static int do_pump(lua_State *L, SessionState *st, int base, bool cache_first);

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
    return 1;
}

/* `dbg.create{ imagepath=…, cmdline=…, cwd=…, is_new_console=…, use_scylla_hide=…, <六个回调> }`
 * = `Run` 的原语版：`CreateProcess` + `DEBUG_PROCESS`，**不循环**；成功返回会话对象。 */
static int dbg_create(lua_State *L)
{
    if (g_in_callback)  return reentry(L);
    if (g_sessions)     return fail(L, kErrOneSess);
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
    if (g_sessions)    return fail(L, kErrOneSess);
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

/* `pump`：`cache_first` = 对象方法（先看 `d.stop`），否则模块级（唯一的「等」）。 */
static int do_pump(lua_State *L, SessionState *st, int base, bool cache_first)
{
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

    /* D57：缓存优先 —— 停着一个未应答事件就立即返回它（不阻塞、不调 core） */
    if (cache_first && st->stop_ref != LUA_NOREF)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, st->stop_ref);
        return 1;
    }

    int   top      = lua_gettop(L);
    DWORD timeout  = (top >= base && lua_isnumber(L, base)) ? (DWORD)lua_tointeger(L, base) : 100;
    bool  autocont = (top >= base + 1 && lua_toboolean(L, base + 1)) ? true : false;

    const char *err = nullptr;
    int         before = st->seq_next;
    int         type;
    {
        SessionScope scope(st);
        type = st->sess->pump(timeout, autocont, &err);
    }
    if (type < 0)
    {
        /* 引擎自己已经跑过清理（D18）⇒ 这里只收**宿主侧**并把它从 registry 摘掉。
         * ❗不 `delete st`：会话对象还可能被一个 userdata 指着（方法之后按「没有会话」失败），
         *   释放留给 `__gc`（那时 `!registered` ⇒ 由它收尾）。 */
        free_strings(st);
        clear_stop(st);
        st->started = false;
        registry_remove(st);
        return fail(L, err ? err : "engine error");
    }
    /* core 在 pump 末尾才算得出默认应答码 ⇒ 这一轮新产生的记录在这里补 cont_code */
    if (type != 0) fill_cont_code(st, before);

    /* 回调里出错 / 取消：与 T7.3 同序（先报回调错误，再问守卫） */
    if (st->cb_error[0])
    {
        char msg[170];
        size_t i = 0;
        for (; st->cb_error[i] && i < sizeof(msg) - 1; i++) msg[i] = st->cb_error[i];
        msg[i] = 0;
        st->cb_error[0] = 0;
        return fail(L, msg);
    }
    if (guard_exec_active() && guard_should_stop()) return fail(L, guard_stop_message());

    /* ❗没等到事件（0）⇒ `nil`，且**不动** `d.stop`：上一轮那个未应答的事件仍然停在那儿
     *（目标被系统挂起），清掉就等于放 `Run` 进来。 */
    if (type == 0)
    {
        lua_pushnil(L);
        return 1;
    }

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
    lua_pushinteger(L, (lua_Integer)ctx.Rip);
#else
    lua_pushinteger(L, (lua_Integer)ctx.Eip);
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

/* `d:detach()`：恢复断点原字节 + `DebugActiveProcessStop`，**目标继续活着**（D51）。
 * ❗只有平台有这个导出时才会注册这个 key（`register_dbg_api` 里按能力装）。 */
static int do_detach(lua_State *L, SessionState *st, int base)
{
    (void)base;
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

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

    if (r != 0) return fail(L, "detach not supported on this platform");
    lua_pushboolean(L, 1);
    return 1;
}

/* `d:kill()`：终止目标（先恢复原字节 + 通知销毁，顺带把断点回调的 ref 还回来）。 */
static int do_kill(lua_State *L, SessionState *st, int base)
{
    (void)base;
    if (g_in_callback) return reentry(L);
    if (!st->started)  return fail(L, kErrNoSess);

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

static int dbg_pump(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_pump(L, g_default, 1, false);   /* 模块级 = 唯一的「等」（D57），不做缓存优先 */
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

/* 没有会话 ⇒ 也没有未应答事件（T7.3 的口径：paused 从不报错） */
static int dbg_paused_method(lua_State *L)
{
    lua_pushboolean(L, (g_default && g_default->stop_ref != LUA_NOREF) ? 1 : 0);
    return 1;
}

static int dbg_caps(lua_State *L) { return do_caps(L, g_default); }
static int dbg_help(lua_State *L) { return do_help(L, g_default); }

static int dbg_detach(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_detach(L, g_default, 1);
}

static int dbg_kill(lua_State *L)
{
    if (!g_default) return fail(L, kErrNoSess);
    return do_kill(L, g_default, 1);
}

/* ---------------- 会话方法（self 在栈 1，第一个实参在栈 2） ---------------- */

static int session_pump(lua_State *L)
{
    SessionState *st = self(L);
    if (!st) return fail(L, kErrNoSess);
    return do_pump(L, st, 2, true);
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
    lua_createtable(L, 0, 14);
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
bool        dbg_paused()        noexcept
{
    return g_default != nullptr && g_default->stop_ref != LUA_NOREF;
}

void register_dbg_api(lua_State *L)
{
    /* 一次执行一个 registry（D43/D54）：上一条执行留下的 C 侧状态在这里统一释放 */
    registry_clear();
    g_in_callback = false;
    g_has_detach  = (resolve_detach() != nullptr);   /* D51：平台能力，建表时定一次 */

    push_methods(L, g_has_detach);
    lua_setfield(L, LUA_REGISTRYINDEX, kSessionMethods);

    push_handle_table(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kSessionHandles);

    /* 会话对象的元表：`__index` 先认动态字段再落到方法表；`__gc` **只解绑**（D54） */
    luaL_newmetatable(L, kSessionMeta);
    lua_pushcfunction(L, session_index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, session_gc);    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_createtable(L, 0, 20);
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
    lua_setglobal(L, "dbg");
}

} /* namespace autodbg::host */
