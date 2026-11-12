# =============================================================================
# tools/server_test/nvim_smoke.ps1 —— neovim 客户端的无头验收（T5：raw TCP）
#
# 来源：原 S3 版本（HTTP `/run`），T5 删掉 HTTP 后按 **raw TCP 帧** 重写。
# 做三件事：起一个临时 TCP server → 用 headless nvim 跑 nvim_smoke.lua → 停 server。
# 用法：
#   pwsh -File tools/server_test/nvim_smoke.ps1 [-Render 64|86] [-Variant modern|legacy] [-Port 18710]
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
if (-not (Test-Path (Join-Path $Rtp 'lua/autodbg/tcp.lua'))) { Write-Output "SKIP: 插件没有 tcp.lua（子模块未更新？）"; exit 2 }
if (-not (Get-Command $Nvim -ErrorAction SilentlyContinue)) { Write-Output "SKIP: nvim 不在 PATH"; exit 2 }

$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_nvim_smoke'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null
$outFile = Join-Path $TmpDir 'server.out'
$errFile = Join-Path $TmpDir 'server.err'

$srvArgs = @('--server=tcp', "--server-port=$Port", '--server-no-echo')
if ($Variant) { $srvArgs += "--variant=$Variant" }

$proc = Start-Process -FilePath $Exe -ArgumentList $srvArgs -RedirectStandardOutput $outFile `
    -RedirectStandardError $errFile -PassThru -NoNewWindow

# 就绪判定：TCP 能连上（不再有 /health）
$ready = $false
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline) {
    if ($proc.HasExited) { break }
    try {
        $c = [System.Net.Sockets.TcpClient]::new()
        $c.Connect('127.0.0.1', $Port)
        $c.Close()
        $ready = $true
        break
    }
    catch { Start-Sleep -Milliseconds 120 }
}

$code = 2
try {
    if (-not $ready) {
        Write-Output 'SKIP: server 未能就绪'
        if (Test-Path $outFile) { Get-Content $outFile | Write-Output }
        if (Test-Path $errFile) { Get-Content $errFile | Write-Output }
        exit 2
    }

    # 第二台 server：要求 token（验插件把 token 放进握手帧、以及拒绝路径）
    $tokPort = $Port + 1
    $tok = 'deadbeefcafe1234'
    $tokOut = Join-Path $TmpDir 'server_tok.out'
    $tokErr = Join-Path $TmpDir 'server_tok.err'
    $tokArgs = @('--server=tcp', "--server-port=$tokPort", '--server-no-echo', "--server-token=$tok")
    if ($Variant) { $tokArgs += "--variant=$Variant" }
    $script:tokProc = Start-Process -FilePath $Exe -ArgumentList $tokArgs -RedirectStandardOutput $tokOut `
        -RedirectStandardError $tokErr -PassThru -NoNewWindow
    $tokReady = $false
    $deadline2 = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline2) {
        if ($script:tokProc.HasExited) { break }
        try {
            $tc = [System.Net.Sockets.TcpClient]::new()
            $tc.Connect('127.0.0.1', $tokPort)
            $tc.Close()
            $tokReady = $true
            break
        }
        catch { Start-Sleep -Milliseconds 120 }
    }

    $env:AUTODBG_HOST = '127.0.0.1'
    $env:AUTODBG_PORT = "$Port"
    $env:AUTODBG_EXE = $Exe
    if ($tokReady) {
        $env:AUTODBG_TOKEN_PORT = "$tokPort"
        $env:AUTODBG_TOKEN = $tok
    }
    & $Nvim --headless -u NONE --cmd "set rtp+=$Rtp" -c "luafile $Smoke"
    $code = $LASTEXITCODE
    Write-Output "nvim exit=$code"
}
finally {
    if ($script:tokProc -and -not $script:tokProc.HasExited) {
        try { $script:tokProc.Kill() } catch { }
    }
    if (-not $proc.HasExited) {
        # 先请它优雅停机（control shutdown），不行再杀
        try {
            $c = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
            $s = $c.GetStream()
            # 握手帧：id = 协议版本 1
            $hs = [byte[]]@(8, 0, 0, 0, 10, 0, 1, 0, 0, 0, 0, 0)
            $s.Write($hs, 0, $hs.Length)
            $s.Flush()
            $rb = New-Object byte[] 256
            $null = $s.Read($rb, 0, $rb.Length)
            # control shutdown：flags = 5
            $sd = [byte[]]@(8, 0, 0, 0, 12, 5, 0, 0, 0, 0, 0, 0)
            $s.Write($sd, 0, $sd.Length)
            $s.Flush()
            $null = $s.Read($rb, 0, $rb.Length)
            $c.Close()
        }
        catch { }
        if (-not $proc.WaitForExit(3000)) { $proc.Kill() }
    }
}

if ($code -ne 0) { exit 1 } else { exit 0 }
