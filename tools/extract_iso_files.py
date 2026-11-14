#!/usr/bin/env python3
"""Extract files from a PS2 ISO9660 image.

usage: extract_iso_files.py <game.iso> <out dir> <path on disc>...

Each file is written to <out dir>/<its file name, upper case>.  Unchanged
outputs are not rewritten, so their timestamps (and dependent builds) stay put.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_elf import SECTOR, find, read  # noqa: E402


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip())
    iso, out_dir, paths = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(out_dir, exist_ok=True)
    with open(iso, 'rb') as f:
        pvd = read(f, 16, SECTOR)
        if pvd[1:6] != b'CD001':
            sys.exit('not an ISO9660 image: %s' % iso)
        root_lba, root_size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
        for path in paths:
            ent = find(f, root_lba, root_size, path)
            if not ent:
                print('missing on disc: %s' % path, file=sys.stderr)
                sys.exit(1)
            data = read(f, ent[0], ent[1])
            out = os.path.join(out_dir, os.path.basename(path.replace('\\', '/')).split(';')[0].upper())
            old = None
            try:
                with open(out, 'rb') as fo:
                    old = fo.read()
            except OSError:
                pass
            if old != data:
                with open(out, 'wb') as fo:
                    fo.write(data)


if __name__ == '__main__':
    main()
