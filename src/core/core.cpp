/* =============================================================================
 * src/core/core.cpp —— `DebuggerSession` 的构造/析构（P3.A.2）
 *
 * 内容对应旧的 `init()` / `release()`：**逐字保留**释放顺序与范围，
 *   包括「`func_chunks` 不释放」这一现状（该字段全仓库从未被分配，属死字段，
 *   P6 清理时再删；此处保持 1:1，见 src/core/types.hpp 的说明）。
 * ========================================================================== */
#include "core.hpp"

#include "../common/allocator.hpp"

#include <assert.h>

namespace autodbg::core
{
/* 当前会话（模块 11 的过渡期状态，见 core.hpp 的说明）。
 * 宿主（绑定层 / 单测）用 `set_current_session()` 把自己的实例挂上来 —— 所有权在宿主，
 * 本文件**不**拥有也不负责释放它（因此不是单例）。 */
namespace
{
DebuggerSession *g_current_session = nullptr;
}

void set_current_session(DebuggerSession *session) noexcept
{
    g_current_session = session;
}

DebuggerSession &current_session() noexcept
{
    assert(g_current_session != nullptr); /* fail-fast：宿主忘了 set_current_session() */
    return *g_current_session;
}

DebuggerSession::DebuggerSession() noexcept
{
    /* 原 init()：三个链表头置空（现在由 `OwnedIntrusiveList` 的默认构造负责，无需手写） */
    /* 硬件断点的回调令牌置为「无」。
     * 旧实现这里是静态全局量 → 实际值是 0；换成 `void*` 后 0 就是 `nullptr`，
     * 语义与当初那套整数 ref 的约定恰好一致（但不再需要依赖任何哨兵值）。
     * 该字段只在 `activate` 置位的同时被设置/读取（单线程 run，无并发交错），
     * 所以显式初始化是**行为中性**的。 */
    for (int i = 0; i < 4; i++)
        m_hw_breakpoints[i].callback = callback_ref_t{};
}

DebuggerSession::~DebuggerSession() noexcept
{
    /* 原 release()：PE 链表 → 线程链表 → 断点链表
     * ❗两点变化（P6.2/P6.4）：
     *   ① 节点释放收归 `OwnedIntrusiveList::clear()`（不再是手写 while + release）；
     *   ② 线程节点的 `hThread` 在这里补关 —— 旧实现从不关（D29 的尾巴）。
     *      但**要避开 `g_pi` 那两个句柄**：主线程的 hThread 与 `m_pi.hThread` 可能是同一个值
     *      （`run_shutdown` 已经关过），再关一次可能误关到已被复用的句柄。 */
    for (ThreadInfo *t = m_threads.head(); t; t = t->next)
    {
        if (t->hThread && t->hThread != m_pi.hThread && t->hThread != m_pi.hProcess)
            CloseHandle(t->hThread);
    }
    m_threads.clear();

    while (m_peinfos.head())
    {
        PEInfo *p = m_peinfos.head();
        common::release(p->sections);
#ifdef _WIN64
        common::release(p->runtime_functions);
        common::release(p->parent_functions);
#endif
        m_peinfos.remove_and_free(p); /* ❗必须逐个释放：PEInfo 有额外分配的 sections/… */
    }
    m_breakpoints.clear();
}
}
