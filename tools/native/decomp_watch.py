#!/usr/bin/env python3
"""The decomp feed for the native road (rung 3+, claude/native-next.md; K's answer 1 on the method, Nov 21).

Every native records where it came from in tools/native/sources.csv: the decomp's matched C (its file, the decomp
commit it was written from, and a hash of the function's C), or "asm" (written from the disassembly because the decomp
had no C for it yet). This script compares that record with a decomp checkout and lists what the decomp's progress
offers the port:

  1. natives written from asm whose function is now matched C (rewrite from the C; the harness proves it equal),
  2. natives written from the decomp whose C changed since (a rename or a fix to read; the native stays bit-exact
     either way: the C matches the same machine code),
  3. functions newly matched in the rung-4 subsystems (motion, collision, cloth, effects; tools/perf/subsystems.py
     classifies them) against the baseline in tools/native/decomp_rung4.txt,
  4. the manifest's own health: natives registered in src/kamite/native without a sources.csv line, or lines whose
     native is gone.

A function counts as matched when the decomp has a C definition for it (the decomp carves functions out of the
monolithic disassembly only once they match; the few INCLUDE_ASM("nonmatching", ...) lines count as asm).

Usage (the weekly upstream watch, claude/upstream-watch.md):
    git clone --depth 1 --filter=blob:none --sparse https://github.com/LucasPicoli/god-hand-decomp /tmp/decomp
    git -C /tmp/decomp sparse-checkout set config include src
    python3 tools/native/decomp_watch.py /tmp/decomp                  # the report
    python3 tools/native/decomp_watch.py /tmp/decomp --update         # also move the rung-4 baseline to this commit
    python3 tools/native/decomp_watch.py /tmp/decomp --show 0x151398  # one function: its file, hash and C
    python3 tools/native/decomp_watch.py /tmp/decomp --record 0x151398 [--note "..."]
                                                                       # the sources.csv line for a native written
                                                                       # from the decomp's C (prints it)
Exit status: 0 = nothing to act on, 1 = something listed, 2 = usage / input error.
"""

import argparse
import csv
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SOURCES = os.path.join(HERE, "sources.csv")
BASELINE = os.path.join(HERE, "decomp_rung4.txt")
NATIVE_DIR = os.path.join(ROOT, "src", "kamite", "native")
FUNCTIONS_CSV = os.path.join(ROOT, "recomp", "kamite_functions.csv")
RUNG4 = ("motion", "collision", "cloth", "effects")


# ---- the decomp side --------------------------------------------------------------------------------------------

