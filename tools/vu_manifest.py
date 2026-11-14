#!/usr/bin/env python3
"""Recover observed VU launch code from user-owned ELF and capture images.

Manifest entries contain only ELF file offsets and micro-memory offsets; game
bytes remain in the external ELF and capture directory, not the repository.
"""
import argparse
from collections import Counter, deque
import json
import pathlib
import struct

from recomp_vu import START, END, decode, load_region, opcode_tables

BRANCHES = {'MI_B', 'MI_BAL', 'MI_IBEQ', 'MI_IBNE', 'MI_IBGEZ',
            'MI_IBGTZ', 'MI_IBLEZ', 'MI_IBLTZ'}
CONDITIONAL = BRANCHES - {'MI_B', 'MI_BAL'}
INDIRECT = {'MI_JR', 'MI_JALR'}


def elf_file_offset(elf: bytes) -> int:
    phoff, = struct.unpack_from('<I', elf, 28)
    entsize, count = struct.unpack_from('<HH', elf, 42)
    for idx in range(count):
        kind, offset, addr, _, size, _, _, _ = struct.unpack_from(
            '<IIIIIIII', elf, phoff + idx * entsize)
        if kind == 1 and addr <= START and END <= addr + size:
            return offset + START - addr
    raise ValueError('VU microcode is missing from the ELF load segments')


