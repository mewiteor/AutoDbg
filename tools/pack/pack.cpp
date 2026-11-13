/* =============================================================================
 * tools/pack/pack.cpp —— 把若干模块压成单条 LZMA 流，生成启动器用的 g_packdata（Plan §2.4 / D14）
 *
 * 来源：重写 `tools/PackEXE/pe/src/compress.cpp`（Plan §3.3）。
 * 与旧实现的差异：
 *   ① 零 CRT + 无 STL（旧版用 iostream/vector/set/memory 与真 CRT）；
 *   ② 不再用 imagehlp/shlwapi 遍历导入图 —— 打包集合由命令行给出，导入表只做**审计**；
 *   ③ 载荷格式按 §2.4（无 magic / 无版本号），且按 D14 用**单流**；
 *   ④ 新增 `-verify`（往返逐字节）与 `-dump <dir>`（把解出来的模块落盘，供外部 sha256 比对）。
 *   ⑤ 扩展名从 .c 改 .cpp：D2 要求分配走 autodbg::common::alloc，而它是 C++ 接口；
 *   ⑥ T8 切片 a1（D63）：entry 加 `kind`（0 = module、1 = exec blob），blob 进同一条 LZMA 单流。
 *
 * 用法：
 *   pack <output.c> [-quiet] [-verify] [-dump <dir>] [--blob=<path>|--no-blob] <module_file>...
 *   ❗`--blob` / `--no-blob` 必须给出其一（D63，构建期挡）：`--blob=<path>` 把 x64 半份字节作为一条
 *   `kind = 1` 的 entry 打进产物，`--no-blob` 是「本次没有 blob 条目」的显式声明（给 x64 那一侧用——
 *   它就是 blob 本体）。两个都不给 → 报错退 1；两个都给 → 互斥报错。没有「静默无 blob」第三档。
 *   ❗选项一律排在文件列表之前（解析在第一个非选项参数处停下）；`--blob=<path>` 带值，走 `=` 内联。
 *
 * 载荷布局（小端，§2.4）：
 *   u32 entry_count
 *   entry: u16 name_len / char name[name_len]（小写、无路径）/ u32 raw_size / u8 kind（0 = module、1 = exec）
 *   u8 props[5]
 *   payload（单条 LZMA 流，各 entry 原始字节按 entry 顺序背靠背；blob 的字节原样参与，不重新编码）
 *
 * 压缩参数与原型逐字一致：level 9、dictSize 0、其余 -1。
 * 编码侧必须带 /D_7ZIP_ST（见 src/sys/lzma 的头注释）。
 * ========================================================================== */
#include <windows.h>
#include <string.h>
#include <stdlib.h>   /* _countof（影子头） */
#include <stddef.h>   /* offsetof（影子头） */

#include "../../src/common/allocator.hpp"
#include "../../src/sys/pack/payload.hpp"   /* D63：entry kind 常量 —— 与消费侧同一份口径（只取头，不链代码） */
#include "../../src/sys/lzma/7zTypes.h"
#include "../../src/sys/lzma/Alloc.h"
#include "../../src/sys/lzma/LzmaEnc.h"
#include "../../src/sys/lzma/LzmaDec.h"

/* 非白名单系统 DLL 又不在打包集合里 → 构建期直接报错（挡「漏打包 vcruntime140.dll」这类事故）。
 * 白名单 = 项目声明的系统库（AGENTS §7 的 SYSLIBS）+ ntdll / msvcrt / ucrtbase。
 *
 * ❗`gdi32.dll` 是 S4（GUI 自动化：截图 / 像素）新引入的：w95 数据集里有，
 *   而且是手写 DIB 截图的必要依赖（D24：PNG stored deflate；原 plan_server.md，现 plan.md §3）。
 *
 * ❗ucrtbase.dll 只出现在 x64 产物（make.bat 的 64 分支链 src\sys\ucrtbase.lib），
 *   x86 legacy 变体不得引入（Universal CRT 在 Win9x 不存在）。P3 接入 make.bat 时，
 *   应按变体传白名单（legacy 不允许 ucrtbase），而不是全局放行。 */
static const char *g_whitelist[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll", "comdlg32.dll",
    "shlwapi.dll", "dbghelp.dll", "ws2_32.dll", "psapi.dll", "ntdll.dll", "msvcrt.dll",
    "ucrtbase.dll"
};

