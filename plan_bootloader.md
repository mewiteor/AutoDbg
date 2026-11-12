# plan_bootloader.md —— 交付面、按 host 选 exe、落盘与 handoff（T8）

> 建议单独开分支（`feat/bootloader`）：它改的是产物矩阵与打包口径，不改引擎状态机。
> 决策仍归 `AGENTS.md` §13；坑归 `PITFALLS.md`；对外契约文档归 `tools/agent/autodbg-skill/`。
> 门禁基线（本轮实测）：import 12 产物全过 / selftest 67 / e2e_tcp 48 / e2e_json 32 / e2e_gui 21（26）/ nvim_smoke 32 / check_skill 14（39 片段）。
> 依赖：`plan_t7.md` —— handoff 之后真正跑脚本的是 child，child 的执行模型必须是逐事件的。

---

## 0. 已定的三条

1. **交付面必须 32 位**。x86 PE 从 Win95 到 Win11 都能跑（x64 上走 WOW64；x86 产物已按 D12 打 4.0 补丁），x64 PE 只能在 x64 上跑。所以「复制到别的系统跑不起来」只有把主 exe 定为 32 位才能消除。
2. **64 位 host 上恒用 x64 产物执行**。性能理由要诚实：真正的收益是原生 8 字节 `size_t` 与寄存器数量，不是 64 位整数运算本身——对 AutoDbg 的实际负载（读内存、字符串、LZMA、Ryu、帧编解码）收益很小。这条的价值是「按目标位宽选 exe」这条分支消失，且 `IsWow64Process` 只在入口判一次。
3. **落盘到 tmpdir**。进程的主模块必须由内核建 image section，而内核只接受**文件背衬**的 section（页文件背衬的 mapping 不能是 `SEC_IMAGE`）⇒ 落盘不可避免。RAM disk 能跑是因为它提供了「文件 + 文件系统」，介质无关；AutoDbg 不能造 RAM disk（需要内核驱动），只能利用已有文件系统。

---

## 1. 产物矩阵与命名

| 产物 | 角色 | `scan_dir.py` 的 auto 口径 |
| --- | --- | --- |
| `AutoDbg.exe`（x86） | 主 exe：launcher + x86 模块 + 内嵌 x64 blob | w95 —— `pick_target_os` 的名字规则要加 `AutoDbg.exe` |
| `AutoDbg_64.exe`（x64） | 与 blob 同一份字节；独立可跑（x64 host 上直接跑） | w7（machine 规则已覆盖） |
| 落盘的 `adbg64.bin`（tmpdir） | 主 exe 在 x64 host 上 exec 的那个 | ❗当前 `scan_one` 只扫 `.exe`/`.dll` ⇒ `.bin` 会被跳过，门禁要显式加扩展名或单独跑 |

- `pever`：x86 产物打 4.0（D12）；blob 是 x64，保持 6.0，不需要处理。
- `tools/pack`：payload entry 需要类型标志（`.dll` = 映射 / blob = 落盘 + CreateProcess）；D11 的结构边界校验按 blob 条目调整（解压长度校验不变）；blob 本身是 PE，导入审计按 x64 口径照旧跑。

---

## 2. 引导器的职责（约 150–250 行）

```text
1. GetCommandLineW 取第一个 token；用 GetModuleFileNameW 拿主 exe 目录
   （❗不要靠 basename：shell 里敲 `adbg` 时 argv[0] 没有目录部分）
2. IsWow64Process(GetCurrentProcess()) —— 只有 wxp 有 ⇒ dynapi；取不到 → 不 handoff，直接跑 x86 模块（95/98 本来没有 64 位）
3. tmpdir 目标文件：按 autodbg_build.h 的 build 串校验，一致 → 不写；不一致或被锁 → 明确报错（不清写）
4. CreateProcessW（bInheritHandles = TRUE）+ job（dynapi，JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE）
5. WaitForSingleObject + GetExitCodeProcess + ExitProcess(code)
```

