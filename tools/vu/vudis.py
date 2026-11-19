"""VU microcode disassembler (VU1 micro memory images vu1_<hash>.bin, or any upload), for reading the game's programs.

    python -I tools/vu/vudis.py <image> [start hex] [end hex]     byte addresses, default the whole image

One line per instruction pair: byte address, upper (with its I/E/M/D/T bits), lower. With the I bit set the lower word
is the I register's float. Encodings as the runtime's interpreter decodes them (ps2_vu1_impl.inl execUpper/execLower).
Standard library only.
"""
import struct
import sys

BC = "xyzw"


def dest(i):
    d = (i >> 21) & 0xF
    return "".join(c for c, bit in zip("xyzw", (8, 4, 2, 1)) if d & bit)


def vf(n):
    return f"vf{n:02d}"


def upper(i):
    d = dest(i)
    ft, fs, fd = (i >> 16) & 31, (i >> 11) & 31, (i >> 6) & 31
    op = i & 0x3F
    bc = BC[i & 3]
    if op < 0x1C:
        name = ["add", "sub", "madd", "msub", "max", "mini", "mul"][op >> 2]
        return f"{name}{bc}.{d} {vf(fd)}, {vf(fs)}, {vf(ft)}{bc}"
    table = {0x1C: ("mulq", "q"), 0x1D: ("maxi", "i"), 0x1E: ("muli", "i"), 0x1F: ("minii", "i"),
             0x20: ("addq", "q"), 0x21: ("maddq", "q"), 0x22: ("addi", "i"), 0x23: ("maddi", "i"),
             0x24: ("subq", "q"), 0x25: ("msubq", "q"), 0x26: ("subi", "i"), 0x27: ("msubi", "i")}
    if op in table:
        name, reg = table[op]
        return f"{name}.{d} {vf(fd)}, {vf(fs)}, {reg}"
    three = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2B: "max", 0x2C: "sub", 0x2D: "msub", 0x2E: "opmsub",
             0x2F: "mini"}
    if op in three:
        return f"{three[op]}.{d} {vf(fd)}, {vf(fs)}, {vf(ft)}"
    if op >= 0x3C:
        s = (i & 3) | ((i >> 4) & 0x7C)
        if s < 0x10:
            name = ["adda", "suba", "madda", "msuba"][s >> 2]
            return f"{name}{BC[s & 3]}.{d} acc, {vf(fs)}, {vf(ft)}{BC[s & 3]}"
        if s < 0x18:
            name = ["itof0", "itof4", "itof12", "itof15", "ftoi0", "ftoi4", "ftoi12", "ftoi15"][s - 0x10]
            return f"{name}.{d} {vf(ft)}, {vf(fs)}"
        if s < 0x1C:
            return f"mula{BC[s & 3]}.{d} acc, {vf(fs)}, {vf(ft)}{BC[s & 3]}"
        sp = {0x1C: f"mulaq.{d} acc, {vf(fs)}, q", 0x1D: f"abs.{d} {vf(ft)}, {vf(fs)}",
              0x1E: f"mulai.{d} acc, {vf(fs)}, i", 0x1F: f"clip.xyz {vf(fs)}, {vf(ft)}w",
              0x20: f"addaq.{d} acc, {vf(fs)}, q", 0x21: f"maddaq.{d} acc, {vf(fs)}, q",
              0x22: f"addai.{d} acc, {vf(fs)}, i", 0x23: f"maddai.{d} acc, {vf(fs)}, i",
              0x24: f"subaq.{d} acc, {vf(fs)}, q", 0x25: f"msubaq.{d} acc, {vf(fs)}, q",
              0x26: f"subai.{d} acc, {vf(fs)}, i", 0x27: f"msubai.{d} acc, {vf(fs)}, i",
              0x28: f"adda.{d} acc, {vf(fs)}, {vf(ft)}", 0x29: f"madda.{d} acc, {vf(fs)}, {vf(ft)}",
              0x2A: f"mula.{d} acc, {vf(fs)}, {vf(ft)}", 0x2C: f"suba.{d} acc, {vf(fs)}, {vf(ft)}",
              0x2D: f"msuba.{d} acc, {vf(fs)}, {vf(ft)}", 0x2E: f"opmula.{d} acc, {vf(fs)}, {vf(ft)}",
              0x2F: "nop", 0x30: "nop"}
        return sp.get(s, f"?upper {i:08x}")
    return f"?upper {i:08x}"


