# FREAK: a paraphonic engine in the spirit of the MicroFreak (design)

Status: feasibility study, 2026-11-01; nothing built. The maintainer asked: "para el siguiente engine, estoy pensando en hacer un emulador
de microfreak? es posible en este hardware?". The answer recorded here: an engine **inspired by** Arturia's MicroFreak fits this hardware
well; an exact clone does not, for licence reasons more than CPU. "MicroFreak" is Arturia's trademark and is used here only to describe the
idea, never as the name of an app or a screen. The first step is M5 (README: "a port, Plaits is the obvious candidate, to prove the unit
API"). The vocoder engine (`docs/VOCODER-DESIGN.md`) comes first in time; this one waits for M5's measurements.

## 1. What fits

- **The oscillators.** Mutable Instruments' Plaits (MIT, compatible with the kit's GPL-3.0-or-later; read each file's header at port
  time, `dsp-port` Gate 1) has engines for most of what the MicroFreak's digital oscillator does: virtual analogue, waveshaping, two-operator
  FM, formant, harmonic (additive), wavetable, chords, speech, string, modal and noise. Plaits runs one voice on a Cortex-M4F at 72 MHz and
  48 kHz: 1,500 cycles a sample. The MK66 has about 4,080 (180 MHz at 44.1 kHz), 2.7 times as many.
- **The filter.** The MicroFreak's filter is analogue, a 12 dB multimode in the Oberheim SEM manner, one for all voices. Here it is a
  zero-delay-feedback state-variable filter (low, band, high pass) with a saturating stage, a few per cent of a block [est].
- **The modulation.** The matrix, a cycling envelope, LFOs, the arpeggiator and a step sequencer run at control rate: next to nothing.
- **The memory.** o3-303's image has about 580 KiB of `.text` of the 1 MiB program flash (build of 2026-11-01); Plaits' whole firmware fits
  the 256 KB of its own chip. Sample and granular engines take the 16 MiB SDRAM (`O3SDK_BIG`).

## 2. The limits

- **Paraphony.** A Plaits voice is estimated at 5-12 % of a block with a light engine and 20-35 % with a heavy one [est; MK66 flash is slow,
  so the hot code may need SRAM, as o3-303's `.ramfunc` does]. Four heavy voices are 80-140 % of a block: they do not fit. Four light ones
  do. The voice count is set per engine (for example 4 light, 2 heavy), and only after each engine is measured on the unit.
- **The keyboard.** The pads give press and release only (`hal::Input`, `docs/FRAMEWORK.md` "Input"): no velocity, no pressure. The
  MicroFreak's pressure arrives by MIDI or becomes an assignable modulation source.
- **The closed engines.** Arturia's own oscillator types and those Arturia licensed from Noise Engineering are closed code. They can be
  recreated from scratch (a supersaw, a wavetable with our own tables, granular on our own samples are known techniques), from a
  description and by ear, never from their firmware or factory tables (CLAUDE.md, Hard rules, apply to every vendor, not only Polyend).
- **Controls.** One encoder against the MicroFreak's knobs: pages and F-keys, as o3-303 does.

## 3. The path

1. **M5:** port Plaits as a unit with `dsp-port`: float only, 48 kHz to 44.1 kHz, its sub-blocks inside the 64-frame block, a host oracle test
   against the original, one voice. Measure every engine on the unit (`o3console.py cpu`).
2. Decide the voice count per engine from those cycles.
3. The app, in its own repository (`tools/new_app.py`, as `../o3-303`): the filter, the matrix, the cycling envelope, the arpeggiator and
   sequencer, reusing the kit's delay, the skin and MIDI. The own engines (supersaw, wavetable, granular) last.
4. The vocoder engine can live inside this app too (the MicroFreak itself has a vocoder mode); see `docs/VOCODER-DESIGN.md` section 9.

Every figure marked [est] is an estimate from cycle counts; none is measured on a unit.
