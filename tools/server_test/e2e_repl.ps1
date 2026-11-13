# =============================================================================
# tools/server_test/e2e_repl.ps1 —— `--repl` 的本地门禁（T7 切片 D1，plan_t7.md §11.7 的 D 行）
#
# 来源：新写（切片 D1：`--repl` + ABI 4 → 5）。与 `e2e_dbg.ps1` 同一套参数/输出风格
#   （`-Render 64|86 [-Variant legacy]`、`Check`/`Ok`/`Bad`、退出码 0/1/2），但**不需要脚本**：
#   REPL 的输入是 stdin，所以这里全部用「喂 stdin + 断言 stdout/退出码」。
#
# 用法：pwsh -File tools/server_test/e2e_repl.ps1 [-Render 64|86] [-Variant ''|modern|legacy]
# 退出码：0 = 全过；1 = 有失败；2 = 前置缺失（产物不存在 / x64+legacy 组合）
#
# 覆盖（10 条，均为纯本地）：
#   repl.stdin-piped          `print(1+1)` → stdout 有 `2`、退出码 0、**stdout 里零提示符**
#                             （管道下打提示符会污染所有人的输出比对，§4 第 4 条）
#   repl.multi-line           `for i=1,2 do` / `print(i)` / `end` → `1` 与 `2`（§11.6 第 2 条）
#   repl.shorthand            `=1+1` → `2`（`=` 改写成 `return`，§11.6 第 1 条）
#   repl.exit-code-eof        空 stdin → 退出码 0（没有命令 = 一路顺利）
#   repl.exit-code-syntax-error  `local x =` 后 EOF → 退出码 **3**（LUA_ERRSYNTAX，透传 §3）
#   repl.last-error-code      先 `=1+1`（0）再 `error("boom")` → 退出码 **2**（LUA_ERRRUN）：
#                             退出码 = **最后一条命令**的码（§4 第 7 条）
#   repl.json-exclusive       `--repl --json` → 启动器报错、退出码 2（D50：那一行独占 stdout）
#   repl.state-shared         `=dbg.caps().arch` → `x86` / `x64`：证明 REPL 与脚本路径共用
#                             同一份 `lua_State`（`dbg`/`gui`/`open_file`/`help` 都在）
#   repl.remote-exclusive     **D2b**：`--repl --remote=<host:port>` → 启动器报错、退出码 2
#                             （D1 里这条是「静默忽略」，现在不再静默）
#   repl.exit-report          **D2b**：`dbg.create{ imagepath = <fixture> }` 后 EOF → stderr 含
#                             `unclosed session` 与 `detaching`（D54 的安全网），退出码仍是 0；
#                             且进程没漏（fixture 是 8 轮后自退的靶子）。只在 x86 跑（fixture 是
#                             x86-only，`parse_pe` 按编译期 `_WIN64` 判位数）。
#
# ❗喂 stdin 的两种写法都试过，这里用 **`Start-Process -RedirectStandardInput <file>`**：
#   · 稳：退出码走 `$p.ExitCode`、stdout/stderr 各自落文件（与 `e2e_dbg.ps1` 的 `Run-Local`
#     同一套读法），不受 PowerShell 管道编码/换行转换影响；
#   · `'…' | & $Exe --repl` 也能跑（PS7 把字符串按 UTF-8 送进子进程 stdin），但 `$LASTEXITCODE`
#     与输出混在同一个管道里、且 PS 会给最后一行补换行 —— 断言「空 stdin」和「没有提示符」时
#     噪声更多。所以门禁用前者，两种都手测过结果一致。
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
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_repl_e2e'
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

