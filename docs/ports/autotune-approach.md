# Live auto-tune (TUNE mode) as an o3sdk unit: recommended approach

Researched 2026-11-01. Research only; nothing cloned, built or run. Every cycle figure below is an *estimate*; only the unit measures it. Facts carry a source in the candidate cards.

## 1. Result in one paragraph

No candidate is usable as a port for live singing. The ones that are free to take (MIT, GPL-2.0-or-later) either carry too much latency (Autotalent 46 ms), spike a whole block with an FFT (Autotalent, zita-at1), or do not fit the language and memory rules (cycfi/q: C++20 and heap; sevagh: STL and FFT libraries). What does exist is a set of **designs worth taking**: YIN for the detector (paper, with the Teensy Audio library as proof that it fits this core), and zita-at1's pitch-synchronous read pointer with whole-period jumps for the shifter. Recommended: **clean-room YIN detector, zita-style shifter written fresh and credited, own scale snap and retune smoothing**, about 11-17 % of a block, about 13 KiB of SRAM, audio delay 3-9 ms.

## 2. Candidate verdicts

| Card | Licence class | Verdict |
|---|---|---|
| `cycfi-q.md` | MIT (permissive) | adapt the BACF core as a benchmark rival; voice robustness unproven |
| `sevagh-pitch-detection.md` | MIT (permissive) | drop; host-side oracle only |
| `autotalent.md` (Autotalent and TalentedHack) | GPL-2.0-or-later (verify header); FFT BSD-3; TalentedHack GPL-3.0 with per-file terms unclear | inspiration only (46 ms, FFT spike) |
| `zita-at1-fat1.md` | GPL-2.0-or-later (not 2-only), attribution dispute on record | adapt the shifter and correction logic, replace the detector |
| `daisysp-pitchshifter.md` | MIT (permissive) | adapt only as a cheap harmonizer layer; not the TUNE engine |
| `clean-room-yin-mpm-psola.md` | papers, no code licence | clean-room is the better route |
| `antares-patent.md` | n/a | US 5,973,252 expired (anticipated 2018-10-14, Google Patents) |
| `mda-talkbox.md` | MIT or GPL-2.0-or-later, licensee's choice | port (adapt) the DSP for TALK |
| `teensy-audio-yin.md` (added) | MIT-style with a funding notice | adapt the structure; best proof YIN fits this core |
| aubio (considered, no card) | GPL-3.0-or-later (`pitchyin.c`, Brossier 2003-2009, as read) | drop: `new_fvec` heap, `smpl_t` precision not seen, whole-buffer API; useful as a host oracle |

## 3. Recommended pipeline

```
in_l/in_r (selected input; mono) -> DC block -> mono ring (2048 floats, 44.1 kHz)
       |
       +-> 4:1 decimation (32-tap FIR, 11.025 kHz) -> YIN (W 192, lags 1..157, sliced over 4 blocks) -> native refine (+-3 lags, W 512)
       |        |
       |        +-> aperiodicity d', RMS, low-band ratio -> VOICED gate (hysteresis, 60 ms hold)
       |        +-> median-of-3 on log-pitch, octave-jump guard
       |
       +-> target pitch: held note (NoteOn) else nearest scale note, with bias hysteresis
                |-> error (cents) -> retune one-pole (SPEED; 0 = instant) x AMOUNT -> ratio per hop (log domain), linear per block
                        |
ring ------------------ pitch-synchronous shifter (read pointer at ratio, whole-period jumps, 64-sample crossfade, cubic read) -> dry/wet -> out
```

### 3.1 Detector: clean-room YIN on a decimated signal
- Decimate 4:1 to 11.025 kHz (a 32-tap FIR evaluated at the output rate). Fundamentals 70-1000 Hz are lags 157 down to 11 samples at that rate.
- YIN per the paper: difference function over a 192-sample window (17.4 ms), cumulative-mean normalisation, first dip below the threshold (0.15), parabolic interpolation. Lags 1..157 are computed (the cumulative mean needs the small lags too).
- Slicing: take a snapshot of the 349 samples needed at the hop start and compute about 40 lags per block over 4 blocks (as the Teensy code does with 64), so the load per block is flat (no spike).
- Refinement at 44.1 kHz: after the decimated estimate T (decimated lags), compute the native difference at lags 4T-3..4T+3 over 512 samples and interpolate parabolically: pitch resolution then is a fraction of a sample at 44.1 kHz, about 3 cents at 400 Hz (estimate).
- Range: the app or a RANGE parameter sets min and max lag (bass 55-400 Hz, normal 70-600, high 110-1000) to cut the work and the octave errors.

