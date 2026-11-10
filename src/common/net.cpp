/* =============================================================================
 * src/common/net.cpp —— winsock 薄封装实现（server 传输层底座，S1）
 *
 * 来源：新写（plan_server.md S1）。
 * 与旧实现的差异：无（新文件）。
 * 关键实现点：
 *   ① **动态加载**：`ws2_32.dll` → 失败退 `wsock32.dll`（Win95 只有后者）。所有入口
 *      都用 `GetProcAddress` 取，绝不静态导入（否则 legacy 模块在 w95 上映射失败）。
 *   ② **不调用 htons/htonl/ntohs/ntohl/inet_addr**：它们在 winsock2.h 里是**导出函数**
 *      声明，直接调会生成对 ws2_32.dll 的静态导入。这里自己写字节序与点分解析。
 *   ③ 只用 Winsock 1.1 就有的 API（见 net.hpp 头注释）。
 *   ④ 每次收/发都先 `select` 等就绪，超时返回 `-2`（不把 socket 设成非阻塞，
 *      避免与上层「读满一个请求」的循环互相干扰）。
 * ========================================================================== */
#include "net.hpp"

#include <winsock2.h> /* ❗必须排在 windows.h 之前（winsock2 的类型定义顺序要求） */
#include <windows.h>
#include <string.h>

namespace autodbg::common
{
namespace
{
/* ------------------------- 函数表 ------------------------- */
struct ws_api
{
    HMODULE  mod  = nullptr;
    const char *name = nullptr;

