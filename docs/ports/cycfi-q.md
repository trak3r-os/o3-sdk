# cycfi/q (Joel de Guzman): bitstream autocorrelation pitch detector

Researched 2026-11-01. Nothing was cloned, built or run; everything below was read through web fetches of the files named.

## 1. Licence (facts)

- Repository: https://github.com/cycfi/q, branch `develop` (the page says v1.5.4 is the latest release, on `master`; v1.6 is in development on `develop`). I did not pin a commit; a port must pin one.
- `LICENSE`: MIT, "Copyright (c) 2014-2026 Joel de Guzman" (https://raw.githubusercontent.com/cycfi/q/develop/LICENSE).
- Headers read (`pitch_detector.hpp`, `bacf_period_detector.hpp`, `bitstream_acf.hpp`, `bitset.hpp`, `zero_crossing_collector.hpp`): each carries "Copyright (c) 2014-2026 Joel de Guzman ... Distributed under the MIT License".
- Not checked: the licence of the `infra` submodule that the detector headers include (`infra/assert.hpp`). A port would not take it.
- Class: **permissive (MIT)**, portable with the copyright and permission notice kept.

## 2. Shape (facts)

- Language: C++20, header-only core (`q_lib`), plus the Cycfi `infra` submodule. o3sdk builds as C++17 (`cmake/O3sdkPlatform.cmake` line 65), so the headers do not compile as they are.
- The detector is `q/pitch/pitch_detector.hpp` (a class `pitch_detector`) over `q/pitch/bacf_period_detector.hpp`, `q/utility/zero_crossing_collector.hpp`, `q/utility/bitstream_acf.hpp`, `q/utility/bitset.hpp`, `q/fx/median.hpp`, `q/fx/moving_average.hpp`.
- Algorithm: a zero-crossing collector with a dB hysteresis turns the signal into pulses; edges whose peak reaches 60 % of the peak pulse become 1-bits in a bitstream; the autocorrelation count at a lag is XOR plus popcount of the stream against a shifted copy; candidates come only from edge pairs, between the minimum period (from the highest frequency) and half the window. Periodicity = 1 - count x (2 / window). Window = two periods of the lowest frequency.
- `pitch_detector` adds: accept a first estimate only at periodicity >= 0.9, keep a lock while periodicity >= 0.8, accept a candidate within 1/32 of the current frequency (about half a semitone), check harmonic and sub-harmonic matches, treat a strong different candidate as a note shift, and median-of-3 smoothing. It also offers `predict_frequency()`, a period estimate from the latest edges before the window is complete.
- Dependencies seen: `std::vector` inside `bitset` (heap), `ring_buffer<info>` for the edge records, `<cmath>`, `<stdexcept>` (the period detector's constructor throws when highest <= lowest), `<array>`, `<utility>`; the user-defined unit types `frequency` and `decibel`. Floats throughout; `double` only in a duration constructor.
- State size (partly inferred): bitset = 2 x lowest period bits (the page's example: 2205 bits for 40 Hz at 44.1 kHz); the edge ring holds window/2 entries of an `info` struct whose size I did not read. For a 70 Hz lowest voice at 44.1 kHz that is a 1260-bit window and about 630 edge records.
- Parameters arrive in the constructor (lowest, highest, sample rate, hysteresis); per-sample `operator()(float)`; no block API.
- Sample rate: taken in the constructor; periods are in samples.
- GUI: none in the core. QIO (RtAudio, libremidi) and QPlug (CLAP, Elements) stay behind.

## 3. Cost on the MK66 (estimates)

*All numbers here are estimates; only the unit measures them.*

- The zero-crossing update is a few float compares per sample: about 15 cycles x 64 = 960 cycles per block = 0.4 % of 261,224.
- The autocorrelation runs when the collector is ready, every window/2 = 630 samples (about 10 blocks) at 70 Hz lowest. Per lag: 1260 bits = 40 words, half of them looped (about 20 words) x about 20 cycles (shift, OR, XOR, a software popcount, since the M4 has no popcount instruction) = 400 cycles per lag. If 20 edge pairs qualify: 20 x 400 = 8,000 cycles per window/2, so about 800 cycles per block = 0.3 %.
- Total detector about 1,800 cycles per block = 0.7 %. The cheapest candidate by a wide margin.
- Memory: about 0.2 KiB bitstream plus the edge ring (about 630 x 16 B = 10 KiB if `info` is 16 bytes). SRAM; static arrays instead of `std::vector`.

## 4. Mapping to a unit

- Only the detector would be taken, as a hand port of about 300 lines: a fixed-size bitset (`std::uint32_t` words), a fixed ring of edge records, no exceptions, no unit types (Hz and linear amplitude as plain `float`). A direct include is impossible (C++20, heap, exceptions).
- Latency is the attraction: a first estimate needs about one window (28.6 ms at 70 Hz lowest, or 20 ms at 100 Hz), but `predict_period()` yields an estimate from the latest edges at about one period.
- Parameters it would add: RANGE (lowest and highest), a hysteresis in dB (the voiced gate), nothing else.

## 5. Risks

- Written for monophonic instruments (guitar and similar); I found no evidence in the sources fetched about how it behaves on sung vowels with strong formants, where one period holds several zero crossings and pulses. The 60 % pulse rule and the harmonic checks may or may not cope. **Unknown: must be tried on recorded voice (sung and spoken, with consonants and breath) on the host before it is trusted.**
- Voiced/unvoiced: periodicity gives a measure (0.8 / 0.9 thresholds) but I read nothing about consonants.
- Size of the edge records and the exact readiness condition were not visible.

## 6. Verdict

**Adapt (port the BACF core) as a benchmark rival to the clean-room YIN.** Cheapest and potentially lowest latency; robustness on voice unproven. Recommended: run both on the same voice recordings on the host and keep the one with fewer octave errors and false voiced decisions. The summary card picks YIN as the default because the YIN paper's evidence is on speech (see `clean-room-yin-mpm-psola.md`).

## Sources

- https://github.com/cycfi/q
- https://raw.githubusercontent.com/cycfi/q/develop/LICENSE
- https://raw.githubusercontent.com/cycfi/q/develop/q_lib/include/q/pitch/pitch_detector.hpp
- https://raw.githubusercontent.com/cycfi/q/develop/q_lib/include/q/pitch/bacf_period_detector.hpp
- https://raw.githubusercontent.com/cycfi/q/develop/q_lib/include/q/utility/bitstream_acf.hpp
- https://raw.githubusercontent.com/cycfi/q/develop/q_lib/include/q/utility/bitset.hpp
- https://raw.githubusercontent.com/cycfi/q/develop/q_lib/include/q/utility/zero_crossing_collector.hpp
