#!/usr/bin/env python3
"""Replay one captured function call on a real x86 emulator and diff it
against what the lifted C did.

    python3 tools/difftest.py <before.bin> [<after.bin>]      (make difftest)

The game (src/difftest.c, armed by MC3_DIFFTEST_CALL=<n>) wrote the guest
registers and memory just before and just after one call of a wrapped
function. This loads "before" into Unicorn, runs the *original* Xbox machine
code from the same state until the function returns, and compares the result
with "after":

  - registers that differ (eax is the return value);
  - memory that differs, grouped into ranges, per region;
  - for the first differing bytes: the x86 instruction that wrote them (PC,
    function, disassembly) and both values -- usually enough to name the
    instruction the lifter translates wrongly.

Kernel calls (the runtime's synthetic thunk addresses, 0xFE0000xx) are not
emulated: common cheap ones are answered (IRQL, critical sections, time
queries); any other stops the replay and is reported. Hardware apertures
(0xFD..., 0xFE8..., 0xFEC...) read as zero.

Differences can also be noise: other guest threads ran during the lifted call
(the guest lock is released in kernel calls), and timing reads (rdtsc,
performance counter) differ. Focus on ranges the x86 replay itself wrote.
"""
import bisect
import json
import os as _os
_os.environ.setdefault("UC_IGNORE_REG_BREAK", "1")   # silence Unicorn's FPx deprecation notes
import os
import struct
import sys

try:
    import numpy as np
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE,
                         UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_PROT_ALL)
    from unicorn.x86_const import *  # noqa: F401,F403
except ImportError as e:
    sys.exit("difftest needs numpy, capstone and unicorn: pip install numpy capstone unicorn (%s)" % e)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.environ.get("MC3_WORK") or os.path.join(REPO, "work")
GAME = os.environ.get("MC3_GAME_DIR") or os.path.join(REPO, "game")
ANALYSIS = os.path.join(WORK, "analysis")
THUNK_BASE = 0xFE000000
MAX_INSN = int(os.environ.get("DIFFTEST_MAX_INSN", "500000000"))

GPR = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
UC_GPR = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
          UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
UC_FP = [UC_X86_REG_FP0 + i for i in range(8)]   # MMX = the x87 registers' mantissas
UC_XMM = [UC_X86_REG_XMM0 + i for i in range(8)]
FS_BASE = 0x4000            # the runtime's fake TIB (xbox_memory_layout.c)
GDT_VA = 0xFE010000         # scratch page for a descriptor table (FS segment)


def set_mmx(uc, i, value):
    uc.reg_write(UC_FP[i], (value & 0xFFFFFFFFFFFFFFFF, 0xFFFF))


def get_mmx(uc, i):
    mantissa, _exponent = uc.reg_read(UC_FP[i])
    return mantissa & 0xFFFFFFFFFFFFFFFF


def gdt_entry(base, limit, access, flags):
    return struct.pack("<HHBBBB", limit & 0xFFFF, base & 0xFFFF, (base >> 16) & 0xFF,
                       access, ((limit >> 16) & 0xF) | (flags << 4), (base >> 24) & 0xFF)


def setup_fs(uc):
    """32-bit Unicorn has no FS_BASE: give FS a descriptor with the TIB base.
    Once a GDT is loaded the data and stack segments need descriptors too
    (with FS alone, `ret` popped from the wrong place): flat 4 GB for them."""
    uc.mem_map(GDT_VA, 0x1000, UC_PROT_ALL)
    uc.mem_write(GDT_VA, gdt_entry(0, 0, 0, 0)                       # 0 null
                 + gdt_entry(0, 0xFFFFF, 0x9A, 0xC)                  # 1 flat code
                 + gdt_entry(0, 0xFFFFF, 0x92, 0xC)                  # 2 flat data
                 + gdt_entry(FS_BASE, 0xFFFFF, 0x92, 0xC))           # 3 FS: the TIB
    uc.reg_write(UC_X86_REG_GDTR, (0, GDT_VA, 4 * 8 - 1, 0))
    for reg in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
        uc.reg_write(reg, 2 << 3)
    uc.reg_write(UC_X86_REG_FS, 3 << 3)

