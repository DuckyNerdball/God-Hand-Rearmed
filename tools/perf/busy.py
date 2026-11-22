#!/usr/bin/env python3
"""Game thread busy % of dev replays, per build window (claude/native-next.md "Busy % (how it is measured)").

    python3 -I tools/perf/busy.py <godhand.log> HH:MM:SS-HH:MM:SS [HH:MM:SS-HH:MM:SS]
    python3 -I tools/perf/busy.py --syms <godhand.log> HH:MM:SS-HH:MM:SS HH:MM:SS-HH:MM:SS [--top 40]

A window selects the replay processes whose "===== GHR.exe run <date> <time>" header falls inside it (one build's
Replay-Cases run). Per replay: the mean over its pc-sample reports of GameThread busy minus its XXH3 share (the chain
digest's hashing, dev replays only); then the mean over replays, and per rate. With two windows: the means over the
case / rate pairs both have, and each pair side by side. --syms: the "EE host thread" top-30 symbols averaged over all
reports of each window, largest differences first (tells a change in our code from a machine-wide shift).
"""
import collections
import re
import statistics as st
import sys

RUN = re.compile(r"===== GHR\.exe run \S+ (\S+) \(pid \d+\)")
CASE = re.compile(r"test: replaying .*[\\/]cases[\\/](.*?\.case):.*\.case\.(\d+)\.csv")
THREAD = re.compile(r'  thread \d+ "(.*?)": busy ([\d.]+)%')
SYM = re.compile(r"\s+([\d.]+)%\s+(\S.*)")


def windows(args):
    return [tuple(a.split("-")) for a in args]


def busy(log, wins):
    runs, cur, thr = [], None, None
    for line in open(log, encoding="utf-8", errors="replace"):
        m = RUN.match(line)
        if m:
            cur = {"t": m.group(1), "case": None, "w": []}
            runs.append(cur)
            thr = None
            continue
        if cur is None:
            continue
        m = CASE.search(line)
        if m:
            cur["case"] = (m.group(1), m.group(2))
            continue
        m = THREAD.match(line)
        if m:
            thr = m.group(1)
            if thr == "GameThread":
                cur["w"].append([float(m.group(2)), 0.0])
            continue
        if thr == "GameThread":
            m = re.match(r"\s+([\d.]+)%\s+GHR\.exe!XXH3", line)
            if m:
                cur["w"][-1][1] += float(m.group(1))
            elif not line.startswith("      "):
                thr = None
    out = []
    for lo, hi in wins:
        per = {}
        for r in runs:
            if lo <= r["t"] <= hi and r["case"] and r["w"]:
                per.setdefault(r["case"], []).append(st.mean(b - x for b, x in r["w"]))
        vals = [v for vs in per.values() for v in vs]
        rate = {k: [v for c, vs in per.items() if c[1] == k for v in vs] for k in ("30", "60")}
        print(f"{lo}-{hi}: {len(vals)} replays, {len(per)} case/rate; net busy {st.mean(vals):.2f}% "
              f"(30 FPS {st.mean(rate['30']):.2f}, 60 FPS {st.mean(rate['60']):.2f})")
        out.append(per)
    if len(out) == 2:
        a, b = out
        keys = sorted(set(a) & set(b))
        print(f"shared {len(keys)} case/rate: {st.mean(v for k in keys for v in a[k]):.2f}% -> "
              f"{st.mean(v for k in keys for v in b[k]):.2f}%")
        for k in keys:
            print(f"  {st.mean(a[k]):6.2f} {st.mean(b[k]):6.2f}  {k[1]} {k[0][:60]}")


def syms(log, wins, top):
    acc = [collections.Counter() for _ in wins]
    n = [0] * len(wins)
    k, inb = None, False
    for line in open(log, encoding="utf-8", errors="replace"):
        m = RUN.match(line)
        if m:
            k = next((i for i, (lo, hi) in enumerate(wins) if lo <= m.group(1) <= hi), None)
            inb = False
            continue
        if k is None:
            continue
        if "EE host thread," in line:
            inb = True
            n[k] += 1
            continue
        if inb:
            m = SYM.match(line)
            if m and not line.startswith("["):
                acc[k][m.group(2).strip()[:100]] += float(m.group(1))
            else:
                inb = False
    a = {s: v / max(n[0], 1) for s, v in acc[0].items()}
    b = {s: v / max(n[1], 1) for s, v in acc[1].items()}
    print(f"reports: {n[0]} vs {n[1]}")
    for s in sorted(set(a) | set(b), key=lambda s: -abs(b.get(s, 0) - a.get(s, 0)))[:top]:
        print(f"{a.get(s, 0):7.3f} {b.get(s, 0):7.3f} {b.get(s, 0) - a.get(s, 0):+7.3f}  {s}")


def main():
    args = sys.argv[1:]
    top = 40
    if "--top" in args:
        i = args.index("--top")
        top = int(args[i + 1])
        del args[i:i + 2]
    if args and args[0] == "--syms":
        if len(args) != 4:
            sys.exit(__doc__)
        syms(args[1], windows(args[2:]), top)
    elif 2 <= len(args) <= 3:
        busy(args[0], windows(args[1:]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
