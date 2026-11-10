/* =============================================================================
 * P3.A.5a：本文件已从 C 切到 C++（`.c` → `.cpp`），**逻辑内容暂不变**，
 *   目的只是让 P3.A.5b 能用上 C++ 的 `DebugCallbacks` 回调契约。
 *
 * 原 `#include "lib.h"` 被拆成两半（见 Plan 5.7）：
 *   - 非 Lua 部分 → `core/util.hpp`（打印/错误/日志、UTF、CRC32、ZeroMem/CopyMem 宏、
 *                  `alloc_`/`free_` 过渡桥接）；
 *   - Lua 部分   → `scripting/lua/helpers.hpp`（`get_lua_str`/`lua_type_error`/`str2integer`）。
 * 旧的 `_Generic` 宏在这里被同名**重载函数**取代（C++ 没有 `_Generic`）。
 *
 * 仍存在的 C 头（cdebugger.h/tools.h/bson.h/hide.h/variant.hpp）本身没有
 * `extern "C"` 护卫 → 在这里用 `extern "C"` 包起来，保证链接名一致
 * （P3.B 这些文件迁到 C++ 时再给头文件加正式护卫）。 */
extern "C"
{
#include "core/types.hpp"
#include "variant.hpp"
#include "bson.hpp"
#include "core/hide.h"
}

#include "core/util.hpp"
#include "core/memory.hpp"                                                                 /* P3.B：内存读写模块 */
#include "core/pe.hpp"                                                                     /* P3.B：PE / 模块信息模块 */
#include "core/context.hpp"                                                                /* P3.B：线程上下文（字段表 + 薄封装） */
#include "core/hardware_breakpoints.hpp"                                                   /* P3.B：硬件断点模块 */
#include "core/breakpoints.hpp"                                                            /* P3.B：软件断点模块 */
#include "core/process.hpp"                                                                /* P3.B：进程 / 线程控制模块 */
#include "core/debug_loop.hpp"                                                             /* P3.B：调试事件主循环（Run） */
#include "common/handle.hpp"                                                               /* P6.1：HANDLE RAII */
#include "scripting/lua/last_pid.hpp"                                                      /* P3.B 模块 12：上次 PID 持久化（原 tools.c） */
#include "core/stack_trace.hpp"                                                            /* P3.B：调用栈回溯（dbghelp 回调 + x86 兼容结构体） */
#include "common/dynapi.hpp"                                                              /* get_proc_as：dbghelp 动态解析（Win9x 兼容） */
#include "core/core.hpp" /* P6.4：状态访问改为显式 current_session()（engine.hpp 已删） */ /* P3.B：模块共享的全局状态（过渡期） */
#include "core/types.hpp"
#include "scripting/lua/helpers.hpp"
#include "scripting/lua/console_input.hpp" /* 宿主层控制台判定（纯函数，可单测；Q27） */
#include "scripting/lua/exec_guard.hpp"    /* S2：远程执行的取消/超时守卫（plan_server.md §5.4） */
#include "registry.hpp"                    /* P4.2：注册表（`register_lua_api`） */
#include "state.hpp"                       /* P4.1：脚本状态（`run_script_file` / `g_wfilepath` / `generate_templete`） */
#include "bindings.hpp"                    /* P4.2：本 TU 的 `lua_*` 入口声明（registry.cpp 也用它） */

/* 迁移期便利：让旧调用点（`PrintF`/`ErrorS`/`log_msg`/`LjGetConsoleWindow`/
 * `get_lua_str`/`lua_type_error`/...）保持**不加限定名**即可使用。
 *
 * ❗ 这里**故意不用 `using namespace`**：`autodbg::core` 里也有 `PEInfo`/`ThreadInfo`
 * 等同名类型，而本文件仍在使用 C 头（cdebugger.h）里的 `PEInfo` ——
 * using-directive 会让两者同时可见 → 二义性报错。（踩过，记录在 TODO 阶段 3）
 * 因此：需引入的少量工具函数用**选择性 using**，类型则显式走 `core::` 别名。 */
namespace core = autodbg::core;
using autodbg::core::ansi_to_utf8_n; /* 声明本身是无条件的（Unicode 构建也会用到）*/
using autodbg::core::calc_crc32;
using autodbg::core::Error;
using autodbg::core::ErrorF;
using autodbg::core::ErrorN;
using autodbg::core::ErrorS;
using autodbg::core::init_tab;
using autodbg::core::LjDebugSetProcessKillOnExit;
using autodbg::core::LjGetConsoleWindow;
using autodbg::core::log_msg;
using autodbg::core::PrintF;
using autodbg::core::PrintN;
using autodbg::core::PrintS;
using autodbg::core::utf16_to_utf8;
using autodbg::core::utf16_to_utf8_n;
using autodbg::core::utf8_to_utf16;
using autodbg::core::utf8_to_utf16_n;
#ifndef _UNICODE
/* 这个只在非 Unicode 构建存在（ANSI 专属 shim），Unicode 下 using 会报 C2039 */
using autodbg::core::LjGetMappedFileNameA;
#endif
using autodbg::common::unique_handle; /* P6.1 */
using autodbg::scripting::lua::get_lua_str;
using autodbg::scripting::lua::lua_type_error;
using autodbg::scripting::lua::str2integer;

/* P6.4：引擎函数已从「全局自由函数」改成 `autodbg::core` 里的会话成员（各模块头保留了同名转发）→
 * 这里用选择性 using 保持调用点可读。
 * ❗本层是**宿主**：运行时只有一个会话（`current_session()`）；多实例的 Lua 可见形态按时
 * Q26 推迟（将来是「每个 adapter 带一个会话」+ 显式传参，见 Plan 附录 C）。 */
/* P6.4：本层是**宿主**，作用于「当前会话」。引擎函数已是 `DebuggerSession` 的成员 ——
 * 这里用 S() 显式表示「对它调用」；多实例的 Lua 可见形态按 Q26 推迟（Plan 附录 C）。 */
static autodbg::core::DebuggerSession &S() noexcept
{
    return autodbg::core::current_session();
}

using autodbg::scripting::lua::pcall; /* P4.3：回调派发辅助（原在本文件） */
using autodbg::scripting::lua::pcall_pre;

/* P3.A.5a：原 lib.h 无条件带了这两个（保持 1:1，见文件末尾 InitCrtDbg/CheckMemLeak 的 _DEBUG 块） */
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>

/* P4.1：`datas.h`（`all_bytecodes` 烘焙库）已随引导段搬到 `state.cpp`。 */

#include <luaconf.h>
#include <assert.h>
#include <dbghelp.h>
#include <psapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <commdlg.h>
#include <intrin.h>

/* 类型统一（删 legacy_types.h）：这些名字现在只有 types.hpp 里的 `autodbg::core::` 那套 */
using autodbg::core::BreakpointInfo;
using autodbg::core::PEInfo;
/* P3.B 模块 9：`run`（调试主循环）与 `run_shutdown` 已迁到 `core/debug_loop.cpp`；
 * ScyllaHide 的条件引入（含 98 构建的空实现）也随之搬走。 */

#define lua_getcheckfield_(k, t) \
    lua_getfield(L, 1, k);       \
    luaL_checktype(L, -1, t)
#define lua_getfield_(k, t) (lua_getfield(L, 1, k), lua_type(L, -1) == t)
#ifndef lua_unref
#   define lua_unref(L, t) luaL_unref(L, LUA_REGISTRYINDEX, t)
#endif

#define THREAD_GETSETSUSPEND (THREAD_SUSPEND_RESUME | THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION)

/* P4.7：会话实例（`s_session`）与入口层（`_tmain` / `mainCRTStartup` / CRT stub /
 * 调试堆）已搬到 `main.cpp`；本文件只剩绑定层（`lua_*` + 适配器）。 */

/* 脚本路径属 **Lua 层**（Q7）→ 不进会话、也不在 core/engine.hpp 里。
 * P4.1：它的**定义**已搬到 `state.cpp`（`state.hpp` 里声明 `extern "C" LPCTSTR g_wfilepath;`）。
 * 注：原先这里是个 `extern "C" { … }` 定义块 —— 搬走后**连花括号一起删掉**；
 *     最初只删了内容、留下 `extern "C" {`，又把后面几千行都拖成 C 链接（C2732/C1075 一片）。 */

/* P3.B 模块 7：`get_thread_by_tid`/`detach_process`/`find_pid_by_mouse` 已迁到
 * `core/process.cpp`；`CheckConsoleInput` 见下方**适配器区**（Q27：它属宿主层策略，
 * 由 `DebugCallbacks::on_wait_timeout` 回调驱动，不再由引擎自己读控制台）。 */

/**
 * Lua interface for read
 * @param LuaVariant::U32 or U64 addr
 * @param LuaVariant::U32 or U64 size
 * @return table
 */
int lua_read_memory(lua_State *L)
{
    if (lua_gettop(L) != 2)
    {
        lua_pushliteral(L, "ReadMemory(addr: UZ, size: UZ): table");
        return lua_error(L);
    }

    size_t addr = lv_check(L, 1, LV_UZ)->val.uz;
    size_t size = lv_check(L, 2, LV_UZ)->val.uz;

    void *data = alloc_(size);
    if (!data)
        return -1;

    if (!S().read(addr, data, size))
    {
        free_(data);
        return -1;
    }

    lua_createtable(L, (int)size, 0);
    for (size_t i = 0; i < size; i++)
    {
        lua_pushinteger(L, (lua_Integer)((const BYTE *)data)[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    free_(data);
    return 1;
}

/**
 * Lua interface for write
 * @param LuaVariant::UZ addr
 * @param LuaVariant::UZ size
 * @param table new_data
 * @param boolean? is_up_intrin
 */
int lua_write_memory(lua_State *L)
{
    if (lua_gettop(L) != 2 && lua_gettop(L) != 3)
    {
        lua_pushliteral(L, "WriteMemory(addr: UZ, new_data: table, is_up_intrin: boolean?)");
        return lua_error(L);
    }

    if (lua_type(L, 3) != LUA_TBOOLEAN && lua_type(L, 3) != LUA_TNIL && lua_type(L, 3) != LUA_TNONE)
        return lua_type_error(L, 3, "boolean (optional)");
    bool is_up_intrin = lua_type(L, 3) == LUA_TBOOLEAN && !!lua_toboolean(L, 3);

    size_t addr = lv_check(L, 1, LV_UZ)->val.uz;
    luaL_checktype(L, 2, LUA_TTABLE);
    int   size = (int)lua_objlen(L, 2);
    BYTE *data = (BYTE *)alloc_(size);
    for (int i = 0; i < size; i++)
    {
        lua_rawgeti(L, 2, i + 1);
        data[i] = (BYTE)luaL_checkinteger(L, -1);
    }
    bool r = S().write(addr, data, size, is_up_intrin);
    free_(data);
    return r ? 0 : -1;
}

/* 断点回调令牌（Plan 5.5 / `core/callback_token.h`）：引擎按**值**存下并原样还回，
 * 含义完全由绑定层定义。零堆分配、零类型 pun、零哨兵赌博：
 *   · Lua 的 registry ref 是整数句柄 → 放进 `token.i`；
 *   · 有效性看 `valid`（**不依赖 ref 的取值**，所以 ref 是不是 0 都无所谓）；
 *   · 令牌随断点节点一起生灭，释放动作只剩 `lua_unref`（没有 holder 要 free）。
 * 将来接 Python/Node 时，它们的句柄是指针 → 用 `token.ptr`，core 侧一行都不用改。 */
static callback_ref_t luaref_to_token(int ref)
{
    callback_ref_t r;
    ZeroMem(r);
    if (ref != LUA_NOREF)
    {
        r.token.i = (intptr_t)ref;
        r.valid   = 1;
    }
    return r;
}

static int token_ref(callback_ref_t r)
{
    return r.valid ? (int)r.token.i : LUA_NOREF;
}

/* P3.B：给「以 lua_State 为 context」的断点删除路径用（Lua 主动调用）。
 * 与 lua_on_breakpoint_destroyed 等价，只是 context 直接就是 lua_State ——
 * 这样调用点不必提前使用后面才定义的 LuaDebugAdapter。 */
static void lua_unref_token_from_state(void *ctx, callback_ref_t callback)
{
    lua_State *L   = (lua_State *)ctx;
    int        ref = token_ref(callback);
    if (ref != LUA_NOREF)
        lua_unref(L, ref);
}

/* 断点删除时的销毁通知表（只有销毁通道 + context = L） */
static void lua_make_destroy_callbacks(lua_State *L, core::DebugCallbacks *cb)
{
    ZeroMem(*cb);
    cb->context                 = L;
    cb->on_breakpoint_destroyed = lua_unref_token_from_state;
}

/**
 * @param table
 * @    field string modname
 * @    field LuaVariant::U32 rva
 * @    field boolean? is_single
 * @    field funciton callback
 */
int lua_set_breakpoint(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "\"\"参数错误。SetBreakpoint原型:\n"
                           "function SetBreakpoint(opt): nil\n"
                           "opt:\n"
                           "  @field string modname\n"
                           "  @field LuaVariant::U32 rva rva数值或16进制字符串\n"
                           "  @field boolean? is_single 是否为一次性断点\n"
                           "  @field function(void):void callback 断点回调\n");
        return lua_error(L);
    }
    lua_getfield(L, 1, "rva");
    DWORD rva = lv_check(L, -1, LV_U32)->val.u32;
    lua_pop(L, 1);
    lua_getcheckfield_("modname", LUA_TSTRING);
    LPCSTR modname  = lua_tostring(L, -1);
    LPXSTR wmodname = utf8_to_tchar(modname);
    lua_pop(L, 1);
    lua_getcheckfield_("callback", LUA_TFUNCTION);
    int callback = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, 1, "is_single");
    bool is_single = false;
    if (lua_type(L, -1) != LUA_TNIL)
    {
        if (lua_type(L, -1) == LUA_TBOOLEAN)
            is_single = !!lua_toboolean(L, -1);
        else
            return lua_type_error(L, -1, "boolean or nil");
    }
    lua_pop(L, 1);
    S().set_breakpoint(wmodname, rva, is_single, luaref_to_token(callback));
    free_from_tchar(wmodname);
    assert(lua_gettop(L) == 1);
    return 0;
}

