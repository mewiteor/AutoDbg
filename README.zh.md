# AutoDbg

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![Core](https://img.shields.io/badge/Core-C%2FC%2B%2B-00599C?style=flat&logo=c&logoColor=white)
![Scripting](https://img.shields.io/badge/Scripting-Lua%205.5%2FLuaJIT-2C2D72?style=flat&logo=lua&logoColor=white)
![Client](https://img.shields.io/badge/Client-Neovim-57A143?style=flat&logo=neovim&logoColor=white)

**[English](./README.md)** | **[中文](./README.zh.md)**

> **脚本化 · 无头引擎 · AI 就绪**

**AutoDbg** 是一个无头 (Headless) 的 Windows 调试引擎（Win32 Debug API），为**自动化逆向工程**与 **AI 辅助分析**提供底层调试原语。

架构是「纯 C/C++ 核心 + 脚本中间层」：核心不感知任何脚本，Lua 绑定层是唯一的 `lua_State` 使用者；交付物是**单一便携 `.exe`**，运行时从自带载荷里解出模块并**在内存中映射**（不落盘）。

硬性目标：**零 CRT 运行时**、**兼容 Windows 95/98**（x86 *legacy* 变体）、**同时支持 Lua 5.5 与 LuaJIT**。

---

## 🌟 为什么选择 AutoDbg？

- **🧩 真正带上下文的回调**  
  所有底层回调都强制携带 `void* user_context`（另有宿主无关的 `callback_ref_t` 令牌），Lua 里的嵌套闭包与状态机无需全局变量即可工作。
- **🪶 每个架构只有一个文件**  
  `.out/AutoDbg_86.exe` 覆盖 Win95 → Win11 32 位 × {Lua 5.5, LuaJIT} × {modern, legacy}；`.out/AutoDbg_64.exe` 覆盖 x64。模块以 LZMA 单流内嵌、内存映射加载——无需安装、不写临时文件。
- **🌐 远程执行 / 编辑器内执行 / AI 就绪**  
  `--server=tcp` 把同一个二进制变成 **raw TCP 帧服务端**，`--remote=<host:port>` 就是它的客户端——脚本在目标机上跑，你可以在任何地方驱动。`--json` 返回一行机器可读信封，而且**任何**一次客户端调用都能 `--cancel` / `--kill` / `--detach` 一个正在跑的脚本。
- **🖱️ GUI 自动化**  
  `gui` 表（键鼠注入、窗口与控件枚举、PNG 截图）让脚本——或 AI——直接操作目标的界面。

---

## 🏗️ 架构设计

```text
AutoDbg_<arch>.exe        零 CRT 启动器 → payload (LZMA 单流) → 手工映射模块 → autodbg_run()
├─ src/core/              调试引擎（C++，禁 Lua）：会话、断点、内存、上下文、栈回溯、GUI 原语
├─ src/scripting/lua/     唯一感知 lua_State 的层：29 个契约全局 + gui 表 + open_file
│                         + tcp_server / client（raw TCP、--remote）/ exec_guard / 输出捕获
├─ src/common/            allocator、containers、dynapi、cmdline、net(winsock)、frame、json、png、selftest
└─ src/sys/               零 CRT 运行时：影子头、导入表 .def、MASM、LZMA、打包/映射、启动器
```

- **核心引擎**（`src/core`）——Win32 调试主循环、软/硬件断点、内存与 PE 解析、线程上下文、栈回溯、PEB 反反调试。零脚本依赖。
- **Lua 中间层**（`src/scripting/lua`）——用**同一套全局命名**同时绑定 Lua 5.5 与 LuaJIT；raw TCP 帧服务端、`--remote` 客户端与 `gui` 表也在这一层。
- **客户端**——命令行（`AutoDbg_<arch>.exe script.lua`）、同一个二进制的服务端/客户端形态（`--server=tcp` / `--remote=`）、neovim 插件（`nvim/`，独立仓库）。

---

## 🚀 核心特性

- **断点**：INT3 软件断点、硬件断点 (DRx)，支持一次性断点与批量设置。
- **深度系统感知**：PE 解析（节/导入/导出）、x86 与 x64 栈回溯（`StackWalk64` + `RUNTIME_FUNCTION` 兜底，全部动态取符号 → legacy 导入表保持干净）。
- **反反调试**：PEB `BeingDebugged` / `NtGlobalFlag` 修补，modern 变体内置 **ScyllaHide** 集成（legacy 变体用 `/DNO_SCYLLA_HIDE` 编掉）。
- **server 模式（raw TCP 帧）**：一次调用 = 一段脚本，源码从内存加载执行、**不落盘**；握手帧带协议版本与 token；`--cancel`（硬）/ `--kill` / `--detach`（软）作用于「当前正在跑的那次执行」。
- **GUI 自动化**：`gui.position/move/click/write/press`、`gui.windows/find/activate/rect/children`、`gui.text/set_text/post/send`、`gui.screenshot`（PNG），以及 `gui.pick()`（在目标机上按鼠标位置反查 pid）。
- **客户端文件 I/O**：`open_file()` 返回 io 风格的文件对象；远程模式下字节就在**客户端**的磁盘上（帧里传原始字节）。`f:read()` 在调试回调里会阻塞等回包，超时返回 `nil, "timeout"` 而不是把会话搞崩。
- **机器可读结果**：`--json` / `--out=<path>` 输出一行纯 ASCII JSON 信封——`ok`、`exit`、`canceled`、`detached`、`pid`、`stdout`、`stderr`、`images`、`result`（脚本的 `return` 值）。
- **编辑器集成**：neovim 插件提供 `:AutoDbgRun`（buffer / 选区）、`:AutoDbgCancel`、`:AutoDbgHealth`，错误行进 quickfix。
- **老系统支持**：x86 子系统版本改写为 4.0，所有 Win9x 风险符号走动态 API 层，静态导入门禁（`tools/api_check`）保证 legacy 导入表里没有 XP+ 符号。

---

## 💻 快速开始

### 本机跑脚本

```bat
.out\AutoDbg_64.exe script.lua
.out\AutoDbg_86.exe --flavor=luajit script.lua      :: 用 LuaJIT 而不是 Lua 5.5
.out\AutoDbg_64.exe --json script.lua               :: stdout 只有一行 ASCII JSON
.out\AutoDbg_64.exe --help                          :: 列出全部启动器选项
.out\AutoDbg_64.exe --version                       :: 版本（git tag/commit）+ abi + 架构
```

```lua
-- script.lua
Run({
    imagepath = "C:\\target.exe",
    callback = function(pid, tid)
        SetBreakpoint({
            modname = "kernel32.dll",
            rva = FindFuncRva("kernel32.dll", "CreateFileW"),
            callback = function(info)
                local ctx = GetContext(info.tid)          -- x86 与 x64 同名（Cax…Cip / R8…）
                print(string.format("CreateFileW 命中, ip=0x%X", ctx.Cip.val))
            end
        })
    end
})
```

脚本首行也可以指定解释器：`--!lua` / `--!luajit`。

### 远程执行（raw TCP）

```bat
:: 目标机上
.out\AutoDbg_64.exe --server=tcp --server-port=8642 --server-no-echo

:: 任意一台机器上
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --json script.lua
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --health      :: 忙不忙？
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --cancel      :: 打断正在跑的脚本
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --detach      :: 只结束当前 Attach，目标继续活着
```

客户端就是同一个二进制：握手（协议版本 + token）→ 把脚本用帧发过去 → 打印它的输出 → 用脚本的退出码退出。`open_file` 打开的文件在**客户端**那台机器上读写。loopback 免 token；非 loopback 监听必须给 token（`--server-token=auto` 会在启动横幅里打印）。

> HTTP 传输（`--server=http`、`POST /run`、`/health`、MCP）已经删除：现在对外契约就是 CLI + 这些帧（`plan.md` 的 D41/D42）。

### 在 neovim 里

```lua
{ dir = "~/src/AutoDbg.nvim", config = function() require("autodbg").setup({}) end }
```

然后 `:AutoDbgRun`（或 `:'<,'>AutoDbgRun`）。详见 [`nvim/README.md`](./nvim/README.md)。

> 插件（独立仓库）正在从已删除的 HTTP 传输迁到 raw TCP 客户端；在此之前**本地模式**（机器上有 exe 时跑 `:AutoDbgRun`）照旧可用。

### 给 AI 代理

代理通过 CLI 驱动 AutoDbg，拿回结构化结果：

```bat
.out\AutoDbg_64.exe --json step1.lua                :: stdout 只有一行 ASCII JSON
.out\AutoDbg_64.exe --out=result.json step1.lua     :: 同一份信封写文件
```

```json
{"ok":true,"exit":0,"canceled":false,"detached":false,"pid":0,
 "stdout":"...","stderr":"","images":[],"result":{"found":3}}
```

`result` 是脚本的 `return` 值，`images` 是它写过盘的文件，`pid` 是被调试进程（没附加时为 0）。

[`tools/agent/autodbg-skill/`](./tools/agent/autodbg-skill/) 是给 agent 的**契约文档**（Agent Skills 格式）：CLI 选项、`--json` 信封、每个 Lua 全局、5 个场景 recipe、以及最费时间的坑。让 agent 加载它即可（pi：`{ "skills": ["<repo>/tools/agent/autodbg-skill"] }`）。

因为 `Run`/`Attach` 会阻塞到目标退出或脚本 `Detach()`，整套调试场景要写在**一段脚本里**（`Run{ callback = function() ... end }`），而不是拆成多次交互式调用。

---

## 📊 进度

### 已完成

- [x] Win32 核心引擎：软/硬件断点、内存与 PE 解析、线程上下文、x64/x86 栈回溯。
- [x] 一个交付物同时支持 **Lua 5.5 与 LuaJIT**；29 个稳定全局名 + `Var` 元表。
- [x] 单文件打包：2 个 exe 取代 6 个，内嵌模块、内存映射、变体回退。
- [x] Win9x 静态导入门禁通过（`tools/api_check`）；x86 子系统版本改写为 4.0。
- [x] CLI 契约：`--json` / `--out` 信封、raw TCP 服务端 + `--remote` 客户端、`--cancel` / `--kill` / `--detach`、`open_file` 读写（阻塞 RPC）、`gui.pick()`、agent 技能包。
- [x] HTTP / MCP / effects 删除（D41/D43/D44）—— 只剩 CLI + 帧。

### 待完成

- [ ] Win9x 实机验证（矩阵、编码、dynapi 降级路径）—— 见 `plan.md` §1.3。
- [ ] neovim 插件：迁到 raw TCP 客户端并恢复 smoke 门禁。
- [ ] `Attach`/`Run` 回调里的阻塞 `f:read()`：补「超时 → 回调返回 → 断点继续」的端到端用例。
- [ ] 引擎 `begin/pump/continue` + `--repl`（T7，可选）—— 不用回调也能「看着目标跑」。
- [ ] PageGuard 内存断点、内存快照 / 分散聚合读取。
- [ ] Python (`cffi`) 中间层、Linux `ptrace` HAL、Capstone/Unicorn 集成、`BsonDecode`。

---

## 📚 文档

| 文件 | 内容 |
| --- | --- |
| `AGENTS.md` | 构建、门禁、硬约束、代码风格、架构、打包 ABI、已定决策 |
| `PITFALLS.md` | 已踩过的坑（现象 → 根因 → 现在的做法） |
| `plan.md` | 剩余工作（Win9x 矩阵、server/GUI 实测、CLI/帧协议尾巴）——`plan_cli.md` / `plan_server.md` / `plan_p7_win9x.md` 的合并版 |
| `tools/agent/autodbg-skill/` | 面向 agent 的契约文档（技能包） |
| `nvim/README.md` | neovim 插件的安装、配置与命令 |

---

## 🤝 贡献与许可

项目仍在活跃开发中，欢迎 issue、PR 与架构讨论。

以 MIT 许可证发布，详见 `LICENSE`。
