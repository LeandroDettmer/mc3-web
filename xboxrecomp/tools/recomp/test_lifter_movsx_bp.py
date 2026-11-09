"""movsx from a 16-bit register sign-extends -- for every one of them.

_lift_movsx listed ax, bx, cx, dx, si, di as 16-bit sources and missed bp and
sp, so `movsx ebp, bp` lifted to `ebp = LO16(ebp)`: a zero extension. A
negative 16-bit value (0xFFFD = -3) came out as 65533. Found in Midnight Club
3's Bink IDCT (0x0037F6BC), where it turned every block with a negative
coefficient in that row into wrong-colour squares; five more sites were in
the game's own code. _lift_movzx already had all eight.
"""

import pytest

from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter


def _reg(name, size):
    op = Operand("reg")
    op.reg = name
    op.size = size
    return op


def _movsx(dst, src):
    insn = Instruction(0, 3, "movsx", f"{dst}, {src}", "0fbfed")
    insn.operands = [_reg(dst, 4), _reg(src, 2)]
    return insn


@pytest.mark.parametrize("src", ["ax", "bx", "cx", "dx", "si", "di", "bp", "sp"])
def test_movsx_sign_extends_every_16_bit_register(src):
    body = "".join(Lifter().lift_instruction(_movsx("ebp", src)))
    assert "SX16(" in body, body


def test_movsx_ebp_bp_is_not_a_zero_extension():
    body = "".join(Lifter().lift_instruction(_movsx("ebp", "bp")))
    assert body.replace(" ", "") != "ebp=LO16(ebp);", body