def reachable(image: bytes, start: int, tables: dict[str, list[str]]) -> tuple[set[int], bool, bool]:
    """Conservative CFG including delay pairs; dynamic targets are explicit."""
    slots = len(image) // 8
    queue = deque([(start // 8, False, -1)])
    seen = set()
    pcs = set()
    dynamic = False
    terminates = False
    while queue:
        pc, end_after, delayed_target = queue.popleft()
        pc %= slots
        key = pc, end_after, delayed_target
        if key in seen:
            continue
        seen.add(key)
        pcs.add(pc)
        lower, upper = struct.unpack_from('<II', image, pc * 8)
        if end_after:
            terminates = True
            continue
        branch = '' if upper & 0x80000000 else decode(lower, False, tables)
        immediate = lower & 0x7ff
        if immediate & 0x400:
            immediate -= 0x800
        next_pc = (pc + 1) % slots
        end = bool(upper & 0x40000000)
        # A branch is retired after one delay pair, and may itself reside in
        # an earlier branch's delay slot. Such nested control is rare; mark
        # dynamic instead of inventing the delayed-branch ordering.
        if delayed_target >= 0 and branch in BRANCHES | INDIRECT:
            dynamic = True
            queue.append((next_pc, end, -1))
            continue
        if branch in INDIRECT:
            dynamic = True
            queue.append((next_pc, end, -1))
        elif branch in BRANCHES:
            destination = (pc + 1 + immediate) % slots
            queue.append((next_pc, end, destination))
            if branch in CONDITIONAL:
                queue.append((next_pc, end, -1))
        elif delayed_target >= 0:
            queue.append((delayed_target, end, -1))
        else:
            queue.append((next_pc, end, -1))
    return pcs, dynamic, terminates


def fragments(image: bytes, visible: set[int], elf_words: list[bytes], file_offset: int) -> list[dict[str, int]]:
    lookup: dict[bytes, list[int]] = {}
    for idx, pair in enumerate(elf_words):
        lookup.setdefault(pair, []).append(idx)
    pending = set(visible)
    result = []
    empty = bytes(8)
    while pending:
        target = min(pending)
        word = image[target * 8:target * 8 + 8]
        if word == empty:
            pending.remove(target)
            continue
        longest = (0, -1)
        for elf_idx in lookup.get(word, []):
            count = 0
            while (target + count in pending and target + count < len(image) // 8
                   and elf_idx + count < len(elf_words)
                   and image[(target + count) * 8:(target + count + 1) * 8] == elf_words[elf_idx + count]):
                count += 1
            if count > longest[0]:
                longest = count, elf_idx
        if not longest[0]:
            raise ValueError(f'captured pair at micro offset {target * 8:#x} is absent from the ELF')
        count, elf_idx = longest
        result.append({'elf_offset': file_offset + elf_idx * 8,
                       'micro_offset': target * 8, 'length': count * 8})
        pending.difference_update(range(target, target + count))
    return result


def build(elf_path: pathlib.Path, capture: pathlib.Path) -> dict:
    elf = elf_path.read_bytes()
    game = load_region(elf_path)
    elf_words = [game[idx:idx + 8] for idx in range(0, len(game), 8)]
    tables = opcode_tables((pathlib.Path(__file__).resolve().parent.parent /
                            'third_party/pcsx2_vu/VUops.cpp').read_text())
    calls = Counter()
    for line in (capture / 'launches.tsv').read_text().splitlines():
        unit, digest, start = line.split('\t')
        calls[int(unit), f'{int(digest, 16):016x}', int(start, 16)] += 1
    # JR/JALR targets reached inside launched images are entry points too:
    # a dispatcher's subroutines are not reachable from its static CFG.
    indirect = capture / 'indirect.tsv'
    if indirect.exists():
        for line in indirect.read_text().splitlines():
            unit, digest, start = line.split('\t')
            calls[int(unit), f'{int(digest, 16):016x}', int(start, 16)] += 0
    records = []
    for (unit, digest, start), count in sorted(calls.items()):
        image = (capture / f'micro_vu{unit}_{digest}.bin').read_bytes()
        if len(image) != (16384 if unit else 4096):
            raise ValueError(f'invalid captured image size for vu{unit}:{digest}')
        visible, dynamic, terminates = reachable(image, start, tables)
        try:
            pieces = fragments(image, visible, elf_words, elf_file_offset(elf))
        except ValueError as error:
            print(f'skipping vu{unit}:{digest}:{start:#x}: {error}')
            continue
        records.append({'unit': unit, 'start_pc': start, 'capture_hash': digest,
                        'launches': count, 'reachable_pairs': sorted(visible),
                        'dynamic_branch': dynamic, 'can_reach_ebit': terminates,
                        'fragments': pieces})
    return {'format': 1, 'programs': records}


def build_images(elf_path: pathlib.Path, captures: list[pathlib.Path]) -> dict:
    """Format 2: one record per captured VU0/VU1 micro image.

    roots: launch starts and every JR/JALR target (byte offsets); edges maps
    a JR/JALR pair (byte offset) to the targets it was seen to take;
    fragments cover all code statically reachable from the roots.
    """
    elf = elf_path.read_bytes()
    game = load_region(elf_path)
    elf_words = [game[idx:idx + 8] for idx in range(0, len(game), 8)]
    tables = opcode_tables((pathlib.Path(__file__).resolve().parent.parent /
                            'third_party/pcsx2_vu/VUops.cpp').read_text())
    images: dict[str, dict] = {}
    files: dict[str, pathlib.Path] = {}
    for capture in captures:
        for line in (capture / 'launches.tsv').read_text().splitlines():
            unit, digest, start = line.split('\t')
            if unit not in ('0', '1'):
                continue
            key = int(unit), f'{int(digest, 16):016x}'
            record = images.setdefault(key, {'launches': Counter(), 'edges': {}})
            record['launches'][int(start, 16)] += 1
            files[key] = capture / f'micro_vu{key[0]}_{key[1]}.bin'
        indirect = capture / 'indirect.tsv'
        if not indirect.exists():
            continue
        for line in indirect.read_text().splitlines():
            fields = line.split('\t')
            if fields[0] not in ('0', '1') or len(fields) < 4:
                continue
            key = int(fields[0]), f'{int(fields[1], 16):016x}'
            record = images.setdefault(key, {'launches': Counter(), 'edges': {}})
            record['edges'].setdefault(int(fields[3], 16), set()).add(int(fields[2], 16))
            files.setdefault(key, capture / f'micro_vu{key[0]}_{key[1]}.bin')
    records = []
    for (unit, digest), record in sorted(images.items(), key=lambda kv: (-kv[0][0], kv[0][1])):
        if not record['launches'] or not files[unit, digest].exists():
            continue
        image = files[unit, digest].read_bytes()
        if len(image) != (16384 if unit else 4096):
            raise ValueError(f'invalid captured image size for vu{unit}:{digest}')
        targets = {t for ts in record['edges'].values() for t in ts}
        roots = sorted(set(record['launches']) | targets)
        visible = set()
        for root in roots:
            visible |= reachable(image, root, tables)[0]
        try:
            pieces = fragments(image, visible, elf_words, elf_file_offset(elf))
        except ValueError as error:
            print(f'skipping vu{unit}:{digest}: {error}')
            continue
        records.append({'unit': unit, 'hash': digest,
                        'launches': {f'{k:x}': v for k, v in sorted(record['launches'].items())},
                        'roots': roots,
                        'edges': {f'{k:x}': sorted(v) for k, v in sorted(record['edges'].items())},
                        'fragments': pieces})
    return {'format': 2, 'images': records}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', action='store_true',
                        help='format 2: group by micro image (several capture directories allowed)')
    parser.add_argument('elf', type=pathlib.Path)
    parser.add_argument('capture', type=pathlib.Path, nargs='+')
    parser.add_argument('manifest', type=pathlib.Path)
    args = parser.parse_args()
    if args.images:
        data = build_images(args.elf, args.capture)
        args.manifest.write_text(json.dumps(data, indent=1) + '\n')
        counts = Counter(record['unit'] for record in data['images'])
        print(f"Recovered {counts[1]} VU1 and {counts[0]} VU0 micro images")
        return
    data = build(args.elf, args.capture[0])
    args.manifest.write_text(json.dumps(data, indent=2) + '\n')
    print(f"Recovered {len(data['programs'])} distinct launch images")


if __name__ == '__main__':
    main()
