# =============================================================================
# tools/boot_test/e2e_boot.ps1 —— 引导器（handoff）的本地门禁（T8 切片 a2 / D63）
#
# 来源：新写（plan_bootloader.md §6 的 14 条）。`Check` / `-Render` / `pass=N fail=M` / 退出码口径
#   照 `tools/server_test/e2e_repl.ps1`（同一套风格），但跑的是**进程边界**而不是单进程：
#   主 exe（x86）落盘 + CreateProcess → child（x64）执行脚本 → 退出码透传回来。
#
# 用法：pwsh -File tools/boot_test/e2e_boot.ps1 [-Render 64|86] [-Variant ''|modern|legacy]
# 退出码：0 = 全过；1 = 有失败；2 = 前置缺失（产物不存在 / 没有 blob 条目 / 32 位 host / 参数组合错误）
# 依赖：**一次两架构全量构建**（`make.bat`，64 先、86 后 —— 86 的 pack 边把 64 的产物当隐式依赖），
#   且必须在 64 位 host 上跑（32 位 host 上 handoff 分支不可达 → 只能 SKIP）。
#
# 覆盖（14 条，名字逐字用 plan_bootloader.md §6）：
#   handoff.cmdline-passthrough    含引号的 argv[0]、带空格/引号的脚本参数、中文路径
#   handoff.exit-code              0 / -1(0xFFFFFFFF) / 2 / Lua 3
#   handoff.no-stdout              引导器一行都不写；`--json` 那一行由 child 写
#   handoff.identical-no-rewrite   build 串一致 → 不清写（mtime 不变）；被锁 → 明确报错
#   handoff.job-kill-on-close      主 exe 被强杀 → child 不留（WMI 查 ParentProcessId）
#   handoff.job-nested-fails-clean 已在 job 里 → 干净降级（不报错退 2）
#   handoff.no-dialog              child 不弹对话框（见下面的「代理判据」）
#   handoff.script-relative-base   env `AUTODBG_BASE_DIR` 生效（见「代理判据」）
#   handoff.ro-media               只读/无权限目录 → 明确报错（不清写、不静默回退）
#   version.no-extract             `--version` 不触发落盘（只读目录上也能跑）
#   version.parity.main-vs-exec    主 exe 报的 arch/proto/abi == child 报的
#   list.blob-entry-kind           `--list` 能区分 module / exec
#   boot.is-x86                    主 exe 必须是 32 位 PE（Machine = 0x14C）
#   handoff.non-exe-name           CreateProcess 按 PE 头判断、不看扩展名（`.bin` 实测）
#
# ❗代理判据（本门禁里「测不到哪一段」必须写清楚，别把它当直接证据）：
#   · `handoff.no-dialog`：**真·非交互会话（无桌面）在门禁里构造不出来**。能测的是「主 exe 拿不到脚本时
#     child 从未被创建」——用「主 exe 挂住等对话框期间，WMI 里没有任何以它为父进程的 child」判；
#     「child 收到的脚本路径就是主 exe 给的那一份」是直接判据（脚本打印自己的 chunkname 与传入参数对照）。
#   · `handoff.script-relative-base`：**直接判据**（相对路径已经走 `common/base_dir`：env > 主 exe 目录）。
#     跑的时候 CWD 故意选另一个目录，所以「解析到原始目录」只能靠解析后的路径成立：读 fixture + 写一个
#     相对文件，断它落在原始目录而不是 CWD。`open_file` 的相对路径走 base_dir，`io.open` 仍按 CWD ⇒
#     这里必须用 `open_file`（用 `io.open` 就只剩「CWD 恰好等价」的代理）。
#   · `handoff.identical-no-rewrite`：mtime 不变是「没写」的直接证据；「build 串一致」是**逐字节比较**
#     的实现口径（本门禁比较的是文件内容 == 构建期 blob 产物，与实现的判据同源）。
#   · `handoff.non-exe-name`：CreateProcess 不看扩展名这件事只能实测「按 `.bin` 名字跑起来了 + 跑的是
#     x64 那份（`dbg.caps().arch` == x64）」，读不到 CreateProcess 内部的判据。
#   · `handoff.cmdline-passthrough`：child 的 **argv[0]** 从 Lua 里读不到 ⇒ 引号口径直接判的是「参数
#     逐字节还原」；argv[0] 只能间接判（落盘文件存在、且被 CreateProcess 成功执行）。
#   · `version.parity.main-vs-exec`：`proto`（协议版本 2）目前不在 `--version` 那一行里（只有 server
#     握手 / `info` 帧有），所以本条对的是 `--version` 实际报出的字段（version / source / dirty / abi /
#     arch）。`proto` 要不要进版本行是拍板项，门禁不自己决定。
#   · `handoff.no-blob-fallback` 不设断言（plan §5.5 第 1 条：构建期挡），`boot.no-wow64-branch`
#     挂 `plan.md` §1.3 的 Win9x 矩阵（本地测不了）。
#
# `-Variant legacy` 也要能跑：显式 legacy 是「不 handoff」那条规则（§13 D63 / 判定规则 ④），所以这一轮
#   断言的是**不 handoff 的那一支**（不写文件、env 不设、主 exe 自己跑 x86 legacy 模块）。门禁按
#   `$ExpectHandoff` 分流，不是把断言关掉。
# ❗全程带 `--quiet`：handoff 的错误在非 `--quiet` 时也会弹窗（与 launcher 口径一致，已定）。模态框在
#   无头会话里不可断言，不带 `--quiet` 就会把跑测挂住，所以门禁断的是 stderr 那一半。
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

