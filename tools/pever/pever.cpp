/* =============================================================================
 * tools/pever/pever.cpp —— 链接后把 PE 可选头的「OS 版本 / 子系统版本」改成 4.0（Plan §2.7 / D12）
 *
 * 来源：新写（Plan P1）。
 * 与旧实现的差异：无旧实现（PackEXE 原型没有这一步）。
 *
 * 为什么不能用 link 选项：`/subsystem:console,4.0` 实测只报 LNK4010 并写出 6.0（P0 §9.4 复验）。
 *
 * 用法：
 *   pever <file> [major.minor]     改写 OS 版本与子系统版本（默认 4.0）
 *   pever -q <file>                只读校验：打印当前两组版本，不改文件（门禁里断言用）
 *
 * 字段偏移（PE32 与 PE32+ 相同，opt = e_lfanew + 24）—— ImageVersion 夹在两组版本之间：
 *   opt+40 MajorOperatingSystemVersion   ← 改
 *   opt+42 MinorOperatingSystemVersion   ← 改
 *   opt+44 MajorImageVersion             ← 不动
 *   opt+46 MinorImageVersion             ← 不动
 *   opt+48 MajorSubsystemVersion         ← 改
 *   opt+50 MinorSubsystemVersion         ← 改
 * 这六个字段是 WORD（各 2 字节），连续 12 字节；只写这 12 字节：不重算 headersum，
 * 不碰其他目录，不碰 .pdb。
 *
 * 零 CRT 口径（AGENTS §5）：分配走 autodbg::common::alloc；打印走 WriteFile
 *   （工具不链 core，否则会把 dbghelp/ws2_32 带进构建期工具）。
 * ========================================================================== */
#include <windows.h>
#include <stddef.h>
#include <string.h>

#include "../../src/common/allocator.hpp"

/* 把 §2.7 的偏移钉死：结构变了就编译失败，而不是运行期读错字段 */
static_assert(offsetof(IMAGE_OPTIONAL_HEADER32, MajorOperatingSystemVersion) == 40, "PE32 OS 版本偏移变了");
static_assert(offsetof(IMAGE_OPTIONAL_HEADER32, MajorImageVersion) == 44, "PE32 Image 版本偏移变了");
static_assert(offsetof(IMAGE_OPTIONAL_HEADER32, MajorSubsystemVersion) == 48, "PE32 子系统版本偏移变了");
static_assert(offsetof(IMAGE_OPTIONAL_HEADER64, MajorOperatingSystemVersion) == 40, "PE32+ OS 版本偏移变了");
static_assert(offsetof(IMAGE_OPTIONAL_HEADER64, MajorImageVersion) == 44, "PE32+ Image 版本偏移变了");
static_assert(offsetof(IMAGE_OPTIONAL_HEADER64, MajorSubsystemVersion) == 48, "PE32+ 子系统版本偏移变了");

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

/* 无 CRT 的十进制格式化：把 v 写进 buf（至少 16 字节），返回 buf */
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

