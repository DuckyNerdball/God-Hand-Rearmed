#!/usr/bin/env python3
"""A case's whole-RAM snapshots, this build against the previous one (Replay-Cases.cmd; claude/limit-break-handoff.md).

    snap_compare.py <case>

Per rate (30, 60): <case>.<rate>.snap.prev.bin (the previous build's replay), .snap.run1.bin and .snap.bin (this
build's two replays). Words that differ between this build's two runs change on their own (timers, audio) and are
left out; any other word that differs from the previous build is a real change. One line per rate on stdout, the
address ranges in <case>.<rate>.snapdiff.txt. Standard library only (no numpy on the testers' PCs).
"""
import os
import struct
import sys
from array import array

WORDS = 0x2000000 // 4
LOW = 0x100000 // 4
CHUNK = 1 << 16


class Snaps:
    """A KSNP file (test_mode.cpp writeSnapshot) replayed one snapshot at a time into a 32 MB image."""

    def __init__(self, path):
        self.f = open(path, "rb")
        if self.f.read(4) != b"KSNP":
            raise SystemExit(f"{path}: not a snapshot file")
        self.ram = bytearray(WORDS * 4)
        self.words = memoryview(self.ram).cast("I")
        self.count = 0

    def step(self):
        head = self.f.read(8)
        if len(head) < 8:
            return False
        _, count = struct.unpack("<II", head)
        pairs = array("I")
        pairs.frombytes(self.f.read(8 * count))
        w = self.words
        for i, v in zip(pairs[0::2], pairs[1::2]):
            w[i] = v
        self.count += 1
        return True


def diff_words(a, b, out):
    if a == b:
        return
    ma, mb = memoryview(a), memoryview(b)
    for off in range(LOW * 4, WORDS * 4, CHUNK):
        if ma[off:off + CHUNK] != mb[off:off + CHUNK]:
            wa, wb = ma[off:off + CHUNK].cast("I"), mb[off:off + CHUNK].cast("I")
            base = off // 4
            for j in range(CHUNK // 4):
                if wa[j] != wb[j]:
                    out.add(base + j)


def ranges(words):
    out, start, last = [], None, None
    for w in sorted(words):
        if start is None:
            start = last = w
        elif w - last <= 4:
            last = w
        else:
            out.append((start, last))
            start = last = w
    if start is not None:
        out.append((start, last))
    return [f"0x{s * 4:07X}-0x{e * 4 + 3:07X} ({e - s + 1} words)" for s, e in out]


def compare(case, rate):
    name = os.path.basename(case)
    cur_path, prev_path, run1_path = (f"{case}.{rate}.snap.bin", f"{case}.{rate}.snap.prev.bin",
                                      f"{case}.{rate}.snap.run1.bin")
    if not os.path.exists(cur_path):
        return f"{name} {rate}: no snapshots (the replay failed?)"
    cur = Snaps(cur_path)
    prev = Snaps(prev_path) if os.path.exists(prev_path) else None
    run1 = Snaps(run1_path) if os.path.exists(run1_path) else None
    drift, changed, k = set(), {}, 0
    while True:
        more = cur.step()
        more_prev = prev.step() if prev else False
        more_run1 = run1.step() if run1 else False
        if not (more or more_prev or more_run1):
            break
        if run1 and more and more_run1:
            diff_words(run1.ram, cur.ram, drift)
        if prev and more and more_prev:
            seen = set()
            diff_words(prev.ram, cur.ram, seen)
            for w in seen:
                changed.setdefault(w, k)
        k += 1
    real = {w: s for w, s in changed.items() if w not in drift}
    counts = f"{cur.count} snapshots" + (f", previous build {prev.count}" if prev and prev.count != cur.count else "")
    with open(f"{case}.{rate}.snapdiff.txt", "w") as out:
        out.write(f"{name} {rate}: {counts}\n\nChanging on their own (run 1 vs run 2 of this build):\n")
        out.write("\n".join(ranges(drift)) + "\n\nDifferent from the previous build (outside those):\n")
        for r in ranges(real):
            out.write(r + "\n")
        if real:
            out.write("\nFirst snapshot with a difference: " + str(min(real.values())) + "\n")
    if not prev:
        return f"{name} {rate}: no previous build to compare ({len(drift)} words change on their own; {counts})"
    verdict = "same as the previous build" if not real else f"DIFFERS from the previous build in {len(real)} words"
    if prev.count != cur.count:
        verdict = "DIFFERS (snapshot count) / " + verdict
    return f"{name} {rate}: {verdict} ({len(drift)} words that change on their own left out; {counts})"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    for rate in (30, 60):
        print(compare(sys.argv[1], rate))
