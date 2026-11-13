# PITFALLS.md —— 已踩过的坑（按主题）

> 用途：**只记录「已经踩过、已经修好或已经定性」的坑**，供改代码前速查。
> 约定：每条给「现象 → 根因 → 现在的做法」。改代码前先扫一遍对应小节，能省掉绝大多数「改了没生效 / 静默算错 / 进程直接没了」的调试时间。
> 相关文档：`AGENTS.md`（工作约定与架构）、`plan.md`（剩余工作：Win9x 矩阵、server/GUI 实测、CLI 尾巴——原三份 plan 的合并版）。
> 编号只用于互相引用（`PITFALLS.md` 的 `B3` / `M2` 等），不表示优先级。

---

## B. 构建与构建脚本（ninja / make.bat）

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| B1 | 改了头文件，`ninja` 报 `no work to do`，只能全量重建 | rule 里有 `deps = msvc` 但 command 里**没有 `/showIncludes`**，`.ninja_deps` 里 `#deps 0` | `cc`/`ccx`/`cxx`/`ccv9x`/`cxxv9x` 五条 rule 全部带 `/showIncludes`（两件套缺一不可） |
| B2 | 补了 `/showIncludes` 仍不记录依赖，终端里多出一堆「注意: 包含文件:」 | `build.ninja` 里的 `msvc_deps_prefix` 与 cl 实际输出的**字节**不一致；`VSLANG=1033` **改不了**这个前缀（只改诊断语言） | `msvc_deps_prefix` 按 CP936 的中文前缀写；**生成 `build.ninja` 时必须先 `chcp 936`**（UTF-8 代码页下 echo 出来的是 `EF BF BD`，前缀对不上） |
| B3 | 并行构建时 `pever.exe` / `pack.exe` / `minilua.exe` 报 `is not recognized as an internal or external command` | 构建期工具没挂在消费它的边的**隐式依赖**（`\|`）上 | 所有用工具的边都写 `^\| %BUILDDIR%\tools\xxx.exe`（AGENTS.md §10.5） |
| B4 | 链接行里的 `&&` 与后续路径被当成 link 的参数（`LNK1149 输出文件名匹配输入文件名`） | **ninja 不经过 cmd.exe**，`rule` 的 command 直接 exec | 串联命令的 rule 必须自己以 `cmd /c` 开头（`cp`/`as`/`imp`/`t`/`pever` 都这么做） |
| B5 | `ninja` 报 `expected '=', got newline`（生成文件里多了一行 `.`） | make.bat 里写空行写成 `echo .`（`echo` 后带空格 + 点） | 空行一律 `echo.` |
| B6 | 往 `make.bat` 里写了 `❗`/emoji，构建时变成 `?` 或乱码 | `make.bat` 是 **GBK(CP936)**，GBK 编不出这些符号 | 批处理注释/usage 只用中文标点，不写 emoji；改写 `make.bat` 必须按 GBK 语义（不要用 UTF-8 工具整文件重写） |
| B7 | 任何 `.pdb`/头依赖相关的「改了没生效」都先怀疑增量 | 见 B1/B2 的豁免情形：`genver` 的 `luajit.h` 比 `relver.txt` 旧会永久重跑 | `genver` 前先比较 `luajit_relver.txt` 内容，变了才写并删掉 `luajit.h` |
| B8 | `buildvm.c` 找不到 `buildvm_arch.h`（`C1083`） | 引号 include 只在「包含者所在目录」找，而该头生成在 build 目录 | `HOST_CFLAGS` 必须带 `/Iluajit\host` |
| B9 | LuaJIT 的 `luajit.lib` 一片 `C4819` 与 `time_t` 不匹配 | 影子头是 UTF-8、x86 影子 `time.h` 的 `time_t` 是 `long` | `LJ_CFLAGS` 带 `%CHARSET_FLAGS%` 与 `%TIME_T_FLAGS%`、`/I%SRCDIR%\sys\include`、`/Iluajit`、`/Iluajit\include`（顺序有意义） |
| B10 | `make.bat a 32` 之类的未知架构被**静默当 x86** 构建 | 旧 `:config` 只判 `x64`，其余全落 x86 分支 | 显式白名单：只认 `64` / `86`；`98` 报「已退役」；其余报错退出 1 |
| B11 | `.out` 里出现 `AutoDbg_*.lib` / `*.exp` 噪音 | DLL 链接默认产出导入库与导出文件，而模块只被手工映射、从不被 import | DLL 链接加 `/noimplib /noexp`（导出表本身不受影响，`resolve("autodbg_run")` 照常工作） |
| B12 | 加了个「构建期生成的版本头」后，每次构建都全量重编 | 生成步骤无条件重写文件 → mtime 变 → 包含它的 TU 全部重编；或者反过来忘了把它放到 include 路径里（`#include` 找不到 → 默默跑“unknown”分支） | `:genversion` 先写 `*.tmp`、`fc /b` 比内容，**变了才 `move`**（同 `luajit_relver.txt` 的路子）；`<build>\gen` 必须同时进 `CFLAGS` 与 `AD_FLAGS` 的 `/I` |
| B13 | 把中文写进启动器打印的文本（`--help` / 错误信息）在控制台上是乱码 | 启动器走 `WriteFile` 写字节，控制台按 CP936 解码；它又是 ANSI 编译 | 启动器输出一律 **ASCII**（中文只走模块内的 `WriteConsoleW`）；构建期生成的 `.h` 注释也只写 ASCII（cmd 代码页不可控，进了 `/utf-8` 编译单元会报 C4819） |

