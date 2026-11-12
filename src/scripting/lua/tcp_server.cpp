/* =============================================================================
 * src/scripting/lua/tcp_server.cpp —— raw TCP 帧服务端（T2：监听 + 握手 + control/ack/info）
 *
 * 来源：新写（plan_cli.md §2.1/§2.2 与 D42/D43）。与 `server.cpp`（HTTP）并存、互不引用。
 *
 * 本文件这一版（T2 第一段）负责：
 *   ① 监听 + accept + **串行**处理（D43：一次调用无状态、并发 1）；
 *   ② **握手帧**：`id` = 协议版本、`data` = token（见 stream.hpp 的字段落点说明）；
 *      版本不符 / token 不符 → `error` 帧 + 关连接；
 *   ③ `script` 帧 → 一次性执行（建状态 → 跑 → 全释放）→ `result` + `end` 帧；
 *   ④ `control` 帧：`health` / `version` → `info` 帧；`cancel` → guard + `ack`；
 *      `kill` / `detach` 先回 `ack{applied:false}`（软动作在 T4 接 on_wait_timeout）；
 *      `shutdown` → 停机；
 *   ⑤ `end` 帧 → 关连接。
 *
 * ❗还没做的（后续小刀）：执行期间的 control 读取（两个 poll 点：`on_wait_timeout` 的
 *   100 ms + `lua_sethook` 的计数钩子）—— 现在执行是同步的，控制帧会排到执行之后才被读到。
 *
 * D28 约束的落地：控制命令放 `flags`、版本放 `id`、token 放 `data`，**C 侧不解析 JSON**
 *   （只写 JSON）；握手 json 里剩下的字段留给执行侧用 `jsonx.decode`。
 * ========================================================================== */
#include "tcp_server.hpp"

#include "exec.hpp"        /* exec_script：一次性执行（D22/D43） */
#include "exec_guard.hpp"  /* guard_exec_begin / guard_request_cancel / guard_stop_reason */
#include "output.hpp"      /* output_begin / output_end：stdout 收进缓冲并发 `out` 帧 */
#include "server.hpp"      /* server_config + server_config_from_boot（复用 S0 的解析/D21 策略） */
#include "stream.hpp"      /* 帧协议（类型/版本/传输钩子） */

#include "common/frame.hpp" /* k_frame_header_size / k_frame_max_payload（帧头常量） */
#include "common/json_writer.hpp"
#include "common/net.hpp"

#include "core/util.hpp" /* PrintS / PrintF / ErrorS */
#include "../../sys/version.h" /* AUTODBG_VERSION：构建期由 make.bat 生成（git describe） */

#include <windows.h>

#include <string.h>

namespace autodbg::server
{
namespace
{
constexpr const char *k_server_version    = AUTODBG_VERSION;
constexpr const char *k_server_ver_source = AUTODBG_VERSION_SOURCE;

constexpr int k_accept_timeout_ms = 250;   /* 让主循环有机会检查 g_shutdown */
constexpr int k_recv_timeout_ms   = 10000; /* 单次 recv 等多久算「对端不说话」 */
constexpr int k_send_timeout_ms   = 10000;

volatile LONG g_shutdown = 0;

BOOL WINAPI on_console_ctrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_shutdown, 1);
    return TRUE;
}

/* 传输钩子：本文件只有一条执行连接，句柄放在全局（D43：串行、无并发） */
long long g_conn = -1;

bool conn_send(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    if (g_conn < 0)
        return false;
    return autodbg::common::net_send_all(g_conn, data, len, k_send_timeout_ms);
}

int conn_recv(void *ctx, void *buf, size_t cap)
{
    (void)ctx;
    if (g_conn < 0)
        return -1;
    return autodbg::common::net_recv(g_conn, buf, cap, k_recv_timeout_ms);
}

const char *flavor_name(unsigned flavor)
{
    return (flavor == 1u) ? "luajit" : "lua";
}

const char *variant_name(unsigned variant)
{
    return (variant == 1u) ? "legacy" : "modern";
}

const char *arch_name()
{
#if defined(_WIN64)
    return "x64";
#else
    return "x86";
#endif
}

/* 常量时间比较（token 不是秘密字符串的「长度」也不泄露）：长度一致才逐字节比 */
bool token_matches(const server_config &cfg, const char *got, size_t got_len)
{
    size_t want = strlen(cfg.token);
    if (want == 0 || got == nullptr || got_len != want)
        return false;
    unsigned diff = 0;
    for (size_t i = 0; i < want; i++)
        diff |= (unsigned)((unsigned char)cfg.token[i] ^ (unsigned char)got[i]);
    return diff == 0;
}

