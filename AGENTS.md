# AGENTS.md

本文件是 AutoDbg 项目对 AI 编码代理的工作约定。

## 0. 项目是什么

**AutoDbg** —— 无头、可脚本化的 Windows 调试引擎（Win32 Debug API），为自动化逆向工程与 AI 驱动分析提供底层调试原语。
架构是「纯 C/C++ 核心 + 脚本中间层」：核心不感知任何脚本，Lua 绑定层是唯一 `lua_State` 使用者；交付物是单一 `.exe`，运行时从内嵌载荷里取出对应模块（见 §11）。

硬性目标：**零 CRT 运行时**、**兼容 Windows 95/98（x86 legacy 变体）**、**同时支持 Lua 5.5 与 LuaJIT**。

仓库里的第三方子模块：`thirdpart/lua`（Lua 5.5.1）、`thirdpart/luaJIT`、`nvim`（neovim 插件，独立仓库 `AutoDbg.nvim`）。

### 0.1 文档分工

| 文件 | 内容 |
| --- | --- |
| `AGENTS.md`（本文） | 工作约定：构建 / 门禁 / 约束 / 风格 / 架构 / 打包 ABI / 已定决策 |
| `PITFALLS.md` | **已踩过的坑**（现象 → 根因 → 现在的做法），改代码前先扫对应小节 |
| `plan.md` | **剩余工作**（Win9x 矩阵、server/GUI 实测、CLI/帧协议尾巴）——原 `plan_p7_win9x.md`、`plan_server.md`、`plan_cli.md` 已合并到这里 |
| `plan_t7.md` | **T7（低级引擎 API + REPL）的独立计划**——在分支 `feat/t7-low-level-api` 上推进（`plan.md` §1.1 第 6 条是它的出处） |
| `plan_bootloader.md` | **T8（交付面 / 按 host 选 exe / 落盘 handoff）的独立计划**——建议单独分支 `feat/bootloader`；含多 session 的进程树路由（`plan_t7.md` §10） |
| `nvim/README.md` | neovim 插件的安装、配置与命令 |

## 1. 构建

```bat
make.bat                 :: 默认：64 + 86 两个架构，release
make.bat d               :: debug 模式
make.bat a 64            :: 只构建一个架构（同时生成 compile_commands.json）
make.bat a 86            :: 同上；`a 98` 已退役，会直接报错（Win9x 口径改用 legacy 变体，见 §11）
make.bat r               :: 删除 .build 后重新生成 build.ninja 并构建
make.bat c               :: 只清空 .build
make.bat b <dir>         :: 指定 build 目录（默认 .build）
make.bat i <dir>         :: 指定输出目录（默认 .out）
```

前置要求：`ninja` 在 PATH；MSVC（`make.bat` 通过固定路径 `vswhere.exe` 调 `vcvarsall.bat`）；MASM（`ml.exe` / `ml64.exe`）。

❗**运行 `make.bat` 前保证控制台代码页是 936**（`cmd /c "chcp 936 && make.bat ..."`）：`build.ninja` 里的 `msvc_deps_prefix` 是中文前缀，在 UTF-8 代码页下生成会静默失效，头文件依赖就再也不被记录。详见 `PITFALLS.md` B2。

产物（每个架构一份）：

- `.out/AutoDbg_<arch>.exe` —— **唯一交付物**：零 CRT 启动器 + LZMA 单流载荷，内含该架构的全部模块（86 → 4 个，64 → 2 个；见 §11）。
- `.out/lua_<arch>.exe` / `.out/luajit_<arch>.exe` —— 零 CRT 的独立解释器宿主（**不交付**：`gendata` 自举要用它们跑工具脚本）。
- `.out/AutoDbg_<flavor>_<arch>.dll` / `.out/AutoDbgLegacy_<flavor>_<arch>.dll` —— 中间产物（模块本体），只被打包与 `--module-dir` 调试用，**不交付**。
- `<build>/gen/autodbg_build.h` —— **构建期生成的版本宏**（`AUTODBG_VERSION` / `_SOURCE` / `_DIRTY`，见 `src/sys/version.h`）：
  `make.bat` 的 `:genversion` 每次构建都算一遍 `git describe --tags --always --dirty`（有 tag 时是 `v0.1.0[-N-g<hash>]`，
  没 tag 时就是 commit 短哈希，工作区脏会带 `-dirty`），**内容没变就不碰文件**（否则每次都全量重编）。
  不做这一步的构建会降级成 `"unknown"`，不会编译失败。

各架构的差异（见 `make.bat`）：

| arch | 关键点 |
| --- | --- |
| `64` | x64 + `ml64`；链接 `ntdll.lib` + `ucrtbase.lib`；modern 口径（`/D_UNICODE /DUNICODE /utf-8`）；**不打 pever 补丁**（OSVer/SubsysVer 保持 6.0，D12）；只有 modern 变体 |
| `86` | x86 + `ml`（`src/sys/flt.asm`）；`/D_USE_32BIT_TIME_T /D_CRTIMP_TYPEINFO`；modern 口径 + **legacy 变体**（`CFLAGS_V9X`：`/source-charset:utf-8 /execution-charset:gbk`、ANSI、`/DNO_SCYLLA_HIDE`）；交付面覆盖 Win95/98 → Win11，所以所有 x86 产物要打子系统版本 4.0 补丁（§11） |

链接一律 `/nodefaultlib`（零 CRT）。

- **变体与 flavor**：`:build` 末尾依次 `call :buildautodbg lua`、`luajit`（modern），86 再加 `lua legacy`、`luajit legacy`；最后 `call :buildlauncher <arch>` 把模块打成那一个 exe。变体差异**只在编译开关**，源码 100% 共用。
- **LuaJIT 接线**：`luajit` 分支带 `/FIlj.h /DAUTODBG_WIN9X /DJIT`，**x86 额外 `/DLUAJIT_NO_UNWIND`**（`%LJ_UNWIND%`）。❗**x64 必须不定义它**（D19，`PITFALLS.md` M5）。LuaJIT 需要三段代码生成，构建期工具 `minilua.exe` / `buildvm.exe` 落在 `<build>/luajit/host/`，**不进** `.out`。
- ❗`/FIlj.h /DAUTODBG_WIN9X` 必须同时加在 `LJ_CFLAGS`（`luajit.lib` 的编译单元）上：`GetModuleHandleExA` 垫片的调用点（`lj_clib.c` / `lib_package.c`）在静态库里，只在 `:buildautodbg` 里加对 `luajit.lib` 无效。
- `src/ScyllaHide/**` 只被 **modern 变体**链接（legacy 靠 `/DNO_SCYLLA_HIDE` 走空实现；`debug_loop.cpp` 的调用点一个字不改）。
- **`/showIncludes`**：`cc` / `ccx` / `cxx` / `ccv9x` / `cxxv9x` 的 rule 都带它，`deps = msvc` 才真正生效（两者缺一，改头文件**不会**触发重编，只能全量重建）。
- 修改或扩展 `make.bat` 必须遵守 **§10 的编写规范**。

## 2. 代码生成与自举

- `tools/gen_lua_lib.lua`：把 `lualib/*.lua` 编译成字节码，生成 `datas_<flavor>/datas_<flavor>.c` + `datas_<flavor>/datas.h`（`all_bytecodes[]` 表）。
  运行期 `state.cpp` 逐条 `luaL_loadbuffer` + `lua_pcall`，结果按**文件名**设为全局（当前为 `inspect`、`jsonx`、`moses`、`dbglib`；`effects`、`mcp` 已随 T5/D44 删除）——脚本里直接用全局名，不是 `require`。
  ❗字节码格式取决于跑脚本的解释器：`lua` 变体必须用本项目的 Lua 5.5 解释器（`lua_<arch>.exe`），`luajit` 变体必须用 `luajit_<arch>.exe`。
  `datas` 是 **flavor 级共享产物**：只有 modern 变体负责生成，legacy 复用（否则 ninja 报 `multiple rules generate`）。
  ❗按**文件名**顺序烘焙意味着「后加载的库不能用先加载的全局」，只能在调用时查（`PITFALLS.md` L4）。
- `tools/gen_hook_data.lua`：把 `HookLibrary.dll` 的字节烘焙成 `HookLibraryData.c`（只有 modern 变体需要：legacy 走 `/DNO_SCYLLA_HIDE`）。
- 两者都依赖「先构建出 `.out/lua_<arch>.exe`」（或 `luajit_<arch>.exe`），即自举依赖（ninja 里用隐式依赖 `|` 表达，且必须同时包含**解释器产物**与**生成脚本本身**）。

## 3. 运行

```text
.out\AutoDbg_<arch>.exe [选项] [<script.lua>] [脚本参数...]
```

- 无脚本参数时弹文件对话框（启动器里的 `GetOpenFileNameW`，9x 降级 A 版）选脚本。
- 选项（启动器层）：`-h` / `--help`（打 usage 后退出 0）、`--version`（版本/ABI/架构后退出 0；版本串来自构建期的 `git describe`，见 §1）、`--flavor=lua|luajit`（默认 lua 5.5）、`--variant=modern|legacy`（仅排障；正常靠平台探测 + 映射失败回退）、`--list`（列出载荷里的模块）、`--module-dir=<dir>`（调试：从磁盘取同名 DLL，仍走内存映射）、`--quiet`（关错误弹窗）、`--selftest`（跑纯函数自测）、`--repl`（REPL：stdin 喂**合法 Lua 语句**，不需要脚本；与 `--json`/`--out`/`--remote` 互斥 → 启动器退 2；**与 `--server` 可并存**：同一连接内 `lua_State` 跨命令存活，每条 `script` 帧 = 一行/一个完整块）、`--json` / `--out=<path>`（机器可读结果信封，§12.6；两者互斥于 `--server`，同时给 → 启动器报错、退出码 2）、`--server[=tcp]`（raw TCP 帧；`--server=http` 已随 D41 删除并报错）及其 `--server-*` 子选项（§12.1）、**客户端（T3/D42）**：`--remote=<host:port>`、`--remote-token=<hex>`、`--pid=<n>`、`--read-timeout=<ms>`、`--max-frame=<bytes>`、`--cancel` / `--kill` / `--detach` / `--health`（这四条离开 `--remote` → 启动器报错、退出码 2；带 `--remote` 时 `--version` 变成「查 server 版本」）。
  启动器的输出一律 **ASCII**（它走 `WriteFile` 写字节，中文在 CP936 控制台上会乱码；模块那边走 `WriteConsoleW` 才例外）。
- **脚本首行标记**也能选 flavor：`--!lua` / `--!luajit`（优先级：命令行 > 首行标记 > 字节码 magic > 默认 lua）。
  其它 `--!xxx` 一律报错退出（D9）；例外：`--!autodbg` / `--!adbg` 是**编辑器侧标记**
  （远程地址 / exe / 架构），launcher 会**跳过它继续看下一行**（所以同一份脚本既能被 nvim 插件跑，
  也能直接命令行跑；`PITFALLS.md` N9）。
- `state.cpp` **不再**往脚本目录写任何文件（`cdebugger.d.lua` / `.luarc.json` 已移除）：编辑器补全改由 neovim 插件的 LuaLS 注解库提供（§12.5）。
- 脚本参数从 `argv[2..]` 压入 Lua；退出码：`0` = 成功；**脚本不存在/无法打开** → `-1`；脚本的**语法错误**是 Lua 的 `LUA_ERRSYNTAX`（Lua 5.5 = **3**）、**运行时错误**是 `LUA_ERRRUN`（= **2**）——两者**透传 Lua 错误码**（启动器与引擎不另外发明）；启动器自己的错误为 `2`（另有 `--list` / `--selftest` / `/shutdown` 等正常返回 0）。

## 4. 测试与门禁

本仓库自带**十条**可复跑的验证链（都不需要额外的机器）：

