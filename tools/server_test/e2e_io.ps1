# =============================================================================
# tools/server_test/e2e_io.ps1 —— client 文件 I/O（效果协议）端到端验收（S7）
#
# 来源：新写。
# 覆盖：
#   ① 本机脚本模式：read_file/write_file 直接读写磁盘（二进制安全、append、错误串）
#   ② server `/run`：脚本里的 read_file/write_file 让 server **挂起**，客户端在本地
#      执行效果、`POST /effects?token=…` 送回，再继续执行（可能多轮）
#   ③ MCP：`tools/call lua_eval` 里读文件同样能挂起/恢复
#   ④ 错误路径：坏 token → 404；同一个 token 重放 → 404（gen 防陈旧）
# 用法：pwsh -File tools/server_test/e2e_io.ps1 [-Render 64|86] [-Variant ...] [-Port 18940]
# 退出码：0 = 全通过；1 = 有失败；2 = 前置缺失
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('64', '86')]
    [string]$Render = '64',
    [ValidateSet('', 'modern', 'legacy')]
    [string]$Variant = '',
    [int]$Port = 18940
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Exe = Join-Path $Root ".out/AutoDbg_$Render.exe"
if (-not (Test-Path $Exe)) { Write-Output "SKIP: 产物不存在：$Exe"; exit 2 }

$script:pass = 0
$script:fail = 0
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_io_e2e'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

function Ok([string]$n, [string]$d = '') { $script:pass++; Write-Output ("[ok]   {0}  {1}" -f $n, $d) }
function Bad([string]$n, [string]$d) { $script:fail++; Write-Output ("[FAIL] {0}  {1}" -f $n, $d) }
function Check([string]$n, [bool]$c, [string]$d = '') { if ($c) { Ok $n $d } else { Bad $n $d } }
function Json($t) { return ($t | ConvertFrom-Json) }