struct pack_entry
{
    char           name[64];         /* 小写、无路径 */
    unsigned       raw_size;
    unsigned char  kind;             /* k_entry_kind_module / k_entry_kind_exec（D63） */
    unsigned char *data;             /* entry 原始字节（模块或 blob） */
};

/* blob 条目在 manifest 里的名字就是运行期落盘的文件名（D63：路径单一、永远 tmpdir）。
 * 与 --blob=<path> 指向的输入文件名无关——输入是构建期产物，名字随 build 目录变。 */
static const char *k_blob_name = "adbg64.bin";

static_assert(offsetof(IMAGE_FILE_HEADER, NumberOfSections) == 2, "IMAGE_FILE_HEADER.NumberOfSections 偏移变了");
static_assert(offsetof(IMAGE_FILE_HEADER, SizeOfOptionalHeader) == 16, "IMAGE_FILE_HEADER.SizeOfOptionalHeader 偏移变了");

namespace autodbg::tool
{

/* 工具侧输出：直接写标准句柄（WriteConsole 在重定向下会丢，门禁要能抓到） */
void out_write(const char *s)
{
    HANDLE  h       = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD   written = 0;
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, s, (DWORD)strlen(s), &written, NULL);
}

void out_error(const char *s)
{
    HANDLE  h       = GetStdHandle(STD_ERROR_HANDLE);
    DWORD   written = 0;
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, s, (DWORD)strlen(s), &written, NULL);
}

char *u32_to_str(char *buf, unsigned v)
{
    char tmp[16];
    int  i = 0;
    int  n = 0;

    do
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);

    while (i > 0)
        buf[n++] = tmp[--i];
    buf[n] = '\0';
    return buf;
}

void out_u32(unsigned v)
{
    char buf[16];
    out_write(u32_to_str(buf, v));
}

unsigned le16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

unsigned le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

void put_le16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

void put_le32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* 大小写不敏感比较（工具侧不用 CRT 的 _stricmp） */
bool str_ieq(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

bool starts_with(const char *s, const char *prefix)
{
    for (; *prefix != '\0'; prefix++)
        if (*s++ != *prefix)
            return false;
    return true;
}

/* 取文件名（去路径）并转小写，写进 dst（dst 至少 64 字节） */
void base_name_lower(const char *path, char *dst, unsigned dst_size)
{
    const char *base = path;
    for (const char *q = path; *q; q++)
        if (*q == '\\' || *q == '/')
            base = q + 1;

    unsigned n = 0;
    for (; *base && n + 1 < dst_size; base++, n++)
    {
        char c = *base;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        dst[n] = c;
    }
    dst[n] = '\0';
}

/* 读整个文件到项目分配器；失败返回 NULL */
unsigned char *read_file(const char *path, unsigned *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    unsigned long hi = 0;
    unsigned long lo = GetFileSize(h, &hi);
    unsigned long long total = ((unsigned long long)hi << 32) | lo;
    if (total == 0 || total > 0xFFFFFFFFULL)
    {
        CloseHandle(h);
        return NULL;
    }

    unsigned char *buf = (unsigned char *)autodbg::common::alloc((size_t)total);
    DWORD read = 0;
    if (!ReadFile(h, buf, (DWORD)total, &read, NULL) || read != (DWORD)total)
    {
        CloseHandle(h);
        autodbg::common::release(buf);
        return NULL;
    }
    CloseHandle(h);
    *size = (unsigned)total;
    return buf;
}

}

/* RVA → 文件偏移（读的是磁盘上的裸文件镜像，不是加载后的内存镜像，所以每个 RVA 都要换算） */
static unsigned rva_to_offset(const unsigned char *sections, unsigned num_sections, unsigned rva)
{
    for (unsigned i = 0; i < num_sections; i++)
    {
        const unsigned char *s = sections + 40 * i;
        unsigned va  = autodbg::tool::le32(s + 12);   /* VirtualAddress */
        unsigned vsz = autodbg::tool::le32(s + 8);    /* VirtualSize */
        unsigned ptr = autodbg::tool::le32(s + 20);   /* PointerToRawData */
        if (rva >= va && rva < va + vsz)
            return ptr + (rva - va);
    }
    return 0xFFFFFFFFU;
}

