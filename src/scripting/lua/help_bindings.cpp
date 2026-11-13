/* =============================================================================
 * src/scripting/lua/help_bindings.cpp —— 全局 `help([topic])`（T7.3 切片 A / D53）
 *
 * 来源：新写（plan_t7.md §11.3 / D53）。T7 之前没有帮助：脚本 API 的「契约文档」只有
 *   `tools/agent/autodbg-skill/` 的 markdown（给 AI 用），人类在 REPL 里没有东西可敲。
 *
 * 与旧实现的差异：无旧实现。三条设计约束：
 *   ① **一处文本**：正文只在下面的 `kHelp[]` 里；`help('dbg')` 与 `dbg.help()` 都是
 *      「按前缀生成」的 —— 不存在第二份成员列表。
 *   ② **输出走 core 的 `PrintS` / `print_styled`**：本地控制台/管道都好使，server 模式下
 *      自动进 `out` 帧（`set_output_sink`），本层不碰句柄。切片 B（下面 `put`/`line`）只给
 *      每一行带一个属性，**文本字节一个不改**；条件不满足（非控制台 / `NO_COLOR` /
 *      server 的 sink / `--json`）时属性位自动退回 0 = 纯文本（`core::print_styled` 的
 *      降级链），所以管道里没有任何属性字节或 ESC。
 *   ③ **错误当值返回**（`nil, err`），不长跳 —— 与 `types.hpp` 的既定口径一致。
 *
 * 上色（切片 B / D53）：只有**真控制台**且没有 `NO_COLOR` 才上色，判定在
 *   `common::console_style_enabled`（纯函数，`--selftest` 直接断言）；不用 ANSI 转义
 *   （9x 控制台没有 VT），真正的 `SetConsoleTextAttribute` 在 `core::print_styled` 里。
 *
 * 文本编码：源码是 UTF-8。现代变体 `/utf-8` ⇒ 窄串就是 UTF-8，`PrintS` 会按控制台需要转
 *   UTF-16；legacy 变体 `/execution-charset:gbk` ⇒ 窄串是 CP936，`PrintS` 走 ANSI 分支。
 *   所以正文一律用**窄串字面量**，不要用 `TEXT()`（那会逼现代变体写两遍正文）。
 *   主题名与签名保持 ASCII —— 门禁逐字匹配它们，不能被编码影响。
 * ========================================================================== */
#include "help_bindings.hpp"

