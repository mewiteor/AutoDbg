-- =============================================================================
-- lualib/dbglib.lua —— 多会话脚本糖：rendezvous（wait_all）与 batch/collect（run_all）
--
-- 文件职责：把 plan_t7.md §11.9.2（D58）三种模型里的两种做成一个库，按文件名烘焙成
--   全局 `dbglib`（tools/gen_lua_lib.lua 的约定，见 AGENTS.md §2），外加**远端会话代理**
--   （T7 切片 C-remote / D56：服务端分发器 `remote_call` + 客户端 `remote`）。
--   只依赖运行时全局 `dbg` / `jsonx`；传输原语是 C 侧的 `dbg.remote_open/remote_xfer/remote_close`
--   （复用 `--remote` 客户端的连接与帧，不写第二套 TCP）。
-- 来源：新写（T7.6）。原型是 plan_t7.md §11.9.2 的「推荐脚本骨架」—— 同一段手写循环在
--   tools/server_test/e2e_dbg.ps1 的 session.multi-two-stopped 里也有一份。
-- 与旧实现的差异：无旧实现（之前多会话脚本只能各写一遍 pump 循环）。
--
-- 硬约束：
--   - **Lua 5.1 语法**：LuaJIT 变体要用自己的解释器编译这一份（字节码按 flavor 分别烘焙）
--     ⇒ 不用 goto / 位运算 / `//` / table.pack。
--   - `dbg` 在**调用时**取，不在本文件顶层抓（PITFALLS.md L4：库按文件名顺序烘焙，
--     顶层拿到的可能是 nil）。
--   - 两个函数都**不抛异常**：一切失败返回 `nil, err`（与 `dbg` 的既定口径一致）。
--
-- 三种模型（D58）别混 —— 区别是「停在哪里」：
--   batch      dbglib.run_all     autocont（record-and-continue）跑到各自结束/超时，回调往
--                                 upvalue 表里记，最后统一比较。❗**不能在命中处检查**
--                                 （引擎在 dispatch 末尾就应答了，没有「停住」这回事）。
--   rendezvous dbglib.wait_all    两边**同时停住**，此时才能读 context / 栈。
--   step       REPL（dbg.pump(200) / d:pump() 一行一行）—— 不在本文件里。
-- =============================================================================

local dbglib = { _version = '0.1.0' }

-- 缺省值：8 秒上界、200 ms 一泵（与 plan_t7.md §11.9.2 的骨架一致）
local DEFAULT_TIMEOUT_MS = 8000
local DEFAULT_PUMP_MS    = 200

-- 迭代上界 = ceil(timeout_ms / pump_ms)，至少一轮。
-- ❗不用 os.clock / os.time：两个 Lua 版本语义不一（LuaJIT 的 os.clock 是 CPU 时间），
--   且 os.time 只有秒级 —— 上界必须可复现，否则门禁会飘。
local function max_rounds(timeout_ms, pump_ms)
    local n = math.ceil(timeout_ms / pump_ms)
    if n < 1 then n = 1 end
    return n
end

-- opts → { timeout_ms = <n>, pump_ms = <n> } | nil, err
local function read_opts(name, opts)
    local o = { timeout_ms = DEFAULT_TIMEOUT_MS, pump_ms = DEFAULT_PUMP_MS }
    if opts == nil then return o end
    if type(opts) ~= 'table' then return nil, name .. ': options table expected' end
    if opts.timeout_ms ~= nil then
        if type(opts.timeout_ms) ~= 'number' or opts.timeout_ms <= 0 then
            return nil, name .. ': timeout_ms (positive number) expected'
        end
        o.timeout_ms = opts.timeout_ms
    end
    if opts.pump_ms ~= nil then
        if type(opts.pump_ms) ~= 'number' or opts.pump_ms <= 0 then
            return nil, name .. ': pump_ms (positive number) expected'
        end
        o.pump_ms = opts.pump_ms
    end
    return o
end

-- 给了 `mod` 才比较 modname。事件表的 modname 与 d:mods() 的 name 是同一份
-- （PEInfo->Path + name_offset ⇒ 只有文件名），Windows 的文件名大小写不敏感 ⇒ 统一降小写比。
local function same_module(want, got)
    if want == nil then return true end
    if type(got) ~= 'string' then return false end
    return string.lower(want) == string.lower(got)
end

