/* =============================================================================
 * src/scripting/lua/variant.hpp —— LuaVariant 的 C 接口（P3.B 模块 12：原 lua_variant.h）
 *
 * 实现是 C++（variant.cpp），但**对外保持 C 链接**：绑定层（C++）与
 * tests/unit/test_lua_variant.c（C）都直接调这些 `lv_*` 符号。
 * ========================================================================== */
#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h> /* C 侧需要（下面两个纯计算核心的声明用了 bool）；C++ 侧无副作用 */
#pragma once
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <stdint.h>

#pragma pack(push, 1)

enum LV_type
{
    LV_I8,
    LV_U8,
    LV_I16,
    LV_U16,
    LV_I32,
    LV_U32,
    LV_I64,
    LV_U64,
    LV_F32,
    LV_F64,

    LV_MAX,
    LV_ANY = -1
};

union LuaVariantVal
{
    int8_t   i8;
    uint8_t  u8;
    int16_t  i16;
    uint16_t u16;
    int32_t  i32;
    uint32_t u32;
    int64_t  i64;
    uint64_t u64;
    float    f32;
    double   f64;
};

struct LuaVariant
{
    enum LV_type type;

    union
    {
        union LuaVariantVal val;
        uint8_t             buf[sizeof(union LuaVariantVal)];
    };
};
#ifdef _WIN64
#   define LV_UZ LV_U64
#   define LV_IZ LV_I64
#   define UZ    "U64"
#   define uz    u64
#   define iz    i64
#else
#   define LV_UZ LV_U32
#   define LV_IZ LV_I32
#   define UZ    "U32"
#   define uz    u32
#   define iz    i32
#endif
#pragma pack(pop)

void               lv_init(lua_State *L);
struct LuaVariant *lv_push(lua_State *L, enum LV_type type);
struct LuaVariant *lv_check(lua_State *L, int index, enum LV_type type);
struct LuaVariant *parse_lv(lua_State *L, int index, struct LuaVariant *lvt);
size_t             lv_size(struct LuaVariant *lv);

/* 纯计算核心：原本**只在实现文件和 C 单测里各自声明**（所以转 C++ 后 C 单测链接不到）
 * → P3.B 模块 12 把声明收进头文件，保证 C 链接。 */
void init_lv_neg_v(const struct LuaVariant *lv, bool *neg, uint64_t *v);
void lv_add_i(struct LuaVariant *lvr, bool neg1, uint64_t v1, bool neg2, uint64_t v2);

#ifdef __cplusplus
}
#endif