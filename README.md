# AutoDbg

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![Core](https://img.shields.io/badge/Core-C%2FC%2B%2B-00599C?style=flat&logo=c&logoColor=white)
![Scripting](https://img.shields.io/badge/Scripting-Lua%205.5%2FLuaJIT-2C2D72?style=flat&logo=lua&logoColor=white)
![Client](https://img.shields.io/badge/Client-Neovim-57A143?style=flat&logo=neovim&logoColor=white)

**[English](./README.md)** | **[中文](./README.zh.md)**

> **Scriptable. Headless. AI-Ready.**

**AutoDbg** is a headless Windows debugging engine (Win32 Debug API) built for **automated reverse engineering** and **AI-driven analysis**.

The architecture is a pure C/C++ core plus a scripting middleware: the core knows nothing about scripts, the Lua binding layer is the only `lua_State` user, and the deliverable is a **single portable `.exe`** that unpacks its own modules at run time and maps them in memory (nothing is written to disk).

Hard goals: **zero CRT runtime**, **Windows 95/98 compatibility** (x86 *legacy* variant), and **both Lua 5.5 and LuaJIT** supported from one binary.

---

## 🌟 Why AutoDbg?

- **🧩 Real callbacks with context**  
  Every native callback carries a `void* user_context` (plus a host-agnostic `callback_ref_t` token), so nested Lua closures and state machines work without global variables.
- **🪶 One file per architecture**  
  `.out/AutoDbg_86.exe` covers Win95 → Win11 32-bit × {Lua 5.5, LuaJIT} × {modern, legacy}; `.out/AutoDbg_64.exe` covers x64. Modules are embedded in an LZMA payload and mapped in memory — no install, no temp files.
- **🌐 Remote, editor-driven, AI-ready**  
  `--server` turns the same binary into an HTTP service (`POST /run` executes Lua source sent over the wire, streamed output comes back in the response), a **neovim plugin** runs the current buffer and shows results in a scratch buffer, and `POST /mcp` serves an **MCP** endpoint for AI agents.
- **🖱️ GUI automation**  
  A `gui` table (mouse/keyboard injection, window & control enumeration, PNG screenshots) lets a script — or an AI — drive a debuggee's UI while it is stopped.

---

## 🏗️ Architecture

```text
AutoDbg_<arch>.exe        零 CRT 启动器 → payload (LZMA 单流) → 手工映射模块 → autodbg_run()
├─ src/core/              调试引擎（C++，禁 Lua）：会话、断点、内存、上下文、栈回溯、GUI 原语
├─ src/scripting/lua/     唯一感知 lua_State 的层：29 个契约全局 + gui 表 + read_file/write_file
│                         + server / exec / MCP 会话 / 输出捕获
├─ src/common/            allocator、containers、dynapi、cmdline、net(winsock)、http、json、png、base64
└─ src/sys/               零 CRT 运行时：影子头、导入表 .def、MASM、LZMA、打包/映射、启动器
```

- **Core engine** (`src/core`) — Win32 debug loop, software/hardware breakpoints, memory & PE parsing, thread contexts, stack unwinding, PEB anti-anti-debug. Zero script dependencies.
- **Lua middleware** (`src/scripting/lua`) — binds the engine to Lua 5.5 and LuaJIT with the *same* global namespace; also hosts the HTTP server, persistent MCP sessions and the `gui` table.
- **Clients** — the CLI (`AutoDbg_<arch>.exe script.lua`), the neovim plugin (`nvim/`, a separate repository), `curl`, and MCP clients.

---

## 🚀 Core Features

- **Breakpoints**: INT3 software breakpoints, hardware (DRx) breakpoints, single-shot and batch variants.
- **Deep system awareness**: PE parsing (sections/imports/exports), x86 + x64 stack unwinding (`StackWalk64` with `RUNTIME_FUNCTION` fallbacks, loaded dynamically so legacy imports stay clean).
- **Anti-anti-debug**: PEB `BeingDebugged` / `NtGlobalFlag` patching, plus bundled **ScyllaHide** integration in the modern variant (`/DNO_SCYLLA_HIDE` compiles it out of the legacy variant).
- **Server mode**: stateless `POST /run` (source executed from memory, never written to disk), `/cancel` with 100 ms granularity, `/shutdown`, `/health`, `--server-token` with loopback-by-default auth.
- **GUI automation**: `gui.position/move/click/write/press`, `gui.windows/find/activate/rect/children`, `gui.text/set_text/post/send`, `gui.screenshot` (PNG).
- **Client-side file I/O**: `read_file`/`write_file` touch the *client's* disks; the server suspends a coroutine and the client fulfils the effect, so binary data keeps working across a remote session.
- **MCP**: `POST /mcp` with `Mcp-Session-Id` sessions and 13 tools (`lua_eval`, debugger primitives, GUI primitives).
- **Editor integration**: neovim plugin with `:AutoDbgRun` (buffer/range), `:AutoDbgCancel`, `:AutoDbgHealth`, quickfix jump-to-error.
- **Legacy-DOS-era support**: x86 subsystem version patched to 4.0, all Win9x-risky imports go through a dynamic-API layer, and a static import gate (`tools/api_check`) keeps XP+ symbols out of the legacy import table.

---

## 💻 Quick Start

### Run a script locally

```bat
.out\AutoDbg_64.exe script.lua
.out\AutoDbg_86.exe --flavor=luajit script.lua      :: LuaJIT instead of Lua 5.5
```

```lua
-- script.lua
Run({
    imagepath = "C:\\target.exe",
    callback = function(pid, tid)
        SetBreakpoint({
            modname = "kernel32.dll",
            rva = FindFuncRva("kernel32.dll", "CreateFileW"),
            callback = function()
                local ctx = GetContext(tid)          -- x86 与 x64 同名（Cax…Cip / R8…）
                print(string.format("CreateFileW hit, ip=0x%X", ctx.Cip))
            end
        })
    end
})
```

The first line of a script may also pick the interpreter: `--!lua` / `--!luajit`.

### Remote execution (server mode)

```bat
.out\AutoDbg_64.exe --server                       :: 127.0.0.1:8642, token-free on loopback
```

```bash
curl -sS --data-binary @- -H 'Content-Type: text/x-lua' \
     -H 'X-AutoDbg-Filename: buffer.lua' \
     'http://127.0.0.1:8642/run?format=text' <<'LUA'
print("hello from", _VERSION)
LUA
```

### From neovim

```lua
{ dir = "~/src/AutoDbg.nvim", config = function() require("autodbg").setup({ url = "http://127.0.0.1:8642" }) end }
```

Then `:AutoDbgRun` (or `:'<,'>AutoDbgRun`). See [`nvim/README.md`](./nvim/README.md).

### For AI agents (MCP)

```bash
curl -sS -X POST http://127.0.0.1:8642/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}'
```

---

## 📊 Status

### Done

- [x] Win32 core engine: software/hardware breakpoints, memory & PE parsing, contexts, x64/x86 stack unwinding.
- [x] Lua 5.5 **and** LuaJIT support from one deliverable; 29 stable global names + `Var` metatable.
- [x] Single-file packaging: 2 exes instead of 6, embedded modules, in-memory mapping, variant fallback.
- [x] Win9x static import gate green (`tools/api_check`); x86 subsystem version patched to 4.0.
- [x] Server mode (HTTP subset), cancel/timeout, nvim client (local-or-remote execution, script-head markers, LuaLS completions per arch, LuaSnip snippets), GUI automation, MCP, client file I/O.

### Open

- [ ] Win9x real-machine verification (matrix, encoding, dynapi degradation paths) — see `plan_p7_win9x.md`.
- [ ] Incremental streaming (SSE frames while a script runs), `GET /screenshot`, pure-`vim.loop` nvim transport — see `plan_server.md`.
- [ ] PageGuard memory breakpoints, memory snapshot / scatter-gather reads.
- [ ] Python (`cffi`) middleware, Linux `ptrace` HAL, Capstone/Unicorn integration, `BsonDecode`.

---

## 📚 Documentation

| File | Contents |
| --- | --- |
| `AGENTS.md` | Build, gates, hard constraints, code style, architecture, packaging ABI, decided items |
| `PITFALLS.md` | Every trap already hit (symptom → cause → current fix) |
| `plan_p7_win9x.md` | Remaining Win9x real-machine test work |
| `plan_server.md` | Remaining server / MCP / GUI / client-I/O work |
| `nvim/README.md` | neovim plugin installation, commands, options |

---

## 🤝 Contributing & License

This project is under active development. Issues, PRs and architectural discussions are welcome.

Distributed under the MIT License. See `LICENSE` for more information.
