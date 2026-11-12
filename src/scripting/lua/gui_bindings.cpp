/* =============================================================================
 * src/scripting/lua/gui_bindings.cpp —— `gui` 表实现（S4）
 *
 * 来源：新写（plan_server.md S4 / §7.1）。
 * 与旧实现的差异：无（新文件）。
 * 约定（写进 LuaLS 模板，见 state.cpp）：
 *   · 坐标一律**屏幕绝对像素**；HWND 用整数表示；
 *   · 失败返回 `nil, errmsg`，不抛异常；
 *   · `screenshot` 返回 `{ w, h, format, data }`，`data` 是二进制字符串
 *     （`png` / `bmp` / `raw`=BGRA）；`screenshot_to` 直接写文件；
 *   · 非 ASCII 文本走 SendInput 的 Unicode 路径（98+）；老系统只支持能映射到 VK 的字符。
 * ========================================================================== */
#include "gui_bindings.hpp"

#include "output.hpp"   /* T1（D50）：`images` 登记表 */

#include "core/gui.hpp"
#include "core/util.hpp" /* TCHAR / alloc_ / free_ 宏 */

#include "common/allocator.hpp"
#include "common/png_writer.hpp"

#include <stdio.h>
#include <string.h>

using namespace autodbg;

namespace
{
constexpr unsigned k_max_enum_windows = 1024;

HWND to_hwnd(lua_State *L, int idx)
{
    lua_Integer v = luaL_checkinteger(L, idx);
    return (HWND)(size_t)v;
}

void push_hwnd(lua_State *L, HWND hwnd)
{
    lua_pushinteger(L, (lua_Integer)(size_t)hwnd);
}

/* TCHAR → Lua 字符串（UNICODE 构建转 UTF-8；ANSI 构建原样，与 legacy 整体口径一致） */
void push_tstr(lua_State *L, LPCTSTR s)
{
#ifdef _UNICODE
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
    {
        lua_pushstring(L, "");
        return;
    }
    LPSTR b = (LPSTR)alloc_((size_t)len);
    if (b == nullptr)
    {
        lua_pushstring(L, "");
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, s, -1, b, len, nullptr, nullptr);
    lua_pushstring(L, b);
    free_(b);
#else
    lua_pushstring(L, s);
#endif
}

/* Lua 字符串（UTF-8）→ TCHAR */
bool str_to_tstr(lua_State *L, int idx, TCHAR *out, size_t cap)
{
    size_t      n = 0;
    const char *s = luaL_checklstring(L, idx, &n);
    if (out == nullptr || cap == 0)
        return false;
#ifdef _UNICODE
    int w = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, nullptr, 0);
    if (w <= 0 || (size_t)w + 1 > cap)
        return false;
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, out, w);
    out[w] = 0;
#else
    if (n + 1 > cap)
        return false;
    memcpy(out, s, n);
    out[n] = 0;
#endif
    return true;
}

