"""One-operand mul/imul/div/idiv at 8 and 16 bits.

They were lifted as the 32-bit forms (edx:eax = eax * src), so `imul dl`
wrote the product's high half into edx. The 8-bit forms touch AX only; the
16-bit forms AX and DX's low halves. Midnight Club 3 Remix's colour-shift
palette fill (0x00270210) counts in edx around an `imul dl`: the counter was
reset by every multiply and the garage's Custom Color Shift screen froze.

The check compiles the lifter's own output and compares it, register by
register, with the x86 answer computed here.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))
"""


def _reg(name, size):
    op = Operand("reg")
    op.reg = name
    op.size = size
    return op


def _lift(m, src):
    insn = Instruction(0, 2, m, "", "00")
    insn.operands = [src]
    return " ".join(Lifter().lift_instruction(insn))


def _trunc_div(d, s):
    q = abs(d) // abs(s)
    q = q if (d < 0) == (s < 0) else -q
    return q, d - q * s


def _sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def _want(m, w, eax, edx, s):
    """x86's eax/edx after `m src`, or None where x86 raises #DE."""
    bits = w * 8
    mask = (1 << bits) - 1
    signed = m in ("imul", "idiv")
    if m in ("mul", "imul"):
        a, b = eax & mask, s & mask
        if signed:
            a, b = _sx(a, bits), _sx(b, bits)
        r = (a * b) & ((1 << (2 * bits)) - 1)
        if w == 1:
            return (eax & 0xFFFF0000) | r, edx
        return (eax & 0xFFFF0000) | (r & 0xFFFF), (edx & 0xFFFF0000) | (r >> 16)
    d = eax & 0xFFFF if w == 1 else ((edx & 0xFFFF) << 16) | (eax & 0xFFFF)
    b = s & mask
    if signed:
        d, b = _sx(d, 2 * bits), _sx(b, bits)
    if b == 0:
        return None
    q, r = _trunc_div(d, b) if signed else (d // b, d % b)
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, mask)
    if not lo <= q <= hi:
        return None
    if w == 1:
        return (eax & 0xFFFF0000) | ((r & 0xFF) << 8) | (q & 0xFF), edx
    return (eax & 0xFFFF0000) | (q & 0xFFFF), (edx & 0xFFFF0000) | (r & 0xFFFF)


VALUES = [0, 1, 2, 3, 0x7F, 0x80, 0xFE, 0xFF, 0x1234, 0x7FFF, 0x8000, 0xFFFF,
          0x12345678, 0xDEADBEEF, 0xFFFFFF80]


@unittest.skipUnless(shutil.which("cc"), "no C compiler")
class NarrowMulDiv(unittest.TestCase):
    def test_sweep(self):
        cases, body = [], []
        for m in ("mul", "imul", "div", "idiv"):
            for w, src in ((1, _reg("bl", 1)), (2, _reg("bx", 2))):
                stmt = _lift(m, src)
                body.append(f"static void f_{m}{w}(void) {{ {stmt} }}")
                for eax in VALUES:
                    for s in VALUES:
                        want = _want(m, w, eax, 0x55AA1234, s)
                        if want is not None:
                            cases.append((m, w, eax, s, want))
        main = ["int main(void) {"]
        for m, w, eax_v, s, _ in cases:
            main.append(f"eax = 0x{eax_v:X}u; edx = 0x55AA1234u; ebx = 0x{s:X}u;"
                        f" f_{m}{w}(); printf(\"%08X %08X\\n\", eax, edx);")
        main.append("return 0; }")
        src = (PRELUDE + "static uint32_t eax, edx, ebx;\n"
               + "\n".join(body) + "\n" + "\n".join(main))
        with tempfile.TemporaryDirectory() as tmp:
            c, exe = os.path.join(tmp, "t.c"), os.path.join(tmp, "t")
            with open(c, "w") as f:
                f.write(src)
            subprocess.run(["cc", "-O1", "-o", exe, c], check=True)
            out = subprocess.run([exe], check=True, capture_output=True,
                                 text=True).stdout.split("\n")
        for (m, w, eax_v, s, want), line in zip(cases, out):
            self.assertEqual(line, "%08X %08X" % want,
                             f"{m} r/m{w * 8}: eax {eax_v:#x} src {s:#x}")

    def test_imul_dl_leaves_edx_alone(self):
        stmt = _lift("imul", _reg("dl", 1))
        self.assertNotIn("edx =", stmt.replace("SET_LO16(eax", ""), stmt)


if __name__ == "__main__":
    unittest.main()
