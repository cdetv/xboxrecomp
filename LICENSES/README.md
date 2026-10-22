# Third-party licence texts

`LGPL-2.1.txt` is the verbatim GNU Lesser General Public License v2.1, from

    https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt

It applies to the files listed under "LGPL-2.1-or-later" in the top-level
NOTICE — the MCPX APU sources and `src/nv2a/nv2a_regs.h`, extracted from xemu
and remaining the copyright of espes, Jannik Vogel and Matt Borgerson.

Shipping the verbatim text alongside those files is an LGPL requirement, not
a courtesy.

`FidelityFX-CAS.txt` is AMD's MIT licence and copyright notice for FidelityFX
CAS, whose sharpening filter `src/kernel/postfx_d3d11.c` ports (see NOTICE).

`FXAA-3.11.txt` repeats NVIDIA's notice from the top of
`third_party/fxaa/Fxaa3_11.h`, the anti-aliasing shader the filter chain
compiles (see NOTICE).

`SMAA.txt` is the MIT licence and copyright notice of SMAA (Jimenez et al.),
the other anti-aliasing shader, vendored in `third_party/smaa/` with its
own copy of the same text (see NOTICE).

`FidelityFX-FSR.txt` is AMD's MIT licence and copyright notice for
FidelityFX Super Resolution 1.0, the upscaler (EASU) and its sharpener
(RCAS), vendored in `third_party/fsr1/` with AMD's own `license.txt` (see
NOTICE).
