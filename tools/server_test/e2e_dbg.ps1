# =============================================================================
# tools/server_test/e2e_dbg.ps1 —— `dbg` 表的本地门禁（plan_t7.md §11.7 切片 A）
#
# 来源：新写（切片 A：D51 收尾动词 / D53 help 与 caps）。T7.3 的 `pump.*` 断言在
#   `e2e_tcp.ps1` 里（跑 server 口径）；本文件跑**本地模式**（`AutoDbg.exe <script>`），
#   因为 help / caps / detach / kill 与传输无关，起一个 server 只是浪费。
#
# 用法：pwsh -File tools/server_test/e2e_dbg.ps1 [-Render 64|86] [-Variant ''|modern|legacy]
# 退出码：0 = 全过；1 = 有失败；2 = 前置缺失（产物不存在 / x64+legacy 组合）
#
# 覆盖（切片 A + C1a + C1b）：
#   caps.*            这台机器/build 的事实：arch / variant / flavor / win9x / attach / detach / hits_cap
#   detach.capability `dbg.detach` 存在且 `dbg.close` **已删**（D51：无隐式回退，能力缺席就是 nil）
#   help.*            help() / help('dbg') / help('dbg.pump') / 未知主题 / 管道下纯文本（无 ESC）
#   kill.*            d:kill 真的终止目标；之后状态复位（pump -> nil + "no session"）
#   session.*         （切片 C1a / D54 / D55 / D57）
#     identity          dbg.last() == dbg.session(root_pid)（**同一个 userdata**）
#     pump-cache-first  d.stop 非空时 d:pump(0) 立即返回同一张表；code == 0 的事件也不清 d.stop
#     event-table       事件表的八个字段齐全，type 是 DEBUG_EVENT 码（CREATE_PROCESS = 3）
#   session.*         （切片 C1b：数据面接上对象，plan_t7.md §2.2 第 12 条 / §11.4）
#     read-write        停在 fixture 的 int3 上读 6 字节 = `B8 44 42 47 50 CC`、小端 u32、写回原字节
#     read-partial      256 MB 请求在第一个不可读页停下（0 < 真实长度 < 请求长度）；完全读不到 → nil
#     context           d:context() 的 `Cip` = 停住那条 int3 的**下一条**；d:context(nil, {Cax=…})
#                       部分更新返回 true 且读回生效（键名与 `GetContext` 逐字一致：x86 是 `Cax…Cip`）
#     mods-threads      d:mods() 有 name 含 x86_loop.exe 且 base > 0；d:threads() 含事件的 tid
#   C2a（多会话 + 进程级事件路由，plan_t7.md §10 / §11.9.1 / D57）—— 起**两个**同名 fixture 实例：
#     session.multi-two-stopped   两个会话各自 arm 到自己的 site → 循环 dbg.pump(300) 直到两边都 paused
#                                 （有界轮数 + stopped() 双判）；两者 stop.rva == site、sessions() == 2
#     pump.dispatch-attribution   dbg.pump 的事件表带 `session`；只泵 b 时 a 的事件照样被**分派**进 a
#                                 （a.stop 非空 = D57 的「分派不能推迟」）；a:pump(0) 返回 a.stop 而不是 b 的
#     route.same-name-two-processes 两个同名实例是两个进程（root_pid 不同）：在 a 里写一个字节，
#                                 b 读同一地址仍是原值 ⇒ 内存走的确实是**各自的**进程句柄
#     run.refused-with-session    存在 dbg 会话时 pcall(Run, {...}) 的第二个返回值 = 新的固定文案
#   T7.6（多会话脚本糖 `lualib/dbglib.lua`，plan_t7.md §11.9.2 / D58）—— 同样是两个同名实例：
#     sugar.wait-all              rendezvous：两边各自 arm 到 site 后 `dbglib.wait_all` 返回 true，
#                                 两边同时 `paused()` 且 `stop.rva == site` ⇒ 才读 context；再 detach/kill
#     sugar.run-all               batch：`dbglib.run_all{{imagepath=EXE},{imagepath=EXE}}` 返回两张结果表，
#                                 每张 ok == true / exit == 0x4000001F / hits >= 1，返回后 sessions() == 0
#                                 ❗exit 是**实测值**：不武装断点时 fixture 的第二个 int3 走 NOT_HANDLED，
#                                 进程被系统以 WOW64 断点状态终止（不是 fixture 契约的 42）；见下面的说明
# ❗session.* / kill.* / C2a 的 4 条 / sugar.* 的 2 条都需要 x86 的 fixture ⇒ x64 上这几段整段 SKIP（8 条）。
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
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) 'autodbg_dbg_e2e'
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

