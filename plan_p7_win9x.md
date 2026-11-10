# plan_p7_win9x.md —— Win9x 实机测试（剩余工作）

> 状态：**harness 与最小往返已跑通**（W0 / W1 / W3 的一部分，见 §1.2）；§2 是**还没做**的部分。
> 口径：被测产物是 **`AutoDbg_86.exe`**（`98` 架构已退役，D1）——在 Win98 上由平台探测自动选 **legacy 变体**。
> 分工：脚本跑在 host（PowerShell 7），被测产物跑在 guest（Win98），全部走 `vmrun`；host 不直接读 guest 文件系统。
> 已踩的坑（vmrun / 快照 / command.com / 输出捕获）**不在本文重复**，见 `PITFALLS.md` 的 **W 节**与 **O 节**。
> 相关：`AGENTS.md` §4（门禁）、§11（打包与 ABI）、D12（子系统版本 4.0）、D15（逐字节比较口径）。

---

## 1. 环境与现状

### 1.1 环境前提（换机器要重配的都在 `config.local.ps1` 里）

| 项 | 要求 |
| --- | --- |
| VMware Workstation + `vmrun.exe` | 默认路径写在 `tools/w9x_test/w9x.ps1` 顶部；可用 `config.local.ps1` 覆盖 `$VMRUN` |
| guest | **中文 Win98 第二版**（CP936，编码测试才有意义），装了 VMware Tools（`runProgramInGuest` / `copyFileFromGuestToHost` 可用） |
| VMX 路径 | 同上，`config.local.ps1` 覆盖 `$VMX`（不入库） |
| 快照 | 只用一个既有快照（默认名 `1`，`config.local.ps1` 覆盖 `$SNAP`）；**不新建** |
| 凭据 | `config.local.ps1` 的 `$GU` / `$GP`，或环境变量 `W9X_GUEST_USER` / `W9X_GUEST_PASS`（**不入库**） |
| guest 工作目录 | 用 guest 里**已存在**的目录（默认 `C:\WINDOWS\TEMP`，`$GUEST_DIR` 可覆盖）：vmrun 不能 mkdir |

harness 入口：`pwsh tools/w9x_test/w9x.ps1 -Action check|run|record|revert|gate [-Test <name>] [-Flavor] [-Variant] [-NoPrepare] [-NoGate]`。
退出码：**0 = PASS / 1 = FAIL / 2 = SKIP**（VM / 凭据 / Tools 不可用时 SKIP，不算失败）。

### 1.2 已经验证过的事实（不必重复摸索）

- `runProgramInGuest` **会**传回真实程序的退出码；但 guest 里的调试会话不退出（引擎无输入事件时一直「继续等」）→ 探针只能靠超时结束，**判定以结果文件内容为准**。
- guest 里没有可用的命令执行通道（`command.com /c` 经 vmrun 不执行），所以没有重定向、没有 mkdir、没有删文件。
- 输出捕获：`Print*` / `Error*` 在非控制台下已走 `WriteFile`（`PITFALLS.md` O1），重定向能抓到；但 guest 里没有控制台，测试脚本仍**用 `io.open` 写结果文件**（每条写完 `flush`）。
- golden 已在 `tests/golden/w9x/`：`probe.txt`、`bom.txt`、`w3_cmdtest.txt`、`w3_dbghelp.txt`、`w3_detach_quick.txt`；比较走 D15 归一化。
- **LuaJIT 在 Win98 上不可用**：独立解释器报 `cannot create state: not enough memory`，`--flavor=luajit` 的 AutoDbg 则卡住不退出 → 相关 case 记 **xfail**，只记录不修。

---

## 2. 剩余工作

### 2.1 加载与变体（脚本侧可自动化）

| # | case | 期望 | 状态 |
| --- | --- | --- | --- |
| 01 | 平台探测 → legacy | 98 上映射的是 `AutoDbgLegacy_lua_86.dll`（`--list` 或诊断行确认） | 待做 |
| 02 | legacy + lua | 跑通，结果与 golden 一致 | 待做 |
| 03 | legacy + luajit | **xfail**（§1.2）：记录实测，不进硬门禁 | 待做（记录型） |
| 04 | `--variant=modern`（显式） | 干净失败 + 回退到 legacy；stderr 可捕获 | 待做 |
| 05 | `--list` | 4 个 entry；当前生效变体 = legacy | 待做 |
| 06 | `--module-dir` | 从 guest 磁盘读 DLL 仍走内存映射 | 待做 |
| 07 | 无参（文件对话框） | **不做自动化**，只人工确认 9x 上 A 版降级不崩 | 人工 |

### 2.2 dynapi 降级点：剩余点走「静态断言 + 人工确认」

自动化部分 = 每轮开头的静态门禁（`-Action gate` → `tools/api_check/scan_dir.py`）：它保证 x86 产物导入表里**不会**出现这些符号，说明这些调用只能走 dynapi、不会在映射时硬失败。
「降级路径真的走到」只能人工在 98 上确认一次，结果回填下表；**填不上就保持「待确认」，不能因为门禁绿就当作通过**。

