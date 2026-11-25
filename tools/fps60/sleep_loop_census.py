#!/usr/bin/env python3
"""Register counters in sleep(1) task loops (claude/room-dll-census.md "Census gap").

A cTaskWork coroutine keeps its saved registers across cTaskWork_sleep (0x2D5AA0). sleep(1) stays one vblank at 60 FPS
(km_fps60_task_sleep doubles only sleeps of 2+), so a counter kept in a saved register (s0-s7, fp, f20-f31) and stepped
once a poll runs twice as fast at 60. r206's spawner (s0 counted down in bgez's delay slot, fixed Nov 24 by
km_fps60_r206_spawn_gate) was one; the memory-counter censuses (countdown_census.py: load / addiu / store) can't see
this shape.

For every `jal 0x2D5AA0` whose a1 is 1 (or not a constant: listed as a1=?), the innermost backward branch / jump after
it that lands at or before it is the loop. Every step of a saved register inside that loop is listed (addiu / addu /
subu / add.s / sub.s / mul.s with the register on both sides), with whether it sits in a delay slot and which branches
in the loop test it.

Usage:
  sleep_loop_census.py emit --rel-dir REL --boot-elf SLUS --out OUT   emit every .rel in REL with snr2 at its slot base
  sleep_loop_census.py scan OUT [SLUS kamite_functions.csv]            scan OUT/<mod>/<mod>.elf (+ the main ELF)
"""
import bisect, csv, glob, os, struct, sys

SLEEP = 0x2D5AA0
JAL_SLEEP = 0x0C000000 | (SLEEP >> 2)
SAVED_GPR = set(range(16, 24)) | {30}
SAVED_FPR = set(range(20, 32))
GPR = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
LOOP_WINDOW = 0x800  # how far past the sleep call a loop's back branch may sit


def load_elf(path):
    d = open(path, 'rb').read()
    phoff, phnum = struct.unpack_from('<I', d, 0x1C)[0], struct.unpack_from('<H', d, 0x2C)[0]
    segs = []
    for i in range(phnum):
        t, off, va, _pa, fs, _ms, fl, _al = struct.unpack_from('<8I', d, phoff + 32 * i)
        if t == 1 and fs:
            segs.append((va, off, fs, fl))
    def rd(a):
        for va, off, fs, _fl in segs:
            if va <= a < va + fs:
                return struct.unpack_from('<I', d, off + a - va)[0]
        return None
    return segs, rd


def simm(w):
    i = w & 0xFFFF
    return i - 0x10000 if i & 0x8000 else i


