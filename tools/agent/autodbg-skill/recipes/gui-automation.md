# Recipe：GUI 自动化（`gui` 表）

场景：目标是个图形程序，没有可用的命令行接口 —— 你要「找到窗口 → 认控件 → 操作 → 截图确认」。

`gui.*` **不需要调试会话**，本地直接跑就行（不要加 `--server`）：

```bat
AutoDbg_64.exe --quiet gui_script.lua
```

## 1. 找窗口 / 认控件

```lua
local h = gui.find{ title = "Untitled - Notepad" }        -- 也可以用 class =
if not h then
  for _, w in ipairs(gui.windows()) do                    -- 自己筛：hwnd/pid/title/class/x/y/w/h/visible
    print(w.hwnd, w.pid, w.title)
  end
  error("window not found")
end

gui.activate(h)                                           -- 前置（要真实输入时必须）
print("rect:", gui.rect(h))                               -- x, y, w, h

for _, c in ipairs(gui.children(h)) do                    -- 子控件
  print(string.format("hwnd=%s class=%s id=%s text=%q",
        tostring(c.hwnd), tostring(c.class), tostring(c.id), tostring(c.text)))
end
```

## 2. 操作（从「不用鼠标」到「真输入」）

```lua
-- ① 最稳：直接发消息（不需要焦点、不会动真实光标）
gui.post(btn, 0x00F5)            -- BM_CLICK
gui.send(edit, 0x000C, 0, ptr)   -- 想拿返回值的消息

-- ② 改控件文本 / 读文本
gui.set_text(edit, "hello")
print(gui.text(edit))

-- ③ 真实点击（需要窗口在前台；会移动光标）
gui.activate(h)
gui.click_control(btn)           -- 控件中心真实点击
gui.click(x, y)                  -- 绝对屏幕坐标
gui.write("abc")                 -- 可打印 ASCII 走 VK+Shift，非 ASCII 走 UNICODE
gui.press("enter")
gui.hotkey("ctrl", "s")
gui.sleep(300)                   -- 等 UI 反应
```

## 3. 截图确认（agent 的眼睛）

```lua
local shot = gui.screenshot{ hwnd = h, format = "png" }    -- { w=, h=, format=, data=<二进制> }
print(string.format("shot %dx%d %d bytes", shot.w, shot.h, #shot.data))

-- 或者直接落盘（会被记进 --json 的 images 列表）
gui.screenshot_to("C:/tmp/target.png")
print("wrote C:/tmp/target.png")

-- 只看一个区域：scale < 1 会缩小（省内存/省 token）
local small = gui.screenshot{ hwnd = h, scale = 0.5, format = "png" }
```

## 4. 典型循环（做一步、看一眼）

```lua
local h = gui.find{ title = "Config Tool" }
gui.activate(h)

local btn = nil
for _, c in ipairs(gui.children(h)) do
  if c.text == "Apply" then btn = c.hwnd end
end
if not btn then error("Apply button not found") end

gui.post(btn, 0x00F5)             -- BM_CLICK
gui.sleep(500)                    -- 等它做事

local status = gui.find{ title = "Config Tool" }   -- 状态可能在标题/某个 label 上
gui.screenshot_to("C:/tmp/after_apply.png")
return { applied = true, shot = "C:/tmp/after_apply.png" }
```

要点：

- **不需要焦点**的操作优先（`gui.post` / `gui.set_text`）；只有在模拟真实用户时才用
  `gui.click` / `gui.write`。
- 目标被调试器挂起时窗口不响应输入：先让它跑起来（或干脆不要 attach）。
- 失败一律返回 `nil, errmsg`（不抛异常）—— 每个 `gui.*` 调用都该判一下。
- 坐标是**屏幕绝对像素**；多显示器/DPI 会变，必要时先 `gui.dpi_aware()`。