int lua_del_breakpoint(lua_State *L)
{
    core::DebugCallbacks cb;
    lua_make_destroy_callbacks(L, &cb);
    DWORD rva;
    if (lua_gettop(L) == 1)
    {
        size_t  ea     = lv_check(L, 1, LV_UZ)->val.uz;
        PEInfo *peinfo = S().get_peinfo_by_ea(ea);
        if (peinfo)
            S().del_breakpoint(&cb, ea, peinfo->Path + peinfo->name_offset, (DWORD)(ea - peinfo->base));
    }
    else if (lua_gettop(L) == 2)
    {
        rva             = lv_check(L, 2, LV_U32)->val.u32;
        LPXSTR wmodname = utf8_to_tchar(luaL_checkstring(L, 1));
        size_t base     = S().get_base(wmodname);
        if (base)
            S().del_breakpoint(&cb, base + rva, wmodname, rva);
        free_from_tchar(wmodname);
    }
    else
    {
        lua_pushliteral(L, "\"DelBreakpoint\"参数错误。DelBreakpoint原型:\n"
                           "function DelBreakpoint(ea: LuaVariant::" UZ "): nil\n"
                           "function DelBreakpoint(modname: string, rva: LuaVariant::U32): nil\n"
                           "  ea 断点地址\n"
                           "  modname 断点所在模块名称\n"
                           "  rva 断点在所在模块的RVA或其16进制字符串\n");
        return lua_error(L);
    }
    return 0;
}

/* ---------- 硬件断点：线程级变更函数（旧代码在 4 个函数里各抄一遍遍历） ----------
 * 统一走 `core::update_all_threads_context`（取 → mutate → 写回），
 * 遍历/取失败跳过 的策略只有一处。 */

int lua_set_hardware_breakpoint(lua_State *L)
{
    /* HWB_SIZE_STR 已提到 bindings.hpp（state.cpp 的模板文本也要用） */
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "\"\"参数错误。SetHardwareBreakpoint原型:\n"
                           "function SetHardwareBreakpoint(opt): nil\n"
                           "opt:\n"
                           "  @field LuaVariant::UZ ea 断点地址\n"
                           "  @field integer? index 硬件断点dr寄存器序号，支持0~3\n"
                           "  @field string type 硬件断点类型，支持e: 执行、w: 写入、r: 读写\n"
                           "  @field integer size 硬件断点大小，支持" HWB_SIZE_STR "\n"
                           "  @field function(void):void callback 断点回调\n");
        return lua_error(L);
    }
    lua_getfield(L, 1, "ea");
    size_t ea = lv_check(L, -1, LV_UZ)->val.uz;
    lua_pop(L, 1);
    DWORD index = 0;
    lua_getfield(L, 1, "index");
    if (lua_type(L, -1) == LUA_TNUMBER)
        index = (DWORD)lua_tointeger(L, -1) + 1;
    else if (lua_type(L, -1) != LUA_TNIL)
        return lua_type_error(L, -1, "nil or number");
    lua_pop(L, 1);
    lua_getfield(L, 1, "type");
    size_t      type_len;
    const char *type = luaL_checklstring(L, -1, &type_len);
    if (type_len != 1 || (type[0] != 'e' && type[0] != 'w' && type[0] != 'r'))
    {
        lua_pushliteral(L, "\"\"参数错误。SetHardwareBreakpoint 中 type 仅支持e: 执行、w: 写入、r: 读写\n");
        return lua_error(L);
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "size");
    lua_Integer size = luaL_checkinteger(L, -1);
#ifdef _WIN64
    if (size != 1 && size != 2 && size != 4 && size != 8)
    {
        lua_pushliteral(L, "\"\"参数错误。SetHardwareBreakpoint 中 size 仅支持1、2、4、8\n");
        return lua_error(L);
    }
#else
    if (size != 1 && size != 2 && size != 4)
    {
        lua_pushliteral(L, "\"\"参数错误。SetHardwareBreakpoint 中 size 仅支持1、2、4\n");
        return lua_error(L);
    }
#endif
    lua_pop(L, 1);
    lua_getcheckfield_("callback", LUA_TFUNCTION);
    int callback = luaL_ref(L, LUA_REGISTRYINDEX);
    S().set_hardware_breakpoint(ea, index, type[0], (DWORD)size, luaref_to_token(callback));
    assert(lua_gettop(L) == 1);
    return 0;
}

int lua_del_hardware_breakpoint(lua_State *L)
{
    if (lua_gettop(L) > 1 || (lua_gettop(L) == 1 && lua_type(L, 1) != LUA_TNUMBER))
    {
        lua_pushliteral(L, "\"\"参数错误。DelHardwareBreakpoint原型:\n"
                           "function DelHardwareBreakpoint(index: integer?): nil\n"
                           "  @param integer? index 硬件断点dr寄存器序号，支持0~3\n");
        return lua_error(L);
    }
    DWORD index = 0;
    if (lua_gettop(L) == 1)
        index = (DWORD)lua_tointeger(L, 1) + 1;
    if (index > 4)
    {
        lua_pushliteral(L, "\"\"参数错误, index 仅支持0~3\n"
                           "function DelHardwareBreakpoint(index: integer?): nil\n"
                           "  @param integer? index 硬件断点dr寄存器序号，\n");
        return lua_error(L);
    }
    if (index > 0)
    {
        int r = token_ref(S().hw_breakpoints()[index - 1].callback);
        if (r != LUA_NOREF)
            lua_unref(L, r);
        S().hw_breakpoints()[index - 1].callback = callback_ref_t{}; /* 防重复 unref */
    }
    else
        for (int i = 0; i < 4; i++)
        {
            int r = token_ref(S().hw_breakpoints()[i].callback);
            if (r != LUA_NOREF)
                lua_unref(L, r);
            S().hw_breakpoints()[i].callback = callback_ref_t{};
        }
    S().del_hardware_breakpoint(index);
    return 0;
}

/* P3.B 模块 8：`loadstr` 已搬到 `core/memory.cpp`（符号表原本就把它归 memory；
 * 模块 8 的 `get_cpp_class_name` 与这里的 `lua_load_str` 都要用）→ 声明见 `core/memory.hpp`。 */

int lua_load_str(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "\"LoadStr\"参数错误。LoadStr原型:\n"
                           "function DelBreakpoint(addr: LuaVariant::" UZ "): string?\n"
                           "  addr 断点地址\n");
        return lua_error(L);
    }
    size_t addr = lv_check(L, 1, LV_UZ)->val.uz;
    char  *s;
    size_t n;
    if (!S().loadstr(addr, &s, &n))
        lua_pushnil(L);
    else
    {
        lua_pushlstring(L, s, n);
        if (s)
            free(s);
    }
    return 1;
}

/* P3.B 模块 8：`DumpCppClassInfo2Lua` 的引擎部分（读目标内存 + 去修饰）已迁到
 * `core/stack_trace.cpp` 的 `get_cpp_class_name`；建表由调用点直接做。 */

void DumpExceptionRecord2Lua(lua_State *L, PEXCEPTION_RECORD record)
{
    lua_createtable(L, 0, 0);
    for (int i = 0; record; i++, record = record->ExceptionRecord)
    {
        lua_createtable(L, 0, 7);
        lv_push(L, LV_U32)->val.u32 = record->ExceptionCode;
        lua_setfield(L, -2, "code");
        {
            /* 异常码文案由 core 提供（P3.B 模块 8 去 Lua 化；文案表本身是**脚本抽取**的，未手抄）。
             * 未知码 name/info 都是 NULL → 与旧实现一致：**不设**这两个字段。 */
            const char *code_name = NULL;
            const char *code_info = NULL;
            describe_exception_code(record->ExceptionCode, &code_name, &code_info);
            if (code_name)
            {
                lua_pushstring(L, code_name);
                lua_setfield(L, -2, "code_name");
            }
            if (code_info)
            {
                lua_pushstring(L, code_info);
                lua_setfield(L, -2, "code_info");
            }
        }
        lv_push(L, LV_U32)->val.u32 = record->ExceptionFlags;
        lua_setfield(L, -2, "flag");
        lua_pushboolean(L, !!(record->ExceptionFlags & EXCEPTION_NONCONTINUABLE));
        lua_setfield(L, -2, "non_continuable");
        lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionAddress;
        lua_setfield(L, -2, "address");
        lua_createtable(L, 0, 0);
        switch (record->ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            assert(record->NumberParameters == 2);
            /* 操作类型名由 core 给（两处重复的 switch 已收敛到 exception_access_type_name） */
            {
                const char *tn = exception_access_type_name(record->ExceptionInformation[0]);
                if (tn)
                    lua_pushstring(L, tn);
                else
                    lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[0];
            }
            lua_setfield(L, -2, "type");
            lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[1];
            lua_setfield(L, -2, "address");
            break;
        case EXCEPTION_IN_PAGE_ERROR:
            assert(record->NumberParameters == 3);
            {
                const char *tn = exception_access_type_name(record->ExceptionInformation[0]);
                if (tn)
                    lua_pushstring(L, tn);
                else
                    lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[0];
            }
            lua_setfield(L, -2, "type");
            lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[1];
            lua_setfield(L, -2, "address");
            lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[2];
            lua_setfield(L, -2, "nt_status");
            break;
        case 0xE06D7363:
        { /* C++ 要求：case 标签不得跳过变量初始化（C 允许）→ 用花括号限定作用域 */
#ifdef _WIN64
            assert(record->NumberParameters == 4);
#else
            assert(record->NumberParameters == 3);
#endif
            assert(record->ExceptionInformation[0] == 0x19930520);
            lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[1];
            lua_setfield(L, -2, "obj_ptr");
            /* 类名读取在 core（get_cpp_class_name）。
             * ❗与旧实现的**有意差异**（D24）：旧代码只看“函数返回 true”就 setfield，
             *   而名字长度 ≤ 3 时函数返回 true 却**什么都不 push** → 那次 setfield 会
             *   把栈顶（information 表）当成 obj_class_name 塞进记录表（串表）。
             *   这里按 `has_name` 决定是否设字段（更正确，且对 Python/Node 宿主也自然）。 */
            {
                char class_name[0x1000];
                bool has_name = false;
                if (S().get_cpp_class_name((size_t)record->ExceptionInformation[1], class_name, sizeof class_name, &has_name) && has_name)
                {
                    lua_pushstring(L, class_name);
                    lua_setfield(L, -2, "obj_class_name");
                }
            }
            lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[2];
            lua_setfield(L, -2, "obj_type_info_ptr");
#ifdef _WIN64
            PEInfo *info = S().get_peinfo_by_ea(record->ExceptionInformation[3]);
            if (!info)
                lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[3];
            else
            {
                int   n;
                LPSTR s = utf16_to_utf8(info->Path + info->name_offset, &n);
                lua_pushlstring(L, s, n);
                free_from_tchar(s);
            }
            lua_setfield(L, -2, "module_hinstance");
#endif
            break;
        }
        default:
        {
            lua_createtable(L, record->NumberParameters, 0);
            for (DWORD j = 0; j < record->NumberParameters; j++)
            {
                lv_push(L, LV_UZ)->val.uz = (size_t)record->ExceptionInformation[j];
                lua_rawseti(L, -2, j + 1);
            }
            lua_setfield(L, -2, "args");
        }
        break;
        }
        lua_setfield(L, -2, "information");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "records");
}

/* P4.3：`pcall_pre` / `pcall` / `PCALL` 已搬到 `helpers.{hpp,cpp}`
 * （Plan P4.3：它们只服务于宿主适配器的回调派发）。`PCALL` 宏在 helpers.hpp 里定义，
 * 且把 `__FILE__` 一并传进去 —— 与旧实现写入日志的文件名逐字一致。 */

static int run_help(lua_State *L)
{
    lua_pushliteral(L, "\"Run\"参数错误。Run原型:\n"
                       "function Run(opt): nil\n"
                       "opt:\n"
                       "  @field string imagepath 程序路径\n"
                       "  @field string cmdline 命令行参数\n"
                       "  @field string cwd 当前目录\n"
                       "  @field table env 环境变量\n"
                       "  @field boolean is_new_console 是否启动新的终端?\n"
                       "  @field function(pid: LuaVariant, tid: LuaVariant):void callback 入口断点\n"
                       "  @field function(info: table):void exception_callback 异常断点\n");
    return lua_error(L);
}