/* 写一帧 json（json 由 json_writer 产出，调用方负责释放；这里只发送） */
bool send_json_frame(unsigned type, unsigned id, unsigned flags, autodbg::common::json_writer &w)
{
    if (w.data() == nullptr)
        return false;
    return autodbg::host::stream_write_frame(type, id, flags, w.data(), w.size(), nullptr, 0);
}

/* `error` 帧 + 关连接（协议层致命错误都走这里） */
void fail_connection(const char *code)
{
    autodbg::common::json_writer w;
    w.begin_object();
    w.key("error");
    w.string(code);
    w.end_object();
    send_json_frame(autodbg::host::stream_frame_error, 0, 0, w);
}

/* `ack` 帧：控制命令的实际结果（applied / reason / pid） */
void send_ack(unsigned cmd, bool applied, const char *reason, unsigned long pid)
{
    autodbg::common::json_writer w;
    w.begin_object();
    w.key("cmd");
    w.number((long long)cmd);
    w.key("applied");
    w.boolean(applied);
    w.key("reason");
    w.string(reason);
    w.key("pid");
    w.number((long long)pid);
    w.end_object();
    send_json_frame(autodbg::host::stream_frame_ack, 0, 0, w);
}

/* `info` 帧：health / version 的应答（替代 GET /health、/version） */
void send_info(const autodbg_boot *boot, const server_config &cfg, bool with_version)
{
    autodbg::common::json_writer w;
    w.begin_object();
    w.key("ok");
    w.boolean(true);
    w.key("server");
    w.string("T2-tcp");
    w.key("transport");
    w.string("tcp");
    w.key("proto");
    w.number((long long)autodbg::host::k_stream_proto_version);
    w.key("busy");
    w.boolean(autodbg::host::guard_exec_active());
    w.key("auth");
    w.boolean(cfg.auth_required);
    w.key("max_frame");
    w.number((long long)cfg.max_body);
    if (with_version)
    {
        w.key("version");
        w.string(k_server_version);
        w.key("version_source");
        w.string(k_server_ver_source);
        w.key("abi");
        w.number((long long)AUTODBG_ABI_VERSION);
        w.key("arch");
        w.string(arch_name());
        w.key("flavor");
        w.string(flavor_name(boot->flavor));
        w.key("variant");
        w.string(variant_name(boot->variant));
        w.key("winsock");
        w.string(autodbg::common::net_module_name() != nullptr ? autodbg::common::net_module_name() : "(none)");
    }
    w.end_object();
    send_json_frame(autodbg::host::stream_frame_info, 0, 0, w);
}

/* 执行一个 `script` 帧：一次性执行（建状态 → 跑 → 全释放），stdout 走 `out` 帧。
 * 顺序与 HTTP 的流式分支一致：output_begin → guard_exec_begin → exec → output_end。 */
void run_script_frame(const server_config &cfg, const void *src, size_t len)
{
    using namespace autodbg::host;

    exec_request er;
    er.src       = (const char *)src;
    er.len       = len;
    er.chunkname = "@buffer.lua";   /* T3 的握手 json 里带 filename 后再用它 */
    er.args      = nullptr;
    er.argc      = 0;

    output_begin(0, cfg.echo);   /* 输出进收集器；`stream_active()` 时同时发 `out` 帧 */
    guard_exec_begin(cfg.timeout_sec);

    exec_result r = exec_script(er);

    output_snapshot snap = output_end();
    int             why  = guard_stop_reason();
    guard_exec_end();

    autodbg::common::json_writer w;
    w.begin_object();
    w.key("ok");
    w.boolean(r.code == 0);
    w.key("exit");
    w.number((long long)r.code);
    w.key("loaded");
    w.boolean(r.loaded);
    w.key("elapsed_ms");
    w.number((long long)r.elapsed_ms);
    w.key("truncated");
    w.boolean(snap.out_truncated || snap.err_truncated);
    w.key("canceled");
    w.boolean(why == guard_cancel);
    w.key("timed_out");
    w.boolean(why == guard_timeout);
    w.key("error");
    if (r.error[0] != '\0')
        w.string(r.error);
    else
        w.string("");
    w.end_object();
    send_json_frame(stream_frame_result, 0, 0, w);

    /* `end` 帧替代 chunked 的终止块（D42：帧自带长度，不需要额外终止符） */
    stream_write_frame(stream_frame_end, 0, 0, nullptr, 0, nullptr, 0);
}

