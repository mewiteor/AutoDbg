#ifndef AUTODBG_SYS_PACK_HANDOFF_HPP
#define AUTODBG_SYS_PACK_HANDOFF_HPP
/* =============================================================================
 * src/sys/pack/handoff.hpp —— handoff（把执行交给落盘的 exec 条目）的执行面（T8 切片 a2 / D63）
 *
 * 职责：把已定的口径落地成一次进程边界：tmpdir 落盘（逐字节校验）→ 设 env `AUTODBG_BASE_DIR`
 *       → CreateProcess（引号口径 = `autodbg_split_cmdline_a/w`）→ job（best-effort）
 *       → wait → 把 child 的退出码原样交回去。
 * 来源：新写（plan_bootloader.md §2 / §5.5 的 (a)）。判定本身在 `src/common/handoff_decision.*`
 *       （纯函数，T8 切片 a1）；本文件是它的 I/O 半边，`launcher.cpp` 仍只解析选项。
 * 与旧实现的差异：无（新文件）。handoff 是**进程边界**、不是 boot 契约 ⇒ 不占 boot 字段
 *       ⇒ ABI 保持 5（§11.3）；`base_dir` 走 env var（plan §5 的「新」条目），不走 `autodbg_boot`。
 *
 * 已定口径（逐条对应 plan_bootloader.md §2）：
 *   ① 落盘路径 = tmpdir + manifest 里 exec 条目的名字（`adbg64.bin`）；校验 = **逐字节比较**
 *      （已有内容 == blob 字节 → 不写）；不一致才写；被锁 / 只读 / 无权限 → 明确报错且**不清写**
 *      （不静默回退到内存映射）；同一 build 串跨进程只读共享（不加锁、不设 marker 文件）。
 *   ② 引导器**不写 stdout**（`--json` 那一行由 child 写）；自己的诊断走 stderr 且纯 ASCII（AGENTS §3）。
 *   ③ 退出码口径不变（0 / -1 / 2 / Lua 错误码；`-1` 走 `0xFFFFFFFF`）——job 是「不留孤儿」的保险，
 *      不是退出码的载体；job 强杀时退出码不可控。
 *   ④ job 三条（`CreateJobObjectW` / `SetInformationJobObject` / `AssignProcessToJobObject`）只有 wxp
 *      有 ⇒ 一律 dynapi；取不到或嵌套 assign 失败 → 干净降级（继续 wait，不报错、不退 2）。
 *   ⑤ `IsWow64Process` 只有 wxp 有 ⇒ dynapi；取不到算「不是 64」⇒ 不 handoff（95/98 本来没有 64 位）。
 *   ⑥ 错误出口只有一个包装（PITFALLS O9）：stderr 一行 ASCII，且**与 launcher 口径一致**——
 *      `--quiet` 只写 stderr，否则同一个文本也弹窗（`MessageBoxA` 已由 launcher 在同一份产物里引入）。
 *      模态框在无头会话里不可断言，所以 `e2e_boot.ps1` 全程带 `--quiet`，断的是 stderr 那一半。
 *
 * 引号口径（最容易出错的地方）：child 不用 CRT 拆 argv，它用 `autodbg_split_cmdline_a/w` 重新拆
 *   `GetCommandLineA/W`。所以拼 child 的命令行时：argv[0] 换成 child 自己那份落盘路径（按同一套
 *   引号规则重新编码），**其余部分逐字节原样透传** —— 原样透传保证「child 拆出来的 token 与主 exe
 *   拆出来的逐字节相同」；只有 argv[0] 被有意替换（它必须指向真正执行的那个进程）。
 * ========================================================================== */

namespace autodbg::sys::pack
{
/* run_handoff 的输入：全部由调用方（launcher）解析 + 探测后传进来，handoff 不解析命令行、
 * 不弹对话框（D7 归主 exe）。字符串一律由调用方保证在调用期间有效。 */
struct handoff_request
{
    const char          *blob_name;      /* manifest 里 exec 条目的名字（小写、无路径，如 adbg64.bin）；
                                             调用方已把它**拷成 NUL 结尾的 C 串**（manifest 里的名字不带 NUL） */
    const unsigned char *blob_data;      /* 解压后的 blob 字节（= child 那份产物的全部字节） */
    unsigned             blob_size;
    const char          *cmdline_a;      /* 原始命令行（ANSI）；W 取不到时的口径（9x） */
    const wchar_t       *cmdline_w;      /* 原始命令行（W）：非 ASCII 参数只有靠它才不被代码页截断 */
    const char          *append_script;  /* 主 exe 用对话框选定的脚本 → 追加进 child 的 argv[1]；NULL = 命令行里已有 / 不需要 */
    bool                 quiet;          /* `--quiet`：只写 stderr，不弹窗（口径与 launcher 一致，已定） */
};

/* host 是不是 64 位：`IsWow64Process`（只有 wxp 有 → dynapi）；取不到算「不是 64」 */
int probe_host_is_64();

/* 「执行脚本的那个进程」的位数（D63 / plan §2 第 6 条）：主 exe 是 x86，但 64 位 host 上真正执行的是
 * child（x64）⇒ `--version` 的 arch 必须按 host 算，否则 `version.parity.main-vs-exec` 会分叉。 */
const char *host_arch_name();

/* 执行 handoff，返回 child 的退出码（口径不变）。引导器自己的失败（落盘 / CreateProcess）返回 2，
 * 诊断已经写进 stderr；失败路径不会静默回退到内存映射。 */
int run_handoff(const handoff_request &req);
}

#endif /* AUTODBG_SYS_PACK_HANDOFF_HPP */
