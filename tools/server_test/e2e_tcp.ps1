# =============================================================================
# tools/server_test/e2e_tcp.ps1 —— raw TCP 帧服务端 + 客户端的端到端断言（T2/T3）
#
# 来源：新写（原 plan_cli.md §7，现合并进 plan.md；T2 = 握手/帧/control，T3 = `--remote` 客户端）。
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
#   ⑬ `open_file` 读向（T4/D45）：远程 open 应答、read/tell/seek/lines 的字节与位置语义、
#      **等回包超时 → nil,"timeout" 且脚本继续**（用一个故意不回 read_req 的假客户端）
#   ⑭ **调试回调里的阻塞读**（T4 收尾）：`Run` 的 callback 里 `f:read`，超时后回调返回、
#      调试会话不断（下一次 `Run` 照常）；并且**迟到的 read_resp 被丢弃**（D45 规则 3）
#   ⑮ 绑 `0.0.0.0` 时的启动横幅**列出本机网卡地址**（client 直接挑一条去 `--remote`）
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

# ------------------------------------------- ⑬ open_file 读向（T4 / D45 / D49）
$dataFile = Join-Path $TmpDir 'data.txt'
[System.IO.File]::WriteAllText($dataFile, "alpha`nbeta`ngamma`n", (New-Object System.Text.UTF8Encoding($false)))