# ❗可切换点（切片 b 只改这一行）：主 exe。D63 落地后是 `.out/AutoDbg.exe`；现在仍用构建期的
#   x86 产物 `AutoDbg_86.exe`（x64 半份是构建期从 `AutoDbg_64.exe` 喂进 blob 的同一份字节）。
#   `boot.is-x86` 断言的就是这一份的 PE 头（0x14C）。
$MainExe = Join-Path $Root '.out/AutoDbg_86.exe'
$BlobSrc = Join-Path $Root '.out/AutoDbg_64.exe'      # 同一份字节的构建期产物（version.parity 用）

if (-not (Test-Path $MainExe))
{
    Write-Output "SKIP: 产物不存在：$MainExe（先跑一次两架构全量构建）"
    exit 2
}

$HostIs64 = [Environment]::Is64BitOperatingSystem
if (-not $HostIs64)
{
    Write-Output 'SKIP: 32 位 host 上 handoff 分支不可达（IsWow64Process 为假），14 条里只剩主 exe 自己跑'
    exit 2
}
if ($Render -eq '64' -and $Variant -eq 'legacy')
{
    Write-Output 'SKIP: x64 没有 legacy 变体（AGENTS.md §11.1）；legacy 口径请用 -Render 86 -Variant legacy'
    exit 2
}

# 显式 --variant=legacy ⇒ 判定规则 ④ ⇒ **不 handoff**（落盘的 x64 半份恒是 modern）。
$ExpectHandoff = ($Variant -ne 'legacy')
# 执行脚本的那个进程该报的位数：handoff → child（x64）；不 handoff → 主 exe 自己的模块（x86）
$WantArch = if ($ExpectHandoff) { 'x64' } else { 'x86' }

$script:pass = 0
$script:fail = 0
$GateTmp = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_boot_e2e'
New-Item -ItemType Directory -Force -Path $GateTmp | Out-Null

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

# 字节级包含判断（legacy 变体的执行字符集是 GBK：中文只能比字节，不能比字符串）
function Contains-Bytes([byte[]]$bytes, [byte[]]$pat)
{
    for ($i = 0; $i + $pat.Length -le $bytes.Length; $i++)
    {
        $same = $true
        for ($j = 0; $j -lt $pat.Length; $j++)
        {
            if ($bytes[$i + $j] -ne $pat[$j]) { $same = $false; break }
        }
        if ($same) { return $true }
    }
    return $false
}

