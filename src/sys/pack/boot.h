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
 *   - 版本号占用：1 = 初版，2 = S0（server / selftest，D27），3 = T1（`--json` / `--out`，D50），
 *     4 = T3（客户端 `--remote` / `--pid` / `--read-timeout` / `--max-frame`，D42），
 *     **5 = T7 切片 D1（`--repl`）**。
 *   - 所有指针由启动器填、DLL 只读。
 * ========================================================================== */

#define AUTODBG_ABI_VERSION 5u

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
    unsigned server_mode;        /* in : 0 = 不开；**1 = http（待 D41 删除）**；**2 = raw TCP 帧（D42 / T2）**
                                  * ❗不复用新字段：`--server=tcp` 只改这个值，ABI 不因此 +1 */
    unsigned selftest;           /* in : 1 = 跑纯函数自测后退出（`--selftest`） */
    const char *server_addr;     /* in : "host:port"；NULL = 127.0.0.1:8642 */
    const char *server_token;    /* in : "auto" | "none" | <hex>；NULL = D21 缺省语义 */
    unsigned server_max_body;    /* in : 请求体上限字节；0 = 8388608 */
    unsigned server_queue;       /* in : 请求队列深度；0 = 8 */
    unsigned server_timeout_sec; /* in : 单请求执行超时秒；0 = 不限 */
    const char *server_log;      /* in : 诊断日志路径；NULL = 不打 */
    unsigned server_no_echo;     /* in : 1 = 执行输出不回显到控制台 */
    /* ---- ABI 3（T1，plan_cli.md D50）：机器可读结果信封 ----
     * 仍是追加式（不改已有字段顺序）。`json_mode` = 1 时 stdout **只有一行 JSON**
     * （`puts("done")` 与 `print` 的直接输出都被让位），`--out=<path>` 时那一行改写文件、
     * stdout 留空。两个字段都允许只给其中一个：只给 `--out` 也算 JSON 模式（写文件）。 */
    unsigned json_mode;          /* in : 1 = 生成 D50 的 JSON 信封（`--json` 或 `--out`） */
    const char *json_out;        /* in : `--out=<path>` 的路径（CP936 字节；NULL = 只打 stdout） */
    /* ---- ABI 4（T3，plan_cli.md D42）：客户端（`--remote`）----
     * `remote_host` 非 NULL ⇔ 客户端模式：模块不去跑载荷里的本地脚本，而是连一台 server，
     *   在握手帧里报协议版本与 token，再把脚本 / 控制命令用帧发过去。
     * token 复用 `server_token` 字段（同一份语义：握手帧里那几个 hex 字符）。 */
    const char *remote_host;     /* in : `--remote=<host:port>` 的 host；NULL = 非客户端 */
    unsigned remote_port;        /* in : 对应端口（1..65535） */
    unsigned remote_cmd;         /* in : 0 = 跑脚本；1 = cancel；2 = kill；3 = detach；4 = health；5 = version */
    unsigned pid;                /* in : `--pid=<n>`（0 = 不指定；D48 解析链里的「显式」） */
    unsigned read_timeout_ms;    /* in : 单次文件操作 / 等回包的超时（0 = 缺省 5000；T4 用） */
    unsigned max_frame;          /* in : 客户端能接受的帧上限（0 = 缺省 8 MB） */
    /* ---- ABI 5（T7 切片 D1，plan_t7.md §11.7 的 D 行）：`--repl` ----
     * 1 = 模块侧跑 REPL 主循环（读 stdin → 求值 → 打结果），**不需要脚本**
     * （与 `selftest` / `server_mode` 同类：`script_a/w` 置 NULL）。
     * 与 `--json` / `--out` 互斥（那一行 JSON 必须独占 stdout），由启动器拒绝。 */
    int repl_mode;               /* in : 0 = 普通脚本；1 = REPL（`--repl`） */
} autodbg_boot;

/* DLL 必须导出的唯一入口（C 链接，__cdecl）：返回码即进程退出码 */
typedef int(__cdecl *autodbg_run_fn)(const autodbg_boot *);

#endif /* AUTODBG_SYS_PACK_BOOT_H */
