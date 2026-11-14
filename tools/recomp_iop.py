#!/usr/bin/env python3
"""Relocate PS2 IRX files and emit R3000A C++ into build/generated_iop.

usage: python3 tools/recomp_iop.py [--out build/generated_iop] [IRX ...]
The default inputs are the four game modules; load bases are fixed by the IOP
memory map. Other modules require an explicit --base and a single input.
"""
import argparse
import pathlib
import re

from irx import IRX, Table, u32
from r3000 import decode

BASES = {'TWOFISH': 0x10000, 'LIBSD': 0x30000,
         'MODMIDI': 0x40000, 'SIRENS3': 0x50000}
DEFAULT = tuple(pathlib.Path('iso/extract/IOP') / (n + '.IRX') for n in BASES)


def write_if_changed(path, text):
    if path.exists() and path.read_text() == text:
        return
    path.write_text(text)


def hex32(value):
    return '0x%08xu' % (value & 0xffffffff)


def signed(expr):
    return 'static_cast<s32>(%s)' % expr


def reg(n):
    return '0u' if n == 0 else 'c.r[%d]' % n


def put(n, expression):
    return 'c.r[%d] = %s;' % (n, expression) if n else ''


def operation(i):
    o, a, b, d, sh = i.op, reg(i.rs), reg(i.rt), i.rd, i.sa
    addr = '(%s + %s)' % (a, hex32(i.simm))
    if o == 'nop':
        return ''
    if o in ('add', 'addu', 'sub', 'subu'):
        return put(d, '(%s %s %s)' % (a, '-' if o.startswith('sub') else '+', b))
    if o in ('addi', 'addiu'):
        return put(i.rt, '(%s + %s)' % (a, hex32(i.simm)))
    if o in ('and', 'or', 'xor', 'nor'):
        v = '(%s %s %s)' % (a, {'and': '&', 'or': '|', 'xor': '^', 'nor': '|'}[o], b)
        return put(d, '~%s' % v if o == 'nor' else v)
    if o in ('andi', 'ori', 'xori'):
        return put(i.rt, '(%s %s %s)' % (a, {'andi': '&', 'ori': '|', 'xori': '^'}[o], hex32(i.imm)))
    if o == 'lui':
        return put(i.rt, hex32(i.imm << 16))
    if o in ('slti', 'sltiu'):
        return put(i.rt, '(%s < %s ? 1u : 0u)' % ((signed(a), str(i.simm)) if o == 'slti' else (a, hex32(i.simm))))
    if o in ('slt', 'sltu'):
        return put(d, '(%s < %s ? 1u : 0u)' % ((signed(a), signed(b)) if o == 'slt' else (a, b)))
    if o in ('sll', 'srl', 'sra', 'sllv', 'srlv', 'srav'):
        left = b
        count = str(sh) if not o.endswith('v') else '(%s & 31u)' % a
        if o in ('sra', 'srav'):
            return put(d, 'static_cast<u32>(%s >> %s)' % (signed(left), count))
        return put(d, '(%s %s %s)' % (left, '<<' if o.startswith('sll') else '>>', count))
    if o in ('lb', 'lbu', 'lh', 'lhu', 'lw'):
        value = {'lb': 'static_cast<u32>(static_cast<s32>(static_cast<std::int8_t>(read8(%s))))',
                 'lbu': 'read8(%s)',
                 'lh': 'static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(read16(%s))))',
                 'lhu': 'read16(%s)', 'lw': 'read32(%s)'}[o] % addr
        return put(i.rt, value)
    if o in ('sb', 'sh', 'sw'):
        return '%s(%s, %s);' % ({'sb': 'write8', 'sh': 'write16', 'sw': 'write32'}[o], addr, b)
    if o in ('lwl', 'lwr'):
        # Little-endian MIPS left/right load merge; byte lanes unaffected by alignment.
        return put(i.rt, 'load_%s(%s, %s)' % (o, addr, b))
    if o in ('swl', 'swr'):
        return 'store_%s(%s, %s);' % (o, addr, b)
    if o in ('mfhi', 'mflo'):
        return put(d, 'c.%s' % ('hi' if o == 'mfhi' else 'lo'))
    if o in ('mthi', 'mtlo'):
        return 'c.%s = %s;' % ('hi' if o == 'mthi' else 'lo', a)
    if o in ('mult', 'multu'):
        product = ('static_cast<std::int64_t>(%s) * static_cast<std::int64_t>(%s)' % (signed(a), signed(b))
                   if o == 'mult' else 'static_cast<std::uint64_t>(%s) * %s' % (a, b))
        return '{ const std::uint64_t v = %s; c.lo = static_cast<u32>(v); c.hi = static_cast<u32>(v >> 32); }' % product
    if o in ('div', 'divu'):
        return 'divide(c, %s, %s, %s);' % (a, b, 'true' if o == 'div' else 'false')
    if o == 'mfc0':
        return put(i.rt, 'cop0_read(c, %d)' % i.rd)
    if o == 'mtc0':
        return 'cop0_write(c, %d, %s);' % (i.rd, b)
    if i.word == 0x42000010:  # R3000 COP0 RFE, decoded as generic cop0.
        return 'cop0_rfe(c);'
    if o in ('syscall', 'break'):
        return 'iop_%s(c, %s, %s);' % (o, hex32(i.pc), hex32(i.word))
    if o == 'invalid' or o.startswith(('cop', 'mfc', 'mtc', 'cfc', 'ctc', 'lwc', 'swc', 'bc')):
        return 'unsupported(c, %s, %s);' % (hex32(i.pc), hex32(i.word))
    raise ValueError('unhandled instruction %s at %08x' % (o, i.pc))


