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
#   session.lv-types（C1c 类型统一）—— 新面里地址类值一律 LuaVariant、小整数仍是 number：
#                        type(ev.ea)=='userdata' 且 tostring 以 0x 开头、type(ev.rva)=='number'、
#                        type(d:mods()[1].base)=='userdata'、type(d:read_u32(base))=='number'、
#                        type(d:read_u64(base))=='userdata'、type(d:context().Cip)=='userdata'、
#                        type(d:context().EFlags)=='number'；jsonx.encode(变体) 走 `.val`（无 `e+`）
#                        ⇒ 精确；同一现场再验证「29 名直接吃 variant」：ReadMemory(ev.ea, U32(4))
#   session.default-parity（C2a 回归门禁，plan_t7.md §11.9.1）—— 同一脚本里用**契约名**与**会话方法**
#                       读同一个事实并逐字节比对：dbg 会话活着时，29 个契约名必须作用在**同一个**引擎上
#                       （而不是宿主那个空会话）：
#     GetModBase('x86_loop.exe')   == d:mods() 里那一项的 base（归一后比）
#     ReadMemory(ea, U32(4))       == d:read(ea, 4) 的四个字节（`ea` 直接交变体：29 名就吃这个型）
#     Ea2ModnameRva(ea)            == 由 d:mods() 推出的 (modname, rva)
#                       ❗归一：LuaVariant 取 `.val`（十进制字符串）、number 取 `tostring`，两边
#                         都成十进制串再比 —— C1c 之后 `d:mods().base` 也是 LuaVariant，所以两边
#                         都走 `.val`（跨宽度与窄化都不赌 `__eq`，见 dbg_bindings.cpp 的口径块）。
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
#   T7 C-remote（跨位数会话代理，plan_t7.md §10.1 / D56）—— **只在 86 宿主上跑**（需两个位数）：
#     本机跑 32 位 `AutoDbg_86.exe <script>`，脚本里用 `dbg.remote` 连一个 64 位的
#     `--repl --server`（PowerShell 起，`finally` 里 `Kill`），在那边 create 一个 64 位 cmd.exe：
#     remote.create          `h:create{...}` 返回代理对象，`d.root_pid > 0`（pid 在远端）
#     remote.pump-context    `d:pump` 拿到事件表（type 1/3，`ev.session` 填回代理对象），
#                            `d:context().Cip > 0`（远端是 64 位，键仍是 `Cip`）⇒「32 位调试器 + 64 位目标」
#     remote.data-plane      `d:read(m.base, 4)` = 四字节 PE 头 / `d:read_u16` 是 number
#                            （字节走 `__hex`、LuaVariant 走 `__u64` 十进制串，见 dbglib.lua 的文件头）
#     remote.teardown        `d:kill()` 真终止那个 64 位目标（PowerShell 用 Get-Process 复查）
# ❗session.* / kill.* / C2a 的 4 条 / sugar.* 的 2 条 / remote.* 的 4 条都需要 x86 宿主（前四段还要
#   x86 fixture）⇒ x64 上这几段整段 SKIP（8 条）。x86（含 legacy）是 **29** 条（C1c 加了 session.lv-types）。
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
-- 新面的地址类值是 LuaVariant（`.val` = 十进制串、`tostring`/`.hex` = hex）。
-- ❗跨宽度与可能被窄化的比较不赌 `__eq`，统一走十进制串（dbg_bindings.cpp 的 C1c 口径块）。
local function dec(v) return type(v) == 'userdata' and v.val or tostring(v) end
local function same_addr(a, b) return dec(a) == dec(b) end
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
    if base and same_addr(ev.ea, base + SITE) then hit = true break end
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
-- `Cip` 是 UPTR ⇒ LuaVariant（`.hex` 是不补零的大写 hex，等价于原来的 `%X`）；`Cax` 同理。
local ctx = d:context()
print('cip=0x' .. ctx.Cip.hex .. ' want=0x' .. (base + SITE + 1).hex)
print('set=' .. tostring(d:context(nil, { Cax = 0x1234 })) .. ' cax=0x' .. d:context().Cax.hex)
d:context(nil, { Cax = ctx.Cax })
print('restored=' .. tostring(dec(d:context().Cax) == dec(ctx.Cax)))

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

    # ------------------------------------------ ⑤c 类型统一（C1c）：地址 = LuaVariant，小整数 = number
    # 一条断言把「新面两种类型」钉在同一个现场上：命中一次（事件的 ea 非 0）就停住，然后逐个看类型。
    # ❗`d:read_u64` 是 LV_U64 而 `ev.ea` 在 x86 上是 LV_U32 ⇒ 这里只断言 `type()`，不做跨宽度 `==`。
    $s_lv = Write-Lua 'lvtypes.lua' @"
