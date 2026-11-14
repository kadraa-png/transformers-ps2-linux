"""Emit one C++ function per captured VU0/VU1 micro image (manifest format 2).

The graph (vu_pipeline.analyze_image) has already resolved, for every
(pair, pipeline state) node, which FMAC/FDIV/EFU slots stall, flush and are
added, and in which order.  The generated code therefore does not maintain
PCSX2's pipe descriptors while it runs:

* slot payloads (MAC/status/clip copies, Q/P results) live in a local
  VULeanPayload and are published to VI exactly where the interpreter would;
* the cycle counter is a local, added once per pair;
* MAC/status flags are only computed when the image contains a reader
  (FM*/FS* or an integer op naming VI16/17); clip is always exact;
* JR/JALR continue at the targets captured for that jump site;
* every exit (unknown target, unmapped code, D/T bits, limit, stop) writes
  the interpreter's complete pipeline state from a static VULeanExit table,
  so the PCSX2 interpreter resumes exactly where the graph left off.

VU0 images (record unit 0) go to their own shard compiled with
VU_IMAGE_UNIT 0, where VUI names VU0: 4 KiB micro/data memory, LQ/SQ through
the interpreter bodies (VU0 maps VU1's registers at 0x4000), exact MAC/status
flags (the EE reads them with CFC2) and their own E-bit/VEW bits.
"""
from __future__ import annotations

import io
import os
import json
import pathlib
import re

from vu_pipeline import BACKUP_OPS, JUMPS, CONDITIONAL, Pair, analyze_image

ARITHMETIC = re.compile(r'MI_(MADD|MSUB|MUL|ADD|SUB)(A?)([iqxyzw]?)')
MS_READERS = frozenset(('MI_FMAND', 'MI_FMEQ', 'MI_FMOR', 'MI_FSAND', 'MI_FSEQ', 'MI_FSOR'))
REG_STATUS, REG_MAC = 16, 17
NO_BRANCH = -2


# VU1MI_<op> wrappers that just call _vu<op>(&VU1) can be inlined through
# vu_inl; the large transcendental EFU bodies stay out of line.
_SOURCE = (pathlib.Path(__file__).resolve().parent.parent / 'third_party/pcsx2_vu/VUops.cpp').read_text()
INLINE_BODY = {f'MI_{op}': body for op, body in re.findall(
    r'static void VU1MI_(\w+)\(\)\s*\{\s*(_vu\w+)\(&VU1\);\s*\}', _SOURCE)}
for big in ('MI_ESIN', 'MI_EATAN', 'MI_EATANxy', 'MI_EATANxz', 'MI_EEXP', 'MI_XGKICK'):
    INLINE_BODY.pop(big, None)


def op_call(opcode: str, unit: int = 1) -> str:
    body = INLINE_BODY.get(opcode)
    return f'vu_inl<{body}>' if body else f'VU{unit}{opcode}'


# ---- clean-lane analysis -------------------------------------------------
# A lane is "clean" when it can never hold a denormal, Inf or NaN, so the
# interpreter's vuDouble clamp is the identity on it.  Bit reg*4+lane
# (lane 0 = x); register 32 is ACC.  VF0 is the constant (0,0,0,1).
ACC = 32
VF0_BITS = 0xf


def _lanes(mask: int) -> list[int]:
    """PCSX2 xyzw field (x = bit 3) -> lane numbers."""
    return [lane for lane in range(4) if mask & (8 >> lane)]


def _bits(reg: int, lanes) -> int:
    return sum(1 << (reg * 4 + lane) for lane in lanes)


def _clean(state: int, reg: int, lanes) -> bool:
    return all(state >> (reg * 4 + lane) & 1 for lane in lanes)


def _arith_operands(pair: Pair):
    """(fs, ft lanes needed, acc needed) for a SIMD arithmetic upper op."""
    match = ARITHMETIC.fullmatch(pair.upper_op)
    word = pair.upper
    lanes = _lanes((word >> 21) & 15)
    operation, accum, source = match.groups()
    fs, ft = (word >> 11) & 31, (word >> 16) & 31
    if source in 'xyzw' and source:
        ft_lanes = [('xyzw').index(source)]
    elif source in ('i', 'q'):
        ft_lanes = None
    else:
        ft_lanes = lanes
    return fs, lanes, ft, ft_lanes, operation in ('MADD', 'MSUB'), accum == 'A'


