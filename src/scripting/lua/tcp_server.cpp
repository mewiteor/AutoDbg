/* =============================================================================
 * src/scripting/lua/tcp_server.cpp —— raw TCP 帧服务端（T2 骨架 + T3 双线程）
 *
 * 来源：新写（plan_cli.md §2.1/§2.2、D42/D43）。与 `server.cpp`（HTTP）并存、互不引用。
 *
 * 线程模型（plan_cli.md §2.2 的落地）：
 *   · **accept 线程**：接连接 → 握手 → 处理 `control`（cancel/kill/detach/health/version/
 *     shutdown）与 `end`；只有收到 `script` 帧才把这条连接**交给 worker**。
 *     其它连接一律是**控制连接** ⇒ 脚本跑着的时候 `--cancel` 也能进来。
 *   · **worker 线程**：一次只跑一个脚本（D43：无状态、并发 1）。它用 `stream.*` 的
 *     单例状态往**执行连接**写 `out` / `result` / `end`（`print`、`f:write` 都靠它）。
 *   · 握手帧：`id` = 协议版本、`data` = token、`json` = 其余字段（留给执行侧解）。
 *   · 执行期的 control：worker 在执行栈里（`on_wait_timeout` / `lua_sethook` →
 *     `stream_poll_and_dispatch`）收**执行连接**上的控制帧；控制连接上的命令则由
 *     accept 线程直接落到 guard 上（同一个执行）。
 *
 * D28：C 侧**不解析 JSON** —— 控制命令放 `flags`、版本放 `id`、退出码放 `result` 帧的 `id`、
 *   状态位放 `flags`（见 `stream.hpp`）。
 *
 * T7 切片 D2a（proto 2）：`script` 帧的 `id` 是**命令号** —— accept 线程把它随作业交给 worker，
 *   `run_one_script` 在跑这一段前 `stream_set_command_id(id)`、跑完清 0；`out` / `event` 帧
 *   据此归因。`result` 帧的 `id` 仍是退出码（两套含义不混）。
 *
 * T7 切片 D2b：`--repl --server`（`boot->repl_mode != 0`）在这里接上 —— 一条连信用一份
 *   **不重建**的 `lua_State` + 引擎会话（`g_repl`，由 `worker_serve` 建/毁），每帧 `script`
 *   帧 = REPL 的一行（或一个完整块）；`result` 帧的 `id` = **这一行的 Lua 错误码**。
 *   ❗非 `--repl` 时 `g_repl == nullptr` ⇒ `exec_script` 的一次性语义（D43）一个字节不改。
 * ========================================================================== */
#include "tcp_server.hpp"

#include "file_io.hpp"    /* T4（D44）：set_client_io —— open_file 指向客户端那台机器 */
#include "exec.hpp"        /* exec_script：一次性执行（D22/D43） */
#include "exec_guard.hpp"  /* guard_exec_begin / request_cancel / soft action */
#include "output.hpp"      /* output_begin / output_end：stdout 收进缓冲并发 `out` 帧 */
#include "repl.hpp"        /* T7 D2b：`--repl --server` 的连接级 state（repl_server_*） */
#include "server_config.hpp" /* S0 解析 / D21 策略（T5：从 HTTP 的 server.cpp 抽出来） */
#include "stream.hpp"      /* 帧协议（类型/版本/传输钩子/poll） */

#include "common/allocator.hpp"
#include "common/frame.hpp"
#include "common/json_writer.hpp"
#include "common/net.hpp"

#include "core/util.hpp" /* PrintS / PrintF / ErrorS */
#include "../../sys/version.h" /* AUTODBG_VERSION：构建期由 make.bat 生成（git describe） */

#include <windows.h>

#include <string.h>   /* strlen（`value` 片段的长度） */

namespace autodbg::server
{
namespace
{
constexpr const char *k_server_version    = AUTODBG_VERSION;
constexpr const char *k_server_ver_source = AUTODBG_VERSION_SOURCE;

constexpr int    k_accept_timeout_ms = 250;    /* 让 accept 循环有机会看 g_shutdown */
constexpr int    k_send_timeout_ms   = 10000;
constexpr int    k_handshake_timeout = 10000;  /* 握手帧最多等这么久 */
constexpr int    k_conn_idle_ms      = 2000;   /* 控制连接空闲这么久就关（不堵 accept） */
constexpr size_t k_script_max        = 64u * 1024u * 1024u;

/* result 帧 flags（与 client.cpp 对应） */
enum
{
    k_result_ok        = 0x01,
    k_result_canceled  = 0x02,
    k_result_timed_out = 0x04,
    k_result_truncated = 0x08,
};

volatile LONG g_shutdown = 0;

BOOL WINAPI on_console_ctrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_shutdown, 1);
    return TRUE;
}

