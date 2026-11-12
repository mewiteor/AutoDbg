/* =============================================================================
 * src/scripting/lua/server.cpp —— server 模式的配置解析、传输层与执行入口
 *
 * 来源：新写（plan_server.md S0 骨架 → S1 落地传输层）。
 * 与旧实现的差异：无（新文件）。
 * S1 范围（plan_server.md §11，含两处「提前」并已回写文档）：
 *   ✅ `net`（ws2_32 → wsock32 dynapi）+ 单线程 accept → 执行 → 响应
 *   ✅ 端点 `/health` `/version` `/run` `/shutdown`（`/shutdown` 从 S2 提前，便于自动化）
 *   ✅ `Authorization: Bearer` / `X-AutoDbg-Token` 校验（定长比较）+ D21 策略
 *   ✅ `Expect: 100-continue`；`Content-Length` 体读取；请求体上限（413）
 *   ✅ 输出收集（core sink + `print` 覆写，见 output.hpp）
 *   ✅ JSON 信封 / `text` 两种响应；`X-AutoDbg-Exit`
 *   ⏳ S2：accept/worker 双线程 + 队列、`/cancel`、`--server-log`、超时看门狗
 *   ⏳ S6：chunked/SSE 流式、`/mcp`、持久会话
 * ========================================================================== */
#include "server.hpp"

#include "exec.hpp"
#include "exec_guard.hpp" /* S2：取消/超时守卫 */
#include "exec_session.hpp" /* S6：持久会话（MCP） */
#include "file_io.hpp"     /* S7：client 文件 I/O（效果协议） */
#include "output.hpp"
#include "stream.hpp"      /* 流式 /run（帧 + chunked）：open_file / f:write 的落点 */
#include "tcp_server.hpp" /* T2（D42）：`--server=tcp` 的 raw TCP 帧服务端（本文件之外） */

#include "common/http.hpp"
#include "common/json_writer.hpp"
#include "common/net.hpp"
#include "common/rand.hpp"

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

/* 本文件大量用到 common 的 HTTP 类型；在匿名命名空间开头统一引入，
 * 免得每个函数签名都要写全名（函数体内的 `using namespace` 管不到签名）。 */
using autodbg::common::http_request;
using autodbg::common::http_response;
using autodbg::common::json_writer;

constexpr int    k_accept_timeout_ms = 250;   /* 让主循环有机会检查 g_shutdown */
constexpr int    k_recv_timeout_ms   = 10000; /* 单次 recv 等多久算「对端不说话」 */
constexpr int    k_send_timeout_ms   = 10000;
constexpr size_t k_head_buf_init     = 4096;
constexpr int    k_max_query_args    = 16;
constexpr size_t k_args_arena_size   = 8192;

volatile LONG g_shutdown = 0;

BOOL WINAPI on_console_ctrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_shutdown, 1);
    return TRUE; /* 吃掉事件：先优雅退出，由主循环收尾 */
}

/* ------------------------------ 小工具 ------------------------------ */
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

const char *flavor_name(unsigned flavor)
{
    return (flavor == 0) ? "lua" : "luajit";
}

const char *variant_name(unsigned variant)
{
    return (variant == 1) ? "legacy" : "modern";
}

const char *arch_name()
{
#ifdef _WIN64
    return "x64";
#else
    return "x86";
#endif
}

/* ------------------------------ 响应 ------------------------------ */
void send_response(long long sock, autodbg::common::http_response *resp)
{
    size_t      len = 0;
    const char *raw = resp->finish(&len);
    if (raw == nullptr)
    {
        /* 内存不足拼不响应：至少给个 500 而不是静默断开（客户端会看到连接被关）。 */
        const char *fallback = "HTTP/1.1 500 Internal Server Error\r\n"
                               "Content-Type: text/plain; charset=utf-8\r\n"
                               "Content-Length: 21\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "response build failed\n";
        autodbg::common::net_send_all(sock, fallback, strlen(fallback), k_send_timeout_ms);
        return;
    }
    autodbg::common::net_send_all(sock, raw, len, k_send_timeout_ms);
}

void send_status(long long sock, int status, const char *text)
{
    using namespace autodbg::common;

    http_response resp(status, "text/plain; charset=utf-8");
    if (text != nullptr)
        resp.write(text);
    send_response(sock, &resp);
}

void send_json_status(long long sock, int status, const char *code, const char *message)
{
    using namespace autodbg::common;

    json_writer w;
    w.begin_object();
    w.key("ok");
    w.boolean(false);
    w.key("error");
    w.string(code);
    w.key("message");
    w.string(message);
    w.end_object();

    http_response resp(status, "application/json; charset=utf-8");
    if (w.data() != nullptr)
        resp.write(w.data(), w.size());
    send_response(sock, &resp);
}

/* ------------------------------ 认证（D21） ------------------------------ */
bool token_ok(const server_config &cfg, const autodbg::common::http_request &req)
{
    if (!cfg.auth_required)
        return true;
    if (!req.has_token)
        return false;

    size_t   a    = strlen(cfg.token);
    size_t   b    = strlen(req.token);
    unsigned diff = (unsigned)(a ^ b);
    for (size_t i = 0; i < a && i < b; i++)
        diff |= (unsigned)((unsigned char)cfg.token[i] ^ (unsigned char)req.token[i]);
    return diff == 0;
}

/* ------------------------------ 请求体读取 ------------------------------ */
struct conn_buffer
{
    char  *p   = nullptr;
    size_t len = 0;
    size_t cap = 0;

    bool ensure(size_t need)
    {
        if (need + 1 <= cap)
            return true;
        size_t c = (cap == 0) ? k_head_buf_init : cap;
        while (c < need + 1)
            c += c / 2;
        char *q = (char *)autodbg::common::resize(p, c);
        if (q == nullptr)
            return false;
        p   = q;
        cap = c;
        return true;
    }

    void reset()
    {
        len = 0;
        if (p != nullptr)
            p[0] = '\0';
    }

    void free_buf()
    {
        if (p != nullptr)
            autodbg::common::release(p);
        p   = nullptr;
        len = 0;
        cap = 0;
    }
};

enum class read_result
{
    ok,
    closed,
    too_large,
    unsupported,
    bad_request,
    timeout,
};

/* 读到「头块完整」（不读 body） */
read_result read_head(long long sock, conn_buffer *b, autodbg::common::http_request *req, size_t *head_len)
{
    using namespace autodbg::common;

    /* ❗首个循环前必须有真实缓冲（S1 踩过：`b->p == nullptr` 时解析器只能报 incomplete，
     * 而 len==0 且“未开始读”与“读不进去”不应混淆；先备好初始缓冲更直接）。 */
    if (!b->ensure(k_head_buf_init))
        return read_result::too_large;

    for (;;)
    {
        size_t head = 0;
        http_parse_status st = http_parse_request(b->p, b->len, req, &head);

        if (st == http_parse_status::ok)
        {
            *head_len = head;
            return read_result::ok;
        }
        if (st == http_parse_status::too_large)
            return read_result::too_large;
        if (st == http_parse_status::bad_request)
            return read_result::bad_request;
        if (st == http_parse_status::unsupported)
            return read_result::unsupported;

        /* incomplete：继续收（头块上限由解析器兜住） */
        if (b->len >= k_http_max_header_block)
            return read_result::too_large;
        if (!b->ensure(b->len + 4096))
            return read_result::too_large;

        int n = net_recv(sock, b->p + b->len, b->cap - b->len - 1, k_recv_timeout_ms);
        if (n == 0)
            return read_result::closed;
        if (n == -2)
            return read_result::timeout;
        if (n < 0)
            return read_result::closed;
        b->len += (size_t)n;
        b->p[b->len] = '\0';
    }
}

