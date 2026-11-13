#ifndef AUTODBG_CORE_CORE_HPP
#define AUTODBG_CORE_CORE_HPP
/* =============================================================================
 * src/core/core.hpp —— `DebuggerSession`：调试器核心的**唯一状态容器**
 *   （Plan P3.A.2 / 5.2；按需多实例、无全局单例）
 *
 * 现在（P3.A 过程态）只搬入了**状态**：原 `cdebugger.c` 的 9 个全局量
 *   （`g_pi` / `g_peinfo_list` / `g_thread_list` / `g_breakpoint_list` /
 *    `g_hardware_breakpoint_infos` / `g_cont` / `g_term` / `g_to_detach`），
 *   以及原 `init()` / `release()`（→ 构造 / 析构）。
 *   注意：`g_wfilepath`（Lua 脚本路径）**按 Q7 归 Lua 层**，不进 session。
 *
 * ❗会话储存的就是 `types.hpp` 里那套结构体（`PEInfo` / `ThreadInfo` /
 *   `BreakpointInfo` / `HardwareBreakpointInfo`）—— ✅ **类型统一已完成**（全仓只此一份，
 *   原来的 `core/legacy_types.h` 副本已删；布局锁见 `tests/unit/test_types.cpp`）。
 *
 * ✅ P6.4 后：引擎函数**就是本类的成员**（见下面的「引擎 API」段），内部一律用自己的状态；`current_session()` 只留给宿主层与签名固定的回调。
 *   模块之间按 Q9 用「内部头 + session 成员」直接访问（即下面的过程态访问器），
 *   只有绑定层看到公有接口，P6.4 再统一收窄。
 *   T7.3 加 `last_code()`（pump 存下的 code 给绑定层读）；`pump` 的 timeout 按 ≤250 ms 切片，
 *   每片没等到事件就问一次 `on_wait_timeout`（core 仍不知道 socket / stdin）。
 *
 * ✅ T7.1（plan_t7.md §2.1）：调试主循环拆成状态机 `begin` / `pump` / `cont` / `end`，`run` 是它的
 *   封装（可观察行为逐字节不变）。C2a 再把 `pump` 拆成 `wait_one` + `dispatch`（宿主层要按事件归属
 *   做进程级路由）。T7.3 给 `end` 补上 D47 的真 detach（`to_detach()` 为真时恢复原字节 +
 *   `DebugActiveProcessStop`，没有该导出就降级成 kill 并返回 1）—— `Run`/`Attach` 的旧路径一字不变。
 *   跨 pump 的循环状态（`m_first_bp` / `m_push_fb` / `m_reset_bpx` /
 *   `m_reset_hw_bp` / `m_first_create_process` / `m_debug_active_process_stop` / `m_last_code`，加上 `begin` 存下的
 *   `m_params` / `m_cb`）**必须是会话成员** —— 手工映射的模块禁止 `thread_local` 与函数内非平凡
 *   构造的 static（§11.5、PITFALLS M10），留在栈上就是跨 pump 状态丢失。
 *
 * ✅ T7 切片 C2b（plan_t7.md §10）：会话的键从「一个 pid」变成**一棵进程树** —— `m_pids` 是
 *   根 pid + 调试期间出现的全部 `CREATE_PROCESS` 的 pid（`owns_pid()`），`m_pid_handles` 是
 *   `OpenProcess` 出来的按 pid 句柄缓存（`handle_for_pid()`）。内存 API 因此多出 pid 重载，
 *   断点补丁按 `PEInfo::dwProcessId` 打（否则子进程的补丁会写到根进程上）。
 *   仍限单进程树的旧口径（每会话一个待应答槽）不变：进程树的“停住”只指最近那个事件。
 *
 * 链表字段目前仍是**裸指针 + 首成员 next**（与旧实现 1:1）；P6.2 换成
 *   `common::IntrusiveList<T>`（容器已实现并有单测）。
 * ========================================================================== */
#include "types.hpp" /* 引擎实际在用的结构体（全局命名空间；见文件头说明） */
#include "common/containers.hpp" /* P6.2/P6.4：三张链表用 OwnedIntrusiveList（拥有节点） */
#include "util.hpp"
/* ❗故意**不** include `types.hpp`：它在本命名空间里定义了一套同名结构体副本，
 *   而引擎用的是全局那套（`PEInfo` …）。若在这里把它拉进来，所有包含本头的
 *   TU 里的 `PEInfo` 都会解析到副本 → 与大堆指针类型不匹配（实测过的坑）。 */

