-- =============================================================================
-- lualib/dbglib.lua —— 多会话脚本糖：rendezvous（wait_all）与 batch/collect（run_all）
--
-- 文件职责：把 plan_t7.md §11.9.2（D58）三种模型里的两种做成一个库，按文件名烘焙成
--   全局 `dbglib`（tools/gen_lua_lib.lua 的约定，见 AGENTS.md §2）。只依赖运行时全局
--   `dbg`，**不新增 C 代码**。
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

return dbglib
