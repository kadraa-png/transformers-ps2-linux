#!/usr/bin/env python3
"""EE (R5900) -> C++ static recompiler.

usage: recomp_ee.py <SLES_523.88> <outdir> [--files N] [--extra tools/extra_entries.txt]

Emits:
  <outdir>/ee_funcs.h      declarations
  <outdir>/ee_NNN.cpp      function bodies (split for parallel builds)
  <outdir>/ee_table.cpp    g_ee_func_table (sorted by address)
  <outdir>/ee_info.txt     statistics

Control-flow contract: see analyze.py.
"""
import argparse
import os
import sys
import zlib
import re

KMODE_RE = re.compile(r'\bee_(lb|lh|lw|ld|lq|sb|sh|sw|sd|sq)\(')

sys.path.insert(0, os.path.dirname(__file__))
from r5900 import disasm  # noqa: E402
import analyze  # noqa: E402

FPU_HELPER = {
    'add.s', 'sub.s', 'mul.s', 'div.s', 'sqrt.s', 'abs.s', 'mov.s', 'neg.s', 'rsqrt.s',
    'adda.s', 'suba.s', 'mula.s', 'madd.s', 'msub.s', 'madda.s', 'msuba.s', 'cvt.w.s',
    'max.s', 'min.s', 'c.f.s', 'c.eq.s', 'c.lt.s', 'c.le.s', 'cvt.s.w',
}
MMI_HELPER = {
    'madd', 'maddu', 'madd1', 'maddu1', 'mult1', 'multu1', 'div1', 'divu1', 'mfhi1',
    'mflo1', 'mthi1', 'mtlo1', 'plzcw', 'psllh', 'psrlh', 'psrah', 'psllw', 'psrlw', 'psraw',
    'paddw', 'psubw', 'pcgtw', 'pmaxw', 'paddh', 'psubh', 'pcgth', 'pmaxh', 'paddb', 'psubb',
    'pcgtb', 'paddsw', 'psubsw', 'pextlw', 'ppacw', 'paddsh', 'psubsh', 'pextlh', 'ppach',
    'paddsb', 'psubsb', 'pextlb', 'ppacb', 'pext5', 'ppac5', 'pabsw', 'pceqw', 'pminw',
    'padsbh', 'pabsh', 'pceqh', 'pminh', 'pceqb', 'padduw', 'psubuw', 'pextuw', 'padduh',
    'psubuh', 'pextuh', 'paddub', 'psubub', 'pextub', 'qfsrv', 'pmaddw', 'psllvw', 'psrlvw',
    'pmsubw', 'pmfhi', 'pmflo', 'pinth', 'pmultw', 'pdivw', 'pcpyld', 'pmaddh', 'phmadh',
    'pand', 'pxor', 'pmsubh', 'phmsbh', 'pexeh', 'prevh', 'pmulth', 'pdivbw', 'pexew',
    'prot3w', 'pmadduw', 'psravw', 'pmthi', 'pmtlo', 'pinteh', 'pmultuw', 'pdivuw', 'pcpyud',
    'por', 'pnor', 'pexch', 'pcpyh', 'pexcw', 'pmfhl.lw', 'pmfhl.uw', 'pmfhl.slw', 'pmfhl.lh',
    'pmfhl.sh', 'pmthl.lw', 'mfsa', 'mtsa', 'mtsab', 'mtsah',
}
ALU_HELPER = {'mult', 'multu', 'div', 'divu'}

LANDING_PAD_JUMP = 0x10b810   # CodeWarrior C++ EH: restore context + jr a2

