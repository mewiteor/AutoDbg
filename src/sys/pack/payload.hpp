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
 *   ③ **不做任何分配** —— 输出缓冲由调用方提供（启动器映射镜像后直接解进去）。
 *
 * 布局（小端）：
 *   u32 entry_count
 *   entry: u16 name_len / char name[name_len]（小写、无路径、**不带 NUL**）/ u32 raw_size
 *   u8 props[5]
 *   payload（单条 LZMA 流，各 entry 原始字节按顺序背靠背）
 * ========================================================================== */
#include <stddef.h>

#include "error.hpp"

namespace autodbg::sys::pack
{
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

    /* 第 i 个 entry 在「解压后的整包缓冲」里的偏移 */
    unsigned raw_offset(unsigned i) const;

    /* 按名字查（大小写敏感；载荷里的名字已由 tools/pack 转成小写）；找不到返回 -1 */
    int find(const char *name) const;
    int find_n(const char *name, unsigned name_len) const;

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
        unsigned    raw_offset; /* 在解压结果里的起始偏移 */
    };

    const unsigned char *data_;
    unsigned             size_;

    unsigned    count_;
    entry       entries_[32];   /* 上限：一个包里最多 32 个模块（x86 4 个 / x64 2 个，足够） */
    unsigned    raw_total_;

    const unsigned char *props_;
    const unsigned char *stream_;
    unsigned             payload_size_;
};
}

#endif /* AUTODBG_SYS_PACK_PAYLOAD_HPP */
