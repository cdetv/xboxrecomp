"""Every branch out of a final body must land on an entry.

The translator lifts a jmp or jcc outside [start, end) as a tail call, and a
target that is no function becomes a stub that only pops the return address.
Conker's 0x00305CD0 was split at a gap prologue (0x00305D5C) after the
tail-jump pass had run, so its "jz 0x00305D9C" (pop ebp; pop esi; pop ebx;
ret) now left the body for a stub: the three registers stayed pushed and the
caller crashed later. 309 of Conker's 313 stubs were such real code.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, size, target=None, cond=False):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        # As the engine sets them: is_jump is "jmp" only, a jcc is not one.
        self.is_jump = target is not None and not cond
        self.is_cond_jump = target is not None and cond
        self.jump_target = target


class _Section:
    executable = True
    virtual_addr = 0x00300000
    virtual_size = 0x10000


class _Image:
    def get_section_at_va(self, addr):
        return _Section()


class _Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class _Engine:
    def __init__(self, insns, returning=()):
        self.instructions = {i.address: i for i in insns}
        self._returning = set(returning)

    def get_instructions_in_range(self, start, end):
        return [i for a, i in sorted(self.instructions.items())
                if start <= a < end]

    def probes_as_returning_body(self, addr, max_insns=256):
        return addr in self._returning

    def probes_as_vcall_thunk(self, addr):
        return False

    def block_tail_jump(self, addr, max_insns=256):
        return None


def _detector(insns, functions, returning=()):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns, returning)
    det.image = _Image()
    det.functions = {f.start: f for f in functions}
    det._alias_entries = {}
    return det


class BodyExitAliasTest(unittest.TestCase):
    def test_jcc_into_the_split_off_half_becomes_an_alias(self):
        # The crash shape: the first half's jz lands inside the second half.
        insns = [_Insn(0x00305CD0, 2),
                 _Insn(0x00305D40, 2, target=0x00305D9C, cond=True),
                 _Insn(0x00305D5C, 1),
                 _Insn(0x00305D9C, 3), _Insn(0x00305D9F, 1)]
        funcs = [_Func(0x00305CD0, 0x00305D5C), _Func(0x00305D5C, 0x00305DA0)]
        det = _detector(insns, funcs)
        self.assertTrue(det._pass_body_exit_aliases())
        self.assertEqual(det._alias_entries, {0x00305D9C: 0x00305DA0})

    def test_branch_inside_its_own_body_adds_nothing(self):
        insns = [_Insn(0x00305CD0, 2, target=0x00305D00, cond=True),
                 _Insn(0x00305D00, 1)]
        det = _detector(insns, [_Func(0x00305CD0, 0x00305D10)])
        self.assertFalse(det._pass_body_exit_aliases())

    def test_branch_to_a_function_start_adds_nothing(self):
        insns = [_Insn(0x00305CD0, 5, target=0x00305E00),
                 _Insn(0x00305E00, 1)]
        funcs = [_Func(0x00305CD0, 0x00305CD5), _Func(0x00305E00, 0x00305E10)]
        det = _detector(insns, funcs)
        self.assertFalse(det._pass_body_exit_aliases())

    def test_loop_head_before_an_alias_start_takes_the_furthest_end(self):
        # An alias at 0x00305D20 shares its owner's tail; its backward branch
        # to the owner's loop head at 0x00305D10 must become an entry too,
        # running to the owner's end (the furthest of the bodies holding it).
        insns = [_Insn(0x00305D00, 1), _Insn(0x00305D10, 2),
                 _Insn(0x00305D20, 2, target=0x00305D10, cond=True),
                 _Insn(0x00305D22, 1)]
        funcs = [_Func(0x00305D00, 0x00305D40), _Func(0x00305D20, 0x00305D40)]
        det = _detector(insns, funcs)
        self.assertTrue(det._pass_body_exit_aliases())
        self.assertEqual(det._alias_entries, {0x00305D10: 0x00305D40})

    def test_gap_target_needs_a_body_that_returns(self):
        insns = [_Insn(0x00305CD0, 2, target=0x00305D80, cond=True),
                 _Insn(0x00305D80, 1)]
        funcs = [_Func(0x00305CD0, 0x00305CE0), _Func(0x00305E00, 0x00305E10)]
        det = _detector(insns, funcs)
        self.assertFalse(det._pass_body_exit_aliases())
        det = _detector(insns, funcs, returning={0x00305D80})
        self.assertTrue(det._pass_body_exit_aliases())
        self.assertEqual(det._alias_entries, {0x00305D80: 0x00305E00})

    def test_a_target_mid_instruction_is_ignored(self):
        insns = [_Insn(0x00305CD0, 2, target=0x00305D81, cond=True),
                 _Insn(0x00305D80, 2)]
        funcs = [_Func(0x00305CD0, 0x00305CE0), _Func(0x00305D00, 0x00305D90)]
        det = _detector(insns, funcs)
        self.assertFalse(det._pass_body_exit_aliases())


if __name__ == "__main__":
    unittest.main()