#include "common/console_style.hpp" /* 上色判定：真控制台 && 没有 NO_COLOR（切片 B / D53） */
#include "core/util.hpp"           /* PrintS / print_styled（走输出 sink：本地控制台 / server 的 out 帧） */

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
        "调试会话（create/attach 拿会话对象 → d:pump → d:cont → d:detach/d:kill）",
        "dbg 是**状态机**，不是 Run 那样「一口气跑到结束」。create/attach 返回**会话对象**：\n"
        "\n"
        "  local d = dbg.create{ imagepath = 'target.exe' }   -- 或 dbg.attach(pid)\n"
        "  local ev = d:pump(100)                 -- 等一个事件；默认**不放行**\n"
        "  if ev then print(ev.type, ev.ea) end   -- 事件停在 d.stop（= 这一张表）\n"
        "  d:cont()                               -- 显式放行（缺省 = ev.cont_code）\n"
        "  d:detach() / d:kill()                  -- 收尾\n"
        "\n"
        "事件是**普通表**：type pid tid ea modname rva code first_chance cont_code。\n"
        "一次执行只能有一个会话；dbg.sessions() / dbg.session(pid) / dbg.last() 用来取回它。\n"
        "dbg.* 上的同名函数仍然可用，作用在默认会话（最近一次 create/attach 的那个）上。\n"
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
        "返回**会话对象**；失败 nil, err。不循环 —— 用 d:pump 推进。\n",
    },
    {
        "dbg.attach",
        "附加到已运行的进程（= Attach 的原语版；Win95/98 不支持）",
        "dbg.attach(pid [, { use_scylla_hide = ..., <同样的回调> }]) -> 会话对象 | nil, err\n"
        "只认**显式** pid（「沿用上次 pid / 鼠标选进程」是 Attach 的策略，不在原语层）。\n"
        "位数必须配对：32 位调试器附 64 位目标会拿到 ERROR_NOT_SUPPORTED。\n",
    },
    {
        "dbg.pump",
        "等一个调试事件；默认不 continue（事件停在未应答状态）",
        "dbg.pump([timeout_ms [, autocont]]) -> 事件表 | nil [, err]\n"
        "  nil = 没等到事件（不是错误；目标还停着时下一次 pump 也是空转）\n"
        "  autocont = true -> 引擎自己 ContinueDebugEvent（record-and-continue 档）\n"
        "d:pump(...) 是**缓存优先**：d.stop 非空就立即返回它（不阻塞）。\n"
        "事件表字段：type pid tid ea modname rva code first_chance cont_code。\n"
        "timeout 按 <=250 ms 切片，每片问一次取消守卫 -> --cancel 能在空闲时插话。\n",
    },
    {
        "dbg.cont",
        "应答当前未应答的事件（显式 code 可以吞异常 / 交给程序）",
        "d:cont([code]) -> true | false          -- 缺省 = d.stop.cont_code\n"
        "dbg.cont([pid [, tid [, code]]]) -> true | false   -- 旧形态，作用在默认会话上\n"
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
        "d:arm(modname, rva, callback) -> true | nil, err\n"
        "rva 是**普通数字**（别用 GetModBase 做算术：LuaVariant 会算错）。\n"
        "允许**延迟武装**：模块还没加载时只登记，CREATE_PROCESS/LOAD_DLL 时补 0xCC。\n"
        "命中后引擎负责压回 EIP 与 re-arm。\n"
        "注意：不要打在本来就写着 int3 的地址上（会反复命中）。\n",
    },
    {
        "dbg.hits",
        "取命中记录（有界窗口，最旧先丢）",
        "d:hits() -> { {seq, type, pid, tid, ea, modname, rva, code, cont_code, first_chance}, ... }\n"
        "type: 1 = 断点 / 2 = 异常 / 3 = 单步。只收这三个通道，不含 LOAD_DLL 之类。\n"
        "是历史窗口的**副本**；要「当前那个」就读 d.stop。\n",
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
        "dbg.sessions",
        "列出这次执行里的会话（跨 d = nil 存活）",
        "dbg.sessions() -> { {root_pid, kind, state, lua_bound}, ... }\n"
        "kind: 'create' / 'attach'；state: 'running' / 'stopped' / 'exited'。\n",
    },
    {
        "dbg.session",
        "按 root_pid 取回同一个会话对象",
        "dbg.session(root_pid) -> 会话对象 | nil\n句柄还在时返回**同一个** userdata（表身份可以判等）。\n",
    },
    {
        "dbg.last",
        "最近一次 create/attach 的会话（= 默认会话）",
        "dbg.last() -> 会话对象 | nil\n收尾（detach/kill）之后为 nil。\n",
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

/* 切片 B（D53）的三种色。取值是 `SetConsoleTextAttribute` 的 `FOREGROUND_*` 位；
 * `STYLE_PLAIN = 0` = 默认色（`core::print_styled` 不设属性）。 */
enum HelpStyle
{
    STYLE_PLAIN     = 0,
    STYLE_TITLE     = FOREGROUND_GREEN | FOREGROUND_INTENSITY, /* 标题：亮绿 */
    STYLE_TOPIC     = FOREGROUND_BLUE | FOREGROUND_INTENSITY,  /* 主题名：亮蓝 */
    STYLE_SECONDARY = FOREGROUND_INTENSITY,                    /* 注释 / 次要行：暗色 */
};

/* 本屏是否上色：**每屏开头赋值一次**（`help_print_index` / `help_print_topic`），整屏复用 ——
 * `help` 是低频调用，但没理由每行都查一遍环境变量。零初始化的 POD（不是线程局部、也没有
 * 非平凡构造），所以不违反映射模块的禁令（AGENTS §11.5）；未探测前默认 false = 纯文本，
 * 即「忘了探测」只会少颜色，不会多属性字节。 */
bool g_styling = false;

/* 真控制台 + 没有 `NO_COLOR`（判定本身是 `common` 的纯函数，`--selftest` 直接断言）。
 * ❗sink 装没装不在这里判：`core::print_styled` 自己先把 sink 当纯文本（server / `--json`）。 */
bool detect_styling()
{
    HANDLE h    = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    bool   is_console = (h != NULL && h != INVALID_HANDLE_VALUE) && GetConsoleMode(h, &mode) != 0;
    return autodbg::common::console_style_enabled(is_console, autodbg::common::no_color_requested());
}

/* 唯一输出点：不带样式的调用一律给 `STYLE_PLAIN`（文本字节因此与切片 A 逐字一致）。 */
void put(HelpStyle style, const char *s)
{
    autodbg::core::print_styled(g_styling ? (unsigned)style : 0u, s);
}

void line(HelpStyle style, const char *s)
{
    put(style, s);
    put(STYLE_PLAIN, "\n");
}

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

void print_entry_summary(const HelpEntry &e)
{
    put(STYLE_PLAIN, "  ");
    put(STYLE_TOPIC, e.topic);
    /* 粗略对齐（按字符数补到 12 列）：索引与分组列表才好扫读 */
    size_t n = slen(e.topic);
    for (size_t i = n; i < 12; i++) put(STYLE_PLAIN, " ");
    line(STYLE_SECONDARY, e.summary);
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
    g_styling = detect_styling();
    if (topic == nullptr || *topic == 0)
    {
        help_print_index();
        return true;
    }
    if (const HelpEntry *e = find_entry(topic))
    {
        put(STYLE_TOPIC, e->topic);
        put(STYLE_PLAIN, " —— ");
        put(STYLE_SECONDARY, e->summary);
        put(STYLE_PLAIN, "\n\n");
        if (e->body != nullptr) put(STYLE_PLAIN, e->body);
        /* 既是条目、又有成员的（`dbg`）：紧跟着把成员列出来 —— 这就是 `help('dbg')`
         * 与 `dbg.help()` 的全部实现（不另存第二份成员表）。 */
        if (has_group(topic))
        {
            line(STYLE_TITLE, "\n成员（help('<名字>') 看详细）：");
            print_group(topic);
        }
        return true;
    }
    if (has_group(topic))
    {
        put(STYLE_TOPIC, topic);
        put(STYLE_SECONDARY, " 的成员（help('");
        put(STYLE_TOPIC, topic);
        put(STYLE_PLAIN, ".<名字>') 看详细）\n\n");
        print_group(topic);
        return true;
    }
    return false;
}

void help_print_index()
{
    g_styling = detect_styling();
    line(STYLE_TITLE, "autodbg 帮助 —— help('topic') 看详细；help('dbg') 看 dbg 表全部成员");
    line(STYLE_SECONDARY, "语义/坑/示例：tools/agent/autodbg-skill/    机器可读能力：dbg.caps()");
    line(STYLE_PLAIN, "");
    for (const HelpEntry &e : kHelp)
    {
        /* 索引只列**顶层主题**（不带点）；成员靠 help('dbg') */
        if (top_level(e.topic)) print_entry_summary(e);
    }
    line(STYLE_PLAIN, "");
    line(STYLE_SECONDARY, "  例：help('dbg.pump')    dbg.help()    dbg.caps()");
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
