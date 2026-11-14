"""Emit C++ VU1 blocks specialized by PCSX2's physical pipeline slots.

The graph never guesses an unknown branch target or live entry state. Those
cases transfer to the indexed interpreter with its authoritative VURegs data.
"""
import json
import re

from vu_pipeline import BACKUP_OPS, JUMPS, Pair, analyze

# PCSX2 VI_BACKUP: these read the pre-write value of a VI register written in
# the previous two pairs (VIBackupCycles).
CONDITIONAL_BRANCHES = {'MI_IBEQ', 'MI_IBNE', 'MI_IBGEZ', 'MI_IBGTZ', 'MI_IBLEZ', 'MI_IBLTZ'}


def upper_call(word, opcode):
    match = re.fullmatch(r'MI_(MADD|MUL|ADD|SUB)(A?)([iqxyzw]?)', opcode)
    if not match:
        return f'VU1{opcode}'
    operation, accum, source = match.groups()
    source_name = {'': 'Vector', 'i': 'I', 'q': 'Q',
                   'x': 'X', 'y': 'Y', 'z': 'Z', 'w': 'W'}[source]
    target = 'ACC' if accum else 'VF'
    return (f'vu_fast_math<0x{word:08x}u, vu_simd::Op::{operation.title()}, '
            f'vu_simd::Source::{source_name}, vu_simd::Target::{target}, '
            f'VU1{opcode}>')