| # | 符号 | 位置 | 98 上要确认什么 | 结果 |
| --- | --- | --- | --- | --- |
| 10 | `IsProcessorFeaturePresent` | `main.cpp`（`__isa_available_init`） | 不崩，走 x87（`flt.asm`） | 待确认 |
| 11 | `GetAncestor` | `core/util.cpp` `LjGetRootWindow` | 窗口选取功能走 `GetParent` 循环 | 待确认 |
| 12 | `FlashWindowEx` | `core/util.cpp` `LjFlashWindow` | 窗口只闪一次（`FlashWindow`） | 待确认 |
| 13 | `SymFunctionTableAccess` / `UnDecorateSymbolName` | `core/stack_trace.cpp` | ✅ **已实测**：`frames=0` 的退化路径生效（golden `w3_dbghelp.txt`） | ✅ |
| 14 | `GetModuleHandleExA` | `src/sys/include/lj.h` 垫片 | 98 上 LuaJIT 不可用 → 只能代码审阅 + 在能跑 LuaJIT 的机器上确认垫片生效 | 待确认 |
| 15 | `strtoull` | `src/sys/strtoull.c` | `tonumber(s, base)` 用**非常量**输入跑一次 | 待确认 |
| 16 | `__DestructExceptionObject` | `src/sys/destruct_eh.c` | 转发失败时降级「不析构」、不崩 | 待确认 |
| 17 | `__chkstk` | legacy 链 `ntdll.lib` | 构造 >4 KB 栈帧的探针，看是否崩（已知风险项） | 待确认 |

### 2.3 编码与文件系统

| # | case | 期望 | 状态 |
| --- | --- | --- | --- |
| 20 | GBK 脚本（CP936 执行字符集） | guest 里抓不到 stdout → **人工确认** | 人工 |
| 21 | UTF-8 BOM 脚本 | ✅ 已自动化（golden `bom.txt`）：98 上 Lua 5.5 直接 `exit=3` | 完成 |
| 22 | 中文路径 + 长路径 | 能打开、能跑；`GetModuleFileName(NULL)` 仍是主 exe（`AGENTS.md` §11.5） | 待做 |
| 23 | 只读 / 不可写目录 | 模块**不再写脚本目录**（编辑器提示已移到 nvim 插件），只读目录下应能正常跑完 | 待做 |

### 2.4 调试 API 在 9x 上的行为差异（记录型，允许 xfail）

| # | case | 期望 | 状态 |
| --- | --- | --- | --- |
| 30 | attach 到**已运行**进程 | Win9x Debug API 不支持 attach → 干净报错，不崩 | 待做 |
| 31 | `GetThreadContext` / `SetThreadContext` | 32 位 CONTEXT 正常；记录实际行为 | 待做 |
| 32 | 硬件断点（DRx） | 9x 上可能不生效 → 记 xfail | 待做 |
| 33 | 栈回溯 | 走 2.2 第 13 条的降级路径，输出修饰名 | 部分（golden 有基线） |
| 34 | 被调试目标 | 需要 x86 fixture（架构必须与产物一致）；本仓库暂无 fixture 目录 | 待做（阻塞项：缺 fixture） |

### 2.5 观察项（不设硬门禁，只记录）

- 启动解压 2.3 MB 在 98 VM 上的实际耗时（host 上 19.5 ms，98 上大概率 100–300 ms）。
- 峰值内存 ≈ 2.9 MB 在 98 上的表现（`VirtualAlloc` 粒度与提交页行为不同）。
- 映射器逐节 `VirtualProtect` 在 98 上是否全部成功。
- `C:\WINDOWS\TEMP` 之外再试一个中文路径目录（与 case 22 合并做）。

### 2.6 harness 自身的待补

- `w9x.ps1` 顶部的 `$VMRUN` / `$VMX` / `$SNAP` / `$GUEST_DIR` 是**机器相关默认值**：换机器要在 `config.local.ps1` 里覆盖（目前靠 dot-source 顺序生效，建议改成「显式从 config 读，缺省才用内置值」并打印实际生效值）。
- `-Action check` 的输出现已覆盖 vmrun/vmx/Tools/凭据/快照；建议再加一行「实际生效的 guest 工作目录」。
- `--quick` 只跑 01–04 + 10–16 的瘦模式尚未实现。

---

## 3. 待确认（开工前要定）

1. 快照策略：继续复用既有快照 `1`（关机态 → 每轮要 `start` + 等 Tools），还是建一个专门的「clean-w9x」快照？
2. case 03（LuaJIT）的期望值：保持 xfail 只记录，还是先花时间定位「not enough memory」（涉及本机 98 的内存与 LuaJIT 的 `mmap` 口径）？
3. case 34 的 fixture 从哪来：本仓库没有 `tests/fixtures/bin`，要么新建，要么由 `tools/server_test/gui_target.ps1` 那类脚本现场生成。
4. 这些 case 要不要进 `tools/w9x_test` 的默认 `-Action run`（会显著拉长单轮时间），还是继续按 `-Test <name>` 逐条跑。

## 4. 明确不做

- Win95 RTM/gold（只有 `crtdll.dll`）。
- x64（98 无 x64）。
- modern 变体在 9x 上的功能（设计上不可用，只测「干净失败 + 回退」）。
- GUI 自动化（文件对话框、`--quiet` 的弹窗）——只人工确认。
- 往 guest 里装任何运行环境（Python / 额外 DLL 都不装）。
- 行为类缺陷的修复（如调试会话不退出）：**只记录不修**（`AGENTS.md` §4）。
