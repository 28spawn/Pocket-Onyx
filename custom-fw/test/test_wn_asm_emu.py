"""test_wn_asm_emu.py - the WaveNet CPU-stage assembly (src/nam/pa_wavenet_asm.c) run in an interpreter of its instruction subset (pi32emu.py) against the C reference.

The assembly is device-only, so no host suite ever executed it; only the unit's load-time self-test did. This assembles it with the pi32 toolchain, runs wn_asm_mix and
wn_asm_head on random rows (ordinary and saturating levels), and checks: the outputs against the reference arithmetic (leaky ReLU, saturating head sum, 3x3 mix on 64-bit
accumulators, saturating residual), that the callee-saved registers r4-r15 and sp survive the call, and that nothing but the outputs is written. The interpreter was
calibrated on the stock firmware's own loop, which passes the unit's self-test (400 of 400 ordinary-level cases agreed). Skipped when the toolchain is absent.
"""
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from pi32emu import Mem, run, parse_dis, s32, sat32, M32  # noqa: E402

TC = next((t for t in (os.path.join(os.path.expanduser('~'), 'tools', 'jieli', 'C$', 'JL', 'pi32'), r'C:\JL\pi32') if os.path.exists(os.path.join(t, 'bin', 'clang.exe'))), None)
if TC is None:
    print('no pi32 toolchain found: skipped')
    sys.exit(0)
CL, OD = os.path.join(TC, 'bin', 'clang.exe'), os.path.join(TC, 'bin', 'llvm-objdump.exe')
INC = os.path.join(TC, 'pi32v2-include')
KLEAKY = 42949632
Z, H, W, B, S, D = 0x1000, 0x2000, 0x3000, 0x3100, 0x4000, 0x5000
STACK = 0x10000


def get_c_asm(c_file):
    obj = os.path.join(os.environ.get('TEMP', HERE), 'wn_asm_emu.o')
    r = subprocess.run([CL, '-target', 'pi32v2', '-mcpu=r3', '-O2', '-ffreestanding', '-isystem', INC, '-I' + os.path.join(FW, 'include'), '-c', c_file, '-o', obj], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr[-1500:])
    return subprocess.run([OD, '-d', obj], capture_output=True, text=True).stdout


def ref_head(z, h, rows):
    h = list(h)
    for r in range(rows):
        for o in range(3):
            a = z[3 * r + o]
            if a < 0:
                a = (a * KLEAKY) >> 32
            h[3 * r + o] = sat32(h[3 * r + o] + a)
    return h


def ref_mix(z, h, w, b64, src, rows):
    h = list(h)
    dst = [0] * (3 * rows)
    for r in range(rows):
        a = []
        for o in range(3):
            v = z[3 * r + o]
            if v < 0:
                v = (v * KLEAKY) >> 32
            a.append(v)
            h[3 * r + o] = sat32(h[3 * r + o] + v)
        for o in range(3):
            acc = b64[o]
            for j in range(3):
                acc += w[3 * j + o] * a[j]
            low = ((acc >> 24) + 2 ** 31) % 2 ** 32 - 2 ** 31          # the low word of the shifted accumulator
            dst[3 * r + o] = sat32(low + src[3 * r + o])
    return h, dst


def rnd32(rng):
    if rng.random() < 0.15:
        return rng.choice([2147483647, -2147483648, 2147483000, -2147483000, 0, 1, -1])
    return rng.randint(-(1 << 23), (1 << 23)) * rng.choice([1, 2, 64, 256])


def common_checks(cpu, caller, mem, expected_writes, problems):
    stray = {a for a in mem.writes if a not in expected_writes and not (STACK - 200 <= a < STACK + 16)}
    if stray:
        problems.append('wrote outside its outputs')
    bad = [i for i in range(4, 16) if cpu.r[i] != caller[i]]
    if bad:
        problems.append('callee-saved registers changed: ' + ', '.join('r%d' % i for i in bad))
    if cpu.sp != STACK:
        problems.append('sp not restored')


def draw_z(rng, rows, zmode):
    z = [rnd32(rng) for _ in range(3 * rows)]
    if zmode:                                      # all negative / all non-negative activations: the dearest and the cheapest path of the leaky ReLU
        z = [max(-(1 << 31), -abs(v) - 1) if zmode == 'neg' else min(abs(v), (1 << 31) - 1) for v in z]
    return z