## K. 零 CRT / 编译器辅助符号 / 结构偏移

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| K1 | LZMA 编码结果首字节错（`comp[0]=1`），自家解码器判 `SZ_ERROR_DATA`；解码一直是对的 | x86 的 `UInt64 >> 32`（变量移位、`/Oi-`）被编译成对**外部辅助符号 `__aullshr`** 的调用，其约定不是 cdecl（值在 `EDX:EAX`、位数在 `CL`、结果回 `EDX:EAX`、调用点是 `jmp` 尾调用）；用 cdecl 的 C 函数去补会静默算错 | **禁止在 C 里重写编译器辅助函数**：x86 产物与 x86 构建期工具一律链 `src\sys\flt.obj`（`__aullshr` / `__allshl` / `__allshr` / `__aullrem`…都在 `flt.asm` 里）（AGENTS.md §5 第 10 条 / D16） |
| K2 | 用测试用例验证 K1 这类辅助符号，测不出问题 | 常量移位（`a >> 32`）会被编译器**常量折叠**，根本不产生符号引用 | 验证这类符号必须用**变量**位移 / 变量操作数 |
| K3 | `pack.exe` 读进垃圾节表（`num_sections=27354`）后直接 `0xC0000005`，**没有任何输出** | `IMAGE_FILE_HEADER` 的偏移记错：`NumberOfSections` 在 **+2**、`SizeOfOptionalHeader` 在 **+16**（不是 +6/+20） | 所有用到的 PE 偏移都用 `offsetof` + `static_assert` 钉死（`pack.cpp` / `pever.cpp` / 映射器一致口径） |
| K4 | `pever` 打补丁后 `ImageVersion` 被写成 `4.00` | `ImageVersion` 夹在 OS 版本与子系统版本之间：OS = `opt+40/42`、**Image = `opt+44/46`**、子系统 = `opt+48/50`（`opt = e_lfanew + 24`） | 只改 40/42/48/50；每次改完用 `dumpbin /headers` 复核 ImageVersion 仍为 0.00 |
| K5 | `LzmaDecode` 一调就崩 | 函数入口无条件 `*status = LZMA_STATUS_NOT_SPECIFIED;`，传 `NULL` 必崩 | 直接调 `LzmaDecode`（为把字典指向镜像缓冲）时必须自己给一个 `ELzmaStatus`；走 `LzmaUncompress` 才安全 |
| K6 | 上游 `LzFind.c` 报 `ptrdiff_t` / `size_t` 未定义 | `7zTypes.h` 只 include `<stddef.h>`（不经过 `string.h`/`stdlib.h`） | 影子头 `src/sys/include/stddef.h` 里补齐 `size_t` / `ptrdiff_t` |
| K7 | C++ 里 `__stosb` 报未定义 | 需要 `<intrin.h>` | 本项目的用途只是清零 → 直接用 `memset` |
| K8 | 影子头下 C 编译报 `C2143`（`wchar_t` 不认） | 影子头里 `wchar_t` 由 `string.h` 提供，`stddef.h` 没有 | 需要 `wchar_t` 的头 include `<string.h>`（`cmdline.h` 就是这么修的） |
| K9 | `1.0/0.0` 编译报 `C2124` | MSVC 把它当编译期错误 | 测试里用 `volatile double zero` 挡住常量折叠 |
| K10 | `GetVersionExA` 报 `C4996` | 该 API 被标记废弃 | 用 `#pragma warning(push/disable/pop)` 局部包住（本项目确实需要它区分 9x） |
| K11 | `GetProcAddress` 取到的 Win32 API 一调就 `0xC0000005`（**只在 x86**，x64 正常） | 函数指针 typedef 没写 `WINAPI`（x86 上 Win32 导出是 **stdcall**）或**参数个数与真实 API 不一致** → 栈不平衡 | typedef 一律写成 `unsigned long (WINAPI *)(真实参数表)`，参数个数与 SDK 原型**逐个一致**（不能“多传一个没用到的”）；x64 只有一种调用约定，测不出来 |
| K12 | legacy（ANSI）变体 `Run()` 随机 `0xC0000005`（同一脚本有时能跑完，**只在 `--variant=legacy` 上出现**，modern/x64 正常） | 拷 TCHAR 路径用了 `__movsw`（`CopyMem2` 的语义）：它按 **2 字节** 一个单元拷，而 ANSI 构建的 `TCHAR` 是 1 字节，分配只有 `len * sizeof(TCHAR)` 字节 → 每拷一次就溢出半个缓冲，堆被写坏 | 按 `sizeof(TCHAR)` 选搬运指令：`#ifdef _UNICODE` → `__movsw(…, len)`，否则 `__movsb(…, len)`（`debug_loop.cpp` 的 CREATE_PROCESS / LOAD_DLL 两处）。这类 bug 在“长度恒为 0”的旧路径上不会发作，一旦上游开始返回真名字就立刻显形 |

## L. Lua / LuaJIT / lualib 烘焙

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| L1 | 换 flavor 后脚本报字节码/库错误 | `datas_<flavor>` 的字节码格式取决于**跑生成脚本的解释器** | `lua` 变体用本项目的 `lua_<arch>.exe`、`luajit` 变体用 `luajit_<arch>.exe`（`gendata` 的 `lua=` 变量） |
| L2 | `ninja` 报 `multiple rules generate datas_<flavor>` | `datas_<flavor>` 是 **flavor 级共享**产物，modern/legacy 都去生成 | 只有 modern 生成，legacy 复用 |
| L3 | 改了 `tools/gen_lua_lib.lua` 没重跑 | 生成边的隐式依赖里漏了**脚本本身** | 隐式依赖同时包含「解释器产物 + 生成脚本」 |
| L4 | `lualib/effects.lua` 里 `local json = jsonx` 取到 `nil` | 烘焙按**文件名**顺序注册全局，`effects.lua` 在 `jsonx.lua` 之前 → 模块加载时 jsonx 还不存在 | 顶层不取其他烘焙库，改成**调用时**查全局 |
| L5 | 脚本带 UTF-8 BOM 时 Lua 5.5 直接失败（exit=3） | 5.5 不跳 BOM；LuaJIT 能跳 | harness 的脚本一律**无 BOM**；server 模式接收脚本时先去 BOM |
| L6 | LuaJIT 下 `pcall` 里调 `read_file`/`write_file` 直接报错 | LuaJIT 的 `pcall` 不能跨 yield | 脚本里直接调用（不用 `pcall` 包），MCP 侧走 `call_yielding` 让 yield 穿透 |
| L7 | `lua_pcall` 返回后循环变量/寄存器莫名其妙变了 | 见 M5（x64 关 SEH 的副作用） | 同 M5 |
| L8 | LuaJIT 下 `runtime.version` 显示成 `Lua 5.5` | `#ifdef JIT` 判断写反了 | 用 `#ifndef JIT` 输出 5.5 版本串 |
| L9 | 回调里调 `write_file`/`read_file` 报 `attempt to yield across a C-call boundary`，随后整个调试会话/回调直接断掉 | 回调是引擎经 `lua_pcall` C 调用进来的，而 client 文件 I/O 靠 `lua_yield` 挂起——LuaJIT 不支持跨 C 边界 yield，Lua 5.5 也不允许跨普通 `lua_pcall` | **回调里写文件用 `open_file()`（流式，脚本线程直接写 `/run` 连接）**；`write_file`/`read_file` 仍然只能在非回调处用，且现在会返回可操作错误（靠 `helpers.cpp` 的 `callback_depth()`，不会打断会话） |
| L10 | 流式请求里调 `write_file` 得到“流式请求下请改用 open_file” | 流式响应的头/帧已经发出去了，中途再走 `/effects` 挂起就没法回到同一个响应 | 设计如此：流式请求禁用 effects（`file_io` 的 `yield_blocker`）；要混用就分两次请求，或去掉 `X-AutoDbg-Stream` 头 |
| L11 | 新写的 `lualib/*.lua` 里明明有内容，但烘焙后就是“加载不了”/语法错 | Lua 的**长字符串会嵌套截断**：外层用 `[[ … ]]` 时，正文里任何 `[[path]]` 会提前闭合外层 | 正文里可能含 `[[ ]]` 就改用 `[==[ … ]==]`（`lualib/autodbg_api.lua` 就是这个坑）；改完用 `lua_<arch>.exe tools/gen_lua_lib.lua x.c lualib/x.lua` 单独跑一遍验证 |
| L12 | 脚本里 `ReadMemory` / `GetContext` / `StackTrace` 失败后拿到**栈上的旧值**（甚至整个脚本卡住不返回） | 这些绑定函数在失败路径上 `return -1` —— `lua_CFunction` 的返回值是「回给 Lua 的结果个数」，**负数 = 未定义行为**（调用方会把 `L->top - nres` 算到栈外） | 统一改成 `lua_pushnil(L); lua_pushliteral(L, "<who> failed"); return 2;`（`bindings.cpp`，T5 之后共 10 处）；写新的绑定函数时**绝不**用负数返回值 |

