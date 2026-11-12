/* =============================================================================
 * src/scripting/lua/file_io.cpp —— 「文件在客户端那台机器上」的进程级开关（见头文件）
 *
 * 来源：原 `file_io.cpp`（read_file / write_file + 效果协议 + base64，约 530 行）。
 * T5（D44）删除了效果协议与 base64；本文件只剩一个布尔量 —— 留着独立文件是因为
 *   `registry.cpp` / `open_file` / `tcp_server.cpp` 都通过它问「文件该去哪一侧读」。
 * ========================================================================== */
#include "file_io.hpp"

namespace autodbg::host
{
namespace
{
bool g_client_io = false;
} /* namespace */

void set_client_io(bool on)
{
    g_client_io = on;
}

bool client_io_enabled()
{
    return g_client_io;
}
} /* namespace autodbg::host */