/* =============================================================================
 * Lua 绑定层适配器（A.5b）—— 把 core 的事件回调契约接回 Lua
 *
 * `run` 现在只认识 `core::RunParams`（纯数据）+ `core::DebugCallbacks`
 * （函数指针 + ctx）。下面这些函数把旧实现里**内联在 run 中的 Lua 代码原样搬出**
 * （pcall + 建表 + 推字段的顺序一律不变，否则黄金会红），只把取值来源从
 * `DBGEvent` 换成事件结构体。
 *
 * 注：`pcall()` 会吞掉 Lua 错误（只 log_msg、从不重抛）→ 回调不会长跳穿引擎，
 * 因此引擎侧无需任何异常处理（见 Plan C.10 / D18）。
 * ========================================================================== */
struct LuaDebugAdapter
{
    struct lua_State *L;
    int               callback;           /* 首次断点（一次性） */
    int               exception_callback; /* user exception（一次性） */
    int               on_any_debug_event;
    int               on_exception;
    int               on_breakpoint;
    int               on_single_step;
    int               on_create_thread;
    int               on_create_process;
    int               on_exit_thread;
    int               on_exit_process;
    int               on_load_dll;
    int               on_unload_dll;
    int               on_output_debug_string;
    int               on_rip;
    int               on_console_event;
};

static void lua_on_any_debug_event(void *ctx, const core::DebugEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_any_debug_event == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_any_debug_event);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_U32)->val.u32 = ev.code;
    lua_setfield(a->L, -2, "code");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_exception(void *ctx, const core::ExceptionEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_exception == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_exception);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_U32)->val.u32 = ev.code;
    lua_setfield(a->L, -2, "code");
    lv_push(a->L, LV_UZ)->val.uz = ev.addr;
    lua_setfield(a->L, -2, "addr");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_breakpoint(void *ctx, const core::BreakpointEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_breakpoint == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_breakpoint);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_UZ)->val.uz = ev.addr;
    lua_setfield(a->L, -2, "addr");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

/* 首次断点：旧实现是**直接传两个参数**（不是建表） */
static void lua_on_first_breakpoint(void *ctx, const core::FirstBreakpointEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->callback == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->callback);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    PCALL(a->L, 2, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_single_step(void *ctx, const core::StepEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_single_step == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_single_step);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_UZ)->val.uz = ev.addr;
    lua_setfield(a->L, -2, "addr");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

/* 断点自身回调：令牌 → Lua ref 的转换在这里；single_shot 时释放引用
 * （旧实现是在 PCALL 之后 unref，这里保持同序） */
static void lua_on_breakpoint_callback(void *ctx, const core::BreakpointCallbackEvent &ev)
{
    LuaDebugAdapter *a        = (LuaDebugAdapter *)ctx;
    int              callback = token_ref(ev.callback);
    if (callback == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, callback);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 5);

    lv_push(a->L, LV_UZ)->val.uz = ev.ea;
    lua_setfield(a->L, -2, "ea");

    LPSTR modname_utf8 = (LPSTR)tchar_to_utf8(ev.modname);
    lua_pushstring(a->L, modname_utf8);
    lua_setfield(a->L, -2, "modname");
    free_from_tchar(modname_utf8);

    lv_push(a->L, LV_U32)->val.u32 = ev.rva;
    lua_setfield(a->L, -2, "rva");

    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");

    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");

    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
    if (ev.single_shot)
        lua_unref(a->L, callback);
}

static void lua_on_hw_breakpoint_callback(void *ctx, const core::HwBreakpointCallbackEvent &ev)
{
    LuaDebugAdapter *a        = (LuaDebugAdapter *)ctx;
    int              callback = token_ref(ev.callback);
    if (callback == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, callback);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 3);

    lv_push(a->L, LV_UZ)->val.uz = ev.ea;
    lua_setfield(a->L, -2, "ea");

    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");

    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");

    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_user_exception(void *ctx, const core::UserExceptionEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->exception_callback == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->exception_callback);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 4);

    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");

    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");

    lua_pushboolean(a->L, ev.first_chance ? 1 : 0);
    lua_setfield(a->L, -2, "first_chance");

    DumpExceptionRecord2Lua(a->L, (PEXCEPTION_RECORD)ev.record);

    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_create_thread(void *ctx, const core::ThreadEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_create_thread == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_create_thread);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_UZ)->val.uz = ev.addr;
    lua_setfield(a->L, -2, "addr");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_create_process(void *ctx, const core::ProcessEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_create_process == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_create_process);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_UZ)->val.uz = ev.addr;
    lua_setfield(a->L, -2, "addr");
    lv_push(a->L, LV_UZ)->val.uz = ev.base;

    lua_setfield(a->L, -2, "base");
    if (ev.path)
    {
        LPSTR path = (LPSTR)tchar_to_utf8(ev.path);
        lua_pushstring(a->L, path);
        free_from_tchar(path);
        lua_setfield(a->L, -2, "path");
    }
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_exit_thread(void *ctx, const core::ExitEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_exit_thread == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_exit_thread);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_U32)->val.u32 = ev.code;
    lua_setfield(a->L, -2, "code");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_exit_process(void *ctx, const core::ExitEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_exit_process == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_exit_process);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_U32)->val.u32 = ev.code;
    lua_setfield(a->L, -2, "code");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_load_dll(void *ctx, const core::DllEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_load_dll == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_load_dll);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_UZ)->val.uz = ev.base;
    lua_setfield(a->L, -2, "base");
    if (ev.path)
    {
        LPSTR path = (LPSTR)tchar_to_utf8(ev.path);
        lua_pushstring(a->L, path);
        free_from_tchar(path);
        lua_setfield(a->L, -2, "path");
    }
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_unload_dll(void *ctx, const core::DllEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_unload_dll == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_unload_dll);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    lv_push(a->L, LV_UZ)->val.uz = ev.base;
    lua_setfield(a->L, -2, "base");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

/* ODS：`str` 字段的构建与旧实现逐字一致（含 ANSI 构建下宽字符**不**推 str 的现状）*/
static void lua_on_output_debug_string(void *ctx, const core::OutputDebugStringEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_output_debug_string == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_output_debug_string);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    if (ev.data)
    {
        if (!ev.unicode)
        {
            lua_pushlstring(a->L, (LPCSTR)ev.data, ev.size);
            lua_setfield(a->L, -2, "str");
        }
#ifdef _UNICODE
        else
        {
            int   sz = (int)ev.size / 2;
            LPSTR s  = utf16_to_utf8_n((LPCWSTR)ev.data, &sz);
            lua_pushlstring(a->L, s, sz);
            free_(s);
            lua_setfield(a->L, -2, "str");
        }
#endif
    }
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

static void lua_on_rip(void *ctx, const core::DebugEvent &ev)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;
    if (a->on_rip == LUA_NOREF)
        return;
    int n = pcall_pre(a->L);
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->on_rip);
    luaL_checktype(a->L, -1, LUA_TFUNCTION);
    lua_createtable(a->L, 0, 0);
    lv_push(a->L, LV_U32)->val.u32 = ev.pid;
    lua_setfield(a->L, -2, "pid");
    lv_push(a->L, LV_U32)->val.u32 = ev.tid;
    lua_setfield(a->L, -2, "tid");
    PCALL(a->L, 1, 0, n);
    lua_pop(a->L, 1); // 弹出 traceback
}

/* ---------------------------------------------------------------------------
 * 空闲轮询（Q27）——「未等到调试事件」时的宿主策略
 *
 * core 的 `run` 在 `WaitForDebugEvent` 超时（100ms）时不再自己读控制台，而是调用
 * `DebugCallbacks::on_wait_timeout` 问宿主：「继续等 / 分离并终止 / 分离但保留目标」。
 * 引擎只执行结论；**控制台 I/O 策略属宿主层**，所以 `CheckConsoleInput` 留在本文件。
 * 行为 1:1：按键判定（'Q' 键抬起；Shift 决定是否保留目标）与旧实现逐字相同。
 *
 * 注：本回调**不是**用户脚本可配的通道（用户能配的仍是 `S().run(opt)` 里那些）
 *   —— 它是宿主内置策略，所以不经过 `pcall`/`lua_rawgeti`，直接调。
 * ------------------------------------------------------------------------- */

static void SetKeyState(struct lua_State *L, DWORD dwControlKeyState)
{
    lua_createtable(L, 0, 0);
    if (dwControlKeyState & RIGHT_ALT_PRESSED)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "left_alt");
    }
    if (dwControlKeyState & LEFT_ALT_PRESSED)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "right_alt");
    }
    if (dwControlKeyState & RIGHT_CTRL_PRESSED)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "left_ctrl");
    }
    if (dwControlKeyState & LEFT_CTRL_PRESSED)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "right_ctrl");
    }
    if (dwControlKeyState & SHIFT_PRESSED)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "shift");
    }
    if (dwControlKeyState & NUMLOCK_ON)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "numlock_on");
    }
    if (dwControlKeyState & SCROLLLOCK_ON)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "scrolllock_on");
    }
    if (dwControlKeyState & CAPSLOCK_ON)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "capslock_on");
    }
    if (dwControlKeyState & ENHANCED_KEY)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "enhanced_key");
    }
    if (dwControlKeyState & NLS_DBCSCHAR)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "nls_dbcschar");
    }
    if (dwControlKeyState & NLS_KATAKANA)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "nls_katakana");
    }
    if (dwControlKeyState & NLS_HIRAGANA)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "nls_hiragana");
    }
    if (dwControlKeyState & NLS_ROMAN)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "nls_roman");
    }
    if (dwControlKeyState & NLS_IME_CONVERSION)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "nls_ime_conversion");
    }
    if (dwControlKeyState & ALTNUMPAD_BIT)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "altnumpad_bit");
    }
    if (dwControlKeyState & NLS_IME_DISABLE)
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "nls_ime_disable");
    }
    lua_setfield(L, -2, "key_state");
}

static const char *vk_names[] = {
    nullptr,
    "lbutton",
    "rbutton",
    "cancel",
    "mbutton",
    "xbutton1",
    "xbutton2",
    nullptr,
    "back",
    "tab",
    nullptr,
    nullptr,
    "clear",
    "return",
    nullptr,
    nullptr,
    "shift",
    "control",
    "menu",
    "pause",
    "capital",
    "kana",
    "ime_on",
    "junja",
    "final",
    "hanja",
    "ime_off",
    "escape",
    "convert",
    "nonconvert",
    "accept",
    "modechange",
    "space",
    "prior",
    "next",
    "end",
    "home",
    "left",
    "up",
    "right",
    "down",
    "select",
    "print",
    "execute",
    "snapshot",
    "insert",
    "delete",
    "help",
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    "a",
    "b",
    "c",
    "d",
    "e",
    "f",
    "g",
    "h",
    "i",
    "j",
    "k",
    "l",
    "m",
    "n",
    "o",
    "p",
    "q",
    "r",
    "s",
    "t",
    "u",
    "v",
    "w",
    "x",
    "y",
    "z",
    "lwin",
    "rwin",
    "apps",
    nullptr,
    "sleep",
    "numpad0",
    "numpad1",
    "numpad2",
    "numpad3",
    "numpad4",
    "numpad5",
    "numpad6",
    "numpad7",
    "numpad8",
    "numpad9",
    "multiply",
    "add",
    "separator",
    "subtract",
    "decimal",
    "divide",
    "f1",
    "f2",
    "f3",
    "f4",
    "f5",
    "f6",
    "f7",
    "f8",
    "f9",
    "f10",
    "f11",
    "f12",
    "f13",
    "f14",
    "f15",
    "f16",
    "f17",
    "f18",
    "f19",
    "f20",
    "f21",
    "f22",
    "f23",
    "f24",
    "navigation_view",
    "navigation_menu",
    "navigation_up",
    "navigation_down",
    "navigation_left",
    "navigation_right",
    "navigation_accept",
    "navigation_cancel",
    "numlock",
    "scroll",
    "oem_nec_equal",
    "oem_fj_masshou",
    "oem_fj_touroku",
    "oem_fj_loya",
    "oem_fj_roya",
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    "lshift",
    "rshift",
    "lcontrol",
    "rcontrol",
    "lmenu",
    "rmenu",
    "browser_back",
    "browser_forward",
    "browser_refresh",
    "browser_stop",
    "browser_search",
    "browser_favorites",
    "browser_home",
    "volume_mute",
    "volume_down",
    "volume_up",
    "media_next_track",
    "media_prev_track",
    "media_stop",
    "media_play_pause",
    "launch_mail",
    "launch_media_select",
    "launch_app1",
    "launch_app2",
    nullptr,
    nullptr,
    "oem_1",
    "oem_plus",
    "oem_comma",
    "oem_minus",
    "oem_period",
    "oem_2",
    "oem_3",
    nullptr,
    nullptr,
    "gamepad_a",
    "gamepad_b",
    "gamepad_x",
    "gamepad_y",
    "gamepad_right_shoulder",
    "gamepad_left_shoulder",
    "gamepad_left_trigger",
    "gamepad_right_trigger",
    "gamepad_dpad_up",
    "gamepad_dpad_down",
    "gamepad_dpad_left",
    "gamepad_dpad_right",
    "gamepad_menu",
    "gamepad_view",
    "gamepad_left_thumbstick_button",
    "gamepad_right_thumbstick_button",
    "gamepad_left_thumbstick_up",
    "gamepad_left_thumbstick_down",
    "gamepad_left_thumbstick_right",
    "gamepad_left_thumbstick_left",
    "gamepad_right_thumbstick_up",
    "gamepad_right_thumbstick_down",
    "gamepad_right_thumbstick_right",
    "gamepad_right_thumbstick_left",
    "oem_4",
    "oem_5",
    "oem_6",
    "oem_7",
    "oem_8",
    nullptr,
    "oem_ax",
    "oem_102",
    "ico_help",
    "ico_00",
    "processkey",
    "ico_clear",
    "packet",
    nullptr,
    "oem_reset",
    "oem_jump",
    "oem_pa1",
    "oem_pa2",
    "oem_pa3",
    "oem_wsctrl",
    "oem_cusel",
    "oem_attn",
    "oem_finish",
    "oem_copy",
    "oem_auto",
    "oem_enlw",
    "oem_backtab",
    "attn",
    "crsel",
    "exsel",
    "ereof",
    "play",
    "zoom",
    "noname",
    "pa1",
    "oem_clear",
};

