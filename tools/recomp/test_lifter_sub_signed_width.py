"""A signed jcc after an 8- or 16-bit sub compares at that width.

The sub arm rebuilds the original destination as result + source and hands
both to CMP_L/CMP_GE/CMP_LE/CMP_G. Those recover the operand width from
sizeof their arguments; (uint32_t) casts made every sub look 32 bits wide, so
`sub ch, cl; jl` with 5 - 7 compared 0x105 against 7 and never jumped. A
decoder that counts its bit buffer down in ch that way never refilled and ran
off the end of its output buffer.

Each fixture is translated, compiled against the CMP_* macros taken verbatim
from the runtime header, and run over operand pairs around every sign edge.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import pytest
from . import config
from .translator import FunctionTranslator
from .test_icall_guarded_runtime import _macro

# dest, source: al/cl, ch/cl, ax/cx, eax/ecx (sub r/m, reg)
SUBS = {'8lo': ('28c8', 8, 0), '8hi': ('28cd', 8, 8),
        '16': ('6629c8', 16, 0), '32': ('29c8', 32, 0)}
JCC = {'jl': '7c', 'jge': '7d', 'jle': '7e', 'jg': '7f'}
VALUES = (0, 1, 2, 5, 7, 0x7e, 0x7f, 0x80, 0x81, 0xfe, 0xff, 0x7fff, 0x8000,
          0xffff, 0x7fffffff, 0x80000000, 0xffffffff)


def _signed(v, width):
    v &= (1 << width) - 1
    return v - (1 << width) if v >> (width - 1) else v


def _taken(jcc, a, b, width):
    a, b = _signed(a, width), _signed(b, width)
    return {'jl': a < b, 'jge': a >= b, 'jle': a <= b, 'jg': a > b}[jcc]


def test_signed_jcc_after_narrow_sub():
    cc = shutil.which('clang') or shutil.which('gcc')
    if not cc and Path(r'C:\Program Files\LLVM\bin\clang.exe').exists():
        cc = r'C:\Program Files\LLVM\bin\clang.exe'
    if not cc:
        pytest.skip('C compiler unavailable')
    code = '''#include <stdint.h>
#include <stdio.h>
static uint32_t eax,ecx,esp;
#define LO8(v) ((uint8_t)(v))
#define HI8(v) ((uint8_t)((v)>>8))
#define LO16(v) ((uint16_t)(v))
#define SET_LO8(v,x) ((v)=((v)&0xffffff00u)|(uint8_t)(x))
#define SET_HI8(v,x) ((v)=((v)&0xffff00ffu)|((uint32_t)(uint8_t)(x)<<8))
#define SET_LO16(v,x) ((v)=((v)&0xffff0000u)|(uint16_t)(x))
'''
    for name in ('RECOMP_FLAG_WIDTH', 'RECOMP_SIGNED',
                 'CMP_L', 'CMP_GE', 'CMP_LE', 'CMP_G'):
        code += _macro(name) + '\n'
    names, cases = [], []
    base = 0x10000
    for sub_name, (sub, width, shift) in SUBS.items():
        for jcc, opcode in JCC.items():
            # sub; jcc taken; mov eax, 0; ret; taken: mov eax, 1; ret
            image = bytes.fromhex(sub + opcode + '06b800000000c3b801000000c3')
            config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                            entry_point=base, kernel_thunk_addr=base, origin='sub-signed-width')
            db = {base: {'start': hex(base), 'end': base + len(image), '_addr': base, 'size': len(image)}}
            name = f'fixture_{sub_name}_{jcc}'
            code += FunctionTranslator(image, db).translate_function(base, db[base]).replace('sub_00010000', name)
            for a in VALUES:
                for b in VALUES:
                    if shift:  # ch is the destination, cl the source
                        if a > 0xff or b > 0xff:
                            continue
                        eax_in, ecx_in = 0, (a << 8) | b
                    else:
                        eax_in, ecx_in = a, b
                    cases.append(f'{{{len(names)},0x{eax_in:x}u,0x{ecx_in:x}u,'
                                 f'{int(_taken(jcc, a, b, width))}u}}')
            names.append(name)
    code += f'static void (*fixtures[])(void)={{{",".join(names)}}};\n'
    code += f'static const struct {{ unsigned f; uint32_t eax, ecx, taken; }} cases[]={{{",".join(cases)}}};\n'
    code += '''int main(void) {
      for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        eax = cases[i].eax; ecx = cases[i].ecx; esp = 1024; fixtures[cases[i].f]();
        if (eax != cases[i].taken) {
          fprintf(stderr, "fixture %u eax %x ecx %x: taken %u, expected %u",
                  cases[i].f, cases[i].eax, cases[i].ecx, eax, cases[i].taken);
          return 1;
        }
      }
      return 0;
    }'''
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / 'test.c'
        source.write_text(code)
        exe = Path(temp) / 'test.exe'
        result = subprocess.run([cc, str(source), '-o', str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
