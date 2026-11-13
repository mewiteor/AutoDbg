# AutoDbg agent skill

这个目录是一份 **Agent Skills** 格式的技能包（`SKILL.md` + `references/` + `recipes/`），
给 coding agent 用：它教模型「怎么给 AutoDbg 写 Lua 脚本、怎么用 CLI 驱动、哪里有坑」。

## 结构

```text
autodbg-skill/
├── SKILL.md                  # 入口：执行模型、快速路径、路由表
├── references/
│   ├── cli.md                # 选项 / --repl / --json 契约 / raw TCP 帧表（协议 2）
│   ├── lua-api.md            # dbg 会话对象 + dbglib + help/caps，然后 29 个契约全局 + Var/LuaVariant + gui + open_file
│   └── pitfalls.md           # 最费时间的坑（回调、ReadMemory、文件 I/O、cancel 三档）
└── recipes/
    ├── run-and-break.md      # 启动 + 断点抓参数（Run + 回调）
    ├── stop-and-inspect.md   # dbg：两个会话同时停住 → 各读 context/内存 → 收尾
    ├── attach-existing.md    # Attach 已在跑的进程（--pid / gui.pick）
    ├── scan-and-patch.md     # FindMemory -> WriteMemory -> 验证
    ├── gui-automation.md     # gui.* 控件 / 输入 / 截图
    └── remote-agent.md       # --remote + --json + --cancel 的代理循环
```

## 怎么让 agent 看到它

- **pi**：`~/.pi/agent/settings.json`（或项目级设置）里加一条
  `{ "skills": ["<repo>/tools/agent/autodbg-skill"] }`；之后 `/skill:autodbg` 可强制加载，
  模型也会在「调试 Windows 进程 / 自动化 GUI / 写 AutoDbg 脚本」时自动匹配。
- **其它 agent**：整个目录烤进上下文即可（先读 `SKILL.md`，需要细节再读 `references/`）。

## 维护约定

- 这份文档是 **CLI 口径下的唯一对外契约文档**：改绑定层（`registry.cpp` / `bindings.cpp` /
  `dbg_bindings.cpp` / `help_bindings.cpp` / `gui_bindings.cpp` / `file_stream.cpp`）时要同步
  `references/lua-api.md`；改 `lualib/dbglib.lua` 要同步它的「dbglib」一节；`help()` 的正文
  在 `help_bindings.cpp` 的 `kHelp[]`（一处文本），改它时对照这里的主题名。
- 改 CLI 选项或帧协议时同步 `references/cli.md`；新增踩过的坑先记仓库根目录 `PITFALLS.md`，
  再把「写脚本的人会撞上的」那几条挑到 `references/pitfalls.md`。
- 本目录是纯文档，不参与构建；`references/lua-api.md` 的正文源自
  `lualib/autodbg_api.lua`（T5 随 MCP 删除，内容搬到这里）。
