"""RECOMP_INLINE: small leaves get a static copy, direct calls reach it.

Only leaves (no calls of any kind) at most n instructions qualify; wrapped
functions keep their wrapper; calls to anything else are left alone.
"""
from .translator import _inline_candidates, _inline_calls


def _fn(name, insns, body):
    return (f"/**\n * {name}\n * Original: 0x0 - 0x1 (4 bytes, {insns} insns)\n */\n"
            f"void {name}(void)\n{{\n    RECOMP_PREEMPT();\n{body}\n}}\n")


def test_inline_leaves():
    leaf = _fn("sub_00010000", 3, "    eax = ecx;\n    return;")
    big = _fn("sub_00020000", 40, "    eax = ecx;")
    caller = _fn("sub_00030000", 3, "    sub_00010000();\n    sub_00020000();\n"
                 "    RECOMP_ABI_CALL(0x00010000u, sub_00010000);\n"
                 "    RECOMP_ABI_CALL(0x00020000u, sub_00020000);")
    icall = _fn("sub_00040000", 2, "    RECOMP_ICALL_SAFE_AT(eax, esp, 1);")
    wrapped = _fn("sub_00050000", 2, "    eax = 1;")
    tr = [(0x10000, "sub_00010000", leaf), (0x20000, "sub_00020000", big),
          (0x30000, "sub_00030000", caller), (0x40000, "sub_00040000", icall),
          (0x50000, "sub_00050000", wrapped)]

    assert _inline_candidates(tr, set(), 0) == {}, "off by default"
    got = _inline_candidates(tr, {0x50000}, 16)
    assert set(got) == {"sub_00010000"}, got
    assert "static inline __attribute__((always_inline)) void sub_00010000_inl(void)" \
        in got["sub_00010000"]
    assert "eax = ecx;" in got["sub_00010000"]

    out = _inline_calls(caller, got)
    assert "sub_00010000_inl();" in out
    assert "RECOMP_ABI_CALL(0x00010000u, sub_00010000_inl);" in out
    assert "sub_00020000();" in out, "a non-candidate keeps its call"
    assert "RECOMP_ABI_CALL(0x00020000u, sub_00020000);" in out
    assert "void sub_00030000(void)" in out, "the definition is not renamed"


if __name__ == "__main__":
    test_inline_leaves()
    print("ok")
