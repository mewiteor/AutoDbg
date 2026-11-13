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
 *
 * T7 切片 C-remote（D56）：`--remote` 用的那条「连接 + 握手 + 发 script + 收 result」的路
 *   **也**给远端会话代理（`dbg.remote` → `lualib/dbglib.lua`）用——但**不做第二套 TCP**。
 *   所以下面三个函数把连接对象与发送/接收抽出来（实现仍在 `client.cpp`，与 `client_main`
 *   共用同一份 `write_frame` / `read_frame` / `net_*`）。区别只有两点：
 *   · `remote_run_script` 把 `result` 帧的 **json 原文**交回去（C 侧不解析，反序列化归 Lua 的
 *     `jsonx.decode`；这正是 `result.value` 存在的理由）；
 *   · 它有**整体超时**（`timeout_ms`）——代理是在别人的脚本里被调的，不能像 `--remote`
 *     那样无上限地等。
 * ========================================================================== */
#include <stddef.h>

#include "../../sys/pack/boot.h"

namespace autodbg::host
{
/* 返回进程退出码（0 = 成功；2 = 连接/协议错误；否则透传 server 报的脚本退出码） */
int client_main(const autodbg_boot *boot);

/* ---- 可复用的远端传输（T7 C-remote，D56） ----
 * 连接对象对调用方**不透明**（只用指针；定义在 `client.cpp`）。 */
struct remote_conn;

/* 连接 + 握手（`id` = 协议版本、`data` = token）→ 等 `info`。
 * 成功返回连接对象（`remote_close` 释放）；失败 → nullptr，并把原因写进 `err`（ASCII，
 * 含服务端拒给时的 json 原文）。`read_timeout_ms` / `max_frame` 传 0 → 用缺省值。 */
remote_conn *remote_connect(const char *host, unsigned port, const char *token, unsigned read_timeout_ms,
                            unsigned max_frame, char *err, size_t errcap);

/* 发一条 `script` 帧（`id` = 连接内递增的命令号，D2a 的归因），收帧直到 `result` / `end`。
 * 成功 → `result` 帧的 json 原文（`common::alloc`，**调用方 `common::release`**）；失败 → nullptr。
 * ❗`out` / `event` 帧在代理路径上直接丢弃（服务端的 REPL 会把返回值 print 出来）；
 *   `open` / `data` / `read_req` / `close`（`open_file`）**不支持**——它们要么需要本地文件，
 *   要么会让服务端阻塞等回包（最终表现为超时）。`timeout_ms` 传 0 → 不设上限。 */
const char *remote_run_script(remote_conn *c, const void *src, size_t len, const char *chunk, unsigned timeout_ms,
                              char *err, size_t errcap);

/* 关连接并释放（`nullptr` 无害）。 */
void remote_close(remote_conn *c);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_CLIENT_HPP */