namespace autodbg::core
{
class DebuggerSession
{
public:
    DebuggerSession() noexcept;  /* 原 init() */
    ~DebuggerSession() noexcept; /* 原 release() */

    DebuggerSession(const DebuggerSession &) = delete;
    DebuggerSession &operator=(const DebuggerSession &) = delete;

    /* ------------------- 状态访问（P3.A 过程态；P6.4 收窄） ------------------- */
    PROCESS_INFORMATION       &proc() noexcept { return m_pi; }        /* 原 g_pi */
    const PROCESS_INFORMATION &proc() const noexcept { return m_pi; }

    /* 三张链表：**拥有节点**的侵入式链表（P6.2/P6.4）——变长尾数组（`Path[1]`/`modname[1]`）
     * 由 `create_front(extra_bytes)` 一次分配；节点在会话析构/`clear()` 时统一释放（RAII）。 */
    common::OwnedIntrusiveList<PEInfo>         &peinfos() noexcept { return m_peinfos; }         /* 原 g_peinfo_list */
    common::OwnedIntrusiveList<ThreadInfo>     &threads() noexcept { return m_threads; }         /* 原 autodbg::core::current_session().threads() */
    common::OwnedIntrusiveList<BreakpointInfo> &breakpoints() noexcept { return m_breakpoints; } /* 原 autodbg::core::current_session().breakpoints() */
    /* ❗返回**数组引用**（不是指针）：旧调用点里有 `&g_hardware_breakpoint_infos`
     * （取整个数组的地址）与 `g_hardware_breakpoint_infos[i]` 两种用法，返回指针会让
     * `&` 作用在 prvalue 上（C2102）。 */
    using HwBpArray = HardwareBreakpointInfo[4];
    HwBpArray &hw_breakpoints() noexcept { return m_hw_breakpoints; } /* 原 autodbg::core::current_session().hw_breakpoints()[4] */

    bool &cont() noexcept { return m_cont; }           /* 原 autodbg::core::current_session().cont() —— ❗与下面的 `cont(pid, tid, code)` 是重载：无参 = 循环标志 */
    bool &term() noexcept { return m_term; }           /* 原 autodbg::core::current_session().term() */
    bool &to_detach() noexcept { return m_to_detach; } /* 原 autodbg::core::current_session().to_detach() */
    /* ------------------------- 引擎 API（P3.A.5 / P3.B 迁入） -------------------------
     * 目标形态见 Plan 5.2：set_breakpoint / read / write / attach / detach / run / …
     * 本轮先只提供状态，函数体在下一步从 cdebugger.c 搬进来。 */

    /* ================================ 引擎 API（P6.4：状态 → 成员函数） ================================
     * 这些函数原本是各模块里的自由函数，靠「当前会话」访问状态；现在改成成员——
     * 作用对象**明确是 this**，因此同一个进程里可以并存多个会话（真正的多实例）。
     * 命名暂时保持旧名字（snake_case 统一留给 P6.6 风格批）。
     * 模块头里还留着同名 inline 转发（过渡期，给尚未迁移的调用方用），迁移完即删。 */

    /* --- 内存（memory.cpp） ---
     * 每个内存原语有**两个重载**：不带 pid 的（= 根进程，1:1 保留旧签名，29 个契约名与既有
     * 调用点零改动）与带 pid 的（进程树里的任意进程，T7 切片 C2b）。`pid == 0` 等价于根进程
     * （`proc().dwProcessId`），句柄由 `handle_for_pid()` 按 pid 取。 */
    bool read(size_t addr, void *data, size_t size);
    bool read(DWORD pid, size_t addr, void *data, size_t size);
    /* T7 切片 C1b（plan_t7.md §2.2 第 12 条）：逐页读，**在第一个读不了的页处停下**，
     * 返回**实际读到的字节数**（0 = 一字节都没读到）。`read()` 是它的全有或全无包装。 */
    size_t read_some(size_t addr, void *data, size_t size);
    size_t read_some(DWORD pid, size_t addr, void *data, size_t size);
    bool write(size_t addr, const void *data, size_t size, bool up_intrin);
    bool write(DWORD pid, size_t addr, const void *data, size_t size, bool up_intrin);
    bool update_memory(size_t addr, void *old_data, const void *new_data, size_t size, bool is_up_intrin);
    bool update_memory(DWORD pid, size_t addr, void *old_data, const void *new_data, size_t size, bool is_up_intrin);
    bool loadstr(size_t addr, char **pp, size_t *nn);
    bool loadstr(DWORD pid, size_t addr, char **pp, size_t *nn);
    void find_memory_init(FindMemoryContext *ctx, const void *pattern, size_t len);
    FindMemoryKind find_memory_next(FindMemoryContext *ctx, FindMemoryMatch *out, char *errbuf, size_t errcap);
    void find_memory_release(FindMemoryContext *ctx);
    void get_memory_region_begin(MemRegionIter *it);
    bool get_memory_region_next(MemRegionIter *it, MemRegionInfo *out);

