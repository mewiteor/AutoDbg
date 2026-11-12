# =============================================================================
# tools/server_test/e2e_json.ps1 —— `--json` / `--out=<path>` 的端到端断言（T1 / D50）
#
# 来源：新写（D50 / T1，原 plan_cli.md，现合并进 plan.md）；Ok/Bad/Check 与产物检查沿用 e2e.ps1 的写法。
# 依赖：只需 PowerShell 7 + 已构建的 `.out/AutoDbg_<arch>.exe`；不需要 VM、不需要网络。
# 用法：
#   pwsh -File tools/server_test/e2e_json.ps1 [-Render 64|86] [-Variant modern|legacy]
# 退出码：0 = 全通过；1 = 有失败；2 = 前置缺失（产物不存在）
#
# 覆盖（每条都是硬断言，不许放宽、不许 SKIP）：
#   ① stdout 只有那一行 JSON，字段齐全（ok/exit/canceled/detached/pid/stdout/stderr/images/result）
#   ② `--out=<path>`：stdout 为空，文件内容 = 那一行 + 结尾 `\n`（二进制写，不转 CRLF）
#   ③ `result` 口径：表 → 值 / function → null 且 `ok` 仍 true / 多返回值 → 数组 / 无返回值 → null
#   ④ `pid` = 被调试进程（不是 AutoDbg 自己）；`detached`：`Detach()` → true、`Kill()` → false
#   ⑤ `images`：空也要输出 `[]`；`open_file`（写模式）的路径进登记表
#   ⑥ 编码口径：那一行必须纯 ASCII（非 ASCII 一律 `\uXXXX`）——legacy 变体的执行字符集是 GBK，
#      C 侧字面量必须按 CP936 解出来再转义，读回来仍是中文；解不出就逐字节 `\u00XX`（仍纯 ASCII）
#   ⑦ `--quiet` + `--json`：stdout 仍只有那一行
#   ⑧ `--json` 与 `--server` 互斥（退出码 2，stdout 不被污染）
#   ⑨ 退出码口径不变：成功 0；脚本不存在 = -1；语法/运行时错误**透传 Lua 错误码**（3 / 2），
#      且 JSON 的 `exit` 字段 = 进程退出码（不额外发明退出码）
#   ⑩ 被调试进程的 stdout 不能顶掉那一行（JSON 模式下它被指到本进程的 stderr）
#   ❗`-Variant legacy` 必须真过（这一段就是用来暴露 GBK/CP936 的）
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

# ❗x64 载荷里只有 modern（AGENTS.md §11.1）—— `-Render 64 -Variant legacy` 是参数组合错误，
#   不是失败：与「产物不存在」同口径（exit 2），否则会得到 24 条与 T1 无关的红。
if ($Render -eq '64' -and $Variant -eq 'legacy')
{
    Write-Output 'SKIP: x64 没有 legacy 变体（AGENTS.md §11.1）；legacy 口径请用 -Render 86 -Variant legacy'
    exit 2
}

$script:pass = 0
$script:fail = 0
$script:seq = 0
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "autodbg_json_e2e"
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

# 那一行必须纯 ASCII：这是 CP936 → UTF-8（或逐字节降级）口径的直接体现。
# modern（执行字符集 UTF-8）与 legacy（执行字符集 GBK）必须给出同一个结果。
function Is-AsciiLine([string]$s)
{
    for ($i = 0; $i -lt $s.Length; $i++)
    {
        if ([int]$s[$i] -ge 0x80) { return $false }
    }
    return $true
}

