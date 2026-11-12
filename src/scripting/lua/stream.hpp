#ifndef AUTODBG_SCRIPTING_LUA_STREAM_HPP
#define AUTODBG_SCRIPTING_LUA_STREAM_HPP
/* =============================================================================
 * src/scripting/lua/stream.hpp —— 双向帧协议的传输层（T2）
 *
 * 来源：从「流式 `/run` 响应（帧 + chunked）」扩成 plan_cli.md §2.1 的**双向**协议。
 *
 * 分层（改之前先认清这三层，否则会改错地方）：
 *   ① **帧头编解码**：`src/common/frame.hpp`（纯函数、可被 `--selftest` 断言）；
 *   ② **本层**：帧类型/协议版本、传输钩子（send/recv）、payload 分帧方式、读一帧/写一帧；
 *   ③ 传输后端：现行 = winsock + HTTP chunked（T5 随 D41 删除）；T3 起 = raw TCP 帧。
 *
 * 帧布局（小端，8 字节头 + 可选 json + 可选原始字节）：
 *   u8 type | u8 flags | u16 id | u32 data_len | [json] | [data]
 * payload 总长由**分帧层**给：chunked 用 hex 长度头、raw 用 u32 前缀 →
 *   json_len = payload_len - 8 - data_len（见 `common/frame.hpp` 的说明）。
 *
 * 约定（D43/D45）：
 *   · 一次调用 = 一次执行，严格串行 ⇒ 执行连接**只有一个 reader**（脚本线程）；
 *   · `u16 id` 是**句柄号**，不是请求相关性 id；同一句柄最多一个在途请求；
 *   · 写这条连接的就是执行脚本的那条线程 ⇒ `f:write` 不需要 yield、回调里也能用。
 *
 * 握手帧（`stream_frame_handshake`）的字段落点（T2 定稿）：
 *   `id`   = 协议版本（u16，与 `k_stream_proto_version` 比）；
 *   `data` = token（utf8 字节；loopback 且未开认证时长度为 0）；
 *   `json` = `{pid, filename, read_timeout, max_frame, timeout_sec}`（可选；C 侧目前不当场解——
 *            D28 不写 JSON parser，需要时由执行侧用 `jsonx.decode`）。
 *   为什么不把 version 也塞进 json：版本不符必须能在**建 `lua_State` 之前**拒绝。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::host
{
/* 协议版本：握手帧（`stream_frame_handshake`）里带；不符 → error 帧 + 关连接。
 * T2 定版为 1；改帧类型/字段语义必须 +1（客户端与模块同版本才能通）。 */
enum
{
    k_stream_proto_version = 1,
};

/* 帧类型：出向 1–9（server → client）、入向 10–14（client → server）、15 = 双向通知 */
enum stream_frame_type
{
    /* ---- 出向 ---- */
    stream_frame_open   = 1,  /* json {path, mode}；id = 句柄号 */
    stream_frame_data   = 2,  /* 原始字节（不编码、不转义）；id = 句柄号 */
    stream_frame_close  = 3,  /* id = 句柄号 */
    stream_frame_out    = 4,  /* flags：0 = stdout / 1 = stderr */
    stream_frame_result = 5,  /* json = `--json` 的同一份信封；❗`id` = 退出码（u16 二补数）、
                               * `flags` 位：0x01 ok / 0x02 canceled / 0x04 timed_out / 0x08 truncated
                               * （客户端要退码与状态但**不得解 JSON**（D28）⇒ 走帧头） */
    stream_frame_error  = 6,  /* json {error} */
    stream_frame_read_req  = 7,  /* json {id, format | bytes, whence, offset} */
    stream_frame_ack       = 8,  /* json {applied, reason, pid}（控制命令的回执）；
                                  * ❗`flags` bit0 = applied（客户端不看 json 就能判成败） */
    stream_frame_info      = 9,  /* json（health / version 的应答，替代 GET /health、/version） */

    /* ---- 入向 ---- */
    stream_frame_handshake = 10,  /* json {version, pid, filename, token, read_timeout, timeout_sec, max_frame} */
    stream_frame_script    = 11,  /* data = Lua 源码（D26：全程不落盘） */
    stream_frame_control   = 12,  /* json {cmd：cancel | kill | detach | health | version} */
    stream_frame_read_resp = 13,  /* data 或 json {error}；data_len == 0 且 json 为 null = EOF */
    stream_frame_end       = 14,  /* 无 body（替代 chunked 终止块） */

