/* =============================================================================
 * src/scripting/lua/variant.cpp —— LuaVariant（P3.B 模块 12：原 lua_variant.c/h，C → C++）
 *
 * 迁移口径：**只做「搬 + 改名 + 转 C++」**，语义 1:1（铁律 3：C++ 化改造归 P6）。
 * 相对旧 C 实现的改动仅四类，全部在下面就地标注：
 *   ① 文件名/头文件名（lua_variant.* → variant.*）；
 *   ② `legacy_lib.h`（C 版工具宏）→ `core/util.hpp`（同一套 E/DV/trim 语义）；
 *   ③ C11 的 `_Generic` → `set_lv_integer` / `decltype`（C++ 无 `_Generic`）；
 *   ④ 需要显式类型的 C 习惯（void* 转换、case 标签后的声明加花括号）。
 * **对外符号保持 C 链接**（`variant.hpp` 有 extern "C" 护卫，tests/unit/test_lua_variant.c
 *   仍是 C 文件、直接调 `lv_*`）。
 * ❗已知缺陷 1:1 保留：`lv_str` 的范围判断写成链式比较（恒真，见 TODO 9.1 D28）、
 *   `lv_simple` 把「可精确表示为 float 的 F64」标成 LV_U32（D14）、`LV_ANY` 分支为空（D8）。
 * ========================================================================== */

#include "variant.hpp"
#include "core/util.hpp"   /* E / DV / alloc_ / free_ / CopyMem / trim 等（旧 legacy_lib.h 的非 Lua 部分） */
#include "scripting/lua/helpers.hpp" /* str2integer / lua_type_error（旧 legacy_lib.h 的 Lua 部分） */
#include <intrin.h>        /* CopyMem 宏（__movsb）——万一用到 */
#include "lua.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dbl2str.h"

/* 旧 legacy_lib.h 把这三个符号放在全局命名空间；新版分属两处（Plan 5.7 的拆分）
 * → 用选择性 using 保持调用点一字不改。 */
using autodbg::core::trim;
using autodbg::scripting::lua::lua_type_error;
using autodbg::scripting::lua::str2integer;

static int new_i8(lua_State *L);
static int new_u8(lua_State *L);
static int new_i16(lua_State *L);
static int new_u16(lua_State *L);
static int new_i32(lua_State *L);
static int new_u32(lua_State *L);
static int new_i64(lua_State *L);
static int new_u64(lua_State *L);
static int new_f32(lua_State *L);
static int new_f64(lua_State *L);
static int new_any(lua_State *L);

static int lua_variant_add(lua_State *L);
static int lua_variant_sub(lua_State *L);
static int lua_variant_mul(lua_State *L);
static int lua_variant_div(lua_State *L);
static int lua_variant_mod(lua_State *L);
static int lua_variant_pow(lua_State *L);
static int lua_variant_unm(lua_State *L);
static int lua_variant_idiv(lua_State *L);
static int lua_variant_band(lua_State *L);
static int lua_variant_bor(lua_State *L);
static int lua_variant_bxor(lua_State *L);
static int lua_variant_bnot(lua_State *L);
static int lua_variant_shl(lua_State *L);
static int lua_variant_shr(lua_State *L);
static int lua_variant_concat(lua_State *L);
static int lua_variant_len(lua_State *L);
static int lua_variant_eq(lua_State *L);
static int lua_variant_lt(lua_State *L);
static int lua_variant_le(lua_State *L);
static int lua_variant_index(lua_State *L);
static int lua_variant_newindex(lua_State *L);
static int lua_variant_call(lua_State *L);
static int lua_variant_gc(lua_State *L);
static int lua_variant_close(lua_State *L);
static int lua_variant_mode(lua_State *L);
static int lua_variant_name(lua_State *L);
static int lua_variant_tostring(lua_State *L);

static int lua_variant_bin(lua_State *L, struct LuaVariant *lv);
static int lua_variant_hex(lua_State *L, struct LuaVariant *lv);
static int lua_variant_val(lua_State *L, struct LuaVariant *lv);
static int lua_variant_rawval(lua_State *L, struct LuaVariant *lv);
static int lua_variant_as(lua_State *L, struct LuaVariant *lv);
static int lua_variant_to(lua_State *L, struct LuaVariant *lv);
static int lua_variant_type(lua_State *L, struct LuaVariant *lv);

static void lv_simple(struct LuaVariant *lv);
static bool lv_hex(char *s, struct LuaVariant *lv);
static bool lv_str(lua_State *L, struct LuaVariant *lv, char *buf);

/* C++ 化（P6.3 提前）：C11 的 `_Generic` 在 C++ 不存在。旧的两个宏**只**用于
 * `(lua_Integer)0`，语义是「lua_Integer 是 64 位 → i64 字段，否则 i32」：
 *   int / 32 位 ptrdiff_t（x86 LuaJIT）→ LV_I32；int64_t（Lua 5.5 / x64）→ LV_I64。
 * 用 sizeof 在编译期选择，行为与 `_Generic` 的类型选择一致。 */
static inline void set_lv_integer(struct LuaVariant *lv, lua_Integer v)
{
    if (sizeof(lua_Integer) >= 8)
    {
        lv->type    = LV_I64;
        lv->val.i64 = (int64_t)v;
    }
    else
    {
        lv->type    = LV_I32;
        lv->val.i32 = (int32_t)v;
    }
}
#define get_lv_val(lv, r, T)                        \
    do switch ((lv)->type)                          \
        {                                           \
        case LV_I8 : (r) = (T)(lv)->val.i8; break;  \
        case LV_U8 : (r) = (T)(lv)->val.u8; break;  \
        case LV_I16: (r) = (T)(lv)->val.i16; break; \
        case LV_U16: (r) = (T)(lv)->val.u16; break; \
        case LV_I32: (r) = (T)(lv)->val.i32; break; \
        case LV_U32: (r) = (T)(lv)->val.u32; break; \
        case LV_I64: (r) = (T)(lv)->val.i64; break; \
        case LV_U64: (r) = (T)(lv)->val.u64; break; \
        case LV_F32: (r) = (T)(lv)->val.f32; break; \
        case LV_F64: (r) = (T)(lv)->val.f64; break; \
        default    : break;                         \
        }                                           \
    while (0)

/* C++ 化：`_Generic((r), T: (T)(e))` 的等价写法 —— 把 `e` 显式转成 `r` 的**值类型**。
 * ❗不能直接写 `decltype(r)`：`r` 可能是 `*v` / `lv->val.u8` 这种**左值表达式**，
 *   `decltype` 会给出**引用**类型，而 C 风格的「转到引用」是**重新解释内存**而不是取值
 *   —— 实测（U32 加法）会把联合体后续字节一起读进来，结果高位是垃圾（`10 + 3` → u64/0x…D）。
 * 项目禁用 `<type_traits>`，所以用一个最小的去引用 traits。 */
