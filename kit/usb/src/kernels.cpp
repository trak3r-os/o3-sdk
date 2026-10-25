// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/usb/kernels.h>

#include <cstddef>
#include <cstring>

#if defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
#endif

namespace o3sdk::usb {
namespace {

// Two 16-bit lanes in one 32-bit word: lane 0 in bits 0..15, lane 1 in bits 16..31.
using Lanes = std::uint32_t;

inline Lanes load_lanes(const std::int16_t* p) noexcept {
  Lanes w;
  std::memcpy(&w, p, sizeof w);  // callers guarantee 4-byte alignment
  return w;
}
inline void store_lanes(std::int16_t* p, Lanes w) noexcept { std::memcpy(p, &w, sizeof w); }

inline std::int16_t as_s16(std::uint32_t v) noexcept {
  const std::uint16_t u = static_cast<std::uint16_t>(v & 0xFFFFu);
  std::int16_t s;
  std::memcpy(&s, &u, sizeof s);
  return s;
}

#if defined(__ARM_FEATURE_SIMD32)
// PKHBT / PKHTB: one instruction each on the M4 (the compiler would emit uxth + orr for the portable form).
inline Lanes pack_lo(Lanes a, Lanes b) noexcept {  // [a.lane0, b.lane0]
  Lanes r;
  __asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(a), "r"(b));
  return r;
}
inline Lanes pack_hi(Lanes a, Lanes b) noexcept {  // [a.lane1, b.lane1]
  Lanes r;
  __asm__("pkhtb %0, %1, %2, asr #16" : "=r"(r) : "r"(b), "r"(a));
  return r;
}
inline Lanes qadd_lanes(Lanes a, Lanes b) noexcept {  // per-lane saturating int16 add (QADD16)
  return static_cast<Lanes>(__qadd16(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b)));
}
#else
inline Lanes pack_lo(Lanes a, Lanes b) noexcept { return (a & 0xFFFFu) | (b << 16); }
inline Lanes pack_hi(Lanes a, Lanes b) noexcept { return (a >> 16) | (b & 0xFFFF0000u); }
inline std::int32_t sat16(std::int32_t v) noexcept { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
inline Lanes qadd_lanes(Lanes a, Lanes b) noexcept {
  const std::int32_t lo = sat16(static_cast<std::int32_t>(as_s16(a)) + static_cast<std::int32_t>(as_s16(b)));
  const std::int32_t hi = sat16(static_cast<std::int32_t>(as_s16(a >> 16)) + static_cast<std::int32_t>(as_s16(b >> 16)));
  return static_cast<Lanes>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(lo) & 0xFFFFu)) |
         (static_cast<Lanes>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(hi) & 0xFFFFu)) << 16);
}
#endif

inline bool aligned4(const void* p) noexcept { return (reinterpret_cast<std::uintptr_t>(p) & 3u) == 0u; }

// Two adjacent samples to `p`: one word store when `p` is 4-byte aligned, two halfword stores when it is not
// (interleaving an odd channel count puts every second frame at a 2-byte offset). Never an unaligned word.
inline void store_pair(std::int16_t* p, Lanes w) noexcept {
  if (aligned4(p)) {
    store_lanes(p, w);
  } else {
    p[0] = as_s16(w);
    p[1] = as_s16(w >> 16);
  }
}

inline std::int16_t sat_add(std::int16_t a, std::int16_t b) noexcept {
  const std::int32_t v = static_cast<std::int32_t>(a) + static_cast<std::int32_t>(b);
  return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

}  // namespace

// ---- interleave -------------------------------------------------------------------------------------------

void interleave_scalar(const std::int16_t* const* planes, std::uint32_t channels, std::uint32_t offset,
                       std::uint32_t frames, std::int16_t* out) noexcept {
  for (std::uint32_t f = 0; f < frames; ++f) {
    for (std::uint32_t c = 0; c < channels; ++c) {
      out[f * channels + c] = planes[c][offset + f];
    }
  }
}

