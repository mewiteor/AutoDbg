# AutoDbg Lua API 参考

> 本文件取代旧 MCP 工具 `lua_api(topic)` 的正文（D41 之后对外只剩 CLI + raw TCP 帧）。
> 事实来源（改绑定层时同步这里）：`src/scripting/lua/registry.cpp` 的 29 个契约全局名、
> `bindings.cpp` 的参数形状、`variant.cpp` 的 LuaVariant、`nvim/addons/autodbg/library/*.lua`。
> ❗**T5（D44）删除了 `read_file` / `write_file`**：文件 I/O 只有 `open_file` 的文件对象；下面 `files` 段已按新口径改写。

## 执行模型（先读这段）

- 一次 `lua_eval` = 执行一段 Lua（**串行**，同一时刻只有一个脚本在跑）。
- `Run{...}` / `Attach{...}` 是**阻塞**的：它们创建/附加目标并进入调试循环，
  一直跑到「目标退出」或脚本里调用 `Detach()`。所以**调试逻辑全部写在回调里**。
- 要「停住目标、逐条看」用 **`dbg` 的会话对象**（下一节）：`d:pump()` 等一个事件、
  `d:cont()` / `d:step()` 推进、`d:context()` / `d:read()` 读停住的目标。`Run`/`Attach` 那套
  没有 step/pause（想「跑几秒看看」就在回调里 `gui.sleep(ms)` 或直接跑到目标退出）。
- 后台没有任何常驻调试会话：上一条 `lua_eval` 里 `Detach()` 之后进程就干净结束了。

最小骨架：

```lua
Run{                                  -- 或 Attach{ pid = 1234, ... }
  imagepath = [[C:\path\target.exe]],
  callback = function(pid, tid)        -- 入口断点（目标刚创建、还没跑）
    SetBreakpoint{
      modname  = "kernel32.dll",
      rva      = FindFuncRva("kernel32.dll", "CreateFileW"),
      callback = function(info)        -- 命中回调
        local c = GetContext(info.tid)
        print("called! rcx =", c.Ccx.hex)
      end,
    }
  end,
}
```

返回值/输出怎么回给工具调用方：
- `print(...)` 的输出会被捕获并放进工具结果（推荐用它输出结论）。
- 脚本的**第一个返回值**也会以 `tostring` 附带返回；结构化数据请
  `print(jsonx.encode(v))`（`jsonx` 是内置库，encode/decode）。
- 出错 → 工具结果 `isError = true`，带 `@文件名:行号` 的 Lua 错误。

## dbg：会话对象（主面）

> **新脚本的主面**（D52/D55/D57）。`Run` / `Attach` 是「一口气跑到结束」的便捷糖；`dbg` 是
> **状态机**：`create` / `attach` 拿一个**会话对象**，之后由你决定什么时候等事件、什么时候
> 放行、什么时候读目标。要「停住看目标」或「一次驱动多个进程」只有这一条路。
> 事实来源：`src/scripting/lua/dbg_bindings.cpp`（方法表、事件表字段、错误文案）、`src/core/context.cpp`
> （`d:context()` 的键名）。拿不准「这台机器上有什么」就问运行时：`dbg.caps()` / `help('dbg')`。

```lua
local d, err = dbg.create{ imagepath = [[C:\path\target.exe]] }   -- 或 dbg.attach(pid)
if not d then error(err) end

local ev = d:pump(3000)            -- 等一个事件；默认**不放行**（目标停在这里）
if ev then
  print(ev.type, ev.pid, ev.tid, 'ea=' .. ev.ea.hex)   -- ❗地址类是 LuaVariant：用 .hex / .val
  local c = d:context()            -- 键名是 Cax…Cip（见下表）
  print('eip=' .. c.Cip.hex)       -- Cip 也是 LuaVariant，不能塞 string.format('%X', …)
  d:cont()                         -- 放行；缺省 = ev.cont_code
end

if d.detach then d:detach() else d:kill() end   -- 收尾；9x 上没有 detach
```

### 创建 / 取回

| 调用 | 说明 |
| --- | --- |
| `dbg.create{ imagepath=… }` | = `Run` 的原语版（`CreateProcess` + `DEBUG_PROCESS`），**不循环**；返回会话对象 / `nil, err` |
| `dbg.attach(pid [, opts])` | = `Attach` 的原语版（`DebugActiveProcess`）：**只认显式 pid**；Win95/98 没有 attach（失败文案里会直说） |
| `dbg.sessions()` | `{ {root_pid, kind, state, lua_bound}, … }`：`kind` = `'create'`/`'attach'`，`state` = `'running'`/`'stopped'`/`'exited'` |
| `dbg.session(pid)` | 按 `root_pid` 取回**同一个** userdata（表身份可判等）；没有 → `nil` |
| `dbg.last()` | 最近一次 `create`/`attach` 的会话（= `dbg.*` 默认作用的那个） |

`create` 的选项：`imagepath`（必填）、`cmdline`、`cwd`、`is_new_console`、`use_scylla_hide`，
以及六个回调 `callback` / `exception_callback` / `on_exception` / `on_breakpoint` /
`on_single_step` / `on_exit_process`。❗回调收的是**一张表** `{ pid, tid, ea, code, modname, rva }`
（`callback` 里 `ea`/`code` 是 0）—— 与 `Run` 的 `callback(pid, tid)` 形状**不同**（D52：新面一律收一张表；
表里的 `ea` 是 LuaVariant、其余是小整数，见下面「字段类型」）。

### 会话对象的字段

| 字段 | 说明 |
| --- | --- |
| `d.root_pid` | 会话的键 = 树根进程（单进程树时就是目标本身）；**与事件的 `ev.pid` 是两回事** |
| `d.kind` | `'create'` / `'attach'` |
| `d.state` | `'running'`（没有未应答事件）/ `'stopped'`（停着一个）/ `'exited'`（目标没了或已收尾） |
| `d.stop` | **当前未应答的事件表**；`nil` = 没有。「停住」就是这个字段非空 |

### 事件表（`d:pump()` 的返回值就是 `d.stop` 那张表）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `type` | number | `DEBUG_EVENT` 码（`1` 异常 / `2` 建线程 / `3` 建进程 / `5` 退出进程 / `6` 加载 DLL …） |
| `pid` / `tid` | number | 事件的进程 / 线程 |
| `ea` | **LuaVariant** | 命中地址（指针宽：x86 `LV_U32`、x64 `LV_U64`）；没有命中语义的事件是 `0` |
| `modname` | string | 归属模块（没有命中语义的事件是 `""`） |
| `rva` | number | 模块内偏移（**普通 number**，不是 LuaVariant） |
| `code` | number | **异常码**（`EXCEPTION_*`；没有异常的事件是 `0`） |
| `first_chance` | number? | `1` / `0`，也可能是 `nil`（该通道 core 不上报首次命中） |
| `cont_code` | number | core 算好的**默认应答码**（`0x00010002` = `DBG_CONTINUE` / `0x80010001` = `NOT_HANDLED`） |
| `session` | userdata | 本事件**归属的会话**：多会话时靠它归因，不要拿 `ev.pid` 去猜 |

❗`code` 与 `cont_code` 不许混用：`code` 是「这是什么异常」，`cont_code` 是「该回的应答码」。

❗**类型口径（切片 C1c / D59）**：**地址类 = LuaVariant**（指针宽），**小整数 = number**。
事件表里只有 `ea` 是 LuaVariant；`type` / `pid` / `tid` / `code` / `cont_code` / `first_chance` / `rva`
都是普通 number。别把 `ev.rva` 也包成 variant —— `d:arm` 吃的就是普通数字。
比较与算术的正确姿势见下面「地址类值的比较与算术」（`ev.ea == 0x401000` 恒为 **false**）。

### 方法

