"""批量导入表审计（按项目口径自动选目标系统）。

用途：改完代码后一次性检查 `.out` 下**全部**产物的导入表，作为「Win9x / XP / Win7
兼容性」的回归门禁（配合 `main.py` 的单文件检查）。

auto 口径（与交付面一致，见 AGENTS.md §11.1 / §11.4）：
    - x64（machine 0x8664）        → w7
    - 名字含 `Legacy`（x86 老系统变体）→ w95   （Win95 = 最老的交付目标）
    - 启动器 `AutoDbg_<arch>.exe`  → w95   （它要在 9x 上跑起来才能选 legacy）
    - `.out/fixtures/` 下的产物（被调试目标 fixture，T7.2）→ w95   （fixture 里禁止 NT-only 调用）
    - 其余 x86（modern 变体 / 构建期 host）→ wxp

用法：
    uv --directory tools/api_check run scan_dir.py [目录] [--os w95|w98|w982|wxp|w7]
    # 目录缺省为 ../../.out；--os 强制单一目标系统（诊断用）
    # 递归扫子目录（fixture 落在 .out/fixtures/ 下）；只看 .exe / .dll

退出码：0 = 全部通过；1 = 有不可用导入；2 = 用法/路径错误。

输出格式（每行一个问题，便于 grep / diff）：
    <file> [<os>] <dll> <symbol>          —— 缺符号
    <file> [<os>] <dll>                   —— 整条 DLL 不在目标系统里
"""
import os
import pickle
import sys

from pefile import PE

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DIR = os.path.normpath(os.path.join(HERE, '..', '..', '.out'))
SUPPORTED_OS = ('w95', 'w98', 'w982', 'wxp', 'w7')

IMAGE_FILE_MACHINE_I386 = 0x14C
IMAGE_FILE_MACHINE_AMD64 = 0x8664
FIXTURE_DIRNAME = 'fixtures'


def load_datas():
    with open(os.path.join(HERE, 'datas.bin'), 'rb') as f:
        return pickle.load(f)


def pick_target_os(path, machine):
    """按 auto 口径给一个产物选目标系统。"""
    name = os.path.basename(path)
    if machine == IMAGE_FILE_MACHINE_AMD64:
        return 'w7'
    # fixture（被调试目标）要在最老的交付目标上跑 ⇒ w95：fixture 源码里禁止任何 XP+ 才有的调用
    if os.path.basename(os.path.dirname(path)).lower() == FIXTURE_DIRNAME:
        return 'w95'
    # 老系统变体与启动器都要能在 Win95 上跑；modern 变体（含构建期 host）只需 XP+
    if name.startswith('AutoDbgLegacy_') or name in ('AutoDbg_86.exe', 'AutoDbg_64.exe'):
        return 'w95'
    return 'wxp'


def scan_one(path, target_os, datas):
    """返回 (目标系统, 问题列表[(dll, sym|None), ...])；非 PE / 不支持架构返回 None。"""
    try:
        pe = PE(path, fast_load=True)
    except Exception:
        return None
    machine = pe.FILE_HEADER.Machine
    if machine not in (IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_AMD64):
        return None
    if target_os is None:
        target_os = pick_target_os(path, machine)
    cur_datas = datas[target_os]
    pe.parse_data_directories(1)
    problems = []
    for dllinfo in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        dllname = dllinfo.dll.decode().lower()
        if dllname not in cur_datas:
            problems.append((dllname, None))
            continue
        cur_syms = cur_datas[dllname]
        for x in dllinfo.imports:
            if x.name is None:            # 按序号导入：无法做名字审计，跳过
                continue
            name = x.name.decode()
            if name not in cur_syms:
                problems.append((dllname, name))
    pe.close()
    return target_os, problems


def main():
    argv = sys.argv[1:]
    target_dir = DEFAULT_DIR
    force_os = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--os':
            i += 1
            if i >= len(argv) or argv[i] not in SUPPORTED_OS:
                print('--os 只支持: ' + ', '.join(SUPPORTED_OS))
                return 2
            force_os = argv[i]
        elif a in ('-h', '--help'):
            print(__doc__)
            return 0
        else:
            target_dir = a
        i += 1

    if not os.path.isdir(target_dir):
        print(f'目录不存在: {target_dir}')
        return 2

    datas = load_datas()
    # 递归：fixture 落在 .out/fixtures/ 下，只看一层目录就不会被计数（门禁口径 12 → 13）。
    # 只看 .exe / .dll：`.bin` 不会被扫（`.bin` 要不要扫属于 plan_bootloader.md 的
    # 遗留项），本片不改这条。
    files = sorted(
        os.path.join(root, n)
        for root, _, names in os.walk(target_dir)
        for n in names
        if n.lower().endswith(('.exe', '.dll'))
    )

    failed = 0
    for path in files:
        r = scan_one(path, force_os, datas)
        if r is None:
            continue
        target_os, problems = r
        if problems:
            failed += 1
            for dll, sym in problems:
                line = f'{os.path.basename(path)} [{target_os}] {dll}'
                if sym:
                    line += f' {sym}'
                print(line)

    total = len(files)
    if failed:
        print(f'--- {failed}/{total} 个产物存在目标系统不可用的导入')
        return 1
    print(f'--- {total} 个产物全部通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())