# Spin-wait loops: backward branches whose body only observes state (loads,
# COP0 Count, DMA/VU condition bits) and computes in registers.  Such a loop
# can only exit through interrupts/DMA/time, so the runtime may idle the host
# while it spins (EE_SPIN_CHECKPOINT).  Anything with a guest-visible effect
# disqualifies the loop; so does a loop that observes nothing (a delay loop).
SPIN_EFFECTS = {
    'sb', 'sh', 'sw', 'swl', 'swr', 'sd', 'sdl', 'sdr', 'sq', 'swc1', 'sqc2',
    'jal', 'jalr', 'bltzal', 'bgezal', 'bltzall', 'bgezall',
    'syscall', 'break', 'eret', 'ei', 'di', 'cache', 'mtc0', 'tlbr', 'tlbwi', 'tlbwr', 'tlbp',
    'tge', 'tgeu', 'tlt', 'tltu', 'teq', 'tne', 'tgei', 'tgeiu', 'tlti', 'tltiu', 'teqi', 'tnei',
    'qmtc2', 'ctc2', 'vcallms', 'vcallmsr', 'invalid',
}
SPIN_OBSERVE = {
    'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'lwl', 'lwr', 'ld', 'ldl', 'ldr', 'lq', 'lwc1', 'lqc2',
    'bc0f', 'bc0t', 'bc0fl', 'bc0tl', 'bc2f', 'bc2t', 'bc2fl', 'bc2tl', 'cfc2', 'qmfc2',
}
SPIN_MAX_INSNS = 64
SPIN_LOADS = SPIN_OBSERVE - {'bc0f', 'bc0t', 'bc0fl', 'bc0tl', 'bc2f', 'bc2t', 'bc2fl', 'bc2tl', 'cfc2', 'qmfc2'}


def h(n):
    return '0x%08x' % n


