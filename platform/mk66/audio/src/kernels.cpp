// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/kernels.h>
#include <o3sdk/platform/mk66/audio/kernels_impl.h>

namespace o3sdk::platform::mk66::audio::kernel {
namespace {

// A lane for a centre kernel: `right` is not read, and is made harmless rather than left to whatever the caller put there.
O3SDK_AUDIO_INLINE StripLane centre_lane(const StripLane& a) noexcept {
  StripLane c = a;
  c.right = GainRamp{0, 0};
  return c;
}

}  // namespace

// ---- The references: the specification, in the plainest code ----------------------------------------------------------------

void gain_ref(std::int16_t* dst, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = detail::gain_sample(src[i], ramp_gain20(start, end, i));
  }
}

void mac_ref(std::int32_t* acc, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    acc[i] = detail::add_wrap(acc[i], detail::prod_q19(src[i], ramp_gain20(start, end, i)));
  }
}

void gain_pan_ref(const StripLane& a, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  gain_ref(a.x, a.x, n, a.level.start, a.level.end);
  mac_ref(acc_l, a.x, n, a.left.start, a.left.end);
  mac_ref(acc_r, a.x, n, a.right.start, a.right.end);
}

void gain_mac_ref(const StripLane& a, std::int32_t* acc, std::uint32_t n) noexcept {
  gain_ref(a.x, a.x, n, a.level.start, a.level.end);
  mac_ref(acc, a.x, n, a.left.start, a.left.end);
}

void gain_pan2_ref(const StripLane& a, const StripLane& b, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  gain_pan_ref(a, acc_l, acc_r, n);
  gain_pan_ref(b, acc_l, acc_r, n);
}

void gain_mac2_ref(const StripLane& a, const StripLane& b, std::int32_t* acc, std::uint32_t n) noexcept {
  gain_mac_ref(a, acc, n);
  gain_mac_ref(b, acc, n);
}

void sat_clear_ref(std::int16_t* dst, std::int32_t* acc, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = detail::q19_to_q15(acc[i]);
    acc[i] = 0;
  }
}

void sat_clear_main_ref(std::int16_t* dst_l, std::int16_t* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t c = acc_c != nullptr ? acc_c[i] : 0;
    dst_l[i] = detail::q19_to_q15(detail::add_wrap(acc_l[i], c));
    dst_r[i] = detail::q19_to_q15(detail::add_wrap(acc_r[i], c));
    acc_l[i] = 0;
    acc_r[i] = 0;
    if (acc_c != nullptr) {
      acc_c[i] = 0;
    }
  }
}

void acc_to_float_main_ref(float* dst_l, float* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t c = acc_c != nullptr ? acc_c[i] : 0;
    dst_l[i] = static_cast<float>(detail::add_wrap(acc_l[i], c)) / 524288.0f;
    dst_r[i] = static_cast<float>(detail::add_wrap(acc_r[i], c)) / 524288.0f;
    acc_l[i] = 0;
    acc_r[i] = 0;
    if (acc_c != nullptr) {
      acc_c[i] = 0;
    }
  }
}

void mono_mix_ref(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(l[i]) + static_cast<std::int32_t>(r[i]) + 1) >> 1);
  }
}

void deinterleave2_ref(std::int16_t* l, std::int16_t* r, const std::int16_t* src, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    l[i] = src[2u * i];
    r[i] = src[2u * i + 1u];
  }
}

void interleave2_ref(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[2u * i] = l[i];
    dst[2u * i + 1u] = r[i];
  }
}

void meter_ref(const std::int16_t* src, std::uint32_t n, MeterAcc& acc) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t v = src[i];
    const std::uint32_t a = static_cast<std::uint32_t>(v < 0 ? -v : v);
    acc.peak = a > acc.peak ? a : acc.peak;
    acc.sumsq += static_cast<std::uint64_t>(static_cast<std::int64_t>(v) * v);
  }
}

void q15_to_float_ref(float* dst, const std::int16_t* src, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = static_cast<float>(src[i]) / 32768.0f;
  }
}

void float_to_q15_ref(std::int16_t* dst, const float* src, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = static_cast<std::int16_t>(q15_from_float(src[i]));
  }
}

// ---- Production: the paired loops on the native instructions (the model of them where there are none) ---------------------

void gain(std::int16_t* dst, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  detail::gain_simd<simd::NativeOps>(dst, src, n, start, end);
}
void mac(std::int32_t* acc, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  detail::mac_simd<simd::NativeOps>(acc, src, n, start, end);
}
void gain_pan(const StripLane& a, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  const StripLane lanes[1] = {a};
  detail::strip_simd<simd::NativeOps, 1, true>(lanes, acc_l, acc_r, n);
}
void gain_mac(const StripLane& a, std::int32_t* acc, std::uint32_t n) noexcept {
  const StripLane lanes[1] = {centre_lane(a)};
  detail::strip_simd<simd::NativeOps, 1, false>(lanes, acc, nullptr, n);
}
void gain_pan2(const StripLane& a, const StripLane& b, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  const StripLane lanes[2] = {a, b};
  detail::strip_simd<simd::NativeOps, 2, true>(lanes, acc_l, acc_r, n);
}
void gain_mac2(const StripLane& a, const StripLane& b, std::int32_t* acc, std::uint32_t n) noexcept {
  const StripLane lanes[2] = {centre_lane(a), centre_lane(b)};
  detail::strip_simd<simd::NativeOps, 2, false>(lanes, acc, nullptr, n);
}
void sat_clear(std::int16_t* dst, std::int32_t* acc, std::uint32_t n) noexcept { detail::sat_clear_simd<simd::NativeOps>(dst, acc, n); }
void sat_clear_main(std::int16_t* dst_l, std::int16_t* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept {
  detail::sat_clear_main_simd<simd::NativeOps>(dst_l, dst_r, acc_l, acc_r, acc_c, n);
}
void acc_to_float_main(float* dst_l, float* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept {
  detail::acc_to_float_main_simd<simd::NativeOps>(dst_l, dst_r, acc_l, acc_r, acc_c, n);
}
void mono_mix(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  detail::mono_mix_simd<simd::NativeOps>(dst, l, r, n);
}
void deinterleave2(std::int16_t* l, std::int16_t* r, const std::int16_t* src, std::uint32_t n) noexcept {
  detail::deinterleave2_simd<simd::NativeOps>(l, r, src, n);
}
void interleave2(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  detail::interleave2_simd<simd::NativeOps>(dst, l, r, n);
}
void meter(const std::int16_t* src, std::uint32_t n, MeterAcc& acc) noexcept { detail::meter_simd<simd::NativeOps>(src, n, acc); }
void q15_to_float(float* dst, const std::int16_t* src, std::uint32_t n) noexcept { detail::q15_to_float_simd<simd::NativeOps>(dst, src, n); }
void float_to_q15(std::int16_t* dst, const float* src, std::uint32_t n) noexcept { detail::float_to_q15_simd<simd::NativeOps>(dst, src, n); }

}  // namespace o3sdk::platform::mk66::audio::kernel