def case_mix(dis, rng, rows, zmode=None):
    mem = Mem()
    z = draw_z(rng, rows, zmode)
    h = [sat32(rnd32(rng) * rng.choice([1, 64, 512])) for _ in range(3 * rows)]
    w = [rng.randint(-(1 << 24), 1 << 24) for _ in range(9)]
    b64 = [rng.randint(-(1 << 40), 1 << 40) for _ in range(3)]
    src = [rnd32(rng) for _ in range(3 * rows + 3)]
    for i, v in enumerate(z): mem.w32(Z + 4 * i, v)
    for i, v in enumerate(h): mem.w32(H + 4 * i, v)
    for i, v in enumerate(w): mem.w32(W + 4 * i, v)
    for o in range(3):
        mem.w32(B + 8 * o, b64[o] & M32)
        mem.w32(B + 8 * o + 4, (b64[o] >> 32) & M32)
    for i, v in enumerate(src): mem.w32(S + 4 * i, v)
    mem.w32(STACK + 0, S); mem.w32(STACK + 4, D); mem.w32(STACK + 8, rows)        # arguments 5, 6, 7 are on the stack
    mem.writes.clear()
    cpu, caller, _ = run(dis, mem, [Z, H, W, B], sp_top=STACK)
    exp_h, exp_d = ref_mix(z, h, w, b64, src, rows)
    problems = []
    if [s32(mem.r32(H + 4 * i)) for i in range(3 * rows)] != exp_h: problems.append('head sums differ')
    if [s32(mem.r32(D + 4 * i)) for i in range(3 * rows)] != exp_d: problems.append('mix output differs')
    common_checks(cpu, caller, mem, {H + 4 * i for i in range(3 * rows)} | {D + 4 * i for i in range(3 * rows)}, problems)
    case_mix.last = cpu
    return problems


def case_head(dis, rng, rows, zmode=None):
    mem = Mem()
    z = draw_z(rng, rows, zmode)
    h = [sat32(rnd32(rng) * rng.choice([1, 64, 512])) for _ in range(3 * rows)]
    for i, v in enumerate(z): mem.w32(Z + 4 * i, v)
    for i, v in enumerate(h): mem.w32(H + 4 * i, v)
    mem.writes.clear()
    cpu, caller, _ = run(dis, mem, [Z, H, rows], sp_top=STACK)
    problems = []
    if [s32(mem.r32(H + 4 * i)) for i in range(3 * rows)] != ref_head(z, h, rows): problems.append('head sums differ')
    common_checks(cpu, caller, mem, {H + 4 * i for i in range(3 * rows)}, problems)
    case_head.last = cpu
    return problems


def per_row(dis, case, zmode, n=40):
    """issue slots / instructions per row: the marginal cost of a row (a 12-row call minus a 2-row call, over 10 rows), averaged over random operands"""
    rng = random.Random(777)
    random.seed(5)
    tot = [0, 0]
    for _ in range(n):
        res = []
        for rows in (2, 12):
            assert not case(dis, rng, rows, zmode)
            res.append((case.last.bundles, case.last.insns))
        tot[0] += (res[1][0] - res[0][0]) / 10.0
        tot[1] += (res[1][1] - res[0][1]) / 10.0
    return tot[0] / n, tot[1] / n


def main():
    text = get_c_asm(os.path.join(FW, 'src', 'nam', 'pa_wavenet_asm.c'))
    bad = 0
    for fn, case in (('wn_asm_mix', case_mix), ('wn_asm_head', case_head)):
        dis = parse_dis(text, fn)
        if not dis:
            print('%s: not found (the kernel is not assembled in this file)' % fn)
            bad += 1
            continue
        rng = random.Random(12345)
        random.seed(99)
        fails, n = {}, 0
        for rows in [1, 2, 3, 5, 8, 11, 11, 11, 11]:
            for _ in range(60):
                for p in case(dis, rng, rows):
                    fails[p] = fails.get(p, 0) + 1
                n += 1
        print('%s: %d cases, %s' % (fn, n, 'all match the reference' if not fails else 'FAILS: ' + '; '.join('%s (%dx)' % kv for kv in fails.items())))
        bad += bool(fails)
        if not fails:
            print('  per row (issue slots / instructions): ' + ', '.join('%s %.1f / %.1f' % ((m or 'random',) + per_row(dis, case, m)) for m in (None, 'pos', 'neg')))
    print('RESULT: FAILED' if bad else 'RESULT: ALL PASSED')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