# 本地模式跑一个脚本：stdout 用文件承接（断言看到的是产物写出的字节，不受控制台代码页影响）。
# 返回 @{ code; self; raw; j; out; err }；self = AutoDbg 自己的 pid（用来证明 pid 不是它）
function Run-LuaLocal([string]$Source, [string[]]$Extra, [string]$ScriptPathOverride = '')
{
    $script:seq = 1 + [int]$script:seq
    $srcFile = Join-Path $TmpDir ("json{0}.lua" -f $script:seq)
    [System.IO.File]::WriteAllText($srcFile, $Source, (New-Object System.Text.UTF8Encoding($false)))

    $outFile = Join-Path $TmpDir ("json{0}.out" -f $script:seq)
    $errFile = Join-Path $TmpDir ("json{0}.err" -f $script:seq)

    $opt = @()
    if ($Variant) { $opt += "--variant=$Variant" }
    $opt += $Extra
    $opt += $(if ($ScriptPathOverride) { $ScriptPathOverride } else { $srcFile })

    $p = Start-Process -FilePath $Exe -ArgumentList $opt -RedirectStandardOutput $outFile `
        -RedirectStandardError $errFile -PassThru -NoNewWindow -Wait

    $raw = ''
    if (Test-Path $outFile) { $raw = [System.IO.File]::ReadAllText($outFile, [System.Text.Encoding]::UTF8) }
    $obj = $null
    if ($raw -ne '') {
        try { $obj = $raw | ConvertFrom-Json } catch { $obj = $null }   # 解析失败 = 那一行被污染：必须报 FAIL，不能中止整段
    }

    return @{ code = $p.ExitCode; self = $p.Id; raw = $raw; j = $obj; out = $outFile; err = $errFile }
}

$probe = 'print("hello 世界") return {a = 1}'

# ① stdout 只有那一行 + 字段齐全 + 纯 ASCII
$r = Run-LuaLocal $probe @('--json')
Check 'json.one-line' ($r.raw.EndsWith("`n") -and ($r.raw.TrimEnd("`n") -notmatch "[`r`n]")) "raw=[$($r.raw)]"
Check 'json.ascii-only' (Is-AsciiLine $r.raw) "raw=[$($r.raw)]"
$fields = if ($null -ne $r.j) { ($r.j.PSObject.Properties.Name | Sort-Object) -join ',' } else { '' }
Check 'json.fields-complete' ($fields -eq 'canceled,detached,exit,images,ok,pid,result,stderr,stdout') "fields=[$fields]"
Check 'json.ok-and-exit' ($r.j.ok -eq $true -and $r.j.exit -eq 0 -and $r.code -eq 0) "code=$($r.code) raw=[$($r.raw)]"
Check 'json.stdout-utf8' ($r.j.stdout -eq "hello 世界`n") "stdout=[$($r.j.stdout)]"
Check 'json.stderr-empty' ($r.j.stderr -eq '') "stderr=[$($r.j.stderr)]"
Check 'json.images-empty-literal' ($r.raw -match '"images":\[\]') "raw=[$($r.raw)]"
Check 'json.result-table' ($null -ne $r.j.result -and $r.j.result.a -eq 1) "result=[$($r.j.result | ConvertTo-Json -Compress)]"
Check 'json.pid-zero-unattached' ($r.j.pid -eq 0) "pid=$($r.j.pid)"
Check 'json.canceled-detached-false' ($r.j.canceled -eq $false -and $r.j.detached -eq $false) "canceled=$($r.j.canceled) detached=$($r.j.detached)"

# ② --out=<path>：stdout 为空，文件内容 = 那一行 + 结尾 `\n`（二进制写，不转 CRLF）
$outFile = Join-Path $TmpDir 'json_out.bin'
$r2 = Run-LuaLocal $probe @('--json', "--out=$outFile")
Check 'json.out.stdout-empty' ($r2.raw -eq '') "stdout=[$($r2.raw)] code=$($r2.code)"
$fileText = ''
if (Test-Path $outFile) { $fileText = [System.IO.File]::ReadAllText($outFile, [System.Text.Encoding]::UTF8) }
Check 'json.out.file-equals-line' ($fileText -eq $r.raw) "file=[$fileText] want=[$($r.raw)]"
Check 'json.out.no-crlf' ($fileText.EndsWith("`n") -and ($fileText -notmatch "`r")) "file=[$fileText]"
# `--out` 时 stdout 必须为空（D50），所以这一条只能读文件，不能读 stdout。
$jf = $null
if ($fileText -ne '') { try { $jf = $fileText | ConvertFrom-Json } catch { $jf = $null } }
Check 'json.out.both-file-wins' ($null -ne $jf -and $jf.stdout -eq "hello 世界`n" -and $jf.ok -eq $true) "file-json=[$($jf | ConvertTo-Json -Compress)]"

# ③ `result` 口径
$r3 = Run-LuaLocal 'return function() end' @('--json')
Check 'json.result-function-null' ($r3.j.ok -eq $true -and ($r3.raw -match '"result":null')) "raw=[$($r3.raw)]"
$r4 = Run-LuaLocal 'return 1, 2, 3' @('--json')
Check 'json.result-multi-array' ($r4.raw -match '"result":\[1,2,3\]') "raw=[$($r4.raw)]"
$r5 = Run-LuaLocal 'print("x")' @('--json')
Check 'json.result-no-return-null' ($r5.j.ok -eq $true -and ($r5.raw -match '"result":null')) "raw=[$($r5.raw)]"

# ④ pid / detached / images：跑一个真被调试进程（Detach → true，Kill → false）
$imgPath = (Join-Path $TmpDir 'json_img.bin').Replace([char]92, '/')
# ❗长括号 [[...]] 里**不能**再写引号：`[[ 'X' ]]` 的字符串内容是带引号的 `'X'`，
#   路径就坏了（open_file 返回 nil → 文件不落盘 → `images` 空）。这里必须直接嵌路径。
$dbgSrc = @'
local f = open_file([[IMGPATH]], 'wb')
if f then f:write('abc'); f:close() end
Run({ cmdline = [[C:\Windows\System32\cmd.exe]] .. ' /c ping -n 4 127.0.0.1', callback = function()
    Detach()
end })
print('pid')
'@
$dbgSrc = $dbgSrc.Replace('IMGPATH', $imgPath)
$r6 = Run-LuaLocal $dbgSrc @('--json')
Check 'json.pid-is-debuggee' ($r6.j.pid -gt 0 -and $r6.j.pid -ne $r6.self) "pid=$($r6.j.pid) selfpid=$($r6.self) raw=[$($r6.raw)]"
Check 'json.detached-true' ($r6.j.detached -eq $true) "detached=$($r6.j.detached) raw=[$($r6.raw)]"
Check 'json.images-listed' (@($r6.j.images) -contains $imgPath) "images=[$($r6.j.images -join ',')] raw=[$($r6.raw)]"
# 被目标污染：跑一个会往控制台写输出的被调试进程时，stdout 仍然必须只有那一行。
Check 'json.one-line-with-spawned-target' ($r6.raw.EndsWith("`n") -and (($r6.raw.TrimEnd("`n")) -notmatch "[`r`n]")) "raw=[$($r6.raw)]"
Check 'json.spawning-target-stdout-clean' ($null -ne $r6.j -and $r6.j.ok -eq $true) "raw=[$($r6.raw)]"
$r7 = Run-LuaLocal ($dbgSrc -replace 'Detach\(\)', 'Kill()') @('--json')
Check 'json.kill-detached-false' ($r7.j.detached -eq $false -and $r7.j.pid -gt 0) "detached=$($r7.j.detached) raw=[$($r7.raw)]"

# ⑤ CP936 口径：C 侧字面量（legacy 是 GBK 字节）必须按 CP936 解出来再转义，读回来仍是中文
$r8 = Run-LuaLocal '' @('--json') 'C:/definitely-not-there/adbg-json-nope.lua'
Check 'json.cp936-escaped' ($r8.j.ok -eq $false -and ($r8.raw -match '\\u[0-9a-fA-F]{4}') -and ($r8.j.stderr -like '*文件不存在*')) "raw=[$($r8.raw)]"
Check 'json.cp936-line-ascii' (Is-AsciiLine $r8.raw) "raw=[$($r8.raw)]"

# ⑥ --quiet + --json：stdout 仍然只有那一行
$r9 = Run-LuaLocal 'print("q")' @('--quiet', '--json')
Check 'json.quiet-line-ascii' (Is-AsciiLine $r9.raw) "raw=[$($r9.raw)]"
Check 'json.quiet-stdout-field' ($r9.j.stdout -eq "q`n" -and $r9.j.ok -eq $true) "raw=[$($r9.raw)]"

# ⑦ --json 与 --server 互斥：报错走 stderr，退出码 2，stdout 不被污染
$r10 = Run-LuaLocal 'print("x")' @('--json', '--server')
Check 'json.server-exclusive-exit2' ($r10.code -eq 2) "code=$($r10.code)"
Check 'json.server-exclusive-stdout-clean' ($r10.raw -eq '') "raw=[$($r10.raw)]"

# ⑧ 退出码口径不变：成功 0；脚本不存在 = -1；语法/运行时错误**透传 Lua 错误码**（不额外发明）
$r11 = Run-LuaLocal 'local x = ' @('--json')
Check 'json.exitcode-lua-syntax' ($r11.j.ok -eq $false -and $r11.j.exit -eq $r11.code -and $r11.code -eq 3) "code=$($r11.code) exit=$($r11.j.exit)"
$r12 = Run-LuaLocal '' @('--json') 'C:/definitely-not-there/adbg-json-missing.lua'
Check 'json.exitcode-missing-file' ($r12.j.ok -eq $false -and $r12.j.exit -eq -1 -and $r12.code -eq -1) "code=$($r12.code) exit=$($r12.j.exit)"
$r13 = Run-LuaLocal 'error("boom")' @('--json')
Check 'json.exitcode-lua-runtime' ($r13.j.ok -eq $false -and $r13.j.exit -eq $r13.code -and $r13.code -eq 2) "code=$($r13.code) exit=$($r13.j.exit) raw=[$($r13.raw)]"

Write-Output ''
Write-Output ("e2e_json: variant={0} pass={1} fail={2}" -f $(if ($Variant) { $Variant } else { 'auto' }), $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
