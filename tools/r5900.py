"""R5900 (PS2 Emotion Engine) instruction decoder.

decode(word, pc) -> Insn.  Insn.op is a lowercase mnemonic; fields are raw
register numbers / immediates.  Used by the analyzer and the C++ code
generator, so the field meanings are part of the recompiler contract:

  rs, rt, rd, sa   GPR / shift fields (also fs/ft/fd for COP1 via fs/ft/fd)
  imm              zero-extended 16-bit immediate
  simm             sign-extended 16-bit immediate
  target           absolute branch/jump target (None when not static)
  dest             VU dest mask (bit3=x, bit2=y, bit1=z, bit0=w)
  bc               VU broadcast field (0=x 1=y 2=z 3=w)
  fsf, ftf         VU field selectors for DIV/SQRT/RSQRT/MTIR/RINIT/RXOR
"""

REG_NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
             't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
             's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
             't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


class Insn:
    __slots__ = ('pc', 'word', 'op', 'rs', 'rt', 'rd', 'sa', 'imm', 'simm',
                 'target', 'fs', 'ft', 'fd', 'dest', 'bc', 'fsf', 'ftf',
                 'imm5', 'imm11', 'imm15', 'code', 'interlock')

    def __init__(self, pc, word, op):
        self.pc = pc
        self.word = word
        self.op = op
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xFFFF
        self.simm = sext16(self.imm)
        self.target = None
        # COP1 / COP2 views of the same bits
        self.ft = self.rt
        self.fs = self.rd
        self.fd = self.sa
        self.dest = (word >> 21) & 15
        self.bc = word & 3
        self.fsf = (word >> 21) & 3
        self.ftf = (word >> 23) & 3
        v = (word >> 6) & 31
        self.imm5 = v - 32 if v & 16 else v
        self.imm11 = word & 0x7FF
        self.imm15 = (word >> 6) & 0x7FFF
        self.code = (word >> 6) & 0xFFFFF
        self.interlock = word & 1

    # ---- control-flow classification ------------------------------------
    @property
    def is_branch(self):
        """Conditional PC-relative branch (has delay slot)."""
        return self.op in BRANCHES

    @property
    def is_likely(self):
        return self.op in LIKELY

    @property
    def is_jump(self):
        return self.op in ('j', 'jal', 'jr', 'jalr')

    @property
    def has_delay(self):
        return self.is_branch or self.is_jump

    @property
    def is_call(self):
        return self.op in ('jal', 'jalr', 'bltzal', 'bgezal', 'bltzall', 'bgezall')

    def __repr__(self):
        return '%08x: %08x %s' % (self.pc, self.word, disasm(self))


BRANCHES = {'beq', 'bne', 'blez', 'bgtz', 'beql', 'bnel', 'blezl', 'bgtzl',
            'bltz', 'bgez', 'bltzl', 'bgezl', 'bltzal', 'bgezal', 'bltzall',
            'bgezall', 'bc0f', 'bc0t', 'bc0fl', 'bc0tl', 'bc1f', 'bc1t',
            'bc1fl', 'bc1tl', 'bc2f', 'bc2t', 'bc2fl', 'bc2tl'}
LIKELY = {'beql', 'bnel', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'bltzall',
          'bgezall', 'bc0fl', 'bc0tl', 'bc1fl', 'bc1tl', 'bc2fl', 'bc2tl'}

PRIMARY = {
    0x02: 'j', 0x03: 'jal', 0x04: 'beq', 0x05: 'bne', 0x06: 'blez', 0x07: 'bgtz',
    0x08: 'addi', 0x09: 'addiu', 0x0A: 'slti', 0x0B: 'sltiu', 0x0C: 'andi',
    0x0D: 'ori', 0x0E: 'xori', 0x0F: 'lui',
    0x14: 'beql', 0x15: 'bnel', 0x16: 'blezl', 0x17: 'bgtzl',
    0x18: 'daddi', 0x19: 'daddiu', 0x1A: 'ldl', 0x1B: 'ldr', 0x1E: 'lq', 0x1F: 'sq',
    0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu',
    0x26: 'lwr', 0x27: 'lwu', 0x28: 'sb', 0x29: 'sh', 0x2A: 'swl', 0x2B: 'sw',
    0x2C: 'sdl', 0x2D: 'sdr', 0x2E: 'swr', 0x2F: 'cache',
    0x31: 'lwc1', 0x33: 'pref', 0x36: 'lqc2', 0x37: 'ld',
    0x39: 'swc1', 0x3E: 'sqc2', 0x3F: 'sd',
}

