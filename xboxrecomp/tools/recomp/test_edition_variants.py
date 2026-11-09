"""--edition-variants: a jcc that differs between editions of a disc is
lifted with both conditions, chosen at run time by g_recomp_xbe_variant
(MC3 Remix: `ja` in Europe, `jae` in the USA, 2026-10-25)."""
from tools.recomp import config, lifter
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000


def translate(image):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='edition-variant-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_jcc_with_an_edition_variant():
    # cmp al, 0x1e ; ja +1 ; nop ; ret   (the ja at BASE+2)
    code_bytes = bytes.fromhex('3c1e' '7701' '90' 'c3')
    lifter.EDITION_VARIANT_JCC.clear()
    lifter.EDITION_VARIANTS_APPLIED.clear()
    lifter.EDITION_VARIANT_JCC[BASE + 2] = [(1, 'jae')]
    try:
        code = translate(code_bytes)
    finally:
        lifter.EDITION_VARIANT_JCC.clear()
    assert 'g_recomp_xbe_variant == 1 ?' in code, code
    assert 'CMP_AE' in code and 'CMP_A(' in code, code
    assert BASE + 2 in lifter.EDITION_VARIANTS_APPLIED


def test_lifted_from_a_variant_disc():
    # The USA disc's bytes (jae at BASE+2) lift to the same C as Europe's ja.
    lifter.EDITION_VARIANT_JCC.clear()
    lifter.EDITION_VARIANT_BASE.clear()
    try:
        lifter.EDITION_VARIANT_JCC[BASE + 2] = [(1, 'jae')]
        lifter.EDITION_VARIANT_BASE[BASE + 2] = 'ja'
        usa = translate(bytes.fromhex('3c1e' '7301' '90' 'c3'))
        eur = translate(bytes.fromhex('3c1e' '7701' '90' 'c3'))
    finally:
        lifter.EDITION_VARIANT_JCC.clear()
        lifter.EDITION_VARIANT_BASE.clear()
    code = lambda c: [l.split('/*')[0] for l in c.splitlines()]   # comments name the mnemonic
    assert 'g_recomp_xbe_variant == 1 ?' in usa and code(usa) == code(eur), (usa, eur)


def test_without_variants_nothing_changes():
    lifter.EDITION_VARIANT_JCC.clear()
    code = translate(bytes.fromhex('3c1e' '7701' '90' 'c3'))
    assert 'g_recomp_xbe_variant' not in code, code
