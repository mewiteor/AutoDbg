# AutoDbg

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C](https://img.shields.io/badge/Core-C%2FC%2B%2B-00599C?style=flat&logo=c&logoColor=white)]()
[![Python](https://img.shields.io/badge/Binding-Python-3776AB?style=flat&logo=python&logoColor=white)]()
[![Lua](https://img.shields.io/badge/Binding-Lua%2FLuaJIT-2C2D72?style=flat&logo=lua&logoColor=white)]()

**[English](./README.md)** | **[中文](./README.zh.md)**

> **脚本化 · 无头引擎 · AI 就绪**

**AutoDbg** 是一个专为**自动化逆向工程**与 **AI 辅助分析**设计的现代无头 (Headless) 调试引擎。

抛弃了传统调试器（如 x64dbg, OllyDbg）臃肿的 UI 耦合与老旧的 API 设计，AutoDbg 采用 **“纯 C 核心 + 多语言中间件”** 的架构。它将极致的底层控制力（C/C++）与高层生态（LuaJIT / Python）完美解耦，让开发者能够以现代脚本语言的优雅，去驾驭 Ring 3 层的断点、内存与线程。

---

## 🌟 为什么选择 AutoDbg？

传统引擎（如 TitanEngine）在应对现代自动化需求时，常面临回调无法传递上下文（无法写闭包）、跨语言 FFI 困难、高频断点卡顿等痛点。AutoDbg 从第一行代码起，就为解决这些问题而生：

- **🧩 真正的闭包与状态机支持**  
  所有底层回调均强制携带 `void* user_context` 参数。无论是在 Lua 中写嵌套回调，还是在 Python 中传递闭包，状态都能完美映射，彻底告别全局变量。
- **⚡ 极致性能与内存快照**  
  针对跨进程 API (`ReadProcessMemory`) 的物理延迟，内置内存快照 (Memory Snapshot) 与 Scatter-Gather 批量读取机制。将高频的跨进程通信转化为本地内存的纳秒级解析。
- **🧠 AI 与生态友好 (Python 原生)**  
  提供对 Python `cffi` 极度友好的纯 C ABI (`extern "C"`)。你可以在断点回调中直接 `import capstone` 进行反汇编，或调用 LLM 大模型实时分析寄存器状态。
- **🪶 轻量级 Lua 独立宿主**  
  将核心引擎与 Lua/LuaJIT 打包为单一、便携的 `.exe` 文件。无需安装任何环境，即可在 Windows XP 甚至更老的系统上运行复杂的自动化追踪脚本。

---

## 🏗️ 架构设计

AutoDbg 采用严格的三层分离架构：

1. **核心引擎 (纯 C/C++ 静态库)**  
   负责 OS 调试 API 交互（Win32 Debug API / Linux `ptrace`）、PE/ELF 解析与线程上下文管理。**零脚本依赖**，仅导出纯 C ABI 与不透明句柄。
2. **Lua 中间件 (独立宿主)**  
   链接核心引擎与 LuaJIT。利用 FFI 实现零开销的底层调用，专为**高频中断响应**与**老旧系统兼容**设计。
3. **Python 中间件 (扩展模块)**  
   通过 `cffi` 动态加载核心引擎。提供 `asyncio` 异步事件流封装，完美契合**复杂数据分析**、**网络通信**与 **AI 模型接入**。

---

## 🚀 核心特性

- **多类型断点**：INT3 软件断点、硬件断点 (DRx) 与 PageGuard 内存断点。
- **深度系统感知**：原生支持 x64 `RUNTIME_FUNCTION` 栈回溯、C++ 异常处理 (EH) 对象深度 Dump 以及 TLS 回调拦截。
- **反反调试基建**：内置 PEB 标志位修复与 Ntdll API 内存 Patch 等基础反检测手段。
- **线程安全设计**：全面句柄化 (`SessionHandle`)，支持单进程内并发调试多个目标。

---

## 💻 快速开始

### Python：AI 与生态集成
利用 Python 的 `asyncio` 与丰富的生态系统，构建线性、可读的自动化脚本。

```python
import asyncio
import capstone
from autodbg import DebugSession

async def trace_large_allocations():
    session = DebugSession()
    await session.start("C:\\target.exe")
    
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    
    # 原生支持嵌套闭包与状态机
    async def on_malloc_hit(ctx):
        size = ctx.rdi
        if size > 1000:
            # 无缝使用 Python 生态 (内存快照)
            caller_rip = ctx.rsp 
            code = session.read_memory_snapshot(caller_rip, 32)
            
            print(f"\n[!] 发现大块内存分配: {size} bytes")
            for insn in md.disasm(code, caller_rip):
                print(f"    0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
                if insn.mnemonic == 'ret': break
                
    await session.set_breakpoint("msvcrt.dll", "malloc", on_malloc_hit)
    await session.resume()

asyncio.run(trace_large_allocations())
```

### Lua：高频响应与老旧环境
通过独立的 `autodbg.exe` 运行。零依赖，借助 LuaJIT FFI 实现极致性能。

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
                -- LuaJIT FFI 零开销寄存器访问
                print(string.format("[命中 %d] CreateFileW 被调用! RCX: %p", state.hits, ctx.Ccx))
            end
        })
    end
})
```

---

## 🗺️ 路线图

- [ ] **阶段 1**: Win32 核心引擎 (软件断点、异常处理、PE 解析、x64 栈回溯)。
- [ ] **阶段 2**: Lua/LuaJIT 独立宿主集成与闭包回调支持。
- [ ] **阶段 3**: 硬件断点 (DRx) 与内存断点 (PageGuard) 支持。
- [ ] **阶段 4**: Python `cffi` 中间件与 `asyncio` 异步事件流封装。
- [ ] **阶段 5**: 跨平台硬件抽象层 (HAL)，引入 Linux `ptrace` 与 `process_vm_readv`。
- [ ] **阶段 6**: 集成 Capstone/Unicorn，打造 AI 驱动的自动化脱壳/分析工作流。

---

## 🤝 贡献与许可

本项目目前处于积极开发阶段。非常欢迎提交 Issue、PR 或参与架构讨论！

本项目基于 MIT 许可证开源。详情请参阅 `LICENSE` 文件。
