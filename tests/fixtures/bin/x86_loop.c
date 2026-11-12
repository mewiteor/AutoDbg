/* =============================================================================
 * tests/fixtures/bin/x86_loop.c —— 被调试目标的 x86（32 位）fixture（T7.2）
 *
 * 文件职责：门禁用的固定靶子 —— 在一个可被脚本发现的固定位置命中断点，跑固定轮数后
 *   干净退出。门禁只看两件事：命中次数与退出码（不打印、不落盘、不做文件 I/O）。
 * 来源：新写（plan_t7.md §6 与 §9 第 4 条；plan.md §1.3 case 34 的共同阻塞项）。
 * 与旧实现的差异：无旧实现 —— 之前没有「可被脚本发现的被调试目标」，门禁只能靠空转脚本
 *   （e2e_tcp.ps1 的引擎空转断言）。
 *
 * 口径（硬）：
 *   - 零 CRT：不 include 任何头（连影子头都不需要）；入口 mainCRTStartup 由
 *     src/sys/main.c 提供，链接走 ld 规则（/nodefaultlib）。
 *   - 导入表必须过 w95（scan_dir.py 对 .out/fixtures/ 自动选 w95）⇒ 禁止任何 XP+ 才有的
 *     调用。实际导入共三条，全部由 src/sys/main.c（零 CRT 的 CRT 兼容层）引入，不由本文件产生：
 *     msvcrt!_ftol、msvcrt!__getmainargs、kernel32!ExitProcess（三条都在 datas.bin 的 w95 段里，
 *     前两条也由 src/sys/msvcrt.def 显式导出）。实测口径用
 *     uv --directory tools/api_check run main.py .out/fixtures/x86_loop.exe w95（无输出 = 通过）。
 *   - 不产生编译器辅助符号（D16）：不用 64 位整数、不做 >1 KB 栈帧 ⇒ 不需要 __aullshr /
 *     __chkstk；构建仍照 D16 链 src\sys\flt.obj。
 *   - 只在 86 架构编译（MSVC 的 inline asm 在 x64 不可用）。
 *
 * 断点位置怎么找（给 T7.3 的 pump.* 断言用）：
 *   循环体里执行这一条固定指令序列 —— `mov eax, 0x50474244`（立即数按小端存成 44 42 47 50）紧跟
 *   `int 3`，在内存里就是 6 个字节
 *   6 个字节 `B8 44 42 47 50 CC`。脚本在目标模块的代码区扫这 6 字节，断点地址 = 模式命中
 *   地址 + 5。
 *   ❗不变量（T7.3 会断言）：这 6 字节在整个 exe 里必须只出现一次 ⇒ 匹配整条指令编码
 *   （B8 起头），不是只匹配立即数；只匹配立即数会撞上 rodata 里的同名数据。
 *   为什么不用固定 RVA：/merge:.rdata=.text 与链接器布局不保证 RVA 稳定，同一份源码重链
 *   一次就可能变。
 *   为什么哨兵不能写成可打印串放在执行流里：'D'（0x44）在 32 位下是 `inc esp`，会把栈指针
 *   改坏；这里的模式全是无害指令（mov imm32）+ int 3。
 *   实现约束：MSVC 可能把 static 函数内联进循环从而让模式出现多次，所以 bp_site 关闭优化
 *   （#pragma optimize("", off)）保证「只出现一次」这条不变量成立 —— 门禁前用字节扫描核对。
 *
 * 契约数字（T7.3 的 pump.record-and-continue 直接依赖，不是随手值）：
 *   LOOP_ROUNDS = 8（⇒ 命中 8 次）、退出码 = 42。退出码口径与 AutoDbg 自己的
 *   0 / -1 / 2 与 Lua 的 2 / 3 不冲突。
 * ========================================================================== */

#define LOOP_ROUNDS       8
#define FIXTURE_EXIT_CODE 42

/* 关掉优化：防止内联把模式复制进循环体（上面的「只出现一次」不变量靠这条保证）。 */
#pragma optimize("", off)

/* bp_site() 里的 int 3 就是断点目标；那条模式只在这里出现一次。 */
static void bp_site(void)
{
    __asm {
        mov eax, 0x50474244      /* 立即数按小端存成 44 42 47 50 = DBGP */
        int 3                    /* CC */
    }
}

#pragma optimize("", on)

/* 签名与 src/sys/main.c 里的 main 声明一致（零 CRT 下由 mainCRTStartup 调用）。 */
int main(int argc, char **argv, char **envp)
{
    int round = 0;

    while (round < LOOP_ROUNDS)
    {
        bp_site();
        round++;
    }

    return FIXTURE_EXIT_CODE;
}