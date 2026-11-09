"""SSE compare flags: LAHF after ucomiss, and a comiss consumer after a join."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000


def translate(image):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='sse-compare-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_lahf_after_ucomiss_loads_the_compare():
    # ucomiss xmm0, xmm1; lahf; test ah, 0x44; jnp +1; nop; ret
    code = translate(bytes.fromhex('0f2ec1' '9f' 'f6c444' '7b01' '90' 'c3'))
    assert 'SET_HI8(eax,' in code, code
    assert '_fca == _fcb' in code, code
    assert 'lahf - load AH' not in code, code


def test_jb_after_two_comiss_predecessors():
    # test ecx, ecx; jz alt; comiss xmm0, xmm1; jmp join;
    # alt: comiss xmm2, xmm0; join: jb +1; nop; ret
    code = translate(bytes.fromhex('85c9' '7405' '0f2fc1' 'eb03' '0f2fd0'
                                   '7201' '90' 'c3'))
    # jb is CF=1: below OR unordered (a NaN operand sets ZF, PF and CF).
    assert '(_fca < _fcb || (_fca != _fca || _fcb != _fcb))' in code, code
    assert '_flags /* jb' not in code, code


def test_unordered_conditions_follow_the_flags():
    # comiss xmm0, xmm1; jcc +1; nop; ret -- for each condition, the C test
    # must be true exactly when the hardware's flags make the jump. With a
    # NaN, ZF=PF=CF=1: jb/jbe/je/jp jump, ja/jae/jne/jnp do not
    # (conformance sse_comiss_*; MC3 Remix, 2026-10-25).
    un = '(_fca != _fca || _fcb != _fcb)'
    want = {
        '77': '(_fca > _fcb)',                    # ja
        '73': '(_fca >= _fcb)',                   # jae
        '72': '(_fca < _fcb || ' + un + ')',      # jb
        '76': '(_fca <= _fcb || ' + un + ')',     # jbe
        '74': '(_fca == _fcb || ' + un + ')',     # je
        '75': '(_fca < _fcb || _fca > _fcb)',     # jne
        '7a': un,                                 # jp
        '7b': '(!' + un + ')',                    # jnp
    }
    for op, cond in want.items():
        code = translate(bytes.fromhex('0f2fc1' + op + '01' '90' 'c3'))
        assert cond in code, (op, code)