def _transfer(pair: Pair, state: int) -> int:
    before = state
    word, op = pair.upper, pair.upper_op
    lanes = _lanes((word >> 21) & 15)
    fs, ft, fd = (word >> 11) & 31, (word >> 16) & 31, (word >> 6) & 31
    uwrite = pair.upper_regs[1]
    if ARITHMETIC.fullmatch(op) and op not in ('MI_ADDi', 'MI_ADDAi'):
        _, _, _, _, _, accum = _arith_operands(pair)
        dest = ACC if accum else fd
        if dest:
            state |= _bits(dest, lanes)
    elif op.startswith(('MI_MAX', 'MI_MINI')):
        suffix = op[6:] if op.startswith('MI_MAX') else op[7:]
        for lane in lanes:
            src = None if suffix == 'i' else (ft, 'xyzw'.index(suffix) if suffix else lane)
            ok = _clean(before, fs, [lane]) and src is not None and _clean(before, src[0], [src[1]])
            if fd:
                state = state | _bits(fd, [lane]) if ok else state & ~_bits(fd, [lane])
    elif op == 'MI_ABS':
        for lane in lanes:
            ok = _clean(before, fs, [lane])
            if ft:
                state = state | _bits(ft, [lane]) if ok else state & ~_bits(ft, [lane])
    elif op.startswith('MI_ITOF'):
        if ft:
            state |= _bits(ft, lanes)
    else:
        if uwrite:
            state &= ~_bits(uwrite, _lanes(pair.upper_regs[2]))
        if op.startswith('MI_OPM'):
            state &= ~_bits(ACC, range(4))
    mid = state
    if not word & 0x80000000:
        lwrite, lmask = pair.lower_regs[1], pair.lower_regs[2]
        discarded = lwrite and lwrite == uwrite
        if pair.lower_op == 'MI_MOVE' and not discarded:
            lw = pair.lower
            src, dst = (lw >> 11) & 31, (lw >> 16) & 31
            view = before if src == uwrite else mid
            for lane in _lanes((lw >> 21) & 15):
                if dst:
                    ok = _clean(view, src, [lane])
                    state = state | _bits(dst, [lane]) if ok else state & ~_bits(dst, [lane])
        elif lwrite and not discarded:
            state &= ~_bits(lwrite, _lanes(lmask))
    return state | VF0_BITS


def clean_lanes(graph, typed) -> dict:
    """Forward must-analysis: clean lanes on entry to every graph node."""
    clean_in = {root: VF0_BITS for root in graph.roots}
    work = list(graph.roots)
    while work:
        node = work.pop()
        edges = graph.edges[node]
        if edges[0].kind == 'fallback' and not edges[0].events:
            continue
        out = _transfer(typed[node.pc], clean_in[node])
        for edge in edges:
            target = edge.target
            if target is None:
                continue
            new = out if target not in clean_in else clean_in[target] & out
            if clean_in.get(target) != new:
                clean_in[target] = new
                work.append(target)
    return clean_in


def upper_call(pair: Pair, flags: int, clean: int = 0, unit: int = 1) -> str:
    word, opcode = pair.upper, pair.upper_op
    match = ARITHMETIC.fullmatch(opcode)
    if not match:
        return op_call(opcode, unit)
    operation, accum, source = match.groups()
    source_name = {'': 'Vector', 'i': 'I', 'q': 'Q',
                   'x': 'X', 'y': 'Y', 'z': 'Z', 'w': 'W'}[source]
    target = 'ACC' if accum else 'VF'
    fs, lanes, ft, ft_lanes, uses_acc, _ = _arith_operands(pair)
    clamp_a = not _clean(clean, fs, lanes)
    clamp_b = ft_lanes is None or not _clean(clean, ft, ft_lanes)
    clamp_acc = uses_acc and not _clean(clean, ACC, lanes)
    b = lambda x: str(x).lower()
    return (f'vu_lean_math<0x{word:08x}u, vu_simd::Op::{operation.title()}, '
            f'vu_simd::Source::{source_name}, vu_simd::Target::{target}, '
            f'VU{unit}{opcode}, {flags}, {b(clamp_a)}, {b(clamp_b)}, {b(clamp_acc or not uses_acc)}>')