# ---- 跑一次主 exe（可选 TMP 覆盖 / 工作目录），拿退出码 + stdout + stderr ----
# TMP 靠「先改 $env:TMP 再 Start-Process」传给子进程（Start-Process 继承当前进程的环境）；
# 跑完立刻还原，否则后面几条的落盘路径会串。
# ❗默认 TMP = $GateTmp：落盘位置必须可预定（否则 `handoff.identical-no-rewrite` 对的是系统 TMP 而不是
#   门禁目录），`handoff.ro-media` 那几条才需要自己传目录。
# ❗默认带 `--quiet`（见头注释：弹窗在无头会话里不可断言，不 `--quiet` 会把跑测挂住）。
function Run-Boot([string[]]$Extra, [string]$WorkDir = $GateTmp, [string]$TmpOverride = $GateTmp, [int]$TimeoutMs = 30000)
{
    $out = Join-Path $GateTmp 'out.txt'
    $err = Join-Path $GateTmp 'err.txt'
    Remove-Item $out, $err -ErrorAction SilentlyContinue

    $savedTmp = $env:TMP
    if ($TmpOverride) { $env:TMP = $TmpOverride }

    $argList = @($Extra) + @('--quiet') + $VariantArgs
    try
    {
        $p = Start-Process -FilePath $MainExe -ArgumentList $argList -WorkingDirectory $WorkDir `
            -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -NoNewWindow
        $null = $p.WaitForExit($TimeoutMs)
        $code = $(if ($p.HasExited) { $p.ExitCode } else { -999 })
    }
    finally
    {
        $env:TMP = $savedTmp
    }

    $o = if (Test-Path $out) { Get-Content $out -Raw } else { '' }
    $e = if (Test-Path $err) { Get-Content $err -Raw } else { '' }
    if ($null -eq $o) { $o = '' }
    if ($null -eq $e) { $e = '' }
    return [pscustomobject]@{ code = $code; out = $o; err = $e; outf = $out; errf = $err }
}

# ---- 门禁脚本：打印自己看到的三件事（env / 位数 / chunkname）+ 每个脚本参数 ----
# 直接判据是「child 看到的 == 主 exe 传入的」；`dbg.caps().arch` 是执行者的位数（handoff 后 = x64）。
$ProbeLua = @'
print('base=' .. tostring(os.getenv('AUTODBG_BASE_DIR')))
print('arch=' .. dbg.caps().arch)
print('chunk=' .. debug.getinfo(1, 'S').source)
local n = select('#', ...)
for i = 1, n do print('arg' .. i .. '=' .. tostring(select(i, ...))) end
'@

function Write-Lua([string]$Path, [string]$Text)
{
    [IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
    return $Path
}

function Get-PEMachine([string]$Path)
{
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64) { return 0 }
    $lf = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($lf -le 0 -or $lf + 6 -gt $bytes.Length) { return 0 }
    if ($bytes[$lf] -ne 0x50 -or $bytes[$lf + 1] -ne 0x45) { return 0 }   # 'PE\0\0'
    return [BitConverter]::ToUInt16($bytes, $lf + 4)
}

# `--version` 那一行：AutoDbg <ver> (<source>[, dirty], abi <n>, <arch>)
function Parse-Version([string]$line)
{
    if ($line -notmatch '^AutoDbg (\S+) \(([^,]+)(, dirty)?, abi (\d+), (x\d+)\)\s*$') { return $null }
    return [pscustomobject]@{
        version = $Matches[1]; source = $Matches[2]; dirty = ($Matches[3] -ne '');
        abi = [int]$Matches[4]; arch = $Matches[5]
    }
}

function Get-ChildPids([int]$ParentPid)
{
    return @(Get-CimInstance -ClassName Win32_Process -Filter "ParentProcessId=$ParentPid")
}

# ============================================================================
# ① 前置：主 exe 必须是 32 位 PE（D63 的便携性前提），`--list` 必须能区分 module / exec
# ============================================================================
Check 'boot.is-x86' ((Get-PEMachine $MainExe) -eq 0x14C) `
    ("Machine=0x{0:X}" -f (Get-PEMachine $MainExe))

$r = Run-Boot @('--list')
$blobName = ''
$hasModule = $false
foreach ($line in ($r.out -split "`n"))
{
    if ($line -match 'kind=exec')
    {
        if ($line -match 'launcher:\s+(\S+)') { $blobName = $Matches[1] }
    }
    elseif ($line -match 'kind=module') { $hasModule = $true }
}
Check 'list.blob-entry-kind' ($blobName -ne '' -and $hasModule -and $blobName -eq 'adbg64.bin') `
    "exec=$blobName module=$hasModule"

if (-not $blobName)
{
    Write-Output '前置缺失：载荷里没有 exec 条目（先 make.bat 跑一次 64 + 86 全量构建）'
    Write-Output ("boot e2e ({0}{1}): {2} passed, {3} failed" -f $Render, $(if ($Variant) { ", $Variant" } else { '' }), $script:pass, $script:fail)
    exit 1
}

$BlobPath = Join-Path $GateTmp $blobName
# ❗起跑前清掉上一轮跑测留下的落盘文件（`handoff.identical-no-rewrite` 的第一次跑必须是「真的第一次」）
Remove-Item $BlobPath -ErrorAction SilentlyContinue

# ============================================================================
# ② version.no-extract + ro-media：只读 / 无权限目录
#    判据：改 DACL 能成就用只读目录；改不动（非管理员）就用「目录不存在」——GetTempPathA 只按 TMP
#    拼串、不校验存在，所以 CreateFileW 一定失败，「明确报错、不清写、不静默回退」的判据不变。
# ============================================================================
$RoDir = Join-Path $GateTmp 'ro_media'
New-Item -ItemType Directory -Force -Path $RoDir | Out-Null
$RoIsAcl = $false
try
{
    $null = & icacls $RoDir /inheritance:r /grant:r "$($env:USERNAME):(RX)"
    try { [IO.File]::WriteAllText((Join-Path $RoDir 'probe.txt'), 'x'); $RoIsAcl = $true } catch { $RoIsAcl = $false }
    Remove-Item (Join-Path $RoDir 'probe.txt') -ErrorAction SilentlyContinue
}
catch { $RoIsAcl = $false }
if (-not $RoIsAcl) { $RoDir = Join-Path $GateTmp 'no_such_dir' }

$RoLua = Write-Lua (Join-Path $GateTmp 'ro.lua') "print('ran')`n"

$r = Run-Boot @($RoLua) $GateTmp $RoDir
if ($ExpectHandoff)
{
    Check 'handoff.ro-media' ($r.code -eq 2 -and $r.err -match 'handoff:' -and $r.out -notmatch 'ran' -and `
        -not (Test-Path (Join-Path $RoDir $blobName))) `
        "rodir=$RoDir acl=$RoIsAcl code=$($r.code) err=[$($r.err.Trim())] out=[$($r.out.Trim())]"
}
else
{
    # 显式 legacy ⇒ 不 handoff ⇒ 根本没有「落盘」这一步 ⇒ 只读目录不构成失败。
    # 这一支断的是「跑起来了 + 一个字节都没写 + 没有 handoff 诊断」，不是把断言关掉。
    Check 'handoff.ro-media' ($r.code -eq 0 -and ($r.out -match 'ran') -and ($r.err -notmatch 'handoff:') -and `
        -not (Test-Path (Join-Path $RoDir $blobName))) `
        "legacy（规则 ④）：没有落盘这步，只读目录不报错；code=$($r.code) out=[$($r.out.Trim())]"
}

# `--version` 不触发落盘（它是出问题时第一个跑的诊断命令）→ 同一个目录里必须还能跑
$r = Run-Boot @('--version') $GateTmp $RoDir
$vMain = Parse-Version ($r.out.Trim())
Check 'version.no-extract' ($r.code -eq 0 -and $null -ne $vMain -and -not (Test-Path (Join-Path $RoDir $blobName))) `
    "rodir=$RoDir code=$($r.code) out=[$($r.out.Trim())]"

# ============================================================================
# ③ identical-no-rewrite：第一次跑落盘、第二次跑 mtime / 内容不变；被锁 → 明确报错
# ============================================================================
$ProbeLua1 = Write-Lua (Join-Path $GateTmp 'probe.lua') $ProbeLua

if (-not $ExpectHandoff)
{
    # 显式 `--variant=legacy` ⇒ 判定规则 ④ ⇒ **不 handoff**：这一条测的是「不落盘」那一支
    # （x64 host 上唯一能跑到 legacy 的路径就是主 exe（x86）自己跑）
    $r = Run-Boot @($ProbeLua1)
    $r = Run-Boot @($ProbeLua1)
    Check 'handoff.identical-no-rewrite' ($r.code -eq 0 -and -not (Test-Path $BlobPath) -and ($r.out -match 'base=nil')) `
        "legacy（规则 ④）：一个字节都没写、文件不存在、env=nil；code=$($r.code)"
}
else
{
    # 第一次跑必须落盘（不能拿上一轮跑测的残留文件当证据）
    $r1 = Run-Boot @($ProbeLua1)
    if (-not (Test-Path $BlobPath))
    {
        Check 'handoff.identical-no-rewrite' $false "第一次跑没有落盘：$BlobPath（code=$($r1.code)）"
    }
    else
    {
    $before = [IO.File]::GetLastWriteTime($BlobPath)
    $bytes1 = [IO.File]::ReadAllBytes($BlobPath)
    $r2 = Run-Boot @($ProbeLua1)
    $same = ([IO.File]::GetLastWriteTime($BlobPath) -eq $before)
    $bytes2 = [IO.File]::ReadAllBytes($BlobPath)
    $sameContent = ($bytes1.Length -eq $bytes2.Length)
    for ($i = 0; $i -lt $bytes1.Length -and $sameContent; $i++)
    {
        if ($bytes1[$i] -ne $bytes2[$i]) { $sameContent = $false }
    }
    # 与构建期 blob 产物逐字节比较（实现里「build 串一致」就是这条）
    $sameSrc = $false
    if (Test-Path $BlobSrc)
    {
        $src = [IO.File]::ReadAllBytes($BlobSrc)
        $sameSrc = ($src.Length -eq $bytes2.Length)
        for ($i = 0; $i -lt $src.Length -and $sameSrc; $i++)
        {
            if ($src[$i] -ne $bytes2[$i]) { $sameSrc = $false }
        }
    }
    $rewriteOk = ($same -and $sameContent -and $sameSrc -and $r2.code -eq 0)

    # 被锁：另一个进程独占这个文件 → 明确报错（不清写、mtime 不变）
    $lock = [IO.File]::Open($BlobPath, 'Open', 'Read', 'None')
    $r3 = Run-Boot @($ProbeLua1)
    $lock.Close()
    $lockedOk = ($r3.code -eq 2 -and $r3.err -match 'handoff:' -and `
        [IO.File]::GetLastWriteTime($BlobPath) -eq $before)

    Check 'handoff.identical-no-rewrite' ($rewriteOk -and $lockedOk) `
        ("mtime_unchanged={0} content==blob_src={1} locked_exit={2}" -f $same, $sameSrc, $r3.code)
    }
}

# ============================================================================
# ④ no-stdout：引导器一行都不写；`--json` 那一行由 child 写
# ============================================================================
$JsonLua = Write-Lua (Join-Path $GateTmp 'json.lua') "print('hi')`nreturn 1`n"
$r = Run-Boot @('--json', $JsonLua)
$oneLine = ($r.out -replace "`r", '' -split "`n")
$oneLine = @($oneLine | Where-Object { $_ -ne '' })
$jsonOk = ($oneLine.Count -eq 1)
$obj = $null
if ($jsonOk) { try { $obj = $oneLine[0] | ConvertFrom-Json } catch { $jsonOk = $false } }
$noLauncher = ($r.out -notmatch 'launcher:' -and $r.err -notmatch 'handoff:')
Check 'handoff.no-stdout' ($jsonOk -and $oneLine.Count -eq 1 -and $null -ne $obj -and `
    ($obj.stdout -match 'hi') -and ($obj.exit -eq 0) -and $noLauncher) `
    "lines=$($oneLine.Count) json_stdout=[$($obj.stdout)] launcher_wrote_nothing=$noLauncher"

# ============================================================================
# ⑤ cmdline-passthrough：含引号的 argv[0]（工作目录带空格）+ 带空格/引号的参数 + 中文路径
# ============================================================================
$SpaceDir = Join-Path $GateTmp 'with space'
New-Item -ItemType Directory -Force -Path $SpaceDir | Out-Null
$ChineseLua = Write-Lua (Join-Path $SpaceDir '中文 测试.lua') $ProbeLua

# ❗命令行必须自己拼：PowerShell 的 `Start-Process` 不会替带空格的参数加引号（它只是把数组用
#   空格拼起来），所以「引号口径」只能由门禁按 `autodbg_split_cmdline_a/w` 的规则拼。
#   期望值也按同一套规则写：`\"` 是转义引号（这个 splitter 不把 `""` 当转义，所以不用它当判据）。
$ArgStr = '"' + $ChineseLua + '" plain "two words" "quo\"te" "back\slash"'
$r = Run-Boot $ArgStr
$gotArg1 = ($r.out -match '(?m)^arg1=plain\s*$')
$gotArg2 = ($r.out -match '(?m)^arg2=two words\s*$')
$gotArg3 = ($r.out -match '(?m)^arg3=quo"te\s*$')
$gotArg4 = ($r.out -match '(?m)^arg4=back\\slash\s*$')   # ❗双反斜杠：单写 `\slash` 里的 `\s` 会被当空白元字符
# ❗中文路径只能比字节：modern 是 UTF-8、legacy 的执行字符集是 GBK（PITFALLS D18/O1b），所以这里用
#   字节级包含判据，两条编码都认。
$Utf8Pat = (New-Object System.Text.UTF8Encoding($false)).GetBytes('chunk=' + $ChineseLua)
$GbkPat  = [Text.Encoding]::GetEncoding(936).GetBytes('chunk=' + $ChineseLua)
$chunkBytesOk = Contains-Bytes ([IO.File]::ReadAllBytes($r.outf)) $Utf8Pat
if (-not $chunkBytesOk) { $chunkBytesOk = Contains-Bytes ([IO.File]::ReadAllBytes($r.outf)) $GbkPat }
$childOk = ($r.out -match 'base=' -and $r.out -match "arch=$WantArch")
# ❗这里先算好 detail 再传：`Check` 是函数调用，后面接 `-f` 会被当参数（格式化不会执行）
$detail5 = (("argstr=[{0}] chunk_ok={1} args={2}/{3}/{4}/{5} want_arch={6}; code={7}; " +
            "argv[0] 本身从 Lua 里读不到（代理），这里直接判的是「门禁自己拼的引号 → child 逐字节还原」") -f `
            $ArgStr, $chunkBytesOk, $gotArg1, $gotArg2, $gotArg3, $gotArg4, $WantArch, $r.code)
Check 'handoff.cmdline-passthrough' ($gotArg1 -and $gotArg2 -and $gotArg3 -and $gotArg4 -and $chunkBytesOk -and `
    $r.code -eq 0 -and $childOk) $detail5

# ============================================================================
# ⑥ exit-code：0 / -1(0xFFFFFFFF) / 2 / Lua 3（口径不变，引导器只透传）
# ============================================================================
$ZeroLua = Write-Lua (Join-Path $GateTmp 'zero.lua') "print('ok')`n"
$RunLua  = Write-Lua (Join-Path $GateTmp 'runerr.lua') "error('boom')`n"
$SynLua  = Write-Lua (Join-Path $GateTmp 'synerr.lua') "local x =`n"
$c0 = (Run-Boot @($ZeroLua)).code
$c1 = (Run-Boot @((Join-Path $GateTmp 'nope.lua'))).code
$c2 = (Run-Boot @($RunLua)).code
$c3 = (Run-Boot @($SynLua)).code
Check 'handoff.exit-code' ($c0 -eq 0 -and $c1 -eq -1 -and $c2 -eq 2 -and $c3 -eq 3) `
    ("0={0} -1(0xFFFFFFFF)={1} 2={2} 3={3}; -1 按 Int32 读（0xFFFFFFFF = -1）" -f $c0, $c1, $c2, $c3)

# ============================================================================
# ⑦ non-exe-name：CreateProcess 按 PE 头判、不看扩展名（`.bin` 实测）
# ============================================================================
$Machine = $(if (Test-Path $BlobPath) { Get-PEMachine $BlobPath } else { 0 })
$r = Run-Boot @($ProbeLua1)
$binNameOk = ($blobName -like '*.bin')
$ranFromBin = ($r.out -match "arch=$WantArch" -and $r.code -eq 0)
Check 'handoff.non-exe-name' ((-not $ExpectHandoff) -or ($binNameOk -and $Machine -eq 0x8664 -and $ranFromBin)) `
    ("落盘名字是 .bin={0}、文件 PE 头 Machine=0x{1:X}（x64）、跑起来后报 arch={2} → 不看扩展名；判据是「跑起来了 + 跑的是 x64 那份」，读不到 CreateProcess 内部" -f `
        $binNameOk, $Machine, $WantArch)

# ============================================================================
# ⑧ script-relative-base：把产物复制到临时目录跑 + 该目录里放 fixture + 脚本用相对路径读写
#    直接判据：CWD 故意选**另一个**目录，所以 `open_file('fixture.txt')` 只能靠 `common/base_dir`
#    （env `AUTODBG_BASE_DIR` > 主 exe 目录）才读得到；相对写出的文件必须落在原始目录而不是 CWD。
#    ❗用 `open_file`（走 base_dir），不用 `io.open`（仍按 CWD）——用 io.open 就只剩「CWD 恰好等价」的代理。
#    门禁不往 .out 写任何东西。
# ============================================================================
$CopyDir = Join-Path $GateTmp 'copy_dir'
$OtherCwd = Join-Path $GateTmp 'other_cwd'
New-Item -ItemType Directory -Force -Path $CopyDir, $OtherCwd | Out-Null
$CopyExeSaved = $MainExe
Copy-Item $MainExe (Join-Path $CopyDir 'adbg_copy.exe') -Force
$CopyLua = Write-Lua (Join-Path $CopyDir 'rel.lua') @'
local f, err = open_file('fixture.txt', 'rb')
print('read=' .. (f and (f:read('*a') or 'MISS') or ('ERR ' .. tostring(err))))
if f then
    f:close()
end
local g, gerr = open_file('written.txt', 'wb')
print('write=' .. (g and 'ok' or ('ERR ' .. tostring(gerr))))
if g then
    g:write('W')
    g:close()
end
print('base=' .. tostring(os.getenv('AUTODBG_BASE_DIR')))
'@
[IO.File]::WriteAllText((Join-Path $CopyDir 'fixture.txt'), 'FIXTURE', (New-Object System.Text.UTF8Encoding($false)))

# 用复制件跑，但 CWD 是另一个目录：解析只能靠 base_dir（handoff 时 = env；不 handoff 时 = 主 exe 目录）
$MainExe = Join-Path $CopyDir 'adbg_copy.exe'
$r = Run-Boot @($CopyLua) $OtherCwd
$MainExe = $CopyExeSaved
$envOk = ($r.out -match [regex]::Escape($CopyDir))
$readOk = ($r.out -match 'read=FIXTURE')
$landed = (Test-Path (Join-Path $CopyDir 'written.txt')) -and (-not (Test-Path (Join-Path $OtherCwd 'written.txt')))
Check 'handoff.script-relative-base' ($r.code -eq 0 -and $readOk -and $landed -and ((-not $ExpectHandoff) -or $envOk)) `
    ("env_ok={0}; 相对读={1}; 相对写落在原始目录而不是 CWD={2}; code={3} out=[$($r.out.Trim())]（CWD={5}）" -f `
        $envOk, $readOk, $landed, $r.code, $r.out.Trim(), $OtherCwd)

# ============================================================================
# ⑨ no-dialog：child 收到的脚本路径就是主 exe 给的那一份（直接判据）+ 主 exe 拿不到脚本时
#    child 从未被创建（代理：真·非交互会话构造不出来，这里判「挂住等对话框期间没有以主 exe 为父的进程」）
# ============================================================================
$DialogLua = Write-Lua (Join-Path $GateTmp 'dialog.lua') "print('chunk=' .. debug.getinfo(1, 'S').source)`n"

# (a) 直接：命令行给了脚本路径 → child 的 chunkname 必须逐字节等于主 exe 传入的那一份
$r = Run-Boot @($DialogLua)
$directOk = ($r.out -match [regex]::Escape($DialogLua))

# (b) 代理：无脚本 + 非交互。这里用「主 exe 挂住等对话框、期间没有任何子进程」判 —— 对话框归主 exe，
#     child 绝不会被 CreateProcess 起来。跑完强杀，避免把门禁挂在那儿。
$p = Start-Process -FilePath $MainExe -ArgumentList (@('--quiet') + $VariantArgs) -WorkingDirectory $GateTmp `
    -RedirectStandardOutput (Join-Path $GateTmp 'd_out.txt') `
    -RedirectStandardError (Join-Path $GateTmp 'd_err.txt') -PassThru -NoNewWindow
Start-Sleep -Seconds 3
$kids = Get-ChildPids $p.Id
$noChild = ($kids.Count -eq 0)
$dErr = if (Test-Path (Join-Path $GateTmp 'd_err.txt')) { Get-Content (Join-Path $GateTmp 'd_err.txt') -Raw } else { '' }
if ($null -eq $dErr) { $dErr = '' }
$reportedByMain = [bool]($dErr -match 'no script')   # 非交互时才有；真控制台会话里会挂住 → 这一半是代理
try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch { }
Start-Sleep -Milliseconds 300
Check 'handoff.no-dialog' ($directOk -and $noChild) `
    ("chunkname_direct={0}; 无脚本时 child 未被创建（代理判据：无桌面会话无法构造，这里判「等对话框期间无子进程」，pid={1} kids={2}；主 exe 自己报错={3}）" -f `
        $directOk, $p.Id, $kids.Count, $reportedByMain)

# ============================================================================
# ⑩ job：kill-on-close（WMI 查父子进程）与 nested 干净降级
# ============================================================================
$BlockLua = Write-Lua (Join-Path $GateTmp 'block.lua') @'
local t = os.time()
while os.time() - t < 6 do end
print('done')
'@

$p = Start-Process -FilePath $MainExe -ArgumentList (@($BlockLua) + @('--quiet') + $VariantArgs) `
    -WorkingDirectory $GateTmp `
    -RedirectStandardOutput (Join-Path $GateTmp 'j_out.txt') `
    -RedirectStandardError (Join-Path $GateTmp 'j_err.txt') -PassThru -NoNewWindow
Start-Sleep -Seconds 3
$kids = Get-ChildPids $p.Id
$mainAlive = ($null -ne (Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$($p.Id)"))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
$left = @()
foreach ($k in $kids)
{
    if ($null -ne (Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$($k.ProcessId)")) { $left += $k.ProcessId }
}
$expectChild = ($ExpectHandoff -and $kids.Count -ge 1)
Check 'handoff.job-kill-on-close' ($mainAlive -and (-not $expectChild -or $left.Count -eq 0)) `
    ("kids={0} left={1}（WMI Win32_Process.ParentProcessId；job 三条是 dynapi + best-effort）" -f $kids.Count, ($left -join ','))

# nested：已在 job 里 → 干净降级（不报错、不退 2）。job 本身不设 limit（已定）。
# ❗不要 `-UsingNamespace 'System.Runtime.InteropServices'`：Add-Type 自己已经带这个 using，再写一遍会
#   报 CS0105（同一个命名空间重复 using）。
Add-Type -Namespace 'AutoDbgGate' -Name 'JobGate' -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern IntPtr CreateJobObjectW(IntPtr sa, string name);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(IntPtr h);
'@
$job = [AutoDbgGate.JobGate]::CreateJobObjectW([IntPtr]::Zero, 'adbg_gate_job')
$p = Start-Process -FilePath $MainExe -ArgumentList (@($ZeroLua) + @('--quiet') + $VariantArgs) `
    -WorkingDirectory $GateTmp `
    -RedirectStandardOutput (Join-Path $GateTmp 'n_out.txt') `
    -RedirectStandardError (Join-Path $GateTmp 'n_err.txt') -PassThru -NoNewWindow
$assigned = $false
if ($job -ne [IntPtr]::Zero)
{
    try { $assigned = [AutoDbgGate.JobGate]::AssignProcessToJobObject($job, $p.Handle) } catch { $assigned = $false }
    [AutoDbgGate.JobGate]::CloseHandle($job) | Out-Null
}
$null = $p.WaitForExit(30000)
$nErr = Get-Content (Join-Path $GateTmp 'n_err.txt') -Raw
if ($null -eq $nErr) { $nErr = '' }
Check 'handoff.job-nested-fails-clean' ($p.ExitCode -eq 0 -and $nErr -notmatch 'handoff:' -and `
    $nErr -notmatch 'job') `
    ("gate_job_assign={0} code={1} err=[$($nErr.Trim())]（Win8+ 允许嵌套 job：assign 成功也算「干净」）" -f $assigned, $p.ExitCode)

# ============================================================================
# ⑪ version.parity.main-vs-exec：同一 build、同一脚本，主 exe 报的 == child 报的
#     `arch` 按 host 算（主 exe 是 x86、64 位 host 上要报 x64）；`proto` 不在版本行里 → 对的是
#     版本行实际报出的字段（version / source / dirty / abi / arch）。
# ============================================================================
$ParityLua = Write-Lua (Join-Path $GateTmp 'parity.lua') @'
print('arch=' .. dbg.caps().arch)
'@
$rMain = Run-Boot @('--version')
$rChild = Run-Boot @($ParityLua)
$vExec = $null
if (Test-Path $BlobSrc)
{
    $savedExe = $MainExe
    $MainExe = $BlobSrc
    $vExec = Parse-Version ((Run-Boot @('--version')).out.Trim())
    $MainExe = $savedExe
}
$mainV = Parse-Version ($rMain.out.Trim())
$archByHost = if ($HostIs64) { 'x64' } else { 'x86' }
$parityOk = ($null -ne $mainV -and $null -ne $vExec -and $mainV.version -eq $vExec.version -and `
    $mainV.source -eq $vExec.source -and $mainV.dirty -eq $vExec.dirty -and $mainV.abi -eq $vExec.abi -and `
    $mainV.arch -eq $archByHost -and ($rChild.out -match "arch=$WantArch"))
Check 'version.parity.main-vs-exec' $parityOk `
    "main=[$($rMain.out.Trim())] exec=[$($vExec.version)] host_arch=$archByHost（proto 不在版本行里 → 只对版本行实际报出的字段）"

# ============================================================================
# ⑫ no-handoff-switch（拍板 A 的排障 / 门禁开关）：本来会 handoff 的组合（64 位 host + 有 exec 条目）
#    显式 `--no-handoff` ⇒ 主 exe 自己跑 x86 半份 ⇒ `dbg.caps().arch` = x86、一个字节都不写、env 不设。
#    ❗直接判据是「开关把执行留在 x86 半份」；纯函数那一侧的规则 ⑤ 由 selftest 的
#      `handoff.decision.no-handoff-switch` 断。`--version` 那一行的 arch 仍然按 host 算（D63），不认这个开关。
#    这开关的存在理由：(c) 的四件做完之前，x64 半份调不了 32 位目标（PITFALLS M12），所以 `e2e_dbg` /
#    `e2e_tcp` / `e2e_repl` 在 64 位 host 上跑 x86 产物时必须带它。
# ============================================================================
$NoTmp = Join-Path $GateTmp 'no_handoff_tmp'
New-Item -ItemType Directory -Force -Path $NoTmp | Out-Null
$SwitchLua = Write-Lua (Join-Path $GateTmp 'switch.lua') @'
print('base=' .. tostring(os.getenv('AUTODBG_BASE_DIR')))
print('arch=' .. dbg.caps().arch)
'@
$rs = Run-Boot (@($SwitchLua) + @('--no-handoff')) $GateTmp $NoTmp
$switchOk = ($rs.code -eq 0 -and ($rs.out -match 'arch=x86') -and ($rs.out -match 'base=nil') -and `
    -not (Test-Path (Join-Path $NoTmp $blobName)))
Check 'handoff.no-handoff-switch' ($switchOk -and $HostIs64) `
    ("arch=x86（留在 x86 半份）base=nil 不落盘={0}; code={1} out=[$($rs.out.Trim())]" -f `
        (-not (Test-Path (Join-Path $NoTmp $blobName))), $rs.code, $rs.out.Trim())

# ============================================================================
# 汇总
# ============================================================================
Write-Output ("boot e2e ({0}{1}, host={2}, handoff_expected={3}): {4} passed, {5} failed" -f `
    $Render, $(if ($Variant) { ", $Variant" } else { '' }), $(if ($HostIs64) { 'x64' } else { 'x86' }), `
    $ExpectHandoff, $script:pass, $script:fail)
if ($script:fail -ne 0) { exit 1 }
exit 0