/* 继续读 `need` 字节（从当前 len 到 need） */
read_result read_more(long long sock, conn_buffer *b, size_t need)
{
    if (!b->ensure(need))
        return read_result::too_large;
    while (b->len < need)
    {
        int n = autodbg::common::net_recv(sock, b->p + b->len, need - b->len, k_recv_timeout_ms);
        if (n == 0)
            return read_result::closed;
        if (n == -2)
            return read_result::timeout;
        if (n < 0)
            return read_result::closed;
        b->len += (size_t)n;
        b->p[b->len] = '\0';
    }
    return read_result::ok;
}

/* ------------------------------ 查询串：收集全部 arg= ------------------------------ */
struct args_holder
{
    char  arena[k_args_arena_size];
    size_t used = 0;
    const char *items[k_max_query_args];
    int    count = 0;

    void reset()
    {
        used  = 0;
        count = 0;
    }

    bool push(const char *value, size_t value_len)
    {
        if (count >= k_max_query_args)
            return false;
        if (used + value_len + 1 > sizeof arena)
            return false;

        char  *dst = arena + used;
        int    ok  = autodbg::common::http_url_decode(value, value_len, dst, sizeof arena - used);
        if (ok < 0)
            return false;

        items[count++] = dst;
        used += (size_t)ok + 1;
        return true;
    }
};

/* 查询串里的 `arg=` 参数（模块是单线程执行 → 文件作用域静态对象）
 * ❗**不能用函数内静态对象**：非平凡构造会触发 MSVC 的线程安全初始化（_Init_thread_*），
 * 进而引用 `_tls_index` → 手工映射的模块里没有 TLS 目录，链接直接 LNK2001（AGENTS §11.5）。 */
args_holder g_args;

void collect_args(const char *query, args_holder *out)
{
    out->reset();
    if (query == nullptr || query[0] == '\0')
        return;

    size_t i = 0;
    while (query[i] != '\0')
    {
        size_t start = i;
        while (query[i] != '\0' && query[i] != '&')
            i++;
        size_t seg_end = i;

        size_t eq = start;
        while (eq < seg_end && query[eq] != '=')
            eq++;

        size_t      name_len = eq - start;
        const char *value    = (eq < seg_end) ? (query + eq + 1) : "";
        size_t      value_len = (eq < seg_end) ? (seg_end - eq - 1) : 0;

        if ((name_len == 3 && memcmp(query + start, "arg", 3) == 0) ||
            (name_len == 4 && memcmp(query + start, "args", 4) == 0))
        {
            out->push(value, value_len);
        }

        if (query[i] == '&')
            i++;
    }
}

/* ============================ 诊断日志（--server-log） ============================
 * 只记「谁在什么时候跑了一个多大的脚本、结果如何」：不记脚本内容（D21 的审计口径）。
 * 零 CRT：用 CreateFileA(FILE_APPEND_DATA) + WriteFile；行内数字自己拼。 */
HANDLE g_log = INVALID_HANDLE_VALUE;

void log_open(const char *path)
{
    if (path == nullptr || path[0] == '\0')
        return;
    g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE)
        g_log = INVALID_HANDLE_VALUE;
}

