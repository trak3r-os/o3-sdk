# mda Talkbox (Paul Kellett): licence check and algorithm shape, for the vocoder's TALK mode

Researched 2026-11-01. Nothing cloned, built or run. Source read: `src/mdaTalkBox.cpp` from the mda-vst SourceForge repository (as raw text through a fetch summary; `BUF_MAX` and `ORD_MAX` are defined in a header I did not see).

## 1. Licence (facts)

- Repository https://sourceforge.net/p/mda-vst/code/HEAD/tree/ lists `MIT License.txt`, `gpl-2.0.txt` and `readme.license` at the top level.
- `readme.license`: "The mda VST plug-ins are released under the MIT license or under the GPL", the GPL option being "either version 2 of the License, or (at your option) any later version" (https://sourceforge.net/p/mda-vst/code/HEAD/tree/readme.license?format=raw).
- Paul Kellett's KVR post: the MIT licence "lets you do anything you want, including commercial use"; the GPL was added so the code could ship with purely-GPL software; "my intention that this source code can be used in proprietary products" (https://www.kvraudio.com/forum/viewtopic.php?p=3109032).
- `mdaTalkBox.cpp` header (as read): "mda Template" VST plug-in comment, Copyright Paul Kellett (maxim digital audio) 1999-2000, "based on the Steinberg VST 1.0 SDK (1996)". No per-file licence paragraph was visible in that header; the repository-level `readme.license` is the grant.
- mda-lv2 (David Robillard's port, based on revision 10 of the mda SVN repository): Debian lists it as GPL-3+ for the port, with the original header comment "GPLv2 or later, or the MIT license" (https://metadata.ftp-master.debian.org/changelogs//main/m/mda-lv2/mda-lv2_1.2.6-1_copyright). The LV2 port's own parts are not needed.
- Student port TalkBoxFloat (https://github.com/chronoB/TalkBoxFloat): MIT per its README, "A digital Talkbox with floating point calculations" (Jade University of Applied Science). I could not confirm whether it derives from mda; not used.
- Class: **dual MIT / GPL-2.0-or-later, the licensee's choice: permissive in effect**. Take the MIT option (attribution only) or the GPL option (compatible with the kit). Leave behind the VST 1.0 SDK scaffolding (the class shell and `getParameterDisplay`-style code), which is what the "Steinberg" line refers to; only the DSP is wanted.

## 2. Shape (facts)

- C++, one class, floats for all buffers and state. libm: `cos`, `sqrt`, `fabs` called in double precision and cast to float (to be replaced by `cosf`, `sqrtf`, `fabsf`). Denormal flush by a 1e-10 threshold. Buffers: five float buffers sized `BUF_MAX`; LPC work arrays sized `ORD_MAX`; no heap in the process path.
- Window: Hann, `0.5 - 0.5 cos(2 pi n / N)`, N about 0.01633 x sample rate = **about 720 samples (16.3 ms) at 44.1 kHz**, capped at `BUF_MAX`; two buffers at 50 % overlap, the second with the complement window.
- **LPC order** `O = (0.0001 + 0.0004 x quality) x fs`: **about 4 to 22 at 44.1 kHz** (the display shows 5 + 95 q^2, "5 to 100", which does not match; trust the formula and check on the unit).
- Hop: each buffer is analysed when its write pointer wraps, and the buffers are half a window apart: **one analysis per N/2 about 360 samples (8.2 ms)**. The fetch summary also said analysis "runs on every second input sample" through a toggle `K`; I could not tell what that does in the unit's terms; it needs a second look in the source.
- Steps per frame: modulator pre-emphasis (first difference), window; autocorrelation at lags 0..O; r[0] x 1.001 and a silence gate; Levinson-Durbin (reflection coefficients clamped to +-0.995, gain = sqrt of the final error); an all-pole **lattice** filter driven by the carrier scaled by the gain; overlap-add; the carrier goes through a two-path allpass-style network (coefficients 0.3 and 0.77) both before and after.
- Inputs: modulator on left (or right, swapped); carrier on the other. The output is mono, copied to both channels, wet and dry mixed by gain.
- Parameters: WET (default 0.5, gain 0.5 p^2), DRY (default 0, gain 2 p^2), CARRIER (swap), QUALITY (default 1.0).
- Sample rate: clamped to 8-96 kHz in `resume()`; N and O scale with it.

## 3. Cost on the MK66 (estimates)

*All numbers are estimates; the unit measures them.*

- Autocorrelation per frame: N x (O + 1) = 720 x 23 = 16,560 multiply-accumulates x about 3 cycles = about 50,000 cycles per 360 samples = 139 cycles per sample = 8,900 cycles per block (3.4 %). It lands as a spike of about 50,000 cycles (19 %) in one block unless sliced across blocks.
- Lattice synthesis: each sample passes through two overlapped buffers, O = 22 stages x about 6 cycles = 132 cycles x 2 = 264 per sample = 16,900 cycles per block (6.5 %).
- Pre-emphasis, windowing, allpass networks, overlap-add: about 30 cycles per sample = 1,900 cycles per block (0.7 %).
- Total about 27,700 cycles per block = **10.6 %** at order 22; at order 12 (quality 0.5) about 18,000 = 7 %.
- Memory: five buffers of about 720-1024 floats = 15-20 KiB in SRAM. No SDRAM needed.

## 4. Mapping

- The vocoder's TALK mode: modulator = the selected input (mic, line, USB), carrier = the unit's own synth (or a second internal oscillator), both already in the unit; no stereo input needed.
- Parameters: WET, DRY, QUALITY (order, 4-22), a CARRIER level; the swap becomes the app's routing.
- Shares nothing with TUNE except the input and an optional DC blocker. TUNE and TALK would be alternative modes, so the unit's worst-case cost is the larger of the two, not the sum.

## 5. Risks

- LPC at low orders sounds like the original talk box only for steady voiced input; the analysis uses the whole mic, so breath and room noise leak (the silence gate helps).
- Latency about one window (16 ms) plus the overlap-add: it is a creative effect, not a live monitor path.
- Order versus sample rate tied to `fs`; at 44.1 kHz fixed anyway.

## 6. Verdict

**Port (adapt) the DSP only.** MIT or GPL-2.0-or-later, take MIT and keep Kellett's copyright line; replace libm double calls, put the buffers in the unit, slice the autocorrelation. Of the cards here, this is the cleanest port.

## Sources

- https://sourceforge.net/p/mda-vst/code/HEAD/tree/
- https://sourceforge.net/p/mda-vst/code/HEAD/tree/readme.license?format=raw
- https://sourceforge.net/p/mda-vst/code/HEAD/tree/src/mdaTalkBox.cpp?format=raw
- https://www.kvraudio.com/forum/viewtopic.php?p=3109032
- https://metadata.ftp-master.debian.org/changelogs//main/m/mda-lv2/mda-lv2_1.2.6-1_copyright
- https://github.com/chronoB/TalkBoxFloat