/**
 * 检查控制台输入，处理退出信号
 * @return
 *  0: continue
 *  1: detach and term
 *  2: detach and not term
 */
static int CheckConsoleInput(struct lua_State *L, int on_console_event, bool can_detach_without_term)
{
    int    ret = 0;
    DWORD  num_events;
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetNumberOfConsoleInputEvents(hStdIn, &num_events))
    {
        E("GetNumberOfConsoleInputEvents");
        return 1;
    }
    if (num_events > 0)
    {
        INPUT_RECORD *input_records = AllocX(INPUT_RECORD, num_events);
        if (input_records)
        {
            DWORD num_read;
            if (!ReadConsoleInput(hStdIn, input_records, num_events, &num_read))
            {
                E("ReadConsoleInput");
                return 1;
            }
            if (num_events != num_read)
                puts("num_events != num_read");
            else
            {
                if (on_console_event != LUA_NOREF && num_events > 0)
                {
                    int n = pcall_pre(L);
                    lua_rawgeti(L, LUA_REGISTRYINDEX, on_console_event);
                    luaL_checktype(L, -1, LUA_TFUNCTION);
                    lua_createtable(L, num_events, 0);
                    for (DWORD i = 0; i < num_events; i++)
                    {
                        const INPUT_RECORD *e = &input_records[i];
                        lua_createtable(L, 0, 0);
                        switch (e->EventType)
                        {
                        case KEY_EVENT: // Event contains key event record
                            lua_pushliteral(L, "key");
                            lua_setfield(L, -2, "type");

                            lua_pushboolean(L, e->Event.KeyEvent.bKeyDown);
                            lua_setfield(L, -2, "down");

                            lv_push(L, LV_U16)->val.u16 = e->Event.KeyEvent.wRepeatCount;
                            lua_setfield(L, -2, "repeat_count");

                            if (0 < e->Event.KeyEvent.wVirtualKeyCode && e->Event.KeyEvent.wVirtualKeyCode < _countof(vk_names) &&
                                vk_names[e->Event.KeyEvent.wVirtualKeyCode])
                                lua_pushstring(L, vk_names[e->Event.KeyEvent.wVirtualKeyCode]);
                            else
                                lv_push(L, LV_U16)->val.u16 = e->Event.KeyEvent.wVirtualKeyCode;
                            lua_setfield(L, -2, "vk_code");

                            lv_push(L, LV_U16)->val.u16 = e->Event.KeyEvent.wVirtualScanCode;
                            lua_setfield(L, -2, "vs_code");

#ifdef _UNICODE
                            char buf[8];
                            int  l;
                            l = WideCharToMultiByte(CP_UTF8, 0, &e->Event.KeyEvent.uChar.UnicodeChar, 1, buf, 8, nullptr, nullptr);
                            lua_pushlstring(L, buf, l);
#else
                            lua_pushlstring(L, &e->Event.KeyEvent.uChar.AsciiChar, 1);
#endif
                            lua_setfield(L, -2, "char");

                            SetKeyState(L, e->Event.KeyEvent.dwControlKeyState);
                            break;
                        case MOUSE_EVENT: // Event contains mouse event record
                            lua_pushliteral(L, "mouse");
                            lua_setfield(L, -2, "type");
                            lua_createtable(L, 0, 2);
                            lv_push(L, LV_I16)->val.i16 = e->Event.MouseEvent.dwMousePosition.X;
                            lua_setfield(L, -2, "x");
                            lv_push(L, LV_I16)->val.i16 = e->Event.MouseEvent.dwMousePosition.Y;
                            lua_setfield(L, -2, "y");
                            lua_setfield(L, -2, "pos");

                            lua_createtable(L, 0, 0);
                            if (e->Event.MouseEvent.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "from_left_1st_button_pressed");
                            }
                            if (e->Event.MouseEvent.dwButtonState & RIGHTMOST_BUTTON_PRESSED)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "rightmost_button_pressed");
                            }
                            if (e->Event.MouseEvent.dwButtonState & FROM_LEFT_2ND_BUTTON_PRESSED)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "from_left_2nd_button_pressed");
                            }
                            if (e->Event.MouseEvent.dwButtonState & FROM_LEFT_3RD_BUTTON_PRESSED)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "from_left_3rd_button_pressed");
                            }
                            if (e->Event.MouseEvent.dwButtonState & FROM_LEFT_4TH_BUTTON_PRESSED)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "from_left_4th_button_pressed");
                            }
                            lua_setfield(L, -2, "btn_state");

                            SetKeyState(L, e->Event.MouseEvent.dwControlKeyState);

                            lua_createtable(L, 0, 0);
                            if (e->Event.MouseEvent.dwEventFlags & MOUSE_MOVED)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "mouse_moved");
                            }
                            if (e->Event.MouseEvent.dwEventFlags & DOUBLE_CLICK)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "double_click");
                            }
                            if (e->Event.MouseEvent.dwEventFlags & MOUSE_WHEELED)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "mouse_wheeled");
                            }
                            if (e->Event.MouseEvent.dwEventFlags & MOUSE_HWHEELED)
                            {
                                lua_pushboolean(L, true);
                                lua_setfield(L, -2, "mouse_hwheeled");
                            }
                            lua_setfield(L, -2, "event_flag");
                            break;
                        case WINDOW_BUFFER_SIZE_EVENT: // Event contains window change event record
                            lua_pushliteral(L, "window_buffer_size");
                            lua_setfield(L, -2, "type");
                            lv_push(L, LV_I16)->val.i16 = e->Event.WindowBufferSizeEvent.dwSize.X;
                            lua_setfield(L, -2, "width");
                            lv_push(L, LV_I16)->val.i16 = e->Event.WindowBufferSizeEvent.dwSize.Y;
                            lua_setfield(L, -2, "height");
                            break;
                        case MENU_EVENT: // Event contains menu event record
                            lua_pushliteral(L, "menu");
                            lua_setfield(L, -2, "type");
                            lv_push(L, LV_U32)->val.u32 = e->Event.MenuEvent.dwCommandId;
                            lua_setfield(L, -2, "cmdid");
                            break;
                        case FOCUS_EVENT: // event contains focus change
                            lua_pushliteral(L, "focus");
                            lua_setfield(L, -2, "type");
                            lua_pushboolean(L, e->Event.FocusEvent.bSetFocus);
                            lua_setfield(L, -2, "set_focus");
                            break;
                        }
                        lua_rawseti(L, -2, i + 1);
                    }
                    PCALL(L, 1, 0, n);
                    lua_pop(L, 1);
                }
                /* 判定段已抽到 `console_input.cpp`（纯函数）：宿主侧判定可单测，
                 * 引擎侧「拿到结论怎么做」归模块 9 —— 两边都不需要控制台（Q27）。 */
                ret = decide_console_action(input_records, num_events, can_detach_without_term);
            }
            free_(input_records);
        }
    }
    return ret;
}

static int lua_on_wait_timeout(void *ctx, bool can_detach_without_term)
{
    LuaDebugAdapter *a = (LuaDebugAdapter *)ctx;

    /* S2（plan_server.md §5.4）：server 模式（有远程执行在进行）下**必须**先问守卫。
     * 原路径 `CheckConsoleInput` 在无控制台时会返回 1（分离并终止）——在 server 里
     * 等于把被调试进程直接杀掉；而且它还会去 `ReadConsoleInput`，那不是远程会话该干的事。 */
    if (autodbg::host::guard_exec_active())
    {
        if (!autodbg::host::guard_should_stop())
            return autodbg::core::WAIT_TIMEOUT_CONTINUE;
        /* 取消：能保留目标就保留（调试器不该因为「脚本被取消」而杀掉目标进程） */
        return can_detach_without_term ? autodbg::core::WAIT_TIMEOUT_DETACH_KEEP_ALIVE
                                       : autodbg::core::WAIT_TIMEOUT_DETACH_AND_TERMINATE;
    }

    return CheckConsoleInput(a->L, a->on_console_event, can_detach_without_term);
}

/* 断点销毁通知：core 内所有删除路径统一走它 → 这里就是**唯一**的令牌释放点 */
static void lua_on_breakpoint_destroyed(void *ctx, callback_ref_t callback)
{
    LuaDebugAdapter *a   = (LuaDebugAdapter *)ctx;
    int              ref = token_ref(callback);
    if (ref != LUA_NOREF)
        lua_unref(a->L, ref);
}

static void lua_on_hw_breakpoint_destroyed(void *ctx, callback_ref_t callback)
{
    LuaDebugAdapter *a   = (LuaDebugAdapter *)ctx;
    int              ref = token_ref(callback);
    if (ref != LUA_NOREF)
        lua_unref(a->L, ref);
}

static void lua_make_callbacks(LuaDebugAdapter *a, core::DebugCallbacks *cb)
{
    ZeroMem(*cb);
    cb->context                    = a;
    cb->on_any_debug_event         = lua_on_any_debug_event;
    cb->on_exception               = lua_on_exception;
    cb->on_breakpoint              = lua_on_breakpoint;
    cb->on_single_step             = lua_on_single_step;
    cb->on_create_thread           = lua_on_create_thread;
    cb->on_create_process          = lua_on_create_process;
    cb->on_exit_thread             = lua_on_exit_thread;
    cb->on_exit_process            = lua_on_exit_process;
    cb->on_load_dll                = lua_on_load_dll;
    cb->on_unload_dll              = lua_on_unload_dll;
    cb->on_output_debug_string     = lua_on_output_debug_string;
    cb->on_rip                     = lua_on_rip;
    cb->on_first_breakpoint        = lua_on_first_breakpoint;
    cb->on_user_exception          = lua_on_user_exception;
    cb->on_breakpoint_callback     = lua_on_breakpoint_callback;
    cb->on_hw_breakpoint_callback  = lua_on_hw_breakpoint_callback;
    cb->on_breakpoint_destroyed    = lua_on_breakpoint_destroyed;
    cb->on_hw_breakpoint_destroyed = lua_on_hw_breakpoint_destroyed;
    /* 空闲轮询（Q27）：**宿主内置策略**，不是用户脚本回调（上面 17 条才是） */
    cb->on_wait_timeout = lua_on_wait_timeout;
    /* 断点销毁：**唯一**的释放点（旧实现里散在 5 处的 lua_unref 收敛到这里） */
}

