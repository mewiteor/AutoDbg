# =============================================================================
# tools/server_test/e2e_tcp.ps1 —— raw TCP 帧服务端 + 客户端的端到端断言（T2/T3）
#
# 来源：新写（plan_cli.md §7 的 e2e_tcp.ps1；T2 = 握手/帧/control，T3 = `--remote` 客户端）。
# 依赖：只需 PowerShell 7 + 已构建的 `.out/AutoDbg_<arch>.exe`；不需要 VM、不需要模型。
# 用法：
#   pwsh -File tools/server_test/e2e_tcp.ps1 [-Render 64|86] [-Variant modern|legacy]
# 退出码：0 = 全通过；1 = 有失败；2 = 前置缺失（产物不存在 / x64+legacy 组合）
#
# 帧布局（见 src/common/frame.hpp 与 src/scripting/lua/stream.hpp）：
#   u32 payload_len | u8 type | u8 flags | u16 id | u32 data_len | [json] | [data]
#   握手帧：id = 协议版本（1），data = token，json = 其余字段（本篇不用）
#   control 帧：flags = 命令（0 cancel / 1 kill / 2 detach / 3 health / 4 version / 5 shutdown）
#   → 应答：ack = 8（flags bit0 = applied）、info = 9、error = 6、result = 5（id = 退出码）
#
# 覆盖：
#   ① 握手成功 → info 帧（proto=1、transport=tcp）
#   ② 版本不符 → error{bad-version} 且连接被关
#   ③ 第一帧不是握手 → error{handshake-required}
#   ④ script 帧 → out 帧（stdout/stderr 分桶）+ result + end
#   ⑤ 语法错误 → result ok=false 且 exit 透传 Lua 错误码（3）
#   ⑥ control health / version → info（替代 GET /health、/version）
#   ⑦ control cancel（无执行）→ ack{applied:false, reason:"no-execution"}
#   ⑧ 未知 control 命令 → ack{applied:false, reason:"unknown-cmd"}
#   ⑨ 显式 token：错 token → error{unauthorized}；对 token → info
#   ⑩ control shutdown → ack + 进程退出码 0
#   ⑪ **执行期的 control**（D32 的两个 poll 点）：Lua 死循环里 cancel、引擎空转里
#      cancel / kill / detach —— 都不需要等脚本跑完
#   ⑫ 客户端 `--remote`（T3）：跑脚本 / `--json` 信封 / 语法错误透传 / health / version /
#      **另一个进程的 `--cancel`**（accept 线程接控制连接）/ 无 `--remote` 时拒绝控制命令
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('64', '86')]
    [string]$Render = '64',
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
    Write-Output 'SKIP: x64 没有 legacy 变体（AGENTS.md §11.1）；legacy 口径请用 -Render 86 -Variant legacy'
    exit 2
}

$script:pass = 0
$script:fail = 0
$Port = 18771
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_tcp_e2e'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

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

