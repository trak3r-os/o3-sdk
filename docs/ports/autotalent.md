# Autotalent (Tom Baran) and TalentedHack (Jeremy Salwen)

Researched 2026-11-01. Nothing cloned, built or run. I could not open the upstream tarball or the author's page (certificate expired on tombaran.info), so the header text below is read through sources.debian.org and a search snippet. **Before any port, read the first 20 lines of `autotalent.c` in the 0.2 tarball yourself.**

## 1. Licence (facts)

### Autotalent 0.2 (LADSPA, C)
- Debian's copyright file (Upstream-Name Autotalent, contact Thomas A. Baran, source tombaran.info): `Files: *` is **GPL-2+**; `ladspa.h` LGPL-2.1+ (Furse, Barton-Davis, Westerfeld); `mayer_fft.*` BSD-3-clause (Ron Mayer, Miller Puckette); `debian/*` GPL-2+ (https://sources.debian.org/copyright/license/autotalent/0.2-5).
- `autotalent.c` header, as read on sources.debian.org: GNU GPL, "either version 2 of the License, or (at your option) any later version" (https://sources.debian.org/src/autotalent/0.2-5/autotalent.c/).
- A search snippet of the author's page says "GPL2, with one exception" (the FFT routines, under the Pure Data BSD-like licence). That wording alone would leave "2-only" open; the file header's "or later" settles it, if the header read is right. The source tree also holds `COPYING` and `COPYING-mayer_fft` (DragonFly listing, https://grok.dragonflybsd.org/xref/dports/audio/autotalent/autotalent-0.2/).
- Class: **GPL-2.0-or-later** (compatible with a GPL-3.0-or-later kit) for `autotalent.c`; BSD-3-clause for the FFT. **Not GPL-2.0-only**, on the evidence above; the verification step above is what makes that final.
- Derivatives to avoid confusing with the original: Bespoke Synth's `Autotalent.cpp` carries Bespoke's own GPL-3-or-later header (a derived work, not the original).

### TalentedHack (LV2, C)
- Repository https://github.com/jeremysalwen/TalentedHack; `LICENSE` is the GPL **version 3** text (first lines read at https://raw.githubusercontent.com/jeremysalwen/TalentedHack/master/LICENSE). Arch's AUR package says GPL-3.0-or-later (https://aur.archlinux.org/packages/talentedhack-git); that is a packager's statement.
- `pitch_detector.c` and `pitch_shifter.c` (read in full as excerpts) carry **no licence header**. `pitch_detector.c` has a comment saying the MPM code was "adapted from their GPL Tartini program" (no version; Tartini's licence not checked). The project is a fork of Autotalent (GPL-2+), so the true licence of each file is a mix of at least GPL-2+ (Autotalent lineage), Tartini's GPL and the author's GPL-3.
- Class: **GPL-3.0 for the repository as a whole, per-file terms unclear; "or later" not stated.** It can go into a GPL-3 combination, but "unclear" is the honest label for the per-file position. Prefer the original Autotalent over this fork for anything taken.

## 2. Shape (facts)

- Language C, LADSPA plugin. Heap: `calloc`/`free`. libm: `sin`, `cos`, `pow`, `sqrt`, `atan`, `log10`. Phase accumulators (`inphinc`, `outphinc`, `phasein`, `phaseout`, `phincfact`) are `double`; the rest `float`; some constants are double literals.
- Analysis buffer: a 2048-sample circular buffer (4096 at 88.2 kHz and above), analysis every N/4 = 512 samples. Window: raised cosine. Pitch: FFT autocorrelation (FFT, power spectrum with DC zeroed, inverse FFT, normalise by lag 0, compensate the window's own autocorrelation); search 1/700 s to 1/70 s (70-700 Hz); peak refined by a three-bin centre of mass; voiced when confidence >= 0.7.
- Correction: nearest enabled note of 12, a sine-shaped transition controlled by SMOOTH, blended by AMOUNT; optional LFO, shift, scale rotation.
- Shifter: pitch-synchronous overlap-add in the style of Fairbanks: input snippets copied N/2 in the past when the input phase wraps, Hann-windowed, cubic-interpolated, summed into an output buffer when the output phase wraps.
- Formant corrector: adaptive 7th-order pre-filter and matching post-filter (a post-filter after the shift).
- Reported latency: N - 1 = 2047 samples (every mention agrees: Bespoke port source and the original).
- Parameters: set by LADSPA ports (floats); GUI none in the original.
- TalentedHack differences (README): LV2; MIDI in and out; pull-to-semitone separated from snap-to-scale; FFTW3 instead of the hand FFT; **MPM** instead of plain autocorrelation; "the formant corrector is broken"; only known to work on 32-bit Linux; trouble with low notes (the author suspects the detector buffer is too small). Its MPM code has a truncating-integer bug in the parabolic interpolation (as read).

## 3. Cost (estimates)

*All numbers are estimates.*

- FFT: the TI E2E post reports `arm_cfft_f32` at 2.587 ms for 1024 and 5.820 ms (280,411 cycles) for 2048 on a Cortex-M4F; 5.820 ms at 48 MHz = 279,360 cycles, which agrees. So a 1024-point complex FFT is about 124,000 cycles. A 2048-point real FFT is about a 1024-point complex FFT plus a split: about 130,000 cycles (estimate). Autocorrelation needs a forward and an inverse: about 260,000 cycles per analysis, **99.5 % of one block's 261,224 cycles in a single block** unless the FFT is split across blocks (a rewrite).
- Averaged: 260,000 / 8 blocks (512 samples per hop) = 32,500 cycles per block = 12.4 %, before the shifter and the formant stage.
- Memory: a few windowed arrays of 2048 floats plus the FFT work area: about 40 KiB (estimate).

## 4. Mapping

- Latency 2047 samples = 46.4 ms at 44.1 kHz. That is more than twice the 20 ms ceiling. The detector window, not the shifter, sets it.
- The detector alone (FFT autocorrelation, window compensation, three-bin centre of mass, 0.7 gate) is useful as a description; the shifter needs a delay of N/2 by design.

## 5. Risks

- Licence ambiguity of the fork; need to verify the original header.
- Latency fails the use case; FFT spike exceeds a block; `double` phase; heap.

## 6. Verdict

**Inspiration only.** The licence (GPL-2.0-or-later) would allow a port; the design (46 ms, a block-sized FFT spike, a 70 Hz floor from a 2048 window) does not fit live singing. TalentedHack adds nothing the original lacks except MPM and an "unclear per-file licence"; its MPM idea is covered in `clean-room-yin-mpm-psola.md`.

## Sources

- https://sources.debian.org/copyright/license/autotalent/0.2-5
- https://sources.debian.org/src/autotalent/0.2-5/autotalent.c/
- https://sources.debian.org/src/bespokesynth/1.3.0%2Bdfsg-3/Source/Autotalent.cpp
- https://grok.dragonflybsd.org/xref/dports/audio/autotalent/autotalent-0.2/ (listing seen in search results only)
- http://tombaran.info/autotalent.html (not opened; certificate expired)
- https://github.com/jeremysalwen/TalentedHack and its `LICENSE`, `pitch_detector.c`, `pitch_shifter.c` raw files
- https://aur.archlinux.org/packages/talentedhack-git
- https://e2e.ti.com/f/1/t/1032595