#define try_get_func(key)                                 \
    do                                                    \
    {                                                     \
        if (lua_getfield_(#key, LUA_TFUNCTION))           \
            adapter.key = luaL_ref(L, LUA_REGISTRYINDEX); \
        else                                              \
        {                                                 \
            lua_pop(L, 1);                                \
            adapter.key = LUA_NOREF;                      \
        }                                                 \
        assert(lua_gettop(L) == 1);                       \
    } while (0)

/* P3.B 模块 9：`run_shutdown` + `run` 已迁到 `core/debug_loop.cpp`（绑定层只留
 * `lua_run`/`lua_attach` 的参数编组与 `lua_on_*` 适配器）。 */

int lua_run(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return run_help(L);
    core::RunParams      params;
    LuaDebugAdapter      adapter;
    core::DebugCallbacks cb;
    ZeroMem(params);
    ZeroMem(adapter);
    adapter.L = L;
    lua_make_callbacks(&adapter, &cb);
    if (lua_getfield_("cmdline", LUA_TSTRING))
        params.cmdline = get_lua_str(L, -1);
    lua_pop(L, 1);
    assert(lua_gettop(L) == 1);
    if (!params.cmdline)
    {
        if (lua_getfield_("imagepath", LUA_TSTRING))
            params.imagepath = get_lua_str(L, -1);
        lua_pop(L, 1);
        if (!params.imagepath)
            return run_help(L);
    }
    assert(lua_gettop(L) == 1);
    if (lua_getfield_("cwd", LUA_TSTRING))
        params.cwd = get_lua_str(L, -1);
    lua_pop(L, 1);
    assert(lua_gettop(L) == 1);
    // TODO
    // if (lua_getfield(L, 1, "env") == LUA_TTABLE)
    // {
    //     LPWCH vars = GetEnvironmentStrings();
    //     struct ListNode
    //     {
    //         struct ListNode *next;
    //         LPTSTR key;
    //         LPTSTR value;
    //     };
    //     struct ListNode _t, *head = &_t;
    //     for (LPWCH p = vars; *p; p++)
    //     {
    //         for (LPWCH q = p; *q != '='; q++);
    //     }
    // }
    // lua_pop(L, 1);
    if (lua_getfield_("is_new_console", LUA_TBOOLEAN) && lua_toboolean(L, -1))
        params.is_new_console = true;
    lua_pop(L, 1);
    if (lua_getfield_("use_scylla_hide", LUA_TBOOLEAN) && lua_toboolean(L, -1))
        params.use_scylla_hide = true;
    lua_pop(L, 1);
    assert(lua_gettop(L) == 1);
    try_get_func(callback);
    try_get_func(exception_callback);
    try_get_func(on_any_debug_event);
    try_get_func(on_exception);
    try_get_func(on_breakpoint);
    try_get_func(on_single_step);
    try_get_func(on_create_thread);
    try_get_func(on_create_process);
    try_get_func(on_exit_thread);
    try_get_func(on_exit_process);
    try_get_func(on_load_dll);
    try_get_func(on_unload_dll);
    try_get_func(on_output_debug_string);
    try_get_func(on_rip);
    try_get_func(on_console_event);
    const char *err = NULL;
    if (S().run(&params, &cb, &err) != 0)
    {
        /* 引擎内部错误：旧实现是在 run 内直接 lua_error（长跳、跳过清理）。
         * 现在引擎已走完清理并正常返回，这里再抛 —— Lua 可见行为一致（D18）。 */
        lua_pushstring(L, err);
        return lua_error(L);
    }
    free_from_tchar(params.imagepath);
    free_from_tchar(params.cmdline);
    free_from_tchar(params.cwd);
    if (adapter.callback != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, adapter.callback);
    if (adapter.exception_callback != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, adapter.exception_callback);
    return 0;
}

static int attach_help(lua_State *L)
{
    lua_pushliteral(L, "\"Attach\"参数错误。Attach原型:\n"
                       "function Attach(callback: function): nil\n"
                       "function Attach(opt): nil\n"
                       "opt:\n"
                       "  @field integer? pid 进程pid\n"
                       "  @field function(pid: LuaVariant, tid: LuaVariant):void callback 入口断点\n"
                       "  @field function(info: table):void exception_callback 异常断点\n"
                       "\n"
                       "未传pid时：优先使用上次附加的PID(若进程仍存活)，\n"
                       "否则鼠标移至目标窗口按Ctrl锁定进程");
    return lua_error(L);
}

/* P3.B 模块 7：`FlashWin` / `find_pid_by_mouse` 已迁到 `core/process.cpp`
 * （引擎不再感知 Lua：选不到窗口时返回错误文案，由下面的 lua_attach 负责 lua_error）。 */
int lua_attach(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return attach_help(L);
    LPCTSTR              suffix = TEXT(".attach.last.pid.bin");
    core::RunParams      params;
    LuaDebugAdapter      adapter;
    core::DebugCallbacks cb;
    ZeroMem(params);
    ZeroMem(adapter);
    adapter.L = L;
    lua_make_callbacks(&adapter, &cb);
    if (lua_type(L, 1) == LUA_TFUNCTION)
    {
        adapter.exception_callback     = LUA_NOREF;
        adapter.exception_callback     = LUA_NOREF; /* user exception（一次性） */
        adapter.on_any_debug_event     = LUA_NOREF;
        adapter.on_exception           = LUA_NOREF;
        adapter.on_breakpoint          = LUA_NOREF;
        adapter.on_single_step         = LUA_NOREF;
        adapter.on_create_thread       = LUA_NOREF;
        adapter.on_create_process      = LUA_NOREF;
        adapter.on_exit_thread         = LUA_NOREF;
        adapter.on_exit_process        = LUA_NOREF;
        adapter.on_load_dll            = LUA_NOREF;
        adapter.on_unload_dll          = LUA_NOREF;
        adapter.on_output_debug_string = LUA_NOREF;
        adapter.on_rip                 = LUA_NOREF;
        adapter.on_console_event       = LUA_NOREF;
        lua_pushvalue(L, 1);
        adapter.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    else if (lua_type(L, 1) == LUA_TTABLE)
    {
        if (lua_getfield_("pid", LUA_TNUMBER))
            params.pid = (DWORD)lua_tointeger(L, -1);
        else
        {
            params.pid = get_last_pid(g_wfilepath, suffix);
            if (params.pid)
            {
                unique_handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, params.pid));
                if (!h)
                    params.pid = 0;
                else
                {
                    DWORD exit_code;
                    if (!GetExitCodeProcess(h.get(), &exit_code) || exit_code != STILL_ACTIVE)
                        params.pid = 0;
                    else
                        PrintF(TEXT("使用上次 PID: %lu\n"), params.pid);
                }
            }
        }
        lua_pop(L, 1);
        try_get_func(callback);
        try_get_func(exception_callback);
        try_get_func(on_any_debug_event);
        try_get_func(on_exception);
        try_get_func(on_breakpoint);
        try_get_func(on_single_step);
        try_get_func(on_create_thread);
        try_get_func(on_create_process);
        try_get_func(on_exit_thread);
        try_get_func(on_exit_process);
        try_get_func(on_load_dll);
        try_get_func(on_unload_dll);
        try_get_func(on_output_debug_string);
        try_get_func(on_rip);
        try_get_func(on_console_event);
    }
    else
        return attach_help(L);
    if (!params.pid)
    {
        /* 1:1 对应旧实现里 `find_pid_by_mouse(L)` 内部的 lua_error（错误文案与时机相同） */
        const char *mouse_err = NULL;
        params.pid            = find_pid_by_mouse(&mouse_err);
        if (mouse_err)
        {
            lua_pushstring(L, mouse_err);
            return lua_error(L);
        }
    }
    if (!params.pid)
        return 0;
    save_last_pid(g_wfilepath, params.pid, suffix);
    const char *err = NULL;
    if (S().run(&params, &cb, &err) != 0)
    {
        lua_pushstring(L, err);
        return lua_error(L);
    }
    if (adapter.callback != LUA_NOREF)
        lua_unref(L, adapter.callback);
    if (adapter.exception_callback != LUA_NOREF)
        lua_unref(L, adapter.exception_callback);
    return 0;
}

int lua_detach(lua_State *L)
{
    S().to_detach() = true;
    return 0;
}

int lua_kill(lua_State *L)
{
    S().cont() = false;
    return 0;
}

int lua_force_kill(lua_State *L)
{
    S().cont() = false;
    if (S().proc().hProcess)
        TerminateProcess(S().proc().hProcess, 0);
    if (S().proc().hThread)
        TerminateThread(S().proc().hThread, 0);
    return 0;
}

static void V(lua_State *L, size_t data)
{
    lua_newuserdata(L, sizeof(size_t));
    __movsb((BYTE *)lua_touserdata(L, -1), (const BYTE *)&data, sizeof(size_t));
    luaL_getmetatable(L, "Var");
    lua_setmetatable(L, -2);
}

/* M.Var implementation */
int lua_v(lua_State *L)
{
    V(L, lv_check(L, 1, LV_UZ)->val.uz);
    return 1;
}

int lua_var_index(lua_State *L)
{
    /* 获取self和key */
    void  *ud   = luaL_checkudata(L, 1, "Var");
    size_t base = *(size_t *)ud;

    size_t addr = 0;
    if (lua_type(L, 2) == LUA_TUSERDATA)
    {
        struct LuaVariant *lv = lv_check(L, 2, LV_IZ);
        if (!lv)
            return 0;
        if (lv->type != LV_IZ)
        {
            lua_pushstring(L, "arg #1 lv->type is not iz");
            return lua_error(L);
        }
        addr = base + lv->val.iz;
    }
    else if (lua_type(L, 2) == LUA_TNUMBER)
        addr = base + (SSIZE_T)lua_tointeger(L, 2);
    if (addr)
    {
        /* 读取指针大小的数据 */
        size_t ptr_value;
        if (S().read(addr, &ptr_value, sizeof(size_t)))
            V(L, ptr_value);
        else
            lua_pushnil(L);
        return 1;
    }
    if (lua_type(L, 2) == LUA_TSTRING && !strcmp(lua_tolstring(L, 2, NULL), "addr"))
    {
        lv_push(L, LV_UZ)->val.uz = base;
        return 1;
    }

    /* 处理其他方法 */
    luaL_getmetatable(L, "Var");
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
    return 1;
}

int lua_var_off(lua_State *L)
{
    /* 获取self和key */
    void  *ud   = luaL_checkudata(L, 1, "Var");
    size_t base = *(size_t *)ud;
    if (!base)
        return -1;
    size_t addr;
    if (lua_type(L, 2) == LUA_TUSERDATA)
    {
        struct LuaVariant *lv = lv_check(L, 2, LV_IZ);
        if (!lv)
            return 0;
        if (lv->type != LV_IZ)
        {
            lua_pushstring(L, "arg #1 lv->type is not iz");
            return lua_error(L);
        }
        addr = base + lv->val.iz;
    }
    else if (lua_type(L, 2) == LUA_TNUMBER)
        addr = base + (SSIZE_T)lua_tointeger(L, 2);
    V(L, addr);
    return 1;
}

int lua_var_to(lua_State *L)
{
    void  *ud   = luaL_checkudata(L, 1, "Var");
    size_t base = *(size_t *)ud;
    if (!base)
        return 0;

    bool check = true;
    check      = check && (lua_gettop(L) == 2 || lua_gettop(L) == 3);
    check      = check && lua_type(L, 2) == LUA_TSTRING;
    check      = check && (lua_gettop(L) == 2 || lua_type(L, 3) == LUA_TNUMBER || lua_type(L, 3) == LUA_TUSERDATA);
    if (!check)
    {
        lua_pushstring(L, "Var:to(type: string, off: (LuaVariant|number)?): LuaVariant");
        return lua_error(L);
    }
    const char *type_str = luaL_checkstring(L, 2);
    size_t      addr     = base;
    if (lua_type(L, 3) == LUA_TUSERDATA)
    {
        struct LuaVariant *lv = lv_check(L, 3, LV_IZ);
        if (!lv)
            return 0;
        if (lv->type != LV_IZ)
        {
            lua_pushstring(L, "arg #2 lv->type is not iz");
            return lua_error(L);
        }
        addr += lv->val.iz;
    }
    else if (lua_type(L, 3) == LUA_TNUMBER)
        addr += (SSIZE_T)lua_tointeger(L, 3);

    size_t       type_size;
    enum LV_type type;
    switch (type_str[0])
    {
    case 'c':
        type_size = sizeof(char);
        type      = LV_I8;
        break;
    case 'b':
        type_size = sizeof(unsigned char);
        type      = LV_U8;
        break;
    case 'h':
        type_size = sizeof(short);
        type      = LV_I16;
        break;
    case 'H':
        type_size = sizeof(unsigned short);
        type      = LV_U16;
        break;
    case 'i':
        type_size = sizeof(int);
        type      = LV_I32;
        break;
    case 'I':
        type_size = sizeof(unsigned int);
        type      = LV_U32;
        break;
    case 'q':
        type_size = sizeof(long long);
        type      = LV_I64;
        break;
    case 'Q':
        type_size = sizeof(unsigned long long);
        type      = LV_U64;
        break;
    case 'z':
        type_size = sizeof(size_t);
        type      = LV_UZ;
        break;
    case 'f':
        type_size = sizeof(float);
        type      = LV_F32;
        break;
    case 'd':
        type_size = sizeof(double);
        type      = LV_F64;
        break;
    default: lua_pushliteral(L, "不支持的类型"); return lua_error(L);
    }

    struct LuaVariant *lv = lv_push(L, type);

    if (!S().read(addr, lv->buf, type_size))
    {
        lua_pop(L, 1);
        luaL_traceback(L, L, NULL, 1);
        size_t n;
        LPCSTR s = luaL_checklstring(L, -1, &n);
        ErrorN(s, n);
        ErrorS("\n");
        lua_pop(L, 1);
        return 0;
    }

    return 1;
}

int lua_var_tos(lua_State *L)
{
    void  *ud   = luaL_checkudata(L, 1, "Var");
    size_t base = *(size_t *)ud;
    if (!base)
        return 0;

    bool check = true;
    check      = check && (lua_gettop(L) == 3 || lua_gettop(L) == 4);
    check      = check && lua_type(L, 2) == LUA_TSTRING;
    check      = check && (lua_type(L, 3) == LUA_TNUMBER || lua_type(L, 3) == LUA_TUSERDATA);
    check      = check && (lua_gettop(L) == 3 || lua_type(L, 4) == LUA_TNUMBER || lua_type(L, 4) == LUA_TUSERDATA);
    if (!check)
    {
        lua_pushstring(L, "Var:tos(type: string, count: LuaVariant|number, off: (LuaVariant|number)?): LuaVariant[]");
        return lua_error(L);
    }

    const char        *type_str = luaL_checkstring(L, 2);
    struct LuaVariant  lvt;
    struct LuaVariant *lv = parse_lv(L, 3, &lvt);
    size_t             count;
    bool               count_error = false;
    switch (lv->type)
    {
    case LV_U8 : count = lv->val.u8; break;
    case LV_U16: count = lv->val.u16; break;
    case LV_U32: count = lv->val.u32; break;
    case LV_U64: count = (size_t)lv->val.u64; break;
    case LV_I8:
        if (lv->val.i8 < 0)
            count_error = true;
        count = lv->val.i8;
        break;
    case LV_I16:
        if (lv->val.i16 < 0)
            count_error = true;
        count = lv->val.i16;
        break;
    case LV_I32:
        if (lv->val.i32 < 0)
            count_error = true;
        count = lv->val.i32;
        break;
    case LV_I64:
        if (lv->val.i64 < 0)
            count_error = true;
        count = (size_t)lv->val.i64;
        break;
    case LV_F32:
    case LV_F64: count_error = true; break;
    default    : assert(false);
    }
    if (count_error)
    {
        lua_pushliteral(L, "Var:tos: count must is non-negative number");
        return lua_error(L);
    }
    size_t addr = base;
    if (lua_gettop(L) == 4)
    {
        lv = parse_lv(L, 4, &lvt);
        switch (lv->type)
        {
        case LV_U8 : addr += lv->val.u8; break;
        case LV_U16: addr += lv->val.u16; break;
        case LV_U32: addr += lv->val.u32; break;
        case LV_U64: addr += (size_t)lv->val.u64; break;
        case LV_I8 : addr += lv->val.i8; break;
        case LV_I16: addr += lv->val.i16; break;
        case LV_I32: addr += lv->val.i32; break;
        case LV_I64: addr += (size_t)lv->val.i64; break;
        case LV_F32:
        case LV_F64: lua_pushstring(L, "Var:tos: off must be integer"); return lua_error(L);
        default    : assert(false);
        }
    }

    size_t type_size;
    switch (type_str[0])
    {
    case 'c': type_size = sizeof(char); break;
    case 'b': type_size = sizeof(unsigned char); break;
    case 'h': type_size = sizeof(short); break;
    case 'H': type_size = sizeof(unsigned short); break;
    case 'i': type_size = sizeof(int); break;
    case 'I': type_size = sizeof(unsigned int); break;
    case 'q': type_size = sizeof(long long); break;
    case 'Q': type_size = sizeof(unsigned long long); break;
    case 'z': type_size = sizeof(size_t); break;
    case 'f': type_size = sizeof(float); break;
    case 'd': type_size = sizeof(double); break;
    default : lua_pushliteral(L, "不支持的类型"); return lua_error(L);
    }

    size_t total_size = type_size * count;
    void  *buffer     = alloc_(total_size);
    if (!buffer)
    {
        lua_pushliteral(L, "out of memory");
        return lua_error(L);
    }

    if (!S().read(addr, buffer, total_size))
    {
        free_(buffer);

        luaL_traceback(L, L, NULL, 1);
        size_t n;
        LPCSTR s = luaL_checklstring(L, -1, &n);
        ErrorN(s, n);
        ErrorS("\n");
        lua_pop(L, 1);
        lua_pushliteral(L, "ReadMemory failed!");
        return lua_error(L);
    }

    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; i++)
    {
        switch (type_str[0])
        {
#define PUSHV(t, x, y) lv_push(L, x)->val.y = ((t *)buffer)[i]
        case 'c': PUSHV(int8_t, LV_I8, i8); break;
        case 'b': PUSHV(uint8_t, LV_U8, u8); break;
        case 'h': PUSHV(int16_t, LV_I16, i16); break;
        case 'H': PUSHV(uint16_t, LV_U16, u16); break;
        case 'i': PUSHV(int32_t, LV_I32, i32); break;
        case 'I': PUSHV(uint32_t, LV_U32, u32); break;
        case 'q': PUSHV(int64_t, LV_I64, i64); break;
        case 'Q': PUSHV(uint64_t, LV_U64, u64); break;
        case 'z': PUSHV(size_t, LV_UZ, uz); break;
        case 'f': PUSHV(float, LV_F32, f32); break;
        case 'd': PUSHV(double, LV_F64, f64); break;
#undef PUSHV
        }
        lua_rawseti(L, -2, (int)i + 1);
    }

    free_(buffer);
    return 1;
}