| 方法 | 说明 |
| --- | --- |
| `d:pump([ms[, autocont]])` | **缓存优先**：`d.stop` 非空就立即返回它（不阻塞）；否则等属于本会话的事件。`nil` = 没等到（**不是错误**，也不清 `d.stop`）。缺省 `ms = 100` |
| `d:cont([code])` | 应答当前事件 → `true` / `false`；缺省 `code` = `cont_code`。吞掉断点用 `0x00010002`，交给程序用 `0x80010001` |
| `d:step([tid])` | 置 TF 单步（下一个事件是 SINGLE_STEP）；之后还要 `d:cont()` |
| `d:eip([tid])` | 当前指令指针（**LuaVariant**，同 `context().Cip`）；拿整张寄存器表用 `d:context()` |
| `d:arm(modname, rva, callback)` | 武装软件断点。`rva` 是**普通数字**（`GetModBase()` 返回的是 LuaVariant：要做算术先 `tonumber(x.val)`，否则 `x + n` 得到的是另一个 variant）；**允许延迟武装**（模块没加载就登记，`CREATE_PROCESS`/`LOAD_DLL` 时补 `0xCC`）。❗别打在本来就写着 `CC` 的地址上 |
| `d:hits()` | 命中窗口的**副本**：`{ {seq,type,pid,tid,ea,modname,rva,code,cont_code,first_chance}, … }`；`type` `1` 断点 / `2` 异常 / `3` 单步。字段类型同事件表（`ea` = LuaVariant，其余 number，见下面「字段类型」） |
| `d:dropped()` | 窗口满了以后被丢掉多少条（你看到的是窗口，不是全部事实） |
| `d:setcap([1..64])` | 设置/查命中窗口上限（返回实际生效值） |
| `d:stopped()` / `d:paused()` | 目标是否已结束（run 循环出口）/ 是否有未应答事件 |
| `d:caps()` / `d:help()` | 同 `dbg.caps()` / `help('dbg')` |
| `d:detach()` | 恢复断点原字节 + `DebugActiveProcessStop`，**目标继续活着**；❗**9x 上这个 key 不存在**（`d.detach == nil`） |
| `d:kill()` | 终止目标（先恢复原字节、释放断点回调） |

### 数据面

```lua
local s   = d:read(addr, 32)          -- **实际读到的字节串**（可能短于 32）；0 字节 → nil, err
local b   = d:read_u8(addr)           -- number；宽度靠函数名：read_u8/u16/u32
local q   = d:read_u64(addr)          -- **LuaVariant**（`LV_U64`：x86 上也不会被截成 4 字节）
local str = d:read_str(addr)          -- 以 NUL 结尾的字节串（不转码，与 LoadStr 同一函数）
d:write(addr, "\x90\x90")             -- 字节串原样写
d:write(addr, 0x1234)                 -- 整数 = u32 小端 4 字节

local c = d:context()                 -- 整张寄存器表：**所有字段都是 LuaVariant**；x64 的 Xmm* 是 { Low=, High= }（两个 variant）
d:context(nil, { Cax = 0x1234 })      -- **部分更新**（只写表里出现的键）→ true / nil, err
local frames = d:stack()              -- { {index, ea, modname, rva}, … }（栈回溯，与 StackTrace 同形）
for _, m in ipairs(d:mods()) do end   -- { name, base, size, path }（base 是 LuaVariant，size 是 number）
for _, t in ipairs(d:threads()) do end-- { {tid}, … }
```

- `d:context([tid])` 的键名（源码：`src/core/context.cpp` 的 `k_context_fields[]`）：
  **`Cax Cbx Ccx Cdx Csi Cdi Csp Cbp Cip`** + `ContextFlags` / `EFlags` / `SegGs…SegSs` / `Dr0-Dr3` /
  `Dr6` / `Dr7`；x64 另加 `R8-R15`、`Xmm0-15`（`{Low, High}`）、`P1Home…P6Home`、`MxCsr`、
  `VectorControl`、`DebugControl`、`LastBranch*`。**不是** `Eax`/`Eip`/`Rip`。
  ❗**所有字段都是 LuaVariant**（切片 C1d 起，与契约名 `GetContext` **逐字段同型**）：`UPTR` 的键
  （`Cax…Cip`、`Dr0-Dr7`、x64 的 `R8-R15` / `P1Home…P6Home` / `LastBranch*Rip` /
  `LastException*Rip`）是 `LV_UZ`；`U32` 的键（`ContextFlags` / `EFlags` / `SegGs…SegSs` /
  `MxCsr`）是 `LV_U32`；`U64` 的键（`VectorControl` / `DebugControl`）是 `LV_U64`；`M128` 的键
  （`Xmm0-15`）是 `{ Low, High }` 两个 `LV_U64`。⇒ 打印一律 `.hex` / `.val`，**不要**
  `string.format('%X', c.Cip)`（variant 不是 number，见下面「地址类值的比较与算术」）。
  **setter 两种都收**：`d:context(nil, { Cip = 0x401000 })` 与 `d:context(nil, { Cip = c.Cip })`
  一样有效（`arg_num_or_lv`），`{ EFlags = 0x202 }` 也不必先包 `U32()`。
- 缺省 tid = 当前停住的那个线程（`d.stop.tid`），没有未应答事件时退到最近一条事件的 tid；
  要看别的线程就**显式**给 tid。
- `d:read(addr, n)` 与契约名 `ReadMemory` 的差别只有形状：那边返回字节数组、部分读被吞成整体失败；
  这边返回**字节串 + 真实长度**。`d:stack([tid[, depth]])` 的 `depth` 缺省 64、上限 512。

### 字段类型：地址类 = LuaVariant，小整数 = number（切片 C1c / D59）

新面（`dbg` 会话）只有两条规则，**按字段位置**分，不按值的形状猜：

| 来源 | 字段 | 类型 |
| --- | --- | --- |
| 事件表 / `d:hits()` / 用户回调 | `ea` | **LuaVariant**（指针宽：x86 `LV_U32`、x64 `LV_U64`） |
| 同上 | `seq` / `type` / `pid` / `tid` / `rva` / `code` / `cont_code` / `first_chance` | number |
| `d:mods()` | `base` | **LuaVariant**（`name` / `path` 是 string） |
| 同上 | `size` | number |
| `d:stack()` | `ea` | **LuaVariant**（`modname` 是 string） |
| 同上 | `index` / `rva` | number |
| `d:context()` | **所有**标量键（`ctx_kind::U32` / `U64` / `UPTR`） | **LuaVariant**（`LV_U32` / `LV_U64` / `LV_UZ`；与 `GetContext` **逐字段同型**） |
| 同上 | `ctx_kind::M128` 的键（x64 `Xmm*`） | 表 `{ Low, High }`（两个 **LuaVariant**，`LV_U64`） |
| `d:eip()` | 返回值 | **LuaVariant** |
| `d:read_u64()` | 返回值 | **LuaVariant**（`LV_U64`：宽度由函数名给出，x86 上也不截断） |
| `d:read_u8/u16/u32()` | 返回值 | number |
| `d:read()` / `d:read_str()` | 返回值 | string（字节串） |
| `d:v(addr)` | 返回值 | **Var**（userdata；`[...]` / `:to` 解引用，见「V 与 d:v：指针链走查」） |
| `dbg.sessions()` / `d.root_pid` / `d:threads()` | `root_pid` / `tid` | number |

