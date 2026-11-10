#ifndef AUTODBG_CORE_TYPES_HPP
#define AUTODBG_CORE_TYPES_HPP
/* =============================================================================
 * src/core/types.hpp —— 核心数据结构（Plan P3.A.1 / 5.4）
 *
 * 来源：`src/debugger/cdebugger.h` 的结构体 + `lib.h` 的类型宏。
 *
 * **迁移纪律（关键）**：结构体的**字段顺序 / 类型 / `#pragma pack(1)` 必须一字不改**
 *   —— `PEInfo` 的 `Path` 是变长尾数组、`BreakpointInfo` 的 `modname` 同理，
 *   且这些结构体参与内存分配与字段偏移计算（见 Plan 4.4 的「PE 偏移与 pack」风险项）。
 *   唯一例外：断点回调令牌 `user_data` 由 `int` 改为 `void*`（A6 的调用方令牌，
 *   哨兵 = `nullptr`）—— 按值使用、无外部布局依赖，且旧结构体仍在
 *   `cdebugger.h` 里独立存在（两者不跨 TU 混用）。
 *
 * 另外本文件定义 core ↔ 绑定层的**回调契约**（`DebugCallbacks` + 各事件载荷），
 *   字段集合与 `tests/golden/events_*.txt` 里锁定的载荷形状一致（P3.A 的安全网）。
 * ========================================================================== */
#include <windows.h>
#include <stddef.h>

#include "callback_token.h" /* callback_ref_t：宿主无关的断点回调令牌（Plan 5.5） */

#ifdef _UNICODE
#   define MAX_FILEPATH 0x8000
#else
#   define MAX_FILEPATH MAX_PATH
#endif

#ifdef _UNICODE
typedef LPWSTR LPXSTR;
typedef LPSTR  LPSSTR;
#else
typedef LPCSTR LPXSTR;
typedef LPCSTR LPSSTR;
#endif

