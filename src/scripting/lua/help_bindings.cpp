/* =============================================================================
 * src/scripting/lua/help_bindings.cpp —— 全局 `help([topic])`（T7.3 切片 A / D53）
 *
 * 来源：新写（plan_t7.md §11.3 / D53）。T7 之前没有帮助：脚本 API 的「契约文档」只有
 *   `tools/agent/autodbg-skill/` 的 markdown（给 AI 用），人类在 REPL 里没有东西可敲。
 *
 * 与旧实现的差异：无旧实现。三条设计约束：
 *   ① **一处文本**：正文只在下面的 `kHelp[]` 里；`help('dbg')` 与 `dbg.help()` 都是
 *      「按前缀生成」的 —— 不存在第二份成员列表。
 *   ② **输出走 core 的 `PrintS`**：本地控制台/管道都好使，server 模式下自动进 `out` 帧
 *      （`set_output_sink`），本层不碰句柄。切片 B 才在 `PrintS` 之上加 `print_styled` 与
 *      「只在真控制台上色」（`GetConsoleMode` 探测），所以现在这一版天然是纯文本。
 *   ③ **错误当值返回**（`nil, err`），不长跳 —— 与 `types.hpp` 的既定口径一致。
 *
 * 文本编码：源码是 UTF-8。现代变体 `/utf-8` ⇒ 窄串就是 UTF-8，`PrintS` 会按控制台需要转
 *   UTF-16；legacy 变体 `/execution-charset:gbk` ⇒ 窄串是 CP936，`PrintS` 走 ANSI 分支。
 *   所以正文一律用**窄串字面量**，不要用 `TEXT()`（那会逼现代变体写两遍正文）。
 *   主题名与签名保持 ASCII —— 门禁逐字匹配它们，不能被编码影响。
 * ========================================================================== */
#include "help_bindings.hpp"

#include "core/util.hpp" /* PrintS（走输出 sink：本地控制台 / server 的 out 帧） */

