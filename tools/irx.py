"""Parse, relocate and disassemble PS2 IOP IRX (ET_SCE_IOPRELEXEC) files.

Usage: python3 tools/irx.py info|dis module.IRX [--base 0x10000]
"""
import argparse
import pathlib
import re
import struct
from dataclasses import dataclass

from r3000 import decode, disasm

# Function indices from ps2sdk iop/*/*/src/exports.tab; entries are zero-based.
# Sony CDVD exports and the game-specific TwoFish functions supplement those lists.
NAMES = {
    'sysmem': {4: 'AllocSysMemory', 5: 'FreeSysMemory', 6: 'QueryMemSize', 7: 'QueryMaxFreeMemSize', 8: 'QueryTotalFreeMemSize', 9: 'QueryBlockTopAddress', 10: 'QueryBlockSize', 14: 'Kprintf'},
    'intrman': {4: 'RegisterIntrHandler', 5: 'ReleaseIntrHandler', 6: 'EnableIntr', 7: 'DisableIntr', 17: 'CpuSuspendIntr', 18: 'CpuResumeIntr'},
    'loadcore': {6: 'RegisterLibraryEntries'},
    'sifcmd': {4: 'sceSifInitCmd', 8: 'sceSifSetCmdBuffer', 10: 'sceSifAddCmdHandler', 12: 'sceSifSendCmd', 13: 'isceSifSendCmd', 14: 'sceSifInitRpc', 15: 'sceSifBindRpc', 16: 'sceSifCallRpc', 17: 'sceSifRegisterRpc', 18: 'sceSifCheckStatRpc', 19: 'sceSifSetRpcQueue', 20: 'sceSifGetNextRequest', 21: 'sceSifExecRequest', 22: 'sceSifRpcLoop', 29: 'isceSifSendCmdIntr'},
    'sifman': {5: 'sceSifInit', 7: 'sceSifSetDma', 8: 'sceSifDmaStat'},
    'stdio': {4: 'printf'},
    'sysclib': {6: '_toupper', 7: '_tolower', 8: 'look_ctype_table', 11: 'memcmp', 12: 'memcpy', 13: 'memmove', 14: 'memset', 19: 'sprintf', 20: 'strcat', 21: 'strchr', 22: 'strcmp', 23: 'strcpy', 27: 'strlen', 28: 'strncat', 29: 'strncmp', 30: 'strncpy', 32: 'strrchr', 34: 'strstr', 36: 'strtol', 38: 'strtoul', 42: 'vsprintf'},
    'thbase': {4: 'CreateThread', 5: 'DeleteThread', 6: 'StartThread', 8: 'ExitThread', 20: 'GetThreadId', 22: 'ReferThreadStatus', 23: 'iReferThreadStatus', 24: 'SleepThread', 25: 'WakeupThread', 26: 'iWakeupThread', 33: 'DelayThread', 34: 'GetSystemTime', 39: 'USec2SysClock', 40: 'SysClock2USec', 45: 'ReferThreadRunStatus'},
    'thsemap': {4: 'CreateSema', 5: 'DeleteSema', 6: 'SignalSema', 7: 'iSignalSema', 8: 'WaitSema', 9: 'PollSema', 11: 'ReferSemaStatus'},
    'timrman': {4: 'AllocHardTimer', 6: 'FreeHardTimer', 20: 'SetTimerHandler', 22: 'SetupHardTimer', 23: 'StartHardTimer', 24: 'StopHardTimer'},
    'vblank': {4: 'WaitVblankStart', 5: 'WaitVblankEnd', 8: 'RegisterVblankHandler', 9: 'ReleaseVblankHandler'},
    'ioman': {4: 'io_open', 5: 'io_close', 6: 'io_read', 7: 'io_write', 8: 'io_lseek', 9: 'io_ioctl'},
    'modload': {17: 'ReferModuleStatus', 23: 'SearchModuleByAddress'},
    'cdvdman': {6: 'sceCdRead', 8: 'sceCdGetError', 11: 'sceCdSync', 12: 'sceCdGetDiskType', 13: 'sceCdDiskReady', 28: 'sceCdStatus', 37: 'sceCdCallback'},
    'libsd': {4: 'sceSdInit', 5: 'sceSdSetParam', 6: 'sceSdGetParam', 7: 'sceSdSetSwitch', 8: 'sceSdGetSwitch', 9: 'sceSdSetAddr', 10: 'sceSdGetAddr', 11: 'sceSdSetCoreAttr', 12: 'sceSdGetCoreAttr', 13: 'sceSdNote2Pitch', 14: 'sceSdPitch2Note', 15: 'sceSdProcBatch', 16: 'sceSdProcBatchEx', 17: 'sceSdVoiceTrans', 18: 'sceSdBlockTrans', 19: 'sceSdVoiceTransStatus', 20: 'sceSdBlockTransStatus', 21: 'sceSdSetTransCallback', 22: 'sceSdSetIRQCallback', 23: 'sceSdSetEffectAttr', 24: 'sceSdGetEffectAttr', 25: 'sceSdClearEffectWorkArea', 26: 'sceSdSetTransIntrHandler', 27: 'sceSdSetSpu2IntrHandler'},
    'TwoFish': {6: 'TwoFish_enqueue_read', 7: 'TwoFish_is_complete', 8: 'TwoFish_wait', 9: 'TwoFish_search_file', 10: 'TwoFish_get_version'},
}


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def s16(value):
    return (value ^ 0x8000) - 0x8000


