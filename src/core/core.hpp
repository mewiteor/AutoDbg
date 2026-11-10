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

    bool &cont() noexcept { return m_cont; }           /* 原 autodbg::core::current_session().cont() */
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

    /* --- 内存（memory.cpp） --- */
    bool read(size_t addr, void *data, size_t size);
    bool write(size_t addr, const void *data, size_t size, bool up_intrin);
    bool update_memory(size_t addr, void *old_data, const void *new_data, size_t size, bool is_up_intrin);
    bool loadstr(size_t addr, char **pp, size_t *nn);
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

    /* --- 栈 / C++ 类名（stack_trace.cpp） --- */
    bool get_cpp_class_name(size_t addr, char *out, size_t cap, bool *has_name);

    /* --- 调试主循环（debug_loop.cpp） --- */
    int run(const RunParams *params, const DebugCallbacks *cb, const char **err);
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
}

#endif /* AUTODBG_CORE_CORE_HPP */
