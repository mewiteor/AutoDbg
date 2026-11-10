# =============================================================================
# tools/server_test/e2e_gui.ps1 —— GUI 自动化端到端验收（S4）
#
# 来源：新写（plan_server.md S4）。
# 形态：起 server + 起一个**我们自己的** WinForms 靶子窗口（gui_target.ps1），
#       用 `/run` 执行调用 `gui.*` 的 Lua 脚本并断言输出。
# 为什么用靶子窗口：标题/类名/子控件/文本都已知，测试可重复、也不去操作用户的真实应用。
#
# 默认只做**非侵入**检查（枚举/查找/矩形/子控件/文本/消息/截图）。
# `-AllowInput` 才测真实鼠标键盘（会移动光标并在靶子窗口里点击/打字）。
#
# 用法：pwsh -File tools/server_test/e2e_gui.ps1 [-Render 64|86] [-Variant ...] [-Port 18800] [-AllowInput]
# 退出码：0 = 全通过；1 = 有失败；2 = 前置缺失
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('64', '86')]
    [string]$Render = '64',
    [ValidateSet('', 'modern', 'legacy')]
    [string]$Variant = '',
    [int]$Port = 18800,
    [switch]$AllowInput,
    [switch]$KeepTarget
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Exe = Join-Path $Root ".out/AutoDbg_$Render.exe"
$TargetScript = Join-Path $PSScriptRoot 'gui_target.ps1'
if (-not (Test-Path $Exe)) { Write-Output "SKIP: 产物不存在：$Exe"; exit 2 }
if (-not (Test-Path $TargetScript)) { Write-Output "SKIP: 缺少 $TargetScript"; exit 2 }

$script:pass = 0
$script:fail = 0
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_gui_e2e'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

function Ok([string]$n, [string]$d = '') { $script:pass++; Write-Output ("[ok]   {0}  {1}" -f $n, $d) }
function Bad([string]$n, [string]$d) { $script:fail++; Write-Output ("[FAIL] {0}  {1}" -f $n, $d) }
function Check([string]$n, [bool]$c, [string]$d = '') { if ($c) { Ok $n $d } else { Bad $n $d } }