**入参一侧（读写对称）**：`d:read*` / `d:read_str` / `d:write` 的**地址实参**、`d:context(tid, t)` 的
**字段值**，number 与 LuaVariant **都收**（`arg_num_or_lv`：userdata 必须是 LuaVariant，number 侧
走 `lua_isnumber`，数字串也照旧收）。
❗唯一「只收普通 number」的是 **`d:arm(modname, rva, callback)` 的 `rva`** —— `ev.rva` 与 `d:arm`
的 rva 都是 **number**，不要包变体（D57 的门禁大量依赖 `ev.rva == 0x1028`）。
为什么要这么统一（而不是一律 number）：29 个契约名（`ReadMemory` / `Ea2ModnameRva`…）本来吃
LuaVariant ⇒ `ReadMemory(ev.ea, U32(4))` 直接可用；且 `--json` 的 `result` 与 `dbg.remote` 过线时
`jsonx` 的 userdata 分支吐 `x.val`（精确十进制），x64 地址不丢低位。

### 地址类值的比较与算术（AI 最容易踩）

地址是 LuaVariant ⇒ **不能**直接 `==` 数字，也不能丢给 `string.format`：

```lua
-- ❌ 错法
if ev.ea == 0x401000 then end          -- 恒为 false：__eq 只在两边**同型**时成立（U32(5) == 5 也是 false）
print(string.format('%X', c.Cip))      -- 运行时报错：variant 不是 number（string.format 不认 __tostring）

-- ✅ 比较：.val（十进制串）或 .hex（大写 hex、无前缀、不补零）—— 跨宽度是同一形态
if ev.ea.val == '4198400' then end     -- 4198400 == 0x401000
if ev.ea.hex == '401000' then end
if c.Cip.val == ev.ea.val then end     -- 「停在哪 == 断点地址」这类判断一律比 .val

-- ✅ 算术：先落地成普通 number 再算（地址实参 number 也收，见上表）
local next_u32 = tonumber(ev.ea.val) + 0x10
local word     = d:read_u32(next_u32)
if word then print('next u32', string.format('%X', word)) end   -- read_u32 返回 number，可以 %X
```

- **别拿 `+` 的结果去比**：算术结果会经 `lv_simple` 归一成**能装下的最窄类型**
  （`ev.ea + 1` 就可能换型）⇒ 类型一变 `==` 立刻判 false。要算就先 `.val` 转数字，全程用 number。
- `tostring(v)` **形态不稳定**：大于 10000 给带 `0x` 前缀的定宽 hex，小值切回十进制 ⇒ 日志/`print`
  里用没问题，**别拿它做比较、也别当载荷格式**（过线用 `.val`）。
- `.val` 是**十进制字符串**、`.hex` 是**大写 hex、不带 `0x`、不补零**（实测 `U32(0x401000).hex ==
  '401000'`）；两者跨宽度是同一形态，`tostring` 不是。
- `tonumber(v.val)` 走 double：x64 用户态地址（≤ 47 位）在 2^53 以内，精确；只有极端高位地址要留心。
- 完整的坑与门禁教训见仓库 `PITFALLS.md` 的 **S25**。

### 模块级 `dbg.*`

除了 `create` / `attach` / `sessions` / `session` / `last`，模块级还保留一组**作用在默认会话**
（= `dbg.last()`）上的转发：`dbg.pump([ms[, autocont]])` / `dbg.cont([pid[, tid[, code]]])` /
`dbg.step([tid])` / `dbg.eip([tid])` / `dbg.arm(...)` / `dbg.hits()` / `dbg.dropped()` /
`dbg.setcap([n])` / `dbg.stopped()` / `dbg.paused()` / `dbg.caps()` / `dbg.help()` /
`dbg.detach([pid])` / `dbg.kill([pid])`（后两个给 pid = 按 `root_pid` 指名哪个会话）。

❗两层 pump 的分工（D57）：

- **`dbg.pump` 是唯一的「等」，而且是进程级的**：读一条事件 → 按 `dwProcessId` 分派到归属会话
  （记录 + 回调 + 引擎记账）→ 返回带 `session` 的事件表。**不缓存**。
- **`d:pump` 缓存优先 + 过滤**：别人的事件已经在它自己的会话里分派完了（分派不能推迟），
  只是不返回给本次调用方。
- 数据面（`read` / `context` / `stack` / `mods` / `threads`）只有 `d:` 形式，没有模块级转发。

### 错误口径（一律 `nil, err`，不抛异常）

门禁逐字判据的三条固定文案：

```text
pump cannot be called from a callback
cannot run while an event is unhandled
cannot run while a dbg session exists (detach/kill it first)
```

- 第 1 条：**回调里**调状态机（`pump`/`cont`/`step`/`arm`/`detach`/`kill`/`create`/`attach`）。
- 第 2 条是 T7.3 的「有未应答事件」判据（`dbg_error_paused()`）；现行 `Run`/`Attach` 走的是第 3 条
  —— C 侧 registry 里**存在**任何 dbg 会话就拒（两条路径都独占整个进程的事件流）。
- 没有会话（或已收尾）时：`dbg: no session (call dbg.create or dbg.attach first)`。

## V 与 d:v：指针链走查

`V(addr)` 是 29 个契约名之一，但切片 C1d 起它**只在调试回调里可用** —— 回调里「产生该事件的
会话」是唯一确定的（`CallbackScope` 已经挂好）；顶层 / REPL 里没有这个答案，引擎**不猜**。

| 场合 | 写法 | 解引用走哪个引擎 |
| --- | --- | --- |
| 调试回调（`Run`/`Attach` 的 `callback`、`SetBreakpoint` 的回调、`d:arm` 的回调） | `V(addr)[off1][off2]:to('I')` | 产生该事件的那个会话 |
| 顶层脚本 / `--repl` | `d:v(addr)[off1][off2]:to('I')` | **`d` 这个会话**（与「最近一次 create/attach」无关） |
| `dbg.remote` 的代理 `d` | 两者都不行 → `d:read_u32` / `d:read_u64` 逐跳读 | 见下面「代理上不支持 `d:v`」 |

- **顶层 / REPL 调 `V()`** → 返回 `nil, msg`（是**返回值**不是 error，所以 `local v, e = V(addr)`
  能直接看到原因），文案逐字是：
  `V() is only usable inside a debug callback; use d:v(addr) or d:read*/d:write instead`
- **REPL 里 `V` 根本不存在**（`register_lua_api(L, repl)` 不注册它）⇒ `V` 是 `nil`，对着提示符敲
  它是 `attempt to call a nil value (global 'V')`（实测原文）。交互那一侧直接用 `d:v`。
- `V(addr)` 的 `addr` **number 与 LuaVariant 都收**（`V(c.Ccx)` = 「把寄存器里的值当地址」）。

```lua
-- 回调里（V 可用）：引擎就是跑这次回调的会话，不需要你点名
Run{ cmdline = [[C:\path\target.exe]], callback = function()
  SetBreakpoint{ modname = 'my.dll', rva = FindFuncRva('my.dll', 'F'),
    callback = function(info)
      local c    = GetContext(info.tid)
      local flag = V(c.Ccx)[0x10][0x8]:to('I')   -- [[ECX]+0x10]+0x8 处的 uint32
      print('flag =', flag.hex)                  -- 返回 LuaVariant：用 .hex / .val
    end }
end }
```

`d:v(addr)` 是**会话绑定**的 Var：载荷 = `{ 地址, 本会话的引擎 }`，拿在手里跨会话都
不串；构造之后 `[...]` 逐跳解引用、`:to` / `:tos` / `:write` / `:writes` / `:swrite` 全走
**那个会话**（与 `current_session()` 无关）。

```lua
-- 顶层（V 不可用）：同一张链，引擎换成 d 这个会话
local d  = dbg.create{ imagepath = [[C:\path\target.exe]] }
local ev = d:pump(3000)                        -- 要有停住的目标才读得到东西
if ev then
  local root = ev.ea                           -- 链头用命中地址；ev.ea 为 0 的事件（如 CREATE_PROCESS）改用 d:mods()[1].base
  local flag = d:v(root)[0x10][0x8]:to('I')    -- 逐跳：取指针 → 取指针 → 读 uint32
  print('flag =', flag.hex)                    -- .hex / .val；别塞 string.format('%X', …)
  d:cont()
end
d:kill()                                       -- 收尾之后 d:v 造出来的 Var 都不能再用
```