void log_close()
{
    if (g_log != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
}

struct log_line_buf
{
    char   s[512];
    size_t n = 0;

    void add(const char *text)
    {
        if (text == nullptr)
            return;
        while (*text != '\0' && n + 1 < sizeof s)
            s[n++] = *text++;
        s[n] = '\0';
    }

    void num(long long v)
    {
        char tmp[24];
        int  i   = 0;
        bool neg = (v < 0);
        unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
        do
        {
            tmp[i++] = (char)('0' + (int)(u % 10));
            u /= 10;
        } while (u != 0);
        if (neg && n + 1 < sizeof s)
            s[n++] = '-';
        while (i > 0 && n + 1 < sizeof s)
            s[n++] = tmp[--i];
        s[n] = '\0';
    }

    void kv(const char *key, long long v)
    {
        add(" ");
        add(key);
        add("=");
        num(v);
    }

    void flush()
    {
        flush_to(g_log);
    }

    /* ❗T1（accept 线程）的诊断行**不能**走 `core::Print*`：worker 正在执行时，
     * 输出 sink 是**全局**装着的，会把 T1 的控制台行捕获进别人的响应里（S2 在 86 上实测到过：
     * `/run` 的 stdout 里冒出 `[5] peer=… queued 0 ms`）。这里直接 WriteFile，绕开 sink。 */
    void flush_to(HANDLE h)
    {
        if (h == nullptr || h == INVALID_HANDLE_VALUE || n == 0)
            return;
        DWORD written = 0;
        WriteFile(h, s, (DWORD)n, &written, nullptr);
    }
};

/* ============================ 执行队列（S2） ============================
 * T1（net）只负责 accept/读请求/控制端点；`/run` 入队，由 T2（worker）串行执行。
 * 为什么要两个线程：`Run()` 会阻塞到调试事件或取消为止，单线程时 `/health` 与
 * `/cancel` 根本进不来（P7 在 guest 里踩过「会话不退出、host 只能等超时」）。
 * 并发度 = 1 是**设计**：`current_session()` 是进程全局，Lua 状态不跨线程。 */
constexpr int k_job_queue_max = 32;

/* ❗`http_request` 约 2 KB，且有默认成员初始化 → 数组会以「带初值数据」进 PE 的 `.data`。
 * 早期版本把 `http_request` 内嵌进 `job` 且队列长 32 → 模块凭空 +72 KB（实测）。
 * 现在 `job` 只持**指针**（请求体堆上分配，worker 释放）：队列本身 ≈ 1.5 KB。 */
struct job
{
    long long     sock       = -1;
    bool          is_mcp     = false; /* true = `/mcp` */
    bool          is_effects = false; /* true = `/effects`（S7：恢复挂起的执行） */
    unsigned      request_id = 0;
    unsigned      peer_ip    = 0;
    char         *buf        = nullptr; /* 拥有：head + body 连续缓冲（worker 释放） */
    size_t        head_len   = 0;
    size_t        body_len   = 0;
    http_request *req        = nullptr; /* 拥有：请求头解析结果（worker 释放） */
};

job            g_jobs[k_job_queue_max];
int            g_q_head = 0;
int            g_q_tail = 0;
int            g_q_count = 0;
int            g_q_limit = k_job_queue_max; /* 实际生效上限（由 --server-queue 收窄） */
CRITICAL_SECTION g_q_lock;
bool             g_q_lock_init = false;
HANDLE           g_q_event = nullptr; /* auto-reset：有 job 或要退出 */
HANDLE           g_worker = nullptr;
volatile LONG    g_worker_busy = 0;

/* 全局上下文（worker 线程用；两者在 server_main 里存活到进程退出） */
const autodbg_boot  *g_boot = nullptr;
const server_config *g_cfg  = nullptr;

int queue_count()
{
    if (!g_q_lock_init)
        return 0;
    EnterCriticalSection(&g_q_lock);
    int n = g_q_count;
    LeaveCriticalSection(&g_q_lock);
    return n;
}

bool queue_push(const job &j)
{
    bool ok = false;
    EnterCriticalSection(&g_q_lock);
    /* 上限计的是「在飞总数」= 排队数 + 正在执行的那一个；
     * 否则 `--server-queue=1` 会变成「1 个在跑 + 1 个排队」= 实际并发 2，与语义不符。 */
    int inflight = g_q_count + ((InterlockedCompareExchange(&g_worker_busy, 0, 0) != 0) ? 1 : 0);
    if (inflight < g_q_limit)
    {
        g_jobs[g_q_tail] = j;
        g_q_tail         = (g_q_tail + 1) % k_job_queue_max;
        g_q_count++;
        ok = true;
    }
    LeaveCriticalSection(&g_q_lock);
    if (ok && g_q_event != nullptr)
        SetEvent(g_q_event);
    return ok;
}

bool queue_pop(job *out)
{
    bool ok = false;
    EnterCriticalSection(&g_q_lock);
    if (g_q_count > 0)
    {
        *out     = g_jobs[g_q_head];
        g_q_head = (g_q_head + 1) % k_job_queue_max;
        g_q_count--;
        ok = true;
    }
    LeaveCriticalSection(&g_q_lock);
    return ok;
}

/* ------------------------------ S7：client 文件 I/O 的挂起响应 ------------------------------
 * 挂起时回 200 + `pending`：告诉客户端「这次执行停在这里，请替我把这些效果做掉，
 * 然后用 POST /effects?token=… 把结果送回来」。协议见 plan_server.md §9。
 * ⚠ 挂起时输出收集器已经收了尾巴，所以把「到目前为止的输出」一起带上（stdout_so_far），
 *   恢复后的响应只包含**恢复之后**的输出。 */
void make_token(char *buf, size_t cap, unsigned sid, unsigned gen)
{
    size_t n = 0;
    char   tmp[16];
    int    i = 0;
    unsigned v = sid;
    do
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (i > 0 && n + 1 < cap)
        buf[n++] = tmp[--i];
    if (n + 1 < cap)
        buf[n++] = ':';
    i = 0;
    v = gen;
    do
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (i > 0 && n + 1 < cap)
        buf[n++] = tmp[--i];
    buf[n] = '\0';
}

void send_pending(long long sock,
                  unsigned sid,
                  unsigned gen,
                  const char *op,
                  const char *effect_json,
                  size_t effect_len,
                  const host::output_snapshot &snap,
                  bool is_mcp,
                  unsigned request_id)
{
    using namespace autodbg::common;

    char token[48];
    make_token(token, sizeof token, sid, gen);

    json_writer w;
    w.begin_object();
    w.key("ok");
    w.boolean(false);
    w.key("pending");
    w.begin_object();
    w.key("token");
    w.string(token);
    w.key("session_id");
    w.number((long long)sid);
    w.key("op");
    w.string(op);
    w.key("kind");
    w.string(is_mcp ? "mcp" : "run");
    w.key("stdout_so_far");
    w.string(snap.out, snap.out_len);
    w.key("stderr_so_far");
    w.string(snap.err, snap.err_len);
    w.key("effects");
    w.begin_array();
    if (effect_json != nullptr && effect_len > 0)
        w.raw(effect_json, effect_len);
    w.end_array();
    w.key("server");
    w.begin_object();
    w.key("version");
    w.string(k_server_version);
    w.key("request_id");
    w.number((long long)request_id);
    w.end_object();
    w.end_object();
    w.end_object();

    http_response resp(200, "application/json; charset=utf-8");
    if (w.data() != nullptr)
        resp.write(w.data(), w.size());
    send_response(sock, &resp);
}

/* 把挂起中的协程栈顶的效果表读出来（并在协程栈上弹掉它——resume 时不能留着） */
bool take_pending_effect(long long sock,
                         host::exec_session *s,
                         int co_ref,
                         char *op, size_t op_cap,
                         char **effect_json, size_t *effect_len)
{
    if (effect_json != nullptr)
        *effect_json = nullptr;
    if (effect_len != nullptr)
        *effect_len = 0;
    if (op != nullptr && op_cap > 0)
        op[0] = '\0';

    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, co_ref);
    if (!lua_isthread(L, -1))
    {
        lua_pop(L, 1);
        (void)sock;
        return false;
    }
    lua_State *co  = lua_tothread(L, -1);
    int        top = lua_gettop(co);
    bool       ok  = false;
    if (top > 0)
    {
        host::effect_op(co, -1, op, op_cap);
        ok = host::effect_to_json(co, -1, 1, effect_json, effect_len);
        lua_settop(co, top - 1); /* 弹掉 yielded 值 */
    }
    lua_pop(L, 1);
    return ok;
}
/* ------------------------------ 端点 ------------------------------ */
void handle_health(long long sock, const server_config &cfg)
{
    using namespace autodbg::common;

    json_writer w;
    w.begin_object();
    w.key("ok");
    w.boolean(true);
    w.key("busy");
    w.boolean(InterlockedCompareExchange(&g_worker_busy, 0, 0) != 0);
    w.key("server");
    w.string("S2");
    w.key("version");
    w.string(k_server_version);
    w.key("queue");
    w.number((long long)queue_count());
    w.key("queue_depth");
    w.number((long long)cfg.queue_depth);
    w.key("auth");
    w.boolean(cfg.auth_required);
    w.end_object();

    http_response resp(200, "application/json; charset=utf-8");
    if (w.data() != nullptr)
        resp.write(w.data(), w.size());
    send_response(sock, &resp);
}

/* `/cancel`：只对「当前正在执行」的那次生效（S2）。
 * 由 T1 直接应答 → 长脚本阻塞在 `Run()` 里时也能取消。 */
void handle_cancel(long long sock)
{
    using namespace autodbg::common;

    bool active = host::guard_exec_active();
    if (active)
        host::guard_request_cancel();

    json_writer w;
    w.begin_object();
    w.key("ok");
    w.boolean(true);
    w.key("busy");
    w.boolean(active);
    w.key("accepted");
    w.boolean(active);
    w.end_object();

    http_response resp(200, "application/json; charset=utf-8");
    if (w.data() != nullptr)
        resp.write(w.data(), w.size());
    send_response(sock, &resp);
}

void handle_version(long long sock, const autodbg_boot *boot, const server_config &cfg)
{
    using namespace autodbg::common;

    json_writer w;
    w.begin_object();
    w.key("ok");
    w.boolean(true);
    w.key("server");
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
    w.string(net_module_name() != nullptr ? net_module_name() : "(none)");
    w.key("auth");
    w.boolean(cfg.auth_required);
    w.end_object();

    http_response resp(200, "application/json; charset=utf-8");
    if (w.data() != nullptr)
        resp.write(w.data(), w.size());
    send_response(sock, &resp);
}

