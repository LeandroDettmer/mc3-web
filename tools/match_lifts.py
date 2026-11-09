#!/usr/bin/env python3
"""Pair the functions of two lifts of related XBEs (MC3 DUB Edition and Remix).

    tools/match_lifts.py A.xbe A/functions.json B.xbe B/functions.json \
        [--manual src/recomp_manual.c] [--seeds config/seed_functions.json] \
        [--out map.json]

Two builds of one game move almost everything: the Remix's libraries (D3D,
Bink, DSound...) are byte-for-byte the same size as the DUB Edition's but sit
at new addresses, and its game code grew by 222 KB. Every override and seed is
an address, so each needs its counterpart in the other build -- or the news
that the function changed.

Matching, in passes:
  1. exact: the instructions with every address masked (call/jump targets,
     pointers into the image) hash the same, and the hash is unique on both
     sides;
  2. order: between two matched functions of the same section, unmatched
     ones in the same order with the same normalised code size pair up (code
     that only shifted, or holds a unique-hash twin);
  3. similar: what is left pairs with the most similar candidate in the same
     section neighbourhood (difflib ratio of the normalised instructions);
     reported with its score as "changed".

Output: a summary per section, every override and seed of A with its B
address (or "not found"), and --out a JSON map A -> {b, how, score}.
"""
import argparse
import difflib
import hashlib
import json
import re
import struct
import sys

try:
    import capstone
except ImportError:
    sys.exit("needs capstone (the lift's venv has it)")


class Xbe:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.d = d
        base = struct.unpack_from("<I", d, 0x104)[0]
        nsec = struct.unpack_from("<I", d, 0x11C)[0]
        sh = struct.unpack_from("<I", d, 0x120)[0] - base
        self.secs = []
        for i in range(nsec):
            o = sh + i * 56
            _fl, va, vs, raw, rs, na = struct.unpack_from("<IIIIII", d, o)
            name = d[na - base:na - base + 16].split(b"\0")[0].decode()
            self.secs.append((name, va, vs, raw, rs))
        self.lo = min(s[1] for s in self.secs)
        self.hi = max(s[1] + s[2] for s in self.secs)

    def read(self, va, n):
        for _name, sva, _vs, raw, rs in self.secs:
            if sva <= va < sva + rs:
                n = min(n, sva + rs - va)
                return self.d[raw + va - sva:raw + va - sva + n]
        return b""

    def section_of(self, va):
        for name, sva, vs, _raw, _rs in self.secs:
            if sva <= va < sva + vs:
                return name
        return "?"


HEX = re.compile(r"0x[0-9a-f]+")


def normalise(xbe, md, start, end):
    """The function's instructions with addresses masked."""
    code = xbe.read(start, max(0, end - start))
    out = []
    for ins in md.disasm(code, start):
        ops = ins.op_str
        if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
            if HEX.fullmatch(ops.strip()):
                ops = "T"                       # relative target
        def mask(m):
            v = int(m.group(0), 16)
            return "A" if xbe.lo <= v < xbe.hi or v >= 0xFE000000 else m.group(0)
        ops = HEX.sub(mask, ops)
        out.append(ins.mnemonic + " " + ops)
    return out


def load(xbe_path, funcs_path):
    xbe = Xbe(xbe_path)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    fs = []
    for f in json.load(open(funcs_path)):
        s = int(f["start"], 16)
        e = int(f["end"], 16)
        sec = f.get("section") or xbe.section_of(s)
        norm = normalise(xbe, md, s, e)
        h = hashlib.sha1("\n".join(norm).encode()).hexdigest()
        fs.append({"start": s, "end": e, "sec": sec, "norm": norm, "hash": h})
    fs.sort(key=lambda f: f["start"])
    return xbe, fs


