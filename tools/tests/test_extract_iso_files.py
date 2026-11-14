#!/usr/bin/env python3
"""Tests for tools/extract_iso_files.py.  Run: python3 -m unittest tools/tests/test_extract_iso_files.py"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
REPO = os.path.join(TOOLS, '..')
SCRIPT = os.path.join(TOOLS, 'extract_iso_files.py')
SECTOR = 2048


def dir_record(name, lba, size, is_dir):
    ident = name.encode('latin1')
    n = 33 + len(ident)
    n += n & 1
    rec = bytearray(n)
    rec[0] = n
    struct.pack_into('<I', rec, 2, lba)
    struct.pack_into('>I', rec, 6, lba)
    struct.pack_into('<I', rec, 10, size)
    struct.pack_into('>I', rec, 14, size)
    rec[25] = 2 if is_dir else 0
    rec[32] = len(ident)
    rec[33:33 + len(ident)] = ident
    return bytes(rec)


def make_iso(path, payload):
    """Root (LBA 18) holds directory IOP (LBA 19), which holds A.IRX;1 (LBA 20)."""
    img = bytearray(SECTOR * 21)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[156:156 + 34] = dir_record('\0', 18, SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    root = dir_record('\0', 18, SECTOR, True) + dir_record('\1', 18, SECTOR, True) + \
        dir_record('IOP', 19, SECTOR, True)
    img[18 * SECTOR:18 * SECTOR + len(root)] = root
    iop = dir_record('\0', 19, SECTOR, True) + dir_record('\1', 18, SECTOR, True) + \
        dir_record('A.IRX;1', 20, len(payload), False)
    img[19 * SECTOR:19 * SECTOR + len(iop)] = iop
    img[20 * SECTOR:20 * SECTOR + len(payload)] = payload
    with open(path, 'wb') as f:
        f.write(img)


def run(*args):
    return subprocess.run([sys.executable, SCRIPT, *args], capture_output=True, text=True)


class ExtractIsoFiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.iso = os.path.join(self.tmp.name, 'disc.iso')
        self.out = os.path.join(self.tmp.name, 'out')
        make_iso(self.iso, b'hello')

    def tearDown(self):
        self.tmp.cleanup()

    def test_extracts_file(self):
        r = run(self.iso, self.out, 'IOP/A.IRX')
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(os.path.join(self.out, 'A.IRX'), 'rb') as f:
            self.assertEqual(f.read(), b'hello')

    def test_missing_file_fails(self):
        r = run(self.iso, self.out, 'IOP/NOPE.IRX')
        self.assertEqual(r.returncode, 1)
        self.assertIn('missing on disc: IOP/NOPE.IRX', r.stderr)

    @unittest.skipUnless(os.path.exists(os.path.join(REPO, 'Transformers.iso')) and
                         os.path.exists(os.path.join(REPO, 'iso/extract/IOP/LIBSD.IRX')),
                         'needs Transformers.iso and the hand-extracted iso/extract/IOP')
    def test_real_disc_matches_hand_extract(self):
        mods = ['TWOFISH.IRX', 'LIBSD.IRX', 'MODMIDI.IRX', 'SIRENS3.IRX']
        r = run(os.path.join(REPO, 'Transformers.iso'), self.out, *['IOP/' + m for m in mods])
        self.assertEqual(r.returncode, 0, r.stderr)
        for m in mods:
            with open(os.path.join(self.out, m), 'rb') as a, \
                    open(os.path.join(REPO, 'iso/extract/IOP', m), 'rb') as b:
                self.assertEqual(a.read(), b.read(), m)


if __name__ == '__main__':
    unittest.main()