void handle_run(long long sock,
                const autodbg_boot *boot,
                const server_config &cfg,
                const http_request &req,
                const char *body,
                size_t body_len,
                unsigned request_id,
                unsigned peer_ip)
{
    using namespace autodbg::common;

    /* 文本模式：头 X-AutoDbg-Format 优先，其次查询串 format= */
    bool text_mode = str_eq(req.format, "text");
    if (req.format[0] == '\0')
    {
        char fmt[16];
        if (http_query_get(req.query, "format", fmt, sizeof fmt) && str_eq(fmt, "text"))
            text_mode = true;
    }

    /* chunkname：`@<客户端文件名>`（错误信息里就是 `buffer.lua:12: …`） */
    char chunk[300];
    chunk[0]   = '@';
    chunk[1]   = '\0';
    const char *fname = (req.filename[0] != '\0') ? req.filename : "buffer.lua";
    size_t      flen  = strlen(fname);
    if (flen > sizeof chunk - 2)
        flen = sizeof chunk - 2;
    memcpy(chunk + 1, fname, flen);
    chunk[flen + 1] = '\0';

    collect_args(req.query, &g_args);

    /* ---- 流式（`X-AutoDbg-Stream: 1`）：响应变成 chunked + 帧；
     * 之后 `print`/`f:write` 都由**本线程**直接往这条 socket 写。 ---- */
    const bool streaming = req.wants_stream;
    if (streaming && !host::stream_begin(sock, k_send_timeout_ms))
        return; /* 头发不出去（客户端已断开）→ 什么也做不了 */

    host::guard_exec_begin(cfg.timeout_sec);
    /* S7：/run 也跑在**会话**里（协程形式），这样 read_file/write_file 才能挂起等客户端；
     * 完成就立刻关掉这个临时会话，挂起则留着等 `/effects`。 */
    char cerr[128];
    host::exec_session *s = host::session_create(cerr, sizeof cerr);
    if (s == nullptr)
    {
        send_json_status(sock, 503, "session_limit", cerr);
        return;
    }
    unsigned sid = s->id;

    host::output_begin(0, cfg.echo);
    host::guard_exec_begin(cfg.timeout_sec);

    exec_request er;
    er.src       = body;
    er.len       = body_len;
    er.chunkname = chunk;
    er.args      = g_args.items;
    er.argc      = g_args.count;

    exec_run     run = ::exec_begin(s->L, s->dbg, er);
    exec_result &r   = run.result; /* 下面的响应组装修复沿用 r.xxx */

    host::output_snapshot snap = host::output_end();
    int  reason    = host::guard_stop_reason();
    host::guard_exec_end();

    if (run.suspended)
    {
        if (streaming)
        {
            /* 流式请求里 client 文件 I/O 被禁（file_io 的 yield_blocker），正常走不到这里；
             * 真走到了就发 error 帧收尾，别把客户端挂在那里。 */
            static const char k_err[] = "{\"ok\":false,\"error\":\"unexpected suspend in streaming mode\"}";
            host::stream_write_frame(host::stream_frame_error, 0, 0, k_err, sizeof k_err - 1, nullptr, 0);
            host::stream_finish();
            host::stream_end();
            host::session_close(sid);
            return;
        }

        char  op[24]  = {};
        char *eff     = nullptr;
        size_t eff_len = 0;
        if (!take_pending_effect(sock, s, run.co_ref, op, sizeof op, &eff, &eff_len))
        {
            /* 效果表读不出来：当作失败处理（别把会话挂在那里） */
            host::session_close(sid);
            send_json_status(sock, 500, "internal_error", "bad pending effect");
            return;
        }
        host::session_set_pending(s, run.co_ref, op, /*is_mcp=*/false);
        send_pending(sock, sid, host::session_pending_gen(s), op, eff, eff_len, snap, false, request_id);
        if (eff != nullptr)
            release(eff);
        return;
    }

    bool ok        = (r.code == 0);
    bool truncated = snap.out_truncated || snap.err_truncated;
    bool canceled  = (reason == host::guard_cancel);
    bool timed_out = (reason == host::guard_timeout);

    if (streaming)
    {
        /* 流式：stdout/stderr 已经以 `out` 帧发过了，这里只补一个 result 帧 + 终止块 */
        json_writer w;
        w.begin_object();
        w.key("ok");
        w.boolean(ok);
        w.key("exit");
        w.number((long long)r.code);
        w.key("elapsed_ms");
        w.number((long long)r.elapsed_ms);
        w.key("truncated");
        w.boolean(truncated);
        w.key("canceled");
        w.boolean(canceled);
        w.key("timed_out");
        w.boolean(timed_out);
        w.key("flavor");
        w.string(flavor_name(boot->flavor));
        w.key("variant");
        w.string(variant_name(boot->variant));
        w.key("error");
        if (ok)
            w.null_value();
        else
            w.string(r.error);
        w.key("loaded");
        w.boolean(r.loaded);
        w.key("server");
        w.begin_object();
        w.key("version");
        w.string(k_server_version);
        w.key("request_id");
        w.number((long long)request_id);
        w.key("pid");
        w.number((long long)GetCurrentProcessId());
        w.end_object();
        w.end_object();

        host::stream_write_frame(host::stream_frame_result, 0, 0, w.data(), w.size(), nullptr, 0);
        host::stream_finish();
        host::stream_end();
    }
    else if (text_mode)
    {
        http_response resp(200, "text/plain; charset=utf-8");
        if (snap.out_len > 0)
            resp.write(snap.out, snap.out_len);
        if (snap.err_len > 0)
        {
            if (snap.out_len > 0)
                resp.write("\n--- stderr ---\n");
            resp.write(snap.err, snap.err_len);
        }
        resp.add_header_number("X-AutoDbg-Exit", (long long)r.code);
        resp.add_header("X-AutoDbg-Truncated", truncated ? "1" : "0");
        resp.add_header("X-AutoDbg-Canceled", canceled ? "1" : "0");
        resp.add_header("X-AutoDbg-Timed-Out", timed_out ? "1" : "0");
        send_response(sock, &resp);
    }
    else
    {
        json_writer w;
        w.begin_object();
        w.key("ok");
        w.boolean(ok);
        w.key("exit");
        w.number((long long)r.code);
        w.key("elapsed_ms");
        w.number((long long)r.elapsed_ms);
        w.key("truncated");
        w.boolean(truncated);
        w.key("canceled");
        w.boolean(canceled);
        w.key("timed_out");
        w.boolean(timed_out);
        w.key("flavor");
        w.string(flavor_name(boot->flavor));
        w.key("variant");
        w.string(variant_name(boot->variant));
        w.key("stdout");
        w.string(snap.out, snap.out_len);
        w.key("stderr");
        w.string(snap.err, snap.err_len);
        w.key("error");
        if (ok)
            w.null_value();
        else
            w.string(r.error);
        w.key("loaded");
        w.boolean(r.loaded);
        w.key("server");
        w.begin_object();
        w.key("version");
        w.string(k_server_version);
        w.key("request_id");
        w.number((long long)request_id);
        w.key("pid");
        w.number((long long)GetCurrentProcessId());
        w.end_object();
        w.end_object();

        http_response resp(200, "application/json; charset=utf-8");
        if (w.data() != nullptr)
            resp.write(w.data(), w.size());
        resp.add_header_number("X-AutoDbg-Exit", (long long)r.code);
        resp.add_header("X-AutoDbg-Truncated", truncated ? "1" : "0");
        resp.add_header("X-AutoDbg-Canceled", canceled ? "1" : "0");
        resp.add_header("X-AutoDbg-Timed-Out", timed_out ? "1" : "0");
        send_response(sock, &resp);
    }

    /* ---- --server-log（不记脚本内容） ---- */
    if (g_log != INVALID_HANDLE_VALUE)
    {
        log_line_buf lb;
        lb.add("req");
        lb.kv("id", (long long)request_id);
        lb.kv("peer", (long long)peer_ip);
        lb.add(" path=/run");
        lb.kv("body", (long long)body_len);
        lb.kv("exit", (long long)r.code);
        lb.kv("ms", (long long)r.elapsed_ms);
        lb.kv("out", (long long)snap.out_len);
        lb.kv("err", (long long)snap.err_len);
        lb.kv("truncated", truncated ? 1 : 0);
        lb.kv("canceled", canceled ? 1 : 0);
        lb.kv("timed_out", timed_out ? 1 : 0);
        lb.add("\n");
        lb.flush();
    }

    /* 临时会话：完成即销毁（挂起分支已提前 return） */
    host::session_close(sid);
}