/* ------------------------------------------------------------------ 进程级上下文
 * worker 是单线程、执行严格串行（D43），所以这三份是模块级的，不需要锁/拷贝。 */
const autodbg_boot   *g_boot      = nullptr;
const server_config  *g_cfg       = nullptr;
long long             g_exec_sock = -1;   /* 当前执行连接（stream.* 的 send/recv 用它） */

/* `--repl --server`（T7 D2b）的连接级上下文：`lua_State` + 引擎会话在**整条连接**的生命周期内
 * 不重建，所以它们不能由一次性的 `exec_script` 持有。`nullptr` = 非 `--repl`（每次 `script`
 * 帧建/毁 state，D43 的无状态语义）。生命周期全在 `worker_serve`（建 → 每帧求值 → 毁）。 */
autodbg::host::repl_server *g_repl = nullptr;

/* ------------------------------------------------------------------ 帧 I/O（按 socket）
 * accept 线程与 worker 都可能写帧，但**目标不同**：控制连接 / 执行连接。
 * `stream.*` 的单例状态只表示「执行连接」，所以控制连接必须走这里。 */
bool conn_send_all(long long sock, const void *p, size_t n)
{
    if (n == 0)
        return true;
    return autodbg::common::net_send_all(sock, p, n, k_send_timeout_ms);
}

bool conn_write_frame(long long sock, unsigned type, unsigned id, unsigned flags, const char *json, size_t json_len,
                      const void *data, size_t data_len)
{
    autodbg::common::frame_header h;
    h.type     = type;
    h.flags    = flags;
    h.id       = id;
    h.data_len = (unsigned)data_len;

    unsigned char hdr[autodbg::common::k_frame_header_size];
    if (!autodbg::common::frame_header_encode(hdr, sizeof hdr, h))
        return false;

    size_t        payload = sizeof hdr + json_len + data_len;
    unsigned char pfx[4];
    if (!autodbg::common::frame_prefix_encode(pfx, sizeof pfx, payload))
        return false;

    return conn_send_all(sock, pfx, sizeof pfx) && conn_send_all(sock, hdr, sizeof hdr) &&
           conn_send_all(sock, json, json_len) && conn_send_all(sock, data, data_len);
}

/* 读一帧：>0 = payload 总长（`hdr`/`buf` 已填，`*json_len` 给出 json 段长度）；
 * 0 = 对端关闭；-1 = 错误；-2 = 超时（一个字节都没读到）；-3 = 帧非法。 */
int conn_read_frame(long long sock, unsigned char *hdr, unsigned char *buf, size_t cap, size_t *json_len,
                    unsigned max_frame, int timeout_ms)
{
    unsigned char pfx[4];
    size_t        got = 0;
    while (got < sizeof pfx)
    {
        int r = autodbg::common::net_recv(sock, pfx + got, sizeof pfx - got, timeout_ms);
        if (r > 0)
        {
            got += (size_t)r;
            continue;
        }
        if (r == 0)
            return 0;
        if (r == -2)
            return (got == 0) ? -2 : -1;
        return -1;
    }

    size_t payload = 0;
    if (!autodbg::common::frame_prefix_decode(pfx, sizeof pfx, &payload) || payload > (size_t)max_frame)
        return -3;

    size_t need = (size_t)autodbg::common::k_frame_header_size;
    size_t hgot = 0;
    while (hgot < need)
    {
        int r = autodbg::common::net_recv(sock, hdr + hgot, need - hgot, timeout_ms);
        if (r <= 0)
            return -1;
        hgot += (size_t)r;
    }

    autodbg::common::frame_header fh;
    if (!autodbg::common::frame_header_decode(hdr, need, &fh))
        return -3;

    size_t jl = 0;
    if (!autodbg::common::frame_json_len(payload, fh.data_len, &jl))
        return -3;
    size_t body = jl + (size_t)fh.data_len;
    if (body > cap)
        return -3;

    size_t bgot = 0;
    while (bgot < body)
    {
        int r = autodbg::common::net_recv(sock, buf + bgot, body - bgot, timeout_ms);
        if (r <= 0)
            return -1;
        bgot += (size_t)r;
    }
    if (json_len != nullptr)
        *json_len = jl;
    return (int)payload;
}

/* ------------------------------------------------------------------ 名字 / 工具 */
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

