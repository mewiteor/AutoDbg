# Recipe：被调试进程派生子进程，读**那个进程**的内存

场景：目标跑起来之后会 `CreateProcess` 起一个子进程（launcher、加载器、多进程宿主）。会话的键是
**一棵进程树**（根 pid + 调试期间出现的全部 `CREATE_PROCESS` 的 pid），所以子进程的事件仍归**同一个**
会话；但读它的内存要用**它自己的**句柄（`handle_for_pid`）—— `d:read` 读的是根进程。

三步：`d:procs()`（`{ pid, ... }`，根在第一个）→ `d:proc(pid)`（进程对象：`pid` 字段 + `read` /
`read_u8|u16|u32|u64` / `read_str` / `write` / `v` / `context` / `stack` / `mods`）→ 用 `p:mods()` 的
base 读那个进程的字节。回调里判断「这个事件属于父还是子」用 `d:current_pid()`。

## 1. 回调里按事件 pid 定位（回调是 pid 归属的唯一权威）

```lua
-- child_in_callback.lua：回调里 d:current_pid() == 事件 pid；读的是那个进程自己的内存
local EXE     = [[C:\path\parent.exe]]        -- 会派生子进程的靶子（换成任何会 CreateProcess 的目标）
local MODNAME = 'parent.exe'                  -- 模块名只有文件名：父子是同名模块的两个副本
local RVA     = 0x1028                        -- 要读的字段在模块里的 RVA（普通数字，D59）

local S    = {}                               -- create 的表构造在赋值之前 ⇒ 回调里用 upvalue 表（D58）
local seen = {}

local function mod_of(p, name)
    for _, m in ipairs(p:mods() or {}) do
        if m.name and string.lower(m.name) == name then return m end
    end
end

local d, err = dbg.create{ imagepath = EXE, on_breakpoint = function(ev)
    local pid = S.d:current_pid()             -- 回调里 = 产生这个事件的进程（可能是子进程）
    local p   = S.d:proc(pid)                 -- 进程对象（普通表）；pid 必须在本会话的树里
    if not p then return end                  -- 还没进树：新树的第一条事件靠「谁在 pump」认领（见要点）
    local m = mod_of(p, MODNAME)
    if m then
        -- m.base 是 LuaVariant：要参与算术先落地成普通 number（跨类型比较不赌 __eq）
        seen[pid] = p:read_u32(tonumber(m.base.val) + RVA)
    end
end }
S.d = d
if not d then error(err) end

for _ = 1, 300 do
    local ev = d:pump(500)                    -- 默认不放行：事件停在未应答状态
    if ev then
        ev.session:cont(0x00010002)           -- 裸 int3 不是引擎登记的软件断点（S18/S19）⇒ 显式放行
    elseif d:stopped() then
        break
    end
    if #d:procs() >= 2 then break end         -- 树里出现第二个 pid = 子进程已归本会话
end

print('procs=', table.concat(d:procs(), ' '), 'root_first=', d:procs()[1] == d.root_pid)
print('top_current_pid=', d:current_pid(), 'root_pid=', d.root_pid)   -- 顶层没有事件 ⇒ 只能是树根
for pid, v in pairs(seen) do
    print('pid', pid, 'value', tostring(v), (pid == d.root_pid) and 'root' or 'child')
end
if d.detach then d:detach() else d:kill() end
```

## 2. 主循环版（`d:procs()` 找子、用 `p:mods()` 的 base 读）