### 3.2 Voiced/unvoiced decision (the "consonants must not make it jump" requirement)
A frame is voiced when all hold: 1 - d' above a threshold (aperiodicity below about 0.2-0.3), signal RMS above a noise floor, and the low-band to total energy ratio (from the decimator's lowpass vs the input) above a floor, which rejects sibilants. Hysteresis on both sides and a hold (about 60 ms, the same idea as the 5 failed estimates of zita-at1, about 64 ms) before declaring unvoiced. On unvoiced: **freeze the ratio, then relax it to 1.0 over about 30 ms** and let the shifter sit in the middle of its delay window; consonants and breath pass through unshifted (a delayed dry signal). A candidate more than about 7 semitones from the held pitch must repeat in two consecutive hops before it is accepted (the octave-error guard); a median of three on log-pitch.

### 3.3 Scale snap
The target is chosen in log-pitch (semitones from the reference A, REF A = 430-450 Hz):
- NOTES-IN: a held note (NoteOn from pads, sequencer or MIDI) wins; the target is that note in the octave nearest to the detected pitch ("fold"), or exact. The most recent held note wins; when all are off, fall back to the scale or to pass-through (a parameter).
- SCALE: nearest enabled degree of KEY and SCALE (a 12-bit mask is stored as a float parameter, exact up to 2^24).
- BIAS: the current note is favoured by a hysteresis of up to 50 cents (the idea of `notebias` in zita-at1) so that vibrato does not flip the target.

### 3.4 Retune smoothing
Per pitch hop (5.8 ms): error e = target - detected (cents); smoothed correction c += alpha x (e - c), alpha = 1 - exp(-hop / SPEED); **SPEED = 0 gives alpha = 1: hard, stepped** (the "One More Time" sound: the voice is pulled to exact notes, its glide between them is cut); SPEED 100-400 ms gives natural correction; AMOUNT scales c (below 100 %, vibrato and expression are partly kept). On a note change the filter state is kept for natural settings (a glide between notes) and bypassed when SPEED is below about 20 ms (zita-at1 bypasses it always on a note change). The ratio is computed in the log domain per hop and interpolated linearly across the block; one `exp2f` per block (about 150 cycles).

### 3.5 Shifter: pitch-synchronous read pointer with whole-period jumps (zita-at1's design, written fresh)
- A read pointer in the mono ring advances by `ratio` per output sample (cubic read). Its distance behind the write pointer drifts by (1 - ratio) per sample. When it leaves a window one period wide (plus the crossfade and a few samples of margin), it jumps back or forward by a **whole number of periods** with a 64-sample raised-cosine crossfade, so the two readers show nearly the same waveform: no phasing, no extra delay.
- The window is period-adaptive, which is the difference from zita-at1's fixed half-ring (21 ms) and the reason the delay stays short.
- Unvoiced: the pointer stays; the ratio relaxes to 1.

### 3.6 Latency (arithmetic; estimates)
Audio path delay = D_lo + (window)/2 on average, with D_lo = 64 (crossfade) + 4 (cubic) + 8 (drift margin) = 76 samples = 1.7 ms and window = one period T.

| F0 | T (samples) | mean delay | maximum delay |
|---|---|---|---|
| 440 Hz | 100 | 126 samples = 2.9 ms | 176 = 4.0 ms |
| 200 Hz | 220 | 186 = 4.2 ms | 296 = 6.7 ms |
| 100 Hz | 441 | 297 = 6.7 ms | 517 = 11.7 ms |
| 70 Hz | 630 | 391 = 8.9 ms | 706 = 16.0 ms |

So the under-10 ms target holds from about 100 Hz up on average; the 20 ms ceiling holds in every case (16 ms worst, at 70 Hz). The engine's block (64 frames = 1.45 ms) and the codec's own I/O delay come on top; neither is measured here.

Separately the **pitch information is older than the audio**: the decision for a frame centres about (192 + T_dec)/2 decimated samples back (about 12 ms at 150 Hz), plus up to 5.8 ms of slicing, plus up to 5.8 ms of waiting for the next hop: about 12-24 ms old, about 8-18 ms older than the audio being played (which is delayed 4-6 ms). This is the same effect that zita-at1's own guide states for its low-latency mode ("the pitch information then lags the audio by about 10 ms"). Audible only as a short wrong-pitch onset at the start of a note under a hard setting (SPEED 0); a natural setting hides it. If it is too much, the cycfi/q edge predictor (one period) or a shorter window for the first hop of a note is the lever.

## 4. Combined cost (estimates)

Block = 261,224 cycles (64 / 44,100 x 180 MHz); 1 % = 2,612 cycles. Per block, worst case, pitch hop 256 samples (4 blocks):