namespace autodbg::core
{
/* 每个断点携带的**调用方令牌**（按值；见 `callback_token.h` 与 Plan 5.5）：
 *   core 只做两件事：存下它、命中时按值还给调用方；从不读取 token 成员。
 * ❗ **有效性显式**：`callback_ref_t::valid` —— 不依赖句柄取值，也没有哨兵常量。 */

#pragma pack(push, 1)

/* 链表基结构体：**首成员必须是 next 指针**（现有全部满足；也是将来换成
 * `common::IntrusiveList<T>` 的前提，见 Plan 4.3 / P6.2） */
struct ThreadInfo
{
    struct ThreadInfo *next;
    DWORD              dwThreadId;
    HANDLE             hThread;
    DWORD              ProcessId;
    size_t             lpStartAddress;
};

struct BreakpointInfo
{
    struct BreakpointInfo *next;
    DWORD                  rva;
    callback_ref_t         callback; /* 宿主无关令牌（按值）；callback.valid = 0 即无回调 */
    BYTE                   orignal_byte;
    BYTE                   has_orignal_byte: 1;
    BYTE                   is_single       : 1;
    BYTE                   activate        : 1;
    TCHAR                  modname[1];
};

struct HardwareBreakpointInfo
{
    size_t         ea;
    callback_ref_t callback; /* 宿主无关令牌（按值） */
    char           DrxBreakPointType;
    DWORD          DrxBreakPointSize;
    BYTE           activate: 1;
};

struct ParentFuncT
{
    DWORD rva;
    DWORD begin_addr;
};

struct FuncInfo
{
    DWORD base;
    char *name;
};

struct FuncRange
{
    DWORD            start;
    DWORD            end;
    struct FuncInfo *infos;
    DWORD            info_count;
};

struct PEInfo
{
    struct PEInfo        *next;
    size_t                base;
    size_t                start_address;
    DWORD                 size;
    int                   section_count;
    IMAGE_SECTION_HEADER *sections;
#ifdef _WIN64
    int                 runtime_function_count;
    PRUNTIME_FUNCTION   runtime_functions;
    int                 parent_function_count;
    struct ParentFuncT *parent_functions;
#endif
    /* 注：`func_chunks` / `func_chunks_count` 在全仓库**从未被分配**（死字段，
     * 见 TODO 待办；P6 清理时一并删掉） */
    struct FuncRange *func_chunks;
    size_t            func_chunks_count;
    DWORD             dwProcessId;
    DWORD             crc32;
    size_t            name_offset;
    TCHAR             Path[1];
};

#pragma pack(pop)

/* ========================= 事件载荷（core → 绑定层） =========================
 * 字段集合与 events.lua 锁定的形状一一对应（tests/golden/events_*.txt）。
 * 约定：所有指针/字符串都是**借用**，仅在回调期间有效（绑定层必须当场构造 Lua 值）。 */
struct EventBase
{
    DWORD pid;
    DWORD tid;
};

struct DebugEvent : EventBase
{
    DWORD code; /* dwDebugEventCode */
};

struct ExceptionEvent : EventBase
{
    DWORD  code;
    size_t addr;
};

struct UserExceptionEvent : EventBase
{
    bool                  first_chance;
    const EXCEPTION_RECORD *record; /* 仅回调期间有效 */
};

struct BreakpointEvent : EventBase
{
    size_t addr;
};

struct FirstBreakpointEvent : EventBase
{
};

struct StepEvent : EventBase
{
    size_t addr;
};

struct ThreadEvent : EventBase
{
    size_t addr;
};

struct ProcessEvent : EventBase
{
    size_t addr;
    size_t base;
    const TCHAR *path; /* 可空 */
};

struct ExitEvent : EventBase
{
    DWORD code;
};

struct DllEvent : EventBase
{
    size_t       base;
    const TCHAR *path; /* 可空 */
};

struct OutputDebugStringEvent : EventBase
{
    const void *data;   /* 调试目标内的原始字节（已读入本地缓冲） */
    DWORD       size;   /* 字节数（含原实现的 NUL，见 events.lua 的说明） */
    bool        unicode; /* 非 0 表示宽字符（UTF-16），绑定层负责转 UTF-8 */
};

/* 断点自身回调（断点自带令牌）：core 只把令牌**按值**还回去 */
struct BreakpointCallbackEvent : EventBase
{
    callback_ref_t callback;
    size_t         ea;
    DWORD          rva;
    const TCHAR   *modname;
    bool           single_shot; /* true → 绑定层须释放该回调（对应旧的 to_unref_cbk） */
};

struct HwBreakpointCallbackEvent : EventBase
{
    callback_ref_t callback;
    size_t         ea;
};

/* ============================ run 参数 / 回调 ============================
 * 对应旧的 `RunParamT`：把「Lua 侧的回调 ref」换成「C 函数指针 + 上下文」，
 * 把「Lua 侧的选项」换成纯数据（见 Plan 4.4/5.3）。 */
struct RunParams
{
    DWORD   pid;          /* 非 0 = 附加（Attach），否则 CreateProcess */
    LPXSTR  imagepath;
    LPXSTR  cmdline;
    LPXSTR  cwd;
    LPVOID  env;
    bool    is_new_console;
    bool    use_scylla_hide;
};

/* 「未等到调试事件」时宿主给出的结论（`DebugCallbacks::on_wait_timeout` 的返回值）。
 * 取值与旧 `CheckConsoleInput` 的 0/1/2 **一一对应**（P3.B 模块 7 / Q27）：
 * core 只执行结论，不再自己读控制台（控制台 I/O 策略属宿主层，见 process.hpp）。 */
enum WaitTimeoutAction : int
{
    WAIT_TIMEOUT_CONTINUE             = 0, /* 继续等待 */
    WAIT_TIMEOUT_DETACH_AND_TERMINATE = 1, /* 分离；目标若由我们创建（CreateProcess 路径）则由 run_shutdown 终止 */
    WAIT_TIMEOUT_DETACH_KEEP_ALIVE    = 2, /* 分离但保留目标（仅当宿主被告诉 can_detach_without_term 时才有意义） */
};

struct DebugCallbacks
{
    /* 会话 / run 级调用方上下文（16 条通道共享；与断点对象的 callback 令牌是两个层次） */
    void *context;

    /* 顺序与旧实现的派发顺序一致（Plan 附录 C · C.3 的前四级） */
    void (*on_any_debug_event)(void *ctx, const DebugEvent &);
    void (*on_exception)(void *ctx, const ExceptionEvent &);
    void (*on_breakpoint)(void *ctx, const BreakpointEvent &);
    void (*on_single_step)(void *ctx, const StepEvent &);
    void (*on_create_thread)(void *ctx, const ThreadEvent &);
    void (*on_create_process)(void *ctx, const ProcessEvent &);
    void (*on_exit_thread)(void *ctx, const ExitEvent &);
    void (*on_exit_process)(void *ctx, const ExitEvent &);
    void (*on_load_dll)(void *ctx, const DllEvent &);
    void (*on_unload_dll)(void *ctx, const DllEvent &);
    void (*on_output_debug_string)(void *ctx, const OutputDebugStringEvent &);
    void (*on_rip)(void *ctx, const DebugEvent &);

