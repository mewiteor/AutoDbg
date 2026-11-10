from pefile import PE
import pickle
import sys
import os

def usage(machine: int = 0):
    if machine == 0x14C:
        print(f'usage: {sys.argv[0]} <exe/dll path> <target_os>')
        print('    target_os support: w95,w98,w982,wxp')
    elif machine == 0x8664:
        print(f'usage: {sys.argv[0]} <exe/dll path>')
    else:
        print(f'usage: {sys.argv[0]} <exe/dll path> [target_os]')
        print('    x86 target_os support: w95,w98,w982,wxp')
        print('    x64 target_os only support: w7, so not need to input target_os')

def main():
    if len(sys.argv) < 2:
        usage()
        return

    cwd,_ = os.path.split(__file__)
    with open(os.path.join(cwd, 'datas.bin'), 'rb') as f:
        datas = pickle.load(f)

    pe = PE(sys.argv[1], fast_load=True)
    if pe.FILE_HEADER.Machine == 0x14C:
        # x86
        if len(sys.argv) != 3:
            usage(pe.FILE_HEADER.Machine)
            return
        target_os = sys.argv[2]
        if target_os not in ['w95','w98','w982','wxp']:
            usage(pe.FILE_HEADER.Machine)
            return
    elif pe.FILE_HEADER.Machine == 0x8664:
        # x64
        if len(sys.argv) != 2:
            usage(pe.FILE_HEADER.Machine)
            return
        target_os = 'w7'
    else:
        print(f'unsupport machine: {pe.FILE_HEADER.Machine}')
        return
    cur_datas = datas[target_os]
    pe.parse_data_directories(1)
    for dllinfo in pe.DIRECTORY_ENTRY_IMPORT:
        dllname = dllinfo.dll.decode().lower()
        if dllname not in cur_datas:
            print(dllname)
            continue
        cur_syms = cur_datas[dllname]
        for x in dllinfo.imports:
            name = x.name.decode()
            if name not in cur_syms:
                print(f'{dllname} {name}')
    # imp = pe.OPTIONAL_HEADER.DATA_DIRECTORY[1]
    # pe.parse_import_directory(imp.VirtualAddress, imp.Size)


if __name__ == "__main__":
    main()