```lua
-- child_process.lua：同一个 RVA、两个进程、两个值（读的是各自进程的句柄）
local EXE     = [[C:\path\parent.exe]]
local MODNAME = 'parent.exe'
local RVA     = 0x1028

local function mod_of(p, name)                 -- p:mods() 只列**那个进程**的模块（d:mods() 列整棵树）
    for _, m in ipairs(p:mods() or {}) do
        if m.name and string.lower(m.name) == name then return m end
    end
end

local function hexstr(s)
    if not s then return '<unreadable>' end
    local out = {}
    for i = 1, #s do out[i] = string.format('%02X', s:byte(i)) end
    return table.concat(out, ' ')
end

local d, err = dbg.create{ imagepath = EXE }
if not d then error(err) end
local root = d.root_pid

for _ = 1, 200 do
    local ev = d:pump(500)
    if ev then
        print('event pid', ev.pid)             -- 事件的真实进程；顶层 d:current_pid() 只会给根
        ev.session:cont(ev.cont_code)
    elseif d:stopped() then
        break
    end
    if #d:procs() >= 2 then break end
end

local child
for _, pid in ipairs(d:procs()) do             -- d:procs() 是 pid 数组（不是进程对象）
    if pid ~= root then child = pid end
end
if not child then print('no child in the tree') return end

local p = d:proc(child)                        -- 普通表：inspect / jsonx.encode 都能直接用
local m = mod_of(p, MODNAME)
if not m then print('child module not loaded yet') return end

print('tree:', table.concat(d:procs(), ' '))   -- 插入序 = CREATE_PROCESS 序，根在第一个
print('child pid', p.pid, 'base=' .. m.base.hex, 'head=', hexstr(p:read(m.base, 2)))

-- p:v 绑的是 (会话, 那个 pid)：与 p:read_u32 在同一个地址上必须一致
local addr = tonumber(m.base.val) + RVA
print('p:read_u32=', tostring(p:read_u32(addr)), ' p:v():to(I)=', tostring(p:v(addr):to('I').val))

-- 线程表是整棵树的：d:threads() 每项带 pid（C2b），所以挑得出子进程的线程
local child_tid
for _, t in ipairs(d:threads()) do
    if t.pid == child and child_tid == nil then child_tid = t.tid end
end
if child_tid then print('child tid', child_tid, 'Cip=', p:context(child_tid).Cip.hex) end

if d.detach then d:detach() else d:kill() end
```

## 要点

- **会话上的同名方法是根进程**：`d:read` / `d:v` / `d:stack` 走的是「根」（`pid = 0` 在 core 里等于
  `proc().dwProcessId`），`p:read` / `p:v` 才是那个进程。`d:mods()` 列**整棵树**的模块、`p:mods()` 只列
  那个进程的。同一个地址在两个进程里可能是同一个 base（fixture 的 ASLR 只有 8 bit 熵）⇒ 判据是「值不同」
  或「写进去再读回来」，不是「base 不相等」。
- **`d:arm` 在进程树里只能打到第一个进程**（`PITFALLS.md` **S27**）：`BreakpointInfo.orignal_byte` 是单字节、
  `has_orignal_byte` 已置位就跳过 ⇒ 同名模块在第二个进程里不会被补 `0xCC`。所以子进程用**它自己的**
  `int 3` / 异常 + `d:current_pid()` + `d:proc(pid)` 定位，命中后显式 `cont`（按 pid 的断点记录层留给后续）。
- **新树的第一条 `CREATE_PROCESS` 归「发起这次 pump 的会话」**（`PITFALLS.md` **S28**）：`DEBUG_EVENT`
  不带 parent pid，只能靠「谁在 pump」推断 ⇒ 一个执行里**串行**驱动（这是设计前提），并显式用 `d:procs()`
  核对归属；单进程树不受影响。
- **`d:procs()` 是 pid 数组（不是进程对象）**，插入序、根在第一个；它是「本次调试**见过**的 pid」——
  进程退出只关掉它的缓存句柄，pid 仍在树里 ⇒ 读已退出的 pid 会 `nil, err`。`d:proc(pid)` 只接受树里的 pid。
- **`p:context(tid)` / `p:stack(tid)` 只按 tid**（线程表是整棵树的）：读一个还在跑的进程是竞态快照，
  要判「停在哪」先让它停住（`d:pump` 默认不放行）。`d:threads()` 每项是 `{ tid, pid }`。
- **地址类是 LuaVariant**（`m.base`）、RVA / pid / tid 是普通 number：比较一律比 `.val` / `.hex`，
  要算术先 `tonumber(m.base.val)`。完整口径见 `references/lua-api.md` 的「字段类型」。
- 会话收尾之后 `p:xxx` 与 `d:xxx` 一样报 `dbg: no session (call dbg.create or dbg.attach first)`；
  `dbg.remote` 的代理上**没有** `proc` / `procs` / `current_pid`（`lualib/dbglib.lua` 的方法白名单里没有）。
- 仓库里可跑的等价物：`tools/server_test/e2e_dbg.ps1` 的 `route.child-process` / `route.child-handle` /
  `d:current-pid` / `d:v-child`（靶子 `.out/fixtures/x86_spawn.exe`：父起自己当子进程、父子各写自己的标记，
  同一 RVA 上父 = 42、子 = 7；`d:current-pid` 断言回调里等于事件 pid、顶层等于 `d.root_pid`）。
