/* =============================================================================
 * src/scripting/lua/last_pid.cpp —— 「上次附加的 PID」持久化（宿主层，P3.B 模块 12）
 *
 * 逐字搬自 `tools.c`（C → C++）。相对旧实现的改动只有三类，都在本文件注释里标注：
 *   ① `L` 形参去掉（D2：全程未使用），脚本路径由调用方传入；
 *   ② 错误/打印宏从 `legacy_lib.h`（C 版）换成 `core/util.hpp`（同一套 `E`/`ERR`/`ErrorF`
 *      语义，只是定义在 `autodbg::core`）→ 报错文案与行号来源（__FILE__/__LINE__）不变；
 *   ③ `g_wfilepath` 由全局量改为参数。
 * ========================================================================== */
#include "last_pid.hpp"

#include "core/types.hpp" /* MAX_FILEPATH / TCHAR 等（与产品其它 TU 同一份定义） */
#include "common/handle.hpp" /* P6.1：HANDLE RAII */
#include "core/util.hpp"

#include <assert.h>
#include <shlwapi.h>
#include <strsafe.h>

using autodbg::core::ErrorF;
using autodbg::common::unique_handle; /* P6.1 */

/* suffix 形如 ".attach.last.pid.bin"：把 script_path 的扩展名替换掉 */
static bool last_pid_path(LPTSTR path, size_t size, LPCTSTR script_path, LPCTSTR suffix)
{
    HRESULT hr;
    if (script_path[0] == TEXT('.') && script_path[1] == TEXT('\\'))
        hr = StringCchCopy(path, size, script_path + 2);
    else
        hr = StringCchCopy(path, size, script_path);
    if (FAILED(hr))
    {
        ERR("StringCchCopy", hr);
        return false;
    }
    LPTSTR ext = PathFindExtension(path);
    if (!ext)
    {
        size_t len;
        hr = StringCchLength(path, size, &len);
        if (FAILED(hr))
        {
            ERR("StringCchLength", hr);
            return false;
        }
        ext = path + len;
    }
    assert(!!ext);
    hr = StringCchCopy(ext, size - (ext - path), suffix);
    if (FAILED(hr))
    {
        ERR("StringCchCopy", hr);
        return false;
    }
    return true;
}

DWORD get_last_pid(LPCTSTR script_path, LPCTSTR suffix)
{
    TCHAR path[MAX_FILEPATH];
    if (!last_pid_path(path, _countof(path), script_path, suffix)) return 0;
    if (!PathFileExists(path))
    {
        ErrorF(TEXT("last pid file \"%s\" not found!\n"), path);
        return 0;
    }
    unique_handle hFile(CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile)
    {
        E("CreateFile(\"%s\")", path);
        return 0;
    }
    DWORD pid  = 0;
    DWORD size = 0;
    if (!ReadFile(hFile.get(), &pid, sizeof pid, &size, NULL))
    {
        E("ReadFile(\"%s\")", path);
        pid = 0;
    }
    else if (size != sizeof pid)
    {
        ErrorF(TEXT("read \"%s\" failed!\n"), path);
        pid = 0;
    }
    return pid;
}

void save_last_pid(LPCTSTR script_path, DWORD pid, LPCTSTR suffix)
{
    TCHAR path[MAX_FILEPATH];
    if (!last_pid_path(path, _countof(path), script_path, suffix)) return;
    unique_handle hFile(CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile)
    {
        E("CreateFile(\"%s\")", path);
        return;
    }
    DWORD size = 0;
    if (!WriteFile(hFile.get(), &pid, sizeof pid, &size, NULL)) E("WriteFile(\"%s\")", path);
    else if (size != sizeof pid) ErrorF(TEXT("write \"%s\" failed!\n"), path);
}
