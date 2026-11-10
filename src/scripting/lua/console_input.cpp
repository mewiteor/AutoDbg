/* =============================================================================
 * src/scripting/lua/console_input.cpp —— 宿主层控制台输入的「判定」纯函数（Q27）
 *
 * 本文件只含从 bindings.cpp 的 `CheckConsoleInput` 里**逐字搬出**的扫描循环
 * （唯一改动：指针加 `const`，行为不变）。它不做 I/O、不碰 Lua，因此可直接单测。
 * ========================================================================== */
#include "console_input.hpp"

int decide_console_action(const INPUT_RECORD *records, DWORD count, bool can_detach_without_term)
{
    int ret = 0;
    for (DWORD i = 0; i < count; i++)
    {
        const INPUT_RECORD *e = &records[i];
        if (e->EventType != KEY_EVENT)
            continue;
        const KEY_EVENT_RECORD *key_rec = &e->Event.KeyEvent;
        if (!key_rec->bKeyDown && key_rec->wVirtualKeyCode == 'Q')
        {
            if (can_detach_without_term && (key_rec->dwControlKeyState & SHIFT_PRESSED))
                ret = 2;
            else
                ret = 1;
            break;
        }
    }
    return ret;
}
