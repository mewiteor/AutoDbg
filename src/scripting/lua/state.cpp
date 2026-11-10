/* =============================================================================
 * src/scripting/lua/state.cpp —— Lua 状态与脚本加载（P4.1，从 bindings.cpp 原样搬出）
 *
 * 内容：`g_wfilepath` 定义 / `my_loadfile()` / `run_script_file()` / `create_state_with_libs()`
 *       / `lua_reset_dir()`。
 * 与旧实现的差异仅三处（其余逐字）：
 *   ① `_tmain` 里「建状态 → 注册 → 烘焙库 → 加载 → 跑脚本 → 打印错误 → 关闭」这段收进
 *      `run_script_file()`（逐字搬入，只是不再嵌在入口函数里）；`_tmain` 改为调它。
 *   ② 文件头与包含调整（`datas.h` 的 `all_bytecodes`、`registry.hpp`、`core/util.hpp`）。
 *   ③ **`generate_templete()` 已删除**：原先生成在脚本目录旁的 `cdebugger.d.lua` +
 *      `.luarc.json` 改由编辑器侧提供（neovim 插件的 LuaLS 注解库，见插件仓库的
 *      `addons/autodbg/` 与 `lua/autodbg/lsp.lua`）。模块因此不再往脚本目录写文件。
 * ========================================================================== */
#include "state.hpp"

#include "registry.hpp" /* register_lua_api（P4.2 搬出） */

#include "core/types.hpp" /* MAX_FILEPATH / TCHAR（类型统一后只有这一份） */
#include "core/util.hpp"

#include "common/handle.hpp" /* P6.1：HANDLE RAII */

#include <intrin.h> /* ZeroMem/CopyMem 宏（__stosb/__movsb）——`RemoveAllFiles` 用到 */
#include <shlwapi.h>
#include <strsafe.h>

/* 选择性 using：旧调用点（`Error*` / `utf16_to_utf8` / `utf8_to_utf16_n` / 宏 `E`/`alloc_`）
 * 原先来自 legacy_lib.h（C 版，全局命名空间），现在分属 `autodbg::core`（Plan 5.7）。 */
using autodbg::core::Error;
using autodbg::core::ErrorF;
using autodbg::core::ErrorN;
using autodbg::core::ErrorS;
using autodbg::core::utf8_to_utf16;
using autodbg::core::utf16_to_utf8;
using autodbg::core::utf8_to_utf16_n;
using autodbg::common::unique_handle; /* P6.1 */

/* extern "C" 花括号形式 = 定义（Q7：脚本路径属 Lua 层状态，不进会话） */
extern "C"
{
LPCTSTR g_wfilepath;
}

extern "C"
{
#include <datas.h> /* all_bytecodes：烘焙进 exe 的 Lua 库 */
}

int my_loadfile(lua_State *L, LPCTSTR path)
{
    /* P6.1：句柄用 RAII（原先 6 条 return 路径上散着 6 处 CloseHandle） */
    unique_handle hFile(CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile)
    {
        E("CreateFile");
        return -1;
    }
    ULARGE_INTEGER fileSize;
    fileSize.LowPart = GetFileSize(hFile.get(), &fileSize.HighPart);
    if (fileSize.LowPart == INVALID_FILE_SIZE)
    {
        DWORD code = GetLastError();
        if (code)
        {
            ERR("GetFileSize", code);
            return -1;
        }
    }
    if (!fileSize.QuadPart)
    {
        ErrorF(TEXT("\"%s\" size == 0\n"), path);
        return -1;
    }
#ifndef _WIN64
    if (fileSize.HighPart)
    {
        ErrorF(TEXT("\"%s\" size to large\n"), path);
        return -1;
    }
#endif
    unique_handle hMapping(CreateFileMapping(hFile.get(), NULL, PAGE_READONLY, 0, 0, NULL));
    if (!hMapping)
    {
        E("CreateFileMapping");
        return -1;
    }
    LPCSTR buf = (LPCSTR)MapViewOfFile(hMapping.get(), FILE_MAP_READ, 0, 0, 0);
    if (!buf)
    {
        E("MapViewOfFile");
        return -1;
    }
    LPSSTR name = tchar_to_utf8(path);
#ifdef _WIN64
    int r = luaL_loadbufferx(L, buf, fileSize.QuadPart, name, "t");
#else
    int r = luaL_loadbufferx(L, buf, fileSize.LowPart, name, "t");
#endif
    UnmapViewOfFile(buf);
    free_from_tchar(name);
    return r;
}

/* ---------------------------------------------------------------- 脚本目录工具（Q7）
 * `RemoveAllFiles` / `lua_reset_dir` 从 bindings.cpp 搬来：它们与调试无关（只操作脚本目录），
 * 按 Plan 5.7 / Q7 归「脚本状态」层。逻辑逐字保留（含失败时的 `E(...)` 与布尔返回）。 */