| 链 | 入口 | 说明 |
| --- | --- | --- |
| 导入表门禁 | `uv --directory tools/api_check run scan_dir.py` | 扫 `.out` 下全部产物，按产物自动选口径（`AutoDbgLegacy_*` 与启动器 → `w95`、`.out/fixtures/` 下的 fixture → `w95`（fixture 里禁止 NT-only 调用）、其余 x86 → `wxp`、x64 → `w7`），有目标系统不可用的导入就打印并返回 1。单文件用法：`uv --directory tools/api_check run main.py <exe\|dll> <w95\|w98\|w982\|wxp>`（x64 免第二个参数）。当前实测 **14 产物**（12 个产物 + `.out/fixtures/` 的 `x86_loop`、`x86_spawn`，两个 fixture 都走 w95） |
| 纯函数自测 | `.out\AutoDbg_<arch>.exe --selftest` | 覆盖 `json_writer` / `png_writer` / `dbl2str` / **帧编解码** / **`decide_console_action`**（T7.1）等纯计算层（基线 70 → T1/D50 转义 +11 = 81 → T2 帧编解码 +20 = 101 → T5 删 `http`/`base64` 两段后 = 67 → **T7.1 补 `decide_console_action` 三条 = 70** → **T7.3 切片 B 补 `console_style_enabled` 三条 = 73**） |
| GUI 自动化（本地） | `pwsh tools/server_test/e2e_gui.ps1 -Render <64\|86> [-Variant legacy]` | 起一个 `gui_target.ps1` 靶子窗口，用**本地 CLI** 跑 `gui.*` 脚本断言（屏幕/窗口/控件/截图/`gui.pick`）；`-AllowInput` 才跑会动真鼠标/键盘的用例。T5 之前它走 HTTP `/run`，现已改成本地（`gui.*` 与传输无关，21 条断言；带 `-AllowInput` 是 26 条） |
| raw TCP 帧（T2/T3/T4/D42/D2a） | `pwsh tools/server_test/e2e_tcp.ps1 -Render <64\|86> [-Variant legacy]` | `--server=tcp`：握手帧（`id` = 协议版本、`data` = token）→ `info`；版本不符 / 首帧非握手 / 错 token 的 `error` 帧；`script` → `out`+`result`+`end`；`control` 的 health / version / cancel / 未知 / shutdown；**执行期的 control**（Lua 死循环 + 引擎空转）；**`dbg` 状态机**（T7.3：`pump.*` 八条 —— 停住不放行 / 默认 code 对齐 / step / 重入拒绝 / record-and-continue + dropped / 空转期 cancel / attach + close 只分离 / attach 参数错）；**`--remote` 客户端**（跑脚本 / `--json` 信封 / 语法错误 / health / version / 另一个进程的 `--cancel`）；**`open_file` 读向**（远程 open 应答、read/tell/seek/lines 字节与位置语义、等回包超时 → `nil,"timeout"` 且脚本继续）、**调试回调里的阻塞读**（超时后回调返回、下一次 `Run` 照常；迟到的 `read_resp` 被丢弃）；**`0.0.0.0` 横幅列出网卡地址**；**协议 2 的归因**（`out.id` = 命令号、`event` 帧带命中记录且 `id` = 命令号、`result.id` 仍是退出码）。59 条断言（`pump.*`/`event.*` 只在 x86 跑，x64 51 条），legacy 同样必须真过 |
| 本地 REPL（T7 切片 D1） | `pwsh tools/server_test/e2e_repl.ps1 -Render <64\|86> [-Variant legacy]` | 管道喂 stdin 跑 `--repl`：`print(1+1)` → `2`（**管道下零提示符**）、多行续行（`for`/`body`/`end`）、`=` 简写（`=1+1`）、EOF → 退出码 **0**、语法错 → **3**、最后一条命令的错 → **2**、`--repl`+`--json` → 启动器退 **2**、`dbg.caps()` 可用（证明复用同一个 `lua_State`）。11 条断言（`repl.remote-exclusive` / `repl.exit-report` / `repl.no-v`；x64 无 fixture ⇒ 10 + 1 SKIP），86 / 86 legacy / 64 都要过 |
| `dbg` 本地门禁（切片 A/B/C1a） | `pwsh tools/server_test/e2e_dbg.ps1 -Render <64\|86> [-Variant legacy]` | 本地模式（不起 server）：`caps.*` 与本 build 一致、**`dbg.detach ~= nil` 且 `dbg.close == nil`**（D51）、`help.*`（含管道下**无 ESC**）、`kill.*`（真终止 + 状态复位）、**`session.*`**（D55：`dbg.last() == dbg.session(pid)`、`d:pump` 缓存优先返回同一张表、事件表字段与 `d.stop`、`read`/`read_u32`/`write` 往返、**partial read 的真实长度**、`context()` 的 `Cip` 与部分更新、`mods()`/`threads()`）。36 条断言（含 `session.*` / `pump.dispatch-attribution` / `route.same-name-two-processes` / `route.child-process` / `route.child-handle` / `d:current-pid` / `d:v-child` / `run.refused-with-session` / **`session.default-parity`**（契约名与会话方法逐字节一致）/ **`session.lv-types`** / **`context.all-variant`** / **`var.callback-only`** / **`var.d-v-bound`**/ `sugar.*` / **`remote.*`：86 宿主经 `--repl --server` 驱动 64 位目标**；x64 无 fixture ⇒ 8 条），legacy 同样必须真过 |
| JSON 信封（T1/D50） | `pwsh tools/server_test/e2e_json.ps1 -Render <64\|86> [-Variant legacy]` | 本地模式 `--json` / `--out=<path>`：那一行 JSON 独占 stdout、字段齐全、`result` / `pid` / `detached` / `images` 口径、`--json` 与 `--server` 互斥（退出码 2）、退出码口径不变；那一行必须**纯 ASCII**（CP936 → UTF-8 或逐字节降级）。`-Variant legacy` **必须真过**（这段就是用来暴露 GBK 的） |
| agent 技能包（T6） | `pwsh tools/agent/check_skill.ps1 -Render <64\|86>` | `tools/agent/autodbg-skill/` 是 CLI 口径下的**唯一对外契约文档**：检查 `SKILL.md` 的 frontmatter（`name` / `description`）并把每个 `.md` 里的 ` ```lua ` 片段交给产物做语法检查（只解析不执行）。17 条断言（6 条静态 + 11 个含 lua 块的 md）/ 58 个 Lua 片段（第 11 个是新增的 `recipes/child-process.md`） |
| nvim 客户端（T5） | `pwsh tools/server_test/nvim_smoke.ps1 -Render <64\|86> [-Variant legacy]` | 起 `--server=tcp` → headless nvim 跑 `nvim_smoke.lua`：远程执行 + 结果 buffer、异步路径不出 `E5560`、语法错误 → chunkname + quickfix、`M.cancel`（控制帧）、health/version（`info`）、`open_file` 写/读回（帧上的 `open`/`data`/`close`/`read_req`）。32 条断言（实测）；**nvim 不在 PATH 时 SKIP** |
| Win9x 实机 | `pwsh tools/w9x_test/w9x.ps1 -Action check\|run\|record` | 需要 VMware 的 Win98 guest 与凭据（`tools/w9x_test/config.local.ps1`，已 gitignore）；**没有 VM / 凭据时 SKIP（exit 0）**。矩阵与剩余项见 `plan.md` §1.3 |

- 黄金输出在本仓库 `tests/golden/w9x/`（Win98 guest 的输出），行尾与比较口径见 D15（golden 统一 LF，比较走「CRLF→LF + 去末尾空行」归一化）。
- 行为类缺陷（需要真机跑调试会话才能确认的）**只记录不修**，等 Win9x 环境就绪（`plan.md` §1.3）。
- 导入表类问题可用 `tools/api_check/main.py <符号>` 静态审计；新增任何 CRT/系统符号前先查。改完引擎/构建开关后跑一次 `scan_dir.py`，别让 `dbghelp.dll`、`K32*`、XP+ 的 `Nt*` 之类偷偷回到 legacy 导入表里。

## 5. 关键约束

1. **零 CRT**：不链接标准 libc（`/nodefaultlib` + `src/sys/msvcrt.def` / `ntdll.def` / `ucrtbase.def`）。`src/sys/include/**` 是**影子头文件**（`stdio.h`/`string.h`/`stddef.h`…），会覆盖 MSVC 的 CRT 头。不得使用 `printf`/`malloc`/`memcpy` 等标准库函数——用项目内封装：分配走 `autodbg::common::alloc/resize/release`，打印走 `autodbg::core` 的 `Print*` / `Error*` / `log_msg`。唯一例外是 `src/sys/include` 与 `msvcrt.def` 已显式引入的符号。
2. **不使用 C++ 标准库**（无 `std::` 容器/算法/`<memory>`）：容器只有 `common::IntrusiveList` / `ArrayList`，RAII 只有 `common::unique_handle`。
3. **Win9x 兼容**：不得用 Vista+ 才引入的 API；新 API 一律走 `src/common/dynapi`（解析失败返回 `nullptr` 是正常路径，必须降级），x86 产物导入表要求零 XP+。
4. **Lua 双版本**：代码需同时兼容 Lua 5.5 与 LuaJIT。已知地雷三例：① LuaJIT 没有位运算元方法（`__band/__bor/__bxor/__shl/__shr/__bnot` 是 5.3+），涉及位运算的 `LuaVariant` 脚本在 LuaJIT 下不可用；② **`LUAJIT_NO_UNWIND` 只允许 x86**（`PITFALLS.md` M4/M5、D19）；③ LuaJIT 的 `pcall` 不能跨 yield（`PITFALLS.md` L6）。
5. **架构边界**：`src/core/**` 与 `src/common/**` **不得感知 Lua**（不得 include `lua.h`/`lua.hpp`/`lauxlib.h`/`lualib.h`）。core 对宿主只暴露 `DebugCallbacks`（函数指针 + `void*` 上下文）与宿主无关的回调令牌 `callback_ref_t`。
6. **脚本 API 名字是契约**：绑定层的 C++ 函数叫 `lua_xxx`（snake_case），但 `lua_register` 的字符串（`Run`/`ReadMemory`/`SetBreakpoint`… 共 29 个，见 `src/scripting/lua/registry.cpp`）和 `Var` 元表方法（`to`/`tos`/`sto`/`write`/`writes`/`swrite`/`off`）**改不得**——改绑定层必须逐字节验证输出。新增能力用**新的全局表**（`gui`）或新名字（`read_file`/`write_file`，二者在 `feat/cli-tcp` 上被 `open_file` 的文件对象取代——它们本来不在 29 个契约名内，见 D44；新名字例：`gui.pick()`），不要平铺更多全局函数。
7. **结构体布局不可动**：`src/core/types.hpp` 里的 `PEInfo` / `BreakpointInfo` / `ThreadInfo` 等是 `#pragma pack(1)`，字段顺序/类型一字不改（`Path[1]`、`modname[1]` 是变长尾数组，参与偏移计算）。侵入式链表要求首成员是 `next` 指针。
8. **入口与 CRT 兼容层必须保持 C 链接**（`/ENTRY:mainCRTStartup` 与 `msvcrt.def` 按 C 名字查找）；零 CRT 下没有 `_atexit`，带非平凡析构的静态对象会 LNK2019（`main.cpp` 用 `new` 而非静态对象的原因）。
9. **CRT 缺口的补法**：`msvcrt.dll` / `ucrtbase.dll` 都没有（或只有 XP+ 版本）的符号，在 `src/sys/` 里自己实现，并同步影子头声明，这样各架构导入表零新增。现有两例：① `src/sys/strtoull.c` + `src/sys/include/stdlib.h`（LuaJIT 的 `tonumber(s, base)` 要 `strtoull`；`msvcrt.dll` 只有 XP+ 的 `_strtoui64`，Win9x 连它都没有）；② `src/sys/destruct_eh.c`（LuaJIT 的 `lj_err.c` 无条件引用 `__DestructExceptionObject`，Win9x 的 `msvcrt.dll` 没有它）——后者无法纯自实现，做法是「自己提供符号 + 运行时向已加载的 `msvcrt.dll` 转发，取不到就降级」。
10. **模块化运行时（§11）是硬边界**：引擎与绑定跑在**手工映射的模块**里，不得依赖 OS 加载器特性——禁止 `__declspec(thread)` / `thread_local`（含「函数内非平凡构造的 static」，`PITFALLS.md` M10）、模块内资源一律传自己的 `HMODULE`、不引入新的导出（唯一契约是 `autodbg_run` + `autodbg_boot`）。x86 的 `__aullshr` 类编译期辅助符号一律用 `src/sys/flt.asm`（**禁止**在 C 里重写），x86 产物与 x86 构建期工具都要链 `src\sys\flt.obj`。
11. **server 模式 = 远程代码执行**：默认只绑 loopback 且免 token（D21），非 loopback 强制 token；不要在文档/示例里鼓励把 `--server-addr` 绑到不可信网络（`plan.md` 的剩余项里也不要引入绕过这条的开关）。

## 6. 代码风格

- **编码**：自有源码统一 **UTF-8 with BOM**（当前例外：`src/sys/**` 与 `src/common/dbl2str.c` 无 BOM；根目录 Markdown 文档无 BOM）。编辑后确认 BOM 未被剥掉。
- ❗`make.bat` 本身是 **GBK(CP936)** 编码，用 UTF-8 语义的工具改写会弄坏其中的中文（历史上出过事故）。批处理注释里**不要写 emoji/生僻符号**（GBK 无法表示，写进去会变成 `?`）。
- **禁止**用 UTF-8 语义的工具改写第三方源码（`src/ScyllaHide/**`、`thirdpart/**`）与 `nvim/**`（子模块，按上游风格）。
- **行尾**：CRLF。
- **缩进**：4 空格，自有代码零制表符。
- **大括号**：Allman（左大括号独占一行）；`if (` / `for (` / `while (` 关键字后留空格。
- **命名**：新代码 `snake_case` 函数/变量、`PascalCase` 类名。有意保留的例外：Win32 风格（`hProcess`、`mainCRTStartup`、动态加载层的 `dNt*`）、第三方代码，以及历史 API 名（`core/util.hpp` 的 `Print`/`Error`/`PrintF`、`dbghelp` 回调）。
- **头文件保护**：C++ 头用 `AUTODBG_<模块>_<文件>_HPP`；纯 C 头用 `_XXX_H_`。
- **注释**：中文为主；每个文件开头有一段「文件职责 + 来源（迁移自哪个旧文件）+ 与旧实现的差异」的块注释，改文件时同步更新它。

## 7. 架构

```text
src/core/            调试引擎（C++，禁 Lua）
  core.hpp           DebuggerSession —— 唯一状态容器 + 引擎 API 成员函数（read/write/get_peinfo_by_ea/set_breakpoint/run…）
  types.hpp          引擎数据结构 + core↔绑定的回调契约（DebugCallbacks、事件载荷、WaitTimeoutAction）
  debug_loop.cpp     run()：WaitForDebugEvent 主循环 + 事件分发（不认 Lua，全部走回调）；C2a 起 `pump = wait_one + dispatch`
                     （宿主层按事件 pid 归属做进程级路由，见 scripting/lua/dbg_bindings.cpp）
  gui.hpp/.cpp       GUI 自动化原语（键鼠/屏幕/窗口/控件，不认 Lua；Lua 侧见 scripting/lua/gui_bindings.cpp）
  memory / pe / context / breakpoints / hardware_breakpoints / process / stack_trace / hide / util
src/scripting/lua/   唯一感知 lua_State 的层
  bindings.cpp       29 个契约全局的薄封装（参数编组 → 调 core）+ on_wait_timeout
  registry.cpp       register_lua_api()：29 个全局 + Var 元表 + `gui` 表（`read_file`/`write_file` 已随 T5/D44 删除——文件 I/O 只有 `open_file` 的文件对象）
  state              create_state_with_libs() / run_script_file() / 烘焙 lualib
  main.cpp           autodbg_run（唯一导出）+ CRT 兼容层 + `--selftest` / `--server` / `--repl` 三个分支
  repl.hpp/.cpp      `run_repl`：`--repl` 的读-求值-打印循环（合法 Lua 语句、`=` 简写、多行续行；管道可跑）
  server_config / tcp_server（raw TCP 服务端）/ client（`--remote`）/ exec / exec_guard / output /
  file_io + file_stream（`open_file`）/ gui_bindings / dbg_bindings（T7.3 的 `dbg` 表：create/attach/pump/cont/
  detach/kill/…）/ help_bindings（全局 `help([topic])`）/ variant / bson / helpers
  · dbg_bindings 自 C1a（D55/D57）起是**会话对象**：`dbg.create/attach` 返回 full userdata，`AutoDbgSession.methods`
    是方法表；会话状态（`SessionState`）进 C 侧侵入式链表 registry（`dbg.sessions/session/last`），
    `d.stop` 是当前未应答事件的**普通表**
  （`http` / `base64` / `exec_session` / `effects` / `mcp.lua` 已在 T5 随 D41/D43/D44 删除）
src/common/          共享基础设施：allocator、containers、dynapi、handle、dbl2str、cmdline、
                     net（winsock 动态载入）、frame（帧头编解码）、json_writer、rand、png_writer、selftest、
                     console_action（控制台输入判定：纯函数）、console_style（help 上色判定：纯函数）、platform（is_win9x / arch / variant / flavor）
src/sys/             零 CRT 运行时：影子头文件、*.def 导入表、x86 MASM（flt.asm）、mainCRTStartup、
                     CRT 缺口实现（strtoull.c / destruct_eh.c）
  pack/              打包与加载：boot.h（exe↔模块 ABI 契约）、payload（LZMA 单流解包）、
                     module_loader（内存模块映射器：map/start/resolve + x64 RtlAddFunctionTable）、
                     launcher.cpp（选项/flavor/variant/server/对话框/boot 组装）、pack_entry.c（启动器入口）
  lzma/              7-Zip LZMA（Public domain）：解码侧进产品，编码侧只给 tools/pack
src/ScyllaHide/      第三方反反调试（只被 modern 变体链接）
thirdpart/           Lua 5.5.1（子模块）与 LuaJIT（子模块）
lualib/              烘焙进模块的 Lua 库（inspect、jsonx、moses、dbglib）
nvim/                neovim 插件（子模块，独立仓库 AutoDbg.nvim）
tools/               构建期：gen_lua_lib.lua / gen_hook_data.lua / pack（载荷生成）/ pever（PE 版本补丁）
                     测试与门禁：api_check（导入表）、server_test（e2e）、w9x_test（Win9x 实机）
tests/golden/w9x/    Win98 guest 的黄金输出
```

会话模型：引擎可多实例（`DebuggerSession` 成员函数用自身状态）；`current_session()` 只留给宿主层与签名固定的回调，为 nullptr 时 fail-fast。server 模式下执行严格串行（并发 1）：同一时刻只有一个脚本持有会话。

交付形态：`.out/AutoDbg_<arch>.exe`（零 CRT 启动器）在运行时就地解包出模块并**手工映射**（不落盘），然后调模块唯一的导出 `autodbg_run`。打包/映射/ABI 的细节见 **§11**。

## 8. Git 规范

- 提交信息用中文，遵循 Conventional Commits（`feat:` / `fix:` / `refactor:` / `test:` / `docs:` / `style:`）。
- 主分支为 `main`。
- 提交粒度：一个可 review 的改动一次；发现型提交（测试暴露的缺陷）与修复分开。
- `nvim/` 是独立仓库的子模块：改插件要在 `nvim/` 里单独提交（本仓库只更新 gitlink）。

## 9. 文档导航

| 位置 | 作用 |
| --- | --- |
| `AGENTS.md`（本文） | 构建 / 门禁 / 约束 / 风格 / 架构 / 打包 ABI / 已定决策 |
| `PITFALLS.md` | 已踩过的坑，按「构建 / 零 CRT / 模块映射 / Lua / 输出 / server / nvim / Win9x / 门禁」分类 |
| `plan.md` | **剩余工作**（Win9x 矩阵、server/GUI 实测与文档同步、CLI/帧协议尾巴）——三份 plan 的合并版 |
| `plan_t7.md` | T7 的独立计划：状态机、数据面原语、REPL 与协议归因、AI + IDA MCP 的缺口；**§12 = 交接清单（剩余任务与复跑基线）** |
| `plan_bootloader.md` | T8 的独立计划：交付面必须 32 位、按 host 选 exe、tmpdir 落盘与 handoff（命令行透传 / 退出码 / job）、job 与 dynapi 口径、多 session 的进程树路由 |
| `nvim/README.md` | neovim 插件的安装、配置、命令与可编程 API |
| `tools/agent/autodbg-skill/` | **面向 agent 的技能包**（Agent Skills 格式）：`SKILL.md` 入口 + `references/`（CLI / Lua API / 地雷）+ `recipes/`（8 个场景，含 `stop-and-inspect.md` 的 `dbg` 会话 + `dbglib` 配方、`cross-arch-remote.md` 的跨位数代理、`child-process.md` 的进程树 / `d:proc(pid)` / `d:current_pid()`）。CLI 口径下的唯一对外契约文档（T7 的主面 `dbg` / `dbglib` / `help` / `caps` / `--repl` 已写进去） |
| `make.bat` | 构建定义（ninja 生成器） |
| 各文件头部注释 | 该文件的职责、来源、与旧实现的差异（第一手依据） |

## 10. make.bat 编写规范（新增/修改构建脚本的标准写法）

本节是对现有 `make.bat` 的归纳。改成或新增构建脚本（含 luajit、打包、变体）时**照此写**，不要另立风格。

### 10.1 文件形态

- `make.bat` 是 **GBK(CP936) + CRLF**；新增的中文注释与 usage 文本必须按 GBK 写入，不得用 UTF-8 语义的工具改写整个文件。
- 固定骨架顺序：`@echo off` → `setlocal enabledelayedexpansion` → 全局变量初始化 → `:next`（参数解析）→ `:start` → 各子程序 → `:usage` → `:err`。
- 注释：批处理注释用 `::`；`REM` 用于生成 ninja 内容时的说明。**不要**用 `VSLANG` 之类的环境变量去「翻译」cl 的输出（它改不了 `/showIncludes` 的前缀，`PITFALLS.md` B2）。

### 10.2 全局变量

只在文件开头初始化一次：`ROOT`（`%~dp0`）、`SRCDIR`、`THIRDPARTDIR`、`BUILDROOT`（`.build`）、`INSTALLROOT`（`.out`）、`BUILD_MODE`、`ARCH_ARG`。

- 这些量带尾斜杠，拼接时不再补斜杠；子程序里**不得**重新定义它们。
- 子程序自己的开关用局部变量（配合 `setlocal`）。

### 10.3 参数解析

- 单字母选项；`if "x%1" == "x" goto start` 收尾；未知参数 `goto usage`。
- 带值的选项 `shift` 两次（`b <path>`、`i <path>`、`a <arch>`）。
- **所有比较都加 `x` 前缀**：`if "x%VAR%" == "x1"` —— 变量未定义时不加前缀会触发批处理语法错误（已出过一次事故）。
- 每个选项块用 `( ... )` 括起来，块内 `shift` + `goto next`。
- 架构名走**显式白名单**（`64` / `86`；`98` 报退役；其余报错），不要让未知值落到默认分支（`PITFALLS.md` B10）。

### 10.4 子程序契约

- label 前用注释写「用法 + `defines:`（调用方必须预置的变量）」，例如 `:build_ccs` 的 `TP_CFLAGS` / `EXCLUDE` / `OBJS` / `ODEPS`。
- 需要隔离变量的子程序开头 `setlocal`（`:config`、`:buildautodbg`）；靠调用方预置累加变量的（`:build_ccs`、`:build_ar`）**不** `setlocal`。
- 调用一律 `call :label args || exit /b 1`（或 `|| goto err`）。
- **架构差异只在 `:build` 开头集中定义一次**（`SUBSYS_VER`、`CFLAGS`、`LDFLAGS`、`MLEXE`、`MACHINE`、`LINK_DEPS`、`LJ_UNWIND`、`PATCHVER`、`TARGET_ARG`、编码），下游子程序只读这些变量，不各自 `if %ARCH%`。变体差异用 `if %1 == luajit ( ... )` **追加**，不改公共基线。
- **两类 flags 不得同名**：`CFLAGS` 是被固化进 rule 的**基线**（`cc`/`cxx`），走 `$flags` 的 per-target flags 另起名（`:buildautodbg` 用 `AD_FLAGS`、第三方用 `TP_CFLAGS`）。
- `MACHINE` 除了 `lib.exe /machine:` 还被用于文件名 `InstrumentationCallback%MACHINE%.asm`（实际文件是大写 `X64`/`X86`，靠文件系统大小写不敏感匹配）——改名时要连文件名一起考虑。

### 10.5 ninja 文件生成

- 先写 `rule`，再写 `build`；每条 build 语句后 `echo.` 空行分隔。
- 批处理变量用 `%`，ninja 变量用 `$`（`$in` / `$out` / `$flags` / `$lua` / `$syslib`）：rule 里固化公共 flags，per-target 差异走 `flags =` 行。
- 路径里的 `:` 必须转义成 `$$`：用 `%SRCDIR::=$:%` 或 `!SRC::=$:!`。
- 管道符在 `echo` 里写 `^|`；`|` 是**隐式依赖**（变新即触发重跑），`||` 才是 order-only（只保证顺序）。
- 输入用绝对路径（`%SRCDIR%` / `%THIRDPARTDIR%`），输出 obj 路径相对 build 根且**镜像源码目录结构**。
- 依赖类规则（`cc` / `ccx` / `cxx` / `ccv9x` / `cxxv9x`）带 `deps = msvc`，**且 command 里必须有 `/showIncludes`**（两者缺一，头文件依赖就不会被记录，改了头等于没改）。
- **构建期工具要作为隐式依赖挂在用它的边**：`tools\pack.exe`（`pack` 边）、`tools\pever.exe`（`pever` 边）都必须写 `^| %BUILDDIR::=$:%\tools\xxx.exe`——否则并行构建时工具可能还没生成。
- 需要串联多条命令（`copy && tool`）时，rule 的 command 必须自己以 `cmd /c` 开头：**ninja 不经过 cmd.exe**（`PITFALLS.md` B4）。

### 10.6 规则清单（新增变体不得另立同名规则）

| rule | 用途 |
| --- | --- |
| `cc` | 自有 C（`/std:c17` + 项目 `CFLAGS`） |
| `ccx` | 第三方 C（自带全套 flags，不继承项目 `CFLAGS`） |
| `cxx` | 自有 C++（`/std:c++20 /Zc:__cplusplus /Zc:preprocessor`） |
| `ccv9x` | 自有 legacy 变体 C（`CFLAGS_V9X`：ANSI + `/execution-charset:gbk`；D18 的独立规则） |
| `cxxv9x` | 自有 legacy 变体 C++（同上 + `/std:c++20`） |
| `ld` | 零 CRT 产物链接（`/nodefaultlib` + `$syslib`） |
| `ldx` | 允许默认库的第三方链接（第三方 DLL） |
| `as` | MASM（`MLEXE`：x86 用 `ml.exe`，x64 用 `ml64.exe`） |
| `ar` | `lib.exe` 打包静态库 |
| `imp` | 由 `.def` 生成导入库 |
| `cp` | 拷贝头文件到 `<flavor>\include` |
| `gendata` | `tools/gen_lua_lib.lua`（字节码烘焙） |
| `genhookdata` | `tools/gen_hook_data.lua` |
| `dasm` | `dynasm/dynasm.lua`（minilua 跑）预处理 `vm_*.dasc` → `luajit\host\buildvm_arch.h` |
| `genvm` | `buildvm.exe -m <mode>`（`peobj` → `lj_vm.obj`；`bcdef/ffdef/libdef/recdef/folddef` → `lj_*.h`） |
| `genver` | `host/genversion.lua` → `luajit\include\luajit.h`（带 `restat = 1`） |
| `pack` | `tools\pack.exe`：把模块列表压成 `packdata.c`（LZMA 单流 + 导入审计，§11） |
| `pever` | `cmd /c copy /y $in $out && $pevertool $out 4.0`：拷到 `.out` 并改写 OSVer/SubsysVer（仅 x86，D12） |
| `t` | 占位锚点（只写 MZ 头的空文件），挂在 exe 链接行的隐式依赖（`\|`）上 |

按扩展名选规则：`if %%~xj == .cpp` → `cxx`，否则 `cc`；第三方排除清单用 `EXCLUDE` + `findstr /i "\|%%~nxi\|"`。

### 10.7 自举与代码生成

- 代码生成 rule 的解释器用 ninja 变量传入（`lua=`），且必须指向**本架构已构建的产物**（`%INSTALLROOT%\lua_%1.exe`）。
- 生成语句的隐式依赖（`|`）必须同时包含：**解释器产物** + **生成脚本本身**（否则改脚本不会触发重跑，只能 touch 输入文件或清一次）。
- 生成物（`datas_<flavor>\datas_<flavor>.c | datas_<flavor>\datas.h`、`HookLibraryData.c | .h`）作为隐式依赖挂在消费它的编译单元上（目前只有 `state.cpp` 依赖 `datas.h`）。`datas_<flavor>` 是 **flavor 级共享**：只有 modern 生成，legacy 复用（否则 ninja 报 `multiple rules generate`）。
- 字节码格式取决于跑脚本的解释器：`lua` 变体用 Lua 5.5 产物，`luajit` 变体必须用 luajit 产物 —— 同一份 `lualib` 需要各自变体的生成目标。
- LuaJIT 的三段生成链（全部靠隐式依赖 `|` 串起来，不靠 order-only）：
  ```text
  minilua.exe ─┬─→ buildvm_arch.h (dasm) ─→ buildvm.exe ─→ lj_vm.obj / lj_bcdef.h … lj_folddef.h (genvm)
               └─→ luajit.h (genver)
  ```
- `genver` 的输入里有 `luajit\luajit_relver.txt`（由 make.bat 从 `git -C thirdpart\luajit show -s --format=%ct` 写入）：**内容没变就不碰文件**，变了才写、并顺手删掉 `luajit.h` 让 genver 重生成——否则 `luajit.h` 永远比 `relver.txt` 旧，ninja 每次都判定要重跑。
- `HOST_CFLAGS`（`minilua`/`buildvm`，真 CRT）必须带 `/Iluajit\host`：`buildvm.c` 里是 `#include "buildvm_arch.h"`，引号包含只会去「包含者所在目录」找，而那个头在 build 目录里。
- `LJ_CFLAGS`（`luajit.lib`，零 CRT）必须带 `/Iluajit`（生成的 `lj_bcdef.h` 等）、`/I%SRCDIR%\sys\include`（影子头）、`/Iluajit\include`（生成的 `luajit.h` 要排在子模块源码目录之前），以及 `%CHARSET_FLAGS%`（影子头是 UTF-8，缺 `/utf-8` 会刷 C4819）与 `%TIME_T_FLAGS%`（x86 影子 `time.h` 的 `time_t` 是 `long`，需 `/D_USE_32BIT_TIME_T` 与 `corecrt.h` 对齐）。

### 10.8 变体（flavor）参数化 —— luajit 脚本的标准写法

- 入口是 `:buildautodbg <lua|luajit> [modern|legacy]`：`%1` = flavor，`%2` = 变体（缺省 modern），`%ARCH` = 架构（由 `:build` 设置）。
- 命名固定：模块 `AutoDbg_<flavor>_<arch>.dll` / `AutoDbgLegacy_<flavor>_<arch>.dll`（目录 `<flavor>_<variant>\`）、静态库 `<flavor>\<flavor>.lib`、头文件目录 `<flavor>\include\`。
- 每个变体必须提供 `<flavor>\include\{lua.h, luaconf.h, lualib.h, lauxlib.h}`（走 `cp` 规则）和 `<flavor>\<flavor>.lib`（走 `ar` 规则），否则链接行里的 `%1\%1.lib` 缺依赖。
- 变体差异只以追加形式表达：`/FIlj.h`（强制包含，必须在 `windows.h` 之后）、`/DAUTODBG_WIN9X`、`/DJIT`、`%LJ_UNWIND%`（目前= `/DLUAJIT_NO_UNWIND`，**仅 x86**）；legacy 变体走 `ccv9x` / `cxxv9x` 并追加 `/DNO_SCYLLA_HIDE`。
- `GetModuleHandleExA` 垫片是 `src/sys/include/lj.h` 里的 `static __inline`（`AUTODBG_WIN9X` 时把调用点改名）。之所以是 inline 而不是 `src/common/win9x_shim.c` 的外部符号：LuaJIT 的调用点被编进 `luajit.lib`，而这份库同时供 `luajit_<arch>.exe` 与（luajit 变体的）模块 DLL 链接，外部符号没有共同的提供者。`win9x_shim.c` 在 `AUTODBG_SHIM_INLINE` 时退让（避免同一 TU 里 static inline 与外部定义打架）。
- LuaJIT 的 dynasm 走 `dasm` 规则（`thirdpart/luajit/src/vm_x86.dasc`（86）/ `vm_x64.dasc`（64） + `dynasm/dynasm.lua`，用 `lua=` 指向 `luajit\host\minilua.exe`；x64 追加 `-D P64`，x86 追加 `-D NO_UNWIND` 与 `%LJ_UNWIND%` 对齐），`buildvm.exe` 走 `genvm` 规则；`lj_vm.obj` 只能由 `buildvm -m peobj` 产生（dynasm 无输出格式选项，不能走 `as` 规则）。
- `default` 目标在文件末尾列出本架构的**交付物 + 自举 host**（`lua_<arch>.exe` / `luajit_<arch>.exe` / `AutoDbg_<arch>.exe`）；模块 DLL 通过 `packdata.c` 的依赖隐式构建，不进 `DEFS`。新增变体要同步 `DEFS` 与 `:usage` 文本（GBK）。

### 10.9 compile_commands.json

- 只在单架构模式（`make.bat a <arch>`，`GEN_COMPILE=1`）生成；多架构循环时不生成。
- 文件以 `[` 开头、`]` 结尾；条目之间用**前导逗号**（`,{...}`），避免尾逗号。
- **条目会成倍增长（预期）**：`src/**` 被 flavor × variant 各编一次，同一源文件有 4 条 entry（lua+modern 先写，其后 luajit+modern、lua+legacy、luajit+legacy）；clangd 取第一条（modern、UNICODE、`/utf-8`）。
- `command` 里带 clangd 需要的 `--target=`（`TARGET_ARG`）；路径反斜杠双写，所以用的是转义版变量（`CFLAGS_JSON` = `%CFLAGS:\=\\% /std:c17`、`CXXFLAGS_JSON` = 同基线 + `/std:c++20`）。
- 只写**实际参与编译**的文件（被 `EXCLUDE` 跳过的不写）。

### 10.10 目录与错误处理

- 目录创建集中在 `:build` 开头（build 根、`<flavor>\include`、各模块/变体 obj 目录、`datas_<flavor>`、`pack`、`pever`、`launcher`、**`gen`**）；子程序只在确实新增目录时 `mkdir`。
- **构建期生成的文件**（都要遵守「内容不变就不写」）：
  · `:genversion` → `<build>\gen\autodbg_build.h`（版本宏；由 `git describe` 算，与 ninja 无关，在 `ninja -C` 之前跑）；
  · `luajit_relver.txt` → 喂给 `genver`（见 §10.7）。
  两者都把结果作为**编译期的头/输入**，所以必须挂在包含它们的 TU 的依赖链上（前者靠 `deps = msvc` 的 `/showIncludes` 记录，后者靠 ninja 的隐式依赖）。
- 失败一律 `|| exit /b 1` 或 `|| goto err`；`:err` 打印 `error` 并 `exit /b 1`。
- `ninja -C %BUILDDIR%` 放在 `:build` 末尾（每个架构生成完立刻构建），不要把构建集中到 `:start`。

## 11. 打包与模块 ABI（单文件 exe + 内嵌模块）

### 11.1 产物矩阵

| arch | 载荷里的模块 | 交付物 |
| --- | --- | --- |
| `86` | `AutoDbg_{lua,luajit}_86.dll`（modern）+ `AutoDbgLegacy_{lua,luajit}_86.dll`（legacy） | `.out/AutoDbg_86.exe` |
| `64` | `AutoDbg_{lua,luajit}_64.dll`（只有 modern：x64 全是 NT5+） | `.out/AutoDbg_64.exe` |

- 模块 DLL 是**中间产物**（留在 `.out` 供打包与 `--module-dir` 调试），交付只有那一个 exe。
- 变体 = 编译开关（modern：UNICODE + ScyllaHide；legacy：ANSI + `/DNO_SCYLLA_HIDE` + `/DAUTODBG_WIN9X`），**源码 100% 共用**。
- x86 的 exe / DLL / 自举 host 都要用 `pever.exe` 把 OSVer + SubsysVer 改成 `4.0`（否则 Win9x 直接拒绝加载）；x64 保持 `6.0`。

### 11.2 载荷格式（无 magic / 无版本号）

小端：

```text
u32 entry_count
entry[entry_count]:  u16 name_len | char name[name_len]（小写、无路径） | u32 raw_size
u8  props[5]         // 全包共用一份 LZMA 属性
payload              // 单条 LZMA 流：各模块原始字节按 entry 顺序背靠背
```

- 生成物是 `.build/<arch>/<mode>/pack/packdata.c`（`g_packdata[]` + `g_packdata_size`），作为**隐式依赖**挂在启动器链接行上。
- **不设 magic / 版本号**：生成器与消费者永远出自同一次构建（ninja 保证）。自检只做结构边界校验（entry 数上限、`name_len` / `raw_size` 越界、解压长度 == `raw_size` 之和）。
- **单流**（不是每模块一条流）：四个 x86 模块高度同质，独立流会多付约 2 倍体积；代价是启动即解压全部模块（x86 ≈19.5 ms / 2.3 MB 缓冲，x64 ≈17 ms / 1.5 MB）。
- `tools/pack` 会**审计导入表**：模块引用了非白名单系统 DLL、又不在打包集合里 → 构建期直接报错（把「漏打包 xxx.dll」挡在构建期）。新增系统依赖必须先加白名单（`PITFALLS.md` X1）。

### 11.3 exe ↔ 模块的 ABI（唯一契约）

`src/sys/pack/boot.h` 的 `autodbg_boot` + `AUTODBG_ABI_VERSION`；模块导出**唯一** `__declspec(dllexport) int __cdecl autodbg_run(const autodbg_boot *)`。

- `abi` / `size` 不符 → 直接拒绝启动（不读半个结构）。
- `cmdline_a` / `cmdline_w`：用 `src/common/cmdline.c` 的 `autodbg_split_cmdline_a/w` 自己拆 argv（映射后的模块拿不到进程参数，不用 `__getmainargs`）。
- `script_a` / `script_w`：启动器已选定的脚本（UNICODE 模块只看 `script_w`，所以启动器两份都填）。
- `flavor` / `variant`：仅供诊断（模块内容已由载荷决定）。
- `selftest` / `server_mode` / `server_*`：S0 追加的字段（D27），ABI 版本因此到 **2**。
- `json_mode` / `json_out`：T1（D50）追加的字段（`--json` / `--out=<path>`），ABI 版本因此到 **3**。
- `remote_host` / `remote_port` / `remote_cmd` / `pid` / `read_timeout_ms` / `max_frame`：T3（D42）追加的字段（`--remote` / `--pid` / `--read-timeout` / `--max-frame`），ABI 版本因此到 **4**。
- `repl_mode`：T7 切片 D1 追加的字段（`--repl`：`0` = 普通脚本、`1` = REPL），ABI 版本因此到 **5**。

改这个结构或导出名 = 改 ABI，必须同步 `AUTODBG_ABI_VERSION` 并重跑全部运行路径。

### 11.4 启动器（exe 侧）的行为

1. 解析启动器选项 → `payload_open()`（只解析 manifest，不解压）
2. 定脚本：`argv[1]`，否则文件对话框（`GetOpenFileNameW` 走 dynapi，9x 降级 A 版）；`--server` / `--selftest` 不要求脚本
3. 定 flavor：`--flavor` > 脚本首行 `--!lua` / `--!luajit` > 字节码 magic > 默认 lua 5.5
4. 定 variant：平台探测（9x / NT4 → legacy）；`--variant` 只用于排障
5. `map()` 失败 → **回退另一个变体**并打印原因；两个都失败 → exit 2
6. `start()` → `autodbg_run(&boot)` → `ExitProcess(返回码)`

### 11.5 硬约束（手工映射的模块里**不得**出现的东西）

1. **禁止 `__declspec(thread)` / `thread_local`**：映射器不分配 TLS 槽，跨线程的静态 TLS 会读到垃圾；线程局部状态一律走显式参数或宿主侧 `TlsAlloc` 封装。**函数内非平凡构造的 static 等价于隐式静态 TLS**（`PITFALLS.md` M10）。
2. **模块内资源一律传自己的 `HMODULE`**：`FindResource(NULL)` 找到的是主 exe 的资源。
3. **不得依赖 OS 加载器的登记**：模块不在加载器名单里（`GetModuleHandle` 枚举、dbghelp 自省都看不到它）。x64 的展开表由映射器 `RtlAddFunctionTable` 注册（F5）→ **x64 可以用 SEH**；x86 不能 → LuaJIT 只在那时 `/DLUAJIT_NO_UNWIND`。
4. **不引入新导出**：`autodbg_run` 是唯一导出。
5. **`GetModuleFileName(NULL)` 返回主 exe**——这是**想要**的语义（脚本相对路径、模板生成都基于它），不要"修正"成模块路径。
6. 项目仍**禁止 C++ 异常与 `__try`**（§5）；SEH 只在 x64 的 LuaJIT 内部间接使用。

### 11.6 排查手法

见 `PITFALLS.md` 的 O 节（输出与排障）与 M 节（映射器）。速记：模块里的 `Print*` / `Error*` 在非控制台下走 `WriteFile`、控制台里走 `WriteConsole`（重定向才能看到）；怀疑寄存器被写坏时用 `RtlCaptureContext` 抓 callee-saved 快照；回退链要带 `load_error.text`；负例判据用「已提交字节的双窗口增量」。

## 12. 已实现的扩展面（server / GUI / client 文件 I/O / REPL / 进程树）

> 这一段只记**现行接口约定**（改之前要知道的事实）；剩余工作与设计取舍见 `plan.md`。
> ❗D41–D50（原 `plan_cli.md`，现 `plan.md`）已全部落地：MCP 与 HTTP 已整体删除，对外只剩 CLI + raw TCP 帧。下面被取代的条目（§12.1 的端点、§12.3 整节、§12.4 的效果协议）只在原地留历史标注。

### 12.1 server 模式

> ❗D41 / D42：HTTP 形态整体删除。`--server-*` 的语义保留（认证默认 D21 不变，token 走握手帧），`--server-max-body` → `--server-max-frame`，新增 `--server-read-timeout`（默认 **5000 ms**，单次文件操作）与 `--server-timeout`（整次执行 wall-clock）分开；端点改成帧，`--health` / `--version` 改由 CLI 经 TCP 提供。
>
> **T2 已落地（分支 `feat/cli-tcp`）**：`--server=tcp` 选 raw TCP 帧（`boot.server_mode = 2`，不需要新 ABI 字段）→
> `src/scripting/lua/tcp_server.cpp`；HTTP 半边已随 T5 删除：`--server` 就是 raw TCP，`--server=http` 直接报错（退码 2）。
> 已实现：监听 + 串行 accept、握手帧（`id` = 协议版本、`data` = token、其余字段留 json 给执行侧）、
> `script` → `out`/`result`/`end`、`control`（health/version → `info`；cancel → guard + `ack`；
> kill/detach → 软动作 + `ack`；shutdown → `ack` + 停机）、**执行期的两个 poll 点**
> （`on_wait_timeout` 的 100 ms + `lua_sethook` 的计数钩子 → `stream_poll_and_dispatch`），
> 所以 `--cancel` / `--kill` / `--detach` 在脚本跑着的时候就能生效。

- 归属：server 跑在**模块（DLL）里**（需要 `lua_State` 与引擎会话），启动器只解析选项；`--server` 时允许不给脚本。
- 选项（全部 `--server-*` 前缀）：`--server[=tcp]`（`http` 已随 D41 删除，传了报错）、`--server-addr=<ip:port>`（默认 `127.0.0.1:8642`）、`--server-port=`、`--server-token=<hex|auto|none>`、`--server-max-body=`（= 一帧上限）、`--server-queue=`、`--server-timeout=`、`--server-log=<path>`、`--server-no-echo`。
- 端点 → **帧**（`tools/agent/autodbg-skill/references/cli.md` 有全表）：`script` / `control`（health·version·cancel·kill·detach·shutdown）/ `out` / `result` / `end` / `read_req` / **`event`（16，命中记录：json = 记录字段，`id` = 产生它的命令号，0 = 无归属）**。\n  **协议版本 2（T7 切片 D2a）**：`out` 帧的 `id` = **命令号**（`script` 帧的 `id`；无命令时 0），`result` 帧的 `id` 仍 = **退出码**（u16 二补数）——客户端必须实现归因，所以只能 bump。
- 绑 `0.0.0.0` 时启动横幅会**列出本机网卡地址**（地址 + 网卡描述），方便 client 直接挑一条去配 `--remote=<地址>:<端口>`：
  地址来自 `iphlpapi!GetAdaptersInfo`（**必须动态载入**，Win95 没有；取不到就退 `gethostname`/`gethostbyname`，
  再取不到就安静不列）。结构体在 `net.cpp` 里自定（不 include `iphlpapi.h`），字段类型与 SDK 逐项对应。
  门禁：`e2e_tcp.ps1` 的 `net.banner-*`（只断言 ASCII 部分，legacy 变体的中文是 GBK 字节）。
- 认证（D21）：loopback 且未指定 token → **不校验**；非 loopback → **强制** token（`auto` 生成或显式给），`--server-token=none` 在非 loopback 上拒绝启动。
- 并发：accept 线程 + **单个** worker 线程（脚本串行）；`/health`、`/cancel`、`/version` 在 accept 线程直答，不被长脚本饿死。
- 取消/超时：`exec_guard` 的两条注入路径（`on_wait_timeout` 100 ms / `lua_sethook` 每 100 万条指令），server 模式下**不得**回落到 `CheckConsoleInput`（`PITFALLS.md` S2）。
- 输出：`core::set_output_sink` 捕获 `Print*`/`Error*`，并覆写 Lua `print`；`io.write` / `puts("done")` 仍是已知缺口（fd 级 `_dup2` 重定向未做）。

### 12.2 GUI 自动化（Lua 全局表 `gui`，D23）

- 落点：`src/core/gui.*`（不感知 Lua）+ `src/scripting/lua/gui_bindings.cpp`（单一 `gui` 表，30 余个方法，方法表在该文件末尾），失败返回 `nil, errmsg`。
- 能力：鼠标/键盘注入（`SendInput` → 9x 落 `mouse_event`/`keybd_event`）、屏幕与像素、窗口枚举/查找/激活/矩形、子控件读写与消息、`gui.screenshot`（PNG stored-deflate，D24，也支持 `bmp`/`raw`）。
- 所有新 API 走 dynapi 并有降级（`PrintWindow`、`MonitorFromWindow`、`SetProcessDPIAware`、`BlockInput`）；静态只多链 `gdi32.lib`（w95 就有）。
- `gui` 是**唯一**的 GUI 入口：不要为 GUI 再加平铺全局名（§5 第 6 条）。

### 12.3 MCP（已删除，D41）

- HTTP + JSON-RPC 分发（`lualib/mcp.lua`）、会话表（`exec_session`）、`POST /mcp` 与 `--server=http`
  已在 T5（D41）整体删除。原 `lua_api` 的正文已搬进 `tools/agent/autodbg-skill/references/lua-api.md`（T6）。
- S12 的教训（每个对外接口都要有门禁断言）在帧命令面上继续适用：`control` / `read_req` 等
  每一条都有 `e2e_tcp.ps1` 的断言。

### 12.4 client 文件 I/O（唯一入口：`open_file` 的文件对象）

> ❗D44 / D45：`read_file` / `write_file` 已被文件对象的 io 风格方法取代（`f:read("*a"|"*l"|"*n"|N)`、`f:read_line()`、`f:lines()`、`f:seek()`、`f:tell()`、`f:setbuf()`、`f:write()`、`f:flush()`、`f:close()`）；效果协议 + `base64` + 协程执行器 + `/effects` 已在 **T5 删除**。
> 「调试回调里不可用」这条限制取消：回调里 `f:read()` 可以**阻塞等回包**（等待循环按 ≤250 ms 切片 `select`，每片检查 `exec_guard`），超时 → `nil, "timeout"` 后回调返回、断点继续。前提：执行连接的**唯一 reader 是脚本线程**（三个消费点：`f:read()` 等待循环、`on_wait_timeout` 的非阻塞 poll、`lua_sethook` tick）；其它连接是控制连接，由 accept 线程处理。

文件在**客户端**那台机器上；本机脚本模式下两者都等价于 `io.open`。

> **T4 已落地（分支 `feat/cli-tcp`）**：`open_file` 现在真支持**读**（r/rb），语义按 D49 钉死：
> 默认 format `"l"`（去行尾）、`f:read(整数)` 读 N 字节、EOF → `nil`、错误 → `nil, err`、
> 字节精确（不做 CRLF 归一）、每句柄 4096 缓冲（`f:setbuf()` 可调，上限 8192）、
> `f:tell()` 是逻辑位置、`f:seek()` 往返且清缓冲、`f:lines()` 自己实现迭代器。
> 帧协议（二进制，D28）：`open` 的路径走 `data`、模式走 `flags`；`read_req` 的
> `flags` = op | (whence << 4)、`data` = {u32 arg}；`read_resp` 的 `flags` bit0 = EOF /
> bit1 = 错误、`data` = 字节或 {u32 pos}。等回包时切片 25 ms，片间查取消 →
> `nil, "cancelled"`；超时 → `nil, "timeout"` 并用 `cancel_notify` 让客户端丢掉那一轮。
> ❗chunked（现行 HTTP）是单向往回的：那种请求里 `open` 不等应答（T5 删 HTTP 后这条分支消失）。

**① 流式（默认，`X-AutoDbg-Stream: 1`）——大块数据、调试回调里都靠它**

- `local f, err = open_file(path [, mode])` → 文件对象；`f:write(...)` / `f:flush()` / `f:close()`。
- `/run` 的响应变成 `Transfer-Encoding: chunked`，每个 chunk 里套一个帧（`src/scripting/lua/stream.hpp`）：
  `u8 type | u8 flags | u16 id | u32 data_len | [json] | [data]`；
  `type` = 1 open / 2 data / 3 close / 4 out / 5 result / 6 error，`out` 的 `flags` 区分 stdout/stderr。
- **写这条 socket 的就是执行脚本的那条线程**（worker 在 `exec_begin` 之前就拿到了 sock）→
  `f:write` 既不 yield 也不落盘：**内存 = 单次 send 的那一块**，整个文件多大都行。
  背压交给 socket（`net_send_all` 带超时）：客户端断了/太慢就 `nil, err`，不会无限卡住调试目标。
- `print`/`Error*` 在流式请求里也走 `out` 帧（`output.cpp` 先查 `stream_active()`），所以长会话也能实时看到输出。
- ❗流式请求下 `read_file`/`write_file` 被禁用（它们靠 yield）：返回 `nil, <提示>`（`file_io.cpp`
  的 `yield_blocker`）；要去掉流式就用旧的非流式路径。
- 客户端实现：`nvim/lua/autodbg/stream.lua`（uv TCP + chunked + 帧解析，`data` 帧直接写本地文件）。

- ❗T5（D44）：`read_file` / `write_file` + 效果协议（`/effects`、token、base64、协程执行器）已整体删除；
  文件 I/O 只有 `open_file` 的文件对象（读写都走帧，见上面的 T4 段）。

### 12.5 neovim 插件（`nvim/`，子模块）

- 命令：`:AutoDbgRun`（当前 buffer / 选区 / 文件）、`:AutoDbgCancel`、`:AutoDbgHealth`（报本次的模式与地址）、`:AutoDbgHeader [remote|local]`（插入脚本头标记）、`:AutoDbgLsp [on|off]`（挂/摘注解库）。
- **两种运行模式（D39）**：能拿到 exe（标记 `exe=` > `setup.exe` > PATH 里的 `AutoDbg_{64,86}.exe`）→ **本地模式**直接跑 exe；否则 → **远程模式**（HTTP `/run`）。标记的 exe 不存在时**回落远程**并提示一次；`setup{ mode = "remote" }` 强制远程，`mode = "local"` 则必须本地（找不到就报错）。
- 本地模式：buffer 写到临时文件跑完即删（AutoDbg 只接受脚本路径），临时路径在输出里被换回 buffer 名（quickfix 仍可用）；默认带 `--quiet`（免 headless 被 MessageBox 挂住）；flavor 由脚本自己的 `--!lua`/`--!luajit` 决定。
- 远程模式：`vim.system` + `curl`（非流式）或 `vim.uv` TCP（**流式，默认**）；结果落 scratch buffer，`<file>:<line>:` 进 quickfix；默认响应格式 **JSON**（D33）。
  ❗uv 回调是 **fast event context**：里面不能调 nvim API，`on_done` 一律 `vim.schedule`（`PITFALLS.md` N12）。
- 客户端只剩 raw TCP 帧（`nvim/lua/autodbg/tcp.lua`）：`open_file` 的读写都走帧；效果循环与 `http.lua` 已随 D41/D44 删除。
- **脚本头标记（modeline）**：`nvim/lua/autodbg/modeline.lua` 解析前/后 5 行里的
  `--!autodbg url=… token=… / host=… port=… / exe=… / arch=x86|x64`（也认 `-- autodbg:` / `-- adbg:`），
  **覆盖 `setup{}` 的默认值**；裸 `--!autodbg` 只做标记。
- **LuaLS 注解库（取代模块的 `generate_templete`）**：插件自带
  `nvim/addons/autodbg/library/*.lua`（与架构无关）+ `nvim/addons/autodbg/arch/{x86,x64}/context.lua`
  （**按架构**的 `Context`：x86 = 23 项，x64 = 60 项含 `Xmm0-15`/`R8-R15`，D40）。
  `nvim/lua/autodbg/lsp.lua` 在「当前 buffer 头部有标记」时把 `library/` + `arch/<arch>/`
  加进 lua_ls 的 `Lua.workspace.library` 并 notify 刷新；换架构时先摘另一个。
  ❗`workspace.library` 是 **client 级全局**（lua_ls 不支持按 buffer 挂库）→ 只能**懒启用**，
  一旦启用本会话内其它 Lua 文件也会看到这些注解；`setup{ lsp = { enable = false } }` 或
  `:AutoDbgLsp off` 可关掉（`PITFALLS.md` N5）。
- **LuaSnip 片段**：`nvim/lua/autodbg/snippets.lua`（**不硬依赖** LuaSnip，没有就安静跳过），
  `setup{ snippets = { enable = true } }` 时在 `FileType lua` 注册；触发词 `adbg`（远程）/ `adbgl`（本地）/ `adrun` /
  `adbp` / `adrunbp` / `adhw` / `adctx` / `admem` / `adstack` / `adexcept` / `adgui` / `adclick` / `adfile`。
  ❗LuaSnip 的 `t{"a","b"}` 是**多行**文本节点（元素之间自动换行）——写片段时把「续行」
  拆成独立元素会把一行拆成两行（`PITFALLS.md` N6）。

### 12.6 CLI 的机器可读输出（`--json` / `--out=<path>`，D50）

- 落点：launcher 解析选项 → `boot.json_mode` / `boot.json_out`（ABI **3**，§11.3）→ `main.cpp` 组装信封。
- 契约（一行，字段齐全、缺一不可）：`{"ok","exit","canceled","detached","pid","stdout","stderr","images","result"}`。
  - `--json`：stdout **只有那一行**；launcher 自己的诊断走 stderr，`--quiet` 下仍然只有那一行。
    `--out=<path>`：同一份 JSON 写文件（二进制、结尾 `\n`，不转 CRLF），stdout 留空；两者同时给 → 只写文件。
  - `exit` = 进程退出码（口径不变：0 / -1 / 2 / Lua 错误码），`--json` 不改退出码。
  - ❗顶层 `pid` 是**被调试进程**（未附加为 0），不是 AutoDbg 自己的 pid（`_autodbg_info().pid` / server JSON 里的 `pid` 是后者）。
  - `detached` 只在目标存活时为 true（真 detach、或 `on_wait_timeout` 的 keep-alive）；被 kill / Win95-98 降级成 kill → false。
  - `images` = 本次执行里落盘过的路径登记表（`open_file` 的写/追加模式、`gui.screenshot_to`），去重、上限 64，**空也要输出 `[]`**。
  - ❗**被调试进程的 stdout/stderr 不进信封**：JSON 模式下 `core::set_child_stdio_capture(true)` 让 `CreateProcess` 用 `STARTF_USESTDHANDLES` 把目标的 stdout/stderr 指到**本进程 stderr 的可继承副本**（stdin = NUL）。这是「那一行独占 stdout」的实现手段（否则 `cmd /c ping` 的输出会顶在它前面）；`stderr` 字段只装**本进程**的诊断。要看目标输出就重定向进程 stderr，或用 `on_output_debug_string`。不用匿名管道的理由（`--detach` 后没人排空 → 目标要么阻塞要么拿到 `ERROR_BROKEN_PIPE`）写在 `core/util.hpp`。
  - `result` = 脚本返回值（1 个 → 那个值，0 个 → `null`，多个 → 数组）；编不了（function / 无 `val` 的 userdata / 稀疏表 / NaN / 循环引用）→ `null`，`ok` 仍可 true。C 侧不写 JSON 解析器：用 `json_writer` 的 raw 片段 + 烘焙的 `jsonx.encode`（`state.cpp` 在 `lua_close` 前编）。
- ❗编码口径：那一行必须**纯 ASCII**。优先按 UTF-8 解码；非法 UTF-8 的高位字节串按 **CP936** 解（legacy 变体的执行字符集是 GBK）；解不出（函数取不到 / 分配失败）→ 逐字节 `\u00XX`（仍是合法 JSON，但码点 = 原字节）。文件头注释里写明这个假设与降级。
- `--json` / `--out` 与 `--server` **互斥**（同时给 → 启动器报错、退出码 2）；JSON 模式不弹文件对话框。
- 门禁：`tools/server_test/e2e_json.ps1`（见 §4）。

### 12.7 `--repl`（本地 REPL 与 `--repl --server`）

> 落点：`src/scripting/lua/repl.{hpp,cpp}` + 启动器解析 `--repl` → `boot.repl_mode`（ABI **5**，§11.3）。
> REPL **不建状态、不注册 API、不 `lua_close`**：它复用脚本路径同一个 `lua_State`（`create_state_with_libs()`），
> 所以 `dbg` / `gui` / `open_file` / `help` / 烘焙的 `dbglib` 全都现成可用（`repl.hpp:6-9`；门禁 `repl.state-shared` 就是这条）。

| 口径 | 现在的做法 | 代码 |
| --- | --- | --- |
| 启动器 | `--repl` **不需要脚本**（输入是 stdin）；与 `--json` / `--out` / `--remote` **互斥 → 退 2**；与 `--server` **可并存** | `launcher.cpp:975`（不需要脚本）、`927-938`、`634` |
| 输入 | 先 `ReadFile(GetStdHandle(STD_INPUT_HANDLE))`（**管道可跑，门禁靠它**），失败且是真控制台才回退 `ReadConsoleW`；等待按 **100 ms** 切片（满足 S13 的 ≤250 ms），片间 `guard_poll_control()` | `repl.cpp:96-107`、`122`、`48`、`84-88` |
| 求值 | `=` 简写 = `return expr`（只在 chunk 为空时生效）；本地多行续行的**判据是 `<eof>` 标记**（`near <eof>` / `near '<eof>'`），不是 `end of input` —— Lua 5.5 与 LuaJIT 都是这个文案（PITFALLS **S20**） | `repl.cpp:313-320`、`232-247` |
| EOF | 文件/重定向读到结尾是 `TRUE` + 0 字节；❗**管道写端关闭是 `FALSE` + `ERROR_BROKEN_PIPE`**（也认 `ERROR_HANDLE_EOF`）—— 只认「0 字节」会把正常 EOF 当读错误 | `repl.cpp:102-107` |
| 退出码 | = **最后一条命令的码**（0 / 2 / 3），干净的 EOF → 0、读输入失败 → -1；失败一律打 **stderr**、不 `exit()` | `repl.cpp:364-423`、`repl.hpp:21-22` |
| server 形态 | 一个帧 = **一个完整块**（`allow_continue = false` ⇒ 不完整输入直接是语法错 3）；`result` 帧的 `id` = 这一行的 Lua 错误码；同一连接内 `lua_State` 与引擎会话**跨命令存活**；非 `--repl` 的 server 一个字节不改 ⇒ **D43（跨执行无状态）没破** | `repl.cpp:477-486`、`tcp_server.cpp:499-501`、`88-91`、`639-644` |
| 中断 | **本地 REPL 不提供中断**（已定，不是 bug）：`dbg` 的 `on_wait_timeout` 有意不读控制台，所以本地 REPL 里长 `pump` 无法 Ctrl+C；`--server` 口径的 `control cancel` 仍生效。历史记录 / 箭头键 / Tab 补全明确不做 | `dbg_bindings.cpp:836`、`repl.hpp:23-26` |
| 退出安全网（D54） | `lua_close` **之前**查 registry，非空就往 **stderr** 报 `repl: N unclosed session(s) - detaching` 并逐个按归属收尾（有 `DebugActiveProcessStop` 就 detach、没有就 kill），**不改退出码**；与 `d:detach()` / `d:kill()` 共用同一对 C 助手 `end_session_by_detach` / `end_session_by_kill`，两条路径不会分叉 | `repl.cpp:297-305`、`dbg_bindings.cpp:2615-2651`、`1931` / `1958` |
| `V` | REPL 里**不注册** `V`：`create_state_with_libs(repl=true)` → `register_lua_api(L, repl)`，`registry.cpp:79` 跳过它 | `registry.cpp:79`、`repl.cpp:436`、`main.cpp:453` |

`ReadConsoleW` 的导入表全 w95 可 ⇒ **不需要 dynapi**（`plan_t7.md` §9 第 5 条）。门禁：`tools/server_test/e2e_repl.ps1`（11 条，86 / 86 legacy / 64）。

### 12.8 进程树（一个会话 = 一棵树，切片 C2b / D61）

> 落点：`src/core/process.cpp`（pid 集合 + 按 pid 的句柄缓存）+ `src/scripting/lua/dbg_bindings.cpp`（按 pid 的路由与进程对象）。
> 会话的键从「一个 pid」变成**一棵树**：根 pid + 调试期间出现的全部 `CREATE_PROCESS` 的 pid（一棵树只有一个
> 调试器 ⇒ 那些 pid 一定归本会话；`registry_find()` 按 `owns_pid(pid)`，`dbg_bindings.cpp:380-386`）。

| 事实 | 口径 | 代码 |
| --- | --- | --- |
| pid 集合 | `kMaxPids = 16`：满了 `add_pid` 直接忽略、`handle_for_pid` 会拒（多出来的 pid 既不进树也不给句柄） | `core.hpp:232`、`process.cpp:95` / `120` |
| 维护点（两处） | `begin()`：先 `close_pid_handles()` 清掉上一轮，再 `add_pid(根)`；`dispatch()` 的 `CREATE_PROCESS` 分支 `add_pid(事件 pid)` | `debug_loop.cpp:225-226`、`595` |
| 句柄 | 根（或 `pid == 0`）用 `proc().hProcess`（调试事件换来的，不进缓存）；其它走 `OpenProcess(VM_READ\|VM_WRITE\|VM_OPERATION\|QUERY_INFORMATION)`，9x 不认 `PROCESS_QUERY_INFORMATION` 时去掉它重试；成功后**缓存** | `process.cpp:118`、`129-135`、`138-144` |
| 关闭点（三处） | `EXIT_PROCESS`（`close_pid_handle`；pid 仍留在树里）、`run_shutdown` / 析构（`close_pid_handles`，连树一起作废）、缓存表满时立即关 | `debug_loop.cpp:681`、`debug_loop.cpp:122` + `process.cpp:166-175`、`process.cpp:147-149` |
| 内存原语 | `(DWORD pid, …)` 重载 + **旧签名转发给根**（`pid == 0` = 根）⇒ 29 个契约名与既有调用点零改动；内存搜索与区域遍历仍只认根进程 | `memory.cpp:12-17`、`66` / `104` / `125` / `185` |
| 断点补丁 | 按 `peinfo->dwProcessId` 选句柄（否则子进程的补丁打到根进程）；顺带修了 `LOAD_DLL` 从不填 `dwProcessId` 的旧缺陷（`create_front` 给 0） | `breakpoints.cpp:104` / `141` / `156`、`debug_loop.cpp:724-727` |
| 对外面 | `d:procs()`（pid 数组、插入序、根在第一个）、`d:proc(pid)` → **普通表**（`pid` + `read*` / `write` / `v` / `context` / `stack` / `mods`，方法是绑 `(会话, pid)` 的闭包，只收本树里的 pid）、`d:current_pid()`（回调里 = 事件 pid、顶层 = `root_pid`）、`d:threads()` 每项带 `pid`；**会话上的同名方法是根进程语义** | `dbg_bindings.cpp:2424-2429`、`2431-2448`、`2450-2465`、`2467-2482`、`2484-2498`、`1405-1408` |

两个已知限制（照 PITFALLS **S27** / **S28**，不是 bug）：

1. `d:arm` 在进程树里**只能打到第一个进程**：`BreakpointInfo.orignal_byte` 只有一个字节且布局不可动（§5 第 7 条），
   `apply_breakpoints_for_module` 遇到 `has_orignal_byte` 已置位就跳过（`breakpoints.cpp:127`）⇒ 子进程拿不到 `0xCC`
   （记录有、补丁打不上）。现在的做法：子进程用**它自己的** `int 3` / 异常 + `d:current_pid()` + `d:proc(pid)` 定位，
   命中后显式 `cont`；按 pid 的记录层（`plan_t7.md` §10 第 4 条）留给后续。
2. `DEBUG_EVENT` 不带 parent pid ⇒ 新树的第一个 `CREATE_PROCESS` 归「发起这次 pump 的会话」（`pump_once` 的 `hint`，
   `dbg_bindings.cpp:1629-1637`）；两个会话都在等时先 pump 的那个认领 ⇒ 一个执行里**串行**驱动。同一个 pid 的后续
   事件按 `owns_pid` 精确归属（`process.cpp:99-105`）。

门禁：`e2e_dbg.ps1` 的 `route.child-process` / `route.child-handle` / `d:current-pid` / `d:v-child`
（fixture `tests/fixtures/bin/x86_spawn.c`）。

## 13. 已定决策（不要再讨论/推翻；改之前先在 `PITFALLS.md` 查有没有相关坑）

> **D51–D58 是 T7 余下切片的已定方向**：D51 / D53 的**切片 A 已落地**（`dbg.detach`/`dbg.kill`/`dbg.caps`/`help`，
> `dbg.close` 已删）；**D55 / D57 / D58 的 C1a + C1b + C2a + sugar + D56 的 C-remote 已落地**（会话对象 + registry + 事件表 + `d:pump` 缓存优先 + 延迟武装 + 数据面 + 多会话路由 + `dbglib` + 跨位数代理），**C2b（进程树 + 按 pid 的句柄 + 进程对象，D61 / §12.8）也已落地**；**C3（29 名转薄转发 + `Var` 退出新面）按本轮拍板不做** —— T7 的代码切片到此收尾，剩余是文档与 nvim 尾巴。进度以 `plan_t7.md` §11.7–§12 为准。

| # | 议题 | 决策 |
| --- | --- | --- |
| D1 | `98` 架构去留 | **退役**：legacy 变体取代；`make.bat` 只留 64/86；`a 98` 明确报错 |
| D2 | `tools/pack` 风格 | **严格项目风格**：零 CRT + `/nodefaultlib` + 无 STL（实现时因 `common::alloc` 是 C++ 接口而落成 `.cpp`） |
| D3 | 载荷嵌入方式 | **C 数组 `.c`**（不引入资源节 / 链接后 OVERLAY） |
| D4 | 交付形态 | **单文件 exe + 内嵌模块**（2 个 exe 覆盖 6 种旧产物） |
| D5 | 「从磁盘加载同名 DLL」的开发开关 | **带**（`--module-dir=<dir>`，仅调试用，仍走内存映射） |
| D6 | Lua / LuaJIT 也做成 DLL、由启动器选 | **采纳**：单 exe 内装 `flavor × variant` 全部模块；默认 Lua 5.5 |
| D7 | 无参时的文件对话框归属 | **迁到启动器**（`GetOpenFileNameW` 走 dynapi，9x 降级 A 版） |
| D8 | exe 命名 | **`AutoDbg_86.exe` / `AutoDbg_64.exe`** |
| D9 | 未知 `--!xxx` 标记 | **报错退出**（typo 可见） |
| D10 | flavor 优先级 | **CLI `--flavor` > 脚本标记 > 字节码 magic > 默认 lua** |
| D11 | 载荷 magic / 版本号 | **都不要**（同一次构建产出）；自检降级为结构边界校验 |
| D12 | 子系统版本 `4.0` 的实现 | **链接后用 `tools/pever` 改 PE 头**（link 选项被忽略），仅 x86 产物 |
| D13 | `lua_/luajit_<arch>.exe` 的 `_86`/`_98` 合并 | **合并**：保留旧 `_98` 的编译开关、改名 `_86`，每 flavor 每架构只剩一份 |
| D14 | 载荷流形式 | **单流**（四模块同质，独立流多付 2.06× 体积） |
| D15 | golden 行尾与比较口径 | golden **统一 LF**；比较按「CRLF→LF + 去末尾空行」归一化；产物输出行为不改 |
| D16 | x86 编译器辅助符号 | **不自己写 C 版**：一律用 `src/sys/flt.asm`；x86 产物与 x86 构建期工具都链 `flt.obj` |
| D17 | 映射器类名 | **`mapped_module`**（`module` 是 C++20 关键字） |
| D18 | legacy 变体的 charset | **独立 rule** `ccv9x`/`cxxv9x`（`/utf-8` 与 `/execution-charset:gbk` 互斥） |
| D19 | LuaJIT 的异常展开 | **仅 x86 定义 `/DLUAJIT_NO_UNWIND`**；x64 保持默认 SEH（`.pdata` 由映射器注册） |
| D20 | server 传输形态 | **HTTP/1.1 子集**（手写零依赖）；`net` 层独立，预留 `--transport=raw`（v1 不实现） |
| D21 | server 认证默认 | **loopback 免 token**；非 loopback 强制 token；`--server-token=none` 在非 loopback 拒绝启动 |
| D22 | server 执行语义 | **无状态**：一请求 = 建 `lua_State` + `DebuggerSession` → 执行 → 全释放（持久会话只给 MCP） |
| D23 | GUI API 形态 | 单一全局表 **`gui`**（不平铺 25+ 个全局名） |
| D24 | 截图格式 | **PNG（stored deflate，自写编码器）** 为默认；`raw`(BGRA)/`bmp` 可选 |
| D25 | 请求选项载体 | 查询串 + 少量头部；C 侧**不写 JSON 解析器**（JSON 请求体直通 Lua 的 `jsonx`） |
| D26 | 脚本传输 | 请求体 = Lua 源码，`luaL_loadbuffer(..., "@<文件名>")`；**全程不落盘**（模块也不再生成 `cdebugger.d.lua` / `.luarc.json`，改由 nvim 插件的 LuaLS 注解库提供） |
| D27 | server 归属 | server 跑在**模块里**；启动器只解析选项；`boot` 末尾追加字段 + `AUTODBG_ABI_VERSION` 1→2 |
| D28 | MCP 长路 | 传输/流式在 C，**JSON-RPC 分发在 Lua**（`jsonx` 已内置 encode+decode） |
| D29 | 命令行 client | **不做专用 `adbgcli`**：`curl` / `Invoke-WebRequest` / nvim 插件就是 client |
| D30 | `print` 覆写时机 | server 起来就覆写（否则脚本最常见的输出进不了响应） |
| D31 | `/shutdown` 时机 | 与 server 同期实现（自动化 e2e 需要可控停机） |
| D32 | 取消的两条注入路径 | 引擎阻塞 → `on_wait_timeout`（100 ms）；纯 Lua 死循环 → `lua_sethook`（100 万条指令）；共用同一份守卫状态 |
| D33 | nvim 插件默认响应格式 | **JSON**（要渲染 `exit`/`canceled`/`timed_out`/`truncated`）；`format="text"` 仍可配 |
| D34 | 文本注入策略 + 焦点 | 可打印 ASCII 走「VK + Shift」；非 ASCII 才用 `KEYEVENTF_UNICODE`；另有 `gui.focus(hwnd)` |
| D35 | MCP 会话契约 | 会话只经 **`Mcp-Session-Id` 头**表达；**不做** REST 的 `/session` CRUD |
| D36 | SSE/流式形态 | `Accept: text/event-stream` → `chunked` + **单事件**；增量推帧留待后续（已随 HTTP 作废，见 `plan.md` §3） |
| D37 | client 文件 I/O 机制 | **协程挂起 + 效果协议**（`read_file`/`write_file` → `yield` 效果表 → 客户端本地执行 → `POST /effects` 恢复）；不重放脚本、不碰客户端磁盘 |
| D38 | 挂起/恢复的落地细节 | ①`/run` 也跑在协程里，完成即关、挂起则留；②MCP 用 `call_yielding` 让 yield 穿透 pcall；③`lualib/effects.lua` 顶层不能取 `jsonx`（按文件名烘焙，effects 先于 jsonx） |
| D39 | nvim 插件的运行模式 | **本地优先**：标记 `exe=` > `setup.exe` > PATH 里的 `AutoDbg_{64,86}.exe`，拿到就跑本地；拿不到（或标记的 exe 不存在）→ **回落远程**（用 `url`/`host`/`port`）。`mode = "remote"` 强制远程，`mode = "local"` 则必须本地（找不到报错，不静默回落） |
| D40 | 架构决定 LSP 注解 | `Context` 按架构拆成两份：x86 = 23 项、x64 = 60 项（含 `Xmm0-15` / `R8-R15` / `P1Home…`）；`lsp.lua` 只挂当前架构那一份（标记 `arch=` > `exe` 文件名 > `setup.lsp.arch` > 缺省 `x64`），切换时先摘另一个 |
| D41 | 对外主契约 | **CLI**；MCP 与 HTTP 代码整体删除（落地分支 `feat/cli-tcp`） |
| D42 | 传输形态 | raw TCP 帧（单端口、握手帧带版本）；**不做 HTTP 嗅探、不留残留报错**；`--health` / `--version` 改由 CLI 经 TCP 提供（D20 的预留位变成唯一形态） |
| D43 | 会话 | **每次调用无状态**：会话表 / `Mcp-Session-Id`（D35 作废）/ ≤8 上限 / 30 min 空闲回收 / 503 全删；跨调用唯一的状态是**被调试进程本身**，靠 pid 显式指认 |
| D44 | 文件 I/O | 只有 `open_file` 文件对象（io 风格 API）；`read_file` / `write_file` + 效果协议（D37）+ `base64` + 协程执行器一起删；`jsonx` 保留（`--json` 的 `result` 要在 Lua 侧 encode） |
| D45 | 回调里的文件读 | `f:read()` **可以阻塞**等回包；超时 → `nil, "timeout"`、回调返回、断点继续；取消立即生效（被取消请求的 stale 应答必须丢弃 + `cancel_notify`）；默认 buffer 4096 |
| D46 | 控制三档 | `cancel`（硬，`lua_sethook`）/ `kill`（软，`on_wait_timeout`→1）/ `detach`（软，→2）；等价 local mode 的 `q` / `Shift+Q`；执行串行 ⇒ 控制命令不需要 id，直接作用于「当前那次执行」 |
| D47 | `--detach` 无条件 | NT+ 走 `DebugActiveProcessStop`——**即使目标是我们 `CreateProcess` 起的也是真 detach**，pid 不变、可用于下一次 `Attach`；Win95/98 没有该导出 → 降级 kill，由 `ack` 帧报 reason；`WaitTimeoutAction` 枚举值与 `DebugCallbacks` 布局不动，`can_detach_without_term` 从「许可」改成「能力上报」 |
| D48 | pid 解析链 | 显式（`--pid=` / 握手帧 / 头标记 `pid=`）> `gui.pick()`（**在目标机上**解析：`GetCursorPos` → `WindowFromPoint` → `GetWindowThreadProcessId`，无交互桌面降级 `gui.foreground_pid()`，取不到就报错）> 脚本字面量；**禁止隐形状态**（「记住的 pid」只存在脚本头标记里） |
| D49 | `open_file` buffer 与字节 | 默认 4096；`f:tell()` 是逻辑位置（client 位置 − 未消费 buffer）；`f:seek()` 是往返且清空 buffer；帧内字节一律精确（不再做 CRLF 归一） |
| D50 | `--json` 契约 | `{"ok","exit","canceled","detached","pid","stdout","stderr","images","result"}`；`result` 暴露脚本返回值；`--out=<path>` 写同一份 JSON；legacy 变体一律 UTF-8 + `\uXXXX` |
| D51 | `dbg` 会话的收尾 | **拆成 `d:detach()` / `d:kill()`**（`dbg.detach(pid)` / `dbg.kill(pid)` 同义，按 pid 操作），删掉 `dbg.close()`；**9x 上 `dbg.detach` 根本不存在**（建表时 dynapi 探 `kernel32!DebugActiveProcessStop`，探不到就不装 key）——能力缺席，不是静默降级；`--detach` 的 CLI 降级仍按 D47 用 `ack.reason` 显式上报。detach 前必须先 `ContinueDebugEvent` 应答未决事件 |
| D52 | 命名口径 | **新面一律小写**（`help`、`dbg.*`、`open_file`、`gui.*`、`Var` 的方法）；**29 个 PascalCase 契约名冻结**（不改名/不删/不加新参数，新文档与示例一律用对象 API），退役留给后续 D 条目；**`V`/`U32`/`I32`/`U64`/`I64`/`F64` 不出现在新面上**（§9.9 的 LuaVariant 算术坑），地址类用 LuaVariant、小整数用普通整数（**D59 细化**）、宽度靠函数名（`d:read_u32`） |
| D53 | 帮助与能力探测的分工 | **三方分工、互不包含**：语义/契约 → skill markdown（唯一 AI 契约文档，`check_skill` 门禁）；「这台机器上有什么」→ `dbg.caps()` 固定形状的普通表（按表成员生成，不引入第二真相源）；「现在敲什么」→ `help([topic])` 人类散文。`help` **只在真控制台才上色**（`GetConsoleMode` 探测 + `SetConsoleTextAttribute`，禁 ANSI/9x 不存在的 VT），落点是 core 的 `print_styled`；尊重 `--quiet` / `NO_COLOR`；`--json` 下走 stderr 且不上色。AI 的权威判据是 `dbg.<name> ~= nil`，`inspect(dbg)` 给人看、明确标注「机器别解析」 |
| D54 | session 的归属与 GC | **`__gc` 不做任何副作用**（不 detach/kill：任意时机触发 + 不可门禁）；session 进 **C 侧 registry，作用域 = 一次执行**（执行结束清空 ⇒ D43 不破，跨调用仍无状态）；`dbg.sessions()` / `dbg.session(pid)` / `dbg.last()` / `dbg.detach(pid)` / `dbg.kill(pid)`；REPL 退出时打印未关闭的会话并逐个收尾。容器先用**侵入式链表线性扫**（个位数会话），**不上 RBTree**（零 CRT 下手写红黑树收益为零；真到几十个会话要的是哈希） |
| D55 | `dbg` 对象化（**不做 Stop 对象**） | 照 `open_file` 的文件对象：`dbg.create{...}` / `dbg.attach(pid[,opts])` 返回 **Session**；`d:pump()` 返回**普通事件表**（无 metatable），同一张表缓存成 `d.stop`（nil = 没有未应答事件）。规则：① `d.root_pid`（树根）与 `e.pid`（事件真实进程）必须分开命名；② `e.code`（异常码）与 `e.cont_code`（core 算好的默认应答码）是两个字段，因此 **`dbg.code()` 取消**；③ 无「代次」——`pump` 返回的就是 `d.stop` 那张表，判「还是不是同一个」用表身份 `e == d.stop`，`d:cont()` 永远应答当前那个；④ `pump` 返回 nil **不清** `d.stop`，`d:cont()` 之后才清；`hits()` 是历史窗口的**副本**；⑤ 缺省 tid 只是便利，看别的线程必须显式 `d:context(tid)` / `d:stack(tid)`（**不要假设其它线程没意义**）；⑥ `d:context([tid[,t]])` 带表为**部分更新**，setter 必须返回 `true` / `nil, err`。事件表与 `hits()` 记录、协议 `event` 帧**同形**（可直接 `jsonx.encode`） |
| D56 | 多 session 的形态 | **单进程 + 进程树路由是主line**（§10 / 切片 C）。**不为「绕开 `WaitForDebugEvent`」做每 session 一进程**——它不消除异步协调，只把「一个事件流的分派」换成「两个在途 RPC」。跨位数/强隔离用**远端 session 代理** `dbg.remote(host:port, token)`（方法面与本地 session 完全相同，复用 `--server` 帧 + `client.cpp`）：**位数是唯一无法绕过的硬理由**（32 位调试器附 64 位目标 = ERROR_NOT_SUPPORTED；x64 模块的 `parse_pe` 按编译期 `_WIN64` 判位数），其余收益（崩溃隔离、一条连接=一个 session 的天然归因）代理都能拿到，而单进程的 `context()` 零 IPC 与状态同源是实打实的好处 |
| D57 | pump 的两层与「停住」 | `dbg.pump([timeout])`（模块级、**进程级**）= 读一条事件 -> **按归属分派**（记录 + 回调 + 引擎记账）-> 返回带 `ev.session` 的事件表；`d:pump([timeout])` = **缓存优先**（`d.stop` 非空立即返回）+ 过滤（别人的照常分派，只是不返回）。**分派不能推迟**（`cont` 必须作用在已 EIP--/已 re-arm/已记录的事件上），所以 §10.5 的「不跑 B 的 callback」收紧为「不**返回**别人的事件」。**停住 = `d.stop` 非空**，不引入 flag；每个 session 最多一个未应答事件（OS 挂起该进程线程）=> 缓存深度 1，存一个原始 `DEBUG_EVENT` 即可。`d:bp` **允许延迟武装**（模块未加载只登记，`CREATE_PROCESS`/`LOAD_DLL` 时补 `0xCC`），「始终没加载」改在收尾/`bps()` 报告 |
| D58 | 多 session 的脚本糖 | 三种模型别混：**batch** `dbglib.run_all{...}`（并行跑到结束 + 每 session 结果表；autocont => **不能**在命中处检查）、**rendezvous** `dbglib.wait_all(pred, timeout)`（跑在 `dbg.pump` + `paused()` 上）、**step**（REPL 原语）。名字不用 `run`（与契约名 `Run` 撞概念）。共享状态：**脚本里用 upvalue 表**（全局会撞 29 个契约名与烘焙的 `inspect/jsonx/moses`，且数据流看不见），**REPL 里用全局**（跨行存活） |
| D59 | 数字类型口径（新面） | **地址 / 指针 / 指针宽的寄存器值 ⇒ LuaVariant**（`LV_UZ`；`d:read_u64` 用 `LV_U64`，因为 x86 上 `LV_UZ` 是 4 字节）：`ev.ea`、`hits().ea`、`mods().base`、`stack().ea`、`context()` 里 `ctx_kind::UPTR` 的字段、`eip()`、`read_u64`。**小整数 ⇒ 普通 number**：`pid/tid/type/code/cont_code/rva/hits/dropped/size/index`、`read_u8` / `read_u16` / `read_u32`、`context()` 的 `U32/U64/M128` 字段（`EFlags`/`Seg*`/`MxCsr`/`VectorControl`/`DebugControl`/`Xmm*`）。**`d:arm` 的 rva 与 `ev.rva` 仍是 number**（D57）。入参地址 number 与 LuaVariant **都收**（`arg_num_or_lv`），但 `lv_check` 本身不动（29 名依赖严格语义）。理由：① 与 29 名天然互通（`ReadMemory(ev.ea)` 直接可用）；② `jsonx` 的 userdata 分支吐 `x.val`（十进制精确）⇒ `--json` 的 `result` 对 x64 地址不再丢低位；③ `dbg.remote` 的 `{"__i":…}` 退役、改 `{"__u64":…}`。❗跨类型/跨宽度比较不要赌 `__eq`（`U32(5) == 5` 为 false），比 `.val` 或 `tostring` |
| D60 | `V` 的进程归属 | **`V()` 只在调试回调上下文可用**（回调派发处用 `CallbackScope` 设 `cb_session`/`cb_pid`，`V`/`Var` 解引用取它；顶层与 REPL 报固定文案 `"V() is only usable inside a debug callback; use d:v(addr) or d:read*/d:write instead"`，**不猜 `current_session()`**）；**REPL 里根本不注册 `V`**（`create_state_with_libs(repl=true)` 时跳过）；顶层读内存用 **`d:v(addr)`** —— 会话绑定的 Var（载荷 `{addr, sess}`，解引用全走该会话的引擎，会话被收尾后报 `Var: the session bound by d:v() is closed`）。**`d:context()` 与契约名 `GetContext` 对齐：所有字段都是 LuaVariant**（`U32→LV_U32`、`U64→LV_U64`、`UPTR→LV_UZ`、`Xmm→{Low,High}` 的两个 variant），setter 仍接受 number 与 variant。**进程树的按 pid 句柄已由 C2b 实现**（`Var` 载荷是 `{base, sess, pid}`：`V()` 的 pid = 0 ⇒ 解引用时取回调事件那个进程，`d:v` 绑根、`p:v` 绑 `d:proc(pid)` 那个进程；`bindings.cpp:1967-1972` / `var_pid` `bindings.cpp:1996-2001` / `handle_for_pid` `process.cpp:116-151`）；`dbg.remote` 的白名单不含 `v` / `proc`（代理上不可用） |
| D61 | 进程树与进程对象 | 一个会话的键是**一棵进程树**（根 pid + 调试期间出现的全部 `CREATE_PROCESS` 的 pid，`owns_pid()`）；内存/断点补丁必须走**按 pid 的句柄**（`handle_for_pid()`：根用 `proc().hProcess`，其它 `OpenProcess` + 9x 降级、缓存并在进程退出/`run_shutdown` 关闭）——否则子进程的补丁会写到根进程。内存原语加 `(DWORD pid, …)` 重载，旧签名转发给根（29 名零改动）。对外：`d:procs()`（插入序、根在前）、`d:proc(pid)` → **普通表**（`pid` + `read*/write/v/context/stack/mods`，方法 upvalue 绑 (会话, pid)）、`d:current_pid()`（回调里 = 事件 pid、顶层 = `root_pid`）、`d:threads()` 每项加 `pid`。**两个已知限制**：① `d:arm` 在进程树里**只能打到第一个进程**（`BreakpointInfo.orignal_byte` 是单字节且布局不可动 ⇒ 子进程拿不到 `0xCC`；按 §10 第 4 条要落到新记录层才行）；② `DEBUG_EVENT` 不带 parent pid ⇒ 新树的第一个 `CREATE_PROCESS` 归给「发起这次 pump 的会话」（`pump_once` 的 hint），多会话同时派生时是「谁在等就归谁」 |