function RunLua([string]$src, [string[]]$Args2 = @())
{
    $q = ''
    if ($Args2.Count -gt 0)
    {
        $pairs = @()
        foreach ($a in $Args2) { $pairs += ('arg=' + [System.Uri]::EscapeDataString($a)) }
        $q = '?' + ($pairs -join '&') + '&format=text'
    }
    else { $q = '?format=text' }

    $bytes = [System.Text.Encoding]::UTF8.GetBytes($src)
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/run$q" -Method Post -Body $bytes `
        -ContentType 'text/x-lua; charset=utf-8' -Headers @{ 'X-AutoDbg-Filename' = 'gui_e2e.lua' } `
        -TimeoutSec 60 -SkipHttpErrorCheck
    return $r.Content
}

function Result([string]$text, [string]$key)
{
    $m = [regex]::Match($text, "(?m)^" + [regex]::Escape($key) + "=(.*)$")
    if ($m.Success) { return $m.Groups[1].Value.Trim() } else { return '' }
}

# ---------------------------------------------------------------- 起 server / 靶子
$srvArgs = @('--server', "--server-port=$Port")
if ($Variant) { $srvArgs += "--variant=$Variant" }
$srvOut = Join-Path $TmpDir 'server.out'
$srvErr = Join-Path $TmpDir 'server.err'
$server = Start-Process -FilePath $Exe -ArgumentList $srvArgs -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr -PassThru -NoNewWindow

$target = $null
try
{
    $deadline = (Get-Date).AddSeconds(15)
    $ready = $false
    while ((Get-Date) -lt $deadline)
    {
        if ($server.HasExited) { break }
        try
        {
            if ((Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2 -SkipHttpErrorCheck).StatusCode -eq 200) { $ready = $true; break }
        }
        catch { Start-Sleep -Milliseconds 150 }
    }
    if (-not $ready) { Write-Output 'SKIP: server 未就绪'; exit 2 }

    $targetOut = Join-Path $TmpDir 'target.out'
    $targetErr = Join-Path $TmpDir 'target.err'
    $target = Start-Process -FilePath 'pwsh' -ArgumentList @('-NoProfile', '-File', $TargetScript) `
        -RedirectStandardOutput $targetOut -RedirectStandardError $targetErr -PassThru

    # 等窗口出现（最多 10s）
    $hwnd = ''
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline)
    {
        $lastOut = RunLua 'local h = gui.find("AutoDbgGuiTarget"); print("HWND=" .. tostring(h or 0))'
        $hwnd = Result $lastOut 'HWND'
        if ($hwnd -and $hwnd -ne '0' -and $hwnd -ne 'nil') { break }
        Start-Sleep -Milliseconds 300
    }
    if (-not $hwnd -or $hwnd -eq '0' -or $hwnd -eq 'nil')
    {
        Write-Output "DIAG: target alive=$($target -and -not $target.HasExited) last=[$lastOut]"
        if (Test-Path $targetErr) { Write-Output ('DIAG target.err: ' + (Get-Content $targetErr -Raw)) }
        if (Test-Path $targetOut) { Write-Output ('DIAG target.out: ' + (Get-Content $targetOut -Raw)) }
        Write-Output 'SKIP: 靶子窗口没有出现（可能没有交互桌面）'
        exit 2
    }
    Ok 'target.window-found' "hwnd=$hwnd"

    # ------------------------------------------------------------ 屏幕
    $out = RunLua 'local w, h = gui.size(); print("SW=" .. w); print("SH=" .. h); local r,g,b = gui.pixel(0,0); print("PIX=" .. r .. "," .. g .. "," .. b)'
    Check 'screen.size' ([int](Result $out 'SW') -gt 0 -and [int](Result $out 'SH') -gt 0) ("SWxSH=" + (Result $out 'SW') + "×" + (Result $out 'SH'))
    Check 'screen.pixel' ((Result $out 'PIX') -match '^\d+,\d+,\d+$') ("PIX=" + (Result $out 'PIX'))

    # ------------------------------------------------------------ 枚举 / 查找 / 矩形
    $out = RunLua @'
local target = ...
local found, count = false, 0
for _, w in ipairs(gui.windows()) do
  count = count + 1
  if w.title:find("AutoDbgGuiTarget") then found = true end
end
print("COUNT=" .. count)
print("FOUND=" .. tostring(found))
local x, y, w, h = gui.rect(tonumber(target))
print("RX=" .. x); print("RY=" .. y); print("RW=" .. w); print("RH=" .. h)
print("TITLE=" .. gui.text(tonumber(target)))
'@ @($hwnd)
    Check 'windows.enumerated' ([int](Result $out 'COUNT') -gt 0) ("COUNT=" + (Result $out 'COUNT'))
    Check 'windows.contains-target' ((Result $out 'FOUND') -eq 'true') (Result $out 'FOUND')
    Check 'window.rect' ([int](Result $out 'RW') -gt 100 -and [int](Result $out 'RH') -gt 80) ("RWxRH=" + (Result $out 'RW') + "×" + (Result $out 'RH'))
    Check 'window.title' ((Result $out 'TITLE') -like '*AutoDbgGuiTarget*') (Result $out 'TITLE')

    # ------------------------------------------------------------ 子控件 + 文本
    $out = RunLua @'
local target = ...
local btn, lbl, txt
for _, c in ipairs(gui.children(tonumber(target))) do
  print("KID=" .. c.class .. "|" .. tostring(c.id) .. "|" .. c.text)
  local low = string.lower(c.class)
  if c.text == "PressMe" then btn = c.hwnd end
  if c.text:find("status") then lbl = c.hwnd end
  if low:find("edit") then txt = c.hwnd end
