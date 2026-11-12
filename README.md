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
  `--server=tcp` turns the same binary into a **raw TCP frame server**, and `--remote=<host:port>` is its client — the script runs on the target machine while you sit anywhere else. `--json` returns one machine-readable envelope, and **any** client invocation can `--cancel` / `--kill` / `--detach` a script that is already running.
- **🖱️ GUI automation**  
  A `gui` table (mouse/keyboard injection, window & control enumeration, PNG screenshots) lets a script — or an AI — drive a debuggee's UI.

---

## 🏗️ Architecture

```text
AutoDbg_<arch>.exe        zero-CRT launcher → payload (single LZMA stream) → mapped module → autodbg_run()
├─ src/core/              debug engine (C++, no Lua): session, breakpoints, memory, contexts, stack walk, GUI primitives
├─ src/scripting/lua/     the only lua_State user: 29 contract globals + gui table + open_file
│                         + tcp_server / client (raw TCP, --remote) / exec_guard / output capture
├─ src/common/            allocator, containers, dynapi, cmdline, net(winsock), frame, json, png, selftest
└─ src/sys/               zero-CRT runtime: shadow headers, .def imports, MASM, LZMA, packaging/mapping, launcher
```

- **Core engine** (`src/core`) — Win32 debug loop, software/hardware breakpoints, memory & PE parsing, thread contexts, stack unwinding, PEB anti-anti-debug. Zero script dependencies.
- **Lua middleware** (`src/scripting/lua`) — binds the engine to Lua 5.5 and LuaJIT with the *same* global namespace; also hosts the raw TCP frame server, the `--remote` client and the `gui` table.
- **Clients** — the CLI (`AutoDbg_<arch>.exe script.lua`), the same binary in server/client mode (`--server=tcp` / `--remote=`), and the neovim plugin (`nvim/`, a separate repository).

---

## 🚀 Core Features