int lua_var_sto(lua_State *L)
{
    void  *ud   = luaL_checkudata(L, 1, "Var");
    size_t base = *(size_t *)ud;
    if (!base)
        return 0;

    bool check = true;
    check      = check && (lua_gettop(L) == 3 || lua_gettop(L) == 4);
    check      = check && lua_type(L, 2) == LUA_TSTRING;
    check      = check && (lua_type(L, 3) == LUA_TNUMBER || lua_type(L, 3) == LUA_TUSERDATA);
    check      = check && (lua_gettop(L) == 3 || lua_type(L, 4) == LUA_TNUMBER || lua_type(L, 4) == LUA_TUSERDATA);
    if (!check)
    {
        lua_pushstring(L, "Var:sto(type: string, count: LuaVariant|number, off: (LuaVariant|number)?): string");
        return lua_error(L);
    }

    const char        *type_str = luaL_checkstring(L, 2);
    struct LuaVariant  lvt;
    struct LuaVariant *lv = parse_lv(L, 3, &lvt);
    size_t             count;
    bool               count_error = false;
    switch (lv->type)
    {
    case LV_U8 : count = lv->val.u8; break;
    case LV_U16: count = lv->val.u16; break;
    case LV_U32: count = lv->val.u32; break;
    case LV_U64: count = (size_t)lv->val.u64; break;
    case LV_I8:
        if (lv->val.i8 < 0)
            count_error = true;
        count = lv->val.i8;
        break;
    case LV_I16:
        if (lv->val.i16 < 0)
            count_error = true;
        count = lv->val.i16;
        break;
    case LV_I32:
        if (lv->val.i32 < 0)
            count_error = true;
        count = lv->val.i32;
        break;
    case LV_I64:
        if (lv->val.i64 < 0)
            count_error = true;
        count = (size_t)lv->val.i64;
        break;
    case LV_F32:
    case LV_F64: count_error = true; break;
    default    : assert(false);
    }
    if (count_error)
    {
        lua_pushliteral(L, "Var:sto: count must is non-negative number");
        return lua_error(L);
    }
    size_t addr = base;
    if (lua_gettop(L) == 4)
    {
        lv = parse_lv(L, 4, &lvt);
        switch (lv->type)
        {
        case LV_U8 : addr += lv->val.u8; break;
        case LV_U16: addr += lv->val.u16; break;
        case LV_U32: addr += lv->val.u32; break;
        case LV_U64: addr += (size_t)lv->val.u64; break;
        case LV_I8 : addr += lv->val.i8; break;
        case LV_I16: addr += lv->val.i16; break;
        case LV_I32: addr += lv->val.i32; break;
        case LV_I64: addr += (size_t)lv->val.i64; break;
        case LV_F32:
        case LV_F64: lua_pushstring(L, "Var:sto: off must be integer"); return lua_error(L);
        default    : assert(false);
        }
    }

    size_t type_size;
    switch (type_str[0])
    {
    case 's': type_size = sizeof(char); break;
#ifdef _UNICODE
    case 'w': type_size = sizeof(wchar_t); break;
#endif
    case 'b': type_size = sizeof(unsigned char); break;
    default : lua_pushliteral(L, "不支持的类型"); return lua_error(L);
    }

    size_t total_size = type_size * count;
    void  *buffer     = alloc_(total_size);
    if (!buffer)
    {
        lua_pushliteral(L, "out of memory");
        return lua_error(L);
    }

    if (!S().read(addr, buffer, total_size))
    {
        free_(buffer);

        luaL_traceback(L, L, NULL, 1);
        size_t n;
        LPCSTR s = luaL_checklstring(L, -1, &n);
        ErrorN(s, n);
        ErrorS("\n");
        lua_pop(L, 1);
        lua_pushliteral(L, "ReadMemory failed!");
        return lua_error(L);
    }

    if (type_str[0] == 'b')
    {
        if (!total_size)
            lua_pushliteral(L, "");
        else
            lua_pushlstring(L, (LPCSTR)buffer, (size_t)count);
    }
    else if (type_str[0] == 's')
    {
        if (!total_size)
            lua_pushliteral(L, "");
        else
        {
#ifdef _UNICODE
            int   n = (int)count;
            LPSTR s = ansi_to_utf8_n((LPCSTR)buffer, &n);
            lua_pushlstring(L, s, n);
            free_(s);
#else
            lua_pushlstring(L, (LPCSTR)buffer, (size_t)count);
#endif
        }
    }
#ifdef _UNICODE
    else if (type_str[0] == 'w')
    {
        if (!total_size)
            lua_pushliteral(L, "");
        else
        {
            int   n = (int)count;
            LPSTR s = utf16_to_utf8_n((LPCWSTR)buffer, &n);
            lua_pushlstring(L, s, n);
            free_(s);
        }
    }
#endif

    free_(buffer);
    return 1;
}

int lua_var_write(lua_State *L)
{
    void  *ud   = luaL_checkudata(L, 1, "Var");
    size_t base = *(size_t *)ud;
    if (!base)
        return 0;

    bool check = true;
    check      = check && (lua_gettop(L) == 2 || lua_gettop(L) == 3);
    check      = check && (lua_type(L, 2) == LUA_TNUMBER || lua_type(L, 2) == LUA_TUSERDATA);
    check      = check && (lua_gettop(L) == 2 || lua_type(L, 3) == LUA_TNUMBER || lua_type(L, 3) == LUA_TUSERDATA);
    if (!check)
    {
        lua_pushstring(L, "Var:write(val: LuaVariant|number, off: (LuaVariant|number)?)");
        return lua_error(L);
    }
    struct LuaVariant  lvr;
    struct LuaVariant *lv_val = parse_lv(L, 2, &lvr);
    if (!lv_val)
    {
        lua_pushstring(L, "arg #1 type error, need number or LuaVariant");
        return lua_error(L);
    }
    size_t addr = base;
    if (lua_type(L, 3) == LUA_TUSERDATA)
    {
        struct LuaVariant *lv = lv_check(L, 3, LV_IZ);
        if (!lv)
            return 0;
        if (lv->type != LV_IZ)
        {
            lua_pushstring(L, "arg #3 lv->type is not iz");
            return lua_error(L);
        }
        addr += lv->val.iz;
    }
    else if (lua_type(L, 3) == LUA_TNUMBER)
        addr += (SSIZE_T)lua_tointeger(L, 3);

    size_t type_size = lv_size(lv_val);
    if (!type_size)
    {
        lua_pushliteral(L, "不支持的类型");
        return lua_error(L);
    }

    if (!S().write(addr, lv_val->buf, type_size, false))
    {
        luaL_traceback(L, L, NULL, 1);
        size_t n;
        LPCSTR s = luaL_checklstring(L, -1, &n);
        ErrorN(s, n);
        ErrorS("\n");
        lua_pop(L, 1);
    }

    return 0;
}

int lua_var_writes(lua_State *L)
{
    void  *ud   = luaL_checkudata(L, 1, "Var");
    size_t base = *(size_t *)ud;
    if (!base)
        return 0;

    bool check = true;
    check      = check && (lua_gettop(L) == 2 || lua_gettop(L) == 3);
    check      = check && lua_type(L, 2) == LUA_TTABLE;
    check      = check && (lua_gettop(L) == 2 || lua_type(L, 3) == LUA_TNUMBER || lua_type(L, 3) == LUA_TUSERDATA);
    if (!check)
    {
        lua_pushstring(L, "Var:tos(vals: (LuaVariant|number)[], off: (LuaVariant|number)?)");
        return lua_error(L);
    }

    size_t n       = (size_t)lua_objlen(L, 2);
    size_t allsize = 0;
    for (int i = 1; i <= (int)n; i++)
    {
        lua_rawgeti(L, 2, i);
        struct LuaVariant  lvt;
        struct LuaVariant *lv   = parse_lv(L, -1, &lvt);
        size_t             size = lv_size(lv);
        if (!size)
        {
            lua_pushfstring(L, "args #2.%d is not number or LuaVariant", i);
            return lua_error(L);
        }
        allsize += size;
    }
    if (!allsize)
        return 0;

    size_t addr = base;
    if (lua_type(L, 3) == LUA_TUSERDATA)
    {
        struct LuaVariant *lv = lv_check(L, 3, LV_IZ);
        if (!lv)
            return 0;
        if (lv->type != LV_IZ)
        {
            lua_pushstring(L, "arg #3 lv->type is not iz");
            return lua_error(L);
        }
        addr += lv->val.iz;
    }
    else if (lua_type(L, 3) == LUA_TNUMBER)
        addr += (SSIZE_T)lua_tointeger(L, 3);

    char *buf = (char *)alloc_(allsize);
    char *p   = buf;
    for (int i = 1; i <= (int)n; i++)
    {
        lua_rawgeti(L, 2, i);
        struct LuaVariant  lvt;
        struct LuaVariant *lv   = parse_lv(L, -1, &lvt);
        size_t             size = lv_size(lv);
        switch (size)
        {
        case 1 : *(uint8_t *)p = lv->val.u8; break;
        case 2 : *(uint16_t *)p = lv->val.u16; break;
        case 4 : *(uint32_t *)p = lv->val.u32; break;
        case 8 : *(uint64_t *)p = lv->val.u64; break;
        default: assert(false);
        }
        p += size;
    }

    bool b = S().write(addr, buf, allsize, false);
    free_(buf);

    if (!b)
    {
        luaL_traceback(L, L, NULL, 1);
        size_t n;
        LPCSTR s = luaL_checklstring(L, -1, &n);
        ErrorN(s, n);
        ErrorS("\n");
        lua_pop(L, 1);
    }

    return 0;
}

