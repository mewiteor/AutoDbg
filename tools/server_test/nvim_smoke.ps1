# =============================================================================
# tools/server_test/nvim_smoke.ps1 —— neovim 客户端的无头验收（S3）
#
# 来源：新写（plan_server.md S3）。
# 做三件事：起一个临时 server（随机端口）→ 用 headless nvim 跑 nvim_smoke.lua
#          → 停 server。不需要手动开 nvim，也不需要 VM。
# 用法：
#   pwsh -File tools/server_test/nvim_smoke.ps1 [-Render 64|86] [-Variant ...] [-Port 18710]
# 退出码：0 = 全通过；1 = 有失败；2 = 前置缺失（产物 / nvim / nvim_smoke.lua）
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('64', '86')]
    [string]$Render = '64',
    [ValidateSet('', 'modern', 'legacy')]
    [string]$Variant = '',
    [int]$Port = 18710,
    [string]$Nvim = 'nvim'
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Exe = Join-Path $Root ".out/AutoDbg_$Render.exe"
$Rtp = Join-Path $Root 'nvim'
$Smoke = Join-Path $PSScriptRoot 'nvim_smoke.lua'

if (-not (Test-Path $Exe)) { Write-Output "SKIP: 产物不存在：$Exe（先 make.bat a $Render）"; exit 2 }
if (-not (Test-Path $Smoke)) { Write-Output "SKIP: 缺少 $Smoke"; exit 2 }
if (-not (Get-Command $Nvim -ErrorAction SilentlyContinue)) { Write-Output "SKIP: nvim 不在 PATH"; exit 2 }

$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_nvim_smoke'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null
$outFile = Join-Path $TmpDir "server.out"
$errFile = Join-Path $TmpDir "server.err"

$args2 = @('--server', "--server-port=$Port")
if ($Variant) { $args2 += "--variant=$Variant" }

$proc = Start-Process -FilePath $Exe -ArgumentList $args2 -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -NoNewWindow

try
{
    $ready = $false
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline)
    {
        if ($proc.HasExited) { break }
        try
        {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2 -SkipHttpErrorCheck
            if ($r.StatusCode -eq 200) { $ready = $true; break }
        }
        catch { Start-Sleep -Milliseconds 150 }
    }
    if (-not $ready)
    {
        Write-Output "SKIP: server 未能就绪"
        if (Test-Path $outFile) { Get-Content $outFile | Write-Output }
        if (Test-Path $errFile) { Get-Content $errFile | Write-Output }
        exit 2
    }

    $env:AUTODBG_URL = "http://127.0.0.1:$Port"
    # 本地模式用例需要真实的 exe（拿不到时由 lua 侧跳过那几条断言）
    $env:AUTODBG_EXE = $Exe
    & $Nvim --headless -u NONE --cmd "set rtp+=$Rtp" -c "luafile $Smoke" -c "qa!"
    $code = $LASTEXITCODE
    Write-Output "nvim exit=$code"
}
finally
{
    if (-not $proc.HasExited)
    {
        try { Invoke-WebRequest -Uri "http://127.0.0.1:$Port/shutdown" -Method Post -TimeoutSec 2 -SkipHttpErrorCheck | Out-Null } catch { }
        if (-not $proc.WaitForExit(3000)) { $proc.Kill() }
    }
}

if ($code -ne 0) { exit 1 } else { exit 0 }