SPECIAL = {
    0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav',
    0x08: 'jr', 0x09: 'jalr', 0x0A: 'movz', 0x0B: 'movn', 0x0C: 'syscall',
    0x0D: 'break', 0x0F: 'sync',
    0x10: 'mfhi', 0x11: 'mthi', 0x12: 'mflo', 0x13: 'mtlo',
    0x14: 'dsllv', 0x16: 'dsrlv', 0x17: 'dsrav',
    0x18: 'mult', 0x19: 'multu', 0x1A: 'div', 0x1B: 'divu',
    0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu', 0x24: 'and',
    0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x28: 'mfsa', 0x29: 'mtsa',
    0x2A: 'slt', 0x2B: 'sltu', 0x2C: 'dadd', 0x2D: 'daddu', 0x2E: 'dsub', 0x2F: 'dsubu',
    0x30: 'tge', 0x31: 'tgeu', 0x32: 'tlt', 0x33: 'tltu', 0x34: 'teq', 0x36: 'tne',
    0x38: 'dsll', 0x3A: 'dsrl', 0x3B: 'dsra', 0x3C: 'dsll32', 0x3E: 'dsrl32', 0x3F: 'dsra32',
}

REGIMM = {
    0x00: 'bltz', 0x01: 'bgez', 0x02: 'bltzl', 0x03: 'bgezl',
    0x08: 'tgei', 0x09: 'tgeiu', 0x0A: 'tlti', 0x0B: 'tltiu', 0x0C: 'teqi', 0x0E: 'tnei',
    0x10: 'bltzal', 0x11: 'bgezal', 0x12: 'bltzall', 0x13: 'bgezall',
    0x18: 'mtsab', 0x19: 'mtsah',
}

MMI = {
    0x00: 'madd', 0x01: 'maddu', 0x04: 'plzcw',
    0x10: 'mfhi1', 0x11: 'mthi1', 0x12: 'mflo1', 0x13: 'mtlo1',
    0x18: 'mult1', 0x19: 'multu1', 0x1A: 'div1', 0x1B: 'divu1',
    0x20: 'madd1', 0x21: 'maddu1',
    0x34: 'psllh', 0x36: 'psrlh', 0x37: 'psrah', 0x3C: 'psllw', 0x3E: 'psrlw', 0x3F: 'psraw',
}

MMI0 = {
    0x00: 'paddw', 0x01: 'psubw', 0x02: 'pcgtw', 0x03: 'pmaxw',
    0x04: 'paddh', 0x05: 'psubh', 0x06: 'pcgth', 0x07: 'pmaxh',
    0x08: 'paddb', 0x09: 'psubb', 0x0A: 'pcgtb',
    0x10: 'paddsw', 0x11: 'psubsw', 0x12: 'pextlw', 0x13: 'ppacw',
    0x14: 'paddsh', 0x15: 'psubsh', 0x16: 'pextlh', 0x17: 'ppach',
    0x18: 'paddsb', 0x19: 'psubsb', 0x1A: 'pextlb', 0x1B: 'ppacb',
    0x1E: 'pext5', 0x1F: 'ppac5',
}
MMI1 = {
    0x01: 'pabsw', 0x02: 'pceqw', 0x03: 'pminw', 0x04: 'padsbh', 0x05: 'pabsh',
    0x06: 'pceqh', 0x07: 'pminh', 0x0A: 'pceqb',
    0x10: 'padduw', 0x11: 'psubuw', 0x12: 'pextuw',
    0x14: 'padduh', 0x15: 'psubuh', 0x16: 'pextuh',
    0x18: 'paddub', 0x19: 'psubub', 0x1A: 'pextub', 0x1B: 'qfsrv',
}
MMI2 = {
    0x00: 'pmaddw', 0x02: 'psllvw', 0x03: 'psrlvw', 0x04: 'pmsubw',
    0x08: 'pmfhi', 0x09: 'pmflo', 0x0A: 'pinth', 0x0C: 'pmultw', 0x0D: 'pdivw',
    0x0E: 'pcpyld', 0x10: 'pmaddh', 0x11: 'phmadh', 0x12: 'pand', 0x13: 'pxor',
    0x14: 'pmsubh', 0x15: 'phmsbh', 0x1A: 'pexeh', 0x1B: 'prevh', 0x1C: 'pmulth',
    0x1D: 'pdivbw', 0x1E: 'pexew', 0x1F: 'prot3w',
}
MMI3 = {
    0x00: 'pmadduw', 0x03: 'psravw', 0x08: 'pmthi', 0x09: 'pmtlo', 0x0A: 'pinteh',
    0x0C: 'pmultuw', 0x0D: 'pdivuw', 0x0E: 'pcpyud', 0x12: 'por', 0x13: 'pnor',
    0x1A: 'pexch', 0x1B: 'pcpyh', 0x1E: 'pexcw',
}
PMFHL = {0: 'pmfhl.lw', 1: 'pmfhl.uw', 2: 'pmfhl.slw', 3: 'pmfhl.lh', 4: 'pmfhl.sh'}

