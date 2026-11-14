"""Function discovery / control-flow analysis for the EE executable.

Produces a Program with:
  functions: entry -> Function(entry, addrs (sorted list of insn addrs),
                               jump_tables {jr_addr: [targets]},
                               tail_calls, labels)
The code generator consumes this.

Semantics contract:
  * every function is "all code reachable from its entry", following
    conditional branches, b/j to non-entries, jump tables and fall-through;
  * jal/jalr are host calls that return to pc+8;
  * j to a strong entry (other than the own entry) is a tail call;
  * fall-through into a strong entry is a tail call;
  * jr ra returns; jr rX (non table) is a dynamic tail call.
"""
import struct
import json
import sys
import os
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
from r5900 import decode  # noqa: E402


class Image:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.raw = d
        e_phoff, = struct.unpack_from('<I', d, 0x1C)
        e_phnum, = struct.unpack_from('<H', d, 0x2C)
        self.entry, = struct.unpack_from('<I', d, 0x18)
        self.segments = []
        for i in range(e_phnum):
            p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
                struct.unpack_from('<8I', d, e_phoff + i * 32)
            if p_type == 1 and p_filesz:
                self.segments.append((p_vaddr, d[p_off:p_off + p_filesz], p_memsz))
        # one big segment for this game
        self.base, self.data, self.memsz = self.segments[0]
        self.end = self.base + len(self.data)
        self.nwords = len(self.data) // 4
        self.words = struct.unpack('<%dI' % self.nwords, self.data[:self.nwords * 4])

    def word(self, a):
        i = (a - self.base) >> 2
        if 0 <= i < self.nwords:
            return self.words[i]
        return None


class Function:
    __slots__ = ('entry', 'addrs', 'jump_tables', 'dyn_jumps', 'calls', 'tail_calls',
                 'branch_targets', 'unresolved', 'falls_into')

    def __init__(self, entry):
        self.entry = entry
        self.addrs = set()
        self.jump_tables = {}      # jr pc -> list of targets
        self.dyn_jumps = set()     # jr pc with unresolved register target
        self.calls = set()
        self.tail_calls = {}       # pc of j / fallthrough -> target
        self.branch_targets = set()
        self.unresolved = []
        self.falls_into = {}       # last addr -> strong entry reached by fall-through


