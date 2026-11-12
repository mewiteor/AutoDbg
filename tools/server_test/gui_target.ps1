# =============================================================================
# tools/server_test/gui_target.ps1 —— GUI 自动化的「靶子窗口」（S4 测试用）
#
# 来源：新写（S4，见 PITFALLS.md；原 plan_server.md，现合并进 plan.md）。
# 为什么要它：GUI 自动化的断言需要一个**我们自己的**窗口（标题/类名/子控件/文本
#   都已知），否则测试得去操作真实应用，既不可重复也不礼貌。
# 形态：WinForms（PowerShell 自带，无需编译）：
#   · Form 标题 = $Title（gui.find 用标题子串找它）
#   · Label `lblStatus`：初始 "status: idle"，按钮被点后变 "status: clicked"
#   · Button `btnPressMe`
#   · TextBox `txtInput`（-AllowInput 的输入测试用）
# 用法：pwsh -File gui_target.ps1 [-Title AutoDbgGuiTarget] [-X 200] [-Y 200]
# 停止：关闭窗口（gui.close / gui.post(hwnd, 0x0010)）或在宿主里 Kill 进程。
# =============================================================================
[CmdletBinding()]
param(
    [string]$Title = 'AutoDbgGuiTarget',
    [int]$X = 200,
    [int]$Y = 200,
    [int]$W = 420,
    [int]$H = 260
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form                  = New-Object System.Windows.Forms.Form
$form.Text             = $Title
$form.Name             = 'AutoDbgGuiTarget'
$form.StartPosition    = 'Manual'
$form.Location         = New-Object System.Drawing.Point($X, $Y)
$form.ClientSize       = New-Object System.Drawing.Size($W, $H)
$form.TopMost          = $true

$lbl                   = New-Object System.Windows.Forms.Label
$lbl.Text              = 'status: idle'
$lbl.Name              = 'lblStatus'
$lbl.AutoSize          = $true
$lbl.Location          = New-Object System.Drawing.Point(16, 16)
$form.Controls.Add($lbl)

$btn                   = New-Object System.Windows.Forms.Button
$btn.Text              = 'PressMe'
$btn.Name              = 'btnPressMe'
$btn.Location          = New-Object System.Drawing.Point(16, 48)
$btn.Size              = New-Object System.Drawing.Size(120, 32)
$btn.Add_Click({ $lbl.Text = 'status: clicked' }.GetNewClosure())
$form.Controls.Add($btn)

$txt                   = New-Object System.Windows.Forms.TextBox
$txt.Text              = ''
$txt.Name              = 'txtInput'
$txt.Location          = New-Object System.Drawing.Point(16, 96)
$txt.Size              = New-Object System.Drawing.Size(240, 24)
$form.Controls.Add($txt)

[void]$form.Show()

# 独立的消息泵：headless 场景（无交互）下也能响应 gui.post 发来的消息。
# ❗必须用 `Application::Run($form)`（带主窗体）：无参的 `Run()` 在没有主窗体时
#   会直接退出消息循环 → 窗口从来不会真正出现（S4 实测踩过）。
$timer          = New-Object System.Windows.Forms.Timer
$timer.Interval = 50
$timer.Add_Tick({
        if (-not $form.Visible) {
            $timer.Stop()
            [System.Windows.Forms.Application]::Exit()
        }
    })
$timer.Start()

[System.Windows.Forms.Application]::Run($form)