COP1_S = {
    0x00: 'add.s', 0x01: 'sub.s', 0x02: 'mul.s', 0x03: 'div.s', 0x04: 'sqrt.s',
    0x05: 'abs.s', 0x06: 'mov.s', 0x07: 'neg.s', 0x16: 'rsqrt.s',
    0x18: 'adda.s', 0x19: 'suba.s', 0x1A: 'mula.s', 0x1C: 'madd.s', 0x1D: 'msub.s',
    0x1E: 'madda.s', 0x1F: 'msuba.s', 0x24: 'cvt.w.s', 0x28: 'max.s', 0x29: 'min.s',
    0x30: 'c.f.s', 0x32: 'c.eq.s', 0x34: 'c.lt.s', 0x36: 'c.le.s',
}

# VU0 macro mode, COP2 CO, funct < 0x3C
VU_SPECIAL1 = {
    0x1C: 'vmulq', 0x1D: 'vmaxi', 0x1E: 'vmuli', 0x1F: 'vminii',
    0x20: 'vaddq', 0x21: 'vmaddq', 0x22: 'vaddi', 0x23: 'vmaddi',
    0x24: 'vsubq', 0x25: 'vmsubq', 0x26: 'vsubi', 0x27: 'vmsubi',
    0x28: 'vadd', 0x29: 'vmadd', 0x2A: 'vmul', 0x2B: 'vmax', 0x2C: 'vsub',
    0x2D: 'vmsub', 0x2E: 'vopmsub', 0x2F: 'vmini',
    0x30: 'viadd', 0x31: 'visub', 0x32: 'viaddi', 0x34: 'viand', 0x35: 'vior',
    0x38: 'vcallms', 0x39: 'vcallmsr',
}
VU_SPECIAL1_BC = {0x00: 'vaddbc', 0x04: 'vsubbc', 0x08: 'vmaddbc', 0x0C: 'vmsubbc',
                  0x10: 'vmaxbc', 0x14: 'vminibc', 0x18: 'vmulbc'}
# funct 0x3C..0x3F: index = (sa << 2) | (funct & 3)
VU_SPECIAL2 = {
    0x10: 'vitof0', 0x11: 'vitof4', 0x12: 'vitof12', 0x13: 'vitof15',
    0x14: 'vftoi0', 0x15: 'vftoi4', 0x16: 'vftoi12', 0x17: 'vftoi15',
    0x1C: 'vmulaq', 0x1D: 'vabs', 0x1E: 'vmulai', 0x1F: 'vclipw',
    0x20: 'vaddaq', 0x21: 'vmaddaq', 0x22: 'vaddai', 0x23: 'vmaddai',
    0x24: 'vsubaq', 0x25: 'vmsubaq', 0x26: 'vsubai', 0x27: 'vmsubai',
    0x28: 'vadda', 0x29: 'vmadda', 0x2A: 'vmula', 0x2C: 'vsuba', 0x2D: 'vmsuba',
    0x2E: 'vopmula', 0x2F: 'vnop',
    0x30: 'vmove', 0x31: 'vmr32', 0x34: 'vlqi', 0x35: 'vsqi', 0x36: 'vlqd', 0x37: 'vsqd',
    0x38: 'vdiv', 0x39: 'vsqrt', 0x3A: 'vrsqrt', 0x3B: 'vwaitq', 0x3C: 'vmtir',
    0x3D: 'vmfir', 0x3E: 'vilwr', 0x3F: 'viswr',
    0x40: 'vrnext', 0x41: 'vrget', 0x42: 'vrinit', 0x43: 'vrxor',
}
VU_SPECIAL2_BC = {0x00: 'vaddabc', 0x04: 'vsubabc', 0x08: 'vmaddabc', 0x0C: 'vmsubabc',
                  0x18: 'vmulabc'}


