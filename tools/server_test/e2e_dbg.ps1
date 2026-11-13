# =============================================================================
# tools/server_test/e2e_dbg.ps1 —— `dbg` 表的本地门禁（plan_t7.md §11.7 切片 A）
#
# 来源：新写（切片 A：D51 收尾动词 / D53 help 与 caps）。T7.3 的 `pump.*` 断言在
#   `e2e_tcp.ps1` 里（跑 server 口径）；本文件跑**本地模式**（`AutoDbg.exe <script>`），
#   因为 help / caps / detach / kill 与传输无关，起一个 server 只是浪费。
#
# 用法：pwsh -File tools/server_test/e2e_dbg.ps1 [-Render 64|86] [-Variant ''|modern|legacy]
# 退出码：0 = 全过；1 = 有失败；2 = 前置缺失（产物不存在 / x64+legacy 组合）
#
# 覆盖（切片 A）：
#   caps.*            这台机器/build 的事实：arch / variant / flavor / win9x / attach / detach / hits_cap
#   detach.capability `dbg.detach` 存在且 `dbg.close` **已删**（D51：无隐式回退，能力缺席就是 nil）
#   help.*            help() / help('dbg') / help('dbg.pump') / 未知主题 / 管道下纯文本（无 ESC）
#   kill.*            dbg.kill 真的终止目标；之后状态复位（pump -> nil + "no session"）
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('64', '86')]
    [string]$Render = '86',
    [ValidateSet('', 'modern', 'legacy')]
    [string]$Variant = ''
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Exe = Join-Path $Root ".out/AutoDbg_$Render.exe"
if (-not (Test-Path $Exe))
{
    Write-Output "SKIP: 产物不存在：$Exe（先 make.bat a $Render）"
    exit 2
}
if ($Render -eq '64' -and $Variant -eq 'legacy')
{
    Write-Output 'SKIP: x64 没有 legacy 变体（AGENTS.md §11.1）'
    exit 2
}

$script:pass = 0
$script:fail = 0
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_dbg_e2e'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

$VariantArgs = @()
if ($Variant) { $VariantArgs += "--variant=$Variant" }

function Ok([string]$name, [string]$detail = '')
{
    $script:pass++
    if ($detail) { Write-Output ("[ok]   {0}  {1}" -f $name, $detail) } else { Write-Output ("[ok]   {0}" -f $name) }
}
function Bad([string]$name, [string]$detail)
{
    $script:fail++
    Write-Output ("[FAIL] {0}  {1}" -f $name, $detail)
}
function Check([string]$name, [bool]$cond, [string]$detail = '')
{
    if ($cond) { Ok $name $detail } else { Bad $name $detail }
}

