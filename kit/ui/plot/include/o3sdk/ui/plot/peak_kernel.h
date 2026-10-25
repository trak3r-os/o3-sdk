// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The int16 min/max kernels under the peak cache (ADR 0010 amendment 2). The Cortex-M4's DSP extension compares two
// int16 lanes at once: SSUB16 sets one GE flag per lane (lane a >= lane b) and SEL then picks, lane by lane, from either
// register. `ssub16 t, a, b; sel r, a, b` is the lane-wise maximum, with the operands swapped the minimum, which is two
// samples (or, for interleaved stereo, the left and the right of one frame) per word, 2.5 instructions a sample against
// about six for a compare-and-select loop.
//
// The same loop is written once, over an "Ops" policy: M4Ops is the two instructions in one inline-asm statement (so nothing
// the compiler emits can separate them or touch the GE flags in between), EmuOps is a step-by-step model of what the
// instructions do, runnable anywhere. The host unit tests run the loop with EmuOps against a scalar reference, which
// proves the lane, head and tail handling; the instructions themselves are what a build for the MK66 adds, and are
// checked on QEMU's Cortex-M4 and on the hardware (docs/specs/OS-003-ui-toolkit.md, 003d). Host builds use the scalar loop.
//
// Frames are int16 samples interleaved by channel (1 = mono, 2 = stereo L R). The buffer is word aligned: that is declared
// by the owner of the samples and checked by PeakCache::init(). Words are loaded with memcpy, never through a cast.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3sdk::ui::plot {

using Frame = std::uint32_t;  // a frame index: the SDRAM holds at most 16 Mi of them

namespace detail {

// Portable reference and the host's implementation: `count` samples `stride` apart.
inline void minmax_scalar(const std::int16_t* p, std::size_t count, std::size_t stride, int& mn, int& mx) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    const int v = p[i * stride];
    mn = v < mn ? v : mn;
    mx = v > mx ? v : mx;
  }
}

constexpr std::int16_t lane_lo(std::uint32_t w) noexcept {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(w & 0xFFFFu));
}
constexpr std::int16_t lane_hi(std::uint32_t w) noexcept {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(w >> 16));
}
constexpr std::uint32_t pack(std::int16_t lo, std::int16_t hi) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint16_t>(lo)) |
         (static_cast<std::uint32_t>(static_cast<std::uint16_t>(hi)) << 16);
}

// What SSUB16 + SEL do (ARMv7-M ARM, A7.7.127 SSUB16 and A7.7.119 SEL): GE[i] = (signed lane i of a) - (lane i of b) >= 0
// and SEL takes lane i from the first register where GE[i] is set, else from the second.
struct EmuOps {
  static std::uint32_t max2(std::uint32_t a, std::uint32_t b) noexcept {
    return pack(lane_lo(a) >= lane_lo(b) ? lane_lo(a) : lane_lo(b), lane_hi(a) >= lane_hi(b) ? lane_hi(a) : lane_hi(b));
  }
  static std::uint32_t min2(std::uint32_t a, std::uint32_t b) noexcept {
    // ssub16 t, a, b; sel r, b, a: lane from b where a >= b, else from a
    return pack(lane_lo(a) >= lane_lo(b) ? lane_lo(b) : lane_lo(a), lane_hi(a) >= lane_hi(b) ? lane_hi(b) : lane_hi(a));
  }
};

#if defined(__ARM_FEATURE_SIMD32)
struct M4Ops {
  static inline std::uint32_t max2(std::uint32_t a, std::uint32_t b) noexcept {
    std::uint32_t r;
    // "=&r": the SSUB16 result is a scratch that must not share a register with an input of the SEL that follows.
    asm("ssub16 %0, %1, %2\n\tsel %0, %1, %2" : "=&r"(r) : "r"(a), "r"(b) : "cc");
    return r;
  }
  static inline std::uint32_t min2(std::uint32_t a, std::uint32_t b) noexcept {
    std::uint32_t r;
    asm("ssub16 %0, %1, %2\n\tsel %0, %2, %1" : "=&r"(r) : "r"(a), "r"(b) : "cc");
    return r;
  }
};
using NativeOps = M4Ops;
#endif