# 把一段文本喂给 `AutoDbg_<arch>.exe --repl`，拿退出码 + stdout + stderr。
# `stdinText` 原样写入临时文件（CRLF 由调用方决定：门禁里一律用 `\n`，源码里也写 `\n`）。
function Run-Repl([string]$stdinText, [string[]]$Extra = @(), [int]$TimeoutMs = 30000)
{
    $in  = Join-Path $TmpDir 'stdin.txt'
    $out = Join-Path $TmpDir 'out.txt'
    $err = Join-Path $TmpDir 'err.txt'
    Remove-Item $in, $out, $err -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText($in, $stdinText, (New-Object System.Text.UTF8Encoding($false)))

    $replArgs = @('--repl') + $VariantArgs + $Extra
    $p = Start-Process -FilePath $Exe -ArgumentList $replArgs -RedirectStandardInput $in `
        -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -NoNewWindow
    $null = $p.WaitForExit($TimeoutMs)
    $o = if (Test-Path $out) { Get-Content $out -Raw } else { '' }
    $e = if (Test-Path $err) { Get-Content $err -Raw } else { '' }
    if ($null -eq $o) { $o = '' }
    if ($null -eq $e) { $e = '' }
    return [pscustomobject]@{ code = $(if ($p.HasExited) { $p.ExitCode } else { -999 }); out = $o; err = $e }
}

# 真·管道喂 stdin（`@('…') | & exe --repl`）：这条路径的 EOF 是 `ReadFile` 失败 +
# `ERROR_BROKEN_PIPE`，与 `Run-Repl` 的「重定向文件 + 读回 0 字节」是两个不同分支
# （repl.cpp 的 `read_once` 两条都要认）—— 所以门禁两条都跑。
function Run-Repl-Pipe([string[]]$Lines)
{
    $replArgs = @('--repl') + $VariantArgs
    $o = @($Lines) | & $Exe @replArgs
    $code = $LASTEXITCODE
    return [pscustomobject]@{ code = $code; out = ($o -join "`n") }
}

