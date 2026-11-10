#ifndef AUTODBG_CORE_GUI_HPP
#define AUTODBG_CORE_GUI_HPP
/* =============================================================================
 * src/core/gui.hpp —— GUI 自动化原语（pyautogui 等价物的底层，**不感知 Lua**）
 *
 * 来源：新写（plan_server.md S4 / §7，决策 D23/D24）。
 * 职责：屏幕截图、像素、鼠标/键盘注入、窗口与控制枚举/操作。
 *   绑定层（`src/scripting/lua/gui_bindings.cpp`）只做参数编组与 UTF-8 ↔ TCHAR 转换。
 * 目标系统差异（实测见 plan_server.md §1.4，一律 dynapi + 降级）：
 *   · `SendInput`：Win98+ → 取不到退 `mouse_event`/`keybd_event`（Win95 都有）；
 *   · `PrintWindow`：XP+ → 取不到退「按窗口矩形抓屏」；
 *   · `BlockInput`：98+、`SetProcessDPIAware`：Vista+ → 取不到返回 false（调用方忽略）。
 * 约定：
 *   · 所有 HWND 都按值传递；调用方负责先确认 `window_is_valid`；
 *   · 截图一律 **BGRA、top-down、stride = w*4**（`common::png_writer` 直接吃这个布局）；
 *   · 字符串用 TCHAR（与构建的 ANSI/UNICODE 口径一致），转换在绑定层做。
 * ========================================================================== */
#include <windows.h>
#include <stddef.h>

namespace autodbg::core::gui
{
/* ------------------------------ 屏幕 ------------------------------ */
bool screen_size(int *w, int *h);
bool pixel_color(int x, int y, unsigned char *r, unsigned char *g, unsigned char *b);

/* 截图：成功时 `*out` 是新分配缓冲（用 `release_capture` 释放）。
 * `scale` == 1 用 BitBlt，否则用 StretchBlt（HALFTONE）采样。 */
bool capture_region(int x, int y, int w, int h, double scale,
                    unsigned char **out, int *out_w, int *out_h, int *out_stride);
bool capture_window(HWND hwnd, double scale,
                    unsigned char **out, int *out_w, int *out_h, int *out_stride);
void release_capture(unsigned char *p);

/* ------------------------------ 鼠标 ------------------------------ */
bool cursor_position(int *x, int *y);
bool mouse_move(int x, int y);
bool mouse_button(int button, bool down); /* 1=left 2=right 3=middle */
bool mouse_wheel(int delta);
/* 按下 → 分步移动 → 抬起（`duration_ms` 是总时长；0 视为瞬时） */
bool mouse_drag(int x, int y, int button, unsigned duration_ms);

/* ------------------------------ 键盘 ------------------------------ */
/* 名称 → 虚拟键码：字母/数字单字符、"f1".."f24"、enter/tab/esc/space/backspace/
 * delete/insert/home/end/pageup/pagedown/up/down/left/right/shift/ctrl/alt/win 等 */
bool vk_from_name(const char *name, unsigned *vk);

bool key_press(unsigned vk, bool down);
bool key_tap(unsigned vk);
/* 输入一个 Unicode 码点：优先 SendInput + KEYEVENTF_UNICODE；老系统退 VkKeyScan */
bool type_codepoint(unsigned codepoint);

void sleep_ms(unsigned ms);

/* ------------------------------ 窗口 ------------------------------ */
struct window_info
{
    HWND  hwnd;
    DWORD pid;
    int   x, y, w, h;
    bool  visible;
    bool  minimized;
    bool  enabled;
    TCHAR title[256];
    TCHAR cls[128];
};

/* 枚举顶层窗口 / 子窗口：返回**写入 out 的条数**（<= cap；超出的被截断） */
unsigned enum_windows(window_info *out, unsigned cap);
unsigned enum_children(HWND parent, window_info *out, unsigned cap);

HWND  window_active();
bool  window_is_valid(HWND hwnd);
DWORD window_pid(HWND hwnd);
bool  window_rect(HWND hwnd, int *x, int *y, int *w, int *h);
bool  window_set_rect(HWND hwnd, int x, int y, int w, int h);
bool  window_show(HWND hwnd, const char *cmd); /* "min"|"max"|"restore"|"hide"|"show" */
bool  window_close(HWND hwnd);
bool  window_activate(HWND hwnd);
/* 把**键盘焦点**给到指定窗口/控件：`SetFocus` 只对同线程窗口有效，跨进程必须先
 * `AttachThreadInput` 到目标线程（S4 实测：只 activate + 真实点击之后，`SendInput`
 * 注入的字符仍然进不了 WinForms 文本框）。 */
bool  window_focus(HWND hwnd);
bool  window_text(HWND hwnd, TCHAR *out, size_t cap);
bool  window_set_text(HWND hwnd, LPCTSTR text);
unsigned window_foreground_pid();
bool  window_center(HWND hwnd, int *x, int *y); /* 窗口中心点（屏幕坐标） */
bool  window_post(HWND hwnd, unsigned msg, unsigned wparam, unsigned lparam);
bool  window_send(HWND hwnd, unsigned msg, unsigned wparam, unsigned lparam, size_t *result);
int   window_ctrl_id(HWND hwnd); /* 子控件 ID（GetDlgCtrlID），失败 -1 */

/* 按条件找窗口：任一已设置的字段都必须是「子串匹配（不区分大小写）」 */
struct window_match
{
    const TCHAR *title_sub; /* nullptr = 不限制 */
    const TCHAR *class_sub;
    DWORD        pid; /* 0 = 不限制 */
};
HWND find_window(const window_match &m);

/* ------------------------------ 增强（dynapi） ------------------------------ */
bool set_dpi_aware();            /* SetProcessDPIAware（Vista+） */
bool set_cursor_locked(bool lock); /* BlockInput（98+） */
} /* namespace autodbg::core::gui */

#endif /* AUTODBG_CORE_GUI_HPP */