def emit(path, pairs, programs, descriptors, tables, decode, manifest=None, elf_offset=None):
    jobs = []
    for program, (entry, size) in enumerate(programs):
        indices = {pc: entry + pc for pc in range(size // 8)}
        jobs.append((program, entry, size, indices, None, 0))
    if manifest:
        if elf_offset is None:
            raise ValueError('ELF VU region offset required for captured images')
        seen = set()
        for record in json.loads(manifest.read_text())['programs']:
            if record['unit'] != 1:
                continue
            indices = {}
            start = record['start_pc'] // 8
            for fragment in record['fragments']:
                source = fragment['elf_offset'] - elf_offset
                if source % 8 or source < 0 or source + fragment['length'] > len(pairs) * 8:
                    raise ValueError('manifest fragment outside ELF VU region')
                for offset in range(0, fragment['length'], 8):
                    pc = ((fragment['micro_offset'] // 8 + offset // 8) - start) & 2047
                    indices[pc] = (source + offset) // 8
            if 0 not in indices:
                raise ValueError('captured program lacks its launch instruction')
            signature = (record['start_pc'], tuple(sorted((pc, pairs[idx])
                                                           for pc, idx in indices.items())))
            if signature in seen:
                continue
            seen.add(signature)
            jobs.append((len(jobs), 0, 16384, indices, True, record['start_pc']))
    roots = {}
    captured = []
    with path.open('w') as out:
        out.write('#include "vu_graph.h"\n#include <array>\n#include <utility>\n')
        for program, entry, size, indices, digest, start in jobs:
            typed = {}
            for pc, idx in indices.items():
                lower, upper = pairs[idx]
                uop = decode(upper, True, tables)
                lop = decode(lower, False, tables) if not upper & 0x80000000 else 'MI_NOP'
                typed[entry + pc] = Pair(lower, upper, uop, lop, *descriptors[idx])
            graph = analyze(typed, entry, size)
            # A pending VI backup (e.g. a JALR whose delay slot wrote a VI)
            # is unobservable unless a conditional branch runs before it
            # expires; such entries may drop it and enter the graph.
            backup_safe = all(typed[pc].lower_op not in CONDITIONAL_BRANCHES
                              for pc in (entry, entry + 1) if pc in typed)
            if digest is None:
                roots[entry] = program, backup_safe
            else:
                lower, upper = pairs[indices[0]]
                captured.append((program, start, lower | (upper << 32), backup_safe))
                # Execution cannot interleave an EE/VIF MPG write (they are
                # ordered with VU1 work).  Validate every reachable word once
                # per micro-memory generation instead of on every entry.
                out.write(f'static bool vu_graph_validate_{program}(u32 base) {{\n'
                          '    static u64 generation = 0;\n'
                          '    static u32 checked_base = 0;\n'
                          '    static bool valid = false;\n'
                          '    const u64 now = vu_compiled_generation(1);\n'
                          '    if (generation == now && checked_base == base) return valid;\n'
                          f'    static constexpr std::array<std::pair<u16, u16>, {len(indices)}> words{{{{\n')
                for pc, idx in sorted(indices.items()):
                    out.write(f'        {{{pc * 8}, {idx}}},\n')
                out.write('    }};\n'
                          '    generation = now;\n    checked_base = base;\n    valid = true;\n'
                          '    for (const auto& [offset, index] : words) {\n'
                          '        const auto& pair = g_vu_compiled_pairs[index];\n'
                          '        if (!vu_graph_pair_matches((base + offset) & 0x3fffu,\n'
                          '                u64(pair.lower_word) | (u64(pair.upper_word) << 32))) return valid = false;\n'
                          '    }\n    return true;\n}\n')
            nodes = list(graph.edges)
            labels = {node: i for i, node in enumerate(nodes)}
            out.write(f'static u32 vu_graph_program_{program}(u32 limit, VUFastPipes& pipes, u32 base) {{\n'
                      '    u32 done = 0;\n')
            for node in nodes:
                i = labels[node]
                edges = graph.edges[node]
                out.write(f'node_{i}: {{\n'
                          f'    if ((VU1.VI[REG_TPC].UL & 0x3fffu) != ((base + {node.pc * 8}u) & 0x3fffu)) return done;\n')
                if edges[0].kind == 'fallback' and not edges[0].events:
                    out.write('    return done;\n}\n')
                    continue
                pair = typed[entry + node.pc]
                index = indices[node.pc]
                if digest is None:
                    out.write(f'    if (pipes.mapped[((base >> 3) + {node.pc}) & 2047] != {index}) return done;\n')
                events = edges[0].events
                # Both branch outcomes share hazard and pipe operations; only
                # their delayed branch successor differs.
                for event in events:
                    tag = event[0]
                    if tag == 'execute_ops':
                        break
                    if tag == 'stall_fmac':
                        out.write(f'    vu_graph_flush_fmac({event[1]});\n'
                                  f'    VU1.cycle += {event[2]};\n')
                    elif tag == 'wait_q':
                        out.write('    _vuFlushFDIV(&VU1);\n')
                    elif tag == 'wait_p':
                        out.write('    _vuFlushEFU(&VU1);\n')
                    elif tag == 'flush_fmac':
                        out.write(f'    vu_graph_flush_fmac({event[1]});\n')
                    elif tag == 'flush_q':
                        out.write('    _vuFDIVflush(&VU1);\n')
                    elif tag == 'flush_p':
                        out.write('    _vuEFUflush(&VU1);\n')
                lower_op = f'VU1{pair.lower_op}'
                if pair.lower_op in ('MI_MOVE', 'MI_LQ', 'MI_SQ', 'MI_IADDIU'):
                    lower_op = f'vu_graph_lower_{pair.lower_op[3:].lower()}<0x{pair.lower:08x}u>'
                out.write(
                    f'    vu_graph_ops<0x{pair.lower:08x}u, 0x{pair.upper:08x}u, '
                    f'{upper_call(pair.upper, pair.upper_op)}, {lower_op}, '
                    f'vu_fast_upper_{index}, vu_fast_lower_{index}>();\n')
                if len(edges) == 2:
                    # _setBranch either creates branch=2 or marks a delayed
                    # branch. Capture the decision before pair retirement.
                    test = 'VU1.takedelaybranch' if node.state.branch is not None else 'VU1.branch == 2'
                    out.write(f'    const bool taken_{i} = {test};\n')
                after_op = False
                for event in events:
                    tag = event[0]
                    if tag == 'execute_ops':
                        after_op = True
                        continue
                    if not after_op:
                        continue
                    if tag == 'add_fmac':
                        out.write(f'    vu_graph_add_fmac({event[1]}, {event[2]}, {event[3]});\n')
                    elif tag == 'add_q':
                        out.write(f'    _vuFDIVAdd(&VU1, {event[1]});\n')
                    elif tag == 'add_p':
                        out.write(f'    _vuEFUAdd(&VU1, {event[1]});\n')
                    elif tag == 'flush_fmac':
                        out.write(f'    vu_graph_flush_fmac({event[1]});\n')
                    elif tag == 'flush_q':
                        out.write('    _vuFDIVflush(&VU1);\n')
                    elif tag == 'flush_p':
                        out.write('    _vuEFUflush(&VU1);\n')
                lower_enabled = not pair.upper & 0x80000000
                may_backup = node.state.vi_backup is not None or (
                    lower_enabled and pair.lower_op in BACKUP_OPS)
                may_branch = node.state.branch is not None or (
                    lower_enabled and pair.lower_op in JUMPS)
                may_end = bool(node.state.ebit or pair.upper & 0x40000000)
                out.write(f'    vu_graph_retire<{str(may_backup).lower()}, '
                          f'{str(may_branch).lower()}, {str(may_end).lower()}>();\n'
                          '    ++done;\n    if (done >= limit) return done;\n')
                if (node.state.ebit == 1 and not pair.upper & 0x40000000 or
                        pair.lower_op == 'MI_XGKICK' and not pair.upper & 0x80000000):
                    out.write('    if (!(vpu_stat() & 0x100u)) return done;\n')
                if len(edges) == 2:
                    true_edge = next(edge for edge in edges if edge.kind == 'branch_taken')
                    false_edge = next(edge for edge in edges if edge.kind == 'branch_not_taken')
                    for edge, conditional in ((true_edge, f'taken_{i}'),
                                               (false_edge, f'!taken_{i}')):
                        if edge.target is not None:
                            out.write(f'    if ({conditional}) goto node_{labels[edge.target]};\n')
                    out.write('    return done;\n')
                elif edges[0].target is None:
                    out.write('    return done;\n')
                else:
                    out.write(f'    goto node_{labels[edges[0].target]};\n')
                out.write('}\n')
            out.write('}\n')
        out.write('u32 vu_port_run_graph_pair(u32 index, u32 limit, VUFastPipes& pipes) {\n'
                  '    if (!vu_graph_empty_entry()) return 0;\n')
        if captured:
            out.write('    const u32 pc = VU1.VI[REG_TPC].UL & 0x3fffu;\n')
            for program, start, word, backup_safe in captured:
                out.write(f'    if (pc == {start}u && vu_graph_pair_matches(pc, '
                          f'0x{word:016x}ull) && vu_graph_validate_{program}({start}u) && '
                          f'vu_graph_backup_entry<{str(backup_safe).lower()}>()) '
                          f'return vu_graph_program_{program}(limit, pipes, {start}u);\n')
        out.write('    switch (index) {\n')
        for entry, (program, backup_safe) in roots.items():
            out.write(f'    case {entry}: return vu_graph_backup_entry<{str(backup_safe).lower()}>() ? '
                      f'vu_graph_program_{program}(limit, pipes, VU1.VI[REG_TPC].UL & 0x3fffu) : 0;\n')
        out.write('    default: return 0;\n    }\n}\n')
