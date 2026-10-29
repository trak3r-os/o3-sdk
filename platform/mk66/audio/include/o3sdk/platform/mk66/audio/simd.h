// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Cortex-M4 DSP instructions the graph's kernels use (OS-005b, ADR 0013 D2: "one kernel source runs on host, QEMU and target"), as
// two interchangeable policies. A kernel (kernels_impl.h) is written once over an `Ops` type:
//
//   M4Ops    the instructions: SMLAWB / SMLAWT (a 32-bit gain times a halfword, the top 32 bits of the 48-bit product, accumulated), SMLALD
//            (two 16 x 16 products into a 64-bit accumulator), SSUB16 + SEL (the lane-wise maximum and minimum of two int16 pairs), SHSUB16 +
//            SSUB16 (the lane-wise average rounded half up), PKHBT / PKHTB (halfwords into words) and SSAT (with its free shift). They come
//            from ACLE <arm_acle.h> where it has them and from one inline-asm statement where it has not (the packs, the pairs of
//            instructions that share GE flags or a scratch register: the same idiom as ui/plot/peak_kernel.h).
//   EmuOps   a step-by-step model of what each instruction does, in portable C++, runnable anywhere.
//
// The host and QEMU unit tests run the kernels with EmuOps against a scalar reference (which proves the lane, head and tail handling of
// the SIMD loops, on every host), and the target run compares M4Ops with EmuOps bit for bit (which proves the model of the instructions).
// `NativeOps` is M4Ops where the DSP extension exists and EmuOps elsewhere, so a host build of the production kernels is the same
// algorithm on the model. A value pair is a std::uint32_t: lane 0 (the lower address, the left sample of a frame) in bits 0..15, lane 1 in
// bits 16..31 (little-endian, which usb/usb_types.h already asserts).
//
// Every function is always_inline: the callers are in SRAM_L (hot.h) and a call into flash from there is refused by tools/check_ramfunc.py.

#include <o3sdk/platform/mk66/audio/hot.h>

#include <cstdint>

#if defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
#endif

namespace o3sdk::platform::mk66::audio::simd {

// ---- Lane helpers (portable, shared by both policies) -------------------------------------------------------------------
O3SDK_AUDIO_INLINE std::int32_t lane0(std::uint32_t w) noexcept { return static_cast<std::int16_t>(static_cast<std::uint16_t>(w & 0xFFFFu)); }
O3SDK_AUDIO_INLINE std::int32_t lane1(std::uint32_t w) noexcept { return static_cast<std::int16_t>(static_cast<std::uint16_t>(w >> 16)); }
O3SDK_AUDIO_INLINE std::uint32_t pack_lanes(std::int32_t lo, std::int32_t hi) noexcept {
  return static_cast<std::uint32_t>(lo & 0xFFFF) | (static_cast<std::uint32_t>(hi) << 16);
}

// ---- The model ----------------------------------------------------------------------------------------------------------
struct EmuOps {
  // SMLAWB: acc + ((a * signed lane 0 of w) >> 16), the shift of the exact 48-bit product (floor), the add modulo 2^32.
  static O3SDK_AUDIO_INLINE std::int32_t smlawb(std::int32_t a, std::uint32_t w, std::int32_t acc) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(acc) +
                                     static_cast<std::uint32_t>(static_cast<std::int32_t>((static_cast<std::int64_t>(a) * lane0(w)) >> 16)));
  }
  static O3SDK_AUDIO_INLINE std::int32_t smlawt(std::int32_t a, std::uint32_t w, std::int32_t acc) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(acc) +
                                     static_cast<std::uint32_t>(static_cast<std::int32_t>((static_cast<std::int64_t>(a) * lane1(w)) >> 16)));
  }
  // SMLALD: acc + lane0(a) * lane0(b) + lane1(a) * lane1(b), modulo 2^64.
  static O3SDK_AUDIO_INLINE std::uint64_t smlald(std::uint32_t a, std::uint32_t b, std::uint64_t acc) noexcept {
    const std::int64_t p = static_cast<std::int64_t>(lane0(a)) * lane0(b) + static_cast<std::int64_t>(lane1(a)) * lane1(b);
    return acc + static_cast<std::uint64_t>(p);
  }
  // SSUB16 + SEL: per lane the larger (smaller) of the two signed halfwords.
  static O3SDK_AUDIO_INLINE std::uint32_t max2(std::uint32_t a, std::uint32_t b) noexcept {
    return pack_lanes(lane0(a) >= lane0(b) ? lane0(a) : lane0(b), lane1(a) >= lane1(b) ? lane1(a) : lane1(b));
  }
  static O3SDK_AUDIO_INLINE std::uint32_t min2(std::uint32_t a, std::uint32_t b) noexcept {
    return pack_lanes(lane0(a) >= lane0(b) ? lane0(b) : lane0(a), lane1(a) >= lane1(b) ? lane1(b) : lane1(a));
  }
  // SHSUB16 + SSUB16: per lane (a + b + 1) >> 1 of the exact 17-bit sum, the average rounded half up (a - ((a - b) >> 1), which stays in range).
  static O3SDK_AUDIO_INLINE std::uint32_t avg_round(std::uint32_t a, std::uint32_t b) noexcept {
    return pack_lanes((lane0(a) + lane0(b) + 1) >> 1, (lane1(a) + lane1(b) + 1) >> 1);
  }
  // PKHBT rd, lo, hi, LSL #16: the low halfword of `lo` and the low halfword of `hi` (callers pass values already in int16 range, or
  // want the truncation).
  static O3SDK_AUDIO_INLINE std::uint32_t pack(std::int32_t lo, std::int32_t hi) noexcept { return pack_lanes(lo, hi); }
  // PKHBT / PKHTB: the low lanes of two words ([a.lane0, b.lane0]) and their high lanes ([a.lane1, b.lane1]): the two planes of two stereo frames.
  static O3SDK_AUDIO_INLINE std::uint32_t pack_lo(std::uint32_t a, std::uint32_t b) noexcept { return pack_lanes(lane0(a), lane0(b)); }
  static O3SDK_AUDIO_INLINE std::uint32_t pack_hi(std::uint32_t a, std::uint32_t b) noexcept { return pack_lanes(lane1(a), lane1(b)); }
  // QADD16: per lane the sum saturated to int16 (OS-CPUOPT1: the loop player adds one value to both channels of a frame in one instruction).
  static O3SDK_AUDIO_INLINE std::uint32_t qadd16(std::uint32_t a, std::uint32_t b) noexcept {
    const std::int32_t l0 = lane0(a) + lane0(b);
    const std::int32_t l1 = lane1(a) + lane1(b);
    return pack_lanes(l0 > 32767 ? 32767 : (l0 < -32768 ? -32768 : l0), l1 > 32767 ? 32767 : (l1 < -32768 ? -32768 : l1));
  }
  // SSAT #16.
  static O3SDK_AUDIO_INLINE std::int32_t ssat16(std::int32_t x) noexcept { return x > 32767 ? 32767 : (x < -32768 ? -32768 : x); }
  // SSAT #16, ASR #4: the shift (a floor) and the saturation of the Q19 -> Q15 step, one instruction.
  static O3SDK_AUDIO_INLINE std::int32_t ssat16_q4(std::int32_t x) noexcept { return ssat16(x >> 4); }
};

