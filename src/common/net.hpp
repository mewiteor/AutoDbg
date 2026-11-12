#ifndef AUTODBG_COMMON_NET_HPP
#define AUTODBG_COMMON_NET_HPP
/* =============================================================================
 * src/common/net.hpp —— 零 CRT 的 winsock 薄封装（server 模式的传输层底座）
 *
 * 来源：新写（plan_server.md S1 / §2.3 / §4.2，决策 D20）。
 * 为什么必须动态加载（而不是链 `ws2_32.lib`）：
 *   `tools/api_check` 的数据集实测 **Win95 根本没有 ws2_32.dll**（只有 wsock32.dll =
 *   Winsock 1.1）。手工映射器解析静态导入是**硬失败** → legacy 模块在 9x 上会整体
 *   加载不了。所以这里运行期依次尝试 `ws2_32.dll` → `wsock32.dll`，两个都取不到就
 *   明确报「本系统不支持 server 模式」，脚本模式仍然可用。
 * 形态：句柄用 `long long` 承载（避免把 `<winsock2.h>` 泄到头文件里，也避开
 *   「winsock2.h 必须先于 windows.h」的经典顺序坑）。调用方只认返回值约定：
 *   `net_recv` 返回 `>0` 字节数 / `0` 对端关闭 / `-1` 错误 / `-2` 超时。
 * 约束：只用 Winsock **1.1 就有**的 API（WSAStartup/socket/bind/listen/accept/recv/
 *   send/select/closesocket/getsockname/setsockopt/ioctlsocket）→ 一套代码同时
 *   适配 ws2_32 与 wsock32；不用 WSAEventSelect/IOCP/WSAPoll。
 * ========================================================================== */
#include <stddef.h>

namespace autodbg::common
{
enum net_status
{
    net_ok = 0,
    net_unavailable,     /* ws2_32.dll 与 wsock32.dll 都载不进 */
    net_startup_failed,  /* WSAStartup 失败 */
    net_bad_addr,        /* host 不是合法 IPv4 / localhost */
    net_socket_failed,
    net_bind_failed,
    net_listen_failed,
};

/* 进程级初始化（幂等）；返回 net_ok 才可用 */
net_status net_startup();

/* 释放（进程退出前调用；重复调用无害） */
void net_cleanup();

/* 实际载入的 winsock 模块名（"ws2_32.dll" / "wsock32.dll" / nullptr） */
const char *net_module_name();

/* 诊断串（日志/错误文案用，ASCII） */
const char *net_status_name(net_status s);

/* IPv4 文本 → 32 位网络序整数；"localhost" 视为 127.0.0.1。
 * 成功返回 true（*out_host_order 为**主机序**的点分十进制值，便于日志）。 */
bool net_parse_ipv4(const char *host, unsigned *out_host_order);

/* 监听：`port == 0` 由系统分配（`*actual_port` 回真实端口）。
 * 成功返回 socket 句柄（>= 0）；失败返回 -1 并填 `*why`（可为 nullptr）。 */
long long net_listen(const char *host, unsigned port, unsigned *actual_port, net_status *why);

/* accept：`timeout_ms < 0` 阻塞等待。返回 socket；-1 = 错误；0 表示「没连进来」（超时） */
long long net_accept(long long listener, int timeout_ms, unsigned *peer_ip);

/* 连接（T3：客户端模式，D42）。成功返回 socket（>= 0）；-1 = 失败并填 `*why`。
 * ❗用**阻塞 connect**（Winsock 1.1 的常规做法）：不可达地址由系统决定超时，本层不另加超时；
 *   对已知的 server 或「拒绝连接的本地端口」它立即返回。 */
long long net_connect(const char *host, unsigned port, net_status *why);

/* 关闭 socket（`< 0` 时什么也不做） */
void net_close(long long sock);

/* 收：`>0` 字节数；`0` 对端正常关闭；`-1` 错误；`-2` 超时 */
int net_recv(long long sock, void *buf, size_t cap, int timeout_ms);

/* 发（写满为止）：true = 全部写出 */
bool net_send_all(long long sock, const void *data, size_t len, int timeout_ms);

/* 本机 IPv4 地址清单（给「绑 0.0.0.0 时客户端该填哪个 IP」用）——
 * 每条网卡占一行「地址 + 网卡描述」，写成 **UTF-8**、以 `\n` 结尾、**不含 NUL**；
 * 返回写入字节数（0 = 取不到，调用方什么也不打）。
 * 实现：首选 iphlpapi!GetAdaptersInfo（带网卡描述名；Win95 可能没 iphlpapi → 动态取不到
 * 就降级），兕底用 winsock 的 gethostname/gethostbyname（只有主机名 + 地址）。 */
size_t net_describe_local_addresses(char *dst, size_t cap);
} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_NET_HPP */
