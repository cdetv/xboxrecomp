# xbox_apu — MCPX APU Audio Emulation

Hardware emulation of the Xbox's MCPX APU (Audio Processing Unit), extracted from [xemu](https://github.com/xemu-project/xemu). The APU is a custom chip with three processors:

- **VP (Voice Processor)** — 256 hardware voices, ADPCM/PCM decode, pitch shifting, envelopes, HRTF 3D audio
- **GP (Global Processor)** — DSP for global effects (reverb, chorus). Currently **stubbed** — effects bypass. The command doorbell's copy commands (3 = load, 2 = copy) are done, so GP X memory holds the effect states DirectSound keeps in its shared copy of the image (`apu_dsp.c`; `RECOMP_APU_DSP_COPY=0` = acknowledge only, `RECOMP_APU_DSP_LOG=1|2` = `[APUDSP]` lines). No DSP56300 core runs: `apu_fx.c` recognises the effects of an XDK 5849 image by their code (gain, mixer, filter, crosstalk, I3DL2 reverb; a C port of yacineolivier-maker's MIT `dsp_image.cpp`) and runs them on the bins, reading their state from that X memory. `RECOMP_APU_PROCESSING=full` plays the chain's speakers (folded to stereo) instead of the monitor mix, and `xbox_ApuSetAudioMode` switches between the two live, crossfaded over 21 ms (the monitor mix rests while only Full DSP plays; Full DSP has its own stop tails per bin and the raw HRTF ears, `RECOMP_APU_DSP_HRTF_NORM=1` the normalised ones); `RECOMP_APU_FX_LOG=1|2` = `[APUFX]` lines, `RECOMP_APU_FX_OFF=<kind>,..` leaves effects out. The I3DL2 listener comes from the game (`xbox_ApuSetI3dl2Listener`). Output (`xbox_ApuSetAudioMode`'s second value; `RECOMP_APU_OUTPUT=stereo|headphones|surround|mono` until the first call): Headphones fades the crosstalk's cross term out (the HRTF ears straight to their sides; Standard has no crosstalk, so it plays as Stereo); Mono makes both sides 0.5 x (L + R) on the stereo frame before the output gain, glided over 21 ms, with Full DSP's cross term out as Headphones (summed, it would only colour 3D sounds); a speaker test (`xbox_ApuSpeakerTestSound` hands over a mono sound once, `xbox_ApuSpeakerTest` plays it on one speaker, both front ones or each in turn, the game's output faded out under it; `xbox_ApuOutputChannels`; `APU_HOST_VERSION` 4) sits before Mono and the master volume and touches nothing until the first test; 5.1 surround is fixed at the start (the kernel answers `XC_AUDIO` with surround + AC3, so DirectSound pans 3D voices front / back), always plays Full DSP and sends the six speaker bins as six channels where the device has six speakers (XAudio2 opened with the device's channels and our own output matrix, surrounds on the side pair; never XAudio2's default matrix; a stereo device with spatial sound on -- Windows Sonic / Dolby Atmos / DTS, whose spatial client offers movable objects -- is opened as 7.1, since Windows 10 still offers apps its 2 channels and renders a 7.1 stream spatially; its mastering voice still reports the device's mask 0x3, so a mask naming fewer speakers than the voice has gives way to Windows' usual layout), else Full DSP's stereo fold; `RECOMP_APU_CHANNELS=6|2` forces six (a 5.1 WAV on a stereo PC) or the fold.
- **EP (Encode Processor)** — DSP for AC3/DTS encoding. Currently **stubbed** — passthrough.

Audio output uses XAudio2 (`apu_xaudio2.c`; Windows `waveOut` when it is missing) at 48 kHz 16-bit, stereo or (5.1 surround) six channels. `RECOMP_APU_WAV=<file>` captures what goes out (a 5.1 run: a 6-channel WAVE_FORMAT_EXTENSIBLE file, FL FR C LFE SL SR).

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `apu.h` | 70 | **Public API** — init, shutdown, MMIO, mixer voices |
| `apu_state.h` | 525 | APU state struct (VP, GP, EP, DSP core, 256 voices) |
| `apu_regs.h` | 366 | MCPX APU register definitions and field masks |
| `apu_core.c` | 762 | MMIO handlers, frame thread, waveOut output, test tone |
| `apu_vp.c` | 1,247 | Voice Processor — decode, resample, mix, envelopes, HRTF |
| `apu_dsp.c` | 90 | GP/EP stubs (mixbin 0/1 passthrough) |
| `apu_fx.c` | 860 | Native GP effects: the effects image's chain run in C (MIT) |
| `apu_mmio_hook.c` | 253 | VEH x86-64 instruction decoder for 0xFE800000+ MMIO |
| `apu_shim.h` | 431 | QEMU compatibility (threads, atomics, clocks → Win32) |
| `apu_debug.h` | 104 | Debug logging macros |
| `fpconv.h` | 70 | IEEE 754 float/int conversion utilities |

## Quick Start

```c
#include "apu.h"

// Initialize APU with pointer to Xbox RAM
// (ram_ptr = native pointer to Xbox VA 0x00000000)
MCPXAPUState *apu = mcpx_apu_init_standalone(ram_ptr);

// Test: play a 440Hz sine tone
mcpx_apu_play_test_tone(apu);

// --- OR use the software mixer for direct PCM playback ---

// Allocate a mixer voice
int voice = apu_mixer_alloc_voice();
APUMixerVoice *v = apu_mixer_get_voice(voice);

// Configure it
v->pcm_data = my_pcm_buffer;     // 16-bit signed PCM
v->pcm_bytes = buffer_size;
v->num_channels = 2;              // 1=mono, 2=stereo
v->sample_rate = 44100;           // Source sample rate
v->volume = 0.8f;                 // 0.0 - 1.0

// Play
apu_mixer_play(voice, 1);  // 1 = looping

// Stop and free
apu_mixer_stop(voice);
apu_mixer_free_voice(voice);

// Shutdown
mcpx_apu_shutdown(apu);
```

## Architecture

```
┌─────────────────────────────────────────────────┐
│                    Xbox Game                     │
│  DirectSound API calls (IDirectSound8)           │
└────────────────────┬────────────────────────────┘
                     │ writes to MMIO registers
                     ▼
┌─────────────────────────────────────────────────┐
│              APU Register Space                  │
│  0xFE800000 - 0xFE87FFFF (512 KB)               │
│  Intercepted via VEH (apu_mmio_hook.c)           │
└────────┬───────────┬───────────┬────────────────┘
         │           │           │
         ▼           ▼           ▼
   ┌──────────┐ ┌─────────┐ ┌─────────┐
   │    VP    │ │   GP    │ │   EP    │
   │256 voices│ │  (stub) │ │  (stub) │
   │ ADPCM/  │ │  effects│ │  encode │
   │  PCM    │ │  bypass │ │  bypass │
   └────┬─────┘ └────┬────┘ └────┬────┘
        │             │           │
        └──────┬──────┘───────────┘
               │ mixed samples
               ▼
   ┌──────────────────────────────┐
   │    waveOut (48kHz stereo)    │
   │  4 × 2048-sample buffers    │
   └──────────────────────────────┘
```

## Two Audio Paths

### 1. Hardware APU Emulation (MMIO)

For games that program the APU directly. The game writes to MMIO registers at 0xFE800000+, which are intercepted by the VEH hook and routed to the APU emulation:

```c
// Game writes: voice 5, set buffer address
*(uint32_t*)(0xFE802000 + 5 * voice_stride + VOICE_BUF_OFFSET) = buffer_addr;

// VEH intercepts → mcpx_apu_mmio_write() → VP processes voice
```

The VP runs in a separate thread, mixing active voices at the hardware frame rate.

### 2. Software Mixer (Direct API)

For games where you've decoded the audio format yourself and just want to play PCM. The mixer provides 64 voice slots with:

- 16-bit signed PCM input (mono or stereo)
- 16.16 fixed-point resampling (any rate → 48kHz)
- Per-voice volume control
- Looping support

```c
int apu_mixer_alloc_voice(void);                    // Returns slot 0-63, or -1
void apu_mixer_free_voice(int slot);
APUMixerVoice *apu_mixer_get_voice(int slot);       // Configure voice params
void apu_mixer_play(int slot, int looping);          // Start playback
void apu_mixer_stop(int slot);                       // Stop playback
```

## MMIO Hook

The APU MMIO hook (`apu_mmio_hook.c`) uses Windows Vectored Exception Handling to intercept memory accesses to the APU register range:

```
Access to 0xFE800000+
  → Page fault (no physical memory there)
  → VEH handler catches EXCEPTION_ACCESS_VIOLATION
  → Decodes the faulting x86-64 instruction (MOV, ADD, XOR, etc.)
  → Routes to mcpx_apu_mmio_read() or mcpx_apu_mmio_write()
  → Advances RIP past the instruction
  → Resumes execution
```

This allows recompiled code to access APU registers using normal memory operations, exactly as the original Xbox code did.

## Voice Processor Details

The VP processes 256 hardware voices per frame:

- **ADPCM decode**: Xbox ADPCM (4-bit, 64-sample blocks)
- **PCM formats**: 8-bit unsigned, 16-bit signed, 24-bit
- **Pitch**: Log2 fixed-point (4.12 format). Pitch 0 = 48kHz native rate.
- **Envelopes**: Multi-segment amplitude envelopes (attack, decay, sustain, release)
- **HRTF**: Head-Related Transfer Function for 3D positional audio
- **Mixbins**: 32 output channels, voices route to any combination
- **Filters**: Per-voice low-pass/high-pass (IIR biquad)

## Register Map (Key Registers)

```
Offset      Name                   Description
──────────────────────────────────────────────────
0x1000      NV_PAPU_ISTS           Interrupt status
0x1100      NV_PAPU_FECTL          Frontend control
0x2000      NV_PAPU_SECTL          Synth engine control
0x202C      NV_PAPU_VPVADDR        VP voice address base
0x3000+     NV_PAPU_GP*            Global Processor registers
0x4000+     NV_PAPU_EP*            Encode Processor registers
```

Full register definitions in `apu_regs.h`.

## Dependencies

- `qemu_shim.h` (from `../nv2a/`) — QEMU type abstraction layer
- `kernel32.lib` — Win32 threading
- `winmm.lib` — waveOut audio output
