#!/usr/bin/env pwsh
# =============================================================================
# tools/w9x_test/w9x.ps1 —— Win9x 实机测试 harness（P7 / plan.md §1.3）
#
# 分工：脚本跑在 host（PowerShell 7），被测产物跑在 guest（Win98），全部走 vmrun。
# 凭据：不入库。优先 config.local.ps1（已 .gitignore），其次环境变量
#       W9X_GUEST_USER / W9X_GUEST_PASS。
#
# 每轮流程（§6）：静态门禁（§16，host 侧，便宜）→ revert 快照 1 → start → 等 Tools → 跑 case
#
# 实测口径（W0/W1/W3 已核实，见 plan.md §1.3 与 PITFALLS.md 的 W 节）：
#   ① `runProgramInGuest` 会传递**真实程序**的退出码（AutoDbg 脚本 error → vmrun 返回 2）。
#   ② `command.com /c <命令>` 经 vmrun 跑时**不执行命令**（只返回 0）→ 不能用批处理
#      捕获退出码或做重定向；退出码只能靠 vmrun 本身拿。
#   ③ guest 里没有控制台 → `WriteConsole` 路径不可用；结果靠脚本写文件（路径走 vararg）。
#   ④ guest 里不能建目录（vmrun 没有 mkdir，command.com 又跑不了）→ 用已存在的目录
#      （默认 `C:\WINDOWS\TEMP`）。
#   ⑤ 快照只用既有的 `1`，不新建。❗快照 `1` 是**关机态**：`revertToSnapshot` 会把 VM 关掉，
#      必须 `start` + 等 Tools 就绪；guest 里卡住的进程从 host 杀不掉，只能靠回退恢复。
#   ⑥ guest 里不能删文件：同名文件被上一次卡住的进程锁住 → copy 报「access rights」，
#      所以 guest 侧用固定名 `w9x_exe.exe`。
#   ⑦ guest 里调试会话不能结束（CheckConsoleInput 无输入事件 → 永远 CONTINUE）→ 探针进程
#      不退出 → 判定只能靠结果文件内容，退出码不可靠（超时记 `exit=timeout`）。
#
# 门禁口径：VM 不可用 / 凭据不可用 / vmrun 缺失 / Tools 未就绪 → **SKIP（退出码 2）**，不算失败。
# 退出码口径：0 = PASS / 1 = FAIL / 2 = SKIP（给上层门禁脚本留出 SKIP 的映射空间）。
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('check', 'run', 'record', 'revert', 'gate')]
    [string]$Action = 'check',
    [string]$Test = 'probe',
    [string]$Flavor = '',
    [string]$Variant = '',
    [int]$TimeoutSec = 120,
    [switch]$NoPrepare,
    [switch]$NoGate
)

$ErrorActionPreference = 'Stop'

$VMRUN     = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
$VMX       = 'E:\Software\VM\Windows 98 中文第二版\Windows 98 中文第二版.vmx'
$SNAP      = '1'
$GUEST_DIR = 'C:\WINDOWS\TEMP'
$ROOT      = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$GOLDEN    = Join-Path $ROOT 'tests/golden/w9x'
$HOST_DIR  = Join-Path ([System.IO.Path]::GetTempPath()) 'w9x_test'

if (Test-Path (Join-Path $PSScriptRoot 'config.local.ps1'))
{
    . (Join-Path $PSScriptRoot 'config.local.ps1')
}
$GU = if ($env:W9X_GUEST_USER) { $env:W9X_GUEST_USER } else { if ($GU) { $GU } else { 'Windows 98' } }
$GP = if ($env:W9X_GUEST_PASS) { $env:W9X_GUEST_PASS } else { if ($GP) { $GP } else { '' } }

function Skip([string]$why)
{
    Write-Output "SKIP: $why"
    exit 2    # VM / 凭据 / Tools 不可用 → SKIP（0=PASS / 1=FAIL / 2=SKIP）
}

# 跑一条 vmrun，返回 @{ Code; Out }；超时 → Code = 258
function V([string[]]$args2, [int]$TimeoutSec = $TimeoutSec)
{
    $job = Start-Job -ArgumentList $VMRUN, $args2 -ScriptBlock {
        param($exe, $a)
        $out = & $exe @a
        [pscustomobject]@{ Out = ($out -join "`n"); Code = $LASTEXITCODE }
    }
    if (Wait-Job $job -Timeout $TimeoutSec)
    {
        $r = Receive-Job $job
        Remove-Job $job
        return $r
    }
    Remove-Job $job -Force
    return [pscustomobject]@{ Out = "timeout after ${TimeoutSec}s"; Code = 258 }
}