/* 常量时间比较（长度也要一致）：token 不是秘密字符串的长度也不泄露 */
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

/* ------------------------------------------------------------------ 按 socket 的应答帧 */
void fail_conn(long long sock, const char *code)
{
    autodbg::common::json_writer w;
    w.begin_object();
    w.key("error");
    w.string(code);
    w.end_object();
    if (w.data() != nullptr)
        conn_write_frame(sock, autodbg::host::stream_frame_error, 0, 0, w.data(), w.size(), nullptr, 0);
}

void send_ack_to(long long sock, unsigned cmd, bool applied, const char *reason, unsigned long pid)
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
    if (w.data() == nullptr)
        return;
    /* flags bit0 = applied：客户端不解 JSON 就能判成败（D28） */
    conn_write_frame(sock, autodbg::host::stream_frame_ack, 0, applied ? 0x01u : 0u, w.data(), w.size(), nullptr, 0);
}

void send_info_to(long long sock, const autodbg_boot *boot, const server_config &cfg, bool with_version)
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
    if (w.data() != nullptr)
        conn_write_frame(sock, autodbg::host::stream_frame_info, 0, 0, w.data(), w.size(), nullptr, 0);
}

/* ------------------------------------------------------------------ 控制命令
 * `sock >= 0`：控制连接（accept 线程）—— 应答写回这条连接。
 * `sock < 0` ：执行连接（worker 执行栈里的 poll）—— 应答走 `stream.*`。
 * ❗两种情形都只能设状态 + 发帧：不得阻塞、不得重入执行。 */
void control_dispatch(long long sock, unsigned cmd, bool *want_shutdown)
{
    using namespace autodbg::host;
    if (want_shutdown != nullptr)
        *want_shutdown = false;

    switch (cmd)
    {
    case stream_control_health:
    case stream_control_version:
        if (sock >= 0)
            send_info_to(sock, g_boot, *g_cfg, cmd == stream_control_version);
        else
        {
            autodbg::common::json_writer w;
            w.begin_object();
            w.key("ok");
            w.boolean(true);
            w.key("busy");
            w.boolean(true);
            w.key("proto");
            w.number((long long)k_stream_proto_version);
            w.key("version");
            w.string(k_server_version);
            w.end_object();
            if (w.data() != nullptr)
                stream_write_frame(stream_frame_info, 0, 0, w.data(), w.size(), nullptr, 0);
        }
        break;
    case stream_control_cancel:
    {
        bool active = guard_exec_active();
        if (active)
            guard_request_cancel();
        if (sock >= 0)
            send_ack_to(sock, cmd, active, active ? "cancel-requested" : "no-execution", 0);
        else
        {
            autodbg::common::json_writer w;
            w.begin_object();
            w.key("cmd");
            w.number((long long)cmd);
            w.key("applied");
            w.boolean(active);
            w.key("reason");
            w.string(active ? "cancel-requested" : "no-execution");
            w.end_object();
            if (w.data() != nullptr)
                stream_write_frame(stream_frame_ack, 0, active ? 0x01u : 0u, w.data(), w.size(), nullptr, 0);
        }
        break;
    }
    case stream_control_kill:
    case stream_control_detach:
    {
        bool active = guard_exec_active();
        if (active)
            guard_request_soft((cmd == stream_control_kill) ? guard_soft_kill : guard_soft_detach);
        if (sock >= 0)
            send_ack_to(sock, cmd, active, active ? "requested" : "no-execution", 0);
        else
        {
            autodbg::common::json_writer w;
            w.begin_object();
            w.key("cmd");
            w.number((long long)cmd);
            w.key("applied");
            w.boolean(active);
            w.key("reason");
            w.string(active ? "requested" : "no-execution");
            w.end_object();
            if (w.data() != nullptr)
                stream_write_frame(stream_frame_ack, 0, active ? 0x01u : 0u, w.data(), w.size(), nullptr, 0);
        }
        break;
    }
    case stream_control_shutdown:
        InterlockedExchange(&g_shutdown, 1);
        if (want_shutdown != nullptr)
            *want_shutdown = true;
        if (sock >= 0)
            send_ack_to(sock, cmd, true, "shutdown", 0);
        else
        {
            autodbg::common::json_writer w;
            w.begin_object();
            w.key("cmd");
            w.number((long long)cmd);
            w.key("applied");
            w.boolean(true);
            w.key("reason");
            w.string("shutdown");
            w.end_object();
            if (w.data() != nullptr)
                stream_write_frame(stream_frame_ack, 0, 0x01u, w.data(), w.size(), nullptr, 0);
        }
        break;
    default:
        if (sock >= 0)
            send_ack_to(sock, cmd, false, "unknown-cmd", 0);
        else
        {
            autodbg::common::json_writer w;
            w.begin_object();
            w.key("cmd");
            w.number((long long)cmd);
            w.key("applied");
            w.boolean(false);
            w.key("reason");
            w.string("unknown-cmd");
            w.end_object();
            if (w.data() != nullptr)
                stream_write_frame(stream_frame_ack, 0, 0, w.data(), w.size(), nullptr, 0);
        }
        break;
    }
}