    /* --- 模块 / PE（pe.cpp） --- */
    PEInfo *get_peinfo_by_name(LPCTSTR modname);
    PEInfo *get_peinfo_by_ea(size_t ea);
    size_t  get_base(LPCTSTR name);
    void    parse_pe(PEInfo *peinfo);

    /* --- 线程上下文（context.cpp） --- */
    bool get_context(HANDLE hThread, DWORD flags, CONTEXT *out);
    bool set_context(HANDLE hThread, DWORD flags, const CONTEXT *in);
    bool update_all_threads_context(DWORD flags, bool (*mutate)(CONTEXT *ctx, void *user), void *user);

    /* --- 软件断点（breakpoints.cpp） --- */
    void set_breakpoint(LPCTSTR modname, DWORD rva, bool is_single, callback_ref_t callback);
    void del_breakpoint(const DebugCallbacks *cb, size_t ea, LPCTSTR modname, size_t rva);
    void enable_breakpoint(size_t ea, LPCTSTR modname, DWORD rva);
    void disable_breakpoint(size_t ea, LPCTSTR modname, DWORD rva);
    void apply_breakpoints_for_module(PEInfo *peinfo);
    void update_breakpoints(PEInfo *peinfo);
    void del_breakpoints_by_modname_without_update_memory(const DebugCallbacks *cb, LPCTSTR modname);

    /* --- 硬件断点（hardware_breakpoints.cpp） --- */
    bool set_hardware_breakpoint(size_t ea, DWORD Index_of_register, char bpx_type, DWORD bpx_size, callback_ref_t callback);
    void enable_hardware_breakpoint(DWORD IndexOfRegister);
    void disable_hardware_breakpoint(DWORD IndexOfRegister);
    void del_hardware_breakpoint(DWORD IndexOfRegister);

    /* --- 进程 / 线程（process.cpp） --- */
    HANDLE get_thread_by_tid(DWORD tid);
    void   detach_process(const DebugCallbacks *cb);

    /* ------------------------- 进程树（T7 切片 C2b） -------------------------
     * plan_t7.md §10 第 1/3 条：一个会话的键是**一棵树**（根进程 + 它派生出来的全部进程）。
     * 「调试期间出现的 CREATE_PROCESS」一定归同一个调试器 ⇒ 那个 pid 属于本会话（一棵树只有一个
     * 调试器，不可能有第二个会话认领它）。
     *
     * `handle_for_pid()` 是这个集合的唯一用途：内存/断点补丁必须打在**属于那个 pid 的**句柄上
     * （旧实现一律 `proc().hProcess` ⇒ 子进程的断点补丁会写到根进程）。根 pid 直接用
     * `proc().hProcess`（调试事件给的句柄）；其它 pid 走 `OpenProcess`（`kernel32!OpenProcess`
     * 全 w95 可，见 plan_t7.md §10 第 3 条）并**缓存**（pid→HANDLE 小表，进程退出 / 会话析构时关）。
     * 9x 不认 `PROCESS_QUERY_INFORMATION` ⇒ 沿用 `util.cpp` 的降级重试（去掉那一项再试一次）。 */
    bool   owns_pid(DWORD pid) const noexcept;
    HANDLE handle_for_pid(DWORD pid);
    int    pid_count() const noexcept { return m_pid_count; }
    DWORD  pid_at(int index) const noexcept;   /* 越界返回 0；插入序 = 根在前 */

    /* --- 栈 / C++ 类名（stack_trace.cpp） --- */
    bool get_cpp_class_name(size_t addr, char *out, size_t cap, bool *has_name);

