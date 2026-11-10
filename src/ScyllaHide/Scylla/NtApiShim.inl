// Pointer-width dependent structures for NtApiShim.h.
//
// This file is included twice by NtApiShim.h (once for 32-bit, once for 64-bit)
// with the following macros defined before inclusion:
//   SCL_PTR                              - pointer-sized type (DWORD / DWORD64)
//   SCL_UNICODE_STRING                   - name to give the UNICODE_STRING struct
//   SCL_CURDIR                           - name to give the CURDIR struct
//   SCL_PROCESS_BASIC_INFORMATION        - name to give the PROCESS_BASIC_INFORMATION struct
//   SCL_RTL_USER_PROCESS_PARAMETERS      - name to give the RTL_USER_PROCESS_PARAMETERS struct

typedef struct SCL_UNICODE_STRING
{
    union
    {
        struct
        {
            WORD Length;
            WORD MaximumLength;
        };
        SCL_PTR dummy;
    };
    SCL_PTR _Buffer;
} SCL_UNICODE_STRING;

typedef struct SCL_CURDIR
{
    SCL_UNICODE_STRING DosPath;
    SCL_PTR Handle;
} SCL_CURDIR;

typedef struct SCL_PROCESS_BASIC_INFORMATION
{
    DWORD ExitStatus;
    SCL_PTR PebBaseAddress;
    SCL_PTR AffinityMask;
    DWORD BasePriority;
    SCL_PTR UniqueProcessId;
    SCL_PTR InheritedFromUniqueProcessId;
} SCL_PROCESS_BASIC_INFORMATION;

typedef struct SCL_RTL_USER_PROCESS_PARAMETERS
{
    ULONG MaximumLength;
    ULONG Length;

    ULONG Flags;
    ULONG DebugFlags;

    SCL_PTR ConsoleHandle;
    ULONG  ConsoleFlags;
    SCL_PTR StandardInput;
    SCL_PTR StandardOutput;
    SCL_PTR StandardError;

    SCL_CURDIR CurrentDirectory;
    SCL_UNICODE_STRING DllPath;
    SCL_UNICODE_STRING ImagePathName;
    SCL_UNICODE_STRING CommandLine;
    SCL_PTR Environment;

    ULONG StartingX;
    ULONG StartingY;
    ULONG CountX;
    ULONG CountY;
    ULONG CountCharsX;
    ULONG CountCharsY;
    ULONG FillAttribute;

    ULONG WindowFlags;
    ULONG ShowWindowFlags;
    SCL_UNICODE_STRING WindowTitle;
    SCL_UNICODE_STRING DesktopInfo;
    SCL_UNICODE_STRING ShellInfo;
    SCL_UNICODE_STRING RuntimeData;
} SCL_RTL_USER_PROCESS_PARAMETERS;
