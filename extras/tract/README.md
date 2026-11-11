# tract: a vocal-tract filter for any signal

A shared DSP for the o3-303 TRACT page and the o3-voc03er FORMANT effect. A mono signal goes in as the voice box's buzz (a synth voice, a
vocoder carrier, a sample), a tube of 22 (Full) or 11 (Lite) sections shapes it, and vowels, closures and consonant hiss come out, mixed with
the dry signal. It is opt-in: the sdk never builds it; a project that wants it does
`add_subdirectory(${O3SDK_DIR}/extras/tract ${CMAKE_BINARY_DIR}/extras_tract)` and links `unit_tract_dsp`
(`o3sdk_unit(tract_dsp ...)`, compiled `-O2 -ffp-contract=off`). Its tests (`tests/*_test.cpp`) are added by the project's `tests/CMakeLists.txt`.

## Parts

| File | What |
|---|---|
| `tube.h`, `tube.cpp` | The Kelly-Lochbaum one-multiplier lattice, N <= 28 sections, `sanitize()` |
| `shape.h`, `shape.cpp` | The area model (tongue, height, lips, three closures) and the five fitted vowel anchors A E I O U |
| `halfband.h`, `halfband.cpp` | 23-tap half-band decimator and interpolator (delay 11 samples of the faster rate per stage) |
| `processor.h`, `processor.cpp` | `Processor`: the whole chain, with the parameters below |
| `tools/fit_vowels.cpp`, `tools/design_halfband.py` | The host tools that produced the frozen constants |

## API

`Processor(Quality::Full | Quality::Lite)`, `prepare(fs)`, `reset()`, `set_vowel(0..1)`, `set_bend(tongue, jaw, lips)`,
`set_closure(lips | tip | back, on)`, `set_glide_ms`, `set_size(-2..+2)`, `set_res`, `set_hiss`, `set_mix`, `process(in, out, n)` (any n, in may
equal out). A NaN parameter takes its default. Bypass (`mix 0`) passes the signal bit for bit. Every block size gives the same samples.
The state is under 1.5 KB; there is no heap, no `double`.

## What differs from the plan, and why

- **Half-band length.** 23 taps (the plan said 15): 45 dB of image rejection from 0.3 fs needs them; the delay is 22 (Full) or 66 (Lite) frames.
- **Level trim.** The tube's gain varies by more than 20 dB between /i/ and /u/ and with RES; no fixed makeup follows that. The wet gain is
  a static `kMakeup (1/(1-r))^1.35` times a slow (60 ms) trim from the ratio of two power followers, held while a closure is on and while the
  source is silent. The level stays within 4 dB of the dry one over the five vowels and RES 0 to 1.
- **Closure.** A sealed tube still rings (a closed cavity has a pressure gain), so the lips' section is not enough: the wet gain also goes to
  `1 - closure` (silence in 12 ms, -40 dB within 30 ms measured), and the release gives the pop of the pressure that built up.
- **Passivity.** The one-multiplier pressure-wave lattice is not passive when the reflections change fast or at random (the plan's claim that
  it is was wrong; `tube_test.cpp` has the finding). It stays bounded under the 5 ms one-pole motion the processor uses; `Tube::sanitize()`
  clears a tube that is not finite or huge and the wet signal is clamped to +-8.
- **F3 of U.** A one-dimensional tube with F1 and F2 on the targets keeps the F3 of /u/ near 3 kHz (+32 %); the test allows 40 % for that one
  formant. The other formants are within 4.2 % (Full) and 6.4 % (Lite) (table in `shape.h`).

## Cost

Counted from the disassembly of the MK66 build (method and numbers in the header of `processor.h`): about 390 cycles per frame Full (9.5 % of
the CPU at 44.1 kHz), about 310 Lite (7.6 %), above the plan's 175 and 84. A unit measurement is owed (skill `rt-optimize`).

## Tests

`tube_test.cpp` (formants of a uniform tube, the two-tube sign test, passivity under realistic motion, decay, sanitize), `shape_test.cpp`
(the five vowels at N = 22 and 11, anchors, closures), `halfband_test.cpp` (ripple, rejection, delay, reset), `processor_test.cpp` (bypass,
block-size independence, clicks, closure, size change, level, silence, NaN). The ear on the unit is the check no host test gives.