# Kernel ordinals answered without the kernel: ordinal -> (stack args, eax).
# stdcall: the callee pops its arguments. KfRaiseIrql/KfLowerIrql are
# fastcall (argument in ecx).
KERNEL_STUBS = {
    160: (0, 0),    # KfRaiseIrql        -> old IRQL 0 (PASSIVE)
    161: (0, 0),    # KfLowerIrql
    129: (0, 0),    # KeRaiseIrqlToDpcLevel
    277: (1, 0),    # RtlEnterCriticalSection
    294: (1, 0),    # RtlLeaveCriticalSection
    306: (1, 1),    # RtlTryEnterCriticalSection -> TRUE
    126: (0, None), # KeQueryPerformanceCounter -> edx:eax from a counter
    128: (1, 0),    # KeQuerySystemTime(PLARGE_INTEGER): leaves it as it was
    175: (3, 0),    # MmLockUnlockBufferPages: nothing to do (no paging)
    176: (2, 0),    # MmLockUnlockPhysicalPage
    # MmGetPhysicalAddress: the contiguous window is physical RAM at
    # 0x80000000; anything else as given (the runtime tags heap RAM with bit
    # 26, which only the GPU's reads care about).
    159: (5, 0),    # KeWaitForSingleObject -> STATUS_SUCCESS (D3D's fence wait: no GPU here)
    173: (1, lambda va: va - 0x80000000 if 0x80000000 <= va < 0x84000000 else va),
}