## M. 手工映射的模块（`src/sys/pack/module_loader`）

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| M1 | 合法的 x86 产物被映射器拒绝 | 断言了 `raw_size <= vsize`；而 `/align:16` 会把 `SizeOfRawData` 补齐到 FileAlignment，最后一节常常 `raw > virtual` | 只校验两条：文件里有这块数据、镜像里放得下（`raw_size > vsize` 是正常的） |
| M2 | 类名 `module` 编译报 `C2059 语法错误:"module"` | C++20 起 `module` 是关键字（`module m;` 被当成模块声明） | 映射器类名用 **`mapped_module`**（D17） |
| M3 | 自己声明 `RtlAddFunctionTable` 报 `C2664` | `winnt.h` 已经声明了它（x64），再 `__declspec(dllimport)` 会冲突 | 直接调用，用 `PRUNTIME_FUNCTION` 转换 |
| M4 | 手工映射的 LuaJIT 模块一跑脚本就整进程死掉，退出码 `0xE24C4A02`，没有任何输出 | x86 的 LuaJIT 把 Lua 错误做成 **SEH 异常**（`RaiseException` + 每帧处理器 + `lj_vm_rtlunwind`），依赖「模块被 OS 加载器登记」；手工映射的镜像不在名单里 → 异常无人处理（全页 RWX 无效，说明不是页保护） | x86 编译开关加 `/DLUAJIT_NO_UNWIND`（走内部 unwind）；**只改开关不改源码**（D19） |
| M5 | x64 的 LuaJIT 模块「能跑但只注册了第一个 lualib 全局」（`inspect` 是 table，`jsonx`/`moses` 是 nil） | **x64 上 `LUAJIT_NO_UNWIND` 是错的**：`LJ_UNWIND_EXT=0` 让 C 侧（`lj_frame.h`）的帧/栈假设与 `vm_x64.dasc` 的 Windows 分支不一致，解释器返回时把调用方的 `r14`/`r15` 写坏 | **x64 绝不定义 `LUAJIT_NO_UNWIND`**：靠映射器 `RtlAddFunctionTable` 注册 `.pdata` 保住 SEH；`%LJ_UNWIND%` 只在 x86 非空，`dasm` 的 `-D NO_UNWIND` 与它联动 |
| M6 | 以为「补 `-D NO_UNWIND` 给 dynasm」能修 M5 | x64/WIN 下 `dynasm -D NO_UNWIND` 生成的头**逐字节不变**（没有对应分支） | 只能靠**不定义**它；不要试图从 asm 侧修 |
| M7 | `0xE24C4A02` 这类退出码看不出含义，也不知道该怀疑谁 | 高字节段是 `LJ_EXCODE = 0xE24C4A00`，低 8 位是 `errcode`（2 = `LUA_ERRRUN`） | 定位手法：探针 + VEH 抓异常码；再与 **`LoadLibrary` 加载同一个 DLL** 做对照（能跑 → 问题在我们的映射器，而不是 DLL 本身） |
| M8 | 负例（篡改导入函数名）判「回滚是否干净」时误报 | 首次失败会触发 CRT 堆提交与依赖库惰性初始化，单窗口比区域数会误报 | 判据用**已提交字节的双窗口增量**（各 50 轮）而不是单次区域数比较 |
| M9 | 回退链打印出的 DLL 名/符号名是乱码 | `unmap()` 之后那些字符串指向已释放的镜像 | `load_error` 内置 `text[64]`，`AUTODBG_PACK_FAIL_STR` 先把名字拷出来 |
| M10 | 手工映射模块链接报 `LNK2001/LNK2019 _tls_index` | **函数内**静态对象（尤其带非平凡构造/动态初始化）会被 MSVC 用线程安全初始化（`_Init_thread_*`）实现 → 引用静态 TLS，而映射器不分配 TLS 槽 | 改成**文件/命名空间作用域**的静态对象 + 惰性标志（模块是单线程执行，不需要函数内惰性初始化）。（`static args_holder s_args;` 与 `static send_input_fn fn = get_proc_as<...>()` 各踩过一次） |
| M11 | 想把参数/状态放进 `__declspec(thread)` | 映射器不分配 TLS 槽 | 硬禁止：线程局部状态一律显式传参或宿主侧 `TlsAlloc` 封装（AGENTS.md §11.5） |