/* ------------------------------------------------------------------ worker（执行连接） */
struct job
{
    long long      sock;
    unsigned char *src;      /* 拥有：worker 执行完 release */
    size_t         src_len;
    unsigned       cmd_id;   /* `script` 帧的 `id` = 命令号（T7 D2a：`out` / `event` 帧的归因） */
    char           chunk[128]; /* script 帧的 json 段 = chunkname（原文，不是 JSON；D28） */
    bool           used;
};

CRITICAL_SECTION g_q_lock;
bool             g_q_lock_init = false;
HANDLE           g_q_event     = nullptr;
job              g_job         = {-1, nullptr, 0, 0, {0}, false};
volatile LONG    g_busy        = 0;

bool enqueue(long long sock, unsigned char *src, size_t src_len, unsigned cmd_id, const char *chunk)
{
    bool ok = false;
    EnterCriticalSection(&g_q_lock);
    if (!g_job.used)
    {
        g_job.sock    = sock;
        g_job.src     = src;
        g_job.src_len = src_len;
        g_job.cmd_id  = cmd_id;
        for (int k = 0; k < (int)sizeof g_job.chunk; k++)
            g_job.chunk[k] = (k < 127 && chunk != nullptr) ? chunk[k] : '\0';
        g_job.chunk[sizeof g_job.chunk - 1] = '\0';
        g_job.used    = true;
        ok            = true;
    }
    LeaveCriticalSection(&g_q_lock);
    if (ok)
        SetEvent(g_q_event);
    return ok;
}

bool dequeue(job *out)
{
    bool ok = false;
    EnterCriticalSection(&g_q_lock);
    if (g_job.used)
    {
        *out       = g_job;
        g_job.used = false;
        ok         = true;
    }
    LeaveCriticalSection(&g_q_lock);
    return ok;
}

/* 执行连接上的 stream 传输（单例）：send/recv 都作用于 g_exec_sock */
bool exec_send(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    if (g_exec_sock < 0)
        return false;
    return autodbg::common::net_send_all(g_exec_sock, data, len, k_send_timeout_ms);
}

int exec_recv(void *ctx, void *buf, size_t cap)
{
    (void)ctx;
    if (g_exec_sock < 0)
        return -1;
    return autodbg::common::net_recv(g_exec_sock, buf, cap, k_send_timeout_ms);
}

int exec_recv_nowait(void *ctx, void *buf, size_t cap)
{
    (void)ctx;
    if (g_exec_sock < 0)
        return -1;
    return autodbg::common::net_recv(g_exec_sock, buf, cap, 0);
}

/* 执行期 poll 的处理器（worker 线程）：控制帧来自**执行连接** → 应答也回执行连接 */
void on_stream_control(void *ctx, unsigned cmd, unsigned flags)
{
    (void)ctx;
    (void)flags;
    control_dispatch(-1, cmd, nullptr);
}

/* 跑一段脚本（流已开）：out 由 output 层直发，末尾补 result + end 帧。
 * ❗`end` 只结束**这一段**脚本，连接继续可用（一个连接可以发多个 script 帧）。
 * ❗T7 D2a：`cmd_id` = 这个 `script` 帧的 `id`（命令号）。它在本段执行期间是 `stream_command_id()`
 *   的值（`out` / `event` 帧据此归因），帧发完就清 0 —— 空闲期任何输出都是无归属的。
 *   `result` 帧的 `id` **仍是退出码**（u16 二补数），与命令号无关。
 * ❗T7 D2b：`g_repl` 非空（`--repl --server`）时求值发生在**连接级**的 state 上，本段只是
 *   该 state 上的一条命令；`result.id` 因而 = **这一行的 Lua 错误码**（0 / 2 / 3），
 *   `exec_script`（建/毁 state）在这个分支里根本不跑。
 * ❗T7 C-remote（D56，协议**不** bump）：`result` 帧的 json 多一个键 `value` = 顶层 chunk 的返回值，
 *   口径与 `--json` 信封的 `result` 完全一致（`state.hpp` 的 `encode_returns`，同一个实现）；
 *   没有返回值 / 失败 ⇒ `null`。多一个键是加法，既有断言与 `--json` 信封不变；远端会话代理
 *   （`dbglib.remote`）就靠它把方法返回值取回来（帧的 `id` 仍只装退出码，两套含义不混）。 */
