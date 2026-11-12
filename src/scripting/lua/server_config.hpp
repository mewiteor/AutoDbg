#ifndef AUTODBG_SCRIPTING_LUA_SERVER_CONFIG_HPP
#define AUTODBG_SCRIPTING_LUA_SERVER_CONFIG_HPP
/* =============================================================================
 * src/scripting/lua/server_config.hpp —— server / 客户端的配置解析（D21/D27）
 *
 * 来源：从 `server.cpp`（HTTP，已在 T5 删除）逐字搬出，语义不变：
 *   · **启动器**只把 `--server*` 的原始字符串塞进 `autodbg_boot`（不认识语义）；
 *   · 本层把原始串解析成 `server_config`，并按 D21 决定「要不要 token」；
 *   · raw TCP 服务端（`tcp_server.cpp`）与 `--remote` 客户端都用这份配置。
 * 为什么解析放在模块侧：token 策略依赖 loopback 判定与 `rand_hex`，都是 common 层的东西；
 *   放模块侧能保证「同一个 exe 在 9x 上回退 legacy 后行为一致」。
 * ========================================================================== */
#include <stddef.h>

#include "../../sys/pack/boot.h"

namespace autodbg::server
{
/* 解析后的配置 */
struct server_config
{
    char     host[64];      /* 监听/连接地址（IPv4 点分或 "localhost"） */
    unsigned port;          /* 1..65535 */
    bool     auth_required; /* D21：loopback 缺省 false；非 loopback 恒 true */
    char     token[80];     /* auth_required 时的 token（hex） */
    unsigned max_body;      /* 一帧的上限（默认 8388608；`--server-max-body`） */
    unsigned queue_depth;   /* 队列深度（默认 8；T5 之后 raw TCP 串行，仅诊断用） */
    unsigned timeout_sec;   /* 单次执行超时（0 = 不限） */
    char     log_path[260]; /* 诊断日志路径（空串 = 不打） */
    bool     echo;          /* 执行输出是否同时回显到控制台（默认 true） */
};

enum server_config_status
{
    server_config_ok = 0,
    server_config_bad_addr,   /* --server-addr 不是 host:port */
    server_config_bad_port,   /* 端口不是 1..65535 */
    server_config_bad_number, /* max-body/queue/timeout 不是数字 */
    server_config_need_token, /* 非 loopback 却没给 token（D21） */
    server_config_bad_token,  /* 显式 token 太短（< 8 字符） */
};

/* 解析 boot 里的原始串 → cfg。失败时 `err` 写一句给人看的中文（UTF-8）。 */
server_config_status server_config_from_boot(const autodbg_boot *boot, server_config *cfg, char *err, size_t err_cap);

/* 状态码 → 短名（日志用，ASCII） */
const char *server_config_status_name(server_config_status s);
} /* namespace autodbg::server */

#endif /* AUTODBG_SCRIPTING_LUA_SERVER_CONFIG_HPP */