-- 失败路径的收尾：已经建起来的会话不能留在 registry 里（目标还在跑）。
-- 单个 kill 失败也继续 —— 这里只求「尽量不留活会话」。
local function kill_all(sessions)
    for i = 1, #sessions do
        sessions[i]:kill()
    end
end

-- 全停 = 每个会话都走到了 run 循环的出口（目标退出 / 被杀）。
-- `d:stopped()` 是 `cont()` 的取反（core 在 EXIT_PROCESS 时把它置假），所以它才是
-- 「这一轮该停」的判据，不要自己拿 d.stop 猜。
local function all_stopped(sessions)
    for i = 1, #sessions do
        if sessions[i]:stopped() ~= true then return false end
    end
    return true
end

-- =============================================================================
-- dbglib.wait_all(goals [, opts]) -> true | nil, err            （rendezvous，D58）
--
-- goals：数组，每项 { sess = <session 对象>, mod = 'x.exe', rva = 0x1028 }
--        `sess` 必填；`mod` 可省（只比 rva）⇒ 与目标断点对齐的比较口径是
--        「同一个会话 + RVA 相同 + 模块名相同（给了 mod 才算）」。
-- opts ：{ timeout_ms = 8000, pump_ms = 200 }
--
-- 循环 `dbg.pump`（**进程级**：别人的事件由它照常分派，见 D57 的不变量 1）：
--   · 命中某会话**未达成**的目标 ⇒ 不 cont（保持停住 = 达成，这正是 rendezvous 的语义）
--   · 其它事件                 ⇒ 用 core 算好的 `cont_code` 放行（别自己猜 DBG_CONTINUE）
--   · 某会话 `stopped()` 而目标还没达成 ⇒ 目标提前退出 / 被杀，等不到了
-- 上界是**迭代次数** ceil(timeout_ms / pump_ms)，不是墙钟。
-- =============================================================================
function dbglib.wait_all(goals, opts)
    if type(goals) ~= 'table' then return nil, 'dbglib.wait_all: goals (array) expected' end
    if #goals < 1 then return nil, 'dbglib.wait_all: goals is empty' end

    local o, oerr = read_opts('dbglib.wait_all', opts)
    if o == nil then return nil, oerr end

    -- 先整表校验：半个不合法的目标表留到循环里才炸是没法定位的
    local done = {}   -- 目标下标 → 是否已达成
    for i = 1, #goals do
        local g = goals[i]
        if type(g) ~= 'table' then
            return nil, 'dbglib.wait_all: goal #' .. i .. ' (table) expected'
        end
        if type(g.sess) ~= 'userdata' then
            return nil, 'dbglib.wait_all: goal #' .. i .. '.sess (session) expected'
        end
        if type(g.rva) ~= 'number' then
            return nil, 'dbglib.wait_all: goal #' .. i .. '.rva (number) expected'
        end
        if g.mod ~= nil and type(g.mod) ~= 'string' then
            return nil, 'dbglib.wait_all: goal #' .. i .. '.mod (string) expected'
        end
        done[i] = false
    end

    local pending = #goals
    local rounds  = max_rounds(o.timeout_ms, o.pump_ms)

    for _ = 1, rounds do
        if pending == 0 then return true end

        -- 「目标提前退出」的双判。`stopped()` 报错（会话已被收尾）也是等不到目标了。
        for i = 1, #goals do
            if not done[i] then
                local st, serr = goals[i].sess:stopped()
                if st == nil then return nil, 'dbglib.wait_all: ' .. tostring(serr) end
                if st then return nil, 'dbglib.wait_all: session exited before its goal' end
            end
        end

        local ev, perr = dbg.pump(o.pump_ms, false)
        if ev == nil then
            -- `perr` 非空 = 引擎错误 / 取消守卫（`dbg.pump` 只给 nil 表示「这一泵没等到」）
            if perr ~= nil then return nil, 'dbglib.wait_all: ' .. tostring(perr) end
        else
            local hit = false
            for i = 1, #goals do
                local g = goals[i]
                if not done[i] and g.sess == ev.session and g.rva == ev.rva and
                   same_module(g.mod, ev.modname) then
                    done[i]    = true
                    pending    = pending - 1
                    hit        = true
                end
            end
            if not hit then
                local cok, cerr = ev.session:cont(ev.cont_code)
                if not cok then return nil, 'dbglib.wait_all: ' .. tostring(cerr) end
            end
        end
    end

    if pending == 0 then return true end
    return nil, 'dbglib.wait_all: timeout'
end

