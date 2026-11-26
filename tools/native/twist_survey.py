#!/usr/bin/env python3
"""Which models carry func_001501D0's twist (cParts +0x154 & 0x800), by model kind, in replays' RAM snapshots
(rung 4, Nov 26; claude/rung4-motion-map.md "The twist"). Needs numpy; Claude runs it on K's PC (device_bash), in the
cases folder:

    python3 twist_survey.py "<case>.60.snap.bin" [...]   (each case's <case>.<rate>.csv beside it)

A model: +0xF0 = itself + 0xB0, a 16-aligned part list (+0x144) of partNum (+0x2B4) <= 80 parts, each part's anchor
+0xD0 = part + 0x30 and its parent chain (+0x148) reaching the model. Kind = +0x564 (the csv's em*_kind); "em" = in the
csv's enemy list at that frame. Every 8th snapshot (one a second of game time)."""
import csv, sys, bisect
import numpy as np
WORDS, MASK = 0x2000000 // 4, 0x1FFFFFF
def snapshots(path, every):
    data = np.fromfile(path, dtype=np.uint32)
    ram = np.zeros(WORDS, dtype=np.uint32)
    at, n = 1, 0
    while at + 2 <= data.size:
        frame, count = int(data[at]), int(data[at + 1]); at += 2
        pairs = data[at:at + 2 * count].reshape(-1, 2); at += 2 * count
        ram[pairs[:, 0]] = pairs[:, 1]
        if n % every == 0:
            yield frame, ram
        n += 1
def w(ram, a): return int(ram[(a & MASK) // 4])
def main():
    every = 8
    addr = np.arange(0, 0x2000000 - 0x600, 16, dtype=np.uint32)
    for snap in sys.argv[1:]:
        rows = {}
        for r in csv.DictReader(open(snap.replace(".snap.bin", ".csv"), newline="")):
            try: rows[int(r["frame"])] = r
            except (KeyError, ValueError): pass
        frames = sorted(rows)
        found, snaps, emseen = {}, 0, {}
        for frame, ram in snapshots(snap, every):
            snaps += 1
            row = rows[frames[max(0, bisect.bisect_right(frames, frame) - 1)]] if frames else {}
            ems = set()
            for k in range(16):
                a = row.get(f"em{k}_addr") or ""
                try: ems.add(int(a, 16) & MASK); emseen[row.get(f"em{k}_kind")] = 1
                except ValueError: pass
            pos = ram[(addr + 0xF0) // 4] & MASK
            first = ram[(addr + 0x144) // 4] & MASK
            cand = addr[(pos == addr + 0xB0) & (first != 0) & ((first & 0xF) == 0) & (first < 0x2000000 - 0x160)]
            for m in cand.tolist():
                n = w(ram, m + 0x2B4) & 0xFF
                if n == 0 or n > 80: continue
                p, k, ok, tw, total = w(ram, m + 0x144), 0, True, [], 0
                while k < n:
                    p &= MASK
                    if p == 0 or (w(ram, p + 0xD0) & MASK) != p + 0x30: ok = False; break
                    q, d = w(ram, p + 0x148) & MASK, 0
                    while q != m and d < 64:
                        if (w(ram, q + 0xD0) & MASK) != q + 0x30: break
                        q, d = w(ram, q + 0x148) & MASK, d + 1
                    if q != m: ok = False; break
                    if w(ram, p + 0x154) & 0x800: tw.append(k)
                    p, k = w(ram, p + 0x144), k + 1
                if not ok or not tw: continue
                key = (hex(w(ram, m + 0x564)), m in ems, n, tuple(tw))
                found[key] = found.get(key, 0) + 1
        name = snap.split("/")[-1][:34]
        print(f"{name}: {snaps} snaps, csv kinds {sorted(k for k in emseen if k)}")
        for (kind, isem, n, tw), c in sorted(found.items()):
            print(f"   kind {kind} {'em' if isem else 'not in csv'} parts {n}: twist {list(tw)} in {c} snaps")
main()