#if defined(__ARM_FEATURE_SIMD32)
struct M4Ops {
  static O3SDK_AUDIO_INLINE std::int32_t smlawb(std::int32_t a, std::uint32_t w, std::int32_t acc) noexcept {
    return __smlawb(a, static_cast<std::int32_t>(w), acc);
  }
  static O3SDK_AUDIO_INLINE std::int32_t smlawt(std::int32_t a, std::uint32_t w, std::int32_t acc) noexcept {
    return __smlawt(a, static_cast<std::int32_t>(w), acc);
  }
  static O3SDK_AUDIO_INLINE std::uint64_t smlald(std::uint32_t a, std::uint32_t b, std::uint64_t acc) noexcept {
    return static_cast<std::uint64_t>(__smlald(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b), static_cast<std::int64_t>(acc)));
  }
  static O3SDK_AUDIO_INLINE std::uint32_t max2(std::uint32_t a, std::uint32_t b) noexcept {
    std::uint32_t r;
    // "=&r": the SSUB16 result is a scratch that must not share a register with an input of the SEL that follows.
    __asm__("ssub16 %0, %1, %2\n\tsel %0, %1, %2" : "=&r"(r) : "r"(a), "r"(b) : "cc");
    return r;
  }
  static O3SDK_AUDIO_INLINE std::uint32_t min2(std::uint32_t a, std::uint32_t b) noexcept {
    std::uint32_t r;
    __asm__("ssub16 %0, %1, %2\n\tsel %0, %2, %1" : "=&r"(r) : "r"(a), "r"(b) : "cc");
    return r;
  }
  static O3SDK_AUDIO_INLINE std::uint32_t avg_round(std::uint32_t a, std::uint32_t b) noexcept {
    std::uint32_t r;
    // ceil((a + b) / 2) = a - floor((a - b) / 2), per lane: SHSUB16 halves the difference with a floor, SSUB16 takes it from a.
    __asm__("shsub16 %0, %1, %2\n\tssub16 %0, %1, %0" : "=&r"(r) : "r"(a), "r"(b));
    return r;
  }
  static O3SDK_AUDIO_INLINE std::uint32_t pack(std::int32_t lo, std::int32_t hi) noexcept {
    std::uint32_t r;
    __asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(lo), "r"(hi));
    return r;
  }
  static O3SDK_AUDIO_INLINE std::uint32_t pack_lo(std::uint32_t a, std::uint32_t b) noexcept {  // [a.lane0, b.lane0]
    std::uint32_t r;
    __asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(a), "r"(b));
    return r;
  }
  static O3SDK_AUDIO_INLINE std::uint32_t pack_hi(std::uint32_t a, std::uint32_t b) noexcept {  // [a.lane1, b.lane1]
    std::uint32_t r;
    __asm__("pkhtb %0, %1, %2, asr #16" : "=r"(r) : "r"(b), "r"(a));
    return r;
  }
  static O3SDK_AUDIO_INLINE std::uint32_t qadd16(std::uint32_t a, std::uint32_t b) noexcept { return __qadd16(a, b); }
  static O3SDK_AUDIO_INLINE std::int32_t ssat16(std::int32_t x) noexcept {
    std::int32_t r;
    __asm__("ssat %0, #16, %1" : "=r"(r) : "r"(x) : "cc");
    return r;
  }
  static O3SDK_AUDIO_INLINE std::int32_t ssat16_q4(std::int32_t x) noexcept {
    std::int32_t r;
    __asm__("ssat %0, #16, %1, asr #4" : "=r"(r) : "r"(x) : "cc");
    return r;
  }
};
using NativeOps = M4Ops;
inline constexpr bool kNativeIsHardware = true;
#else
using NativeOps = EmuOps;
inline constexpr bool kNativeIsHardware = false;
#endif

}  // namespace o3sdk::platform::mk66::audio::simd
