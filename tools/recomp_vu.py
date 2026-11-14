#!/usr/bin/env python3
"""Specialize SLES-52388 VU pairs against the original PCSX2 opcode tables.

The table parser reads the PCSX2 source of truth rather than maintaining an
independent opcode list; secondary upper/lower dispatch is resolved at build
time. The emitted wrappers directly call the original VUops implementations.
"""
import argparse
import json
import pathlib
import re
import subprocess
import struct

START = 0x5DCB80
END = 0x607F20


def elf_file_offset(elf: bytes) -> int:
    if elf[:4] != b'\x7fELF' or elf[4:6] != b'\x01\x01':
        raise ValueError('expected little-endian ELF32')
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    for i in range(phnum):
        kind, offset, addr, _, size, _, _, _ = struct.unpack_from(
            '<IIIIIIII', elf, phoff + i * phentsize)
        if kind == 1 and addr <= START and END <= addr + size:
            return offset + START - addr
    raise ValueError('VU range is not inside a PT_LOAD segment')


def load_region(path: pathlib.Path) -> bytes:
    elf = path.read_bytes()
    offset = elf_file_offset(elf)
    return elf[offset:offset + END - START]

def opcode_tables(source: str) -> dict[str, list[str]]:
    names = ('_LOWER_OPCODE', 'LowerOP_OPCODE', 'LowerOP_T3_00_OPCODE',
             'LowerOP_T3_01_OPCODE', 'LowerOP_T3_10_OPCODE',
             'LowerOP_T3_11_OPCODE', '_UPPER_OPCODE',
             '_UPPER_FD_00_TABLE', '_UPPER_FD_01_TABLE',
             '_UPPER_FD_10_TABLE', '_UPPER_FD_11_TABLE')
    tables = {}
    for name in names:
        start = source.find('PREFIX##' + name + '[')
        if start == -1:
            raise ValueError(f'PCSX2 opcode table {name} is missing')
        body = source[source.index('{', start):source.index('};', start)]
        body = re.sub(r'/\\*.*?\\*/', '', body, flags=re.S)
        tables[name] = re.findall(r'PREFIX##([A-Za-z0-9_]+)', body)
    if len(tables['_LOWER_OPCODE']) != 128 or len(tables['_UPPER_OPCODE']) != 64:
        raise ValueError('PCSX2 primary opcode table shape changed')
    return tables


def decode(word: int, upper: bool, tables: dict[str, list[str]]) -> str:
    if upper:
        op = tables['_UPPER_OPCODE'][word & 63]
        if op.startswith('_UPPER_FD_'):
            op = tables[op + '_TABLE'][(word >> 6) & 31]
    else:
        op = tables['_LOWER_OPCODE'][word >> 25]
        if op == 'LowerOP':
            op = tables['LowerOP_OPCODE'][word & 63]
            if op.startswith('LowerOP_T3_'):
                op = tables[op + '_OPCODE'][(word >> 6) & 31]
    if not (op.startswith('MI_') or op == 'unknown'):
        raise ValueError(f'unresolved secondary opcode {op}')
    return op