- **逐跳语义**：`v[off]` 在「当前地址 + `off`」处读**指针宽**（x86 4 字节 / x64 8 字节），返回
  **新的 Var**（地址 = 读到的值，仍绑同一个会话）；`off` 收 number 也收整数类 LuaVariant。
- **`:to(type[, off])` 的单字母码**（`Var` 用单字母，`LuaVariant:to` 才是全名）：
  `c` int8 / `b` uint8 / `h` int16 / `H` uint16 / `i` int32 / `I` uint32 / `q` int64 /
  `Q` uint64 / **`z` = `size_t`** / `f` float / `d` double。返回 **LuaVariant**（不是 number）⇒
  比较与打印用 `.val`（十进制串）/ `.hex`，见上面「地址类值的比较与算术」与仓库 `PITFALLS.md` 的
  **S25**。要连续 n 个值用 `:tos(type, n[, off])`（返回变体表）。
- **会话收尾之后**（`d:detach()` / `d:kill()`）再用同一个 Var：
  `Var: the session bound by d:v() is closed (detach/kill)` —— 宁可报错，也不对已释放的引擎读写。
- `d:v` 自己的入参错误照旧是 `nil, err`：`d:v: addr (number or LuaVariant) expected` /
  `d:v: addr must be positive`（`d:v(0)` 不给 Var —— 0 地址没有意义，拿它判失败即可；
  `CREATE_PROCESS` / `LOAD_DLL` 这类事件的 `ev.ea` 就是 `0`，别拿它当链头）。

**代理上不支持 `d:v`**：`dbg.remote` 的 `RemoteSession` 只转发 `lualib/dbglib.lua` 的
`SESSION_METHODS` 白名单，里面**没有 `v`** ⇒ 代理上调 `d:v(...)` 是普通 Lua 错误
`unknown method 'v'`（`Var` 的载荷带本机引擎指针，本来也过不了线）。代理那一侧读指针链用
`d:read_u32` / `d:read_u64` 逐跳：

```lua
-- 代理（32 位宿主看 64 位目标）：没有 d:v，逐跳用数据面读
local p = d:read_u64(ev.ea)                     -- 指针宽：x64 目标用 read_u64
local q = d:read_u64(tonumber(p.val) + 0x10)
print('flag =', d:read_u32(tonumber(q.val) + 0x8))
```

## dbglib：多会话脚本糖

`lualib/dbglib.lua` 烘焙成全局 `dbglib`（`dbglib._version` 当前是 `'0.1.0'`）。它**只调** `dbg`，
不新增 C 代码；两个函数都把失败当值返回，不抛异常。

### `dbglib.wait_all(goals [, opts])` —— rendezvous（两边同时停住）

```lua
local EXE, MOD, SITE = [[C:\path\target.exe]], 'target.exe', 0x1028
local a = dbg.create{ imagepath = EXE }
local b = dbg.create{ imagepath = EXE }
a:arm(MOD, SITE, function() end)
b:arm(MOD, SITE, function() end)

local ok, err = dbglib.wait_all({
  { sess = a, mod = MOD, rva = SITE },
  { sess = b, mod = MOD, rva = SITE },
})
if ok then
  local ca, cb = a:context(), b:context()   -- 两边都停住 ⇒ 现在才能读 context
  print(ca.Cip.hex, cb.Cip.hex)             -- 地址类是 LuaVariant：比/印都用 .hex / .val
end
a:cont(); b:cont()                          -- 收尾前先放行
if a.detach then a:detach() else a:kill() end
b:kill()
```

- `goals` 每项 `{ sess = <会话对象>, mod = '名字'(可省), rva = <普通数字> }`；比较口径是
  「同一个会话 + RVA 相同 + 模块名相同（给了 `mod` 才算，大小写不敏感）」。
- 命中目标的会话**不 cont**（= 停住，这就是 rendezvous 的语义）；其它事件用 **`ev.cont_code`** 放行。
- `opts = { timeout_ms = 8000, pump_ms = 200 }`；上界是**迭代次数** `ceil(timeout_ms/pump_ms)`
  （不用墙钟：两个 Lua 版的 `os.clock`/`os.time` 语义不一致）。
- 任一未达成目标的会话 `stopped()`（目标先退了）→ 立即 `nil, err`；超时 → `nil, 'dbglib.wait_all: timeout'`。

### `dbglib.run_all(params_list [, opts])` —— batch / collect

```lua
local EXE = [[C:\path\target.exe]]
local hits = 0                                   -- 回调里用 **upvalue** 收集（D58：别用全局）
local res, perr = dbglib.run_all({
  { imagepath = EXE, on_breakpoint = function(ev) hits = hits + 1 end },
  { imagepath = EXE },
})
if res then
  for i = 1, #res do
    print(res[i].root_pid, res[i].ok, res[i].exit, res[i].hits)
  end
end
if perr then print('timeout:', perr) end          -- 'dbglib.run_all: timeout'
print('breakpoint hits seen in callbacks:', hits)
```

- 每项就是一个 `dbg.create{...}` 的选项表；全部 `create` 成功后才开泵，`dbg.pump(pump_ms, true)`
  （**autocont**）跑到所有会话 `stopped()`。
- 返回每会话一张结果表 `{ root_pid, ok, exit, hits }`：`exit` = 收到的 `EXIT_PROCESS` 退出码，
  没收到（超时/被杀）就是 `nil`；`hits` 是 `#d:hits()`（`kill` 之前取）。
- ❗**autocont ⇒ 不能在命中处检查**（引擎在 `dispatch` 末尾就应答了，没有「停住」这回事）。
  要「两边同时停住」用 `wait_all`；要「跑到结束再比较」用 `run_all`。
- 超时**不吞**：返回已经收集到的结果 + 第二个返回值 `'dbglib.run_all: timeout'`；
  结束时会 `kill` 每个会话（`ok = false` = 那次 kill 失败，其余照常收）。

## dbg.remote：跨位数的会话代理

> **为什么需要它**：位数是唯一绕不过去的硬理由（D56）—— 32 位调试器的 `DebugActiveProcess`
> 附 64 位目标拿到的是 `ERROR_NOT_SUPPORTED`，x64 模块的 `parse_pe` 也处理不了 32 位目标。
> 所以「32 位宿主调 64 位目标」只能是**两个进程**：64 位那一侧真正调试，这一侧用**代理对象**
> 驱动过去。代理的方法面与本地 session 相同（`d:pump` 有时是 OS wait，有时是 socket wait），
> 脚本不用分两套写；强隔离 / 崩溃隔离是顺带的好处，硬理由只有位数。
> 事实来源：`lualib/dbglib.lua`（`remote` / `RemoteHost` / `RemoteSession` / `remote_call`）、
> `src/scripting/lua/dbg_bindings.cpp` 的 `dbg.remote*` 转发、`src/scripting/lua/client.cpp` 的
> `remote_connect` / `remote_run_script` / `remote_close`；散文版是 `help('dbg.remote')`。

### 前提：远端必须是 `--repl --server`

```bat
:: 位数够的那一侧（这里是 64 位）：--repl 让连接内的 lua_State 跨命令存活 —— 代理全靠这一条
.out\AutoDbg_64.exe --server=tcp --repl --server-port=8642 --server-token=c0ffee1234abcd02
```

- **每次方法调用 = 一条 `script` 帧**：body 就是 `return dbglib.remote_call(<pid>, "<method>", "<args json>")`；
  返回值从 `result` 帧的 `json.value` 取回（协议面唯一增量，**proto 不 bump**，仍是 2）。
