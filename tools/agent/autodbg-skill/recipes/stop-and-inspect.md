# Recipe：两个会话同时停在断点上，各读各的状态

场景：同一份 exe 起了**两个实例**（多开、并行跑同一段代码），你要让两边**同时停在各自**
的断点上，再分别读 `context()` / 内存做对比。

`Run` / `Attach` 做不到这件事：它们「一口气跑到结束」，没有「停住」这个状态。要用 `dbg` 的
**会话对象**（`d.stop` 非空 = 停住）。另外事件流是**进程级**的（一个调试器进程只有一条
`WaitForDebugEvent`），所以两个会话必须由**同一个** `dbg.pump` 循环推进 —— 不能各写一个循环。

## 1. 手写循环版（每一步都看得见）

```lua
-- two_stopped.lua：两个同名实例各自停在自己的断点上，再分别读 context / 内存
local EXE  = [[C:\path\target.exe]]   -- 同一份 exe 起两次
local MOD  = 'target.exe'             -- 模块名（只有文件名；比较时大小写不敏感）
local SITE = 0x1028                   -- 断点的**模块 RVA**（普通数字，不是 LuaVariant）

local function base_of(d, modname)
  for _, m in ipairs(d:mods()) do
    if m.name and string.lower(m.name) == string.lower(modname) then return m.base end
  end
end

-- base 是 **LuaVariant**（地址类，D59）：算术先落地成普通 number（d:read 收 number 也收 variant）
local function site_of(d, modname, rva)
  local base = base_of(d, modname)
  if not base then return nil end
  return tonumber(base.val) + rva          -- base.val 是十进制串；rva（SITE）本来就是普通 number
end

local function hex(d, addr, n)
  if not addr then return '<no module>' end
  local s = d:read(addr, n)
  if not s then return '<unreadable>' end
  local out = {}
  for i = 1, #s do out[i] = string.format('%02X', s:byte(i)) end
  return table.concat(out, ' ')
end

local a, ea = dbg.create{ imagepath = EXE }
local b, eb = dbg.create{ imagepath = EXE }
if not a or not b then
  print('create failed:', ea, eb)
  return
end
print('pids', a.root_pid, b.root_pid)          -- 同名两实例 = 两个进程

-- 允许延迟武装：模块还没加载也能登记，CREATE_PROCESS / LOAD_DLL 时补 0xCC
a:arm(MOD, SITE, function() end)
b:arm(MOD, SITE, function() end)

-- 有界轮数 + stopped() 双判：目标提前退出就永远等不到，别写死循环
for _ = 1, 200 do
  if a:paused() and b:paused() then break end
  if a:stopped() or b:stopped() then break end
  local ev = dbg.pump(300, false)              -- 进程级：读一条，按 pid 分派到归属会话
  if ev and ev.rva ~= SITE then
    ev.session:cont(ev.cont_code)              -- 不是目标断点就放行（缺省码别自己猜）
  end
end

if not (a:paused() and b:paused()) then
  print('did not reach both sites')
  a:kill(); b:kill()
  return
end

print('stop rva', a.stop.rva, b.stop.rva)      -- 两边各停在自己的 site（rva 是普通 number）
print('cache-first', a:pump(0) == a.stop)      -- d:pump 返回的就是 d.stop 那张表
print('a Cip=' .. a:context().Cip.hex .. '  b Cip=' .. b:context().Cip.hex)
print('a site:', hex(a, site_of(a, MOD, SITE), 8))
print('b site:', hex(b, site_of(b, MOD, SITE), 8))

-- 两边都读完了再放行（cont 之后 d.stop 清空，那张表不再代表「当前」）
a:cont()
b:cont()

-- 收尾：两个会话都要**显式**收尾（d = nil / 脚本结束不会替你关）
for _, d in ipairs({ a, b }) do
  if d.detach then d:detach() else d:kill() end   -- 9x 上没有 DebugActiveProcessStop ⇒ 退回 kill
end
```

## 2. 同一件事用 `dbglib.wait_all`（rendezvous）