namespace autodbg::host
{
namespace
{
struct HelpEntry
{
    const char *topic;   /* 全小写、点分：`dbg.pump` */
    const char *summary; /* 一行，进索引与分组列表 */
    const char *body;    /* 多行正文；nullptr ⇒ 只有 summary（细节留给 skill） */
};

/* 主题表。新增 API 时**只改这里**：`help('dbg')` / `dbg.help()` 都是前缀匹配生成的。 */
const HelpEntry kHelp[] = {
    {
        "help",
        "本帮助（help([topic])）",
        "help()            列出所有主题\n"
        "help('dbg')       列出 dbg 表的成员\n"
        "help('dbg.pump')  看单个成员\n"
        "\n"
        "分工（D53）：语义/坑/示例看 skill 包（tools/agent/autodbg-skill/）；\n"
        "「这台机器上有什么」用 dbg.caps()（机器可读）；本帮助只管「现在该敲什么」。\n"
        "所有调用失败时返回 nil, err，不抛异常。\n",
    },
    {
        "dbg",
        "调试会话原语（create/attach → pump → cont → detach/kill）",
        "dbg 是**状态机**，不是 Run 那样「一口气跑到结束」：\n"
        "\n"
        "  dbg.create{ imagepath = 'target.exe' }   -- 或 dbg.attach(pid)\n"
        "  local ev = dbg.pump(100)                 -- 等一个事件；默认**不放行**\n"
        "  if ev then print(ev.type, ev.ea) end     -- 事件停在未应答状态\n"
        "  dbg.cont()                               -- 显式放行（用 core 算好的默认 code）\n"
        "  dbg.detach() / dbg.kill()                -- 收尾\n"
        "\n"
        "与 Run/Attach 的关系：它们是同一套引擎原语的便捷糖；两条路径在同一个会话上\n"
        "不可混用（dbg 停着未应答事件时调 Run/Attach 会报错）。\n",
    },
    {
        "dbg.create",
        "创建并调试一个新进程（= Run 的原语版）",
        "dbg.create{ imagepath = ...,     -- 必填\n"
        "            cmdline = ..., cwd = ...,\n"
        "            is_new_console = false, use_scylla_hide = false,\n"
        "            callback = ..., exception_callback = ...,\n"
        "            on_exception = ..., on_breakpoint = ...,\n"
        "            on_single_step = ..., on_exit_process = ... }\n"
        "返回 true；失败 nil, err。不循环 —— 用 dbg.pump 推进。\n",
    },
    {
        "dbg.attach",
        "附加到已运行的进程（= Attach 的原语版；Win95/98 不支持）",
        "dbg.attach(pid [, { use_scylla_hide = ..., <同样的回调> }])\n"
        "只认**显式** pid（「沿用上次 pid / 鼠标选进程」是 Attach 的策略，不在原语层）。\n"
        "位数必须配对：32 位调试器附 64 位目标会拿到 ERROR_NOT_SUPPORTED。\n",
    },
    {
        "dbg.pump",
        "等一个调试事件；默认不 continue（事件停在未应答状态）",
        "dbg.pump([timeout_ms [, autocont]]) -> code | nil [, err]\n"
        "  nil = 没等到事件（不是错误；目标还停着时下一次 pump 也是空转）\n"
        "  autocont = true -> 引擎自己 ContinueDebugEvent（record-and-continue 档）\n"
        "timeout 按 <=250 ms 切片，每片问一次取消守卫 -> --cancel 能在空闲时插话。\n",
    },
    {
        "dbg.cont",
        "应答当前未应答的事件（显式 code 可以吞异常 / 交给程序）",
        "dbg.cont([pid [, tid [, code]]]) -> true | false\n"
        "不带 code 时用 core 算好的默认应答（dbg.code()）；\n"
        "吞掉断点/异常用 0x00010002（DBG_CONTINUE），交给程序用 0x80010001。\n",
    },
    {
        "dbg.step",
        "单步（置 TF，下一个事件是 SINGLE_STEP）",
        "dbg.step([tid]) -> true | nil, err\n单步后仍要 dbg.cont() 放行；断点的 re-arm 由引擎处理。\n",
    },
    {
        "dbg.eip",
        "读当前线程的指令指针",
        "dbg.eip([tid]) -> integer | nil, err\n（切片 C 会并入 d:context()）\n",
    },
    {
        "dbg.arm",
        "在 (modname, rva) 处武装软件断点并给回调",
        "dbg.arm(modname, rva, callback) -> true | nil, err\n"
        "rva 是**普通数字**（别用 GetModBase 做算术：LuaVariant 会算错）。\n"
        "模块必须已加载（先 pump 到它的 load 事件）；命中后引擎负责压回 EIP 与 re-arm。\n"
        "注意：不要打在本来就写着 int3 的地址上（会反复命中）。\n",
    },
    {
        "dbg.hits",
        "取命中记录（有界窗口，最旧先丢）",
        "dbg.hits() -> { {seq, type, pid, tid, ea, modname, rva, code, first_chance}, ... }\n"
        "type: 1 = 断点 / 2 = 异常 / 3 = 单步。只收这三个通道，不含 LOAD_DLL 之类。\n",
    },
    {
        "dbg.dropped",
        "被窗口丢掉了多少条命中记录（诚实计数）",
        "dbg.dropped() -> integer\n窗口满了还继续命中时每丢一条 +1：你看到的是窗口，不是全部事实。\n",
    },
    {
        "dbg.setcap",
        "设置命中窗口的条数上限",
        "dbg.setcap([1..64]) -> 实际生效的值\n不带参数 = 查当前值。收窄时丢的是最旧那几条（计入 dropped）。\n",
    },
    {
        "dbg.code",
        "上一次事件的默认应答码（0x00010002 / 0x80010001）",
        "dbg.code() -> integer\n切片 C 会把它换成事件表里的 ev.cont_code（逐事件，而不是「上一次」）。\n",
    },
    {
        "dbg.stopped",
        "循环是否已终止（= Run 的 while 出口）",
        "dbg.stopped() -> true | false\n目标退出 / detach / kill 之后为 true。\n",
    },
    {
        "dbg.paused",
        "当前是否停着一个未应答的事件",
        "dbg.paused() -> true | false\n为 true 时 Run/Attach 会拒绝（两条路径不可混用）。\n",
    },
    {
        "dbg.detach",
        "分离：恢复断点原字节 + DebugActiveProcessStop，目标继续活着",
        "dbg.detach() -> true | nil, err\n"
        "注意：Win95/98 没有 DebugActiveProcessStop -> 这个 key **根本不存在**（dbg.detach == nil）。\n"
        "有未应答事件时先替你放行（DBG_CONTINUE）：否则目标会停在被系统挂起的线程上。\n"
        "自己 create 的目标也能 detach（目标继续跑，不再被调试）。\n",
    },
    {
        "dbg.kill",
        "终止目标并收尾",
        "dbg.kill() -> true | nil, err\n先恢复断点原字节（顺带释放断点回调），再 TerminateProcess。\n",
    },
    {
        "dbg.caps",
        "这台机器 / 这个 build 实际有什么（机器可读）",
        "dbg.caps() -> { arch, variant, flavor, win9x, attach, detach, hits_cap }\n"
        "布尔能力按**表成员**生成，所以「detach 在不在」的权威判据仍是 dbg.detach ~= nil。\n",
    },
    {"dbg.help", "打印 dbg 这一节（等价于 help('dbg')）", nullptr},
    {
        "gui",
        "GUI 自动化：键鼠 / 屏幕 / 窗口 / 控件 / 截图",
        "唯一入口是全局表 gui：gui.pick() / gui.screenshot() / gui.send(...) …\n"
        "失败一律返回 nil, errmsg。平台相关的调用（SendInput / PrintWindow / BlockInput）\n"
        "走动态解析并在 9x 上降级。细节见 skill 包的 references/lua-api.md。\n",
    },
    {
        "open_file",
        "文件对象（本地或 client 侧文件，io 风格 API）",
        "local f, err = open_file(path [, mode])\n"
        "f:read('*a'|'*l'|'*n'|N) / f:read_line() / f:lines() / f:write(...) / f:flush()\n"
        "f:seek(...) / f:tell() / f:setbuf(n) / f:close()\n"
        "调试回调里 f:read() 可以阻塞等回包（超时 -> nil, 'timeout'）。\n",
    },
    {
        "legacy",
        "29 个历史契约名（冻结；新脚本请用 dbg / gui / open_file）",
        "Run / Attach / Detach / Kill / ForceKill / SetBreakpoint / DelBreakpoint /\n"
        "SetHardwareBreakpoint / DelHardwareBreakpoint / ReadMemory / WriteMemory / V /\n"
        "GetContext / SetContext / Ea2ModnameRva / GetModBase / StackTrace / SetBreakpoints /\n"
        "EnableBreakpoint / DisableBreakpoint / LoadStr / Hide / FindFuncRva / GetCppClassName /\n"
        "FindMemory / GetMemoryRegion / ResetDir / BsonEncode / BsonDecode\n"
        "它们的行为被门禁逐字节锁定（AGENTS.md §5 第 6 条），不改名、不加新参数。\n"
        "注意 V/U32/U64 那套 LuaVariant 参与算术会算错（plan_t7.md §9.9）。\n",
    },
};

bool streq(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) return a == b;
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}

size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

/* 顶层主题 = 不含 '.' 的（`help` / `dbg` / `gui` / `open_file` / `legacy`） */
bool top_level(const char *topic)
{
    for (const char *p = topic; *p; ++p)
    {
        if (*p == '.') return false;
    }
    return true;
}

const HelpEntry *find_entry(const char *topic)
{
    for (const HelpEntry &e : kHelp)
    {
        if (streq(e.topic, topic)) return &e;
    }
    return nullptr;
}

/* 是否存在 `topic.<something>` 形式的条目（`dbg` → `dbg.pump` …） */
bool has_group(const char *topic)
{
    size_t n = slen(topic);
    for (const HelpEntry &e : kHelp)
    {
        size_t i = 0;
        while (i < n && e.topic[i] == topic[i]) ++i;
        if (i == n && e.topic[n] == '.') return true;
    }
    return false;
}

void print_line(const char *s)
{
    autodbg::core::PrintS(s);
    autodbg::core::PrintS("\n");
}

void print_entry_summary(const HelpEntry &e)
{
    autodbg::core::PrintS("  ");
    autodbg::core::PrintS(e.topic);
    /* 粗略对齐（按字符数补到 12 列）：索引与分组列表才好扫读 */
    size_t n = slen(e.topic);
    for (size_t i = n; i < 12; i++) autodbg::core::PrintS(" ");
    print_line(e.summary);
}

void print_group(const char *topic)
{
    size_t n = slen(topic);
    for (const HelpEntry &e : kHelp)
    {
        size_t i = 0;
        while (i < n && e.topic[i] == topic[i]) ++i;
        if (i == n && e.topic[n] == '.') print_entry_summary(e);
    }
}
} /* namespace */