def read_capture(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"MC3DIFF1":
        sys.exit("%s: not a difftest capture" % path)
    o = 8
    version, func, call = struct.unpack_from("<III", data, o); o += 12
    gpr = list(struct.unpack_from("<8I", data, o)); o += 32
    fp_top, fpcw = struct.unpack_from("<II", data, o); o += 8
    fp = list(struct.unpack_from("<8d", data, o)); o += 64
    mm = list(struct.unpack_from("<8Q", data, o)); o += 64
    xmm = [int.from_bytes(data[o + 16 * i:o + 16 * i + 16], "little") for i in range(8)]; o += 128
    (nreg,) = struct.unpack_from("<I", data, o); o += 4
    regions = []
    for _ in range(nreg):
        va, size = struct.unpack_from("<II", data, o); o += 8
        regions.append((va, np.frombuffer(data, dtype=np.uint8, count=size, offset=o)))
        o += size
    return dict(func=func, call=call, gpr=gpr, fp_top=fp_top, fpcw=fpcw & 0xFFFF,
                fp=fp, mm=mm, xmm=xmm, regions=regions)


class Symbols:
    def __init__(self):
        self.starts, self.ident, self.sections = [], {}, []
        try:
            fj = json.load(open(os.path.join(ANALYSIS, "disasm", "functions.json")))
            fs = fj if isinstance(fj, list) else fj.get("functions", fj)
            fs = list(fs.values()) if isinstance(fs, dict) else fs
            self.starts = sorted(int(f["start"], 16) for f in fs)
            idf = json.load(open(os.path.join(ANALYSIS, "func_id", "identified_functions.json")))
            items = idf if isinstance(idf, list) else idf.get("functions", idf)
            items = list(items.values()) if isinstance(items, dict) else items
            self.ident = {int(v["start"], 16): v.get("identified_name") or v.get("category", "")
                          for v in items}
            info = json.load(open(os.path.join(ANALYSIS, "xbe_analysis.json")))
            self.sections = [(int(s["virtual_addr"], 16), s["virtual_size"], s["name"])
                             for s in info["sections"]]
            self.thunk_va = int(info["kernel_thunk_addr"], 16)
        except (OSError, ValueError, KeyError):
            self.thunk_va = None

    def where(self, va):
        sec = next((n for lo, sz, n in self.sections if lo <= va < lo + sz), "?")
        if not self.starts or va < self.starts[0]:
            return sec, None
        f = self.starts[bisect.bisect_right(self.starts, va) - 1]
        return sec, f


def thunk_ordinals(mem_ram, thunk_va):
    """Slot -> kernel ordinal, from the XBE (the runtime rewrote the table)."""
    try:
        xbe = open(os.path.join(GAME, "default.xbe"), "rb").read()
        info = json.load(open(os.path.join(ANALYSIS, "xbe_analysis.json")))
    except OSError:
        return []
    for s in info["sections"]:
        lo = int(s["virtual_addr"], 16)
        if lo <= thunk_va < lo + s["raw_size"]:
            off = int(s["raw_addr"], 16) + thunk_va - lo
            out = []
            while True:
                (v,) = struct.unpack_from("<I", xbe, off + 4 * len(out))
                if not v:
                    return out
                out.append(v & 0x7FFFFFFF)
    return []


def make_emulator(cap, syms, ordinals, log):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    for va, mem in cap["regions"]:
        uc.mem_map(va, len(mem), UC_PROT_ALL)
        uc.mem_write(va, mem.tobytes())
    for r, v in zip(UC_GPR, cap["gpr"]):
        uc.reg_write(r, v)
    for i, v in enumerate(cap["mm"]):
        set_mmx(uc, i, v)
    for r, v in zip(UC_XMM, cap["xmm"]):
        uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_FPCW, cap["fpcw"] or 0x027F)
    setup_fs(uc)
    # Synthetic kernel-thunk targets: a page we intercept.
    uc.mem_map(THUNK_BASE, 0x1000, UC_PROT_ALL)
    uc.mem_write(THUNK_BASE, b"\xF4" * 0x1000)     # hlt, never actually run
    state = {"stop": None, "perf": 0}

    def on_thunk(uc_, addr, size, _):
        slot = (addr - THUNK_BASE) // 4
        ordinal = ordinals[slot] if slot < len(ordinals) else None
        stub = KERNEL_STUBS.get(ordinal)
        if stub is None:
            state["stop"] = "kernel call: ordinal %s (slot %d)" % (ordinal, slot)
            uc_.emu_stop()
            return
        nargs, eax = stub
        esp = uc_.reg_read(UC_X86_REG_ESP)
        ret = struct.unpack("<I", uc_.mem_read(esp, 4))[0]
        if callable(eax):
            eax = eax(*struct.unpack("<%dI" % nargs, uc_.mem_read(esp + 4, 4 * nargs))) & 0xFFFFFFFF
        if eax is None:     # performance counter: a steady 733 MHz-ish clock
            state["perf"] += 1000
            uc_.reg_write(UC_X86_REG_EAX, state["perf"] & 0xFFFFFFFF)
            uc_.reg_write(UC_X86_REG_EDX, state["perf"] >> 32)
        else:
            uc_.reg_write(UC_X86_REG_EAX, eax)
        uc_.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * nargs)
        uc_.reg_write(UC_X86_REG_EIP, ret)
    uc.hook_add(UC_HOOK_CODE, on_thunk, begin=THUNK_BASE, end=THUNK_BASE + 0xFFF)

    def on_unmapped(uc_, access, addr, size, value, _):
        page = addr & ~0xFFF
        if addr >= 0xFD000000:          # hardware apertures: zeroed pages
            uc_.mem_map(page, 0x1000, UC_PROT_ALL)
            log.append("mapped a zero page for hardware access at 0x%08X" % addr)
            return True
        state["stop"] = "unmapped access at 0x%08X (pc 0x%08X)" % (addr, uc_.reg_read(UC_X86_REG_EIP))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    return uc, state


