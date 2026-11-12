---
name: autodbg-skill
description: Write and run AutoDbg Lua scripts to debug Windows processes (breakpoints, memory read/write, PE/symbols, memory scanning, GUI automation). Use when debugging a live Windows target, automating a GUI program, or scripting Win32 debugging from an agent.
---

# AutoDbg

AutoDbg is a headless, scriptable Windows debugger (Win32 Debug API). You drive it with a **Lua script**; the script is the whole program — there is no interactive prompt and no "paused target" state.

## Fast path

```bat
:: drive a local target
.out\AutoDbg_64.exe --json my_script.lua

:: drive a remote machine / a server you started
.out\AutoDbg_64.exe --server=tcp --server-port=8642          :: on the target machine
.out\AutoDbg_64.exe --remote=127.0.0.1:8642 --json my_script.lua
```

Minimal script:

```lua
Run({
  imagepath = [[C:\Windows\System32\notepad.exe]],
  callback = function(pid, tid)      -- entry breakpoint: target created, not yet running
    print("pid", pid)
    Detach()                          -- let it run; or use breakpoints below
  end
})
```

## Read this first

| File | When |
| --- | --- |
| `references/cli.md` | CLI flags, `--json` envelope, raw-TCP frames, control commands (`--cancel` / `--kill` / `--detach`) |
| `references/lua-api.md` | every Lua global: `Run`/`Attach`/`SetBreakpoint`/`ReadMemory`/`FindMemory`/`GetContext`/`V()`/`gui.*`/`open_file` |
| `references/pitfalls.md` | the traps that cost the most time (callback-only logic, `ReadMemory` shape, callback file I/O, cancel semantics) |

## Execution model (do not fight it)

- **One invocation = one script.** Strictly serial; no concurrency.
- **`Run` / `Attach` block** until the target exits or your script calls `Detach()`.
  All debugging logic lives in the **callbacks** (`callback`, `on_exception`, `on_load_dll`, ...).
- There is **no step / pause / continue** and no way to inspect a stopped target between calls.
  If you need "let it run a few seconds", sleep inside a callback (`gui.sleep(ms)`) or set breakpoints.
- State across invocations = **the target process itself**, addressed by pid
  (`--pid=`, or `gui.pick()` on the target machine). Nothing else is remembered.

## Recipes

| Recipe | Use for |
| --- | --- |
| `recipes/run-and-break.md` | launch a target, break on a function, dump arguments |
| `recipes/attach-existing.md` | attach to something already running (incl. `gui.pick()` / `--pid`) |
| `recipes/scan-and-patch.md` | find a pattern with `FindMemory`, patch it, verify |
| `recipes/gui-automation.md` | drive a GUI app via the `gui` table (click / type / screenshot) |
| `recipes/remote-agent.md` | the agent loop: `--remote` + `--json` + `--cancel`, long runs, `f:read()` in callbacks |

## Output contract for agents

Use `--json` (or `--out=<path>`) — stdout is exactly **one ASCII JSON line**:

```json
{"ok":true,"exit":0,"canceled":false,"detached":false,"pid":12345,"stdout":"...","stderr":"","images":[],"result":null}
```

- `result` is the script's `return` value (a table becomes JSON; functions become `null`).
- `images` lists files the script wrote (`open_file` write modes, `gui.screenshot_to`).
- `exit` mirrors the process exit code (0 ok / -1 host failure / Lua error codes / 2 launcher).
- Name the files you depend on by **returning them**, e.g. `return { dump = path }`, instead of
  parsing `print` output.

## Ground rules

1. **Don't invent API names.** Everything callable is in `references/lua-api.md`;
   the 29 contract globals are frozen (`Run`, `Attach`, `ReadMemory`, `FindMemory`, ...).
   New capabilities live in the `gui` table or in `open_file`'s file object.
2. **Breakpoints are Lua callbacks**, so keep them short; a slow callback stalls the target.
3. `Detach()` in a callback = "leave it running, give me the pid back" — the pid stays valid for
   the next invocation (NT+; on Win9x it degrades to kill).
4. Never assume a console: under `--json` the target's own stdout is redirected to stderr,
   and `print` goes into the `stdout` field, not to your terminal.