- 必须是 `--repl --server`，不能是裸 `--server`：非 `--repl` 的 server 每条命令都是**新** `lua_State`，
  上一条命令建起来的会话在下一帧里就不存在，代理必然失败。
- 显式给 `--server-token=<hex>` 时 loopback 上**也**校验（显式 token ⇒ `auth_required = true`）
  ⇒ 客户端 `opts.token` 必须同值；两边都不给（loopback 且没设 token）才免校验。

### 形状

```lua
local host, err   = dbg.remote('127.0.0.1:8642', { token = 'c0ffee1234abcd02' })              -- RemoteHost
local one,  err2  = dbg.remote('127.0.0.1:8642', { token = 'c0ffee1234abcd02', pid = 4242 })  -- 钉住的 RemoteSession
```

| 参数 | 说明 |
| --- | --- |
| `addr` | `'host:port'`（IPv4 文本或 `localhost`；**不做 DNS**） |
| `opts.token` | 握手帧里的 token（远端要求的那个） |
| `opts.pid` | 给了就直接返回**钉住**这个 `root_pid` 的 RemoteSession；不给返回 RemoteHost |
| `opts.timeout_ms` | **每次调用**的整次上限，缺省 8000（到点 ⇒ `nil, 'dbg.remote: timeout'`） |
| `opts.read_timeout_ms` | 单帧读超时；0 / 缺省 ⇒ C 侧的 5000 |
| `opts.max_frame` | 能接受的帧上限；0 / 缺省 ⇒ 8 MB |

### RemoteHost：远端宿主

| 方法 | 说明 |
| --- | --- |
| `h:create(opts)` | 与 `dbg.create{...}` 同参（**`imagepath` 必填**，`cmdline` 是参数串）；返回本机代理对象，`d.root_pid` 是**远端**那个进程。`opts` 里带函数 ⇒ `nil, 'dbg.remote: create options cannot cross the wire (function/userdata)'` |
| `h:attach(pid)` | 远端的 `dbg.attach(pid)`，返回代理对象；`pid` 不是数字 ⇒ `nil, 'dbg.remote: attach: pid (number) expected'` |
| `h:sessions()` | 远端 `root_pid` 的数组（session 对象是 userdata，过不了线） |
| `h:close()` | 关连接（重复调无害：C 侧先把指针置空）；**不动目标** —— 收尾永远是显式的 `d:kill()` / `d:detach()`（D51） |

### RemoteSession：方法面与本地 session 相同

白名单原文（`dbglib.lua` 的 `SESSION_METHODS`）：

`pump` `cont` `step` `eip` `arm` `hits` `dropped` `setcap` `stopped` `paused` `caps`
`detach` `kill` `read` `read_u8` `read_u16` `read_u32` `read_u64` `read_str` `write`
`context` `stack` `mods` `threads`

- 另有 `d:close()`（等同 `h:close()`）：连接是**共享**的，任何一个 close 都会让同一条连接上的其余代理失效。
- `d.root_pid` 是远端的 `root_pid`。字段就这一个 —— 代理上没有 `d.stop` / `d.state` / `d.kind`
  （那些是 C 侧会话对象的字段）；「停住」仍然看 `d:pump()` 返回的那张事件表。
- **`d:pump([ms])` 返回普通表，且 `ev.session` 被填回这个代理对象** ⇒ `ev.session:cont(ev.cont_code)`
  在本地会话与代理上写法完全一致（远端表里的 `session` 是远端那个 userdata，过不了线，客户端补回来）。
  省略 `ms` 就是远端 session 的缺省（`100`）。
- 能力要**问远端**：`d:caps()` 报的是远端那台机器 / 那个 build 有什么。代理对象的 `d.detach`
  **永远是函数**（薄转发），远端是 9x 时调用才失败（见下面「错误」的最后一行）。

### 过线的三种标记（代理已自动处理）

- **LuaVariant**（地址类：`ev.ea` / `hits()[i].ea` / `mods()[i].base` / `stack()[i].ea` / `d:eip()` /
  `read_u64` / `context()` 的 `UPTR` 键…）⇒ `{"__u64":"140707532264324"}`，取的是 `x.val`
  （**十进制串**）；解码侧 `U64(那个串)` 走 `strtoull`，**不经过 Lua 的 double** ⇒ x64 的 15 位地址
  一位都不丢（`jsonx.encode` 的 number 分支是 `%.14g`，实测 `jsonx.encode(140707532264324)` →
  `1.4070753226432e+014` —— 直接过线会静默丢低位）。
  ❗C1c 起地址类值本来就是 LuaVariant（`jsonx` 的 `userdata` 分支吐 `x.val`）⇒ 编码侧走 `__u64`；
  旧对端发的 `{"__i":"…"}` **解码侧仍收**（兼容，不是现行编码）。
- **普通 number** 且 `>= 1e14` ⇒ `{"__i":"140699000000000"}`。这一支只给「脚本自己传了裸大整数」
  兜底（例如自己算出来的绝对地址）—— 传地址首选用会话里拿到的 variant，别自己拼 number。
- **字节串** ⇒ `{"__hex":"B8444250…"}`（`read` / `read_str` 的**返回值**、`write` 的**第二个参数**）；
  `nil` 参数 ⇒ `{"__nil":true}` 占位。
- 两端共用 `encode_value` / `decode_value`，所以脚本这一侧拿到的是**普通 Lua 值**（variant 过线回来
  仍是 LuaVariant，`d:read(m.base, 4)` 直接用），不用管标记。
  ❗过线回来的地址一律是 `U64(…)`（`LV_U64`）⇒ 与本地 x86 的 `ev.ea`（`LV_U32`）**不同型**，
  跨代理比较同样只能比 `.val` / `.hex`。
- `--json` 的 `result` 走同一个 `jsonx` userdata 分支 ⇒ 地址类是精确十进制，不会被 `%.14g` 截断。
  为什么这么定：见仓库 `PITFALLS.md` 的 **S23**。

### 能力边界（不是待办，是设计）

- **函数 / userdata / session 对象过不了线**：客户端编码阶段就返回
  `nil, 'dbg.remote: argument cannot cross the wire (function/userdata/Nan)'`；服务端侧另有一条
  `{"ok":false,"err":"unserializable"}`。
- **`d:arm` 在代理上不可用**（第三个参数是回调函数，上面那条就是它的结果）。回调只能活在**远端**
  那个 state 里 ⇒ 要武装断点就在远端那侧做，代理这一侧负责 `pump` / `cont` / 数据面 / 收尾。
- **单条命令有 4096 字节上限**：一条 REPL 命令的 chunk 上限是 `k_repl_chunk_cap = 4096`
  （超了按语法错 `3` 处理）⇒ `d:write` 一次别超过**约 1.8 KB**（字节过线是十六进制，
  一个字节两个字符，还要算上命令本身的开销）。
- **`open_file` 在代理路径上不支持**：代理连接把 `open` / `data` / `read_req` / `close` 帧一律丢弃
  ⇒ 远端那条 `open_file` 只会等到超时。
- 远端 REPL 会把返回值 `print` 进 `out` 帧（`--repl` 本就如此），客户端直接丢弃；大 payload 因此
  多跑一趟 socket，功能无影响。

### 错误（一律 `nil, err`，不抛异常）

| 情形 | 文案 |
| --- | --- |
| `addr` 形状不对 | `nil, "dbglib.remote: addr must be 'host:port'"` |
| 连不上 / 没有 winsock | `'dbg.remote: connect failed: <net_status_name>'`（连不上复用 `bind-failed` 这个名字）/ `'dbg.remote: no winsock (ws2_32.dll / wsock32.dll)'` |
| token / 版本不符 | `'dbg.remote: {"error":"unauthorized"}'` / `{"error":"bad-version",…}` —— 服务端 `error` 帧的 json **原文** |
| 握手没应答 / 回帧不对 | `'dbg.remote: no handshake response'` / `'dbg.remote: unexpected handshake reply'` |
| 单次调用到点 | `'dbg.remote: timeout'` |
| 连接已关 | `'dbg.remote: connection closed'` |
| 远端那条命令失败 | `'dbg.remote: <reply.err 或 Lua 错误文本>'`，例如 `unserializable` / `no session 1234` |
| 远端能力缺席 | `'dbg.remote: method 'detach' is not available on this host'` |

