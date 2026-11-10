#ifndef AUTODBG_COMMON_JSON_WRITER_HPP
#define AUTODBG_COMMON_JSON_WRITER_HPP
/* =============================================================================
 * src/common/json_writer.hpp —— 极简 JSON 输出（只写不解析）
 *
 * 来源：新写（plan_server.md S0 / D25 / D28）。
 * 职责：给 server 的响应信封（§4.5）与将来的 JSON-RPC 结果提供**零依赖**的 JSON 序列化。
 *   **只写不解析**：请求里的 JSON 由 Lua 层的 `jsonx.decode` 处理（D28），C 侧不写 parser。
 * 约束：零 CRT + 无 STL（缓冲走 `common::alloc/release`）；不抛异常（项目禁 C++ 异常）；
 *   所有边界都检查，超限时**停止追加**并置 `overflow()`，绝不越界。
 * 与旧实现的差异：无（新模块）。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
class json_writer
{
public:
    json_writer() noexcept;
    explicit json_writer(size_t cap) noexcept;
    ~json_writer() noexcept;

    json_writer(const json_writer &) = delete;
    json_writer &operator=(const json_writer &) = delete;

    void reset() noexcept;

    const char *data() const noexcept { return m_buf; }
    size_t      size() const noexcept { return m_len; }
    bool        overflow() const noexcept { return m_overflow; }

    /* ---- 结构（自动补逗号与冒号；层级超过 k_max_depth 即 overflow） ---- */
    void begin_object() noexcept;
    void end_object() noexcept;
    void begin_array() noexcept;
    void end_array() noexcept;
    void key(const char *name) noexcept;
    void key(const char *name, size_t n) noexcept;

    /* ---- 值 ----
     * ❗数字有三个重载（`long long` / `unsigned long long` / `double`）：传**字面量**时要写清
     * 类型（`w.number(1LL)`），否则 `int` → 三者都是同级别的转换 → C2668 重载歧义。 */
    void string(const char *s) noexcept;            /* NUL 结尾 */
    void string(const char *s, size_t n) noexcept;  /* 任意字节，做 JSON 转义 */
    void number(long long v) noexcept;
    void number(unsigned long long v) noexcept;
    void number(double v) noexcept;
    void boolean(bool v) noexcept;
    void null_value() noexcept;

    /* 原样片段（调用方保证是合法 JSON；给「Lua 侧 jsonx.encode 的结果」用） */
    void raw(const char *json, size_t n) noexcept;

private:
    enum class container : char
    {
        root   = 0,
        object = 1,
        array  = 2,
    };

    static constexpr unsigned k_max_depth = 16;

    container top() const noexcept;
    void      before_value() noexcept; /* 父容器里补逗号 */
    void      put(char c) noexcept;
    void      put(const char *s, size_t n) noexcept;
    void      put_escaped(const char *s, size_t n) noexcept;
    void      put_u64(unsigned long long v) noexcept;
    bool      reserve(size_t extra) noexcept;

    char     *m_buf      = nullptr;
    size_t    m_len      = 0;
    size_t    m_cap      = 0;
    bool      m_overflow = false;
    unsigned  m_depth    = 0;
    container m_stack[k_max_depth + 1]   = {};
    bool      m_need_comma[k_max_depth + 1] = {};
};
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_JSON_WRITER_HPP */