    /* 一次性/自身回调（旧实现里带具体回调 ref 的两条通道） */
    void (*on_first_breakpoint)(void *ctx, const FirstBreakpointEvent &);
    void (*on_user_exception)(void *ctx, const UserExceptionEvent &);
    void (*on_breakpoint_callback)(void *ctx, const BreakpointCallbackEvent &);
    void (*on_hw_breakpoint_callback)(void *ctx, const HwBreakpointCallbackEvent &);

    /* 引擎内部错误（**1:1 保留旧行为**）：旧代码在 run 内部直接
     * `lua_pushstring + lua_error`（→ 长跳转、立即终止脚本）。绑定层应把它
     * 转成 `lua_error` 以保持完全相同的可观察行为（含不执行后续清理）。 */
    void (*on_engine_error)(void *ctx, const char *msg);

    /* 断点销毁通知：core 内**所有**删除断点的路径统一走它，
     * 使绑定层只需一个释放点（unref + 释放令牌内存）——见 Plan 5.5。
     * 缺了它，绑定层就得镜像 core 的 5 条死亡路径（模块卸载 / Detach /
     * single-shot 自删 / 单点删除 / 硬件断点删除），漏一处即泄漏。 */
    void (*on_breakpoint_destroyed)(void *ctx, callback_ref_t callback);
    void (*on_hw_breakpoint_destroyed)(void *ctx, callback_ref_t callback);

    /* 「未等到调试事件」（`WaitForDebugEvent` 超时）→ 问宿主下一步怎么办（Q27）。
     * ❗ 这是**唯一**允许宿主决定「要不要停/分离」的通道：core 只把结论付诸执行
     *   （`can_detach_without_term` = 引擎能否在分离的同时不终止目标，即
     *   `DebugActiveProcessStop` 是否可用）；宿主可以在这里读控制台/UI。
     * 返回值为 `WaitTimeoutAction`；回调为 nullptr 时等同于「一直等」（同时意味着
     *   宿主没有提供中断手段 → `run` 只能靠断点/事件自己结束）。 */
    int (*on_wait_timeout)(void *ctx, bool can_detach_without_term);
};

/* -------------------- 内存搜索 / 区域遍历的纯数据（P6.4 从 memory.hpp 搬来，便于会话类声明成员） -------------------- */
/* 搜索上下文（旧实现是 Lua userdata 本体；现归引擎，绑定层只当黑盒存着） */
struct FindMemoryContext
{
    size_t         pattern_len;
    unsigned char *pattern;
    size_t         shift[256];
    BYTE          *addr;
    BYTE          *max_addr;
    unsigned char *buf;
    size_t         buf_len;
    size_t         buf_maxlen;
    int            state;
    size_t         pos;
    size_t         base;
};

enum class FindMemoryKind
{
    Match, /* 命中：`out` 有效 */
    End,   /* 结束（包括旧实现里“读失败就 break”的那条路） */
    Failed /* 需要向宿主报错：`errbuf` 里是文案 */
};

struct FindMemoryMatch
{
    size_t               ea;   /* 命中地址（= 区域基址 + 区域内偏移） */
    const unsigned char *data; /* 指向 ctx->buf 中的匹配字节（借用，下一次调用前有效） */
    size_t               len;  /* = pattern 长度 */
};

/* 初始化（分配 pattern 副本 + shift 表；addr/max_addr 取系统用户空间范围） */

/* 迭代一步（`errbuf` 用于 Failed 的文案，建议 128 字节以上，与旧实现一致） */

/* 释放内部缓冲（= 旧 `__gc`：只释放 `buf`/`pattern` 并置 NULL，上下文本身由宿主回收） */

/* 内存区域（`VirtualQueryEx`）遍历；字段与旧 Lua 表的 7 个键一一对应 */
struct MemRegionInfo
{
    size_t BaseAddress;
    size_t AllocationBase;
    size_t RegionSize;
    DWORD  AllocationProtect;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
};

struct MemRegionIter
{
    size_t addr;
    size_t max_addr;
};
}

#endif /* AUTODBG_CORE_TYPES_HPP */