## N. neovim 插件

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| N1 | 插件拼结果字符串时抛错，明明没有错误 | `vim.json.decode` 把 JSON `null` 解成 `vim.NIL`（userdata，**为真值**）→ `"error":null` 被当成「有错误」 | 解码后统一 `nn()` 归一化 + `ui` 层再加一层 `type(text)=="string"` 兜底 |
| N2 | 无头测试一旦脚本抛错就永久挂住 | `--headless` 不会自己退出 | `nvim_smoke.ps1` 用「`luafile` + `qa!`」两条 `-c` |
| N3 | 测试中断并报 `E95` | 多个用例复用了同一个 buffer 名（`nvim_buf_set_name` 不允许重名） | 每个用例用不同文件名（`ok.lua` / `syntax.lua` …），顺便验证 chunkname 传递 |
| N4 | 插件把脚本写到临时文件 | 早期实现走文件 | 脚本正文走 `curl --data-binary @-`（stdin），响应只用 `-D -` 解析，**全程不落盘** |
| N5 | 想让注解只对「当前这个 AutoDbg 脚本」生效，但 lua_ls 没有按 buffer 挂库的接口 | `Lua.workspace.library` 是 **client 级全局**的设置（不支持按文件/按 buffer） | 只能**懒启用**：检测到脚本头部的 `--!autodbg` 标记才把它加进 library（避免污染其它纯 Lua 项目）；代价是一旦启用，本会话内其它 Lua 文件也会看到这些注解。`setup{ lsp = { enable = false } }` / `:AutoDbgLsp off` 可关掉 |
| N6 | 片段展开后字符串被拆成两行（`imagepath = "` 与路径分家） | LuaSnip 的 `t{"a","b"}` 是**多行节点**：元素之间自动换行，`t` 结束后不会再加换行 | 想与后面的 insert 拼在同一行时，把续行写在**同一个** `t` 元素里（或用单字符串 + `\n`）；验证法是「把所有节点渲染成文本再 `load()`」（已进 `nvim_smoke`） |
| N7 | LuaLS 注解库挂上了，但 `Run` / `gui` / `jsonx` 仍报 undefined global | `---@meta` 文件的首行必须正好是 `---@meta`（带 BOM 或先写注释都会让 LuaLS 不当它是 meta 文件）；另外 addon 目录要含 `config.json` | 注解文件一律**无 BOM、首行 `---@meta`**；`nvim_smoke` 逐文件断言首行（`lsp.meta.*`） |
| N8 | 本地模式一跑就 `timed_out`（exit=124）且 stdout/stderr 全空 | `vim.system(...):wait(0)` 是「立即返回」（没跑完就是 `nil`），不是「无限等」 | 无限等要 `:wait()` 不带参；只有 `timeout > 0` 才传 `:wait(timeout)`，拿到 `nil` 时 `:kill()` 并记 `timed_out` |
| N9 | 脚本首行写 `--!autodbg …` 后，直接命令行跑 exe 报 `launcher: unknown script marker --!autodbg` | launcher 只认 `--!lua` / `--!luajit`，其它 `--!xxx` 一律当 typo 报错（D9） | launcher 把 `--!autodbg` / `--!adbg` 当**编辑器侧标记**跳过整行继续扫（找不到 flavor 就用默认）；未知名字仍报错（D9 不变） |
| N10 | 换架构后补全里出现了根本取不到的字段（x86 上给 `Xmm0` 之类） | `Context` 的字段集**按架构不同**（x86 23 项 / x64 60 项），而 lua_ls 会把 library 目录**递归扫完** | 注解拆成 `addons/autodbg/library/`（公共）+ `arch/{x86,x64}/context.lua`（二选一），`lsp.lua` 注册时只挂当前架构那份，切架构先摘另一个 |
| N11 | 插件里解帧/解包报 `attempt to call a nil value (field 'unpack')` | nvim 跑的是 **LuaJIT**，没有 5.3+ 的 `string.unpack` / 整数除法 `//` / 位运算符 | 帧头小端自己手写（`le16`/`le32` 用 `string.byte`）；插件代码里不用 5.3+ 语法（`PITFALLS` 同样适用于 snippets 与 modeline） |
| N12 | 流式跑完后报 `E5560: nvim_create_buf must not be called in a fast event context` | `vim.uv` 的读回调（`tcp:read_start` / timer）属于 **fast event context**，里面不能调任何 nvim API（buffer/window/qflist/notify 都会报） | `stream.lua` 的 `finish()` 一律用 `vim.schedule()` 调 `on_done`；handlers 里的 nvim API（如 `on_open` 失败提示）也自己 schedule；`nvim_smoke` 的 `async.*` 用例看护这条 |
| N13 | nvim 插件一 require 就报 `tcp.lua:306: ')' expected near '&'`（或 `attempt to call field 'pack'`） | neovim 的 Lua 是 **LuaJIT**：`string.pack`/`string.unpack` 是 5.3+，`& | << >>` **运算符本身**也是 5.3 语法（不只是「缺位运算元方法」） | 帧编解码一律用 `bit.band`/`bit.rshift` + `string.char`/`string.byte` 手写（见 `nvim/lua/autodbg/tcp.lua`）；写完先 `lua_64.exe -e "assert(loadfile(...))"` 或让 `nvim_smoke.ps1` 跑一遍 |
| N14 | 插件发出去的握手帧 server 收到了，但回包永远读不到（整条请求超时） | `handle:read_start()` 在 `connect` **成功之前**调 —— luv 会丢掉那个回调 | `read_start` 放到 connect 的回调里（`nvim/lua/autodbg/tcp.lua` 的 `handshake`）；另外 `done` 回调一律 `vim.schedule`，否则画 buffer/塞 quickfix 会 E5560 |