# ---------------------------------------------------------------- 帧编解码
# ❗不用 List.AddRange：PowerShell 把「空数组」绑成缺参（PITFALLS X10）。
function Send-Frame($s, [int]$Type, [int]$Flags, [int]$Id, [string]$Json, [byte[]]$Data)
{
    $jb = if ($Json) { [System.Text.Encoding]::UTF8.GetBytes($Json) } else { [byte[]]@() }
    if ($null -eq $Data) { $Data = [byte[]]@() }
    $payload = 8 + $jb.Length + $Data.Length
    $buf = New-Object System.Collections.Generic.List[byte]
    foreach ($b in [BitConverter]::GetBytes([uint32]$payload)) { $buf.Add($b) }
    $buf.Add([byte]$Type)
    $buf.Add([byte]$Flags)
    foreach ($b in [BitConverter]::GetBytes([uint16]$Id)) { $buf.Add($b) }
    foreach ($b in [BitConverter]::GetBytes([uint32]$Data.Length)) { $buf.Add($b) }
    foreach ($b in $jb) { $buf.Add($b) }
    foreach ($b in $Data) { $buf.Add($b) }
    $arr = $buf.ToArray()
    $s.Write($arr, 0, $arr.Length)
    $s.Flush()
}
function Recv-Exact($s, [byte[]]$Buf)
{
    $rd = 0
    while ($rd -lt $Buf.Length)
    {
        try { $n = $s.Read($Buf, $rd, $Buf.Length - $rd) }
        catch { return $false }   # 读超时/断开 → 报 FAIL，而不是把脚本炸掉（PITFALLS X11）
        if ($n -le 0) { return $false }
        $rd += $n
    }
    return $true
}
function Recv-Frame($s)
{
    $pfx = New-Object byte[] 4
    if (-not (Recv-Exact $s $pfx)) { return $null }
    $plen = [BitConverter]::ToUInt32($pfx, 0)
    if ($plen -lt 8 -or $plen -gt 8388608) { return $null }
    $body = New-Object byte[] $plen
    if (-not (Recv-Exact $s $body)) { return $null }
    $type = [int]$body[0]
    $flags = [int]$body[1]
    $id = [int][BitConverter]::ToUInt16($body, 2)
    $dlen = [int][BitConverter]::ToUInt32($body, 4)
    $jlen = $plen - 8 - $dlen
    $json = if ($jlen -gt 0) { [System.Text.Encoding]::UTF8.GetString($body, 8, $jlen) } else { '' }
    $data = if ($dlen -gt 0) { [System.Text.Encoding]::UTF8.GetString($body, 8 + $jlen, $dlen) } else { '' }
    return [pscustomobject]@{ type = $type; flags = $flags; id = $id; json = $json; data = $data }
}
# 读到指定类型的帧为止（其它帧收下但不动）；超时 / 对端关闭 → $null
function Recv-Until($s, [int]$Type, [int]$TimeoutMs = 8000)
{
    $deadline = [datetime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([datetime]::UtcNow -lt $deadline)
    {
        $f = Recv-Frame $s
        if ($null -eq $f) { return $null }
        if ($f.type -eq $Type) { return $f }
    }
    return $null
}

# ---------------------------------------------------------------- 进程管理
function Connect-Retry([int]$TimeoutMs = 5000)
{
    $deadline = [datetime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([datetime]::UtcNow -lt $deadline)
    {
        try
        {
            $c = New-Object System.Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $Port)
            $c.ReceiveTimeout = 8000
            return $c
        }
        catch { Start-Sleep -Milliseconds 80 }
    }
    return $null
}
function Start-Server([string[]]$Extra)
{
    $out = Join-Path $TmpDir 'srv.out'
    $err = Join-Path $TmpDir 'srv.err'
    $args = @('--server=tcp', "--server-port=$Port", '--server-no-echo')
    if ($Variant) { $args += "--variant=$Variant" }
    $args += $Extra
    $script:proc = Start-Process -FilePath $Exe -ArgumentList $args -RedirectStandardOutput $out `
        -RedirectStandardError $err -PassThru -NoNewWindow
    $script:outFile = $out
    $script:errFile = $err
    return (Connect-Retry)
}
function Stop-Server
{
    if ($script:proc -and -not $script:proc.HasExited)
    {
        $script:proc.Kill()
        $script:proc.WaitForExit(2000) | Out-Null
    }
}
function With-Server([string[]]$Extra, [scriptblock]$Body)
{
    $c = Start-Server $Extra
    if ($null -eq $c)
    {
        Bad 'server.start' "connect failed; stderr=$(Get-Content $script:errFile -Raw -ErrorAction SilentlyContinue)"
        return
    }
    try { & $Body $c }
    finally
    {
        try { $c.Close() } catch { }
        Stop-Server
        Start-Sleep -Milliseconds 120   # 让端口释放
    }
}
# 跑一次**命令行客户端**（同步）
function Run-Client([string[]]$ClientArgs, [int]$TimeoutMs = 25000)
{
    $out = Join-Path $TmpDir 'cli.out'
    $err = Join-Path $TmpDir 'cli.err'
    Remove-Item $out, $err -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $Exe -ArgumentList $ClientArgs -RedirectStandardOutput $out `
        -RedirectStandardError $err -PassThru -NoNewWindow
    $null = $p.WaitForExit($TimeoutMs)
    $o = if (Test-Path $out) { Get-Content $out -Raw } else { '' }
    $e = if (Test-Path $err) { Get-Content $err -Raw } else { '' }
    return [pscustomobject]@{ code = $(if ($p.HasExited) { $p.ExitCode } else { -999 }); out = $o; err = $e }
}

# ---------------------------------------------------------------- ① 握手成功
With-Server @() {
    param($c)
    $s = $c.GetStream()
    Send-Frame $s 10 0 1 '' $null
    $f = Recv-Frame $s
    Check 'proto.info' ($f -and $f.type -eq 9) "type=$($f.type)"
    $j = if ($f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'proto.info-fields' ($j.ok -eq $true -and $j.proto -eq 1 -and $j.transport -eq 'tcp') "json=$($f.json)"
}

# ------------------------------------------------- ② 版本不符 → error 并断开
With-Server @() {
    param($c)
    $s = $c.GetStream()
    Send-Frame $s 10 0 99 '' $null
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'handshake.bad-version' ($f -and $f.type -eq 6 -and $j.error -eq 'bad-version') "frame=$($f.type) json=$($f.json)"
    Check 'handshake.bad-version-values' ($j.want -eq 1 -and $j.got -eq 99) "want=$($j.want) got=$($j.got)"
    $next = Recv-Frame $s
    Check 'handshake.bad-version-closes' ($null -eq $next) 'connection should be closed'
}

# ------------------------------------------- ③ 第一帧不是握手
With-Server @() {
    param($c)
    $s = $c.GetStream()
    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes('print("x")'))
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'handshake.required' ($f -and $f.type -eq 6 -and $j.error -eq 'handshake-required') "json=$($f.json)"
}

# ---------------------------------------- ④⑤ script 帧：out/result/end + 语法错误
With-Server @() {
    param($c)
    $s = $c.GetStream()
    Send-Frame $s 10 0 1 '' $null
    $null = Recv-Frame $s

    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes('print("hi")'))
    $outText = ''
    $result = $null
    $sawEnd = $false
    for ($i = 0; $i -lt 8; $i++)
    {
        $f = Recv-Frame $s
        if ($null -eq $f) { break }
        if ($f.type -eq 4) { $outText += $f.data }
        elseif ($f.type -eq 5) { $result = $f.json | ConvertFrom-Json }
        elseif ($f.type -eq 14) { $sawEnd = $true; break }
    }
    Check 'script.out-frame' ($outText -eq "hi`n") "stdout=[$outText]"
    Check 'script.result' ($result -and $result.ok -eq $true -and $result.exit -eq 0) "result=$($result | ConvertTo-Json -Compress)"
    Check 'script.end' $sawEnd 'expected end frame (14)'

    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes('local x = '))
    $result2 = $null
    for ($i = 0; $i -lt 8; $i++)
    {
        $f = Recv-Frame $s
        if ($null -eq $f) { break }
        if ($f.type -eq 5) { $result2 = $f.json | ConvertFrom-Json }
        elseif ($f.type -eq 14) { break }
    }
    Check 'script.syntax-error' ($result2 -and $result2.ok -eq $false -and $result2.exit -eq 3 -and $result2.error.Length -gt 0) `
        "result=$($result2 | ConvertTo-Json -Compress)"
}

# ------------------------ ⑥⑦⑧⑩ control：health / version / cancel / 未知 / shutdown
With-Server @() {
    param($c)
    $s = $c.GetStream()
    Send-Frame $s 10 0 1 '' $null
    $null = Recv-Frame $s

    Send-Frame $s 12 3 0 '' $null
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'control.health' ($f.type -eq 9 -and $j.ok -eq $true) "json=$($f.json)"

    Send-Frame $s 12 4 0 '' $null
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'control.version' ($f.type -eq 9 -and $j.version.Length -gt 0 -and $j.abi -eq 4 -and $j.arch -match '^(x64|x86)$') "json=$($f.json)"

    Send-Frame $s 12 0 0 '' $null
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'control.cancel-idle' ($f.type -eq 8 -and $j.applied -eq $false -and $j.reason -eq 'no-execution') "json=$($f.json)"

    Send-Frame $s 12 99 0 '' $null
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'control.unknown-cmd' ($f.type -eq 8 -and $j.applied -eq $false -and $j.reason -eq 'unknown-cmd') "json=$($f.json)"

    Send-Frame $s 12 5 0 '' $null
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'control.shutdown-ack' ($f.type -eq 8 -and $j.applied -eq $true -and $j.reason -eq 'shutdown') "json=$($f.json)"
    $null = $script:proc.WaitForExit(5000)
    Check 'control.shutdown-exits' ($script:proc.HasExited -and $script:proc.ExitCode -eq 0) `
        "exited=$($script:proc.HasExited) code=$(if ($script:proc.HasExited) { $script:proc.ExitCode } else { 'n/a' })"
    $script:proc = $null
}