    int(WSAAPI *startup)(WORD, LPWSADATA)                          = nullptr;
    int(WSAAPI *cleanup)(void)                                     = nullptr;
    SOCKET(WSAAPI *socket)(int, int, int)                          = nullptr;
    int(WSAAPI *bind)(SOCKET, const struct sockaddr *, int)        = nullptr;
    int(WSAAPI *listen)(SOCKET, int)                               = nullptr;
    SOCKET(WSAAPI *accept)(SOCKET, struct sockaddr *, int *)       = nullptr;
    int(WSAAPI *recv)(SOCKET, char *, int, int)                    = nullptr;
    int(WSAAPI *send)(SOCKET, const char *, int, int)              = nullptr;
    int(WSAAPI *select)(int, fd_set *, fd_set *, fd_set *, const struct timeval *) = nullptr;
    int(WSAAPI *closesocket)(SOCKET)                               = nullptr;
    int(WSAAPI *getsockname)(SOCKET, struct sockaddr *, int *)     = nullptr;
    int(WSAAPI *setsockopt)(SOCKET, int, int, const char *, int)   = nullptr;
    int(WSAAPI *ioctlsocket)(SOCKET, long, u_long *)               = nullptr;
};

ws_api g_ws;
bool   g_started = false;

/* 字节序（自己实现，见文件头 ②） */
inline unsigned short swap16(unsigned short v)
{
    return (unsigned short)((v >> 8) | (unsigned short)(v << 8));
}

inline unsigned swap32(unsigned v)
{
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

template <class FnT>
bool bind_sym(HMODULE m, const char *name, FnT *out)
{
    *out = (FnT)GetProcAddress(m, name);
    return *out != nullptr;
}

/* 载入一个 winsock 模块并解析全部入口；任一缺失即放弃该模块 */
bool load_module(const char *dll, bool try_wsa2)
{
    HMODULE m = LoadLibraryA(dll);
    if (m == nullptr)
        return false;

    ws_api a;
    a.mod  = m;
    a.name = dll;

    bool ok = bind_sym(m, "WSAStartup", &a.startup) && bind_sym(m, "WSACleanup", &a.cleanup) &&
              bind_sym(m, "socket", &a.socket) && bind_sym(m, "bind", &a.bind) &&
              bind_sym(m, "listen", &a.listen) && bind_sym(m, "accept", &a.accept) &&
              bind_sym(m, "recv", &a.recv) && bind_sym(m, "send", &a.send) &&
              bind_sym(m, "select", &a.select) && bind_sym(m, "closesocket", &a.closesocket) &&
              bind_sym(m, "getsockname", &a.getsockname) && bind_sym(m, "setsockopt", &a.setsockopt) &&
              bind_sym(m, "ioctlsocket", &a.ioctlsocket);
    if (!ok)
    {
        FreeLibrary(m);
        return false;
    }

    WSADATA wsa;
    memset(&wsa, 0, sizeof wsa);
    WORD want = try_wsa2 ? MAKEWORD(2, 2) : MAKEWORD(1, 1);
    if (a.startup(want, &wsa) != 0)
    {
        /* 2.2 不行时退回 1.1（部分老 win95 的 ws2_32 只认 1.1） */
        if (try_wsa2 && a.startup(MAKEWORD(1, 1), &wsa) != 0)
        {
            FreeLibrary(m);
            return false;
        }
        if (!try_wsa2)
        {
            FreeLibrary(m);
            return false;
        }
    }

    g_ws = a;
    return true;
}
} /* namespace */

/* ------------------------- 生命周期 ------------------------- */
net_status net_startup()
{
    if (g_started)
        return net_ok;

    if (load_module("ws2_32.dll", true) || load_module("wsock32.dll", false))
    {
        g_started = true;
        return net_ok;
    }
    return net_unavailable;
}

void net_cleanup()
{
    if (!g_started)
        return;
    if (g_ws.cleanup != nullptr)
        g_ws.cleanup();
    if (g_ws.mod != nullptr)
        FreeLibrary(g_ws.mod);
    memset(&g_ws, 0, sizeof g_ws);
    g_started = false;
}

const char *net_module_name()
{
    return g_started ? g_ws.name : nullptr;
}

const char *net_status_name(net_status s)
{
    switch (s)
    {
    case net_ok:             return "ok";
    case net_unavailable:    return "no-winsock";
    case net_startup_failed: return "wsa-startup-failed";
    case net_bad_addr:       return "bad-addr";
    case net_socket_failed:  return "socket-failed";
    case net_bind_failed:    return "bind-failed";
    case net_listen_failed:  return "listen-failed";
    default:                 return "unknown";
    }
}

bool net_parse_ipv4(const char *host, unsigned *out_host_order)
{
    if (host == nullptr || out_host_order == nullptr)
        return false;
    if (strcmp(host, "localhost") == 0)
    {
        *out_host_order = 0x7F000001u;
        return true;
    }

    unsigned parts[4];
    int      idx = 0;
    unsigned cur = 0;
    bool     any = false;

    for (const char *p = host;; p++)
    {
        if (*p >= '0' && *p <= '9')
        {
            cur = cur * 10 + (unsigned)(*p - '0');
            any = true;
            if (cur > 255)
                return false;
        }
        else if (*p == '.' || *p == '\0')
        {
            if (!any || idx >= 4)
                return false;
            parts[idx++] = cur;
            cur          = 0;
            any          = false;
            if (*p == '\0')
                break;
        }
        else
        {
            return false;
        }
    }
    if (idx != 4)
        return false;
    *out_host_order = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

/* ------------------------- 监听 / 接受 ------------------------- */
long long net_listen(const char *host, unsigned port, unsigned *actual_port, net_status *why)
{
    if (why != nullptr)
        *why = net_ok;
    if (actual_port != nullptr)
        *actual_port = port;

    if (!g_started)
    {
        net_status st = net_startup();
        if (st != net_ok)
        {
            if (why != nullptr)
                *why = st;
            return -1;
        }
    }

    unsigned host_order = 0;
    if (!net_parse_ipv4((host != nullptr) ? host : "127.0.0.1", &host_order))
    {
        if (why != nullptr)
            *why = net_bad_addr;
        return -1;
    }

    SOCKET s = g_ws.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        if (why != nullptr)
            *why = net_socket_failed;
        return -1;
    }

    BOOL yes = TRUE;
    g_ws.setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, (int)sizeof yes);

    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = swap16((unsigned short)port);
    sa.sin_addr.s_addr = swap32(host_order);