HELPERS = '''// Little-endian MIPS unaligned load/store operations.
static u32 load_lwl(u32 a, u32 old) {
    const unsigned shift = (a & 3u) * 8u;
    return (old & (0x00ffffffu >> shift)) | (read32(a & ~3u) << (24u - shift));
}
static u32 load_lwr(u32 a, u32 old) {
    const unsigned shift = (a & 3u) * 8u;
    return (old & (0xffffff00u << (24u - shift))) | (read32(a & ~3u) >> shift);
}
static void store_swl(u32 a, u32 value) {
    const unsigned shift = (a & 3u) * 8u;
    const u32 mask = 0xffffffffu >> (24u - shift);
    const u32 aligned = a & ~3u;
    write32(aligned, (read32(aligned) & ~mask) | (value >> (24u - shift)));
}
static void store_swr(u32 a, u32 value) {
    const unsigned shift = (a & 3u) * 8u;
    const u32 mask = 0xffffffffu << shift;
    const u32 aligned = a & ~3u;
    write32(aligned, (read32(aligned) & ~mask) | (value << shift));
}
static void divide(Context& c, u32 a, u32 b, bool sign) {
    if (!b) { c.lo = sign && static_cast<s32>(a) >= 0 ? ~0u : 1u; c.hi = a; return; }
    if (sign) {
        const auto x = static_cast<std::int64_t>(static_cast<s32>(a));
        const auto y = static_cast<std::int64_t>(static_cast<s32>(b));
        c.lo = static_cast<u32>(x / y); c.hi = static_cast<u32>(x % y);
    } else { c.lo = a / b; c.hi = a % b; }
}
'''


