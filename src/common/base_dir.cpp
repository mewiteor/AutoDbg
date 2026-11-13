/* =============================================================================
 * src/common/base_dir.cpp —— 基准目录的读取与「相对路径 → 基准目录」的拼接（T8 切片 a2 / D63）
 *
 * 职责：`base_dir()`（env `AUTODBG_BASE_DIR` 优先，否则主 exe 所在目录）+ 两条纯函数
 *       （`path_is_absolute`、`join_base_dir`）。判定与 I/O 分开，是因为前者要在 `--selftest` 里
 *       逐字节断言，后者拿不到缓存值也不影响断言。
 * 来源：新写（plan_bootloader.md §5 / §7「脚本相对路径基准」条目；口径与 `handoff.cpp` 的头注释配对：
 *       主 exe 设 env，这里读 env）。
 * 与旧实现的差异：以前相对路径就是 CWD，没有「基准目录」这一层。
 *
 * 零 CRT：不 `sprintf`、不用 `std::`；字符复制自己写（同 `handoff.cpp`）。
 * 符号口径（`tools/api_check/datas.bin` 的 w95 段实测）：`GetEnvironmentVariableA`、
 *   `GetModuleFileNameA` 全 w95 有 ⇒ 直接链，不需要 dynapi。缓存是**文件作用域的 POD 数组**
 *   （§11.5 第 1 条禁的是 `thread_local` 与「函数内非平凡构造的 static」，POD 数组不受影响）。
 * ========================================================================== */
#include <windows.h>

#include "base_dir.hpp"

namespace autodbg::common
{
namespace
{
constexpr unsigned k_base_cap = 512;

char s_base[k_base_cap];
bool s_base_cached = false;

/* 逐字节复制（限 cap，越界不写） */
bool copy_capped(const char *src, char *out, unsigned cap)
{
    unsigned n = 0;
    while (src[n] != '\0')
    {
        if (n + 1 >= cap)
            return false;
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';
    return true;
}

bool last_is_sep(const char *s)
{
    unsigned n = 0;
    while (s[n] != '\0')
        n++;
    if (n == 0)
        return false;
    return s[n - 1] == '\\' || s[n - 1] == '/';
}

/* 主 exe 所在目录（不带尾分隔符）：自己切，不用 `PathFindFileNameA`（它在 shlwapi，会新增导入库） */
const char *exe_dir(char *out, unsigned cap)
{
    char exe[k_base_cap];
    if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe) == 0)
        return "";

    unsigned cut = 0;
    for (unsigned i = 0; exe[i] != '\0'; i++)
    {
        if (exe[i] == '\\' || exe[i] == '/')
            cut = i;          /* 分隔符本身不要（已定：base_dir 不带尾分隔符） */
    }
    if (cut == 0)
        return "";

    if (cut + 1 > cap)
        return "";
    for (unsigned i = 0; i < cut; i++)
        out[i] = exe[i];
    out[cut] = '\0';
    return out;
}

} /* namespace（匿名） */

bool path_is_absolute(const char *path)
{
    if (path == nullptr || path[0] == '\0')
        return false;

    /* UNC：\\server\share */
    if (path[0] == '\\' && path[1] == '\\')
        return true;

    /* 盘符式：字母 + ':'（`C:\x`、`C:/x`，以及盘符相对的 `C:foo` —— 一律原样透传） */
    char c = path[0];
    bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return letter && path[1] == ':';
}

bool join_base_dir(const char *base, const char *path, char *out, unsigned cap)
{
    if (path == nullptr || out == nullptr || cap == 0)
        return false;

    if (path_is_absolute(path) || base == nullptr || base[0] == '\0')
        return copy_capped(path, out, cap);

    unsigned bl = 0;
    while (base[bl] != '\0')
        bl++;

    unsigned pl = 0;
    while (path[pl] != '\0')
        pl++;

    unsigned need = bl + (last_is_sep(base) ? 0u : 1u) + pl + 1u;
    if (need >= cap)
        return false;

    unsigned n = 0;
    for (unsigned i = 0; i < bl; i++)
        out[n++] = base[i];
    if (!last_is_sep(base))
        out[n++] = '\\';
    for (unsigned i = 0; i < pl; i++)
        out[n++] = path[i];
    out[n] = '\0';
    return true;
}

const char *base_dir()
{
    if (s_base_cached)
        return s_base;

    s_base_cached = true;

    /* env 优先（handoff 的那半份由主 exe 设）；空 = 没有，回退主 exe 目录 */
    unsigned n = (unsigned)GetEnvironmentVariableA("AUTODBG_BASE_DIR", s_base, k_base_cap);
    if (n == 0)
    {
        exe_dir(s_base, k_base_cap);
        return s_base;
    }

    /* 已定：env 值不带尾分隔符。这里只做「防御性吸收」——调用方给了尾分隔符也不会在拼接时翻倍。 */
    while (n > 0 && (s_base[n - 1] == '\\' || s_base[n - 1] == '/'))
        s_base[--n] = '\0';

    return s_base;
}

} /* namespace autodbg::common */