void run_one_script(const unsigned char *src, size_t src_len, const char *chunkname, unsigned cmd_id,
                    const server_config &cfg)
{
    using namespace autodbg::host;

    stream_set_command_id(cmd_id);

    output_begin(0, cfg.echo);
    guard_exec_begin(cfg.timeout_sec);

    exec_result r;

    if (g_repl != nullptr)
    {
        /* `--repl --server`（T7 D2b）：一帧 = REPL 的一行 / 一个完整块。
         * `chunkname` 在这里没有意义 —— 错误位置固定报 `[string "=repl"]`（与本地 REPL 同口径）。 */
        r = repl_server_chunk(g_repl, (const char *)src, src_len);
    }
    else
    {
        exec_request er;
        er.src  = (const char *)src;
        er.len  = src_len;
        /* chunkname：脚本帧的 json 段（客户端给的文件名）。`@` 前缀让 Lua 按「文件」报位置
         * （`你的文件:12: ...`）——不加的话错误信息会变成 `[string "x.lua"]:12`，插件就没法
         * 把错误行塞进 quickfix 了。 */
        char chunkbuf[132];
        if (chunkname != nullptr && chunkname[0] != '\0')
        {
            chunkbuf[0] = '@';
            size_t ci = 0;
            while (chunkname[ci] != '\0' && ci + 2 < sizeof chunkbuf)
            {
                chunkbuf[ci + 1] = chunkname[ci];
                ci++;
            }
            chunkbuf[ci + 1] = '\0';
        }
        else
        {
            const char *dflt = "@buffer.lua";
            size_t      ci   = 0;
            while (dflt[ci] != '\0' && ci + 1 < sizeof chunkbuf)
            {
                chunkbuf[ci] = dflt[ci];
                ci++;
            }
            chunkbuf[ci] = '\0';
        }
        er.chunkname = chunkbuf;
        er.args      = nullptr;
        er.argc      = 0;

        r = exec_script(er);
    }

    output_snapshot snap = output_end();
    int             why  = guard_stop_reason();
    guard_exec_end();

    unsigned flags = 0;
    if (r.code == 0)
        flags |= (unsigned)k_result_ok;
    if (why == guard_cancel)
        flags |= (unsigned)k_result_canceled;
    if (why == guard_timeout)
        flags |= (unsigned)k_result_timed_out;
    if (snap.out_truncated || snap.err_truncated)
        flags |= (unsigned)k_result_truncated;

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
    w.boolean((flags & (unsigned)k_result_truncated) != 0);
    w.key("canceled");
    w.boolean((flags & (unsigned)k_result_canceled) != 0);
    w.key("timed_out");
    w.boolean((flags & (unsigned)k_result_timed_out) != 0);
    w.key("error");
    w.string(r.error);
    w.key("value");
    if (r.value_json != nullptr)
    {
        /* `jsonx.encode` 的片段：原样嵌入，只把非 ASCII 转义（与 `--json` 信封的 `result` 同口径，
         * 见 `main.cpp` 的 `w.raw_escaped(s_json_result, …)`）。 */
        w.raw_escaped(r.value_json, strlen(r.value_json));
    }
    else
    {
        w.null_value();   /* 没有返回值 / 失败：字段不省（与 `--json` 的 `result` 一致） */
    }
    w.end_object();
    if (w.data() != nullptr)
        stream_write_frame(stream_frame_result, (unsigned)(unsigned short)(int)r.code, flags, w.data(), w.size(), nullptr, 0);
    stream_write_frame(stream_frame_end, 0, 0, nullptr, 0, nullptr, 0);
    stream_set_command_id(0);   /* 本段命令结束：之后的输出 / 事件都是无归属的（id = 0） */

    /* `value` 片段是本段自己 alloc 的（`encode_returns` 的约定），发完就释放 */
    if (r.value_json != nullptr)
        autodbg::common::release((void *)r.value_json);
}

/* worker 线程服务一条连接（即 plan_cli.md §2.2 的「执行连接」）：
 * 先跑首帧（accept 线程已经读过的那一段），然后继续读同一连接的帧 ——
 * script → 执行，control → 分发，end → 收尾。空闲超时就把连接关掉。
 * ❗这一段在 worker 线程上，`stream.*` 的单例状态就指向这条连接。 */
