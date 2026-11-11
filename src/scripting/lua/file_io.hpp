#ifndef AUTODBG_SCRIPTING_LUA_FILE_IO_HPP
#define AUTODBG_SCRIPTING_LUA_FILE_IO_HPP
/* =============================================================================
 * src/scripting/lua/file_io.hpp —— `read_file` / `write_file`（**文件在客户端机器上**）
 *
 * 来源：新写（client-side file I/O，S7）。
 * 语义：
 *   · 非 server 模式（`AutoDbg.exe script.lua`）：直接读写**本机**文件系统；
 *   · server 模式：文件在**客户端**那台机器上 → 本层 `lua_yield` 一个「效果」给宿主，
 *     宿主把它交给客户端执行，客户端回传结果后宿主再 `resume`。
 *     效果协议见 plan_server.md §14（`POST /effects`）。
 * 二进制安全：全程按字节处理，不做编码转换；跨 JSON 的传输用 base64。
 * 常量上限：单次读/写 ≤ `k_file_effect_max`（默认 8 MB，与请求体上限一致）。
 * ========================================================================== */
#include <stddef.h>

#include <lua.hpp>

namespace autodbg::host
{
constexpr size_t k_file_effect_max = 8u * 1024u * 1024u;

/* server 模式开关（server_main 启动时置 true；默认 false = 本机文件） */
void set_client_io(bool on);
bool client_io_enabled();

/* 本机实现（非 server 模式用；也给 selftest/e2e 用） */
bool local_read_file(const char *path, long long offset, long long length,
                     char **data, size_t *n, char *err, size_t err_cap);
bool local_write_file(const char *path, const void *data, size_t n, bool append,
                      size_t *written, char *err, size_t err_cap);

/* 注册全局 `read_file` / `write_file`（在 `register_lua_api` 里调一次） */
void register_file_api(lua_State *L);

/* 注册 `open_file` 与文件对象（流式 client 文件 I/O；实现见 `file_stream.cpp`） */
void register_stream_file_api(lua_State *L);

/* ---- 给宿主（server）用的效果协议助手 ---- */
/* 把「协程里 yield 出来的效果表」（idx 处）序列化成 JSON：
 *   { "id":1, "op":"read_file", "path":"…", "offset":0, "length":0 }
 *   { "id":2, "op":"write_file", "path":"…", "data":"<base64>", "append":false }
 * 成功时 `*out` 是新分配缓冲（common::release 释放）。 */
bool effect_to_json(lua_State *L, int idx, unsigned effect_id, char **out, size_t *out_len);

/* 读效果表里的 `op`（"read_file" / "write_file"），失败返回 false */
bool effect_op(lua_State *L, int idx, char *buf, size_t cap);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_FILE_IO_HPP */
