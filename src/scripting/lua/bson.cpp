/* =============================================================================
 * src/scripting/lua/bson.cpp —— BSON 编码（P3.B 模块 12：原 bson.c/h，C → C++）
 *
 * 迁移口径：**只做「搬 + 改名 + 转 C++」**，语义 1:1（铁律 3）。
 * 相对旧 C 实现的改动仅四类，已就地标注：
 *   ① 文件名/头文件名（bson.* → bson.cpp/hpp）；
 *   ② `Buf` 与 `create_*` 的声明搬进 `bson.hpp`（旧 bson.h 只声明了两个 `lua_*`
 *      → C 单测不得不在自己文件里**复制**一份，test_bson.c 的注释早就写了这是 P3 该做的）；
 *   ③ `legacy_lib.h`（C 版工具宏）→ `core/util.hpp`；`realloc` → `common::resize`
 *      （同一 msvcrt `realloc`；OOM 时 fail-fast，与 alloc_/free_ 桥接策略一致）；
 *   ④ C 习惯转换：`void*` 显式转换、`free()` → `free_()`。
 * **对外符号保持 C 链接**（bson.hpp 有 extern "C" 护卫；tests/unit/test_bson.c 是 C 文件）。
 * ❗已知缺陷 1:1 保留：`create_bytes` 的 binary 分支多写 1 字节（TODO 9.1 D15，已被
 *   tests/unit/test_bson.c 锁成现状）；`lua_bson_decode` 仍是 stub（D8/F13）。
 * ========================================================================== */

#include "bson.hpp"

#include "core/util.hpp" /* alloc_ / free_ / CopyMem（旧 legacy_lib.h 的非 Lua 部分） */
#include "variant.hpp"   /* struct LuaVariant / lv_check */

#include <assert.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

using autodbg::common::resize;

void BufAppendChar(Buf *l, char r)
{
    l->s       = (char *)resize(l->s, l->n + 1);
    l->s[l->n] = r;
    l->n += 1;
}

void BufAppendStr(Buf *l, const char *r, uint32_t n)
{
    l->s = (char *)resize(l->s, l->n + n);
    CopyMem(l->s + l->n, r, n);
    l->n += n;
}

void BufAppend(Buf *l, const Buf *r) { BufAppendStr(l, r->s, (uint32_t)r->n); }

void BufFree(Buf *v)
{
    if (v)
    {
        free_(v->s);
        free_(v);
    }
}

Buf *create_nil(const char *key)
{
    Buf   *ret = AllocX(Buf, 1);
    size_t n   = strlen(key);
    ret->s     = (char *)alloc_(ret->n = n + 2);
    ret->s[0]  = 10;
    CopyMem(ret->s + 1, key, n + 1);
    return ret;
}

Buf *create_boolean(const char *key, bool value)
{
    Buf   *ret = AllocX(Buf, 1);
    size_t n   = strlen(key);
    ret->s     = (char *)alloc_(ret->n = n + 3);
    ret->s[0]  = 8;
    CopyMem(ret->s + 1, key, n + 1);
    ret->s[n + 2] = value ? 1 : 0;
    return ret;
}

#ifndef JIT
Buf *create_integer(const char *key, lua_Integer number)
{
    Buf   *ret      = AllocX(Buf, 1);
    size_t n        = strlen(key);
    bool   is_int32 = number == (int32_t)number;
    if (is_int32)
    {
        ret->s    = (char *)alloc_(ret->n = n + 6);
        ret->s[0] = 0x10;
        CopyMem(ret->s + 1, key, n + 1);
        *(int32_t *)&ret->s[n + 2] = (int32_t)number;
    }
    else
    {
        ret->s    = (char *)alloc_(ret->n = n + 10);
        ret->s[0] = 0x12;
        CopyMem(ret->s + 1, key, n + 1);
        *(lua_Integer *)&ret->s[n + 2] = number;
    }
    return ret;
}
#endif

Buf *create_number(const char *key, lua_Number number)
{
    Buf   *ret      = AllocX(Buf, 1);
    size_t n        = strlen(key);
    bool   is_int   = number == (int64_t)number;
    bool   is_int32 = number == (int32_t)number;
    if (is_int32)
    {
        ret->s    = (char *)alloc_(ret->n = n + 6);
        ret->s[0] = 0x10;
        CopyMem(ret->s + 1, key, n + 1);
        *(int32_t *)&ret->s[n + 2] = (int32_t)number;
    }
    else if (is_int)
    {
        ret->s    = (char *)alloc_(ret->n = n + 10);
        ret->s[0] = 0x12;
        CopyMem(ret->s + 1, key, n + 1);
        *(int64_t *)&ret->s[n + 2] = (int64_t)number;
    }
    else
    {
        ret->s    = (char *)alloc_(ret->n = n + 10);
        ret->s[0] = 1;
        CopyMem(ret->s + 1, key, n + 1);
        *(lua_Number *)&ret->s[n + 2] = number;
    }
    return ret;
}

