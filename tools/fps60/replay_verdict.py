#!/usr/bin/env python3
"""One verdict line per case for a replay batch: the file a chat reads instead of fingerprints / snapdiffs / chains.

    python tools/fps60/replay_verdict.py              (in the play folder; Replay-Cases.cmd writes cases/verdict.txt)
    python tools/fps60/replay_verdict.py --promote    (make this build the reference: cases/ref/)

Against the reference (cases/ref/: a build's chain digests, both runs, and its fingerprint lines; set by --promote once
a build is accepted; without it the previous build's .prev files and fingerprint lines stand in):
  ram / csv   this build's last run = the reference's (fingerprints.txt hashes); csv "~": differs, but so do this
              build's own two runs (the host-timing columns), so it says nothing
  chain       the DMA chain digest (tools/dma/chain_compare.py): "=" or how many chains differ from which test frame on.
              A chain counts only where run 1 = run 2 in both builds (single chains differ between two runs of one
              build: host-timing noise)
  runs        this build's run 1 = run 2 (ram, csv, chain); only with RUNS=2
  snap        snap_compare.py's verdict against the previous build (fingerprints.txt), when it ran
Then the natives' shadow totals (all *.native.txt) and the commit range. Anything that moves: list every commit in the
range (render, HUD and tools commits too, not only natives) before blaming one (Nov 23: a 16:9 HUD commit moved 8 cases'
chains and was blamed on func_00154240 for two builds).
"""
import glob
import os
import shutil
import struct
import sys

CASES = "cases"
REF = os.path.join(CASES, "ref")


