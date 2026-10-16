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

## Then: the default became xemu's monitor mix

Measured against a recording of xemu (Conker field and Birdy scenes), the
speaker table put 3D voices 4-6 dB too loud at their peaks. The default is
now `RECOMP_APU_MIXDOWN=xemu`, xemu's no-DSP monitor mix in `apu_vp.c`, which
matched within 0.5 dB. The table above is still selectable
(`RECOMP_APU_MIXDOWN=speakers`) and is what these tests exercise: the
speakers arm sets it explicitly.

## Then: the monitor mix's centre channel

xemu's monitor mix puts a voice's sample column 0 on the left and column 1 on
the right. Conker's 5.1 music (the Great Mighty Poo song) is three stereo
voices: front L/R to bins 0/1, centre + LFE to bins 2/3, back L/R to bins
4/5. The vocals, on the centre column, came out on the left only, and the LFE
column (its bin silent) on the right at the centre's gain. Now each column of
a stereo voice takes its own slots' gain and goes to its bin's side, centre
and LFE to both at -3 dB (`mcpx_apu_monitor_place`); mono voices and plain
left/right stereo are unchanged. `RECOMP_APU_MONITOR_STEREO=old` brings
xemu's placement back.

## Running

    cmake -S tests/apu_mixdown -B build/apu-mixdown
    cmake --build build/apu-mixdown
    ctest --test-dir build/apu-mixdown

Five tests. `apu_mixdown_speakers` puts a signal in single bins and checks
each lands where the table says (3D bins 6..10 on both channels, centre equal
on both sides, XTLK left/right on their side, LFE and I3DL2 silent by
default). `apu_mixdown_evenodd` requires the old sum to put the centre on the
left only, and `apu_mixdown_two_bins` requires bins 6..10 to be *lost* with
`RECOMP_APU_MIXDOWN_ALL=0`. Without those controls, a mixdown that ignored
its own switch would pass. `apu_mixdown_monitor` checks the monitor mix's
placement (a stereo voice's centre column equal on both sides at the same
power, its silent LFE column silent, front L/R columns on their side, mono
voices as before); `apu_mixdown_monitor_old` is its negative control and
requires xemu's placement to show the defect.