def lower(i, pc):
    if i in (0, 0x8000033C):
        return "nop"
    op = (i >> 25) & 0x7F
    d = dest(i)
    it, is_, id_ = (i >> 16) & 15, (i >> 11) & 15, (i >> 6) & 15
    ft, fs = (i >> 16) & 31, (i >> 11) & 31
    imm11 = (i & 0x7FF) - (0x800 if i & 0x400 else 0)
    imm15 = (i & 0x7FF) | ((i >> 10) & 0x7800)
    target = (pc + 8 + imm11 * 8) & 0x3FFF
    if op == 0x00:
        return f"lq.{d} {vf(ft)}, {imm11}(vi{is_:02d})"
    if op == 0x01:
        return f"sq.{d} {vf(fs)}, {imm11}(vi{it:02d})"
    if op == 0x04:
        return f"ilw.{d} vi{it:02d}, {imm11}(vi{is_:02d})"
    if op == 0x05:
        return f"isw.{d} vi{it:02d}, {imm11}(vi{is_:02d})"
    if op == 0x08:
        return f"iaddiu vi{it:02d}, vi{is_:02d}, {imm15:#x}"
    if op == 0x09:
        return f"isubiu vi{it:02d}, vi{is_:02d}, {imm15:#x}"
    flags = {0x10: "fceq vi01", 0x11: "fcset", 0x12: "fcand vi01", 0x13: "fcor vi01", 0x14: f"fseq vi{it:02d}",
             0x15: "fsset", 0x16: f"fsand vi{it:02d}", 0x17: f"fsor vi{it:02d}"}
    if op in flags:
        imm = i & 0xFFFFFF if op < 0x14 else ((i >> 10) & 0x800) | (i & 0x7FF)
        return f"{flags[op]}, {imm:#x}"
    if op == 0x18:
        return f"fmeq vi{it:02d}, vi{is_:02d}"
    if op == 0x1A:
        return f"fmand vi{it:02d}, vi{is_:02d}"
    if op == 0x1B:
        return f"fmor vi{it:02d}, vi{is_:02d}"
    if op == 0x1C:
        return f"fcget vi{it:02d}"
    if op == 0x20:
        return f"b {target:#06x}"
    if op == 0x21:
        return f"bal vi{it:02d}, {target:#06x}"
    if op == 0x24:
        return f"jr vi{is_:02d}"
    if op == 0x25:
        return f"jalr vi{it:02d}, vi{is_:02d}"
    br = {0x28: "ibeq", 0x29: "ibne"}
    if op in br:
        return f"{br[op]} vi{it:02d}, vi{is_:02d}, {target:#06x}"
    br1 = {0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez"}
    if op in br1:
        return f"{br1[op]} vi{is_:02d}, {target:#06x}"
    if op == 0x40:
        f = i & 0x3F
        if f == 0x30:
            return f"iadd vi{id_:02d}, vi{is_:02d}, vi{it:02d}"
        if f == 0x31:
            return f"isub vi{id_:02d}, vi{is_:02d}, vi{it:02d}"
        if f == 0x32:
            imm5 = ((i >> 6) & 31) - (32 if i & (1 << 10) else 0)
            return f"iaddi vi{it:02d}, vi{is_:02d}, {imm5}"
        if f == 0x34:
            return f"iand vi{id_:02d}, vi{is_:02d}, vi{it:02d}"
        if f == 0x35:
            return f"ior vi{id_:02d}, vi{is_:02d}, vi{it:02d}"
        if f >= 0x3C:
            s = (i & 3) | ((i >> 4) & 0x7C)
            fsf, ftf = BC[(i >> 21) & 3], BC[(i >> 23) & 3]
            sp = {0x30: f"move.{d} {vf(ft)}, {vf(fs)}", 0x31: f"mr32.{d} {vf(ft)}, {vf(fs)}",
                  0x34: f"lqi.{d} {vf(ft)}, (vi{is_:02d}++)", 0x35: f"sqi.{d} {vf(fs)}, (vi{it:02d}++)",
                  0x36: f"lqd.{d} {vf(ft)}, (--vi{is_:02d})", 0x37: f"sqd.{d} {vf(fs)}, (--vi{it:02d})",
                  0x38: f"div q, {vf(fs)}{fsf}, {vf(ft)}{ftf}", 0x39: f"sqrt q, {vf(ft)}{ftf}",
                  0x3A: f"rsqrt q, {vf(fs)}{fsf}, {vf(ft)}{ftf}", 0x3B: "waitq",
                  0x3C: f"mtir vi{it:02d}, {vf(fs)}{fsf}", 0x3D: f"mfir.{d} {vf(ft)}, vi{is_:02d}",
                  0x3E: f"ilwr.{d} vi{it:02d}, (vi{is_:02d})", 0x3F: f"iswr.{d} vi{it:02d}, (vi{is_:02d})",
                  0x40: f"rnext.{d} {vf(ft)}, r", 0x41: f"rget.{d} {vf(ft)}, r", 0x42: f"rinit r, {vf(fs)}{fsf}",
                  0x43: f"rxor r, {vf(fs)}{fsf}", 0x64: f"mfp.{d} {vf(ft)}, p", 0x68: f"xtop vi{it:02d}",
                  0x69: f"xitop vi{it:02d}", 0x6C: f"xgkick vi{is_:02d}", 0x70: f"esadd p, {vf(fs)}",
                  0x71: f"ersadd p, {vf(fs)}", 0x72: f"eleng p, {vf(fs)}", 0x73: f"erleng p, {vf(fs)}",
                  0x74: f"eatanxy p, {vf(fs)}", 0x75: f"eatanxz p, {vf(fs)}", 0x76: f"esum p, {vf(fs)}",
                  0x77: f"ersqrt p, {vf(fs)}{fsf}", 0x78: f"esqrt p, {vf(fs)}{fsf}", 0x79: f"esin p, {vf(fs)}{fsf}",
                  0x7A: f"ercpr p, {vf(fs)}{fsf}", 0x7B: "waitp", 0x7C: f"eatan p, {vf(fs)}{fsf}",
                  0x7D: f"eexp p, {vf(fs)}{fsf}"}
            return sp.get(s, f"?lower {i:08x}")
    return f"?lower {i:08x}"


def main():
    data = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else len(data)
    for pc in range(start, min(end, len(data)), 8):
        lo, up = struct.unpack_from("<II", data, pc)
        bits = "".join(c for c, b in zip("IEMDT", (31, 30, 29, 28, 27)) if up >> b & 1)
        u = upper(up & 0x07FFFFFF)
        if up >> 31 & 1:
            l = f"loi {struct.unpack('<f', struct.pack('<I', lo))[0]!r}"
        else:
            l = lower(lo, pc)
        print(f"{pc:04x}: {bits:<3} {u:<38} {l}")


if __name__ == "__main__":
    main()