# ------------------------------------------ ⑪ 执行期的 control（D32 两个 poll 点）
# ❗命令用 Lua 长括号：here-string 里的路径是字面的（PITFALLS X11）。
$RunLongSrc = @'
-- 引擎会一直阻塞在 WaitForDebugEvent（目标闲置）→ on_wait_timeout 的 100 ms poll
Run({ cmdline = [[C:\Windows\System32\cmd.exe]] .. ' /c ping -n 5 127.0.0.1' })
'@
$BusySrc = 'while true do end'   # 纯 Lua 死循环 → 只能靠 lua_sethook 的计数钩子

With-Server @() {
    param($c)
    $s = $c.GetStream()
    Send-Frame $s 10 0 1 '' $null
    $null = Recv-Frame $s

    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes($BusySrc))
    Start-Sleep -Milliseconds 400
    Send-Frame $s 12 0 0 '' $null
    $ack = Recv-Until $s 8 8000
    $aj = if ($ack) { $ack.json | ConvertFrom-Json } else { $null }
    Check 'cancel.lua-loop-ack' ($ack -and $aj.applied -eq $true -and $aj.reason -eq 'cancel-requested') "json=$($ack.json)"
    $res = Recv-Until $s 5 8000
    $rj = if ($res) { $res.json | ConvertFrom-Json } else { $null }
    Check 'cancel.lua-loop-result' ($rj -and $rj.ok -eq $false -and $rj.canceled -eq $true) "json=$($res.json)"
    $null = Recv-Until $s 14 3000

    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes($RunLongSrc))
    Start-Sleep -Milliseconds 600
    Send-Frame $s 12 0 0 '' $null
    $ack = Recv-Until $s 8 8000
    $aj = if ($ack) { $ack.json | ConvertFrom-Json } else { $null }
    Check 'cancel.engine-wait-ack' ($ack -and $aj.applied -eq $true) "json=$($ack.json)"
    $res = Recv-Until $s 5 8000
    $rj = if ($res) { $res.json | ConvertFrom-Json } else { $null }
    Check 'cancel.engine-wait-result' ($rj -and $rj.canceled -eq $true) "json=$($res.json)"
    $null = Recv-Until $s 14 3000

    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes($RunLongSrc))
    Start-Sleep -Milliseconds 600
    Send-Frame $s 12 1 0 '' $null
    $ack = Recv-Until $s 8 8000
    $aj = if ($ack) { $ack.json | ConvertFrom-Json } else { $null }
    Check 'kill.engine-wait-ack' ($ack -and $aj.applied -eq $true -and $aj.reason -eq 'requested') "json=$($ack.json)"
    $res = Recv-Until $s 5 8000
    $rj = if ($res) { $res.json | ConvertFrom-Json } else { $null }
    Check 'kill.engine-wait-result' ($rj -and $rj.ok -eq $true) "json=$($res.json)"
    $null = Recv-Until $s 14 3000

    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes($RunLongSrc))
    Start-Sleep -Milliseconds 600
    Send-Frame $s 12 2 0 '' $null
    $ack = Recv-Until $s 8 8000
    $aj = if ($ack) { $ack.json | ConvertFrom-Json } else { $null }
    Check 'detach.engine-wait-ack' ($ack -and $aj.applied -eq $true -and $aj.reason -eq 'requested') "json=$($ack.json)"
    $res = Recv-Until $s 5 8000
    $rj = if ($res) { $res.json | ConvertFrom-Json } else { $null }
    Check 'detach.engine-wait-result' ($rj -and $rj.ok -eq $true) "json=$($res.json)"
    $null = Recv-Until $s 14 3000
}

