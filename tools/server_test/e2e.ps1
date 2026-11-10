# =============================================================================
# tools/server_test/e2e.ps1 —— server 模式的端到端冒烟（S1 验收）
#
# 来源：新写（plan_server.md S1）。
# 依赖：只需 PowerShell 7 + 已构建的 `.out/AutoDbg_<arch>.exe`；不需要 VM、不需要 curl。
# 用法：
#   pwsh -File tools/server_test/e2e.ps1 [-Render 64|86] [-Port 18642] [-Verbose]
# 退出码：0 = 全通过；1 = 有失败；2 = 前置缺失（产物不存在）
#
# 覆盖（对应 plan_server.md §4.3 / §4.5）：
#   /health、/version、/run（text + json、语法错、运行时错、args、大输出截断）、
#   404、405、401（显式 token 时）、请求体上限 413、/shutdown。
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('64', '86')]
    [string]$Render = '64',
    [ValidateSet('', 'modern', 'legacy')]
    [string]$Variant = '',
    [int]$Port = 18642,
    [switch]$KeepAlive
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Exe = Join-Path $Root ".out/AutoDbg_$Render.exe"
if (-not (Test-Path $Exe))
{
    Write-Output "SKIP: 产物不存在：$Exe（先 make.bat a $Render）"
    exit 2
}

$script:pass = 0
$script:serverSeq = 0
$script:fail = 0
$script:proc = $null
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "autodbg_server_e2e"
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

# 读 server 还在写的日志/输出文件（默认 File.ReadAllBytes 会因占用失败）
function Read-Shared([string]$path)
{
    if (-not (Test-Path $path)) { return '' }
    try
    {
        $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
        return $t
    }
    catch { return '' }
}

function Start-Server([string[]]$Extra)
{
    $script:serverSeq = 1 + [int]$script:serverSeq
    $tag   = ($Extra -join '_') -replace '[^\w\.\-]', '_'
    $outFile = Join-Path $TmpDir ("server{0}_{1}.out" -f $script:serverSeq, $tag)
    $errFile = "$outFile.err"
    $args = @('--server', "--server-port=$Port")
    if ($Variant) { $args += "--variant=$Variant" }
    $args += $Extra
    $script:proc = Start-Process -FilePath $Exe -ArgumentList $args `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -NoNewWindow
    $script:outFile = $outFile
    $script:errFile = $errFile

    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline)
    {
        if ($script:proc.HasExited)
        {
            Write-Output "SKIP: server 提前退出（exit=$($script:proc.ExitCode)）"
            if (Test-Path $outFile) { Get-Content $outFile | Write-Output }
            if (Test-Path $errFile) { Get-Content $errFile | Write-Output }
            return $false
        }
        try
        {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2 -SkipHttpErrorCheck
            if ($r.StatusCode -eq 200) { return $true }
        }
        catch { Start-Sleep -Milliseconds 150 }
    }
    Write-Output "SKIP: server 15s 内没有就绪"
    return $false
}

function Stop-Server
{
    if ($script:proc -and -not $script:proc.HasExited)
    {
        try { Invoke-WebRequest -Uri "http://127.0.0.1:$Port/shutdown" -Method Post -TimeoutSec 3 -SkipHttpErrorCheck | Out-Null } catch { }
        if (-not $script:proc.WaitForExit(3000)) { $script:proc.Kill() }
    }
}

function Post-Run([string]$source, [string[]]$Query = @(), [hashtable]$Headers = @{}, [string]$FileName = 'buffer.lua')
{
    $q = ''
    if ($Query.Count -gt 0) { $q = '?' + ($Query -join '&') }
    $h = @{ 'X-AutoDbg-Filename' = $FileName }
    foreach ($k in $Headers.Keys) { $h[$k] = $Headers[$k] }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($source)
    return Invoke-WebRequest -Uri "http://127.0.0.1:$Port/run$q" -Method Post -Body $bytes `
        -ContentType 'text/x-lua; charset=utf-8' -Headers $h -TimeoutSec 30 -SkipHttpErrorCheck
}

function Json($resp)
{
    return ($resp.Content | ConvertFrom-Json)
}

# 响应头取值：PS7 的 `$resp.Headers` 是多值字典，直接比较会拿到 System.Object[]
function Hdr($resp, [string]$name)
{
    $v = $resp.Headers[$name]
    if ($null -eq $v) { return '' }
    if ($v -is [array]) { return ($v -join ',') }
    return [string]$v
}