def decode(word, pc):
    op = word >> 26
    name = None
    if op == 0x00:
        fn = word & 63
        name = SPECIAL.get(fn)
        if name == 'sll' and word == 0:
            name = 'nop'
    elif op == 0x01:
        name = REGIMM.get((word >> 16) & 31)
    elif op == 0x1C:
        fn = word & 63
        sa = (word >> 6) & 31
        if fn == 0x08:
            name = MMI0.get(sa)
        elif fn == 0x28:
            name = MMI1.get(sa)
        elif fn == 0x09:
            name = MMI2.get(sa)
        elif fn == 0x29:
            name = MMI3.get(sa)
        elif fn == 0x30:
            name = PMFHL.get(sa)
        elif fn == 0x31:
            name = 'pmthl.lw' if sa == 0 else None
        else:
            name = MMI.get(fn)
    elif op == 0x10:
        rs = (word >> 21) & 31
        if rs == 0x00:
            name = 'mfc0'
        elif rs == 0x04:
            name = 'mtc0'
        elif rs == 0x08:
            name = {0: 'bc0f', 1: 'bc0t', 2: 'bc0fl', 3: 'bc0tl'}.get((word >> 16) & 31)
        elif rs == 0x10:
            name = {0x01: 'tlbr', 0x02: 'tlbwi', 0x06: 'tlbwr', 0x08: 'tlbp',
                    0x18: 'eret', 0x38: 'ei', 0x39: 'di'}.get(word & 63)
    elif op == 0x11:
        rs = (word >> 21) & 31
        if rs == 0x00:
            name = 'mfc1'
        elif rs == 0x02:
            name = 'cfc1'
        elif rs == 0x04:
            name = 'mtc1'
        elif rs == 0x06:
            name = 'ctc1'
        elif rs == 0x08:
            name = {0: 'bc1f', 1: 'bc1t', 2: 'bc1fl', 3: 'bc1tl'}.get((word >> 16) & 31)
        elif rs == 0x10:
            name = COP1_S.get(word & 63)
        elif rs == 0x14:
            name = 'cvt.s.w' if (word & 63) == 0x20 else None
    elif op == 0x12:
        rs = (word >> 21) & 31
        if rs & 0x10:
            fn = word & 63
            if fn >= 0x3C:
                idx = (((word >> 6) & 31) << 2) | (fn & 3)
                name = VU_SPECIAL2.get(idx)
                if name is None:
                    name = VU_SPECIAL2_BC.get(idx & ~3)
            else:
                name = VU_SPECIAL1.get(fn)
                if name is None:
                    name = VU_SPECIAL1_BC.get(fn & ~3)
        elif rs == 0x01:
            name = 'qmfc2'
        elif rs == 0x02:
            name = 'cfc2'
        elif rs == 0x05:
            name = 'qmtc2'
        elif rs == 0x06:
            name = 'ctc2'
        elif rs == 0x08:
            name = {0: 'bc2f', 1: 'bc2t', 2: 'bc2fl', 3: 'bc2tl'}.get((word >> 16) & 31)
    else:
        name = PRIMARY.get(op)

    if name is None:
        return Insn(pc, word, 'invalid')
    ins = Insn(pc, word, name)
    if name in BRANCHES:
        ins.target = (pc + 4 + (ins.simm << 2)) & 0xFFFFFFFF
    elif name in ('j', 'jal'):
        ins.target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    return ins


# ---------------------------------------------------------------------------
# Disassembly text (for comments / debugging only)

R = REG_NAMES


def _dest(d):
    return ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if d & b)


