/* =============================================================================
 * src/sys/pack/module_loader.cpp —— 内存模块映射器实现（Plan §3.1 / F5 / F12）
 *
 * 来源：重写 `tools/PackEXE/pe/src/memoryModule.cpp`；差异见 module_loader.hpp 头注释。
 *
 * 设计要点：
 *   ① **两阶段**：map() 只做「可以完整回滚」的事（VirtualAlloc / LoadLibrary），
 *      start() 才跑 TLS 与入口 —— 一旦入口跑起来就回滚不了了（F12）。
 *      → map() 的任何失败路径**都保证已回滚**，错误信息用 err->text 自带（见 error.hpp）。
 *   ② **不给页面留 RWX**：按节属性逐节 VirtualProtect（SectionAlignment=0x1000，节不重叠）。
 *   ③ 所有 RVA 读取先过 `in_range()`，替代原型的 `IsBadReadPtr` 边读边判。
 *   ④ x86 的 32 位辅助符号（`__aullshr` 等）由 `src/sys/flt.asm` 提供（D16）。
 * ========================================================================== */
#include "module_loader.hpp"

#include <string.h>   /* memcpy（影子头） */

#include "../../common/allocator.hpp"

#ifdef _WIN64
/* x64 展开表注册：`windows.h`（winnt.h）已经声明了 RtlAddFunctionTable /
 * RtlDeleteFunctionTable，kernel32.lib 里也有导入 —— 不自己重复声明（会与 winnt.h 类型冲突）。 */
#endif

namespace autodbg::sys::pack
{
namespace
{
constexpr unsigned k_max_sections = 63;   /* NumberOfSections 上限（实际远小于此） */

#ifdef _WIN64
constexpr unsigned k_host_machine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr unsigned k_host_machine = IMAGE_FILE_MACHINE_I386;
#endif

inline unsigned rd_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

inline unsigned rd_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* 模块内偏移是否可读 need 字节 —— 所有 RVA 读取都先过这里 */
inline bool in_range(unsigned offset, unsigned size, unsigned need)
{
    return offset <= size && need <= size - offset;
}

struct pe_layout
{
    const unsigned char *data;
    unsigned             size;
    unsigned             file_header;
    unsigned             opt_header;
    unsigned             sections;
    unsigned             num_sections;
    unsigned             size_opt;
    bool                 pe32plus;

