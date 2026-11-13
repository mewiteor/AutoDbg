# Recipe：32 位宿主调 64 位目标（`dbg.remote`）

场景：手上的 AutoDbg 是 **32 位**（`AutoDbg_86.exe` —— Win9x / 老机器上只有它），而要调的目标是
**64 位**进程。位数是唯一绕不过去的硬理由（D56）：32 位调试器 `DebugActiveProcess` 附 64 位目标
拿到的是 `ERROR_NOT_SUPPORTED`，x64 模块的 `parse_pe` 也处理不了 32 位目标。所以只能**两个进程**：

- 位数够的那一侧起一个 **64 位** AutoDbg，用 `--server=tcp --repl` 挂在那儿（它才调得动 64 位目标）；
- 32 位这一侧的脚本用 `dbg.remote` 连过去，拿到一个**代理对象**（方法面与本地 session 相同），
  `pump` / `context` / `read` / `kill` 照本地写法用。

## 1. 先起远端（管位数的那一侧）

```powershell
# 终端 A：64 位 server + REPL。--repl 是硬要求：代理靠「连接内 lua_State 跨命令存活」把
# 一次 create + 之后几十次方法调用串到同一个 state 上；裸 --server 每条命令都是新 state。
$port = 18642
$tok  = 'c0ffee1234abcd02'          # 显式给了 token 就连 loopback 也校验
$srv = Start-Process -FilePath .\.out\AutoDbg_64.exe `
    -ArgumentList @('--server=tcp', '--repl', "--server-port=$port", "--server-token=$tok", '--server-no-echo') `
    -PassThru -NoNewWindow

# 等监听就绪（连一下再关；最多 ~5 s）
for ($i = 0; $i -lt 50; $i++) {
    try { $tc = New-Object System.Net.Sockets.TcpClient; $tc.Connect('127.0.0.1', $port); $tc.Close(); break }
    catch { Start-Sleep -Milliseconds 100 }
}
```

收尾别忘 `Stop-Process -Id $srv.Id -Force` —— 它可能还抱着被调试的 64 位进程。

## 2. 32 位这一侧：连上去、起目标、看现场

```lua
-- cross_arch.lua（用 AutoDbg_86.exe 跑）
local PORT  = 18642
local TOKEN = 'c0ffee1234abcd02'

local h, herr = dbg.remote('127.0.0.1:' .. PORT, { token = TOKEN, timeout_ms = 20000 })
if not h then error(herr) end                    -- 连不上 / 错 token：nil, 'dbg.remote: …'
print('connected')

-- 在远端 create 一个**真正的 64 位**目标（32 位宿主自己 CreateProcess 只会拿到 SysWOW64 那个）
local d, cerr = h:create{
  imagepath = [[C:\Windows\System32\cmd.exe]],   -- dbg.create 的 imagepath 必填，代理同参
  cmdline   = '/c ping -n 20 127.0.0.1',         -- 活 20 秒左右，够我们看完现场
}
if not d then h:close(); error(cerr) end
print('remote root_pid', d.root_pid)             -- 这个 pid 在**远端**那个进程里

-- 第一条事件通常是 CREATE_PROCESS（type = 3）；有界轮数，别写死循环
local ev
for _ = 1, 3 do
  ev = d:pump(4000)
  if ev then break end
end
if not ev then d:kill(); h:close(); error('no event from the remote session') end
print('ev', ev.type, ev.pid, ev.tid)

-- 停住时读寄存器：键名与本地完全一致（Cax … Cip），event.session 已填回这个代理对象
print(string.format('Cip=%X', d:context().Cip))

-- 数据面：模块表 + 内存。地址过线走 __i（x64 基址 15 位十进制，否则 jsonx 会丢低位），
-- 字节过线走 __hex —— 两种标记代理已自动处理，脚本拿到的就是普通整数 / 字节串。
local mods, merr = d:mods()
if not mods or not mods[1] then d:kill(); h:close(); error(merr or 'no modules') end
local m = mods[1]
local head = d:read(m.base, 4)
print(m.name, string.format('base=%X', m.base), 'head_len=' .. tostring(head and #head))
print('is MZ:', head ~= nil and #head == 4 and string.byte(head, 1) == 0x4D)
print('u16 as number:', type(d:read_u16(m.base)) == 'number')

d:cont(ev.cont_code)                             -- 应答码用 cont_code，不是 ev.code
print('kill', d:kill())                          -- 收尾动词仍是 kill / detach（D51）
print('close', h:close())                        -- 只关连接；目标已 kill
```

