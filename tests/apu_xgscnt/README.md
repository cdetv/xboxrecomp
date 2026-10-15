# apu_xgscnt

`NV_PAPU_XGSCNT` (APU `+0x200C`) is the MCPX global sample counter: 48 kHz
output samples while the sample engine runs, set by the title (DirectSound
zeroes it at reset). XACT reads it as its clock (`count * 10^7 / 48000`, 100 ns
units) and schedules interactive-cue transitions on it; the XPP voice code
takes 48 counts per 1 ms USB frame.

The runtime (from xemu) returned `ns / 100`, a 10 MHz tick, and ignored the
write. XACT's clock then ran ~208 times fast and the 32-bit count wrapped every
7 minutes. In Conker: Live & Reloaded the barn pit music, an interactive XACT
cue, kept its next variation waiting forever and so never acted on its stop
(the music looped on into the next level).

Arms: `samples` (the fix) must pass; `wall` (`RECOMP_APU_XGSCNT=wall`, the old
read) is the negative control and must fail a check.