def match(A, B):
    m = {}                                       # A start -> (B start, how, score)
    used = set()
    ha, hb = {}, {}
    for f in A:
        ha.setdefault(f["hash"], []).append(f)
    for f in B:
        hb.setdefault(f["hash"], []).append(f)
    # 1. unique exact
    for h, la in ha.items():
        lb = hb.get(h, [])
        if len(la) == 1 and len(lb) == 1 and la[0]["norm"]:
            m[la[0]["start"]] = (lb[0]["start"], "exact", 1.0)
            used.add(lb[0]["start"])
    # 2. order: walk both lists per section between anchors
    bidx = {f["start"]: i for i, f in enumerate(B)}
    aidx = {f["start"]: i for i, f in enumerate(A)}
    changed = True
    while changed:
        changed = False
        for i, fa in enumerate(A):
            if fa["start"] not in m:
                continue
            j = bidx[m[fa["start"]][0]]
            for step in (1, -1):
                k, l = i + step, j + step
                while 0 <= k < len(A) and 0 <= l < len(B):
                    xa, xb = A[k], B[l]
                    if xa["start"] in m or xb["start"] in used or xa["sec"] != xb["sec"]:
                        break
                    if xa["hash"] == xb["hash"]:
                        m[xa["start"]] = (xb["start"], "order", 1.0)
                        used.add(xb["start"])
                        changed = True
                    else:
                        break
                    k += step
                    l += step
    # 3. similar: unmatched A against unmatched B between the neighbouring anchors
    for i, fa in enumerate(A):
        if fa["start"] in m or not fa["norm"]:
            continue
        lo_j, hi_j = 0, len(B)
        for k in range(i - 1, -1, -1):
            if A[k]["start"] in m:
                lo_j = bidx[m[A[k]["start"]][0]] + 1
                break
        for k in range(i + 1, len(A)):
            if A[k]["start"] in m:
                hi_j = bidx[m[A[k]["start"]][0]]
                break
        if hi_j - lo_j > 400:                    # too far apart to be the same place
            continue
        best, bs = None, 0.0
        for fb in B[lo_j:hi_j]:
            if fb["start"] in used or fb["sec"] != fa["sec"] or not fb["norm"]:
                continue
            ln_a, ln_b = len(fa["norm"]), len(fb["norm"])
            if min(ln_a, ln_b) < 0.5 * max(ln_a, ln_b):
                continue
            r = difflib.SequenceMatcher(None, fa["norm"], fb["norm"], autojunk=False).ratio()
            if r > bs:
                best, bs = fb, r
        if best and bs >= 0.6:
            m[fa["start"]] = (best["start"], "similar", round(bs, 3))
            used.add(best["start"])
    return m


def shifted(xb, md, A, m, fa, xa_secs=()):
    """4. shifted: the B lift may not have the function at all (A found it
    through a seed, B was lifted without). Shift it by the nearest exactly
    matched neighbour's distance and compare the code there."""
    best = None
    for g in A:                                  # nearest anchor before and after
        r = m.get(g["start"])
        if not r or r[1] not in ("exact", "order") or g["sec"] != fa["sec"]:
            continue
        d = abs(g["start"] - fa["start"])
        if best is None or d < best[0]:
            best = (d, r[0] - g["start"])
    deltas = []
    if best is not None and best[0] <= 0x4000:
        deltas.append(best[1])
    # A section of the same size in both (the libraries): its start's shift.
    sa = next((x for x in xa_secs if x[0] == fa["sec"]), None)
    sb = next((x for x in xb.secs if x[0] == fa["sec"]), None)
    if sa and sb and sa[2] == sb[2]:
        deltas.append(sb[1] - sa[1])
    for d in deltas:
        b = fa["start"] + d
        norm = normalise(xb, md, b, b + (fa["end"] - fa["start"]))
        if norm == fa["norm"]:
            return b
    return None


class Mapper:
    """Map any address of A to B: a function start (the match), a place
    inside a matched function (same offset), a shifted function (no start in
    B), or data an identical function refers to (the same operand of the
    same instruction in B's twin)."""

    def __init__(self, xa, A, xb, B, m):
        self.xa, self.A, self.xb, self.B, self.m = xa, A, xb, B, m
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.starts = {f["start"]: f for f in A}
        self.refs = None

    def _index_refs(self):
        """Every address-sized operand of every identically matched function
        of A, with where it sits: value -> [(A start, insn #, operand #)]."""
        self.refs = {}
        for f in self.A:
            r = self.m.get(f["start"])
            if not r or r[1] not in ("exact", "order"):
                continue
            code = self.xa.read(f["start"], f["end"] - f["start"])
            for k, ins in enumerate(self.md.disasm(code, f["start"])):
                for j, h in enumerate(HEX.findall(ins.op_str)):
                    v = int(h, 16)
                    if self.xa.lo <= v < self.xa.hi:
                        self.refs.setdefault(v, []).append((f["start"], k, j))

    def _operand_in_b(self, a_start, k, j):
        b_start = self.m[a_start][0]
        fa = self.starts[a_start]
        code = self.xb.read(b_start, fa["end"] - fa["start"] + 16)
        for kk, ins in enumerate(self.md.disasm(code, b_start)):
            if kk == k:
                hs = HEX.findall(ins.op_str)
                return int(hs[j], 16) if j < len(hs) else None
        return None

    def map(self, ad):
        """(B address or None, how)."""
        r = self.m.get(ad)
        if r:
            return r[0], r[1] + ("" if r[1] != "similar" else " %.2f" % r[2])
        f = self.starts.get(ad)
        if f:
            b = shifted(self.xb, self.md, self.A, self.m, f, self.xa.secs)
            return (b, "shifted") if b is not None else (None, "function not found")
        cont = next((g for g in self.A if g["start"] < ad < g["end"]), None)
        if cont:
            rc = self.m.get(cont["start"])
            if rc and rc[1] in ("exact", "order"):
                return rc[0] + (ad - cont["start"]), "inside sub_%08X" % cont["start"]
            b = shifted(self.xb, self.md, self.A, self.m, cont, self.xa.secs) if not rc else None
            if b is not None:
                return b + (ad - cont["start"]), "inside shifted sub_%08X" % cont["start"]
            return None, "inside a changed function (sub_%08X)" % cont["start"]
        if self.refs is None:
            self._index_refs()
        votes = {}
        for a_start, k, j in self.refs.get(ad, [])[:40]:
            v = self._operand_in_b(a_start, k, j)
            if v is not None:
                votes[v] = votes.get(v, 0) + 1
        if votes:
            v, n = max(votes.items(), key=lambda kv: kv[1])
            return v, "data, %d of %d references agree" % (n, sum(votes.values()))
        return None, "data, no reference in identical code"


