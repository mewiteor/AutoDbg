/* =============================================================================
 * src/common/base_dir.hpp —— 脚本相对路径的「基准目录」（T8 切片 a2 / D63）
 *
 * 职责：回答两件事，两者分开：
 *   ① 纯判定/纯拼接（可单测）：`path_is_absolute(path)`、`join_base_dir(base, path, out, cap)`；
 *   ② 基准目录本身（要读 env / 主 exe 路径）：`base_dir()`。
 * 来源：新写（plan_bootloader.md §5 第 46 行 / §7 的「脚本相对路径基准」条目）。
 * 与旧实现的差异：以前没有任何代码把「基准目录」当独立概念——相对路径就是 CWD，而 `GetModuleFileName
 *   (NULL)`（§11.5 第 5 条）只在排障时被提。handoff 之后 `GetModuleFileName(NULL)` 在 child 里是
 *   tmpdir 那份，基准会漂 ⇒ 主 exe 把原始目录用 env `AUTODBG_BASE_DIR` 传过来，本文件是它的读取点。
 *
 * 已定口径（逐条）：
 *   · env 名字大写 `AUTODBG_BASE_DIR`（D63），不占 `autodbg_boot` 字段 ⇒ ABI 保持 5；
 *   · env 值**不带尾分隔符**（已定），拼接时由本文件补；
 *   · env 取不到（不是 handoff 来的，或单进程本地模式）→ 回退主 exe 所在目录；两处都取不到 → 空串，
 *     调用方必须按「不解析、原样用」处理（不是失败）；
 *   · 绝对路径原样透传：盘符式 `X:`（含盘符相对 `C:foo`）与 UNC `\\server\share` 都算绝对；
 *   · 只解析「脚本给的那一份相对路径」——`images` 登记表与 `tostring` 的口径不变（仍报调用方的原串）。
 *
 * 为什么在 `common/`：判定这半截是纯函数（不 I/O、不碰 Lua），消费它的是绑定层（`file_stream.cpp`、
 *   `gui_bindings.cpp`）与 `selftest.cpp`；`base_dir()` 那半截要 `GetEnvironmentVariableA` /
 *   `GetModuleFileNameA`（全 w95 有，直接链），所以整份归 `common/`，与 `console_action.*` 同一条理由。
 * ========================================================================== */
#ifndef AUTODBG_COMMON_BASE_DIR_HPP
#define AUTODBG_COMMON_BASE_DIR_HPP

namespace autodbg::common
{
/* ---- 纯函数（selftest 断言的就是这两条） ---- */

/* 绝对路径判据：盘符式（`C:\x`、`C:/x`、`C:foo`）或 UNC（`\\server\share`）。 */
bool path_is_absolute(const char *path);

/* 把 `path` 按基准目录拼进 `out`：`path` 是绝对路径 → 原样复制；否则 `base` + 分隔符 + `path`。
 * `base` 为空或结果放不下 → 返回 false（不写半个结果）。`base` 已带尾分隔符时不重复补。 */
bool join_base_dir(const char *base, const char *path, char *out, unsigned cap);

/* ---- 基准目录（要读 env / 主 exe 路径，首次调用后缓存） ----
 * 优先级：env `AUTODBG_BASE_DIR` > 主 exe 所在目录；两者都拿不到 → 空串。
 * 返回值不带尾分隔符；调用方自己决定「相对路径要不要解析」。 */
const char *base_dir();

} /* namespace autodbg::common */

#endif /* AUTODBG_COMMON_BASE_DIR_HPP */
