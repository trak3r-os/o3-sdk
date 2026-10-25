// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::usb {

// Sample-format kernels of the USB path. Each has a scalar reference (`*_scalar`, the specification) and a
// word-at-a-time variant (no suffix) that handles two 16-bit samples per 32-bit operation: PKHBT/PKHTB
// packing and QADD16 saturating adds on the Cortex-M4 (the compiler emits them from the portable code and the
// intrinsic where one exists), plain shifts and masks on the host. The two are tested against each other for
// random lengths, offsets and alignments and must be bit-identical.
//
// No loop is unbounded: every loop runs for `frames` or `channels`, both small and caller-bounded.
// Word accesses only happen at 4-byte aligned addresses (ADR 0011 lesson 1), so none of these kernels needs
// unaligned-access support (a test runs them with CCR.UNALIGN_TRP set on the target):
//   - loads: the planes must be 4-byte aligned at their first frame (checked per call, scalar path otherwise);
//     an odd frame offset is fixed with one scalar frame first;
//   - stores: a word store goes only to a 4-byte aligned address, the other half of an interleaved pair (odd
//     channel counts, e.g. the 11 channels of profile P11) is written as two halfwords;
//   - deinterleave and sum take the scalar path unless their destinations are 4-byte aligned.
// Buffers that feed these kernels are declared alignas(4).

// Interleave planar channels into frame-major order:
//   out[f * channels + c] = planes[c][offset + f]   for f < frames, c < channels.
// `planes` holds `channels` pointers, each to at least offset + frames samples; `out` has frames * channels
// samples. out and the planes must not overlap.
void interleave_scalar(const std::int16_t* const* planes, std::uint32_t channels, std::uint32_t offset,
                       std::uint32_t frames, std::int16_t* out) noexcept;
void interleave(const std::int16_t* const* planes, std::uint32_t channels, std::uint32_t offset,
                std::uint32_t frames, std::int16_t* out) noexcept;

// Split packed stereo frames (one uint32_t per frame: left in bits 0..15, right in bits 16..31, which is
// the memory layout of interleaved little-endian int16 L,R) into two planes:
//   left[i] = (int16)(in[i] & 0xFFFF), right[i] = (int16)(in[i] >> 16).
void deinterleave_stereo_scalar(const std::uint32_t* in, std::uint32_t frames, std::int16_t* left,
                                std::int16_t* right) noexcept;
void deinterleave_stereo(const std::uint32_t* in, std::uint32_t frames, std::int16_t* left,
                         std::int16_t* right) noexcept;

// Slot-mask saturating sum (ADR 0012 Decision 3). A stem slot sums a set of sources: bit i of `mask`
// selects sources[i] (T1..T12 taps, Master L, Master R, Send A, Send B: up to kSlotSources). A selected
// null pointer counts as silence. The sum is sequential in ascending bit order with int16 saturation after
// every addition: acc = sat(sat(s_a + s_b) + s_c) ...; saturation makes addition non-associative, so the
// order is part of the definition and both variants follow it.
inline constexpr std::uint32_t kSlotSources = 16;
void sum_slots_scalar(const std::int16_t* const* sources, std::uint32_t mask, std::uint32_t frames,
                      std::int16_t* dst) noexcept;
void sum_slots(const std::int16_t* const* sources, std::uint32_t mask, std::uint32_t frames,
               std::int16_t* dst) noexcept;

// What a slot actually needs: with exactly one (non-null) source selected it returns that source's own
// pointer (no arithmetic, no copy: the caller must treat the block as read-only); with none it zero-fills dst
// and returns dst; with several it sums into dst and returns dst. mask bits above kSlotSources are ignored.
const std::int16_t* mix_slot(const std::int16_t* const* sources, std::uint32_t mask, std::uint32_t frames,
                             std::int16_t* dst) noexcept;

}  // namespace o3sdk::usb