local EXE = '$fxLua'
local d, err = dbg.create{ imagepath = EXE }
print('lv_obj=' .. tostring(d ~= nil) .. ' err=' .. tostring(err))
if not d then return end
local ev = d:pump(3000)      -- 先吃掉 CREATE_PROCESS，否则模块表还是空的（`d:mods()` 拿不到 base）
local base
for _, m in ipairs(d:mods()) do
    if m.name and string.lower(m.name):find('x86_loop.exe', 1, true) then base = m.base end
end
if ev then d:cont(0x00010002) end
-- 泵到「这一泵里有命中记录且 ea 非 0」为止（CREATE_PROCESS 那一泵的 ea 是 0）
for i = 1, 24 do
    ev = d:pump(3000)
    if ev == nil then break end
    if ev.ea ~= nil and ev.ea.val ~= '0' then break end
    d:cont(0x00010002)
end
if ev == nil or ev.ea == nil or ev.ea.val == '0' then
    print('lv_hit=false')
    d:kill()
    return
end
if base == nil then
    print('lv_base=false')
    d:kill()
    return
end

local ctx  = d:context()
local mods = d:mods()
local ea_obj   = type(ev.ea) == 'userdata'
local ea_hex   = string.sub(tostring(ev.ea), 1, 2) == '0x'
local rva_num  = type(ev.rva) == 'number'
local base_obj = type(mods[1] and mods[1].base) == 'userdata'
local size_num = type(mods[1] and mods[1].size) == 'number'
local u8_num   = type(d:read_u8(base)) == 'number'
local u32_num  = type(d:read_u32(base)) == 'number'
local u64_obj  = type(d:read_u64(base)) == 'userdata'
local cip_obj  = type(ctx.Cip) == 'userdata'
local efl_num  = type(ctx.EFlags) == 'number'

-- jsonx：userdata 分支直接吐 `.val`（十进制、精确）⇒ 15 位的值不会变成 `1.40699e+14`；
-- 对照的**普通 number** 走 `%.14g`，必然带 `e+` —— 这就是 C1c 的第 ② 条动机。
local jv = jsonx.encode(d:read_u64(base))
local ju = jsonx.encode(U64('140699000000000'))
local jn = jsonx.encode(140699000000000)
local precise = string.find(jv, 'e+', 1, true) == nil and
                string.find(ju, 'e+', 1, true) == nil and
                string.find(jn, 'e+', 1, true) ~= nil

-- 29 名直接吃 variant：ReadMemory 的 addr 走 lv_check(..., LV_UZ) ⇒ ev.ea 原样能进
local mem29 = ReadMemory(ev.ea, U32(4)) ~= nil

print('lv_ea=' .. tostring(ea_obj) .. ' ea_hex=' .. tostring(ea_hex) ..
      ' rva_num=' .. tostring(rva_num) .. ' base_obj=' .. tostring(base_obj) ..
      ' size_num=' .. tostring(size_num))
print('lv_regs=' .. tostring(u8_num) .. '/' .. tostring(u32_num) .. '/' .. tostring(u64_obj) ..
      ' cip_obj=' .. tostring(cip_obj) .. ' eflags_num=' .. tostring(efl_num))