function GuestArgs([string[]]$body)
{
    return @('-gu', $GU, '-gp', $GP) + $body
}

# D15 归一化（字节级）：CRLF → LF、去尾部空行。结果文件里可能是 GBK 字节，所以不能按
# 文本解码比较 —— 一律按字节。
function NormBytes([byte[]]$b)
{
    $out = New-Object System.Collections.Generic.List[byte]
    for ($i = 0; $i -lt $b.Length; $i++)
    {
        if ($b[$i] -eq 0x0D -and $i + 1 -lt $b.Length -and $b[$i + 1] -eq 0x0A) { $out.Add([byte]0x0A); $i++ }
        else { $out.Add($b[$i]) }
    }
    while ($out.Count -gt 0 -and ($out[$out.Count - 1] -eq [byte]0x0A -or $out[$out.Count - 1] -eq [byte]0x0D))
    {
        $out.RemoveAt($out.Count - 1)
    }
    return $out.ToArray()
}

function ShowBytes([byte[]]$b)
{
    # 比较失败时打印可读形式：能按 UTF-8 解就读 UTF-8，否则按 GBK
    $u = [System.Text.Encoding]::UTF8.GetString($b)
    if ($u.Contains([char]0xFFFD)) { return [System.Text.Encoding]::GetEncoding(936).GetString($b) }
    return $u
}

function VmRunning
{
    $list = (& $VMRUN list) -join "`n"
    return ($list -match [regex]::Escape($VMX))
}

# §16 静态门禁（host 侧，便宜，每轮先跑）
function Gate
{
    if (-not (Get-Command uv -ErrorAction SilentlyContinue)) { Write-Output 'gate: SKIP（uv 不在 PATH）'; return 0 }
    $out = & uv --directory (Join-Path $ROOT 'tools/api_check') run scan_dir.py
    $code = $LASTEXITCODE
    Write-Output "gate: $out (exit=$code)"
    return $code
}

function WaitTools([int]$Seconds = 180)
{
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline)
    {
        $r = V (GuestArgs @('copyFileFromGuestToHost', $VMX, 'C:\WINDOWS\win.ini', (Join-Path $HOST_DIR 'win.ini')))
        if ($r.Code -eq 0) { return $true }
        Start-Sleep -Seconds 10
    }
    return $false
}

# 每轮开头：revert 快照 1 → start（快照是关机态，回退会把 VM 关掉）→ 等 Tools
function PrepareRound
{
    if (-not (Test-Path $VMRUN)) { Skip "vmrun 不存在：$VMRUN" }

    if (VmRunning)
    {
        $r = V @('revertToSnapshot', $VMX, $SNAP) -TimeoutSec 180
        Write-Output "revert($SNAP): exit=$($r.Code) $($r.Out)"
    }

    if (-not (VmRunning))
    {
        $r = V @('start', $VMX, 'nogui') -TimeoutSec 180
        Write-Output "start: exit=$($r.Code) $($r.Out)"
    }

    if (-not (WaitTools)) { Skip "Tools 未就绪（等 180s 超时）" }
    Write-Output "guest ready（快照 $SNAP 已回退）"
}

# ---- W0 能力检查 ----
function Check
{
    if (-not (Test-Path $VMRUN)) { Skip "vmrun 不存在：$VMRUN" }
    New-Item -ItemType Directory -Force -Path $HOST_DIR | Out-Null

    if (-not $NoGate) { Gate }

    if (-not (VmRunning)) { Skip "VM 未在运行（快照 1 是关机态，先跑 -Action run 或手动 vmrun start）" }

    $ip = V (GuestArgs @('getGuestIPAddress', $VMX))
    if ($ip.Code -ne 0) { Skip "凭据不可用或 Tools 未运行（getGuestIPAddress: $($ip.Out)）" }

    Write-Output "vmrun   : $VMRUN"
    Write-Output "VM      : $VMX"
    Write-Output "guestIP : $($ip.Out)"
    Write-Output "snapshot: $SNAP（只用既有快照；它是关机态）"

    $r = V (GuestArgs @('copyFileFromGuestToHost', $VMX, 'C:\WINDOWS\win.ini', (Join-Path $HOST_DIR 'win.ini')))
    if ($r.Code -ne 0) { Skip "guest 文件读取失败：$($r.Out)" }
    Write-Output "guest 往返: OK（win.ini）"
    exit 0
}

