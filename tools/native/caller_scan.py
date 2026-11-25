"""Rung 4's caller scan (claude/rung4-motion-map.md, K's Q8, Nov 25): for each jal site of the given functions, does
the caller read a scratch register after the call before writing it? Natives compared by what lasts (kRenderPath's
rule) need 0 such sites; run it per batch, on the ELF here and on the room DLLs too.

python3 -I tools/native/caller_scan.py <SLUS_215.03> <repo> <addr[:returns]> ...   returns: v (v0), f (f0), n (none)
Scratch = at, a0-a3, t0-t9, v0/v1 (unless returned), hi/lo, f1-f19 (f0 unless returned), vf1-vf31, ACC, Q, vi1-vi15.
Bounded walk of the caller's control flow (both branch ways) from the return point, 96 instructions a path.
"""
import csv, re, sys, bisect
import rabbitizer as rz
from elftools.elf.elffile import ELFFile

elf, repo = sys.argv[1:3]
funcs = {}
with open(f"{repo}/recomp/kamite_functions.csv") as f:
    for r in csv.DictReader(f):
        funcs[int(r["Start"], 16)] = (r["Name"], int(r["End"], 16))
starts = sorted(funcs)
with open(elf, "rb") as f:
    segs = [(s["p_vaddr"], s.data()) for s in ELFFile(f).iter_segments() if s["p_type"] == "PT_LOAD"]


def word(a):
    for v, d in segs:
        if v <= a < v + len(d) - 3:
            return int.from_bytes(d[a - v:a - v + 4], "little")


GPR = {"$at": 1, "$v0": 2, "$v1": 3, "$a0": 4, "$a1": 5, "$a2": 6, "$a3": 7, "$t0": 8, "$t1": 9, "$t2": 10, "$t3": 11,
       "$t4": 12, "$t5": 13, "$t6": 14, "$t7": 15, "$t8": 24, "$t9": 25}
STORES = {"sb", "sh", "sw", "sd", "sq", "swc1", "sqc2", "swl", "swr", "sdl", "sdr", "ctc1", "ctc2"}
reg_rx = re.compile(r"\$(vf\d+|vi\d+|f\d+|[a-z]\w*)")


def regs_of(i):
    """(reads, writes) as sets of names, for scratch tracking."""
    op = i.getOpcodeName()
    if op.startswith(("qmtc2", "qmfc2", "cfc2", "ctc2")):
        op = op.split(".")[0]
    txt = i.disassemble().split(None, 1)
    ops = [o.strip() for o in txt[1].split(",")] if len(txt) > 1 else []
    names = []
    for o in ops:
        names.append(["$" + m for m in reg_rx.findall(o)])
    flat = [n for g in names for n in g]
    reads, writes = set(), set()
    if not flat:
        pass
    elif op in STORES or i.isBranch() or op in ("jr", "mtc1", "qmtc2", "mthi", "mtlo") and False:
        reads |= set(flat)
    elif op in ("mtc1", "qmtc2", "ctc2"):
        reads |= set(names[0]); writes |= set(names[1]) if len(names) > 1 else set()
    elif i.isBranch() or op in STORES or op == "jr":
        reads |= set(flat)
    elif op.startswith("c.") or op in ("vnop", "vwaitq"):
        reads |= set(flat)
    else:
        writes |= set(names[0]); reads |= set(flat[len(names[0]):])
        # base registers inside the first operand of loads are reads too
        if "(" in (ops[0] if ops else ""):
            pass
    # memory operands: base register is a read (it sits in the last operand "off($reg)")
    if ops and "(" in ops[-1]:
        m = reg_rx.findall(ops[-1].split("(")[1])
        reads |= {"$" + x for x in m}
        if op not in STORES:
            writes -= {"$" + x for x in m} if ops[-1] is not ops[0] else set()
    # ACC / Q
    if op.startswith("v"):
        base = op.split(".")[0]
        if re.match(r"v(madd|msub)(?!a)", base) or base.startswith("vopmsub"):
            reads.add("ACC")
        if re.match(r"v(mula|adda|suba|madda|msuba|opmula)", base) or base.startswith("vmula"):
            writes.add("ACC")
        if base.startswith(("vmadda", "vmsuba")):
            reads.add("ACC")
        if base in ("vdiv", "vsqrt", "vrsqrt"):
            writes.add("Q")
        if base.endswith("q") and base not in ("vwaitq",):
            reads.add("Q")
    if op in ("mfhi", "mflo", "mfhi1", "mflo1"):
        reads.add("HILO")
    if op in ("mult", "multu", "div", "divu", "madd", "maddu", "mult1", "div1", "divu1"):
        writes.add("HILO")
    if i.isFunctionCall():
        writes |= {"*call"}
    return reads, writes


