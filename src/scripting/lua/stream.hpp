#ifndef AUTODBG_SCRIPTING_LUA_STREAM_HPP
#define AUTODBG_SCRIPTING_LUA_STREAM_HPP
/* =============================================================================
 * src/scripting/lua/stream.hpp —— 流式 `/run` 响应（帧 + chunked）
 *
 * 来源：新写（用户需求：`write_file` 一次性 + effects 往返不适合「大块数据流式写」）。
 *
 * 背景：server 模式的 `read_file`/`write_file` 靠 `lua_yield` 把活交给客户端，而
 *   **调试回调里不能 yield**（LuaJIT 不支持跨 C 边界，Lua 5.5 也不允许跨普通 lua_pcall），
 *   于是只能在回调里把数据攒起来——数据量大就爆内存。
 *
 * 解法：`POST /run` 带 `X-AutoDbg-Stream: 1` 时，响应变成 `Transfer-Encoding: chunked`；
 *   每个 HTTP chunk 里套一个 8 字节帧头 + 可选 JSON 头 + 原始数据。
 *   **写这条 socket 的就是执行脚本的那条线程**（worker 在 `exec_begin` 之前就拿到了 sock），
 *   所以 `open_file()` 的 `f:write()` 既不需要 yield，也不需要额外线程/队列/临时文件：
 *   数据直接进内核发送缓冲，内存占用 = 单次 send 的那一块。
 *
 * 帧布局（小端）：
 *   u8  type   1=open 2=data 3=close 4=out 5=result 6=error
 *   u8  flags  out 帧：0=stdout 1=stderr；其它帧为 0
 *   u16 id     句柄号（open/data/close）；out/result/error 为 0
 *   u32 data_len
 *   [json]     仅 open/result/error（长度 = chunk 长度 - 8 - data_len）
 *   [data]     原始字节（不编码、不转义）
 *
 * 背压：`net_send_all(sock, …, send_timeout_ms)` —— 客户端读得慢就压在 socket 缓冲里，
 *   客户端死了/超时就返回失败，`f:write` 得到 `nil, err`（不会无限卡住调试目标）。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::host
{
enum stream_frame_type
{
    stream_frame_open   = 1,
    stream_frame_data   = 2,
    stream_frame_close  = 3,
    stream_frame_out    = 4,
    stream_frame_result = 5,
    stream_frame_error  = 6,
};

/* 开始一条流：发响应头（chunked）并把 socket 记下来。返回 false = 发头失败。 */
bool stream_begin(long long sock, unsigned send_timeout_ms);

/* 结束流状态（不发终止块；终止块用 `stream_finish`） */
void stream_end();

/* 当前是否有活跃的流（= 本次请求是流式请求） */
bool stream_active();

/* 写一帧。`json`/`data` 可为空。返回 false = 发送失败（客户端断开/超时）。 */
bool stream_write_frame(unsigned type,
                        unsigned id,
                        unsigned flags,
                        const char *json,
                        size_t      json_len,
                        const void *data,
                        size_t      data_len);

/* 发终止块 `0\r\n\r\n`（chunked 结束） */
bool stream_finish();
} /* namespace autodbg::host */

#endif /* AUTODBG_SCRIPTING_LUA_STREAM_HPP */