/* 导入审计：非白名单且不在打包集合里的依赖 → 报错 */
static bool audit_imports(const unsigned char *img, const char *module_name,
                          const pack_entry *entries, unsigned entry_count)
{
    using autodbg::tool::le16;
    using autodbg::tool::le32;
    using autodbg::tool::str_ieq;

    unsigned long e_lfanew = le32(img + 60);
    const unsigned char *nt = img + e_lfanew;
    if (le32(nt) != 0x00004550U)
    {
        autodbg::tool::out_error("pack: module is not a PE file\n");
        return false;
    }

    const unsigned char *file_hdr = nt + 4;
    unsigned num_sections = le16(file_hdr + 2);    /* IMAGE_FILE_HEADER.NumberOfSections（+2，不是 +6） */
    unsigned size_opt     = le16(file_hdr + 16);   /* IMAGE_FILE_HEADER.SizeOfOptionalHeader（+16，不是 +20） */
    const unsigned char  *opt     = file_hdr + 20;
    unsigned magic        = le16(opt);
    unsigned dd_base      = (magic == 0x20B) ? 112 : 96;   /* DataDirectory 起点（PE32 / PE32+） */
    unsigned import_rva   = le32(opt + dd_base + 8);        /* DataDirectory[1] */
    unsigned import_size  = le32(opt + dd_base + 12);
    if (import_rva == 0 || import_size == 0)
        return true;

    const unsigned char *sections = opt + size_opt;
    unsigned base  = rva_to_offset(sections, num_sections, import_rva);
    if (base == 0xFFFFFFFFU)
    {
        autodbg::tool::out_error("pack: import directory not covered by any section\n");
        return false;
    }

    const unsigned char *d = img + base;
    while (le32(d + 12) != 0)   /* Name RVA */
    {
        unsigned name_off = rva_to_offset(sections, num_sections, le32(d + 12));
        if (name_off == 0xFFFFFFFFU)
        {
            autodbg::tool::out_error("pack: import name RVA out of range\n");
            return false;
        }
        const char *dll = (const char *)(img + name_off);

        bool ok = false;
        for (unsigned i = 0; i < _countof(g_whitelist); i++)
            if (str_ieq(dll, g_whitelist[i])) { ok = true; break; }
        if (!ok)
            for (unsigned i = 0; i < entry_count; i++)
                if (str_ieq(dll, entries[i].name)) { ok = true; break; }

        if (!ok)
        {
            autodbg::tool::out_write("pack: ");
            autodbg::tool::out_write(module_name);
            autodbg::tool::out_write(" imports non-system dependency not in pack set: ");
            autodbg::tool::out_write(dll);
            autodbg::tool::out_write("\n");
            return false;
        }
        d += 20;
    }
    return true;
}

