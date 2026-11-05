"""A tiny pi32v2 interpreter for exactly the instruction subset of the WaveNet CPU-stage kernels (wn_asm_mix / wn_asm_head), driven by llvm-objdump text.
It is only as good as its model of the ISA, so it is calibrated first on the stock-identical kernel (known-good on the unit) before it judges anything else."""
import re
import subprocess
import sys
import random

M32 = 0xFFFFFFFF


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def sat32(v):
    return 2147483647 if v > 2147483647 else (-2147483648 if v < -2147483648 else v)


class Mem:
    def __init__(self):
        self.d = {}
        self.writes = set()

    def r32(self, a):
        assert a % 4 == 0, f"unaligned read {a:#x}"
        return self.d.get(a, 0xDEADBEEF)

    def w32(self, a, v):
        assert a % 4 == 0, f"unaligned write {a:#x}"
        self.d[a] = v & M32
        self.writes.add(a)


def parse_dis(text, sym):
    """-> (instrs, addr_index) for function `sym`: list of (addr, text)."""
    out, on = [], False
    for ln in text.split('\n'):
        if re.match(r'^[A-Za-z_.][\w.]*:\s*$', ln):
            on = ln.strip() == sym + ':'
            continue
        m = re.match(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(.*)$', ln)
        if on and m:
            txt = m.group(2).rstrip()
            if 'goto' not in txt:
                txt = re.sub(r'\s*<[^>]*>\s*$', '', txt)            # objdump's '<sym+off : addr>' note on immediates
            out.append((int(m.group(1), 16), txt))
        elif on and ln.strip() == '}':
            out.append((None, '}'))
    return out


def regs_in_list(s):
    """'r6-r4' / 'r8, r7' / 'rets, r15-r4' -> sorted register numbers (ascending) and flags"""
    regs, flags = set(), []
    for part in [p.strip() for p in s.split(',')]:
        if part in ('rets', 'pc'):
            flags.append(part)
            continue
        m = re.match(r'r(\d+)-r(\d+)$', part)
        if m:
            a, b = int(m.group(1)), int(m.group(2))
            regs.update(range(min(a, b), max(a, b) + 1))
        else:
            regs.add(int(part[1:]))
    return sorted(regs), flags


class CPU:
    def __init__(self, mem):
        self.r = [0] * 16
        self.mem = mem
        self.rets = None

    # ---- operand helpers
    def val(self, tok):
        tok = tok.strip()
        if tok == 'sp':
            return self.r[13] if False else self.sp
        if re.match(r'^r\d+$', tok):
            return self.r[int(tok[1:])]
        return int(tok, 0) & M32

    def pair(self, name):  # 'r9_r8' -> (hi, lo)
        m = re.match(r'r(\d+)_r(\d+)$', name)
        return int(m.group(1)), int(m.group(2))

    def rp_get(self, name):
        hi, lo = self.pair(name)
        return s64((self.r[hi] << 32) | self.r[lo])

    def rp_set(self, regs, name, v):
        hi, lo = self.pair(name)
        v &= (1 << 64) - 1
        regs[lo] = v & M32
        regs[hi] = (v >> 32) & M32


def s64(v):
    v &= (1 << 64) - 1
    return v - (1 << 64) if v & (1 << 63) else v


def run(dis, mem, args, sp_top=0x10000, trace=False):
    """Execute from the function's first instruction until it returns. args: list of up to 4 register args; stack args are written by the caller into mem."""
    cpu = CPU(mem)
    cpu.sp = sp_top
    for i, a in enumerate(args[:4]):
        cpu.r[i] = a & M32
    for i in range(4, 16):
        cpu.r[i] = random.getrandbits(32)          # the callee-saved registers hold the caller's values: they must survive
    caller_regs = list(cpu.r)
    cpu.bundles = 0                                # issue slots used: one per instruction or `A # B` pair, plus the body of an executed ifs
    cpu.insns = 0                                  # instructions executed (the two halves of a pair count separately)
    addr_to_idx = {a: i for i, (a, t) in enumerate(dis) if a is not None}
    pc_i = 0
    steps = 0
    while True:
        steps += 1
        assert steps < 200000, 'runaway'
        a, t = dis[pc_i]
        # a predicated block:  ifs (rX < 0) {   ...   }
        m = re.match(r'ifs \((r\d+) < 0\) \{$', t)
        if m:
            cond = s32(cpu.r[int(m.group(1)[1:])]) < 0
            cpu.bundles += 1
            cpu.insns += 1
            j = pc_i + 1
            body = []
            while dis[j][1] != '}':
                body.append(dis[j][1])
                j += 1
            if cond:
                for b in body:
                    step([b], cpu, mem)
                    cpu.bundles += 1
                    cpu.insns += 1
            pc_i = j + 1
            continue
        # a '#'-paired group: the two instructions read the state from before either of them
        group = [t]
        n = pc_i + 1
        while group[-1].rstrip().endswith('#'):
            group.append(dis[n][1])
            n += 1
        cpu.bundles += 1
        cpu.insns += len(group)
        ret = step(group, cpu, mem, addr_to_idx=addr_to_idx, pc_i=pc_i)
        if ret == 'RET':
            break
        pc_i = ret if isinstance(ret, int) else n
    return cpu, caller_regs, steps


def _regs(tok):
    out = set()
    for m in re.finditer(r'r(\d+)_r(\d+)', tok):
        out |= {int(m.group(1)), int(m.group(2))}
    for m in re.finditer(r'\{r(\d+)-r(\d+)\}', tok):
        out |= set(range(int(m.group(1)), int(m.group(2)) + 1))
    tok = re.sub(r'r\d+_r\d+|\{[^}]*\}', '', tok)
    return out | {int(m.group(1)) for m in re.finditer(r'r(\d+)', tok)}


def reads_writes(t):
    """(registers read, registers written) of one instruction, from its text (only the forms the kernels use)"""
    t = t.strip()
    if t.startswith('['):                                          # a store: [rB+off] = rS  /  [rB++=4] = rS  /  [rB++] = {list}
        lhs, rhs = t.split(' = ', 1)
        w = _regs(lhs) if '++' in lhs else set()
        return _regs(lhs) | _regs(rhs), w
    m = re.match(r'^(.*?) (\+=|>>>=|<<=|=) (.*)$', t)
    lhs, op, rhs = m.group(1), m.group(2), m.group(3)
    r = _regs(rhs)
    w = _regs(lhs)
    if op != '=':
        r |= w
    if '++' in rhs:                                                 # post-increment: the base register is written too
        w |= {int(x) for x in re.findall(r'\[r(\d+)\+\+', rhs)}
    return r, w


def step(group, cpu, mem, addr_to_idx=None, pc_i=None):
    """execute one instruction or a parallel pair. Returns None (fall through), an index (branch) or 'RET'."""
    group = [g.rstrip().rstrip('#').rstrip() for g in group]
    if len(group) == 2:                    # the kernels never rely on the order the two halves of a pair take effect: they must be independent
        (r0_, w0_), (r1_, w1_) = reads_writes(group[0]), reads_writes(group[1])
        assert not (w0_ & (r1_ | w1_)) and not (w1_ & r0_), 'dependent instructions in one pair: %r / %r' % tuple(group)
    snap = list(cpu.r)
    writes_r = {}                                  # register writes, applied after all reads
    writes_m = []                                  # (addr, value)
    post = {}                                      # post-increment register updates
    branch = None

    def rd(tok):
        tok = tok.strip()
        if tok == 'sp':
            return cpu.sp
        m = re.match(r'^r(\d+)$', tok)
        if m:
            return snap[int(m.group(1))]
        return int(tok, 0) & M32

    for t in group:
        # --- prologue / epilogue
        m = re.match(r'\[--sp\] = \{(.*)\}$', t)
        if m:
            regs, flags = regs_in_list(m.group(1))
            words = ([0xAAAA0000] if 'rets' in flags else []) + [snap[x] for x in regs]
            cpu.sp -= 4 * len(words)
            for i, w in enumerate(words):
                mem.w32(cpu.sp + 4 * i, w)
            cpu.pushed = (regs, flags, cpu.sp)
            continue
        m = re.match(r'\{pc, (.*)\} = \[sp\+\+\]$', t)
        if m:
            regs, flags = regs_in_list(m.group(1))
            base = cpu.sp
            words = [mem.r32(base + 4 * i) for i in range(len(regs) + 1)]
            # the popped list must be what was pushed: registers come back in the same order
            for i, x in enumerate(regs):
                writes_r[x] = words[1 + i]
            cpu.sp += 4 * (len(regs) + 1)
            branch = 'RET'
            continue
        m = re.match(r'\{(.*)\} = \[sp\+\+\]$', t)
        if m:
            regs, _ = regs_in_list(m.group(1))
            for i, x in enumerate(regs):
                writes_r[x] = mem.r32(cpu.sp + 4 * i)
            cpu.sp += 4 * len(regs)
            continue
        if t == 'rts':
            branch = 'RET'
            continue
        m = re.match(r'sp \+= (-?\d+)$', t)
        if m:
            cpu.sp += int(m.group(1))
            continue
        # --- ifs-less single predicated lines never appear here
        # --- loads of several registers
        m = re.match(r'\{([^}]*)\} = \[(r\d+)\+\+\]$', t)
        if m:
            regs, _ = regs_in_list(m.group(1))
            base = int(m.group(2)[1:])
            addr = snap[base]
            for i, x in enumerate(regs):
                writes_r[x] = mem.r32((addr + 4 * i) & M32)
            post[base] = (addr + 4 * len(regs)) & M32
            continue
        m = re.match(r'\[(r\d+)\+\+\] = \{([^}]*)\}$', t)
        if m:
            regs, _ = regs_in_list(m.group(2))
            base = int(m.group(1)[1:])
            addr = snap[base]
            for i, x in enumerate(regs):
                writes_m.append(((addr + 4 * i) & M32, snap[x]))
            post[base] = (addr + 4 * len(regs)) & M32
            continue
        # --- single loads / stores
        m = re.match(r'sp = sp \+ (-?\d+)$', t)
        if m:
            cpu.sp += int(m.group(1))
            continue
        m = re.match(r'(r\d+_r\d+) = d\[(sp|r\d+)(?:\+(-?\d+))?\]$', t)
        if m:
            hi, lo = cpu.pair(m.group(1))
            a_ = (cpu.sp if m.group(2) == 'sp' else snap[int(m.group(2)[1:])]) + int(m.group(3) or 0)
            writes_r[lo] = mem.r32(a_ & M32)
            writes_r[hi] = mem.r32((a_ + 4) & M32)
            continue
        m = re.match(r'(r\d+) = (r\d+_r\d+) >> (\d+) \(s\)$', t)
        if m:
            hi, lo = cpu.pair(m.group(2))
            writes_r[int(m.group(1)[1:])] = (s64((snap[hi] << 32) | snap[lo]) >> int(m.group(3))) & M32
            continue
        m = re.match(r'if \((r\d+) (==|!=) (r\d+|-?\d+)\) goto (-?\d+) <[^:]*: ([0-9a-f]+) >$', t)
        if m:
            a_ = snap[int(m.group(1)[1:])]
            b_ = rd(m.group(3))
            if (a_ == b_) == (m.group(2) == '=='):
                branch = addr_to_idx[int(m.group(5), 16)]
            continue
        m = re.match(r'(r\d+) = \[sp\+(\d+)\]$', t)
        if m:
            writes_r[int(m.group(1)[1:])] = mem.r32(cpu.sp + int(m.group(2)))
            continue
        m = re.match(r'\[sp\+(\d+)\] = (r\d+)$', t)
        if m:
            writes_m.append((cpu.sp + int(m.group(1)), snap[int(m.group(2)[1:])]))
            continue
        m = re.match(r'(r\d+) = \[(r\d+)\+\+=4\]$', t)
        if m:
            base = int(m.group(2)[1:])
            addr = snap[base]
            writes_r[int(m.group(1)[1:])] = mem.r32(addr)
            post[base] = (addr + 4) & M32
            continue
        m = re.match(r'\[(r\d+)\+\+=4\] = (r\d+)$', t)
        if m:
            base = int(m.group(1)[1:])
            addr = snap[base]
            writes_m.append((addr, snap[int(m.group(2)[1:])]))
            post[base] = (addr + 4) & M32
            continue
        m = re.match(r'(r\d+) = \[(r\d+)\+(-?\d+)\]$', t)
        if m:
            writes_r[int(m.group(1)[1:])] = mem.r32((snap[int(m.group(2)[1:])] + int(m.group(3))) & M32)
            continue
        m = re.match(r'\[(r\d+)\+(-?\d+)\] = (r\d+)$', t)
        if m:
            writes_m.append(((snap[int(m.group(1)[1:])] + int(m.group(2))) & M32, snap[int(m.group(3)[1:])]))
            continue
        # --- ALU
        m = re.match(r'(r\d+) = (\d+|0x[0-9a-fA-F]+|-\d+)$', t)
        if m:
            writes_r[int(m.group(1)[1:])] = int(m.group(2), 0) & M32
            continue
        m = re.match(r'(r\d+) = (r\d+)$', t)
        if m:
            writes_r[int(m.group(1)[1:])] = snap[int(m.group(2)[1:])]
            continue
        m = re.match(r'(r\d+) = (r\d+) ([+-]) (r\d+|0x[0-9a-fA-F]+|\d+)( \(ssat\))?$', t)
        if m:
            a_ = s32(snap[int(m.group(2)[1:])])
            b_ = s32(rd(m.group(4)))
            v = a_ + b_ if m.group(3) == '+' else a_ - b_
            writes_r[int(m.group(1)[1:])] = (sat32(v) if m.group(5) else v) & M32
            continue
        m = re.match(r'(r\d+) \+= (r\d+)$', t)
        if m:
            d = int(m.group(1)[1:])
            writes_r[d] = (snap[d] + snap[int(m.group(2)[1:])]) & M32
            continue
        m = re.match(r'(r\d+_r\d+) = (r\d+) \* (r\d+) \(s\)$', t)
        if m:
            hi, lo = cpu.pair(m.group(1))
            p = s32(snap[int(m.group(2)[1:])]) * s32(snap[int(m.group(3)[1:])])
            writes_r[lo] = p & M32
            writes_r[hi] = (p >> 32) & M32
            continue
        m = re.match(r'(r\d+_r\d+) \+= (r\d+) \* (r\d+) \(s\)$', t)
        if m:
            hi, lo = cpu.pair(m.group(1))
            acc = s64((snap[hi] << 32) | snap[lo])
            p = acc + s32(snap[int(m.group(2)[1:])]) * s32(snap[int(m.group(3)[1:])])
            writes_r[lo] = p & M32
            writes_r[hi] = (p >> 32) & M32
            continue
        m = re.match(r'(r\d+_r\d+) >>>= (\d+)$', t)
        if m:
            hi, lo = cpu.pair(m.group(1))
            acc = s64((snap[hi] << 32) | snap[lo]) >> int(m.group(2))
            writes_r[lo] = acc & M32
            writes_r[hi] = (acc >> 32) & M32
            continue
        m = re.match(r'if \(--(r\d+) != 0\) goto (-?\d+) <[^:]*: ([0-9a-f]+) >$', t)
        if m:
            d = int(m.group(1)[1:])
            writes_r[d] = (snap[d] - 1) & M32
            if ((snap[d] - 1) & M32) != 0:
                branch = addr_to_idx[int(m.group(3), 16)]
            continue
        raise SystemExit(f'unsupported instruction: {t!r}')

    for k, v in writes_r.items():
        cpu.r[k] = v & M32
    for k, v in post.items():
        cpu.r[k] = v
    for a, v in writes_m:
        mem.w32(a, v)
    return branch