### 骨架（32 位宿主看 64 位目标）

```lua
-- cross_arch.lua：先在另一侧起好 AutoDbg_64.exe --server=tcp --repl --server-port=8642 --server-token=...
local h, herr = dbg.remote('127.0.0.1:8642', { token = 'c0ffee1234abcd02', timeout_ms = 20000 })
if not h then error(herr) end                     -- 连不上 / 错 token：nil, 'dbg.remote: …'

local d, cerr = h:create{
  imagepath = [[C:\Windows\System32\cmd.exe]],  -- dbg.create 的必填项，代理同参
  cmdline   = '/c ping -n 20 127.0.0.1',
}
if not d then h:close(); error(cerr) end
print('remote root_pid', d.root_pid)              -- pid 在**远端**那个进程里

local ev
for _ = 1, 3 do                                   -- 第一条事件通常是 CREATE_PROCESS（type = 3）
  ev = d:pump(4000)
  if ev then break end
end
if not ev then d:kill(); h:close(); error('no event from the remote session') end
print('ev', ev.type, ev.pid, ev.tid)

print('Cip=' .. d:context().Cip.hex)              -- 停住时读；键名/类型与本地一致（Cax … Cip）

local mods, merr = d:mods()                       -- { { name, base, size, path }, … }
if not mods or not mods[1] then d:kill(); h:close(); error(merr or 'no modules') end
local m    = mods[1]
local head = d:read(m.base, 4)                    -- 字节串（base 是 variant ⇒ 走 __u64、字节走 __hex）
print(m.name, 'base=' .. m.base.hex, 'head_len=' .. tostring(head and #head))

d:cont(ev.cont_code)                              -- 应答码用 cont_code，不是 ev.code
print('kill', d:kill())                           -- 收尾动词仍是 kill / detach（D51）
print('close', h:close())                         -- 只关连接；不动目标
```

### 与本地 registry 的关系

- 代理**不进**本地 `dbg.sessions()` / `dbg.session(pid)`：它只是本机的一个 Lua 对象（连着一条 socket），
  不是调试会话。
- 远端有哪些会话问 `h:sessions()`（只给 `root_pid` 列表 —— session 是 userdata，过不了线）。
- `dbglib.wait_all` / `run_all` 是**本地**多会话糖：它们跑在 `dbg.pump` 这条进程级事件流上，
  代理的事件不在里面 ⇒ 跨位数的组合得自己写 `d:pump` 循环（形状见 `recipes/cross-arch-remote.md`）。

## help 与 caps

三方分工（D53），互不包含：

| 问题 | 去哪里 |
| --- | --- |
| 语义 / 契约 / 坑 / 例子 | 本技能包（`references/` + `recipes/`） |
| 「这台机器 / 这个 build 上有什么」 | `dbg.caps()`（**机器可读**） |
| 「现在敲什么」 | `help([topic])` / `dbg.help()`（人类散文） |

```lua
help()              -- 索引（顶层主题），返回 true
help('dbg')         -- dbg 表全部成员（= dbg.help()）
help('dbg.pump')    -- 单个成员：签名 + 口径 + 坑
local ok, err = help('no-such-topic')       -- nil, "no help for 'no-such-topic' (try help())"

local c = dbg.caps()
-- c.arch='x86'|'x64'  c.variant='modern'|'legacy'  c.flavor='lua'|'luajit'
-- c.win9x=false  c.attach=true  c.detach=true  c.hits_cap=64
print(tostring(dbg.detach ~= nil))          -- ❗AI 的权威能力判据
```

- `help` 的主题全部来自源码的一张表（`help_bindings.cpp` 的 `kHelp[]`）：`help` / `dbg` /
  `dbg.create` / `dbg.attach` / `dbg.pump` / `dbg.cont` / `dbg.step` / `dbg.eip` / `dbg.arm` /
  `dbg.hits` / `dbg.dropped` / `dbg.setcap` / `dbg.sessions` / `dbg.session` / `dbg.last` /
  `dbg.stopped` / `dbg.paused` / `dbg.detach` / `dbg.kill` / `dbg.caps` / `dbg.help` /
  `gui` / `open_file` / `legacy`。`help('dbg')` 的成员列表是**按主题前缀生成**的，没有第二份。
- **能力判据 = 成员存在性**：`dbg.detach ~= nil`（9x 上不存在）、`dbg.caps().detach`（同一份事实）。
  `inspect(dbg)` 只给人看（表成员带 C 函数，**机器别解析**）。
- 上色只在**真控制台**且没有 `NO_COLOR` 时发生，走 `SetConsoleTextAttribute`（不是 ANSI）；
  管道 / `--json` / server 的 sink 下一律纯文本（不会有 ESC 字节）。

> ❗**冻结**：下面的 29 个 PascalCase 契约名（`Run` / `Attach` / `SetBreakpoint` / `ReadMemory` / …）
> **不改名、不删、不加参数**，行为被门禁逐字节锁定；新脚本请用 `dbg`（会话对象）、`gui`、`open_file`。
> 它们仍然可用，是上面这些便捷面的底层原语。

## Run{ ... }：启动并调试新进程

必需：`imagepath`（或把完整命令行放进 `cmdline`）。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `imagepath` | string | 程序路径 |
| `cmdline` | string? | 命令行（含 imagepath 时可省略 imagepath） |
| `cwd` | string? | 工作目录 |
| `env` | table? | 环境变量 |
| `is_new_console` | boolean? | 是否新开控制台 |
| `use_scylla_hide` | boolean? | 是否启用 ScyllaHide（只有 modern 变体有） |
| `callback` | fun(pid, tid)? | **入口断点**回调（下断点、改内存都在这里做） |
| `exception_callback` | fun(info)? | 异常回调（info.type/address/…） |
| `on_*` | fun(opt)? | 其它事件，见 `lua_api` topic=`events` |

```lua
Run{
  cmdline = [[C:\Windows\System32\notepad.exe]],
  callback = function(pid, tid)
    print("pid", pid.val, "tid", tid.val)
    Detach()            -- 不想继续调就直接分离（目标会继续跑）
  end,
}
```

返回：目标退出/分离后返回，不返回值。

## Attach{ ... }：附加到已运行的进程

```lua
Attach{ pid = 1234, callback = function()
  -- 附加成功后的第一站；在这里下断点
  SetBreakpoint{ modname = "kernel32.dll", rva = FindFuncRva("kernel32.dll", "CreateFileW"),
                 callback = function(info) print("hit"); Detach() end }
end }
```

- 老式写法 `Attach(function() ... end)` 也支持（只有回调）。
- 目标进程会**暂停**在附加点；`callback` 里做完事情后，正常返回 = 继续运行。
- Win9x 不支持 attach（只有 Run），这是系统限制。

## 断点（软件断点 = INT3）

```lua
-- 单个
SetBreakpoint{
  modname = "my.dll",
  rva     = FindFuncRva("my.dll", "SomeFunc"),   -- 或 U32("0x1234")
  is_single = true,                              -- 可选：命中一次后自动删除
  callback = function(info)
    -- info.ea / info.modname / info.rva / info.pid / info.tid
  end,
}

-- 批量（按模块 -> RVA -> 回调）
SetBreakpoints{
  ["my.dll"] = {
    ["0x1234"] = function(info) end,
    ["0x5678"] = function(info) end,
  },
}

DelBreakpoint("my.dll", U32("0x1234"))   -- 或 DelBreakpoint(ea)
EnableBreakpoint("my.dll", U32("0x1234"))
DisableBreakpoint("my.dll", U32("0x1234"))
```

