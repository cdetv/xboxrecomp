"""cmovp/cmovnp after the lahf; test ah, 0x44 float-compare idiom."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000


def translate(image):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='cmov-parity-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image), '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_cmovnp_after_lahf_test():
    # Conker 0x00266E1A: ucomiss xmm1,xmm0; lahf; test ah,0x44; mov eax,ebx;
    # cmovnp eax,ecx; ret -- eax = (xmm1 != xmm0) ? ebx : ecx. The mov
    # between test and cmovnp overwrites ah, so the snapshot must be used.
    code = translate(bytes.fromhex('0f2ec89ff6c4448bc30f4bc1c3'))
    assert 'RECOMP_UNIMPL' not in code, code
    assert 'if ((!RECOMP_PARITY8((_fa) & (_fb)))) eax = ecx;' in code, code


def test_cmovp_after_lahf_test():
    code = translate(bytes.fromhex('0f2ec89ff6c4448bc30f4ac1c3'))
    assert 'RECOMP_UNIMPL' not in code, code
    assert 'if (RECOMP_PARITY8((_fa) & (_fb))) eax = ecx;' in code, code