class Analyzer:
    def __init__(self, img, code_ranges, data_ranges, extra_entries=()):
        self.extra_entries = tuple(extra_entries)
        self.img = img
        self.code_ranges = code_ranges
        self.data_ranges = data_ranges
        self._cache = {}
        self.strong = set()
        self.weak = set()
        self.jump_table_targets = set()
        self.functions = {}
        self.log = []

    # -- helpers ---------------------------------------------------------
    def in_code(self, a):
        if a & 3:
            return False
        for lo, hi in self.code_ranges:
            if lo <= a < hi:
                return True
        return False

    def insn(self, a):
        i = self._cache.get(a)
        if i is None:
            w = self.img.word(a)
            i = decode(w if w is not None else 0xFFFFFFFF, a)
            self._cache[a] = i
        return i

    def looks_like_start(self, a):
        """Heuristic: address directly follows the end of a previous function."""
        for lo, hi in self.code_ranges:
            if a == lo:
                return True
        b = a - 4
        # skip alignment nops
        n = 0
        while self.in_code(b) and self.img.word(b) == 0 and n < 8:
            b -= 4
            n += 1
        # b is now the last non-nop; the function end is "jr ra; delay" or "j x; delay"
        for cand in (b - 4, b):
            if not self.in_code(cand):
                continue
            i = self.insn(cand)
            if i.op == 'jr' or i.op == 'j' or i.op == 'eret':
                return True
            if i.op == 'beq' and i.rs == 0 and i.rt == 0:
                return True
        return False

    # -- entry discovery ------------------------------------------------
    def collect_entries(self):
        img = self.img
        self.strong.add(img.entry)
        for lo, hi in self.code_ranges:
            for a in range(lo, hi, 4):
                i = self.insn(a)
                if i.op == 'jal' and self.in_code(i.target):
                    self.strong.add(i.target)
        # code constants built with lui/addiu or lui/ori in code
        self.code_consts = set()
        for lo, hi in self.code_ranges:
            for a in range(lo, hi - 4, 4):
                i = self.insn(a)
                if i.op != 'lui':
                    continue
                # look ahead a few instructions for addiu/ori with same reg
                for b in range(a + 4, min(a + 40, hi), 4):
                    j = self.insn(b)
                    if j.op in ('addiu', 'ori') and j.rs == i.rt:
                        v = ((i.imm << 16) + (j.simm if j.op == 'addiu' else j.imm)) & 0xFFFFFFFF
                        if self.in_code(v):
                            self.code_consts.add(v)
                        break
                    if j.op in ('lw', 'sw', 'lq', 'sq', 'lwc1', 'swc1', 'ld', 'sd', 'lb',
                                'lbu', 'lh', 'lhu', 'sb', 'sh') and j.rs == i.rt:
                        break
                    # register overwritten
                    if self._writes_gpr(j) == i.rt:
                        break
        # data pointers
        self.data_ptrs = set()
        for lo, hi in self.data_ranges:
            for a in range(lo, hi, 4):
                w = img.word(a)
                if w is not None and self.in_code(w):
                    self.data_ptrs.add(w)

    @staticmethod
    def _writes_gpr(i):
        o = i.op
        if o in ('addiu', 'addi', 'daddiu', 'daddi', 'slti', 'sltiu', 'andi', 'ori', 'xori',
                 'lui', 'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'ld', 'lq', 'lwl', 'lwr',
                 'ldl', 'ldr', 'mfc1', 'cfc1', 'qmfc2', 'cfc2', 'mfc0'):
            return i.rt
        if o in ('jal',):
            return 31
        if o in ('sw', 'sb', 'sh', 'sd', 'sq', 'swc1', 'sqc2', 'lwc1', 'lqc2', 'swl', 'swr',
                 'sdl', 'sdr', 'nop', 'j', 'jr', 'syscall', 'break', 'sync', 'cache', 'pref',
                 'mtc1', 'ctc1', 'qmtc2', 'ctc2', 'mtc0', 'mult', 'multu', 'div', 'divu',
                 'mthi', 'mtlo', 'mtsa', 'mtsab', 'mtsah', 'mult1', 'multu1', 'div1', 'divu1',
                 'mthi1', 'mtlo1', 'pmthi', 'pmtlo', 'pmthl.lw', 'ei', 'di', 'eret', 'invalid'):
            return None
        if i.is_branch:
            return 31 if i.op in ('bltzal', 'bgezal', 'bltzall', 'bgezall') else None
        if o.endswith('.s') or o.startswith('cvt') or o.startswith('c.') or o.startswith('v') \
                or o.startswith('bc'):
            return None
        return i.rd

    # -- jump tables ------------------------------------------------------
    def resolve_jump_table(self, jr_addr, func_lo_hint):
        """Return list of targets for a jr through a jump table, or None."""
        jr = self.insn(jr_addr)
        reg = jr.rs
        # walk backwards within ~16 insns
        load = None
        a = jr_addr - 4
        steps = 0
        while steps < 24 and self.in_code(a):
            i = self.insn(a)
            if i.op == 'lw' and i.rt == reg:
                load = (a, i)
                break
            if self._writes_gpr(i) == reg:
                return None
            a -= 4
            steps += 1
        if load is None:
            return None
        la, li = load
        base_reg = li.rs
        off = li.simm
        # find addu base_reg, X, Y
        a = la - 4
        steps = 0
        addu = None
        while steps < 24 and self.in_code(a):
            i = self.insn(a)
            if self._writes_gpr(i) == base_reg:
                if i.op in ('addu', 'daddu'):
                    addu = (a, i)
                break
            a -= 4
            steps += 1
        if addu is None:
            return None
        aa, ai = addu
        consts = self._const_regs_before(aa, 40)
        tbl = None
        idx_reg = None
        for r1, r2 in ((ai.rs, ai.rt), (ai.rt, ai.rs)):
            if r1 in consts:
                tbl = (consts[r1] + off) & 0xFFFFFFFF
                idx_reg = r2
                break
        if tbl is None:
            return None
        bound = self._jump_table_bound(aa, jr_addr)
        targets = []
        n = bound if bound is not None else 4096
        for k in range(n):
            w = self.img.word(tbl + 4 * k)
            if w is None or not self.in_code(w):
                break
            if bound is None and abs(w - jr_addr) > 0x40000:
                break
            targets.append(w)
        if not targets:
            return None
        if bound is not None and len(targets) != bound:
            self.log.append('jump table %08x bound %d but %d valid' % (jr_addr, bound, len(targets)))
        return targets

    def _jump_table_bound(self, index_addr, jr_addr):
        """N of the `sltiu R, idx, N` / `beq R, zero` guard before a table index.

        The guard register is found from the nearest preceding `beq/bne R, zero`;
        its definition may sit before that branch or in the delay slot of a
        branch into it (a loop's back edge), not just anywhere earlier.
        """
        a = index_addr - 4
        guard = None
        for _ in range(16):
            if not self.in_code(a):
                return None
            i = self.insn(a)
            if i.op in ('beq', 'bne') and i.rt == 0 and i.rs != 0:
                guard = (a, i.rs)
                break
            a -= 4
        if guard is None:
            return None
        guard_addr, reg = guard

        def bound_of(i):
            if self._writes_gpr(i) != reg:
                return False, None
            return True, (i.imm if i.op == 'sltiu' and i.imm < 0x8000 else None)
        # delay slots of branches that enter the guard
        for b in range(max(jr_addr - 0x1000, guard_addr - 0x1000), jr_addr + 0x1000, 4):
            if not self.in_code(b):
                continue
            i = self.insn(b)
            if i.is_branch and i.target == guard_addr:
                found, n = bound_of(self.insn(b + 4))
                if found:
                    return n
        # straight-line predecessors
        a = guard_addr - 4
        for _ in range(40):
            if not self.in_code(a):
                return None
            i = self.insn(a)
            found, n = bound_of(i)
            if found:
                return n
            a -= 4
        return None

    def _const_regs_before(self, addr, window):
        """Very small constant propagation over the straight-line code before addr."""
        start = addr - 4 * window
        consts = {}
        a = max(start, self.code_ranges[0][0])
        while a < addr:
            if not self.in_code(a):
                a += 4
                continue
            i = self.insn(a)
            o = i.op
            if o == 'lui':
                consts[i.rt] = (i.imm << 16) & 0xFFFFFFFF
            elif o == 'addiu' and i.rs in consts:
                consts[i.rt] = (consts[i.rs] + i.simm) & 0xFFFFFFFF
            elif o == 'ori' and i.rs in consts:
                consts[i.rt] = consts[i.rs] | i.imm
            else:
                w = self._writes_gpr(i)
                if w is not None:
                    consts.pop(w, None)
            if i.is_branch or i.is_jump:
                pass  # keep simple: tables are set up in one block typically
            a += 4
        return consts

    # -- function traversal ----------------------------------------------
    def traverse(self, entry):
        f = Function(entry)
        work = [entry]
        strong = self.strong
        while work:
            a = work.pop()
            while True:
                if a in f.addrs:
                    break
                if not self.in_code(a):
                    f.unresolved.append(('leave_code', a))
                    break
                i = self.insn(a)
                f.addrs.add(a)
                o = i.op
                if i.is_branch:
                    f.addrs.add(a + 4)
                    tgt = i.target
                    f.branch_targets.add(tgt)
                    work.append(tgt)
                    always = (o == 'beq' and i.rs == 0 and i.rt == 0) or \
                             (o == 'bgez' and i.rs == 0) or (o == 'bgezal' and i.rs == 0)
                    never = (o == 'bne' and i.rs == 0 and i.rt == 0)
                    if never:
                        f.branch_targets.discard(tgt)
                        work.pop()
                    if always:
                        break
                    a = self._fallthrough(f, a + 8)
                    if a is None:
                        break
                    continue
                if o == 'j':
                    f.addrs.add(a + 4)
                    tgt = i.target
                    if tgt in strong and tgt != entry:
                        f.tail_calls[a] = tgt
                    else:
                        f.branch_targets.add(tgt)
                        work.append(tgt)
                    break
                if o == 'jal':
                    f.addrs.add(a + 4)
                    f.calls.add(i.target)
                    a = self._fallthrough(f, a + 8)
                    if a is None:
                        break
                    continue
                if o == 'jalr':
                    f.addrs.add(a + 4)
                    a = self._fallthrough(f, a + 8)
                    if a is None:
                        break
                    continue
                if o == 'jr':
                    f.addrs.add(a + 4)
                    if i.rs != 31:
                        t = self.resolve_jump_table(a, entry)
                        if t:
                            f.jump_tables[a] = t
                            for x in t:
                                f.branch_targets.add(x)
                                work.append(x)
                                self.jump_table_targets.add(x)
                        else:
                            f.dyn_jumps.add(a)
                    break
                if o == 'eret':
                    break
                if o == 'invalid':
                    f.unresolved.append(('invalid', a))
                    break
                a = self._fallthrough(f, a + 4)
                if a is None:
                    break
        return f

    def _fallthrough(self, f, nxt):
        """Linear continuation into nxt; returns nxt or None if it is a tail call."""
        if nxt != f.entry and nxt in self.strong and nxt not in f.addrs:
            f.falls_into[nxt - 4] = nxt
            return None
        return nxt

    def run(self):
        self.collect_entries()
        # First pass: traverse from jal targets to find jump tables (so their
        # targets are not mistaken for function starts).
        for e in sorted(self.strong):
            self.functions[e] = self.traverse(e)
        # data pointers / code constants at plausible function starts become
        # strong entries unless they are known jump-table targets
        extra_strong = set()
        extra_weak = set()
        for p in sorted(self.data_ptrs | self.code_consts):
            if p in self.strong or p in self.jump_table_targets:
                continue
            if self.looks_like_start(p):
                extra_strong.add(p)
            else:
                extra_weak.add(p)
        self.strong |= extra_strong
        self.weak = extra_weak
        # Manually supplied entries (found at runtime) are strong.
        self.strong |= set(self.extra_entries)
        # Second pass with the final strong set
        self.functions = {}
        for e in sorted(self.strong | self.weak):
            self.functions[e] = self.traverse(e)
        # Code never reached from any entry (dead or referenced in ways we do
        # not see): give every plausible function start a weak entry so a
        # runtime dispatch to it still finds recompiled code.
        covered = set()
        for f in self.functions.values():
            covered |= f.addrs
        for lo, hi in self.code_ranges:
            for a in range(lo, hi, 4):
                if a in covered or self.img.word(a) == 0:
                    continue
                if self.looks_like_start(a):
                    f = self.traverse(a)
                    self.functions[a] = f
                    self.weak.add(a)
                    covered |= f.addrs
        return self


def default_analyzer(elf_path):
    img = Image(elf_path)
    code_ranges = [(0x100000, 0x5dcb60), (0x671980, 0x6adf20)]
    data_ranges = [(0x5dcb60, 0x671980), (0x6adf20, img.end)]
    return Analyzer(img, code_ranges, data_ranges)


if __name__ == '__main__':
    an = default_analyzer(sys.argv[1]).run()
    fs = an.functions
    total = sum(len(f.addrs) for f in fs.values())
    print('strong', len(an.strong), 'weak', len(an.weak), 'functions', len(fs),
          'total insns (with dup)', total)
    print('jump tables', sum(len(f.jump_tables) for f in fs.values()),
          'dyn jumps', sum(len(f.dyn_jumps) for f in fs.values()))
    for l in an.log[:20]:
        print(l)