class Gen:
    def __init__(self, an):
        self.an = an
        self.strong = an.strong
        self.funcs = an.functions
        self.unknown_ops = {}
        self.spin_loops = 0

    # ---- register expressions --------------------------------------------
    @staticmethod
    def d(r):   # 64-bit unsigned value
        return '0ull' if r == 0 else 'c->r[%d].d[0]' % r

    @staticmethod
    def sd(r):
        return '0ll' if r == 0 else 'c->r[%d].sd[0]' % r

    @staticmethod
    def w(r):   # low 32 bits unsigned
        return '0u' if r == 0 else 'c->r[%d].w[0]' % r

    @staticmethod
    def sw(r):
        return '0' if r == 0 else 'c->r[%d].sw[0]' % r

    @staticmethod
    def setd(r, expr):
        if r == 0:
            return ''
        return 'c->r[%d].d[0] = %s;' % (r, expr)

    def addr(self, i):
        if i.rs == 0:
            return '(u32)%s' % h(i.simm & 0xFFFFFFFF)
        if i.simm == 0:
            return 'c->r[%d].w[0]' % i.rs
        return '(c->r[%d].w[0] + (u32)%d)' % (i.rs, i.simm)

    # ---- non-control instructions ------------------------------------------
    def simple(self, i):
        o = i.op
        rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
        d, sd, w, sw, setd = self.d, self.sd, self.w, self.sw, self.setd
        imm, simm = i.imm, i.simm
        ram = i.rs in (28, 29)   # gp / sp relative: plain RAM
        if o in ('nop', 'sync', 'pref', 'eret'):   # eret: emitted by emit_function
            return ''
        if o == 'addiu' or o == 'addi':
            return setd(rt, 'SE32(%s + (u32)%d)' % (w(rs), simm))
        if o == 'daddiu' or o == 'daddi':
            return setd(rt, '%s + (u64)(s64)%d' % (d(rs), simm))
        if o == 'slti':
            return setd(rt, '(%s < (s64)%d) ? 1ull : 0ull' % (sd(rs), simm))
        if o == 'sltiu':
            return setd(rt, '(%s < (u64)(s64)%d) ? 1ull : 0ull' % (d(rs), simm))
        if o == 'andi':
            return setd(rt, '%s & %s' % (d(rs), h(imm)))
        if o == 'ori':
            return setd(rt, '%s | %s' % (d(rs), h(imm)))
        if o == 'xori':
            return setd(rt, '%s ^ %s' % (d(rs), h(imm)))
        if o == 'lui':
            return setd(rt, 'SE32(%s)' % h((imm << 16) & 0xFFFFFFFF))
        if o in ('addu', 'add'):
            return setd(rd, 'SE32(%s + %s)' % (w(rs), w(rt)))
        if o in ('subu', 'sub'):
            return setd(rd, 'SE32(%s - %s)' % (w(rs), w(rt)))
        if o in ('daddu', 'dadd'):
            if rt == 0:
                return setd(rd, d(rs))
            if rs == 0:
                return setd(rd, d(rt))
            return setd(rd, '%s + %s' % (d(rs), d(rt)))
        if o in ('dsubu', 'dsub'):
            return setd(rd, '%s - %s' % (d(rs), d(rt)))
        if o == 'and':
            return setd(rd, '%s & %s' % (d(rs), d(rt)))
        if o == 'or':
            if rt == 0:
                return setd(rd, d(rs))
            if rs == 0:
                return setd(rd, d(rt))
            return setd(rd, '%s | %s' % (d(rs), d(rt)))
        if o == 'xor':
            return setd(rd, '%s ^ %s' % (d(rs), d(rt)))
        if o == 'nor':
            return setd(rd, '~(%s | %s)' % (d(rs), d(rt)))
        if o == 'slt':
            return setd(rd, '(%s < %s) ? 1ull : 0ull' % (sd(rs), sd(rt)))
        if o == 'sltu':
            return setd(rd, '(%s < %s) ? 1ull : 0ull' % (d(rs), d(rt)))
        if o == 'sll':
            return setd(rd, 'SE32(%s << %d)' % (w(rt), sa))
        if o == 'srl':
            return setd(rd, 'SE32(%s >> %d)' % (w(rt), sa))
        if o == 'sra':
            return setd(rd, 'SE32(%s >> %d)' % (sw(rt), sa))
        if o == 'sllv':
            return setd(rd, 'SE32(%s << (%s & 31))' % (w(rt), w(rs)))
        if o == 'srlv':
            return setd(rd, 'SE32(%s >> (%s & 31))' % (w(rt), w(rs)))
        if o == 'srav':
            return setd(rd, 'SE32(%s >> (%s & 31))' % (sw(rt), w(rs)))
        if o == 'dsll':
            return setd(rd, '%s << %d' % (d(rt), sa))
        if o == 'dsrl':
            return setd(rd, '%s >> %d' % (d(rt), sa))
        if o == 'dsra':
            return setd(rd, '(u64)(%s >> %d)' % (sd(rt), sa))
        if o == 'dsll32':
            return setd(rd, '%s << %d' % (d(rt), sa + 32))
        if o == 'dsrl32':
            return setd(rd, '%s >> %d' % (d(rt), sa + 32))
        if o == 'dsra32':
            return setd(rd, '(u64)(%s >> %d)' % (sd(rt), sa + 32))
        if o == 'dsllv':
            return setd(rd, '%s << (%s & 63)' % (d(rt), w(rs)))
        if o == 'dsrlv':
            return setd(rd, '%s >> (%s & 63)' % (d(rt), w(rs)))
        if o == 'dsrav':
            return setd(rd, '(u64)(%s >> (%s & 63))' % (sd(rt), w(rs)))
        if o == 'movz':
            if rd == 0:
                return ''
            return 'if (%s == 0) c->r[%d].d[0] = %s;' % (d(rt), rd, d(rs))
        if o == 'movn':
            if rd == 0:
                return ''
            return 'if (%s != 0) c->r[%d].d[0] = %s;' % (d(rt), rd, d(rs))
        if o == 'mfhi':
            return setd(rd, 'c->hi.d[0]')
        if o == 'mflo':
            return setd(rd, 'c->lo.d[0]')
        if o == 'mthi':
            return 'c->hi.d[0] = %s;' % d(rs)
        if o == 'mtlo':
            return 'c->lo.d[0] = %s;' % d(rs)
        if o in ALU_HELPER:
            return 'alu_%s(c, %s);' % (o, h(i.word))
        if o in MMI_HELPER:
            return 'mmi_%s(c, %s);' % (o.replace('.', '_'), h(i.word))
        if o in FPU_HELPER:
            return 'fpu_%s(c, %s);' % (o.replace('.', '_'), h(i.word))
        # ---- loads ----
        if o == 'lb':
            return self._load(rt, 'SE32((s32)(s8)ee_lb(%s))' % self.addr(i), i)
        if o == 'lbu':
            return self._load(rt, '(u64)ee_lb(%s)' % self.addr(i), i)
        if o == 'lh':
            return self._load(rt, 'SE32((s32)(s16)ee_lh(%s))' % self.addr(i), i)
        if o == 'lhu':
            return self._load(rt, '(u64)ee_lh(%s)' % self.addr(i), i)
        if o == 'lw':
            return self._load(rt, 'SE32(%s(%s))' % ('ee_lw_ram' if ram else 'ee_lw', self.addr(i)), i)
        if o == 'lwu':
            return self._load(rt, '(u64)ee_lw(%s)' % self.addr(i), i)
        if o == 'ld':
            return self._load(rt, '%s(%s)' % ('ee_ld_ram' if ram else 'ee_ld', self.addr(i)), i)
        if o == 'lq':
            fn = 'ee_lq_ram' if ram else 'ee_lq'
            if rt == 0:
                return '{ qword t; %s(%s, &t); }' % (fn, self.addr(i))
            return '%s(%s, &c->r[%d]);' % (fn, self.addr(i), rt)
        if o in ('lwl', 'lwr', 'ldl', 'ldr', 'swl', 'swr', 'sdl', 'sdr'):
            return 'ee_%s(c, %d, %s);' % (o, rt, self.addr(i))
        if o == 'lwc1':
            return 'c->fpr[%d].u = %s(%s);' % (i.ft, 'ee_lw_ram' if ram else 'ee_lw', self.addr(i))
        if o == 'lqc2':
            if i.ft == 0:
                return '{ qword t; ee_lq(%s, &t); }' % self.addr(i)
            return 'ee_lq(%s, &g_vu0.vf[%d]);' % (self.addr(i), i.ft)
        # ---- stores ----
        if o == 'sb':
            return 'ee_sb(%s, (u8)%s);' % (self.addr(i), w(rt))
        if o == 'sh':
            return 'ee_sh(%s, (u16)%s);' % (self.addr(i), w(rt))
        if o == 'sw':
            return '%s(%s, %s);' % ('ee_sw_ram' if ram else 'ee_sw', self.addr(i), w(rt))
        if o == 'sd':
            return '%s(%s, %s);' % ('ee_sd_ram' if ram else 'ee_sd', self.addr(i), d(rt))
        if o == 'sq':
            fn = 'ee_sq_ram' if ram else 'ee_sq'
            if rt == 0:
                return '{ qword t; t.q = 0; %s(%s, t); }' % (fn, self.addr(i))
            return '%s(%s, c->r[%d]);' % (fn, self.addr(i), rt)
        if o == 'swc1':
            return '%s(%s, c->fpr[%d].u);' % ('ee_sw_ram' if ram else 'ee_sw', self.addr(i), i.ft)
        if o == 'sqc2':
            return 'ee_sq(%s, g_vu0.vf[%d]);' % (self.addr(i), i.ft)
        # ---- COP1 moves ----
        if o == 'mfc1':
            return setd(rt, 'SE32(c->fpr[%d].u)' % i.fs)
        if o == 'mtc1':
            return 'c->fpr[%d].u = %s;' % (i.fs, w(rt))
        if o == 'cfc1':
            if i.fs == 31:
                return setd(rt, 'SE32(c->fcr31)')
            if i.fs == 0:
                return setd(rt, '0x2e00ull')
            return setd(rt, '0ull')
        if o == 'ctc1':
            if i.fs == 31:
                return 'c->fcr31 = %s;' % w(rt)
            return ''
        # ---- COP2 moves ----
        if o == 'qmfc2':
            if rt == 0:
                return ''
            return 'c->r[%d] = g_vu0.vf[%d];' % (rt, rd)
        if o == 'qmtc2':
            if rd == 0:
                return ''
            if rt == 0:
                return 'g_vu0.vf[%d].q = 0;' % rd
            return 'g_vu0.vf[%d] = c->r[%d];' % (rd, rt)
        if o == 'cfc2':
            if rt == 0:
                return 'vu0_cfc2(%d);' % rd
            return setd(rt, 'SE32(vu0_cfc2(%d))' % rd)
        if o == 'ctc2':
            return 'vu0_ctc2(%d, %s);' % (rd, w(rt))
        if o.startswith('v'):
            return 'vu0m_%s(%s);' % (o, h(i.word))
        # ---- COP0 / system ----
        if o == 'mfc0':
            return setd(rt, 'SE32(ee_mfc0(c, %d))' % rd) if rt else 'ee_mfc0(c, %d);' % rd
        if o == 'mtc0':
            return 'ee_mtc0(c, %d, %s);' % (rd, w(rt))
        if o == 'ei':
            return 'ee_ei(c);'
        if o == 'di':
            return 'ee_di(c);'
        if o == 'cache':
            return 'ee_cache(c, %d, %s);' % (rt, self.addr(i))
        if o in ('tlbr', 'tlbwi', 'tlbwr', 'tlbp'):
            return 'ee_unhandled_insn(c, %s, %s);' % (h(i.pc), h(i.word))
        if o == 'syscall':
            return 'c->pc = %s; ee_syscall(c, %s, %s);' % (h(i.pc), h(i.pc), h(i.code))
        if o == 'break':
            return 'ee_break(c, %s, %s);' % (h(i.pc), h(i.code))
        if o in ('tge', 'tgeu', 'tlt', 'tltu', 'teq', 'tne'):
            a, b = (sd(rs), sd(rt)) if o in ('tge', 'tlt') else (d(rs), d(rt))
            op = {'tge': '>=', 'tgeu': '>=', 'tlt': '<', 'tltu': '<', 'teq': '==', 'tne': '!='}[o]
            return 'if (%s %s %s) ee_trap(c, %s);' % (a, op, b, h(i.pc))
        if o in ('tgei', 'tgeiu', 'tlti', 'tltiu', 'teqi', 'tnei'):
            op = {'tgei': '>=', 'tgeiu': '>=', 'tlti': '<', 'tltiu': '<', 'teqi': '==', 'tnei': '!='}[o]
            if o in ('tgei', 'tlti', 'teqi', 'tnei'):
                return 'if (%s %s (s64)%d) ee_trap(c, %s);' % (sd(rs), op, simm, h(i.pc))
            return 'if (%s %s (u64)(s64)%d) ee_trap(c, %s);' % (d(rs), op, simm, h(i.pc))
        self.unknown_ops[o] = self.unknown_ops.get(o, 0) + 1
        return 'ee_unhandled_insn(c, %s, %s);' % (h(i.pc), h(i.word))

    def _load(self, rt, expr, i):
        if rt == 0:
            return '(void)%s;' % expr if not (i.rs in (28, 29)) else ''
        return 'c->r[%d].d[0] = %s;' % (rt, expr)

    # ---- branch conditions ---------------------------------------------------
    def cond(self, i):
        o = i.op
        d, sd = self.d, self.sd
        if o in ('beq', 'beql'):
            return '%s == %s' % (d(i.rs), d(i.rt))
        if o in ('bne', 'bnel'):
            return '%s != %s' % (d(i.rs), d(i.rt))
        if o in ('blez', 'blezl'):
            return '%s <= 0' % sd(i.rs)
        if o in ('bgtz', 'bgtzl'):
            return '%s > 0' % sd(i.rs)
        if o in ('bltz', 'bltzl'):
            return '%s < 0' % sd(i.rs)
        if o in ('bgez', 'bgezl'):
            return '%s >= 0' % sd(i.rs)
        if o in ('bc1f', 'bc1fl'):
            return '!(c->fcr31 & 0x800000)'
        if o in ('bc1t', 'bc1tl'):
            return '(c->fcr31 & 0x800000)'
        if o in ('bc0f', 'bc0fl'):
            return '!ee_cpcond0()'
        if o in ('bc0t', 'bc0tl'):
            return 'ee_cpcond0()'
        if o in ('bc2f', 'bc2fl'):
            return '!vu_cpcond2()'
        if o in ('bc2t', 'bc2tl'):
            return 'vu_cpcond2()'
        raise ValueError('no condition for ' + o)

    # ---- function emission ---------------------------------------------------
    def goto(self, f, target, from_pc, labels_needed):
        """Transfer control to `target` from within function f."""
        if target in f.addrs:
            labels_needed.add(target)
            pre = ''
            if target <= from_pc:
                spin = self.is_spin_loop(f, target, from_pc)
                self.spin_loops += spin
                pre = '%s(c, %s); ' % ('EE_SPIN_CHECKPOINT' if spin else 'EE_CHECKPOINT', h(target))
            return '%sgoto L_%08x;' % (pre, target)
        if target in self.funcs:
            return 'f_%08x(c); return;' % target
        return 'ee_unknown_target(c, %s, %s);' % (h(target), h(from_pc))

    def is_spin_loop(self, f, target, from_pc):
        """True if [target, from_pc + delay slot] is a side-effect-free polling loop."""
        end = from_pc + 8
        if (end - target) // 4 > SPIN_MAX_INSNS:
            return False
        observes = False
        written, bases = set(), set()
        for a in range(target, end, 4):
            if a not in f.addrs:
                return False
            i = self.an.insn(a)
            if i.op in SPIN_EFFECTS:
                return False
            if i.op in SPIN_OBSERVE or (i.op == 'mfc0' and i.rd == 9):
                observes = True
                if i.op in SPIN_LOADS:
                    bases.add(i.rs)
            w = self.an._writes_gpr(i)
            if w:
                written.add(w)
        # A polling loop re-reads fixed locations; one that advances its load
        # addresses (list walk, array loop) is computing and must never idle.
        return observes and not (bases & written)

    def call(self, target, pc):
        if target in self.funcs:
            s = 'f_%08x(c);' % target
        else:
            s = 'ee_call(c, %s);' % h(target)
        s += ' EE_RET_CHECK(c, %s, %s);' % (h(pc), h(pc + 8))
        if pc in CAMERA_TAN_CALLS:
            s += ' c->fpr[0].f *= g_ee_camera_fov_scale;'
        return s

    def emit_function(self, f):
        an = self.an
        addrs = sorted(f.addrs)
        aset = f.addrs
        labels_needed = set()
        out = []
        # branch targets that need labels are added while emitting; first pass
        # emits into `out` with label placeholders, second pass filters.
        k = 0
        n = len(addrs)
        while k < n:
            pc = addrs[k]
            i = an.insn(pc)
            out.append(('label', pc))
            if pc in CAMERA_HOOKS:
                out.append(('code', CAMERA_HOOKS[pc]))
            if i.has_delay:
                if pc + 4 not in aset:
                    raise RuntimeError('delay slot missing %08x' % pc)
                ds = an.insn(pc + 4)
                if ds.has_delay:
                    out.append(('code', 'ee_unhandled_insn(c, %s, %s); /* branch in delay slot */'
                                % (h(pc), h(i.word))))
                    k += 1
                    continue
                dcode = self.simple(ds)
                falls, code = self.control(f, i, dcode, labels_needed)
                out.append(('code', '/* %08x: %s ; %s */' % (pc, disasm(i), disasm(ds))))
                out.append(('code', code))
                nxt = pc + 8
                # delay slot is also a branch target (or the function entry): emit it standalone
                if pc + 4 in f.branch_targets or pc + 4 == f.entry:
                    if falls:
                        out.append(('code', self.goto(f, nxt, pc, labels_needed)))
                    out.append(('label_force', pc + 4))
                    out.append(('code', dcode))
                    falls = True
                k += 2 if (k + 1 < n and addrs[k + 1] == pc + 4) else 1
            else:
                out.append(('code', self.simple(i) + ' /* %08x: %s */' % (pc, disasm(i))))
                nxt = pc + 4
                falls = i.op not in ('eret',)
                if i.op == 'eret':
                    out.append(('code', 'ee_eret(c, %s); return;' % h(pc)))
                k += 1
            if falls:
                nxt_emitted = addrs[k] if k < n else None
                if nxt_emitted != nxt:
                    if nxt in aset:
                        labels_needed.add(nxt)
                        out.append(('code', 'goto L_%08x;' % nxt))
                    elif nxt in self.funcs:
                        out.append(('code', 'f_%08x(c); return;' % nxt))
                    else:
                        out.append(('code', 'ee_unknown_target(c, %s, %s);' % (h(nxt), h(pc))))
        labels_needed.add(f.entry)
        # Kernel-mode helpers (contain ERET) may reach hardware via kseg0/kseg1.
        kmode = any(an.insn(a).op == 'eret' for a in aset)
        if kmode:
            out = [(k, KMODE_RE.sub(r'ee_\1_k(', v) if k == 'code' else v) for k, v in out]
        lines = ['void f_%08x(EEContext* __restrict c) {' % f.entry,
                 'EE_TRACE(c, 0x%08x);' % f.entry]
        if addrs[0] != f.entry:
            lines.append('goto L_%08x;' % f.entry)
        for kind, v in out:
            if kind == 'label':
                if v in labels_needed:
                    lines.append('L_%08x:;' % v)
            elif kind == 'label_force':
                lines.append('L_%08x:;' % v)
            else:
                if v:
                    lines.append(v)
        lines.append('}')
        return '\n'.join(lines)

    def control(self, f, i, dcode, labels_needed):
        """Return (falls_through, code) for a branch/jump with delay slot code."""
        o = i.op
        pc = i.pc
        if i.is_branch:
            cond = self.cond(i)
            tgt = i.target
            go = self.goto(f, tgt, pc, labels_needed)
            always = (o in ('beq', 'beql') and i.rs == 0 and i.rt == 0) or \
                     (o in ('bgez', 'bgezl') and i.rs == 0)
            never = (o in ('bne', 'bnel') and i.rs == 0 and i.rt == 0)
            if never:
                if i.is_likely:
                    return True, ''
                return True, '{ %s }' % dcode
            if always:
                return False, '{ %s %s }' % (dcode, go)
            if i.is_likely:
                return True, 'if (%s) { %s %s }' % (cond, dcode, go)
            return True, '{ const bool br = %s; %s if (br) { %s } }' % (cond, dcode, go)
        if o == 'j':
            tgt = i.target
            if pc in f.tail_calls:
                return False, '{ %s f_%08x(c); return; }' % (dcode, tgt)
            return False, '{ %s %s }' % (dcode, self.goto(f, tgt, pc, labels_needed))
        if o == 'jal':
            return True, 'c->r[31].d[0] = %s; %s %s' % (h(pc + 8), dcode, self.call(i.target, pc))
        if o == 'jalr':
            link = 'c->r[%d].d[0] = %s;' % (i.rd, h(pc + 8)) if i.rd else ''
            ck = ' EE_RET_CHECK(c, %s, %s);' % (h(pc), h(pc + 8)) if i.rd == 31 else ''
            return True, '{ const u32 t = %s; %s %s ee_call(c, t);%s }' % (
                self.w(i.rs), link, dcode, ck)
        if o == 'jr':
            if i.rs == 31:
                return False, '{ %s return; }' % dcode
            if f.entry == LANDING_PAD_JUMP or pc in (LANDING_PAD_JUMP + 0x5c,):
                return False, '{ ee_unhandled_insn(c, %s, %s); /* C++ EH landing pad */ }' % (
                    h(pc), h(i.word))
            if pc in f.jump_tables:
                cases = []
                for t in sorted(set(f.jump_tables[pc])):
                    labels_needed.add(t)
                    cases.append('case %s: goto L_%08x;' % (h(t), t))
                return False, '{ const u32 t = %s; %s switch (t) { %s default: ee_unknown_target(c, t, %s); } }' % (
                    self.w(i.rs), dcode, ' '.join(cases), h(pc))
            return False, '{ const u32 t = %s; %s ee_call(c, t); return; }' % (self.w(i.rs), dcode)
        raise ValueError('control: ' + o)

