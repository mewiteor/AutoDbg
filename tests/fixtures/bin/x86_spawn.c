/* =============================================================================
 * tests/fixtures/bin/x86_spawn.c —— 「会派生子进程」的被调试目标（T7 切片 C2b）
 *
 * 文件职责：门禁用的**进程树**靶子 —— 父进程用 `CreateProcessA` 起**自己**（同一个 exe，
 *   `/child <父pid>` 参数）当子进程，父子都跑同一段固定的标记代码，然后各自等对方退出。
 *   门禁只看三件事：进程树里有几个 pid、**按 pid 的句柄**读到的是不是各自进程的内存、
 *   每个进程的标记变量是不是各自的值（不打印、不落盘、不做文件 I/O）。
 * 来源：新写（plan_t7.md §10 / §11.7 的 C2b；`x86_loop.c` 是单进程口径，不能覆盖这一层）。
 * 与旧实现的差异：无旧实现 —— 之前没有「一个会话 = 一棵进程树」的门禁靶子。
 *
 * 口径（硬）：
 *   - 零 CRT：不 include 任何头（连影子头都不需要）；入口 mainCRTStartup 由 src/sys/main.c
 *     提供，链接走 ld 规则（/nodefaultlib）。用到的 kernel32 导出在下面自己声明。
 *   - 导入表必须过 w95（scan_dir.py 对 .out/fixtures/ 自动选 w95）⇒ 禁止任何 XP+ 才有的调用。
 *     本文件用到的 `CreateProcessA` / `GetModuleFileNameA` / `GetCurrentProcessId` /
 *     `OpenProcess` / `WaitForSingleObject` / `Sleep` / `CloseHandle` 全部在
 *     `tools/api_check/datas.bin` 的 **w95 段**里（逐条查过），实测口径是
 *     `uv --directory tools/api_check run main.py .out/fixtures/x86_spawn.exe w95`（无输出 = 通过）。
 *   - 不产生编译器辅助符号（D16）：不用 64 位整数、不做 > 1 KB 栈帧 ⇒ 不需要 __aullshr /
 *     __chkstk；构建仍照 D16 链 src\sys\flt.obj。
 *   - 只在 86 架构编译（MSVC 的 inline asm 在 x64 不可用）。
 *
 * 门禁怎么用这两类固定量（都是契约，改了就同时改 e2e_dbg.ps1）：
 *   ① **子进程的一次 `EXCEPTION_BREAKPOINT`**：子进程在写完自己的标记之后执行一条 `int 3`。
 *      门禁用 `dbg.create{ ..., on_breakpoint = f }` 收它 —— 那个回调带着**子进程的 pid**
 *      （父进程的 ntdll 初始断点也走同一通道，所以两边的 pid 都出现过）。门禁以显式
 *      `DBG_CONTINUE` 放行（裸 `int 3` 不是引擎登记的软件断点 ⇒ `ev.cont_code` 是
 *      NOT_HANDLED，用它目标会被第二次机会打死），`int 3` 的下一条指令接着跑。
 *      ❗为什么不用 `d:arm`：`BreakpointInfo` 的 key 只有 `(modname, rva)`、`orignal_byte`
 *      也只有一个（布局不可动）⇒ 同名模块在树里只有**第一个**被补 `0xCC`，子进程拿不到
 *      （断点记录的 pid 维度不属本切片，见 dbg_bindings.cpp 的文件头）。
 *   ② **标记变量** `g_tag`，初值 `0x53575030`（"0PWS" 小端 = 30 50 57 53）—— 门禁扫这 4 字节
 *      拿到它的 RVA，再按 `模块 base + rva` 在各进程的地址空间里读到**各自**的值：
 *      父 = 42（`TAG_PARENT`）、子 = 7（`TAG_CHILD`）。同一个 RVA、两个进程、两个值 ⇒ 这就是
 *      「按 pid 的句柄 + 按 pid 的 Var 绑定」最直接的证据（`ASLR` 只有 8 bit 熵，image base
 *      可能撞，所以判据不能靠「两个 base 不等」，见 e2e_dbg.ps1 的注释）。
 *   ③ 退出码：父 = 42、子 = 7；派生失败给 44 / 45（门禁看到它们就是失败，不是契约值）。
 *
 * ❗父子互等的写法（不能互锁）：父等子**有界**（最多 10 s），子等父**有界**（最多 10 s）——
 *   门禁 `d:kill()` 掉父进程之后，子进程的 `WaitForSingleObject` 立刻返回 ⇒ 子进程自己退出，
 *   不会在机器上留一个孤儿（即便留了，AutoDbg 进程退出时系统也会终止被调试树）。
 * ========================================================================== */