- 引导器**不写 stdout**：`--json` 那一行由 child 写；引导器自己的诊断走 stderr（§12.6 已有口径）。
- 退出码口径不变（0 / -1 / 2 / Lua 错误码）；`-1` 走 `0xFFFFFFFF`。job 强杀时退出码不可控 ⇒ job 是「不留孤儿」的保险，不是退出码的载体。
- 嵌套 job（主 exe 已在 job 里）会 assign 失败 ⇒ 必须干净降级（继续 wait，不报错退 2）。
- ❗引号口径必须与 `autodbg_split_cmdline_a/w` 一致（child 不用 CRT 拆 argv）——这是最容易出 bug 的地方。
- `GetModuleFileName(NULL)` 在 child 里是 tmpdir 路径 ⇒ 脚本相对路径基准变（§11.5 第 5 条是**想要的**语义）⇒ 必须显式把原始 exe 目录传过去（建议 `--base-dir=<dir>` → `boot.base_dir`）。
- handoff 后 child 不得弹对话框（脚本路径已由主 exe 选定，D7 归主 exe）。

---

## 3. 数据集实测（`tools/api_check/datas.bin`，按 OS 段判定）

| 符号 | 段 | 结论 |
| --- | --- | --- |
| `CreateProcessW`、`GetCommandLineA/W`、`GetModuleFileNameA`、`PathFindFileNameA`、`GetTempPathA`、`CreateFileW`、`SetEndOfFile`、`WriteFile`、`ReadFile`、`SetFilePointer`、`WaitForSingleObject`、`GetExitCodeProcess`、`CloseHandle`、`ExitProcess`、`GetEnvironmentVariableA`、`SetEnvironmentVariableA`、`OpenProcess`、`LoadLibraryA`、`GetProcAddress`、`VirtualAlloc`、`CreateFileMappingA`、`MapViewOfFile`、`UnmapViewOfFile`、`OpenFileMappingA` | **全 w95** | 引导器不需要 dynapi（除 job） |
| `CreateJobObjectW`、`AssignProcessToJobObject`、`SetInformationJobObject`（JO009） | **只有 wxp** | 必须 dynapi；取不到 → 降级 |
| `IsWow64Process`、`GetNativeSystemInfo`、`Wow64GetThreadContext`、`Wow64SetThreadContext` | **只有 wxp** | dynapi；95/98 取不到 → 恒走 x86（本来没有 64 位）→ 干净降级 |
| `ReadConsoleW` | **全 w95** | REPL 的 stdin 不需要 dynapi（`plan_t7.md` §9 第 5 条可以关掉） |

---

## 4. 「恒用 x64」换来的真工作量：x64 产物必须处理 32 位节点

`parse_pe`（`src/core/pe.cpp:129-138`）现在按**编译期** `#ifdef _WIN64` 判位宽 → 混合节点被 `LOG` + `return`（只有 name/base/crc32，无节区）。四件：

1. `parse_pe` 按运行期 `Machine` 走两套 OptionalHeader（PE 头是数据，零新增导入）。
2. 32 位节点的寄存器视图：64 位进程里 `GetThreadContext` 是 64 位视图；要 32 位视图必须 `Wow64GetThreadContext`（只有 wxp）→ dynapi + 能力上报，取不到就报错，不能"跑但结果错"。
3. `stack_trace.cpp`：`find_runtime_function`（x64 `.pdata`）对 32 位节点不适用 → 走 EBP 链；64 位 dbghelp 走 WOW64 栈是已知易错组合 ⇒ 必须实测。
4. PEB/TEB 按位宽（`util.cpp:654/692` 的回退路径；9x 不认 `PROCESS_QUERY_INFORMATION` 的降级已存在）。

这条是**可观察行为变更**（今天 32 位节点在 x64 产物下根本没有输出）⇒ 发现型提交 + 门禁 + 黄金口径不能跨架构比较（D15）。

---

## 5. 需要拍板/修订的决策

| # | 议题 | 建议 |
| --- | --- | --- |
| D8 | exe 命名 | 主 exe `AutoDbg.exe`（x86）+ 落盘 `adbg64.bin`；`AutoDbg_64.exe` 保留（独立可跑） |
| D10 | flavor | flavor 是**脚本级**，不再由「映射到哪个模块」决定；变体在 x64 恒 modern |
| D11 | 载荷 | entry 加类型标志；仍无 magic / 版本号 |
| D12 | pever | x86 打 4.0；blob（x64）保持 6.0 |
| D26 | 不落盘 | 脚本仍然全程不落盘；**可执行文件必须落盘**（进程主模块必须是文件背衬的 image section）——作为显式例外记录 |
| D40 | LSP 注解 | 按 **host** 挂（不再按脚本 `arch=`）；混合树里 32 位节点按 x86 的 23 项理解 |
| 新 | job | dynapi + best-effort + 嵌套失败降级；不设任何 limit |
| 新 | 命令行 / 退出码 | 引导器透传命令行（引号口径 = `autodbg_split_cmdline_a/w`）+ 透传退出码 + 不写 stdout |
| 新 | 脚本相对路径基准 | 显式传原始 exe 目录（`--base-dir`）→ `boot` 加字段 |