end
print("BTN=" .. tostring(btn or 0))
print("LBL=" .. tostring(lbl or 0))
print("TXT=" .. tostring(txt or 0))
print("LBLTEXT=" .. tostring(lbl and gui.text(lbl) or ""))
'@ @($hwnd)
    Check 'children.button' ([int64](Result $out 'BTN') -ne 0) ("BTN=" + (Result $out 'BTN'))
    Check 'children.label' ([int64](Result $out 'LBL') -ne 0) ("LBL=" + (Result $out 'LBL'))
    Check 'children.textbox' ([int64](Result $out 'TXT') -ne 0) ("TXT=" + (Result $out 'TXT'))
    Check 'children.label-text' ((Result $out 'LBLTEXT') -eq 'status: idle') (Result $out 'LBLTEXT')

    $btnHwnd = Result $out 'BTN'
    $lblHwnd = Result $out 'LBL'
    $txtHwnd = Result $out 'TXT'

    # set_text 往返
    $out = RunLua @'
local h = ...
gui.set_text(tonumber(h), "status: renamed")
print("BACK=" .. gui.text(tonumber(h)))
gui.set_text(tonumber(h), "status: idle")
print("RESET=" .. gui.text(tonumber(h)))
'@ @($lblHwnd)
    Check 'control.set-text' ((Result $out 'BACK') -eq 'status: renamed') (Result $out 'BACK')
    Check 'control.set-text-reset' ((Result $out 'RESET') -eq 'status: idle') (Result $out 'RESET')

    # send（同步消息）
    $out = RunLua @'
local h = ...
local r = gui.send(tonumber(h), 0x0000, 0, 0)
print("SEND=" .. tostring(r))
'@ @($hwnd)
    Check 'window.send' ((Result $out 'SEND') -eq '0') (Result $out 'SEND')

    # post BM_CLICK → 标签文本应变化（非侵入：不发真实鼠标）
    $out = RunLua @'
local _, btn, lbl = ...
gui.post(tonumber(btn), 0xF5, 0, 0)   -- BM_CLICK
for i = 1, 30 do
  if gui.text(tonumber(lbl)) == "status: clicked" then break end
  gui.sleep(50)
end
print("LBL=" .. gui.text(tonumber(lbl)))
'@ @($hwnd, $btnHwnd, $lblHwnd)
    Check 'control.post-click' ((Result $out 'LBL') -eq 'status: clicked') (Result $out 'LBL')

    # ------------------------------------------------------------ 截图
    $shotPath = Join-Path $TmpDir 'shot.png'
    Remove-Item $shotPath -ErrorAction SilentlyContinue
    $out = RunLua @'
