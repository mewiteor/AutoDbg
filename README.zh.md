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
  `--server` 把同一个二进制变成 HTTP 服务（`POST /run` 直接执行**请求体里的 Lua 源码**并把输出随响应返回）；**neovim 插件**一键跑当前 buffer 并把结果显示在 scratch buffer；`POST /mcp` 提供 **MCP** 端点给 AI 代理用。
- **🖱️ GUI 自动化**  
  `gui` 表（键鼠注入、窗口与控件枚举、PNG 截图）让脚本——或 AI——能在目标停下时直接操作它的界面。

---

## 🏗️ 架构设计

```text
AutoDbg_<arch>.exe        零 CRT 启动器 → payload (LZMA 单流) → 手工映射模块 → autodbg_run()
├─ src/core/              调试引擎（C++，禁 Lua）：会话、断点、内存、上下文、栈回溯、GUI 原语
├─ src/scripting/lua/     唯一感知 lua_State 的层：29 个契约全局 + gui 表 + read_file/write_file
│                         + server / exec / MCP 会话 / 输出捕获
├─ src/common/            allocator、containers、dynapi、cmdline、net(winsock)、http、json、png、base64
└─ src/sys/               零 CRT 运行时：影子头、导入表 .def、MASM、LZMA、打包/映射、启动器
```

- **核心引擎**（`src/core`）——Win32 调试主循环、软/硬件断点、内存与 PE 解析、线程上下文、栈回溯、PEB 反反调试。零脚本依赖。
- **Lua 中间层**（`src/scripting/lua`）——用**同一套全局命名**同时绑定 Lua 5.5 与 LuaJIT；HTTP server、持久 MCP 会话与 `gui` 表也在这一层。
- **客户端**——命令行（`AutoDbg_<arch>.exe script.lua`）、neovim 插件（`nvim/`，独立仓库）、`curl`、MCP 客户端。

---

## 🚀 核心特性

- **断点**：INT3 软件断点、硬件断点 (DRx)，支持一次性断点与批量设置。
- **深度系统感知**：PE 解析（节/导入/导出）、x86 与 x64 栈回溯（`StackWalk64` + `RUNTIME_FUNCTION` 兜底，全部动态取符号 → legacy 导入表保持干净）。
- **反反调试**：PEB `BeingDebugged` / `NtGlobalFlag` 修补，modern 变体内置 **ScyllaHide** 集成（legacy 变体用 `/DNO_SCYLLA_HIDE` 编掉）。
- **server 模式**：无状态 `POST /run`（源码从内存加载执行，**不落盘**）、`/cancel`（100 ms 粒度）、`/shutdown`、`/health`、`--server-token`（默认 loopback 免 token）。
- **GUI 自动化**：`gui.position/move/click/write/press`、`gui.windows/find/activate/rect/children`、`gui.text/set_text/post/send`、`gui.screenshot`（PNG）。
- **客户端文件 I/O**：`read_file`/`write_file` 读写的是**客户端**那台机器的磁盘；server 挂起协程、由客户端完成效果后恢复，二进制数据在远程会话里照样安全。
- **MCP**：`POST /mcp` + `Mcp-Session-Id` 会话，13 个工具（`lua_eval`、调试原语、GUI 原语）。
- **编辑器集成**：neovim 插件提供 `:AutoDbgRun`（buffer / 选区）、`:AutoDbgCancel`、`:AutoDbgHealth`，错误行进 quickfix。
- **老系统支持**：x86 子系统版本改写为 4.0，所有 Win9x 风险符号走动态 API 层，静态导入门禁（`tools/api_check`）保证 legacy 导入表里没有 XP+ 符号。

---

## 💻 快速开始

### 本机跑脚本

```bat
.out\AutoDbg_64.exe script.lua
.out\AutoDbg_86.exe --flavor=luajit script.lua      :: 用 LuaJIT 而不是 Lua 5.5
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
                print(string.format("CreateFileW 命中, ip=0x%X", ctx.Cip))
            end
        })
    end
})
```

脚本首行也可以指定解释器：`--!lua` / `--!luajit`。

### 远程执行（server 模式）

```bat
.out\AutoDbg_64.exe --server                       :: 127.0.0.1:8642，loopback 免 token
```

```bash
curl -sS --data-binary @- -H 'Content-Type: text/x-lua' \
     -H 'X-AutoDbg-Filename: buffer.lua' \
     'http://127.0.0.1:8642/run?format=text' <<'LUA'
print("hello from", _VERSION)
LUA
```

### 在 neovim 里

```lua
{ dir = "~/src/AutoDbg.nvim", config = function() require("autodbg").setup({ url = "http://127.0.0.1:8642" }) end }
```

然后 `:AutoDbgRun`（或 `:'<,'>AutoDbgRun`）。详见 [`nvim/README.md`](./nvim/README.md)。

### 给 AI 代理（MCP）

```bash
curl -sS -X POST http://127.0.0.1:8642/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}'
```

---

## 📊 进度

### 已完成

- [x] Win32 核心引擎：软/硬件断点、内存与 PE 解析、线程上下文、x64/x86 栈回溯。
- [x] 同一份交付物同时支持 Lua 5.5 与 LuaJIT；29 个稳定全局名 + `Var` 元表。
- [x] 单文件打包：2 个 exe 取代 6 个旧产物，模块内嵌 + 内存映射 + 变体回退。
- [x] Win9x 静态导入门禁全绿（`tools/api_check`）；x86 子系统版本改写为 4.0。
- [x] server 模式（HTTP 子集）、取消/超时、nvim 客户端（本地/远程自动选、脚本头标记、按架构的 LuaLS 补全、LuaSnip 片段）、GUI 自动化、MCP、客户端文件 I/O。

### 待完成

- [ ] Win9x 实机验收（测试矩阵、编码、dynapi 降级路径）——见 `plan_p7_win9x.md`。
- [ ] 增量流式（脚本执行中推 SSE 帧）、`GET /screenshot`、nvim 的纯 `vim.loop` 传输——见 `plan_server.md`。
- [ ] PageGuard 内存断点、内存快照 / Scatter-Gather 批量读。
- [ ] Python (`cffi`) 中间件、Linux `ptrace` HAL、Capstone/Unicorn 集成、`BsonDecode`。

---

## 📚 文档

| 文件 | 内容 |
| --- | --- |
| `AGENTS.md` | 构建、门禁、硬约束、风格、架构、打包 ABI、已定决策 |
| `PITFALLS.md` | 已经踩过的坑（现象 → 根因 → 现在的做法） |
| `plan_p7_win9x.md` | Win9x 实机测试的剩余工作 |
| `plan_server.md` | server / MCP / GUI / 客户端文件 I/O 的剩余工作 |
| `nvim/README.md` | neovim 插件的安装、命令与配置 |

---

## 🤝 贡献与许可

本项目目前处于积极开发阶段。非常欢迎提交 Issue、PR 或参与架构讨论！

本项目基于 MIT 许可证开源。详情请参阅 `LICENSE` 文件。