# ---------------------------------------------------------------- ① 真管道：读、求值、无提示符
# 这一条走的是**真管道**（不是重定向文件），所以它同时覆盖 `read_once` 的
# `ERROR_BROKEN_PIPE` 那一支；断言里「stdout 里零提示符」是结构性的要求：stdout 不是控制台，
# 提示符一个字节都不能出现（否则所有自动化输出比对都会带噪音）。
$r = Run-Repl-Pipe @('print(1+1)')
Check 'repl.stdin-piped' ($r.code -eq 0 -and $r.out -match '(?m)^2\s*$' -and $r.out -notmatch 'autodbg>') `
    "code=$($r.code) out=[$($r.out)]"

# ---------------------------------------------------------------- ② 多行续行
$r = Run-Repl "for i=1,2 do`nprint(i)`nend`n"
Check 'repl.multi-line' ($r.code -eq 0 -and $r.out -match '(?m)^1\s*$' -and $r.out -match '(?m)^2\s*$') `
    "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

# ---------------------------------------------------------------- ③ `=expr` 简写
$r = Run-Repl "=1+1`n"
Check 'repl.shorthand' ($r.code -eq 0 -and $r.out -match '(?m)^2\s*$') `
    "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

# ---------------------------------------------------------------- ④ 空 stdin = 干净 EOF
$r = Run-Repl ''
Check 'repl.exit-code-eof' ($r.code -eq 0) "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

# ---------------------------------------------------------------- ⑤ 语法错在 EOF 处 = 3
# `local x =` 是「还没输完」（两家 Lua 都在 `<eof>` 处报错）⇒ REPL 继续读；
# 到 EOF 就把它当**语法错**上报，退出码透传 Lua 的 LUA_ERRSYNTAX = 3（AGENTS.md §3）。
$r = Run-Repl "local x =`n"
Check 'repl.exit-code-syntax-error' ($r.code -eq 3 -and $r.err -match 'syntax error') `
    "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

# ---------------------------------------------------------------- ⑥ 退出码 = 最后一条命令的码
# 实测口径（写死在这里）：`error("boom")` 是运行错 ⇒ LUA_ERRRUN = 2；前一条 `=1+1` 的 0 被覆盖，
# 所以退出码是 **2**（不是 0、也不是「任何错误都变 2」——⑤ 证明语法错仍是 3）。
$r = Run-Repl "=1+1`nerror(`"boom`")`n"
Check 'repl.last-error-code' ($r.code -eq 2 -and $r.out -match '(?m)^2\s*$' -and $r.err -match 'runtime error') `
    "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

# ---------------------------------------------------------------- ⑦ `--repl` 与 `--json` 互斥
# D50：`--json` 要求 stdout 只有那一行，REPL 的输出没法塞进信封 ⇒ 启动器拒绝、退出码 2。
$r = Run-Repl "=1+1`n" @('--json')
Check 'repl.json-exclusive' ($r.code -eq 2 -and $r.err -match '--repl cannot be combined with --json') `
    "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

# ---------------------------------------------------------------- ⑧ 复用同一个 state
# `dbg.caps()` 是 `dbg` 表上的能力上报（切片 A），REPL 里能取到就说明它与脚本路径共用
# 同一份 `create_state_with_libs()`（不是另建了一个只装了基库的 state）。
$want = if ($Render -eq '64') { 'x64' } else { 'x86' }
$r = Run-Repl "=dbg.caps().arch`n"
Check 'repl.state-shared' ($r.code -eq 0 -and $r.out -match "(?m)^$want\s*$") `
    "want=$want code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

# ---------------------------------------------------------------- ⑨ `--repl` 与 `--remote` 互斥
# D2b：`--remote` 是「把脚本送到远端 server 去跑」，它不转发 stdin；与 `--repl` 叠加只会让
# `--remote` 被静默忽略（D1 的行为）⇒ 现在显式报错、退出码 2（与 `--repl --json` 同一风格）。
# `--repl --server` 是**可用**的（D2b），它的断言在 e2e_tcp.ps1 的 `repl.server-state`。
$r = Run-Repl "=1+1`n" @('--remote=127.0.0.1:1')
Check 'repl.remote-exclusive' ($r.code -eq 2 -and $r.err -match '--repl cannot be combined with --remote') `
    "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"

# ---------------------------------------------------------------- ⑩ 退出时的会话报告（D54 安全网）
# 退出前（`lua_close` 之前）查 `dbg.sessions()`：>0 就先打一行 `repl: N unclosed session(s) -
# detaching`，再逐个 `d:detach()`（没有 `DebugActiveProcessStop` 的平台退 `d:kill()`）。
# 这条**不改退出码**（仍是「最后一条命令的码」= 0），报告只进 stderr。
# 只用 x86 fixture（`.out/fixtures/x86_loop.exe`，8 轮后自退）：x64 的 `parse_pe` 按编译期
# `_WIN64` 判位数 ⇒ 32 位靶子在 64 位产物下建不起来。
if ($Render -ne '86')
{
    Write-Output 'SKIP: repl.exit-report 只在 x86 跑（fixture 是 x86-only）'
}
else
{
    $fx = Join-Path $Root '.out/fixtures/x86_loop.exe'
    if (-not (Test-Path $fx))
    {
        Bad 'repl.exit-report' '先 make.bat a 86 生成 .out/fixtures/x86_loop.exe'
    }
    else
    {
        $fxLua = $fx.Replace('\', '/')
        # `=d ~= nil` 既证明 create 成功，又让最后一条命令的码保持 0（= 退出码口径的旁证）。
        $r = Run-Repl "d = dbg.create{ imagepath = '$fxLua' }`n=d ~= nil`n"
        Check 'repl.exit-report' ($r.code -eq 0 -and $r.out -match '(?m)^true\s*$' -and `
            $r.err -match 'unclosed session' -and $r.err -match 'detaching') `
            "code=$($r.code) out=[$($r.out)] err=[$($r.err)]"
    }
}

# ---------------------------------------------------------------- 汇总
Write-Output ("repl e2e ({0}{1}): {2} passed, {3} failed" -f $Render, $(if ($Variant) { ", $Variant" } else { '' }), $script:pass, $script:fail)
if ($script:fail -ne 0) { exit 1 }
exit 0