int lua_var_swrite(lua_State *L)
{
    void  *ud   = luaL_checkudata(L, 1, "Var");
    size_t base = *(size_t *)ud;
    if (!base)
        return 0;

    bool check = true;
    check      = check && (lua_gettop(L) == 2 || lua_gettop(L) == 3);
    check      = check && lua_type(L, 2) == LUA_TSTRING;
    check      = check && (lua_gettop(L) == 2 || lua_type(L, 3) == LUA_TNUMBER || lua_type(L, 3) == LUA_TUSERDATA);
    if (!check)
    {
        lua_pushstring(L, "Var:sto(val: string, off: (LuaVariant|number)?)");
        return lua_error(L);
    }

    size_t      n;
    const char *val  = luaL_checklstring(L, 2, &n);
    size_t      addr = base;
    if (lua_type(L, 3) == LUA_TUSERDATA)
    {
        struct LuaVariant *lv = lv_check(L, 3, LV_IZ);
        if (!lv)
            return 0;
        if (lv->type != LV_IZ)
        {
            lua_pushstring(L, "arg #3 lv->type is not iz");
            return lua_error(L);
        }
        addr += lv->val.iz;
    }
    else if (lua_type(L, 3) == LUA_TNUMBER)
        addr += (SSIZE_T)lua_tointeger(L, 3);

    if (!S().write(addr, val, n, false))
    {
        luaL_traceback(L, L, NULL, 1);
        size_t n;
        LPCSTR s = luaL_checklstring(L, -1, &n);
        ErrorN(s, n);
        ErrorS("\n");
        lua_pop(L, 1);
    }

    return 0;
}

int lua_get_context(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "GetContext(tid: LuaVariant)\n"
                           "\tparam tid LuaVariant::U32 线程id\n"
                           "\treturn table CONTEXT\n");
        return lua_error(L);
    }
    DWORD tid = lv_check(L, 1, LV_U32)->val.u32;

    HANDLE hThread = S().get_thread_by_tid(tid);
    if (!hThread)
    {
        LOG(TEXT("not found thread: %lu"), tid);
        return -1;
    }

    CONTEXT context;
    if (!S().get_context(hThread, CONTEXT_ALL, &context))
        return -1;

    /* 字段集合/名字来自 core 的字段表（宿主无关数据）；这里只负责按 kind 映射成 Lua 值。
     * ❗ 名字与旧 X-macro 逐字一致（黄金依赖它们） */
    size_t                 count  = 0;
    const core::ctx_field *fields = core::context_fields(&count);
    lua_createtable(L, 0, (int)count);
    for (size_t i = 0; i < count; i++)
    {
        const BYTE *p = (const BYTE *)&context + fields[i].offset;
        switch (fields[i].kind)
        {
        case core::ctx_kind::U32:
            lv_push(L, LV_U32)->val.u32 = *(const uint32_t *)p;
            lua_setfield(L, -2, fields[i].name);
            break;
        case core::ctx_kind::U64:
            lv_push(L, LV_U64)->val.u64 = *(const uint64_t *)p;
            lua_setfield(L, -2, fields[i].name);
            break;
        case core::ctx_kind::UPTR:
            lv_push(L, LV_UZ)->val.uz = *(const size_t *)p;
            lua_setfield(L, -2, fields[i].name);
            break;
        case core::ctx_kind::M128:
        {
            /* M128A = { Low, High }（旧宏同样只暴露这两个 64 位半部） */
            const uint64_t *q = (const uint64_t *)p;
            lua_createtable(L, 0, 2);
            lv_push(L, LV_U64)->val.u64 = q[0];
            lua_setfield(L, -2, "Low");
            lv_push(L, LV_U64)->val.u64 = q[1];
            lua_setfield(L, -2, "High");
            lua_setfield(L, -2, fields[i].name);
            break;
        }
        }
    }

    return 1;
}

int lua_set_context(lua_State *L)
{
    if (lua_gettop(L) != 2)
    {
        lua_pushliteral(L, "SetContext(tid: LuaVariant, context: table)\n"
                           "\tparam tid LuaVariant::U32 线程id\n"
                           "\tparam table CONTEXT\n");
        return lua_error(L);
    }

    DWORD tid = lv_check(L, 1, LV_U32)->val.u32;
    luaL_checktype(L, 2, LUA_TTABLE);

    HANDLE hThread = S().get_thread_by_tid(tid);
    if (!hThread)
    {
        LOG(TEXT("not found thread: %lu"), tid);
        return -1;
    }

    /* 读-改-写：先取基准，再只覆盖表中出现的字段（旧行为：nil = 不改） */
    CONTEXT context;
    if (!S().get_context(hThread, CONTEXT_ALL, &context))
        return -1;

    size_t                 count  = 0;
    const core::ctx_field *fields = core::context_fields(&count);
    for (size_t i = 0; i < count; i++)
    {
        lua_getfield(L, 2, fields[i].name);
        if (lua_type(L, -1) != LUA_TNIL)
        {
            BYTE *p = (BYTE *)&context + fields[i].offset;
            switch (fields[i].kind)
            {
            case core::ctx_kind::U32 : *(uint32_t *)p = lv_check(L, -1, LV_U32)->val.u32; break;
            case core::ctx_kind::U64 : *(uint64_t *)p = lv_check(L, -1, LV_U64)->val.u64; break;
            case core::ctx_kind::UPTR: *(size_t *)p = lv_check(L, -1, LV_UZ)->val.uz; break;
            case core::ctx_kind::M128:
            {
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_getfield(L, -1, "Low");
                lua_getfield(L, -2, "High");
                uint64_t *q = (uint64_t *)p;
                q[0]        = lv_check(L, -2, LV_U64)->val.u64;
                q[1]        = lv_check(L, -1, LV_U64)->val.u64;
                lua_pop(L, 2);
                break;
            }
            }
        }
        lua_pop(L, 1);
    }

    return S().set_context(hThread, CONTEXT_ALL, &context) ? 0 : -1;
}

int lua_ea2_modname_rva(lua_State *L)
{
    size_t  ea     = lv_check(L, 1, LV_UZ)->val.uz;
    PEInfo *peinfo = S().get_peinfo_by_ea(ea);
    if (peinfo)
    {
        char *modname = tchar_to_utf8(peinfo->Path + peinfo->name_offset);
        lua_pushstring(L, modname);
        free_from_tchar(modname);
        lv_push(L, LV_U32)->val.u32 = (DWORD)(ea - peinfo->base);
    }
    else
    {
        lua_pushnil(L);
        lua_pushnil(L);
    }
    return 2;
}

/**
 * GetModBase - 获取模块基址
 * @param string 模块名
 * @return LuaVariant::UZ 基址
 */
int lua_get_mod_base(lua_State *L)
{
    LPXSTR  name   = utf8_to_tchar(lua_tostring(L, 1));
    PEInfo *peinfo = S().get_peinfo_by_name(name);
    free_from_tchar(name);
    if (peinfo)
        lv_push(L, LV_UZ)->val.uz = peinfo->base;
    else
        lua_pushnil(L);
    return 1;
}

typedef struct
{
    bool         done;
    DWORD        machineType;
    size_t       index;
    HANDLE       hThread;
    STACKFRAME64 stackFrame;
#ifndef _WIN64
    STACKFRAME_  stackFrame32;
    StackWalkT   StackWalk_;
#endif
    StackWalk64T StackWalk64_; /* x86 / x64 共用：dbghelp 的 StackWalk64（动态解析） */
    CONTEXT context;
} StackTraceContext;

int lua_stack_trace_iter(lua_State *L)
{
    StackTraceContext *stContext = (StackTraceContext *)lua_touserdata(L, lua_upvalueindex(1));
    if (stContext->done)
        return 0;

    size_t  offset;
    CONTEXT context = stContext->context;
#ifdef _WIN64
    /* ❗dbghelp 改为动态解析（Win9x 兼容 / 去掉静态导入）：取不到时优雅退化 */
    if (!stContext->StackWalk64_)
    {
        stContext->done = true;
        return 0;
    }
    if (!stContext->StackWalk64_(stContext->machineType, S().proc().hProcess, stContext->hThread, &stContext->stackFrame, &context, StackTrace_ReadProcessMemory,
                                 StackTrace_FunctionTableAccess, StackTrace_GetModuleBase, StackTrace_TranslateAddress))
    {
        stContext->done = true;
        return 0;
    }
    offset = stContext->stackFrame.AddrPC.Offset;
#else
    if (stContext->StackWalk64_)
    {
        /* D27 修复：优先用 dbghelp 的 `StackWalk64`（x64 风格回调），并**把我们自己的回调传进去**
         * （旧代码给 dbghelp 传 NULL → 完全依赖 dbghelp 内部状态）。 */
        if (!stContext->StackWalk64_(stContext->machineType, S().proc().hProcess, stContext->hThread, &stContext->stackFrame, &context,
                                     StackTrace_ReadProcessMemory, StackTrace_FunctionTableAccess64, StackTrace_GetModuleBase, StackTrace_TranslateAddress))
        {
            stContext->done = true;
            return 0;
        }
        offset = (size_t)stContext->stackFrame.AddrPC.Offset;
    }
    else if (stContext->StackWalk_)
    {
        /* 老系统只有 `StackWalk`（32 位风格回调） */
        if (!stContext->StackWalk_(stContext->machineType, S().proc().hProcess, stContext->hThread, &stContext->stackFrame32, &context,
                                   StackTrace_ReadProcessMemory32, StackTrace_FunctionTableAccess32, StackTrace_GetModuleBase32, StackTrace_TranslateAddress32))
        {
            stContext->done = true;
            return 0;
        }
        offset = (DWORD)stContext->stackFrame32.AddrPC.Offset;
    }
    else
    {
        /* ❗两个入口都取不到（D27：绝不能再调用空指针）→ 当作不支持，直接结束迭代 */
        stContext->done = true;
        return 0;
    }
#endif

    if (!offset)
    {
        stContext->done = true;
        return 0;
    }

    stContext->context        = context;
    lv_push(L, LV_UZ)->val.uz = stContext->index++;

    // 设置ea (有效地址)
    lv_push(L, LV_UZ)->val.uz = offset;

    // 获取模块名和RVA
    PEInfo *peinfo = S().get_peinfo_by_ea(offset);
    if (!peinfo)
        return 2;
    char *modname = tchar_to_utf8(peinfo->Path + peinfo->name_offset);
    lua_pushstring(L, modname);
    free_from_tchar(modname);
    lv_push(L, LV_U32)->val.u32 = (DWORD)(offset - peinfo->base);
    return 4;
}

/* P3.B 模块 8：`StackTrace_*` 四个 dbghelp 回调 + `find_runtime_function` + x86 兼容结构体
 * 已迁到 `core/stack_trace.cpp` / `core/stack_trace.hpp`（本文件用 hpp 里的
 * `STACKFRAME_`/`StackWalkT`/`StackWalk64T` 定义下面这个 Lua userdata）。 */

/**
 * StackTrace - 生成堆栈跟踪
 * @param tid 线程id
 * @return table 堆栈帧数组
 */
int lua_stack_trace(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "StackTrace(tid: LuaVariant::U32)->iterator\n"
                           "param:\n"
                           "\ttid LuaVariant::U32 线程id\n"
                           "return iterator 堆栈帧迭代");
        return lua_error(L);
    }

    DWORD tid = lv_check(L, 1, LV_U32)->val.u32;
    if (!tid)
    {
        lua_pushliteral(L, "tid为0");
        return lua_error(L);
    }

    StackTraceContext *stContext = (StackTraceContext *)lua_newuserdata(L, sizeof(StackTraceContext));
    stContext->done              = true;
    CONTEXT context;
    ZeroMem(context);

    // 获取当前线程上下文
    context.ContextFlags = CONTEXT_FULL;
    stContext->hThread   = S().get_thread_by_tid(tid);
    if (!stContext->hThread)
        return -1;
    if (!GetThreadContext(stContext->hThread, &context))
    {
        E("GetThreadContext");
        return -1;
    }

    // 设置堆栈帧结构
    ZeroMem(stContext->stackFrame);

#ifdef _WIN64
    stContext->machineType                 = IMAGE_FILE_MACHINE_AMD64;
    stContext->stackFrame.AddrPC.Offset    = context.Rip;
    stContext->stackFrame.AddrFrame.Offset = context.Rbp;
    stContext->stackFrame.AddrStack.Offset = context.Rsp;