def disasm(i):
    o = i.op
    if o in ('nop', 'sync', 'eret', 'ei', 'di', 'tlbr', 'tlbwi', 'tlbwr', 'tlbp', 'vnop', 'vwaitq'):
        return o
    if o == 'invalid':
        return '.word 0x%08x' % i.word
    if o in ('j', 'jal'):
        return '%s 0x%08x' % (o, i.target)
    if o in BRANCHES:
        if o.startswith('bc'):
            return '%s 0x%08x' % (o, i.target)
        if o in ('beq', 'bne', 'beql', 'bnel'):
            return '%s %s, %s, 0x%08x' % (o, R[i.rs], R[i.rt], i.target)
        return '%s %s, 0x%08x' % (o, R[i.rs], i.target)
    if o == 'jr':
        return 'jr %s' % R[i.rs]
    if o == 'jalr':
        return 'jalr %s, %s' % (R[i.rd], R[i.rs])
    if o in ('sll', 'srl', 'sra', 'dsll', 'dsrl', 'dsra', 'dsll32', 'dsrl32', 'dsra32'):
        return '%s %s, %s, %d' % (o, R[i.rd], R[i.rt], i.sa)
    if o in ('sllv', 'srlv', 'srav', 'dsllv', 'dsrlv', 'dsrav'):
        return '%s %s, %s, %s' % (o, R[i.rd], R[i.rt], R[i.rs])
    if o in ('syscall', 'break'):
        return '%s 0x%x' % (o, i.code)
    if o in ('mfhi', 'mflo', 'mfhi1', 'mflo1', 'mfsa'):
        return '%s %s' % (o, R[i.rd])
    if o in ('mthi', 'mtlo', 'mthi1', 'mtlo1', 'mtsa'):
        return '%s %s' % (o, R[i.rs])
    if o in ('mult', 'multu', 'div', 'divu', 'mult1', 'multu1', 'div1', 'divu1',
             'madd', 'maddu', 'madd1', 'maddu1'):
        if o.startswith('div'):
            return '%s %s, %s' % (o, R[i.rs], R[i.rt])
        return '%s %s, %s, %s' % (o, R[i.rd], R[i.rs], R[i.rt])
    if o in ('addi', 'addiu', 'slti', 'sltiu', 'daddi', 'daddiu'):
        return '%s %s, %s, %d' % (o, R[i.rt], R[i.rs], i.simm)
    if o in ('andi', 'ori', 'xori'):
        return '%s %s, %s, 0x%x' % (o, R[i.rt], R[i.rs], i.imm)
    if o == 'lui':
        return 'lui %s, 0x%x' % (R[i.rt], i.imm)
    if o in ('lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr', 'lwu', 'sb', 'sh', 'swl', 'sw',
             'sdl', 'sdr', 'swr', 'ldl', 'ldr', 'ld', 'sd', 'lq', 'sq'):
        return '%s %s, %d(%s)' % (o, R[i.rt], i.simm, R[i.rs])
    if o in ('lwc1', 'swc1'):
        return '%s $f%d, %d(%s)' % (o, i.ft, i.simm, R[i.rs])
    if o in ('lqc2', 'sqc2'):
        return '%s vf%d, %d(%s)' % (o, i.ft, i.simm, R[i.rs])
    if o in ('cache', 'pref'):
        return '%s 0x%x, %d(%s)' % (o, i.rt, i.simm, R[i.rs])
    if o in ('mfc0', 'mtc0'):
        return '%s %s, $%d' % (o, R[i.rt], i.rd)
    if o in ('mfc1', 'mtc1', 'cfc1', 'ctc1'):
        return '%s %s, $f%d' % (o, R[i.rt], i.fs)
    if o in ('qmfc2', 'qmtc2'):
        return '%s%s %s, vf%d' % (o, '.i' if i.interlock else '', R[i.rt], i.rd)
    if o in ('cfc2', 'ctc2'):
        return '%s%s %s, vi%d' % (o, '.i' if i.interlock else '', R[i.rt], i.rd)
    if o.endswith('.s') or o.startswith('cvt'):
        if o.startswith('c.'):
            return '%s $f%d, $f%d' % (o, i.fs, i.ft)
        if o in ('adda.s', 'suba.s', 'mula.s', 'madda.s', 'msuba.s'):
            return '%s $f%d, $f%d' % (o, i.fs, i.ft)
        if o in ('sqrt.s',):
            return '%s $f%d, $f%d' % (o, i.fd, i.ft)
        if o in ('abs.s', 'mov.s', 'neg.s', 'cvt.w.s', 'cvt.s.w'):
            return '%s $f%d, $f%d' % (o, i.fd, i.fs)
        return '%s $f%d, $f%d, $f%d' % (o, i.fd, i.fs, i.ft)
    if o.startswith('v'):
        d = _dest(i.dest)
        bc = 'xyzw'[i.bc]
        if o in ('viadd', 'visub', 'viand', 'vior'):
            return '%s vi%d, vi%d, vi%d' % (o, i.fd, i.fs, i.ft)
        if o == 'viaddi':
            return 'viaddi vi%d, vi%d, %d' % (i.ft, i.fs, i.imm5)
        if o in ('vcallms', 'vcallmsr'):
            return '%s 0x%x' % (o, i.imm15 * 8) if o == 'vcallms' else o
        if o in ('vdiv', 'vrsqrt'):
            return '%s Q, vf%d%s, vf%d%s' % (o, i.fs, 'xyzw'[i.fsf], i.ft, 'xyzw'[i.ftf])
        if o == 'vsqrt':
            return 'vsqrt Q, vf%d%s' % (i.ft, 'xyzw'[i.ftf])
        if o == 'vmtir':
            return 'vmtir vi%d, vf%d%s' % (i.ft, i.fs, 'xyzw'[i.fsf])
        if o == 'vmfir':
            return 'vmfir.%s vf%d, vi%d' % (d, i.ft, i.fs)
        if o in ('vilwr', 'viswr'):
            return '%s.%s vi%d, (vi%d)' % (o, d, i.ft, i.fs)
        if o in ('vlqi', 'vlqd'):
            return '%s.%s vf%d, (vi%d%s)' % (o, d, i.ft, i.fs, '++' if o == 'vlqi' else '--')
        if o in ('vsqi', 'vsqd'):
            return '%s.%s vf%d, (vi%d%s)' % (o, d, i.fs, i.ft, '++' if o == 'vsqi' else '--')
        if o in ('vrnext', 'vrget'):
            return '%s.%s vf%d, R' % (o, d, i.ft)
        if o in ('vrinit', 'vrxor'):
            return '%s R, vf%d%s' % (o, i.fs, 'xyzw'[i.fsf])
        if o == 'vclipw':
            return 'vclipw.xyz vf%d, vf%dw' % (i.fs, i.ft)
        if o in ('vmove', 'vmr32', 'vabs') or o.startswith('vitof') or o.startswith('vftoi'):
            return '%s.%s vf%d, vf%d' % (o, d, i.ft, i.fs)
        if o.endswith('abc'):
            return '%s.%s ACC, vf%d, vf%d%s' % (o[:-2] + bc, d, i.fs, i.ft, '')
        if o.endswith('bc'):
            return '%s.%s vf%d, vf%d, vf%d%s' % (o[:-2] + bc, d, i.fd, i.fs, i.ft, bc)
        if o in ('vadda', 'vmadda', 'vmula', 'vsuba', 'vmsuba', 'vopmula'):
            return '%s.%s ACC, vf%d, vf%d' % (o, d, i.fs, i.ft)
        if o in ('vaddaq', 'vmaddaq', 'vsubaq', 'vmsubaq', 'vmulaq', 'vaddai', 'vmaddai',
                 'vsubai', 'vmsubai', 'vmulai'):
            return '%s.%s ACC, vf%d' % (o, d, i.fs)
        if o in ('vaddq', 'vmaddq', 'vsubq', 'vmsubq', 'vmulq', 'vaddi', 'vmaddi', 'vsubi',
                 'vmsubi', 'vmuli', 'vmaxi', 'vminii'):
            return '%s.%s vf%d, vf%d' % (o, d, i.fd, i.fs)
        return '%s.%s vf%d, vf%d, vf%d' % (o, d, i.fd, i.fs, i.ft)
    if o in ('mtsab', 'mtsah'):
        return '%s %s, %d' % (o, R[i.rs], i.simm)
    if o in ('tgei', 'tgeiu', 'tlti', 'tltiu', 'teqi', 'tnei'):
        return '%s %s, %d' % (o, R[i.rs], i.simm)
    if o in ('tge', 'tgeu', 'tlt', 'tltu', 'teq', 'tne'):
        return '%s %s, %s' % (o, R[i.rs], R[i.rt])
    if o == 'plzcw':
        return 'plzcw %s, %s' % (R[i.rd], R[i.rs])
    if o in ('psllh', 'psrlh', 'psrah', 'psllw', 'psrlw', 'psraw'):
        return '%s %s, %s, %d' % (o, R[i.rd], R[i.rt], i.sa)
    if o in ('pmfhi', 'pmflo') or o.startswith('pmfhl'):
        return '%s %s' % (o, R[i.rd])
    if o in ('pmthi', 'pmtlo', 'pmthl.lw'):
        return '%s %s' % (o, R[i.rs])
    if o in ('pabsw', 'pabsh', 'pexeh', 'prevh', 'pexew', 'prot3w', 'pexch', 'pcpyh',
             'pexcw', 'pext5', 'ppac5'):
        return '%s %s, %s' % (o, R[i.rd], R[i.rt])
    if o in ('pdivw', 'pdivuw', 'pdivbw'):
        return '%s %s, %s' % (o, R[i.rs], R[i.rt])
    return '%s %s, %s, %s' % (o, R[i.rd], R[i.rs], R[i.rt])
