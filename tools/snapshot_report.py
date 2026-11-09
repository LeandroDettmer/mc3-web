#!/usr/bin/env python3
"""Read a crash or watchdog snapshot from a run log and say where the game is.

    python3 tools/snapshot_report.py [run.log]        (default: newest work-<game>/logs/run_*.log)
    make report [LOG=work-<game>/logs/run_x.log]

For the first snapshot in the log (watchdog or crash) it prints:
  - the registers, and Win32 error codes among them (0x8007000E = E_OUTOFMEMORY);
  - the guest call chain: every word on the guest stack that lands inside a
    code section, with the section (DSOUND, XPP, D3D, .text ...), the
    containing function and what func_id identified it as;
  - the recent indirect calls, kernel thunks resolved to their ordinal names.

Adds the per-section and kernel-name view on top of the toolkit's
tools/stackwalk.py, which reads the same GS lines.
"""
import bisect
import glob
import json
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.environ.get("MC3_WORK") or os.path.join(REPO, "work-" + (os.environ.get("MC3_GAME") or os.environ.get("GAME") or "dub"))
GAME = os.environ.get("MC3_GAME_DIR") or os.path.join(REPO, "game")
ANALYSIS = os.path.join(WORK, "analysis")
THUNK_VA_BASE = 0xFE000000      # synthetic kernel-thunk addresses (runtime)
HRESULTS = {0x8007000E: "E_OUTOFMEMORY", 0x80004005: "E_FAIL",
            0x80070057: "E_INVALIDARG", 0x8000FFFF: "E_UNEXPECTED",
            0xC0000034: "STATUS_OBJECT_NAME_NOT_FOUND",
            0xC0000017: "STATUS_NO_MEMORY"}


def load_json(path):
    with open(path) as f:
        return json.load(f)


def as_list(data, key):
    if isinstance(data, dict):
        data = data.get(key, data)
    return list(data.values()) if isinstance(data, dict) else data


def num(v):
    return int(v, 16) if isinstance(v, str) else v


def main():
    log_path = sys.argv[1] if len(sys.argv) > 1 else max(
        glob.glob(os.path.join(WORK, "logs", "run_*.log")), key=os.path.getmtime)
    log = open(log_path, errors="replace").read()
    at = min([i for i in (log.find("[WATCHDOG]"), log.find("[CRASH]")) if i >= 0],
             default=-1)
    print("== %s" % log_path)
    if at < 0:
        print("no [WATCHDOG] or [CRASH] snapshot in this log")
        return 1
    snap = log[at:]
    print(snap.splitlines()[0])

    xbe_info = load_json(os.path.join(ANALYSIS, "xbe_analysis.json"))
    sections = xbe_info["sections"]
    code = [(num(s["virtual_addr"]), num(s["virtual_addr"]) + s["virtual_size"], s["name"])
            for s in sections
            if s["executable"] and s["name"] not in (".rdata", ".data", ".data1", "BINKDATA")]
    funcs = as_list(load_json(os.path.join(ANALYSIS, "disasm", "functions.json")), "functions")
    starts = sorted(num(f["start"]) for f in funcs)
    ident = {num(v["start"]): v.get("identified_name") or v.get("category", "")
             for v in as_list(load_json(os.path.join(ANALYSIS, "func_id",
                                                     "identified_functions.json")),
                              "functions")}

    regs = re.search(r"regs: (.*)", snap) or re.search(r"guest: (.*)", snap)
    if regs:
        print("registers: " + regs.group(1).strip())
        for name, val in re.findall(r"(\w+)=([0-9A-F]{8})", snap[:2000]):
            if int(val, 16) in HRESULTS:
                print("  %s = %s = %s" % (name, val, HRESULTS[int(val, 16)]))

    print("\ncall chain (innermost first):")
    words = re.findall(r"GS ([0-9A-F]{8}) ([0-9A-F]{8})", snap) or \
        re.findall(r"\[esp\+\s*(\d+)\s*\] 0x([0-9A-F]{8})", snap)
    for slot, word in words:
        v = int(word, 16)
        for lo, hi, name in code:
            if lo <= v < hi:
                f = starts[bisect.bisect_right(starts, v) - 1]
                if f == v:          # an entry point is a pointer, not a return site
                    break
                print("  0x%08X  sub_%08X  %-8s %s" % (v, f, name, ident.get(f, "")))
                break

    # Kernel thunk slots -> ordinals -> names (from the XBE and the parse report).
    xbe = open(os.path.join(GAME, "default.xbe"), "rb").read()
    thunk_va = num(xbe_info["kernel_thunk_addr"])

    def raw(va, n):
        for s in sections:
            lo = num(s["virtual_addr"])
            if lo <= va < lo + s["raw_size"]:
                off = num(s["raw_addr"]) + va - lo
                return xbe[off:off + n]
        return b""
    table = raw(thunk_va, 4 * 400)
    ordinals = []
    for i in range(len(table) // 4):
        v = struct.unpack_from("<I", table, 4 * i)[0]
        if not v:
            break
        ordinals.append(v & 0x7FFFFFFF)
    names = {}
    summary = os.path.join(ANALYSIS, "xbe_summary.txt")
    if os.path.exists(summary):
        for line in open(summary):
            m = re.match(r"\s+#(\d+)\s+(\w+)", line)
            if m:
                names[int(m.group(1))] = m.group(2)

    icalls = re.search(r"recent ICALL targets:? (.*)", snap)
    if icalls:
        print("\nrecent indirect calls (oldest to newest, duplicates folded):")
        for t in dict.fromkeys(icalls.group(1).split()):
            v = int(t, 16)
            if v >= THUNK_VA_BASE and (v - THUNK_VA_BASE) // 4 < len(ordinals):
                o = ordinals[(v - THUNK_VA_BASE) // 4]
                print("  %s  kernel #%-3d %s" % (t, o, names.get(o, "?")))
            elif v:
                f = starts[bisect.bisect_right(starts, v) - 1] if v >= starts[0] else 0
                print("  %s  code (sub_%08X %s)" % (t, f, ident.get(f, "")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