## O. 输出、日志与排障手法

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| O1 | 重定向到文件后 `Print*` / `Error*` 的输出**整段消失** | 它们走 `WriteConsoleA/W`；句柄不是控制台时 `WriteConsole` 直接失败 | `PrintExN` 已分流：`GetConsoleMode` 成功 → `WriteConsole`（控制台零乱码）；否则 `WriteFile`（UTF-8 无 BOM，行尾 `\n`）。ANSI(legacy) 变体一律 `WriteFile`，分流开关用 **`_UNICODE`**（不是 `AUTODBG_WIN9X`：后者只加在 luajit flavor 上） |
| O1b | 控制台里中文变乱码：`server: 配置错误 …` 显示成 `閰嶇疆閿欒 …`（ASCII 部分正常） | UNICODE 构建的**窄串是 UTF-8**（`/utf-8` 同时改了执行字符集），而控制台分支原来用 `WriteConsoleA` → 控制台按**代码页**（CP936）解释这些字节 | 控制台分支里先 `utf8_to_utf16_n()` 再 `WriteConsoleW`（代码页无关）；转不出来（非法 UTF-8）才退回按字节写。**UNICODE 构建里不要再出现 `WriteConsoleA`**；重定向路径不变（仍是 UTF-8 字节） |
| O2 | 重定向后 stdout/stderr 前缀与数字错位、出现乱码 | 前缀走 `out_err`（stderr）而数字走 `out_u32`（stdout） | 同一条输出必须走同一个流 |
| O3 | 用 PowerShell 的 `>` 抓输出后字节对不上 | `>` 会按 PowerShell 的编码重写 | 取**原始字节**：`Start-Process -RedirectStandardOutput/-RedirectStandardError`，或 `cmd /c "... > file 2>&1"` |
| O4 | 想在调试期看模块的 stderr 却什么都没有 | `WriteConsole` 在重定向/无控制台下不可见 | 用 PTY（`Start-Pty` 能抓到 `WriteConsole`），或探针**写文件**（绕开标准输出） |
| O5 | 怀疑「寄存器/栈被写坏」但看不出是哪一步 | 无异常可抓 | 用 `RtlCaptureContext` 抓调用前后的 callee-saved 快照对比（`r14`/`r15` 被写坏 = M5） |
| O6 | 输出 sink 一装上，别的线程的诊断也被收进当前请求的 `stdout` | `core::set_output_sink` 是**全局**的 | 非执行线程的诊断**直写 `WriteFile`**，不经 `core::Print*` |
| O7 | 模块凭空长大几十 KB | 大结构体数组（内嵌 `http_request` ≈2 KB、带默认成员初始化）以初值形态进 `.data`（`job g_jobs[32]` → +72 KB） | 队列/表只持**指针**，请求体堆上分配 |

## S. server 模式 / MCP / client 文件 I/O（❗T5/D41/D44 之后：MCP、HTTP、effects、base64 均已删除，S 节里涉及它们条目仅作历史备查）

> `feat/cli-tcp`（D41 / D44）落地后 **S1 / S5 / S8 / S10 / S11 作废**（它们绑在 HTTP 与效果协议上）。保留备查：这些是当时定位问题的第一手依据，删除后仍是「为什么会写成那样」的答案。S12 的教训——每个对外接口都要有门禁断言——在帧命令面上继续适用。

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| S1 | 所有第一个 HTTP 请求都变 400（curl 与 .NET 客户端都复现） | 首次 `read_head` 时缓冲是 `nullptr`，而解析器把「空输入」当非法 | 解析器把 `(buf==nullptr, len==0)` 判 `incomplete`，`read_head` 前先 `ensure(4096)`；selftest 补了回归断言 |
| S2 | 没有控制台的机器上，一进 `Run()` 就把被调试进程杀掉 | `CheckConsoleInput` 在 `GetNumberOfConsoleInputEvents` 失败时返回 1（= 分离并终止） | server 模式替换 `on_wait_timeout`：有远程执行时先问 `exec_guard`，否则才回落 `CheckConsoleInput` |
| S3 | `/cancel` 之后脚本 515 ms 才停，或纯 Lua 死循环根本停不下来 | 引擎阻塞在 `WaitForDebugEvent`（100 ms 粒度）；纯 Lua 循环没有插话点 | 两条注入路径：`on_wait_timeout`（引擎） + `lua_sethook` 计数钩子（100 万条指令一次），共用同一份守卫状态（D32） |
| S4 | `lua_resume` 的错误被当成成功（`error()` 之后响应还是 `ok:true`） | 协程执行路径的错误分支忘了把返回码写进 `result.code`，`code` 停留在 `loadbuffer` 的 0 | 所有错误分支都写 `out.result.code = rr`（S7 回归时 6 条 e2e + 1 条 nvim 断言同时变红才抓到） |
| S5 | 连续两个 `read_file` 时第二次挂起读不到效果表（`bad pending effect`） | 挂起点清理顺手 `luaL_unref` 了协程，而两轮用的是**同一个协程** | `session_set_pending` 只在 `co_ref` 变化时清；`/effects` 完成时才清 |
| S6 | 会话里第二次 `lua_pcall` 之后栈越来越长 / `error` 对象残留 | 错误对象执行后没弹出 | 执行后显式弹出错误对象（`exec_on_state` 收尾） |
| S7 | JSON 里的浮点数与源文本往返不一致 | `"%.6f"` 既丢精度又受 locale 影响 | `json_writer::number(double)` 走 Ryu（`common/dbl2str`），并有 `roundtrip-bits` 断言（`0.1`、`1/3`、`1e300`、denormal、`2^53+1`、`-0.0`） |
| S8 | `httptool`/`curl` 发大 body 前先卡 1 s | `curl` 对 >1 KB 的 body 默认发 `Expect: 100-continue`，我们没答 | 必须回 `HTTP/1.1 100 Continue` |
| S9 | 用 `ws2_32` 的 `htons`/`inet_addr` 后导入表里冒出静态导入 | 它们都是导出函数，直接调就是静态导入 | `net` 层自己实现字节序与点分十进制格式化，全部函数走 dynapi（`ws2_32` → `wsock32` 兜底） |
| S10 | `/mcp` 的 SSE 一直等到脚本跑完才回 | 响应写入器当前是 `begin/write/end` 一次性形态 | 已知限制：增量推帧未做（D36 后半已随 HTTP 一起作废，见 `plan.md` §3） |
| S11 | 启动横幅里拿到了 token，`curl -H 'X-AutoDbg-Token: <token>'` 仍回 `{"ok":false,"error":"unauthorized"}` | 在 **cmd.exe** 里粘了 bash 写法的**单引号**：cmd 不认 `'`，curl 收到的头名是 `'X-AutoDbg-Token`（带前导单引号）→ 服务端认为没带 token（附带 curl 报 `Could not resolve host: application`） | cmd.exe 用**双引号**（或 PowerShell / Git Bash）；服务端现在会在 stdout 打一行诊断（`server: 401 no-token-header path=/mcp` 或 `401 token-mismatch`），401 体里也加了 `hint` 字段 |
| S12 | MCP 的 `dbg_run` / `dbg_mem_read` / `dbg_breakpoint_set` **看上去存在但一发就报错** | ①参数形状写错：`Run`/`SetBreakpoint` 要**一个 table**（且断点必须带 `callback`），`ReadMemory` 的参数是 **LuaVariant**（`V()`/`U32()`），裸数字/裸字符串不行；②更根本：它们作用于 `current_session()`，而**只有 `Attach`/`Run` 的回调里**才有活着的目标 | 删除这三个工具（只留 `lua_eval` + `dbg_status` + `gui_*`），调试逻辑写在 `lua_eval` 的场景脚本里；每个 MCP 工具都要有 e2e 断言（`e2e_mcp.ps1` 现在校验工具数量/名字/参数错误），否则会静默烂成死工具 |
| S13 | 回调里 `f:read()` 阻塞等回包时，`cancel`（控制帧 / CLI `--cancel`）打不断它 | `lua_sethook` 只在 **Lua 指令边界**触发，阻塞在 C 调用里不会触发；`on_wait_timeout`（100 ms）也到不了，因为线程不在 `WaitForDebugEvent` 里 | 等待循环按 ≤250 ms 切片 `select`，每片检查 `exec_guard` 标志（D45）。另一半：被取消的请求可能已有 **stale 应答**——必须丢弃该帧并给 client 发 `cancel_notify`，否则协议串位；且执行连接的唯一 reader 只能是脚本线程，其它连接才是控制连接（由 accept 线程处理） |
| S14 | 读超时之后的**下一次** `f:read` 神秘地拿到了上一轮的旧数据（或位置乱跳） | 超时/取消只发了 `cancel_notify`（D45 规则 4）就结束；不守规矩的客户端仍会把那一轮的 `read_resp` 发回来，而它在同一个句柄上与下一次请求**长得一模一样** | 服务端自己兜底：句柄上记 `stale_replies`，超时/取消时 +1，下一次发 `read_req` **之前**非阻塞地把滞留的 `read_resp` 吃掉（`file_stream.cpp` 的 `remote_req`）。门禁：`e2e_tcp.ps1` 的 `callback.read-stale-discarded` |