print('lv_json=u64:' .. jv .. ' u64big:' .. ju .. ' num:' .. jn .. ' precise=' .. tostring(precise))
print('lv_29=' .. tostring(mem29))
print('kill=' .. tostring(d:kill()))
"@
    $r = Run-Local $s_lv
    Check 'session.lv-types' ($r.out -match 'lv_obj=true' -and `
        $r.out -match 'lv_ea=true ea_hex=true rva_num=true base_obj=true size_num=true' -and `
        $r.out -match 'lv_regs=true/true/true cip_obj=true eflags_num=true' -and `
        $r.out -match 'precise=true' -and $r.out -match 'lv_29=true' -and $r.out -match 'kill=true') `
        "新面的地址类值 = LuaVariant（ev.ea / mods().base / read_u64 / context().Cip）、小整数 = number（ev.rva / mods().size / read_u8|u32 / EFlags）；jsonx.encode(变体) 精确（无 e+）且 ReadMemory(ev.ea, U32(4)) 能通；out=[$($r.out)]"

    # ------------------------------------------ ⑤b 契约名 vs 会话方法（C2a 回归：default-parity）
    # 现场：dbg 会话活着（`g_default == st`）且**停住**在 fixture 的 int3 上。此时 29 个契约名走的
    # `current_session()` 必须就是本会话的引擎：三组值逐字节相同才算通过。
    $s_parity = Write-Lua 'parity.lua' @"
local EXE = '$fxLua'
local SITE = $($site.site)
local d, err = dbg.create{ imagepath = EXE }
print('parity_create=' .. tostring(d ~= nil) .. ' err=' .. tostring(err))
if not d then return end
d:arm('x86_loop.exe', SITE, function() end)
local ev, rounds = nil, 0
while rounds < 200 do
    rounds = rounds + 1
    ev = dbg.pump(3000, false)
    if ev == nil then break end
    if ev.rva == SITE then break end
    ev.session:cont(ev.cont_code)
    if ev.session:stopped() then break end
end
print('parity_hit=' .. tostring(ev ~= nil and ev.rva == SITE))
if not ev or ev.rva ~= SITE then print('kill=' .. tostring(d:kill())) return end

local base
for _, m in ipairs(d:mods()) do
    if m.name and string.lower(m.name):find('x86_loop.exe', 1, true) then base = m.base end
end

-- 归一：LuaVariant（userdata，`.val` 是十进制字符串）取 `.val`；number 用 tostring。
-- C1c 之后会话方法给的地址类值也是 LuaVariant，所以两边都走 `.val`（不赌 `__eq`）。
local function dec(v)
    if type(v) == 'userdata' or type(v) == 'table' then return tostring(v.val) end
    return tostring(v)
end
local function hexbytes(t, is_table)
    local h = {}
    for i = 1, #t do h[i] = string.format('%02X', is_table and t[i] or t:byte(i)) end
    return table.concat(h, ' ')
end

-- ① GetModBase（契约名，LuaVariant）vs d:mods().base（会话方法，C1c 起也是 LuaVariant）
local cbase = GetModBase('x86_loop.exe')
print('p_base_contract=' .. (cbase ~= nil and dec(cbase) or 'nil') ..
      ' session=' .. dec(base) ..
      ' same=' .. tostring(cbase ~= nil and base ~= nil and dec(cbase) == dec(base)))

-- ② ReadMemory(LuaVariant 参数) vs d:read(同族的地址值)：四个字节相同
-- （`base + SITE - 5` 已经是 LuaVariant，直接交给契约名 —— 这就是 C1c 要的「天然互通」）
local cmem = ReadMemory(base + SITE - 5, U32(4))
local smem = d:read(base + SITE - 5, 4)
print('p_read_contract=' .. hexbytes(cmem, true) .. ' session=' .. hexbytes(smem, false) ..
      ' same=' .. tostring(hexbytes(cmem, true) == hexbytes(smem, false)))

-- ③ Ea2ModnameRva(LuaVariant) vs 由 d:mods() 推出的 (modname, rva)
local cmod, crva = Ea2ModnameRva(base + SITE)
local want_rva = (base + SITE) - base
print('p_ea_contract=' .. tostring(cmod) .. '/' .. (crva ~= nil and dec(crva) or 'nil') ..
      ' session=x86_loop.exe/' .. dec(want_rva) ..
      ' same=' .. tostring(tostring(cmod) == 'x86_loop.exe' and crva ~= nil and dec(crva) == dec(want_rva)))

-- 收尾之后契约名必须回到宿主会话（空引擎 ⇒ nil），不能悬在已删除的会话上
print('kill=' .. tostring(d:kill()))
print('p_after_teardown=' .. tostring(GetModBase('x86_loop.exe')))
"@
    $r = Run-Local $s_parity
    Check 'session.default-parity' ($r.out -match 'parity_create=true' -and $r.out -match 'parity_hit=true' -and `
        $r.out -match 'p_base_contract=\d+ session=\d+ same=true' -and `
        $r.out -match 'p_read_contract=([0-9A-F]{2} ){3}[0-9A-F]{2} session=([0-9A-F]{2} ){3}[0-9A-F]{2} same=true' -and `
        $r.out -match 'p_ea_contract=x86_loop\.exe/\d+ session=x86_loop\.exe/\d+ same=true' -and `
        $r.out -match 'p_after_teardown=nil') `
        "dbg 会话活着时契约名必须作用在**同一个**引擎上（GetModBase / ReadMemory / Ea2ModnameRva 与会话方法逐字节相同），收尾后回到宿主会话；out=[$($r.out)]"

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

# ---------------------------------------------------------------- ⑧ T7 C-remote：跨位数会话代理
# 这一段的**宿主**是 32 位的 `AutoDbg_86.exe`（跑本地脚本），它用 `dbg.remote` 连一个 64 位的
# `--repl --server`，在那边 create 一个 **64 位** 目标。这条路本地根本做不到：32 位调试器附
# 64 位目标拿的是 ERROR_NOT_SUPPORTED（D56 的硬理由）⇒ 跨位数只能两个进程。
# 代理的每条方法调用 = 一条 `script` 帧（`return dbglib.remote_call(...)`），返回值走协议
# `result` 帧的 `json.value`（C-remote 唯一增量，proto 不 bump）。
if ($Render -eq '86')
{
    $Exe64 = Join-Path $Root '.out/AutoDbg_64.exe'
    if (-not (Test-Path $Exe64))
    {
        Write-Output 'SKIP: remote.* 需要 64 位产物（.out/AutoDbg_64.exe）'
    }
    else
    {
        $rport = Get-Random -Minimum 18900 -Maximum 19100   # 随机端口：并行跑门禁时不容易撞
        $rtok  = 'c0ffee1234abcd02'
        $rout  = Join-Path $TmpDir 'remote_srv.out'
        $rerr  = Join-Path $TmpDir 'remote_srv.err'
        $srv   = $null
        try
        {
            $srv = Start-Process -FilePath $Exe64 `
                -ArgumentList @('--server=tcp', '--repl', "--server-port=$rport", "--server-token=$rtok", '--server-no-echo') `
                -RedirectStandardOutput $rout -RedirectStandardError $rerr -PassThru -NoNewWindow

            # 等监听就绪（连一下再关；最多 ~5 s）
            $up = $false
            for ($i = 0; $i -lt 50 -and -not $up; $i++)
            {
                Start-Sleep -Milliseconds 100
                try
                {
                    $tc = New-Object System.Net.Sockets.TcpClient
                    $tc.Connect('127.0.0.1', $rport)
                    $tc.Close()
                    $up = $true
                }
                catch { }
            }
            if (-not $up)
            {
                Bad 'remote.create' "64 位 server 没起来（port=$rport）：$(Get-Content $rerr -Raw -ErrorAction SilentlyContinue)"
            }
            else
            {
            # 端口/令牌直接烘进脚本：Run-Local 只传一个脚本路径，插值比脚本参数稳
            $s_remote = Write-Lua 'remote.lua' @"
local TOKEN = '$rtok'
local PORT  = $rport

local h, herr = dbg.remote('127.0.0.1:' .. PORT, { token = TOKEN, timeout_ms = 20000 })
print('host=' .. tostring(h ~= nil) .. ' err=' .. tostring(herr))
if not h then return end

-- ① 远端 create：返回的是本机代理对象，root_pid 在**远端**那个进程里
local d, cerr = h:create{ imagepath = 'C:/Windows/System32/cmd.exe', cmdline = '/c ping -n 20 127.0.0.1' }
print('create=' .. tostring(d ~= nil) .. ' root_pos=' .. tostring(d ~= nil and d.root_pid > 0) ..
      ' err=' .. tostring(cerr))
if not d then print('kill=n/a close=' .. tostring(h:close())) return end
print('root=' .. tostring(d.root_pid))

-- ② pump + context：远端是**64 位**目标，寄存器表键名仍是 Cip（与本地口径同一份 context_fields）
local ev, perr = d:pump(4000)
local tries = 1
while ev == nil and tries < 3 do ev = d:pump(4000) tries = tries + 1 end
print('ev_type=' .. tostring(ev and ev.type) .. ' ev_session=' .. tostring(ev ~= nil and ev.session == d) ..
      ' err=' .. tostring(perr))
if ev then d:cont(ev.cont_code) end
local ev2 = d:pump(4000)
if ev2 then d:cont(ev2.cont_code) end
local ctx = d:context()
print('cip=' .. tostring(ctx and ctx.Cip) .. ' cip_pos=' .. tostring(ctx ~= nil and ctx.Cip > 0))

-- ③ 数据面：地址走 `__i`（≥1e14 的整数不能走 jsonx 的 %.14g）、字节走 `__hex`
local mods, merr = d:mods()
local m = mods and mods[1]
print('mods=' .. tostring(mods ~= nil and #mods > 0) .. ' m1=' .. tostring(m and m.name) ..
      ' base_pos=' .. tostring(m ~= nil and m.base > 0) .. ' err=' .. tostring(merr))
local s = m and d:read(m.base, 4)
local u16 = m and d:read_u16(m.base)
print('read_len=' .. tostring(s and #s) ..
      ' mz=' .. tostring(s ~= nil and #s == 4 and string.byte(s, 1) == 0x4D and string.byte(s, 2) == 0x5A) ..
      ' u16=' .. tostring(u16) .. ' u16_num=' .. tostring(type(u16) == 'number'))

-- ④ teardown：代理只转发动作，收尾动词仍是 detach / kill（D51）；这里用 kill
print('kill=' .. tostring(d:kill()))
print('close=' .. tostring(h:close()))
"@
            $r = Run-Local $s_remote 90000

            $mroot = [regex]::Match($r.out, 'root=(\d+)')
            $tpid  = if ($mroot.Success) { [int]$mroot.Groups[1].Value } else { 0 }
            $mq    = [regex]::Match($r.out, 'cip=(-?\d+)')
            $cip   = if ($mq.Success) { $mq.Groups[1].Value } else { 'none' }

            Check 'remote.create' ($r.out -match 'host=true' -and $r.out -match 'create=true root_pos=true' -and `
                $tpid -gt 0) "root_pid=$tpid out=[$($r.out)]"
            Check 'remote.pump-context' ($r.out -match 'ev_type=(1|3)\b' -and $r.out -match 'ev_session=true' -and `
                $r.out -match 'cip_pos=true') `
                "远端 64 位目标的第一个事件 + context().Cip>0（键仍是 Cip）；cip=$cip out=[$($r.out)]"
            Check 'remote.data-plane' ($r.out -match 'mods=true' -and $r.out -match 'base_pos=true' -and `
                $r.out -match 'read_len=4' -and $r.out -match 'mz=true' -and $r.out -match 'u16_num=true') `
                "d:read(m.base,4) 四字节 PE 头、d:read_u16 是 number（__hex / __i 过线）；out=[$($r.out)]"

            Start-Sleep -Milliseconds 400
            $alive = $false
            if ($tpid -gt 0) { $alive = $null -ne (Get-Process -Id $tpid -ErrorAction SilentlyContinue) }
            Check 'remote.teardown' ($r.out -match 'kill=true' -and $r.out -match 'close=true' -and `
                $tpid -gt 0 -and -not $alive) "pid=$tpid alive=$alive out=[$($r.out)]"

            if ($alive) { Stop-Process -Id $tpid -Force -ErrorAction SilentlyContinue }
            }
        }
        finally
        {
            # 任何一步失败都要把 64 位 server 收掉（它可能还抱着被调试的进程）
            if ($srv -and -not $srv.HasExited)
            {
                $srv.Kill()
                $srv.WaitForExit(2000) | Out-Null
            }
        }
    }
}

Write-Output ''
Write-Output ("e2e_dbg: variant={0} pass={1} fail={2}" -f $(if ($Variant) { $Variant } else { 'auto' }), $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