# 本地跑一个脚本，拿 stdout+stderr（两种变体都当文本读：断言只用 ASCII 子串）
function Run-Local([string]$script, [int]$TimeoutMs = 30000)
{
    $out = Join-Path $TmpDir 'out.txt'
    $err = Join-Path $TmpDir 'err.txt'
    Remove-Item $out, $err -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $Exe -ArgumentList (@($VariantArgs) + @($script)) `
        -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -NoNewWindow
    $null = $p.WaitForExit($TimeoutMs)
    $o = if (Test-Path $out) { Get-Content $out -Raw } else { '' }
    $e = if (Test-Path $err) { Get-Content $err -Raw } else { '' }
    return [pscustomobject]@{ code = $(if ($p.HasExited) { $p.ExitCode } else { -999 }); out = $o; err = $e }
}
function Write-Lua([string]$name, [string]$text)
{
    $path = Join-Path $TmpDir $name
    [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
    return $path
}

# fixture 里 `mov eax, 0x50474244` + `int 3` 的 6 字节模式定位（与 e2e_tcp.ps1 的扫描逻辑同一份口径：
# 匹配整条指令编码 `B8 44 42 47 50 CC`，命中的 RVA + 5 = 那条 int3 的地址）。
# ❗不在 PowerShell 里算 RVA → EA：运行时 base 由 `d:mods()` 给（plan_t7.md §9.9：`GetModBase` 返回
# 的是 LuaVariant，做算术会被截断）。
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

# ---------------------------------------------------------------- ① caps / 能力
$s_caps = Write-Lua 'caps.lua' @'
local c = dbg.caps()
print(string.format('arch=%s variant=%s flavor=%s win9x=%s attach=%s detach=%s hits_cap=%d',
    c.arch, c.variant, c.flavor, tostring(c.win9x), tostring(c.attach), tostring(c.detach), c.hits_cap))
-- 「caps 报的能力」与「表成员」必须一致（否则就有第二真相源）
print('detach_member=' .. tostring(dbg.detach ~= nil) .. ' close_member=' .. tostring(dbg.close))
'@
$r = Run-Local $s_caps
$arch = if ($Render -eq '64') { 'x64' } else { 'x86' }
$variant = if ($Variant) { $Variant } else { 'modern' }
Check 'caps.arch-variant' ($r.out -match "arch=$arch variant=$variant flavor=lua") "expected arch=$arch variant=$variant; out=[$($r.out)]"
Check 'caps.detach' ($r.out -match 'win9x=false attach=true detach=true hits_cap=64') "out=[$($r.out)]"
Check 'detach.capability' ($r.out -match 'detach_member=true close_member=nil') `
    "dbg.detach 必须存在、dbg.close 必须已删（D51）；out=[$($r.out)]"

# ---------------------------------------------------------------- ② help
$s_help = Write-Lua 'help.lua' @'
print('idx=' .. tostring(help()))
print('group=' .. tostring(help('dbg')))
print('topic=' .. tostring(help('dbg.pump')))
local ok, err = help('no-such-topic')
print('unknown=' .. tostring(ok) .. ' ' .. tostring(err))
print('dbg_help=' .. tostring(dbg.help()))
'@
$r = Run-Local $s_help
# ❗只断言 ASCII 部分：legacy 变体的中文是 GBK 字节（e2e_tcp.ps1 的既有口径）
Check 'help.index' ($r.out -match 'idx=true' -and $r.out -match "help\('dbg\.pump'\)" -and `
    $r.out -match '(?m)^\s+dbg\s{2,}\S') "out=[$($r.out)]"
Check 'help.group' ($r.out -match 'group=true' -and $r.out -match 'dbg\.detach\s+') "out=[$($r.out)]"
Check 'help.topic' ($r.out -match 'topic=true' -and $r.out -match 'autocont') "out=[$($r.out)]"
Check 'help.unknown-topic' ($r.out -match "unknown=nil no help for 'no-such-topic' \(try help\(\)\)") "out=[$($r.out)]"
# 管道 ⇒ 不是控制台 ⇒ 必须纯文本（切片 B 上色后这条仍然要过）
$hasEsc = ($r.out + $r.err).Contains([char]27)
Check 'help.plain-when-piped' ((-not $hasEsc) -and ($r.out -match 'dbg_help=true')) `
    "has_esc=$hasEsc out=[$($r.out)]"

# ---------------------------------------------------------------- ③ kill
$fx = Join-Path $Root '.out/fixtures/x86_loop.exe'
if ($Render -ne '86' -or -not (Test-Path $fx))
{
    Write-Output "SKIP: kill.* / session.* 需要 x86 的 fixture（.out/fixtures/x86_loop.exe）"
}
else
{
    $fxLua = $fx.Replace('\', '/')
    $s_kill = Write-Lua 'kill.lua' @"
local d, err = dbg.create{ imagepath = '$fxLua' }
print('create=' .. tostring(d ~= nil) .. ' err=' .. tostring(err))
if not d then return end
local tpid
for i = 1, 12 do
    local ev = d:pump(2000)
    if ev == nil then break end
    local h = d:hits()
    if #h >= 1 then tpid = h[1].pid break end
    d:cont(0x00010002)   -- DBG_CONTINUE：fixture 的 int3 是写死的，不显式放行就会终止
end
print('pid=' .. tostring(tpid))
print('kill=' .. tostring(d:kill()))
print('after=' .. tostring(select(2, dbg.pump(10, false))))
"@
    $r = Run-Local $s_kill
    $m = [regex]::Match($r.out, 'pid=(\d+)')
    $tpid = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
    Start-Sleep -Milliseconds 400
    $alive = $false
    if ($tpid -gt 0) { $alive = $null -ne (Get-Process -Id $tpid -ErrorAction SilentlyContinue) }
    Check 'kill.terminates' ($r.out -match 'create=true' -and $r.out -match 'kill=true' -and $tpid -gt 0 -and -not $alive) `
        "pid=$tpid alive=$alive out=[$($r.out)]"
    Check 'kill.state-reset' ($r.out -match 'after=dbg: no session \(call dbg\.create or dbg\.attach first\)') "out=[$($r.out)]"
    if ($alive) { Stop-Process -Id $tpid -Force -ErrorAction SilentlyContinue }

    # ------------------------------------------------------------ ④ session 对象（C1a）
    $s_sess = Write-Lua 'session.lua' @"
local EXE = '$fxLua'
local d, err = dbg.create{ imagepath = EXE }
print('obj=' .. tostring(d ~= nil) .. ' type=' .. type(d) .. ' err=' .. tostring(err))
if not d then return end
local pid = d.root_pid
print('statics=' .. tostring(pid > 0) .. ' kind=' .. tostring(d.kind) .. ' state=' .. tostring(d.state))
local ev = d:pump(3000)
local fields = ev ~= nil and ev.type ~= nil and ev.pid ~= nil and ev.tid ~= nil and
    ev.ea ~= nil and ev.modname ~= nil and ev.rva ~= nil and ev.code ~= nil and ev.cont_code ~= nil
print('ev_type=' .. tostring(ev and ev.type) .. ' ev_fields=' .. tostring(fields) ..
      ' ev_same=' .. tostring(ev ~= nil and ev == d.stop))
print('ev_code=' .. tostring(ev and ev.code) .. ' ev_cont=' .. tostring(ev and ev.cont_code))
local ev2 = d:pump(0)
print('cache_first=' .. tostring(ev2 == ev) .. ' still=' .. tostring(d.stop ~= nil) ..
      ' paused=' .. tostring(d:paused()))
print('identity=' .. tostring(dbg.last() == dbg.session(pid)) ..
      ' last_is_d=' .. tostring(dbg.last() == d))
local ss = dbg.sessions()
print('sessions=' .. #ss .. ' kind=' .. tostring(ss[1] and ss[1].kind) ..
      ' bound=' .. tostring(ss[1] and ss[1].lua_bound))
print('cont=' .. tostring(d:cont()) .. ' cleared=' .. tostring(d.stop == nil))
print('kill=' .. tostring(d:kill()))
"@
    $r = Run-Local $s_sess
    Check 'session.identity' ($r.out -match 'obj=true type=userdata' -and `
        $r.out -match 'identity=true last_is_d=true' -and $r.out -match 'cont=true cleared=true') `
        "dbg.last() 必须是 dbg.session(root_pid) 的**同一个** userdata；out=[$($r.out)]"
    Check 'session.pump-cache-first' ($r.out -match 'cache_first=true still=true paused=true' -and `
        $r.out -match 'ev_code=0' -and $r.out -match 'ev_same=true') `
        "d.stop 非空 ⇒ d:pump(0) 立即返回同一张表；code == 0 的事件也不清 d.stop；out=[$($r.out)]"
    Check 'session.event-table' ($r.out -match 'ev_type=3' -and $r.out -match 'ev_fields=true' -and `
        $r.out -match 'ev_cont=65538' -and $r.out -match 'sessions=1 kind=create bound=true') `
        "事件表八个字段齐全、type = CREATE_PROCESS_DEBUG_EVENT(3)；out=[$($r.out)]"

    # ------------------------------------------------------------ ⑤ 数据面（C1b）
    $site = Find-FixtureSite $fx
    $s_data = Write-Lua 'data.lua' @"
local EXE = '$fxLua'
local SITE = $($site.rva) + 5
local d, err = dbg.create{ imagepath = EXE }
print('data_obj=' .. tostring(d ~= nil) .. ' err=' .. tostring(err))
if not d then return end
local ev = d:pump(3000)
local base
for _, m in ipairs(d:mods()) do
    if m.name and string.lower(m.name):find('x86_loop.exe', 1, true) then base = m.base end
end
if ev then d:cont(0x00010002) end
local hit = false
for i = 1, 24 do
    ev = d:pump(3000)
    if ev == nil then break end
    if base and ev.ea == base + SITE then hit = true break end
    d:cont(0x00010002)
end
print('hit=' .. tostring(hit) .. ' base_pos=' .. tostring(base ~= nil and base > 0))
if not hit then print('kill=' .. tostring(d:kill())) return end

-- session.read-write
local pat = d:read(base + SITE - 5, 6)
local hex = {}
for i = 1, #pat do hex[#hex + 1] = string.format('%02X', pat:byte(i)) end
print('read6=' .. table.concat(hex, ' ') .. ' len=' .. #pat)
print('u32=0x' .. string.format('%08X', d:read_u32(base + SITE - 4)))
print('write=' .. tostring(d:write(base + SITE - 5, pat)) .. ' same=' .. tostring(d:read(base + SITE - 5, 6) == pat))

-- session.read-partial
local s = d:read(base, 0x10000000)
print('partial=' .. tostring(s ~= nil) .. ' len=' .. tostring(s and #s) ..
      ' short=' .. tostring(s ~= nil and #s > 0 and #s < 0x10000000))
local bad, berr = d:read(0x7FFF0000, 16)
print('bad=' .. tostring(bad) .. ' err=' .. tostring(berr))

-- session.context
local ctx = d:context()
print('cip=0x' .. string.format('%X', ctx.Cip) .. ' want=0x' .. string.format('%X', base + SITE + 1))
print('set=' .. tostring(d:context(nil, { Cax = 0x1234 })) .. ' cax=0x' .. string.format('%X', d:context().Cax))
d:context(nil, { Cax = ctx.Cax })
print('restored=' .. tostring(d:context().Cax == ctx.Cax))

-- session.mods-threads
local modok = false
for _, m in ipairs(d:mods()) do
    if m.name and string.lower(m.name):find('x86_loop.exe', 1, true) and m.base > 0 then modok = true end
end
local tidok = false
for _, t in ipairs(d:threads()) do
    if t.tid == ev.tid then tidok = true end
end
print('mod_ok=' .. tostring(modok) .. ' nthreads=' .. #d:threads() .. ' tid_ok=' .. tostring(tidok))
print('kill=' .. tostring(d:kill()))
"@
    $r = Run-Local $s_data
    Check 'session.read-write' ($r.out -match 'data_obj=true' -and $r.out -match 'hit=true base_pos=true' -and `
        $r.out -match 'read6=B8 44 42 47 50 CC len=6' -and $r.out -match 'u32=0x50474244' -and `
        $r.out -match 'write=true same=true') `
        "停在 fixture 的 int3 上读/写目标内存；out=[$($r.out)]"
    Check 'session.read-partial' ($r.out -match 'partial=true' -and $r.out -match ' short=true' -and `
        $r.out -match 'bad=nil err=d:read: unreadable address') `
        "256 MB 请求必须在第一个不可读页停下（0 < 真实长度 < 请求长度），而不是整体失败；out=[$($r.out)]"
    $m = [regex]::Match($r.out, 'cip=(0x[0-9A-Fa-f]+) want=(0x[0-9A-Fa-f]+)')
    $cipOk = $false
    if ($m.Success)
    {
        # `Cip` 必须等于「那条 int3 的下一条」——裸 int3 被系统停在 CC 之后（plan_t7.md §2.1 实测口径）
        $cipOk = ([Convert]::ToInt64($m.Groups[1].Value.Substring(2), 16) -eq
                  [Convert]::ToInt64($m.Groups[2].Value.Substring(2), 16))
    }
    Check 'session.context' ($cipOk -and $r.out -match 'set=true' -and $r.out -match 'cax=0x1234' -and `
        $r.out -match 'restored=true') `
        "d:context() 的 Cip = 停住那条 int3 的下一条；d:context(nil, {Cax=…}) 部分更新生效；out=[$($r.out)]"
    Check 'session.mods-threads' ($r.out -match 'mod_ok=true' -and $r.out -match 'nthreads=[1-9]\d* tid_ok=true') `
        "d:mods() 有 x86_loop.exe 且 base > 0；d:threads() 含事件 tid；out=[$($r.out)]"

    # ------------------------------------------------------------ ⑥ C2a：多会话 + 进程级路由
    # 两个**同名**实例（同一份 fixture）：这是「每会话一个引擎会话」与「按 pid 归属分派」的最小现场。
    # `arm` 走**延迟武装**（D57）：create 之后模块还没加载也能登记，CREATE_PROCESS 时补 0xCC。
    $s_multi = Write-Lua 'multi.lua' @"
local EXE = '$fxLua'
local SITE = $($site.rva) + 5
local a, ea = dbg.create{ imagepath = EXE }
local b, eb = dbg.create{ imagepath = EXE }
print('multi_obj=' .. tostring(a ~= nil) .. '/' .. tostring(b ~= nil) .. ' err=' .. tostring(ea) .. '|' .. tostring(eb))
if not a or not b then return end
print('pids_differ=' .. tostring(a.root_pid ~= b.root_pid) .. ' n=' .. #dbg.sessions())
print('arm=' .. tostring(a:arm('x86_loop.exe', SITE, function() end)) .. '/' ..
                tostring(b:arm('x86_loop.exe', SITE, function() end)))
-- 有界超时 + stopped() 双判：两边都停到自己的 site 才退出
local rounds = 0
while not (a:paused() and b:paused()) do
    rounds = rounds + 1
    if rounds > 200 then break end
    if a:stopped() or b:stopped() then break end
    local ev = dbg.pump(300, false)
    if ev ~= nil and ev.rva ~= SITE then ev.session:cont() end
end
print('both=' .. tostring(a:paused() and b:paused()) .. ' rounds=' .. tostring(rounds < 200))
print('ra=' .. tostring(a.stop and a.stop.rva) .. ' rb=' .. tostring(b.stop and b.stop.rva) ..
      ' n2=' .. #dbg.sessions())
-- 同名两个进程：各自 mods()/read 都要走自己的句柄
local ab, bb
for _, m in ipairs(a:mods()) do if m.name and string.lower(m.name):find('x86_loop.exe', 1, true) then ab = m.base end end
for _, m in ipairs(b:mods()) do if m.name and string.lower(m.name):find('x86_loop.exe', 1, true) then bb = m.base end end
print('base_pos=' .. tostring(ab ~= nil and bb ~= nil and ab > 0 and bb > 0))
print('cc=' .. tostring(a:read_u8(ab + SITE) == 0xCC) .. '/' .. tostring(b:read_u8(bb + SITE) == 0xCC))
-- 在 a 的进程里改一个字节（SITE-4 = 那条 mov 的立即数首字节，改了不影响控制流）：
-- b 读同一个地址必须还是原值 —— 这是「同名两实例 = 两个进程、各走各的句柄」的硬证据。
print('wa=' .. tostring(a:write(ab + SITE - 4, string.char(0xAA))) ..
      ' ra=' .. tostring(a:read_u8(ab + SITE - 4) == 0xAA) ..
      ' rb=' .. tostring(b:read_u8(bb + SITE - 4) == 0x44))
print('runwhile=' .. tostring(select(2, pcall(Run, { imagepath = EXE }))))
print('detach=' .. tostring(a:detach()) .. ' kill=' .. tostring(b:kill()) .. ' left=' .. #dbg.sessions())
"@
    $r = Run-Local $s_multi 90000
    Check 'session.multi-two-stopped' ($r.out -match 'multi_obj=true/true' -and $r.out -match 'pids_differ=true n=2' -and `
        $r.out -match 'arm=true/true' -and $r.out -match 'both=true rounds=true' -and `
        $r.out -match "ra=$($site.site) rb=$($site.site) n2=2" -and `
        $r.out -match 'detach=true kill=true left=0') `
        "两个会话各停在自己的 site（$($siteHex)）、sessions() 从 2 到 0；out=[$($r.out)]"
    Check 'route.same-name-two-processes' ($r.out -match 'base_pos=true' -and $r.out -match 'cc=true/true' -and `
        $r.out -match 'wa=true ra=true rb=true') `
        "同名两实例：各自 mods() 有 base>0、各读自己的 site 是 CC；在 a 里写 0xAA 后 b 读同一地址仍是 0x44（各走自己的进程句柄）；out=[$($r.out)]"
    Check 'run.refused-with-session' ("$($r.out)".Contains('runwhile=cannot run while a dbg session exists (detach/kill it first)')) `
        "存在 dbg 会话时 Run 必须拒绝（新固定文案）；out=[$($r.out)]"

    # `pump.dispatch-attribution`：只泵 b 的时候，a 的事件照样被分派进 a（D57 的不变量 1），
    # 而 `a:pump(0)`（a 停着）返回的是 a 自己的 d.stop，绝不是 b 的事件。
    $s_attr = Write-Lua 'attribution.lua' @"
local EXE = '$fxLua'
local a = dbg.create{ imagepath = EXE }
local b = dbg.create{ imagepath = EXE }
if not a or not b then print('attr_create=false') return end
local evb = b:pump(3000)                      -- 事件流是**进程级**的：a 的事件也会被读出来
print('own=' .. tostring(evb ~= nil and evb.session == b) ..
      ' a_pending=' .. tostring(a.stop ~= nil) ..
      ' a_session=' .. tostring(a.stop ~= nil and a.stop.session == a))
print('cached=' .. tostring(a:pump(0) == a.stop) .. ' distinct=' .. tostring(a.stop ~= b.stop))
print('kill=' .. tostring(a:kill()) .. '/' .. tostring(b:kill()) .. ' left=' .. #dbg.sessions())
"@
    $r = Run-Local $s_attr
    Check 'pump.dispatch-attribution' ($r.out -match 'own=true a_pending=true a_session=true' -and `
        $r.out -match 'cached=true distinct=true' -and $r.out -match 'kill=true/true left=0') `
        "dbg.pump 的事件表带 session；只泵 b 时 a 的事件也被分派进 a（a.stop 非空）；a:pump(0) 返回 a.stop 而不是 b 的；out=[$($r.out)]"

    # ------------------------------------------------------------ ⑦ T7.6：dbglib 的两个模型（D58）
    # `dbglib` 是 lualib/dbglib.lua 烘烤出来的全局（按文件名设全局，AGENTS.md §2）；
    # 本段交替验证它的两个函数喂的是**真的** session 对象，而不是另一套平行 API。
    $s_sugar_wait = Write-Lua 'sugar_wait.lua' @"
local EXE = '$fxLua'
local SITE = $($site.site)
print('sugar_lib=' .. tostring(type(dbglib)) .. '/' .. tostring(dbglib and dbglib._version))
local a = dbg.create{ imagepath = EXE }
local b = dbg.create{ imagepath = EXE }
if not a or not b then print('sugar_two=false') return end
print('sugar_two=true pids_differ=' .. tostring(a.root_pid ~= b.root_pid))
print('sugar_arm=' .. tostring(a:arm('x86_loop.exe', SITE, function() end)) .. '/' ..
                tostring(b:arm('x86_loop.exe', SITE, function() end)))
-- rendezvous：两边**同时**停住才算达成（命中目标就**不** cont）
local ok, err = dbglib.wait_all({ { sess = a, mod = 'x86_loop.exe', rva = SITE },
                                  { sess = b, mod = 'x86_loop.exe', rva = SITE } })
print('wait=' .. tostring(ok) .. ' err=' .. tostring(err))
print('both_paused=' .. tostring(a:paused() and b:paused()) ..
      ' ra=' .. tostring(a.stop and a.stop.rva) .. ' rb=' .. tostring(b.stop and b.stop.rva))
-- 停住之后才能读 context（这正是用 wait_all 而不用 run_all 的理由）
local ca, cb = a:context(), b:context()
print('ctx=' .. tostring(ca ~= nil and cb ~= nil and ca.Cip ~= nil and cb.Cip ~= nil))
print('close=' .. tostring(a:detach()) .. '/' .. tostring(b:kill()) .. ' left=' .. #dbg.sessions())
"@
    $r = Run-Local $s_sugar_wait 60000
    Check 'sugar.wait-all' ($r.out -match 'sugar_lib=table/0\.1\.0' -and `
        $r.out -match 'sugar_two=true pids_differ=true' -and `
        $r.out -match 'sugar_arm=true/true' -and $r.out -match 'wait=true err=nil' -and `
        $r.out -match "both_paused=true ra=$($site.site) rb=$($site.site)" -and `
        $r.out -match 'ctx=true' -and $r.out -match 'close=true/true left=0') `
        "dbglib.wait_all 必须让两个会话同时停在自己的 site（$($site.site)）再读 context；out=[$($r.out)]"

    # `sugar.run-all`：autocont（record-and-continue）⇒ 命中处**不能**检查，只能靠回调收集。
    # ❗`exit` 的期望值是**实测**出来的 0x4000001F（= 1073741855）：不武装断点时 fixture 的第二个 int3
    #   走 NOT_HANDLED、进程被系统以 WOW64 断点状态终止 —— 所以它**不是** fixture 契约里的 42。
    #   一、二两个会话的 exit / hits 必须一致（同一个 target、同一条代码路径）。
    $s_sugar_run = Write-Lua 'sugar_run.lua' @"
local EXE = '$fxLua'
local hits = 0
local res, perr = dbglib.run_all({
    { imagepath = EXE, on_breakpoint = function(ev) hits = hits + 1 end },   -- upvalue 收集（D58）
    { imagepath = EXE },
})
print('run_res=' .. tostring(res ~= nil) .. ' err=' .. tostring(perr) .. ' n=' .. tostring(res and #res))
if res then
    for i = 1, #res do
        print(string.format('r%d ok=%s exit=%s hits=%d pid_pos=%s', i, tostring(res[i].ok),
            tostring(res[i].exit), res[i].hits, tostring(res[i].root_pid > 0)))
    end
end
print('left=' .. #dbg.sessions() .. ' cb_hits=' .. tostring(hits > 0))
"@
    $r = Run-Local $s_sugar_run 60000
    $rwant = 'ok=true exit=1073741855 hits=[1-9]\d* pid_pos=true'
    Check 'sugar.run-all' ($r.out -match 'run_res=true err=nil n=2' -and `
        $r.out -match "r1 $rwant" -and $r.out -match "r2 $rwant" -and `
        $r.out -match 'left=0 cb_hits=true') `
        "run_all 返回两张结果表（ok/exit/hits/root_pid），跑完不留下会话；out=[$($r.out)]"
}

Write-Output ''
Write-Output ("e2e_dbg: variant={0} pass={1} fail={2}" -f $(if ($Variant) { $Variant } else { 'auto' }), $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
