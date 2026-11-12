#ifndef AUTODBG_SCRIPTING_LUA_TCP_SERVER_HPP
#define AUTODBG_SCRIPTING_LUA_TCP_SERVER_HPP
/* =============================================================================
 * src/scripting/lua/tcp_server.hpp —— raw TCP 帧服务端（plan_cli.md T2 / D42）
 *
 * 与 `server.cpp`（HTTP）的关系：并行存在，互不引用。`--server=tcp` 走这里，
 *   `--server` / `--server=http` 仍是旧的 HTTP（T5 随 D41 一起删）。
 *
 * 为什么单独一个 TU：HTTP 那份要约 1900 行且整段将被删除；TCP 这份是留下来的那半，
 *   现在分开，T5 只需要删 `server.cpp` / `http.*`，不用在这堆代码里刨。
 *
 * 协议（帧类型见 `stream.hpp`）：
 *   连接 = 握手帧 → （script | control | end)*。一次连接**串行**处理（D43 无状态），
 *   每个 script 帧 = 一次一次性执行（建状态 → 执行 → 全释放）。
 * ========================================================================== */
#include <stddef.h>

#include "../../sys/pack/boot.h"

namespace autodbg::server
{
/* 模块入口：`autodbg_run` 在 `boot->server_mode == 2` 时调这里。
 * 返回进程退出码（0 = 正常停机；2 = 配置/网络/监听失败）。 */
int tcp_server_main(const autodbg_boot *boot);
} /* namespace autodbg::server */

#endif /* AUTODBG_SCRIPTING_LUA_TCP_SERVER_HPP */
