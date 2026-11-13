/* =============================================================================
 * src/scripting/lua/dbg_bindings.cpp —— `dbg` 表：T7 状态机原语的 Lua 绑定（T7.3）
 *
 * 来源：新写（plan_t7.md §2.1 / §2.2 / §4）。形态照 `gui`：注册**一个全局表**（AGENTS.md §5 第 6 条），
 *   29 个契约名一个都不动；便捷层（until / on_hit / 条件断点 / Lua sugar）归 T7.6，不在这里。
 *
 * 与旧实现的差异：
 *   · 旧 `run()` 是「拿到事件后立刻 ContinueDebugEvent」；这里 `pump` 默认**不 continue**，事件停在
 *     未应答状态（`dbg.paused()`），由脚本显式 `dbg.cont([pid[,tid[,code]]])` —— `Run` 仍是那条便捷糖，
 *     两条路径在同一个会话上不可混用：`dbg` 停着一个未应答事件时 `Run`/`Attach` 报固定文案（§2.1 第 4 条）。
 *   · 两个入口分开：`dbg.create{...}`（= `Run` 的原语版，`CreateProcess` + `DEBUG_PROCESS`）与
 *     `dbg.attach(pid [, opts])`（= `Attach` 的原语版，`DebugActiveProcess`）。不再用 `params.pid != 0`
 *     隐式分派 —— 两个入口各自校验必填项（create 要 `imagepath`、attach 要非 0 的 `pid`），
 *     且 `attach` 能诚实地把「Win95/98 没有附加能力」作为 `nil, err` 上报（core 的 `-2`）。
 *     `attach` 只认显式 pid：`Attach{ }` 的「沿用上次 pid / 鼠标选进程」属 D48 解析链，是隐形状态，不进原语层。
 *   · `pump` 的 timeout 按 ≤250 ms 切片（core），每片没等到事件就问一次 `on_wait_timeout` ⇒ `--cancel`
 *     能在 pump 空闲时插话（D32 的第①个 poll 点）。本地模式的控制台 q / Shift+Q 策略仍只属 `Run` 路径
 *     （`CheckConsoleInput`）：`dbg` 不回读控制台，这条是**有意的**（免得以后被当 bug）。
 *   · 命中记录缓冲在**宿主层**（core 不认识队列）：环形、满则丢最旧、`dropped()` 可数、`setcap(1..64)` 可调。
 *     记录只在四个事件通道产生（`on_breakpoint` / `on_exception` / `on_user_exception` / `on_single_step`）；
 *     断点**自带**的回调（`on_breakpoint_callback`）只转交用户，不另记 —— 否则同一个命中会被记两次。
 *     例外：断点的 `on_exception` 通道不记（同一命中已经由 `on_breakpoint` 记过），但仍会调用户回调。
 *   · 回调里出错**不**长跳穿 C++ 帧（D18）：`lua_pcall` 兜住，消息存起来，下一次 `pump` 把它作为第二返回值。
 *   · 收尾是**两个动词**（D51）：`detach`（恢复原字节 + `DebugActiveProcessStop`，目标继续活着）与
 *     `kill`（先恢复原字节/释放回调，再 TerminateProcess）。**9x 上 `detach` 这个 key 不存在** ——
 *     `register_dbg_api` 里按 `DebugActiveProcessStop` 的可用性装。T7.3 提交过的 `close`（按目标来源
 *     隐式决定杀不杀 + 降级返回 1）已删：那是切片 A 要消除的隐式回退。
 *   · 收尾方法叫 `close` 而不是 `end`：`end` 是 Lua 关键字，`dbg.end` 写不出来（只有 `dbg["end"]()` 能调）。
 *   · `first_chance`：只有 `on_user_exception` 通道有真值；通用 `on_exception` 通道 core 不传 → 记 -1
 *     （数据面补口归 T7.4 §2.2 第 5 条）。
 *   · 重入：回调里调 `begin/pump/cont/step/arm/eip/detach/kill` → `nil, "pump cannot be called from a callback"`
 *     （固定文案；plan_t7.md §2.3 最后一条：回调里不能再 pump）。
 *   · `arm` 要求模块**已加载**（`get_base` 非 0）——否则 core 的 `set_breakpoint` 只会建一个
 *     `has_orignal_byte = 0` 的节点，直到命中时才报 "tmp->has_orignal_byte is zero!"（§9 第 9 条）；
 *     所以这里先判 base 并给一句能看懂的错。
 *   · ❗`arm` 不能打在「本来就写着 `CC` 的地址」上（例如 fixture 的 int 3）：core 命中后会把那里
 *     当「原始字节」还原、退回 EIP、置 TF 单步 —— 单步执行的又是 `CC`，于是再撞一次断点。
 *     对写死的 `int 3`，正确的做法是 `dbg.cont(nil, nil, 0x00010002)`（= `DBG_CONTINUE`）显式放行。
 *   · 用户回调的参数是**普通表** `{pid,tid,ea,code,modname,rva}`（新接口一律普通数字，不再用 LuaVariant）：
 *     `on_breakpoint` / `on_single_step` / `on_exception` / 断点自带回调都带 modname+rva，
 *     `callback`（入口）/ `on_exit_process` 没有 EA，modname 为空串、rva = 0。
 *   · `close` 把本层状态复位（`started` / `paused`），所以同一个 `lua_State` 里可以再 `begin`；
 *     命中环形队列留到下一次 `begin` 再清（复盘的窗口）。
 *   · `pump` 没等到事件（返回 0）时**不改** `paused`：未应答的事件仍然停在那儿（否则 `Run` 会被放进来）。
 *
 * 状态是模块级（不是 thread_local、也不是函数内非平凡 static —— §11.5 / PITFALLS M10）：执行严格串行
 *   （并发 1），与 `exec_guard` 同一口径。跨 pump 的引擎状态在会话成员里，这里只存「宿主侧」的那部分。
 * ========================================================================== */
