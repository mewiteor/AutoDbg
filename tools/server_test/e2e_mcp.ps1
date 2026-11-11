# =============================================================================
# tools/server_test/e2e_mcp.ps1 —— MCP（streamable HTTP 子集）端到端验收（S6）
#
# 来源：新写（plan_server.md S6 / §8）。
# 覆盖：initialize（+Mcp-Session-Id）、ping、tools/list、tools/call（lua_eval /
#       gui_screenshot / 未知工具）、会话持久性、未知方法 -32601、解析错 -32700、
#       通知 202、未知会话 404、Accept: text/event-stream 的 SSE 帧。
# 用法：pwsh -File tools/server_test/e2e_mcp.ps1 [-Render 64|86] [-Variant ...] [-Port 18900]
# 退出码：0 = 全通过；1 = 有失败；2 = 前置缺失
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('64', '86')]
    [string]$Render = '64',
    [ValidateSet('', 'modern', 'legacy')]
    [string]$Variant = '',
    [int]$Port = 18900
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Exe = Join-Path $Root ".out/AutoDbg_$Render.exe"
if (-not (Test-Path $Exe)) { Write-Output "SKIP: 产物不存在：$Exe"; exit 2 }

$script:pass = 0
$script:fail = 0
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_mcp_e2e'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

function Ok([string]$n, [string]$d = '') { $script:pass++; Write-Output ("[ok]   {0}  {1}" -f $n, $d) }
function Bad([string]$n, [string]$d) { $script:fail++; Write-Output ("[FAIL] {0}  {1}" -f $n, $d) }
function Check([string]$n, [bool]$c, [string]$d = '') { if ($c) { Ok $n $d } else { Bad $n $d } }

function Hdr($resp, [string]$name)
{
    $v = $resp.Headers[$name]
    if ($null -eq $v) { return '' }
    if ($v -is [array]) { return ($v -join ',') }
    return [string]$v
}

