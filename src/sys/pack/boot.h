#ifndef AUTODBG_SYS_PACK_BOOT_H
#define AUTODBG_SYS_PACK_BOOT_H
/* =============================================================================
 * src/sys/pack/boot.h —— 启动器 exe ↔ 模块 DLL 的 ABI 契约（Plan §2.3）
 *
 * 职责：定义两侧唯一共享的结构体 `autodbg_boot` 与 ABI 版本号。
 * 来源：新写（Plan P2）。原型 `tools/PackEXE` 没有这层（它是 exe 装 DLL、按导出名取函数）。
 * 与旧实现的差异：无旧实现。
 *
 * 约定：
 *   - **纯 C 头**（两侧都 include，DLL 侧是 C++ 但按 C 布局用）；字段顺序/类型一字不改。
 *   - 只加字段不改语义时 `abi` 也要加 1：旧启动器配新 DLL 会因 `size` 不符被拒，
 *     但我们要的是「明确拒绝」而不是「读到半个结构」。
 *   - 所有指针由启动器填、DLL 只读。
 * ========================================================================== */

#define AUTODBG_ABI_VERSION 2u

typedef struct autodbg_boot
{
    unsigned abi;              /* in : AUTODBG_ABI_VERSION（不符则拒绝启动） */
    unsigned size;             /* in : sizeof(autodbg_boot) */

    void *host_image;          /* in : 主 exe 的 HMODULE（GetModuleHandleA(NULL)） */
    const char    *cmdline_a;  /* in : GetCommandLineA()（两侧共用 split_command_line） */
    const wchar_t *cmdline_w;  /* in : GetCommandLineW()；Win9x 为 NULL */

    const char    *script_a;   /* in : 启动器选定的脚本路径（NULL = 用 argv[1]） */
    const wchar_t *script_w;   /* in : 同上，宽字符版 */

    unsigned flavor;           /* in : 0 = lua(5.5)，1 = luajit（诊断/日志用） */
    unsigned variant;          /* in : 0 = modern，1 = legacy（诊断/日志用） */
    /* ---- ABI 2（S0，plan_server.md D27）：server / 自测模式 ----
     * 追加而非改写，但 `size` 比较会把旧启动器拦下（这正是要的）。
     * ❗两个模式都**不需要脚本**：`script_a/w` 置 NULL。 */
    unsigned server_mode;        /* in : 1 = 启动 server（S1 起真正监听） */
    unsigned selftest;           /* in : 1 = 跑纯函数自测后退出（`--selftest`） */
    const char *server_addr;     /* in : "host:port"；NULL = 127.0.0.1:8642 */
    const char *server_token;    /* in : "auto" | "none" | <hex>；NULL = D21 缺省语义 */
    unsigned server_max_body;    /* in : 请求体上限字节；0 = 8388608 */
    unsigned server_queue;       /* in : 请求队列深度；0 = 8 */
    unsigned server_timeout_sec; /* in : 单请求执行超时秒；0 = 不限 */
    const char *server_log;      /* in : 诊断日志路径；NULL = 不打 */
    unsigned server_no_echo;     /* in : 1 = 执行输出不回显到控制台 */
} autodbg_boot;

/* DLL 必须导出的唯一入口（C 链接，__cdecl）：返回码即进程退出码 */
typedef int(__cdecl *autodbg_run_fn)(const autodbg_boot *);

#endif /* AUTODBG_SYS_PACK_BOOT_H */