# The engine camera (vlCamera: constructor, SetFov, field-of-view refresh)
# stores tan(fov / 2), computed by these calls to tanf.  The game's own 16:9
# mode keeps the horizontal view and crops the top and bottom; scaling the
# tangent by 4/3 (--widescreen) shows the 4:3 vertical view plus more on the
# sides instead.
CAMERA_TAN_CALLS = {0x001f27f8, 0x001f2890, 0x001f2a28}

# Mouse look.  f_0046d410 is the player camera controller: it turns the
# camera object (a1 -> s1) by right-stick input times a capped turn rate, yaw
# at +284 (wrapped to [-pi, pi)) and pitch at +280 (clamped).  These calls run
# before the instruction at that address:
#   0046d488  both invert-Y branches joined: $f3 = +-1 (game option), s1 set
#   0046d7c0  every path joined, just before the camera matrix update
CAMERA_HOOKS = {
    0x0046d488: 'ee_camera_hook_begin(c);',
    0x0046d7c0: 'ee_camera_hook_end(c);',
}


def write_if_changed(path, data):
    old = open(path).read() if os.path.exists(path) else None
    if old != data:
        with open(path, 'w') as fh:
            fh.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('outdir')
    ap.add_argument('--files', type=int, default=160)
    ap.add_argument('--extra', default=os.path.join(os.path.dirname(__file__), 'extra_entries.txt'))
    args = ap.parse_args()

    extra = []
    if os.path.exists(args.extra):
        for line in open(args.extra):
            line = line.split('#')[0].strip()
            if line:
                extra.append(int(line, 16))

    img = analyze.Image(args.elf)
    an = analyze.Analyzer(img, [(0x100000, 0x5dcb60), (0x671980, 0x6adf20)],
                          [(0x5dcb60, 0x671980), (0x6adf20, img.end)], extra).run()
    gen = Gen(an)
    os.makedirs(args.outdir, exist_ok=True)
    entries = sorted(an.functions)

    write_if_changed(os.path.join(args.outdir, 'ee_funcs.h'),
                     '#pragma once\n#include "ps2/ee_gen.h"\n' +
                     ''.join('void f_%08x(EEContext* __restrict c);\n' % e for e in entries))

    # split by cumulative size
    sizes = [len(an.functions[e].addrs) for e in entries]
    total = sum(sizes)
    per = max(1, total // args.files + 1)
    chunks = []
    cur, acc = [], 0
    for e, s in zip(entries, sizes):
        cur.append(e)
        acc += s
        if acc >= per:
            chunks.append(cur)
            cur, acc = [], 0
    if cur:
        chunks.append(cur)

    written = set()
    for idx, chunk in enumerate(chunks):
        path = os.path.join(args.outdir, 'ee_%03d.cpp' % idx)
        body = ['// generated by tools/recomp_ee.py - do not edit',
                '#include "ee_funcs.h"',
                '#pragma GCC diagnostic ignored "-Wunused-label"', '']
        for e in chunk:
            body.append(gen.emit_function(an.functions[e]))
            body.append('')
        data = '\n'.join(body)
        write_if_changed(path, data)   # keeps incremental builds fast
        written.add(os.path.basename(path))
    for name in os.listdir(args.outdir):
        if name.startswith('ee_') and name.endswith('.cpp') and name not in written \
                and name != 'ee_table.cpp':
            os.remove(os.path.join(args.outdir, name))

    write_if_changed(os.path.join(args.outdir, 'ee_table.cpp'),
                     '#include "ee_funcs.h"\n' +
                     'extern const u32 g_ee_elf_crc32 = 0x%08x;  // CRC32 of the recompiled executable\n'
                     % (zlib.crc32(img.raw) & 0xFFFFFFFF) +
                     'const EEFuncEntry g_ee_func_table[] = {\n' +
                     ''.join('  {0x%08x, f_%08x},\n' % (e, e) for e in entries) +
                     '};\nconst u32 g_ee_func_count = %d;\n' % len(entries))

    with open(os.path.join(args.outdir, 'ee_info.txt'), 'w') as fh:
        fh.write('functions %d (strong %d weak %d)\n' % (len(entries), len(an.strong), len(an.weak)))
        fh.write('instructions (with duplication) %d\n' % total)
        fh.write('files %d\n' % len(chunks))
        fh.write('unknown ops %r\n' % gen.unknown_ops)
        fh.write('spin-wait back-edges %d\n' % gen.spin_loops)
        for l in an.log:
            fh.write(l + '\n')
    print(open(os.path.join(args.outdir, 'ee_info.txt')).read())


if __name__ == '__main__':
    main()
