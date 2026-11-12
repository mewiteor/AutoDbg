/* =============================================================================
 * src/scripting/lua/server_config.cpp —— 配置解析实现（见 server_config.hpp）
 *
 * 来源：从 `server.cpp`（HTTP，T5 删除）逐字搬出 —— `split_addr` / `parse_u32` /
 *   `set_err` / `is_loopback_host` 与 `server_config_from_boot` 的判断顺序都没动，
 *   只是把「解析」与「HTTP 传输」解耦（D41 之后只剩 raw TCP 需要它）。
 * ========================================================================== */
#include "server_config.hpp"

#include "common/rand.hpp" /* D21：非 loopback 的 token 自动生成 */

#include <string.h>

namespace autodbg::server
{
namespace
{
bool str_eq(const char *a, const char *b)
{
    return (a != nullptr && b != nullptr) ? (strcmp(a, b) == 0) : false;
}

bool parse_u32(const char *s, unsigned *out, unsigned min_v, unsigned max_v)
{
    if (s == nullptr || *s == '\0')
        return false;
    unsigned long long v = 0;
    for (const char *p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 0xFFFFFFFFull)
            return false;
    }
    if (v < min_v || v > max_v)
        return false;
    *out = (unsigned)v;
    return true;
}

bool split_addr(const char *s, char *host, size_t host_cap, unsigned *port)
{
    if (s == nullptr || host == nullptr || port == nullptr || host_cap == 0)
        return false;

    const char *colon = nullptr;
    for (const char *p = s; *p != '\0'; p++)
    {
        if (*p == ':')
            colon = p;
    }
    if (colon == nullptr || colon == s)
        return false;

    size_t hlen = (size_t)(colon - s);
    if (hlen + 1 > host_cap)
        return false;
    memcpy(host, s, hlen);
    host[hlen] = '\0';

    unsigned p = 0;
    if (!parse_u32(colon + 1, &p, 1, 65535))
        return false;
    *port = p;
    return true;
}

void set_err(char *err, size_t cap, const char *text)
{
    if (err == nullptr || cap == 0)
        return;
    size_t n = 0;
    if (text != nullptr)
    {
        while (text[n] != '\0' && n + 1 < cap)
        {
            err[n] = text[n];
            n++;
        }
    }
    err[n] = '\0';
}

bool is_loopback_host(const char *host)
{
    if (host == nullptr)
        return false;
    if (strcmp(host, "localhost") == 0)
        return true;
    if (strcmp(host, "127.0.0.1") == 0)
        return true;
    if (strncmp(host, "127.", 4) == 0)
        return true;
    return false;
}
} /* namespace */

const char *server_config_status_name(server_config_status s)
{
    switch (s)
    {
    case server_config_ok:         return "ok";
    case server_config_bad_addr:   return "bad-addr";
    case server_config_bad_port:   return "bad-port";
    case server_config_bad_number: return "bad-number";
    case server_config_need_token: return "need-token";
    case server_config_bad_token:  return "bad-token";
    default:                       return "unknown";
    }
}

server_config_status server_config_from_boot(const autodbg_boot *boot, server_config *cfg, char *err, size_t err_cap)
{
    if (err != nullptr && err_cap > 0)
        err[0] = '\0';
    if (boot == nullptr || cfg == nullptr)
        return server_config_bad_addr;

    memset(cfg, 0, sizeof *cfg);

    cfg->max_body    = (boot->server_max_body != 0) ? boot->server_max_body : 8388608u;
    cfg->queue_depth = (boot->server_queue != 0) ? boot->server_queue : 8u;
    cfg->timeout_sec = boot->server_timeout_sec;
    cfg->echo        = (boot->server_no_echo == 0);

    if (boot->server_log != nullptr && boot->server_log[0] != '\0')
    {
        size_t n = strlen(boot->server_log);
        if (n + 1 > sizeof cfg->log_path)
        {
            set_err(err, err_cap, "--server-log 路径过长");
            return server_config_bad_addr;
        }
        memcpy(cfg->log_path, boot->server_log, n + 1);
    }

    const char *addr = (boot->server_addr != nullptr && boot->server_addr[0] != '\0')
                           ? boot->server_addr
                           : "127.0.0.1:8642";
    if (!split_addr(addr, cfg->host, sizeof cfg->host, &cfg->port))
    {
        set_err(err, err_cap, "--server-addr 必须是 host:port（v1 只支持 IPv4/localhost）");
        return server_config_bad_addr;
    }

    bool loopback = is_loopback_host(cfg->host);

    /* ---- D21：token 策略 ---- */
    const char *tok = boot->server_token;
    if (tok == nullptr || tok[0] == '\0')
    {
        if (loopback)
        {
            cfg->auth_required = false; /* loopback 免 token（用户已拍板） */
        }
        else
        {
            cfg->auth_required = true; /* 非 loopback：自动生成 */
            if (!autodbg::common::rand_hex(cfg->token, sizeof cfg->token, 16))
            {
                set_err(err, err_cap, "token 生成失败");
                return server_config_bad_token;
            }
        }
    }
    else if (str_eq(tok, "auto"))
    {
        cfg->auth_required = true;
        if (!autodbg::common::rand_hex(cfg->token, sizeof cfg->token, 16))
        {
            set_err(err, err_cap, "token 生成失败");
            return server_config_bad_token;
        }
    }
    else if (str_eq(tok, "none"))
    {
        if (!loopback)
        {
            set_err(err, err_cap, "非 loopback 地址不允许 --server-token=none（D21）");
            return server_config_need_token;
        }
        cfg->auth_required = false;
    }
    else
    {
        size_t n = strlen(tok);
        if (n < 8)
        {
            set_err(err, err_cap, "显式 token 至少 8 个字符");
            return server_config_bad_token;
        }
        if (n + 1 > sizeof cfg->token)
        {
            set_err(err, err_cap, "token 过长");
            return server_config_bad_token;
        }
        memcpy(cfg->token, tok, n + 1);
        cfg->auth_required = true;
    }

    return server_config_ok;
}
} /* namespace autodbg::server */