## S（续）dbg 会话对象 / 事件表 / REPL（T7 切片 C–D）

> 编号接着上面的 S 节往下排（S14 已被 T4 的文件 I/O 占掉）。全部来自实测：`tools/server_test/e2e_dbg.ps1`、
> `e2e_tcp.ps1` 的 `pump.*` / `proto.event-frame`、`e2e_repl.ps1`；对外文档是 `tools/agent/autodbg-skill/`。

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| S15 | 在断点回调里调 `d:pump()` / `d:cont()` 想「先看一眼再放行」，拿到的是 `nil, "pump cannot be called from a callback"`；另有一处（`Run`/`Attach`）报 `cannot run while a dbg session exists (detach/kill it first)` | 回调是 `pump_once` 派发进来的（`g_in_callback` 置位），状态机重入会让同一个事件被应答两次；`Run`/`Attach` 与 `dbg` 都要独占**整个进程**的事件流，而 `dbg_busy()` 查的是 C 侧 registry 非空 | 回调里只**记录**（写 upvalue 表 / 打日志），推进与放行都回到外层 `dbg.pump` 循环；要 `Run`/`Attach` 先把会话 `detach`/`kill` 干净。文案是门禁逐字判据（`pump.reentry-rejected` / `run.refused-with-session`）；`dbg_error_paused()` 里的 `cannot run while an event is unhandled` 是同族的历史文案，现行 `Run`/`Attach` 走的是上面那条 registry 判据 |
| S16 | 以为 `d:pump()` 返回「事件码」，拿去 `if ev == 1 then` 判异常；或者空转一轮就以为事件丢了 | `pump` 返回的是**普通事件表**（`type` 是 `dwDebugEventCode`，还有 `pid/tid/ea/modname/rva/code/cont_code/session`）；`d:pump` **缓存优先**（`d.stop` 非空立刻返回同一张表，不阻塞），模块级 `dbg.pump` **不缓存** | 判事件看 `ev.type`、判「还是不是同一个」看表身份 `ev == d.stop`；「没等到」= `nil`（不是错误，也**不清** `d.stop`）。`pump.no-auto-continue` 里的「目标停着时第二泵是空转」正是模块级 `dbg.pump` 不缓存的表现：目标已被系统挂起，`WaitForDebugEvent` 只是超时 |
| S17 | `d:context().Eip` / `.Eax` 全是 `nil`，脚本静默算出 0 | 键名沿用旧 X-macro 的 `C` 前缀（`src/core/context.cpp` 的 `k_context_fields[]`，`GetContext` 与 `d:context` 同一张表）：**`Cax Cbx Ccx Cdx Csi Cdi Csp Cbp Cip`** + `EFlags`/`SegGs…`/`Dr0…`；x64 另加 `R8-R15`/`Xmm0-15`/`P1Home…P6Home` 等 | 一律用 `Cip`/`Cax`…（不是 `Eip`/`Eax`，也不是 `Rip`/`Rax`；x64 上 `Cip` 就是 `Rip`）。`d:context(tid, t)` 是**部分更新**：未知键按 `SetContext` 的既有口径忽略，写成功返回 `true`。❗停住时 `Cip` 是否等于 `ev.ea` 取决于事件（实测）：`d:arm` 的断点被**压回 EIP** ⇒ `Cip == ea`（且引擎已把原字节写回 —— `d:read` 读到的是原指令、不是 `CC`），靶子**自带的 `int 3`** 系统停在它之后 ⇒ `Cip == ea + 1` —— 别拿 `Cip` 反推断点地址 |
| S18 | `d:arm()` 打在本来就写着 `CC`（`int 3`）的地址上，同一个断点被反复命中，`hits()` 一直涨 | 那本来就是断点指令：引擎压回 EIP 后它仍是 `CC`，`re-arm` 与「恢复原字节」都没得做 | 不要对已有的 `int 3` 用 `d:arm`（`help('dbg.arm')` 里是明写的）；靶子里那种自带的 `int 3` 只能**显式** `d:cont(0x00010002)`（= `DBG_CONTINUE`，吞掉断点）/ `dbg.cont(nil, nil, 0x00010002)` 放行，坐标按缺省 `cont_code` 会走 `NOT_HANDLED` |
| S19 | 想用 `dbg.pump(ms, true)`（autocont）验证靶子的「8 轮命中 + `exit 42`」，拿到的却是 `0x4000001F` | autocont = record-and-continue：引擎在 `dispatch` 末尾就自己应答了，**第二轮**那个不由 core 管的 `int 3` 走 `NOT_HANDLED` ⇒ 进程被系统以 WOW64 断点状态终止 | 要拿靶子契约里的 `42` 与 8 次命中，必须 `dbg.setcap(2)` + 逐次 `dbg.pump(ms, false)` + 显式 `dbg.cont(nil, nil, 0x00010002)`（`pump.record-and-continue` 断言 `hits=2 dropped=7 exit=42`）；`autocont` 只适合「跑到结束/超时、结果靠回调收集」的 batch 档（`dbglib.run_all`） |
| S20 | REPL 多行输入不续行，或者管道喂完 stdin 之后 REPL 卡在「读错误」 | 续行判据不是 plan 里写的 `end of input`（实测 Lua 5.5 = `unexpected symbol near <eof>`、LuaJIT = `… near '<eof>'`）；而管道写端关闭时 `ReadFile` 返回的是 **FALSE + `ERROR_BROKEN_PIPE`**（不是 `TRUE` + 0 字节） | 判据取共同出现的 **`<eof>` 标记**（`repl.cpp` 的 `error_is_incomplete`），不认完整句子；EOF 认 `TRUE+0` 与 `FALSE+ERROR_BROKEN_PIPE`/`ERROR_HANDLE_EOF` 两种表象，否则 `printf … \| AutoDbg_86.exe --repl` 的第二次读会被当成读错误 |
| S21 | 拿 `ev.code` 去当 `ContinueDebugEvent` 的应答码，异常被「吃掉」或者本该吞的又交给程序 | 事件表有两个码：`code` = **异常码**（`EXCEPTION_*`，没有异常的事件是 0），`cont_code` = core 在 `pump` 末尾算好的**默认应答码**（`DBG_CONTINUE` = `0x00010002` = 65538 / `NOT_HANDLED` = `0x80010001` = 2147549185） | 应答一律 `d:cont()`（缺省 = `cont_code`）或 `ev.session:cont(ev.cont_code)`；`code` 只用来判「这是什么异常」。`cont_code` 之所以是**泵完**才回填的（`fill_cont_code`），是因为 core 在 `dispatch` 末尾才算得出它 |
| S22 | 想「探一下」`pcall(dbg.detach)` 看平台支不支持，结果目标被真 detach（或被 kill）掉了 | `dbg.detach` 在**没有** `DebugActiveProcessStop` 的平台（Win95/98）上**根本不安装**（建表时 dynapi 探一次，`g_has_detach`）—— 但它在 NT 上是**真函数**，`pcall` 试探 = 真收尾 | 判据是**成员存在性**：`dbg.detach ~= nil`（或机器可读的 `dbg.caps().detach`）；`d.detach` 同理（方法表按能力装）。要兼容 9x 就 `if d.detach then d:detach() else d:kill() end` |
| S23 | 把 x64 的指针/模块基址当**普通 number** 塞进 JSON，往返后低位变了（`0x7FF9067A5384` → `…5300`），`read(base,4)` 摸到错地址 | `jsonx.encode` 的**普通 number** 分支是 `%.14g`（14 位有效数字），x64 用户态地址的十进制有 **15 位** ⇒ 静默丢低位（实测 `jsonx.encode(140707532264324)` → `1.4070753226432e+014`，也就是 `…320`）。**❗但 LuaVariant 是例外**：`jsonx` 的 `userdata` 分支直接吐 `x.val`（C 侧预格式化的十进制片段），所以 `jsonx.encode(U64(140707532264324))` → `140707532264324` **精确**（`U64(x).val` 是字符串，`tostring` 才是 hex） | 传大整数：① 用 **`U64(addr)`** 走 LuaVariant（精确，首选）；② 或 `tostring(addr)` 传十进制字符串；③ 代理 `dbg.remote` 已对 `>= 1e14` 的普通 number 自动编成 `{"__i":"…"}` 并在对端还原（`dbglib.lua`）—— 那是兜底，不是首选 |

