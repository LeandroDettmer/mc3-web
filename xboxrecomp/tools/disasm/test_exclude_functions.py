"""Self-check for --exclude-functions: an excluded address never becomes a
function start, whichever pass proposes it.

Run: py -3 -m pytest tools/disasm/test_exclude_functions.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


def _detector(excluded=()):
    det = FunctionDetector.__new__(FunctionDetector)
    det._candidates = {}
    det.excluded = set(excluded)
    return det


def test_excluded_address_is_no_candidate():
    det = _detector({0x003AEA35})
    det._add_candidate(0x003AEA35, 0.9, "call_target")
    det._add_candidate(0x003AE8C0, 0.9, "call_target")
    assert 0x003AEA35 not in det._candidates
    assert 0x003AE8C0 in det._candidates


def test_without_exclusions_nothing_changes():
    det = _detector()
    det._add_candidate(0x003AEA35, 0.9, "call_target")
    assert det._candidates[0x003AEA35] == (0.9, "call_target")


def test_excluded_address_is_no_alias_entry():
    class Engine:
        def get_instructions_in_range(self, lo, hi):
            return [object()]
    class Image:
        def get_section_at_va(self, va):
            return None
    class Labels:
        def get(self, va):
            return None
        def auto_name_function(self, *a):
            pass
    det = _detector({0x002C6C53})
    det.functions = {}
    det.engine, det.image, det.labels = Engine(), Image(), Labels()
    det._alias_entries = {0x002C6C53: 0x002C7037, 0x002C6D00: 0x002C7037}
    det._build_alias_entries()
    assert 0x002C6C53 not in det.functions
    assert 0x002C6D00 in det.functions
