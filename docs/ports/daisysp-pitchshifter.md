# DaisySP PitchShifter (delay-line shifter)

Researched 2026-11-01. Nothing cloned, built or run. Read: `Source/Effects/pitchshifter.h` (the class is inline in the header; a `pitchshifter.cpp` returned 404).

## 1. Licence (facts)

- Repository https://github.com/electro-smith/DaisySP, branch `master` (no commit pinned). `LICENSE`: MIT, covering DaisySP (Electrosmith, Corp., 2020), Plaits files (Emilie Gillet, 2016) and Soundpipe files (Paul Batchelor, 2015) (https://raw.githubusercontent.com/electro-smith/DaisySP/master/LICENSE).
- `pitchshifter.h`: credited to shensley, based on a "Pitch Shifting" reference from UCSD, "Electrosmith 2020", MIT-style notice.
- Class: **permissive (MIT)**.

## 2. Shape (facts)

- C++ header; floats only; libm `sinf` and `pow` unless `USE_ARM_DSP` (CMSIS) is defined.
- Two `DelayLine<float, 16384>` (about 64 KiB each, about 128 KiB total), two `Phasor` LFOs half a cycle apart; each tap's delay offset follows its phasor and its gain is `sin(fade x pi)`. Transposition: LFO frequency = (2^(semitones/12) - 1) x sr / delSize; `SetTransposition(float)`, `SetDelSize(uint32)` (clamped to 16384), `SetFun(float)` (random depth modulation); `quantize_semitones` option.
- Block API: per-sample `Process(float)`. Params arrive by setters. `myrand()` uses a static global seed (shared between instances); `transpose_` is not initialised in `Init()` but read by `SetDelSize()` (reads as a bug).
- Dependencies from daisysp: `dsp.h`, `delayline.h`, `phasor.h`.

## 3. Cost (estimates)

*All numbers are estimates.*

- Per sample: two phasor steps (about 10 cycles each), two `sinf` (about 100 cycles each with newlib on the M4F, assumed), two interpolated delay reads (about 30 cycles each), mix (about 10): about 290 cycles x 64 = 18,600 cycles per block = 7.1 %. With a sine table or a polynomial instead of `sinf`: about 110 cycles x 64 = 7,000 = 2.7 %.
- Memory: 128 KiB at the compiled size; a 2400-sample (50 ms) delay needs 2 x 9.4 KiB = 19 KiB. Delay lines in SDRAM only if kept large.

## 4. Mapping

- Effect or a harmonizer voice: TRANSPOSE (st), DELAY SIZE (ms), FUN.
- Why it does not suit auto-tune: the delay sweeps over its whole size (the delay changes by the shift all the time and jumps back every sweep), so the audible delay averages half the delay size and the crossfade of two non-coherent taps modulates amplitude and comb-filters a sustained voice; at shifts below a semitone that artefact is the whole sound. There is no pitch information in it, so it cannot do retune; it is a fixed-ratio shifter.

## 5. Risks

- Granular/tremolo character on sustained vowels; no formant control; shared random seed.

## 6. Verdict

**Adapt only as an optional cheap "thick" harmonizer voice (an extra ratio-only layer for the harmonizer stretch goal); not the TUNE engine.** The zita-style pitch-synchronous shifter is the better main shifter (see `zita-at1-fat1.md`). Port is straightforward and licence-clean if wanted (MIT, notice kept).

## Sources

- https://github.com/electro-smith/DaisySP
- https://raw.githubusercontent.com/electro-smith/DaisySP/master/Source/Effects/pitchshifter.h
- https://raw.githubusercontent.com/electro-smith/DaisySP/master/LICENSE
