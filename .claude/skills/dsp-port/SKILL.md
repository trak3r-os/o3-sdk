---
name: dsp-port
description: Bring an open-source DSP algorithm (oscillator, filter, 303 voice, Plaits-style engine, effect) into o3sdk as a unit - licence gate, float-only block-based port against the Unit API, host oracle test, cost estimate. Use together with app-feature for any port or reimplementation.
---

# Port DSP into a unit

Only the DSP ports; the GUI never does (the screen is built with the kit). The shape of the target is `docs/FRAMEWORK.md`, "Units" and "Porting a plugin"; the contract is `kit/dsp/include/o3sdk/dsp/unit.h`. Candidates and licence research: the `dsp-scout` agent.

## Gate 1 - licence (before reading code closely)
Read the LICENSE file and the file headers of what you would port; record repo URL, tag or commit.
- MIT, BSD, ISC, zlib, Apache-2.0: may port; attribution in `THIRD_PARTY.md` (a row) and the file header.
- GPL-3.0, GPL-2.0-or-later, LGPL: compatible with the kit (GPL-3.0-or-later); same attribution.
- GPL-2.0-only, AGPL, non-commercial, unclear: stop and ask the maintainer. No licence or proprietary: inspiration only (a clean-room port from a paper or a description, written by someone who has not read the code).
- One project can mix licences by directory and by era (Mutable Instruments' repositories do): read the headers of the files you take, do not assume the project's headline licence.
- The official Tracker firmware is never a source (CLAUDE.md, Hard rules).

## Gate 2 - oracle
Build the original on the host in the scratchpad (not in the repository unless its licence and a `THIRD_PARTY.md` row allow vendoring) and render reference output for fixed stimuli: an impulse, a sine sweep, a saw at several pitches, seeded noise, parameter ramps. The port is tested against it: max abs error 1e-4 or less, or SNR of 80 dB or more against the double-precision original, unless the spec documents a deviation (a cheaper `tanh`, with its measured error). Keep only small reference tables as source text in the test; no WAV or binary files in git.

## Port rules
- `unit_<name>` in `units/<name>/` via `o3sdk_unit(<name> SOURCES ...)` (depends on `dsp` only); copy `units/synth` (instrument) or `units/delay` (effect). Add the include dir `include/o3sdk/units/<name>/`.
- `float` only, `f`-suffixed constants, `sinf`/`tanhf` or table/polynomial approximations; the unit target compiles with `-fsingle-precision-constant -fno-math-errno`. No `double`, no STL containers, no heap, no virtual calls inside the sample loop.
- The plugin's parameters become a constexpr `core::ParamDesc` table (range, default, curve, display, detents, coarse); the id is the index, append never insert. The plugin's `setParameter()` becomes the `Param` event handled in `process()`; the unit does not own the values the user sees.
- `process()` accepts any frame count from 1 to the block; handle events with `for_each_run()`; state in a struct; coefficients per block (smoothed) unless per-sample modulation is the point; `prepare(sample_rate)` computes everything rate-dependent.
- Big buffers (delay lines, wavetables, samples) belong to the app in the SDRAM (`O3SDK_BIG`) and are handed to the unit at construction; hot state is small.
- CMSIS-DSP or M4 SIMD intrinsics only where a measured hot spot justifies them. Structure of `process()` for cost: skill `rt-optimize`.

## Cost
Estimate cycles per sample from the op count (show the arithmetic) and the share of the 64-frame block (261,224 cycles at 44.1 kHz); label it *estimate*. It is measured on the unit by the maintainer (skill `hw-session`); track3r-os's own TB-303 voice costs 15-22 % of a block, so a port states how many voices it can promise only after that measurement.

## Tests
Host doctest (`tests/`, as `units_test.cpp` does against the engine): the oracle comparison, parameter extremes (no NaN or Inf, bounded output), reset and state, block-size independence, event timing. Run under `-DO3SDK_SANITIZE=ON`. Then the normal `app-feature` verify and review.