-- =============================================================================
-- dbglib.run_all(params_list [, opts]) -> results [, 'timeout'] | nil, err   （batch，D58）
--
-- params_list：数组，每项就是一个 `dbg.create{...}` 的选项表（可自带 `on_breakpoint` /
--   `on_exit_process` 回调；回调里用脚本的 **upvalue** 收集结果 —— 别用全局，D58：全局会撞
--   29 个契约名与烘焙的 inspect/jsonx/moses，数据流在源码里也看不见）。
-- opts       ：{ timeout_ms = 8000, pump_ms = 200 }
--
-- 逐个 `dbg.create`（任一失败 ⇒ 把已建的都 kill 掉，再 `nil, err`），然后
-- `dbg.pump(pump_ms, true)` **autocont** 到所有会话 stopped()。
-- ❗autocont ⇒ 引擎在 dispatch 末尾就应答了，**不能在命中处检查**；要「两边同时停住」
--   用 dbglib.wait_all。
--
-- 返回每会话一张结果表 `{ root_pid =, ok =, exit =, hits = }` 的数组：
--   root_pid 会话的树根进程
--   ok       收尾（d:kill）是否成功（false = 那次 kill 失败，其余会话照常收）
--   exit     收到的 EXIT_PROCESS 退出码；没收到（超时 / 被杀）就是 nil
--   hits     该会话这一轮记录到的命中条数（`#d:hits()`，在 kill 之前取）
-- 超时**不吞**：返回已经收集到的结果 + 第二个返回值 'dbglib.run_all: timeout'。
-- =============================================================================
function dbglib.run_all(params_list, opts)
    if type(params_list) ~= 'table' then
        return nil, 'dbglib.run_all: params_list (array) expected'
    end

    local o, oerr = read_opts('dbglib.run_all', opts)
    if o == nil then return nil, oerr end

    local sessions = {}   -- 下标 → session 对象
    local results  = {}   -- 下标 → 结果表（活的，最后原地补齐）
    local exits    = {}   -- 下标 → on_exit_process 的 code（没收到就是 nil）

    for i = 1, #params_list do
        local params = params_list[i]
        if type(params) ~= 'table' then
            kill_all(sessions)
            return nil, 'dbglib.run_all: params #' .. i .. ' (table) expected'
        end

        local idx = i   -- ❗闭包要抓每轮各自的副本（for 变量的捕获行为两版不完全一致）
        local merged = {}
        for k, v in pairs(params) do merged[k] = v end
        -- 包一层 on_exit_process：`exit` 只能从这条通道拿到（`stopped()` 只说「该停」）。
        -- 先记下 code，再原样交给调用方的回调（用户不给就只记）。
        local user_exit = merged.on_exit_process
        merged.on_exit_process = function(ev)
            exits[idx] = ev.code
            if type(user_exit) == 'function' then user_exit(ev) end
        end

        local d, cerr = dbg.create(merged)
        if not d then
            kill_all(sessions)
            return nil, 'dbglib.run_all: create #' .. i .. ': ' .. tostring(cerr)
        end
        sessions[i] = d
        results[i]  = { root_pid = d.root_pid, ok = true, exit = nil, hits = 0 }
    end

    local rounds = max_rounds(o.timeout_ms, o.pump_ms)
    for _ = 1, rounds do
        if all_stopped(sessions) then break end
        local ev, perr = dbg.pump(o.pump_ms, true)
        if ev == nil and perr ~= nil then
            kill_all(sessions)
            return nil, 'dbglib.run_all: ' .. tostring(perr)
        end
    end
    local timed_out = not all_stopped(sessions)

    for i = 1, #sessions do
        local d = sessions[i]
        results[i].exit = exits[i]
        results[i].hits = #d:hits()          -- ❗kill 之前取（收尾会把会话状态清掉）
        local kok = d:kill()
        if kok ~= true then results[i].ok = false end
    end

    if timed_out then return results, 'dbglib.run_all: timeout' end
    return results
end