int main(int argc, char **argv, char **envp)
{
    using autodbg::tool::out_write;
    using autodbg::tool::out_error;
    using autodbg::tool::out_u32;
    using autodbg::tool::read_file;
    using autodbg::tool::base_name_lower;
    using autodbg::tool::put_le16;
    using autodbg::tool::put_le32;
    using autodbg::tool::starts_with;
    using autodbg::sys::pack::k_entry_kind_module;
    using autodbg::sys::pack::k_entry_kind_exec;

    if (argc < 3)
    {
        out_write("usage: pack <output.c> [-quiet] [-verify] [-dump <dir>] [--blob=<path>|--no-blob] <module_file>...\n");
        return 1;
    }

    const char *out_path  = argv[1];
    const char *dump_dir  = NULL;
    const char *blob_path = NULL;      /* --blob=<path>：一条 kind = 1 的 entry（D63） */
    bool        no_blob   = false;     /* --no-blob：显式声明本次没有 blob 条目（D63） */
    bool        verify    = false;
    bool        quiet     = false;
    int         module_first = argc;

    for (int a = 2; a < argc; a++)
    {
        if (strcmp(argv[a], "-verify") == 0)
            verify = true;
        else if (strcmp(argv[a], "-quiet") == 0)
            quiet = true;
        else if (strcmp(argv[a], "-dump") == 0 && a + 1 < argc)
        {
            dump_dir = argv[++a];
        }
        else if (starts_with(argv[a], "--blob="))
        {
            blob_path = argv[a] + 7;
        }
        else if (strcmp(argv[a], "--no-blob") == 0)
        {
            no_blob = true;
        }
        else
        {
            module_first = a;
            break;
        }
    }

    /* D63：blob 的有无必须是**显式**的 —— 两个都不给 = 构建期直接报错（没有静默无 blob） */
    if (blob_path != NULL && no_blob)
    {
        out_error("pack: --blob and --no-blob are mutually exclusive\n");
        return 1;
    }
    if (blob_path == NULL && !no_blob)
    {
        out_error("pack: --blob=<path> or --no-blob is required\n");
        return 1;
    }

    if (module_first >= argc)
    {
        out_error("pack: no module file given\n");
        return 1;
    }

    unsigned   file_count = (unsigned)(argc - module_first);
    unsigned   count      = file_count + ((blob_path != NULL) ? 1u : 0u);
    pack_entry *entries = (pack_entry *)autodbg::common::alloc(sizeof(pack_entry) * count);

    /* 1) 读入模块 */
    unsigned raw_total = 0;
    for (unsigned e = 0; e < file_count; e++)
    {
        const char *path = argv[module_first + (int)e];
        unsigned    size = 0;
        unsigned char *data = read_file(path, &size);
        if (data == NULL)
        {
            out_error("pack: cannot read module\n");
            return 1;
        }
        base_name_lower(path, entries[e].name, sizeof entries[e].name);
        entries[e].raw_size = size;
        entries[e].kind     = k_entry_kind_module;
        entries[e].data     = data;
        raw_total += size;

    }

    /* 1b) blob 条目（D63）：排在文件列表条目之后（manifest 顺序 = 单流字节顺序），
     *     字节原样参与单流；名字固定为 `adbg64.bin`，读不到 → 报错退 1。 */
    if (blob_path != NULL)
    {
        unsigned      blob_size = 0;
        unsigned char *blob_data = read_file(blob_path, &blob_size);
        if (blob_data == NULL)
        {
            out_error("pack: cannot read blob\n");
            return 1;
        }

        pack_entry *be   = &entries[file_count];
        unsigned    nlen = (unsigned)strlen(k_blob_name);
        memcpy(be->name, k_blob_name, nlen);
        be->name[nlen] = '\0';
        be->raw_size   = blob_size;
        be->kind       = k_entry_kind_exec;
        be->data       = blob_data;
        raw_total     += blob_size;
    }

    /* 2) 导入审计：blob 条目也是 PE，照同一个白名单跑（X1）。
     *    pack 侧只有「白名单 + 是否在打包集合」这一层，没有按 OS 的数据集——按 OS 选口径的是
     *    `tools/api_check/scan_dir.py`（blob 是 x64 → w7 口径，跑在**构建期的 blob 产物**上，不扫 tmpdir）。
     *    白名单里的 `ucrtbase.dll` 只应出现在 x64 半份（X1 的按变体白名单口径未改）。 */
    for (unsigned e = 0; e < count; e++)
        if (!audit_imports(entries[e].data, entries[e].name, entries, count))
            return 1;

    /* 3) 压缩（单流，D14） */
    unsigned char *raw = (unsigned char *)autodbg::common::alloc(raw_total);
    unsigned off = 0;
    for (unsigned e = 0; e < count; e++)
    {
        memcpy(raw + off, entries[e].data, entries[e].raw_size);
        off += entries[e].raw_size;
    }

    unsigned      cap    = raw_total + raw_total / 3 + 128;
    unsigned char *comp  = (unsigned char *)autodbg::common::alloc(cap);
    unsigned char props[LZMA_PROPS_SIZE];
    size_t        propsSize = LZMA_PROPS_SIZE;

    CLzmaEncProps enc_props;
    LzmaEncProps_Init(&enc_props);
    enc_props.level    = 9;
    enc_props.dictSize = 0;

    size_t compLen = cap;
    SRes   r = LzmaEncode(comp, &compLen, raw, raw_total, &enc_props, props, &propsSize, 0, NULL, &g_Alloc, &g_Alloc);
    if (r != SZ_OK)
    {
        out_error("pack: LzmaEncode failed\n");
        return 1;
    }

    /* 4) 往返校验（逐字节）+ 可选落盘 */
    if (verify)
    {
        unsigned char *out = (unsigned char *)autodbg::common::alloc(raw_total);
        size_t outLen = raw_total;
        size_t inLen  = compLen;
        ELzmaStatus status;   /* LzmaDecode 会无条件写 *status → 不能传 NULL */
        SRes   dr = LzmaDecode(out, &outLen, comp, &inLen, props, (unsigned)propsSize, LZMA_FINISH_ANY, &status, &g_Alloc);
        if (dr != SZ_OK || outLen != raw_total || memcmp(out, raw, raw_total) != 0)
        {
                out_error("pack: round-trip mismatch\n");
            return 1;
        }
        out_write("pack: round-trip ok\n");

        /* blob 条目按它的名字落文件（`adbg64.bin`），不当模块处理：pack 侧没有映射器，
         * `-verify` 对 blob 就是「解码长度 + 逐字节」这一层校验。 */
        if (dump_dir != NULL)
        {
            unsigned o = 0;
            for (unsigned e = 0; e < count; e++)
            {
                char path[260];
                unsigned n = (unsigned)strlen(dump_dir);
                memcpy(path, dump_dir, n);
                path[n++] = '\\';
                unsigned m = 0;
                for (; entries[e].name[m] && n + m + 1 < 256; m++)
                    path[n + m] = entries[e].name[m];
                path[n + m] = '\0';

                HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE)
                {
                    out_error("pack: cannot write dump file\n");
                    return 1;
                }
                DWORD written = 0;
                if (!WriteFile(h, out + o, entries[e].raw_size, &written, NULL) || written != entries[e].raw_size)
                {
                    CloseHandle(h);
                    out_error("pack: dump write failed\n");
                    return 1;
                }
                CloseHandle(h);
                o += entries[e].raw_size;
            }
            out_write("pack: dumped modules\n");
        }
    }

    /* 5) 生成 .c */
    /* D63：每条 entry 在 raw_size 之后多一个 `u8 kind` → 每条 +1 */
    unsigned manifest_len = 4;
    for (unsigned e = 0; e < count; e++)
        manifest_len += 2 + (unsigned)strlen(entries[e].name) + 4 + 1;

    unsigned total = manifest_len + (unsigned)propsSize + (unsigned)compLen;
    unsigned char *packdata = (unsigned char *)autodbg::common::alloc(total);

    unsigned char *p = packdata;
    put_le32(p, count);
    p += 4;
    for (unsigned e = 0; e < count; e++)
    {
        unsigned nlen = (unsigned)strlen(entries[e].name);
        put_le16(p, nlen);
        p += 2;
        memcpy(p, entries[e].name, nlen);
        p += nlen;
        put_le32(p, entries[e].raw_size);
        p += 4;
        *p++ = entries[e].kind;   /* D63：u8 kind（小端单字节） */
    }
    memcpy(p, props, propsSize);
    p += propsSize;
    memcpy(p, comp, compLen);

    HANDLE out = CreateFileA(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE)
    {
        out_error("pack: cannot write output .c\n");
        return 1;
    }

    char text[512];
    unsigned len = 0;
    char num[16];

    #define EMIT(s) do { \
        unsigned n = (unsigned)strlen(s); \
        if (len + n > sizeof text) { WriteFile(out, text, len, &written, NULL); len = 0; } \
        memcpy(text + len, s, n); len += n; \
    } while (0)

    DWORD written = 0;
    EMIT("/* 由 tools/pack/pack.cpp 生成 —— 不要手工编辑（Plan §2.4 / D14） */\n");
    EMIT("const unsigned char g_packdata[] = {\n");
    for (unsigned i = 0; i < total; i++)
    {
        if (i)
            EMIT(",");
        EMIT(autodbg::tool::u32_to_str(num, packdata[i]));
    }
    EMIT("\n};\n");
    EMIT("const unsigned int g_packdata_size = ");
    EMIT(autodbg::tool::u32_to_str(num, total));
    EMIT(";\n");
    if (len)
        WriteFile(out, text, len, &written, NULL);
    #undef EMIT

    CloseHandle(out);

    if(!quiet)
    {
        out_write("pack: ");
        out_write(out_path);
        out_write(" raw=");
        out_u32(raw_total);
        out_write(" comp=");
        out_u32((unsigned)compLen);
        out_write(" ratio=");
        /* 用 64 位算：compLen * 10000 在 32 位下会溢出（x86 四模块就溢出） */
        out_u32((unsigned)((unsigned long long)compLen * 10000ULL / raw_total));
        out_write(" (1e-4)\n");
    }
    return 0;
}
