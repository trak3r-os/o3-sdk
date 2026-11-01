# zita-at1 (Fons Adriaensen) and x42 fat1.lv2 (Robin Gareus)

Researched 2026-11-01. Nothing cloned, built or run. The source was read as `retuner.cc`, `retuner.h` and `fat1.cc` from the fat1 repository (fat1 carries Fons's `Retuner` class, only wrapped); I did not read the zita-at1 tarball itself.

## 1. Licence (facts)

- zita-at1: Debian's copyright file: `Files: *`, "Copyright 2007-2024 Fons Adriaensen", **GPL-2+** ("either version 2 of the License, or (at your option) any later version") (https://metadata.ftp-master.debian.org/changelogs/main/z/zita-at1/stable_copyright).
- fat1.lv2 (https://github.com/x42/fat1.lv2): GitHub labels it GPL-2.0 with a `COPYING`. The file headers are more precise: `src/fat1.cc`: "Copyright (C) 2016 Robin Gareus", "version 2, or (at your option) any later version"; `src/retuner.cc` and `retuner.h`: "Copyright Fons Adriaensen 2009-2011", GNU GPL "either version 2 of the License, or (at your option) any later version". So **GPL-2.0-or-later**, not 2-only.
- Per-directory differences in fat1: `gui/`, `robtk/` (a submodule pinned to a commit), `modgui/`: GUI toolkit and MOD-platform files, never ported; I did not read their licences.
- Class: **GPL-2.0-or-later**: compatible with a GPL-3.0-or-later kit (take the "or later" grant).
- Attribution: in October 2014 Fons objected publicly to a plugin ("zitaretuner") that rewrote his C++ class in C and presented it without his name or a note of the changes (https://lists.linuxaudio.org/archives/linux-audio-dev/2014-October/035502.html). If anything is derived from `retuner.cc`, keep "Copyright Fons Adriaensen" and say what was changed.

## 2. Shape (facts)

- C++ (an `LV2AT::Retuner` class), no virtuals. Floats: `double` appears only where `M_PI` makes a `cos` argument double. libm: `cosf`, `log2f`, `powf`, `floorf`, `ceilf`, `fabsf`. Heap: `new float[]` for the input ring, `fftwf_malloc` for the FFT arrays, FFTW plans, and a 2x `LV2AT::Resampler` (zita-resampler; its source licence not checked).
- Constants at 44.1/48 kHz: input resampled 2x up to about 96 kHz (`_upsamp`); input ring `ipsize` = 4096 (at the 2x rate); FFT length 2048 at the native rate; fragment `frsize` = 128 samples (2.67 ms at 48 kHz); a pitch estimate every 4 fragments = 512 samples (10.7 ms at 48 kHz); crossfade = one fragment; pitch range from `fsamp/1200` to `fsamp/60` samples (60-1200 Hz).
- Pitch detector (`findcycle`): Hann window over 2048 samples, forward FFT, power spectrum weighted by 1/(1+(f/3 kHz)^2), inverse FFT, normalise by acf[0]+0.1 and a window-correction term, scan for the first descent, then local maxima above 0.8 scored by height x window correction, parabolic interpolation. Returns a fractional period or 0.
- **Shifter, the notable part.** A read pointer moves through the input ring at `ratio` samples per output sample (cubic 4-tap interpolation). At each fragment boundary, if the pointer has drifted outside a window about one fragment wide (relative to the write pointer), it **jumps back or forward by a whole number of pitch periods** (`dr = cycle x ceil(frsize/cycle)`) with a one-fragment raised-cosine crossfade between the old and new reader. A whole-period jump copies or drops complete cycles, so the splice joins nearly identical waveforms. When the signal is unvoiced, the pointer is set to a fixed delay (`ipsize/2`) with a crossfade.
- Correction (`finderror`): the period gives pitch in octaves relative to the reference A; the nearest enabled note of 12 (or a MIDI-held note) gives the target; the note currently locked gets a bias (`notebias`, divided by 13, hysteresis) so the voice can drift; while the nearest note is unchanged the error is smoothed by a one-pole (`corrfilt`), and on a note change the error jumps; ratio = 2^(offset/12 - error x gain). Failure to find a pitch holds the ratio for 5 estimates (about 64 ms), then declares unvoiced.
- Parameters: float setters (`set_refpitch`, `set_notebias`, `set_corrfilt`, `set_corrgain`, `set_corroffs`, `set_notemask`, `set_notescale`, `set_fastmode`); the LV2 wrapper adds a MIDI channel.
- Sample rate: the constructor picks the constants from three bands; the 44.1/48 kHz band is the one that applies.
- User guide: AT1 "is an autotuner meant mainly to correct a voice singing slightly out of tune", **no formant correction, small corrections only**; latency "about 21 ms", the low-latency button halves it to about 10 ms "but the pitch information then lags the audio by about 10 ms" (https://kokkinizita.linuxaudio.org/linuxaudio/zita-at1-doc/quickguide.html). The code's derived figures: nominal delay `ipsize/2` = 21 ms; fast mode read-ahead `ipsize x 7/16`, nominal delay about 2.7 ms.
- fat1 differences from zita-at1 (README): reports its latency to the host, saves state, MIDI input with sidechain semantics. Man pages show the microtonal and scale variants.

## 3. Cost (estimates)

*All numbers are estimates.*

- Detector as written: a 2048-point real FFT forward plus inverse per 512 samples: about 260,000 cycles per estimate (basis: `autotalent.md`), 99.5 % of one block if unsliced; 32,500 cycles per block averaged (12.4 %).
- The 2x resampler (medium quality, 32): roughly 64 multiply-accumulates per input sample x about 3 cycles = about 190 cycles x 64 = 12,000 cycles per block (4.7 %).
- Shifter: cubic interpolation, one reader (two during a crossfade): about 35-70 cycles per sample x 64 = 2,200-4,500 cycles per block (0.9-1.7 %).
- Total as written: about 47,000-49,000 cycles per block = 18-19 %, with a block-sized spike. Not acceptable as is; the shifter alone is cheap.
- Memory: about 49 KB (the code's own buffers at 44.1/48 kHz, as the fetch summary computed them).

## 4. Mapping

- Keep: the **shifter design** (read pointer at ratio, whole-period jumps, one-fragment crossfade, fixed delay when unvoiced), the **note bias** (hysteresis), the **one-pole error filter, bypassed on a note change**, the MIDI-held-note priority, the hold of 5 failed estimates.
- Replace: the FFT detector by a cheaper time-domain one (YIN, see `autotune-approach.md`), the 2x resampler (drop it; cubic at 44.1 kHz should suffice for a jump shifter), `fftw`, `new`; `powf`/`log2f` only once per hop.
- Latency with this design: the original's nominal delay is half the input ring (21 ms), more than needed. The window within which the pointer wanders only has to span one pitch period plus the crossfade, so with a period-adaptive window the delay is about (crossfade + margin + half a period): 4-9 ms at 100-250 Hz (arithmetic in `autotune-approach.md`, *estimate*). The original's fast mode (about 3 ms nominal) shows that a short delay works, at the price of the pitch lagging the audio.
- Parameter inspirations: Filter (retune speed), Correction, Bias, Offset, Tuning, note buttons.

## 5. Risks

- GPL-2+ with an attribution dispute in the record: keep notices if code is derived.
- Whole-period jumps assume a stable period; at large shifts (several semitones) the jump rate rises to a grain per one or two periods, the sound becomes granular, and formants move with the ratio (the guide says it is for small corrections).
- A wrong period (octave error) copies half or double periods: audible as a warble; this puts the weight on the detector.

## 6. Verdict

**Adapt the shifter and the correction logic; replace the detector.** An own implementation from this description with zita-at1 credited, or a port with Fons's notice kept: both are licence-clean under the GPL-2.0-or-later grant. Recommended: write it fresh, name zita-at1 in the source header and the credits, and test it on the host against a plain ratio-only shifter.

## Sources

- https://metadata.ftp-master.debian.org/changelogs/main/z/zita-at1/stable_copyright
- https://kokkinizita.linuxaudio.org/linuxaudio/zita-at1-doc/quickguide.html
- https://github.com/x42/fat1.lv2
- https://raw.githubusercontent.com/x42/fat1.lv2/master/src/fat1.cc
- https://raw.githubusercontent.com/x42/fat1.lv2/master/src/retuner.cc
- https://raw.githubusercontent.com/x42/fat1.lv2/master/src/retuner.h
- https://manpath.be/f36/1/x42-fat1
- https://lists.linuxaudio.org/archives/linux-audio-dev/2014-October/035502.html
- https://e2e.ti.com/f/1/t/1032595
