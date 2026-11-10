/* =============================================================================
 * src/scripting/lua/helpers.cpp —— 从 src/debugger/lib.c 1:1 移植（见 helpers.hpp）
 *   仅做机械替换：`alloc_`/`free_` → `common::alloc/release`、`utf8_to_utf16` → `core::`。
 *   行为一致性由 tests/unit/test_diff_lib.cpp 的差分用例保证。
 * ========================================================================== */
#include "helpers.hpp"
#include "../../core/util.hpp"

#include <assert.h>
#include <intrin.h>
#include <stdint.h>
#include <stdlib.h>

namespace autodbg::scripting::lua
{
LPTSTR get_lua_str(lua_State *L, int idx)
{
    const char *str = luaL_checkstring(L, idx);
#ifdef _UNICODE
    return core::utf8_to_utf16(str, NULL);
#else
    return _strdup(str);
#endif
}

int lua_type_error(lua_State *L, int arg, const char *tname)
{
    const char *msg;
    const char *typearg;                                                                   /* name for the type of the actual argument */
    if (luaL_getmetafield(L, arg, "__name") == LUA_TSTRING) typearg = lua_tostring(L, -1); /* use the given type name */
    else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA) typearg = "light userdata";           /* special name for messages */
    else typearg = luaL_typename(L, arg);                                                  /* standard name */
    msg = lua_pushfstring(L, "%s expected, got %s", tname, typearg);
    return luaL_argerror(L, arg, msg);
}

void str2integer(const char *s, int bs, bool sign, void *v)
{
    assert(bs > 0 && bs <= 8 && !(bs & bs - 1));
    __stosb((PBYTE)v, 0, bs);
    int neg = 0, base = 10, d;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' || *s == '\f') s++;
    if (sign && *s == '-')
    {
        neg = 1;
        s++;
    }
    else if (*s == '+') s++;
    if (*s == '0')
    {
        base = (s[1] == 'x' || s[1] == 'X') ? 16 : 8;
        if (base == 16) s += 2;
    }
    for (; *s; s++)
    {
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        switch (bs)
        {
        case 1: *(uint8_t *)v = *(uint8_t *)v * base + d; break;
        case 2: *(uint16_t *)v = *(uint16_t *)v * base + d; break;
        case 4: *(uint32_t *)v = *(uint32_t *)v * base + d; break;
        case 8: *(uint64_t *)v = *(uint64_t *)v * base + d; break;
        }
    }
    if (neg) switch (bs)
        {
        case 1: *(int8_t *)v = -*(int8_t *)v; break;
        case 2: *(int16_t *)v = -*(int16_t *)v; break;
        case 4: *(int32_t *)v = -*(int32_t *)v; break;
        case 8: *(int64_t *)v -= *(int64_t *)v; break;
        }
}

/* ---------------------------------------------------------------- P4.3：回调派发辅助
 * 从 bindings.cpp 原样搬入（新增 `file` 形参 —— 就是调用点的 `__FILE__`，由 `PCALL` 宏传入，
 * 保证日志里的文件名与旧实现逐字一致）。 */
int pcall_pre(lua_State *L)
{
    lua_getglobal(L, "debug");
    lua_getfield(L, -1, "traceback");
    lua_remove(L, -2);
    return lua_gettop(L);
}

bool pcall(lua_State *L, int narg, int nresult, int oldtop, LPCTSTR file, int line)
{
    int r = lua_pcall(L, narg, nresult, oldtop);
    switch (r)
    {
    case 0        : assert(lua_gettop(L) == oldtop + nresult); return true;
    case LUA_YIELD: core::log_msg(file, line, TEXT("yield")); break;
    case LUA_ERRRUN:
        assert(lua_gettop(L) == oldtop + 1);
        core::log_msg(file, line, TEXT("run error: type: %hs"), luaL_checkstring(L, -1));
        lua_pop(L, 1);
        return false;
    case LUA_ERRSYNTAX: core::log_msg(file, line, TEXT("syntax error")); break;
    case LUA_ERRMEM   :
        assert(lua_gettop(L) == oldtop + 1);
        core::log_msg(file, line, TEXT("memory error: %hs"), luaL_checkstring(L, -1));
        lua_pop(L, 1);
        return false;
    case LUA_ERRERR   : core::log_msg(file, line, TEXT("error")); break;
    default           : core::log_msg(file, line, TEXT("r: %d"), r); break;
    }
    core::log_msg(file, line, TEXT("top: %d, %d"), oldtop, lua_gettop(L));
    lua_settop(L, oldtop);
    return false;
}
}