def branch_target(w, a):
    """Target of a branch / jump at a, or None (jr / jalr / not a branch). 'link' jumps (jal / bal) are calls."""
    op = w >> 26
    if op == 0x02:  # j
        return (a & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    if op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
        return a + 4 + (simm(w) << 2)
    if op == 0x01 and ((w >> 16) & 31) in (0x00, 0x01, 0x02, 0x03):  # bltz / bgez / bltzl / bgezl (not bal)
        return a + 4 + (simm(w) << 2)
    if op == 0x11 and ((w >> 21) & 31) == 8:  # bc1f / bc1t (+l)
        return a + 4 + (simm(w) << 2)
    return None


def is_branch(w):
    op = w >> 26
    if op == 0 and (w & 0x3F) in (0x08, 0x09):
        return True
    if op == 0x03 or (op == 0x01 and ((w >> 16) & 31) in (0x10, 0x11)):
        return True
    return branch_target(w, 0) is not None


def gpr_written(w):
    op = w >> 26
    if op == 0:
        return (w >> 11) & 31 if (w & 0x3F) not in (0x08, 0x18, 0x19, 0x1A, 0x1B) else None
    if 0x08 <= op <= 0x0F or op in (0x18, 0x19) or 0x20 <= op <= 0x27 or op in (0x1A, 0x1B, 0x37):
        return (w >> 16) & 31
    if op == 0x03:
        return 31
    if op == 0x11 and ((w >> 21) & 31) in (0, 2):  # mfc1 / cfc1
        return (w >> 16) & 31
    return None


def a1_value(rd, call):
    """a1 at the sleep call: the delay slot first, then back to the last write of a1 (None = not a constant)."""
    for a in [call + 4] + list(range(call - 4, call - 0x40, -4)):
        w = rd(a)
        if w is None:
            return None
        if a != call + 4 and is_branch(w):
            return None
        if gpr_written(w) == 5:
            op = w >> 26
            if op in (0x09, 0x19, 0x0D) and ((w >> 21) & 31) == 0:  # addiu / daddiu / ori a1, zero, imm
                return (w & 0xFFFF) if op == 0x0D else simm(w)
            if op == 0 and (w & 0x3F) in (0x21, 0x25, 0x2D) and ((w >> 21) & 31) == 0 and ((w >> 16) & 31) == 0:
                return 0
            return None
    return None


def step_of(w):
    """(kind, reg, text) when w steps a saved register in place, else None."""
    op = w >> 26
    rs, rt, rd_ = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op in (0x09, 0x19) and rs == rt and rt in SAVED_GPR:
        return 'gpr', rt, f'addiu {GPR[rt]},{GPR[rs]},{simm(w)}'
    if op == 0 and (w & 0x3F) in (0x21, 0x23, 0x2D, 0x2F) and rd_ in SAVED_GPR and rd_ in (rs, rt):
        name = {0x21: 'addu', 0x23: 'subu', 0x2D: 'daddu', 0x2F: 'dsubu'}[w & 0x3F]
        return 'gpr', rd_, f'{name} {GPR[rd_]},{GPR[rs]},{GPR[rt]}'
    if op == 0x11 and ((w >> 21) & 31) == 0x10:  # COP1 .s
        fn, ft, fs, fd = w & 0x3F, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        if fn in (0, 1, 2) and fd in SAVED_FPR and fd in (fs, ft):
            return 'fpr', fd, f'{["add", "sub", "mul"][fn]}.s $f{fd},$f{fs},$f{ft}'
    return None


def reads_gpr(w, reg):
    op = w >> 26
    rs, rt = (w >> 21) & 31, (w >> 16) & 31
    if op == 0:
        return reg in (rs, rt)
    if op in (0x04, 0x05, 0x14, 0x15) or op >= 0x28:  # beq / bne (+l) and stores read rt too
        return reg in (rs, rt)
    return rs == reg


def scan(path, csv_path, label):
    segs, rd = load_elf(path)
    names = {}
    if csv_path and os.path.isfile(csv_path):
        for r in csv.reader(open(csv_path)):
            try:
                names[int(r[1], 16)] = r[0]
            except (ValueError, IndexError):
                pass
    keys = sorted(names)
    def fn(a):
        i = bisect.bisect_right(keys, a) - 1
        return names[keys[i]] if i >= 0 else '?'
    out = []
    for va, _off, fs, fl in segs:
        if not fl & 1:  # executable segments only
            continue
        for call in range(va, va + fs, 4):
            if rd(call) != JAL_SLEEP:
                continue
            n = a1_value(rd, call)
            if n not in (1, None):
                continue
            loop = None
            for b in range(call + 8, min(call + LOOP_WINDOW, va + fs), 4):
                t = branch_target(rd(b), b)
                if t is not None and va <= t <= call and (loop is None or t > loop[0]):
                    loop = (t, b)
                    break  # the first back branch past the call that reaches it closes the innermost loop
            if not loop:
                out.append((label, fn(call), call, n, None, []))
                continue
            top, back = loop
            # Inner loops that don't hold the sleep (array walks): their steps run many times a poll, not once.
            inner = []
            for c in range(top, back, 4):
                t = branch_target(rd(c), c)
                if t is not None and top <= t <= c and not (t <= call <= c):
                    inner.append((t, c + 4))
            steps = []
            for a in range(top, back + 8, 4):
                s = step_of(rd(a))
                if not s or any(t <= a <= e for t, e in inner):
                    continue
                kind, reg, text = s
                tests = []
                if kind == 'gpr':
                    for c in range(top, back + 8, 4):
                        w = rd(c)
                        regs = {reg}
                        # a copy just before (move vX, reg: the r206 spawner's daddu v0,s0,zero) tests the same value
                        for p in range(c - 4, c - 16, -4):
                            x = rd(p)
                            if x >> 26 == 0 and (x & 0x3F) in (0x21, 0x25, 0x2D) and reg in ((x >> 21) & 31, (x >> 16) & 31) \
                                    and 0 in ((x >> 21) & 31, (x >> 16) & 31):
                                regs.add((x >> 11) & 31)
                        if branch_target(w, c) is not None and any(reads_gpr(w, r) for r in regs):
                            tests.append(c)
                        elif w >> 26 in (0x0A, 0x0B) and ((w >> 21) & 31) in regs:  # slti / sltiu reg, imm
                            tests.append(c)
                        elif w >> 26 == 0 and (w & 0x3F) in (0x2A, 0x2B) and regs & {(w >> 21) & 31, (w >> 16) & 31}:
                            tests.append(c)
                steps.append((a, text, is_branch(rd(a - 4)), tests))
            out.append((label, fn(call), call, n, loop, steps))
    return out


def emit(rel_dir, boot_elf, out):
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(here, '..', 'snr2'))
    import snr2
    def all_modules(self):  # the rel dir stands in for the disc (export_index would otherwise open the ISO)
        for i, p in enumerate(sorted(glob.glob(os.path.join(rel_dir, '*.rel')))):
            with open(p, 'rb') as f:
                yield i, snr2.Module(f.read(), os.path.basename(p))
    snr2.Ctx.all_modules = all_modules
    bases = {'event': 0x7A8080, 'mcport': 0x7C0080}
    for rel in sorted(glob.glob(os.path.join(rel_dir, '*.rel'))):
        mod = os.path.splitext(os.path.basename(rel))[0].lower()
        dst = os.path.join(out, mod)
        if os.path.isfile(os.path.join(dst, mod + '.elf')):
            continue
        base = bases.get(mod, 0x799080)
        try:
            snr2.main(['--rel-dir', rel_dir, '--boot-elf', boot_elf, 'emit', rel, '--base', hex(base), '--out', dst])
        except SystemExit as e:
            print(f'{mod}: emit stopped: {e}', file=sys.stderr)