| Stage | Arithmetic | Cycles per block |
|---|---|---|
| A. input select, mono, DC block, ring write | 64 x (5 + 15 + 4) | 1,500 |
| B. 4:1 decimator | 16 outputs x 32 taps x 3 + writes | 1,600 |
| C. YIN difference | 157 lags x 192 = 30,144 iterations x 4 cycles = 120,600 per hop; / 4 blocks = 30,150; plus normalisation 157 x 20 = 3,100 per hop = 800 per block, threshold scan 200 | 31,150 |
| D. native refine | 7 lags x 512 = 3,584 x 4 = 14,300 per hop; / 4 | 3,600 |
| E. gate, median, log, snap, smoothing | about 1,000 per hop / 4 | 300 |
| F. shifter | 64 x (4 write + 35 cubic + 35 second reader during crossfade + 6 mix) = 64 x 80 | 5,100 |
| G. dry/wet, level, stereo copy | 64 x 8 | 500 |
| H. events, params, `exp2f` | | 500 |
| **Total (worst)** | 1,500 + 1,600 + 31,150 + 3,600 + 300 + 5,100 + 500 + 500 | **44,250 = 16.9 %** |

Variants (same arithmetic, rounded):
- Typical voiced input with the first-dip early exit (about 90 lags computed) and the crossfade active a quarter of the time (shifter 3,500): C = 90 x 192 x 4 / 4 + 1,000 = 18,300, total about 29,900 = **11.4 %**.
- Pitch hop 512 samples (8 blocks): C = 15,600, D = 1,800: total about 27,000 = **10.3 %** worst case; pitch updates every 11.6 ms.
- Integer detector (int16 samples with `SSUB16` and `SMLAD`, two terms per instruction pair, about 2 cycles per iteration instead of 4): C = 15,600 at hop 256: total about 29,000 = **11.1 %** worst. Needs a core intrinsic that `dsp` may not expose; a float loop is the baseline.
- No block exceeds its mean: the work is sliced, so the worst block equals the average, about 17 %. For comparison the framework reports 15-22 % for the TB-303 voice (FRAMEWORK.md, Porting a plugin, point 5).

Extras (estimates, not in the total):
- **Harmonizer / formant preservation**: LPC order 12 at 44.1 kHz (autocorrelation 512 x 13 = 6,656 multiply-accumulates x 3 = 20,000 per hop; /4 = 5,000 per block) plus a whitening FIR and a re-colouring all-pole filter (24 multiply-accumulates x 3 x 64 = 4,600): about **10,000 per block = +3.8 %**; each additional voice with its own shifter and filters about +3.8 %; TD-PSOLA voices instead about 1,500 each plus 500 for mark tracking (0.6 % + 0.2 %) but with the mark-tracking risk (`clean-room-yin-mpm-psola.md`). Large shifts raise the jump rate to a grain per one or two periods and the sound turns granular: set expectations.
- **TALK mode** (mda Talkbox DSP): about **27,700 = 10.6 %** (`mda-talkbox.md`); TUNE and TALK are alternative modes, so the unit's worst case is the larger of the two, not their sum.

## 5. Memory (estimates)

| Item | Size |
|---|---|
| mono ring, 2048 floats (covers the 706-sample maximum delay and the 1,145-sample refine span) | 8 KiB |
| decimated ring, 512 floats | 2 KiB |
| YIN snapshot (349 floats) and d, d' (2 x 160 floats) | 2.7 KiB |
| FIR coefficients and state, parameters, hold and smoothing state | about 1 KiB |
| **TUNE total** | **about 14 KiB SRAM**, no SDRAM |
| formant stage (LPC history 512 floats, coefficients) | +2.5 KiB |
| TALK (five buffers of 720-1,024 floats, LPC arrays) | +15-20 KiB |

The unit needs no SDRAM; if it ever does (a long harmonizer delay), the app owns it and hands it in at construction as `units/delay` does.

## 6. Unit shape and parameters

Instrument, name `TUNE` or part of a `VOCODER` unit with a mode parameter. The selected input arrives in `in_l`/`in_r` (the app does the choice of mic, line or USB return; the unit takes the left channel, or the sum when both carry signal); output mono to both channels. Notes arrive as `NoteOn`/`NoteOff` events with frame positions (sample-accurate target changes inside a block via `for_each_run`). Draft table (append-only ids; ranges and curves are proposals):

