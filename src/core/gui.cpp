/* =============================================================================
 * src/core/gui.cpp —— GUI 自动化原语实现（S4）
 *
 * 来源：新写（plan_server.md S4 / §7）。
 * 与旧实现的差异：无（新文件）。
 * 实现要点：
 *   ① 新 API 一律 dynapi（`SendInput` 98+、`PrintWindow` XP+、`BlockInput` 98+、
 *      `SetProcessDPIAware` Vista+），取不到就走 Win95 就有的等价实现或返回 false；
 *   ② 截图用 `CreateDIBSection` + 负高度（top-down），出来的就是 BGRA 连续布局，
 *      直接交给 `common::png_writer`；
 *   ③ 鼠标/键盘优先 `SendInput`，退 `mouse_event` / `keybd_event`（Win95 有）；
 *   ④ 枚举用模块级静态上下文（模块是单线程执行，不需要锁）。
 * ========================================================================== */
#include "gui.hpp"

#include "common/allocator.hpp"
#include "common/dynapi.hpp"

#include <string.h>

namespace autodbg::core::gui
{
namespace
{
/* ------------------------------ dynapi 缓存 ------------------------------ */
typedef UINT(WINAPI *send_input_fn)(UINT, LPINPUT, int);
typedef BOOL(WINAPI *block_input_fn)(BOOL);
typedef BOOL(WINAPI *set_dpi_aware_fn)();
typedef BOOL(WINAPI *print_window_fn)(HWND, HDC, UINT);

send_input_fn get_send_input()
{
    /* ❗不能用「函数内静态对象 + 动态初始化」（`static send_input_fn fn = get_proc_as(...)`）：
     * MSVC 会为它生成线程安全初始化（`_Init_thread_*`）→ 引用 `_tls_index`，
     * 而手工映射的模块里没有 TLS 目录 → LNK2019（S2/S4 各踩过一次）。
     * 这里用命名空间作用域的**常量初始化**变量 + 惰性标志。 */
    static bool checked = false;
    static send_input_fn fn = nullptr;
    if (!checked)
    {
        fn      = common::get_proc_as<send_input_fn>("user32.dll", "SendInput");
        checked = true;
    }
    return fn;
}

/* 大小写不敏感的子串匹配（TCHAR；自己实现，避免依赖 shlwapi 的宏映射） */
bool contains_ci(LPCTSTR hay, LPCTSTR needle)
{
    if (hay == nullptr || needle == nullptr)
        return false;
    size_t hn = 0, nn = 0;
    while (hay[hn] != 0)
        hn++;
    while (needle[nn] != 0)
        nn++;
    if (nn == 0)
        return true;
    if (nn > hn)
        return false;

    for (size_t i = 0; i + nn <= hn; i++)
    {
        size_t k = 0;
        for (; k < nn; k++)
        {
            TCHAR a = hay[i + k];
            TCHAR b = needle[k];
            if (a >= 'A' && a <= 'Z')
                a = (TCHAR)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (TCHAR)(b + 32);
            if (a != b)
                break;
        }
        if (k == nn)
            return true;
    }
    return false;
}

void copy_tstr(TCHAR *dst, size_t cap, LPCTSTR src)
{
    if (dst == nullptr || cap == 0)
        return;
    size_t n = 0;
    if (src != nullptr)
    {
        while (src[n] != 0 && n + 1 < cap)
        {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = 0;
}

bool fill_window_info(HWND hwnd, window_info *wi)
{
    if (hwnd == nullptr || wi == nullptr)
        return false;
    RECT r;
    if (!GetWindowRect(hwnd, &r))
        return false;

    wi->hwnd      = hwnd;
    wi->pid       = 0;
    GetWindowThreadProcessId(hwnd, &wi->pid);
    wi->x         = r.left;
    wi->y         = r.top;
    wi->w         = r.right - r.left;
    wi->h         = r.bottom - r.top;
    wi->visible   = IsWindowVisible(hwnd) != FALSE;
    wi->minimized = IsIconic(hwnd) != FALSE;
    wi->enabled   = IsWindowEnabled(hwnd) != FALSE;
    GetWindowText(hwnd, wi->title, (int)(sizeof wi->title / sizeof wi->title[0]));
    GetClassName(hwnd, wi->cls, (int)(sizeof wi->cls / sizeof wi->cls[0]));
    return true;
}

/* ------------------------------ 输入注入 ------------------------------ */
bool send_mouse(DWORD flags, int dx, int dy, int data)
{
    send_input_fn send_input = get_send_input();
    if (send_input != nullptr)
    {
        INPUT in;
        memset(&in, 0, sizeof in);
        in.type       = INPUT_MOUSE;
        in.mi.dwFlags = flags;
        in.mi.dx      = dx;
        in.mi.dy      = dy;
        in.mi.mouseData = (DWORD)data;
        return send_input(1, &in, (int)sizeof in) == 1;
    }
    mouse_event(flags, (DWORD)dx, (DWORD)dy, (DWORD)data, 0);
    return true;
}

bool send_key(unsigned vk, bool down)
{
    send_input_fn send_input = get_send_input();
    if (send_input != nullptr)
    {
        INPUT in;
        memset(&in, 0, sizeof in);
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = (WORD)vk;
        in.ki.wScan   = (WORD)MapVirtualKey(vk, 0);
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        return send_input(1, &in, (int)sizeof in) == 1;
    }
    keybd_event((BYTE)vk, (BYTE)MapVirtualKey(vk, 0), down ? 0 : KEYEVENTF_KEYUP, 0);
    return true;
}

bool send_unicode_unit(unsigned unit)
{
    send_input_fn send_input = get_send_input();
    if (send_input == nullptr)
        return false;
    INPUT down;
    memset(&down, 0, sizeof down);
    down.type       = INPUT_KEYBOARD;
    down.ki.wScan   = (WORD)(unit & 0xFFFFu);
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    INPUT up = down;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    INPUT both[2] = { down, up };
    return send_input(2, both, (int)sizeof(INPUT)) == 2;
}

/* ------------------------------ 截图公共部分 ------------------------------ */
/* 建一个 top-down 的 32bpp DIB，返回 bits（由 release 时一起删） */
bool make_dib(HDC ref, int w, int h, HBITMAP *bmp, void **bits)
{
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *p = nullptr;
    HBITMAP hb = CreateDIBSection(ref, &bmi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (hb == nullptr || p == nullptr)
        return false;
    *bmp  = hb;
    *bits = p;
    return true;
}

bool copy_bits(const void *src, int w, int h, unsigned char **out, int *out_w, int *out_h, int *out_stride)
{
    size_t stride = (size_t)w * 4;
    size_t len    = stride * (size_t)h;
    unsigned char *buf = (unsigned char *)common::alloc(len);
    if (buf == nullptr)
        return false;
    memcpy(buf, src, len);
    *out        = buf;
    *out_w      = w;
    *out_h      = h;
    *out_stride = (int)stride;
    return true;
}
} /* namespace */

/* ------------------------------ 屏幕 ------------------------------ */
bool screen_size(int *w, int *h)
{
    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);
    if (cx <= 0 || cy <= 0)
        return false;
    if (w != nullptr)
        *w = cx;
    if (h != nullptr)
        *h = cy;
    return true;
}

bool pixel_color(int x, int y, unsigned char *r, unsigned char *g, unsigned char *b)
{
    HDC dc = GetDC(nullptr);
    if (dc == nullptr)
        return false;
    COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID)
        return false;
    if (r != nullptr)
        *r = (unsigned char)(c & 0xFF);
    if (g != nullptr)
        *g = (unsigned char)((c >> 8) & 0xFF);
    if (b != nullptr)
        *b = (unsigned char)((c >> 16) & 0xFF);
    return true;
}

bool capture_region(int x, int y, int w, int h, double scale,
                    unsigned char **out, int *out_w, int *out_h, int *out_stride)
{
    if (out == nullptr || w <= 0 || h <= 0)
        return false;
    if (scale <= 0.0 || scale > 8.0)
        scale = 1.0;

    int outw = (int)((double)w * scale + 0.5);
    int outh = (int)((double)h * scale + 0.5);
    if (outw < 1)
        outw = 1;
    if (outh < 1)
        outh = 1;

    HDC screen = GetDC(nullptr);
    if (screen == nullptr)
        return false;
    HDC     mem = CreateCompatibleDC(screen);
    HBITMAP dib = nullptr;
    void   *bits = nullptr;
    bool    ok   = false;

    if (mem != nullptr && make_dib(screen, outw, outh, &dib, &bits))
    {
        HGDIOBJ old = SelectObject(mem, dib);
        BOOL drawn;
        if (outw == w && outh == h)
        {
            drawn = BitBlt(mem, 0, 0, outw, outh, screen, x, y, SRCCOPY);
        }
        else
        {
            SetStretchBltMode(mem, HALFTONE);
            drawn = StretchBlt(mem, 0, 0, outw, outh, screen, x, y, w, h, SRCCOPY);
        }
        GdiFlush();
        if (drawn)
            ok = copy_bits(bits, outw, outh, out, out_w, out_h, out_stride);
        SelectObject(mem, old);
        DeleteObject(dib);
    }

    if (mem != nullptr)
        DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

bool capture_window(HWND hwnd, double scale,
                    unsigned char **out, int *out_w, int *out_h, int *out_stride)
{
    if (!IsWindow(hwnd))
        return false;

    RECT r;
    if (!GetWindowRect(hwnd, &r))
        return false;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return false;

    /* 首选 PrintWindow（能拍到被遮挡的窗口；XP+，dynapi） */
    print_window_fn print_window = common::get_proc_as<print_window_fn>("user32.dll", "PrintWindow");
    if (print_window != nullptr)
    {
        HDC     screen = GetDC(nullptr);
        HBITMAP dib    = nullptr;
        void   *bits   = nullptr;
        bool    ok     = false;
        if (screen != nullptr && make_dib(screen, w, h, &dib, &bits))
        {
            HDC     mem = CreateCompatibleDC(screen);
            HGDIOBJ old = SelectObject(mem, dib);
            GdiFlush();
            if (print_window(hwnd, mem, 0))
            {
                GdiFlush();
                ok = copy_bits(bits, w, h, out, out_w, out_h, out_stride);
            }
            SelectObject(mem, old);
            DeleteDC(mem);
            DeleteObject(dib);
        }
        if (screen != nullptr)
            ReleaseDC(nullptr, screen);
        if (ok)
            return true;
        /* 否则落到屏幕截取 */
    }

    return capture_region(r.left, r.top, w, h, scale, out, out_w, out_h, out_stride);
}

void release_capture(unsigned char *p)
{
    if (p != nullptr)
        common::release(p);
}

/* ------------------------------ 鼠标 ------------------------------ */
bool cursor_position(int *x, int *y)
{
    POINT pt;
    if (!GetCursorPos(&pt))
        return false;
    if (x != nullptr)
        *x = pt.x;
    if (y != nullptr)
        *y = pt.y;
    return true;
}

bool mouse_move(int x, int y)
{
    return SetCursorPos(x, y) != FALSE;
}

bool mouse_button(int button, bool down)
{
    DWORD flags = 0;
    switch (button)
    {
    case 1: flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case 2: flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case 3: flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    default: return false;
    }
    return send_mouse(flags, 0, 0, 0);
}

bool mouse_wheel(int delta)
{
    return send_mouse(MOUSEEVENTF_WHEEL, 0, 0, delta);
}

bool mouse_drag(int x, int y, int button, unsigned duration_ms)
{
    int sx = 0, sy = 0;
    if (!cursor_position(&sx, &sy))
        return false;
    if (!mouse_button(button, true))
        return false;

    const int steps = 8;
    for (int i = 1; i <= steps; i++)
    {
        int nx = sx + (x - sx) * i / steps;
        int ny = sy + (y - sy) * i / steps;
        mouse_move(nx, ny);
        if (duration_ms > 0)
            Sleep(duration_ms / steps > 0 ? duration_ms / steps : 1);
    }
    mouse_move(x, y);
    return mouse_button(button, false);
}

/* ------------------------------ 键盘 ------------------------------ */
bool vk_from_name(const char *name, unsigned *vk)
{
    if (name == nullptr || vk == nullptr || name[0] == '\0')
        return false;

    /* 单字符：字母 / 数字 / 常见符号（美国布局的 VK） */
    if (name[1] == '\0')
    {
        char c = name[0];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        {
            *vk = (unsigned)c;
            return true;
        }
        switch (c)
        {
        case ' ': *vk = VK_SPACE; return true;
        case '+': *vk = VK_OEM_PLUS; return true;
        case '-': *vk = VK_OEM_MINUS; return true;
        case ',': *vk = VK_OEM_COMMA; return true;
        case '.': *vk = VK_OEM_PERIOD; return true;
        case '/': *vk = VK_OEM_2; return true;
        case ';': *vk = VK_OEM_1; return true;
        case '\'': *vk = VK_OEM_7; return true;
        case '[': *vk = VK_OEM_4; return true;
        case ']': *vk = VK_OEM_6; return true;
        case '\\': *vk = VK_OEM_5; return true;
        case '`': *vk = VK_OEM_3; return true;
        case '=': *vk = VK_OEM_PLUS; return true;
        default: return false;
        }
    }

    struct named
    {
        const char *name;
        unsigned    vk;
    };
    static const named k_names[] = {
        { "enter", VK_RETURN },   { "return", VK_RETURN },   { "tab", VK_TAB },
        { "esc", VK_ESCAPE },     { "escape", VK_ESCAPE },   { "space", VK_SPACE },
        { "backspace", VK_BACK }, { "back", VK_BACK },       { "delete", VK_DELETE },
        { "del", VK_DELETE },     { "insert", VK_INSERT },   { "ins", VK_INSERT },
        { "home", VK_HOME },      { "end", VK_END },         { "pageup", VK_PRIOR },
        { "pgup", VK_PRIOR },     { "pagedown", VK_NEXT },   { "pgdn", VK_NEXT },
        { "up", VK_UP },          { "down", VK_DOWN },       { "left", VK_LEFT },
        { "right", VK_RIGHT },    { "shift", VK_SHIFT },     { "ctrl", VK_CONTROL },
        { "control", VK_CONTROL }, { "alt", VK_MENU },       { "menu", VK_MENU },
        { "win", VK_LWIN },       { "capslock", VK_CAPITAL }, { "numlock", VK_NUMLOCK },
        { "scrolllock", VK_SCROLL }, { "pause", VK_PAUSE },  { "printscreen", VK_SNAPSHOT },
        { "apps", VK_APPS },      { "numpad0", VK_NUMPAD0 }, { "numpad1", VK_NUMPAD1 },
        { "numpad2", VK_NUMPAD2 }, { "numpad3", VK_NUMPAD3 }, { "numpad4", VK_NUMPAD4 },
        { "numpad5", VK_NUMPAD5 }, { "numpad6", VK_NUMPAD6 }, { "numpad7", VK_NUMPAD7 },
        { "numpad8", VK_NUMPAD8 }, { "numpad9", VK_NUMPAD9 },
    };
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
    {
        if (strcmp(name, k_names[i].name) == 0)
        {
            *vk = k_names[i].vk;
            return true;
        }
    }

    /* f1..f24 */
    if ((name[0] == 'f' || name[0] == 'F') && name[1] >= '0' && name[1] <= '9')
    {
        unsigned n = 0;
        for (const char *p = name + 1; *p != '\0'; p++)
        {
            if (*p < '0' || *p > '9')
                return false;
            n = n * 10 + (unsigned)(*p - '0');
        }
        if (n >= 1 && n <= 24)
        {
            *vk = VK_F1 + (n - 1);
            return true;
        }
    }
    return false;
}

bool key_press(unsigned vk, bool down)
{
    return send_key(vk, down);
}

bool key_tap(unsigned vk)
{
    if (!send_key(vk, true))
        return false;
    return send_key(vk, false);
}

bool type_codepoint(unsigned codepoint)
{
    /* ① 可打印 ASCII 先走「VK + Shift」路径：它产生的就是真实键盘的 WM_KEYDOWN，
     *    兼容性最好（S4 实测：WinForms 文本框会忽略 `KEYEVENTF_UNICODE` 的 VK_PACKET，
     *    但对 VK 路径正常响应）。pyautogui 也是这个做法。 */
    if (codepoint >= 0x20 && codepoint < 0x7F)
    {
        SHORT scan = VkKeyScanA((CHAR)codepoint);
        if (scan != -1)
        {
            unsigned vk      = (unsigned)(scan & 0xFF);
            bool     shifted = (scan & 0x100) != 0;
            if (shifted)
                send_key(VK_SHIFT, true);
            bool ok = key_tap(vk);
            if (shifted)
                send_key(VK_SHIFT, false);
            return ok;
        }
    }

    /* ② 其余（非 ASCII、控制字符等）→ Unicode 注入（98+） */
    if (get_send_input() != nullptr)
        return send_unicode_unit(codepoint);
    return false;
}

void sleep_ms(unsigned ms)
{
    Sleep(ms);
}

/* ------------------------------ 窗口 ------------------------------ */
namespace
{
struct enum_ctx
{
    window_info *out;
    unsigned     cap;
    unsigned     written;
    unsigned     total;
};

BOOL CALLBACK enum_top_proc(HWND hwnd, LPARAM lp)
{
    enum_ctx *ctx = (enum_ctx *)lp;
    ctx->total++;
    if (ctx->written < ctx->cap && fill_window_info(hwnd, &ctx->out[ctx->written]))
        ctx->written++;
    return TRUE;
}

struct find_ctx
{
    const window_match *m;
    HWND                found;
};

bool match_info(const window_info &wi, const window_match &m)
{
    if (m.pid != 0 && wi.pid != m.pid)
        return false;
    if (m.title_sub != nullptr && !contains_ci(wi.title, m.title_sub))
        return false;
    if (m.class_sub != nullptr && !contains_ci(wi.cls, m.class_sub))
        return false;
    return true;
}

BOOL CALLBACK enum_find_proc(HWND hwnd, LPARAM lp)
{
    find_ctx *ctx = (find_ctx *)lp;
    if (!IsWindowVisible(hwnd))
        return TRUE; /* 找窗口时跳过不可见的（不可见的仍可用 gui.windows() 拿到） */

    window_info wi;
    if (fill_window_info(hwnd, &wi) && match_info(wi, *ctx->m))
    {
        ctx->found = hwnd;
        return FALSE; /* 找到就停 */
    }
    return TRUE;
}
} /* namespace */

unsigned enum_windows(window_info *out, unsigned cap)
{
    enum_ctx ctx;
    ctx.out     = out;
    ctx.cap     = (out != nullptr) ? cap : 0;
    ctx.written = 0;
    ctx.total   = 0;
    EnumWindows(enum_top_proc, (LPARAM)&ctx);
    return ctx.written;
}

unsigned enum_children(HWND parent, window_info *out, unsigned cap)
{
    enum_ctx ctx;
    ctx.out     = out;
    ctx.cap     = (out != nullptr) ? cap : 0;
    ctx.written = 0;
    ctx.total   = 0;
    EnumChildWindows(parent, enum_top_proc, (LPARAM)&ctx);
    return ctx.written;
}

HWND window_active()
{
    return GetForegroundWindow();
}

bool window_is_valid(HWND hwnd)
{
    return hwnd != nullptr && IsWindow(hwnd) != FALSE;
}

DWORD window_pid(HWND hwnd)
{
    DWORD pid = 0;
    if (hwnd != nullptr)
        GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

bool window_rect(HWND hwnd, int *x, int *y, int *w, int *h)
{
    RECT r;
    if (hwnd == nullptr || !GetWindowRect(hwnd, &r))
        return false;
    if (x != nullptr)
        *x = r.left;
    if (y != nullptr)
        *y = r.top;
    if (w != nullptr)
        *w = r.right - r.left;
    if (h != nullptr)
        *h = r.bottom - r.top;
    return true;
}

bool window_set_rect(HWND hwnd, int x, int y, int w, int h)
{
    if (hwnd == nullptr)
        return false;
    return MoveWindow(hwnd, x, y, w, h, TRUE) != FALSE;
}

bool window_show(HWND hwnd, const char *cmd)
{
    if (hwnd == nullptr || cmd == nullptr)
        return false;
    int show = -1;
    if (strcmp(cmd, "min") == 0 || strcmp(cmd, "minimize") == 0)
        show = SW_MINIMIZE;
    else if (strcmp(cmd, "max") == 0 || strcmp(cmd, "maximize") == 0)
        show = SW_MAXIMIZE;
    else if (strcmp(cmd, "restore") == 0 || strcmp(cmd, "normal") == 0)
        show = SW_RESTORE;
    else if (strcmp(cmd, "hide") == 0)
        show = SW_HIDE;
    else if (strcmp(cmd, "show") == 0)
        show = SW_SHOW;
    if (show < 0)
        return false;
    ShowWindow(hwnd, show);
    return true;
}

bool window_close(HWND hwnd)
{
    if (hwnd == nullptr)
        return false;
    return PostMessage(hwnd, WM_CLOSE, 0, 0) != FALSE;
}

bool window_activate(HWND hwnd)
{
    if (hwnd == nullptr || !IsWindow(hwnd))
        return false;
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    if (SetForegroundWindow(hwnd))
        return true;

    /* 前台锁兜底：AttachThreadInput 到当前前台线程再抢（Win95 就有） */
    DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD my_thread = GetCurrentThreadId();
    bool  ok        = false;
    if (fg_thread != 0 && fg_thread != my_thread && AttachThreadInput(my_thread, fg_thread, TRUE))
    {
        BringWindowToTop(hwnd);
        ok = SetForegroundWindow(hwnd) != FALSE;
        AttachThreadInput(my_thread, fg_thread, FALSE);
    }
    return ok;
}

bool window_focus(HWND hwnd)
{
    if (hwnd == nullptr || !IsWindow(hwnd))
        return false;

    DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
    DWORD my_thread     = GetCurrentThreadId();
    if (target_thread == 0)
        return false;

    if (target_thread == my_thread)
        return SetFocus(hwnd) != nullptr || GetFocus() == hwnd;

    if (!AttachThreadInput(my_thread, target_thread, TRUE))
        return false;
    SetFocus(hwnd);
    bool ok = (GetFocus() == hwnd);
    AttachThreadInput(my_thread, target_thread, FALSE);
    return ok;
}

bool window_text(HWND hwnd, TCHAR *out, size_t cap)
{
    if (hwnd == nullptr || out == nullptr || cap == 0)
        return false;
    out[0] = 0;
    GetWindowText(hwnd, out, (int)cap);
    return true;
}

bool window_set_text(HWND hwnd, LPCTSTR text)
{
    if (hwnd == nullptr)
        return false;
    return SetWindowText(hwnd, (text != nullptr) ? text : TEXT("")) != FALSE;
}

unsigned window_foreground_pid()
{
    HWND  hwnd = GetForegroundWindow();
    DWORD pid  = 0;
    if (hwnd != nullptr)
        GetWindowThreadProcessId(hwnd, &pid);
    return (unsigned)pid;
}

bool window_center(HWND hwnd, int *x, int *y)
{
    RECT r;
    if (hwnd == nullptr || !GetWindowRect(hwnd, &r))
        return false;
    if (x != nullptr)
        *x = r.left + (r.right - r.left) / 2;
    if (y != nullptr)
        *y = r.top + (r.bottom - r.top) / 2;
    return true;
}

bool window_post(HWND hwnd, unsigned msg, unsigned wparam, unsigned lparam)
{
    if (hwnd == nullptr)
        return false;
    return PostMessage(hwnd, (UINT)msg, (WPARAM)wparam, (LPARAM)lparam) != FALSE;
}

bool window_send(HWND hwnd, unsigned msg, unsigned wparam, unsigned lparam, size_t *result)
{
    if (hwnd == nullptr)
        return false;
    LRESULT r = SendMessage(hwnd, (UINT)msg, (WPARAM)wparam, (LPARAM)lparam);
    if (result != nullptr)
        *result = (size_t)r;
    return true;
}

int window_ctrl_id(HWND hwnd)
{
    if (hwnd == nullptr)
        return -1;
    return GetDlgCtrlID(hwnd);
}

HWND find_window(const window_match &m)
{
    find_ctx ctx;
    ctx.m     = &m;
    ctx.found = nullptr;
    EnumWindows(enum_find_proc, (LPARAM)&ctx);
    return ctx.found;
}

/* ------------------------------ 增强 ------------------------------ */
bool set_dpi_aware()
{
    set_dpi_aware_fn fn = common::get_proc_as<set_dpi_aware_fn>("user32.dll", "SetProcessDPIAware");
    if (fn == nullptr)
        return false;
    return fn() != FALSE;
}

bool set_cursor_locked(bool lock)
{
    block_input_fn fn = common::get_proc_as<block_input_fn>("user32.dll", "BlockInput");
    if (fn == nullptr)
        return false;
    return fn(lock ? TRUE : FALSE) != FALSE;
}
} /* namespace autodbg::core::gui */