-- =============================================================================
-- 远端会话代理（T7 切片 C-remote，plan_t7.md §10.1 / D56）—— 跨位数 / 强隔离的唯一形态
--
-- **为什么要代理**：位数是唯一绕过不过去的硬理由（32 位调试器 `DebugActiveProcess` 附 64 位目标
--   会拿到 ERROR_NOT_SUPPORTED；x64 模块的 `parse_pe` 也处理不了 32 位目标）⇒ 「同一件事的 32 / 64
--   两种实现」只能靠**两个进程**。形态照 D56：代理对象的方法面与本地 session **相同**，所以脚本不用
--   分成两套写；`pump` 有时是 OS wait、有时是 socket wait。
--
-- 两个方向都在本文件里（同一份代码跑在两端，靠**方法名**区分）：
--   服务端 `dbglib.remote_call(pid, method, args_json)` —— 跑在 `--repl --server` 那个 state 里，
--     把参数 JSON 解包 → 按 method 调 `dbg.session(pid)` / `dbg.last()` 的方法 → 结果包成
--     `{"ok":true,"value":…}` / `{"ok":false,"err":"…"}` 的 **JSON 字符串** 返回。
--     返回字符串（而不是 nil/表）是刻意的：客户端能可靠区分「nil 结果」与「出错」。
--   客户端 `dbglib.remote(addr[, opts])` —— 每条方法调用 = **一条 `script` 帧**，body 就是
--     `return dbglib.remote_call(<pid>, "<method>", "<args json>")`；从 `result` 帧的 `json.value`
--     （协议面唯一增量，见 `tcp_server.cpp`）里取回那个 JSON 字符串再 `jsonx.decode`。
--   传输原语在 C（`dbg.remote_open` / `remote_xfer` / `remote_close`，复用了 `client.cpp` 的
--     `--remote` 连接/发帧/收帧 —— 不写第二套 TCP）；**JSON 编解码留在 Lua**（D28：C 侧不写 parser）。
--
-- 过线的三种标记（两端共用 `encode_value` / `decode_value`）：
--   `{ __hex = "B8444250…" }`  字节串。`read` / `read_str` 的**返回值**、`write` 的**第二个参数**。
--                             JSON 承载不了任意字节（既不是 UTF-8，也可能含 NUL）。
--   `{ __u64 = "140699000000000" }` **LuaVariant**（地址类值：`ev.ea` / `mods().base` /
--                             `read_u64` / `context()` 的 UPTR 字段…，见 dbg_bindings.cpp 的类型口径）。
--                             取 `x.val`（**十进制串**）过线，解码侧用 `U64(那个串)` 还原 —— `U64(str)`
--                             走 strtoull，**不经过 Lua 的 double**，所以 x64 的 15 位地址一位都不丢。
--                             （`tostring` 对大值给 hex、小值反而给十进制，两种形态混在一起，
--                             不能当载荷格式；`x.val` 则一律十进制。）
--   `{ __i = "140699000000000" }`  ≥1e14 的**普通 number**。`jsonx.encode` 的数字是 `%.14g`（14 位
--                             有效数字），number 过线会在那里丢掉低位 ⇒ 十进制串过线再 `tonumber`。
--                             C1c 起地址类值不再走这条路（它们本来就是 LuaVariant，走 `__u64`）；
--                             这一支只给真的传了 number 的场景用，**解码**保留为兼容旧对端。
--
-- **能力边界**（写进注释就是契约，不是待办项）：
--   · 函数 / userdata / session 对象**不能过线**（回调、`d:bp(..., cb)` 之类的参数得留在本机）
--     ⇒ 服务端包成 `{"ok":false,"err":"unserializable"}`，客户端返回 `nil, err`；
--   · 每次调用的请求体是一条 REPL 命令 ⇒ 受 `--repl` 的 **4096 字节 chunk 上限**约束
--     （`d:write` 一次写 ±1.8 KB 以上就会以语法错 3 失败）；
--   · 服务端 REPL 会把返回值 print 进 `out` 帧（`--repl` 本就如此）⇒ 客户端直接丢弃；
--     大 payload 因此会多跑一趟 socket（功能无影响）；
--   · `open_file` 的帧在代理路径上**不支持**（客户端没有本地文件后端，服务端会等到超时）。
-- =============================================================================

-- Lua 5.1（LuaJIT）里 `table.unpack` 不存在、`unpack` 是全局；5.5 反过来。两个都认。
-- （本文件必须保持 5.1 语法，所以这里用 `or` 而不是条件编译手段。）
local unpack = table.unpack or unpack

-- 会话方法白名单（**不含** create / attach / sessions：那三个与 pid 无关，走宿主级分支；
-- 也不含任何回调形参的方法 —— 函数过不了线，这是 D56 的能力边界）。
local SESSION_METHODS = {
    pump = true, cont = true, step = true, eip = true, arm = true,
    hits = true, dropped = true, setcap = true, stopped = true, paused = true, caps = true,
    detach = true, kill = true,
    read = true, read_u8 = true, read_u16 = true, read_u32 = true, read_u64 = true, read_str = true,
    write = true, context = true, stack = true, mods = true, threads = true,
}

