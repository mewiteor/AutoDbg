#ifndef AUTODBG_SCRIPTING_LUA_FILE_IO_HPP
#define AUTODBG_SCRIPTING_LUA_FILE_IO_HPP
/* =============================================================================
 * src/scripting/lua/file_io.hpp —— 「文件在客户端那台机器上」的进程级开关
 *
 * 来源：原 `file_io.cpp`（read_file / write_file + effects 协议 + base64）。
 * T5（D44）删掉了效果协议那一整套，本头文件只剩：
 *   · `set_client_io` / `client_io_enabled`：server 模式为 true → `open_file()`
 *     走帧协议（文件在**客户端**机器上）；本地脚本模式为 false → 就是 `io.open`；
 *   · `register_stream_file_api`：注册 `open_file` 与文件对象（实现见 `file_stream.cpp`）；
 *   · `set_client_read_timeout`：一次文件操作/等回包的上限（ABI 4 的 `--read-timeout`）。
 * ❗审计记录（为什么删）：`read_file` / `write_file` 那套靠 `lua_yield` + 效果回调，
 *   在调试回调里用不了（LuaJIT 跨不了 C 边界），且要 base64 过 JSON；
 *   现在的唯一文件 API 是 `open_file` 的文件对象（io 风格，帧里传原始字节）。
 * ========================================================================== */
#include <stddef.h>

#include <lua.hpp>

namespace autodbg::host
{
/* server 模式（文件在客户端）开关 */
void set_client_io(bool on);
bool client_io_enabled();

/* 注册 `open_file` 与文件对象（流式 client 文件 I/O；实现见 `file_stream.cpp`） */
void register_stream_file_api(lua_State *L);

/* T4（D45）：一次文件操作（等客户端回包）的等待上限；默认 5000 ms。
 * 宿主层用 `boot->read_timeout_ms` 覆盖（ABI 4 的 `--read-timeout=`）。 */
void set_client_read_timeout(unsigned ms);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_FILE_IO_HPP */