void interleave(const std::int16_t* const* planes, std::uint32_t channels, std::uint32_t offset,
                std::uint32_t frames, std::int16_t* out) noexcept {
  // Word loads need every plane 4-byte aligned at its first frame; planes share one frame offset, so one
  // scalar frame fixes an odd offset for all of them.
  bool ok = true;
  for (std::uint32_t c = 0; c < channels; ++c) {
    ok = ok && aligned4(planes[c]);
  }
  if (!ok) {
    interleave_scalar(planes, channels, offset, frames, out);
    return;
  }
  std::uint32_t f = 0;
  if ((offset & 1u) != 0u && frames > 0u) {
    for (std::uint32_t c = 0; c < channels; ++c) {
      out[c] = planes[c][offset];
    }
    f = 1;
  }
  const std::uint32_t pairs_c = channels & ~1u;
  for (; f + 2u <= frames; f += 2u) {
    std::int16_t* o0 = out + f * channels;
    std::int16_t* o1 = o0 + channels;
    for (std::uint32_t c = 0; c < pairs_c; c += 2u) {
      const Lanes a = load_lanes(planes[c] + offset + f);
      const Lanes b = load_lanes(planes[c + 1u] + offset + f);
      store_pair(o0 + c, pack_lo(a, b));  // frame f:   [plane c, plane c+1]
      store_pair(o1 + c, pack_hi(a, b));  // frame f+1: [plane c, plane c+1]
    }
    if (pairs_c != channels) {  // odd channel count: the last plane one sample at a time
      o0[pairs_c] = planes[pairs_c][offset + f];
      o1[pairs_c] = planes[pairs_c][offset + f + 1u];
    }
  }
  if (f < frames) {  // odd remainder: one frame
    for (std::uint32_t c = 0; c < channels; ++c) {
      out[f * channels + c] = planes[c][offset + f];
    }
  }
}

// ---- stereo deinterleave -----------------------------------------------------------------------------------

void deinterleave_stereo_scalar(const std::uint32_t* in, std::uint32_t frames, std::int16_t* left,
                                std::int16_t* right) noexcept {
  for (std::uint32_t i = 0; i < frames; ++i) {
    left[i] = as_s16(in[i]);
    right[i] = as_s16(in[i] >> 16);
  }
}

void deinterleave_stereo(const std::uint32_t* in, std::uint32_t frames, std::int16_t* left,
                         std::int16_t* right) noexcept {
  std::uint32_t i = 0;
  if (aligned4(left) && aligned4(right)) {
    for (; i + 2u <= frames; i += 2u) {
      store_lanes(left + i, pack_lo(in[i], in[i + 1u]));    // [L(i), L(i+1)]
      store_lanes(right + i, pack_hi(in[i], in[i + 1u]));   // [R(i), R(i+1)]
    }
  }
  for (; i < frames; ++i) {
    left[i] = as_s16(in[i]);
    right[i] = as_s16(in[i] >> 16);
  }
}

// ---- slot-mask saturating sum ------------------------------------------------------------------------------

void sum_slots_scalar(const std::int16_t* const* sources, std::uint32_t mask, std::uint32_t frames,
                      std::int16_t* dst) noexcept {
  for (std::uint32_t f = 0; f < frames; ++f) {
    std::int16_t acc = 0;
    for (std::uint32_t s = 0; s < kSlotSources; ++s) {
      if (((mask >> s) & 1u) != 0u && sources[s] != nullptr) {
        acc = sat_add(acc, sources[s][f]);
      }
    }
    dst[f] = acc;
  }
}

void sum_slots(const std::int16_t* const* sources, std::uint32_t mask, std::uint32_t frames,
               std::int16_t* dst) noexcept {
  // Gather the selected non-null sources once; the sample loops then run over a short dense list.
  const std::int16_t* sel[kSlotSources];
  std::uint32_t n = 0;
  bool ok = aligned4(dst);
  for (std::uint32_t s = 0; s < kSlotSources; ++s) {
    if (((mask >> s) & 1u) != 0u && sources[s] != nullptr) {
      sel[n++] = sources[s];
      ok = ok && aligned4(sources[s]);
    }
  }
  if (n == 0u) {
    for (std::uint32_t f = 0; f < frames; ++f) {
      dst[f] = 0;
    }
    return;
  }
  std::uint32_t f = 0;
  if (ok) {
    for (; f + 2u <= frames; f += 2u) {
      Lanes acc = load_lanes(sel[0] + f);
      for (std::uint32_t k = 1; k < n; ++k) {
        acc = qadd_lanes(acc, load_lanes(sel[k] + f));
      }
      store_lanes(dst + f, acc);
    }
  }
  for (; f < frames; ++f) {
    std::int16_t acc = sel[0][f];  // sat(0 + x) == x
    for (std::uint32_t k = 1; k < n; ++k) {
      acc = sat_add(acc, sel[k][f]);
    }
    dst[f] = acc;
  }
}

const std::int16_t* mix_slot(const std::int16_t* const* sources, std::uint32_t mask, std::uint32_t frames,
                             std::int16_t* dst) noexcept {
  std::uint32_t count = 0;
  const std::int16_t* only = nullptr;
  for (std::uint32_t s = 0; s < kSlotSources; ++s) {
    if (((mask >> s) & 1u) != 0u && sources[s] != nullptr) {
      ++count;
      only = sources[s];
    }
  }
  if (count == 1u) {
    return only;
  }
  sum_slots(sources, mask, frames, dst);
  return dst;
}

}  // namespace o3sdk::usb