def decomp_commit(decomp):
    try:
        return subprocess.run(["git", "-C", decomp, "rev-parse", "--short=7", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def load_symbols(decomp):
    """address -> decomp name, for every type:func line of config/symbol_addrs.txt."""
    path = os.path.join(decomp, "config", "symbol_addrs.txt")
    rx = re.compile(r"^\s*([A-Za-z_]\w*)\s*=\s*0x([0-9A-Fa-f]+)\s*;.*type:func")
    by_address = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = rx.match(line)
            if m:
                by_address.setdefault(int(m.group(2), 16), m.group(1))
    return by_address


def decomp_name(symbols, address):
    """The decomp's name at an address; splat's auto-name for an address symbol_addrs.txt doesn't list yet."""
    return symbols.get(address, f"func_{address:08X}")


_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
_DEF = re.compile(
    r"^(?![ \t#])(?:[A-Za-z_][\w \t\*]*?[\s\*])?([A-Za-z_]\w*)[ \t]*\(((?:[^;{}()]|\([^()]*\))*)\)[ \t\r\n]*\{",
    re.M)
_ASM = re.compile(r'^\s*INCLUDE_ASM\(\s*"[^"]*"\s*,\s*([A-Za-z_]\w*)\s*\)', re.M)
_NOT_FUNCTIONS = {"if", "while", "for", "switch", "return", "sizeof", "do", "else"}


def _blank_comments(text):
    """Comments replaced by spaces of the same length (offsets stay valid)."""
    return _COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def _body_end(text, brace):
    """The offset just past the brace matching text[brace] ('{'), skipping strings and character constants."""
    depth, i, n = 0, brace, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            q, i = c, i + 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def index_definitions(decomp):
    """name -> (relative file, the definition's text without comments); plus the names still INCLUDE_ASM'd."""
    defs, asm = {}, set()
    src = os.path.join(decomp, "src")
    for base, _, files in os.walk(src):
        for name in sorted(files):
            if not name.endswith((".c", ".cpp")):
                continue
            path = os.path.join(base, name)
            rel = os.path.relpath(path, decomp).replace(os.sep, "/")
            with open(path, encoding="utf-8", errors="replace") as f:
                raw = f.read()
            text = _blank_comments(raw)
            for m in _ASM.finditer(text):
                asm.add(m.group(1))
            for m in _DEF.finditer(text):
                fn = m.group(1)
                if fn in _NOT_FUNCTIONS:
                    continue
                end = _body_end(text, m.end() - 1)
                defs.setdefault(fn, (rel, text[m.start():end]))
    for fn in asm:
        defs.pop(fn, None)
    return defs, asm


def definition_hash(text):
    """A short hash of a definition with comments gone and whitespace runs collapsed: formatting alone changes nothing."""
    norm = re.sub(r"\s+", " ", _blank_comments(text)).strip()
    return hashlib.sha1(norm.encode("utf-8")).hexdigest()[:12]


# ---- our side ----------------------------------------------------------------------------------------------------

def load_sources():
    rows = []
    with open(SOURCES, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(line for line in f if not line.startswith("#")):
            row["address"] = int(row["address"], 16)
            rows.append(row)
    return rows


def registered_natives():
    """The recompiled names registered by src/kamite/native/*.cpp (registerFunction / the add lambdas)."""
    names = set()
    rx = re.compile(r'(?:NativeFunction\{|\badd\w*\()\s*"([^"]+)"')
    for name in sorted(os.listdir(NATIVE_DIR)):
        if name.endswith(".cpp"):
            with open(os.path.join(NATIVE_DIR, name), encoding="utf-8") as f:
                names.update(rx.findall(f.read()))
    return names


def recompiled_names():
    """address -> our recompiled name (recomp/kamite_functions.csv)."""
    out = {}
    with open(FUNCTIONS_CSV, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            out[int(row["Start"], 16)] = row["Name"]
    return out


def classifier():
    sys.path.insert(0, os.path.join(ROOT, "tools", "perf"))
    try:
        import subsystems  # noqa: E402 (rung 1's rules: tools/perf/subsystems.csv)
    finally:
        sys.path.pop(0)
    return subsystems.Classifier()


def load_baseline():
    """(commit, {address: (name, subsystem)}) of the rung-4 functions unmatched at the baseline's commit."""
    if not os.path.exists(BASELINE):
        return None, None
    commit, rows = None, {}
    with open(BASELINE, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("# commit "):
                commit = line.split()[2]
            elif line and not line.startswith("#"):
                address, name, subsystem = line.split()
                rows[int(address, 16)] = (name, subsystem)
    return commit, rows


def write_baseline(commit, unmatched):
    with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The rung-4 functions (motion, collision, cloth, effects; tools/perf/subsystems.py) the decomp had not\n")
        f.write("# matched at this commit: tools/native/decomp_watch.py lists the ones matched since. --update rewrites it.\n")
        f.write(f"# commit {commit}\n")
        for address in sorted(unmatched):
            name, subsystem = unmatched[address]
            f.write(f"0x{address:08X} {name} {subsystem}\n")


# ---- the report --------------------------------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("decomp", help="a god-hand-decomp checkout (config + src)")
    ap.add_argument("--update", action="store_true", help="move the rung-4 baseline to this decomp commit")
    ap.add_argument("--show", help="one function by address: its decomp name, file, hash and C")
    ap.add_argument("--record", help="print the sources.csv line for a native written from this address's C")
    ap.add_argument("--note", default="", help="the note column for --record")
    args = ap.parse_args(argv[1:])

    if not os.path.isdir(os.path.join(args.decomp, "src")):
        print(f"decomp_watch: {args.decomp} has no src/ (sparse-checkout set config include src)", file=sys.stderr)
        return 2
    commit = decomp_commit(args.decomp)
    symbols = load_symbols(args.decomp)
    defs, asm = index_definitions(args.decomp)

    if args.show or args.record:
        address = int(args.show or args.record, 16)
        name = decomp_name(symbols, address)
        if name not in defs:
            print(f"0x{address:08X} {name}: no matched C at {commit}", file=sys.stderr)
            return 2
        rel, text = defs[name]
        if args.show:
            print(f"0x{address:08X} {name} {rel} {commit} {definition_hash(text)}\n{text}")
        else:
            ours = recompiled_names().get(address, name)
            print(f"{ours},0x{address:08X},decomp,{name},{rel},{commit},{definition_hash(text)},{args.note}")
        return 0

    listed = 0
    print(f"decomp {commit}: {len(defs)} functions with matched C ({len(asm)} INCLUDE_ASM), {len(symbols)} named")

    # 4. The manifest's health first: a native without a source line can't be watched.
    sources = load_sources()
    registered = registered_natives()
    in_manifest = {row["name"] for row in sources}
    missing = sorted(registered - in_manifest)
    gone = sorted(in_manifest - registered)
    for name in missing:
        print(f"  MANIFEST  {name}: registered in src/kamite/native without a line in tools/native/sources.csv")
    for name in gone:
        print(f"  MANIFEST  {name}: in tools/native/sources.csv but no longer registered")
    listed += len(missing) + len(gone)

    # 1 + 2. Natives against the decomp.
    rewrite, changed = [], []
    for row in sources:
        address, source = row["address"], row["source"]
        name = decomp_name(symbols, address)
        have = name in defs
        if source == "asm":
            if have:
                rewrite.append((row, name, defs[name][0]))
        elif source == "decomp":
            if not have:
                changed.append((row, name, "no matched C any more (renamed? check symbol_addrs.txt)"))
            else:
                h = definition_hash(defs[name][1])
                if h != row["decomp_hash"]:
                    was = f" (was {row['decomp_name']})" if row["decomp_name"] != name else ""
                    changed.append((row, name, f"{defs[name][0]}{was}: C changed since {row['decomp_commit']} "
                                               f"({row['decomp_hash']} -> {h})"))
    print(f"natives: {len(sources)} ({sum(r['source'] == 'decomp' for r in sources)} from the decomp's C, "
          f"{sum(r['source'] == 'asm' for r in sources)} from asm)")
    for row, name, rel in rewrite:
        print(f"  REWRITE   {row['name']} 0x{row['address']:08X}: written from asm, now matched C as {name} in {rel}")
    for row, name, why in changed:
        print(f"  CHANGED   {row['name']} 0x{row['address']:08X}: {why}")
    listed += len(changed)
    # Asm natives with matched C are a standing list until each is rewritten: print them, but only new ones count.
    base_commit, baseline = load_baseline()

    # 3. Rung-4 subsystems.
    classify = classifier()
    ours = recompiled_names()
    unmatched, matched_count = {}, {s: 0 for s in RUNG4}
    total = {s: 0 for s in RUNG4}
    for address, name in symbols.items():
        subsystem = classify(address, ours.get(address, name))
        if subsystem not in RUNG4:
            continue
        total[subsystem] += 1
        if name in defs:
            matched_count[subsystem] += 1
        else:
            unmatched[address] = (name, subsystem)
    print("rung 4 (matched / classified): " + ", ".join(f"{s} {matched_count[s]}/{total[s]}" for s in RUNG4))
    if baseline is None:
        print(f"  no baseline yet ({os.path.relpath(BASELINE, ROOT)}): run with --update to start one")
    else:
        newly = sorted(a for a in baseline if a not in unmatched)
        for address in newly:
            name, subsystem = baseline[address]
            now = decomp_name(symbols, address)
            where = defs[now][0] if now in defs else "?"
            print(f"  RUNG4     0x{address:08X} {now} ({subsystem}): matched since {base_commit}, {where}")
        listed += len(newly)
    if args.update:
        write_baseline(commit, unmatched)
        print(f"baseline -> {commit} ({len(unmatched)} rung-4 functions unmatched)")
    if rewrite:
        print(f"({len(rewrite)} REWRITE lines: asm natives with matched C, a standing list until each is rewritten)")
    return 1 if listed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