## W. Win9x 实机自动化（vmrun）与 GUI 自动化

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| W1 | 经 vmrun 跑 `command.com /c <命令>` **什么都不执行**（只返回 0）：`echo x>file`、`copy`、`mkdir` 全无效果 | guest 侧没有可用的命令执行通道 | 不依赖 guest 批处理：退出码直接取 `runProgramInGuest` 的返回值（实测会传真实程序退出码），工作目录用 guest 已存在的目录（默认 `C:\WINDOWS\TEMP`） |
| W2 | 想删除/覆盖 guest 里的同名文件时报「access rights」 | 上一次卡住的进程仍锁着它 | guest 侧用固定名（`w9x_exe.exe`），每轮先 `revertToSnapshot` |
| W3 | `revertToSnapshot` 之后 VM 直接关了 | 快照 `1` 是**关机态** | `revertToSnapshot` → `vmrun start <vmx> nogui` → 等 Tools 就绪 |
| W4 | guest 里的探针进程永远不退出（host 只能等超时） | 见 S2 的 `CheckConsoleInput`：no-events 时返回「继续等」 | 判定只看**结果文件内容**，退出码不可靠；每条结果写完立即 `flush`（`io` 是缓冲写，否则 host 取回空文件） |
| W5 | 卡住的 guest 进程从 host 杀不掉 | 没有 command.com | 只能 `revertToSnapshot`；卡住期间文件 op 仍可用、program op 永远挂 |
| W6 | 98 上 `luajit_86.exe` 报 `cannot create state: not enough memory`；`--flavor=luajit` 的 AutoDbg 则卡住不退出 | 已知失败（未定位） | Win9x 矩阵里这条记 **xfail**，只记录不修 |
| W7 | WinForms 靶子窗口从不出现 | `Application.Run()` 无参 = 消息循环立刻退出 | 必须 `Application.Run($form)` |
| W8 | 测试脚本的子进程以 `-File $null` 启动 | PowerShell 变量名**大小写不敏感**：`$Target`（路径）与 `$target`（进程对象）是同一个变量 | 改名区分（`$TargetScript`） |
| W9 | `SendInput` 返回成功但文本没进 WinForms 的 Edit | `KEYEVENTF_UNICODE` 产生的是 `VK_PACKET`，该类控件忽略它 | 可打印 ASCII 走「VK + Shift」路径，非 ASCII 才用 Unicode 注入（D34）；仍不响应时用 `gui.set_text`（已知观察，不设门禁） |
| W10 | `gui.children()` 拿到的控件没有 `text`，脚本报 `concatenate nil` | 早期只返回 `title` | `text` 作为 `title` 的别名一起提供（对控件而言窗口文本就是它的标签） |

## X. 其它门禁相关