- `rva` 必须是 **LuaVariant**：用 `FindFuncRva(...)` / `U32("0x1234")` / `U64(...)`，
  直接写 `0x1234`（裸 number）会被拒绝。
- 回调是**同步**执行在调试循环里：别在这里做长耗时的事（会卡住目标）。

## 硬件断点（DRx）

```lua
SetHardwareBreakpoint{
  ea       = 0x401000,        -- 绝对地址（LuaVariant/V())
  index    = 0,               -- 可选：DR0..DR3
  type     = "e",             -- e=执行 / w=写入 / r=读写
  size     = 4,               -- 1/2/4（x86）；x64 还能 8
  callback = function(info) end,
}
DelHardwareBreakpoint(0)      -- 按 index 删除
```

硬件断点不需要改目标内存（对付自校验/CRC 的程序有用），但同时最多 4 个。

## 内存读写

```lua
local bytes = ReadMemory(V(addr), U32(64))   -- ❗返回 **table of bytes**（1..n），不是字符串
print("first byte", bytes[1])
print(#bytes)

WriteMemory(V(addr), { 0x90, 0x90, 0x90 })   -- table of bytes，写回目标
```

- `ReadMemory(addr, size)`：**两个参数都必填**，都是 **LuaVariant**（`V(x)` / `U32(x)` / `U64(x)`）。
  失败返回 **`nil, err`**（不可读地址 / 越界都走这条）。
- `addr` 也可以是**寄存器值**：`V(ctx.Ccx)` 就是「把 Ccx 里的值当地址」（见下面的 Var）。
- 要字符串：`local s = string.char(table.unpack(bytes))`；要 hex：`inspect(bytes)` 或自己格式化。
- 大块读取建议分片（每次 ≤ 一张页更稳）：`for i = 0, n-1, 0x1000 do ... end`。
- 内存布局：`GetMemoryRegion()` 返回 `{ BaseAddress, AllocationBase, RegionSize, State, Protect, Type }` 列表。

## LuaVariant 与 Var（读目标内存的两种口味）

### 1) Var：`V(addr)` —— 按「地址 + 类型字母」读

❗**`V()` 只在调试回调里可用**：顶层 / REPL 里没有「当前会话」这个答案，`V()` 返回
`nil, msg`（REPL 里 `V` 干脆是 `nil`）；顶层读内存用会话绑定的 **`d:v(addr)`**。两边的完整
口径与错误文案见上面「V 与 d:v：指针链走查」。

❗`V(x)` 的 `x` 既可以是数字/`U32()`，也可以是 **LuaVariant**（比如 `GetContext` 取到的寄存器）——
意义是「把它的**值当成地址**」。所以读「参数在 ECX 里、参数是 ECX 指向的 int32」就是：

```lua
-- 在回调里（V 可用）
local c   = GetContext(tid)
local arg = V(c.Ccx):to("i").val      -- 读 [RCX] 处的 int32；.val 是**字符串**，.rawval 才是数字
print("arg =", arg)                    -- "7"
print("arg as number =", V(c.Ccx):to("i").rawval)
```

```lua
local v   = V(addr)              -- Var：地址 + 类型字母的读窗口
local ptr = v[0x10]              -- 把 addr+0x10 处的**指针**取出来（返回新的 Var）
local i   = v[0x10]:to("i")      -- 在该指针处读 int32
local ds  = v[0x10]:tos("d", 130)-- 读 130 个 double（LuaVariant 数组）
local u32 = v:to("I", 0x20)      -- 在 addr+0x20 处读 uint32
local s   = v:sto("i", 4, 0x20)  -- 4 个 int32 的字符串表示
v:write(U32(1), 0x20)            -- 写 uint32
v:writes({1,2,3}, 0x20)          -- 批量写
v:swrite("abc", 0x20)            -- 写字节串
```

单字母类型（`Var:*` 用）：

| 字母 | 类型 | 字母 | 类型 |
| --- | --- | --- | --- |
| `c` | int8 | `b` | uint8 |
| `h` | int16 | `H` | uint16 |
| `i` | int32 | `I` | uint32 |
| `q` | int64 | `Q` | uint64 |
| `z` | size_t | `f` | float |
| `d` | double | | |

Var 还有只读字段：`hex`（十六进制串）/ `val`（**字符串**，打印用）/ `rawval`（数字）/ `type` / `bin`（字节表）。

### 2) LuaVariant：`U32(x)` / `I64(x)` / `F64(x)` … —— 值本身

- 构造：`U8 I8 U16 I16 U32 I32 U64 I64 F32 F64`（也接受 `"0x1234"` 字符串）。
- 方法：`:to("u32")` / `:as("i64")`（**全名**：i8/u8/i16/u16/i32/u32/i64/u64/f32/f64）。
- 字段：`.hex`（十六进制串）/ **`.val`（字符串，打印用）** / **`.rawval`（数字）** / `.type` / `.bin`；
  算术/比较元方法都有（`+ - * / % // ^ == < > <= >= .. #`）——❗但拿 `.val` 去算会当成字符串拼接。
- 注意：LuaJIT **没有位运算元方法**——`& | ~ << >>` 在 LuaJIT 下不可用。

## 线程上下文

```lua
local c = GetContext(tid)     -- tid 来自 info.tid / callback 参数
print(c.Cip.hex, c.Cax.rawval) -- 通用寄存器（x86/x64 同名：Cax Cbx Ccx Cdx Csi Cdi Csp Cbp Cip）
print(c.EFlags.rawval, c.Dr7.hex)

c.Cax = U64(0)                -- 修改后写回
SetContext(tid, c)
```

- 字段（x86 23 项）：`ContextFlags SegGs SegFs SegEs SegDs SegCs SegSs EFlags Dr0..Dr3 Dr6 Dr7`
  + `Cax..Cip`。
- x64 另有 37 项：`P1Home..P6Home MxCsr R8..R15 Xmm0..Xmm15 VectorControl DebugControl
  LastBranchToRip LastBranchFromRip LastExceptionToRip LastExceptionFromRip`。
- 取到的每个字段都是 **LuaVariant**：**`.val` 是字符串**（打印用）、`.rawval` 是数字、`.hex` 是十六进制串。

## 模块 / 符号 / 栈

```lua
local base = GetModBase("kernel32.dll")        -- LuaVariant?（模块未加载 -> nil）
local rva  = FindFuncRva("kernel32.dll", "CreateFileW")  -- 导出 RVA（LuaVariant?）
local mod, rva2 = Ea2ModnameRva(0x7FFE1234)    -- 地址 -> 模块名 + RVA
print(LoadStr(addr))                          -- 读宽/窄字符串（自动判断，返回 string?）
print(GetCppClassName(addr))                  -- C++ 对象 -> 类名（读 RTTI，string?）
Hide()                                        -- PEB/Heap 反反调试修补（modern 变体含 ScyllaHide）

for i, fr in ipairs({ StackTrace(tid) }) do    -- 栈帧列表：ea / modname / rva
  print(i, fr.modname, fr.rva)
end
```

- `StackTrace` 依赖 dbghelp；Win9x/无 dbghelp 时退化（`frames = 0`），不报错。

## 特征扫描与内存布局

```lua
-- 在目标内存里找字节特征（返回 addr, hexstr ... ; 具体形态见 GetMemoryRegion）
for addr, s in FindMemory("48 8B 05 ?? ?? ?? ??") do
  print(addr.hex, s)
end

for _, r in ipairs(GetMemoryRegion()) do
  print(r.BaseAddress.hex, r.RegionSize.val, r.Protect.val)
end
```