```lua
-- two_stopped_sugar.lua：等价短版 —— 「命中就不放行」的循环在库里
local EXE, MOD, SITE = [[C:\path\target.exe]], 'target.exe', 0x1028

local a = dbg.create{ imagepath = EXE }
local b = dbg.create{ imagepath = EXE }
a:arm(MOD, SITE, function() end)
b:arm(MOD, SITE, function() end)

-- 达成目标的会话**不 cont**（= 停住）；其它事件用 ev.cont_code 放行
local ok, err = dbglib.wait_all({
  { sess = a, mod = MOD, rva = SITE },
  { sess = b, mod = MOD, rva = SITE },
})
print('both stopped:', ok, err)               -- true / nil, 'dbglib.wait_all: timeout'

if ok then
  local ca, cb = a:context(), b:context()     -- 两边都停住 ⇒ 现在才能读 context
  print('a Cip=' .. ca.Cip.hex .. '  b Cip=' .. cb.Cip.hex)   -- 地址类是 LuaVariant：用 .hex
end

a:cont(); b:cont()                            -- 收尾前先放行
a:kill(); b:kill()
```

## 要点

- **归属靠 `ev.session`，不是 `ev.pid`**：`dbg.pump` 是进程级的，别人的事件也在这一条流里；
  `d:pump` 会替你过滤，但它**缓存优先**（`d.stop` 非空就立刻返回那张表，一个事件都不读）。
- **`d.stop` 就是 `d:pump()` 返回的那张表**：判「还是不是同一个事件」用表身份 `ev == d.stop`，
  没有 seq、没有代次。`pump` 返回 `nil` **不清** `d.stop`，`cont` 成功后才清。
- **`cont_code` 才是应答码**：`ev.code` 是异常码（这是什么异常），`ev.cont_code` 是 core 算好的
  默认应答码（`0x00010002` `DBG_CONTINUE` / `0x80010001` `NOT_HANDLED`）。要吞断点用
  `d:cont(0x00010002)`。
- **别把 `arm` 打在本来就写着 `CC` 的地址上**（会反复命中）；靶子自带的 `int 3` 只能显式
  `d:cont(0x00010002)` 放行。
- **地址类是 LuaVariant，小整数是 number**：`ev.ea` / `m.base` / `stack()[i].ea` / `context()` 的
  `Cax…Cip`（以及 `Dr*` / x64 的 `R8-R15`）`d:eip()` / `read_u64` 都是 **LuaVariant**；
  `pid` / `tid` / `type` / `code` / `cont_code` / `rva` / `size` / `index` / `read_u8|u16|u32` 是
  **普通 number**。比较一律比 `.val`（十进制串）或 `.hex`（大写 hex、无前缀），**不要** `ev.ea == 0x…`
  （`__eq` 只在两边同型时成立，`U32(5) == 5` 都是 false）；要算术先 `tonumber(x.val)` 落地成数字。
  完整口径见 `references/lua-api.md` 的「字段类型」/「地址类值的比较与算术」与仓库 `PITFALLS.md` 的 **S25**。
- **`context()` 的键是 `Cax Cbx Ccx Cdx Csi Cdi Csp Cbp Cip`**（不是 `Eax`/`Eip`），另加
  `EFlags` / `SegGs…` / `Dr*`；x64 才有 `R8-R15` / `Xmm0-15`（`{ Low, High }`）。
- 停住时 `Cip` 与 `ev.ea` 的关系要看**是哪种事件**（实测，同一份靶子）：
  · `d:arm` 武装的断点被引擎**压回 EIP** ⇒ `c.Cip.val == ev.ea.val`；停住时引擎已把**原字节写回**
    （`d:read` 读到的是原指令、不是 `CC`），原指令还没执行，`cont` 时才重新补 `0xCC`；
  · 目标里**自带的 `int 3`** 系统停在它之后 ⇒ `tonumber(c.Cip.val) == tonumber(ev.ea.val) + 1`。
  ❗一律比 `.val`（或比 `tonumber(...val)`），**不要** `c.Cip == ev.ea` —— 两边都是 LuaVariant，
  `__eq` 只在两边同型时成立，而 `+ 1` 的结果会被 `lv_simple` 换成另一种类型。
  所以不要拿 `Cip` 去反推断点地址，也不要自己加减 1 当断点地址。
- 跑着的会话读内存随时可以（`d:read`），但读 context 是**竞态快照**；要判「停在哪」必须先让它停住。
- 收尾两个动词：`d:detach()`（目标继续活着，NT+）与 `d:kill()`（终止）。**9x 上 `d.detach` 根本
  不存在**（`nil`）—— 用成员存在性判 `if d.detach then … else … end`，不要 `pcall` 试探（在 NT 上
  试探 = 真收尾）。
- 仓库里可跑的等价物：`tools/server_test/e2e_dbg.ps1` 的 `session.multi-two-stopped` 与
  `sugar.wait-all`（用 `.out/fixtures/x86_loop.exe` 起两个同名实例，断言两边 `stop.rva` 相等）。