#include "dbg_bindings.hpp"

#include <windows.h>
#include <intrin.h>   /* ZeroMem 宏（__stosb）；与 core/process.cpp 同一做法 */
#include <lua.hpp>

#include "common/allocator.hpp"   /* free_from_tchar → common::release（UNICODE 下才真释放） */
#include "common/dynapi.hpp"     /* get_proc_as：DebugActiveProcessStop 的能力探测（D51） */
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

/* 两条固定文案（门禁逐字断，plan_t7.md §2.1 第 4 条）。
 * 另两条「入口错」用常量，免得 rename 时改漏： */
const char *const kErrReentry = "pump cannot be called from a callback";
const char *const kErrPaused  = "cannot run while an event is unhandled";
const char *const kErrStarted = "dbg: already started (call dbg.detach or dbg.kill first)";
const char *const kErrNoSess  = "dbg: no session (call dbg.create or dbg.attach first)";

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
    DWORD  code;
    int    first_chance;   /* -1 = core 没报（见文件头） */
};

struct DbgState
{
    lua_State *L;

    int ref_entry;           /* `callback`（入口断点，一次性） */
    int ref_exception_cb;    /* `exception_callback`（一次性） */
    int ref_on_exception;
    int ref_on_breakpoint;
    int ref_on_single_step;
    int ref_on_exit_process;

    bool in_callback;        /* 正在执行用户回调 ⇒ 状态机调用一律拒 */
    bool started;            /* begin 已跑、close 未跑 */
    bool paused;             /* 有一个事件未应答（pump 的 autocont = false） */

    DWORD last_pid;
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
    LPXSTR s_imagepath;      /* 本层分配，close 释放 */
    LPXSTR s_cmdline;
    LPXSTR s_cwd;
};

DbgState g{};

core::DebuggerSession &sess() { return autodbg::core::current_session(); }