#else
    stContext->machineType                 = IMAGE_FILE_MACHINE_I386;
    stContext->stackFrame.AddrPC.Offset    = context.Eip;
    stContext->stackFrame.AddrFrame.Offset = context.Ebp;
    stContext->stackFrame.AddrStack.Offset = context.Esp;

    ZeroMem(stContext->stackFrame32);

    stContext->stackFrame32.AddrPC.Offset    = context.Eip;
    stContext->stackFrame32.AddrFrame.Offset = context.Ebp;
    stContext->stackFrame32.AddrStack.Offset = context.Esp;
    stContext->stackFrame32.AddrPC.Mode      = AddrModeFlat;
    stContext->stackFrame32.AddrFrame.Mode   = AddrModeFlat;
    stContext->stackFrame32.AddrStack.Mode   = AddrModeFlat;
#endif

    stContext->stackFrame.AddrPC.Mode    = AddrModeFlat;
    stContext->stackFrame.AddrFrame.Mode = AddrModeFlat;
    stContext->stackFrame.AddrStack.Mode = AddrModeFlat;

    /* ❗D27 修复 + Win9x 兼容：`StackWalk`/`StackWalk64` 是 **dbghelp.dll** 的导出（旧代码从
     * `kernel32.dll` 取 → 恒为 NULL → 迭代器进 else 分支**调用空指针** → 宿主 0xC0000005）。
     * 两只架构都**动态解析**（dynapi：先查已加载模块再按需加载）→ 导入表里不再出现
     * dbghelp.dll（Win95/98 没有这个 DLL，静态导入会让手工映射直接失败）；取不到则优雅退化。 */
    stContext->StackWalk64_ = autodbg::common::get_proc_as<StackWalk64T>("dbghelp.dll", "StackWalk64");
#ifndef _WIN64
    stContext->StackWalk_ = autodbg::common::get_proc_as<StackWalkT>("dbghelp.dll", "StackWalk");
#endif

    stContext->context = context;
    stContext->index   = 1;
    stContext->done    = false;
    lua_pushcclosure(L, lua_stack_trace_iter, 1);
    return 1;
}

/**
 * SetBreakpoints - 批量设置断点
 * @param table breakpoints 断点表
 */
int lua_set_breakpoints(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return -1;
    luaL_checktype(L, 1, LUA_TTABLE);

    // 遍历模块表
    lua_pushnil(L);
    while (lua_next(L, 1))
    {
        // key是模块名，value是RVA表
        luaL_checktype(L, -2, LUA_TSTRING);
        luaL_checktype(L, -1, LUA_TTABLE);
#ifdef _UNICODE
        const char *_modname = lua_tostring(L, -2);
        WCHAR       modname[MAX_PATH];
        int         n = MultiByteToWideChar(CP_UTF8, 0, _modname, -1, modname, MAX_PATH);
        modname[n]    = 0;
#else
        size_t      n;
        const char *modname = lua_tolstring(L, -2, &n);
#endif

        lua_pushnil(L);
        while (lua_next(L, -2))
        {
            DWORD rva;
            if (lua_type(L, -2) == LUA_TUSERDATA)
                rva = lv_check(L, -2, LV_U32)->val.u32;
            else if (lua_type(L, LUA_TSTRING))
                str2integer(luaL_checkstring(L, -2), 4, false, &rva);
            else if (lua_type(L, LUA_TNUMBER))
                rva = (DWORD)luaL_checkinteger(L, -2);
            else
                return lua_type_error(L, -2, "LuaVariant or string or integer");
            if (lua_type(L, -1) != LUA_TTABLE && lua_type(L, -1) != LUA_TFUNCTION)
                return lua_type_error(L, -1, "table or function");
            // 创建断点信息
            /* P6.2：变长节点（`modname[1]`）由链表一次分配（= 旧 `alloc_(sizeof + n*sizeof(TCHAR))`） */
            BreakpointInfo *info = S().breakpoints().create_front(n * sizeof(TCHAR));
            CopyMem(info->modname, modname, n * sizeof(TCHAR));
            info->modname[n]       = 0;
            info->rva              = rva;
            info->has_orignal_byte = 0;
            info->activate         = 1;

            if (lua_type(L, -1) == LUA_TFUNCTION)
            {
                info->callback  = luaref_to_token(luaL_ref(L, LUA_REGISTRYINDEX));
                info->is_single = 0;
            }
            else
            {
                // 获取callback
                lua_getfield(L, -1, "callback");
                if (lua_type(L, -1) == LUA_TFUNCTION)
                    info->callback = luaref_to_token(luaL_ref(L, LUA_REGISTRYINDEX));
                else
                {
                    lua_pop(L, 1);
                    info->callback = callback_ref_t{};
                }

                // 获取is_single标志
                lua_getfield(L, -1, "is_single");
                if (lua_type(L, -1) == LUA_TNIL)
                    info->is_single = 0;
                else if (lua_type(L, -1) == LUA_TBOOLEAN)
                    info->is_single = lua_toboolean(L, -1) ? 1 : 0;
                else
                    return lua_type_error(L, -1, "boolean or nil");
                lua_pop(L, 2);
            }
        }
        PEInfo *peinfo = S().get_peinfo_by_name(modname);
        if (peinfo)
            S().apply_breakpoints_for_module(peinfo);
        lua_pop(L, 1);
    }

    return 0;
}

int lua_enable_breakpoint(lua_State *L)
{
    DWORD rva;
    if (lua_gettop(L) == 1)
    {
        size_t  ea     = lv_check(L, 1, LV_UZ)->val.uz;
        PEInfo *peinfo = S().get_peinfo_by_ea(ea);
        if (peinfo)
            S().enable_breakpoint(ea, peinfo->Path + peinfo->name_offset, (DWORD)(ea - peinfo->base));
    }
    else if (lua_gettop(L) == 2)
    {
        rva             = lv_check(L, 2, LV_U32)->val.u32;
        LPXSTR wmodname = utf8_to_tchar(luaL_checkstring(L, 1));
        size_t base     = S().get_base(wmodname);
        if (base)
            S().enable_breakpoint(base + rva, wmodname, rva);
        free_from_tchar(wmodname);
    }
    else
    {
        lua_pushliteral(L, "\"EnableBreakpoint\"参数错误。EnableBreakpoint原型:\n"
                           "function EnableBreakpoint(ea: LuaVariant::" UZ "): nil\n"
                           "function EnableBreakpoint(modname: string, rva: LuaVariant::U32): nil\n"
                           "  ea 断点地址\n"
                           "  modname 断点所在模块名称\n"
                           "  rva 断点在所在模块的RVA或其16进制字符串\n");
        return lua_error(L);
    }
    return 0;
}

int lua_disable_breakpoint(lua_State *L)
{
    DWORD rva;
    if (lua_gettop(L) == 1)
    {
        size_t  ea     = lv_check(L, 1, LV_UZ)->val.uz;
        PEInfo *peinfo = S().get_peinfo_by_ea(ea);
        if (peinfo)
            S().disable_breakpoint(ea, peinfo->Path + peinfo->name_offset, (DWORD)(ea - peinfo->base));
    }
    else if (lua_gettop(L) == 2)
    {
        rva             = lv_check(L, 2, LV_U32)->val.u32;
        LPXSTR wmodname = utf8_to_tchar(luaL_checkstring(L, 1));
        size_t base     = S().get_base(wmodname);
        if (base)
            S().disable_breakpoint(base + rva, wmodname, rva);
        free_from_tchar(wmodname);
    }
    else
    {
        lua_pushliteral(L, "\"DisableBreakpoint\"参数错误。DisableBreakpoint原型:\n"
                           "function DisableBreakpoint(ea: LuaVariant::" UZ "): nil\n"
                           "function DisableBreakpoint(modname: string, rva: LuaVariant::U32): nil\n"
                           "  ea 断点地址\n"
                           "  modname 断点所在模块名称\n"
                           "  rva 断点在所在模块的RVA或其16进制字符串\n");
        return lua_error(L);
    }
    return 0;
}

int lua_hide(lua_State *L)
{
    if (lua_gettop(L) != 0)
    {
        lua_pushliteral(L, "\"Hide\"参数错误。Hide原型:\n"
                           "function Hide(): nil\n");
        return lua_error(L);
    }
    if (!S().proc().hProcess)
        return 0;
    fix_peb(S().proc().hProcess, TRUE);
    patch_antidebug_api(S().proc().hProcess);
    return 0;
}

int lua_find_func_rva(lua_State *L)
{
    if (lua_gettop(L) != 2)
    {
        lua_pushliteral(L, "\"FindFuncRva\"参数错误。FindFuncRva原型:\n"
                           "function FindFuncRva(dllname: string, funcname: string): LuaVariant?\n");
        return lua_error(L);
    }
    LPXSTR  dllname = utf8_to_tchar(luaL_checkstring(L, 1));
    PEInfo *info    = S().get_peinfo_by_name(dllname);
    free_from_tchar(dllname);
    if (!info)
        return 0;
    HMODULE hLocal = LoadLibrary(info->Path);
    if (!hLocal)
        return 0;
    size_t fun  = (size_t)GetProcAddress(hLocal, luaL_checkstring(L, 2));
    size_t base = (size_t)hLocal;
    FreeLibrary(hLocal);
    if (!fun)
        return 0;
    lv_push(L, LV_U32)->val.u32 = (DWORD)(fun - base);
    return 1;
}

int lua_get_cpp_class_name(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "\"GetCppClassName\"参数错误。GetCppClassName原型:\n"
                           "function GetCppClassName(addr: LuaVariant): string?\n");
        return lua_error(L);
    }
    size_t addr = lv_check(L, 1, LV_UZ)->val.uz;
    /* 类名读取在 core（P3.B 模块 8）。
     * ❗1:1 保留旧行为：读内存失败 → 不返回值（return 0）；
     *   名字长度 ≤ 3（has_name=false）时旧实现也是 return 1 但什么都没 push
     *   （Lua 会拿到那个参数本身）→ 这里同样 return 1 且不 push（见 D24）。 */
    char class_name[0x1000];
    bool has_name = false;
    if (!S().get_cpp_class_name(addr, class_name, sizeof class_name, &has_name))
        return 0;
    if (has_name)
        lua_pushstring(L, class_name);
    return 1;
}

/* P4.6：`FindMemoryContext` 的结构与搜索状态机已下沉到 `core/memory.{hpp,cpp}`
 * （绑定层只把结果推回 Lua 栈 / 把 `errbuf` 文案 `lua_error`；__gc 只调 find_memory_release）。 */

int lua_find_memory_context_gc(lua_State *L)
{
    core::FindMemoryContext *context = (core::FindMemoryContext *)luaL_checkudata(L, 1, "FindMemoryContext");
    S().find_memory_release(context);
    return 0;
}

int lua_find_memory_iter(lua_State *L)
{
    core::FindMemoryContext *context = (core::FindMemoryContext *)lua_touserdata(L, lua_upvalueindex(1));
    if (!context)
        return 0;
    if (!context->state)
        return 0;

    char                  errbuf[128];
    core::FindMemoryMatch match;
    switch (S().find_memory_next(context, &match, errbuf, sizeof errbuf))
    {
    case core::FindMemoryKind::Match:
        lv_push(L, LV_UZ)->val.uz = match.ea;
        lua_pushlstring(L, (const char *)match.data, match.len);
        return 2;
    case core::FindMemoryKind::Failed: lua_pushstring(L, errbuf); return lua_error(L);
    default                          : return 0; /* End */
    }
}

int lua_find_memory(lua_State *L)
{
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
    {
        lua_pushliteral(L, "\"FindMemory\"参数错误。FindMemory原型:\n"
                           "function FindMemory(pattern: string): (LuaVariant[]\n");
        return lua_error(L);
    }
    size_t      len     = 0;
    const char *pattern = lua_tolstring(L, 1, &len);
    if (!pattern || !len)
    {
        lua_pushliteral(L, "\"FindMemory\"参数为空。");
        return lua_error(L);
    }
    core::FindMemoryContext *context = (core::FindMemoryContext *)lua_newuserdata(L, sizeof(core::FindMemoryContext));
    luaL_getmetatable(L, "FindMemoryContext");
    lua_setmetatable(L, -2);
    S().find_memory_init(context, pattern, len);

    lua_pushcclosure(L, lua_find_memory_iter, 1);
    return 1;
}

int lua_get_memory_region(lua_State *L)
{
    core::MemRegionIter it;
    S().get_memory_region_begin(&it);

    lua_createtable(L, 0, 0);
    int                 i = 0;
    core::MemRegionInfo region;
    while (S().get_memory_region_next(&it, &region))
    {
        lua_createtable(L, 0, 0);
        lv_push(L, LV_UZ)->val.uz = region.BaseAddress;
        lua_setfield(L, -2, "BaseAddress");
        lv_push(L, LV_UZ)->val.uz = region.AllocationBase;
        lua_setfield(L, -2, "AllocationBase");
        lv_push(L, LV_U32)->val.u32 = region.AllocationProtect;
        lua_setfield(L, -2, "AllocationProtect");
        lv_push(L, LV_UZ)->val.uz = region.RegionSize;
        lua_setfield(L, -2, "RegionSize");
        lv_push(L, LV_U32)->val.u32 = region.State;
        lua_setfield(L, -2, "State");
        lv_push(L, LV_U32)->val.u32 = region.Protect;
        lua_setfield(L, -2, "Protect");
        lv_push(L, LV_U32)->val.u32 = region.Type;
        lua_setfield(L, -2, "Type");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

/* P4.6：`RemoveAllFiles` / `lua_reset_dir`（脚本目录工具，Q7）已搬到 `state.cpp`。 */