    /* --- 调试主循环（debug_loop.cpp） --- */
    int run(const RunParams *params, const DebugCallbacks *cb, const char **err);

    /* T7.1（plan_t7.md §2.1）：`run` 就是下面四件套的封装（begin + while + pump(autocont = true) + end）。
     * 返回值口径（与 run 一致）：0 = 正常；-1 = 引擎内部错误（`*err` 已置，那条路径已自行跑收尾）；
     * `begin` 另用 -2 上报「附加失败但 1:1 不跑收尾」。`pump` 返回调试事件码（0 = 没等到事件）。
     * ❗`begin` 存的是调用方的 `params` / `cb` 指针 —— 调用方必须保证它们活过整个状态机。 */
    int  begin(const RunParams *params, const DebugCallbacks *cb, const char **err);
    int  pump(DWORD timeout_ms, bool autocont, const char **err);   /* = wait_one + dispatch；autocont = false ⇒ ❗不 continue；timeout_ms 按 ≤250 ms 切片（T7.3） */
    int  pump_now(const char **err);                                /* pump(0, false) */
    /* T7 切片 C2a（plan_t7.md §10 / D57）：把「读一条事件」与「分派一条事件」拆开，让宿主层能做
     * **进程级的按归属分派**（`WaitForDebugEvent` 是进程级的，一个进程只有一个事件流）。
     * `wait_one` 只做「≤250 ms 切片等待 + `on_wait_timeout`（D32 唯一的 poll 点）+ detach/取消路径」，
     * 成功时填 `*out` 并返回 `dwDebugEventCode`，没等到返回 0，引擎错返回 -1。
     * ❗它作用于 `this`（`proc()` / `m_debug_active_process_stop` / `term()` / `to_detach()`），但**不**按
     *   `params->pid` 过滤 —— 所以在任意会话实例上调都能读到事件，宿主靠 `ev.dwProcessId` 找归属。
     * `dispatch` 是 `wait_one` 之后的那一整段（回调 → switch → 记账 → `autocont` 时 continue）。 */
    int  wait_one(DWORD timeout_ms, DEBUG_EVENT *out, const char **err);
    int  dispatch(const DEBUG_EVENT &ev, bool autocont);            /* -1 = 引擎内部错误（文案见 `last_error()`） */
    bool cont(DWORD pid, DWORD tid, DWORD code);                    /* ContinueDebugEvent（code 显式） */
    int  end(const char **err);                                     /* 收尾；`to_detach()` 为真时先做真 detach（返回 1 = 无 DebugActiveProcessStop，已降级成 kill） */
    /* T7.3（plan_t7.md §4.1 / 已拍板第 5 条）：pump 算出来的「本来该用的 code」存在会话里，绑定层读它。
     * `pump` 的返回值只表达事件码（0 = 没等到事件）；「没等到事件」与「循环已终止」在返回值上
     * 不可区分（§2.1 记录 ②），所以 code 单独存一份 —— `cont` 必须显式带它（`pump.default-code-parity`）。 */
    DWORD last_code() const noexcept { return m_last_code; }
    /* C2a：`dispatch` 的内部错误文案（只有它返回 -1 时才有意义）。`pump` 把它转成自己的 `*err` ——
     * `dispatch` 不带 `err` 形参是为了让它与事件一一对应（一个事件一次分派，没有 second output）。 */
    const char *last_error() const noexcept { return m_last_error; }
private:
    /* `run` 的收尾（P6.4：随 run 一起进会话，使用会话自身的状态） */
    void run_shutdown(const RunParams *params);

    PROCESS_INFORMATION     m_pi{};
    common::OwnedIntrusiveList<PEInfo>         m_peinfos{};
    common::OwnedIntrusiveList<ThreadInfo>     m_threads{};
    common::OwnedIntrusiveList<BreakpointInfo> m_breakpoints{};
    HardwareBreakpointInfo m_hw_breakpoints[4]{};
    bool                   m_cont{};
    bool                   m_term{};
    bool                   m_to_detach{};