❗**ABI 撞车**：T7 要 `repl_mode`（4 → 5），本方案要 `base_dir` ⇒ 必须一次 bump 加两个字段，或排好顺序。`e2e_tcp.ps1` 的 `client.version-abi` 与 `nvim_smoke.lua` 的 `version.abi` 会跟着改。

---

## 6. 门禁断言名

```text
handoff.cmdline-passthrough      含引号的 argv[0]、带空格/引号的脚本参数、中文路径
handoff.exit-code                0 / -1(0xFFFFFFFF) / 2 / Lua 3
handoff.no-stdout                引导器一行都不写；--json 那一行由 child 写
handoff.identical-no-rewrite     build 串一致 → 不清写；被锁 → 明确报错
handoff.job-kill-on-close        主 exe 被强杀 → child 不留
handoff.job-nested-fails-clean   已在 job 里 → 干净降级（不报错退 2）
handoff.no-dialog                handoff 后 child 不弹对话框
handoff.script-relative-base     --base-dir 生效（脚本相对路径解析到原始目录）
handoff.ro-media                 只读/无权限目录 → 明确报错（case 23 的口径从「只影响对话框」升级）
boot.no-wow64-branch             98 上：dynapi 取不到 → 不 exec（这是 9x 唯一能覆盖的 handoff 断言）
boot.is-x86                      主 exe 必须是 32 位 PE（便携性前提）
handoff.non-exe-name             CreateProcess 按 PE 头判断、不看扩展名（若用 `.bin` 必须先实测）
```

---

## 7. 待确认

1. `--version` / `--list` / `-h` 归谁：引导器只能报自己那部分（arch、build 串），`proto`/`abi` 是 child 的 ⇒ 要么 `--version` 也走 handoff（要为它落盘），要么把 `arch`/`abi` 重新定义为「执行脚本的那个进程」，引导器另加一个 `bootstrapper` 字段。
2. 同目录伙伴文件优先还是永远 tmpdir：已定 tmpdir；「同目录已有且 build 串一致就直接 exec」能省一次写与一次锁风险 ⇒ 要不要作为例外（建议不，保持路径单一）。
3. `adbg64.bin` 的扩展名（要实测）+ `scan_dir.py` 要不要加 `.bin`。
4. `--base-dir` 走 CLI 还是 env var（`Set/GetEnvironmentVariableA` 全 w95 有）；建议 CLI（进 usage、可断言）。
5. `--module-dir` 在 handoff 时的语义（要透传；child 的模块来自落盘文件）。
6. job 是否值得做（正常路径靠 wait 已保证；job 只覆盖「主 exe 被强杀」这一种）。
7. 落盘文件的复用口径：同一 build 串能否跨进程共享（两个主 exe 实例）→ 只读共享还是加锁。
8. nvim 的 local mode（D39）PATH 查找要加「host 位宽」档；quickfix / 临时文件口径不变。
9. `check_skill` 的 `references/cli.md` 要加一节「产物与位数 + 找不到 x64 半时的行为」（这是 CLI 口径下唯一对外契约文档）。
10. 空窗期口径：WOW64 四件完成前，64 位 host 上跑 x64 产物调试 32 位目标必须**明确失败**（不是跑但结果错）→ 一条断言 `wow64.target-unsupported`。

---

## 8. 顺序

```text
T7.1 状态机（pump/cont，行为不变）        ← handoff 之后回调模型的前提
引导器                                    ← 最便宜的一块，但改名要一次改完（make.bat + scan_dir + 4 个 e2e + nvim）
WOW64 四件                                ← 「恒用 x64」的真代价
多 session（进程树路由，plan_t7.md §10）   ← A/B 对比用例在这一步才成立
```
