// Pointer-width dependent PEB structures for Peb.h.
//
// This file is included twice by Peb.h (once for 32-bit, once for 64-bit)
// with the following macros defined before inclusion:
//   SCL_PTR                       - pointer-sized type (DWORD / DWORD64)
//   SCL_NGF                       - type used to size the NtGlobalFlag union
//   SCL_GDI_HANDLE_BUFFER_LEN     - number of entries in GdiHandleBuffer
//   SCL_UNICODE_STRING            - NtApiShim UNICODE_STRING type (32/64)
//   SCL_LIST_ENTRY                - name for LIST_ENTRY
//   SCL_LDR_UNICODE_STRING        - name for the LDR UNICODE_STRING
//   SCL_LDR_DATA_TABLE_ENTRY      - name for LDR_DATA_TABLE_ENTRY
//   SCL_PEB_LDR_DATA              - name for PEB_LDR_DATA
//   SCL_PEB                       - name for PEB

#pragma pack(push, 1)

typedef struct SCL_LIST_ENTRY
{
    SCL_PTR Flink;
    SCL_PTR Blink;
} SCL_LIST_ENTRY;

typedef struct SCL_LDR_UNICODE_STRING
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
    SCL_PTR Buffer;
} SCL_LDR_UNICODE_STRING;

typedef struct SCL_LDR_DATA_TABLE_ENTRY
{
    SCL_LIST_ENTRY InLoadOrderLinks;
    SCL_LIST_ENTRY InMemoryOrderLinks;
    SCL_LIST_ENTRY InInitializationOrderLinks;
    SCL_PTR DllBase;
    SCL_PTR EntryPoint;
    union
    {
        DWORD SizeOfImage;
        SCL_PTR dummy01;
    };
    SCL_LDR_UNICODE_STRING FullDllName;
    SCL_LDR_UNICODE_STRING BaseDllName;
    DWORD Flags;
    WORD LoadCount;
    WORD TlsIndex;
    union
    {
        SCL_LIST_ENTRY HashLinks;
        struct
        {
            SCL_PTR SectionPointer;
            SCL_PTR CheckSum;
        };
    };
    union
    {
        SCL_PTR LoadedImports;
        DWORD TimeDateStamp;
    };
    SCL_PTR EntryPointActivationContext;
    SCL_PTR PatchInformation;
    SCL_LIST_ENTRY ForwarderLinks;
    SCL_LIST_ENTRY ServiceTagLinks;
    SCL_LIST_ENTRY StaticLinks;
    SCL_PTR ContextInformation;
    SCL_PTR OriginalBase;
    LARGE_INTEGER LoadTime;
} SCL_LDR_DATA_TABLE_ENTRY;

typedef struct SCL_PEB_LDR_DATA
{
    DWORD Length;
    DWORD Initialized;
    SCL_PTR SsHandle;
    SCL_LIST_ENTRY InLoadOrderModuleList;
    SCL_LIST_ENTRY InMemoryOrderModuleList;
    SCL_LIST_ENTRY InInitializationOrderModuleList;
    SCL_PTR EntryInProgress;
    DWORD ShutdownInProgress;
    SCL_PTR ShutdownThreadId;
} SCL_PEB_LDR_DATA;

typedef struct SCL_PEB
{
    union
    {
        struct
        {
            BYTE InheritedAddressSpace;
            BYTE ReadImageFileExecOptions;
            BYTE BeingDebugged;
            BYTE _SYSTEM_DEPENDENT_01;
        };
        SCL_PTR dummy01;
    };
    SCL_PTR Mutant;
    SCL_PTR ImageBaseAddress;
    SCL_PTR Ldr;
    SCL_PTR ProcessParameters;
    SCL_PTR SubSystemData;
    SCL_PTR ProcessHeap;
    SCL_PTR FastPebLock;
    SCL_PTR _SYSTEM_DEPENDENT_02;
    SCL_PTR _SYSTEM_DEPENDENT_03;
    SCL_PTR _SYSTEM_DEPENDENT_04;
    union
    {
        SCL_PTR KernelCallbackTable;
        SCL_PTR UserSharedInfoPtr;
    };
    DWORD SystemReserved;
    DWORD _SYSTEM_DEPENDENT_05;
    SCL_PTR _SYSTEM_DEPENDENT_06;
    SCL_PTR TlsExpansionCounter;
    SCL_PTR TlsBitmap;
    DWORD TlsBitmapBits[2];
    SCL_PTR ReadOnlySharedMemoryBase;
    SCL_PTR _SYSTEM_DEPENDENT_07;
    SCL_PTR ReadOnlyStaticServerData;
    SCL_PTR AnsiCodePageData;
    SCL_PTR OemCodePageData;
    SCL_PTR UnicodeCaseTableData;
    DWORD NumberOfProcessors;
    union
    {
        DWORD NtGlobalFlag;
        SCL_NGF dummy02;
    };
    LARGE_INTEGER CriticalSectionTimeout;
    SCL_PTR HeapSegmentReserve;
    SCL_PTR HeapSegmentCommit;
    SCL_PTR HeapDeCommitTotalFreeThreshold;
    SCL_PTR HeapDeCommitFreeBlockThreshold;
    DWORD NumberOfHeaps;
    DWORD MaximumNumberOfHeaps;
    SCL_PTR ProcessHeaps;
    SCL_PTR GdiSharedHandleTable;
    SCL_PTR ProcessStarterHelper;
    SCL_PTR GdiDCAttributeList;
    SCL_PTR LoaderLock;
    DWORD OSMajorVersion;
    DWORD OSMinorVersion;
    WORD OSBuildNumber;
    WORD OSCSDVersion;
    DWORD OSPlatformId;
    DWORD ImageSubsystem;
    DWORD ImageSubsystemMajorVersion;
    SCL_PTR ImageSubsystemMinorVersion;
    union
    {
        SCL_PTR ImageProcessAffinityMask;
        SCL_PTR ActiveProcessAffinityMask;
    };
    SCL_PTR GdiHandleBuffer[SCL_GDI_HANDLE_BUFFER_LEN];
    SCL_PTR PostProcessInitRoutine;
    SCL_PTR TlsExpansionBitmap;
    DWORD TlsExpansionBitmapBits[32];
    SCL_PTR SessionId;
    ULARGE_INTEGER AppCompatFlags;
    ULARGE_INTEGER AppCompatFlagsUser;
    SCL_PTR pShimData;
    SCL_PTR AppCompatInfo;
    SCL_UNICODE_STRING CSDVersion;
    SCL_PTR ActivationContextData;
    SCL_PTR ProcessAssemblyStorageMap;
    SCL_PTR SystemDefaultActivationContextData;
    SCL_PTR SystemAssemblyStorageMap;
    SCL_PTR MinimumStackCommit;
} SCL_PEB;

#pragma pack(pop)