/* ------------------------------ /mcp（S6） ------------------------------
 * MCP 的 streamable HTTP 入口：body 是一条（或一批）JSON-RPC 消息，分发在 Lua 层
 * （`lualib/mcp.lua`，D28）。会话用 `Mcp-Session-Id` 头表达（§8.3）：
 *   · 带上已有 id → 复用该会话（未知 id → 404，让客户端重新 initialize）；
 *   · 没带 → 新建一个，并在响应头里把 id 回给客户端。 */
void handle_mcp(long long sock,
                const server_config &cfg,
                const http_request &req,
                const char *body,
                size_t body_len,
                unsigned request_id)
{
    using namespace autodbg::common;

    host::exec_session *s = nullptr;
    if (req.mcp_session_id[0] != '\0')
    {
        s = host::session_find_by_text(req.mcp_session_id);
        if (s == nullptr)
        {
            json_writer w;
            w.begin_object();
            w.key("jsonrpc");
            w.string("2.0");
            w.key("id");
            w.null_value();
            w.key("error");
            w.begin_object();
            w.key("code");
            w.number(-32000LL);
            w.key("message");
            w.string("unknown session");
            w.end_object();
            w.end_object();

            http_response resp(404, "application/json; charset=utf-8");
            resp.add_header_number("Mcp-Session-Id", (long long)0);
            if (w.data() != nullptr)
                resp.write(w.data(), w.size());
            send_response(sock, &resp);
            return;
        }
    }

    char cerr[128];
    bool created = false;
    if (s == nullptr)
    {
        s = host::session_create(cerr, sizeof cerr);
        if (s == nullptr)
        {
            send_json_status(sock, 503, "session_limit", cerr);
            return;
        }
        created = true;
    }

    host::session_sweep(GetTickCount());

    unsigned sid = s->id;

    /* S7：`mcp.dispatch` 也跑在协程里 —— 工具内部（lua_eval）可能 yield 出
     * client 文件 I/O 效果，需要一路冒泡到宿主（见 lualib/mcp.lua 的 call_yielding）。 */
    static const char k_bootstrap[] = "return mcp.dispatch(...)";
    exec_request      mer;
    mer.src       = k_bootstrap;
    mer.len       = sizeof k_bootstrap - 1;
    mer.chunkname = "@mcp";
    const char *margs[1] = { body };
    mer.args      = margs;
    mer.argc      = 1;

    host::output_begin(0, false); /* MCP 不把 core 输出写控制台（工具自己捕获 print） */
    host::guard_exec_begin(cfg.timeout_sec);
    exec_run mrun = ::exec_begin(s->L, s->dbg, mer);
    host::output_snapshot msnap = host::output_end();
    host::guard_exec_end();

    if (mrun.suspended)
    {
        char   op[24]  = {};
        char  *eff     = nullptr;
        size_t eff_len = 0;
        if (!take_pending_effect(sock, s, mrun.co_ref, op, sizeof op, &eff, &eff_len))
        {
            host::session_clear_pending(s);
            send_json_status(sock, 500, "internal_error", "bad pending effect");
            return;
        }
        host::session_set_pending(s, mrun.co_ref, op, /*is_mcp=*/true);
        send_pending(sock, sid, host::session_pending_gen(s), op, eff, eff_len, msnap, true, request_id);
        if (eff != nullptr)
            release(eff);
        return;
    }

    char  *out     = mrun.ret; /* 协程返回值 = JSON-RPC 响应文本（nullptr = 通知） */
    size_t out_len = mrun.ret_len;
    bool   ok      = mrun.result.loaded;

    if (!ok)
    {
        if (out != nullptr)
            release(out);

        json_writer w;
        w.begin_object();
        w.key("jsonrpc");
        w.string("2.0");
        w.key("id");
        w.null_value();
        w.key("error");
        w.begin_object();
        w.key("code");
        w.number(-32603LL);
        w.key("message");
        w.string("internal error");
        w.key("data");
        w.string(mrun.result.error);
        w.end_object();
        w.end_object();

        http_response resp(200, "application/json; charset=utf-8");
        resp.add_header_number("Mcp-Session-Id", (long long)sid);
        if (w.data() != nullptr)
            resp.write(w.data(), w.size());
        send_response(sock, &resp);
        return;
    }

    if (out == nullptr)
    {
        /* 通知（无 id）：按 MCP 约定回 202 + 空体 */
        http_response resp(202, "text/plain; charset=utf-8");
        resp.add_header_number("Mcp-Session-Id", (long long)sid);
        send_response(sock, &resp);
    }
    else if (req.wants_sse)
    {
        /* streamable HTTP：SSE 单事件。整包作为一个 chunk 发出（增量流式见 plan_server.md §2.1） */
        size_t flen = out_len + 32;
        char  *frame = (char *)alloc(flen);
        if (frame != nullptr)
        {
            size_t n = 0;
            const char *prefix = "event: message\ndata: ";
            while (*prefix != '\0')
                frame[n++] = *prefix++;
            memcpy(frame + n, out, out_len);
            n += out_len;
            frame[n++] = '\n';
            frame[n++] = '\n';
            frame[n]   = '\0';

            http_response resp(200, "text/event-stream");
            resp.use_chunked(true);
            resp.add_header("Cache-Control", "no-cache");
            resp.add_header_number("Mcp-Session-Id", (long long)sid);
            resp.write(frame, n);
            send_response(sock, &resp);
            release(frame);
        }
        else
        {
            send_json_status(sock, 500, "oom", "out of memory");
        }
    }
    else
    {
        http_response resp(200, "application/json; charset=utf-8");
        resp.add_header_number("Mcp-Session-Id", (long long)sid);
        resp.write(out, out_len);
        send_response(sock, &resp);
    }

    if (out != nullptr)
        release(out);

    if (g_log != INVALID_HANDLE_VALUE)
    {
        log_line_buf lb;
        lb.add("req");
        lb.kv("id", (long long)request_id);
        lb.add(" path=/mcp");
        lb.kv("sid", (long long)sid);
        lb.kv("new", created ? 1 : 0);
        lb.kv("body", (long long)body_len);
        lb.add("\n");
        lb.flush();
    }
}
/* ------------------------------ /effects（S7） ------------------------------
 * `POST /effects?token=<sid>:<gen>`，body = `{"results":[{"ok":true,"data":"<base64>"}]}`
 * 把客户端替我们做完的文件 I/O 结果送回挂起的协程，继续执行。
 * token 里的 gen 防陈旧请求（每个挂起点 +1）。协议见 plan_server.md §9。 */