def manual_addrs(path):
    """Addresses src/recomp_manual.c replaces, as the lift reads them."""
    import os
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "xboxrecomp"))
    from tools.recomp.manual_scan import scan
    return sorted(scan(path)[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xbe_a"); ap.add_argument("funcs_a")
    ap.add_argument("xbe_b"); ap.add_argument("funcs_b")
    ap.add_argument("--manual"); ap.add_argument("--seeds"); ap.add_argument("--out")
    a = ap.parse_args()

    xa, A = load(a.xbe_a, a.funcs_a)
    xb, B = load(a.xbe_b, a.funcs_b)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    m = match(A, B)

    print(f"functions: A {len(A)}, B {len(B)}; matched {len(m)}")
    secs = {}
    for f in A:
        s = secs.setdefault(f["sec"], [0, 0, 0, 0])
        s[0] += 1
        how = m.get(f["start"], (None, None, 0))[1]
        if how in ("exact", "order"):
            s[1] += 1
        elif how == "similar":
            s[2] += 1
        else:
            s[3] += 1
    print(f"{'section':10s} {'funcs':>6s} {'same':>6s} {'changed':>8s} {'none':>6s}")
    for name, (n, same, ch, no) in sorted(secs.items(), key=lambda kv: -kv[1][0]):
        print(f"{name:10s} {n:6d} {same:6d} {ch:8d} {no:6d}")

    def report(title, addrs):
        if not addrs:
            return
        print(f"\n{title}:")
        starts = {f["start"]: f for f in A}
        for ad in addrs:
            f = starts.get(ad)
            if not f:
                # inside a function: map by the offset within it
                cont = next((g for g in A if g["start"] <= ad < g["end"]), None)
                if cont and cont["start"] in m and m[cont["start"]][1] in ("exact", "order"):
                    b = m[cont["start"]][0] + (ad - cont["start"])
                    print(f"  {ad:08X} -> {b:08X}  (inside sub_{cont['start']:08X}, same code)")
                else:
                    print(f"  {ad:08X} -> ?         (not a function start in A)")
                continue
            r = m.get(ad)
            if not r:
                b = shifted(xb, md, A, m, f, xa.secs)
                if b is not None:
                    m[ad] = (b, "shifted", 1.0)
                    print(f"  {ad:08X} -> {b:08X}  shifted (no function there in B: seed it)  [{f['sec']}]")
                else:
                    print(f"  {ad:08X} -> not found   [{f['sec']}]")
            else:
                print(f"  {ad:08X} -> {r[0]:08X}  {r[1]}{'' if r[1] != 'similar' else f' {r[2]:.2f}'}  [{f['sec']}]")

    if a.manual:
        report("overrides (" + a.manual + ")", manual_addrs(a.manual))
    if a.seeds:
        report("seeds (" + a.seeds + ")", sorted(int(s["start"], 16) for s in json.load(open(a.seeds))))
    if a.out:
        json.dump({f"{k:08X}": {"b": f"{v[0]:08X}", "how": v[1], "score": v[2]}
                   for k, v in sorted(m.items())}, open(a.out, "w"), indent=1)
        print(f"\nmap: {a.out}")


if __name__ == "__main__":
    main()
