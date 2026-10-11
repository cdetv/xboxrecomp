# apu_mixdown

`mcpx_apu_dsp_frame` used to read mixbins 0 and 1 and discard 2..31. On
hardware the GP and EP mix the submixes down; here they are stubs, so thirty
of thirty-two bins were computed correctly and thrown away every frame, with
no counter anywhere to say so.

Titles route their 3D positional voices — their sound effects — to bins above
1. Measured on Jet Set Radio Future over one 200 s gameplay run:

    [APU-BIN] 2D heard=557466 lost=0
              3D heard=0      lost=377768
              lost by bin: 6,7,8,9,10

Music is on 2D voices and lands in bins 0 and 1, which is why it was always
audible while no effect ever was.

## Then: even bins left, odd bins right

The first fix summed even bins to the left and odd bins to the right. That
keeps the 6/7 and 8/9 pairs, but in DirectSound's DSMIXBIN layout bin 2 is
FRONT_CENTER, 3 LOW_FREQUENCY and 10 the I3DL2 reverb send. Conker: Live &
Reloaded routes dialogue to the centre bin and every 3D voice to LFE and
I3DL2 at its direct level, so voices and effects came out on the left only
(RMS 6 dB louder in the field, 9.5 dB in a dialogue cutscene) and positional
sounds were summed two or three times. The mixdown is now a speaker table
(`mixdown_init` in `src/apu/apu_dsp.c`): fronts and XTLK bins to their
side, centre to both at -3 dB, backs at -3 dB, FXSEND centred at -3 dB, LFE
and I3DL2 not fed dry (`RECOMP_APU_MIX_LFE` / `RECOMP_APU_MIX_I3DL2` set a
gain). `RECOMP_APU_MIXDOWN=evenodd` brings the old sum back.

## Running

    cmake -S tests/apu_mixdown -B build/apu-mixdown
    cmake --build build/apu-mixdown
    ctest --test-dir build/apu-mixdown

Three tests. `apu_mixdown_speakers` puts a signal in single bins and checks
each lands where the table says (3D bins 6..10 on both channels, centre equal
on both sides, XTLK left/right on their side, LFE and I3DL2 silent by
default). `apu_mixdown_evenodd` requires the old sum to put the centre on the
left only, and `apu_mixdown_two_bins` requires bins 6..10 to be *lost* with
`RECOMP_APU_MIXDOWN_ALL=0`. Without those controls, a mixdown that ignored
its own switch would pass.