function Mcp([string]$json, [hashtable]$Extra = @{})
{
    $h = @{ 'Content-Type' = 'application/json' }
    foreach ($k in $Extra.Keys) { $h[$k] = $Extra[$k] }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    return Invoke-WebRequest -Uri "http://127.0.0.1:$Port/mcp" -Method Post -Body $bytes -Headers $h `
        -TimeoutSec 60 -SkipHttpErrorCheck
}

function ToolCall([string]$name, [string]$argsJson, [string]$sid)
{
    $body = '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"' + $name + '","arguments":' + $argsJson + '}}'
    $extra = @{}
    if ($sid) { $extra['Mcp-Session-Id'] = $sid }
    return Mcp $body $extra
}

# ---------------------------------------------------------------- 起 server
$srvArgs = @('--server', "--server-port=$Port")
if ($Variant) { $srvArgs += "--variant=$Variant" }
$server = Start-Process -FilePath $Exe -ArgumentList $srvArgs `
    -RedirectStandardOutput (Join-Path $TmpDir 'server.out') `
    -RedirectStandardError (Join-Path $TmpDir 'server.err') -PassThru -NoNewWindow

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

    # ------------------------------------------------------------ initialize
    $r = Mcp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"e2e","version":"0"}}}'
    $sid = Hdr $r 'Mcp-Session-Id'
    $j = $r.Content | ConvertFrom-Json
    Check 'initialize.status' ($r.StatusCode -eq 200) "status=$($r.StatusCode)"
    Check 'initialize.protocol' ($j.result.protocolVersion -eq '2024-11-05') "pv=$($j.result.protocolVersion)"
    Check 'initialize.server-name' ($j.result.serverInfo.name -eq 'AutoDbg') "name=$($j.result.serverInfo.name)"
    Check 'initialize.capabilities' ($null -ne $j.result.capabilities.tools) "capabilities.tools=$($j.result.capabilities.tools)"
    Check 'initialize.session-header' ([int]$sid -gt 0) "Mcp-Session-Id=$sid"
    $initJson = $j   # 留给后面与 /version 比对 serverInfo.version

    # ------------------------------------------------------------ initialized 通知 → 202
    $r = Mcp '{"jsonrpc":"2.0","method":"notifications/initialized"}' @{ 'Mcp-Session-Id' = $sid }
    Check 'notification.202' ($r.StatusCode -eq 202) "status=$($r.StatusCode)"

    # ------------------------------------------------------------ ping / tools/list
    $j = (Mcp '{"jsonrpc":"2.0","id":2,"method":"ping"}' @{ 'Mcp-Session-Id' = $sid }).Content | ConvertFrom-Json
    Check 'ping' ($null -ne $j.result) "result=$($j.result)"

    $j = (Mcp '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' @{ 'Mcp-Session-Id' = $sid }).Content | ConvertFrom-Json
    $names = @($j.result.tools | ForEach-Object { $_.name })
    Check 'tools.list.count' ($names.Count -eq 11) ("count=" + $names.Count + " [" + ($names -join ',') + "]")
    Check 'tools.list.has-lua-eval' ($names -contains 'lua_eval') ($names -join ',')
    Check 'tools.list.has-gui-screenshot' ($names -contains 'gui_screenshot') ($names -join ',')
    Check 'tools.list.has-dbg-status' ($names -contains 'dbg_status') ($names -join ',')
    # 误导性工具已删：它们拿不到活动会话，参数形状也不对（只有 1 个 lua_eval 能真做事）
    Check 'tools.list.no-broken-dbg' (-not ($names -contains 'dbg_run') -and -not ($names -contains 'dbg_mem_read') -and -not ($names -contains 'dbg_breakpoint_set')) ($names -join ',')
    Check 'tools.list.schema' (($j.result.tools | Where-Object { $_.name -eq 'lua_eval' }).inputSchema.type -eq 'object') 'lua_eval inputSchema'

    # dbg_status：返回真实版本（不再写死 0.1.0）与调试模型说明
    $j = (ToolCall 'dbg_status' '{}' $sid).Content | ConvertFrom-Json
    $st = $j.result.content[0].text
    Check 'tool.dbg_status.ok' ($j.result.isError -ne $true) "isError=$($j.result.isError)"
    Check 'tool.dbg_status.version' ($st -match 'version\s*=\s*\S+' -and $st -notmatch 'version\s*=\s*0\.1\.0\s') "text=[$($st -split "`n" | Select-Object -First 3)]"
    Check 'tool.dbg_status.model-note' ($st -like '*没有*交互式*') 'should describe the blocking model'

    # 已删的工具 → -32602（未知工具）
    $j = (ToolCall 'dbg_run' '{"path":"C:\\Windows\\System32\\cmd.exe"}' $sid).Content | ConvertFrom-Json
    Check 'tools.removed.dbg_run' ($j.error.code -eq -32602) "code=$($j.error.code)"

    # serverInfo.version 与 /version 一致（构建期 git describe）
    $ver = (Invoke-WebRequest -Uri "http://127.0.0.1:$Port/version" -TimeoutSec 5).Content | ConvertFrom-Json
    Check 'initialize.version-matches' ($initJson.result.serverInfo.version -eq $ver.server) "mcp=$($initJson.result.serverInfo.version) http=$($ver.server)"

    # ------------------------------------------------------------ tools/call: lua_eval
    $r = ToolCall 'lua_eval' '{"code":"print(\"hello mcp\"); return 40 + 2"}' $sid
    $j = $r.Content | ConvertFrom-Json
    $txt = $j.result.content[0].text
    Check 'tool.lua_eval.ok' ($j.result.isError -ne $true) "isError=$($j.result.isError)"
    Check 'tool.lua_eval.print' ($txt -like '*hello mcp*') "text=[$txt]"
    Check 'tool.lua_eval.return' ($txt -like '*42*') "text=[$txt]"

    # 会话持久性：全局变量跨请求保留
    $null = ToolCall 'lua_eval' '{"code":"counter = (counter or 0) + 1"}' $sid
    $null = ToolCall 'lua_eval' '{"code":"counter = (counter or 0) + 1"}' $sid
    $j = (ToolCall 'lua_eval' '{"code":"return counter"}' $sid).Content | ConvertFrom-Json
    Check 'session.persistent' ($j.result.content[0].text -match '2') "text=$($j.result.content[0].text)"

    # 语法错 → 工具错误（isError）而不是 JSON-RPC error
    $j = (ToolCall 'lua_eval' '{"code":"local x = = 1"}' $sid).Content | ConvertFrom-Json
    Check 'tool.lua_eval.syntax-error' ($j.result.isError -eq $true) "isError=$($j.result.isError)"

    # 未知工具
    $j = (ToolCall 'no_such_tool' '{}' $sid).Content | ConvertFrom-Json
    Check 'tool.unknown' ($j.error.code -eq -32602) "error=$($j.error.code)"

    # ------------------------------------------------------------ tools/call: gui_screenshot（图片内容）
    $j = (ToolCall 'gui_screenshot' '{"x":0,"y":0,"w":64,"h":48}' $sid).Content | ConvertFrom-Json
    $img = $j.result.content | Where-Object { $_.type -eq 'image' }
    $pngOk = $false
    if ($img -and $img.data) {
        $bytes = [Convert]::FromBase64String($img.data)
        $pngOk = ($bytes.Length -gt 60 -and $bytes[0] -eq 0x89 -and $bytes[1] -eq 0x50 -and $bytes[2] -eq 0x4E -and $bytes[3] -eq 0x47)
    }
    Check 'tool.gui_screenshot.image' ($null -ne $img) "type=$($img.type)"
    Check 'tool.gui_screenshot.mime' ($img.mimeType -eq 'image/png') "mime=$($img.mimeType)"
    Check 'tool.gui_screenshot.png' $pngOk "base64 PNG 头"

    # ------------------------------------------------------------ 错误路径
    $j = (Mcp '{"jsonrpc":"2.0","id":4,"method":"no/such/method"}' @{ 'Mcp-Session-Id' = $sid }).Content | ConvertFrom-Json
    Check 'error.method-not-found' ($j.error.code -eq -32601) "code=$($j.error.code)"

    $j = (Mcp '{not json' @{ 'Mcp-Session-Id' = $sid }).Content | ConvertFrom-Json
    Check 'error.parse' ($j.error.code -eq -32700) "code=$($j.error.code)"

    $r = Mcp '{"jsonrpc":"2.0","id":5,"method":"ping"}' @{ 'Mcp-Session-Id' = '9999' }
    Check 'error.unknown-session' ($r.StatusCode -eq 404) "status=$($r.StatusCode)"

    # ------------------------------------------------------------ SSE（streamable HTTP）
    $r = Mcp '{"jsonrpc":"2.0","id":6,"method":"ping"}' @{ 'Mcp-Session-Id' = $sid; 'Accept' = 'application/json, text/event-stream' }
    $sseOk = ($r.Content -like 'event: message*') -and ($r.Content -like '*"jsonrpc":"2.0"*')
    Check 'sse.framing' $sseOk "body=$($r.Content.Substring(0, [Math]::Min(80, $r.Content.Length)))"

    # ------------------------------------------------------------ 批量（JSON 数组）
    $j = (Mcp '[{"jsonrpc":"2.0","id":20,"method":"ping"},{"jsonrpc":"2.0","id":21,"method":"tools/list"}]' @{ 'Mcp-Session-Id' = $sid }).Content | ConvertFrom-Json
    Check 'batch.two-responses' ($j.Count -eq 2) "count=$($j.Count)"
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
Write-Output ("mcp e2e: pass={0} fail={1}" -f $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