def run(cap, uc, state):
    esp = cap["gpr"][4]
    ret = struct.unpack("<I", bytes(uc.mem_read(esp, 4)))[0]

    def on_return(uc_, addr, size, _):
        # Stop *at* the return address, before anything there executes.
        if uc_.reg_read(UC_X86_REG_ESP) > esp:
            uc_.emu_stop()
    uc.hook_add(UC_HOOK_CODE, on_return, begin=ret, end=ret)
    try:
        uc.emu_start(cap["func"], ret, count=MAX_INSN)
    except UcError as e:
        state["stop"] = state["stop"] or "emulator error %s at pc 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    pc = uc.reg_read(UC_X86_REG_EIP)
    if pc != ret and not state["stop"]:
        state["stop"] = "did not return within %d instructions (pc 0x%08X)" % (MAX_INSN, pc)
    return ret


def ranges(idx, gap=16):
    if len(idx) == 0:
        return []
    out, lo, prev = [], idx[0], idx[0]
    for i in idx[1:]:
        if i > prev + gap:
            out.append((lo, prev)); lo = i
        prev = i
    out.append((lo, prev))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    before_path = sys.argv[1]
    after_path = sys.argv[2] if len(sys.argv) > 2 else before_path.replace("_before.bin", "_after.bin")
    before, after = read_capture(before_path), read_capture(after_path)
    syms = Symbols()
    ordinals = thunk_ordinals(before["regions"][0][1], syms.thunk_va) if syms.thunk_va else []
    log = []
    print("== replaying sub_%08X call #%d on x86 (Unicorn)" % (before["func"], before["call"]))
    uc, state = make_emulator(before, syms, ordinals, log)
    run(before, uc, state)
    for line in sorted(set(log))[:8]:
        print("   " + line)
    if state["stop"]:
        print("!! replay stopped early: %s -- the diff below is partial" % state["stop"])

    print("\n== registers after the call (x86 replay vs lifted)")
    for name, r, lifted in zip(GPR, UC_GPR, after["gpr"]):
        x86 = uc.reg_read(r)
        flag = "" if x86 == lifted else "   <-- differs"
        print("   %-4s x86=%08X lifted=%08X%s" % (name, x86, lifted, flag))
    for i in range(8):
        x86 = get_mmx(uc, i)
        if x86 != after["mm"][i]:
            print("   mm%d  x86=%016X lifted=%016X   <-- differs" % (i, x86, after["mm"][i]))

    print("\n== memory")
    diffs = []
    for (va, b), (_, a) in zip(before["regions"], after["regions"]):
        x86 = np.frombuffer(bytes(uc.mem_read(va, len(b))), dtype=np.uint8)
        changed_x86 = np.count_nonzero(x86 != b)
        changed_lifted = np.count_nonzero(a != b)
        idx = np.nonzero(x86 != a)[0]
        print("   region 0x%08X: x86 changed %d bytes, lifted changed %d, they differ in %d"
              % (va, changed_x86, changed_lifted, len(idx)))
        rs = ranges(idx)
        for lo, hi in sorted(rs, key=lambda r: r[0] - r[1])[:12]:
            n = int(np.count_nonzero(x86[lo:hi + 1] != a[lo:hi + 1]))
            x86_wrote = bool(np.any(x86[lo:hi + 1] != b[lo:hi + 1]))
            print("      0x%08X..0x%08X  %6d bytes differ  %s"
                  % (va + lo, va + hi, n, "(x86 wrote here)" if x86_wrote else "(x86 did not touch: other thread or timing?)"))
        diffs += [(va + int(i), int(x86[i]), int(a[i])) for i in idx[:4096] if x86[i] != b[i]]

    if not diffs:
        print("\nno differences in memory the x86 replay wrote: the lifted call matches")
        return 0

    # Pass 2: who wrote the first differing bytes, on x86?
    first = diffs[:32]
    watch = {va for va, _, _ in first}
    writer = {}
    uc2, state2 = make_emulator(before, syms, ordinals, [])

    def on_write(uc_, access, addr, size, value, _):
        for k in range(size):
            if addr + k in watch:
                writer[addr + k] = uc_.reg_read(UC_X86_REG_EIP)
    # A write is reported by its start address: watch from 16 bytes before.
    lo = min(watch) - 16; hi = max(watch)
    uc2.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=lo, end=hi)
    run(before, uc2, state2)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    print("\n== first differing bytes and the x86 instruction that last wrote them")
    seen_pc = set()
    for va, xv, lv in first:
        pc = writer.get(va)
        if pc is None:
            print("   0x%08X x86=%02X lifted=%02X  (writer not seen)" % (va, xv, lv)); continue
        sec, f = syms.where(pc)
        code = bytes(uc2.mem_read(pc, 16))
        ins = next(md.disasm(code, pc), None)
        txt = "%s %s" % (ins.mnemonic, ins.op_str) if ins else "?"
        print("   0x%08X x86=%02X lifted=%02X  written at 0x%08X in sub_%08X (%s %s): %s"
              % (va, xv, lv, pc, f or 0, sec, syms.ident.get(f, ""), txt))
        seen_pc.add(pc)
    print("\n%d distinct writing instruction(s) above; inspect their lifted C in work/gen" % len(seen_pc))
    if os.environ.get("DIFFTEST_ORDER") == "time":
        report_by_time(before, after, syms, ordinals, md)
    return 1