/* ---- kernel32 导出（零 CRT：自己声明；参数类型按 Win32 的原样，只用指针/整数） ---- */
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *hModule, char *lpFilename, unsigned long nSize);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
__declspec(dllimport) int __stdcall CreateProcessA(const char *lpApplicationName, char *lpCommandLine, void *lpProcessAttributes,
                                                  void *lpThreadAttributes, int bInheritHandles, unsigned long dwCreationFlags,
                                                  void *lpEnvironment, const char *lpCurrentDirectory, void *lpStartupInfo,
                                                  void *lpProcessInformation);
__declspec(dllimport) void *__stdcall OpenProcess(unsigned long dwDesiredAccess, int bInheritHandle, unsigned long dwProcessId);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *hHandle, unsigned long dwMilliseconds);
__declspec(dllimport) void __stdcall Sleep(unsigned long dwMilliseconds);
__declspec(dllimport) int __stdcall CloseHandle(void *hObject);

#define SYNCHRONIZE     0x00100000UL
#define WAIT_TIMEOUT_V  258UL      /* WAIT_TIMEOUT */
#define WAIT_ROUNDS     200        /* 200 × 50 ms = 10 s 上限（父子各自的有界等待） */

/* STARTUPINFOA 在 x86 上是 68 字节（cb=4 + 64），PROCESS_INFORMATION 是 16 字节。
 * 零 CRT 下不引入 SDK 头，所以按布局自己开一块（`cb` 必须**精确**等于 68，否则 CreateProcessA
 * 报 ERROR_INVALID_PARAMETER）。 */
typedef struct
{
    unsigned long cb;
    unsigned char rest[64];
} startup_info_a;

typedef struct
{
    void         *hProcess;
    void         *hThread;
    unsigned long dwProcessId;
    unsigned long dwThreadId;
} process_information;

#define TAG_INIT         0x53575030UL /* 门禁用它的 4 字节初值定位 g_tag 的 RVA（见文件头 ②） */
#define TAG_PARENT       42UL
#define TAG_CHILD        7UL
#define PARENT_EXIT_CODE 42
#define CHILD_EXIT_CODE  7
#define SPAWN_FAIL_EXIT  45

/* 门禁读的就是这个变量（同一 RVA、各自进程各一个值）。`volatile` 保证这次写在编译期不会被
 * 合并/推到后面 —— 子进程先写 tag 再抛断点，门禁在**命中那一刻**读到的是自己的值而不是初值。 */
static volatile unsigned long g_tag = TAG_INIT;

/* 关掉优化：保证「写 tag 在前、抛断点在后」的次序，且 `int 3` 不被优化掉。 */
#pragma optimize("", off)

/* 父与子都跑它：把自己的 tag 写进 `g_tag`（同一 RVA、各自的进程各一份）。 */
static void pass_tag(unsigned long tag)
{
    g_tag = tag;
}

/* 子进程专用：抛一次 `EXCEPTION_BREAKPOINT`，让门禁的 `on_breakpoint` 回调拿到**子进程的 pid**。
 * 门禁以 `DBG_CONTINUE` 放行 ⇒ 从 `int 3` 的下一条指令继续（同 x86_loop 的 int3 口径）。 */
static void raise_breakpoint(void)
{
    __asm {
        int 3
    }
}

#pragma optimize("", on)