int fail(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

int ok_bool(lua_State *L, bool ok)
{
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/* 把 window_info 压成表 */
void push_window(lua_State *L, const core::gui::window_info &wi)
{
    lua_createtable(L, 0, 10);
    lua_pushinteger(L, (lua_Integer)(size_t)wi.hwnd);
    lua_setfield(L, -2, "hwnd");
    lua_pushinteger(L, (lua_Integer)wi.pid);
    lua_setfield(L, -2, "pid");
    push_tstr(L, wi.title);
    lua_setfield(L, -2, "title");
    /* `text` 是 `title` 的别名：对**子控件**而言窗口文本就是它的标签（按钮/标签/编辑框），
     * 用同名别名可以让脚本不必区分「顶层窗口的标题」与「控件的文本」（S4 踩过） */
    push_tstr(L, wi.title);
    lua_setfield(L, -2, "text");
    push_tstr(L, wi.cls);
    lua_setfield(L, -2, "class");
    lua_pushinteger(L, wi.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, wi.y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, wi.w);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, wi.h);
    lua_setfield(L, -2, "h");
    lua_pushboolean(L, wi.visible ? 1 : 0);
    lua_setfield(L, -2, "visible");
    lua_pushboolean(L, wi.enabled ? 1 : 0);
    lua_setfield(L, -2, "enabled");
}

/* ------------------------------ 屏幕 ------------------------------ */
int lua_gui_size(lua_State *L)
{
    int w = 0, h = 0;
    if (!core::gui::screen_size(&w, &h))
        return fail(L, "screen_size failed");
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

int lua_gui_pixel(lua_State *L)
{
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    unsigned char r = 0, g = 0, b = 0;
    if (!core::gui::pixel_color(x, y, &r, &g, &b))
        return fail(L, "pixel_color failed");
    lua_pushinteger(L, r);
    lua_pushinteger(L, g);
    lua_pushinteger(L, b);
    return 3;
}

/* 解析截图 opts 表 → 共享参数 */
struct shot_opts
{
    int         x = 0, y = 0, w = 0, h = 0;
    bool        has_rect = false;
    HWND        hwnd     = nullptr;
    double      scale    = 1.0;
    const char *format   = "png";
};

void parse_shot_opts(lua_State *L, int idx, shot_opts *o)
{
    if (idx < 0 || !lua_istable(L, idx))
        return;

    lua_getfield(L, idx, "x");
    lua_getfield(L, idx, "y");
    lua_getfield(L, idx, "w");
    lua_getfield(L, idx, "h");
    if (lua_isnumber(L, -4) && lua_isnumber(L, -3) && lua_isnumber(L, -2) && lua_isnumber(L, -1))
    {
        o->x        = (int)lua_tointeger(L, -4);
        o->y        = (int)lua_tointeger(L, -3);
        o->w        = (int)lua_tointeger(L, -2);
        o->h        = (int)lua_tointeger(L, -1);
        o->has_rect = (o->w > 0 && o->h > 0);
    }
    lua_pop(L, 4);

    lua_getfield(L, idx, "hwnd");
    if (lua_isnumber(L, -1))
        o->hwnd = (HWND)(size_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "scale");
    if (lua_isnumber(L, -1))
        o->scale = lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "format");
    if (lua_isstring(L, -1))
        o->format = lua_tostring(L, -1);
    lua_pop(L, 1);

    if (o->hwnd == nullptr && !o->has_rect)
    {
        /* 默认：整个主屏 */
        int sw = 0, sh = 0;
        if (core::gui::screen_size(&sw, &sh))
        {
            o->x        = 0;
            o->y        = 0;
            o->w        = sw;
            o->h        = sh;
            o->has_rect = true;
        }
    }
}

/* 截图并编码；成功返回 true 并给出数据（*out 需 release） */
bool take_shot(const shot_opts &o, unsigned char **data, size_t *len, int *w, int *h, const char **format)
{
    unsigned char *bgra   = nullptr;
    int            cw = 0, ch = 0, stride = 0;

    bool got = false;
    if (o.hwnd != nullptr)
        got = core::gui::capture_window(o.hwnd, o.scale, &bgra, &cw, &ch, &stride);
    else
        got = core::gui::capture_region(o.x, o.y, o.w, o.h, o.scale, &bgra, &cw, &ch, &stride);
    if (!got || bgra == nullptr)
        return false;

    unsigned char *enc = nullptr;
    size_t         n   = 0;
    const char    *fmt = o.format;

    if (strcmp(fmt, "png") == 0)
    {
        n = common::png_encode_bgra(bgra, (unsigned)cw, (unsigned)ch, (unsigned)stride, &enc);
    }
    else if (strcmp(fmt, "bmp") == 0)
    {
        n = common::bmp_encode_bgra(bgra, (unsigned)cw, (unsigned)ch, (unsigned)stride, &enc);
    }
    else if (strcmp(fmt, "raw") == 0)
    {
        enc = bgra; /* 直接把 BGRA 交出去 */
        n   = (size_t)stride * (size_t)ch;
        bgra = nullptr;
    }
    else
    {
        core::gui::release_capture(bgra);
        return false;
    }

    if (bgra != nullptr)
        core::gui::release_capture(bgra);
    if (n == 0 || enc == nullptr)
    {
        if (enc != nullptr)
            common::release(enc);
        return false;
    }

    *data   = enc;
    *len    = n;
    *w      = cw;
    *h      = ch;
    *format = fmt;
    return true;
}

int lua_gui_screenshot(lua_State *L)
{
    shot_opts o;
    parse_shot_opts(L, 1, &o);

    unsigned char *data = nullptr;
    size_t         len  = 0;
    int            w = 0, h = 0;
    const char    *fmt = "png";
    if (!take_shot(o, &data, &len, &w, &h, &fmt))
        return fail(L, "screenshot failed");

    lua_createtable(L, 0, 4);
    lua_pushinteger(L, w);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, h);
    lua_setfield(L, -2, "h");
    lua_pushstring(L, fmt);
    lua_setfield(L, -2, "format");
    lua_pushlstring(L, (const char *)data, len);
    lua_setfield(L, -2, "data");
    common::release(data);
    return 1;
}

