---
name: autodbg-skill
description: Write and run AutoDbg Lua scripts to debug Windows processes (breakpoints, memory read/write, PE/symbols, memory scanning, GUI automation). Use when debugging a live Windows target, automating a GUI program, or scripting Win32 debugging from an agent.
---

# AutoDbg

AutoDbg is a headless, scriptable Windows debugger (Win32 Debug API). You drive it with a **Lua script**; the script is the whole program — there is no interactive prompt (except `--repl`) and no "paused target" state that survives between invocations.

**Main face** (everything new is lowercase, D52): the **`dbg` session object** — `dbg.create{...}` /
`dbg.attach(pid)` → a session; `d:pump()` gets a **plain-table** event and **does not continue** by
default, `d:cont()` / `d:step()` drive it, `d:context()` / `d:read()` inspect the stopped target,
`d:detach()` / `d:kill()` finish. Multi-process scripts use `dbg.pump` (process-wide) or the
`dbglib` sugar (`wait_all` / `run_all`). Ask the runtime what this build has: `dbg.caps()`;
human prose: `help([topic])` / `dbg.help()`. Interactive Lua from stdin: `--repl`.

**Pointer chains**: `V(addr)` works **only inside a debug callback** (top level / REPL: it returns
`nil, msg`, and in the REPL `V` is simply `nil`). At top level use the session-bound **`d:v(addr)`**:
`d:v(base)[0x10]:to('I')`. On a `dbg.remote` proxy neither exists — use `d:read_u32` / `d:read_u64`.

## Fast path

```bat
:: drive a local target
.out\AutoDbg_64.exe --json my_script.lua

:: interactive Lua from stdin — no script needed (statements, `=` shorthand, multi-line;
:: `--repl --server` keeps one lua_State alive across commands)
.out\AutoDbg_64.exe --repl

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

Same thing with the `dbg` session object (stop, look, continue, finish):

```lua
local d = dbg.create{ imagepath = [[C:\Windows\System32\notepad.exe]] }
d:arm('notepad.exe', 0x1234, function(ev) print('hit', ev.ea) end)  -- 0x1234 = placeholder RVA
local ev = d:pump(3000)                     -- blocks up to 3 s; leaves the event unanswered
if ev then
  print(ev.type, ev.tid, 'eip=' .. d:context().Cip.hex)  -- address-class values are LuaVariant: .hex / .val
  d:cont()                                  -- default = ev.cont_code
end
if d.detach then d:detach() else d:kill() end
```

## Read this first

| File | When |
| --- | --- |
| `references/cli.md` | CLI flags, `--repl`, `--json` envelope, raw-TCP frames (proto **2**, `event` frame), control commands (`--cancel` / `--kill` / `--detach`) |
| `references/lua-api.md` | every Lua global: **`dbg` session objects + `dbglib`**, `V`/`d:v` pointer chains, `help()`/`dbg.caps()`, then the 29 frozen names `Run`/`Attach`/`SetBreakpoint`/`ReadMemory`/`FindMemory`/`GetContext`/`V()`/`gui.*`/`open_file` |
| `references/pitfalls.md` | the traps that cost the most time (callback-only logic, `ReadMemory` shape, callback file I/O, cancel semantics) |

## Execution model (do not fight it)

- **One invocation = one script.** Strictly serial; no concurrency.
- **`Run` / `Attach` block** until the target exits or your script calls `Detach()`.
  All debugging logic lives in the **callbacks** (`callback`, `on_exception`, `on_load_dll`, ...).
- **Need to stop and look?** That is `dbg`, not `Run`: `d:pump()` leaves the event **unanswered**
  (`d.stop` = the same table), so `d:context()` / `d:read()` see a stopped target; `d:cont()` /
  `d:step()` move on. `Run` / `Attach` have no step/pause. Two sessions (two processes) in one
  script are driven by **one** `dbg.pump` loop — the event stream is process-wide.
- There is no way to inspect a stopped target **between** invocations, and no way to resume one.
  If you need "let it run a few seconds", sleep inside a callback (`gui.sleep(ms)`) or set breakpoints.
- State across invocations = **the target process itself**, addressed by pid
  (`--pid=`, or `gui.pick()` on the target machine). Nothing else is remembered.

## Recipes

| Recipe | Use for |
| --- | --- |
| `recipes/run-and-break.md` | launch a target, break on a function, dump arguments (`Run` + callbacks) |
| `recipes/stop-and-inspect.md` | **`dbg`**: two sessions stopped at the same time, read each `context()` / memory, then finish |
| `recipes/attach-existing.md` | attach to something already running (incl. `gui.pick()` / `--pid`) |
| `recipes/scan-and-patch.md` | find a pattern with `FindMemory`, patch it, verify |
| `recipes/gui-automation.md` | drive a GUI app via the `gui` table (click / type / screenshot) |
| `recipes/remote-agent.md` | the agent loop: `--remote` + `--json` + `--cancel`, long runs, `f:read()` in callbacks |
| `recipes/cross-arch-remote.md` | a **32-bit** host driving a **64-bit** target: `dbg.remote` proxy over a `--repl --server` on the other side |

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
   the 29 contract globals are frozen (`Run`, `Attach`, `ReadMemory`, `FindMemory`, ...) —
   new scripts should use the `dbg` session object instead.
   New capabilities live in `dbg`, the `gui` table, or `open_file`'s file object.
2. **`dbg` events are plain tables and do not auto-continue.** `d:pump()` returns the same table as
   `d.stop`; answer it with `d:cont()` (default = `ev.cont_code`, **not** `ev.code`).
   `ev.code` is the exception code; `cont_code` is the reply code.
3. **Breakpoints are Lua callbacks**, so keep them short; a slow callback stalls the target.
4. `Detach()` in a callback = "leave it running, give me the pid back" — the pid stays valid for
   the next invocation (NT+; on Win9x it degrades to kill). With `dbg`, `d:detach()` exists only
   where the platform has it (`d.detach == nil` on 9x) — test the member, don't `pcall` it.
5. Never assume a console: under `--json` the target's own stdout is redirected to stderr,
   and `print` goes into the `stdout` field, not to your terminal.
