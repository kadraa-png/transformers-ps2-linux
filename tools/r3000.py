"""R3000A / PS2 IOP MIPS-I instruction decoder and disassembler.

decode(word, pc) returns an Insn using the same field/control-flow interface as
r5900.decode. Addresses are unsigned 32-bit guest addresses.
"""
from r5900 import REG_NAMES, sext16

PRIMARY = {
    0x02: 'j', 0x03: 'jal', 0x04: 'beq', 0x05: 'bne', 0x06: 'blez',
    0x07: 'bgtz', 0x08: 'addi', 0x09: 'addiu', 0x0a: 'slti',
    0x0b: 'sltiu', 0x0c: 'andi', 0x0d: 'ori', 0x0e: 'xori',
    0x0f: 'lui', 0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw',
    0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr', 0x28: 'sb', 0x29: 'sh',
    0x2a: 'swl', 0x2b: 'sw', 0x2e: 'swr', 0x30: 'lwc0',
    0x31: 'lwc1', 0x32: 'lwc2', 0x33: 'lwc3', 0x38: 'swc0',
    0x39: 'swc1', 0x3a: 'swc2', 0x3b: 'swc3',
}
SPECIAL = {
    0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x04: 'sllv',
    0x06: 'srlv', 0x07: 'srav', 0x08: 'jr', 0x09: 'jalr',
    0x0c: 'syscall', 0x0d: 'break', 0x10: 'mfhi', 0x11: 'mthi',
    0x12: 'mflo', 0x13: 'mtlo', 0x18: 'mult', 0x19: 'multu',
    0x1a: 'div', 0x1b: 'divu', 0x20: 'add', 0x21: 'addu',
    0x22: 'sub', 0x23: 'subu', 0x24: 'and', 0x25: 'or',
    0x26: 'xor', 0x27: 'nor', 0x2a: 'slt', 0x2b: 'sltu',
}
REGIMM = {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}
BRANCHES = {'beq', 'bne', 'blez', 'bgtz', *REGIMM.values(),
            'bc0f', 'bc0t', 'bc1f', 'bc1t', 'bc2f', 'bc2t', 'bc3f', 'bc3t'}
R = REG_NAMES


class Insn:
    __slots__ = ('pc', 'word', 'op', 'rs', 'rt', 'rd', 'sa', 'imm', 'simm',
                 'target', 'code')

    def __init__(self, pc, word, op):
        self.pc, self.word, self.op = pc, word, op
        self.rs, self.rt = (word >> 21) & 31, (word >> 16) & 31
        self.rd, self.sa = (word >> 11) & 31, (word >> 6) & 31
        self.imm = word & 0xffff
        self.simm = sext16(self.imm)
        self.code = (word >> 6) & 0xfffff
        self.target = None

    @property
    def is_branch(self):
        return self.op in BRANCHES

    @property
    def is_likely(self):
        return False  # MIPS-I has no branch-likely forms

    @property
    def is_jump(self):
        return self.op in ('j', 'jal', 'jr', 'jalr')

    @property
    def has_delay(self):
        return self.is_branch or self.is_jump

    @property
    def is_call(self):
        return self.op in ('jal', 'jalr', 'bltzal', 'bgezal')

    def __repr__(self):
        return '%08x: %08x %s' % (self.pc, self.word, disasm(self))


def decode(word, pc):
    major = word >> 26
    if major == 0:
        name = SPECIAL.get(word & 63)
        if word == 0:
            name = 'nop'
    elif major == 1:
        name = REGIMM.get((word >> 16) & 31)
    elif 16 <= major <= 19:
        cop = major - 16
        rs = (word >> 21) & 31
        if rs == 0:
            name = 'mfc%d' % cop
        elif rs == 2:
            name = 'cfc%d' % cop
        elif rs == 4:
            name = 'mtc%d' % cop
        elif rs == 6:
            name = 'ctc%d' % cop
        elif rs == 8:
            name = ('bc%df' if not (word & 0x10000) else 'bc%dt') % cop
        elif rs & 16:
            name = 'cop%d' % cop  # implementation-defined COP operation
        else:
            name = None
    else:
        name = PRIMARY.get(major)
    i = Insn(pc, word, name or 'invalid')
    if i.is_branch:
        i.target = (pc + 4 + (i.simm << 2)) & 0xffffffff
    elif name in ('j', 'jal'):
        i.target = ((pc + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)
    return i


def disasm(i):
    o = i.op
    if o == 'invalid':
        return '.word 0x%08x' % i.word
    if o == 'nop':
        return o
    if o in ('j', 'jal'):
        return '%s 0x%08x' % (o, i.target)
    if o in BRANCHES:
        if o.startswith('bc'):
            return '%s 0x%08x' % (o, i.target)
        if o in ('beq', 'bne'):
            return '%s %s, %s, 0x%08x' % (o, R[i.rs], R[i.rt], i.target)
        return '%s %s, 0x%08x' % (o, R[i.rs], i.target)
    if o == 'jr':
        return 'jr %s' % R[i.rs]
    if o == 'jalr':
        return 'jalr %s, %s' % (R[i.rd], R[i.rs])
    if o in ('sll', 'srl', 'sra'):
        return '%s %s, %s, %d' % (o, R[i.rd], R[i.rt], i.sa)
    if o in ('sllv', 'srlv', 'srav'):
        return '%s %s, %s, %s' % (o, R[i.rd], R[i.rt], R[i.rs])
    if o in ('syscall', 'break'):
        return '%s 0x%x' % (o, i.code)
    if o in ('mfhi', 'mflo'):
        return '%s %s' % (o, R[i.rd])
    if o in ('mthi', 'mtlo'):
        return '%s %s' % (o, R[i.rs])
    if o in ('mult', 'multu', 'div', 'divu'):
        return '%s %s, %s' % (o, R[i.rs], R[i.rt])
    if o in ('addi', 'addiu', 'slti', 'sltiu'):
        return '%s %s, %s, %d' % (o, R[i.rt], R[i.rs], i.simm)
    if o in ('andi', 'ori', 'xori'):
        return '%s %s, %s, 0x%x' % (o, R[i.rt], R[i.rs], i.imm)
    if o == 'lui':
        return 'lui %s, 0x%x' % (R[i.rt], i.imm)
    if o in ('lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr', 'sb', 'sh', 'swl', 'sw', 'swr'):
        return '%s %s, %d(%s)' % (o, R[i.rt], i.simm, R[i.rs])
    if o.startswith(('lwc', 'swc')):
        return '%s $%d, %d(%s)' % (o, i.rt, i.simm, R[i.rs])
    if o[:2] in ('mf', 'cf', 'mt', 'ct') and len(o) == 4 and o[-1].isdigit():
        return '%s %s, $%d' % (o, R[i.rt], i.rd)
    if o.startswith('cop'):
        return '%s 0x%x' % (o, i.word & 0x1ffffff)
    return '%s %s, %s, %s' % (o, R[i.rd], R[i.rs], R[i.rt])