- 扫描是**逐区域**的，大进程会慢；先用 `GetMemoryRegion` 过滤（只看 `Protect` 可读的区域）。

## 其它调试事件（都在 Run{...} 的选项里）

| 字段 | 回调参数 | 含义 |
| --- | --- | --- |
| `exception_callback` | `info` | 异常（`type/address/nt_status/obj_*`，含 C++ EH 对象） |
| `on_any_debug_event` | `opt` | 任意调试事件（`code` 等） |
| `on_single_step` | `opt` | 单步 |
| `on_create_thread` / `on_exit_thread` | `opt` | 线程创建/退出 |
| `on_create_process` / `on_exit_process` | `opt` | 进程创建/退出 |
| `on_load_dll` / `on_unload_dll` | `opt` | 模块加载/卸载（`on_load_dll` 可用来下“还没加载的模块”的断点） |
| `on_output_debug_string` | `opt` | 目标的 `OutputDebugString` |
| `on_rip` | `opt` | RIP 事件 |
| `on_console_event` | `opt` | 控制台事件 |

```lua
Run{ cmdline = "target.exe", on_load_dll = function(e) print("load", e.modname) end,
     exception_callback = function(e) print("exc", e.type.hex, e.address.hex) end }
```

## gui.* —— GUI 自动化（不需要调试会话也能用）

```lua
local w, h    = gui.size()                          -- 屏幕尺寸
local r, g, b = gui.pixel(0, 0)                     -- 单像素颜色
local shot    = gui.screenshot{ format = "png" }    -- { w=, h=, format=, data=<二进制> }
local small   = gui.screenshot{ hwnd = hwnd, scale = 0.5 }
gui.screenshot_to("C:/tmp/shot.png")                -- 落盘（会进 --json 的 images）

local wins = gui.windows()                          -- { {hwnd=,pid=,title=,class=,x=,y=,w=,h=,visible=} ... }
local hwnd = gui.find{ title = "记事本" }            -- hwnd | nil
gui.active(); gui.activate(hwnd); gui.focus(hwnd)
gui.rect(hwnd); gui.set_rect(hwnd, 0, 0, 800, 600); gui.show(hwnd, "max")

local kids = gui.children(hwnd)                     -- { {hwnd=,class=,text=,id=,x=,y=,w=,h=} ... }
gui.text(hwnd); gui.set_text(hwnd, "x")
gui.post(hwnd, 0x0010, 0, 0)                        -- PostMessage（异步，不阻塞目标）
gui.send(hwnd, 0x000F, 0, 0)                        -- SendMessage（拿返回值）
gui.click_control(hwnd)                             -- 控件中心真实点击

gui.click(100, 200); gui.move(100, 200); gui.write("abc"); gui.press("enter")
gui.hotkey("ctrl", "shift", "esc"); gui.sleep(500)
```

- 坐标是**屏幕绝对像素**；失败返回 `nil, errmsg`（不抛异常）。
- 截图给 AI 看：`print(#shot.data)` 后由 MCP 的 `gui_screenshot` 工具直接回 image/png
  （比在 `lua_eval` 里 base64 更省事）。
- 目标被调试器挂起时窗口不响应输入：先在脚本里让它继续跑，再操作 GUI。

## files —— `open_file` 的文件对象（唯一文件 API，D44/D49）

```text
local f, err = open_file(path [, mode])     -- mode: "r"/"rb"（默认读）/ "w"/"wb" / "a"/"ab"
f:read([fmt])          -- 默认 "l"；"a" / "l" / "n" / 整数 N（读 N 字节）
f:read_line()          -- = f:read("l")
f:lines()              -- for line in f:lines() do ... end
f:seek([whence][,off]) -- "set" / "cur"（默认）/ "end"；返回新位置
f:tell()               -- 逻辑位置
f:setbuf(n)            -- 服务端缓冲（默认 4096，上限 8192）
f:write(...)   f:flush()   f:close()
```

- **本地模式**：等价 `io.open`。
- **remote 模式**（`--remote`）：文件在**客户端**那台机器上；读是阻塞 RPC，回调里也能用。
- 语义（D49）：`"l"` 去行尾、`f:read(N)` 精确 N 字节、EOF → `nil`、错误 → `nil, err`、
  字节不做 CRLF 归一；`f:tell()` 是逻辑位置、`f:seek()` 往返且清缓冲。
- **回调里**：等回包超时 → `nil, "timeout"`（回调返回、断点继续）；被 `--cancel` → `nil, "cancelled"`。

## 已知地雷（务必读）

1. **回调里不能 yield**：`Run`/`Attach` 的回调是引擎经 C 调用进来的，`write_file`/`read_file`
   （以及任何需要挂起的 API）在那里会失败 → 用 `open_file` 流式写。
2. **`ReadMemory` 返回 table（不是字符串）**，`addr`/`size` 必须是 LuaVariant（`V()`/`U32()`）。
3. **`SetBreakpoint` 传 table**（`modname`/`rva`/`callback`），不是两个位置参数。
4. **`Var:*` 用单字母类型**（`i`/`I`/`d`/`q`…），**`LuaVariant:*` 用全名**（`u32`/`f64`…）。
5. **LuaJIT 没有位运算元方法**（`& | ~ << >>`）与 `string.unpack`/整数除法 `//`。
6. `BsonDecode` 是**桩**（会报 `not implement BsonDecode yet!`），只用 `BsonEncode`。
7. 脚本文件带 UTF-8 BOM 时 Lua 5.5 会直接失败（`lua_eval` 的 body 不受影响）。
8. 会话是**串行**的：一个 `Run`/`Attach` 没结束前，其它工具调用会排队（满了 503）。
   想提前收工：`/cancel`（或脚本里 `Detach()`）。
9. `GetContext` 的字段名 x86/x64 同名（`Cax..Cip`），但 x64 才有 `R8..R15`/`Xmm*`。

## 典型场景模板

### A. 抓函数参数（IAT/thunk 处下断点）

```lua
Run{ cmdline = [[C:\path\target.exe]],
  callback = function()
    SetBreakpoint{ modname = "kernel32.dll", rva = FindFuncRva("kernel32.dll", "CreateFileW"),
      callback = function(info)
        local c = GetContext(info.tid)
        print("path =", LoadStr(c.Ccx))             -- x64 第 1 参数 RCX（x86 是栈）；LoadStr 要地址
        Detach()
      end }
  end }
```

### B. 统计某函数被调用多少次 / 每次的计数

```lua
local n = 0
Run{ cmdline = "target.exe", callback = function()
  SetBreakpoint{ modname = "my.dll", rva = FindFuncRva("my.dll", "F"),
    callback = function() n = n + 1 end }
end }
print("hits =", n)
```

### C. 把大块内存 dump 到客户端文件（流式，内存恒定）

```lua
local f = open_file("Z:/dump.bin", "wb")
Run{ cmdline = "target.exe", callback = function()
  SetBreakpoint{ modname = "my.dll", rva = FindFuncRva("my.dll", "F"),
    callback = function(info)
      local c = GetContext(info.tid)
      local n = V(c.Ccx):to("I")
      f:write(BsonEncode{ vols = V(GetModBase("my.dll"))[0x56C5C0]:tos("d", 130 * (n + 1)) })
      Detach()
    end }
end }
f:close()
```

### D. 看界面 + 点按钮（AI 操作的典型循环）

```lua
local h = gui.find{ title = "Target" }
print("hwnd", h and h.val)
if h then
  gui.activate(h)
  local shot = gui.screenshot{ hwnd = h, format = "png" }
  print("shot bytes", #shot.data)          -- 或直接用 MCP 工具 gui_screenshot
  gui.click(400, 300)                      -- 屏幕上该控件的绝对坐标
  gui.sleep(300)
end
```