def scratch(name, ret):
    if name in ("ACC", "Q", "HILO"):
        return True
    if name in GPR:
        if name == "$v0" and "v" in ret:
            return False
        return True
    m = re.match(r"\$f(\d+)$", name)
    if m:
        n = int(m.group(1))
        return (1 <= n <= 19) or (n == 0 and "f" not in ret)
    m = re.match(r"\$vf(\d+)$", name)
    if m:
        return int(m.group(1)) != 0
    m = re.match(r"\$vi(\d+)$", name)
    if m:
        return int(m.group(1)) != 0
    return False


def walk(pc, end, ret, written, budget, seen, flags):
    while budget > 0 and pc < end:
        if (pc, frozenset(written)) in seen:
            return
        seen.add((pc, frozenset(written)))
        i = rz.Instruction(word(pc), pc, category=rz.InstrCategory.R5900)
        reads, writes = regs_of(i)
        for r in reads:
            if scratch(r, ret) and r not in written:
                flags.add((pc, r, i.disassemble()))
        if i.isFunctionCall() or i.isJrRa():
            # delay slot reads too
            d = rz.Instruction(word(pc + 4), pc + 4, category=rz.InstrCategory.R5900)
            rr, _ = regs_of(d)
            for r in rr:
                if scratch(r, ret) and r not in written and not r.startswith("$a") and r != "$v0":
                    flags.add((pc + 4, r, d.disassemble()))
            return  # a call or return: everything scratch is dead / redefined
        written = written | writes
        budget -= 1
        if i.isBranch() or i.isJump():
            d = rz.Instruction(word(pc + 4), pc + 4, category=rz.InstrCategory.R5900)
            rr, ww = regs_of(d)
            for r in rr:
                if scratch(r, ret) and r not in written:
                    flags.add((pc + 4, r, d.disassemble()))
            w2 = written | ww
            if i.isJump() and not i.isBranch():
                if i.getOpcodeName() == "j":
                    walk(i.getInstrIndexAsVram(), end, ret, w2, budget, seen, flags)
                return
            t = i.getBranchVramGeneric()
            likely = i.isBranchLikely()
            walk(t, end, ret, w2, budget, seen, flags)
            if i.isUnconditionalBranch():
                return
            pc += 8
            written = written if likely else w2
            continue
        pc += 4


for arg in sys.argv[3:]:
    a, _, ret = arg.partition(":")
    a = int(a, 16)
    ret = ret or "n"
    sites = []
    for v, d in segs:
        for off in range(0, len(d) - 3, 4):
            w = int.from_bytes(d[off:off + 4], "little")
            if (w >> 26) == 3 and (((v + off) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) == a:
                sites.append(v + off)
    bad = []
    for s in sites:
        k = bisect.bisect_right(starts, s) - 1
        fs = starts[k]
        end = funcs[fs][1]
        flags = set()
        walk(s + 8, end, ret, set(), 96, set(), flags)
        if flags:
            bad.append((s, funcs[fs][0], sorted(flags)[:3]))
    print(f"{a:06X} {funcs[a][0]}: {len(sites)} call sites, {len(bad)} read a scratch register after the call")
    for s, n, fl in bad[:8]:
        print(f"   {s:X} in {n}: " + "; ".join(f"{p:X} {r} [{t}]" for p, r, t in fl))
