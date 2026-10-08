"""`in` / `out` call the runtime instead of vanishing.

They were RECOMP_UNIMPL, so `in al, dx` left al holding whatever it held
before. Conker's D3D vblank handler reads the TV field from bit 5 of port
0x80C0 that way, and XMV refused to start its clock on the stale value, so
the intro movie never released a frame.

Pinned here:
  1. `in` with a dx or immediate port reads through xbox_PortIn, passing the
     register's old value (which the runtime returns for an unmodelled port)
     and writing only the instruction's width;
  2. `out` writes through xbox_PortOut;
  3. the string forms stay unimplemented -- they are what a linear sweep
     reads over data -- and so does anything with a non-accumulator register.
"""
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, op_str, operands, address=0x00540FED):
    lifter = Lifter()
    insn = Instruction(address, 1, mnemonic, op_str, "",
                       operands=list(operands))
    return lifter, " ".join(lifter.lift_instruction(insn))


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


class PortIoTest(unittest.TestCase):

    def test_in_al_dx_reads_the_port_into_al_only(self):
        lifter, out = _lift("in", "al, dx", [_reg("al"), _reg("dx")])
        self.assertIn("SET_LO8(eax, xbox_PortIn(LO16(edx), 1, LO8(eax)));", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_in_eax_dx_replaces_eax(self):
        _, out = _lift("in", "eax, dx", [_reg("eax"), _reg("dx")])
        self.assertIn("eax = xbox_PortIn(LO16(edx), 4, eax);", out)

    def test_in_ax_imm_writes_the_low_word(self):
        _, out = _lift("in", "ax, 0x36", [_reg("ax"), _imm(0x36)])
        self.assertIn("SET_LO16(eax, xbox_PortIn(0x36u, 2, LO16(eax)));", out)

    def test_out_dx_al_writes_the_port(self):
        lifter, out = _lift("out", "dx, al", [_reg("dx"), _reg("al")])
        self.assertIn("xbox_PortOut(LO16(edx), 1, LO8(eax));", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_out_imm_eax(self):
        _, out = _lift("out", "0xb2, eax", [_imm(0xB2), _reg("eax")])
        self.assertIn("xbox_PortOut(0xB2u, 4, eax);", out)

    def test_string_forms_stay_unimplemented(self):
        # Negative control: insb/outsd are data on every title seen so far,
        # and a translation would make the runtime log them as port traffic.
        lifter, out = _lift("insb", "byte ptr es:[edi], dx",
                            [Operand(type="mem", mem_base="edi", mem_size=1),
                             _reg("dx")])
        self.assertIn("RECOMP_UNIMPL", out)
        self.assertIn("insb", lifter.unimplemented)

    def test_non_accumulator_register_stays_unimplemented(self):
        # `in` only exists with al/ax/eax; anything else is a decode of data.
        lifter, out = _lift("in", "bl, dx", [_reg("bl"), _reg("dx")])
        self.assertIn("RECOMP_UNIMPL", out)
        self.assertIn("in", lifter.unimplemented)


if __name__ == "__main__":
    unittest.main()
