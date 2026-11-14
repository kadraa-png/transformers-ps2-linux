#!/usr/bin/env python3
"""Extract the boot executable named by SYSTEM.CNF from a PS2 ISO9660 image.

usage: extract_elf.py <game.iso> <out.elf>
"""
import struct
import sys

SECTOR = 2048


def read(f, lba, size):
    f.seek(lba * SECTOR)
    return f.read(size)


def find(f, root_lba, root_size, path):
    lba, size, is_dir = root_lba, root_size, True
    for comp in [c for c in path.replace('\\', '/').split('/') if c]:
        comp = comp.split(';')[0].upper().rstrip('.')
        if not is_dir:
            return None
        data = read(f, lba, size)
        pos, found = 0, None
        while pos < len(data):
            n = data[pos]
            if n == 0:
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + n]
            nlen = rec[32]
            name = rec[33:33 + nlen].decode('latin1').split(';')[0].upper().rstrip('.')
            if name == comp:
                found = (struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0],
                         bool(rec[25] & 2))
                break
            pos += n
        if not found:
            return None
        lba, size, is_dir = found
    return lba, size


def main():
    iso, out = sys.argv[1], sys.argv[2]
    with open(iso, 'rb') as f:
        pvd = read(f, 16, SECTOR)
        if pvd[1:6] != b'CD001':
            sys.exit('not an ISO9660 image: %s' % iso)
        root_lba, root_size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
        cnf = find(f, root_lba, root_size, 'SYSTEM.CNF')
        if not cnf:
            sys.exit('SYSTEM.CNF not found')
        text = read(f, cnf[0], cnf[1]).decode('latin1')
        boot = None
        for line in text.splitlines():
            if line.strip().upper().startswith('BOOT2'):
                boot = line.split('=', 1)[1].strip()
        if not boot:
            sys.exit('no BOOT2 in SYSTEM.CNF')
        path = boot.split(':', 1)[1] if ':' in boot else boot
        ent = find(f, root_lba, root_size, path)
        if not ent:
            sys.exit('boot file %s not found' % boot)
        data = read(f, ent[0], ent[1])
    old = None
    try:
        old = open(out, 'rb').read()
    except OSError:
        pass
    if old != data:
        with open(out, 'wb') as fo:
            fo.write(data)
    print('extracted %s (%d bytes) -> %s' % (boot, len(data), out))


if __name__ == '__main__':
    main()
