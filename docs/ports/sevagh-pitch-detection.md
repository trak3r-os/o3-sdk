# sevagh/pitch-detection (YIN, MPM, pYIN)

Researched 2026-11-01. Nothing cloned, built or run.

## 1. Licence (facts)

- Repository: https://github.com/sevagh/pitch-detection, branch `master`; latest release noted on the page: December 27, 2023. No commit pinned.
- `LICENSE`: MIT, "Copyright (c) 2018 Sevag Hanssian" (https://raw.githubusercontent.com/sevagh/pitch-detection/master/LICENSE). The README itself states no licence.
- `src/yin.cpp` excerpt: no licence header in the file.
- Dependencies (library): `ffts` and `mlpack`; I did not read their licences.
- Class: **permissive (MIT)** for the repository's own code; the dependencies are unchecked.

## 2. Shape (facts)

- C++, CMake, Linux/GCC only ("not officially supported" elsewhere). Algorithms: MPM, YIN (FFT variant), pYIN, a probabilistic MPM of the author's own.
- Templates on `float` and `double`; the `#define` thresholds are unsuffixed literals (double); `std::vector` and `std::map<int, T>` inside `yin.cpp`.
- API: a whole buffer in, one pitch out. `pitch::` free functions allocate on every call; `pitch_alloc::Yin` / `Mpm` objects reuse buffers for a fixed size. The FFT length equals the input length. Continuous tracking is left to the caller (the example splits a WAV into 0.01 s chunks).
- YIN steps seen: difference function through an autocorrelation (`util::acorr_r`), cumulative-mean normalisation, absolute threshold (0.20 by default in the define), parabolic interpolation. pYIN sweeps 100 thresholds with a beta prior and runs an HMM (`util::pitch_from_hmm`) over the sequence: an offline Viterbi, not a causal tracker.

## 3. Cost (estimates)

- Not measured, and not worth estimating in cycles: an FFT autocorrelation of a few thousand samples costs on the order of 100k+ cycles per call on this MCU (the TI E2E post reports 280,411 cycles for a 2048-point complex `arm_cfft_f32` on a Cortex-M4F; see `autotalent.md`), and the code allocates per call.

## 4. Mapping

- None directly. The code cannot enter a unit: STL containers and heap in the render path, `double`, `ffts`/`mlpack` dependencies, whole-buffer API.

## 5. Risks

- pYIN is not usable live (HMM over a track).
- FFT window = buffer length, so latency would be set by the buffer.

## 6. Verdict

**Drop as a port.** MIT licence, but unsuitable as code. Two uses remain: (a) its YIN is a convenient **host-side oracle** to compare a clean-room YIN against, on synthetic and recorded voices (keep it out of the image; if it is vendored into a test, keep the MIT notice); (b) its README is a clear map of the algorithms.

## Sources

- https://github.com/sevagh/pitch-detection
- https://raw.githubusercontent.com/sevagh/pitch-detection/master/LICENSE
- https://raw.githubusercontent.com/sevagh/pitch-detection/master/README.md
- https://raw.githubusercontent.com/sevagh/pitch-detection/master/src/yin.cpp
- https://e2e.ti.com/f/1/t/1032595
