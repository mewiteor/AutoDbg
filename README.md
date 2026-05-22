# AutoDbg

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C](https://img.shields.io/badge/Core-C%2FC%2B%2B-00599C?style=flat&logo=c&logoColor=white)]()
[![Python](https://img.shields.io/badge/Binding-Python-3776AB?style=flat&logo=python&logoColor=white)]()
[![Lua](https://img.shields.io/badge/Binding-Lua%2FLuaJIT-2C2D72?style=flat&logo=lua&logoColor=white)]()

**[English](./README.md)** | **[中文](./README.zh.md)**

> **Scriptable. Headless. AI-Ready.**

**AutoDbg** is a modern, headless debugging engine designed specifically for **automated reverse engineering** and **AI-driven analysis**. 

Unlike traditional debuggers (e.g., x64dbg, OllyDbg) that are heavily coupled with UI threads and legacy APIs, AutoDbg adopts a **"Pure C Core + Multi-language Middleware"** architecture. It decouples extreme low-level control (C/C++) from high-level ecosystems (LuaJIT / Python), allowing developers to orchestrate Ring 3 breakpoints, memory, and threads with the elegance of modern scripting.

---

## 🌟 Why AutoDbg?

Traditional engines (like TitanEngine) often struggle with modern automation needs: callbacks lacking context (preventing closures), painful cross-language FFI, and severe lag during high-frequency breakpoints. AutoDbg is engineered from the ground up to solve these bottlenecks:

- **🧩 True Closure & State-Machine Support**  
  All native callbacks strictly enforce a `void* user_context` parameter. Whether writing nested callbacks in Lua or passing closures in Python, state is perfectly mapped without relying on global variables.
- **⚡ Extreme Performance & Memory Snapshots**  
  Overcome the physical latency of cross-process APIs (`ReadProcessMemory`) with built-in Memory Snapshot and Scatter-Gather batch reading mechanisms. Transform high-frequency IPC into nanosecond-level local memory parsing.
- **🧠 AI & Ecosystem Ready (Python Native)**  
  Exposes a pure C ABI (`extern "C"`) highly optimized for Python's `cffi`. Import `capstone` for disassembly, use `pefile` for structure parsing, or invoke LLMs to analyze register states directly within breakpoint callbacks.
- **🪶 Lightweight Lua Standalone Host**  
  Bundles the Core Engine with Lua/LuaJIT into a single, portable `.exe`. Zero installation required, capable of running complex automation scripts even on legacy systems like Windows XP.

---

## 🏗️ Architecture

AutoDbg utilizes a strict three-tier separation:

1. **Core Engine (Pure C/C++ Static Library)**  
   Handles OS debugging APIs (Win32 Debug API / Linux `ptrace`), PE/ELF parsing, and thread contexts. **Zero script dependencies**, exporting pure C ABI and opaque handles.
2. **Lua Middleware (Standalone Host)**  
   Links the Core Engine with LuaJIT. Leverages FFI for zero-overhead native calls, ideal for **high-frequency interrupt responses** and **legacy OS compatibility**.
3. **Python Middleware (Extension Module)**  
   Dynamically loads the Core Engine via `cffi`. Provides `asyncio` event-stream wrappers, perfect for **complex data analysis**, **network communication**, and **AI model integration**.

---

## 🚀 Core Features

- **Multi-type Breakpoints**: INT3 software breakpoints, Hardware (DRx) breakpoints, and PageGuard memory breakpoints.
- **Deep System Awareness**: Native support for x64 `RUNTIME_FUNCTION` stack unwinding, deep dumping of C++ Exception Handling (EH) objects, and TLS callback interception.
- **Anti-Anti-Debug Basics**: Built-in PEB flag fixing and Ntdll API inline patching.
- **Thread-Safe Design**: Fully handle-based (`SessionHandle`), supporting concurrent debugging of multiple targets within a single process.

---

## 💻 Quick Start

### Python: AI & Ecosystem Integration
Leverage Python's `asyncio` and rich ecosystem to build linear, readable automation scripts.

```python
import asyncio
import capstone
from autodbg import DebugSession

async def trace_large_allocations():
    session = DebugSession()
    await session.start("C:\\target.exe")
    
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    
    # Nested closures and state machines are natively supported
    async def on_malloc_hit(ctx):
        size = ctx.rdi
        if size > 1000:
            # Seamlessly use Python ecosystem (Memory Snapshot)
            caller_rip = ctx.rsp 
            code = session.read_memory_snapshot(caller_rip, 32)
            
            print(f"\n[!] Large allocation: {size} bytes")
            for insn in md.disasm(code, caller_rip):
                print(f"    0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
                if insn.mnemonic == 'ret': break
                
    await session.set_breakpoint("msvcrt.dll", "malloc", on_malloc_hit)
    await session.resume()

asyncio.run(trace_large_allocations())
```

### Lua: High-Frequency & Legacy Environment
Run via the standalone `autodbg.exe`. Zero dependencies, blazing fast via LuaJIT FFI.

```lua
-- autodbg.exe script.lua
local state = { hits = 0 }

Run({
    imagepath = "C:\\target.exe",
    callback = function(pid, tid)
        SetBreakpoint({
            modname = "kernel32.dll",
            rva = FindFuncRva("kernel32.dll", "CreateFileW"),
            callback = function(info)
                state.hits = state.hits + 1
                local ctx = GetContext(info.tid)
                -- LuaJIT FFI zero-overhead register access
                print(string.format("[Hit %d] CreateFileW called! RCX: %p", state.hits, ctx.Ccx))
            end
        })
    end
})
```

---

## 🗺️ Roadmap

- [ ] **Phase 1**: Win32 Core Engine (Software breakpoints, Exception handling, PE parsing, x64 Stack Unwinding).
- [ ] **Phase 2**: Lua/LuaJIT Standalone Host integration & Closure callback support.
- [ ] **Phase 3**: Hardware (DRx) & Memory (PageGuard) Breakpoints.
- [ ] **Phase 4**: Python `cffi` middleware & `asyncio` event-stream wrappers.
- [ ] **Phase 5**: Cross-platform HAL introducing Linux `ptrace` & `process_vm_readv`.
- [ ] **Phase 6**: Integrate Capstone/Unicorn for AI-driven automated unpacking workflows.

---

## 🤝 Contributing & License

This project is under active development. Issues, PRs, and architectural discussions are highly welcome! 

Distributed under the MIT License. See `LICENSE` for more information.