## 3. 目标已经在跑：用 `h:attach(pid)`

```lua
-- pid 由远端给出（远端机的任务管理器 / 远端脚本 print 出来的）；这里写死一个例子
local h = dbg.remote('127.0.0.1:18642', { token = 'c0ffee1234abcd02', timeout_ms = 20000 })
if not h then return { error = 'connect-failed' } end

local d, err = h:attach(4242)                    -- 远端的 dbg.attach(pid)
if not d then h:close(); return { error = 'attach-failed', detail = tostring(err) } end

local ev = d:pump(4000)                          -- 附加后的第一站（通常是 CREATE_PROCESS / LOAD_DLL）
if ev then
  print(ev.type, ev.pid, ev.tid)
  d:cont(ev.cont_code)
end
print('detach', d:detach())                      -- 远端是 NT+ 才有；9x 上要 d:kill()
print('close', h:close())
```

## 要点

- **远端必须 `--repl --server`**：`dbg.remote` 的每一次方法调用 = 一条 `script` 帧
  （`return dbglib.remote_call(...)`）；没有 `--repl` 的 server 每条命令都是新 `lua_State`，
  上一条命令建的会话在下一帧就不存在。
- **显式 `--server-token=` 时 loopback 也校验**，客户端的 `opts.token` 必须同值；两边都不给才免校验。
- **`ev.session` 是代理对象**，所以 `ev.session:cont(ev.cont_code)` 在本地 / 远端写法一致。
  事件表是普通表，字段与本地同一份（`type` / `pid` / `tid` / `ea` / `modname` / `rva` / `code` /
  `cont_code` / `first_chance`）。
- **回调过不了线**：`d:arm(mod, rva, cb)` 在代理上必失败
  （`nil, 'dbg.remote: argument cannot cross the wire (function/userdata/Nan)'`）。断点只能在
  **远端**那个 state 里武装；代理这侧负责 `pump` / `cont` / 数据面 / 收尾。
- **单条命令 4096 字节上限**（`--repl` 的 chunk 上限）⇒ `d:write` 一次别超过约 **1.8 KB**
  （字节过线是十六进制，一个字节两个字符 + 命令本身的开销）。超了按语法错（`3`）失败。
- **`open_file` 在代理路径上不支持**：代理连接把 `open` / `data` / `read_req` / `close` 帧一律丢弃，
  远端那条 `open_file` 只会等到超时。
- **能力问远端**：`d:caps()` 报的是远端那台机器 / 那个 build；代理对象的 `d.detach` 永远是函数
  （薄转发），远端是 9x 时调用才报 `method 'detach' is not available on this host`。
- **代理不进本地 registry**：`dbg.sessions()` / `dbg.session(pid)` 看不到它；远端会话用
  `h:sessions()` 查（只给 `root_pid` 列表）。
- 仓库里可跑的等价物：`tools/server_test/e2e_dbg.ps1` 的 `remote.create` / `remote.pump-context` /
  `remote.data-plane` / `remote.teardown` 四条（x86 宿主 + x64 server；断言 `d:read(m.base, 4)` 读到
  `MZ`、`d:read_u16` 是 number、`Cip > 0`、`kill` 之后那个 64 位进程真的没了）。