-- 返回值是**字节**的方法（服务端方向：⇒ `__hex`）
local BYTE_RESULTS = { read = true, read_str = true }
-- 第二个参数是**字节**的方法（客户端方向：`d:write(addr, data)` 的 data）
local BYTE_ARG2 = { write = true }

local HEXDIGITS = '0123456789ABCDEF'

local function hex_encode(s)
    local out = {}
    for i = 1, #s do
        local b  = string.byte(s, i)
        local hi = math.floor(b / 16) + 1
        local lo = b % 16 + 1
        out[i] = string.sub(HEXDIGITS, hi, hi) .. string.sub(HEXDIGITS, lo, lo)
    end
    return table.concat(out)
end

local function hex_decode(s)
    local out = {}
    if #s % 2 ~= 0 then return nil end
    for i = 1, #s, 2 do
        local hi = tonumber(string.sub(s, i, i), 16)
        local lo = tonumber(string.sub(s, i + 1, i + 1), 16)
        if hi == nil or lo == nil then return nil end
        out[#out + 1] = string.char(hi * 16 + lo)
    end
    return table.concat(out)
end

-- LuaVariant 的识别：比**元表**（拿一份 `U64(0)` 当探针），不猜字段 —— `d:read` 的返回是 string、
-- session / conn 也是 userdata，只有 LuaVariant 的元表带 `__add`/`__eq`/`val` 那套。
-- `U64` 是 C 侧 `lv_init` 注册的全局（state 建起来就有），与「`dbg` 要调用时取」不是同一回事。
local lv_meta = nil
local function variant_decimal(v)
    if lv_meta == nil then lv_meta = getmetatable(U64(0)) end
    if getmetatable(v) ~= lv_meta then return nil end
    return v.val          -- `.val` 是**十进制**串（`tostring` 是 hex，且小值会变十进制）
end

-- 值 → 可 JSON 化的值（`bytes` = 这一层的字符串是字节载荷）。
-- ❗编不了的对象（function / 非 LuaVariant 的 userdata / thread / NaN / inf）一律
--   `error('unserializable')` —— 调用方 pcall 接住并转成 `{"ok":false,...}`，不把 Lua 错误抛回去。
local function encode_value(v, bytes)
    local t = type(v)
    if t == 'nil' or t == 'boolean' then return v end
    if t == 'number' then
        if v ~= v or v >= math.huge or v <= -math.huge then error('unserializable', 0) end
        -- `math.floor` 对负数也是向下取整；整数且 |v| ≥ 1e14 才需要 `__i`（见文件头注释）
        if v == math.floor(v) and (v >= 1e14 or v <= -1e14) then
            return { __i = string.format('%.0f', v) }
        end
        return v
    end
    if t == 'string' then
        if bytes then return { __hex = hex_encode(v) } end
        return v
    end
    if t == 'userdata' then
        local d = variant_decimal(v)
        if d == nil then error('unserializable', 0) end
        return { __u64 = d }
    end
    if t == 'table' then
        local r = {}
        for k, x in pairs(v) do r[k] = encode_value(x, false) end
        return r
    end
    error('unserializable', 0)
end

-- 反序列化：把 `__hex` / `__u64` / `__i` / `__nil` 标记还原（两端共用；`__nil` = 显式占位的 nil 参数）。
local function decode_value(v)
    if type(v) ~= 'table' then return v end
    if type(v.__hex) == 'string' then
        local s = hex_decode(v.__hex)
        if s == nil then error('bad __hex marker', 0) end
        return s
    end
    if type(v.__u64) == 'string' then
        -- ❗拿**串**造变体（`U64("140699…")` 走 strtoull）：不经过 Lua 的 double，> 2^53 也精确
        return U64(v.__u64)
    end
    if type(v.__i) == 'string' then
        local n = tonumber(v.__i)
        if n == nil then error('bad __i marker', 0) end
        return n
    end
    if v.__nil == true then return nil end
    local r = {}
    for k, x in pairs(v) do r[k] = decode_value(x) end
    return r
end

-- Lua 源串字面量（**不用 `%q`**：Lua 5.4+ 的 `%q` 会吐 `\xNN`，Lua 5.1 不认；
-- 而客户端与服务端可能是不同版本 —— 这里只用手写转义，5.1–5.5 逐字一致）。
local function lua_quote(s)
    local out = { '"' }
    for i = 1, #s do
        local b  = string.byte(s, i)
        local ch = string.sub(s, i, i)
        if ch == '\\' or ch == '"' then
            out[#out + 1] = '\\' .. ch
        elseif b == 10 then
            out[#out + 1] = '\\n'
        elseif b == 13 then
            out[#out + 1] = '\\r'
        elseif b < 32 or b == 127 then
            out[#out + 1] = string.format('\\%03d', b)
        else
            out[#out + 1] = ch
        end
    end
    out[#out + 1] = '"'
    return table.concat(out)
end

-- =============================================================================
-- 服务端：dbglib.remote_call(pid, method, args_json) -> JSON 字符串
-- =============================================================================

-- 宿主级方法（与 pid 无关）。`sessions` 只回 root_pid 列表：session 对象是 userdata，过不了线。
local function host_method(method, args)
    if method == 'create' then
        local d, err = dbg.create(args[1])
        if not d then error(tostring(err), 0) end
        return d.root_pid
    end
    if method == 'attach' then
        local d, err = dbg.attach(args[1])
        if not d then error(tostring(err), 0) end
        return d.root_pid
    end
    -- 'sessions'
    local out = {}
    local ss  = dbg.sessions()
    for i = 1, #ss do out[i] = ss[i].root_pid end
    return out
end

-- 返回 (value, is_bytes)；协议级/参数级错误用 error()（被 remote_call 的 pcall 收成 err）。
local function remote_dispatch(pid, method, args_json)
    local args = {}
    if type(args_json) == 'string' and #args_json > 0 then
        args = jsonx.decode(args_json)
        if type(args) ~= 'table' then error('args must be a JSON array', 0) end
        args = decode_value(args)
        if type(args) ~= 'table' then args = {} end
    end

    if method == 'create' or method == 'attach' or method == 'sessions' then
        return host_method(method, args), false
    end
    if not SESSION_METHODS[method] then
        error("unknown method '" .. tostring(method) .. "'", 0)
    end

    local d
    if pid == 0 or pid == nil then
        d = dbg.last()
        if not d then error('no session (call h:create / h:attach first)', 0) end
    else
        d = dbg.session(pid)
        if not d then error('no session ' .. tostring(pid), 0) end
    end
    if type(d[method]) ~= 'function' then
        -- 能力缺席（D51：9x 上 `dbg.detach` 根本不存在）—— 这是事实，不是错误回退
        error("method '" .. method .. "' is not available on this host", 0)
    end

    -- `unpack(args, 1, #args)`：`#args` 丢掉了尾部 nil（`d:context(nil)` 与 `d:context()` 等价）
    local r = { d[method](d, unpack(args, 1, #args)) }
    if r[1] == nil and r[2] ~= nil then error(tostring(r[2]), 0) end

    if method == 'pump' and type(r[1]) == 'table' then
        -- 事件表里的 `session` 是**本进程的**会话句柄（userdata）—— 过不了线；客户端会把自己
        -- 那个代理对象填回 `ev.session`（脚本写法因此与本地一致）。其余字段直接过（全是标量）。
        local ev = {}
        for k, x in pairs(r[1]) do
            if k ~= 'session' then ev[k] = x end
        end
        return ev, false
    end
    return r[1], BYTE_RESULTS[method] == true
end

-- 永远返回一个 JSON 字符串（出错也是）——客户端靠 `ok` 区分「nil 结果」与「出错」。
function dbglib.remote_call(pid, method, args_json)
    if type(pid) ~= 'number' then return '{"ok":false,"err":"pid (number) expected"}' end
    if type(method) ~= 'string' then return '{"ok":false,"err":"method (string) expected"}' end

    local ok, value, is_bytes = pcall(remote_dispatch, pid, method, args_json)
    if not ok then
        local eok, quoted = pcall(jsonx.encode, tostring(value))
        if not eok or type(quoted) ~= 'string' then quoted = '"remote call failed"' end
        return '{"ok":false,"err":' .. quoted .. '}'
    end

    local eok, encoded = pcall(jsonx.encode, encode_value(value, is_bytes == true))
    if not eok or type(encoded) ~= 'string' then
        -- 函数 / userdata / 稀疏表 / 循环引用：都是「过不了线」，不细分原因（见文件头的能力边界）
        return '{"ok":false,"err":"unserializable"}'
    end
    return '{"ok":true,"value":' .. encoded .. '}'
end

-- =============================================================================
-- 客户端：dbg.remote(addr[, opts]) -> RemoteHost | RemoteSession
-- =============================================================================

local RemoteSession = {}
RemoteSession.__index = RemoteSession

local RemoteHost = {}
RemoteHost.__index = RemoteHost

-- addr = 'host:port'（IPv4 文本或 localhost；不做 DNS）
local function split_addr(addr)
    if type(addr) ~= 'string' then return nil, 'dbglib.remote: addr ("host:port") expected' end
    local i = string.find(addr, ':', 1, true)
    if i == nil or i == 1 then return nil, "dbglib.remote: addr must be 'host:port'" end
    local port = tonumber(string.sub(addr, i + 1))
    if port == nil or port <= 0 or port > 65535 then
        return nil, "dbglib.remote: addr must be 'host:port'"
    end
    return string.sub(addr, 1, i - 1), port
end

-- 参数表 → JSON 数组文本（nil 参数用 `{"__nil":true}` 占位，否则 jsonx 会把它当稀疏数组拒掉）
local function encode_args(method, ...)
    local n    = select('#', ...)
    local args = {}
    for i = 1, n do
        local v = select(i, ...)
        if v == nil then
            args[i] = { __nil = true }
        else
            local bytes = (i == 2 and BYTE_ARG2[method] == true and type(v) == 'string')
            args[i] = encode_value(v, bytes)
        end
    end
    return jsonx.encode(args)
end

-- 一次远程调用：`pid` = 0 → 服务端取 `dbg.last()`；返回方法的值，失败 `nil, err`（**不抛异常**）。
local function remote_exchange(conn, opts, pid, method, args_json)
    local body = 'return dbglib.remote_call(' .. string.format('%d', pid) .. ', ' .. lua_quote(method) .. ', ' ..
                 lua_quote(args_json) .. ')'

    local rj, xerr = dbg.remote_xfer(conn, body, opts.timeout_ms)
    if rj == nil then return nil, 'dbg.remote: ' .. tostring(xerr) end

    -- 两层解码：① `result` 帧的 json（`{ok, exit, …, value}`）；② `value` 里那个回复串
    local fok, frame = pcall(jsonx.decode, rj)
    if not fok or type(frame) ~= 'table' then return nil, 'dbg.remote: bad result frame' end
    if frame.ok ~= true then
        local why = frame.error
        if why == nil or why == '' then why = 'exit ' .. tostring(frame.exit) end
        return nil, 'dbg.remote: ' .. tostring(why)
    end
    if type(frame.value) ~= 'string' then return nil, 'dbg.remote: remote call returned no value' end

    local rok, reply = pcall(jsonx.decode, frame.value)
    if not rok or type(reply) ~= 'table' then return nil, 'dbg.remote: bad reply' end
    if reply.ok ~= true then return nil, 'dbg.remote: ' .. tostring(reply.err) end

    local vok, value = pcall(decode_value, reply.value)
    if not vok then return nil, 'dbg.remote: bad value marker' end
    return value
end

local function session_call(sess, method, ...)
    if sess._closed then return nil, 'dbg.remote: connection closed' end
    local ok, args_json = pcall(encode_args, method, ...)
    if not ok then
        -- 函数 / userdata / session 对象都过不了线（D56 的能力边界）
        return nil, 'dbg.remote: argument cannot cross the wire (function/userdata/Nan)'
    end
    return remote_exchange(sess._conn, sess._opts, sess.root_pid, method, args_json)
end

-- `d:pump([timeout])`：远端事件表是**普通表**（JSON 反序列化），但 `ev.session` 填回本代理对象
-- —— 于是 `ev.session:cont(ev.cont_code)` 这种写法在本地/远端两种会话上完全一致。
function RemoteSession:pump(timeout_ms)
    local ev, err = session_call(self, 'pump', timeout_ms)
    if ev == nil then return nil, err end
    if type(ev) == 'table' then ev.session = self end
    return ev
end

-- 其余方法都是薄转发（名字与本地 session 的方法表**同源**：SESSION_METHODS 的白名单）。
local FORWARDED = {
    'cont', 'step', 'eip', 'arm', 'hits', 'dropped', 'setcap', 'stopped', 'paused', 'caps',
    'detach', 'kill',
    'read', 'read_u8', 'read_u16', 'read_u32', 'read_u64', 'read_str', 'write',
    'context', 'stack', 'mods', 'threads',
}
for i = 1, #FORWARDED do
    local name = FORWARDED[i]
    RemoteSession[name] = function(self, ...)
        return session_call(self, name, ...)
    end
end

-- `d:close()`：只关连接（**不**动目标 —— 收尾永远是显式的 `d:detach()` / `d:kill()`，D51）。
-- 钉住 pid 的用法（`dbg.remote(addr, { pid = N })`）没有 RemoteHost 可关连接，所以这里也给
-- RemoteSession 一个 `close`（对共享同一条连接的多个会话而言，任何一个 close 都会让其余失效）。
function RemoteSession:close()
    self._closed = true
    return dbg.remote_close(self._conn)
end

function RemoteHost:create(opts)
    if type(opts) ~= 'table' then return nil, 'dbglib.remote: create options (table) expected' end
    local ok, args_json = pcall(encode_args, 'create', opts)
    if not ok then return nil, 'dbglib.remote: create options cannot cross the wire (function/userdata)' end
    local pid, err = remote_exchange(self._conn, self._opts, 0, 'create', args_json)
    if pid == nil then return nil, err end
    return setmetatable({ _conn = self._conn, root_pid = pid, _opts = self._opts }, RemoteSession)
end

function RemoteHost:attach(pid)
    if type(pid) ~= 'number' then return nil, 'dbglib.remote: attach: pid (number) expected' end
    local args_json = encode_args('attach', pid)
    local rpid, err = remote_exchange(self._conn, self._opts, 0, 'attach', args_json)
    if rpid == nil then return nil, err end
    return setmetatable({ _conn = self._conn, root_pid = rpid, _opts = self._opts }, RemoteSession)
end

-- 远端的 root_pid 列表（session 对象是 userdata，过不了线）
function RemoteHost:sessions()
    return remote_exchange(self._conn, self._opts, 0, 'sessions', '[]')
end

function RemoteHost:close()
    self._closed = true
    return dbg.remote_close(self._conn)
end

-- =============================================================================
-- dbglib.remote(addr[, opts]) -> RemoteHost | RemoteSession | nil, err
--
-- opts = { token = <hex>, pid = <root_pid>, timeout_ms = 8000, read_timeout_ms = 5000, max_frame = n }
--   无 `opts.pid` ⇒ 返回 RemoteHost（h:create / h:attach / h:sessions / h:close）
--   有 `opts.pid` ⇒ 直接返回钉住那个 root_pid 的 RemoteSession
-- ❗端点必须是跑着 `--repl --server` 的 AutoDbg（代理靠**连接内 state 存活**把多个调用串到同一个
--   `lua_State` 上；非 `--repl` 的 server 每条命令都是新 state ⇒ 会话不存在，代理必然失败）。
-- ❗`h:close()` 必关连接（不关也会在 C 侧 conn userdata 的 `__gc` 里关 —— 但那时候多半已经
--   来不及让远端会话干净收尾了，所以显式 close 仍是推荐写法）。
-- =============================================================================
function dbglib.remote(addr, opts)
    local host, port = split_addr(addr)
    if host == nil then return nil, port end
    if opts ~= nil and type(opts) ~= 'table' then return nil, 'dbglib.remote: opts (table) expected' end

    local o = { timeout_ms = DEFAULT_TIMEOUT_MS }
    if opts ~= nil then
        if opts.timeout_ms ~= nil then
            if type(opts.timeout_ms) ~= 'number' or opts.timeout_ms < 0 then
                return nil, 'dbglib.remote: timeout_ms (number) expected'
            end
            o.timeout_ms = opts.timeout_ms
        end
    end

    local token = (opts ~= nil) and opts.token or nil
    local rtmo  = (opts ~= nil and opts.read_timeout_ms ~= nil) and opts.read_timeout_ms or 0
    local mfrm  = (opts ~= nil and opts.max_frame ~= nil) and opts.max_frame or 0

    local conn, err = dbg.remote_open(host, port, token, rtmo, mfrm)
    if not conn then return nil, 'dbg.remote: ' .. tostring(err) end

    if opts ~= nil and opts.pid ~= nil then
        return setmetatable({ _conn = conn, root_pid = opts.pid, _opts = o }, RemoteSession)
    end
    return setmetatable({ _conn = conn, _opts = o }, RemoteHost)
end

return dbglib