# ---- W1/W2/W3 跑一个 case ----
function RunTest([bool]$Record)
{
    New-Item -ItemType Directory -Force -Path $HOST_DIR | Out-Null
    New-Item -ItemType Directory -Force -Path $GOLDEN   | Out-Null

    if (-not $NoPrepare) { PrepareRound }
    if (-not $NoGate)     { Gate }

    $lua = Join-Path $PSScriptRoot (Join-Path 'scripts' ($Test + '.lua'))
    if (-not (Test-Path $lua)) { Skip "测试脚本不存在：$lua" }

    $exe = Join-Path $ROOT '.out/AutoDbg_86.exe'
    if (-not (Test-Path $exe)) { Skip "产物不存在：$exe（先 make.bat a 86）" }

    $gexe = Join-Path $GUEST_DIR 'w9x_exe.exe'
    $glua = Join-Path $GUEST_DIR ($Test + '.lua')
    $gres = Join-Path $GUEST_DIR ($Test + '.result.txt')

    $r = V (GuestArgs @('copyFileFromHostToGuest', $VMX, $exe, $gexe))
    if ($r.Code -ne 0) { Skip "送 exe 失败：$($r.Out)" }
    # 整个 scripts/ 都送过去：探针之间会互相调用（例如 w3_dbghelp 起一个跑 sleep.lua 的子实例）
    foreach ($s in Get-ChildItem (Join-Path $PSScriptRoot 'scripts') -Filter '*.lua')
    {
        $r = V (GuestArgs @('copyFileFromHostToGuest', $VMX, $s.FullName, (Join-Path $GUEST_DIR $s.Name)))
        if ($r.Code -ne 0) { Skip "送脚本 $($s.Name) 失败：$($r.Out)" }
    }

    $argv = GuestArgs @('runProgramInGuest', $VMX, $gexe)
    if ($Flavor)  { $argv += "--flavor=$Flavor" }
    if ($Variant) { $argv += "--variant=$Variant" }
    $argv += $glua
    $argv += $gres                       # 脚本用 vararg 拿结果路径

    $run = V $argv
    $code = if ($run.Code -eq 258) { 'timeout' } else { $run.Code }
    Write-Output "exit=$code"

    $hres = Join-Path $HOST_DIR ($Test + '.result.txt')
    Remove-Item $hres -ErrorAction SilentlyContinue
    $back = V (GuestArgs @('copyFileFromGuestToHost', $VMX, $gres, $hres))
    if ($back.Code -ne 0) { Write-Output "结果文件取回失败（$($back.Out)）；只有退出码可用" }

    $body = if (Test-Path $hres) { NormBytes ([System.IO.File]::ReadAllBytes($hres)) } else { [byte[]]@() }
    $full = NormBytes ([System.Text.Encoding]::UTF8.GetBytes("exit=$code`n") + $body)
    $gfile = Join-Path $GOLDEN ($Test + '.txt')

    if ($Record)
    {
        [System.IO.File]::WriteAllBytes($gfile, $full + [byte[]]@([byte]0x0A))
        Write-Output "golden 已录：$gfile"
        exit 0
    }

    if (-not (Test-Path $gfile)) { Skip "golden 不存在：$gfile（先 -Action record）" }

    $want = NormBytes ([System.IO.File]::ReadAllBytes($gfile))
    if ($want.Length -eq $full.Length -and (($want -join ',') -eq ($full -join ',')))
    {
        Write-Output "PASS"
        exit 0
    }

    Write-Output "FAIL"
    Write-Output "--- golden ---"
    Write-Output (ShowBytes $want)
    Write-Output "--- actual ---"
    Write-Output (ShowBytes $full)
    exit 1
}

if ($Action -eq 'check') { Check }
elseif ($Action -eq 'gate') { exit (Gate) }
elseif ($Action -eq 'revert') { PrepareRound }
else { RunTest ($Action -eq 'record') }