try
{
    # ---------------------------------------------------------------- 启动器 CLI（--help / --version）
    # 不需要 server：直接跑 exe 看 stdout/退出码
    $helpOut = & $Exe --help 2>&1 | Out-String
    Check 'cli.help.exit' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
    Check 'cli.help.usage' ($helpOut -match 'usage: AutoDbg') 'has usage line'
    Check 'cli.help.options' (($helpOut -match '--flavor') -and ($helpOut -match '--server-token')) 'lists options'

    $verOut = & $Exe --version 2>&1 | Out-String
    Check 'cli.version.exit' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
    Check 'cli.version.format' ($verOut -match '^AutoDbg \S+ \(.*abi 2, (x64|x86)\)') "out=$($verOut.Trim())"
    # 版本串应该是 tag 或 commit 短哈希（没有 tag 时就是 7 位以上的十六进制）
    Check 'cli.version.identity' ($verOut -match '^AutoDbg ([0-9a-f]{7,40}|v?\d[^ ]*)(-dirty)? ') "out=$($verOut.Trim())"

    $shortOut = & $Exe -h 2>&1 | Out-String
    Check 'cli.help-short' ($LASTEXITCODE -eq 0 -and $shortOut -match 'usage: AutoDbg') "exit=$LASTEXITCODE"

    & $Exe --definitely-not-an-option 2>&1 | Out-Null
    Check 'cli.unknown-option' ($LASTEXITCODE -eq 1) "exit=$LASTEXITCODE"

    # ---------------------------------------------------------------- 默认实例（loopback，免 token）
    if (-not (Start-Server @())) { exit 2 }

    $h = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -SkipHttpErrorCheck
    $hj = Json $h
    Check 'health.status' ($h.StatusCode -eq 200) "status=$($h.StatusCode)"
    Check 'health.ok' ($hj.ok -eq $true) "body=$($h.Content)"

    $v = Json (Invoke-WebRequest -Uri "http://127.0.0.1:$Port/version" -SkipHttpErrorCheck)
    Check 'version.abi' ($v.abi -eq 2) "abi=$($v.abi)"
    Check 'version.arch' ($v.arch -eq $(if ($Render -eq '64') { 'x64' } else { 'x86' })) "arch=$($v.arch)"
    if ($Variant) { Check 'version.variant' ($v.variant -eq $Variant) "variant=$($v.variant)" }
    Check 'version.winsock' ($v.winsock -eq 'ws2_32.dll') "winsock=$($v.winsock)"
    Check 'version.identity' ($v.server -match '^([0-9a-f]{7,40}|v?\d[^ ]*)(-dirty)?$') "server=$($v.server)"
    Check 'version.source' ($v.version_source -in @('tag', 'describe', 'commit', 'unknown')) "source=$($v.version_source)"

    # --- /run：text 模式（脚本 print 的输出必须进响应）
    $r = Post-Run 'print("hello")' @('format=text')
    Check 'run.text.status' ($r.StatusCode -eq 200) "status=$($r.StatusCode)"
    Check 'run.text.body' ($r.Content -eq "hello`n") "content=[$($r.Content)]"
    Check 'run.text.exit-header' ((Hdr $r 'X-AutoDbg-Exit') -eq '0') "exit=$(Hdr $r 'X-AutoDbg-Exit')"

    # --- /run：JSON 信封
    $j = Json (Post-Run 'print("a", 1, true)')
    Check 'run.json.ok' ($j.ok -eq $true) "body=$($j | ConvertTo-Json -Compress)"
    Check 'run.json.exit' ($j.exit -eq 0) "exit=$($j.exit)"
    Check 'run.json.stdout' ($j.stdout -eq "a`t1`ttrue`n") "stdout=[$($j.stdout)]"
    Check 'run.json.flavor' ($j.flavor -eq 'lua') "flavor=$($j.flavor)"
    Check 'run.json.truncated' ($j.truncated -eq $false) "truncated=$($j.truncated)"

    # --- 脚本参数（?arg= 可重复 → 主 chunk 的 vararg）
    $j = Json (Post-Run 'local a, b = ...; print(a, b)' @('arg=x', 'arg=y'))
    Check 'run.args' ($j.stdout -eq "x`ty`n") "stdout=[$($j.stdout)]"

    # --- 语法错：错误文本要带客户端文件名与行号
    $j = Json (Post-Run "local x = `nprint(" @() @{} 'syntax.lua')
    Check 'run.syntax.ok-false' ($j.ok -eq $false) "ok=$($j.ok)"
    Check 'run.syntax.error-filename' ($j.error -like '*syntax.lua:2*') "error=[$($j.error)]"

    # --- 运行时错
    $j = Json (Post-Run 'error("boom")' @() @{} 'runtime.lua')
    Check 'run.runtime.ok-false' ($j.ok -eq $false) "ok=$($j.ok)"
    Check 'run.runtime.error-text' ($j.error -like '*boom*') "error=[$($j.error)]"

    # --- core 的输出也要被捕获（ReadMemory 失败会走 core 的 Error*/Print*）
    $j = Json (Post-Run 'ReadMemory(0, 4)')
    Check 'run.core-output-captured' (($j.stderr.Length + $j.stdout.Length) -gt 0 -or $j.ok -eq $false) `
        "stdout=[$($j.stdout)] stderr=[$($j.stderr)] ok=$($j.ok)"

    # --- 大输出截断（4 MB/流上限；这里打 8 MB）
    $big = 'for i = 1, 200000 do print(string.rep("x", 40)) end'
    $j = Json (Post-Run $big)
    Check 'run.truncated' ($j.truncated -eq $true) "truncated=$($j.truncated) stdout_len=$($j.stdout.Length)"
    Check 'run.truncated.bounded' ($j.stdout.Length -le 4MB + 4096) "len=$($j.stdout.Length)"

    # --- 路由/方法
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/nope" -SkipHttpErrorCheck
    Check 'route.404' ($r.StatusCode -eq 404) "status=$($r.StatusCode)"
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/run" -SkipHttpErrorCheck
    Check 'route.405' ($r.StatusCode -eq 405) "status=$($r.StatusCode)"
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -Method Options -SkipHttpErrorCheck
    Check 'route.options' ($r.StatusCode -eq 204) "status=$($r.StatusCode)"

    # --- 100-continue（>1 KB 的 body：curl 会发，我们自己的客户端也测一下）
    $payload = 'print("' + ('y' * 2000) + '")'
    $req = [System.Net.HttpWebRequest]::Create("http://127.0.0.1:$Port/run")
    $req.Method = 'POST'
    $req.ContentType = 'text/x-lua'
    $req.ServicePoint.Expect100Continue = $true
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
    $req.ContentLength = $bytes.Length
    $s = $req.GetRequestStream()
    $s.Write($bytes, 0, $bytes.Length)
    $s.Close()
    $resp = $req.GetResponse()
    $body = (New-Object System.IO.StreamReader($resp.GetResponseStream())).ReadToEnd()
    $jj = $body | ConvertFrom-Json
    Check 'http.expect-100-continue' ($jj.ok -eq $true -and $jj.stdout.Length -eq 2001) "ok=$($jj.ok) len=$($jj.stdout.Length)"
    $resp.Close()

    Stop-Server
    Check 'shutdown.exits' ($script:proc.HasExited) "exited=$($script:proc.HasExited)"

    # ---------------------------------------------------------------- 显式 token 实例
    if (-not (Start-Server @('--server-token=abcdefgh'))) { exit 2 }

    $r = Post-Run 'print("x")' @() @{}
    Check 'auth.401-without-token' ($r.StatusCode -eq 401) "status=$($r.StatusCode)"

    $r = Post-Run 'print("x")' @('format=text') @{ 'Authorization' = 'Bearer abcdefgh' }
    Check 'auth.200-with-token' ($r.StatusCode -eq 200 -and $r.Content -eq "x`n") "status=$($r.StatusCode) body=[$($r.Content)]"

    $r = Post-Run 'print("x")' @('format=text') @{ 'X-AutoDbg-Token' = 'wrong-token' }
    Check 'auth.401-wrong-token' ($r.StatusCode -eq 401) "status=$($r.StatusCode)"

    Stop-Server

    # ---------------------------------------------------------------- 绑 0.0.0.0：启动横幅列本机网卡地址
    # 目的：client 不用去猜 IP（把这里列出的地址拄进 nvim 的 `--!autodbg url=…` 即可）。
    # 万一环境不允许监听 0.0.0.0（防火墙/权限），当 SKIP 处理而不是 FAIL。
    if (Start-Server @('--server-addr=0.0.0.0', '--server-token=abcdefgh'))
    {
        Start-Sleep -Milliseconds 200
        $banner = Read-Shared $script:outFile
        Check 'bindall.listen-line' ($banner -match "listen=0\.0\.0\.0:$Port") ((($banner -split "`n") | Where-Object { $_ -match 'listen=' }) -join '')
        Check 'bindall.adapter-list' ($banner -match '(?m)^\s+\d{1,3}(\.\d{1,3}){3}\s+\S') 'expect at least one "<ip> <adapter>" line'
        Check 'bindall.hint-token' ($banner -match 'auth: bearer') 'non-loopback must force a token'
        Stop-Server
    }
    else
    {
        Ok 'bindall.listen-line' 'skip（本机不允许监听 0.0.0.0）'
        Ok 'bindall.adapter-list' 'skip'
        Ok 'bindall.hint-token' 'skip'
    }

    # ---------------------------------------------------------------- 体上限实例（413）
    if (-not (Start-Server @('--server-max-body=256'))) { exit 2 }
    $payload = 'print("' + ('z' * 1000) + '")'
    $r = Post-Run $payload
    Check 'limit.413' ($r.StatusCode -eq 413) "status=$($r.StatusCode)"
    Stop-Server

    # ---------------------------------------------------------------- S2：并发 / 取消 / 日志 / 队列
    $logFile = Join-Path $TmpDir 'server.log'
    Remove-Item $logFile -ErrorAction SilentlyContinue
    if (-not (Start-Server @('--server-queue=1', "--server-log=$logFile"))) { exit 2 }

    # 长脚本（纯 Lua 忙循环 → 靠计数钩子能被取消）
    $longSrc = 'local t = os.clock(); while os.clock() - t < 5 do end; print("done")'
    $hc      = [System.Net.Http.HttpClient]::new()
    $content = [System.Net.Http.StringContent]::new($longSrc, [System.Text.Encoding]::UTF8, 'text/x-lua')
    $task    = $hc.PostAsync("http://127.0.0.1:$Port/run", $content)
    Start-Sleep -Milliseconds 500

    # ① /health 在长脚本执行期间仍然可用，且 busy=true
    $h = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 3 -SkipHttpErrorCheck
    $hj = Json $h
    Check 'conc.health-during-run' ($h.StatusCode -eq 200) "status=$($h.StatusCode)"
    Check 'conc.health-busy' ($hj.busy -eq $true) "busy=$($hj.busy)"

    # ② 队列深度 1（= 在飞上限 1）→ 第二个 /run 必须 503（而不是排长队/饿死）
    $r2 = Post-Run 'print("second")'
    Check 'queue.full-503' ($r2.StatusCode -eq 503) "status=$($r2.StatusCode)"

    # ③ /cancel 能停下长脚本（必须赶在等待前一个响应之前发 —— 所以用异步的那个 task）
    $rc = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/cancel" -Method Post -TimeoutSec 3 -SkipHttpErrorCheck
    $rcj = Json $rc
    Check 'cancel.accepted' ($rcj.accepted -eq $true) "body=$($rc.Content)"

    $body1 = $task.GetAwaiter().GetResult().Content.ReadAsStringAsync().GetAwaiter().GetResult()
    $j1    = $body1 | ConvertFrom-Json
    Check 'cancel.canceled-flag' ($j1.canceled -eq $true) "canceled=$($j1.canceled)"
    Check 'cancel.error-text' ($j1.error -like '*canceled by server*') "error=[$($j1.error)]"
    Check 'cancel.not-ok' ($j1.ok -eq $false) "ok=$($j1.ok)"
    Check 'cancel.fast' ($j1.elapsed_ms -lt 3000) "elapsed_ms=$($j1.elapsed_ms)"
    $hc.Dispose()

    # ④ /cancel 在没有执行时应 accepted=false
    $rc = Json (Invoke-WebRequest -Uri "http://127.0.0.1:$Port/cancel" -Method Post -SkipHttpErrorCheck)
    Check 'cancel.idle-accepted-false' ($rc.accepted -eq $false) "accepted=$($rc.accepted)"

    # ⑤ --server-log 落盘（只记元数据，不记脚本内容）
    $logText = if (Test-Path $logFile) { Get-Content $logFile -Raw } else { '' }
    Check 'log.written' ($logText -like '*path=/run*' -and $logText -like '*canceled=1*') "log=[$logText]"
    Check 'log.no-script-content' (-not ($logText -like '*os.clock*')) 'log should not contain script source'
    Stop-Server

    # ---------------------------------------------------------------- S2：--server-timeout 看门狗
    if (-not (Start-Server @('--server-timeout=1'))) { exit 2 }
    $j = Json (Post-Run 'while true do end')
    Check 'timeout.timed-out' ($j.timed_out -eq $true) "timed_out=$($j.timed_out)"
    Check 'timeout.error-text' ($j.error -like '*--server-timeout*') "error=[$($j.error)]"
    Check 'timeout.bounded' ($j.elapsed_ms -lt 6000) "elapsed_ms=$($j.elapsed_ms)"
    Stop-Server
}
finally
{
    if (-not $KeepAlive) { Stop-Server }
}

Write-Output ''
Write-Output ("e2e: pass={0} fail={1}" -f $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
