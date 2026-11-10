#ifndef AUTODBG_SYS_VERSION_H
#define AUTODBG_SYS_VERSION_H
/* =============================================================================
 * src/sys/version.h —— 版本宏（构建期生成 + 缺省兜底）
 *
 * 版本号来源（见 make.bat 的 `:genversion`）：`git describe --tags --always --dirty`
 *   · 已打 tag 且 HEAD 正好在 tag 上 → `v0.1.0`
 *   · 打过 tag 但 HEAD 领先 N 个提交   → `v0.1.0-3-g1fa399c`
 *   · 还没有任何 tag（当前情况）      → `1fa399c`（= 提交短哈希）
 *   · 工作区有改动                    → 追加 `-dirty`
 *
 * 生成物是 `<build>/gen/autodbg_build.h`（每个架构/模式一份，由 make.bat 写，
 * 内容没变就不碰文件，避免每次都全量重建）。这里只做 include + 兜底：
 * 没走 make.bat 的构建（或用其它构建系统）自动降级成 "unknown"，不会编译失败。
 *
 * 宏：
 *   AUTODBG_VERSION        版本串（tag 或 commit，可能带 -dirty）
 *   AUTODBG_VERSION_SOURCE "tag" | "describe" | "commit" | "unknown"
 *   AUTODBG_VERSION_DIRTY  1 = 工作区有未提交改动
 *   AUTODBG_VERSION_ARCH   "x86" | "x64"（编译期按 _WIN64 决定）
 * ========================================================================== */
#if defined(__has_include)
#   if __has_include("autodbg_build.h")
#       include "autodbg_build.h"
#   endif
#endif

#ifndef AUTODBG_VERSION
#   define AUTODBG_VERSION "unknown"
#endif
#ifndef AUTODBG_VERSION_SOURCE
#   define AUTODBG_VERSION_SOURCE "unknown"
#endif
#ifndef AUTODBG_VERSION_DIRTY
#   define AUTODBG_VERSION_DIRTY 0
#endif
#ifndef AUTODBG_VERSION_ARCH
#   ifdef _WIN64
#       define AUTODBG_VERSION_ARCH "x64"
#   else
#       define AUTODBG_VERSION_ARCH "x86"
#   endif
#endif

#endif /* AUTODBG_SYS_VERSION_H */