class Compiler:
    def __init__(self, module, base):
        self.module, self.base = module, base
        self.exports = list(module.exports)
        if not self.exports:
            # An export's first function can be at offset zero. irx.py stops
            # there as though it were the table terminator; distinguish it by
            # its R_MIPS_32 relocation (the actual terminator is not relocated).
            relocated = {at for at, info in module.relocations if (info & 255) == 2}
            for at in range(0, len(module.image) - 20, 4):
                if u32(module.image, at) != 0x41c00000:
                    continue
                name = bytes(module.image[at + 12:at + 20]).split(b'\0')[0].decode('ascii', 'replace')
                if not re.fullmatch('[A-Za-z0-9_]+', name):
                    continue
                entries = []
                p = at + 20
                while p + 4 <= len(module.image) and len(entries) < 512:
                    value = u32(module.image, p)
                    if not value and p not in relocated:
                        break
                    entries.append((len(entries), value))
                    p += 4
                if entries:
                    self.exports.append(Table(at, name, int.from_bytes(module.image[at+8:at+10], 'little'), entries))
        self.image = module.relocate(base)
        self.code = set()
        self.primary_code = set()
        for s in module.sections:
            if s[3] & 0x6 == 0x6 and s[2] != 8:  # ELF SHF_ALLOC | SHF_EXECINSTR
                addresses = range(base + s[4], base + s[4] + s[6], 4)
                self.code.update(addresses)
                if s[0] == '.text':
                    self.primary_code.update(addresses)
        self.imports = {base + at for table in module.imports for _, at in table.entries}
        self.code.difference_update(self.imports)
        self.decoded = {pc: decode(u32(self.image, pc-base), pc) for pc in self.code}
        entries = {base + module.entry, base + module.mod_entry}
        entries.update(base + addr for table in self.exports for _, addr in table.entries)
        entries.update(self.decoded[pc].target for pc in self.primary_code
                       if pc in self.decoded and self.decoded[pc].op == 'jal'
                       and self.decoded[pc].target in self.code)
        entries.update(self.code_constants())
        # A relocated R_MIPS_32 word in an allocated data section is a real
        # pointer, unlike arbitrary words that happen to resemble code VAs.
        for at, info in module.relocations:
            if (info & 255) != 2:
                continue
            if not any(s[0] != '.text' and s[3] & 2 and s[4] <= at < s[4] + s[6]
                       for s in module.sections):
                continue
            target = u32(self.image, at)
            if target in self.primary_code or self.looks_like_code(target):
                entries.add(target)

        def discover():
            # Calls from code embedded in .data become visible only after
            # tracing entry/export/data-pointer roots.
            while True:
                self.entries = sorted(entries & self.code)
                self.entry_set = set(self.entries)
                self.functions = {entry: self.trace(entry) for entry in self.entries}
                calls = {self.decoded[pc].target
                         for pcs in self.functions.values() for pc in pcs
                         if self.decoded[pc].op == 'jal'
                         and self.decoded[pc].target in self.code}
                if calls <= entries:
                    return
                entries.update(calls)

        discover()
        covered = {pc for pcs in self.functions.values() for pc in pcs}
        # Last resort for register-indirect calls whose pointer is constructed
        # outside the analyzed window. Do not assume all .text bytes are code:
        # accept uncovered starts only after a return/tail-call and its delay.
        for pc in sorted(self.primary_code):
            if pc not in covered and self.looks_like_start(pc):
                entries.add(pc)
                self.entry_set.add(pc)
                covered.update(self.trace(pc))
        discover()
        self.owners = {}
        for entry, pcs in self.functions.items():
            for pc in pcs:
                self.owners.setdefault(pc, entry)

    def looks_like_code(self, pc):
        if pc not in self.code or pc in self.primary_code:
            return False
        # Reject relocated string/data pointers that accidentally decode as
        # arithmetic: a short valid instruction run must reach control flow.
        for at in range(pc, pc + 32 * 4, 4):
            i = self.decoded.get(at)
            if i is None or i.op == 'invalid' or i.op.startswith(('cop1', 'cop2', 'cop3')):
                return False
            if i.has_delay:
                return at + 4 in self.decoded
        return False


    @staticmethod
    def writes_gpr(i):
        if i.op in ('addiu', 'addi', 'slti', 'sltiu', 'andi', 'ori', 'xori',
                    'lui', 'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwl', 'lwr', 'mfc0', 'cfc0'):
            return i.rt
        if i.op == 'jal':
            return 31
        if i.op == 'jalr':
            return i.rd
        if i.op in ('bltzal', 'bgezal'):
            return 31
        if i.op in ('sll', 'srl', 'sra', 'sllv', 'srlv', 'srav', 'mfhi', 'mflo',
                    'add', 'addu', 'sub', 'subu', 'and', 'or', 'xor', 'nor',
                    'slt', 'sltu'):
            return i.rd
        return None

    def code_constants(self):
        """Track interleaved lui/addiu and lui/ori code pointers through stores."""
        targets = set()
        highs = [None] * 32
        previous = None
        for pc in sorted(self.primary_code):
            if previous is not None and pc != previous + 4:
                highs = [None] * 32
            previous = pc
            i = self.decoded.get(pc)
            if i is None:
                highs = [None] * 32
                continue
            if i.op in ('addiu', 'ori') and highs[i.rs] is not None:
                origin, upper = highs[i.rs]
                if pc - origin <= 128 * 4:
                    value = (upper + (i.simm if i.op == 'addiu' else i.imm)) & 0xffffffff
                    if value in self.primary_code:
                        targets.add(value)
            written = self.writes_gpr(i)
            if written and i.op == 'lui':
                highs[written] = (pc, i.imm << 16)
            elif written:
                highs[written] = None
        return targets

    def looks_like_start(self, pc):
        if pc not in self.primary_code or pc not in self.decoded or self.decoded[pc].op in ('invalid', 'nop'):
            return False
        if pc - 4 in self.decoded and self.decoded[pc - 4].has_delay:
            return False
        before = pc - 4
        while before in self.decoded and self.decoded[before].op == 'nop' and pc - before <= 32:
            before -= 4
        for addr in (before, before - 4):
            i = self.decoded.get(addr)
            if i and (i.op == 'jr' and i.rs == 31 or i.op == 'j'):
                return True
        return False

    def constant_regs_before(self, pc, count):
        values = {}
        for at in range(pc - count * 4, pc, 4):
            i = self.decoded.get(at)
            if i is None:
                values.clear()
                continue
            value = None
            if i.op == 'lui':
                value = i.imm << 16
            elif i.op in ('addiu', 'ori') and i.rs in values:
                value = ((values[i.rs] + i.simm) if i.op == 'addiu'
                         else (values[i.rs] | i.imm)) & 0xffffffff
            written = self.writes_gpr(i)
            if written:
                if value is None:
                    values.pop(written, None)
                else:
                    values[written] = value
        return values

    def jump_targets(self, pc):
        """Recognize a MIPS switch: lw destination, table[index]; jr destination."""
        branch = self.decoded[pc]
        load = next((self.decoded[at] for at in range(pc - 4, pc - 25 * 4, -4)
                     if at in self.decoded and self.decoded[at].op == 'lw'
                     and self.decoded[at].rt == branch.rs), None)
        if load is None:
            return ()
        writer = next((self.decoded[at] for at in range(load.pc - 4, load.pc - 25 * 4, -4)
                       if at in self.decoded and self.writes_gpr(self.decoded[at]) == load.rs), None)
        if writer is None or writer.op != 'addu':
            return ()
        values = self.constant_regs_before(writer.pc, 64)
        table = next(((values[r] + load.simm) & 0xffffffff
                      for r in (writer.rs, writer.rt) if r in values), None)
        if table is None or table & 3:
            return ()
        size = next((self.decoded[at].imm for at in range(writer.pc - 4, writer.pc - 41 * 4, -4)
                     if at in self.decoded and self.decoded[at].op == 'sltiu'
                     and self.decoded[at].imm <= 256), None)
        if size is None:
            size = 256
        targets = []
        for index in range(size):
            at = table + index * 4 - self.base
            if at < 0 or at + 4 > len(self.image):
                break
            target = u32(self.image, at)
            if target not in self.code:
                break
            targets.append(target)
        return tuple(targets)

    def trace(self, entry):
        seen, pending = set(), [entry]
        while pending:
            pc = pending.pop()
            if pc not in self.code or pc in seen or (pc in self.entry_set and pc != entry):
                continue
            i = self.decoded[pc]
            seen.add(pc)
            if i.has_delay:
                if pc + 4 not in self.decoded:
                    raise ValueError('missing delay slot at %08x' % pc)
                if i.op in ('j', 'jal'):
                    if i.op == 'j' and i.target in self.code:
                        pending.append(i.target)
                elif i.is_branch:
                    pending.append(i.target)
                    pending.append(pc + 8)
                elif i.op == 'jr' and i.rs != 31:
                    pending.extend(self.jump_targets(pc))
                # Other register-indirect transfers remain dynamic calls.
                if i.is_call:
                    pending.append(pc + 8)
            else:
                pending.append(pc + 4)
        return sorted(seen)

    @staticmethod
    def observable(i):
        return i.op in ('syscall', 'break', 'invalid') or i.op.startswith(
            ('cop', 'mfc', 'mtc', 'cfc', 'ctc', 'lwc', 'swc', 'bc'))

    @staticmethod
    def label(pc):
        return 'L_%08x' % pc

    def control(self, i, pcs):
        pc, op, target = i.pc, i.op, i.target
        nextpc = hex32(pc + 8)
        delay = self.decoded[pc + 4]
        if delay.has_delay:
            raise ValueError('branch in delay slot at %08x' % (pc + 4))
        pre = ['u32 dest = 0u;', 'bool take = false;']
        a, b = reg(i.rs), reg(i.rt)
        if op in ('j', 'jal'):
            pre += ['dest = %s;' % hex32(target), 'take = true;']
        elif op in ('jr', 'jalr'):
            pre += ['dest = %s;' % a, 'take = true;']
        else:
            if op in ('beq', 'bne'):
                cond = '%s %s %s' % (a, '==' if op == 'beq' else '!=', b)
            elif op in ('blez', 'bgtz'):
                cond = '%s %s 0' % (signed(a), '<=' if op == 'blez' else '>')
            elif op in ('bltz', 'bltzal', 'bgez', 'bgezal'):
                cond = '%s %s 0' % (signed(a), '<' if op.startswith('bltz') else '>=')
            else:
                cond = None
            if cond is None:
                pre += ['c.pc = %s; unsupported(c, %s, %s);' %
                        (hex32(pc), hex32(pc), hex32(i.word))]
            else:
                pre += ['take = (%s);' % cond, 'dest = %s;' % hex32(target)]
        if op in ('jal', 'jalr'):
            pre += [put(31 if op == 'jal' else i.rd, nextpc)]
        if op in ('bltzal', 'bgezal'):
            pre += ['if (take) %s' % put(31, nextpc)]
        if self.observable(delay):
            pre += ['c.pc = %s;' % hex32(pc + 4)]
        pre += [operation(delay)]

        def transfer(addr):
            if addr in pcs:
                return 'goto %s;' % self.label(addr)
            return 'c.pc = %s; invoke(%s, c); return;' % (hex32(pc + 4), hex32(addr))

        if op == 'jr' and i.rs == 31:
            pre += ['return;']
        elif i.is_call:
            pre += ['if (take) { c.pc = %s; invoke(dest, c); }' % hex32(pc + 4),
                    transfer(pc + 8)]
        elif op == 'j':
            pre += [transfer(target)]
        elif op == 'jr':
            pre += ['pc = dest; goto dispatch;']
        else:
            pre += ['if (take) { %s }' % transfer(target),
                    transfer(pc + 8)]
        return [x for x in pre if x]

    def emit_function(self, entry, pcs):
        symbol = 'fn_%08x' % entry
        reachable = set(pcs)
        backward = {self.decoded[pc].target for pc in pcs
                    if self.decoded[pc].has_delay and
                    (self.decoded[pc].is_branch or self.decoded[pc].op == 'j') and
                    self.decoded[pc].target in reachable and
                    self.decoded[pc].target <= pc}
        # Module dispatch accepts every traced instruction address, including
        # return-address re-entries and register-indirect jump destinations.
        # This switch runs only on entry (or an indirect jump), not per opcode.
        lines = ['void %s(Context& c, u32 pc) {' % symbol,
                 'dispatch:', '    switch (pc) {']
        lines.extend('    case %s: goto %s;' % (hex32(at), self.label(at))
                     for at in pcs)
        lines += ['    default: c.pc = pc; invoke(pc, c); return;', '    }']
        for pc in pcs:
            i = self.decoded[pc]
            lines.append('%s:' % self.label(pc))
            if pc in backward:
                lines.append('    c.pc = %s; checkpoint();' % hex32(pc))
            if i.has_delay:
                lines.append('    {')
                lines.extend('        ' + s for s in self.control(i, reachable))
                lines.append('    }')
            else:
                if self.observable(i):
                    lines.append('    c.pc = %s;' % hex32(pc))
                statement = operation(i)
                if statement:
                    lines.append('    ' + statement)
                if pc + 4 not in reachable:
                    lines.append('    c.pc = %s; invoke(%s, c); return;' %
                                 (hex32(pc), hex32(pc + 4)))
        lines += ['}', '']
        return '\n'.join(lines)

    def emit(self, out):
        name = re.sub('[^a-z0-9_]', '_', self.module.name.lower())
        # Most compilers can compile these independent pieces in parallel.
        chunks = [self.entries[n:n+64] for n in range(0, len(self.entries), 64)]
        for index, chunk in enumerate(chunks):
            text = ['#include "iop/recompiled.h"', '#include <cstdint>', 'namespace iop {', 'namespace {', HELPERS, '}']
            # Helpers need internal linkage but are visible inside this namespace.
            text += [self.emit_function(entry, self.functions[entry]) for entry in chunk]
            text += ['} // namespace iop', '']
            write_if_changed(out / ('%s_%03d.cpp' % (name, index)), '\n'.join(text))
        for stale in out.glob('%s_???.cpp' % name):
            if int(stale.stem[-3:]) >= len(chunks):
                stale.unlink()
        declarations = ['void fn_%08x(Context&, u32);' % e for e in self.entries]
        handlers = ['    case %s: fn_%08x(c, addr); return;' % (hex32(pc), owner)
                    for pc, owner in sorted(self.owners.items())]
        imports = ['{%s, "%s", %d}' % (hex32(self.base + at), t.name, ordinal)
                   for t in self.module.imports for ordinal, at in t.entries]
        exports = ['{"%s", %d, %s}' % (t.name, ordinal, hex32(self.base + at))
                   for t in self.exports for ordinal, at in t.entries]
        data = ','.join('0x%02x' % b for b in self.image)
        text = ['#include "iop/recompiled.h"', 'namespace iop {'] + declarations
        text += ['static const u8 image[] = {%s};' % data,
                 'static const Import imports[] = {%s};' % (','.join(imports) or '{0u, nullptr, 0}'),
                 'static const Export exports[] = {%s};' % (','.join(exports) or '{nullptr, 0, 0u}'),
                 'static void dispatch(Context& c, u32 addr) {', '    switch (addr) {']
        text += handlers
        text += ['    default: unsupported(c, addr, read32(addr));', '    }', '}',
                 'extern const Module module_%s = {"%s", %s, %su, %s, %s, image, dispatch, imports, %d, exports, %d};' %
                 (name, self.module.name, hex32(self.base), len(self.image),
                  hex32(self.base + self.module.entry), hex32(self.base + self.module.gp),
                  len(imports), len(exports)), '} // namespace iop', '']
        write_if_changed(out / (name + '_module.cpp'), '\n'.join(text))
        return name, len(self.entries), len(self.owners)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('modules', nargs='*', type=pathlib.Path)
    p.add_argument('--out', type=pathlib.Path, default=pathlib.Path('build/generated_iop'))
    p.add_argument('--base', type=lambda value: int(value, 0), help='base for a single nonstandard IRX')
    args = p.parse_args()
    paths = args.modules or DEFAULT
    if args.base is not None and len(paths) != 1:
        p.error('--base requires exactly one module')
    args.out.mkdir(parents=True, exist_ok=True)
    modules = []
    for path in paths:
        module = IRX(path)
        base = args.base if args.base is not None else BASES.get(path.stem.upper())
        if base is None:
            p.error('no load base for %s; pass --base' % path)
        compiled = Compiler(module, base)
        name, count, locations = compiled.emit(args.out)
        modules.append(name)
        print('%s: %d entries, %d static instruction addresses' % (module.name, count, locations))
    registry = ['#include "iop/recompiled.h"', 'namespace iop {']
    registry += ['extern const Module module_%s;' % name for name in modules]
    registry += ['const Module* const* generated_modules(std::size_t& count) {',
                 '    static const Module* const modules[] = {%s};' % ', '.join('&module_%s' % n for n in modules),
                 '    count = %d;' % len(modules), '    return modules;', '}', '}', '']
    write_if_changed(args.out / 'iop_registry.cpp', '\n'.join(registry))


if __name__ == '__main__':
    main()
