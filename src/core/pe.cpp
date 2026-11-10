/* =============================================================================
 * src/core/pe.cpp —— PE / 模块信息（P3.B 模块拆分，逐字从 bindings.cpp 迁入）
 *
 * 内容：模块名反查、地址反查、PE 解析（导入表/导出表/重定位/异常目录）。
 * 改动仅两处：TranslateNativeName / get_peinfo_by_ea 去掉 static（跨模块调用）。
 * 依赖：g_peinfo_list（engine.hpp 的 extern）+ util 的错误/日志。
 * ========================================================================== */
#include "pe.hpp"
#include "core.hpp" /* P6.4：引擎函数是会话成员（本 TU 多数定义即成员，直接用自身状态） */
#include "types.hpp"
#include "util.hpp"

#include <dbghelp.h>
#include <psapi.h>
#include <assert.h>
#include <intrin.h>
#include <strsafe.h>
#include <string.h>
#include <shlwapi.h>

/* 类型统一（删 legacy_types.h）：这些名字现在只有 types.hpp 里的 `autodbg::core::` 那套 */
using autodbg::core::PEInfo;
using autodbg::core::DebuggerSession; /* P6.4：引擎函数已是它的成员 */
using autodbg::core::ParentFuncT;
LPCTSTR TranslateNativeName(LPTSTR nativeName)
{
    TCHAR name[5];
    name[1] = TEXT(':');
    name[2] = 0;
    TCHAR tname[MAX_FILEPATH];

    // A ~ Z
    for (TCHAR x = 'A'; x <= 'Z'; x++)
    {
        name[0] = x;
        ZeroMemory(tname, sizeof tname);
        if (!QueryDosDevice(name, tname, _countof(tname)))
            continue;
        size_t n = _tcslen(tname);
        tname[n] = TEXT('\\');
        if (memcmp(tname, nativeName, n * sizeof(TCHAR)))
            continue;
        nativeName[n - 2] = x;
        nativeName[n - 1] = TEXT(':');
        return nativeName + n - 2;
    }

    // COM0 ~ COM9
    name[0] = TEXT('C');
    name[1] = TEXT('O');
    name[2] = TEXT('M');
    for (TCHAR x = '0'; x <= '9'; x++)
    {
        name[3] = x;
        name[4] = '\0';
        ZeroMemory(tname, sizeof tname);
        if (!QueryDosDevice(name, tname, _countof(tname)))
            continue;
        size_t n = _tcslen(tname);
        tname[n] = '\\';
        if (memcmp(tname, nativeName, n * sizeof(TCHAR)))
            continue;
        nativeName[n - 4] = TEXT('C');
        nativeName[n - 3] = TEXT('O');
        nativeName[n - 2] = TEXT('M');
        nativeName[n - 1] = x;
        return nativeName + n - 4;
    }
    return nativeName;
}

PEInfo *DebuggerSession::get_peinfo_by_name(LPCTSTR modname)
{
    for (PEInfo *info = peinfos().head(); info; info = info->next)
    {
        LPCTSTR wname = (LPCTSTR)(info->Path + info->name_offset);
        if (wname && !_tcsicmp(wname, modname))
            return info;
    }
    return NULL;
}

PEInfo *DebuggerSession::get_peinfo_by_ea(size_t ea)
{
    for (PEInfo *info = peinfos().head(); info; info = info->next)
    {
        if (info->base <= ea && ea < info->base + info->size)
            return info;
    }
    return NULL;
}

size_t DebuggerSession::get_base(LPCTSTR name)
{
    PEInfo *peinfo = get_peinfo_by_name(name);
    return peinfo ? peinfo->base : 0;
}

#ifdef _WIN64
static int compare_runtime_function(const void *a, const void *b)
{
    PRUNTIME_FUNCTION aa = (PRUNTIME_FUNCTION)a;
    PRUNTIME_FUNCTION bb = (PRUNTIME_FUNCTION)b;
    return aa->BeginAddress == bb->BeginAddress ? 0 : aa->BeginAddress < bb->BeginAddress ? -1 : 1;
}

static int compare_parent_function(const void *a, const void *b)
{
    ParentFuncT *aa = (ParentFuncT *)a;
    ParentFuncT *bb = (ParentFuncT *)b;
    return aa->begin_addr < bb->begin_addr ? -1 : aa->begin_addr > bb->begin_addr ? 1 : 0;
}
#endif