/* ---- 极小的字符串/数字帮手（零 CRT 没有 strcmp / atoi / sprintf） ---- */

/* argv[1] 是不是 "/child" */
static int is_child_arg(int argc, char **argv)
{
    const char *s;
    const char *want = "/child";
    int         i;
    if (argc < 2 || argv[1] == 0) return 0;
    s = argv[1];
    for (i = 0; want[i]; i++)
    {
        if (s[i] != want[i]) return 0;
    }
    return s[i] == 0;
}

/* 十进制串 → unsigned long（只认 0..9，遇到别的就停） */
static unsigned long parse_ulong(const char *s)
{
    unsigned long v = 0;
    int           n = 0;
    if (s == 0) return 0;
    while (s[n] >= '0' && s[n] <= '9' && n < 10)
    {
        v = v * 10 + (unsigned long)(s[n] - '0');
        n++;
    }
    return v;
}

static char *append_str(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    return d;
}

static char *append_ulong(char *d, unsigned long v)
{
    char tmp[12];
    int  n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v)
    {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n) *d++ = tmp[--n];
    return d;
}

static void zero_mem(void *p, unsigned long n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

/* ---- 父：起自己当子进程，然后有界地等它 ---- */
static int spawn_child(void)
{
    char               path[512];
    char               cmd[600];
    char              *p;
    unsigned long      n;
    startup_info_a     si;
    process_information pi;
    unsigned long      i;

    n = GetModuleFileNameA(0, path, (unsigned long)sizeof path);
    if (!n || n >= (unsigned long)sizeof path - 8) return SPAWN_FAIL_EXIT;

    /* 命令行 = "<自己的完整路径>" /child <本进程 pid>（带引号：路径里可能有空格） */
    p    = cmd;
    *p++ = '"';
    p    = append_str(p, path);
    *p++ = '"';
    p    = append_str(p, " /child ");
    p    = append_ulong(p, GetCurrentProcessId());
    *p   = 0;

    zero_mem(&si, (unsigned long)sizeof si);
    zero_mem(&pi, (unsigned long)sizeof pi);
    si.cb = (unsigned long)sizeof si;   /* = 68，必须精确 */

    if (!CreateProcessA(path, cmd, 0, 0, 0, 0, 0, 0, &si, &pi)) return SPAWN_FAIL_EXIT;

    /* 有界等待（子进程也在等我们 ⇒ 两边都有上界，不会互锁；见文件头） */
    for (i = 0; i < WAIT_ROUNDS; i++)
    {
        if (WaitForSingleObject(pi.hProcess, 50) != WAIT_TIMEOUT_V) break;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return PARENT_EXIT_CODE;
}

/* ---- 子：有界地等父进程退出 ---- */
static int wait_parent(unsigned long parent_pid)
{
    void         *hp;
    unsigned long i;

    hp = parent_pid ? OpenProcess(SYNCHRONIZE, 0, parent_pid) : 0;
    if (hp)
    {
        for (i = 0; i < WAIT_ROUNDS; i++)
        {
            if (WaitForSingleObject(hp, 50) != WAIT_TIMEOUT_V) break;
        }
        CloseHandle(hp);
    }
    else
    {
        /* 拿不到父进程句柄（Win9x 不认 SYNCHRONIZE / 权限不足）：留一段时间给门禁读内存，然后退出 */
        Sleep(2000);
    }
    return CHILD_EXIT_CODE;
}

/* 签名与 src/sys/main.c 里的 main 声明一致（零 CRT 下由 mainCRTStartup 调用）。 */
int main(int argc, char **argv, char **envp)
{
    (void)envp;

    if (is_child_arg(argc, argv))
    {
        pass_tag(TAG_CHILD);
        raise_breakpoint();   /* 门禁的 on_breakpoint 回调在这里拿到子进程的 pid（见文件头 ①） */
        return wait_parent(argc >= 3 ? parse_ulong(argv[2]) : 0);
    }

    pass_tag(TAG_PARENT);
    return spawn_child();
}