static int fail(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

static int reentry(lua_State *L) { return fail(L, kErrReentry); }

/* `begin` 分配的三份 TCHAR 串（`close` 与 `begin` 的失败路径都要收尾） */
static void free_strings()
{
    if (g.s_imagepath) free_from_tchar(g.s_imagepath);
    if (g.s_cmdline)   free_from_tchar(g.s_cmdline);
    if (g.s_cwd)       free_from_tchar(g.s_cwd);
    g.s_imagepath = nullptr;
    g.s_cmdline   = nullptr;
    g.s_cwd       = nullptr;
}

static void reset_state(lua_State *L)
{
    ZeroMem(g);
    g.L        = L;
    g.cap      = kHitCapMax;
    g.seq_next = 1;
}

static int store_ref(lua_State *L, int n, const char *field)
{
    lua_getfield(L, n, field);
    int ref = LUA_NOREF;
    if (lua_isfunction(L, -1)) ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    return ref;
}

static void unref(int ref)
{
    if (ref != LUA_NOREF) luaL_unref(g.L, LUA_REGISTRYINDEX, ref);
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

static void record_hit(int type, DWORD pid, DWORD tid, size_t ea, DWORD code, int first_chance,
                       LPCTSTR modname, DWORD rva)
{
    HitRecord *r;
    if (g.count < g.cap)
    {
        r = &g.ring[(g.head + g.count) % kHitCapMax];
        g.count++;                        /* 窗口里还剩格子 */
    }
    else
    {
        g.dropped++;                        /* 满则丢最旧（可断） */
        g.head = (g.head + 1) % kHitCapMax; /* 窗口前移：最旧那条格子腾出来 */
        r = &g.ring[(g.head + g.count - 1) % kHitCapMax];
    }
    r->seq          = g.seq_next++;
    r->type         = type;
    r->pid          = pid;
    r->tid          = tid;
    r->ea           = ea;
    r->code         = code;
    r->rva          = rva;
    r->first_chance = first_chance;
    copy_name(r->modname, sizeof(r->modname), modname);
}

/* 用户回调的调用点（定义在 `push_hit_table` 之后；这里先声明，因为 `record_hit_by_ea`
 * 要「记录 + 回调」两件事一起做）。 */
static void call_user(int ref, DWORD pid, DWORD tid, size_t ea, DWORD code,
                      LPCTSTR modname, DWORD rva);

/* 在 `ea` 处记录一条命中，并把同一份 modname/rva 交给用户回调（字段名与 `hits()` 记录一致） */
static void record_hit_by_ea(int type, DWORD pid, DWORD tid, size_t ea, DWORD code, int first_chance,
                             int ref)
{
    core::PEInfo *pe      = sess().get_peinfo_by_ea(ea);
    LPCTSTR       modname = pe ? (pe->Path + pe->name_offset) : TEXT("");
    DWORD         rva     = pe ? (DWORD)(ea - pe->base) : 0;
    record_hit(type, pid, tid, ea, code, first_chance, modname, rva);
    call_user(ref, pid, tid, ea, code, modname, rva);
}

static void push_hit_table(lua_State *L)
{
    for (int i = 0; i < g.count; i++)
    {
        const HitRecord &r = g.ring[(g.head + i) % kHitCapMax];
        lua_createtable(L, 0, 9);
        lua_pushinteger(L, (lua_Integer)r.seq);          lua_setfield(L, -2, "seq");
        lua_pushinteger(L, (lua_Integer)r.type);         lua_setfield(L, -2, "type");
        lua_pushinteger(L, (lua_Integer)r.pid);          lua_setfield(L, -2, "pid");
        lua_pushinteger(L, (lua_Integer)r.tid);          lua_setfield(L, -2, "tid");
        lua_pushinteger(L, (lua_Integer)r.ea);           lua_setfield(L, -2, "ea");
        lua_pushstring(L, r.modname);                    lua_setfield(L, -2, "modname");
        lua_pushinteger(L, (lua_Integer)r.rva);          lua_setfield(L, -2, "rva");
        lua_pushinteger(L, (lua_Integer)r.code);         lua_setfield(L, -2, "code");
        if (r.first_chance < 0) lua_pushnil(L);
        else                    lua_pushinteger(L, r.first_chance ? 1 : 0);
        lua_setfield(L, -2, "first_chance");
        lua_rawseti(L, -2, i + 1);
    }
}

/* 用户回调的参数形状（§2.1 第 3 条：新接口一律普通数字/字符串，不再用 LuaVariant）：
 *   { pid, tid, ea, code, modname, rva } —— 与 `hits()` 的记录同名同义（AI 侧只学一套字段）。
 * `ref == LUA_NOREF` ⇒ 没有回调：**仍然记录**（记录与回调是两条独立通道）。 */
static void call_user(int ref, DWORD pid, DWORD tid, size_t ea, DWORD code,
                      LPCTSTR modname, DWORD rva)
{
    if (ref == LUA_NOREF) return;
    lua_rawgeti(g.L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(g.L, -1)) { lua_pop(g.L, 1); return; }

    char modbuf[40];
    copy_name(modbuf, sizeof(modbuf), modname ? modname : TEXT(""));

    lua_createtable(g.L, 0, 6);
    lua_pushinteger(g.L, (lua_Integer)pid);  lua_setfield(g.L, -2, "pid");
    lua_pushinteger(g.L, (lua_Integer)tid);  lua_setfield(g.L, -2, "tid");
    lua_pushinteger(g.L, (lua_Integer)ea);   lua_setfield(g.L, -2, "ea");
    lua_pushinteger(g.L, (lua_Integer)code); lua_setfield(g.L, -2, "code");
    lua_pushstring(g.L, modbuf);             lua_setfield(g.L, -2, "modname");
    lua_pushinteger(g.L, (lua_Integer)rva);  lua_setfield(g.L, -2, "rva");

    g.in_callback = true;
    if (lua_pcall(g.L, 1, 0, 0) != 0)
    {
        const char *msg = lua_tostring(g.L, -1);
        size_t i = 0;
        if (msg)
        {
            for (; msg[i] && i < sizeof(g.cb_error) - 1; i++) g.cb_error[i] = msg[i];
        }
        g.cb_error[i] = 0;
        lua_pop(g.L, 1);
    }
    g.in_callback = false;
}

/* ---------------- C 回调（签名与 DebugCallbacks 逐字对应） ---------------- */

static void h_any_event(void *ctx, const core::DebugEvent &ev)
{
    (void)ctx;
    g.last_pid = ev.pid;      /* `cont()` 不带参数时用的就是这一对 */
    g.last_tid = ev.tid;
}

static void h_exception(void *ctx, const core::ExceptionEvent &ev)
{
    (void)ctx;
    /* 断点由 `on_breakpoint` 记录（同一个命中只记一次）；这里记其余异常。
     * ❗用户回调**每条异常都调**（含断点）——与 `Run` 的 `exception_callback` 口径一致。 */
    if (ev.code != EXCEPTION_BREAKPOINT)
        record_hit_by_ea(HIT_EXCEPTION, ev.pid, ev.tid, ev.addr, ev.code, -1, LUA_NOREF);
    core::PEInfo *pe = sess().get_peinfo_by_ea(ev.addr);
    call_user(g.ref_on_exception, ev.pid, ev.tid, ev.addr, ev.code,
              pe ? (pe->Path + pe->name_offset) : TEXT(""), pe ? (DWORD)(ev.addr - pe->base) : 0);
}

static void h_user_exception(void *ctx, const core::UserExceptionEvent &ev)
{
    (void)ctx;
    /* 这条通道带真 first_chance（`on_exception` 通道 core 不传 → 那边记 -1） */
    record_hit_by_ea(HIT_EXCEPTION, ev.pid, ev.tid, (size_t)ev.record->ExceptionAddress,
                     ev.record->ExceptionCode, ev.first_chance ? 1 : 0, g.ref_exception_cb);
}

static void h_breakpoint(void *ctx, const core::BreakpointEvent &ev)
{
    (void)ctx;
    record_hit_by_ea(HIT_BREAKPOINT, ev.pid, ev.tid, ev.addr, EXCEPTION_BREAKPOINT, -1,
                     g.ref_on_breakpoint);
}

static void h_single_step(void *ctx, const core::StepEvent &ev)
{
    (void)ctx;
    record_hit_by_ea(HIT_SINGLE_STEP, ev.pid, ev.tid, ev.addr, EXCEPTION_SINGLE_STEP, -1,
                     g.ref_on_single_step);
}

static void h_first_breakpoint(void *ctx, const core::FirstBreakpointEvent &ev)
{
    (void)ctx;
    call_user(g.ref_entry, ev.pid, ev.tid, 0, 0, TEXT(""), 0);
}

static void h_exit_process(void *ctx, const core::ExitEvent &ev)
{
    (void)ctx;
    call_user(g.ref_on_exit_process, ev.pid, ev.tid, 0, ev.code, TEXT(""), 0);
}

static void h_bp_callback(void *ctx, const core::BreakpointCallbackEvent &ev)
{
    (void)ctx;
    /* 该通道不带首次命中的记录（`on_breakpoint` 已经记过），只转交用户回调 */
    call_user((int)ev.callback.token.i, ev.pid, ev.tid, ev.ea, EXCEPTION_BREAKPOINT,
              ev.modname, ev.rva);
}

static void h_bp_destroyed(void *ctx, callback_ref_t callback)
{
    if (!callback.valid) return;
    luaL_unref((lua_State *)ctx, LUA_REGISTRYINDEX, (int)callback.token.i);
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

/* ---------------- 表方法 ---------------- */

/* 选项表里**两个入口共用**的那部分：`use_scylla_hide` + 六个回调。
 * `n` = 表在栈上的绝对下标（`create` 的表是 1，`attach` 的第二个参数是 2）。 */
static void read_common_opts(lua_State *L, int n)
{
    lua_getfield(L, n, "use_scylla_hide");
    g.params.use_scylla_hide = lua_toboolean(L, -1) ? true : false;
    lua_pop(L, 1);

    g.ref_entry           = store_ref(L, n, "callback");
    g.ref_exception_cb    = store_ref(L, n, "exception_callback");
    g.ref_on_exception    = store_ref(L, n, "on_exception");
    g.ref_on_breakpoint   = store_ref(L, n, "on_breakpoint");
    g.ref_on_single_step  = store_ref(L, n, "on_single_step");
    g.ref_on_exit_process = store_ref(L, n, "on_exit_process");
}

/* 装好 `DebugCallbacks` 调 core 的 `begin`；两个入口的最后一步（差别已在 `params` 里）。 */
static int start_session(lua_State *L)
{
    ZeroMem(g.cb);
    g.cb.context                 = L;
    g.cb.on_any_debug_event      = h_any_event;
    g.cb.on_exception            = h_exception;
    g.cb.on_breakpoint           = h_breakpoint;
    g.cb.on_single_step          = h_single_step;
    g.cb.on_exit_process         = h_exit_process;
    g.cb.on_first_breakpoint     = h_first_breakpoint;
    g.cb.on_user_exception       = h_user_exception;
    g.cb.on_breakpoint_callback  = h_bp_callback;
    g.cb.on_breakpoint_destroyed = h_bp_destroyed;
    g.cb.on_wait_timeout         = h_wait_timeout;

    const char *err = nullptr;
    int r = sess().begin(&g.params, &g.cb, &err);
    if (r == -2)
    {
        /* 1:1：这条路径不跑收尾（§4.1）。`-2` 只来自 `DebugActiveProcess` 失败
         * （Win95/98 根本没有附加能力 ⇒ legacy 变体上 `dbg.attach` 就是这句）。 */
        free_strings();
        return fail(L, "attach failed (DebugActiveProcess) — Win95/98 has no attach");
    }
    if (r < 0)
    {
        g.started = false;                          /* -1 的清理已在 core 内跑过（D18） */
        free_strings();
        return fail(L, err ? err : "engine error");
    }
    g.started = true;
    lua_pushboolean(L, 1);
    return 1;
}

/* `dbg.create{ imagepath=…, cmdline=…, cwd=…, is_new_console=…, use_scylla_hide=…, <六个回调> }`
 * = `Run` 的原语版：`CreateProcess` + `DEBUG_PROCESS`，**不循环**。 */
static int dbg_create(lua_State *L)
{
    if (g.in_callback) return reentry(L);
    if (g.started)     return fail(L, kErrStarted);
    if (!lua_istable(L, 1)) return fail(L, "dbg.create: table expected");

    reset_state(L);

    lua_getfield(L, 1, "imagepath");
    if (lua_isstring(L, -1))
    {
        g.s_imagepath      = utf8_to_tchar(lua_tostring(L, -1));
        g.params.imagepath = g.s_imagepath;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "cmdline");
    if (lua_isstring(L, -1))
    {
        g.s_cmdline      = utf8_to_tchar(lua_tostring(L, -1));
        g.params.cmdline = g.s_cmdline;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "cwd");
    if (lua_isstring(L, -1))
    {
        g.s_cwd      = utf8_to_tchar(lua_tostring(L, -1));
        g.params.cwd = g.s_cwd;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "is_new_console");
    g.params.is_new_console = lua_toboolean(L, -1) ? true : false;
    lua_pop(L, 1);

    /* 必填项显式判：core 会拿 NULL 去 `CreateProcess`，那只会换来一句 CreateProcess 失败 */
    if (!g.params.imagepath)
    {
        free_strings();
        return fail(L, "dbg.create: imagepath expected");
    }

    read_common_opts(L, 1);
    return start_session(L);
}

/* `dbg.attach(pid [, { use_scylla_hide=…, <六个回调> }])` = `Attach` 的原语版。
 * ❗只认**显式** pid：`Attach{ }` 的「沿用上次 pid / 鼠标选进程」魔法属 D48 的解析链，
 *   在状态机原语层里是隐形状态（脚本该自己 `gui.pick()` 并把结果写进脚本头标记）。 */
static int dbg_attach(lua_State *L)
{
    if (g.in_callback) return reentry(L);
    if (g.started)     return fail(L, kErrStarted);
    if (!lua_isnumber(L, 1)) return fail(L, "dbg.attach: pid (number) expected");

    DWORD pid = (DWORD)lua_tointeger(L, 1);
    if (!pid) return fail(L, "dbg.attach: pid expected");

    bool has_opts = lua_istable(L, 2);
    if (!has_opts && lua_gettop(L) >= 2) return fail(L, "dbg.attach: options table expected");

    reset_state(L);
    g.params.pid = pid;                    /* 非 0 ⇒ core 走 `DebugActiveProcess` 而不是 CreateProcess */
    if (has_opts) read_common_opts(L, 2);  /* 不给表 = 只要命中记录，不要回调 */
    return start_session(L);
}

static int dbg_pump(lua_State *L)
{
    if (g.in_callback) return reentry(L);
    if (!g.started)     return fail(L, kErrNoSess);

    int    top     = lua_gettop(L);
    DWORD  timeout = (top >= 1 && lua_isnumber(L, 1)) ? (DWORD)lua_tointeger(L, 1) : 100;
    bool   autocont = (top >= 2 && lua_toboolean(L, 2)) ? true : false;

    const char *err = nullptr;
    int code = sess().pump(timeout, autocont, &err);
    if (code < 0)
    {
        g.started = false;
        g.paused  = false;
        free_strings();
        return fail(L, err ? err : "engine error");
    }
    /* ❗只有真的等到事件才更新 `paused`：`pump` 返回 0 = 没等到事件，此时**上一轮那个未应答的
     * 事件还停在那儿**（目标被系统挂起），把 paused 清掉就等于放 `Run` 进来。 */
    if (code != 0) g.paused = !autocont;

    /* 取消 / 超时：pump 的空闲片里问守卫（D32），这里把守卫结论转成 `nil, msg` */
    if (g.cb_error[0])
    {
        char msg[170];
        size_t i = 0;
        for (; g.cb_error[i] && i < sizeof(msg) - 1; i++) msg[i] = g.cb_error[i];
        msg[i] = 0;
        g.cb_error[0] = 0;
        return fail(L, msg);
    }
    if (guard_exec_active() && guard_should_stop()) return fail(L, guard_stop_message());

    lua_pushinteger(L, (lua_Integer)code);
    return 1;
}

static int dbg_cont(lua_State *L)
{
    if (g.in_callback) return reentry(L);
    if (!g.started)     return fail(L, kErrNoSess);

    int   top  = lua_gettop(L);
    DWORD pid  = (top >= 1 && lua_isnumber(L, 1)) ? (DWORD)lua_tointeger(L, 1) : g.last_pid;
    DWORD tid  = (top >= 2 && lua_isnumber(L, 2)) ? (DWORD)lua_tointeger(L, 2) : g.last_tid;
    DWORD code = (top >= 3 && lua_isnumber(L, 3)) ? (DWORD)lua_tointeger(L, 3) : sess().last_code();

    bool ok = sess().cont(pid, tid, code);
    if (ok) g.paused = false;       /* 应答失败（没事件可应答）⇒ 事件仍未应答 */
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/* 会话收尾的**宿主侧**那一半（core 的 `end()` 管引擎侧）：释放回调 ref、释放 TCHAR 串、
 * 复位本地状态 —— 之后可以再 `create` / `attach`。命中环形队列留到下一次入口再清
 * （它是复盘窗口，不是会话资源）。 */
static void finish_session()
{
    unref(g.ref_entry);
    unref(g.ref_exception_cb);
    unref(g.ref_on_exception);
    unref(g.ref_on_breakpoint);
    unref(g.ref_on_single_step);
    unref(g.ref_on_exit_process);

    free_strings();

    g.started     = false;
    g.paused      = false;
    g.cb_error[0] = 0;
    g.params.pid  = 0;
}

/* `dbg.detach()`：恢复断点原字节 + `DebugActiveProcessStop`，**目标继续活着**（D51）。
 * ❗只有平台有这个导出时才会注册这个 key（`register_dbg_api` 里按能力装）。 */
static int dbg_detach(lua_State *L)
{
    if (g.in_callback) return reentry(L);
    if (!g.started)     return fail(L, kErrNoSess);

    /* ❗未应答的事件必须先放行：否则 `DebugActiveProcessStop` 之后目标停在被系统挂起的
     * 线程上（DBG_CONTINUE = 这个断点「已处理」，然后我们撒手 —— 此处唯一合理的选择）。 */
    if (g.paused)
    {
        sess().cont(g.last_pid, g.last_tid, DBG_CONTINUE);
        g.paused = false;
    }

    sess().to_detach() = true;   /* core 的 `end()` 做实事：恢复原字节 + 通知销毁 + DebugActiveProcessStop */
    const char *err = nullptr;
    int r = sess().end(&err);
    (void)err;
    finish_session();

    if (r != 0) return fail(L, "detach not supported on this platform");
    lua_pushboolean(L, 1);
    return 1;
}

/* `dbg.kill()`：终止目标（先恢复原字节 + 通知销毁，顺带把断点回调的 ref 还回来）。 */
static int dbg_kill(lua_State *L)
{
    if (g.in_callback) return reentry(L);
    if (!g.started)     return fail(L, kErrNoSess);

    sess().detach_process(&g.cb); /* `end()` 在 to_detach 为假时不做这一步，所以这里显式做 */
    sess().to_detach() = false;
    sess().term()      = true;    /* `run_shutdown` 会 TerminateProcess / TerminateThread */
    const char *err = nullptr;
    sess().end(&err);             /* 返回值只有「detach 降级」一种含义 ⇒ kill 路径必然 0 */
    (void)err;
    finish_session();

    lua_pushboolean(L, 1);
    return 1;
}

/* `dbg.caps()`：这台机器 / 这个 build 实际有什么（D53）。形状固定、可门禁。 */
static int dbg_caps(lua_State *L)
{
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

/* `dbg.help()` = `help('dbg')`：文本表只有一份（help_bindings.cpp） */
static int dbg_help(lua_State *L)
{
    if (!help_print_topic("dbg")) return fail(L, "dbg.help: no help topic");
    lua_pushboolean(L, 1);
    return 1;
}

static HANDLE step_thread(DWORD tid)
{
    HANDLE h = sess().get_thread_by_tid(tid);
    if (!h) return nullptr;
    CONTEXT ctx;
    ZeroMem(ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!sess().get_context(h, CONTEXT_CONTROL, &ctx)) return nullptr;
    ctx.EFlags |= 0x100;      /* TF（§2.1 第 6 条） */
    if (!sess().set_context(h, CONTEXT_CONTROL, &ctx)) return nullptr;
    return h;
}

static int dbg_step(lua_State *L)
{
    if (g.in_callback) return reentry(L);
    if (!g.started)     return fail(L, kErrNoSess);

    int   top = lua_gettop(L);
    DWORD tid = (top >= 1 && lua_isnumber(L, 1)) ? (DWORD)lua_tointeger(L, 1) : g.last_tid;
    if (!step_thread(tid)) return fail(L, "dbg.step: thread not found or context unavailable");
    lua_pushboolean(L, 1);
    return 1;
}

static int dbg_eip(lua_State *L)
{
    if (g.in_callback) return reentry(L);
    if (!g.started)     return fail(L, kErrNoSess);

    int   top = lua_gettop(L);
    DWORD tid = (top >= 1 && lua_isnumber(L, 1)) ? (DWORD)lua_tointeger(L, 1) : g.last_tid;
    HANDLE h = sess().get_thread_by_tid(tid);
    if (!h) return fail(L, "dbg.eip: thread not found");
    CONTEXT ctx;
    ZeroMem(ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!sess().get_context(h, CONTEXT_CONTROL, &ctx)) return fail(L, "dbg.eip: GetThreadContext failed");
#ifdef _WIN64
    lua_pushinteger(L, (lua_Integer)ctx.Rip);
#else
    lua_pushinteger(L, (lua_Integer)ctx.Eip);
#endif
    return 1;
}

static int dbg_arm(lua_State *L)
{
    if (g.in_callback)  return reentry(L);
    if (!g.started)     return fail(L, kErrNoSess);
    if (!lua_isstring(L, 1))  return fail(L, "dbg.arm: modname (string) expected");
    if (!lua_isnumber(L, 2))  return fail(L, "dbg.arm: rva (number) expected");   /* 普通数字，不经 LuaVariant（§2.1 第 3 条） */
    if (!lua_isfunction(L, 3)) return fail(L, "dbg.arm: callback expected");

    LPXSTR modname = utf8_to_tchar(lua_tostring(L, 1));   /* UNICODE 下是分配来的；ANSI 下宏就是原样返回 */
    /* ❗模块没加载时 core 的 `set_breakpoint` 只会建一个 `has_orignal_byte = 0` 的节点，
     * 直到命中才报 "tmp->has_orignal_byte is zero!"（§9 第 9 条）。先判 base，给能看懂的错。 */
    if (!sess().get_base(modname))
    {
#ifdef _UNICODE
        release(modname);
#endif
        return fail(L, "dbg.arm: module not loaded (pump to its load event first)");
    }

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* 令牌由 core 在断点销毁时经 `on_breakpoint_destroyed` 还回来 */

    callback_ref_t tok;
    tok.token.i = (intptr_t)ref;
    tok.valid   = 1;

    sess().set_breakpoint(modname, (DWORD)lua_tointeger(L, 2), false, tok);
#ifdef _UNICODE
    release(modname);
#endif
    lua_pushboolean(L, 1);
    return 1;
}

static int dbg_hits(lua_State *L)
{
    lua_createtable(L, g.count, 0);
    push_hit_table(L);
    return 1;
}

static int dbg_dropped(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)g.dropped);
    return 1;
}

static int dbg_setcap(lua_State *L)
{
    int n = (lua_isnumber(L, 1)) ? (int)lua_tointeger(L, 1) : g.cap;
    if (n < 1) n = 1;
    if (n > kHitCapMax) n = kHitCapMax;
    if (n < g.count)
    {
        g.dropped += g.count - n;                      /* 收窄时丢的是最旧那几条 */
        g.head = (g.head + (g.count - n)) % kHitCapMax;
        g.count = n;
    }
    g.cap = n;
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int dbg_code(lua_State *L)
{
    if (!g.started) return fail(L, kErrNoSess);
    lua_pushinteger(L, (lua_Integer)sess().last_code());
    return 1;
}

static int dbg_stopped(lua_State *L)
{
    if (!g.started) return fail(L, kErrNoSess);
    lua_pushboolean(L, sess().cont() ? 0 : 1);   /* 「这一轮该停」= run 的 while 出口（§2.1 记录 ②） */
    return 1;
}

static int dbg_paused_method(lua_State *L)
{
    lua_pushboolean(L, g.paused ? 1 : 0);
    return 1;
}

} /* anonymous namespace */

const char *dbg_error_reentry() noexcept { return kErrReentry; }
const char *dbg_error_paused()  noexcept { return kErrPaused; }
bool        dbg_paused()        noexcept { return g.paused; }

void register_dbg_api(lua_State *L)
{
    reset_state(L);
    g_has_detach = (resolve_detach() != nullptr);   /* D51：平台能力，建表时定一次 */

    lua_createtable(L, 0, 17);
    lua_pushcfunction(L, dbg_create);  lua_setfield(L, -2, "create");   /* = Run 的原语版（CreateProcess） */
    lua_pushcfunction(L, dbg_attach);  lua_setfield(L, -2, "attach");   /* = Attach 的原语版（DebugActiveProcess；Win95/98 没有） */
    lua_pushcfunction(L, dbg_pump);    lua_setfield(L, -2, "pump");
    lua_pushcfunction(L, dbg_cont);    lua_setfield(L, -2, "cont");
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
    lua_pushcfunction(L, dbg_code);    lua_setfield(L, -2, "code");
    lua_pushcfunction(L, dbg_stopped); lua_setfield(L, -2, "stopped");
    lua_pushcfunction(L, dbg_paused_method); lua_setfield(L, -2, "paused");
    lua_setglobal(L, "dbg");
}

} /* namespace autodbg::host */