int lua_gui_screenshot_to(lua_State *L)
{
    TCHAR path[MAX_PATH];
    if (!str_to_tstr(L, 1, path, sizeof path / sizeof path[0]))
        return fail(L, "path too long");

    shot_opts o;
    parse_shot_opts(L, 2, &o);

    unsigned char *data = nullptr;
    size_t         len  = 0;
    int            w = 0, h = 0;
    const char    *fmt = "png";
    if (!take_shot(o, &data, &len, &w, &h, &fmt))
        return fail(L, "screenshot failed");

    FILE *f = _tfopen(path, TEXT("wb"));
    if (f == nullptr)
    {
        common::release(data);
        return fail(L, "cannot open file");
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    common::release(data);

    if (written != len)
        return fail(L, "short write");
    /* T1（D50）：写盘成功才算「本次运行写出的文件」（与 open_file 的 w/a 句柄同一口径） */
    host::output_record_path(lua_tostring(L, 1));
    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

/* ------------------------------ 鼠标 ------------------------------ */
int lua_gui_position(lua_State *L)
{
    int x = 0, y = 0;
    if (!core::gui::cursor_position(&x, &y))
        return fail(L, "cursor_position failed");
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    return 2;
}

int lua_gui_move(lua_State *L)
{
    int      x  = (int)luaL_checkinteger(L, 1);
    int      y  = (int)luaL_checkinteger(L, 2);
    unsigned ms = (unsigned)luaL_optinteger(L, 3, 0);
    if (ms == 0)
        return ok_bool(L, core::gui::mouse_move(x, y));

    int sx = 0, sy = 0;
    core::gui::cursor_position(&sx, &sy);
    const int steps = 10;
    for (int i = 1; i <= steps; i++)
    {
        core::gui::mouse_move(sx + (x - sx) * i / steps, sy + (y - sy) * i / steps);
        core::gui::sleep_ms(ms / steps > 0 ? ms / steps : 1);
    }
    return ok_bool(L, core::gui::mouse_move(x, y));
}

int parse_button(lua_State *L, int idx, int def)
{
    if (lua_type(L, idx) == LUA_TSTRING)
    {
        const char *s = lua_tostring(L, idx);
        if (strcmp(s, "left") == 0)
            return 1;
        if (strcmp(s, "right") == 0)
            return 2;
        if (strcmp(s, "middle") == 0)
            return 3;
        return def;
    }
    if (lua_type(L, idx) == LUA_TNUMBER)
        return (int)lua_tointeger(L, idx);
    return def;
}

/* gui.click([x, y][, button][, times]) */
int lua_gui_click(lua_State *L)
{
    int top = lua_gettop(L);
    int pos = 1;
    int x = 0, y = 0;
    bool has_xy = false;

    if (top >= 2 && lua_isnumber(L, 1) && lua_isnumber(L, 2))
    {
        x      = (int)lua_tointeger(L, 1);
        y      = (int)lua_tointeger(L, 2);
        has_xy = true;
        pos    = 3;
    }

    int button = parse_button(L, pos, 1);
    pos++;
    int times = (lua_isnumber(L, pos)) ? (int)lua_tointeger(L, pos) : 1;
    if (times < 1)
        times = 1;

    if (has_xy && !core::gui::mouse_move(x, y))
        return fail(L, "mouse_move failed");

    bool ok = true;
    for (int i = 0; i < times && ok; i++)
    {
        ok = core::gui::mouse_button(button, true);
        if (ok)
            ok = core::gui::mouse_button(button, false);
        if (ok && times > 1)
            core::gui::sleep_ms(40);
    }
    return ok_bool(L, ok);
}

int lua_gui_down(lua_State *L)
{
    int button = parse_button(L, 1, 1);
    return ok_bool(L, core::gui::mouse_button(button, true));
}

int lua_gui_up(lua_State *L)
{
    int button = parse_button(L, 1, 1);
    return ok_bool(L, core::gui::mouse_button(button, false));
}

int lua_gui_drag(lua_State *L)
{
    int      x      = (int)luaL_checkinteger(L, 1);
    int      y      = (int)luaL_checkinteger(L, 2);
    int      button = parse_button(L, 3, 1);
    unsigned ms     = (unsigned)luaL_optinteger(L, 4, 200);
    return ok_bool(L, core::gui::mouse_drag(x, y, button, ms));
}

int lua_gui_scroll(lua_State *L)
{
    int delta = (int)luaL_checkinteger(L, 1);
    if (lua_gettop(L) >= 3)
        core::gui::mouse_move((int)lua_tointeger(L, 2), (int)lua_tointeger(L, 3));
    return ok_bool(L, core::gui::mouse_wheel(delta));
}

/* ------------------------------ 键盘 ------------------------------ */
/* UTF-8 → UTF-16 code unit 序列（SendInput 的 UNICODE 分支按 code unit 输入） */
int lua_gui_write(lua_State *L)
{
    size_t      n = 0;
    const char *s = luaL_checklstring(L, 1, &n);
    unsigned    interval = (unsigned)luaL_optinteger(L, 2, 0);

#ifdef _UNICODE
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, nullptr, 0);
    if (wlen <= 0)
        return fail(L, "decode failed");
    LPWSTR units = (LPWSTR)alloc_((size_t)wlen * sizeof(WCHAR));
    if (units == nullptr)
        return fail(L, "oom");
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, units, wlen);
    bool ok = true;
    for (int i = 0; i < wlen && ok; i++)
    {
        ok = core::gui::type_codepoint((unsigned)units[i]);
        if (ok && interval > 0)
            core::gui::sleep_ms(interval);
    }
    free_(units);
#else
    /* legacy：逐字节（ANSI）走 VkKeyScan 路径 */
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++)
    {
        ok = core::gui::type_codepoint((unsigned char)s[i]);
        if (ok && interval > 0)
            core::gui::sleep_ms(interval);
    }
