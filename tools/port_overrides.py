#!/usr/bin/env python3
"""Port one game's overrides to another build: every address rewritten.

    tools/port_overrides.py A.xbe A/functions.json B.xbe B/functions.json \
        --src src/games/dub/recomp_manual.c --out src/games/remix/recomp_manual.c \
        --map config/remix/port_map.txt

Reads A's override file and rewrites every guest address in it -- sub_XXXXXXXX
names and 0x00XXXXXX literals inside A's image -- to B's, through
tools/match_lifts.py: function matches, places inside identical functions,
functions shifted with their neighbours, and data found through the same
operand of the twin instruction. What cannot be mapped is left as it was
and marked /* PORT? */ in the output; the report lists each with the reason.
Review the marked places by hand: a mapped override still needs checking
when its function only came out "similar".
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import match_lifts as ml  # noqa: E402

TOKEN = re.compile(r"\bsub_([0-9A-Fa-f]{8})(?![0-9A-Fa-f])|\b0x([0-9A-Fa-f]{6,8})u?\b")


def is_address(h, v, xa):
    """A literal names an address when it lies in A's image -- and, written
    with fewer than eight digits, from 0x100000 up: 0x482AA4 is Bink's data,
    0x40000 a flag (2026-10-24: six-digit addresses went across unported)."""
    if not (xa.lo <= v < xa.hi):
        return False
    return len(h) == 8 or (v >= 0x100000 and v & 0xFFFF)   # 0x100000: a size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xbe_a"); ap.add_argument("funcs_a")
    ap.add_argument("xbe_b"); ap.add_argument("funcs_b")
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--report")
    ap.add_argument("--map", help="hand-found pairs: '<A> <B>  # note' per line (config/<game>/port_map.txt)")
    a = ap.parse_args()

    xa, A = ml.load(a.xbe_a, a.funcs_a)
    xb, B = ml.load(a.xbe_b, a.funcs_b)
    m = ml.match(A, B)
    mp = ml.Mapper(xa, A, xb, B, m)

    text = open(a.src).read()
    seen = {}
    if a.map:
        for line in open(a.map):
            line = line.split("#", 1)[0].split()
            if len(line) == 2:
                seen[int(line[0], 16)] = (int(line[1], 16), "by hand (" + os.path.basename(a.map) + ")")
    for t in TOKEN.finditer(text):
        h = t.group(1) or t.group(2)
        v = int(h, 16)
        if not is_address(h, v, xa):
            continue                              # a constant, not an address
        if v not in seen:
            seen[v] = mp.map(v)

    def repl(t):
        h = t.group(1) or t.group(2)
        v = int(h, 16)
        if v not in seen:
            return t.group(0)
        b, _how = seen[v]
        if b is None:
            return t.group(0) + " /* PORT? */"
        width = max(len(h), len("%x" % b))       # 0x482AA4 stays six digits wide
        new = ("%0*x" if any(c in "abcdef" for c in h) else "%0*X") % (width, b)
        if t.group(1):
            return "sub_" + new
        return t.group(0).replace(h, new, 1)

    # Where the comments are: a marker in a comment is plain text (a "*/"
    # would end the comment early).
    in_comment = bytearray(len(text))
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            in_comment[i:j] = b"\1" * (j - i)
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            in_comment[i:j] = b"\1" * (j - i)
            i = j
        elif text[i] in "\"'":
            q, j = text[i], i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            i += 1

    def repl_at(t):
        r = repl(t)
        if r.endswith(" /* PORT? */") and in_comment[t.start()]:
            r = r[:-len(" /* PORT? */")] + " (PORT?)"
        return r

    out = TOKEN.sub(repl_at, text)
    head = ("/* Ported by tools/port_overrides.py from %s: every guest address\n"
            " * rewritten for this build. Places marked PORT? could not be mapped;\n"
            " * each needs a look before its override is trusted. */\n\n" % a.src)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    open(a.out, "w").write(head + out)

    lines = []
    ok = sum(1 for b, _ in seen.values() if b is not None)
    lines.append("%d addresses: %d mapped, %d not" % (len(seen), ok, len(seen) - ok))
    for v in sorted(seen):
        b, how = seen[v]
        lines.append("  %08X -> %s  %s" % (v, "%08X" % b if b is not None else "?       ", how))
    rep = "\n".join(lines) + "\n"
    if a.report:
        open(a.report, "w").write(rep)
    sys.stdout.write(rep)


if __name__ == "__main__":
    main()
