"""An unresolved branch target that is a shared epilogue is lifted, not stubbed.

MC3's race-event listener sub_0023D630 leaves through `jne 0x23D72D` onto
`pop edi; pop esi; ret 4`. Those bytes sat inside a falsely split
neighbour, so they became a stub that only did the `ret 4`: both pops were
skipped, every race event returned with esi/edi wrong and esp off, and the
game crashed starting any career race (2026-10-21).
"""
from . import config
from .translator import FunctionTranslator

BASE = 0x10000


def translator_for(code_hex):
    image = bytes.fromhex(code_hex)
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='stub-epilogue-test')
    return FunctionTranslator(image, {})


def test_pops_then_ret_is_an_epilogue():
    t = translator_for('5f5ec20400')              # pop edi; pop esi; ret 4
    assert t._stub_epilogue_end(BASE) == BASE + 5


def test_leave_ret_is_an_epilogue():
    t = translator_for('c9c21000')                # leave; ret 0x10
    assert t._stub_epilogue_end(BASE) == BASE + 4


def test_a_bare_ret_stays_a_stub():
    t = translator_for('c20400')                  # the stub is exact there
    assert t._stub_epilogue_end(BASE) is None


def test_control_flow_before_the_ret_stays_a_stub():
    assert translator_for('5f7402c3c3')._stub_epilogue_end(BASE) is None   # jz
    assert translator_for('5fe800000000c3')._stub_epilogue_end(BASE) is None  # call


def test_the_lifted_epilogue_pops_both_registers():
    t = translator_for('5f5ec20400')
    code = t.translate_function(BASE, {"start": hex(BASE), "end": BASE + 5,
                                       "size": 5, "_addr": BASE,
                                       "name": "sub_00010000"})
    assert code and "POP32(esp, edi)" in code and "POP32(esp, esi)" in code