@dataclass
class Table:
    address: int
    name: str
    version: int
    entries: list  # imports: (index, stub address); exports: (index, function address)


class IRX:
    def __init__(self, path):
        self.path = str(path)
        self.file = pathlib.Path(path).read_bytes()
        d = self.file
        if d[:4] != b'\x7fELF' or d[4:7] != b'\x01\x01\x01' or struct.unpack_from('<H', d, 18)[0] != 8:
            raise ValueError('expected little-endian 32-bit MIPS ELF')
        self.entry = u32(d, 24)
        (self.module_id, self.mod_entry, self.gp, self.text_size,
         self.data_size, self.bss_size) = struct.unpack_from('<6I', d, 0x74)
        self.version = struct.unpack_from('<H', d, 0x8c)[0]
        self.name = d[0x8e:d.find(b'\0', 0x8e, 0xc0)].decode('ascii', 'replace')
        self.sections = []
        shoff, shentsize, shnum, shstrndx = u32(d, 32), struct.unpack_from('<H', d, 46)[0], struct.unpack_from('<H', d, 48)[0], struct.unpack_from('<H', d, 50)[0]
        raw = [struct.unpack_from('<10I', d, shoff + j * shentsize) for j in range(shnum)]
        names = raw[shstrndx]
        strtab = d[names[4]:names[4]+names[5]]
        for s in raw:
            n = strtab[s[0]:].split(b'\0', 1)[0].decode('ascii', 'replace') if s[0] < len(strtab) else ''
            self.sections.append((n, *s))
        phoff = u32(d, 28)
        phnum = struct.unpack_from('<H', d, 44)[0]
        memory_size = max((u32(d, phoff + j*32 + 8) + u32(d, phoff + j*32 + 20)
                           for j in range(phnum) if u32(d, phoff + j*32) == 1), default=0)
        self.image = bytearray(max(memory_size, max((s[4] + s[6] for s in self.sections if s[2] & 2), default=0)))
        for s in self.sections:
            name, _, typ, flags, addr, off, size, *_ = s
            if flags & 2 and typ == 1:
                self.image[addr:addr+size] = d[off:off+size]
        self.imports, self.exports = [], []
        # Scan loaded image only; tables can live in .text OR .data.
        for magic, dest in ((0x41e00000, self.imports), (0x41c00000, self.exports)):
            for at in range(0, len(self.image)-20, 4):
                if u32(self.image, at) != magic:
                    continue
                version = struct.unpack_from('<H', self.image, at+8)[0]
                name = self.image[at+12:at+20].split(b'\0')[0].decode('ascii', 'replace')
                if not name or not re.fullmatch('[A-Za-z0-9_]+', name):
                    continue
                entries = []
                p = at + 20
                if dest is self.imports:
                    while p+8 <= len(self.image) and u32(self.image, p) == 0x03e00008 and self.image[p+6:p+8] == b'\x00\x24':
                        entries.append((struct.unpack_from('<H', self.image, p+4)[0], p))
                        p += 8
                else:
                    while p+4 <= len(self.image) and u32(self.image, p) and len(entries) < 512:
                        entries.append((len(entries), u32(self.image, p)))
                        p += 4
                if entries:
                    dest.append(Table(at, name, version, entries))
        self.relocations = []
        for s in self.sections:
            name, _, typ, flags, addr, off, size, *_ = s
            if typ == 9:
                self.relocations += [struct.unpack_from('<II', d, x) for x in range(off, off+size, 8)]

    def relocate(self, base):
        """Return a loaded image at `base`, with REL addends interpreted in place.

        PS2 IRX REL records use symbol zero for the module's load-base delta.
        HI16 may have multiple preceding highs sharing a single LO16.
        """
        result = bytearray(self.image)
        pending = []
        for addr, info in self.relocations:
            kind, symbol = info & 255, info >> 8
            if symbol:
                raise ValueError('external ELF relocation symbol %d at %08x' % (symbol, addr))
            if addr + 4 > len(result):
                raise ValueError('relocation outside image at %08x' % addr)
            word = u32(result, addr)
            if kind == 2:  # R_MIPS_32
                value = (word + base) & 0xffffffff
            elif kind == 4:  # R_MIPS_26
                value = (word & 0xfc000000) | ((((word & 0x3ffffff) << 2) + base >> 2) & 0x3ffffff)
            elif kind == 5:  # R_MIPS_HI16
                pending.append((addr, word))
                continue
            elif kind == 6:  # R_MIPS_LO16
                low = s16(word & 0xffff)
                for high_addr, high_word in pending:
                    full = ((high_word & 0xffff) << 16) + low + base
                    struct.pack_into('<I', result, high_addr, (high_word & 0xffff0000) | (((full + 0x8000) >> 16) & 0xffff))
                pending.clear()
                value = (word & 0xffff0000) | ((low + base) & 0xffff)
            elif kind == 7:  # R_MIPS_GPREL16: both target and GP move by base
                value = word
            else:
                raise ValueError('unsupported MIPS REL type %d at %08x' % (kind, addr))
            struct.pack_into('<I', result, addr, value)
        if pending:
            raise ValueError('unpaired R_MIPS_HI16 at %s' % ', '.join(hex(a) for a, _ in pending))
        return result

    def disassemble(self, base=0):
        image = self.relocate(base)
        stub_names = {base + addr: '%s:%d %s' % (t.name, ix, NAMES.get(t.name, {}).get(ix, '?'))
                      for t in self.imports for ix, addr in t.entries}
        for s in self.sections:
            name, _, typ, flags, start, off, size, *_ = s
            if name != '.text':
                continue
            for at in range(start, start+size, 4):
                pc = base + at
                i = decode(u32(image, at), pc)
                comment = ' ; ' + stub_names[pc] if pc in stub_names else ''
                yield '%08x: %08x  %-38s%s' % (pc, i.word, disasm(i), comment)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('command', choices=('info', 'dis'))
    ap.add_argument('path')
    ap.add_argument('--base', type=lambda s: int(s, 0), default=0)
    a = ap.parse_args()
    irx = IRX(a.path)
    if a.command == 'dis':
        try:
            for line in irx.disassemble(a.base):
                print(line)
        except BrokenPipeError:
            return
    else:
        print('%s: %s version %d.%d module_id %08x entry %08x gp %08x text %x data %x bss %x' %
              (a.path, irx.name, irx.version >> 8, irx.version & 255,
               irx.module_id+a.base, irx.mod_entry+a.base, irx.gp+a.base,
               irx.text_size, irx.data_size, irx.bss_size))
        for label, tables in (('imports', irx.imports), ('exports', irx.exports)):
            print(label + ':')
            for t in tables:
                print('  %08x %-8s %d.%d' % (t.address+a.base, t.name, t.version >> 8, t.version & 255))
                for ix, addr in t.entries:
                    print('    %3d  %08x  %s' % (ix, addr+a.base,
                          NAMES.get(t.name, {}).get(ix, '')))
        print('relocations:', len(irx.relocations))


if __name__ == '__main__':
    main()