#endif
    return ok_bool(L, ok);
}

/* 支持 "ctrl+c" 这种写法：按 '+' 拆成多个键 */
int lua_gui_press(lua_State *L)
{
    const char *name  = luaL_checkstring(L, 1);
    int         times = (int)luaL_optinteger(L, 2, 1);
    if (times < 1)
        times = 1;

    if (strchr(name, '+') != nullptr && name[0] != '+' && strlen(name) > 1)
    {
        /* 逐段按下 + 逆序抬起 */
        unsigned vks[8];
        int      count = 0;
        char     part[32];
        size_t   pi = 0;
        for (const char *p = name;; p++)
        {
            if (*p == '+' || *p == '\0')
            {
                part[pi] = '\0';
                if (pi == 0 || count >= 8)
                {
                    lua_pushnil(L);
                    lua_pushstring(L, "bad key chord");
                    return 2;
                }
                if (!core::gui::vk_from_name(part, &vks[count]))
                {
                    lua_pushnil(L);
                    lua_pushstring(L, "unknown key in chord");
                    return 2;
                }
                count++;
                pi = 0;
                if (*p == '\0')
                    break;
                continue;
            }
            if (pi + 1 < sizeof part)
                part[pi++] = *p;
        }
        bool ok = true;
        for (int i = 0; i < count && ok; i++)
            ok = core::gui::key_press(vks[i], true);
        for (int i = count - 1; i >= 0 && ok; i--)
            ok = core::gui::key_press(vks[i], false);
        return ok_bool(L, ok);
    }

    unsigned vk = 0;
    if (!core::gui::vk_from_name(name, &vk))
    {
        lua_pushnil(L);
        lua_pushstring(L, "unknown key name");
        return 2;
    }
    bool ok = true;
    for (int i = 0; i < times && ok; i++)
        ok = core::gui::key_tap(vk);
    return ok_bool(L, ok);
}