def chain_records(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    if data[:8] != b"GHRCHN1\0":
        return None
    return [struct.unpack_from("<4I", data, off) for off in range(8, len(data) - 15, 16)]


def chain_verdict(cur, cur1, ref, ref1):
    a, b = chain_records(ref), chain_records(cur)
    if a is None or b is None:
        return "chain -"
    a1, b1 = chain_records(ref1) or a, chain_records(cur1) or b
    if len(a) != len(b):
        return f"chain !={len(a)}->{len(b)} chains"
    diff = [b[i][0] for i in range(len(a)) if a[i] != b[i]
            and (i >= len(a1) or a1[i] == a[i]) and (i >= len(b1) or b1[i] == b[i])]
    if not diff:
        return "chain ="
    return f"chain != {len(diff)} from frame {diff[0]} to {diff[-1]}"


def fingerprints(path):
    """{build: {(case, run): {field: hash}}, ...} in file order, plus snap verdicts per build."""
    builds, verdicts, order, cur = {}, {}, [], None
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return builds, verdicts, order
    for line in lines:
        parts = line.split(";")
        if len(parts) > 3 and parts[1].startswith("run "):
            cur = parts[0]
            if cur not in builds:
                builds[cur], verdicts[cur] = {}, {}
                order.append(cur)
            fields = dict(p.split("=", 1) for p in parts[3:] if "=" in p)
            builds[cur][(parts[2], parts[1][4:])] = fields
        elif cur and ".case " in line and ": " in line:
            name, text = line.split(": ", 1)
            verdicts[cur][name.strip()] = text
    return builds, verdicts, order


def last_run(build, case):
    runs = sorted(r for (c, r) in build if c == case)
    return build.get((case, runs[-1])) if runs else None


def main():
    promote = "--promote" in sys.argv
    cases = sorted(os.path.basename(p) for p in glob.glob(os.path.join(CASES, "*.case")))
    builds, verdicts, order = fingerprints(os.path.join(CASES, "fingerprints.txt"))
    if not order:
        sys.exit("no fingerprints yet")
    now = order[-1]
    if promote:
        os.makedirs(REF, exist_ok=True)
        for p in glob.glob(os.path.join(REF, "*")):
            os.remove(p)
        for p in glob.glob(os.path.join(CASES, "*.chain.bin")) + glob.glob(os.path.join(CASES, "*.chain.run1.bin")):
            shutil.copy2(p, REF)
        with open(os.path.join(CASES, "fingerprints.txt"), encoding="utf-8", errors="replace") as f, \
                open(os.path.join(REF, "ref-fingerprints.txt"), "w", encoding="utf-8") as out:
            out.writelines(line for line in f if line.startswith(now + ";"))
        print(f"reference = {now}")
        return
    ref_fp = os.path.join(REF, "ref-fingerprints.txt")
    if os.path.exists(ref_fp):
        rb, _, ro = fingerprints(ref_fp)
        ref_name, ref_build = ro[0], rb[ro[0]]
    else:
        ref_name = order[-2] if len(order) > 1 else None
        ref_build = builds.get(ref_name, {})
    # the batch's own shadow files: a case no longer in cases/ leaves its last ones behind (Nov 23: 2026-11-03_14-53-38's
    # from an older build showed up in the totals and the commit list)
    natives = [p for p in glob.glob(os.path.join(CASES, "*.native.txt"))
               if os.path.basename(p).rsplit(".", 3)[0] in cases]
    commits = set()
    for p in natives:
        head = open(p, encoding="utf-8", errors="replace").readline()
        if "commit=" in head:
            commits.add(head.split("commit=")[1].split()[0])
    print(f"build {now}  commit {', '.join(sorted(commits)) or '?'}")
    print(f"reference {ref_name or '-'} ({'cases/ref' if os.path.exists(ref_fp) else 'the previous build'})")
    print("list every commit between the reference's and this one before attributing anything that moved")
    print()
    moved = 0
    for case in cases:
        cur = last_run(builds[now], case)
        ref = last_run(ref_build, case) if ref_build else None
        run1 = builds[now].get((case, "1"))
        for rate in ("30", "60"):
            out = []
            for field in ("ram.bin", "csv"):
                k = f"{rate}.{field}"
                if cur and ref and k in cur and k in ref:
                    noisy = field == "csv" and run1 is not None and run1 is not cur and run1.get(k) != cur.get(k)
                    out.append(f"{field.split('.')[0]} " + ("~" if noisy and cur[k] != ref[k] else
                                                           "=" if cur[k] == ref[k] else "!="))
            base = os.path.join(CASES, f"{case}.{rate}")
            if os.path.exists(os.path.join(REF, f"{case}.{rate}.chain.bin")):
                rbase = os.path.join(REF, f"{case}.{rate}")
                cv = chain_verdict(base + ".chain.bin", base + ".chain.run1.bin", rbase + ".chain.bin",
                                   rbase + ".chain.run1.bin")
            else:
                cv = chain_verdict(base + ".chain.bin", base + ".chain.run1.bin", base + ".chain.prev.bin", "")
            out.append(cv)
            if run1 and cur and run1 is not cur:
                same = [f for f in ("ram.bin", "csv", "chain.bin")
                        if f"{rate}.{f}" in run1 and run1.get(f"{rate}.{f}") == cur.get(f"{rate}.{f}")]
                out.append("runs " + ("=" if len(same) == 3 else "differ: " + ",".join(
                    f for f in ("ram.bin", "csv", "chain.bin") if f not in same)))
            v = verdicts[now].get(f"{case} {rate}")
            if v:
                out.append("snap " + ("=" if v.startswith("same") else v.split("(")[0].replace("DIFFERS from the previous build in ", "!= ").strip()))
            line = "  ".join(out)
            if any(o.startswith(("ram !=", "csv !=", "chain !=")) for o in out):
                moved += 1
            print(f"{case[:48]:48} {rate}  {line}")
    print()
    totals = {}
    for p in natives:
        for line in open(p, encoding="utf-8", errors="replace"):
            if line.startswith("#"):
                continue
            f = line.split()
            if len(f) < 9:
                continue
            t = totals.setdefault(f[0], [0] * 6)
            for i, col in enumerate((2, 3, 4, 5, 6, 8)):
                t[i] += int(f[col])
    print("shadowed natives: calls checked matched declined skipped mismatches")
    for name, t in sorted(totals.items()):
        print(f"  {name:28} {t[0]:>9} {t[1]:>9} {t[2]:>9} {t[3]:>6} {t[4]:>6} {t[5]:>5}{'  <-- MISMATCHES' if t[5] else ''}")
    print()
    print(f"{moved} case/rate lines with ram, csv or chain != the reference")
    print("(csv ~ = differs but this build's own two runs differ too: host-timing columns; a few snap words per case")
    print(" and single noisy chains are usual; a block of ~1000 words or chains from a frame on is not)")


if __name__ == "__main__":
    main()