void worker_serve(const job &j, const autodbg_boot *boot, const server_config &cfg)
{
    using namespace autodbg::host;

    g_exec_sock = j.sock;

    stream_transport tp;
    tp.send        = exec_send;
    tp.recv        = exec_recv;         /* 整帧读（未用，留给将来） */
    tp.recv_nowait = exec_recv_nowait;  /* D32：执行期两个 poll 点 */
    tp.ctx         = nullptr;

    if (!stream_begin_ex(tp, stream_framing_raw, (unsigned)k_send_timeout_ms))
    {
        g_exec_sock = -1;
        return;
    }
    stream_set_control_handler(on_stream_control, nullptr);

    /* T7 D2b：`--repl` 下这条连接的 `lua_State` + 引擎会话**只建一次**（跨帧存活）；
     * 非 `--repl` 时 `g_repl` 保持 nullptr，下面走 `exec_script` 的一次性语义（D43）。 */
    bool repl = (boot->repl_mode != 0);
    if (repl)
    {
        g_repl = repl_server_begin();
        if (g_repl == nullptr)
        {
            fail_conn(j.sock, "oom");
            stream_set_control_handler(nullptr, nullptr);
            stream_end();
            g_exec_sock = -1;
            return;
        }
    }

    /* 首帧（accept 线程已经确认它是 script） */
    run_one_script(j.src, j.src_len, (j.chunk[0] != '\0') ? j.chunk : nullptr, j.cmd_id, cfg);

    /* 后续帧 */
    size_t         cap = (size_t)cfg.max_body + (size_t)autodbg::common::k_frame_header_size;
    unsigned char *buf = (unsigned char *)autodbg::common::alloc(cap);
    unsigned char  hdr[autodbg::common::k_frame_header_size];
    if (buf != nullptr)
    {
        for (;;)
        {
            size_t json_len = 0;
            int    n        = conn_read_frame(j.sock, hdr, buf, cap, &json_len, cfg.max_body, k_conn_idle_ms);
            if (n == -2 || n <= 0)
                break;   /* 空闲 / 对端关闭 / 帧坏：关连接 */

            unsigned type = hdr[0];
            if (type == stream_frame_script)
            {
                size_t dl = (size_t)n - (size_t)autodbg::common::k_frame_header_size - json_len;
                char   chunk[128];
                size_t cn = (json_len < sizeof chunk - 1) ? json_len : sizeof chunk - 1;
                for (size_t i = 0; i < cn; i++)
                    chunk[i] = (char)buf[i];
                chunk[cn] = '\0';
                run_one_script(buf + json_len, dl, chunk, (unsigned)hdr[2] | ((unsigned)hdr[3] << 8), cfg);
                continue;
            }
            if (type == stream_frame_control)
            {
                bool want_shutdown = false;
                control_dispatch(j.sock, hdr[1], &want_shutdown);
                if (want_shutdown)
                    break;
                continue;
            }
            if (type == stream_frame_end)
            {
                conn_write_frame(j.sock, stream_frame_end, 0, 0, nullptr, 0, nullptr, 0);
                break;
            }
            fail_conn(j.sock, "unknown-frame");
            break;
        }
        autodbg::common::release(buf);
    }

    /* T7 D2b：连接结束（对端关闭 / 空闲 / shutdown）→ 安全网（会话报告）→ `lua_close` → 释放会话。
     * 这一步必须在 `stream_end()` 之前：报告走 stderr，与帧无关，但 `stream_*` 单例还指着这条连接。 */
    if (g_repl != nullptr)
    {
        repl_server_end(g_repl);
        g_repl = nullptr;
    }

    stream_set_control_handler(nullptr, nullptr);
    stream_end();
    g_exec_sock = -1;
    (void)boot;
}

DWORD WINAPI worker_proc(void *param)
{
    (void)param;
    for (;;)
    {
        if (WaitForSingleObject(g_q_event, 250) != WAIT_OBJECT_0)
        {
            if (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0)
                return 0;
            continue;
        }
        job j;
        if (!dequeue(&j))
            continue;

        InterlockedExchange(&g_busy, 1);
        worker_serve(j, g_boot, *g_cfg);
        InterlockedExchange(&g_busy, 0);

        if (j.src != nullptr)
            autodbg::common::release(j.src);
        autodbg::common::net_close(j.sock);

        if (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0)
            return 0;
    }
}