/* 处理一条连接：握手 → 帧循环。返回 false = 主机要求停机。 */
bool serve_connection(const autodbg_boot *boot, const server_config &cfg, long long sock)
{
    using namespace autodbg::host;

    g_conn = sock;
    stream_transport tp;
    tp.send = conn_send;
    tp.recv = conn_recv;
    tp.ctx  = nullptr;

    bool keep_host = true;
    if (!stream_begin_ex(tp, stream_framing_raw, (unsigned)k_send_timeout_ms))
    {
        stream_end();
        g_conn = -1;
        return true;
    }

    /* 帧缓冲：一帧的 json + data。上限取配置的 max_body（同一份数值即 max_frame）。 */
    size_t        cap = cfg.max_body;
    unsigned char *buf = (unsigned char *)autodbg::common::alloc(cap);
    unsigned char  hdr[autodbg::common::k_frame_header_size];
    if (buf == nullptr)
    {
        fail_connection("oom");
        stream_end();
        g_conn = -1;
        return true;
    }

    /* ---- ① 握手（第一帧必须是 handshake；不认「先发别的」） ---- */
    bool handshaken = false;
    for (;;)
    {
        size_t json_len = 0;
        long   n        = stream_read_frame(hdr, buf, cap, &json_len, cfg.max_body);
        if (n == -2)
        {
            if (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0)
                keep_host = false;
            if (!keep_host)
                break;
            continue;   /* 空闲超时：继续等（对端可能还在打算发话） */
        }
        if (n <= 0)
        {
            fail_connection(n == 0 ? "closed" : (n == -3 ? "bad-frame" : "io-error"));
            break;
        }

        unsigned type = hdr[0];
        unsigned ver  = (unsigned)hdr[2] | ((unsigned)hdr[3] << 8);   /* 握手帧的 id = 版本 */

        if (type != stream_frame_handshake)
        {
            fail_connection("handshake-required");
            break;
        }
        if (ver != (unsigned)k_stream_proto_version)
        {
            autodbg::common::json_writer w;
            w.begin_object();
            w.key("error");
            w.string("bad-version");
            w.key("want");
            w.number((long long)k_stream_proto_version);
            w.key("got");
            w.number((long long)ver);
            w.end_object();
            send_json_frame(stream_frame_error, 0, 0, w);
            break;
        }
        if (cfg.auth_required)
        {
            /* token 在 `data` 段：`data_len` 由帧头给，内容紧跟在 json 之后 */
            const char *tok = (const char *)(buf + json_len);
            size_t      tl  = (size_t)((unsigned)hdr[4] | ((unsigned)hdr[5] << 8) | ((unsigned)hdr[6] << 16) |
                                      ((unsigned)hdr[7] << 24));
            if (!token_matches(cfg, tok, tl))
            {
                fail_connection("unauthorized");
                break;
            }
        }

        handshaken = true;
        send_info(boot, cfg, /*with_version=*/true);   /* 握手的同步应答：客户端可立即断言 */
        break;
    }

    /* ---- ② 帧循环（script / control / end） ---- */
    while (handshaken && keep_host)
    {
        size_t json_len = 0;
        long   n        = stream_read_frame(hdr, buf, cap, &json_len, cfg.max_body);
        if (n == -2)
        {
            if (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0)
                keep_host = false;
            continue;
        }
        if (n <= 0)
            break;   /* 对端关闭 / 帧坏 / 传输错：直接断开（没有可回的 json 时不必回帧） */

        unsigned type  = hdr[0];
        unsigned flags = hdr[1];

        if (type == stream_frame_script)
        {
            /* json 段可能带 args/filename（T3 接）；本篇只吃 data 段 = 源码 */
            run_script_frame(cfg, buf + json_len, (size_t)(n - (long)common::k_frame_header_size - (long)json_len));
            continue;
        }
        if (type == stream_frame_control)
        {
            switch (flags)
            {
            case stream_control_health:
                send_info(boot, cfg, /*with_version=*/false);
                break;
            case stream_control_version:
                send_info(boot, cfg, /*with_version=*/true);
                break;
            case stream_control_cancel:
            {
                bool active = guard_exec_active();
                if (active)
                    guard_request_cancel();
                send_ack(flags, active, active ? "cancel-requested" : "no-execution", 0);
                break;
            }
            case stream_control_kill:
                /* T4：接到 on_wait_timeout（D46 的软动作）；现在如实回「做不到」 */
                send_ack(flags, false, "not-implemented", 0);
                break;
            case stream_control_detach:
                send_ack(flags, false, "not-implemented", 0);
                break;
            case stream_control_shutdown:
                send_ack(flags, true, "shutdown", 0);
                keep_host = false;
                InterlockedExchange(&g_shutdown, 1);
                break;
            default:
                send_ack(flags, false, "unknown-cmd", 0);
                break;
            }
            continue;
        }
        if (type == stream_frame_end)
        {
            autodbg::host::stream_write_frame(autodbg::host::stream_frame_end, 0, 0, nullptr, 0, nullptr, 0);
            break;
        }
        fail_connection("unknown-frame");
        break;
    }

    autodbg::host::stream_finish(); /* chunked 分帧下不发；raw 下是 no-op */
    autodbg::host::stream_end();
    autodbg::common::release(buf);
    g_conn = -1;
    return keep_host;
}

} /* namespace */

