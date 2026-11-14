"""Bounded static VU1 pair-PC / interpreter-pipeline-state graph.

Inputs are the pair words and _VURegsNum tuples produced by recomp_vu.analyse_fast:
(pipe, VFwrite, VFwxyzw, VFr0xyzw, VFr1xyzw, VFread0, VFread1,
 VIwrite, VIread, cycles). Graph PCs are VU1 micro-memory pair offsets
 (0..2047), while entry is the global ELF pair index of the uploaded program.

Each Node is the state *before* executing its pair. Each Edge carries the
successor state after exactly one pair and its interpreter-ordered pipeline
operations. A target of None is an interpreter fallback, never a guessed PC.
An edge with kind 'stop' retires the E-bit delay pair and flushes all pipes;
'fallback' leaves the registered upload span or reaches an unknown target.
The entry state is only valid for a freshly started, empty pipeline; callers
with a live VU pipeline must pass its actual abstract state or fall back.

A pipe record contains (remaining cycles, register, lane mask, producer PC),
not its *value*: generated code must preserve the interpreter's real pipe
payloads and flags. Records retain physical FMAC slot order because flushing
several slots can overwrite flags in slot order. Q and P hold remaining cycles
and their producer PC. Relative cycle ages advance on interpreter stalls;
events record stalls, additions, and flushes in order. VI backup records
(reg, remaining pair reads, origin), not a fabricated register value. A branch
comparison must use the actual VI/VIOldValue values at runtime. Branches in
branch delay slots retain the second target for execution after the first
branch target's first pair, matching the interpreter's takedelaybranch path.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from typing import Mapping, Sequence


FMAC, FDIV, EFU, BRANCH, XGKICK = 1, 2, 3, 5, 6
BACKUP_OPS = frozenset(('MI_IADDI', 'MI_IADDIU', 'MI_IADD', 'MI_IAND',
                        'MI_IOR', 'MI_ISUB', 'MI_ISUBIU', 'MI_MTIR',
                        'MI_ILW', 'MI_ILWR'))
CONDITIONAL = frozenset(('MI_IBEQ', 'MI_IBNE', 'MI_IBGEZ', 'MI_IBGTZ',
                         'MI_IBLEZ', 'MI_IBLTZ'))
JUMPS = CONDITIONAL | {'MI_B', 'MI_BAL', 'MI_JR', 'MI_JALR'}


@dataclass(frozen=True)
class Pair:
    lower: int
    upper: int
    upper_op: str
    lower_op: str
    upper_regs: tuple[int, ...]
    lower_regs: tuple[int, ...]


@dataclass(frozen=True)
class State:
    # None denotes an empty slot; cycle counts are relative to the next pair.
    fmac: tuple[tuple[int, int, int, int] | None, ...] = (None,) * 8
    fdiv: tuple[int, int] | None = None
    efu: tuple[int, int] | None = None
    vi_backup: tuple[int, int, int] | None = None
    branch: int | None = None
    delayed_branch: int | None = None
    ebit: int = 0


@dataclass(frozen=True)
class Node:
    pc: int
    state: State


@dataclass(frozen=True)
class Edge:
    source: Node
    target: Node | None
    kind: str
    events: tuple[tuple, ...]
    reason: str = ''
    # Successor pc/state after this pair, kept for exits (target None) so
    # generated code can rebuild the interpreter's pipeline state there.
    next_pc: int | None = None
    next_state: State | None = None


@dataclass
class Graph:
    entry: Node
    edges: dict[Node, tuple[Edge, ...]]

    @property
    def fallback_count(self) -> int:
        return sum(edge.kind == 'fallback' for edges in self.edges.values()
                   for edge in edges)


# PCSX2 interpreter VUops.cpp: _vuFMACTestStall sets cycle to sCycle+4
# (even if a subsequent read shares a pair); _vuFlushFDIV/_vuFlushEFU set
# cycle to their completion cycle. _vuTestPipes runs after both pipe adds.
def _advance(fmac, fdiv, efu, backup, elapsed):
    fmac = [None if x is None else (x[0] - elapsed, *x[1:]) for x in fmac]
    fdiv = None if fdiv is None else (fdiv[0] - elapsed, fdiv[1])
    efu = None if efu is None else (efu[0] - elapsed, efu[1])
    backup = None if backup is None else (backup[0], backup[1], backup[2])
    return fmac, fdiv, efu, backup


def _step(node: Node, pair: Pair, outcome: bool | None, dest: int | None = None) -> tuple[int | None, State, tuple[tuple, ...], str]:
    pc, state = node.pc, node.state
    fmac, fdiv, efu = list(state.fmac), state.fdiv, state.efu
    backup = state.vi_backup
    events: list[tuple] = []
    elapsed = 0
    lower = pair.lower_regs if not pair.upper & 0x80000000 else (0,) * 10
    upper = pair.upper_regs
    # The interpreter probes upper and then lower stalls before executing
    # either operation; a lower VF write discarded by an upper write still
    # creates a pipe descriptor afterwards.
    for regs in (upper, lower):
        pipe = regs[0]
        if pipe == FMAC:
            for reg, lanes in ((regs[5], regs[3]), (regs[6], regs[4])):
                if not reg:
                    continue
                for slot, item in enumerate(fmac):
                    if item is None or item[1] != reg or not item[2] & lanes:
                        continue
                    # A prior age >=3 would already have been flushed at the
                    # previous pair end. The interpreter's +1 hack is real.
                    delta = item[0] + 1
                    elapsed += delta
                    fmac, fdiv, efu, backup = _advance(fmac, fdiv, efu, backup, delta)
                    fmac[slot] = None
                    events.append(('stall_fmac', slot, delta, item[3]))
                    fmac, fdiv, efu = _flush(fmac, fdiv, efu, events)
                    break
        elif pipe == FDIV and fdiv is not None:
            delta = fdiv[0]
            elapsed += delta
            fmac, fdiv, efu, backup = _advance(fmac, fdiv, efu, backup, delta)
            events.append(('wait_q', delta, fdiv[1]))
            fdiv = None
        elif pipe == EFU and efu is not None:
            delta = efu[0]
            elapsed += delta
            fmac, fdiv, efu, backup = _advance(fmac, fdiv, efu, backup, delta)
            events.append(('wait_p', delta, efu[1]))
            efu = None
    events.append(('execute_ops',))
    if pair.lower_op == 'MI_XGKICK' and not pair.upper & 0x80000000:
        # The lower opcode drains existing pipes before sending GIF PATH1,
        # then _vuAddUpperStalls queues this pair's upper result afterwards.
        # Its original opcode body performs the drain and GIF callback at
        # runtime; only advance the graph's physical slot state here.
        drain: list[tuple] = []
        while True:
            active = any(x is not None for x in fmac) or fdiv or efu
            fmac, fdiv, efu = _flush(fmac, fdiv, efu, drain)
            fmac, fdiv, efu, backup = _advance(fmac, fdiv, efu, backup, 1)
            elapsed += 1
            if not active:
                break
        events.append(('xgkick_drain', tuple(drain)))
    for regs in (upper, lower):
        if regs[0] == FMAC:
            reg = regs[1] if regs[1] else -18 if regs[7] & (1 << 18) else 0
            lanes = regs[2] if regs[1] else 0
            for slot, item in enumerate(fmac):
                if item is None:
                    fmac[slot] = (3, reg, lanes, pc)
                    events.append(('add_fmac', slot, reg, lanes))
                    break
            else:
                events.append(('fmac_full',))
        elif regs[0] == FDIV and regs[7] & (1 << 22):
            fdiv = (regs[9], pc)
            events.append(('add_q', regs[9]))
        elif regs[0] == EFU and regs[7] & (1 << 23):
            efu = (regs[9], pc)
            events.append(('add_p', regs[9]))
    fmac, fdiv, efu = _flush(fmac, fdiv, efu, events)

    if pair.lower_op in BACKUP_OPS and not (pair.upper & 0x80000000):
        # IADD/IAND/IOR/ISUB save VI[_Id_] but mark VIRegNumber as
        # _It_, even though _Id_ is the actual write destination.
        dest = (pair.lower >> 6) & 15 if pair.lower_op in (
            'MI_IADD', 'MI_IAND', 'MI_IOR', 'MI_ISUB') else (pair.lower >> 16) & 15
        if dest:
            reg = (pair.lower >> 16) & 15
            backup = (reg, 2, pc)
            events.append(('backup_vi', reg, dest, pc))
    if backup:
        backup = (backup[0], backup[1] - 1, backup[2])
        if not backup[1]:
            backup = None

    target = (pc + 1) & 2047
    pending, delayed = state.branch, state.delayed_branch
    if outcome:
        if dest is None:
            dest = ((pc + 1 + _signed11(pair.lower)) & 2047)
        if pending is not None:
            delayed = dest
        else:
            pending = dest
        events.append(('branch', dest, bool(state.branch is not None)))
    if state.branch is not None:
        target = state.branch
        pending, delayed = delayed, None
    ebit = 2 if pair.upper & 0x40000000 else state.ebit
    if ebit:
        ebit -= 1
        if not ebit:
            # _vuFlushAll increments cycle once even when no pipe remains.
            end: list[tuple] = []
            while any(x is not None for x in fmac) or fdiv or efu:
                # _vuFlushAll's drain order (recorded for static publication).
                fmac, fdiv, efu = _flush(fmac, fdiv, efu, end)
                fmac, fdiv, efu, backup = _advance(fmac, fdiv, efu, backup, 1)
                elapsed += 1
            fmac, fdiv, efu, backup = _advance(fmac, fdiv, efu, backup, 1)
            elapsed += 1
            events.append(('end_flush', elapsed, tuple(end)))
            backup = None
    fmac, fdiv, efu, backup = _advance(fmac, fdiv, efu, backup, 1)
    events.append(('cycles', elapsed + 1))
    return target, State(tuple(fmac), fdiv, efu, backup, pending, delayed, ebit), tuple(events), 'stop' if (pair.upper & 0x40000000 == 0 and state.ebit == 1) else 'next'


def _flush(fmac, fdiv, efu, events):
    for slot, item in enumerate(fmac):
        if item is not None and item[0] <= 0:
            events.append(('flush_fmac', slot, item[3]))
            fmac[slot] = None
    if fdiv is not None and fdiv[0] <= 0:
        events.append(('flush_q', fdiv[1]))
        fdiv = None
    if efu is not None and efu[0] <= 0:
        events.append(('flush_p', efu[1]))
        efu = None
    return fmac, fdiv, efu


def _signed11(word: int) -> int:
    value = word & 0x7ff
    return value - 0x800 if value & 0x400 else value


def analyze(pairs: Mapping[int, Pair] | Sequence[Pair], entry: int,
            length: int, *, initial: State | None = None,
            max_states: int = 10000) -> Graph:
    """Analyze a registered (pair entry, byte length); never follow unknown edges."""
    return analyze_image(pairs, entry, length, [0], initial=initial, max_states=max_states)


def analyze_image(pairs: Mapping[int, Pair] | Sequence[Pair], entry: int,
                  length: int, roots: Sequence[int], *, initial: State | None = None,
                  indirect: Mapping[int, Sequence[int]] | None = None,
                  max_states: int = 10000) -> Graph:
    """Graph from several empty-pipeline roots.

    indirect (when given) maps a JR/JALR pair to its observed targets: the
    jump executes in the graph, known targets continue statically and any
    other runtime target leaves through an 'indirect_unknown' exit whose
    next_state carries branch=-1 (the runtime branchpc).
    """
    if length <= 0 or length % 8 or length > 16384 or entry < 0 or max_states < 1:
        raise ValueError('invalid VU1 program span or state limit')
    start = [Node(root, initial if initial is not None else State()) for root in roots]
    graph = Graph(start[0], {})
    graph.roots = start
    queue = deque(start)
    scheduled = set(start)
    end = length // 8
    while queue:
        node = queue.popleft()
        if not 0 <= node.pc < end or entry + node.pc not in pairs:
            graph.edges[node] = (Edge(node, None, 'fallback', (), 'pair outside decoded program'),)
            continue
        pair = pairs[entry + node.pc]
        reason = ''
        jump = pair.lower_op in ('MI_JR', 'MI_JALR') and not pair.upper & 0x80000000
        if pair.upper & 0x18000000:
            reason = 'D/T interrupt enable depends on runtime FBRST'
        elif jump and (indirect is None or node.state.branch is not None):
            reason = 'runtime VI jump target'
        elif pair.lower_regs[0] == BRANCH and pair.lower_op not in JUMPS and not pair.upper & 0x80000000:
            reason = 'unknown branch opcode'
        if reason:
            graph.edges[node] = (Edge(node, None, 'fallback', (), reason),)
            continue
        branch = pair.lower_op if not pair.upper & 0x80000000 else ''
        if jump:
            steps = [(True, t & 2047, 'indirect') for t in sorted(set(indirect.get(entry + node.pc, ())))]
            steps.append((True, -1, 'indirect_unknown'))
        else:
            outcomes = (False, True) if branch in CONDITIONAL else (True,) if branch in JUMPS else (None,)
            steps = [(outcome, None, None) for outcome in outcomes]
        edges = []
        for outcome, dest, jump_kind in steps:
            pc, state, events, kind = _step(node, pair, outcome, dest)
            if jump_kind == 'indirect_unknown':
                edges.append(Edge(node, None, jump_kind, events, 'unobserved jump target', pc, state))
                continue
            if kind == 'stop':
                edges.append(Edge(node, None, kind, events, '', pc, state))
            elif not 0 <= pc < end or entry + pc not in pairs:
                edges.append(Edge(node, None, 'fallback', events,
                                  f'branch target {pc} outside decoded program', pc, state))
            else:
                target = Node(pc, state)
                if target not in scheduled:
                    if len(scheduled) >= max_states:
                        edges.append(Edge(node, None, 'fallback', events, 'state limit', pc, state))
                        continue
                    scheduled.add(target)
                    queue.append(target)
                edges.append(Edge(node, target, jump_kind or ('branch_taken' if outcome is True else
                                  'branch_not_taken' if outcome is False else kind), events,
                                  '', pc, state))
        graph.edges[node] = tuple(edges)
    return graph