/* ------------------------------------------------------------------ 握手 + 连接（accept 线程） */
bool do_handshake(long long sock, unsigned char *hdr, unsigned char *buf, size_t cap, unsigned max_frame)
{
    size_t json_len = 0;
    int    n        = conn_read_frame(sock, hdr, buf, cap, &json_len, max_frame, k_handshake_timeout);
    if (n == -2)
        return false;   /* 连上来一句话不说 */
    if (n <= 0)
    {
        fail_conn(sock, n == -3 ? "bad-frame" : "io-error");
        return false;
    }
    if (hdr[0] != autodbg::host::stream_frame_handshake)
    {
        fail_conn(sock, "handshake-required");
        return false;
    }

    unsigned ver = (unsigned)hdr[2] | ((unsigned)hdr[3] << 8);   /* id = 协议版本 */
    if (ver != (unsigned)autodbg::host::k_stream_proto_version)
    {
        autodbg::common::json_writer w;
        w.begin_object();
        w.key("error");
        w.string("bad-version");
        w.key("want");
        w.number((long long)autodbg::host::k_stream_proto_version);
        w.key("got");
        w.number((long long)ver);
        w.end_object();
        if (w.data() != nullptr)
            conn_write_frame(sock, autodbg::host::stream_frame_error, 0, 0, w.data(), w.size(), nullptr, 0);
        return false;
    }

    if (g_cfg->auth_required)
    {
        size_t      data_len = (size_t)((unsigned)hdr[4] | ((unsigned)hdr[5] << 8) | ((unsigned)hdr[6] << 16) |
                              ((unsigned)hdr[7] << 24));
        const char *tok      = (const char *)(buf + json_len);
        if (!token_matches(*g_cfg, tok, data_len))
        {
            fail_conn(sock, "unauthorized");
            return false;
        }
    }

    send_info_to(sock, g_boot, *g_cfg, /*with_version=*/true);   /* 握手的同步应答 */
    return true;
}

/* 处理一条已握手的连接：控制命令就地处理；`script` 帧把所有权交给 worker。
 * 返回时**不**关闭 sock（调用方决定；交棒给 worker 时调用方也不关）。 */
enum conn_result
{
    conn_closed = 0,   /* 调用方要关闭（控制连接结束 / 出错） */
    conn_handed = 1,   /* 所有权已交给 worker，调用方**不要**关 */
};

