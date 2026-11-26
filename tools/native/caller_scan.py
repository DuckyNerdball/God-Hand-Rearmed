"""Rung 4's caller scan (claude/rung4-motion-map.md, K's Q8, Nov 25): for each jal site of the given functions, does
the caller read a scratch register after the call before writing it? Natives compared by what lasts (kRenderPath's
rule) need 0 such sites; run it per batch, on the ELF here and on the room DLLs too.

python3 -I tools/native/caller_scan.py <SLUS_215.03> <repo> [--rels <dir>] <addr[:returns]> ...
returns: v (v0), f (f0), n (none)
Scratch = at, a0-a3, t0-t9, v0/v1 (unless returned), hi/lo, f1-f19 (f0 unless returned), vf1-vf31, ACC, Q, vi1-vi15.
Bounded walk of the caller's control flow (both branch ways) from the return point, 96 instructions a path.
--rels <dir>: also every DLL in <dir> (God Hand Extract\\dll\\debugoptimize: 79 rooms, event, mcport), each linked at
its slot base by tools/snr2 (rooms 0x799080, event 0x7A8080, mcport 0x7C0080; raw .rel immediates are 0 + relocations,
so only linked images are scanned). For the ELF and every DLL it also counts code pointers to the functions (lui +
addiu / ori pairs, data words outside the ELF's SNR2 export table): a pointer means calls the scan can't see (jalr).
--table <addr>:<returns>: functions called through a jump table (rung 4 M3: the pose's interpolators at 0x3BD910): every
lui + addiu / ori pair forming the table's address marks its function, and every jalr in that function is walked like
a jal site (a function may keep the base in a saved register: the pose's s7); data words holding the table's address
are listed (calls the scan can't follow).
"""
import csv, os, re, sys, bisect
import rabbitizer as rz
from elftools.elf.elffile import ELFFile

args = sys.argv[1:]
rels = None
if "--rels" in args:
    k = args.index("--rels")
    rels = args[k + 1]
    del args[k:k + 2]
table = None
if "--table" in args:
    k = args.index("--table")
    ta, _, tret = args[k + 1].partition(":")
    table = (int(ta, 16), tret or "n")
    del args[k:k + 2]
elf, repo = args[:2]
funcs = {}
with open(f"{repo}/recomp/kamite_functions.csv") as f:
    for r in csv.DictReader(f):
        funcs[int(r["Start"], 16)] = (r["Name"], int(r["End"], 16))
starts = sorted(funcs)
with open(elf, "rb") as f:
    segs = [(s["p_vaddr"], s.data()) for s in ELFFile(f).iter_segments() if s["p_type"] == "PT_LOAD"]
elf_segs = list(segs)


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


