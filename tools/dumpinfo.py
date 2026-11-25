# -*- coding: utf-8 -*-
"""极简 minidump 解析：取异常码、异常地址，并映射到模块（用于判断崩溃是否与本次改动有关）。

用法：python tools/dumpinfo.py <dump.dmp>
"""
import struct
import sys

MODULES, THREADS, EXCEPTION = 4, 3, 6


def main(path):
    data = open(path, 'rb').read()
    sig, ver, nstreams, dir_rva, checksum, ts, flags = struct.unpack_from('<IIIIIIQ', data, 0)
    assert sig == 0x504D324D, 'not a minidump'
    dirs = []
    for i in range(nstreams):
        off = dir_rva + i * 12
        stype, size, rva = struct.unpack_from('<III', data, off)
        dirs.append((stype, size, rva))

    # 模块表
    modules = []
    for stype, size, rva in dirs:
        if stype != MODULES:
            continue
        count = struct.unpack_from('<I', data, rva)[0]
        p = rva + 4
        for _ in range(count):
            base, msize = struct.unpack_from('<QQ', data, p)
            name_rva = struct.unpack_from('<I', data, p + 8 + 24 + 8)[0] if False else None
            # MINIDUMP_MODULE: BaseOfImage(8) SizeOfImage(8) CheckSum(4) TimeDateStamp(4)
            # ModuleNameRva(4) VS_FIXEDFILEINFO(52) CvRecord(8) MiscRecord(8) ...
            name_rva = struct.unpack_from('<I', data, p + 24)[0]
            nlen = struct.unpack_from('<I', data, name_rva)[0]
            name = data[name_rva + 4:name_rva + 4 + nlen].decode('utf-16-le', 'replace')
            modules.append((base, msize, name))
            p += 108

    def mod_of(addr):
        for base, size, name in modules:
            if base <= addr < base + size:
                return name
        return '<unknown>'

    for stype, size, rva in dirs:
        if stype != EXCEPTION:
            continue
        tid, align = struct.unpack_from('<II', data, rva)
        code, flags2, rec, addr, nparams = struct.unpack_from('<IIQQI', data, rva + 8)
        print(f'exception code=0x{code:08X} flags=0x{flags2:08X} address=0x{addr:016X}')
        print(f'  module = {mod_of(addr)}')
        params = struct.unpack_from('<15Q', data, rva + 8 + 4 + 4 + 8 + 8)
        for i in range(min(nparams, 4)):
            print(f'  param[{i}] = 0x{params[i]:016X} ({mod_of(params[i])})')

    if not modules:
        print('no modules')
    print('--- modules ---')
    for base, size, name in modules:
        print(f'  0x{base:016X} 0x{size:08X} {name}')


if __name__ == '__main__':
    main(sys.argv[1])