void handle_effects(long long sock,
                    const server_config &cfg,
                    const http_request &req,
                    const char *body,
                    size_t body_len,
                    unsigned request_id)
{
    using namespace autodbg::common;

    char token[64];
    if (!http_query_get(req.query, "token", token, sizeof token))
    {
        send_json_status(sock, 400, "bad_request", "missing ?token=");
        return;
    }

    host::exec_session *s = host::session_by_token(token, nullptr);
    if (s == nullptr)
    {
        send_json_status(sock, 404, "unknown_token", "token unknown or expired");
        return;
    }

    /* op 指针指向会话内部；`session_clear_pending` 会清掉它，先拷一份 */
    char   op_copy[24];
    const char *op_src = host::session_pending_op(s);
    size_t on = strlen(op_src);
    if (on > sizeof op_copy - 1)
        on = sizeof op_copy - 1;
    memcpy(op_copy, op_src, on);
    op_copy[on] = '\0';

    int      co_ref = host::session_pending_co_ref(s);
    bool     is_mcp = host::session_pending_is_mcp(s);
    unsigned sid    = s->id;
    lua_State *L    = s->L;

    host::output_begin(0, cfg.echo);

    /* ---- 用 effects.unpack(op, body) 解出「挂起函数要返回的值」（C 侧不写 JSON 解析器） ---- */
    int  base     = lua_gettop(L);
    int  nvals    = 0;
    bool unpacked = false;

    lua_getglobal(L, "effects");
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, "unpack");
        if (lua_isfunction(L, -1))
        {
            lua_pushstring(L, op_copy);
            lua_pushlstring(L, body, body_len);
            if (lua_pcall(L, 2, LUA_MULTRET, 0) == 0)
            {
                nvals    = lua_gettop(L) - base - 1; /* pcall 已弹掉 unpack，只剩 effects + 结果 */
                unpacked = true;
            }
        }
    }

    if (!unpacked)
    {
        lua_settop(L, base);
        host::output_end();
        host::session_clear_pending(s);
        send_json_status(sock, 400, "bad_effects", "cannot decode /effects body");
        return;
    }

    bool moved = ::exec_push_results(L, co_ref, nvals);
    lua_settop(L, base);
    if (!moved)
    {
        host::output_end();
        host::session_clear_pending(s);
        send_json_status(sock, 500, "internal_error", "cannot resume coroutine");
        return;
    }

    host::guard_exec_begin(cfg.timeout_sec);
    exec_run           run  = ::exec_resume(L, s->dbg, co_ref, nvals);
    host::output_snapshot snap = host::output_end();
    host::guard_exec_end();

    /* ❗**挂起时不能清 pending**：`clear_pending` 会 unref 协程，而同一个协程下一段
     * 还要继续用（连续 read_file/write_file 是同一个 co_ref）。只有真正跑完才清理。 */
    if (!run.suspended)
        host::session_clear_pending(s);

    if (run.suspended)
    {
        char   op2[24]  = {};
        char  *eff      = nullptr;
        size_t eff_len  = 0;
        if (!take_pending_effect(sock, s, run.co_ref, op2, sizeof op2, &eff, &eff_len))
        {
            host::session_close(sid);
            send_json_status(sock, 500, "internal_error", "bad pending effect");
            return;
        }
        host::session_set_pending(s, run.co_ref, op2, is_mcp);
        send_pending(sock, sid, host::session_pending_gen(s), op2, eff, eff_len, snap, is_mcp, request_id);
        if (eff != nullptr)
            release(eff);
        return;
    }

    if (is_mcp)
    {
        char  *out     = run.ret;
        size_t out_len = run.ret_len;

        if (!run.result.loaded)
        {
            if (out != nullptr)
                release(out);
            send_json_status(sock, 500, "internal_error", run.result.error);
            return;
        }
        if (out == nullptr)
        {
            http_response resp(202, "text/plain; charset=utf-8");
            resp.add_header_number("Mcp-Session-Id", (long long)sid);
            send_response(sock, &resp);
            return;
        }
        if (req.wants_sse)
        {
            size_t flen  = out_len + 32;
            char  *frame = (char *)alloc(flen);
            if (frame != nullptr)
            {
                size_t n = 0;
                const char *prefix = "event: message\ndata: ";
                while (*prefix != '\0')
                    frame[n++] = *prefix++;
                memcpy(frame + n, out, out_len);
                n += out_len;
                frame[n++] = '\n';
                frame[n++] = '\n';
                frame[n]   = '\0';

                http_response resp(200, "text/event-stream");
                resp.use_chunked(true);
                resp.add_header("Cache-Control", "no-cache");
                resp.add_header_number("Mcp-Session-Id", (long long)sid);
                resp.write(frame, n);
                send_response(sock, &resp);
                release(frame);
            }
            else
            {
                send_json_status(sock, 500, "oom", "out of memory");
            }
        }
        else
        {
            http_response resp(200, "application/json; charset=utf-8");
            resp.add_header_number("Mcp-Session-Id", (long long)sid);
            resp.write(out, out_len);
            send_response(sock, &resp);
        }
        if (out != nullptr)
            release(out);
        return;
    }

    /* /run 的恢复：与 /run 完成的信封一致，但 stdout 只含**恢复之后**的部分 */
    exec_result &r = run.result;
    json_writer w;
    w.begin_object();
    w.key("ok");
    w.boolean(r.code == 0);
    w.key("resumed");
    w.boolean(true);
    w.key("exit");
    w.number((long long)r.code);
    w.key("elapsed_ms");
    w.number((long long)r.elapsed_ms);
    w.key("truncated");
    w.boolean(snap.out_truncated || snap.err_truncated);
    w.key("canceled");
    w.boolean(host::guard_stop_reason() == host::guard_cancel);
    w.key("timed_out");
    w.boolean(host::guard_stop_reason() == host::guard_timeout);
    w.key("stdout");
    w.string(snap.out, snap.out_len);
    w.key("stderr");
    w.string(snap.err, snap.err_len);
    w.key("error");
    if (r.code == 0)
        w.null_value();
    else
        w.string(r.error);
    w.end_object();

    http_response resp(200, "application/json; charset=utf-8");
    if (w.data() != nullptr)
        resp.write(w.data(), w.size());
    resp.add_header_number("X-AutoDbg-Exit", (long long)r.code);
    send_response(sock, &resp);

    host::session_close(sid);
}
/* ============================ 工作线程（T2） ============================
 * 串行执行 `/run`：一个请求 = 建会话 + 建 Lua 状态 → 跑 → 全释放（D22 的无状态语义）。
 * 取消/超时由 `exec_guard` 在两条路径上插话（`on_wait_timeout` / Lua 计数钩子）。 */
DWORD WINAPI worker_proc(void *param)
{
    (void)param;

    for (;;)
    {
        WaitForSingleObject(g_q_event, 200);

        bool stopping = (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0);

        job j;
        if (queue_pop(&j))
        {
            InterlockedExchange(&g_worker_busy, 1);
            DWORD t0 = GetTickCount();

            if (j.is_effects)
                handle_effects(j.sock, *g_cfg, *j.req, j.buf + j.head_len, j.body_len, j.request_id);
            else if (j.is_mcp)
                handle_mcp(j.sock, *g_cfg, *j.req, j.buf + j.head_len, j.body_len, j.request_id);
            else
                handle_run(j.sock, g_boot, *g_cfg, *j.req, j.buf + j.head_len, j.body_len, j.request_id, j.peer_ip);

            /* 顺手回收空闲会话（S6）：只在 worker 线程上动会话表，不需要锁 */
            autodbg::host::session_sweep(GetTickCount());

            autodbg::common::net_close(j.sock);
            if (j.buf != nullptr)
                autodbg::common::release(j.buf);
            if (j.req != nullptr)
                autodbg::common::release(j.req);

            InterlockedExchange(&g_worker_busy, 0);

            log_line_buf lb;
            lb.add("[worker] req=");
            lb.num((long long)j.request_id);
            lb.add(" ");
            lb.num((long long)(GetTickCount() - t0));
            lb.add(" ms\n");
            lb.flush_to(GetStdHandle(STD_OUTPUT_HANDLE));
            continue;
        }

        if (stopping)
            break; /* 退出前把已入队的 job 清完 */
    }
    return 0;
}