# 本地跑一个脚本，拿 stdout+stderr（两种变体都当文本读：断言只用 ASCII 子串）
function Run-Local([string]$script, [int]$TimeoutMs = 30000)
{
    $out = Join-Path $TmpDir 'out.txt'
    $err = Join-Path $TmpDir 'err.txt'
    Remove-Item $out, $err -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $Exe -ArgumentList (@($VariantArgs) + @($script)) `
        -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -NoNewWindow
    $null = $p.WaitForExit($TimeoutMs)
    $o = if (Test-Path $out) { Get-Content $out -Raw } else { '' }
    $e = if (Test-Path $err) { Get-Content $err -Raw } else { '' }
    return [pscustomobject]@{ code = $(if ($p.HasExited) { $p.ExitCode } else { -999 }); out = $o; err = $e }
}
function Write-Lua([string]$name, [string]$text)
{
    $path = Join-Path $TmpDir $name
    [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
    return $path
}

# ---------------------------------------------------------------- ① caps / 能力
$s_caps = Write-Lua 'caps.lua' @'
local c = dbg.caps()
print(string.format('arch=%s variant=%s flavor=%s win9x=%s attach=%s detach=%s hits_cap=%d',
    c.arch, c.variant, c.flavor, tostring(c.win9x), tostring(c.attach), tostring(c.detach), c.hits_cap))
-- 「caps 报的能力」与「表成员」必须一致（否则就有第二真相源）
print('detach_member=' .. tostring(dbg.detach ~= nil) .. ' close_member=' .. tostring(dbg.close))
'@
$r = Run-Local $s_caps
$arch = if ($Render -eq '64') { 'x64' } else { 'x86' }
$variant = if ($Variant) { $Variant } else { 'modern' }
Check 'caps.arch-variant' ($r.out -match "arch=$arch variant=$variant flavor=lua") "expected arch=$arch variant=$variant; out=[$($r.out)]"
Check 'caps.detach' ($r.out -match 'win9x=false attach=true detach=true hits_cap=64') "out=[$($r.out)]"
Check 'detach.capability' ($r.out -match 'detach_member=true close_member=nil') `
    "dbg.detach 必须存在、dbg.close 必须已删（D51）；out=[$($r.out)]"

# ---------------------------------------------------------------- ② help
$s_help = Write-Lua 'help.lua' @'
print('idx=' .. tostring(help()))
print('group=' .. tostring(help('dbg')))
print('topic=' .. tostring(help('dbg.pump')))
local ok, err = help('no-such-topic')
print('unknown=' .. tostring(ok) .. ' ' .. tostring(err))
print('dbg_help=' .. tostring(dbg.help()))
'@
$r = Run-Local $s_help
# ❗只断言 ASCII 部分：legacy 变体的中文是 GBK 字节（e2e_tcp.ps1 的既有口径）
Check 'help.index' ($r.out -match 'idx=true' -and $r.out -match "help\('dbg\.pump'\)" -and `
    $r.out -match '(?m)^\s+dbg\s{2,}\S') "out=[$($r.out)]"
Check 'help.group' ($r.out -match 'group=true' -and $r.out -match 'dbg\.detach\s+') "out=[$($r.out)]"
Check 'help.topic' ($r.out -match 'topic=true' -and $r.out -match 'autocont') "out=[$($r.out)]"
Check 'help.unknown-topic' ($r.out -match "unknown=nil no help for 'no-such-topic' \(try help\(\)\)") "out=[$($r.out)]"
# 管道 ⇒ 不是控制台 ⇒ 必须纯文本（切片 B 上色后这条仍然要过）
$hasEsc = ($r.out + $r.err).Contains([char]27)
Check 'help.plain-when-piped' ((-not $hasEsc) -and ($r.out -match 'dbg_help=true')) `
    "has_esc=$hasEsc out=[$($r.out)]"

# ---------------------------------------------------------------- ③ kill
$fx = Join-Path $Root '.out/fixtures/x86_loop.exe'
if ($Render -ne '86' -or -not (Test-Path $fx))
{
    Write-Output "SKIP: kill.* 需要 x86 的 fixture（.out/fixtures/x86_loop.exe）"
}
else
{
    $fxLua = $fx.Replace('\', '/')
    $s_kill = Write-Lua 'kill.lua' @"
local ok, err = dbg.create{ imagepath = '$fxLua' }
print('create=' .. tostring(ok) .. ' err=' .. tostring(err))
if not ok then return end
local tpid
for i = 1, 12 do
    local code = dbg.pump(2000, false)
    if code == nil or code == 0 then break end
    local h = dbg.hits()
    if #h >= 1 then tpid = h[1].pid break end
    dbg.cont(nil, nil, 0x00010002)
end
print('pid=' .. tostring(tpid))
print('kill=' .. tostring(dbg.kill()))
print('after=' .. tostring(select(2, dbg.pump(10, false))))
"@
    $r = Run-Local $s_kill
    $m = [regex]::Match($r.out, 'pid=(\d+)')
    $tpid = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
    Start-Sleep -Milliseconds 400
    $alive = $false
    if ($tpid -gt 0) { $alive = $null -ne (Get-Process -Id $tpid -ErrorAction SilentlyContinue) }
    Check 'kill.terminates' ($r.out -match 'create=true' -and $r.out -match 'kill=true' -and $tpid -gt 0 -and -not $alive) `
        "pid=$tpid alive=$alive out=[$($r.out)]"
    Check 'kill.state-reset' ($r.out -match 'after=dbg: no session \(call dbg.create or dbg.attach first\)') "out=[$($r.out)]"
    if ($alive) { Stop-Process -Id $tpid -Force -ErrorAction SilentlyContinue }
}

Write-Output ''
Write-Output ("e2e_dbg: variant={0} pass={1} fail={2}" -f $(if ($Variant) { $Variant } else { 'auto' }), $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
