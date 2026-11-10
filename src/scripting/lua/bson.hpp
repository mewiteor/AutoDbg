#ifndef AUTODBG_SCRIPTING_LUA_BSON_HPP
#define AUTODBG_SCRIPTING_LUA_BSON_HPP
/* =============================================================================
 * src/scripting/lua/bson.hpp —— BSON 编码的 C 接口（P3.B 模块 12：原 bson.h）
 *
 * 实现是 C++（bson.cpp），但**对外保持 C 链接**（extern "C" 护卫）：
 *   · 绑定层（C++）用它注册 `BsonEncode`/`BsonDecode`；
 *   · tests/unit/test_bson.c（C）用它逐字节锁 BSON 布局。
 *
 * 注：`Buf` 与 `create_*` 原先只在 bson.c 内部声明（bson.h 只有两个 `lua_*`），
 * 导致 C 单测不得不在自己文件里**复制**一份定义 —— 按 test_bson.c 注释里的计划，
 * P3.B 模块 12 把它们正式收进本头文件。
 * ========================================================================== */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ❗`<lua.h>` 必须放在 `extern "C"` **内**：Lua 的 C 头没有 C++ 护卫，
 *   否则 C++ TU 会把 `lua_*` 当成 C++ 符号引用（链接不到）——与 variant.hpp 同一做法。 */
#include <lua.h>

/* 可增长缓冲区（追加用 `resize` 增长；`BufFree` 释放本体与缓冲） */
typedef struct
{
    char  *s;
    size_t n;
} Buf;

void BufAppendChar(Buf *l, char r);
void BufAppendStr(Buf *l, const char *r, uint32_t n);
void BufAppend(Buf *l, const Buf *r);
void BufFree(Buf *v);

Buf *create_nil(const char *key);
Buf *create_boolean(const char *key, bool value);
#ifndef JIT
Buf *create_integer(const char *key, lua_Integer number);
#endif
Buf *create_number(const char *key, lua_Number number);
Buf *create_lua_variant(const char *key, struct LuaVariant *lv);
Buf *create_bytes(const char *key, const char *s, uint32_t sn);
Buf *create_table(lua_State *L, int n, bool *is_arr);
Buf *create_item(const char *key, lua_State *L, int n);

int lua_bson_encode(lua_State *L);
int lua_bson_decode(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* AUTODBG_SCRIPTING_LUA_BSON_HPP */