/* ------------------------------ 单连接处理 ------------------------------
 * 返回 true = **socket 所有权已转给 worker**（本线程不得再 close/发送）。 */
bool handle_connection(long long sock,
                       const autodbg_boot *boot,
                       const server_config &cfg,
                       unsigned request_id,
                       unsigned peer_ip)
{
    using namespace autodbg::common;

    conn_buffer    buf;
    http_request   req;
    size_t         head_len = 0;
    read_result    rr       = read_head(sock, &buf, &req, &head_len);

    /* 所有失败路径统一走这里：应答（可选）+ 释放缓冲 + 交回「可以 close」 */
    auto fail = [&](int status, const char *text) -> bool
    {
        if (text != nullptr)
            send_status(sock, status, text);
        buf.free_buf();
        return false;
    };

    switch (rr)
    {
    case read_result::ok:
        break;
    case read_result::too_large:      return fail(413, "request header too large\n");
    case read_result::unsupported:    return fail(501, "chunked request bodies are not supported\n");
    case read_result::bad_request:    return fail(400, "bad request\n");
    case read_result::timeout:        return fail(408, "request timeout\n");
    case read_result::closed:
    default:                          return fail(0, nullptr);
    }

    /* ---- 认证（尽早拒绝，避免读大 body） ---- */
    if (!token_ok(cfg, req))
    {
        /* 诊断：区分「没带 token 头」与「值不对」。
         * ❗典型误判：在 **cmd.exe** 里粘 bash 写法的单引号（`-H 'X-AutoDbg-Token: xxx'`）→
         *   头名会变成 `'X-AutoDbg-Token`（带前导单引号）→ 看起来就是「没带 token」。
         * 直接写 stdout（不经 core 的 sink：T1 的诊断不能被捕进别人的响应）。 */
        {
            log_line_buf lb;
            lb.add("server: 401 ");
            lb.add(req.has_token ? "token-mismatch" : "no-token-header");
            lb.add(" path=");
            lb.add(req.path);
            lb.add("\n");
            lb.flush_to(GetStdHandle(STD_OUTPUT_HANDLE));
        }
        http_response resp(401, "application/json; charset=utf-8");
        resp.add_header("WWW-Authenticate", "Bearer");
        resp.write("{\"ok\":false,\"error\":\"unauthorized\","
                   "\"hint\":\"send X-AutoDbg-Token: <token> or Authorization: Bearer <token>\"}");
        send_response(sock, &resp);
        buf.free_buf();
        return false;
    }

    const char *path    = req.path;
    bool        is_get  = (req.method == http_method::get);
    bool        is_post = (req.method == http_method::post);
    bool        is_opt  = (req.method == http_method::options);

    if (is_opt)
    {
        http_response resp(204, nullptr);
        resp.add_header("Access-Control-Allow-Origin", "*");
        resp.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        resp.add_header("Access-Control-Allow-Headers", "content-type, authorization, x-autodbg-token, x-autodbg-filename, x-autodbg-format");
        send_response(sock, &resp);
        buf.free_buf();
        return false;
    }

    /* ---- /cancel：T1 直接应答 → 长脚本阻塞在 Run() 里也能取消（S2） ---- */
    if (str_eq(path, "/cancel"))
    {
        if (!is_post)
            send_json_status(sock, 405, "method_not_allowed", "POST /cancel");
        else
            handle_cancel(sock);
        buf.free_buf();
        return false;
    }

    /* ---- /shutdown ---- */
    if (str_eq(path, "/shutdown"))
    {
        json_writer w;
        w.begin_object();
        w.key("ok");
        w.boolean(true);
        w.key("stopping");
        w.boolean(true);
        w.end_object();

        http_response resp(200, "application/json; charset=utf-8");
        if (w.data() != nullptr)
            resp.write(w.data(), w.size());
        send_response(sock, &resp);

        InterlockedExchange(&g_shutdown, 1);
        if (g_q_event != nullptr)
            SetEvent(g_q_event);
        buf.free_buf();
        return false;
    }

    /* ---- /run：读 body → 入队（worker 执行 + 应答 + close） ---- */
    bool is_run     = str_eq(path, "/run");
    bool is_mcp     = str_eq(path, "/mcp");
    bool is_effects = str_eq(path, "/effects");

    if (is_run || is_mcp || is_effects)
    {
        if (!is_post)
        {
            send_json_status(sock, 405, "method_not_allowed",
                             is_run ? "POST /run" : (is_mcp ? "POST /mcp" : "POST /effects"));
            buf.free_buf();
            return false;
        }
        if (req.chunked_request)
        {
            send_json_status(sock, 501, "unsupported", "chunked request bodies are not supported");
            buf.free_buf();
            return false;
        }
        if (!req.has_content_length)
        {
            send_json_status(sock, 411, "length_required", "Content-Length required");
            buf.free_buf();
            return false;
        }
        if (req.content_length < 0 || (unsigned long long)req.content_length > (unsigned long long)cfg.max_body)
        {
            send_json_status(sock, 413, "too_large", "script body exceeds --server-max-body");
            buf.free_buf();
            return false;
        }

        if (req.expect_continue && buf.len == head_len)
        {
            const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
            net_send_all(sock, cont, strlen(cont), k_send_timeout_ms);
        }

        size_t      need = head_len + (size_t)req.content_length;
        read_result br   = read_more(sock, &buf, need);
        if (br != read_result::ok)
        {
            if (br == read_result::timeout)
                send_status(sock, 408, "request body timeout\n");
            else if (br == read_result::too_large)
                send_status(sock, 413, "request body too large\n");
            buf.free_buf();
            return false;
        }

        /* 入队：把 socket / 缓冲 / 解析结果的所有权一起交出去 */
        job j;
        j.is_mcp     = is_mcp;
        j.is_effects = is_effects;
        j.sock       = sock;
        j.request_id = request_id;
        j.peer_ip    = peer_ip;
        j.buf        = buf.p;
        j.head_len   = head_len;
        j.body_len   = (size_t)req.content_length;
        j.req        = (http_request *)alloc(sizeof(http_request));
        *j.req       = req;

        if (!queue_push(j))
        {
            send_json_status(sock, 503, "queue_full", "execution queue is full, retry later");
            release(j.req);
            buf.free_buf();
            return false;
        }

        buf.p = nullptr; /* 所有权已转移 → 本线程不再释放 */
        buf.len = 0;
        buf.cap = 0;
        return true;
    }

    /* ---- 只读端点 ---- */
    if (str_eq(path, "/health"))
    {
        if (!is_get)
            send_json_status(sock, 405, "method_not_allowed", "GET /health");
        else
            handle_health(sock, cfg);
        buf.free_buf();
        return false;
    }
    if (str_eq(path, "/version"))
    {
        if (!is_get)
            send_json_status(sock, 405, "method_not_allowed", "GET /version");
        else
            handle_version(sock, boot, cfg);
        buf.free_buf();
        return false;
    }
    if (str_eq(path, "/") || str_eq(path, "/help"))
    {
        if (!is_get)
            send_json_status(sock, 405, "method_not_allowed", "GET /");
        else
            send_status(sock, 200,
                        "AutoDbg server " AUTODBG_VERSION
                        "\n"
                        "GET  /health      liveness (+busy/queue)\n"
                        "GET  /version     build info\n"
                        "POST /run         execute lua source (body = source)\n"
                        "POST /cancel      cancel the running script\n"
                        "POST /shutdown    stop server\n");
        buf.free_buf();
        return false;
    }

    send_json_status(sock, 404, "not_found", path);
    buf.free_buf();
    return false;
}
} /* namespace */