static bool RemoveAllFiles(LPCTSTR dir)
{
    WIN32_FIND_DATA wfd;
    ZeroMem(wfd);
    size_t n = _tcslen(dir);
    TCHAR  buf[0x8000];
    CopyMemT(buf, dir, n);
    if (buf[n] != TEXT('/') && buf[n] != TEXT('\\'))
        buf[n++] = TEXT('\\');
    CopyTStr(buf + n, TEXT("*"));
    HANDLE hFindFile = FindFirstFile(buf, &wfd);
    if (INVALID_HANDLE_VALUE == hFindFile)
    {
        E("FindFirstFile(%s)", buf);
        return false;
    }
    do
    {
        if (!_tcsicmp(wfd.cFileName, TEXT(".")))
            continue;
        if (!_tcsicmp(wfd.cFileName, TEXT("..")))
            continue;
        size_t m = _tcslen(wfd.cFileName);
        CopyMemT(buf + n, wfd.cFileName, m + 1);
        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!RemoveAllFiles(buf))
            {
                CloseHandle(hFindFile);
                return false;
            }
        }
        else
        {
            if (!DeleteFile(buf))
            {
                E("DeleteFile(%s)", buf);
                CloseHandle(hFindFile);
                return false;
            }
        }
    } while (FindNextFile(hFindFile, &wfd));
    buf[--n] = 0;
    if (!RemoveDirectory(buf))
    {
        E("RemoveDirectory(%s)", buf);
        CloseHandle(hFindFile);
        return false;
    }
    CloseHandle(hFindFile);
    return true;
}

int lua_reset_dir(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "ResetDir(dir: string): nil");
        return lua_error(L);
    }
    LPXSTR root = utf8_to_tchar(lua_tostring(L, 1));
    if (PathFileExists(root))
    {
        if (PathIsDirectory(root))
        {
            if (!RemoveAllFiles(root))
            {
                free_from_tchar(root);
                lua_pushboolean(L, 0);
                return 1;
            }
        }
        else
        {
            if (!DeleteFile(root))
            {
                E("DeleteFile(%s)", root);
                free_from_tchar(root);
                lua_pushboolean(L, 0);
                return 1;
            }
        }
    }
    if (!CreateDirectory(root, NULL))
    {
        E("CreateDirectory(%s)", root);
        free_from_tchar(root);
        lua_pushboolean(L, 0);
    }
    else
    {
        free_from_tchar(root);
        lua_pushboolean(L, 1);
    }
    return 1;
}

/* 从 `_tmain` 搬来的引导 + 运行段（逐字；只把整段收进一个函数） */
/* =============================================================================
 * create_state_with_libs —— 建状态 → 注册 API → 烘焙内核库（S0，`plan_server.md` §5.1）
 *
 * 从 `run_script_file()` 的前半段**原样提出**（逐字一致）：server 的 `/run` 加载的是
 * **内存里的源码**（不落盘，D26），但状态初始化必须与文件路径完全相同，
 * 否则全局库/注册表会分叉。返回 nullptr = `luaL_newstate()` 失败。
 * ========================================================================== */
lua_State *create_state_with_libs(void)
{
    lua_State *L = luaL_newstate();
    if (L == nullptr)
        return nullptr;
    luaL_openlibs(L);
    register_lua_api(L); /* P4.2：注册表已搬到 registry.cpp（含 Var / FindMemoryContext 元表与 lv_init） */

    for (size_t i = 0; i < all_bytecodes_count; i++)
    {
        if (luaL_loadbuffer(L, (const char *)all_bytecodes[i].bytecode, all_bytecodes[i].len, all_bytecodes[i].path))
        {
            ErrorS(TEXT("加载 "));
            Error(all_bytecodes[i].name);
            ErrorS(TEXT(" 失败: "));
            size_t n;
            LPCSTR err = lua_tolstring(L, -1, &n);
#ifdef _UNICODE
            int    nn   = (int)n + 1;
            LPWSTR werr = utf8_to_utf16_n(err, &nn);
            ErrorN(werr, nn);
            free_(werr);
#else

            ErrorN(err, n);
#endif
            ErrorS("\n");
        }
        else
        {
            luaL_checktype(L, -1, LUA_TFUNCTION);
            lua_pcall(L, 0, 1, 0);
            lua_setglobal(L, all_bytecodes[i].name);
        }
    }
    return L;
}

int run_script_file(int argc, LPTSTR argv[])
{
    lua_State *L = create_state_with_libs();
    if (L == nullptr)
    {
        ErrorS(TEXT("luaL_newstate 失败\n"));
        return -1;
    }

    int r = my_loadfile(L, g_wfilepath);
    if (r > 0)
        ErrorS(L"加载\"");
    else
    {
        for (int i = 2; i < argc; i++)
        {
#ifdef _UNICODE
            int   n;
            LPSTR s = utf16_to_utf8(argv[i], &n);
            lua_pushlstring(L, s, n);
            free_(s);
#else
            lua_pushstring(L, argv[i]);
#endif
        }
        r = lua_pcall(L, argc > 2 ? argc - 2 : 0, LUA_MULTRET, 0);
        if (r > 0)
            ErrorS(L"运行\"");
    }
    if (r > 0)
    {
        Error(g_wfilepath);
        ErrorS(TEXT("\"失败: "));
        size_t n;
        LPCSTR err = lua_tolstring(L, -1, &n);
#ifdef _UNICODE
        int    nn   = (int)n + 1;
        LPWSTR werr = utf8_to_utf16_n(err, &nn);
        ErrorN(werr, nn);
        free_(werr);
#else

        ErrorN(err, n);
#endif
        ErrorS("\n");
    }
    lua_close(L);
    return r;
}