# --------------------------------------- ⑨ 显式 token：错的被拒、对的通过（D21）
$tok = 'deadbeefcafe1234'
With-Server @("--server-token=$tok") {
    param($c)
    $s = $c.GetStream()
    Send-Frame $s 10 0 1 '' ([System.Text.Encoding]::UTF8.GetBytes('wrong-token'))
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'auth.bad-token' ($f.type -eq 6 -and $j.error -eq 'unauthorized') "json=$($f.json)"
}
With-Server @("--server-token=$tok") {
    param($c)
    $s = $c.GetStream()
    Send-Frame $s 10 0 1 '' ([System.Text.Encoding]::UTF8.GetBytes($tok))
    $f = Recv-Frame $s
    $j = if ($f -and $f.json) { $f.json | ConvertFrom-Json } else { $null }
    Check 'auth.good-token' ($f.type -eq 9 -and $j.ok -eq $true -and $j.auth -eq $true) "json=$($f.json)"
}

# ------------------------------------------- ⑫ 客户端 `--remote`（T3）
$hello = Join-Path $TmpDir 'hello.lua'
[System.IO.File]::WriteAllText($hello, "print('hello from remote')", (New-Object System.Text.UTF8Encoding($false)))
$badLua = Join-Path $TmpDir 'bad.lua'
[System.IO.File]::WriteAllText($badLua, 'local x = ', (New-Object System.Text.UTF8Encoding($false)))
$long = Join-Path $TmpDir 'long.lua'
[System.IO.File]::WriteAllText($long,
    "Run({ cmdline = [[C:\Windows\System32\cmd.exe]] .. ' /c ping -n 5 127.0.0.1' })`nprint('after run')",
    (New-Object System.Text.UTF8Encoding($false)))