/* ============================== 配置解析 ============================== */
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
            set_err(err, err_cap, "非 loopback 地址不允许 --server-token=none（除非另加 --server-allow-remote-no-auth）");
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

/* ============================== 入口 ============================== */
int server_main(const autodbg_boot *boot)
{
    using namespace autodbg::common;
    using autodbg::core::ErrorS;
    using autodbg::core::PrintF;
    using autodbg::core::PrintS;

    /* T2（D42）：`--server=tcp` 走 raw TCP 帧（新的主契约）；1 = 现行 HTTP（待删）。 */
    if (boot->server_mode == 2u)
        return tcp_server_main(boot);

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

    if (cfg.timeout_sec != 0)
    {
        PrintF("autodbg server: --server-timeout=%us 已生效（引擎阻塞走 on_wait_timeout，纯 Lua 死循环走计数钩子）\n",
               cfg.timeout_sec);
    }

    net_status why = net_ok;
    if (net_startup() != net_ok)
    {
        ErrorS("server: 本系统没有可用的 winsock（ws2_32.dll / wsock32.dll 都载不进）\n");
        return 2;
    }

    unsigned actual_port = 0;
    long long listener = net_listen(cfg.host, cfg.port, &actual_port, &why);
    if (listener < 0)
    {
        ErrorS("server: 监听失败 ");
        ErrorS(net_status_name(why));
        ErrorS("（");
        ErrorS(cfg.host);
        ErrorS(":");
        PrintF("%u", cfg.port);
        ErrorS("）\n");
        net_cleanup();
        return 2;
    }

    PrintS("autodbg server: listen=");
    PrintS(cfg.host);
    PrintF(":%u", actual_port);
    if (cfg.auth_required)
    {
        PrintS(" (auth: bearer, token ");
        PrintS(cfg.token);
        PrintS(")\n");
    }
    else
    {
        PrintS(" (auth: none, loopback only)\n");
    }
    PrintF("autodbg server: max_body=%u queue=%u timeout=%us echo=%s winsock=%s\n",
           cfg.max_body, cfg.queue_depth, cfg.timeout_sec, cfg.echo ? "on" : "off",
           (net_module_name() != nullptr) ? net_module_name() : "(none)");

    /* 绑到 0.0.0.0 时把本机网卡地址列出来：client 直接用其中一条就能连（不用猜 IP）。
     * 走 PrintS（core 的输出分流：控制台 / 重定向都能看）；取不到就安静地不列。 */
    {
        unsigned any = 1;
        if (autodbg::common::net_parse_ipv4(cfg.host, &any) && any == 0)
        {
            char   addrs[1024];
            size_t n = autodbg::common::net_describe_local_addresses(addrs, sizeof addrs);
            if (n > 0)
            {
                PrintS("autodbg server: 本机网卡地址（client 填其中任一条）:\n");
                PrintS(addrs);
            }
            PrintS("autodbg server: 提示: 非 loopback 监听需要 token，且防火墙可能要放行该端口\n");
        }
    }

    /* ---- S2：队列 + 工作线程 ---- */
    if (cfg.queue_depth > (unsigned)k_job_queue_max)
        cfg.queue_depth = (unsigned)k_job_queue_max;
    g_q_limit = (int)cfg.queue_depth;

    InitializeCriticalSection(&g_q_lock);
    g_q_lock_init = true;

    g_q_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (g_q_event == nullptr)
    {
        ErrorS("server: CreateEvent 失败\n");
        net_close(listener);
        net_cleanup();
        return 2;
    }

    g_boot = boot;
    g_cfg  = &cfg;
    log_open(cfg.log_path);

    /* S7：server 模式下 read_file/write_file 指向**客户端**那台机器（效果协议），
     * 而不是服务器本机磁盘。 */
    host::set_client_io(true);

    g_worker = CreateThread(nullptr, 0, worker_proc, nullptr, 0, nullptr);
    if (g_worker == nullptr)
    {
        ErrorS("server: CreateThread 失败\n");
        CloseHandle(g_q_event);
        g_q_event = nullptr;
        net_close(listener);
        net_cleanup();
        return 2;
    }

    PrintS("autodbg server: ready（Ctrl+C 或 POST /shutdown 退出）\n");

    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    InterlockedExchange(&g_shutdown, 0);

    unsigned request_id = 0;
    while (InterlockedCompareExchange(&g_shutdown, 0, 0) == 0)
    {
        unsigned peer_ip = 0;
        long long c      = net_accept(listener, k_accept_timeout_ms, &peer_ip);
        if (c == 0)
            continue; /* 超时：回去看 g_shutdown */
        if (c < 0)
        {
            /* 单个 accept 失败不该让 server 停掉（被杀毒/防火墙抖动打断的常见形态） */
            Sleep(20);
            continue;
        }

        request_id++;
        DWORD t0 = GetTickCount();

        /* true = socket 交给 worker；它负责 send + close */
        bool deferred = handle_connection(c, boot, cfg, request_id, peer_ip);
        if (!deferred)
            net_close(c);

        /* 走 WriteFile（不经 core 的 sink）：否则 worker 正在执行时这一行会被捕进别人的响应 */
        log_line_buf lb;
        lb.add("[");
        lb.num((long long)request_id);
        lb.add("] peer=");
        lb.num((long long)((peer_ip >> 24) & 0xFF));
        lb.add(".");
        lb.num((long long)((peer_ip >> 16) & 0xFF));
        lb.add(".");
        lb.num((long long)((peer_ip >> 8) & 0xFF));
        lb.add(".");
        lb.num((long long)(peer_ip & 0xFF));
        lb.add(deferred ? " queued " : " inline ");
        lb.num((long long)(GetTickCount() - t0));
        lb.add(" ms\n");
        lb.flush_to(GetStdHandle(STD_OUTPUT_HANDLE));
    }

    PrintS("autodbg server: stopping\n");

    /* 先停监听（不再接新连接），再等工作线程把已入队的 job 跑完 */
    net_close(listener);
    if (g_q_event != nullptr)
        SetEvent(g_q_event);
    if (g_worker != nullptr)
    {
        WaitForSingleObject(g_worker, 30000);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_q_event != nullptr)
    {
        CloseHandle(g_q_event);
        g_q_event = nullptr;
    }

    log_close();
    autodbg::host::session_close_all(); /* S6：会话里有 lua_State 与引擎会话 */
    DeleteCriticalSection(&g_q_lock);
    g_q_lock_init = false;
    net_cleanup();
    SetConsoleCtrlHandler(on_console_ctrl, FALSE);
    return 0;
}
} /* namespace autodbg::server */