template <class T>
struct lv_val_t
{
    typedef T type;
};
template <class T>
struct lv_val_t<T &>
{
    typedef T type;
};
template <class T>
struct lv_val_t<T &&>
{
    typedef T type;
};
#define LV_CAST_TO_R(r, e) ((lv_val_t<decltype(r)>::type)(e))

#define get_lv_val_s(lv, r)                                       \
    do switch ((lv)->type)                                        \
        {                                                         \
        case LV_I8 : (r) = LV_CAST_TO_R(r, (lv)->val.i8); break;  \
        case LV_U8 : (r) = LV_CAST_TO_R(r, (lv)->val.u8); break;  \
        case LV_I16: (r) = LV_CAST_TO_R(r, (lv)->val.i16); break; \
        case LV_U16: (r) = LV_CAST_TO_R(r, (lv)->val.u16); break; \
        case LV_I32: (r) = LV_CAST_TO_R(r, (lv)->val.i32); break; \
        case LV_U32: (r) = LV_CAST_TO_R(r, (lv)->val.u32); break; \
        case LV_I64: (r) = LV_CAST_TO_R(r, (lv)->val.i64); break; \
        case LV_U64: (r) = LV_CAST_TO_R(r, (lv)->val.u64); break; \
        case LV_F32: (r) = LV_CAST_TO_R(r, (lv)->val.f32); break; \
        case LV_F64: (r) = LV_CAST_TO_R(r, (lv)->val.f64); break; \
        default    : break;                                       \
        }                                                         \
    while (0)