# 没有 `--remote` 时控制命令必须被拒（启动器层，退出码 2）—— 先跑，不依赖 server
$r = Run-Client @('--cancel')
Check 'client.control-needs-remote' ($r.code -eq 2) "code=$($r.code) err=[$($r.err)]"

With-Server @() {
    param($c)
    $null = $c   # server 起来即可；连接由命令行客户端建立

    $r = Run-Client @("--remote=127.0.0.1:$Port", $hello)
    Check 'client.run-script' ($r.code -eq 0 -and $r.out -match 'hello from remote') "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

    $r = Run-Client @("--remote=127.0.0.1:$Port", '--json', $hello)
    $j = if ($r.out) { try { $r.out | ConvertFrom-Json } catch { $null } } else { $null }
    Check 'client.json-envelope' ($r.code -eq 0 -and $j.ok -eq $true -and $j.exit -eq 0 -and $j.stdout -match 'hello from remote') "code=$($r.code) out=[$($r.out)]"
    Check 'client.json-one-line-ascii' (($r.out.TrimEnd("`n") -notmatch "[`r`n]") -and ($r.out -match '"images":\[\]')) "out=[$($r.out)]"

    $r = Run-Client @("--remote=127.0.0.1:$Port", $badLua)
    Check 'client.syntax-error' ($r.code -eq 3 -and $r.err -match 'unexpected symbol') "code=$($r.code) err=[$($r.err)]"

    $r = Run-Client @("--remote=127.0.0.1:$Port", '--health')
    $j = if ($r.out) { try { $r.out | ConvertFrom-Json } catch { $null } } else { $null }
    Check 'client.health' ($r.code -eq 0 -and $j.ok -eq $true -and $j.proto -eq 1) "code=$($r.code) out=[$($r.out)]"

    $r = Run-Client @("--remote=127.0.0.1:$Port", '--version')
    $j = if ($r.out) { try { $r.out | ConvertFrom-Json } catch { $null } } else { $null }
    Check 'client.version-abi' ($r.code -eq 0 -and $j.abi -eq 4 -and $j.arch -match '^(x64|x86)$') "code=$($r.code) out=[$($r.out)]"

    # 另一个进程的 `--cancel`：accept 线程接控制连接 → 落到正在跑的这次执行上（D46）
    $runnerOut = Join-Path $TmpDir 'runner.out'
    $runnerErr = Join-Path $TmpDir 'runner.err'
    $rp = Start-Process -FilePath $Exe -ArgumentList @("--remote=127.0.0.1:$Port", $long) `
        -RedirectStandardOutput $runnerOut -RedirectStandardError $runnerErr -PassThru -NoNewWindow
    Start-Sleep -Milliseconds 800
    $cr = Run-Client @("--remote=127.0.0.1:$Port", '--cancel')
    $cj = if ($cr.out) { try { $cr.out | ConvertFrom-Json } catch { $null } } else { $null }
    Check 'client.cancel-other-process' ($cr.code -eq 0 -and $cj.applied -eq $true -and $cj.reason -eq 'cancel-requested') "out=[$($cr.out)]"
    $null = $rp.WaitForExit(8000)
    Check 'client.cancel-ends-runner' $rp.HasExited 'runner still running'
    if (-not $rp.HasExited) { $rp.Kill() }
}

Write-Output ''
Write-Output ("e2e_tcp: variant={0} pass={1} fail={2}" -f $(if ($Variant) { $Variant } else { 'auto' }), $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
