# Teensy Audio Library: AudioAnalyzeNoteFrequency (YIN on a Cortex-M4)  [candidate added by the researcher]

Researched 2026-11-01. Nothing cloned, built or run. Read: `analyze_notefreq.cpp` and `analyze_notefreq.h`.

## 1. Licence (facts)

- Repository https://github.com/PaulStoffregen/Audio, branch `master` (no commit pinned). A top-level `LICENSE` at the URL tried returned 404; the licence is in each file's header.
- `analyze_notefreq.cpp` / `.h`: "Audio Library Note Frequency Detection & Guitar/Bass Tuner", "Copyright (c) 2015, Colin Duffy". An MIT-style permission grant (use, copy, modify, merge, publish, distribute, sublicense, sell) with one extra condition: "The above copyright notice, development funding notice, and this permission notice shall be included in all copies or substantial portions of the Software." The funding notice is a paragraph in the header that must be carried along.
- Class: **permissive (MIT-style, with the funding notice)**. Per-file headers may differ across the library; only these two files were read.
- Why it matters: it is YIN written for Teensy boards, the family the MK66 belongs to (the Teensy 3.6 uses the MK66; that the 2015 code ran on 3.x boards is my inference, the header only names the Arduino core).

## 2. Shape (facts)

- C++, derives from the library's `AudioStream` (Arduino core; `update()` is called every 128 samples = 2.9 ms), which must be cut away.
- YIN: difference function in integer arithmetic (`int16_t` samples, `uint64_t` sums), cumulative-mean normalisation by running sums, an absolute threshold, parabolic interpolation for the sub-sample lag. No decimation (the comments suggest down-sampling as a fix for low notes and an FFT as a speed-up). **Resumable lag search**: each call processes 64 lags and keeps the current lag between calls. Inner loop unrolled by four. A five-entry ring holds neighbouring values.
- Window: `AUDIO_GUITARTUNER_BLOCKS` = 24 blocks = 3072 samples, double-buffered with two block lists; the maximum lag is half the window (the lowest detectable note is about 29 Hz). Output: `read()` frequency in Hz, `probability()` = 1 - normalised value at the accepted lag, `available()`.
- `float` only (no double literals); no `new`/`malloc` in the `.cpp` (buffers are class members).

## 3. Cost (estimates)

*All numbers are estimates.*

- As configured (integration window 1536 samples, lags up to 1536): about 1536 x 1536 = 2.36 M inner iterations for a full search, at 3-4 cycles each: 7-9 M cycles per estimate. Far too much for a voice tracker.
- With a voice-sized window it is the plain YIN cost computed in `autotune-approach.md` (a 4:1-decimated 192-sample window and 157 lags: about 30,000 iterations per estimate).

## 4. Mapping

- Borrow the structure: integer difference function, running sums, a resumable lag search spread across blocks, a probability output. A voice version needs a smaller window (decimated), a lag range of 22-630 samples at 44.1 kHz (or 11-157 decimated) and no `AudioStream`.

## 5. Risks

- Written for guitar tuning (a stable note, seconds of time), not fast note changes; no voiced/unvoiced logic beyond the threshold; its latency is one to two windows (my inference from the structure, not a stated figure): about 70-140 ms as configured.

## 6. Verdict

**Adapt (take the structure) / clean-room (take the algorithm from the YIN paper).** A rewrite around a decimated window is smaller than trimming this file. It is the best existing evidence that YIN runs on this core family with block-sliced work. Keep the notices if any line is copied.

## Sources

- https://raw.githubusercontent.com/PaulStoffregen/Audio/master/analyze_notefreq.cpp
- https://raw.githubusercontent.com/PaulStoffregen/Audio/master/analyze_notefreq.h
- https://github.com/PaulStoffregen/Audio
