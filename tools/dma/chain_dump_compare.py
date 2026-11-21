#!/usr/bin/env python3
"""Compare two early-frame chain dumps (<case>.<rate>.chain.dump, written by dev builds with KAMITE_CHAIN_DUMP=<frame>:
src/kamite/native/chain_digest.cpp; dev probe, claude/strip-list.md). Each chain is listed as its source pieces (RAM
offset, bytes, hash; the bytes themselves for pieces of at most 4096 bytes), so a difference is pinned to where in RAM
it came from: big pieces are the models' static data (as loaded from disc), small ones the packets the EE builds each
frame (tags, matrices, GS registers).

    python3 tools/dma/chain_dump_compare.py <a.chain.dump> <b.chain.dump> [--max 20]
"""
import struct
import sys

CHANNELS = {0x10008000: "VIF0", 0x10009000: "VIF1", 0x1000A000: "GIF"}


def read(path):
    data = open(path, "rb").read()
    if data[:8] != b"GHRCDMP1":
        sys.exit(f"{path}: not a chain dump")
    chains, at = [], 8
    while at + 12 <= len(data):
        frame, channel, count = struct.unpack_from("<3I", data, at)
        at += 12
        pieces = []
        for _ in range(count):
            offset, size, lo, hi = struct.unpack_from("<4I", data, at)
            at += 16
            body = None
            if size <= 4096:
                body = data[at:at + size]
                at += size
            pieces.append((offset, size, lo | hi << 32, body))
        chains.append((frame, channel, pieces))
    return chains


def words(a, b):
    out = []
    for i in range(0, min(len(a), len(b)) - 3, 4):
        x, y = a[i:i + 4], b[i:i + 4]
        if x != y:
            out.append((i, struct.unpack("<I", x)[0], struct.unpack("<I", y)[0]))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    limit = int(argv[argv.index("--max") + 1]) if "--max" in argv else 20
    a, b = read(argv[0]), read(argv[1])
    print(f"{argv[0]}: {len(a)} chains; {argv[1]}: {len(b)} chains")
    shown = 0
    big = small = 0
    for n, (ca, cb) in enumerate(zip(a, b)):
        fa, cha, pa = ca
        fb, chb, pb = cb
        if (fa, cha) != (fb, chb) or len(pa) != len(pb):
            print(f"chain {n}: frame {fa} / {fb}, {CHANNELS.get(cha, hex(cha))} / {CHANNELS.get(chb, hex(chb))}, "
                  f"{len(pa)} / {len(pb)} pieces: the chains' shape differs")
            shown += 1
            continue
        for i, (x, y) in enumerate(zip(pa, pb)):
            if x[:3] == y[:3]:
                continue
            kind = "small" if x[1] <= 4096 else "big"
            big += kind == "big"
            small += kind == "small"
            if shown >= limit:
                continue
            shown += 1
            line = (f"frame {fa} {CHANNELS.get(cha, hex(cha))} piece {i}: RAM {x[0]:08X} / {y[0]:08X}, "
                    f"{x[1]} / {y[1]} bytes ({kind})")
            if x[3] is not None and y[3] is not None and x[1] == y[1]:
                diffs = words(x[3], y[3])
                line += f", {len(diffs)} words differ: " + ", ".join(
                    f"+{o:X} ({x[0] + o:08X}) {u:08X}/{v:08X}" for o, u, v in diffs[:6])
            print(line)
    print(f"differing pieces: {big} big (static model data), {small} small (EE-built packets)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