int tcp_server_main(const autodbg_boot *boot)
{
    using autodbg::core::ErrorS;
    using autodbg::core::PrintF;
    using autodbg::core::PrintS;

    server_config cfg;
    char          err[256];
    server_config_status st = server_config_from_boot(boot, &cfg, err, sizeof err);
    if (st != server_config_ok)
    {
        ErrorS("server: 配置错误 ");
        ErrorS(server_config_status_name(st));
        ErrorS(": ");
        ErrorS(err);
        ErrorS("\n");
        return 2;
    }
    if (cfg.max_body < (unsigned)autodbg::common::k_frame_header_size + 64u)
        cfg.max_body = (unsigned)autodbg::common::k_frame_max_payload;   /* 别让配置把帧卡死 */

    autodbg::common::net_status why = autodbg::common::net_ok;
    if (autodbg::common::net_startup() != autodbg::common::net_ok)
    {
        ErrorS("server(tcp): 本系统没有可用的 winsock（ws2_32.dll / wsock32.dll 都载不进）\n");
        return 2;
    }

    unsigned  actual_port = 0;
    long long listener    = autodbg::common::net_listen(cfg.host, cfg.port, &actual_port, &why);
    if (listener < 0)
    {
        ErrorS("server(tcp): 监听失败 ");
        ErrorS(autodbg::common::net_status_name(why));
        ErrorS("（");
        ErrorS(cfg.host);
        ErrorS(":");
        PrintF("%u", cfg.port);
        ErrorS("）\n");
        autodbg::common::net_cleanup();
        return 2;
    }

    PrintS("autodbg server(tcp): listen=");
    PrintS(cfg.host);
    PrintF(":%u proto=%u", actual_port, (unsigned)autodbg::host::k_stream_proto_version);
    if (cfg.auth_required)
    {
        PrintS(" (auth: token ");
        PrintS(cfg.token);
        PrintS(")\n");
    }
    else
    {
        PrintS(" (auth: none, loopback only)\n");
    }
    PrintF("autodbg server(tcp): max_frame=%u timeout=%us echo=%s winsock=%s\n", cfg.max_body, cfg.timeout_sec,
           cfg.echo ? "on" : "off",
           (autodbg::common::net_module_name() != nullptr) ? autodbg::common::net_module_name() : "(none)");
    PrintS("autodbg server(tcp): ready（Ctrl+C 或 control/shutdown 退出）\n");

    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    bool keep_host = true;
    while (keep_host && InterlockedCompareExchange(&g_shutdown, 0, 0) == 0)
    {
        unsigned  peer = 0;
        long long c    = autodbg::common::net_accept(listener, k_accept_timeout_ms, &peer);
        if (c == 0)
            continue;   /* 超时：回到循环顶部看停机标志 */
        if (c < 0)
            continue;   /* 单个 accept 失败不该让 server 停掉 */

        /* D43：一次只服务一条连接（严格串行）；处理完立刻关。 */
        keep_host = serve_connection(boot, cfg, c);
        autodbg::common::net_close(c);
    }

    SetConsoleCtrlHandler(on_console_ctrl, FALSE);
    autodbg::common::net_close(listener);
    autodbg::common::net_cleanup();
    PrintS("autodbg server(tcp): stopped\n");
    return 0;
}
} /* namespace autodbg::server */