def load_images(manifest, pairs, elf_offset, decode, tables, descriptors):
    """Yield (record, typed pairs by micro pair pc, ELF index by pc)."""
    for record in json.loads(manifest.read_text()).get('images', []):
        indices = {}
        for fragment in record['fragments']:
            source = fragment['elf_offset'] - elf_offset
            if source % 8 or source < 0 or source + fragment['length'] > len(pairs) * 8:
                raise ValueError('manifest fragment outside ELF VU region')
            for offset in range(0, fragment['length'], 8):
                indices[(fragment['micro_offset'] + offset) // 8 & 2047] = (source + offset) // 8
        typed = {}
        for pc, idx in indices.items():
            lower, upper = pairs[idx]
            uop = decode(upper, True, tables)
            lop = decode(lower, False, tables) if not upper & 0x80000000 else 'MI_NOP'
            typed[pc] = Pair(lower, upper, uop, lop, *descriptors[idx])
        yield record, typed, indices


def needs_ms_flags(typed) -> bool:
    for pair in typed.values():
        if pair.upper & 0x80000000:
            continue
        if pair.lower_op in MS_READERS:
            return True
        if pair.lower_regs[8] & ((1 << REG_STATUS) | (1 << REG_MAC)):
            return True
    return False


# Upper-op flag work per graph node (vu_simd::FlagsNone..FlagsFull).
FLAGS_NONE, FLAGS_Z, FLAGS_MAC, FLAGS_FULL = 0, 1, 2, 3
MAC_READERS = frozenset(('MI_FMAND', 'MI_FMEQ', 'MI_FMOR'))
# FSSET keeps the published status bits 0-5.
STATUS_READERS = frozenset(('MI_FSAND', 'MI_FSEQ', 'MI_FSOR', 'MI_FSSET'))
# FDIV snapshots the current (unpublished) status into its Q payload, which
# later becomes the published status.
DIV_OPS = frozenset(('MI_DIV', 'MI_SQRT', 'MI_RSQRT'))
# Pairs (each at least one cycle) after which an FMAC op has certainly left
# the 4-cycle pipe and published its MAC/status copy.
MS_WINDOW = 6
# A Q result is published at most 7 cycles after its FDIV op.
DIV_LATENCY = 7


def _reads_ms(pair: Pair) -> bool:
    if pair.upper & 0x80000000:
        return False
    return (pair.lower_op in MS_READERS or pair.lower_op in STATUS_READERS or
            pair.lower_op in DIV_OPS or
            bool(pair.lower_regs[8] & ((1 << REG_STATUS) | (1 << REG_MAC))))


def _reader_kind(pair: Pair):
    if pair.upper & 0x80000000:
        return None
    if pair.lower_op in STATUS_READERS or pair.lower_regs[8] & (1 << REG_STATUS):
        return 'status'
    if pair.lower_op in MAC_READERS or pair.lower_regs[8] & (1 << REG_MAC):
        return 'mac'
    if pair.lower_op in DIV_OPS:
        return 'div'
    return None


def _writes_ms(pair: Pair) -> bool:
    """Upper ops whose PCSX2 bodies update MAC/status (the descriptors'
    VIwrite does not name the flags for FMAC ops)."""
    return bool(ARITHMETIC.fullmatch(pair.upper_op)) or pair.upper_op in ('MI_OPMULA', 'MI_OPMSUB')


def ms_modes(graph, typed) -> dict:
    """Per node: how much of its MAC/status work the program can observe.

    Observers are MAC readers (FMAND/FMEQ/FMOR, VI17), status readers
    (FSAND/FSEQ/FSOR/FSSET, VI16) and exits that resume the interpreter
    inside this program (unknown jump target, unmapped code, D/T bit), which
    see both.  Program end and the pair limit are not observers, as in the
    image-wide "unobserved" mode.

    A published-flag observer sees the newest retired FMAC op: one of the
    writers within MS_WINDOW pairs before it, or the first writer beyond.
    Those need an exact MAC (FLAGS_MAC); for status observers they and the
    writer before the oldest of them (status bits 6-9 = the previous op's
    bits 0-3) need FLAGS_FULL.  FDIV copies the current status of the last
    executed writer (and so of the one before it) into Q's payload; that copy
    only matters if a status observer can run before a later FMAC op
    republishes the status.  A MAC update keeps the old zero bit of a lane
    that overflows, so every writer that can still reach a FLAGS_MAC or
    FLAGS_FULL writer keeps the four zero bits exact (FLAGS_Z); all others
    skip the flags entirely."""
    preds: dict = {}
    succs: dict = {}
    status_obs = set()
    mac_obs = []
    divs = []
    for node, node_edges in graph.edges.items():
        if node_edges[0].kind == 'fallback' and not node_edges[0].events:
            status_obs.add(node)
            continue
        kind = _reader_kind(typed[node.pc])
        if kind == 'status':
            status_obs.add(node)
        elif kind == 'mac':
            mac_obs.append(node)
        elif kind == 'div':
            divs.append(node)
        for edge in node_edges:
            if edge.target is not None:
                preds.setdefault(edge.target, []).append(node)
                succs.setdefault(node, []).append(edge.target)
            elif edge.kind != 'stop':
                status_obs.add(node)

    def div_matters(start) -> bool:
        # Forward: can a status observer run before an FMAC op issued after
        # the Q publish (>= DIV_LATENCY + 1 pairs on) has itself published?
        seen = set()
        work = [(start, 0, None)]
        while work:
            node, d, left = work.pop()
            if (node, d, left) in seen:
                continue
            seen.add((node, d, left))
            if node != start and node in status_obs:
                return True
            if left is None and d > DIV_LATENCY and _writes_ms(typed[node.pc]):
                left = MS_WINDOW + 1
            if left is not None:
                left -= 1
                if left < 0:
                    continue
            for nxt in succs.get(node, ()):
                work.append((nxt, min(d + 1, DIV_LATENCY + 1), left))
        return False

    roots = set(graph.roots)
    stops = [node for node, node_edges in graph.edges.items()
             if any(edge.kind == 'stop' for edge in node_edges)]

    def solve(status_obs) -> tuple[dict, bool]:
        """Modes for these observers, and whether an observer can see flags
        produced before the launch (a root reached before its writers)."""
        need: dict = {}
        exposed = False

        def mark(node, mode):
            if need.get(node, FLAGS_NONE) < mode:
                need[node] = mode

        def walk_back(starts, mode, beyond, window=MS_WINDOW):
            """Mark the writers within the window before each start, plus
            the first `beyond` writers past it, with `mode`."""
            nonlocal exposed
            seen = set()
            work = [(node, 0, 0) for node in starts]
            while work:
                node, d, k = work.pop()
                if (node, d, k) in seen:
                    continue
                seen.add((node, d, k))
                if _writes_ms(typed[node.pc]):
                    mark(node, mode)
                    if d > window:
                        k += 1
                        if k >= beyond:
                            continue
                if node in roots:
                    exposed = True
                nd = min(d + 1, window + 1)
                for pred in preds.get(node, ()):
                    work.append((pred, nd, k))

        walk_back(mac_obs, FLAGS_MAC, 1)
        walk_back(status_obs, FLAGS_FULL, 2)
        # FDIV reads the current status: its own pair's upper op and the
        # writer before it (a window of -1 counts every writer as "beyond").
        walk_back([n for n in divs if div_matters(n)], FLAGS_FULL, 2, window=-1)
        return need, exposed

    need, exposed = solve(status_obs)
    if exposed:
        # Some reader can see flags from the previous launch: keep the
        # flags exact at program end as well.
        status_obs |= set(stops)
        need, _ = solve(status_obs)

    live = {n for n, m in need.items() if m >= FLAGS_MAC}
    work = list(live)
    while work:
        node = work.pop()
        for pred in preds.get(node, ()):
            if pred not in live:
                live.add(pred)
                work.append(pred)
            if need.get(pred, FLAGS_NONE) < FLAGS_Z:
                need[pred] = FLAGS_Z
    return {node: need.get(node, FLAGS_NONE) for node in graph.edges}


def exit_entry(pc, state, typed) -> str:
    fr, freg, flanes = [], [], []
    for item in state.fmac:
        if item is None:
            fr.append(-1); freg.append(0); flanes.append(0)
        else:
            fr.append(item[0]); freg.append(item[1]); flanes.append(item[2])
    fdiv_r = fdiv_c = efu_r = efu_c = -1
    if state.fdiv is not None:
        fdiv_r = state.fdiv[0]
        fdiv_c = typed[state.fdiv[1]].lower_regs[9]
    if state.efu is not None:
        efu_r = state.efu[0]
        efu_c = typed[state.efu[1]].lower_regs[9]
    branch = NO_BRANCH if state.branch is None else state.branch
    delayed = NO_BRANCH if state.delayed_branch is None else state.delayed_branch
    lists = lambda xs: '{' + ', '.join(str(x) for x in xs) + '}'
    return (f'{{{pc & 2047}, {branch}, {delayed}, {state.ebit}, {fdiv_r}, {max(fdiv_c, 0)}, '
            f'{efu_r}, {max(efu_c, 0)}, {lists(fr)}, {lists(freg)}, {lists(flanes)}}}')


IMAGE_SHARDS = 4   # fixed: CMake lists vu_image_shard0..3.cpp (VU1) and shard4 (VU0)


def write_shards(path, texts, vu0_texts=()):
    """Spread the image graphs over IMAGE_SHARDS translation units (balanced
    by generated size) so no single compile needs more than ~2 GB.  Each shard
    compiles its own copy of VUops.cpp's opcode bodies (localized afterwards)
    so the graphs can inline them."""
    bins = [[] for _ in range(IMAGE_SHARDS)]
    sizes = [0] * IMAGE_SHARDS
    for number, text in sorted(texts, key=lambda t: -len(t[1])):
        k = sizes.index(min(sizes))
        bins[k].append((number, text))
        sizes[k] += len(text)
    bins.append(list(vu0_texts))   # shard IMAGE_SHARDS: the VU0 images
    for k, items in enumerate(bins):
        inc = path.parent / f'vu_image_{k}.inc'
        unit = 0 if k == IMAGE_SHARDS else 1
        with inc.open('w') as f:
            f.write(f'// Generated by tools/vu_image_emit.py: whole-image VU{unit} graphs (shard).\n')
            for _, text in sorted(items):
                f.write(text)
        with (path.parent / f'vu_image_shard{k}.cpp').open('w') as f:
            f.write('// Generated by tools/vu_image_emit.py.\n'
                    '#define VU_IMAGE_SHARD 1\n'
                    f'#define VU_IMAGE_UNIT {unit}\n'
                    f'#define VU_IMAGE_SHARD_INC "vu_image_{k}.inc"\n'
                    '#include "VUops.cpp"\n')
            for number, _ in sorted(items):
                f.write(f'u32 vu_img{number}_run_x(u32 limit, u32 root) '
                        f'{{ return vu_img{number}_run(limit, root); }}\n'
                        f'bool vu_img{number}_valid_x() {{ return vu_img{number}_valid(); }}\n')
        # Every other global symbol of the shard object is made local after
        # compiling (objcopy --keep-global-symbols), see runtime/src/vu/CMakeLists.txt.
        with (path.parent / f'vu_image_shard{k}.keep').open('w') as f:
            for number, _ in sorted(items):
                f.write(f'_Z{len(f"vu_img{number}_run_x")}vu_img{number}_run_xjj\n'
                        f'_Z{len(f"vu_img{number}_valid_x")}vu_img{number}_valid_xv\n')



def write_dispatch(out, texts, dispatch, unit):
    """vu_port_run_image (VU1) / vu_port_run_image0 (VU0).  The entry checks
    run in the dispatch TU, which is compiled for VU1, so VU0 names its
    registers explicitly."""
    regs = 'VU1' if unit else 'VU0'
    for number, _ in texts:
        out.write(f'u32 vu_img{number}_run_x(u32 limit, u32 root);\nbool vu_img{number}_valid_x();\n')
    out.write(f'u32 vu_port_run_image{"" if unit else "0"}(u32 limit) {{\n'
              f'    if (!vu_graph_empty_entry_of({regs})) return 0;\n'
              f'    switch (({regs}.VI[REG_TPC].UL & {"0x3fffu" if unit else "0xfffu"}) >> 3) {{\n')
    for pc, entries in sorted(dispatch.items()):
        out.write(f'    case {pc}:\n')
        for number, safe in entries:
            out.write(f'        if (vu_img{number}_valid_x() && vu_graph_backup_entry_of<{str(safe).lower()}>({regs}))\n'
                      f'            return vu_img{number}_run_x(limit, {pc}u);\n')
        out.write('        return 0;\n')
    out.write('    default: return 0;\n    }\n}\n')


def emit_images(path, pairs, tables, decode, descriptors, manifest, elf_offset) -> int:
    images = list(load_images(manifest, pairs, elf_offset, decode, tables, descriptors))
    dispatch = {}   # root byte pc -> [(image number, backup safe)]
    total_nodes = 0
    dispatch0 = {}  # the same for VU0 images
    texts = []      # (image number, generated C++)
    texts0 = []
    for number, (record, typed, indices) in enumerate(images):
        unit = record.get('unit', 1)
        size = 16384 if unit else 4096
        pcmask = size - 1
        if unit == 0 and any(p.lower_op == 'MI_XGKICK' for p in typed.values()):
            continue
        out = io.StringIO()
        roots = sorted({r // 8 & 2047 for r in record['roots']} & set(typed))
        if not roots:
            continue
        edges = {}
        for source, targets in record.get('edges', {}).items():
            edges[int(source, 16) // 8 & 2047] = [t // 8 & 2047 for t in targets]
        graph = analyze_image(typed, 0, size, roots, indirect=edges, max_states=60000)
        image_ms = needs_ms_flags(typed) or unit == 0
        # VU0 keeps exact flags everywhere: the EE reads them with CFC2.
        mode_of = ms_modes(graph, typed) if image_ms and unit else None
        clean = clean_lanes(graph, typed)
        nodes = list(graph.edges)
        total_nodes += len(nodes)
        label = {node: i for i, node in enumerate(nodes)}
        exits: dict[tuple, int] = {}

        def exit_id(pc, state):
            key = (pc & 2047, state)
            if key not in exits:
                exits[key] = len(exits)
            return exits[key]

        # Loop heads (any taken/indirect target) and roots check the pair limit.
        heads = set(graph.roots)
        for node, node_edges in graph.edges.items():
            for edge in node_edges:
                if edge.target is not None and edge.kind in ('branch_taken', 'indirect'):
                    heads.add(edge.target)

        body = []
        w = body.append
        for node in nodes:
            i = label[node]
            node_edges = graph.edges[node]
            w(f'n{i}: {{')
            if node in heads:
                w(f'    if (PS2_UNLIKELY(done >= limit)) return vu_lean_ret(X[{exit_id(node.pc, node.state)}], cyc, pl, done);')
            if node_edges[0].kind == 'fallback' and not node_edges[0].events:
                w(f'    return vu_lean_ret(X[{exit_id(node.pc, node.state)}], cyc, pl, done);')
                w('}')
                continue
            pair = typed[node.pc]
            index = indices[node.pc]
            events = node_edges[0].events
            # Slot payload copies and publishes stay whenever the image reads
            # flags (two stores); only the flag arithmetic itself varies.
            ms = image_ms
            mode = FLAGS_FULL if mode_of is None else mode_of[node]
            lower_enabled = not pair.upper & 0x80000000
            lop = pair.lower_op if lower_enabled else 'MI_NOP'

            def publish_slot(slot):
                w(f'    VUI.VI[REG_CLIP_FLAG].UL = pl.sc[{slot}];')
                if ms:
                    w(f'    VUI.VI[REG_MAC_FLAG].UL = pl.sm[{slot}];')
                    w(f'    VUI.VI[REG_STATUS_FLAG].UL = pl.ss[{slot}];')

            def publish(event):
                tag = event[0]
                if tag in ('stall_fmac', 'flush_fmac'):
                    publish_slot(event[1])
                elif tag in ('wait_q', 'flush_q'):
                    w('    VUI.VI[REG_Q].UL = pl.qv;')
                    if ms:
                        w('    VUI.VI[REG_STATUS_FLAG].UL = pl.qs;')
                elif tag in ('wait_p', 'flush_p'):
                    w('    VUI.VI[REG_P].UL = pl.pv;')

            after = False
            for event in events:
                if event[0] == 'execute_ops':
                    break
                publish(event)
            # --- the pair's operations ---
            if lop in JUMPS:
                w(f'    VUI.VI[REG_TPC].UL = {(node.pc * 8 + 8) & pcmask}u;')
                w(f'    VUI.branch = {1 if node.state.branch is not None else 0};')
            uregs, lregs = f'vu_fast_upper_{index}', f'vu_fast_lower_{index}'
            ucall = upper_call(pair, mode if ms else FLAGS_NONE, clean.get(node, 0), unit)
            kick = lop == 'MI_XGKICK'
            if kick:
                lcall = 'vu_lean_nop'
            elif lop in (('MI_MOVE', 'MI_LQ', 'MI_SQ', 'MI_IADDIU') if unit else ('MI_MOVE', 'MI_IADDIU')):
                lcall = f'vu_graph_lower_{lop[3:].lower()}<0x{pair.lower:08x}u>'
            else:
                lcall = op_call(lop, unit)
            w(f'    vu_graph_ops<0x{pair.lower:08x}u, 0x{pair.upper:08x}u, {ucall}, {lcall}, '
              f'{uregs}, {lregs}, false>();')
            if kick:
                drain = next(e for e in events if e[0] == 'xgkick_drain')[1]
                for event in drain:
                    publish(event)
                w(f'    vu_lean_kick<0x{pair.lower:08x}u>();')
            conditional = lop in CONDITIONAL
            if conditional:
                test = 'VUI.takedelaybranch' if node.state.branch is not None else 'VUI.branch == 2'
                w(f'    const bool taken = {test};')
            if lop in ('MI_JR', 'MI_JALR'):
                w('    const u32 jump = (VUI.branchpc >> 3) & 2047u;')
            for event in events:
                if event[0] == 'execute_ops':
                    after = True
                    continue
                if not after:
                    continue
                tag = event[0]
                if tag == 'add_fmac':
                    w(f'    pl.sc[{event[1]}] = VUI.clipflag;')
                    if ms:
                        w(f'    pl.sm[{event[1]}] = VUI.macflag;')
                        w(f'    pl.ss[{event[1]}] = VUI.statusflag;')
                elif tag == 'add_q':
                    w('    pl.qv = VUI.q.UL;')
                    if ms:
                        w('    pl.qs = VUI.statusflag;')
                elif tag == 'add_p':
                    w('    pl.pv = VUI.p.UL;')
                elif tag in ('flush_fmac', 'flush_q', 'flush_p'):
                    publish(event)
            may_backup = node.state.vi_backup is not None or (
                lower_enabled and pair.lower_op in BACKUP_OPS)
            if may_backup:
                w('    if (VUI.VIBackupCycles > 0) --VUI.VIBackupCycles;')
            stop = next((e for e in node_edges if e.kind == 'stop'), None)
            if stop is not None:
                end = next(e for e in stop.events if e[0] == 'end_flush')
                for event in end[2]:
                    publish(event)
                w('    VUI.VIBackupCycles = 0;')
            cycles = next(e for e in events if e[0] == 'cycles')[1]
            w(f'    cyc += {cycles};')
            w('    ++done;')
            if stop is not None:
                w(f'    vpu_stat_clear({"0x100u" if unit else "0x1u"});')
                w(f'    vu_port_vif_vew_end({unit});')
                w(f'    return vu_lean_ret(X[{exit_id(stop.next_pc, stop.next_state)}], cyc, pl, done);')
                w('}')
                continue
            if kick:
                edge = node_edges[0]
                w(f'    if (!(vpu_stat() & 0x100u)) return vu_lean_ret(X[{exit_id(edge.next_pc, edge.next_state)}], cyc, pl, done);')

            def go(edge):
                if edge.target is not None:
                    return f'goto n{label[edge.target]};'
                return f'return vu_lean_ret(X[{exit_id(edge.next_pc, edge.next_state)}], cyc, pl, done);'

            if conditional:
                taken = next(e for e in node_edges if e.kind == 'branch_taken')
                fallthrough = next(e for e in node_edges if e.kind == 'branch_not_taken')
                w(f'    if (taken) {go(taken)}')
                w(f'    {go(fallthrough)}')
            elif lop in ('MI_JR', 'MI_JALR'):
                w('    switch (jump) {')
                for edge in node_edges:
                    if edge.kind == 'indirect':
                        w(f'    case {edge.next_state.branch}u: {go(edge)}')
                unknown = next(e for e in node_edges if e.kind == 'indirect_unknown')
                w(f'    default: {go(unknown)}')
                w('    }')
            else:
                w(f'    {go(node_edges[0])}')
            w('}')

        # --- emit the image ---
        arith = [n for n in nodes if ARITHMETIC.fullmatch(typed[n.pc].upper_op)]
        if not image_ms:
            ms_note = 'unobserved'
        elif mode_of is None:
            ms_note = 'exact'
        else:
            counts = [sum(1 for n in arith if mode_of[n] == m)
                      for m in (FLAGS_FULL, FLAGS_MAC, FLAGS_Z, FLAGS_NONE)]
            ms_note = (f'exact where observable (arithmetic nodes: {counts[0]} full, '
                       f'{counts[1]} MAC only, {counts[2]} zero bits only, {counts[3]} none)')
        if os.environ.get('VU_MS_STATS'):
            import collections
            readers = collections.Counter(typed[n.pc].lower_op for n in nodes if _reads_ms(typed[n.pc]))
            print(f'img{number}: {len(nodes)} nodes, MAC/status {ms_note} readers {dict(readers)}')
        words = sorted(indices.items())
        out.write(f'// image {record["hash"]}: {len(nodes)} nodes, {len(exits)} exit states, '
                  f'MAC/status {ms_note}\n')
        out.write(f'static const VULeanExit vu_img{number}_exits[{max(len(exits), 1)}] = {{\n')
        for (pc, state), _ in sorted(exits.items(), key=lambda kv: kv[1]):
            out.write(f'    {exit_entry(pc, state, typed)},\n')
        if not exits:
            out.write('    {0, -2, -2, 0, -1, 0, -1, 0, {-1,-1,-1,-1,-1,-1,-1,-1}, {}, {}},\n')
        out.write('};\n')
        out.write(f'static bool vu_img{number}_valid() {{\n'
                  '    static u64 generation = 0;\n    static bool valid = false;\n'
                  '    const u64 now = vu_compiled_generation(VU_IMAGE_UNIT);\n'
                  '    if (generation == now) return valid;\n'
                  '    generation = now;\n'
                  f'    static constexpr std::pair<u16, u16> words[{len(words)}] = {{\n')
        for pc, idx in words:
            out.write(f'        {{{pc * 8}, {idx}}},\n')
        out.write('    };\n'
                  '    for (const auto& [offset, index] : words) {\n'
                  '        const auto& pair = g_vu_compiled_pairs[index];\n'
                  '        if (!vu_graph_pair_matches(offset,\n'
                  '                u64(pair.lower_word) | (u64(pair.upper_word) << 32))) return valid = false;\n'
                  '    }\n    return valid = true;\n}\n')
        out.write(f'static u32 vu_img{number}_run(u32 limit, u32 root) {{\n'
                  f'    const VULeanExit* const X = vu_img{number}_exits;\n'
                  '    u32 done = 0, cyc = VUI.cycle;\n'
                  '    VULeanPayload pl{};\n'
                  '    switch (root) {\n')
        for root in graph.roots:
            out.write(f'    case {root.pc}u: goto n{label[root]};\n')
        out.write('    default: return 0;\n    }\n')
        out.write('\n'.join(body))
        out.write('\n}\n')
        for root in graph.roots:
            # A pending VI backup only matters to a conditional branch in
            # the first two pairs (the interpreter's VI_BACKUP readers).
            safe = all(typed[pc].lower_op not in CONDITIONAL or typed[pc].upper & 0x80000000
                       for pc in (root.pc, (root.pc + 1) & 2047) if pc in typed)
            (dispatch if unit else dispatch0).setdefault(root.pc, []).append((number, safe))
        (texts if unit else texts0).append((number, out.getvalue()))
    write_shards(path, texts, texts0)
    out = path.open('w')
    out.write('// Generated by tools/vu_image_emit.py: whole-image VU0/VU1 graph dispatch.\n'
              '// The graphs themselves live in vu_image_shard*.cpp.\n')
    write_dispatch(out, texts, dispatch, 1)
    write_dispatch(out, texts0, dispatch0, 0)
    out.close()
    return total_nodes
