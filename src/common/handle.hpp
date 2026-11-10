#ifndef AUTODBG_COMMON_HANDLE_HPP
#define AUTODBG_COMMON_HANDLE_HPP
/* =============================================================================
 * src/common/handle.hpp —— 最小 HANDLE RAII 包装（Plan P6.1）
 *
 * 目的：把散落的 `CloseHandle`（含「某个 error 分支忘了关」的历史写法）收敛成
 *   「作用域退出即关闭」。用法：`common::unique_handle h(CreateFile(...));` 之后正常用
 *   `h.get()`；需要把所有权交出去时 `h.release()`。
 *
 * ❗**不使用 C++ 标准库**（没有 `<memory>`/`std::unique_ptr`）：本项目禁用标准库，且要在
 *   `/nodefaultlib`（零 CRT）下工作。实现只有几十行，语义也更好把握。
 *
 * 语义：
 *   · 默认构造 = 空；`INVALID_HANDLE_VALUE`（CreateFile 失败值）也视为空；
 *   · 不可拷贝；只允许**显式**移动 / `release()` / `reset()` —— 避免隐式所有权歧义；
 *   · 析构时若持有有效句柄 → `CloseHandle`（失败忽略，与旧代码一致）；
 *   · `NULL` 与 `INVALID_HANDLE_VALUE` **不**调用 CloseHandle（旧代码就有这两种约定）。
 * ========================================================================== */
#include <windows.h>

namespace autodbg::common
{
/* `INVALID_HANDLE_VALUE` 与 `NULL` 都算「空」——两类 API 的失败值不同（CreateFile vs
 * CreateProcess/OpenProcess），旧代码在各处也按各自约定判断。 */
inline bool handle_valid(HANDLE h) noexcept
{
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

class unique_handle
{
public:
    unique_handle() noexcept = default;
    explicit unique_handle(HANDLE h) noexcept : m_h(handle_valid(h) ? h : nullptr) {}
    ~unique_handle() noexcept { close(); }

    unique_handle(const unique_handle &)            = delete;
    unique_handle &operator=(const unique_handle &) = delete;

    unique_handle(unique_handle &&other) noexcept : m_h(other.m_h) { other.m_h = nullptr; }
    unique_handle &operator=(unique_handle &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_h       = other.m_h;
            other.m_h = nullptr;
        }
        return *this;
    }

    HANDLE get() const noexcept { return m_h; }
    explicit operator bool() const noexcept { return m_h != nullptr; }

    /* 交出所有权（调用方负责关闭） */
    HANDLE release() noexcept
    {
        HANDLE h = m_h;
        m_h      = nullptr;
        return h;
    }

    /* 换一个句柄（先关掉旧的） */
    void reset(HANDLE h = nullptr) noexcept
    {
        close();
        m_h = handle_valid(h) ? h : nullptr;
    }

private:
    void close() noexcept
    {
        if (m_h)
            CloseHandle(m_h);
        m_h = nullptr;
    }

    HANDLE m_h = nullptr;
};
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_HANDLE_HPP */