int lua_gui_hotkey(lua_State *L)
{
    int      count = lua_gettop(L);
    if (count <= 0 || count > 8)
        return fail(L, "hotkey takes 1..8 keys");

    unsigned vks[8];
    for (int i = 0; i < count; i++)
    {
        const char *name = luaL_checkstring(L, i + 1);
        if (!core::gui::vk_from_name(name, &vks[i]))
            return fail(L, "unknown key name");
    }

    bool ok = true;
    for (int i = 0; i < count && ok; i++)
        ok = core::gui::key_press(vks[i], true);
    for (int i = count - 1; i >= 0 && ok; i--)
        ok = core::gui::key_press(vks[i], false);
    return ok_bool(L, ok);
}

int lua_gui_key_down(lua_State *L)
{
    unsigned vk = 0;
    if (!core::gui::vk_from_name(luaL_checkstring(L, 1), &vk))
        return fail(L, "unknown key name");
    return ok_bool(L, core::gui::key_press(vk, true));
}

int lua_gui_key_up(lua_State *L)
{
    unsigned vk = 0;
    if (!core::gui::vk_from_name(luaL_checkstring(L, 1), &vk))
        return fail(L, "unknown key name");
    return ok_bool(L, core::gui::key_press(vk, false));
}

/* ------------------------------ 窗口 ------------------------------ */
int lua_gui_windows(lua_State *L)
{
    core::gui::window_info *buf = (core::gui::window_info *)alloc_(sizeof(core::gui::window_info) * k_max_enum_windows);
    if (buf == nullptr)
        return fail(L, "oom");

    unsigned n = core::gui::enum_windows(buf, k_max_enum_windows);
    lua_createtable(L, (int)n, 0);
    for (unsigned i = 0; i < n; i++)
    {
        push_window(L, buf[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    free_(buf);
    return 1;
}

int lua_gui_children(lua_State *L)
{
    HWND parent = to_hwnd(L, 1);
    if (!core::gui::window_is_valid(parent))
        return fail(L, "invalid hwnd");

    core::gui::window_info *buf = (core::gui::window_info *)alloc_(sizeof(core::gui::window_info) * k_max_enum_windows);
    if (buf == nullptr)
        return fail(L, "oom");

    unsigned n = core::gui::enum_children(parent, buf, k_max_enum_windows);
    lua_createtable(L, (int)n, 0);
    for (unsigned i = 0; i < n; i++)
    {
        push_window(L, buf[i]);
        /* 子控件多一个 `id`（GetDlgCtrlID）：对标准对话框/按钮很有用 */
        lua_pushinteger(L, core::gui::window_ctrl_id(buf[i].hwnd));
        lua_setfield(L, -2, "id");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    free_(buf);
    return 1;
}

/* gui.find("title 子串" | {title=, class=, pid=}) → hwnd | nil */
int lua_gui_find_spec(lua_State *L, int idx, core::gui::window_match *m, TCHAR *title, size_t title_cap,
                      TCHAR *cls, size_t cls_cap)
{
    m->title_sub = nullptr;
    m->class_sub = nullptr;
    m->pid       = 0;

    if (lua_type(L, idx) == LUA_TSTRING)
    {
        if (!str_to_tstr(L, idx, title, title_cap))
            return -1;
        m->title_sub = title;
        return 0;
    }
    if (lua_type(L, idx) != LUA_TTABLE)
        return -1;

    lua_getfield(L, idx, "title");
    if (lua_isstring(L, -1))
    {
        if (!str_to_tstr(L, lua_gettop(L), title, title_cap))
        {
            lua_pop(L, 1);
            return -1;
        }
        m->title_sub = title;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "class");
    if (lua_isstring(L, -1))
    {
        if (!str_to_tstr(L, lua_gettop(L), cls, cls_cap))
        {
            lua_pop(L, 1);
            return -1;
        }
        m->class_sub = cls;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "pid");
    if (lua_isnumber(L, -1))
        m->pid = (DWORD)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return 0;
}

int lua_gui_find(lua_State *L)
{
    core::gui::window_match m;
    TCHAR                   title[256];
    TCHAR                   cls[128];
    if (lua_gui_find_spec(L, 1, &m, title, sizeof title / sizeof title[0], cls, sizeof cls / sizeof cls[0]) != 0)
        return fail(L, "bad spec");

    HWND hwnd = core::gui::find_window(m);
    if (hwnd == nullptr)
        return fail(L, "not found");
    push_hwnd(L, hwnd);
    return 1;
}

int lua_gui_active(lua_State *L)
{
    push_hwnd(L, core::gui::window_active());
    return 1;
}

int lua_gui_activate(lua_State *L)
{
    return ok_bool(L, core::gui::window_activate(to_hwnd(L, 1)));
}

int lua_gui_focus(lua_State *L)
{
    return ok_bool(L, core::gui::window_focus(to_hwnd(L, 1)));
}

int lua_gui_rect(lua_State *L)
{
    int x = 0, y = 0, w = 0, h = 0;
    if (!core::gui::window_rect(to_hwnd(L, 1), &x, &y, &w, &h))
        return fail(L, "window_rect failed");
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 4;
}

int lua_gui_set_rect(lua_State *L)
{
    HWND hwnd = to_hwnd(L, 1);
    int  x = (int)luaL_checkinteger(L, 2);
    int  y = (int)luaL_checkinteger(L, 3);
    int  w = (int)luaL_checkinteger(L, 4);
    int  h = (int)luaL_checkinteger(L, 5);
    return ok_bool(L, core::gui::window_set_rect(hwnd, x, y, w, h));
}

int lua_gui_show(lua_State *L)
{
    return ok_bool(L, core::gui::window_show(to_hwnd(L, 1), luaL_checkstring(L, 2)));
}

int lua_gui_close(lua_State *L)
{
    return ok_bool(L, core::gui::window_close(to_hwnd(L, 1)));
}

int lua_gui_text(lua_State *L)
{
    TCHAR buf[512];
    if (!core::gui::window_text(to_hwnd(L, 1), buf, sizeof buf / sizeof buf[0]))
        return fail(L, "window_text failed");
    push_tstr(L, buf);
    return 1;
}

int lua_gui_set_text(lua_State *L)
{
    HWND  hwnd = to_hwnd(L, 1);
    TCHAR text[1024];
    if (!str_to_tstr(L, 2, text, sizeof text / sizeof text[0]))
        return fail(L, "text too long");
    return ok_bool(L, core::gui::window_set_text(hwnd, text));
}

int lua_gui_post(lua_State *L)
{
    HWND     hwnd = to_hwnd(L, 1);
    unsigned msg  = (unsigned)luaL_checkinteger(L, 2);
    unsigned wp   = (unsigned)luaL_optinteger(L, 3, 0);
    unsigned lp   = (unsigned)luaL_optinteger(L, 4, 0);
    return ok_bool(L, core::gui::window_post(hwnd, msg, wp, lp));
}

int lua_gui_send(lua_State *L)
{
    HWND     hwnd = to_hwnd(L, 1);
    unsigned msg  = (unsigned)luaL_checkinteger(L, 2);
    unsigned wp   = (unsigned)luaL_optinteger(L, 3, 0);
    unsigned lp   = (unsigned)luaL_optinteger(L, 4, 0);
    size_t   result = 0;
    if (!core::gui::window_send(hwnd, msg, wp, lp, &result))
        return fail(L, "send failed");
    lua_pushinteger(L, (lua_Integer)result);
    return 1;
}

/* 控件中心真实点击（= pyautogui 的 click，而不是 BM_CLICK 消息） */
int lua_gui_click_control(lua_State *L)
{
    HWND hwnd = to_hwnd(L, 1);
    int  x = 0, y = 0;
    if (!core::gui::window_center(hwnd, &x, &y))
        return fail(L, "window_center failed");
    if (!core::gui::window_activate(hwnd))
    {
        /* 激活失败不致命（有的控件属于后台窗口），继续点 */
    }
    if (!core::gui::mouse_move(x, y))
        return fail(L, "mouse_move failed");
    bool ok = core::gui::mouse_button(1, true) && core::gui::mouse_button(1, false);
    return ok_bool(L, ok);
}

/* gui.wait(spec_or_hwnd [, timeout_ms]) → hwnd | nil */
int lua_gui_wait(lua_State *L)
{
    int      timeout = (int)luaL_optinteger(L, 2, 5000);
    DWORD    deadline = GetTickCount() + (DWORD)(timeout > 0 ? timeout : 0);

    HWND direct = (lua_isnumber(L, 1)) ? (HWND)(size_t)lua_tointeger(L, 1) : nullptr;
    core::gui::window_match m;
    TCHAR                   title[256];
    TCHAR                   cls[128];
    bool                    by_spec = false;

    if (direct == nullptr)
    {
        if (lua_gui_find_spec(L, 1, &m, title, sizeof title / sizeof title[0], cls, sizeof cls / sizeof cls[0]) != 0)
            return fail(L, "bad spec");
        by_spec = true;
    }

    for (;;)
    {
        HWND found = direct != nullptr ? direct : core::gui::find_window(m);
        if (found != nullptr && core::gui::window_is_valid(found))
        {
            push_hwnd(L, found);
            return 1;
        }
        if ((LONG)(GetTickCount() - deadline) >= 0)
            return fail(L, "timeout");
        core::gui::sleep_ms(100);
        (void)by_spec;
    }
}

/* ------------------------------ 杂项 ------------------------------ */
int lua_gui_sleep(lua_State *L)
{
    core::gui::sleep_ms((unsigned)luaL_checkinteger(L, 1));
    return 0;
}

int lua_gui_foreground_pid(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)core::gui::window_foreground_pid());
    return 1;
}

int lua_gui_dpi_aware(lua_State *L)
{
    return ok_bool(L, core::gui::set_dpi_aware());
}

int lua_gui_cursor_locked(lua_State *L)
{
    bool lock = lua_toboolean(L, 1) != 0;
    return ok_bool(L, core::gui::set_cursor_locked(lock));
}

/* ------------------------------ 注册 ------------------------------ */
struct gui_fn
{
    const char *name;
    lua_CFunction fn;
};

const gui_fn k_gui_fns[] = {
    { "size", lua_gui_size },
    { "pixel", lua_gui_pixel },
    { "screenshot", lua_gui_screenshot },
    { "screenshot_to", lua_gui_screenshot_to },
    { "position", lua_gui_position },
    { "move", lua_gui_move },
    { "click", lua_gui_click },
    { "down", lua_gui_down },
    { "up", lua_gui_up },
    { "drag", lua_gui_drag },
    { "scroll", lua_gui_scroll },
    { "write", lua_gui_write },
    { "press", lua_gui_press },
    { "hotkey", lua_gui_hotkey },
    { "key_down", lua_gui_key_down },
    { "key_up", lua_gui_key_up },
    { "windows", lua_gui_windows },
    { "children", lua_gui_children },
    { "find", lua_gui_find },
    { "active", lua_gui_active },
    { "activate", lua_gui_activate },
    { "focus", lua_gui_focus },
    { "rect", lua_gui_rect },
    { "set_rect", lua_gui_set_rect },
    { "show", lua_gui_show },
    { "close", lua_gui_close },
    { "wait", lua_gui_wait },
    { "text", lua_gui_text },
    { "set_text", lua_gui_set_text },
    { "post", lua_gui_post },
    { "send", lua_gui_send },
    { "click_control", lua_gui_click_control },
    { "sleep", lua_gui_sleep },
    { "foreground_pid", lua_gui_foreground_pid },
    { "dpi_aware", lua_gui_dpi_aware },
    { "cursor_locked", lua_gui_cursor_locked },
};
} /* namespace */

void register_gui_api(lua_State *L)
{
    lua_createtable(L, 0, (int)(sizeof k_gui_fns / sizeof k_gui_fns[0]));
    for (size_t i = 0; i < sizeof k_gui_fns / sizeof k_gui_fns[0]; i++)
    {
        lua_pushcfunction(L, k_gui_fns[i].fn);
        lua_setfield(L, -2, k_gui_fns[i].name);
    }
    lua_setglobal(L, "gui");
}