def registered_entries(elf: bytes) -> list[tuple[int, int]]:
    """Recover constant program start/end pointers at register(0x2256b0).

    Many game call sites pass a1 via a callee-saved s0, so track MIPS
    LUI/ADDIU and DADDU moves backwards; leave dynamic values unresolved.
    """
    jal = 0x0c000000 | (0x2256b0 >> 2)

    def insn(offset: int) -> int:
        return struct.unpack_from('<I', elf, offset)[0]

    def constant(before: int, reg: int, depth: int = 0) -> int | None:
        if depth > 4 or reg == 0:
            return None
        for pos in range(before - 4, max(0x100, before - 144), -4):
            word = insn(pos)
            op, src, dest = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            rd = (word >> 11) & 31
            if op == 9 and dest == reg and src == reg:  # addiu
                for prev in range(pos - 4, max(0x100, pos - 40), -4):
                    value = insn(prev)
                    if value >> 26 == 15 and (value >> 16) & 31 == reg:
                        imm = (word & 65535) ^ 32768
                        return ((value & 65535) << 16) + imm - 32768
                return None
            if op == 15 and dest == reg:  # lui
                return (word & 65535) << 16
            if op == 0 and rd == reg and (word & 63) in (33, 37, 45):
                from_reg = src if dest == 0 else dest if src == 0 else 0
                return constant(pos, from_reg, depth + 1)
            if dest == reg and op in (8, 9, 12, 13, 14, 15, 35, 36, 37, 39):
                return None
        return None

    entries = {}
    for pos in range(0x100, len(elf) - 4, 4):
        if insn(pos) != jal:
            continue
        address = constant(pos, 5)
        if address is None or not (START <= address < END) or (address - START) % 8:
            continue
        end = None
        # The call setup builds the end pointer in v0, subtracts a1 and
        # arithmetic-shifts four bits to pass its size in quadwords as a2.
        for cursor in range(pos - 4, pos - 72, -4):
            word = insn(cursor)
            if word >> 16 != 0x2442:
                continue
            for prev in range(cursor - 4, cursor - 36, -4):
                hi = insn(prev)
                if hi >> 16 == 0x3c02:
                    imm = ((word & 65535) ^ 32768) - 32768
                    candidate = ((hi & 65535) << 16) + imm
                    if address < candidate <= END and (candidate - address) % 8 == 0:
                        end = candidate
                    break
            if end is not None:
                break
        entries[(address - START) // 8] = end - address if end else 512
    return sorted(entries.items())

def analyse_fast(out_dir: pathlib.Path, selected: set[int],
                 pairs: list[tuple[int, int]]) -> dict[int, tuple[tuple[int, ...], tuple[int, ...]]]:
    """Ask the original PCSX2 register-use functions for exact constants."""
    root = pathlib.Path(__file__).resolve().parent.parent
    source = out_dir / 'vu_analyse.cpp'
    executable = out_dir / 'vu_analyse'
    source.write_text('''#include "VUops.h"
#include <cstdio>
VUPortCpuRegs cpuRegs{};
bool vu_port_addsub_hack = false;
u32 vu_port_vif_reg(unsigned, unsigned) { return 0; }
void vu_port_xgkick(unsigned, const u8*, unsigned) {}
void vu_port_sync_vu1() {}
void vu_port_indirect_branch(unsigned, u32) {}
int main() {
    u32 words[2];
    while (std::fread(words, sizeof(words), 1, stdin) == 1) {
        _VURegsNum upper{}, lower{};
        VU1.code = words[1];
        VU1regs_UPPER_OPCODE[words[1] & 63](&upper);
        if (!(words[1] & 0x80000000u)) {
            VU1.code = words[0];
            VU1regs_LOWER_OPCODE[words[0] >> 25](&lower);
        }
        std::fwrite(&upper, sizeof(upper), 1, stdout);
        std::fwrite(&lower, sizeof(lower), 1, stdout);
    }
}
''')
    subprocess.run(
        ['c++', '-std=c++20', '-O0', '-DVU_ANALYSIS_TOOL',
         '-I' + str(root / 'third_party/pcsx2_vu'),
         str(root / 'third_party/pcsx2_vu/VUops.cpp'),
         str(root / 'third_party/pcsx2_vu/VUflags.cpp'),
         str(source), '-o', str(executable)], check=True,
        stdout=subprocess.DEVNULL)
    indices = sorted(selected)
    raw = b''.join(struct.pack('<II', *pairs[index]) for index in indices)
    result = subprocess.run([str(executable)], input=raw, capture_output=True, check=True)
    if len(result.stdout) != len(indices) * 40:
        raise ValueError('unexpected PCSX2 register-analysis descriptor layout')
    descriptors = {}
    for slot, index in enumerate(indices):
        upper = struct.unpack_from('<7BxIIi', result.stdout, slot * 40)
        lower = struct.unpack_from('<7BxIIi', result.stdout, slot * 40 + 20)
        descriptors[index] = upper, lower
    source.unlink()
    executable.unlink()
    return descriptors


def generate_fast(path: pathlib.Path, pairs: list[tuple[int, int]],
                  programs: list[tuple[int, int]],
                  tables: dict[str, list[str]], manifest=None,
                  elf_offset=None, images=None) -> int:
    # Registered spans are also eligible for the legacy pair fallback.
    selected = set()
    for entry, length in programs:
        selected.update(range(entry, entry + length // 8))
    chunks: dict[int, list[int]] = {}
    for pair in sorted(selected):
        chunks.setdefault(pair // 16, []).append(pair)
    descriptor_pairs = selected.copy()
    if manifest:
        for record in json.loads(manifest.read_text())['programs']:
            if record['unit'] != 1:
                continue
            for fragment in record['fragments']:
                source = fragment['elf_offset'] - elf_offset
                if source < 0 or source % 8 or source + fragment['length'] > len(pairs) * 8:
                    raise ValueError('manifest fragment outside ELF VU region')
                descriptor_pairs.update(range(source // 8, (source + fragment['length']) // 8))
    if images:
        for record in json.loads(images.read_text())['images']:
            for fragment in record['fragments']:
                source = fragment['elf_offset'] - elf_offset
                if source < 0 or source % 8 or source + fragment['length'] > len(pairs) * 8:
                    raise ValueError('image fragment outside ELF VU region')
                descriptor_pairs.update(range(source // 8, (source + fragment['length']) // 8))
    analysis = analyse_fast(path.parent, descriptor_pairs, pairs)
    # Register descriptors go to their own file: the image shards need them
    # without the (large) chunk bodies below.
    with (path.parent / 'vu_fast_regs.inc').open('w') as regs_out:
        for pair in sorted(descriptor_pairs):
            for side, regs in zip(('upper', 'lower'), analysis[pair]):
                fields = ', '.join(str(x) for x in regs)
                regs_out.write(f'static constexpr _VURegsNum vu_fast_{side}_{pair}'
                               f'{{{fields}}};\n')
    with path.open('w') as out:
        out.write('#include "vu_fast.h"\n#include "vu_fast_regs.inc"\n')
        for chunk, indices in chunks.items():
            out.write(f'static u32 vu_fast_chunk_{chunk}(u32 index, u32 limit, '
                      'VUFastPipes& pipes) {\n'
                      '    u32 done = 0, oldpc;\n    switch (index) {\n')
            for pair in indices:
                out.write(f'    case {pair}: goto pair_{pair};\n')
            out.write('    default: return 0;\n    }\n')
            for pair in indices:
                lower, upper = pairs[pair]
                uop = decode(upper, True, tables)
                lop = decode(lower, False, tables) if not upper & 0x80000000 else 'MI_NOP'
                optimized_upper = f'VU1{uop}'
                match = re.fullmatch(r'MI_(MADD|MUL|ADD|SUB)(A?)([iqxyzw]?)', uop)
                if match:
                    operation, accum, source = match.groups()
                    source_name = {'': 'Vector', 'i': 'I', 'q': 'Q',
                                   'x': 'X', 'y': 'Y', 'z': 'Z', 'w': 'W'}[source]
                    target_name = 'ACC' if accum else 'VF'
                    optimized_upper = (
                        f'vu_fast_math<0x{upper:08x}u, vu_simd::Op::{operation.title()}, '
                        f'vu_simd::Source::{source_name}, vu_simd::Target::{target_name}, '
                        f'VU1{uop}>')
                out.write(f'pair_{pair}:\n'
                          '    VU1.VI[REG_TPC].UL &= 0x3fffu;\n'
                          '    oldpc = VU1.VI[REG_TPC].UL;\n'
                          f'    vu_fast_pair<{pair}, 0x{lower:08x}u, 0x{upper:08x}u, '
                          f'{optimized_upper}, VU1{lop}, vu_fast_upper_{pair}, '
                          f'vu_fast_lower_{pair}>(pipes);\n'
                          '    ++done;\n'
                          '    if (done >= limit || !(vpu_stat() & 0x100)) return done;\n')
                if pair + 1 in indices:
                    out.write('    if ((VU1.VI[REG_TPC].UL & 0x3fffu) == '
                              '((oldpc + 8) & 0x3fffu) && '
                              f'pipes.mapped[(VU1.VI[REG_TPC].UL & 0x3fffu) >> 3] '
                              f'== {pair + 1}) goto pair_{pair + 1};\n')
                out.write('    return done;\n')
            out.write('}\n')
        out.write('u32 vu_port_run_fast_pair(u32 index, u32 limit, VUFastPipes& pipes) {\n'
                  '    switch (index / 16) {\n')
        for chunk in chunks:
            out.write(f'    case {chunk}: return vu_fast_chunk_{chunk}(index, limit, pipes);\n')
        out.write('    default: return 0;\n    }\n}\n')
    from vu_graph_emit import emit
    emit(path.parent / 'vu_graph.inc', pairs, programs, analysis, tables, decode,
         manifest, elf_offset)
    from vu_image_emit import emit_images, write_shards
    image_out = path.parent / 'vu_image.inc'
    if images:
        nodes = emit_images(image_out, pairs, tables, decode, analysis, images, elf_offset)
        print(f'Generated {nodes} whole-image VU graph nodes')
    else:
        write_shards(image_out, [])
        image_out.write_text('u32 vu_port_run_image(u32) { return 0; }\n'
                             'u32 vu_port_run_image0(u32) { return 0; }\n')
    return len(selected)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=pathlib.Path)
    parser.add_argument('out', type=pathlib.Path)
    parser.add_argument('--manifest', type=pathlib.Path)
    parser.add_argument('--images', type=pathlib.Path,
                        help='format-2 manifest (tools/vu_manifest.py --images)')
    args = parser.parse_args()
    data = load_region(args.elf)
    if len(data) != END - START or len(data) % 8:
        raise ValueError('incomplete or unaligned VU instruction region')
    args.out.mkdir(parents=True, exist_ok=True)
    tables = opcode_tables((pathlib.Path(__file__).resolve().parent.parent /
                            'third_party/pcsx2_vu/VUops.cpp').read_text())
    pairs = list(struct.iter_unpack('<II', data))
    operations = set()
    with (args.out / 'vu_pairs.cpp').open('w') as out:
        out.write('#include "vu_compiled.h"\n#include "vu_direct.h"\n'
                  '// Decoded lower/upper pairs from SLES-52388 main segment.\n'
                  'VUCompiledPair g_vu_compiled_pairs[] = {\n')
        for low, high in pairs:
            upper = decode(high, True, tables)
            lower = decode(low, False, tables) if not (high & 0x80000000) else None
            operations.add(upper)
            if lower:
                operations.add(lower)
            lower_fns = ', '.join('vu_direct_' + str(unit) + '_' + lower
                                  if lower else 'nullptr' for unit in range(2))
            upper_fns = ', '.join('vu_direct_' + str(unit) + '_' + upper
                                  for unit in range(2))
            out.write(f'    {{0x{low:08x}u, 0x{high:08x}u, '
                      f'{{{lower_fns}}}, {{{upper_fns}}}}},\n')
        out.write('};\n'
                  'const u32 g_vu_compiled_count = sizeof(g_vu_compiled_pairs) / '
                  'sizeof(g_vu_compiled_pairs[0]);\n')
    with (args.out / 'vu_direct.h').open('w') as header:
        header.write('#pragma once\n')
        for unit in range(2):
            for operation in sorted(operations):
                header.write(f'void vu_direct_{unit}_{operation}();\n')
    with (args.out / 'vu_direct.inc').open('w') as out:
        for unit in range(2):
            for operation in sorted(operations):
                out.write(f'void vu_direct_{unit}_{operation}() '
                          f'{{ VU{unit}{operation}(); }}\n')
    programs = registered_entries(args.elf.read_bytes())
    with (args.out / 'vu_program_entries.h').open('w') as out:
        out.write('#pragma once\n#include <array>\n'
                  f'inline constexpr std::array<unsigned, {len(programs)}> vu_program_entries = {{')
        out.write(', '.join(str(pair) for pair, _ in programs))
        out.write('};\n'
                  f'inline constexpr std::array<unsigned, {len(programs)}> vu_program_bytes = {{')
        out.write(', '.join(str(length) for _, length in programs))
        out.write('};\n')
    fast_count = generate_fast(args.out / 'vu_fast.inc', pairs, programs, tables,
                               args.manifest,
                               elf_file_offset(args.elf.read_bytes()),
                               args.images)
    print(f'Generated {fast_count} straight-line VU1 pairs')
    print(f'Recovered {len(programs)} statically rooted registered entries')
    print(f'Generated {len(pairs)} VU pairs, {len(operations)} decoded opcodes')


if __name__ == '__main__':
    main()