def report_by_time(before, after, syms, ordinals, md):
    """DIFFTEST_ORDER=time: the differing bytes in the order the x86 replay
    last wrote them. The earliest is closest to the cause; the rest are
    usually its consequences. Bytes below the stack pointer the call
    returns with (dead frames of callees, saved ebp) are left out."""
    esp_end = after["gpr"][4]
    esp_lo = esp_end - 0x10000
    uc, state = make_emulator(before, syms, ordinals, [])
    lastw = {}
    n = [0]

    def on_code(uc_, addr, size, _):
        n[0] += 1
    def on_write(uc_, access, addr, size, value, _):
        pc = uc_.reg_read(UC_X86_REG_EIP)
        for k in range(size):
            lastw[addr + k] = (n[0], pc)
    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    run(before, uc, state)
    rows = []
    for (va, b), (_, a) in zip(before["regions"], after["regions"]):
        x86 = np.frombuffer(bytes(uc.mem_read(va, len(b))), dtype=np.uint8)
        for i in np.nonzero((x86 != a) & (x86 != b))[0]:
            addr = va + int(i)
            if esp_lo <= addr < esp_end:
                continue
            w = lastw.get(addr)
            if w:
                rows.append((w[0], addr, w[1], int(x86[i]), int(a[i])))
    rows.sort()
    print("\n== differing bytes in the order x86 last wrote them (live memory only)")
    seen = set()
    for t, addr, pc, xv, lv in rows:
        key = (pc, addr & ~3)
        if key in seen:
            continue
        sec, f = syms.where(pc)
        if sec in os.environ.get("DIFFTEST_SKIP", "").split(","):
            continue                    # DIFFTEST_SKIP=XPP: a library's own noise
        seen.add(key)
        sec, f = syms.where(pc)
        ins = next(md.disasm(bytes(uc.mem_read(pc, 16)), pc), None)
        txt = "%s %s" % (ins.mnemonic, ins.op_str) if ins else "?"
        print("   #%-9d 0x%08X x86=%02X lifted=%02X  at 0x%08X in sub_%08X: %s"
              % (t, addr, xv, lv, pc, f or 0, txt))
        if len(seen) >= int(os.environ.get("DIFFTEST_ROWS", "40")):
            break


if __name__ == "__main__":
    sys.exit(main())