    /* ---- 双向 ---- */
    stream_frame_cancel_notify = 15,  /* json {id}：被取消的请求，client 必须丢弃该轮本地 I/O */
};

/* payload 的分帧方式（帧布局本身与传输无关，只有这一层不同） */
enum stream_framing
{
    stream_framing_chunked = 0,  /* HTTP chunked：`<hex>\r\n` + payload + `\r\n`（现行 server） */
    stream_framing_raw     = 1,  /* raw TCP：`u32` 小端 = payload 总长（D42） */
};

/* `control` 帧（type 12）的命令：放 `flags` 里，**不靠 JSON**——
 * D28 说 C 侧不写 JSON parser，而控制命令必须能在不建 `lua_State` 的情况下分发（cancel 要
 * 在脚本抦死的时候也能进）。JSON 只用于**输出**（ack / info）。 */
enum stream_control_cmd
{
    stream_control_cancel   = 0,  /* 硬取消：`lua_sethook` + 等待循环里的 guard（D46） */
    stream_control_kill     = 1,  /* 软：结束当前 Attach/Run（T4 接 on_wait_timeout） */
    stream_control_detach   = 2,  /* 软：真 detach（T4；D47） */
    stream_control_health   = 3,  /* 应答走 `info` 帧（替代 GET /health） */
    stream_control_version  = 4,  /* 同上（替代 GET /version） */
    stream_control_shutdown = 5,  /* 停机（e2e 用；等价 POST /shutdown） */
};

/* `ack` 帧的 `reason`：短 ASCII 串，直接进 JSON（不带引号） */

/* ---- T4（D45/D49）`open_file` 的读写协议（二进制，D28：两侧都不得解 JSON） ----
 * `open`（1，server → client）：json {path, mode}，`id` = 句柄。file / client 均回一个
 *   `read_resp`（flags bit1 = 失败）；服务端 `open_file` 会等这个应答。
 * `read_req`（7，server → client）：`id` = 句柄，`flags` = op | (whence << 4)，
 *   `data` = 8 字节 {u32 bytes 或 offset, u32 保留}。
 * `read_resp`（13，client → server）：`id` = 句柄，`flags` bit0 = EOF / bit1 = 错误，
 *   `data` = 字节（read/line）或 8 字节 {u32 pos}（seek/tell）。
 * `data`（2，server → client）：写模式的原始字节；`close`（3）：关句柄。 */
enum stream_read_op
{
    stream_read_bytes = 0,  /* 最多读 N 字节 */
    stream_read_line  = 1,  /* 读一行（含 `\n`） */
    stream_read_seek  = 2,  /* offset + whence */
    stream_read_tell  = 3,  /* 要当前位置 */
};
enum stream_read_whence
{
    stream_whence_set = 0,
    stream_whence_cur = 1,
    stream_whence_end = 2,
};
enum stream_read_resp_flags
{
    stream_resp_eof   = 0x01,  /* 这一轮到了文件尾 */
    stream_resp_error = 0x02,  /* 客户端本地 I/O 失败（
                                * 详细文本不进回路：D28 不解 JSON） */
};

/* 传输钩子（单连接、串行）。
 *   send：写满为止；false = 客户端断开/超时。
 *   recv：>0 = 读到的字节数；0 = 对端关闭；-1 = 错误；-2 = 超时（调用方自己切片）。
 *         只有「执行连接」需要读；纯写场景（HTTP 响应）可以给 nullptr。
 *   recv_nowait：**非阻塞**读：>0 字节数 / 0 对端关闭 / -1 错误 / **-2 = 现在没数据**。
 *         执行期间的两个 poll 点（`on_wait_timeout` 的 100 ms、`lua_sethook` 的计数钩子）
 *         用它——那两个点绝不能阻塞，否者调试目标会跟着卡。可给 nullptr（= 不支持，poll 返回 0）。 */
typedef bool (*stream_send_fn)(void *ctx, const void *data, size_t len);
typedef int (*stream_recv_fn)(void *ctx, void *buf, size_t cap);
typedef int (*stream_recv_nowait_fn)(void *ctx, void *buf, size_t cap);

struct stream_transport
{
    stream_send_fn       send;
    stream_recv_fn       recv;
    stream_recv_nowait_fn recv_nowait;
    void                *ctx;
};

/* 开始一条流：现行调用点（HTTP + chunked over winsock）。返回 false = 发响应头失败。 */
bool stream_begin(long long sock, unsigned send_timeout_ms);

/* 同上，但传输与分帧由调用方给（T3 的 raw TCP 走这个；`recv` 必须给）。 */
bool stream_begin_ex(const stream_transport &tp, int framing, unsigned send_timeout_ms);