| id | name | range | default | curve and display |
|---|---|---|---|---|
| 0 | SPEED | 0-500 ms | 25 | table curve so 0 is reachable (log needs min > 0); ms |
| 1 | AMOUNT | 0-100 % | 100 | linear, percent |
| 2 | KEY | C..B | C | enum, 12 |
| 3 | SCALE | chromatic, major, minor, major pent, minor pent, blues, dorian, custom | chromatic | enum |
| 4 | TARGET | scale, notes-in, notes-in then scale | notes-in then scale | enum |
| 5 | BIAS | 0-50 ct | 20 | linear, int |
| 6 | TRANSPOSE | -12..+12 st | 0 | linear, Fixed1 |
| 7 | RANGE | bass, low, normal, high | normal | enum (sets min and max lag) |
| 8 | GATE | aperiodicity 0.05-0.5 | 0.25 | linear |
| 9 | HOLD | 20-120 ms | 60 | linear, ms |
| 10 | REF A | 430-450 Hz | 440 | linear, Hz |
| 11 | GLIDE | 0-300 ms | 0 | note-in portamento |
| 12 | DRY | 0-100 % | 0 | percent |
| 13 | LEVEL | 0-100 % | 80 | percent |
| 14 | MASK | 0-4095 | 4095 | the custom scale, a 12-bit mask in a float |
| 15 | FORMANT | off, on (stretch goal) | off | enum |

SPEED, AMOUNT, DRY, LEVEL, TRANSPOSE and GLIDE are smoothed inside the unit (`kParamSmooth`); the enums and the mask apply at their frame (framework text: smoothing is the unit's).

## 7. Ports versus clean-room

| Part | Route | Basis |
|---|---|---|
| detector (YIN, decimation, refine, gate) | clean-room | YIN paper; Teensy structure as proof; sevagh/aubio as host oracles |
| scale snap, bias, retune smoothing | clean-room | trivial; ideas from zita-at1 (`notebias`, `corrfilt`) |
| pitch-synchronous shifter | written fresh from zita-at1's described design, credited (a port with Fons Adriaensen's notice is equally licence-clean) | GPL-2.0-or-later |
| formant stage (stretch) | clean-room LPC | textbook; mda Talkbox's Levinson is an MIT reference |
| TD-PSOLA harmonizer voices (stretch) | clean-room | Moulines and Charpentier |
| TALK mode | port (adapt) | mda Talkbox, MIT or GPL-2.0-or-later |
| DaisySP shifter | optional port for a thick layer | MIT |

## 8. Risks and what stays unknown

1. Everything above is arithmetic: cycles per iteration (4), per `sinf`, per cubic read are assumptions. Measure stage C first.
2. Robustness on sung voice is **not demonstrated by anything fetched**: YIN's paper evidence is on speech; q's BACF is unproven on voice; the threshold values (0.15, 0.25, 60 ms) are starting points to tune on recordings.
3. Pitch information is older than the audio (8-18 ms). Fine for natural settings, audible as an onset glitch for SPEED 0.
4. 70 Hz voices see up to 16 ms of audio delay.
5. Formant-preserving harmonizing is the weakest part of the design; the period-jump shifter moves formants with the ratio, which is fine for under a semitone and chipmunk-like at several semitones until the LPC stage or PSOLA is added.
6. Input routing: a unit of kind Instrument receives the codec input in `in_l`/`in_r` (unit.h comment: "for a recorder"). The USB-return source depends on whether the engine delivers it there; not checked in this research. Checked afterwards (2026-11-01): it does. With `AudioIn`'s source `Usb` the engine's input is what the computer plays (`kit/app/include/o3sdk/app/audio_in.h`, `usb_audio.h`), so it reaches `in_l`/`in_r` like the microphone and the line; the return is host-tested only (`docs/FRAMEWORK.md` section 10).

## 9. Decisions for the maintainer

1. Confirm clean-room (YIN, shifter written fresh and credited) over porting, and whether q's BACF is worth a host benchmark.
2. Autotalent's `autotalent.c` header: read the first lines of the tarball (the fetch was indirect) if anything is ever to be taken; TalentedHack's per-file terms are unclear, so do not take from it.
3. zita-at1: if its code is copied rather than rewritten, keep Fons Adriaensen's copyright and a note of the changes (the 2014 attribution dispute is the precedent).
4. mda Talkbox: choose the MIT option (attribution only) or the GPL one; the VST scaffolding stays behind.
5. Whether the harmonizer stretch goal justifies the extra 4 to 6 %.
6. How the app presents the three sources (mic, line, USB return) to a unit of kind Instrument, and what NOTES-IN does when no note is held (silence, pass-through, scale).

## Sources
All sources are listed in the candidate cards. The cycle basis for FFT cost: https://e2e.ti.com/f/1/t/1032595 (TI E2E post: `arm_cfft_f32` 2.587 ms for 1024 and 5.820 ms or 280,411 cycles for 2048 on a Cortex-M4F; the clock and compiler settings are not stated there). Framework facts: `kit/dsp/include/o3sdk/dsp/unit.h`, `kit/dsp/include/o3sdk/dsp/event.h`, `docs/FRAMEWORK.md` (Units, Porting a plugin), `cmake/O3sdkPlatform.cmake` (C++17).