void lv_init(lua_State *L)
{
    if (luaL_newmetatable(L, "LuaVariant"))
    {
#define M(n)                               \
    lua_pushcfunction(L, lua_variant_##n); \
    lua_setfield(L, -2, "__" #n)
        M(add);
        M(sub);
        M(mul);
        M(div);
        M(mod);
        M(pow);
        M(unm);
        M(idiv);
        M(band);
        M(bor);
        M(bxor);
        M(bnot);
        M(shl);
        M(shr);
        M(concat);
        M(len);
        M(eq);
        M(lt);
        M(le);
        M(index);
        M(newindex);
        M(call);
        M(gc);
        M(close);
        M(mode);
        M(name);
        M(tostring);
#undef M
    }
    lua_pop(L, 1);

    lua_register(L, "I8", new_i8);
    lua_register(L, "U8", new_u8);
    lua_register(L, "I16", new_i16);
    lua_register(L, "U16", new_u16);
    lua_register(L, "I32", new_i32);
    lua_register(L, "U32", new_u32);
    lua_register(L, "I64", new_i64);
    lua_register(L, "U64", new_u64);
    lua_register(L, "F32", new_f32);
    lua_register(L, "F64", new_f64);
    lua_register(L, "LV", new_any);
}

struct LuaVariant *lv_push(lua_State *L, enum LV_type type)
{
    struct LuaVariant *v = (struct LuaVariant *)lua_newuserdata(L, sizeof(struct LuaVariant));
    v->type              = type;
    luaL_getmetatable(L, "LuaVariant");
    lua_setmetatable(L, -2);
    return v;
}

size_t lv_size(struct LuaVariant *lv)
{
    if (!lv) return 0;
    switch (lv->type)
    {
    case LV_I8 :
    case LV_U8 : return 1;
    case LV_I16:
    case LV_U16: return 2;
    case LV_I32:
    case LV_U32:
    case LV_F32: return 4;
    case LV_I64:
    case LV_U64:
    case LV_F64: return 8;
    default    : return 0;
    }
}

struct LuaVariant *lv_check(lua_State *L, int index, enum LV_type type)
{
    struct LuaVariant *lv = (struct LuaVariant *)luaL_checkudata(L, index, "LuaVariant");
    if (lv->type < 0 || lv->type >= LV_MAX)
    {
        lua_pushliteral(L, "LuaVariant type error");
        lua_error(L);
        return NULL;
    }
    if (type == LV_ANY) return lv;
    if (type == lv->type) return lv;
    lv_simple(lv);
    if (type == lv->type) return lv;
    bool same;
    switch (type)
    {
    case LV_F64:
        same = false;
        switch (lv->type)
        {
        case LV_I8 : same = lv->val.i8 == (int8_t)(double)lv->val.i8; break;
        case LV_U8 : same = lv->val.u8 == (uint8_t)(double)lv->val.u8; break;
        case LV_I16: same = lv->val.i16 == (int16_t)(double)lv->val.i16; break;
        case LV_U16: same = lv->val.u16 == (uint16_t)(double)lv->val.u16; break;
        case LV_I32: same = lv->val.i32 == (int32_t)(double)lv->val.i32; break;
        case LV_U32: same = lv->val.u32 == (uint32_t)(double)lv->val.u32; break;
        case LV_I64: same = lv->val.i64 == (int64_t)(double)lv->val.i64; break;
        case LV_U64: same = lv->val.u64 == (uint64_t)(double)lv->val.u64; break;
        default    : break;
        }
        if (same)
        {
            get_lv_val_s(lv, lv->val.f64);
            return lv;
        }
        break;
    case LV_F32:
        same = false;
        switch (lv->type)
        {
        case LV_I8 : same = lv->val.i8 == (int8_t)(float)lv->val.i8; break;
        case LV_U8 : same = lv->val.u8 == (uint8_t)(float)lv->val.u8; break;
        case LV_I16: same = lv->val.i16 == (int16_t)(float)lv->val.i16; break;
        case LV_U16: same = lv->val.u16 == (uint16_t)(float)lv->val.u16; break;
        case LV_I32: same = lv->val.i32 == (int32_t)(float)lv->val.i32; break;
        case LV_U32: same = lv->val.u32 == (uint32_t)(float)lv->val.u32; break;
        default    : break;
        }
        if (same)
        {
            get_lv_val_s(lv, lv->val.f32);
            return lv;
        }
        break;
    case LV_I64:
        switch (lv->type)
        {
        case LV_I8 :
        case LV_U8 :
        case LV_I16:
        case LV_U16:
        case LV_I32:
        case LV_U32:
            get_lv_val_s(lv, lv->val.i64);
            lv->type = LV_I64;
            return lv;
        case LV_U64:
            if (lv->val.u64 <= INT64_MAX)
            {
                lv->type    = LV_I64;
                lv->val.i64 = lv->val.u64;
                return lv;
            }
            break;
        default: break;
        }
        break;
    case LV_U64:
        switch (lv->type)
        {
        case LV_U8 :
        case LV_U16:
        case LV_U32:
            get_lv_val_s(lv, lv->val.u64);
            lv->type = LV_U64;
            return lv;
        case LV_I8 :
        case LV_I16:
        case LV_I32:
        case LV_I64:
        {
            int64_t v;
            get_lv_val_s(lv, v);
            if (v >= 0)
            {
                lv->type    = LV_U64;
                lv->val.u64 = v;
                return lv;
            }
            break;
        }
        default: break;
        }
        break;
    case LV_I32:
        switch (lv->type)
        {
        case LV_I8 :
        case LV_U8 :
        case LV_I16:
        case LV_U16:
            get_lv_val_s(lv, lv->val.i32);
            lv->type = LV_I32;
            return lv;
        case LV_U32:
            if (lv->val.u32 <= INT32_MAX)
            {
                lv->type    = LV_I32;
                lv->val.i32 = lv->val.u32;
                return lv;
            }
            break;
        default: break;
        }
        break;
    case LV_U32:
        switch (lv->type)
        {
        case LV_U8:
        case LV_U16:
            get_lv_val_s(lv, lv->val.u32);
            lv->type = LV_U32;
            return lv;
        case LV_I8 :
        case LV_I16:
        case LV_I32:
        {
            int32_t v;
            get_lv_val_s(lv, v);
            if (v >= 0)
            {
                lv->type    = LV_U32;
                lv->val.u32 = v;
                return lv;
            }
            break;
        }
        default: break;
        }
        break;
    case LV_I16:
        switch (lv->type)
        {
        case LV_I8:
        case LV_U8:
            get_lv_val_s(lv, lv->val.i16);
            lv->type = LV_I16;
            return lv;
        case LV_U16:
            if (lv->val.u16 <= INT16_MAX)
            {
                lv->type    = LV_I16;
                lv->val.i16 = lv->val.u16;
                return lv;
            }
            break;
        default: break;
        }
        break;
    case LV_U16:
        switch (lv->type)
        {
        case LV_U8:
            get_lv_val_s(lv, lv->val.u16);
            lv->type = LV_U16;
            return lv;
        case LV_I8:
        case LV_I16:
        {
            int16_t v;
            get_lv_val_s(lv, v);
            if (v >= 0)
            {
                lv->type    = LV_U16;
                lv->val.u16 = v;
                return lv;
            }
            break;
        }
        default: break;
        }
        break;
    case LV_I8:
        if (lv->type == LV_U8 && lv->val.u8 <= INT8_MAX)
        {
            lv->type   = LV_I8;
            lv->val.i8 = lv->val.u8;
            return lv;
        }
        break;
    case LV_U8:
        if (lv->type == LV_I8 && lv->val.i8 >= 0)
        {
            lv->type   = LV_U8;
            lv->val.u8 = lv->val.i8;
            return lv;
        }
        break;
    default: break;
    }
    lua_pushfstring(L, "LuaVariant type need to be %s",
                    type == LV_I8    ? "I8"
                    : type == LV_U8  ? "U8"
                    : type == LV_I16 ? "I16"
                    : type == LV_U16 ? "U16"
                    : type == LV_I32 ? "I32"
                    : type == LV_U32 ? "U32"
                    : type == LV_I64 ? "I64"
                    : type == LV_U64 ? "U64"
                    : type == LV_F32 ? "F32"
                                     : "F64");
    lua_error(L);
    return NULL;
}

static bool check_str_num(char *ptr, bool *isneg, bool *isdbl)
{
    char *p = ptr;
    *isneg  = false;
    *isdbl  = false;
    if (*p == '+') p++;
    else if (*p == '-')
    {
        *isneg = true;
        p++;
    }
    if (p[0] == '0')
    {
        p++;
        if ((p[1] | ' ') == 'x')
        {
            // 16进制整数
            p++;
            for (; *p; p++)
            {
                if ('0' <= DV(*p) <= '9') continue;
                if ('a' <= DV(*p | ' ') <= 'f') continue;
                return false;
            }
        }
        else if (p[1] == '.')
        {
            // double
            p++;
            bool hase = false;
            for (; *p; p++)
            {
                if ('0' <= DV(*p) <= '9') continue;
                if ((*p | ' ') != 'e') return false;
                if (hase) return false;
                hase = true;
                if (p[1] == '+' || p[1] == '-') p++;
            }
            *isdbl = true;
        }
        else
        {
            // 8进制整数
            for (; *p; p++)
            {
                if ('0' <= DV(*p) <= '7') continue;
                return false;
            }
        }
        return true;
    }
    for (; *p; p++)
    {
        if ('0' <= *p && *p <= '9') continue;
        if (*p == '.') *isdbl = true;
        if ((*p | ' ') == 'e') *isdbl = true;
        return 0;
    }
}

static int new_variant(lua_State *L, enum LV_type type)
{
    int                arg_type = lua_type(L, 1);
    struct LuaVariant *v        = lv_push(L, type);
    if (arg_type == LUA_TSTRING)
    {
        const char *s = lua_tostring(L, 1);
        switch (type)
        {
        case LV_I8 : str2integer(s, 1, true, &v->val.i8); break;
        case LV_U8 : str2integer(s, 1, false, &v->val.u8); break;
        case LV_I16: str2integer(s, 2, true, &v->val.i16); break;
        case LV_U16: str2integer(s, 2, false, &v->val.u16); break;
        case LV_I32: str2integer(s, 4, true, &v->val.i32); break;
        case LV_U32: str2integer(s, 4, false, &v->val.u32); break;
        case LV_I64: str2integer(s, 8, true, &v->val.i64); break;
        case LV_U64: str2integer(s, 8, false, &v->val.u64); break;
        case LV_F32: v->val.f32 = (float)strtod(s, NULL); break;
        case LV_F64: v->val.f64 = strtod(s, NULL); break;
        case LV_ANY:
        {
            char *buf   = _strdup(s);
            char *ptr   = trim(buf);
            bool  neg   = false;
            bool  isdbl = false;
            free(buf);
        }
        default: assert(false);
        }
    }
    else if (arg_type == LUA_TNUMBER)
    {
        switch (type)
        {
        case LV_I8 : v->val.i8 = (int8_t)lua_tointeger(L, 1); break;
        case LV_U8 : v->val.u8 = (uint8_t)lua_tointeger(L, 1); break;
        case LV_I16: v->val.i16 = (int16_t)lua_tointeger(L, 1); break;
        case LV_U16: v->val.u16 = (uint16_t)lua_tointeger(L, 1); break;
        case LV_I32: v->val.i32 = (int32_t)lua_tointeger(L, 1); break;
        case LV_U32: v->val.u32 = (uint32_t)lua_tointeger(L, 1); break;
        case LV_I64: v->val.i64 = (int64_t)lua_tointeger(L, 1); break;
        case LV_U64: v->val.u64 = (uint64_t)lua_tointeger(L, 1); break;
        case LV_F32: v->val.f32 = (float)lua_tonumber(L, 1); break;
        case LV_F64:
        case LV_ANY: v->val.f64 = (double)lua_tonumber(L, 1); break;
        default    : assert(false);
        }
    }
    else return lua_type_error(L, 1, "string or number");
    return 1;
}

int new_i8(lua_State *L) { return new_variant(L, LV_I8); }

int new_u8(lua_State *L) { return new_variant(L, LV_U8); }

int new_i16(lua_State *L) { return new_variant(L, LV_I16); }

int new_u16(lua_State *L) { return new_variant(L, LV_U16); }

int new_i32(lua_State *L) { return new_variant(L, LV_I32); }

int new_u32(lua_State *L) { return new_variant(L, LV_U32); }

int new_i64(lua_State *L) { return new_variant(L, LV_I64); }

int new_u64(lua_State *L) { return new_variant(L, LV_U64); }

int new_f32(lua_State *L) { return new_variant(L, LV_F32); }

int new_f64(lua_State *L) { return new_variant(L, LV_F64); }

int new_any(lua_State *L) { return new_variant(L, LV_ANY); }

void lv_simple(struct LuaVariant *lv)
{
    if (lv->type == LV_F64)
    {
        double   v  = lv->val.f64;
        float    vf = (float)lv->val.f64;
        int64_t  vi = (int64_t)lv->val.f64;
        uint64_t vu = (uint64_t)lv->val.f64;
        if (vi == v)
        {
            lv->type    = LV_I64;
            lv->val.i64 = vi;
        }
        else if (vu == v)
        {
            lv->type    = LV_U64;
            lv->val.u64 = vu;
        }
        else if (vf == v)
        {
            lv->type    = LV_U32;
            lv->val.f32 = vf;
        }
    }
    if (lv->type == LV_F32)
    {
        float    v  = lv->val.f32;
        int64_t  vi = (int64_t)lv->val.f32;
        uint64_t vu = (uint64_t)lv->val.f32;
        if (vi == v)
        {
            lv->type    = LV_I64;
            lv->val.i64 = vi;
        }
        else if (vu == v)
        {
            lv->type    = LV_U64;
            lv->val.u64 = vu;
        }
    }
    switch (lv->type)
    {
    case LV_I64:
        if (INT32_MIN <= lv->val.i64 && lv->val.i64 <= INT32_MAX)
        {
            lv->type    = LV_I32;
            lv->val.i32 = (int32_t)lv->val.i64;
        }
        else if (0 <= lv->val.i64 && lv->val.i64 <= UINT32_MAX)
        {
            lv->type    = LV_U32;
            lv->val.u32 = (uint32_t)lv->val.i64;
        }
        break;
    case LV_U64:
        if (lv->val.u64 < UINT32_MAX)
        {
            lv->type    = LV_U32;
            lv->val.u32 = (uint32_t)lv->val.u64;
        }
        break;
    default: break;
    }
    switch (lv->type)
    {
    case LV_I32:
        if (INT16_MIN <= lv->val.i32 && lv->val.i32 <= INT16_MAX)
        {
            lv->type    = LV_I16;
            lv->val.i16 = (int16_t)lv->val.i32;
        }
        else if (0 <= lv->val.i32 && lv->val.i32 <= UINT16_MAX)
        {
            lv->type    = LV_U16;
            lv->val.u16 = (uint16_t)lv->val.i32;
        }
        break;
    case LV_U32:
        if (lv->val.u32 < UINT16_MAX)
        {
            lv->type    = LV_U16;
            lv->val.u16 = (uint16_t)lv->val.u32;
        }
        break;
    default: break;
    }
    switch (lv->type)
    {
    case LV_I16:
        if (INT8_MIN <= lv->val.i16 && lv->val.i16 <= INT8_MAX)
        {
            lv->type   = LV_I8;
            lv->val.i8 = (int8_t)lv->val.i16;
        }
        else if (0 <= lv->val.i16 && lv->val.i16 <= UINT8_MAX)
        {
            lv->type   = LV_U8;
            lv->val.u8 = (uint8_t)lv->val.i16;
        }
        break;
    case LV_U16:
        if (lv->val.u16 < UINT8_MAX)
        {
            lv->type   = LV_U8;
            lv->val.u8 = (uint8_t)lv->val.u16;
        }
        break;
    default: break;
    }
}

struct LuaVariant *parse_lv(lua_State *L, int index, struct LuaVariant *lvt)
{
    struct LuaVariant *lv;
    if (lua_type(L, index) == LUA_TNUMBER)
    {
        lv = lvt;
#ifndef JIT
        if (lua_isinteger(L, index))
        {
            set_lv_integer(lv, lua_tointeger(L, index));
        }
        else
        {
#endif
            lua_Number  v  = lua_tonumber(L, index);
            lua_Integer vi = lua_tointeger(L, index);
            if (vi == v)
            {
                set_lv_integer(lv, vi);
            }
            else
            {
                lv->type    = LV_F64;
                lv->val.f64 = lua_tonumber(L, index);
            }
#ifndef JIT
        }
#endif
    }
    else if (lua_type(L, index) == LUA_TUSERDATA) lv = lv_check(L, index, LV_ANY);
    else return NULL;
    lv_simple(lv);
    return lv;
}

void init_lv_neg_v(const struct LuaVariant *lv, bool *neg, uint64_t *v)
{
    int64_t vv;
    switch (lv->type)
    {
    case LV_I8 :
    case LV_I16:
    case LV_I32:
    case LV_I64:
        get_lv_val_s(lv, vv);
        *neg = vv < 0;
        *v   = *neg ? -vv : vv;
        break;
    case LV_U8 :
    case LV_U16:
    case LV_U32:
    case LV_U64:
        *neg = false;
        get_lv_val_s(lv, *v);
        break;
    default: break;
    }
}

void lv_add_i(struct LuaVariant *lvr, bool neg1, uint64_t v1, bool neg2, uint64_t v2)
{
    if (neg1 == neg2)
    {
        if (v2 > ULLONG_MAX - v1) v1 = ULLONG_MAX;
        else v1 += v2;
        if (!neg1)
        {
            lvr->type    = LV_U64;
            lvr->val.u64 = v1;
        }
        else
        {
            lvr->type    = LV_I64;
            lvr->val.i64 = v1 <= (uint64_t)LLONG_MAX + 1 ? -(int64_t)v1 : LLONG_MIN;
        }
    }
    else
    {
        if (neg1)
        {
            uint64_t t = v1;
            v1         = v2;
            v2         = t;
        }
        if (v1 >= v2)
        {
            lvr->type    = LV_U64;
            lvr->val.u64 = v1 - v2;
        }
        else
        {
            lvr->type    = LV_I64;
            lvr->val.i64 = (int64_t)v1 - (int64_t)v2;
        }
    }
}

int lua_variant_add(lua_State *L)
{
    assert(lua_gettop(L) == 2);

    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1 = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2 = parse_lv(L, 2, &lv_t2);
    if (!lv1) return lua_type_error(L, 1, "LuaVariant or number");
    if (!lv2) return lua_type_error(L, 2, "LuaVariant or number");
    struct LuaVariant *lvr = lv_push(L, LV_ANY);
    if (lv1->type == LV_F64 || lv2->type == LV_F64 || lv1->type == LV_F32 || lv2->type == LV_F32)
    {
        lvr->type = LV_F64;
        double v1 = 0, v2 = 0;
        get_lv_val_s(lv1, v1);
        get_lv_val_s(lv2, v2);
        lvr->val.f64 = v1 + v2;
    }
    else
    {
        bool     neg1 = false;
        bool     neg2 = false;
        uint64_t v1   = 0;
        uint64_t v2   = 0;
        init_lv_neg_v(lv1, &neg1, &v1);
        init_lv_neg_v(lv2, &neg2, &v2);
        lv_add_i(lvr, neg1, v1, neg2, v2);
    }
    lv_simple(lvr);

    return 1;
}

int lua_variant_sub(lua_State *L)
{
    assert(lua_gettop(L) == 2);

    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1 = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2 = parse_lv(L, 2, &lv_t2);
    if (!lv1) return lua_type_error(L, 1, "LuaVariant or number");
    if (!lv2) return lua_type_error(L, 2, "LuaVariant or number");
    struct LuaVariant *lvr = lv_push(L, LV_ANY);
    if (lv1->type == LV_F64 || lv2->type == LV_F64 || lv1->type == LV_F32 || lv2->type == LV_F32)
    {
        lvr->type = LV_F64;
        double v1 = 0, v2 = 0;
        get_lv_val_s(lv1, v1);
        get_lv_val_s(lv2, v2);
        lvr->val.f64 = v1 - v2;
    }
    else
    {
        bool     neg1 = false;
        bool     neg2 = false;
        uint64_t v1   = 0;
        uint64_t v2   = 0;
        init_lv_neg_v(lv1, &neg1, &v1);
        init_lv_neg_v(lv2, &neg2, &v2);
        if (v2) lv_add_i(lvr, neg1, v1, !neg2, v2);
        else *lvr = *lv1;
    }
    lv_simple(lvr);

    return 1;
}

int lua_variant_mul(lua_State *L)
{
    assert(lua_gettop(L) == 2);

    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1 = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2 = parse_lv(L, 2, &lv_t2);
    if (!lv1) return lua_type_error(L, 1, "LuaVariant or number");
    if (!lv2) return lua_type_error(L, 2, "LuaVariant or number");
    struct LuaVariant *lvr = lv_push(L, LV_ANY);
    if (lv1->type == LV_F64 || lv2->type == LV_F64 || lv1->type == LV_F32 || lv2->type == LV_F32)
    {
        lvr->type = LV_F64;
        double v1 = 0, v2 = 0;
        get_lv_val_s(lv1, v1);
        get_lv_val_s(lv2, v2);
        lvr->val.f64 = v1 * v2;
    }
    else
    {
        bool     neg1 = false;
        bool     neg2 = false;
        uint64_t v1   = 0;
        uint64_t v2   = 0;
        init_lv_neg_v(lv1, &neg1, &v1);
        init_lv_neg_v(lv2, &neg2, &v2);
        if (neg1 ^ neg2)
        {
            lvr->type    = LV_I64;
            lvr->val.i64 = -(int64_t)(v1 * v2 & LLONG_MAX);
        }
        else
        {
            lvr->type    = LV_U64;
            lvr->val.i64 = v1 * v2;
        }
    }
    lv_simple(lvr);

    return 1;
}

int lua_variant_div(lua_State *L)
{
    assert(lua_gettop(L) == 2);
    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1 = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2 = parse_lv(L, 2, &lv_t2);
    if (!lv1) return lua_type_error(L, 1, "LuaVariant or number");
    if (!lv2) return lua_type_error(L, 2, "LuaVariant or number");
    struct LuaVariant *lv = NULL;
    double             a, b;
    get_lv_val_s(lv1, a);
    get_lv_val_s(lv2, b);
    lv          = lv_push(L, LV_F64);
    lv->val.f64 = a / b;
    lv_simple(lv);
    return 1;
}

int lua_variant_mod(lua_State *L)
{
    assert(lua_gettop(L) == 2);
    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1 = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2 = parse_lv(L, 2, &lv_t2);
    if (!lv1) return lua_type_error(L, 1, "LuaVariant or number");
    if (!lv2) return lua_type_error(L, 2, "LuaVariant or number");
    struct LuaVariant *lv = NULL;
    if (lv1->type == LV_F32 || lv1->type == LV_F64 || lv2->type == LV_F32 || lv2->type == LV_F64)
    {
        double a, b;
        get_lv_val_s(lv1, a);
        get_lv_val_s(lv2, b);
        lv          = lv_push(L, LV_F64);
        lv->val.f64 = fmod(a, b);
    }
    else
    {
        bool     neg1 = false;
        bool     neg2 = false;
        uint64_t a, b;
        switch (lv1->type)
        {
        case LV_I8 :
        case LV_I16:
        case LV_I32:
        case LV_I64:
            int64_t v;
            get_lv_val_s(lv1, v);
            if (v >= 0) a = v;
            else
            {
                neg1 = true;
                a    = -v;
            }
            break;
        default: get_lv_val_s(lv1, a); break;
        }
        switch (lv2->type)
        {
        case LV_I8 :
        case LV_I16:
        case LV_I32:
        case LV_I64:
            int64_t v;
            get_lv_val_s(lv2, v);
            if (v >= 0) b = v;
            else
            {
                neg2 = true;
                b    = -v;
            }
            break;
        default: get_lv_val_s(lv2, b); break;
        }
        a %= b;
        if (!neg1)
        {
            lv          = lv_push(L, LV_U64);
            lv->val.u64 = a;
        }
        else
        {
            lv          = lv_push(L, LV_I64);
            lv->val.i64 = -(int64_t)a;
        }
    }
    lv_simple(lv);
    return 1;
}

int lua_variant_pow(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `pow` yet!");
    return lua_error(L);
}

int lua_variant_unm(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `unm` yet!");
    return lua_error(L);
}

int lua_variant_idiv(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `idiv` yet!");
    return lua_error(L);
}

int lua_variant_band(lua_State *L)
{
    assert(lua_gettop(L) == 2);
    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1 = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2 = parse_lv(L, 2, &lv_t2);
    if (!lv1) return lua_type_error(L, 1, "LuaVariant or number");
    if (!lv2) return lua_type_error(L, 2, "LuaVariant or number");
    if (lv1->type == LV_F64 || lv1->type == LV_F32)
    {
        lua_pushliteral(L, "#1 can only use integer");
        return lua_error(L);
    }
    if (lv2->type == LV_F64 || lv2->type == LV_F32)
    {
        lua_pushliteral(L, "#1 can only use integer");
        return lua_error(L);
    }
    struct LuaVariant *lv = lv_push(L, LV_U64);
    uint64_t           a, b;
    get_lv_val_s(lv1, a);
    get_lv_val_s(lv2, b);
    lv->val.u64 = a & b;
    lv_simple(lv);
    return 1;
}

int lua_variant_bor(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `bor` yet!");
    return lua_error(L);
}

int lua_variant_bxor(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `bxor` yet!");
    return lua_error(L);
}

int lua_variant_bnot(lua_State *L)
{
    assert(lua_gettop(L) == 2);
    assert(lua_toboolean(L, 1));
    struct LuaVariant  lv_t1;
    struct LuaVariant *lv1 = parse_lv(L, 2, &lv_t1);
    if (!lv1) return lua_type_error(L, 2, "LuaVariant or number");
    if (lv1->type == LV_F64 || lv1->type == LV_F32)
    {
        lua_pushliteral(L, "#1 can only use integer");
        return lua_error(L);
    }
    struct LuaVariant *lv = lv_push(L, LV_U64);
    uint64_t           a;
    get_lv_val_s(lv1, a);
    lv->val.u64 = ~a;
    lv_simple(lv);
    return 1;
}

int lua_variant_shl(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `shl` yet!");
    return lua_error(L);
}

int lua_variant_shr(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `shr` yet!");
    return lua_error(L);
}

int lua_variant_concat(lua_State *L)
{
    // TODO: 待实现
    char        sbuf[2][0x20];
    const char *s[2];
    assert(lua_gettop(L) == 2);
    if (lua_type(L, 1) == LUA_TUSERDATA)
    {
        struct LuaVariant *x = lv_check(L, 1, LV_ANY);
        lv_str(L, x, sbuf[0]);
        s[0] = sbuf[0];
    }
    else
    {
        s[0] = lua_tostring(L, 1);
    }
    if (lua_type(L, 2) == LUA_TUSERDATA)
    {
        struct LuaVariant *x = lv_check(L, 2, LV_ANY);
        lv_str(L, x, sbuf[1]);
        s[1] = sbuf[1];
    }
    else
    {
        s[1] = lua_tostring(L, 2);
    }
    lua_pushfstring(L, "%s%s", s[0], s[1]);
    return 1;
}

int lua_variant_len(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `len` yet!");
    return lua_error(L);
}

int lua_variant_eq(lua_State *L)
{
    assert(lua_gettop(L) == 2);
    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1  = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2  = parse_lv(L, 2, &lv_t2);
    bool               same = false;
    if (lv1->type == lv2->type)
    {
        switch (lv1->type)
        {
        case LV_I8 : same = lv1->val.i8 == lv2->val.i8; break;
        case LV_U8 : same = lv1->val.u8 == lv2->val.u8; break;
        case LV_I16: same = lv1->val.i16 == lv2->val.i16; break;
        case LV_U16: same = lv1->val.u16 == lv2->val.u16; break;
        case LV_I32: same = lv1->val.i32 == lv2->val.i32; break;
        case LV_U32: same = lv1->val.u32 == lv2->val.u32; break;
        case LV_I64: same = lv1->val.i64 == lv2->val.i64; break;
        case LV_U64: same = lv1->val.u64 == lv2->val.u64; break;
        case LV_F32: same = lv1->val.f32 == lv2->val.f32; break;
        case LV_F64: same = lv1->val.f64 == lv2->val.f64; break;
        default    : lua_pushfstring(L, "unknown LuaVariant type: %d", lv1->type); return lua_error(L);
        }
    }
    lua_pushboolean(L, same);
    return 1;
}

static int64_t lv_to_i64(struct LuaVariant *lv)
{
    switch (lv->type)
    {
    case LV_I8 : return lv->val.i8;
    case LV_U8 : return (int64_t)lv->val.u8;
    case LV_I16: return lv->val.i16;
    case LV_U16: return (int64_t)lv->val.u16;
    case LV_I32: return lv->val.i32;
    case LV_U32: return (int64_t)lv->val.u32;
    case LV_I64: return lv->val.i64;
    case LV_U64: return (int64_t)lv->val.u64;
    default    : return 0;
    }
}

static double lv_to_f64(struct LuaVariant *lv)
{
    switch (lv->type)
    {
    case LV_I8 : return (double)lv->val.i8;
    case LV_U8 : return (double)lv->val.u8;
    case LV_I16: return (double)lv->val.i16;
    case LV_U16: return (double)lv->val.u16;
    case LV_I32: return (double)lv->val.i32;
    case LV_U32: return (double)lv->val.u32;
    case LV_I64: return (double)lv->val.i64;
    case LV_U64: return (double)lv->val.u64;
    case LV_F32: return (double)lv->val.f32;
    case LV_F64: return lv->val.f64;
    default    : return 0;
    }
}

static int lv_lt(struct LuaVariant *lv1, struct LuaVariant *lv2)
{
    // Float promotion: if either is F64, compare as double
    if (lv1->type == LV_F64 || lv2->type == LV_F64 || lv1->type == LV_F32 || lv2->type == LV_F32) return lv_to_f64(lv1) < lv_to_f64(lv2);

    // Same type: direct comparison
    if (lv1->type == lv2->type)
    {
        switch (lv1->type)
        {
        case LV_I8 : return lv1->val.i8 < lv2->val.i8;
        case LV_U8 : return lv1->val.u8 < lv2->val.u8;
        case LV_I16: return lv1->val.i16 < lv2->val.i16;
        case LV_U16: return lv1->val.u16 < lv2->val.u16;
        case LV_I32: return lv1->val.i32 < lv2->val.i32;
        case LV_U32: return lv1->val.u32 < lv2->val.u32;
        case LV_I64: return lv1->val.i64 < lv2->val.i64;
        case LV_U64: return lv1->val.u64 < lv2->val.u64;
        default    : return 0;
        }
    }

    // Mixed integer types: check for "large" U64 (>= 2^63)
    // All I32/U32/I64 values are < any large U64, so handle that branch first
    int lv1_large = (lv1->type == LV_U64 && (lv1->val.u64 >> 63));
    int lv2_large = (lv2->type == LV_U64 && (lv2->val.u64 >> 63));

    if (lv1_large) return 0; // large U64 > all non-U64 values
    if (lv2_large) return 1; // all non-U64 values < large U64

    // No large U64 involved, all values fit in signed i64 safely
    return lv_to_i64(lv1) < lv_to_i64(lv2);
}

int lua_variant_lt(lua_State *L)
{
    assert(lua_gettop(L) == 2);
    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1 = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2 = parse_lv(L, 2, &lv_t2);
    if (!lv1) return lua_type_error(L, 1, "LuaVariant or number");
    if (!lv2) return lua_type_error(L, 2, "LuaVariant or number");
    lua_pushboolean(L, lv_lt(lv1, lv2));
    return 1;
}

static int lv_le(struct LuaVariant *lv1, struct LuaVariant *lv2)
{
    // Float promotion: if either is F64, compare as double
    if (lv1->type == LV_F64 || lv2->type == LV_F64 || lv1->type == LV_F32 || lv2->type == LV_F32) return lv_to_f64(lv1) <= lv_to_f64(lv2);

    // Same type: direct comparison
    if (lv1->type == lv2->type)
    {
        switch (lv1->type)
        {
        case LV_I8 : return lv1->val.i8 <= lv2->val.i8;
        case LV_U8 : return lv1->val.u8 <= lv2->val.u8;
        case LV_I16: return lv1->val.i16 <= lv2->val.i16;
        case LV_U16: return lv1->val.u16 <= lv2->val.u16;
        case LV_I32: return lv1->val.i32 <= lv2->val.i32;
        case LV_U32: return lv1->val.u32 <= lv2->val.u32;
        case LV_I64: return lv1->val.i64 <= lv2->val.i64;
        case LV_U64: return lv1->val.u64 <= lv2->val.u64;
        default    : return 0;
        }
    }

    // Mixed integer types: check for "large" U64 (>= 2^63)
    // All I32/U32/I64 values are < any large U64, so handle that branch first
    int lv1_large = (lv1->type == LV_U64 && (lv1->val.u64 >> 63));
    int lv2_large = (lv2->type == LV_U64 && (lv2->val.u64 >> 63));

    if (lv1_large) return 0; // large U64 > all non-U64 values
    if (lv2_large) return 1; // all non-U64 values < large U64

    // No large U64 involved, all values fit in signed i64 safely
    return lv_to_i64(lv1) <= lv_to_i64(lv2);
}

int lua_variant_le(lua_State *L)
{
    assert(lua_gettop(L) == 2);
    struct LuaVariant  lv_t1, lv_t2;
    struct LuaVariant *lv1 = parse_lv(L, 1, &lv_t1);
    struct LuaVariant *lv2 = parse_lv(L, 2, &lv_t2);
    if (!lv1) return lua_type_error(L, 1, "LuaVariant or number");
    if (!lv2) return lua_type_error(L, 2, "LuaVariant or number");
    lua_pushboolean(L, lv_le(lv1, lv2));
    return 1;
}

int lua_variant_index(lua_State *L)
{
    struct LuaVariant *lv  = lv_check(L, 1, LV_ANY);
    const char        *key = luaL_checkstring(L, 2);
    if (!strcmp(key, "bin")) return lua_variant_bin(L, lv);
    if (!strcmp(key, "hex")) return lua_variant_hex(L, lv);
    if (!strcmp(key, "val")) return lua_variant_val(L, lv);
    if (!strcmp(key, "rawval")) return lua_variant_rawval(L, lv);
    if (!strcmp(key, "as")) return lua_variant_as(L, lv);
    if (!strcmp(key, "to")) return lua_variant_to(L, lv);
    if (!strcmp(key, "type")) return lua_variant_type(L, lv);
    // TODO: 待实现
    return luaL_error(L, "not implement `index`.`%s` yet!", key);
}

int lua_variant_newindex(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `newindex` yet!");
    return lua_error(L);
}

int lua_variant_call(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `call` yet!");
    return lua_error(L);
}

int lua_variant_gc(lua_State *L)
{
    assert(lua_gettop(L) == 1);
    assert(lua_type(L, 1) == LUA_TUSERDATA);
    return 0;
}

int lua_variant_close(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `close` yet!");
    return lua_error(L);
}

int lua_variant_mode(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `mode` yet!");
    return lua_error(L);
}

int lua_variant_name(lua_State *L)
{
    // TODO: 待实现
    lua_pushstring(L, "not implement `name` yet!");
    return lua_error(L);
}

#define ABS(x) ((x) < 0 ? -(x) : (x))

int lua_variant_tostring(lua_State *L)
{
    assert(lua_gettop(L) == 1);
    char               buf[0x20];
    struct LuaVariant *lv = lv_check(L, 1, LV_ANY);
    switch (lv->type)
    {
    case LV_I8 : sprintf(buf, ABS(lv->val.i8) <= 100 ? "%hd" : "0x%02hX", (int16_t)lv->val.i8); break;
    case LV_U8 : sprintf(buf, lv->val.u8 <= 100 ? "%hu" : "0x%02hX", (uint16_t)lv->val.u8); break;
    case LV_I16: sprintf(buf, ABS(lv->val.i16) <= 1000 ? "%hd" : "0x%04hX", lv->val.i16); break;
    case LV_U16: sprintf(buf, lv->val.u16 <= 1000 ? "%hu" : "0x%04hX", lv->val.u16); break;
    case LV_I32: sprintf(buf, ABS(lv->val.i32) <= 10000 ? "%d" : "0x%08X", lv->val.i32); break;
    case LV_U32: sprintf(buf, lv->val.u32 <= 10000 ? "%u" : "0x%08X", lv->val.u32); break;
    case LV_I64: sprintf(buf, ABS(lv->val.i64) <= 10000 ? "%lld" : "0x%016llX", lv->val.i64); break;
    case LV_U64: sprintf(buf, lv->val.u64 < 10000 ? "%llu" : "0x%016llX", lv->val.u64); break;
    case LV_F32: dbl2str(buf, lv->val.f32); break;
    case LV_F64: dbl2str(buf, lv->val.f64); break;
    default    : return luaL_error(L, "LuaVariant type error: %d", lv->type);
    }
    lua_pushstring(L, buf);
    return 1;
}

static int lua_variant_bin(lua_State *L, struct LuaVariant *lv)
{
    const char *ptr  = (const char *)&lv->val.u64;
    size_t      size = 0;
    switch (lv->type)
    {
    case LV_I8 :
    case LV_U8 : size = 1; break;
    case LV_I16:
    case LV_U16: size = 2; break;
    case LV_I32:
    case LV_U32:
    case LV_F32: size = 4; break;
    case LV_I64:
    case LV_U64:
    case LV_F64: size = 8; break;
    default    : return luaL_error(L, "LuaVariant type error: %d", lv->type);
    }
    lua_pushlstring(L, ptr, size);
    return 1;
}

static bool lv_hex(char *s, struct LuaVariant *lv)
{
    switch (lv->type)
    {
    case LV_I8 :
    case LV_U8 : sprintf(s, "%hX", (uint16_t)lv->val.u8); break;
    case LV_I16:
    case LV_U16: sprintf(s, "%hX", lv->val.u16); break;
    case LV_I32:
    case LV_U32:
    case LV_F32: sprintf(s, "%X", lv->val.u32); break;
    case LV_I64:
    case LV_U64:
    case LV_F64: sprintf(s, "%llX", lv->val.u64); break;
    default    : return false;
    }
    return true;
}

static int lua_variant_hex(lua_State *L, struct LuaVariant *lv)
{
    char buf[0x11];
    if (!lv_hex(buf, lv)) return luaL_error(L, "LuaVariant type error: %d", lv->type);
    lua_pushstring(L, buf);
    return 1;
}

static bool lv_str(lua_State *L, struct LuaVariant *lv, char *buf)
{
    switch (lv->type)
    {
    case LV_I8 : sprintf(buf, "%hd", (int16_t)lv->val.i8); break;
    case LV_U8 : sprintf(buf, "%hu", (uint16_t)lv->val.u8); break;
    case LV_I16: sprintf(buf, "%hd", lv->val.i16); break;
    case LV_U16: sprintf(buf, "%hu", lv->val.u16); break;
    case LV_I32: sprintf(buf, "%d", lv->val.i32); break;
    case LV_U32: sprintf(buf, "%u", lv->val.u32); break;
    case LV_I64: sprintf(buf, "%lld", lv->val.i64); break;
    case LV_U64: sprintf(buf, "%llu", lv->val.u64); break;
    case LV_F32: dbl2str(buf, lv->val.f32); break;
    case LV_F64: dbl2str(buf, lv->val.f64); break;
    default    : return false;
    }
    return true;
}

static int lua_variant_val(lua_State *L, struct LuaVariant *lv)
{
    char buf[0x20];
    if (!lv_str(L, lv, buf)) return luaL_error(L, "LuaVariant type error: %d", lv->type);
    lua_pushstring(L, buf);
    return 1;
}

static int lua_variant_rawval(lua_State *L, struct LuaVariant *lv)
{
    switch (lv->type)
    {
    case LV_I8 : lua_pushinteger(L, (lua_Integer)lv->val.i8); break;
    case LV_U8 : lua_pushinteger(L, (lua_Integer)lv->val.u8); break;
    case LV_I16: lua_pushinteger(L, (lua_Integer)lv->val.i16); break;
    case LV_U16: lua_pushinteger(L, (lua_Integer)lv->val.u16); break;
    case LV_I32: lua_pushinteger(L, (lua_Integer)lv->val.i32); break;
    case LV_U32: lua_pushinteger(L, (lua_Integer)lv->val.u32); break;
    case LV_I64: lua_pushinteger(L, (lua_Integer)lv->val.i64); break;
    case LV_U64: lua_pushinteger(L, (lua_Integer)lv->val.u64); break;
    case LV_F32: lua_pushnumber(L, (lua_Number)lv->val.f32); break;
    case LV_F64: lua_pushnumber(L, (lua_Number)lv->val.f64); break;
    default    : return luaL_error(L, "LuaVariant type error: %d", lv->type);
    }
    return 1;
}

static int lua_variant_as_fun(lua_State *L)
{
    struct LuaVariant *lv = lv_check(L, 1, LV_ANY);
    const char        *t  = luaL_checkstring(L, 2);
    if (!strcmp(t, "i8")) lv_push(L, LV_I8)->val.i8 = lv->val.i8;
    else if (!strcmp(t, "u8")) lv_push(L, LV_U8)->val.u8 = lv->val.u8;
    else if (!strcmp(t, "i16")) lv_push(L, LV_I16)->val.i16 = lv->val.i16;
    else if (!strcmp(t, "u16")) lv_push(L, LV_U16)->val.u16 = lv->val.u16;
    else if (!strcmp(t, "i32")) lv_push(L, LV_I32)->val.i32 = lv->val.i32;
    else if (!strcmp(t, "u32")) lv_push(L, LV_U32)->val.u32 = lv->val.u32;
    else if (!strcmp(t, "i64")) lv_push(L, LV_I64)->val.i64 = lv->val.i64;
    else if (!strcmp(t, "u64")) lv_push(L, LV_U64)->val.u64 = lv->val.u64;
    else if (!strcmp(t, "f32")) lv_push(L, LV_F32)->val.f32 = lv->val.f32;
    else if (!strcmp(t, "f64")) lv_push(L, LV_F64)->val.f64 = lv->val.f64;
    else
    {
        lua_pushliteral(L, "as argument #2 available: i8、u8、i16、u16、i32、u32、i64、u64、f32、f64");
        return lua_error(L);
    }
    return 1;
}

static int lua_variant_as(lua_State *L, struct LuaVariant *lv)
{
    lua_pushcfunction(L, lua_variant_as_fun);
    return 1;
}

static int lua_variant_to_fun(lua_State *L)
{
    struct LuaVariant *lv = lv_check(L, 1, LV_ANY);
    const char        *t  = luaL_checkstring(L, 2);
    if (!strcmp(t, "i8")) get_lv_val_s(lv, lv_push(L, LV_I8)->val.i8);
    else if (!strcmp(t, "u8")) get_lv_val_s(lv, lv_push(L, LV_U8)->val.u8);
    else if (!strcmp(t, "i16")) get_lv_val_s(lv, lv_push(L, LV_I16)->val.i16);
    else if (!strcmp(t, "u16")) get_lv_val_s(lv, lv_push(L, LV_U16)->val.u16);
    else if (!strcmp(t, "i32")) get_lv_val_s(lv, lv_push(L, LV_I32)->val.i32);
    else if (!strcmp(t, "u32")) get_lv_val_s(lv, lv_push(L, LV_U32)->val.u32);
    else if (!strcmp(t, "i64")) get_lv_val_s(lv, lv_push(L, LV_I64)->val.i64);
    else if (!strcmp(t, "u64")) get_lv_val_s(lv, lv_push(L, LV_U64)->val.u64);
    else if (!strcmp(t, "f32")) get_lv_val_s(lv, lv_push(L, LV_F32)->val.f32);
    else if (!strcmp(t, "f64")) get_lv_val_s(lv, lv_push(L, LV_F64)->val.f64);
    else
    {
        lua_pushliteral(L, "as argument #2 available: i8、u8、i16、u16、i32、u32、i64、u64、f32、f64");
        return lua_error(L);
    }
    return 1;
}

static int lua_variant_to(lua_State *L, struct LuaVariant *lv)
{
    lua_pushcfunction(L, lua_variant_to_fun);
    return 1;
}

static int lua_variant_type(lua_State *L, struct LuaVariant *lv)
{
    switch (lv->type)
    {
    case LV_I8 : lua_pushliteral(L, "i8"); break;
    case LV_U8 : lua_pushliteral(L, "u8"); break;
    case LV_I16: lua_pushliteral(L, "i16"); break;
    case LV_U16: lua_pushliteral(L, "u16"); break;
    case LV_I32: lua_pushliteral(L, "i32"); break;
    case LV_U32: lua_pushliteral(L, "u32"); break;
    case LV_I64: lua_pushliteral(L, "i64"); break;
    case LV_U64: lua_pushliteral(L, "u64"); break;
    case LV_F32: lua_pushliteral(L, "f32"); break;
    case LV_F64: lua_pushliteral(L, "f64"); break;
    default    : return luaL_error(L, "LuaVariant type error: %d", lv->type);
    }
    return 1;
}