    const unsigned char *opt() const { return data + opt_header; }
};

/* 可选头字段偏移：PE32 与 PE32+ 只在 ImageBase 之后错开，这里只取用到的几个 */
struct opt_offsets
{
    unsigned image_base;
    unsigned data_dir;
};

inline opt_offsets offsets_of(bool pe32plus)
{
    opt_offsets o;
    o.image_base = pe32plus ? 24 : 28;
    o.data_dir   = pe32plus ? 112 : 96;
    return o;
}

constexpr unsigned k_off_entry      = 16;
constexpr unsigned k_off_image_size = 56;
constexpr unsigned k_off_headers    = 60;

inline unsigned opt_u32(const pe_layout &l, unsigned rel)
{
    return rd_u32(l.opt() + rel);
}

inline unsigned long long opt_u64(const pe_layout &l, unsigned rel)
{
    unsigned long long lo = opt_u32(l, rel);
    unsigned long long hi = opt_u32(l, rel + 4);
    return lo | (hi << 32);
}

bool parse_layout(const void *file_data, unsigned file_size, pe_layout *out, load_error *err)
{
    const unsigned char *d = (const unsigned char *)file_data;

    if (d == nullptr || file_size < 64)
    {
        AUTODBG_PACK_FAIL(err, stage::header, "file too small", file_size);
        return false;
    }
    if (d[0] != 'M' || d[1] != 'Z')
    {
        AUTODBG_PACK_FAIL(err, stage::header, "bad DOS signature", 0);
        return false;
    }

    unsigned e_lfanew = rd_u32(d + 60);
    if (!in_range(e_lfanew, file_size, 24))
    {
        AUTODBG_PACK_FAIL(err, stage::header, "e_lfanew out of range", e_lfanew);
        return false;
    }
    if (d[e_lfanew] != 'P' || d[e_lfanew + 1] != 'E' || d[e_lfanew + 2] != 0 || d[e_lfanew + 3] != 0)
    {
        AUTODBG_PACK_FAIL(err, stage::header, "bad PE signature", 0);
        return false;
    }

    unsigned fh           = e_lfanew + 4;
    unsigned machine      = rd_u16(d + fh);
    unsigned num_sections = rd_u16(d + fh + 2);    /* IMAGE_FILE_HEADER.NumberOfSections */
    unsigned size_opt     = rd_u16(d + fh + 16);   /* IMAGE_FILE_HEADER.SizeOfOptionalHeader */

    if (machine != k_host_machine)
    {
        AUTODBG_PACK_FAIL(err, stage::header, "machine mismatch (module != host)", machine);
        return false;
    }
    if (num_sections == 0 || num_sections > k_max_sections)
    {
        AUTODBG_PACK_FAIL(err, stage::header, "NumberOfSections out of range", num_sections);
        return false;
    }

    unsigned opt = fh + 20;
    if (!in_range(opt, file_size, 2))
    {
        AUTODBG_PACK_FAIL(err, stage::header, "optional header truncated", opt);
        return false;
    }
    unsigned magic = rd_u16(d + opt);
    bool     plus  = (magic == 0x20B);
    if (magic != 0x10B && magic != 0x20B)
    {
        AUTODBG_PACK_FAIL(err, stage::header, "bad optional header magic", magic);
        return false;
    }
    if (!in_range(opt, file_size, size_opt))
    {
        AUTODBG_PACK_FAIL(err, stage::header, "SizeOfOptionalHeader out of range", size_opt);
        return false;
    }

    unsigned sec = opt + size_opt;
    if (!in_range(sec, file_size, 40 * num_sections))
    {
        AUTODBG_PACK_FAIL(err, stage::header, "section table truncated", sec);
        return false;
    }

    out->data         = d;
    out->size         = file_size;
    out->file_header  = fh;
    out->opt_header   = opt;
    out->sections     = sec;
    out->num_sections = num_sections;
    out->size_opt     = size_opt;
    out->pe32plus     = plus;
    return true;
}

struct section_info
{
    unsigned va;
    unsigned vsize;
    unsigned raw_ptr;
    unsigned raw_size;
    unsigned characteristics;
};

inline section_info section_at(const pe_layout &l, unsigned i)
{
    const unsigned char *s = l.data + l.sections + 40 * i;
    section_info info;
    info.vsize           = rd_u32(s + 8);
    info.va              = rd_u32(s + 12);
    info.raw_size        = rd_u32(s + 16);
    info.raw_ptr         = rd_u32(s + 20);
    info.characteristics = rd_u32(s + 36);
    return info;
}

/* 节属性 → 页保护（不产生任何 RWX 页） */
inline DWORD protect_of(unsigned characteristics)
{
    bool exec  = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    bool write = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;

    if (exec)
        return write ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return write ? PAGE_READWRITE : PAGE_READONLY;
}
}

mapped_module::mapped_module()
    : image_(nullptr)
    , image_size_(0)
    , preferred_base_(nullptr)
    , entry_rva_(0)
    , is_dll_(false)
    , started_(false)
    , table_added_(false)
    , table_(nullptr)
    , deps_(nullptr)
    , dep_count_(0)
    , dep_cap_(0)
{
}

mapped_module::~mapped_module()
{
    unmap();
}

bool mapped_module::dep_push(HMODULE handle)
{
    if (dep_count_ == dep_cap_)
    {
        unsigned cap = dep_cap_ ? dep_cap_ * 2 : 8;
        HMODULE *p = (HMODULE *)autodbg::common::resize(deps_, sizeof(HMODULE) * cap);
        deps_      = p;
        dep_cap_   = cap;
    }
    deps_[dep_count_++] = handle;
    return true;
}

void mapped_module::unmap()
{
#ifdef _WIN64
    if (table_added_ && table_ != nullptr)
    {
        RtlDeleteFunctionTable((PRUNTIME_FUNCTION)table_);
    }
#endif
    table_added_ = false;
    table_       = nullptr;

    if (image_ != nullptr)
    {
        VirtualFree(image_, 0, MEM_RELEASE);
        image_ = nullptr;
    }
    image_size_ = 0;

    for (unsigned i = 0; i < dep_count_; i++)
    {
        if (deps_[i] != nullptr)
            FreeLibrary(deps_[i]);
    }
    dep_count_ = 0;
    if (deps_ != nullptr)
    {
        autodbg::common::release(deps_);
        deps_ = nullptr;
    }
    dep_cap_ = 0;
    started_ = false;
}

bool mapped_module::map(const void *file_data, unsigned file_size, const map_options *opt, load_error *err)
{
    unmap();

    pe_layout layout;
    if (!parse_layout(file_data, file_size, &layout, err))
        return false;

    const opt_offsets o        = offsets_of(layout.pe32plus);
    const unsigned    size_img = opt_u32(layout, k_off_image_size);
    const unsigned    size_hdr = opt_u32(layout, k_off_headers);

    if (size_img == 0 || size_img > (1u << 30))
    {
        AUTODBG_PACK_FAIL(err, stage::header, "SizeOfImage out of range", size_img);
        return false;
    }
    if (size_hdr == 0 || size_hdr > size_img || size_hdr > file_size)
    {
        AUTODBG_PACK_FAIL(err, stage::header, "SizeOfHeaders out of range", size_hdr);
        return false;
    }

    preferred_base_ = (void *)(size_t)opt_u64(layout, o.image_base);
    entry_rva_      = opt_u32(layout, k_off_entry);
    is_dll_         = (rd_u16(layout.data + layout.file_header + 18) & IMAGE_FILE_DLL) != 0;

    /* --- 1) 分配镜像：优先首选基址，失败则任意位置 --- */
    void *image = VirtualAlloc(preferred_base_, size_img, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#ifdef _WIN64
    /* x64：镜像不得跨 4GB 边界（RVA 是 32 位） */
    while (image != nullptr &&
           ((((unsigned long long)(size_t)image) >> 32) != ((((unsigned long long)(size_t)image) + size_img) >> 32)))
    {
        VirtualFree(image, 0, MEM_RELEASE);
        image = VirtualAlloc(nullptr, size_img, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
#endif
    if (image == nullptr)
        image = VirtualAlloc(nullptr, size_img, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (image == nullptr)
    {
        AUTODBG_PACK_FAIL(err, stage::header, "VirtualAlloc failed", GetLastError());
        return false;
    }
    image_      = image;
    image_size_ = size_img;

    unsigned char *base = (unsigned char *)image;

    /* --- 2) 拷贝头，并把 ImageBase 改写成实际基址 --- */
    memcpy(base, file_data, size_hdr);
    {
        unsigned long long actual = (unsigned long long)(size_t)image;
        unsigned char *    p      = base + layout.opt_header + o.image_base;
        unsigned           width  = layout.pe32plus ? 8 : 4;
        for (unsigned i = 0; i < width; i++)
            p[i] = (unsigned char)((actual >> (8 * i)) & 0xFF);
    }

    /* --- 3) 节 --- */
    for (unsigned i = 0; i < layout.num_sections; i++)
    {
        section_info s = section_at(layout, i);

        if (s.va >= size_img || s.vsize > size_img - s.va)
        {
            AUTODBG_PACK_FAIL(err, stage::section, "section virtual range out of image", i);
            unmap();
            return false;
        }
        if (s.raw_size == 0)
            continue;                              /* 未初始化节：VirtualAlloc 已保证为 0 */

        /* ❗不能要求 raw_size <= vsize：/align:16 会把 SizeOfRawData 补齐到 FileAlignment，
         * 而 VirtualSize 是精确值 → 最后一个节常常 raw > virtual。真正的约束只有两条：
         * 文件内有这块数据、镜像里放得下。 */
        if (!in_range(s.raw_ptr, file_size, s.raw_size))
        {
            AUTODBG_PACK_FAIL(err, stage::section, "section raw range out of file", i);
            unmap();
            return false;
        }
        if (s.raw_size > size_img - s.va)
        {
            AUTODBG_PACK_FAIL(err, stage::section, "section raw range out of image", i);
            unmap();
            return false;
        }
        memcpy(base + s.va, layout.data + s.raw_ptr, s.raw_size);
    }

    /* --- 4) 重定位 --- */
    {
        unsigned long long delta      = (unsigned long long)(size_t)image - (unsigned long long)(size_t)preferred_base_;
        unsigned           reloc_rva  = opt_u32(layout, o.data_dir + 5 * 8);
        unsigned           reloc_size = opt_u32(layout, o.data_dir + 5 * 8 + 4);

        if (delta != 0)
        {
            if (reloc_rva == 0 || reloc_size == 0)
            {
                AUTODBG_PACK_FAIL(err, stage::reloc, "no relocation table but base differs", 0);
                unmap();
                return false;
            }
            if (reloc_rva >= size_img || reloc_size > size_img - reloc_rva)
            {
                AUTODBG_PACK_FAIL(err, stage::reloc, "relocation directory out of image", reloc_rva);
                unmap();
                return false;
            }

            unsigned off = reloc_rva;
            unsigned end = reloc_rva + reloc_size;
            while (off + 8 <= end)
            {
                unsigned page_rva = *(const unsigned *)(base + off);
                unsigned block    = *(const unsigned *)(base + off + 4);
                if (block < 8 || block > end - off)
                {
                    AUTODBG_PACK_FAIL(err, stage::reloc, "bad relocation block size", block);
                    unmap();
                    return false;
                }
                unsigned count = (block - 8) / 2;
                for (unsigned k = 0; k < count; k++)
                {
                    unsigned short item = *(const unsigned short *)(base + off + 8 + k * 2);
                    unsigned       type = item >> 12;
                    unsigned       rva  = page_rva + (item & 0xFFF);

                    if (type == IMAGE_REL_BASED_ABSOLUTE)
                        continue;
                    if (rva + 4 > size_img)
                    {
                        AUTODBG_PACK_FAIL(err, stage::reloc, "reloc target out of image", rva);
                        unmap();
                        return false;
                    }
                    if (type == IMAGE_REL_BASED_HIGHLOW)
                    {
                        *(unsigned *)(base + rva) += (unsigned)delta;
                    }
#ifdef _WIN64
                    else if (type == IMAGE_REL_BASED_DIR64)
                    {
                        *(unsigned long long *)(base + rva) += delta;
                    }
#endif
                    else
                    {
                        AUTODBG_PACK_FAIL(err, stage::reloc, "unsupported reloc type", type);
                        unmap();
                        return false;
                    }
                }
                off += block;
            }
        }
    }

    /* --- 5) 导入 --- */
    {
        unsigned imp_rva  = opt_u32(layout, o.data_dir + 1 * 8);
        unsigned imp_size = opt_u32(layout, o.data_dir + 1 * 8 + 4);

        if (imp_rva != 0 && imp_size != 0)
        {
            if (imp_rva >= size_img || imp_size > size_img - imp_rva)
            {
                AUTODBG_PACK_FAIL(err, stage::import, "import directory out of image", imp_rva);
                unmap();
                return false;
            }

            const unsigned char *desc     = base + imp_rva;
            const unsigned char *desc_end = desc + imp_size;

            while (desc + 20 <= desc_end)
            {
                unsigned name_rva = *(const unsigned *)(desc + 12);
                if (name_rva == 0)
                    break;
                if (name_rva >= size_img)
                {
                    AUTODBG_PACK_FAIL(err, stage::import, "import name rva out of image", name_rva);
                    unmap();
                    return false;
                }

                const char *dll_name = (const char *)(base + name_rva);
                HMODULE     handle   = nullptr;

                if (opt != nullptr && opt->load != nullptr)
                    handle = opt->load(dll_name, opt->ctx);
                else
                    handle = LoadLibraryA(dll_name);

                if (handle == nullptr)
                {
                    /* 名字在镜像里 → 先拷进 err->text 再回滚 */
                    AUTODBG_PACK_FAIL_STR(err, stage::import, dll_name, GetLastError());
                    unmap();
                    return false;
                }
                if (!dep_push(handle))
                {
                    AUTODBG_PACK_FAIL(err, stage::import, "dependency table out of memory", 0);
                    FreeLibrary(handle);
                    unmap();
                    return false;
                }

                unsigned oft = *(const unsigned *)(desc + 0);    /* OriginalFirstThunk */
                unsigned ft  = *(const unsigned *)(desc + 16);   /* FirstThunk */
                if (oft == 0 || oft >= size_img || ft >= size_img)
                {
                    AUTODBG_PACK_FAIL_STR(err, stage::import, dll_name, oft);
                    unmap();
                    return false;
                }

                const unsigned char *thunk = base + oft;
                unsigned char *      slot  = base + ft;

                for (;;)
                {
                    unsigned long long value;
                    unsigned           step;

                    if (layout.pe32plus)
                    {
                        if (!in_range((unsigned)(thunk - base), size_img, 8))
                        {
                            AUTODBG_PACK_FAIL_STR(err, stage::import, dll_name, 0);
                            unmap();
                            return false;
                        }
                        value = *(const unsigned long long *)thunk;
                        step  = 8;
                    }
                    else
                    {
                        if (!in_range((unsigned)(thunk - base), size_img, 4))
                        {
                            AUTODBG_PACK_FAIL_STR(err, stage::import, dll_name, 0);
                            unmap();
                            return false;
                        }
                        value = *(const unsigned *)thunk;
                        step  = 4;
                    }
                    if (value == 0)
                        break;

                    void *fn = nullptr;
                    if ((value >> 31) != 0)
                    {
                        /* 按序号导入（高位置 1） */
                        fn = (void *)GetProcAddress(handle, (const char *)(size_t)(value & 0xFFFF));
                        if (fn == nullptr)
                        {
                            AUTODBG_PACK_FAIL_STR(err, stage::import, dll_name, (unsigned)(value & 0xFFFF));
                            unmap();
                            return false;
                        }
                    }
                    else
                    {
                        unsigned    hint    = (unsigned)value;
                        unsigned    name_at = hint + 2;   /* 跳过 Hint/Name 表的 Hint */
                        if (name_at >= size_img)
                        {
                            AUTODBG_PACK_FAIL_STR(err, stage::import, dll_name, name_at);
                            unmap();
                            return false;
                        }
                        const char *symbol = (const char *)(base + name_at);
                        fn = (void *)GetProcAddress(handle, symbol);
                        if (fn == nullptr)
                        {
                            /* ❗detail 必须能活过 unmap() → 用 _STR 版本（拷进 err->text） */
                            AUTODBG_PACK_FAIL_STR(err, stage::import, symbol, hint);
                            unmap();
                            return false;
                        }
                    }

                    if (layout.pe32plus)
                    {
                        *(void **)slot = fn;
                        slot += 8;
                    }
                    else
                    {
                        *(void **)slot = fn;
                        slot += 4;
                    }
                    thunk += step;
                }

                desc += 20;
            }
        }
    }

    /* --- 6) x64 展开表 --- */
#ifdef _WIN64
    {
        unsigned exc_rva  = opt_u32(layout, o.data_dir + 3 * 8);
        unsigned exc_size = opt_u32(layout, o.data_dir + 3 * 8 + 4);
        if (exc_rva != 0 && exc_size != 0)
        {
            if (exc_rva >= size_img || exc_size > size_img - exc_rva)
            {
                AUTODBG_PACK_FAIL(err, stage::exception, "exception directory out of image", exc_rva);
                unmap();
                return false;
            }
            unsigned count = exc_size / 12;   /* sizeof(RUNTIME_FUNCTION) */
            if (RtlAddFunctionTable((PRUNTIME_FUNCTION)(base + exc_rva), count,
                                    (DWORD64)(size_t)image) == 0)
            {
                AUTODBG_PACK_FAIL(err, stage::exception, "RtlAddFunctionTable failed", GetLastError());
                unmap();
                return false;
            }
            table_added_ = true;
            table_       = (void *)(base + exc_rva);
        }
    }
#endif

    /* --- 7) 按节属性设保护 --- */
    for (unsigned i = 0; i < layout.num_sections; i++)
    {
        section_info s    = section_at(layout, i);
        unsigned     span = s.raw_size ? s.raw_size : s.vsize;
        if (span == 0)
            continue;

        unsigned start = s.va & ~0xFFFu;
        unsigned last  = s.va + span;
        unsigned end   = (last + 0xFFFu) & ~0xFFFu;
        if (start >= size_img)
            continue;
        if (end > size_img)
            end = size_img;

        DWORD old = 0;
        if (!VirtualProtect(base + start, end - start, protect_of(s.characteristics), &old))
        {
            AUTODBG_PACK_FAIL(err, stage::section, "VirtualProtect failed", i);
            unmap();
            return false;
        }
    }

    return true;
}

bool mapped_module::start(load_error *err)
{
    if (image_ == nullptr)
    {
        AUTODBG_PACK_FAIL(err, stage::entry, "module not mapped", 0);
        return false;
    }

    pe_layout layout;
    if (!parse_layout(image_, image_size_, &layout, err))
        return false;

    const opt_offsets o = offsets_of(layout.pe32plus);

    /* TLS 回调（F3：我们自己的模块都没有；为第三方 DLL 保留） */
    {
        unsigned tls_rva  = opt_u32(layout, o.data_dir + 9 * 8);
        unsigned tls_size = opt_u32(layout, o.data_dir + 9 * 8 + 4);
        unsigned cb_off   = layout.pe32plus ? 24u : 12u;
        unsigned cb_width = layout.pe32plus ? 8u : 4u;

        if (tls_rva != 0 && tls_size >= cb_off + cb_width && in_range(tls_rva, image_size_, cb_off + cb_width))
        {
            unsigned long long cb = layout.pe32plus
                ? *(unsigned long long *)((unsigned char *)image_ + tls_rva + cb_off)
                : *(unsigned *)((unsigned char *)image_ + tls_rva + cb_off);
            if (cb != 0)
            {
                typedef void(__stdcall *tls_cb)(void *h, unsigned reason, void *reserved);
                tls_cb *list = (tls_cb *)(size_t)cb;
                for (unsigned i = 0; list[i] != nullptr; i++)
                    list[i](image_, 1 /* DLL_PROCESS_ATTACH */, nullptr);
            }
        }
    }

    /* `/dll /noentry` 编出来的模块没有入口（AddressOfEntryPoint == 0）——
     * 这是合法的（真正的 Windows 加载器同样直接跳过 DllMain）。只有 EXE 必须有入口。 */
    if (entry_rva_ == 0)
    {
        if (!is_dll_)
        {
            AUTODBG_PACK_FAIL(err, stage::entry, "exe module has no entry point", 0);
            return false;
        }
        started_ = true;
        return true;
    }
    if (entry_rva_ >= image_size_)
    {
        AUTODBG_PACK_FAIL(err, stage::entry, "entry point out of image", entry_rva_);
        return false;
    }

    if (is_dll_)
    {
        typedef int(__stdcall *dll_main_fn)(void *h, unsigned reason, void *reserved);
        dll_main_fn entry = (dll_main_fn)((unsigned char *)image_ + entry_rva_);
        if (entry(image_, 1 /* DLL_PROCESS_ATTACH */, nullptr) == 0)
        {
            AUTODBG_PACK_FAIL(err, stage::entry, "DllMain returned FALSE", 0);
            return false;
        }
    }
    else
    {
        typedef int(__cdecl *exe_entry_fn)();
        exe_entry_fn entry = (exe_entry_fn)((unsigned char *)image_ + entry_rva_);
        entry();   /* EXE 入口通常不返回（内部 ExitProcess） */
    }
    started_ = true;
    return true;
}

void *mapped_module::resolve(const char *name) const
{
    if (image_ == nullptr || name == nullptr)
        return nullptr;

    pe_layout layout;
    if (!parse_layout(image_, image_size_, &layout, nullptr))
        return nullptr;

    const opt_offsets o = offsets_of(layout.pe32plus);

    unsigned exp_rva  = opt_u32(layout, o.data_dir);
    unsigned exp_size = opt_u32(layout, o.data_dir + 4);
    if (exp_rva == 0 || exp_size < 40 || exp_rva + 40 > image_size_)
        return nullptr;

    const unsigned char *dir = (const unsigned char *)image_ + exp_rva;

    unsigned num_names   = *(const unsigned *)(dir + 24);
    unsigned funcs_rva   = *(const unsigned *)(dir + 28);
    unsigned names_rva   = *(const unsigned *)(dir + 32);
    unsigned ords_rva    = *(const unsigned *)(dir + 36);

    if (num_names == 0 || !in_range(names_rva, image_size_, 4 * num_names) ||
        !in_range(ords_rva, image_size_, 2 * num_names) || funcs_rva >= image_size_)
        return nullptr;

    const unsigned *      names = (const unsigned *)((const unsigned char *)image_ + names_rva);
    const unsigned short *ords  = (const unsigned short *)((const unsigned char *)image_ + ords_rva);
    const unsigned *      funcs = (const unsigned *)((const unsigned char *)image_ + funcs_rva);

    for (unsigned i = 0; i < num_names; i++)
    {
        if (names[i] >= image_size_)
            continue;

        const char *a = (const char *)image_ + names[i];
        const char *b = name;
        while (*a != '\0' && *a == *b)
        {
            a++;
            b++;
        }
        if (*a != '\0' || *b != '\0')
            continue;

        unsigned idx = ords[i];
        if (!in_range(funcs_rva + idx * 4, image_size_, 4))
            return nullptr;

        unsigned fn_rva = funcs[idx];
        if (fn_rva >= image_size_)
            return nullptr;
        return (void *)((unsigned char *)image_ + fn_rva);
    }
    return nullptr;
}

void *mapped_module::resolve_ordinal(unsigned ordinal) const
{
    if (image_ == nullptr)
        return nullptr;

    pe_layout layout;
    if (!parse_layout(image_, image_size_, &layout, nullptr))
        return nullptr;

    const opt_offsets o = offsets_of(layout.pe32plus);

    unsigned exp_rva  = opt_u32(layout, o.data_dir);
    unsigned exp_size = opt_u32(layout, o.data_dir + 4);
    if (exp_rva == 0 || exp_size < 40 || exp_rva + 40 > image_size_)
        return nullptr;

    const unsigned char *dir = (const unsigned char *)image_ + exp_rva;

    unsigned base_index = *(const unsigned *)(dir + 16);
    unsigned num_funcs  = *(const unsigned *)(dir + 20);
    unsigned funcs_rva  = *(const unsigned *)(dir + 28);

    if (ordinal < base_index || (ordinal - base_index) >= num_funcs)
        return nullptr;
    if (!in_range(funcs_rva + (ordinal - base_index) * 4, image_size_, 4))
        return nullptr;

    const unsigned *funcs = (const unsigned *)((const unsigned char *)image_ + funcs_rva);
    unsigned        rva   = funcs[ordinal - base_index];
    if (rva >= image_size_)
        return nullptr;
    return (void *)((unsigned char *)image_ + rva);
}
}