    /* -------------- T7.1：跨 pump 的循环状态（原 `run` 的栈局部）--------------
     * 这些是「原来在 while 的栈帧里、pump 之间必须保留」的东西（§11.5 / PITFALLS M10）。 */
    const RunParams      *m_params{};   /* begin 存下的调用方对象（与调用期同寿命） */
    const DebugCallbacks *m_cb{};
    bool m_first_bp{};                  /* 原 `FirstBPX`：首个 EXCEPTION_BREAKPOINT 走 DBG_CONTINUE */
    bool m_push_fb{};                   /* 原 `pushfbpx`：pushf 特殊情况（EFlags 被置了就要清） */
    int  m_reset_hw_bp{};               /* 原 `reset_hwbpx`：DR0-3 的位掩码（单步后 re-arm） */
    bool m_first_create_process{};      /* 原 `first_create_process`（旧 `dbgfilehandle` 的实际语义） */

    /* 原 `reset_bpx {modname, rva, ea}`：待 re-arm 的软件断点（`modname == NULL` = 没有）。
     * ❗D32：显式初始化必须在构造函数或 `begin`（「武装后首次访问必定漏掉」不能复发）。 */
    struct ResetBpx
    {
        LPCTSTR modname;
        DWORD   rva;
        size_t  ea;
    };
    ResetBpx m_reset_bpx{};

    /* `DebugActiveProcessStop`（NT4+，Win95/98 没有）：在 `begin` 里解析一次；取不到 → D47 的
     * 降级（只能 terminate）。这里按 C 名字保留（与 `bindings.cpp` 的 `lua_detach` 同一口径）。 */
    typedef BOOL(WINAPI *DebugActiveProcessStopFn)(DWORD dwProcessId);
    DebugActiveProcessStopFn m_debug_active_process_stop{};

    /* T7.3：`pump` 每处理一个事件就把算出来的 code 存这里（绑定层读它做显式应答；
     * 没等到事件时不更新，读到的是上一次的 code）。 */
    DWORD m_last_code{};

    /* C2a：`dispatch` 的内部错误文案（每次分派开头清空）。 */
    const char *m_last_error{};

    /* ---------------- T7 切片 C2b：进程树的 pid 集合 + 按 pid 的句柄缓存 ----------------
     * 两个数组的容量是同一个上限（一棵树里超过 16 个进程就只剩根进程的句柄能取，其余会在
     * `handle_for_pid()` 里失败并报错）—— 零 CRT 下不为它引入容器；会话数也是个位数。 */
    static const int kMaxPids = 16;

    void add_pid(DWORD pid) noexcept;          /* 已存在 / 满 / pid == 0 ⇒ 无操作 */
    void close_pid_handle(DWORD pid) noexcept; /* 关掉缓存里那个 pid 的句柄（进程退出） */
    void close_pid_handles() noexcept;         /* 全关（`run_shutdown` / 析构） */

    DWORD m_pids[kMaxPids]{};
    int   m_pid_count{};
    /* pid → HANDLE（pid == 0 = 空槽）。根 pid 不进这里（它是 `proc().hProcess`，归调试事件）。 */
    struct PidHandle
    {
        DWORD  pid;
        HANDLE handle;
    };
    PidHandle m_pid_handles[kMaxPids]{};
};

/* ---------------- 当前会话（P3.B 模块 11 引入；P6.4 后范围收窄） ----------------
 * 引擎函数**已是本类成员**（见上面的「引擎 API」段），所以下面这对函数只剩两个用途：
 *   ① 宿主（Lua 绑定 / 单测）把自己的会话实例挂出来，供仍按「当前会话」语义工作的调用方使用；
 *   ② 签名固定的回调（dbghelp 的 `StackTrace_*`、`hide.cpp` 的自由入口）从那里取会话。
 * ❗不是单例：类本身可多实例（`tests/unit/test_session.cpp` 已证明互不干扰；
 *   `test_debug_loop.cpp` 就是在**自己的会话实例**上跑 `run`）。
 * `session` 为 nullptr 时 `current_session()` 会断言失败（fail-fast）。 */
void            set_current_session(DebuggerSession *session) noexcept;
DebuggerSession &current_session() noexcept;
/* 与 `current_session()` 同源，但**允许宿主还没挂上**（返回 nullptr 而不断言）。
 * 用途：宿主层要在「自己也可能没挂」的时刻记一份「宿主会话」—— `dbg` 的收尾回退链
 *   （`scripting/lua/dbg_bindings.cpp` 的 `g_host_sess`）就是这样取的：`--server` 口径下
 *   `exec.cpp` 是 `create_state_with_libs()` **之后**才 `set_current_session()`。 */
DebuggerSession *current_session_or_null() noexcept;
}

#endif /* AUTODBG_CORE_CORE_HPP */
