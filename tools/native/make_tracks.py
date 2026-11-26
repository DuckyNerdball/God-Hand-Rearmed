#!/usr/bin/env python3
"""The sandbox's real Hermite tracks for shadow_test's KM_TRACKS (rung 4 M3; tools/native/README.md "M3 batch 1").

    python3 -I tools/native/make_tracks.py <out> <em00.dat> <pl00.dat> [...]
    python3 -I tools/native/make_tracks.py --blocks <out> <em00.dat> <pl00.dat> [...]

Every kind-1 (Hermite) record of every mtb3 block in the given files (God Hand Extract's em\\em00.dat, pl\\pl00.dat):
its track (12-byte header + 4-byte keys), deduplicated, shuffled (seed 7), up to 6,000 with at most 180 keys, written as
u32 n, u32 the motion's frames, u32 bytes, the track. Disc data: the file stays in the sandbox, never committed.
--blocks (rung 4 M4, the pose's KM_BLOCKS): whole mtb3 blocks instead, from "mtb3" to the end of their last kind-1 / 3
track, deduplicated, shuffled (seed 7), up to 2,000 of at most 64 KB (every block with a kind-3 record first), as u32
records, u32 frames, u32 bytes, the block.
"""
import random, struct, sys


def tracks(b):
    i = b.find(b"mtb3")
    while i >= 0:
        try:
            frames, records, _ = struct.unpack_from("<HBB", b, i + 4)
            for k in range(records):
                _, channel, count, _, payload = struct.unpack_from("<BBHII", b, i + 8 + 12 * k)
                if channel == 0xFF:
                    break
                if channel >> 4 == 1 and 8 + 12 * records <= payload < 0x200000 and i + payload + 12 + 4 * count <= len(b):
                    yield count, frames, b[i + payload:i + payload + 12 + 4 * count]
        except struct.error:
            pass
        i = b.find(b"mtb3", i + 4)


def blocks(b):
    i = b.find(b"mtb3")
    while i >= 0:
        try:
            frames, records, _ = struct.unpack_from("<HBB", b, i + 4)
            end, kind3 = 8 + 12 * records, False
            for k in range(records):
                _, channel, count, _, payload = struct.unpack_from("<BBHII", b, i + 8 + 12 * k)
                if channel == 0xFF:
                    break
                if channel >> 4 == 1:
                    end = max(end, payload + 12 + 4 * count)
                elif channel >> 4 == 3:
                    end, kind3 = max(end, payload + 0x18 + 8 * count), True
            if end <= 0x10000 and i + end <= len(b):
                yield records, frames, kind3, b[i:i + end]
        except struct.error:
            pass
        i = b.find(b"mtb3", i + 4)


def main_blocks(argv):
    seen, pool = set(), []
    for path in argv[2:]:
        for records, frames, kind3, blk in blocks(open(path, "rb").read()):
            if blk not in seen:
                seen.add(blk)
                pool.append((records, frames, kind3, blk))
    random.seed(7)
    random.shuffle(pool)
    pool = ([p for p in pool if p[2]] + [p for p in pool if not p[2]])[:2000]
    with open(argv[1], "wb") as f:
        for records, frames, _, blk in pool:
            f.write(struct.pack("<III", records, frames, len(blk)) + blk)
    print(f"{len(pool)} blocks to {argv[1]}")
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--blocks":
        return main_blocks(argv[1:])
    if len(argv) < 3:
        print(__doc__)
        return 1
    seen, pool = set(), []
    for path in argv[2:]:
        for n, frames, t in tracks(open(path, "rb").read()):
            if t not in seen:
                seen.add(t)
                pool.append((n, frames, t))
    random.seed(7)
    random.shuffle(pool)
    pool = [p for p in pool if p[0] <= 180][:6000]
    with open(argv[1], "wb") as f:
        for n, frames, t in pool:
            f.write(struct.pack("<III", n, frames, len(t)) + t)
    print(f"{len(pool)} tracks to {argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
