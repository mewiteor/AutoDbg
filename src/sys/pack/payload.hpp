#ifndef AUTODBG_SYS_PACK_PAYLOAD_HPP
#define AUTODBG_SYS_PACK_PAYLOAD_HPP
/* =============================================================================
 * src/sys/pack/payload.hpp —— 载荷（manifest + 单条 LZMA 流）的解析与解压（Plan §2.4 / D14）
 *
 * 职责：把 `tools/pack` 生成的 `g_packdata[]` 拆成「entry 表 + props + 压缩流」，
 *       并按需求把整包解压到调用方给的缓冲（不落盘、不自己持有大内存）。
 * 来源：新写（Plan P2）。原型 `tools/PackEXE/pe/src/loader.cpp` 只认它自己的
 *       「按名字解到 HeapAlloc」格式，且没有边界校验。
 * 与旧实现的差异：
 *   ① 单流（D14）：一次解压全部模块，而不是按名字单独解一个流；
 *   ② 全字段边界校验（§2.4 的 D11：无 magic / 无版本号，自检靠结构边界）；
 *   ③ **不做任何分配** —— 输出缓冲由调用方提供（启动器映射镜像后直接解进去）；
 *   ④ T8 切片 a1（D63）：entry 加 `kind`（0 = module、1 = exec blob），`kind` 只允许 0 / 1，
 *      其它走现有的 `AUTODBG_PACK_FAIL`（stage::payload）。blob 不是可映射模块，本层只认
 *      「字节 + 长度」；谁拿它去落盘 / CreateProcess 由调用方决定（`handoff.cpp`）。
 *
 * 布局（小端）：
 *   u32 entry_count
 *   entry: u16 name_len / char name[name_len]（小写、无路径、**不带 NUL**）/ u32 raw_size / u8 kind
 *   u8 props[5]
 *   payload（单条 LZMA 流，各 entry 原始字节按顺序背靠背）
 * ========================================================================== */
#include <stddef.h>

#include "error.hpp"

namespace autodbg::sys::pack
{
/* entry 类型（D63）：0 = 可被映射器接管的模块，1 = 落盘 + CreateProcess 的 exec blob */
constexpr unsigned char k_entry_kind_module = 0;
constexpr unsigned char k_entry_kind_exec   = 1;

class payload
{
public:
    payload();

    /* 解析 manifest（不解压）。成功返回 true；`data` 必须在 payload 生命周期内保持有效 */
    bool open(const unsigned char *data, unsigned size, load_error *err);

    /* 清空（不释放任何东西：数据是别人的） */
    void close();

    unsigned count() const { return count_; }
    unsigned total_raw_size() const { return raw_total_; }

    /* 第 i 个 entry 的名字（**不保证 NUL 结尾**）与长度 */
    const char *name(unsigned i) const;
    unsigned name_len(unsigned i) const;

    unsigned raw_size(unsigned i) const;

    /* 第 i 个 entry 的类型（k_entry_kind_module / k_entry_kind_exec）；越界返回 0 */
    unsigned char kind(unsigned i) const;

    /* 第 i 个 entry 在「解压后的整包缓冲」里的偏移 */
    unsigned raw_offset(unsigned i) const;

    /* 按名字查（大小写敏感；载荷里的名字已由 tools/pack 转成小写）；找不到返回 -1 */
    int find(const char *name) const;
    int find_n(const char *name, unsigned name_len) const;

    /* 按 kind 找第一条 entry（D63：handoff 要的是 `adbg64.bin` 这条 exec 条目）；找不到返回 -1 */
    int find_exec() const;

    /* 把整包解压到 out（`out_size` 必须 == total_raw_size()）。
     * 返回 true 表示「解压成功且输出长度与 manifest 一致」——**逐字节内容**由调用方比对。 */
    bool extract_all(unsigned char *out, unsigned out_size, load_error *err) const;

    /* 压缩流属性（诊断用）：props[0] = (pb*5 + lp)*9 + lc，props[1..4] = dictSize（小端） */
    const unsigned char *props() const { return props_; }
    unsigned payload_size() const { return payload_size_; }

private:
    struct entry
    {
        const char *name;      /* 指向 data 内，**不含 NUL** */
        unsigned    name_len;
        unsigned    raw_size;
        unsigned char kind;      /* D63：0 = module、1 = exec blob */
        unsigned    raw_offset; /* 在解压结果里的起始偏移 */
    };

    const unsigned char *data_;
    unsigned             size_;

    unsigned    count_;
    entry       entries_[32];   /* 上限：一个包里最多 32 个 entry（x86 = 4 模块 + 1 blob，x64 = 2） */
    unsigned    raw_total_;

    const unsigned char *props_;
    const unsigned char *stream_;
    unsigned             payload_size_;
};
}

#endif /* AUTODBG_SYS_PACK_PAYLOAD_HPP */
