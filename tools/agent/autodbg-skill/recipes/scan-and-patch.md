# Recipe：特征扫描 → 改内存 → 验证

场景：你要在一个大进程里找到某段特征码（比如某个值、某个指令序列），改掉它并确认生效。

## 1. 先看清内存布局（别全量瞎扫）

```lua
for _, r in ipairs(GetMemoryRegion()) do
  -- BaseAddress / RegionSize / State / Protect / Type
  if r.Protect == "PAGE_READWRITE" or r.Protect == "PAGE_READONLY" then
    print(string.format("%016X  size=%X  %s", tonumber(r.BaseAddress), tonumber(r.RegionSize), r.Protect))
  end
end
```

## 2. 扫特征（`FindMemory` 支持 `??` 通配）

```lua
-- scan_patch.lua
local pattern = "48 8B 05 ?? ?? ?? ?? 48 85 C0"    -- x64 例子
local found, patched = 0, 0

for addr, hex in FindMemory(pattern) do
  found = found + 1
  print(string.format("hit #%d at 0x%X", found, addr))

  -- 只改前 N 处，避免把进程改坏
  if found <= 3 then
    local old = ReadMemory(V(addr), U32(9))     -- ❗返回 table of bytes（1..n）
    WriteMemory(V(addr), { 0x90, 0x90, 0x90 })  -- NOP 掉前三个字节
    patched = patched + 1
    print("  patched, first byte was", old[1])
  end
end

print(string.format("found=%d patched=%d", found, patched))
return { found = found, patched = patched }
```

要点：

- `ReadMemory(addr, size)` 的 `addr` / `size` 都是 **LuaVariant**（`V(x)` / `U32(n)`），返回
  **字节 table**（不是字符串）。
- 扫描是**逐区域**的：大进程会慢。想要快就先 `GetMemoryRegion()` 过滤（或者用
  `Run` 的模块加载事件只扫目标模块）。
- 改代码页（`PAGE_EXECUTE_*`）前确认目标已经跑起来了；必要时先 `SetBreakpoint` 让它在安全点停一下。

## 3. 拿模块内地址（ASLR 下的正确姿势）

```lua
local base = GetModBase("target.exe")           -- 模块未加载 → nil
if not base then error("module not loaded") end

local rva  = FindFuncRva("target.exe", "SomeFunc")     -- 导出函数的 RVA
local ea   = U64(tonumber(base) + tonumber(rva))       -- 绝对地址

local mod, rva2 = Ea2ModnameRva(addr)                -- 反过来：ea -> modname + rva（两个返回值）
print(mod, rva2)
```

## 4. 验证（别只看返回值）

```lua
-- 重新读一遍确认写进去了
local after = ReadMemory(V(addr), U32(0x10000))   -- 注意：上限/类型都是 LuaVariant
print(string.format("%02X %02X %02X", after[1], after[2], after[3]))   -- 期望 90 90 90
```

更稳的做法：改完之后**继续跑**目标，用一个断点/日志确认行为变化，再把结论 `return` 出去：

```lua
return { found = found, patched = patched, verified = (after[1] == 0x90) }
```

`--json` 下这个表会出现在 `result` 字段里，代理不用解析 `print` 散文。