# ① 真客户端跑一个读脚本：open 往返 + read("l") + read(N) + tell（逻辑位置）+ seek + lines
$readLua = Join-Path $TmpDir 'read.lua'
[System.IO.File]::WriteAllText($readLua, @"
local f, err = open_file('$($dataFile.Replace('\', '/'))', 'rb')
if not f then print('open-failed ' .. tostring(err)) return end
print('line1=' .. tostring(f:read('l')))
print('bytes3=' .. tostring(f:read(3)))
print('tell=' .. f:tell())
print('seek0=' .. f:seek('set', 0))
print('line2=' .. tostring(f:read('l')))
local n = 0
for line in f:lines() do n = n + 1 end
print('lines=' .. n)
f:close()
"@, (New-Object System.Text.UTF8Encoding($false)))

With-Server @() {
    param($c)
    $null = $c
    $r = Run-Client @("--remote=127.0.0.1:$Port", $readLua)
    Check 'open.read-lines' ($r.code -eq 0 -and $r.out -match 'line1=alpha' -and $r.out -match 'line2=alpha' -and $r.out -match 'lines=2') `
        "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"
    Check 'open.read-bytes-and-tell' ($r.out -match 'bytes3=bet' -and $r.out -match 'tell=9') "out=[$($r.out)]"
    Check 'open.seek-roundtrip' ($r.out -match 'seek0=0') "out=[$($r.out)]"
}

# ② 超时：假客户端回 open 的 ok，但**永不回 read_req** → 脚本拿到 nil,"timeout" 且继续往下跑
# 服务端的等待上限由 `--read-timeout=400` 给（ABI 4 的字段）
$toSrc = @'
local f = open_file([[C:/ProgramData/Temp/autodbg_tcp_e2e/data.txt]], 'rb')
if not f then print('open-failed') return end
local d, e = f:read('l')
print('read=' .. tostring(d) .. ' err=' .. tostring(e))
print('after-timeout')
'@

$clientRes = Start-Server @('--read-timeout=400')
if ($null -eq $clientRes) {
    Bad 'read.timeout' 'server did not start'
} else {
    $s = $clientRes.GetStream()
    Send-Frame $s 10 0 1 '' $null
    $null = Recv-Frame $s                                   # info
    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes($toSrc))

    $outText = ''
    $result = $null
    $sawReadReq = $false
    for ($i = 0; $i -lt 40; $i++) {
        $f = Recv-Frame $s
        if ($null -eq $f) { break }
        if ($f.type -eq 1) {                                # open：回一个 ok（否则 open 也会超时）
            Send-Frame $s 13 0 $f.id '' $null
            continue
        }
        if ($f.type -eq 7) { $sawReadReq = $true; continue } # read_req：故意不回
        if ($f.type -eq 4) { $outText += $f.data; continue }
        if ($f.type -eq 5) { $result = $f.json | ConvertFrom-Json; continue }
        if ($f.type -eq 14) { break }
    }
    try { $clientRes.Close() } catch { }
    Stop-Server
    $script:proc = $null

    Check 'read.timeout-saw-request' $sawReadReq 'server never asked the client for bytes'
    Check 'read.timeout-nil-and-reason' ($outText -match 'read=nil err=timeout') "stdout=[$outText]"
    Check 'read.timeout-script-continues' ($outText -match 'after-timeout' -and $result.ok -eq $true) "stdout=[$outText] result=$($result | ConvertTo-Json -Compress)"
}

# ---------------------------------- ⑭ 回调里的阻塞读：超时 → 回调返回 → 断点继续（T4 收尾）
# 这一段就是 D45 的核心：`f:read()` 在**调试回调**里等回包（不 yield）。假客户端：
#   · `open` → 回 ok；
#   · `read_req` → **第一次不回**（让 hit1 超时），等看到 hit1 的 stdout 后再补一个
#     **迟到的 read_resp（带 LATE 字样）** —— 它必须被丢弃，不能被 hit2 当成数据；
#   · 第二次 read_req 也不回 → hit2 也必须超时。
# 两次 hit 是两个独立的 `Run`，所以也验证了「超时之后调试会话没坏（下一次 Run 照常）」。
$cbSrc = @'
local path = [[C:/ProgramData/Temp/autodbg_tcp_e2e/data.txt]]
local f = open_file(path, 'rb')
if not f then print('open-failed') return end

for i = 1, 2 do
  Run({
    cmdline = [[C:\Windows\System32\cmd.exe]] .. ' /c ping -n 2 127.0.0.1',
    callback = function()
      -- ❗调试回调里阻塞读（不 yield）：等不到回包就超时返回
      local d, e = f:read('l')
      print('hit' .. i .. ' read=' .. tostring(d) .. ' err=' .. tostring(e))
      Detach()
      return true
    end,
  })
end
f:close()
print('after-both-runs')
'@

$cbRes = Start-Server @('--read-timeout=400')
if ($null -eq $cbRes) {
    Bad 'callback.read' 'server did not start'
} else {
    $s = $cbRes.GetStream()
    Send-Frame $s 10 0 1 '' $null
    $null = Recv-Frame $s
    Send-Frame $s 11 0 0 '' ([System.Text.Encoding]::UTF8.GetBytes($cbSrc))

    $outText = ''
    $result = $null
    $handleId = -1
    $readReqs = 0
    $staleSent = $false
    for ($i = 0; $i -lt 120; $i++) {
        $f = Recv-Frame $s
        if ($null -eq $f) { break }
        if ($f.type -eq 1) {                                # open：记下句柄，回 ok
            $handleId = $f.id
            Send-Frame $s 13 0 $f.id '' $null
            continue
        }
        if ($f.type -eq 7) {                                # read_req：前两次都故意不回
            $readReqs++
            continue
        }
        if ($f.type -eq 4) {
            $outText += $f.data
            # hit1 超时之后补一个**迟到**的应答（不守规矩的客户端行为）→ 服务端必须丢掉
            if (-not $staleSent -and $handleId -ge 0 -and $f.data -match 'hit1') {
                $staleSent = $true
                Send-Frame $s 13 0 $handleId '' ([System.Text.Encoding]::UTF8.GetBytes("LATE\n"))
            }
            continue
        }
        if ($f.type -eq 5) { $result = $f.json | ConvertFrom-Json; continue }
        if ($f.type -eq 14) { break }
    }
    try { $cbRes.Close() } catch { }
    Stop-Server
    $script:proc = $null

    Check 'callback.read-asked-twice' ($readReqs -eq 2) "read_req=$readReqs（每 hit 一次 refill；多出来说明 stale 被当成数据了）"
    Check 'callback.read-timeout-per-run' (([regex]::Matches($outText, 'read=nil err=timeout')).Count -ge 2) "stdout=[$outText]"
    Check 'callback.read-session-survives' ($outText -match 'hit2 read=nil' -and $outText -match 'after-both-runs' -and $result.ok -eq $true) "stdout=[$outText]"
    Check 'callback.read-stale-discarded' (-not ($outText -match 'LATE')) "stdout=[$outText]"
}

# ---------------------------- ⑮ 绑 0.0.0.0 时横幅列出本机网卡地址（T5 重写时曾漏掉）
$banner = Start-Server @("--server-addr=0.0.0.0:$Port", '--server-token=auto')
if ($null -eq $banner) {
    Write-Output '[skip] net.banner（这台机器绑不了 0.0.0.0，跳过）'
} else {
    $banner.Close()
    Start-Sleep -Milliseconds 200
    $text = if (Test-Path $script:outFile) { Get-Content $script:outFile -Raw } else { '' }
    Stop-Server
    $script:proc = $null

    Check 'net.banner-listen-any' ($text -match 'listen=0\.0\.0\.0') "banner=[$($text -replace "`r?`n", ' | ')]"
    # 地址 + 网卡描述两列（纯 ASCII，legacy 变体也能比）；至少一条
    $addrs = [regex]::Matches($text, '(?m)^\s+(\d{1,3}(?:\.\d{1,3}){3})\s{2,}(\S.*)$')
    Check 'net.banner-lists-addresses' ($addrs.Count -ge 1) "count=$($addrs.Count)"
    Check 'net.banner-token-hint' ($text -match 'token') 'banner 应提示非 loopback 需要 token'
}

Write-Output ''

# ---------------- ⑯ T7.3 `dbg` 状态机（pump.*，8 条） ----------------
# 只在 x86 跑：fixture 是 x86-only（MSVC 的 inline asm 在 x64 不可用），而 `parse_pe` 按编译期
# _WIN64 判位数（§5 第 5 条的架构边界）⇒ x64 产物看 32 位节点会被跳过；64 口径下这段整段 SKIP。
# 口径（本轮实测后钉死，见 plan_t7.md §9 第 8-11 条）：
#   · 断点地址 = 模式命中地址 + 5（= 每条 int 3 被执行的那一条的地址）；模式在产物里必须只出现一次；
#   · `pump` 的默认 code（事件表的 `ev.cont_code`）：首泵（首个 EXCEPTION_BREAKPOINT，m_first_bp）= DBG_CONTINUE
#     = 65538；之后未武装的 int3 = DBG_EXCEPTION_NOT_HANDLED = 2147549185（这两条就是「与旧 run
#     用的码一致」的可断形式）；
#   · 切片 C1a（D55）：`pump` 返回**普通事件表**，`dbg.code()` 已删 —— code 分两个字段：
#     `ev.code`（异常码，没有异常的事件 = 0）与 `ev.cont_code`（core 算好的默认应答码）；
#     `d.stop` 就是那一张表（`d:pump` 缓存优先），`d:cont()` 成功后才清。
#   · `hits()` 的 type：1=断点、2=异常、3=单步；记录只在 `on_breakpoint` 通道产生（arm 的回调不另记）。
#   ❗fixture 的 int3 是**写死的指令**，不是 core 管的软件断点 ⇒ core 算出来的是 NOT_HANDLED，
#     所以脚本想让它继续跑就必须显式 `dbg.cont(nil, nil, 0x00010002)`（= DBG_CONTINUE）。
#     这是 plan_t7.md §9 第 8 条留下的尾巴：8 次命中 + 退出码 42 只在每次 int3 都被放行时成立。
#   ❗NOT_HANDLED 的那一次会在同一个 EA 上再报一次（first chance → second chance）：autocont 段
#     因此是 3 条记录（ntdll 初始 int3 + site int3 的两次）。first/second chance 的真值要等 T7.4
#     （§2.2 第 5 条）才能区分，所以这里是固定数字而不是「命中次数 = 轮数」。
function Find-FixtureSite([string]$path)
{
    $b = [IO.File]::ReadAllBytes($path)
    $peOff = [BitConverter]::ToUInt32($b, 60)
    $soh = [BitConverter]::ToUInt16($b, $peOff + 20)
    $nsec = [BitConverter]::ToUInt16($b, $peOff + 6)
    $secOff = $peOff + 24 + $soh
    $pattern = [byte[]](0xB8, 0x44, 0x42, 0x47, 0x50, 0xCC)
    $hits = New-Object System.Collections.Generic.List[int]
    for ($i = 0; $i -le $b.Length - $pattern.Length; $i++)
    {
        $ok = $true
        for ($k = 0; $k -lt $pattern.Length; $k++)
        {
            if ($b[$i + $k] -ne $pattern[$k]) { $ok = $false; break }
        }
        if ($ok) { $hits.Add($i) }
    }
    $rva = -1
    foreach ($h in $hits)
    {
        for ($s = 0; $s -lt $nsec; $s++)
        {
            $e = $secOff + $s * 40
            $va = [BitConverter]::ToUInt32($b, $e + 12)
            $rawsize = [BitConverter]::ToUInt32($b, $e + 16)
            $raw = [BitConverter]::ToUInt32($b, $e + 20)
            if ($h -ge $raw -and $h -lt ($raw + $rawsize)) { $rva = $va + ($h - $raw) }
        }
    }
    return @{ count = $hits.Count; rva = $rva; site = $rva + 5 }
}

if ($Render -ne '86')
{
    Write-Output 'SKIP: pump.* 段只在 x86 跑（fixture 是 x86-only；x64 的 parse_pe 按编译期 _WIN64 判位数）'
}
else
{
    $fx = Join-Path $Root '.out/fixtures/x86_loop.exe'
    if (-not (Test-Path $fx))
    {
        Bad 'pump.fixture-missing' '先 make.bat a 86 生成 .out/fixtures/x86_loop.exe'
    }
    else
    {
        $site = Find-FixtureSite $fx
        $fxLua = $fx.Replace('\', '/')
        $siteHex = ('0x{0:X}' -f $site.site)

        $s1 = Join-Path $TmpDir 'pump_no_auto.lua'
        [IO.File]::WriteAllText($s1, @"
local EXE = '$fxLua'
local SITE = $($site.rva) + 5
local d, err = dbg.create{ imagepath = EXE }
if not d then print('create ' .. tostring(err)) return end
for i = 1, 24 do
    local ev = dbg.pump(3000, false)
    if ev == nil then break end
    local h = dbg.hits()
    local last = h[#h]
    if last and last.type == 1 and last.rva == SITE then
        -- 目标停在 int3 之后那一条上：再泵一次是空转（进程被系统挂起），EIP 必须不动
        local e1 = d:eip()
        local idle = dbg.pump(300, false)
        local e2 = d:eip()
        print(string.format('paused=%s eip_after_int3=%s idle=%s eip_still=%s',
            tostring(d:paused()), tostring(e1 == last.ea + 1), tostring(idle == nil), tostring(e1 == e2)))
        break
    end
    dbg.cont(nil, nil, 0x00010002)   -- DBG_CONTINUE：fixture 的 int3 是写死的，不显式放行就会终止
end
d:kill()
"@, (New-Object System.Text.UTF8Encoding($false)))

        $s2 = Join-Path $TmpDir 'pump_code.lua'
        [IO.File]::WriteAllText($s2, @"
local d, err = dbg.create{ imagepath = '$fxLua' }
if not d then print('create ' .. tostring(err)) return end
local codes, seen = {}, 0
for i = 1, 24 do
    local ev = dbg.pump(3000, false)
    if ev == nil then break end
    local h = dbg.hits()
    if #h > seen then seen = #h; codes[#codes + 1] = ev.cont_code end
    if #codes >= 2 then break end
    dbg.cont(nil, nil, 0x00010002)
end
print('cont1=' .. tostring(codes[1]) .. ' cont2=' .. tostring(codes[2]))
d:kill()
"@, (New-Object System.Text.UTF8Encoding($false)))

        $s3 = Join-Path $TmpDir 'pump_step.lua'
        [IO.File]::WriteAllText($s3, @"
local d, err = dbg.create{ imagepath = '$fxLua' }
if not d then print('create ' .. tostring(err)) return end
for i = 1, 14 do
    local ev = d:pump(3000)
    if ev == nil then break end
    local h = d:hits()
    if #h == 1 then
        print('step=' .. tostring(d:step()))
        d:cont()
        for j = 1, 6 do
            local ev2 = d:pump(3000)
            if ev2 == nil then break end
            local h2 = d:hits()
            for k = 1, #h2 do if h2[k].type == 3 then print('steprec=' .. h2[k].type) end end
            d:cont()
        end
        break
    end
    d:cont(0x00010002)
end
d:kill()
"@, (New-Object System.Text.UTF8Encoding($false)))

        $s4 = Join-Path $TmpDir 'pump_reentry.lua'
        [IO.File]::WriteAllText($s4, @"
local EXE = '$fxLua'
local d, err = dbg.create{ imagepath = EXE, on_breakpoint = function(info)
    local r, e = dbg.pump()
    print('reentry=' .. tostring(r) .. ' ' .. tostring(e))
end }
if not d then print('create ' .. tostring(err)) return end
for i = 1, 24 do
    local ev = dbg.pump(3000, false)
    if ev == nil then break end
    if #dbg.hits() >= 1 then break end
    dbg.cont(nil, nil, 0x00010002)
end
print('runwhile=' .. tostring(select(2, pcall(Run, { imagepath = EXE }))))
d:kill()
"@, (New-Object System.Text.UTF8Encoding($false)))

        # record-and-continue 分两段：
        #   ① `autocont = true` 时引擎自己 continue（脚本一次都不调 `dbg.cont`）⇒ 目标会自己跑到
        #      site 的 int3（那条不是 core 管的断点 ⇒ NOT_HANDLED ⇒ 目标终止）。能跑到 = 引擎自己放行了。
        #   ② 用显式 `DBG_CONTINUE` 逐次放行 ⇒ fixture 的契约数字：8 次命中 + 退出码 42；
        #      窗口只有 2 格 ⇒ dropped 必须诚实计数（9 条记录 - 2）。
        $s5 = Join-Path $TmpDir 'pump_record.lua'
        [IO.File]::WriteAllText($s5, @"
local EXE = '$fxLua'
local d, err = dbg.create{ imagepath = EXE }
if not d then print('create ' .. tostring(err)) return end
for i = 1, 40 do
    local ev = dbg.pump(3000, true)
    if ev == nil then break end
    if dbg.stopped() then break end
end
print('autocont_hits=' .. #dbg.hits() .. ' autocont_paused=' .. tostring(dbg.paused()))
d:kill()

local exited = nil
local d2
d2, err = dbg.create{ imagepath = EXE, on_exit_process = function(info) exited = info.code end }
if not d2 then print('create2 ' .. tostring(err)) return end
print('cap=' .. tostring(dbg.setcap()) .. ' cap2=' .. tostring(dbg.setcap(2)) .. ' capbig=' .. tostring(dbg.setcap(200)))
dbg.setcap(2)
for i = 1, 60 do
    local ev = dbg.pump(3000, false)
    if ev == nil then break end
    dbg.cont(nil, nil, 0x00010002)   -- 记录 + 显式放行
    if dbg.stopped() then break end
end
print(string.format('hits=%d dropped=%d exit=%s', #dbg.hits(), dbg.dropped(), tostring(exited)))
d2:kill()
"@, (New-Object System.Text.UTF8Encoding($false)))

        $s6 = Join-Path $TmpDir 'pump_cancel.lua'
        [IO.File]::WriteAllText($s6, @"
local d, err = dbg.create{ imagepath = '$fxLua' }
if not d then print('create ' .. tostring(err)) return end
local ev1 = dbg.pump(30000, false)
print('event1=' .. tostring(ev1 and ev1.type))
local ev2, e2 = dbg.pump(30000, false)             -- 目标停在未应答的事件上 ⇒ 这一泵是空转
print('idle=' .. tostring(ev2) .. ' err=' .. tostring(e2))
d:kill()
"@, (New-Object System.Text.UTF8Encoding($false)))

        # `dbg.attach`：附到一个**已在跑**的 32 位目标（自己起的 64 位进程附不了：32 位调试器对 64 位目标会拿到
        # ERROR_NOT_SUPPORTED = 50）。`ping -n 20` 约 19 s，够门禁用。
        # ❗切片 A（D51）：收尾是两个动词，**9x 上 `dbg.detach` 不存在**。这里故意**留一个未应答的
        #   事件**再 `dbg.detach()`（detach 前必须自己先 `ContinueDebugEvent`，否则目标停在被挂起的
        #   线程上），然后断言目标仍活着（detach.pending-event）。
        $syswow = Join-Path $env:WINDIR 'SysWOW64\cmd.exe'
        $cmd32  = if (Test-Path $syswow) { $syswow } else { Join-Path $env:WINDIR 'system32\cmd.exe' }
        $tgt = Start-Process -FilePath $cmd32 -ArgumentList '/c', 'ping -n 20 127.0.0.1 >NUL' `
            -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 200
        $s7 = Join-Path $TmpDir 'pump_attach.lua'
        [IO.File]::WriteAllText($s7, @"
print('caps_detach=' .. tostring(dbg.caps().detach) .. ' detach_key=' .. tostring(dbg.detach ~= nil) ..
      ' close_key=' .. tostring(dbg.close))
local d, err = dbg.attach($($tgt.Id))
print('attach=' .. tostring(d ~= nil) .. ' err=' .. tostring(err))
if not d then return end
local ev = d:pump(2000)                   -- CREATE_PROCESS：故意**不放行**
print('create_process=' .. tostring(ev and ev.type) .. ' paused=' .. tostring(d:paused()))
print('detach=' .. tostring(d:detach()))
print('nopid=' .. tostring(select(2, dbg.attach(0))))
print('badpid=' .. tostring(select(2, dbg.attach('x'))))
print('after_detach=' .. tostring(select(2, dbg.pump(10, false))))
"@, (New-Object System.Text.UTF8Encoding($false)))

        With-Server @('--server-timeout=20000') {
            param($c)
            $null = $c

            $r1 = Run-Client @("--remote=127.0.0.1:$Port", $s1)
            Check 'pump.no-auto-continue' ($site.count -eq 1 -and $r1.out -match 'paused=true' -and `
                $r1.out -match 'eip_after_int3=true' -and $r1.out -match 'eip_still=true' -and $r1.out -match 'idle=true') `
                ("pattern=" + $site.count + " site=" + $siteHex + " out=[" + $r1.out + "]")

            $r2 = Run-Client @("--remote=127.0.0.1:$Port", $s2)
            Check 'pump.default-code-parity' ($r2.out -match 'cont1=65538' -and $r2.out -match 'cont2=2147549185') `
                "ev.cont_code: DBG_CONTINUE=65538 NOT_HANDLED=2147549185 out=[$($r2.out)]"

            $r3 = Run-Client @("--remote=127.0.0.1:$Port", $s3)
            Check 'pump.step' ($r3.out -match 'step=true' -and $r3.out -match 'steprec=3') "out=[$($r3.out)]"

            $r4 = Run-Client @("--remote=127.0.0.1:$Port", $s4)
            Check 'pump.reentry-rejected' ($r4.out -match 'reentry=nil pump cannot be called from a callback' -and `
                $r4.out -match 'runwhile=cannot run while an event is unhandled') "out=[$($r4.out)]"

            $r5 = Run-Client @("--remote=127.0.0.1:$Port", $s5)
            Check 'pump.record-and-continue' ($r5.out -match 'autocont_hits=3' -and $r5.out -match 'autocont_paused=false' -and `
                $r5.out -match 'cap=64' -and $r5.out -match 'cap2=2' -and $r5.out -match 'capbig=64' -and `
                $r5.out -match 'hits=2 dropped=7 exit=42') "out=[$($r5.out)]"

            # ⑥ 单连接：script 帧先被 worker 拿走（开始空转），control 帧由 accept 线程直答
            # ❗必须新开一条连接：上面 5 次 Run-Client 期间 `$c` 一直空闲，而控制连接空闲 2 s 就被
            #   服务端关掉（tcp_server.cpp 的 k_conn_idle_ms = 2000）——复用 `$c` 会在第一次写就拿到
            #   WSAECONNABORTED。
            $c2 = Connect-Retry
            if ($null -eq $c2) { Bad 'pump.cancel-while-idle' 'connect failed' }
            else
            {
            $c2.ReceiveTimeout = 12000
            $s = $c2.GetStream()
            Send-Frame $s 10 0 1 '' $null
            $null = Recv-Frame $s
            Send-Frame $s 11 0 1 '' ([System.Text.Encoding]::UTF8.GetBytes((Get-Content $s6 -Raw)))
            $first = Recv-Until $s 4 8000
            Send-Frame $s 12 0 1 '' $null
            $ackf = Recv-Until $s 8 8000
            $ack = if ($ackf -and $ackf.json) { $ackf.json | ConvertFrom-Json } else { $null }
            $second = Recv-Until $s 4 12000
            Check 'pump.cancel-while-idle' ($first -and $first.data -match 'event1=3' -and `
                $ack -and $ack.applied -eq $true -and $second -and $second.data -match 'err=canceled by server') `
                "first=[$($first.data)] ack=[$($ackf.json)] second=[$($second.data)]"
            try { $c2.Close() } catch { }
            }

            # ⑦ attach + detach 的归属口径：`d:detach` 只分离（目标仍活着），`dbg.close` 已删
            $r7 = Run-Client @("--remote=127.0.0.1:$Port", $s7)
            Start-Sleep -Milliseconds 400
            $tgt.Refresh()
            Check 'pump.attach-detach' ($r7.out -match 'caps_detach=true detach_key=true close_key=nil' -and `
                $r7.out -match 'attach=true' -and $r7.out -match 'create_process=3' -and `
                $r7.out -match 'paused=true' -and $r7.out -match 'detach=true' -and -not $tgt.HasExited) `
                "alive=$(-not $tgt.HasExited) out=[$($r7.out)]"
            Check 'pump.attach-arg-errors' ($r7.out -match 'nopid=dbg.attach: pid expected' -and `
                $r7.out -match 'badpid=dbg.attach: pid \(number\) expected' -and `
                $r7.out -match 'after_detach=dbg: no session') "out=[$($r7.out)]"
            if (-not $tgt.HasExited) { Stop-Process -Id $tgt.Id -Force -ErrorAction SilentlyContinue }
        }
    }
}
Write-Output ("e2e_tcp: variant={0} pass={1} fail={2}" -f $(if ($Variant) { $Variant } else { 'auto' }), $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }





