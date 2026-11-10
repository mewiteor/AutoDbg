#ifndef AUTODBG_SYS_PACK_MODULE_LOADER_HPP
#define AUTODBG_SYS_PACK_MODULE_LOADER_HPP
/* =============================================================================
 * src/sys/pack/module_loader.hpp —— 内存模块映射器（Plan §3.1 / F5 / F12）
 *
 * 职责：把一个**已经在内存里的 PE 模块**映射进当前进程并启动它：
 *         map()     → 头/节/重定位/导入（全部成功才返回 true）+ x64 展开表注册
 *         start()   → TLS 回调 → 入口（DLL: DllMain(DLL_PROCESS_ATTACH)；EXE: 入口函数）
 *         resolve() → 按名字查导出（线性查找，零分配）
 *       失败必须能**完整回滚**（F12：启动器据此换另一个变体重试）。
 * 来源：重写 `tools/PackEXE/pe/src/memoryModule.cpp`。
 * 与旧实现的差异（对应 plan §1 F13 的清单）：
 *   ① `GetNativeSystemInfo`(XP+) → `GetSystemInfo`（Win95 可用）；
 *   ② 原型 `LinkList::Pop` 删错节点、`modules`/`blockedMemory` 泄漏 → 改成显式数组 + 统一释放；
 *   ③ 原型失败一律 `MessageBox` → 改成返回 `load_error`（调用方决定打印/回退）；
 *   ④ 原型用 `IsBadReadPtr` 边读边判 → 改成本文件内的一次性边界校验（`in_range`）；
 *   ⑤ 不分配导出表（原型 `HeapAlloc` + 空实现 `sort`）→ 线性查名字，零分配；
 *   ⑥ 新增 x64 展开表注册（F5，`RtlAddFunctionTable`）与依赖解析钩子（P3 用）。
 *
 * ❗不支持的加载器特性（本方案的已知代价，见 plan §1）：
 *   - 不使用 `__declspec(thread)`（F3）：`map()` 不分配 TLS 槽、不写 `AddressOfIndex`。
 *   - 资源：模块内若用 `FindResource` 必须传自己的 `HMODULE`（F6）。
 * ========================================================================== */
#include <windows.h>
#include <stddef.h>

#include "error.hpp"

namespace autodbg::sys::pack
{
/* 依赖解析钩子：P3 用它把「模块间依赖」指到已映射的模块上。
 * 返回 nullptr 表示该依赖解析失败（map() 会把 DLL 名写进 load_error::detail）。 */
typedef HMODULE (*dependency_loader)(const char *dll_name, void *ctx);

struct map_options
{
    dependency_loader load;   /* nullptr = 用 LoadLibraryA */
    void             *ctx;
};

class mapped_module
{
public:
    mapped_module();
    ~mapped_module();

    /* 两阶段的第一阶段：全部成功才返回 true；任何失败都已回滚到「未映射」状态。
     * `file_data` 是磁盘上的裸 PE 文件字节（不是加载后的镜像），必须在 start() 之前有效。 */
    bool map(const void *file_data, unsigned file_size, const map_options *opt, load_error *err);

    /* 两阶段的第二阶段：TLS 回调 + 入口。EXE 模块的入口通常不返回（内部 ExitProcess）。 */
    bool start(load_error *err);

    /* 按名字查导出（名称表大小写敏感，与 GetProcAddress 一致）；找不到返回 nullptr */
    void *resolve(const char *name) const;

    /* 按序号查导出（ordinal 是 PE 里的序号，不是索引） */
    void *resolve_ordinal(unsigned ordinal) const;

    void    *image() const { return image_; }
    unsigned image_size() const { return image_size_; }
    bool     is_dll() const { return is_dll_; }

    /* 回滚：释放镜像、注销展开表、释放依赖库引用。可重复调用。 */
    void unmap();

private:
    void *   image_;
    unsigned image_size_;
    void *   preferred_base_;
    unsigned entry_rva_;
    bool     is_dll_;
    bool     started_;
    bool     table_added_;      /* x64：已注册展开表 */
    void *   table_;            /* x64：展开表地址（注销用） */

    /* 依赖库句柄（LoadLibrary 成功过的），unmap() 时统一 FreeLibrary */
    HMODULE *deps_;
    unsigned dep_count_;
    unsigned dep_cap_;

    bool dep_push(HMODULE handle);
};
}

#endif /* AUTODBG_SYS_PACK_MODULE_LOADER_HPP */