    if (g_ws.bind(s, (const struct sockaddr *)&sa, (int)sizeof sa) != 0)
    {
        g_ws.closesocket(s);
        if (why != nullptr)
            *why = net_bind_failed;
        return -1;
    }
    if (g_ws.listen(s, 16) != 0)
    {
        g_ws.closesocket(s);
        if (why != nullptr)
            *why = net_listen_failed;
        return -1;
    }

    if (actual_port != nullptr)
    {
        sockaddr_in got;
        int         len = (int)sizeof got;
        memset(&got, 0, sizeof got);
        if (g_ws.getsockname(s, (struct sockaddr *)&got, &len) == 0)
            *actual_port = (unsigned)swap16(got.sin_port);
        else
            *actual_port = port;
    }

    return (long long)s;
}

long long net_accept(long long listener, int timeout_ms, unsigned *peer_ip)
{
    if (!g_started || listener < 0)
        return -1;

    SOCKET s = (SOCKET)listener;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);

    timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);

    int r = g_ws.select(0, &rd, nullptr, nullptr, (timeout_ms < 0) ? nullptr : &tv);
    if (r == 0)
        return 0; /* 超时：没连进来 */
    if (r < 0)
        return -1;

    sockaddr_in peer;
    int         len = (int)sizeof peer;
    memset(&peer, 0, sizeof peer);

    SOCKET c = g_ws.accept(s, (struct sockaddr *)&peer, &len);
    if (c == INVALID_SOCKET)
        return -1;

    if (peer_ip != nullptr)
        *peer_ip = swap32((unsigned)peer.sin_addr.s_addr);

    return (long long)c;
}

void net_close(long long sock)
{
    if (!g_started || sock < 0)
        return;
    g_ws.closesocket((SOCKET)sock);
}

/* ------------------------- 收发 ------------------------- */
namespace
{
/* select 等可读/可写：1 = 就绪，0 = 超时，-1 = 错误 */
int wait_fd(SOCKET s, bool want_write, int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);

    timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);

    const timeval *ptv = (timeout_ms < 0) ? nullptr : &tv;
    int r = want_write ? g_ws.select(0, nullptr, &set, nullptr, ptv)
                       : g_ws.select(0, &set, nullptr, nullptr, ptv);
    if (r == 0)
        return 0;
    if (r < 0)
        return -1;
    return 1;
}
} /* namespace */

int net_recv(long long sock, void *buf, size_t cap, int timeout_ms)
{
    if (!g_started || sock < 0 || buf == nullptr || cap == 0)
        return -1;

    SOCKET s = (SOCKET)sock;
    int    ready = wait_fd(s, false, timeout_ms);
    if (ready == 0)
        return -2;
    if (ready < 0)
        return -1;

    int want = (cap > 0x40000000u) ? 0x40000000 : (int)cap;
    int r    = g_ws.recv(s, (char *)buf, want, 0);
    if (r == 0)
        return 0; /* 对端正常关闭 */
    if (r < 0)
        return -1;
    return r;
}

bool net_send_all(long long sock, const void *data, size_t len, int timeout_ms)
{
    if (!g_started || sock < 0 || (data == nullptr && len != 0))
        return false;

    SOCKET             s = (SOCKET)sock;
    const char *p = (const char *)data;

    while (len > 0)
    {
        int ready = wait_fd(s, true, timeout_ms);
        if (ready != 1)
            return false;

        int   want = (len > 0x40000000u) ? 0x40000000 : (int)len;
        int   r    = g_ws.send(s, p, want, 0);
        if (r <= 0)
            return false;
        p += r;
        len -= (size_t)r;
    }
    return true;
}
} /* namespace autodbg::common */