Buf *create_lua_variant(const char *key, struct LuaVariant *lv)
{
    if (lv->type == LV_U64 && lv->val.u64 >= 0x8000000000000000ull) return NULL;

    Buf   *ret = AllocX(Buf, 1);
    size_t n   = strlen(key);
    switch (lv->type)
    {
    case LV_I8 :
    case LV_U8 :
    case LV_I16:
    case LV_U16:
    case LV_I32: ret->n = n + 6; break;
    case LV_I64:
    case LV_U32:
    case LV_U64:
    case LV_F32:
    case LV_F64: ret->n = n + 10; break;
    }
    ret->s = (char *)alloc_(ret->n);
    switch (lv->type)
    {
    case LV_I8 :
    case LV_U8 :
    case LV_I16:
    case LV_U16:
    case LV_I32: ret->s[0] = 0x10; break;
    case LV_I64:
    case LV_U32:
    case LV_U64: ret->s[0] = 0x12; break;
    case LV_F32:
    case LV_F64: ret->s[0] = 1; break;
    }
    CopyMem(ret->s + 1, key, n + 1);
    switch (lv->type)
    {
    case LV_I8 : *(int32_t *)&ret->s[n + 2] = lv->val.i8; break;
    case LV_U8 : *(int32_t *)&ret->s[n + 2] = lv->val.u8; break;
    case LV_I16: *(int32_t *)&ret->s[n + 2] = lv->val.i16; break;
    case LV_U16: *(int32_t *)&ret->s[n + 2] = lv->val.u16; break;
    case LV_I32: *(int32_t *)&ret->s[n + 2] = lv->val.i32; break;
    case LV_U32: *(int64_t *)&ret->s[n + 2] = lv->val.u32; break;
    case LV_I64: *(int64_t *)&ret->s[n + 2] = lv->val.i64; break;
    case LV_U64: *(int64_t *)&ret->s[n + 2] = lv->val.u64; break;
    case LV_F32: *(double *)&ret->s[n + 2] = lv->val.f32; break;
    case LV_F64: *(double *)&ret->s[n + 2] = lv->val.f64; break;
    }
    return ret;
}

Buf *create_bytes(const char *key, const char *s, uint32_t sn)
{
    Buf   *ret         = AllocX(Buf, 1);
    size_t n           = strlen(key);
    ret->s             = (char *)alloc_(ret->n = n + sn + 7);
    bool is_normal_str = true;
    for (uint32_t i = 0; i < sn && is_normal_str; i++)
        if (!s[i]) is_normal_str = false;
    ret->s[0] = is_normal_str ? 2 : 5;
    CopyMem(ret->s + 1, key, n + 1);
    if (is_normal_str)
    {
        *(uint32_t *)&ret->s[n + 2] = sn + 1;
        CopyMem(ret->s + n + 6, s, sn);
        ret->s[n + sn + 6] = 0;
    }
    else
    {
        *(uint32_t *)&ret->s[n + 2] = sn;
        ret->s[n+6] = 0;
        CopyMem(ret->s + n + 7, s, sn);
    }
    return ret;
}

Buf *create_item(const char *key, lua_State *L, int n);

Buf *create_table(lua_State *L, int n, bool *is_arr)
{
    int old_top = lua_gettop(L);
    *is_arr     = true;
    Buf *ret    = AllocX(Buf, 1);
    ret->s      = (char *)alloc_(ret->n = 4);
    lua_pushnil(L);
    while (lua_next(L, n))
    {
        if (lua_type(L, -2) == LUA_TNUMBER)
        {
            char skey[16];
            sprintf(skey, "%d", (int)(lua_tonumber(L, -2) - 0.5));
            Buf *v = create_item(skey, L, lua_gettop(L));
            BufAppend(ret, v);
            BufFree(v);
        }
        else
        {
            luaL_checktype(L, -2, LUA_TSTRING);
            const char *skey = lua_tostring(L, -2);
            *is_arr          = false;
            Buf *v           = create_item(skey, L, lua_gettop(L));
            BufAppend(ret, v);
            BufFree(v);
        }
        lua_pop(L, 1);
    }
    BufAppendChar(ret, 0);
    *(uint32_t *)ret->s = (uint32_t)ret->n;
    assert(old_top == lua_gettop(L));
    return ret;
}

Buf *create_item(const char *key, lua_State *L, int n)
{
    Buf *ret     = NULL;
    int  old_top = lua_gettop(L);
    switch (lua_type(L, n))
    {
    case LUA_TNIL    : ret = create_nil(key); break;
    case LUA_TBOOLEAN: ret = create_boolean(key, lua_toboolean(L, n)); break;
    case LUA_TNUMBER:
#ifndef JIT
        if (lua_isinteger(L, n)) ret = create_integer(key, (lua_Integer)lua_tonumber(L, n));
        else
#endif
            ret = create_number(key, lua_tonumber(L, n));
        break;
    case LUA_TSTRING:
    {
        size_t      m;
        const char *s = lua_tolstring(L, n, &m);
        ret           = create_bytes(key, s, (uint32_t)m);
        break;
    }
    case LUA_TTABLE:
    {
        bool is_arr;
        Buf *v    = create_table(L, n, &is_arr);
        ret       = AllocX(Buf, 1);
        size_t kn = strlen(key);
        ret->s    = (char *)alloc_(ret->n = kn + 2 + v->n);
        ret->s[0] = is_arr ? 4 : 3;
        CopyMem(ret->s + 1, key, kn + 1);
        CopyMem(ret->s + kn + 2, v->s, v->n);
        BufFree(v);
        break;
    }
    case LUA_TUSERDATA:
    {
        struct LuaVariant *lv = lv_check(L, n, LV_ANY);
        if (lv) ret = create_lua_variant(key, lv);
        break;
    }
    }
    assert(old_top == lua_gettop(L));
    return ret;
}

int lua_bson_encode(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "BsonEncode(obj: table): string");
        return lua_error(L);
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    bool is_arr;
    Buf *buf = create_table(L, 1, &is_arr);
    if (!is_arr) lua_pushlstring(L, buf->s, buf->n);
    BufFree(buf);
    if (is_arr)
    {
        lua_pushliteral(L, "BsonEncode's args is not map");
        return lua_error(L);
    }
    return 1;
}

int lua_bson_decode(lua_State *L)
{
    lua_pushliteral(L, "not implement `BsonDecode` yet!");
    return lua_error(L);
}