inline std::uint32_t load_word(const std::int16_t* p) noexcept {
  std::uint32_t w;
  std::memcpy(&w, p, sizeof(w));
  return w;
}

// Min and max of channel `ch` over frames [a, b) of the interleaved buffer `p`, folded into mn / mx. `p` is word aligned.
template <class Ops>
inline void minmax_simd(const std::int16_t* p, unsigned channels, unsigned ch, Frame a, Frame b, int& mn,
                        int& mx) noexcept {
  if (a >= b) {
    return;
  }
  std::uint32_t lo = pack(32767, 32767);    // running minimum, both lanes
  std::uint32_t hi = pack(-32768, -32768);  // running maximum
  if (channels == 2) {                      // one word is one frame: lane 0 = L, lane 1 = R
    Frame f = a;
    for (; f + 4 <= b; f += 4) {
      const std::uint32_t w0 = load_word(p + 2u * f), w1 = load_word(p + 2u * (f + 1)), w2 = load_word(p + 2u * (f + 2)),
                          w3 = load_word(p + 2u * (f + 3));
      lo = Ops::min2(Ops::min2(lo, w0), Ops::min2(w1, Ops::min2(w2, w3)));
      hi = Ops::max2(Ops::max2(hi, w0), Ops::max2(w1, Ops::max2(w2, w3)));
    }
    for (; f < b; ++f) {
      const std::uint32_t w = load_word(p + 2u * f);
      lo = Ops::min2(lo, w);
      hi = Ops::max2(hi, w);
    }
    const int lo_v = ch == 0 ? lane_lo(lo) : lane_hi(lo);
    const int hi_v = ch == 0 ? lane_lo(hi) : lane_hi(hi);
    mn = lo_v < mn ? lo_v : mn;
    mx = hi_v > mx ? hi_v : mx;
    return;
  }
  Frame i = a;  // mono: two consecutive samples a word; an odd first sample is taken alone
  if ((i & 1u) != 0u) {
    const int v = p[i];
    mn = v < mn ? v : mn;
    mx = v > mx ? v : mx;
    ++i;
  }
  for (; i + 8 <= b; i += 8) {
    const std::uint32_t w0 = load_word(p + i), w1 = load_word(p + i + 2), w2 = load_word(p + i + 4),
                        w3 = load_word(p + i + 6);
    lo = Ops::min2(Ops::min2(lo, w0), Ops::min2(w1, Ops::min2(w2, w3)));
    hi = Ops::max2(Ops::max2(hi, w0), Ops::max2(w1, Ops::max2(w2, w3)));
  }
  for (; i + 2 <= b; i += 2) {
    const std::uint32_t w = load_word(p + i);
    lo = Ops::min2(lo, w);
    hi = Ops::max2(hi, w);
  }
  if (i < b) {
    const int v = p[i];
    mn = v < mn ? v : mn;
    mx = v > mx ? v : mx;
  }
  const int lo_v = lane_lo(lo) < lane_hi(lo) ? lane_lo(lo) : lane_hi(lo);
  const int hi_v = lane_lo(hi) > lane_hi(hi) ? lane_lo(hi) : lane_hi(hi);
  mn = lo_v < mn ? lo_v : mn;
  mx = hi_v > mx ? hi_v : mx;
}

// The kernel the cache uses: the SIMD loop on the MK66 for ranges long enough to pay for it, the scalar loop otherwise.
inline void scan_raw(const std::int16_t* p, unsigned channels, unsigned ch, Frame a, Frame b, int& mn,
                     int& mx) noexcept {
#if defined(__ARM_FEATURE_SIMD32)
  if (b - a >= 16u) {
    minmax_simd<NativeOps>(p, channels, ch, a, b, mn, mx);
    return;
  }
#endif
  minmax_scalar(p + static_cast<std::size_t>(a) * channels + ch, b - a, channels, mn, mx);
}

}  // namespace detail
}  // namespace o3sdk::ui::plot