- **Breakpoints**: INT3 software breakpoints, hardware (DRx) breakpoints, single-shot and batch variants.
- **Deep system awareness**: PE parsing (sections/imports/exports), x86 + x64 stack unwinding (`StackWalk64` with `RUNTIME_FUNCTION` fallbacks, loaded dynamically so legacy imports stay clean).
- **Anti-anti-debug**: PEB `BeingDebugged` / `NtGlobalFlag` patching, plus bundled **ScyllaHide** integration in the modern variant (`/DNO_SCYLLA_HIDE` compiles it out of the legacy variant).
- **Server mode (raw TCP frames)**: one script per call, source executed from memory and never written to disk; handshake carries the protocol version and the token; `--cancel` (hard) / `--kill` / `--detach` (soft) act on the execution that is currently running.
- **GUI automation**: `gui.position/move/click/write/press`, `gui.windows/find/activate/rect/children`, `gui.text/set_text/post/send`, `gui.screenshot` (PNG), `gui.pick()` to resolve the pid under the mouse on the target machine.
- **Client-side file I/O**: `open_file()` returns an io-style file object; in remote mode the bytes live on the *client's* disk (binary-safe frames). `f:read()` blocks inside debug callbacks with a timeout (`nil, "timeout"`) instead of failing the session.
- **Machine-readable results**: `--json` / `--out=<path>` emit one ASCII JSON envelope — `ok`, `exit`, `canceled`, `detached`, `pid`, `stdout`, `stderr`, `images`, `result` (the script's `return` value).
- **Editor integration**: neovim plugin with `:AutoDbgRun` (buffer/range), `:AutoDbgCancel`, `:AutoDbgHealth`, quickfix jump-to-error.
- **Legacy-DOS-era support**: x86 subsystem version patched to 4.0, all Win9x-risky imports go through a dynamic-API layer, and a static import gate (`tools/api_check`) keeps XP+ symbols out of the legacy import table.

---

## 💻 Quick Start

### Run a script locally

```bat
.out\AutoDbg_64.exe script.lua
.out\AutoDbg_86.exe --flavor=luajit script.lua      :: LuaJIT instead of Lua 5.5
.out\AutoDbg_64.exe --json script.lua               :: one ASCII JSON line on stdout
.out\AutoDbg_64.exe --help                          :: all launcher options
.out\AutoDbg_64.exe --version                       :: version (git tag/commit) + abi + arch
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
                local ctx = GetContext(tid)          -- x86 and x64 share the names (Cax…Cip / R8…)
                print(string.format("CreateFileW hit, ip=0x%X", ctx.Cip.val))
            end
        })
    end
})
```

The first line of a script may also pick the interpreter: `--!lua` / `--!luajit`.

### Remote execution (raw TCP)

```bat
:: on the target machine
.out\AutoDbg_64.exe --server=tcp --server-port=8642 --server-no-echo

:: from anywhere else
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --json script.lua
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --health      :: is it busy?
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --cancel      :: interrupt the running script
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --detach      :: end the current Attach, keep the target
```

The client is the same binary: it handshakes (protocol version + token), streams the script, prints
its output and exits with the script's exit code. Files opened with `open_file` are read and written
on **the client's** machine. Loopback needs no token; a non-loopback listen address requires one
(`--server-token=auto` prints it in the banner).

> The HTTP transport (`--server=http`, `POST /run`, `/health`, MCP) was removed: the CLI plus these
> frames is the whole contract now (D41/D42 in `plan_cli.md`).

### From neovim

```lua
{ dir = "~/src/AutoDbg.nvim", config = function() require("autodbg").setup({}) end }
```

Then `:AutoDbgRun` (or `:'<,'>AutoDbgRun`). See [`nvim/README.md`](./nvim/README.md).

> The plugin (separate repository) is being moved from the deleted HTTP transport to the raw TCP
> client; local mode (`:AutoDbgRun` on a machine that has the exe) works unchanged meanwhile.

### For AI agents

Agents drive AutoDbg through the CLI and get structured results back:

```bat
.out\AutoDbg_64.exe --json step1.lua                :: one ASCII JSON line on stdout
.out\AutoDbg_64.exe --out=result.json step1.lua     :: same envelope, written to a file
```

```json
{"ok":true,"exit":0,"canceled":false,"detached":false,"pid":0,
 "stdout":"...","stderr":"","images":[],"result":{"found":3}}
```

`result` is the script's `return` value, `images` lists the files it wrote, and `pid` is the
debuggee (0 when nothing was attached).

The **skill package** in [`tools/agent/autodbg-skill/`](./tools/agent/autodbg-skill/) is the contract
documentation for agents: CLI flags, the `--json` envelope, every Lua global, five recipes and the
pitfalls that cost the most time. Point your agent at it (for pi:
`{ "skills": ["<repo>/tools/agent/autodbg-skill"] }`).

Because `Run`/`Attach` block until the target exits or the script calls `Detach()`, the whole
debugging scenario belongs **inside one script** (`Run{ callback = function() ... end }`), not in a
sequence of interactive calls.

---

## 📊 Status

### Done

- [x] Win32 core engine: software/hardware breakpoints, memory & PE parsing, contexts, x64/x86 stack unwinding.
- [x] Lua 5.5 **and** LuaJIT support from one deliverable; 29 stable global names + `Var` metatable.
- [x] Single-file packaging: 2 exes instead of 6, embedded modules, in-memory mapping, variant fallback.
- [x] Win9x static import gate green (`tools/api_check`); x86 subsystem version patched to 4.0.
- [x] CLI contract: `--json` / `--out` envelopes, raw TCP server + `--remote` client, `--cancel` / `--kill` / `--detach`, `open_file` read/write with a blocking RPC, `gui.pick()`, agent skill package.
- [x] HTTP / MCP / effects removed (D41/D43/D44) — CLI + frames only.

### Open

- [ ] Win9x real-machine verification (matrix, encoding, dynapi degradation paths) — see `plan_p7_win9x.md`.
- [ ] neovim plugin: move from HTTP to the raw TCP client, restore its smoke gate.
- [ ] Blocking `f:read()` inside `Attach`/`Run` callbacks: end-to-end case with "timeout → callback returns → breakpoint continues".
- [ ] Engine `begin/pump/continue` + `--repl` (T7, optional) — a script-free way to watch a target run.
- [ ] PageGuard memory breakpoints, memory snapshot / scatter-gather reads.
- [ ] Python (`cffi`) middleware, Linux `ptrace` HAL, Capstone/Unicorn integration, `BsonDecode`.

---

## 📚 Documentation

| File | Contents |
| --- | --- |
| `AGENTS.md` | Build, gates, hard constraints, code style, architecture, packaging ABI, decided items |
| `PITFALLS.md` | Every trap already hit (symptom → cause → current fix) |
| `plan_cli.md` | The decided CLI + raw TCP roadmap (D41–D50) and its task list |
| `plan_p7_win9x.md` | Remaining Win9x real-machine test work |
| `plan_server.md` | Historical server/MCP notes (transport decisions superseded by `plan_cli.md`) |
| `tools/agent/autodbg-skill/` | The agent-facing contract document (skill package) |
| `nvim/README.md` | neovim plugin installation, commands, options |

---

## 🤝 Contributing & License

This project is under active development. Issues, PRs and architectural discussions are welcome.

Distributed under the MIT License. See `LICENSE` for more information.