bool help_print_topic(const char *topic)
{
    if (topic == nullptr || *topic == 0)
    {
        help_print_index();
        return true;
    }
    if (const HelpEntry *e = find_entry(topic))
    {
        autodbg::core::PrintS(e->topic);
        autodbg::core::PrintS(" —— ");
        autodbg::core::PrintS(e->summary);
        autodbg::core::PrintS("\n\n");
        if (e->body != nullptr) autodbg::core::PrintS(e->body);
        /* 既是条目、又有成员的（`dbg`）：紧跟着把成员列出来 —— 这就是 `help('dbg')`
         * 与 `dbg.help()` 的全部实现（不另存第二份成员表）。 */
        if (has_group(topic))
        {
            autodbg::core::PrintS("\n成员（help('<名字>') 看详细）：\n");
            print_group(topic);
        }
        return true;
    }
    if (has_group(topic))
    {
        autodbg::core::PrintS(topic);
        autodbg::core::PrintS(" 的成员（help('");
        autodbg::core::PrintS(topic);
        autodbg::core::PrintS(".<名字>') 看详细）\n\n");
        print_group(topic);
        return true;
    }
    return false;
}

void help_print_index()
{
    print_line("autodbg 帮助 —— help('topic') 看详细；help('dbg') 看 dbg 表全部成员");
    print_line("语义/坑/示例：tools/agent/autodbg-skill/    机器可读能力：dbg.caps()");
    print_line("");
    for (const HelpEntry &e : kHelp)
    {
        /* 索引只列**顶层主题**（不带点）；成员靠 help('dbg') */
        if (top_level(e.topic)) print_entry_summary(e);
    }
    print_line("");
    print_line("  例：help('dbg.pump')    dbg.help()    dbg.caps()");
}

namespace
{
int lua_help(lua_State *L)
{
    if (lua_isnoneornil(L, 1))
    {
        help_print_index();
        lua_pushboolean(L, 1);
        return 1;
    }
    if (!lua_isstring(L, 1))
    {
        lua_pushnil(L);
        lua_pushstring(L, "help: topic (string) expected");
        return 2;
    }
    const char *topic = lua_tostring(L, 1);
    if (help_print_topic(topic))
    {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushfstring(L, "no help for '%s' (try help())", topic);
    return 2;
}
} /* namespace */

void register_help_api(lua_State *L) { lua_register(L, "help", lua_help); }

} /* namespace autodbg::host */
