#!/usr/bin/env python3
"""Localize a VU image shard object: keep its vu_img*_x entry points (listed
in the .keep file) and its weak/COMDAT symbols global, make every other
defined symbol local.

Weak symbols (inline functions, template instances) must stay global: the
linker deduplicates their COMDAT groups across objects, and a localized
reference into a discarded group fails to link.  They are identical in every
object, so sharing them is harmless.  The strong definitions (the private copy
of VUops.cpp's opcode bodies) are what would clash with ps2_vu's own.

usage: localize_shard.py OBJCOPY NM KEEP_FILE IN.o OUT.o
"""
import subprocess
import sys


def main() -> None:
    objcopy, nm, keep_file, src, dst = sys.argv[1:6]
    keep = set(open(keep_file).read().split())
    listing = subprocess.run([nm, '--defined-only', src], check=True,
                             capture_output=True, text=True).stdout
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'WVu':
            keep.add(parts[2])
    keep_all = dst + '.keep'
    with open(keep_all, 'w') as f:
        f.write('\n'.join(sorted(keep)) + '\n')
    subprocess.run([objcopy, f'--keep-global-symbols={keep_all}', src, dst], check=True)


if __name__ == '__main__':
    main()