| # | 现象 | 根因 | 现在的做法 |
| --- | --- | --- | --- |
| X1 | 打包时莫名多出一个系统依赖 | 引入新 DLL（如 `gdi32.dll`） | `tools/pack` 的导入审计**按设计**会挡住，白名单在 `tools/pack/pack.cpp`；这是它存在的意义，不是 bug |
| X2 | x86 产物导入表里冒出 XP+ 的符号（`K32*`、`dbghelp.dll`…） | 静态 `#include <Psapi.h>` 默认 `PSAPI_VERSION=2` 会映射成 Vista+ 的 `K32*` | ScyllaHide 侧按架构加 `/DPSAPI_VERSION=1`（仅 x86）；其余新 API 一律走 `common::dynapi`；改完跑 `scan_dir.py` |
| X3 | 逐字节比对 golden 时永远不相等 | 产物重定向输出 CRLF（msvcrt 文本模式），而 golden 是 LF | golden 统一 LF，比较走「CRLF→LF + 去末尾空行」归一化（D15） |
| X4 | x86 产物在 Win9x 上「需要更新版本的 Windows」 | `link /subsystem:console,4.0` 被忽略（`LNK4010`，产物仍是 6.0/5.1） | 只能链接后改 PE 头：`tools/pever`（`pever` rule，仅 x86，x64 保持 6.0） |
| X5 | 想列出网卡地址，顺手 `#include <iphlpapi.h>` / 链 `iphlpapi.lib` | Win95 没有 `iphlpapi.dll` → 静态导入 = legacy 模块映射硬失败；头文件还会拉进 `iptypes`/`winsock2` 的顺序坑 | `LoadLibraryA("iphlpapi.dll")` + `GetProcAddress("GetAdaptersInfo")`，结构体在 `net.cpp` 里**自定**（字段类型与 SDK 逐项对应，布局即一致；用不到的尾部 `time_t` 字段不写）；取不到就降级成 `gethostname`/`gethostbyname` |
| X6 | 想把 GBK(CP936) 字节转成 UTF-8（`--json` 的转义口径），顺手用 `common::dynapi` 取 `MultiByteToWideChar` / `WideCharToMultiByte` | dynapi **不会减少导入表**：`kernel32` 已经在链接里，这两个符号本来就是**既有静态导入**（`src/core/util.cpp`，w95 白名单内）；走 dynapi 只是把已批准的导入换成动态查找 + 多一层失败分支，门禁看到的白名单条目反而更多 | `src/common/json_writer.cpp` 直接用影子头已声明的这两个符号（`scan_dir.py` 确认零新增）；新增任何 CRT/系统符号前先跑 `tools/api_check/main.py <符号>` |
| X7 | `--json` 的 `stdout` / `stderr` 里混进 `\u0000`（那一行不再是干净的 JSON） | `nDebugStringLength`（ODS 事件）与 Lua 的 `n`（`lua_tolstring`）都是**不含** NUL 的长度，但旧代码照“含 NUL”用：`PrintN(ods_data, ods_size)` 把 0x00 一起写出；`nn = (int)n + 1` 再 `utf8_to_utf16_n` 把 NUL 当成一个宽字符（UNICODE 分支才可见） | `debug_loop.cpp` 的 ODS 回显先去掉结尾 NUL（并同步 `ev.size`，否则脚本里的 `ev.str` 会再污染一次）；`state.cpp` 两处 `utf8_to_utf16_n` 的入参**不加** `+1`。新增任何“带长度”的输出点前先确认这两个 API 的长度口径 |
| X8 | `e2e_json.ps1` 的 `images` 一直是 `[]`、被调试进程也看不到落盘 | here-string 里写成了 `open_file([[ 'X' ]])` —— Lua 长括号内的内容是**带引号**的 `'X'`，路径就坏了（`open_file` 返回 `nil`，而脚本里是 `if f then`，静默失败） | 长括号里直接嵌值（`[[IMGPATH]]`），不要在 `[[...]]` 里再包引号；写这种“静默失败”的用例时带上 `if not f then print(...)`，否则断言只能看到下游现象 |
| X9 | legacy 变体 `Run()` 里 CREATE_PROCESS 拿不到映像路径（`CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)` 报 **299** `ERROR_PARTIAL_COPY`），接着 `calc_crc32` 拿着空路径 `CreateFile("")` 失败、回调里的 `Detach()` 不执行 | Toolhelp 快照在“刚 `CreateProcess`、目标还停在加载器初始化”的瞬间不稳定（调试中的挂起进程尤其如此）；32 位宿主枚举 WOW64 目标还需要 `TH32CS_SNAPMODULE32` | `LjGetMappedFileNameA` 改成三档回退 + 3 次重试：① Toolhelp（带 `TH32CS_SNAPMODULE32`）② `psapi!GetMappedFileNameA`（`common::dynapi` 动态解析；Win95 没这个 DLL → 正常降级）③ 自己走目标的 `PEB → Ldr → InLoadOrderModuleList`（只要 `ReadProcessMemory` + 动态 `NtQueryInformationProcess`，零新增导入）。排在同一轮里谁先成谁返回 |
| X10 | `e2e_tcp.ps1` 里发空帧时报 `AddRange` 参数为 null | PowerShell 把**空数组**绑成「缺参」（`List.AddRange(@())` → collection 为 null） | 帧编码改用逐字节 `foreach + List.Add`；写这种自己编帧的脚本时不要用 `AddRange` 传可能为空的数组 |
| X11 | 门禁脚本里 `Run({cmdline='C:\Windows\…'})` 一律 `exit=3`（`invalid escape sequence`） | PowerShell **here-string 是字面的**：写进去的 `C:\Windows` 到 Lua 眼里就是非法转义（双引号字符串才会被 PS 处理） | 命令行走 Lua 长括号：`[[C:\Windows\System32\cmd.exe]] .. ' /c …'`；另外 `Recv-Frame` 的 `Stream.Read` 要 `try/catch`，否则读超时会把整个脚本炸掉而不是报 FAIL |
| X12 | legacy 变体在 Win95 上映射/加载失败，`api_check` 报 `kernel32.dll GetFileSizeEx` | `GetFileSizeEx` 是 NT4/98 才有的（Win95 没有）；写代码时习惯性用它读文件大小 | 用 `GetFileSize`（Win95 有）或 `SetFilePointer(…, FILE_END)`；新增任何 file/process API 后先跑 `uv --directory tools/api_check run scan_dir.py`（它会按产物自动选 w95/wxp/w7 口径） |