def main():
    a = sys.argv[1:]
    if a and a[0] == 'emit':
        emit(a[a.index('--rel-dir') + 1], a[a.index('--boot-elf') + 1], a[a.index('--out') + 1])
        return
    if not a or a[0] != 'scan':
        sys.exit(__doc__)
    rows = []
    for elf in sorted(glob.glob(os.path.join(a[1], '*', '*.elf'))):
        mod = os.path.splitext(os.path.basename(elf))[0]
        rows += scan(elf, os.path.join(os.path.dirname(elf), mod + '_functions.csv'), mod)
    if len(a) > 2:
        rows += scan(a[2], a[3] if len(a) > 3 else None, 'main')
    calls = len(rows)
    counted = [r for r in rows if r[5]]
    print(f'{calls} sleep(1 or ?) calls, {sum(1 for r in rows if r[4] is None)} not in a loop found, '
          f'{len(counted)} loops step a saved register')
    for label, f, call, n, loop, steps in counted:
        print(f'{label:7s} {f:40s} sleep@0x{call:08X} a1={"?" if n is None else n} loop 0x{loop[0]:08X}..0x{loop[1]:08X}')
        for a, text, delay, tests in steps:
            print(f'          0x{a:08X} {text:28s}{" DELAY SLOT" if delay else ""}'
                  f'{"  tested at " + ", ".join(f"0x{t:08X}" for t in tests) if tests else "  (not tested in the loop)"}')


if __name__ == '__main__':
    main()
