"""
Self-check: `push ecx` + a callee-saved push counts as a prologue.

Run: python3 -m pytest tools/disasm/test_prologue_push_ecx.py

MC3's 0x00123420 starts `push ecx; push esi; mov esi, ecx` right after a
switch's byte table. The sweep decodes the table as code and runs over the
function's first byte, so its seed lands "inside an instruction" and is kept
only if the bytes probe as a prologue. They did not, and the seed was dropped.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.test_decode_at import _engine, BASE  # noqa: E402


def test_push_ecx_then_register_save_is_a_prologue():
    eng, _ = _engine(BASE, bytes.fromhex("51568bf1e800000000c3"))   # push ecx; push esi; mov esi,ecx
    assert eng.probes_as_prologue(BASE)


def test_push_ecx_alone_is_not():
    eng, _ = _engine(BASE, bytes.fromhex("518b442408c3"))           # push ecx; mov eax,[esp+8]
    assert not eng.probes_as_prologue(BASE)