def transfers(code, a):
    """jal and j sites to a in the [(vaddr, bytes)] ranges."""
    jal, j = [], []
    for v, d in code:
        for off in range(0, len(d) - 3, 4):
            w = int.from_bytes(d[off:off + 4], "little")
            if (w >> 26) in (2, 3) and (((v + off) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) == a:
                (jal if (w >> 26) == 3 else j).append(v + off)
    return jal, j


def pointers(code, data, a, skip=range(0)):
    """lui + addiu / ori pairs (within 16 instructions, same register) and data words equal to a."""
    hi_add, hi_or, lo = ((a + 0x8000) >> 16) & 0xFFFF, a >> 16, a & 0xFFFF
    out = []
    for v, d in code:
        for off in range(0, len(d) - 3, 4):
            w = int.from_bytes(d[off:off + 4], "little")
            if (w >> 26) != 0x0F or (w & 0xFFFF) not in (hi_add, hi_or):
                continue
            rt = (w >> 16) & 31
            for k in range(1, 17):
                o2 = off + 4 * k
                if o2 + 4 > len(d):
                    break
                w2 = int.from_bytes(d[o2:o2 + 4], "little")
                if (w2 >> 21) & 31 == rt and (w2 & 0xFFFF) == lo and (
                        ((w2 >> 26) == 0x09 and (w & 0xFFFF) == hi_add) or ((w2 >> 26) == 0x0D and (w & 0xFFFF) == hi_or)):
                    out.append(v + off)
                    break
    pat = a.to_bytes(4, "little")
    for v, d in data:
        i = d.find(pat)
        while i >= 0:
            if i % 4 == 0 and v + i not in skip:
                out.append(v + i)
            i = d.find(pat, i + 1)
    return out


def scan(label, code, data, owner, a, ret, skip=range(0)):
    """(jal sites, j sites, sites read after, pointer refs); prints the flagged sites."""
    jal, j = transfers(code, a)
    bad = []
    for s in jal:
        name, end = owner(s)
        flags = set()
        walk(s + 8, end, ret, set(), 96, set(), flags)
        if flags:
            bad.append((s, name, sorted(flags)[:3]))
    ptr = pointers(code, data, a, skip)
    for s, n, fl in bad[:8]:
        print(f"   {label} {s:X} in {n}: " + "; ".join(f"{p:X} {r} [{t}]" for p, r, t in fl))
    for s in j[:4]:
        print(f"   {label} {s:X} in {owner(s)[0]}: j (tail call: its caller's callers see the state)")
    for s in ptr[:4]:
        print(f"   {label} {s:X}: code pointer")
    return len(jal), len(j), len(bad), len(ptr)


def scan_table(label, code, data, owner, a, ret, skip=range(0)):
    """(jalr sites, sites read after, data refs) for the jump table at a; prints the flagged sites."""
    refs = pointers(code, data, a, skip)
    code_refs = [r for r in refs if any(v <= r < v + len(d) for v, d in code)]
    data_refs = [r for r in refs if r not in code_refs]
    sites, bad, done = [], [], set()
    for r in code_refs:
        name, end = owner(r)
        if name in done:
            continue
        done.add(name)
        # every jalr of a function that loads the table (it may keep the base in a saved register: the pose's s7)
        start = r
        while start - 4 >= 0 and owner(start - 4)[0] == name and word(start - 4) is not None:
            start -= 4
        for pc in range(start, end, 4):
            if word(pc) is None:
                break
            i = rz.Instruction(word(pc), pc, category=rz.InstrCategory.R5900)
            if i.getOpcodeName() == "jalr":
                sites.append(pc)
                flags = set()
                walk(pc + 8, end, ret, set(), 96, set(), flags)
                if flags:
                    bad.append((pc, name, sorted(flags)[:3]))
    for s, n, fl in bad[:8]:
        print(f"   {label} {s:X} in {n}: " + "; ".join(f"{p:X} {r} [{t}]" for p, r, t in fl))
    for s in data_refs[:4]:
        print(f"   {label} {s:X}: the table's address in data")
    return len(done), len(sites), len(bad), len(data_refs)


def elf_owner(s):
    k = bisect.bisect_right(starts, s) - 1
    return funcs[starts[k]][0], funcs[starts[k]][1]


targets = []
for arg in args[2:]:
    a, _, ret = arg.partition(":")
    targets.append((int(a, 16), ret or "n"))

# the ELF's SNR2 export table ({name, address, hash, type, flag} x count at .sndata 0x40A400's +0x0C / +0x10): its
# address words are the room DLLs' import resolution, not code pointers
exp = range(0)
for v, d in elf_segs:
    if v <= 0x40A400 < v + len(d) - 20:
        fa = int.from_bytes(d[0x40A400 - v + 0x0C:0x40A400 - v + 0x10], "little")
        fc = int.from_bytes(d[0x40A400 - v + 0x10:0x40A400 - v + 0x14], "little")
        exp = range(fa, fa + 12 * fc)
for a, ret in targets:
    n_jal, n_j, n_bad, n_ptr = scan("ELF", elf_segs, elf_segs, elf_owner, a, ret, exp)
    print(f"{a:06X} {funcs[a][0]}: {n_jal} call sites, {n_bad} read a scratch register after the call"
          f"{f', {n_j} j' if n_j else ''}{f', {n_ptr} code pointers' if n_ptr else ''}")
if table:
    n_ref, n_sites, n_bad, n_data = scan_table("ELF", elf_segs, elf_segs, elf_owner, table[0], table[1], exp)
    print(f"table {table[0]:06X}: {n_ref} functions load it, {n_sites} jalr sites, {n_bad} read a scratch register after the "
          f"call{f', {n_data} data words' if n_data else ''}")

if rels:
    sys.path.insert(0, os.path.join(repo, "tools", "snr2"))
    import snr2  # noqa: E402
    from snr2fmt import Module  # noqa: E402
    from snr2link import Registry  # noqa: E402
    with open(elf, "rb") as f:
        reg = Registry.from_boot_elf(f.read())
    total = {a: [0, 0, 0, 0, []] for a, _ in targets}
    table_total = [0, 0, 0, 0, []]
    names = sorted(n for n in os.listdir(rels) if n.lower().endswith(".rel"))
    for nm in names:
        base = {"event.rel": 0x7A8080, "mcport.rel": 0x7C0080}.get(nm.lower(), 0x799080)
        with open(os.path.join(rels, nm), "rb") as f:
            an = snr2.Analysis(Module(f.read(), nm), base, reg)
        img = bytes(an.L.img)
        code = [(base + an.cs, img[an.cs:an.ce])]
        fstarts = [fn["start"] for fn in an.functions]

        def owner(s, fl=an.functions, fs=fstarts):
            i = bisect.bisect_right(fs, s) - 1
            return fl[i]["name"], fl[i]["end"]

        segs = elf_segs + [(base, img)]
        h = an.m.h  # the DLL's own symbol table holds its resolved imports: not pointers
        own = range(base + h["funcs_off"], base + h["funcs_off"] + 12 * h["func_count"])
        for a, ret in targets:
            r = scan(nm, code, [(base, img)], owner, a, ret, own)
            for k in range(4):
                total[a][k] += r[k]
            if r[0] or r[1] or r[3]:
                total[a][4].append(nm[:-4])
        if table:
            r = scan_table(nm, code, [(base, img)], owner, table[0], table[1], own)
            for k in range(4):
                table_total[k] += r[k]
            if any(r):
                table_total[4].append(nm[:-4])
    segs = elf_segs
    print(f"DLLs: {len(names)} linked")
    for a, ret in targets:
        n_jal, n_j, n_bad, n_ptr, mods = total[a]
        print(f"{a:06X} {funcs[a][0]}: {n_jal} DLL call sites, {n_bad} read a scratch register after the call"
              f"{f', {n_j} j' if n_j else ''}{f', {n_ptr} code pointers' if n_ptr else ''}"
              f"{' (' + ' '.join(mods) + ')' if mods else ''}")
    if table:
        n_ref, n_sites, n_bad, n_data, mods = table_total
        print(f"table {table[0]:06X}: {n_ref} DLL functions load it, {n_sites} jalr sites, {n_bad} read a scratch register "
              f"after the call{f', {n_data} data words' if n_data else ''}{' (' + ' '.join(mods) + ')' if mods else ''}")