int serve_connection(long long sock, unsigned char *hdr, unsigned char *buf, size_t cap, unsigned max_frame)
{
    for (;;)
    {
        size_t json_len = 0;
        int    n        = conn_read_frame(sock, hdr, buf, cap, &json_len, max_frame, k_conn_idle_ms);
        if (n == -2)
            return conn_closed;   /* 空闲：控制连接没事就关（不占 accept 线程） */
        if (n <= 0)
            return conn_closed;

        unsigned type = hdr[0];
        if (type == autodbg::host::stream_frame_control)
        {
            bool want_shutdown = false;
            control_dispatch(sock, hdr[1], &want_shutdown);
            if (want_shutdown)
                return conn_closed;
            continue;
        }
        if (type == autodbg::host::stream_frame_script)
        {
            size_t data_len = (size_t)n - (size_t)autodbg::common::k_frame_header_size - json_len;
            if (InterlockedCompareExchange(&g_busy, 0, 0) != 0)
            {
                fail_conn(sock, "busy");   /* 已有脚本在跑（D43：并发 1） */
                return conn_closed;
            }
            unsigned char *src = (unsigned char *)autodbg::common::alloc(data_len + 1);
            if (src == nullptr)
            {
                fail_conn(sock, "oom");
                return conn_closed;
            }
            if (data_len > 0)
                memcpy(src, buf + json_len, data_len);
            src[data_len] = '\0';

            char chunk[128];
            size_t cn = (json_len < sizeof chunk - 1) ? json_len : sizeof chunk - 1;
            for (size_t i = 0; i < cn; i++)
                chunk[i] = (char)buf[i];
            chunk[cn] = '\0';
            if (!enqueue(sock, src, data_len, (unsigned)hdr[2] | ((unsigned)hdr[3] << 8), chunk))
            {
                autodbg::common::release(src);
                fail_conn(sock, "busy");
                return conn_closed;
            }
            return conn_handed;
        }
        if (type == autodbg::host::stream_frame_end)
        {
            conn_write_frame(sock, autodbg::host::stream_frame_end, 0, 0, nullptr, 0, nullptr, 0);
            return conn_closed;
        }
        fail_conn(sock, "unknown-frame");
        return conn_closed;
    }
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
        cfg.max_body = (unsigned)autodbg::common::k_frame_max_payload;

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

    /* 绑到 0.0.0.0 时把本机网卡地址列出来：client 直接用其中一条就能连（不用猜 IP）。
     * 走 PrintS（core 的输出分流：控制台 / 重定向都能看）；取不到就安静地不列。
     * ❗T5 重写 TCP server 时漏了这一块（HTTP 那份才有）—— 现在补回来，
     *   并换成 `--remote=<ip>:<port>` 的说法。 */
    {
        unsigned any = 1;
        if (autodbg::common::net_parse_ipv4(cfg.host, &any) && any == 0)
        {
            char   addrs[1024];
            size_t n = autodbg::common::net_describe_local_addresses(addrs, sizeof addrs);
            if (n > 0)
            {
                PrintS("autodbg server(tcp): 本机网卡地址（client 用 `--remote=<地址>:端口` 里的任一条）:\n");
                PrintS(addrs);
            }
            PrintS("autodbg server(tcp): 提示: 非 loopback 监听需要 token，且防火墙可能要放行该端口\n");
        }
    }

    /* T4（D44）：server 模式下 open_file 指向**客户端**那台机器（帧协议），不是本机磁盘。 */
    autodbg::host::set_client_io(true);

    /* ---- worker 线程 + 单槽队列 ---- */
    g_boot = boot;
    g_cfg  = &cfg;
    InitializeCriticalSection(&g_q_lock);
    g_q_lock_init = true;
    g_q_event     = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (g_q_event == nullptr)
    {
        ErrorS("server(tcp): CreateEvent 失败\n");
        autodbg::common::net_close(listener);
        autodbg::common::net_cleanup();
        return 2;
    }
    HANDLE worker = CreateThread(nullptr, 0, worker_proc, nullptr, 0, nullptr);
    if (worker == nullptr)
    {
        ErrorS("server(tcp): CreateThread 失败\n");
        CloseHandle(g_q_event);
        autodbg::common::net_close(listener);
        autodbg::common::net_cleanup();
        return 2;
    }

    PrintS("autodbg server(tcp): ready（Ctrl+C / control shutdown 退出）\n");
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    /* 每连接的帧缓冲：帧头 8 字节 + json/data（上限 = cfg.max_body） */
    size_t         cap = (size_t)cfg.max_body + (size_t)autodbg::common::k_frame_header_size;
    unsigned char *buf = (unsigned char *)autodbg::common::alloc(cap);
    unsigned char  hdr[autodbg::common::k_frame_header_size];
    if (buf == nullptr)
    {
        ErrorS("server(tcp): out of memory\n");
        InterlockedExchange(&g_shutdown, 1);
        SetEvent(g_q_event);
        WaitForSingleObject(worker, 2000);
        CloseHandle(worker);
        CloseHandle(g_q_event);
        autodbg::common::net_close(listener);
        autodbg::common::net_cleanup();
        return 2;
    }

    while (InterlockedCompareExchange(&g_shutdown, 0, 0) == 0)
    {
        unsigned  peer = 0;
        long long c    = autodbg::common::net_accept(listener, k_accept_timeout_ms, &peer);
        if (c == 0)
            continue;    /* 超时：回去看停机标志 */
        if (c < 0)
            continue;    /* 单个 accept 失败不该让 server 停掉 */

        if (!do_handshake(c, hdr, buf, cap, cfg.max_body))
        {
            autodbg::common::net_close(c);
            continue;
        }

        /* 握手成功：交棒给 worker 前先把 buffer 语义说清楚 —— worker 有自己的状态，
         * 这里只把**连接**与**脚本字节**交出去（脚本已拷进独立的缓冲）。 */
        if (serve_connection(c, hdr, buf, cap, cfg.max_body) == conn_handed)
            continue;    /* 所有权在 worker，不能关 */

        autodbg::common::net_close(c);
    }

    /* 停机：等 worker 退出（正在跑的脚本会先跑完，见 control_dispatch 的 shutdown 语义） */
    SetEvent(g_q_event);
    WaitForSingleObject(worker, 5000);
    CloseHandle(worker);
    CloseHandle(g_q_event);

    autodbg::common::release(buf);
    if (g_q_lock_init)
    {
        DeleteCriticalSection(&g_q_lock);
        g_q_lock_init = false;
    }
    SetConsoleCtrlHandler(on_console_ctrl, FALSE);
    autodbg::common::net_close(listener);
    autodbg::common::net_cleanup();
    PrintS("autodbg server(tcp): stopped\n");
    return 0;
}
} /* namespace autodbg::server */