/* 结束流状态（不发终止块；chunked 的终止块用 `stream_finish`） */
void stream_end();

/* 当前是否有活跃的流（= 本次请求是流式请求） */
bool stream_active();

/* 写一帧。`json` / `data` 可为空。返回 false = 发送失败（客户端断开/超时），
 * 失败后流失效，后续写入立刻返回 false（不反复等超时）。 */
bool stream_write_frame(unsigned type,
                        unsigned id,
                        unsigned flags,
                        const char *json,
                        size_t      json_len,
                        const void *data,
                        size_t      data_len);

/* 读一帧（执行连接的唯一 reader 用它）。`hdr` 收 8 字节帧头、`buf` 收 json+data。
 * 返回：>0 = payload 总长（`*out_json_len` 已填、`hdr` 已有内容）
 *       0  = 对端正常关闭
 *      -1  = 传输错误（含对端在帧中间关连接）
 *      -2  = 超时（**不是**错误：调用方切片重试，并顺手查取消标志）
 *      -3  = 帧非法（长度越界 / type == 0 / 超 `max_frame` / JSON 段放不下）
 * `max_frame` = 0 时按 `common::k_frame_max_payload`。 */
long stream_read_frame(unsigned char *hdr,
                       void          *buf,
                       size_t         cap,
                       size_t        *out_json_len,
                       unsigned       max_frame);

/* chunked 终止块 `0\r\n\r\n`（raw 分帧下不发；随 D41 的 HTTP 删除一起消失） */
bool stream_finish();

/* ---------------- 执行期间的 control（D32/D45：两个 poll 点用这套） ---------------- */

/* 控制帧处理器（由服务端实现：cancel → guard_request_cancel；kill/detach → 软动作……）。
 * ❗它跑在**脚本线程**上（`on_wait_timeout` / `lua_sethook` 的调用栈里），所以里面**不得**
 *   做阻塞等待，也不得重入执行。 */
typedef void (*stream_control_fn)(void *ctx, unsigned cmd, unsigned flags);
void stream_set_control_handler(stream_control_fn fn, void *ctx);

/* 非阻塞收一帧：执行期的两个 poll 点调它。
 * 返回：0 = 没收到完整帧（继续干活）；1 = 取到一帧（`*out_type` / `*out_flags` 有效，
 *       json 段拷进 `json` 并 NUL 结尾）；-1 = 传输错误/断线；-2 = 帧非法/超过 `k_ctrl_max`。
 * ❗只有脚本线程调（唯一 reader）；内部有 4 KB 重组缓冲，只应付 `control` / `end` 这种小帧。 */
int stream_poll_frame(unsigned *out_type, unsigned *out_flags, char *json, size_t json_cap);

/* poll 一帧：若是 `control` 就交给 handler（不是 control 就忽略）。
 * 执行期两个点的**唯一**入口——`bindings.cpp` 与 `exec_guard.cpp` 都调它。 */
void stream_poll_and_dispatch();

/* ---- 主读路径（T4 / D45）：脚本线程等一帧（阻塞读的回包、open 的应答都用它） ----
 * 切片等待（≤ 250 ms 一片），片间查 `exec_guard`；`timeout_ms` 是整次等待的上限。
 * 处理规则：`control` → 分发后继续等；`read_resp` 但 id 不符 → **丢弃**（stale，D45 规则 3）；
 * `cancel_notify` → 返回 -4；其它类型原样报给调用方（调用方自己决定要不要）。
 * 返回：>0 = payload 总长（json+data 已拷进 `buf`，切分见 `*out_json_len` / `*out_data_len`）
 *       0 = 对端关闭 / `end` 帧；-1 = 传输错误；-2 = 超时；-3 = 帧非法；-4 = 被取消 */
long stream_wait_frame(unsigned want_type, unsigned want_id, void *buf, size_t cap, size_t *out_json_len,
                       size_t *out_data_len, unsigned *out_type, unsigned *out_flags, unsigned *out_id,
                       unsigned timeout_ms);

/* 当前传输能不能**双向**（收到客户端的回包）：只有 raw 分帧且 recv_nowait 就位才行。
 * chunked（现行 HTTP）是单向的 → `open_file` 在那种请求里只能 fire-and-forget（T5 删）。 */
bool stream_can_wait();

/* 给客户端发 `cancel_notify {id}`：告诉它「刚才那个请求作废，别回包」（D45 规则 4） */
bool stream_cancel_notify(unsigned id);
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_STREAM_HPP */