static void parse_pe_sub(PBYTE pMapping, PEInfo *peinfo)
{
    // 1. 读取DOS头
    PIMAGE_DOS_HEADER pDosHdr = (PIMAGE_DOS_HEADER)pMapping;
    if (pDosHdr->e_magic != IMAGE_DOS_SIGNATURE)
        return;

    // 2. 读取NT头
    PIMAGE_NT_HEADERS64 pNtHdr = (PIMAGE_NT_HEADERS64)(pMapping + pDosHdr->e_lfanew);
    if (pNtHdr->Signature != IMAGE_NT_SIGNATURE)
        return;
    PIMAGE_FILE_HEADER       pFileHdr = &pNtHdr->FileHeader;
    PIMAGE_OPTIONAL_HEADER64 pOptHdr  = &pNtHdr->OptionalHeader;

#ifdef _WIN64
    if (pOptHdr->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || pFileHdr->Machine != IMAGE_FILE_MACHINE_AMD64)
    {
        LOG(TEXT("%s is not 64bit PE: %hX"), peinfo->Path, pFileHdr->Machine);
        return;
    }
#else
    if (pOptHdr->Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || pFileHdr->Machine != IMAGE_FILE_MACHINE_I386)
    {
        LOG(TEXT("%s is not 32bit PE: %hX"), peinfo->Path, pFileHdr->Machine);
        return;
    }
#endif

    // 3. 读取节表
    PIMAGE_SECTION_HEADER pSections     = (PIMAGE_SECTION_HEADER)((PBYTE)pOptHdr + pFileHdr->SizeOfOptionalHeader);
    int                   section_count = pFileHdr->NumberOfSections;
    for (int i = 0; i < section_count; i++)
    {
        PIMAGE_SECTION_HEADER p = &pSections[i];
        if (!p->VirtualAddress || !p->SizeOfRawData)
            continue;
        assert((ULONGLONG)p->VirtualAddress + p->SizeOfRawData < 0x100000000ull);
        DWORD sz = p->VirtualAddress + p->SizeOfRawData;
        if (sz > peinfo->size)
            peinfo->size = sz;
    }

    // 4. 创建PE信息结构
    peinfo->section_count = section_count;
    peinfo->sections      = AllocX(IMAGE_SECTION_HEADER, section_count);

    if (!peinfo->sections)
        return;
    CopyMem(peinfo->sections, pSections, section_count * sizeof(IMAGE_SECTION_HEADER));

#ifdef _WIN64
    // 5. 解析异常目录 (Exception Directory)
    PIMAGE_DATA_DIRECTORY pExceptionDir = &pOptHdr->DataDirectory[3]; // IMAGE_DIRECTORY_ENTRY_EXCEPTION
    if (!pExceptionDir->VirtualAddress || !pExceptionDir->Size)
        return;
    // 查找包含异常目录的节区
    DWORD fileOffset = 0;
    for (int i = 0; i < section_count; i++)
    {
        PIMAGE_SECTION_HEADER section = &peinfo->sections[i];
        if (pExceptionDir->VirtualAddress >= section->VirtualAddress && pExceptionDir->VirtualAddress < section->VirtualAddress + section->SizeOfRawData)
        {
            // 计算文件偏移
            fileOffset = section->PointerToRawData + (pExceptionDir->VirtualAddress - section->VirtualAddress);
            break;
        }
    }

    if (!fileOffset)
        return;

    // 映射到内存地址
    PRUNTIME_FUNCTION pRuntimeFunctions    = (PRUNTIME_FUNCTION)((PBYTE)pMapping + fileOffset);
    int               runtimeFunctionCount = pExceptionDir->Size / sizeof(RUNTIME_FUNCTION);

    if (runtimeFunctionCount <= 0)
        return;
    peinfo->runtime_function_count = runtimeFunctionCount;
    peinfo->runtime_functions      = AllocX(RUNTIME_FUNCTION, runtimeFunctionCount);

    if (!peinfo->runtime_functions)
        return;
    CopyMem(peinfo->runtime_functions, pRuntimeFunctions, runtimeFunctionCount * sizeof(RUNTIME_FUNCTION));

    // 处理间接函数引用，构建parent_functions
    int parentFunctionCount = 0;
    for (int i = 0; i < runtimeFunctionCount; i++)
    {
        PRUNTIME_FUNCTION rf = &((PRUNTIME_FUNCTION)peinfo->runtime_functions)[i];
        if (rf->UnwindData & 1)
            parentFunctionCount++; // RUNTIME_FUNCTION_INDIRECT
    }

    if (parentFunctionCount <= 0)
        return;
    peinfo->parent_function_count = parentFunctionCount;
    peinfo->parent_functions      = AllocX(ParentFuncT, parentFunctionCount);

    if (!peinfo->parent_functions)
        return;
    int parentIndex = 0;
    for (int i = 0; i < runtimeFunctionCount; i++)
    {
        PRUNTIME_FUNCTION rf = &((PRUNTIME_FUNCTION)peinfo->runtime_functions)[i];
        if (rf->UnwindData & 1) // RUNTIME_FUNCTION_INDIRECT
        {
            DWORD parentRva   = rf->UnwindData & ~1;
            DWORD relativeRva = parentRva - pExceptionDir->VirtualAddress;

            assert(relativeRva < pExceptionDir->Size);
            int parentIndex2 = relativeRva / sizeof(RUNTIME_FUNCTION);
            assert(parentIndex2 < runtimeFunctionCount);
            PRUNTIME_FUNCTION parentRf                       = &pRuntimeFunctions[parentIndex2];
            peinfo->parent_functions[parentIndex].rva        = parentRva;
            peinfo->parent_functions[parentIndex].begin_addr = parentRf->BeginAddress;
            parentIndex++;
        }
    }
    assert(parentIndex == parentFunctionCount);
    qsort(peinfo->runtime_functions, peinfo->runtime_function_count, sizeof(RUNTIME_FUNCTION), compare_runtime_function);
    qsort(peinfo->parent_functions, peinfo->parent_function_count, sizeof(ParentFuncT), compare_parent_function);
#endif
}

/**
 * parse_pe - 解析PE文件获取节区信息
 * @param base 模块基址
 * @param fullpath 模块完整路径
 */
void DebuggerSession::parse_pe(PEInfo *peinfo)
{
    HANDLE hFile = CreateFile(peinfo->Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (hMapping)
    {
        LPVOID pMapping = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        if (pMapping)
        {
            parse_pe_sub((PBYTE)pMapping, peinfo);
            UnmapViewOfFile(pMapping);
        }
        CloseHandle(hMapping);
    }
    CloseHandle(hFile);
}