#ifndef AUTODBG_SCRIPTING_LUA_CLIENT_HPP
#define AUTODBG_SCRIPTING_LUA_CLIENT_HPP
/* =============================================================================
 * src/scripting/lua/client.hpp —— raw TCP 帧客户端（plan_cli.md T3 / D42）
 *
 * 职责：`--remote=<host:port>` 时，模块不再跑本地脚本，而是：
 *   ① 连 server → 握手帧（`id` = 协议版本、`data` = token）→ 等 `info` 应答；
 *   ② `remote_cmd == 0`：读本地脚本文件 → `script` 帧 → 把 `out` 帧打到
 *      stdout/stderr（`--json` 时先收着）→ 收 `result` / `end` → 用 `result` 帧的
 *      `id`（退出码）作为本进程退出码；
 *   ③ `remote_cmd != 0`：发 `control` 帧（cancel / kill / detach / health / version），
 *      把 `ack` / `info` 的 json 打到 stdout，按 `applied` 决定退出码。
 *
 * ❗D28：客户端**不解析 JSON**——退出码走 `result` 帧的 `id`、状态走 `flags` 位
 *   （见 `stream.hpp` 的 result 帧说明）。
 * ========================================================================== */
#include <stddef.h>

#include "../../sys/pack/boot.h"

namespace autodbg::host
{
/* 返回进程退出码（0 = 成功；2 = 连接/协议错误；否则透传 server 报的脚本退出码） */
int client_main(const autodbg_boot *boot);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_CLIENT_HPP */