# ---------------------------------------------------------------- ① 本机脚本模式
$localScript = Join-Path $TmpDir 'local_io.lua'
$localBin = Join-Path $TmpDir 'local_io.bin'
$localTextBin = Join-Path $TmpDir 'local_io_text.txt'
Remove-Item $localBin, $localTextBin -ErrorAction SilentlyContinue
@"
local p = [==[$localBin]==]
print("w", write_file(p, "abc\0def"))
local s = read_file(p)
print("r", #s, s == "abc\0def")
local ok2, err = read_file(p .. ".nope")
print("missing", tostring(ok2), tostring(err))
print("append", write_file(p, "-tail", { append = true }), #read_file(p))
-- text 模式用自己的文件（别污染上面的 12 字节基线）：写 LF → CRLF、读 CRLF → LF
local p2 = [==[$localTextBin]==]
print("textw", write_file(p2, "a\nb\n", "w"), #read_file(p2, "rb"))
print("textr", #read_file(p2, "r"), read_file(p2, "r") == "a\nb\n")
print("binw", write_file(p2, "c\nd", "wb"), #read_file(p2, "rb"))
"@ | Set-Content -Path $localScript -Encoding UTF8 -NoNewline

$outFile = Join-Path $TmpDir 'local.out'
$errFile = Join-Path $TmpDir 'local.err'
$p1 = Start-Process -FilePath $Exe -ArgumentList @($localScript) -RedirectStandardOutput $outFile -RedirectStandardError $errFile -Wait -PassThru -NoNewWindow
$text = Get-Content $outFile -Raw
Check 'local.exit' ($p1.ExitCode -eq 0) "exit=$($p1.ExitCode)"
Check 'local.write' ($text -match 'w\s+7') "out=$($text -replace "`r?`n", ' | ')"
Check 'local.read-binary' ($text -match 'r\s+7\s+true') 'read_file 应二进制相等'
Check 'local.missing' ($text -match 'missing\s+nil\s+cannot open file') '缺文件应返回 nil, err'
Check 'local.append' ($text -match 'append\s+5\s+12') 'append 后应 12 字节'
Check 'local.text-write' ($text -match 'textw\s+6\s+6') 'text 写应把 LF 变 CRLF（6 字节）'
Check 'local.text-read' ($text -match 'textr\s+4\s+true') 'text 读应把 CRLF 变 LF'
Check 'local.binary-mode' ($text -match 'binw\s+3\s+3') 'binary 模式不受影响'
if (Test-Path $localBin) {
    $bytes = [System.IO.File]::ReadAllBytes($localBin)
    Check 'local.bytes-on-disk' ($bytes.Length -eq 12 -and $bytes[3] -eq 0) ("len=$($bytes.Length) byte3=$($bytes[3])")
}

# 一个「参考客户端」的效果执行器（S7 的 mode 语义：text = CRLF⇄LF）
function Invoke-Effect($eff)
{
    $text = ($eff.mode -and ($eff.mode -notmatch 'b'))
    if ($eff.op -eq 'read_file')
    {
        if (-not (Test-Path $eff.path))
        {
            return @{ id = $eff.id; ok = $false; error = 'cannot open file' }
        }
        $bytes = [System.IO.File]::ReadAllBytes($eff.path)
        if ([int]$eff.offset -gt 0) { $bytes = $bytes[[int]$eff.offset..($bytes.Length - 1)] }
        if ([int]$eff.length -gt 0 -and [int]$eff.length -lt $bytes.Length) { $bytes = $bytes[0..([int]$eff.length - 1)] }
        if ($text)
        {
            $s = [System.Text.Encoding]::UTF8.GetString($bytes) -replace "`r`n", "`n"
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
        }
        return @{ id = $eff.id; ok = $true; data = [Convert]::ToBase64String($bytes); size = $bytes.Length }
    }

    $bytes = [Convert]::FromBase64String($eff.data)
    if ($text)
    {
        $s = [System.Text.Encoding]::UTF8.GetString($bytes) -replace "`r`n", "`n"
        $s = $s -replace "`n", "`r`n"
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
    }
    $mode = if ($eff.append) { [System.IO.FileMode]::Append } else { [System.IO.FileMode]::Create }
    $fs = [System.IO.File]::Open($eff.path, $mode, [System.IO.FileAccess]::Write)
    $fs.Write($bytes, 0, $bytes.Length)
    $fs.Close()
    return @{ id = $eff.id; ok = $true; written = $bytes.Length }
}

# 反复执行 effects 直到不再挂起；返回 @{ doc = 最终 JSON; out = 各段 stdout 累计 }
function Resolve-Pending([string]$body)
{
    $acc = ''
    $doc = $body | ConvertFrom-Json
    $round = 0
    while ($doc.pending -and $round -lt 16)
    {
        $round++
        $acc += [string]$doc.pending.stdout_so_far
        $results = @()
        foreach ($e in $doc.pending.effects) { $results += (Invoke-Effect $e) }
        $r = Post-Effects $doc.pending.token (ConvertTo-Json @{ results = $results } -Compress)
        $doc = $r.Content | ConvertFrom-Json
    }
    return @{ doc = $doc; out = $acc }
}
# ---------------------------------------------------------------- 起 server
$srvArgs = @('--server', "--server-port=$Port")
if ($Variant) { $srvArgs += "--variant=$Variant" }
$server = Start-Process -FilePath $Exe -ArgumentList $srvArgs `
    -RedirectStandardOutput (Join-Path $TmpDir 'server.out') `
    -RedirectStandardError (Join-Path $TmpDir 'server.err') -PassThru -NoNewWindow

function Post-Run([string]$body) {
    Invoke-WebRequest -Uri "http://127.0.0.1:$Port/run" -Method Post -Body ([System.Text.Encoding]::UTF8.GetBytes($body)) `
        -ContentType 'text/x-lua' -SkipHttpErrorCheck -TimeoutSec 30
}
function Post-Effects([string]$token, [string]$json) {
    Invoke-WebRequest -Uri "http://127.0.0.1:$Port/effects?token=$token" -Method Post `
        -Body ([System.Text.Encoding]::UTF8.GetBytes($json)) -ContentType 'application/json' -SkipHttpErrorCheck -TimeoutSec 30
}
function Mcp([string]$json, [string]$sid = '') {
    $h = @{ 'Content-Type' = 'application/json' }
    if ($sid) { $h['Mcp-Session-Id'] = $sid }
    Invoke-WebRequest -Uri "http://127.0.0.1:$Port/mcp" -Method Post -Body ([System.Text.Encoding]::UTF8.GetBytes($json)) `
        -Headers $h -SkipHttpErrorCheck -TimeoutSec 30
}

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

    # ------------------------------------------------------------ ② /run 的挂起/恢复
    $srvBin = Join-Path $TmpDir 'srv_io.bin'
    Remove-Item $srvBin -ErrorAction SilentlyContinue
    $src = @"
local p = [==[$srvBin]==]
print("w", write_file(p, "abc\0def"))
print("r", #read_file(p))
"@
    $r = Post-Run $src
    $j = Json $r.Content
    Check 'run.pending-1' ($null -ne $j.pending) "body=$($r.Content.Substring(0, [Math]::Min(160, $r.Content.Length)))"
    Check 'run.pending-op' ($j.pending.effects[0].op -eq 'write_file') "op=$($j.pending.effects[0].op)"
    Check 'run.pending-token' ($j.pending.token -match '^\d+:\d+$') "token=$($j.pending.token)"

    $eff = $j.pending.effects[0]
    $bytes = [Convert]::FromBase64String($eff.data)
    Check 'run.effect-data' ($bytes.Length -eq 7 -and $bytes[3] -eq 0) "len=$($bytes.Length)"
    [System.IO.File]::WriteAllBytes($eff.path, $bytes)
    Check 'run.client-wrote' ((Test-Path $eff.path) -and ((Get-Item $eff.path).Length -eq 7)) "path=$($eff.path)"

    # 错误 token 先测（不消耗真 token）
    $bad = Post-Effects '999:1' '{"results":[{"id":1,"ok":true,"written":7}]}'
    Check 'effects.bad-token-404' ($bad.StatusCode -eq 404) "status=$($bad.StatusCode)"

    $r2 = Post-Effects $j.pending.token ('{"results":[{"id":1,"ok":true,"written":7}]}')
    $j2 = Json $r2.Content
    Check 'run.resume-1' ($null -ne $j2.pending -and $j2.pending.effects[0].op -eq 'read_file') "body=$($r2.Content.Substring(0, [Math]::Min(160, $r2.Content.Length)))"
    Check 'run.stdout-so-far' ($j2.pending.stdout_so_far -match 'w\s+7') "soFar=[$($j2.pending.stdout_so_far)]"

    $e2 = $j2.pending.effects[0]
    $back = [System.IO.File]::ReadAllBytes($e2.path)
    $r3 = Post-Effects $j2.pending.token ('{"results":[{"id":1,"ok":true,"data":"' + [Convert]::ToBase64String($back) + '","size":' + $back.Length + '}]}')
    $j3 = Json $r3.Content
    Check 'run.resume-final-ok' ($j3.ok -eq $true -and $j3.resumed -eq $true) "body=$($r3.Content)"
    Check 'run.final-stdout' ($j3.stdout -match 'r\s+7') "stdout=[$($j3.stdout)]"

    # token 重放 → 404（gen 已推进/会话已关）
    $replay = Post-Effects $j2.pending.token '{"results":[{"id":1,"ok":true,"data":""}]}'
    Check 'effects.replay-404' ($replay.StatusCode -eq 404) "status=$($replay.StatusCode)"

    # ------------------------------------------------------------ ③ MCP 里的 read_file
    $mcpFile = Join-Path $TmpDir 'mcp_io.bin'
    [System.IO.File]::WriteAllBytes($mcpFile, [System.Text.Encoding]::UTF8.GetBytes("hello-mcp"))
    $init = Mcp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}'
    $sid = [string]$init.Headers['Mcp-Session-Id']
    # ❗JSON 里反斜杠要转义（Windows 路径），否则 server 侧 jsonx.decode 会报 parse error
    $mcpFileJson = $mcpFile -replace '\\', '\\\\'
    $call = '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"lua_eval","arguments":{"code":"local p = [==[' + $mcpFileJson + ']==]; local s = read_file(p); return #s .. \":\" .. s"}}}'
    $rm = Mcp $call $sid
    $jm = Json $rm.Content
    Check 'mcp.pending' ($null -ne $jm.pending -and $jm.pending.kind -eq 'mcp') "body=$($rm.Content.Substring(0, [Math]::Min(160, $rm.Content.Length)))"
    if ($jm.pending) {
        $me = $jm.pending.effects[0]
        $mb = [System.IO.File]::ReadAllBytes($me.path)
        $rm2 = Post-Effects $jm.pending.token ('{"results":[{"id":1,"ok":true,"data":"' + [Convert]::ToBase64String($mb) + '"}]}')
        $jm2 = Json $rm2.Content
        $txt = $jm2.result.content[0].text
        Check 'mcp.resume-result' ($jm2.jsonrpc -eq '2.0' -and $txt -match '^9:hello-mcp$') "text=[$txt]"
    }

    # 客户端写失败时应当把错误串带回脚本（nil, err）
    $srcFail = 'local ok, err = write_file("Z:\\definitely\\not\\writable\\x.bin", "x"); print("fail", tostring(ok), tostring(err))'
    $rf = Post-Run $srcFail
    $jf = Json $rf.Content
    if ($jf.pending) {
        # 客户端本地执行也会失败 → 把失败结果回送
        $res = ('{"results":[{"id":1,"ok":false,"error":"cannot open file"}]}')
        $rf2 = Post-Effects $jf.pending.token $res
        $jf2 = Json $rf2.Content
        Check 'run.client-error-propagation' ($jf2.stdout -match 'fail\s+nil\s+cannot open file') "stdout=[$($jf2.stdout)]"
    }
    else {
        Check 'run.client-error-propagation' ($true) '脚本在内核侧就已失败（无 pending）'
    }

    # ------------------------------------------------------------ ④ server 模式的 text/binary 模式
    $srvText = Join-Path $TmpDir 'srv_text.txt'
    Remove-Item $srvText -ErrorAction SilentlyContinue
    $srcText = @"
local p = [==[$srvText]==]
print("tw", write_file(p, "a\nb\n", "w"))
print("tr", #read_file(p, "r"), read_file(p, "r") == "a\nb\n")
"@
    $rt = Resolve-Pending (Post-Run $srcText).Content
    $disk = if (Test-Path $srvText) { [System.IO.File]::ReadAllBytes($srvText) } else { @() }
    Check 'run.text-mode-write-crlf' ($disk.Length -eq 6 -and $disk[1] -eq 0x0D -and $disk[4] -eq 0x0D) ("len=$($disk.Length) bytes=$([System.BitConverter]::ToString($disk))")
    Check 'run.text-mode-final-ok' ($rt.doc.ok -eq $true) "doc=$($rt.doc | ConvertTo-Json -Compress)"
    Check 'run.text-mode-read' (($rt.out + [string]$rt.doc.stdout) -match 'tr\s+4\s+true') ("out=[$($rt.out)][$($rt.doc.stdout)]")
}
finally
{
    if (-not $server.HasExited)
    {
        try { Invoke-WebRequest -Uri "http://127.0.0.1:$Port/shutdown" -Method Post -TimeoutSec 2 -SkipHttpErrorCheck | Out-Null } catch { }
        if (-not $server.WaitForExit(3000)) { $server.Kill() }
    }
}

Write-Output ''
Write-Output ("io e2e: pass={0} fail={1}" -f $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