/* 定位 + 读 size 字节到 dst */
bool read_at(HANDLE file, unsigned long long offset, unsigned char *dst, unsigned size)
{
    LONG high = (LONG)(offset >> 32);
    LONG low  = (LONG)(offset & 0xFFFFFFFFULL);

    if (SetFilePointer(file, low, &high, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        return false;

    DWORD read = 0;
    if (!ReadFile(file, dst, size, &read, NULL) || read != size)
        return false;
    return true;
}

/* 小端读 / 写一个 WORD（PE 文件里这些版本字段是 WORD） */
unsigned read_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

void write_u16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

/* "major.minor" → 两个数；失败返回 false */
bool parse_version(const char *s, unsigned *major, unsigned *minor)
{
    const char *p = s;

    if (*p < '0' || *p > '9')
        return false;
    unsigned m = 0;
    while (*p >= '0' && *p <= '9')
        m = m * 10 + (unsigned)(*p++ - '0');
    if (*p != '.')
        return false;
    p++;
    if (*p < '0' || *p > '9')
        return false;
    unsigned n = 0;
    while (*p >= '0' && *p <= '9')
        n = n * 10 + (unsigned)(*p++ - '0');
    if (*p != '\0')
        return false;

    *major = m;
    *minor = n;
    return true;
}

}

int main(int argc, char **argv, char **envp)
{
    using autodbg::tool::out_write;
    using autodbg::tool::out_error;
    using autodbg::tool::out_u32;
    using autodbg::tool::read_at;
    using autodbg::tool::read_u16;
    using autodbg::tool::write_u16;
    using autodbg::tool::parse_version;

    int  arg         = 1;
    bool check_only  = false;
    bool quiet       = false;

    if (arg < argc && strcmp(argv[arg], "-q") == 0)
    {
        check_only = true;
        arg++;
    }

    if (arg < argc && strcmp(argv[arg], "-Q") == 0)
    {
        quiet = true;
        arg++;
    }
    if (arg >= argc)
    {
        out_write("usage: pever [-Q] <file.exe|file.dll> [major.minor]\n"
                  "       pever -q <file>\n");
        return 1;
    }

    const char *path = argv[arg++];

    unsigned major = 4;
    unsigned minor = 0;
    if (arg < argc && !parse_version(argv[arg], &major, &minor))
    {
        out_error("pever: bad version (expect major.minor)\n");
        return 1;
    }

    HANDLE file = CreateFileA(path, check_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE),
                              0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        out_error("pever: cannot open file\n");
        return 1;
    }

    unsigned char dos[64];
    if (!read_at(file, 0, dos, sizeof dos))
    {
        out_error("pever: cannot read DOS header\n");
        CloseHandle(file);
        return 1;
    }
    if (dos[0] != 'M' || dos[1] != 'Z')
    {
        out_error("pever: not a PE file (bad DOS signature)\n");
        CloseHandle(file);
        return 1;
    }
    unsigned long e_lfanew = (unsigned long)((unsigned)dos[60] | ((unsigned)dos[61] << 8) | ((unsigned)dos[62] << 16) | ((unsigned)dos[63] << 24));

    /* 可选头最大 240 字节（PE32+）：一次读 24 + 240 */
    unsigned char nt[24 + 240];
    if (!read_at(file, e_lfanew, nt, sizeof nt))
    {
        out_error("pever: cannot read NT header\n");
        CloseHandle(file);
        return 1;
    }
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != '\0' || nt[3] != '\0')
    {
        out_error("pever: bad PE signature\n");
        CloseHandle(file);
        return 1;
    }

    const unsigned char *opt = nt + 24;
    unsigned magic = read_u16(opt);
    if (magic != 0x10B && magic != 0x20B)
    {
        out_error("pever: unexpected optional header magic\n");
        CloseHandle(file);
        return 1;
    }

    unsigned os_major    = read_u16(opt + 40);
    unsigned os_minor    = read_u16(opt + 42);
    unsigned sys_major   = read_u16(opt + 48);
    unsigned sys_minor   = read_u16(opt + 50);

    if (check_only)
    {
        out_write("pever: ");
        out_write(path);
        out_write(" OSVer=");
        out_u32(os_major);
        out_write(".");
        out_u32(os_minor);
        out_write(" SubsysVer=");
        out_u32(sys_major);
        out_write(".");
        out_u32(sys_minor);
        out_write("\n");
        CloseHandle(file);
        return 0;
    }

    /* 12 字节连续：OS 版本(2+2) + ImageVersion(2+2，原样保留) + 子系统版本(2+2) */
    unsigned char block[12];
    if (!read_at(file, e_lfanew + 24 + 40, block, sizeof block))
    {
        out_error("pever: cannot reposition to optional header\n");
        CloseHandle(file);
        return 1;
    }
    write_u16(block + 0, major);
    write_u16(block + 2, minor);
    write_u16(block + 8, major);
    write_u16(block + 10, minor);

    /* e_lfanew 是 32 位文件偏移：高 32 位恒为 0（x86 上 >>32 是 UB，会报 C4293） */
    LONG high = 0;
    LONG low  = (LONG)(e_lfanew + 24 + 40);
    if (SetFilePointer(file, low, &high, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
    {
        out_error("pever: cannot seek to optional header\n");
        CloseHandle(file);
        return 1;
    }

    DWORD written = 0;
    if (!WriteFile(file, block, sizeof block, &written, NULL) || written != sizeof block)
    {
        out_error("pever: cannot write version fields\n");
        CloseHandle(file);
        return 1;
    }

    if(!quiet)
    {
        out_write("pever: ");
        out_write(path);
        out_write(" OSVer=");
        out_u32(major);
        out_write(".");
        out_u32(minor);
        out_write(" SubsysVer=");
        out_u32(major);
        out_write(".");
        out_u32(minor);
        out_write("\n");
    }

    CloseHandle(file);
    return 0;
}