local target, path = ...
local s = gui.screenshot{ hwnd = tonumber(target) }
print("PNG=" .. tostring(s ~= nil and s.data:sub(1,4) == "\137PNG"))
print("SW=" .. tostring(s and s.w or 0))
print("SH=" .. tostring(s and s.h or 0))
print("LEN=" .. tostring(s and #s.data or 0))
local b = gui.screenshot{ x = 0, y = 0, w = 120, h = 60, format = "bmp" }
print("BMP=" .. tostring(b ~= nil and b.data:sub(1,2) == "BM"))
local n = gui.screenshot_to(path, { x = 10, y = 10, w = 64, h = 64, scale = 0.5 })
print("WROTE=" .. tostring(n or 0))
'@ @($hwnd, $shotPath)
    Check 'screenshot.window-png' ((Result $out 'PNG') -eq 'true') ("w=" + (Result $out 'SW') + " h=" + (Result $out 'SH') + " len=" + (Result $out 'LEN'))
    Check 'screenshot.window-size' ([int](Result $out 'SW') -gt 100 -and [int](Result $out 'SH') -gt 80) ("SWxSH=" + (Result $out 'SW') + "×" + (Result $out 'SH'))
    Check 'screenshot.region-bmp' ((Result $out 'BMP') -eq 'true') (Result $out 'BMP')
    Check 'screenshot.to-file' ([int](Result $out 'WROTE') -gt 100) ("WROTE=" + (Result $out 'WROTE'))
    $fileOk = $false
    if (Test-Path $shotPath)
    {
        $head = [System.IO.File]::ReadAllBytes($shotPath)
        $fileOk = ($head.Length -gt 100 -and $head[0] -eq 0x89 -and $head[1] -eq 0x50)
    }
    Check 'screenshot.file-is-png' $fileOk ("path=$shotPath")

    # ------------------------------------------------------------ 可选：真实输入
    if ($AllowInput)
    {
        $out = RunLua @'
local target, btn, lbl, txt = ...
gui.activate(tonumber(target))
gui.set_text(tonumber(lbl), "status: idle")
local x, y = gui.position()
gui.move(x + 30, y)
local nx, ny = gui.position()
print("MOVED=" .. tostring(nx == x + 30))
local bx, by = gui.rect(tonumber(btn))
-- ① 真实鼠标点击（屏幕坐标）
gui.click(bx + 60, by + 16)
gui.sleep(250)
print("AFTERCLICK=" .. gui.text(tonumber(lbl)))
-- ② 真实键盘：给按钮焦点后按 Space（可见效果 = 标签变 clicked）
gui.set_text(tonumber(lbl), "status: idle")
local focus_ok = gui.focus(tonumber(btn))
gui.sleep(150)
local space_ok = gui.press("space")
gui.sleep(250)
print("FOCUS=" .. tostring(focus_ok))
print("AFTERKEY=" .. gui.text(tonumber(lbl)))
-- ③ gui.write 的返回值（文本注入 API 本身；控件级文本用 set_text 验证）
print("WRITE=" .. tostring(gui.write("hi", 10)))
gui.move(x, y)
'@ @($hwnd, $btnHwnd, $lblHwnd, $txtHwnd)
        Check 'input.move' ((Result $out 'MOVED') -eq 'true') (Result $out 'MOVED')
        Check 'input.click' ((Result $out 'AFTERCLICK') -eq 'status: clicked') (Result $out 'AFTERCLICK')
        Check 'input.focus' ((Result $out 'FOCUS') -eq 'true') (Result $out 'FOCUS')
        Check 'input.key' ((Result $out 'AFTERKEY') -eq 'status: clicked') (Result $out 'AFTERKEY')
        Check 'input.write-api' ((Result $out 'WRITE') -eq 'true') (Result $out 'WRITE')
    }
    else
    {
        Write-Output '[skip] input.move / input.click / input.focus / input.key / input.write-api（加 -AllowInput 才跑；会移动真实光标）'
    }

    # ------------------------------------------------------------ 关闭（真实 WM_CLOSE）
    $out = RunLua @'
local target = ...
gui.post(tonumber(target), 0x0010, 0, 0)  -- WM_CLOSE
print("CLOSED=1")
'@ @($hwnd)
    Start-Sleep -Milliseconds 800
    $out = RunLua 'local h = gui.find("AutoDbgGuiTarget"); print("GONE=" .. tostring(h == nil))'
    Check 'window.close' ((Result $out 'GONE') -eq 'true') (Result $out 'GONE')
}
finally
{
    if ($target -and -not $KeepTarget -and -not $target.HasExited) { try { $target.Kill() } catch { } }
    if (-not $server.HasExited)
    {
        try { Invoke-WebRequest -Uri "http://127.0.0.1:$Port/shutdown" -Method Post -TimeoutSec 2 -SkipHttpErrorCheck | Out-Null } catch { }
        if (-not $server.WaitForExit(3000)) { $server.Kill() }
    }
}

Write-Output ''
Write-Output ("gui e2e: pass={0} fail={1}" -f $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
