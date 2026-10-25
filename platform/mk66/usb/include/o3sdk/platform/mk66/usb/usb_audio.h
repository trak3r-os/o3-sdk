// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB audio capture stream's source in o3-sdk (docs/M2B-PLAN.md, slice D; the maintainer, 2026-10-25: the image boots in track3r-os's default AUDIO set, "8 OUT / 2 IN",
// and USB audio is the master in parallel with the main output). Where track3r-os feeds the stem ring from its StemTap (sums of the tracks the user mapped to each slot of the
// profile), o3-sdk has one source: the MASTER, the block the main output plays. usb::InStream (the USB interrupt) reads the ring this tap fills.
//
//   WHAT THE HOST SEES   the channels of the set presented (usb0::profile(), P8 at boot: eight channels, 16 bit, 44.1 kHz), the master on the LAST TWO, as in every profile of track3r-os
//                        (P8: T1..T6 then master left and right; P10: T1..T8 then master; P11: T1..T9 then master; P2: the master alone). So in the boot set the master is on channels 7 and 8
//                        and channels 1 to 6 are digital silence, reserved for the app's stems later (o3-sdk has no per-track stems yet): a DAW's routing is the same for track3r-os and
//                        o3-sdk (the maintainer, 2026-10-25). The descriptors are track3r-os's, byte for byte, so a host's cached layout is the proven one. P4S (four stereo post-pan
//                        stems) is refused, as in track3r-os.
//   THE RETURN (OUT)     The set still has the stereo return stream and its feedback endpoint (the descriptors are identical); what the host plays into the unit is DISCARDED in this
//                        round (USB return is deferred): while the OUT stream runs, every render block pulls its 64 frames out of the return ring with usb::ReturnReader::pull and drops them,
//                        which is what track3r-os's Graph does before it mixes them. So the ring's fill follows the proven feedback loop, the host gets a working feedback value, and no
//                        overrun is counted; nothing reaches the outputs. The seam for the return is drain_return().
//
// CONTEXTS (one writer per datum):
//   render (priority kRender)  push(), drain_return(), the pool, the drop planes, the phase: wrap() calls them after the app's block callback, once per block.
//   USB interrupt (64)         set_enabled(), set_channels(), set_return_enabled() (the stream hook, usb0.cpp on_streams) and attach_return() (usb0::start): single atomic stores; it also
//                              reads the pool through the ring (usb::StemReader), a packet at a time. It preempts the render.
//   main loop                  the getters.
// STEM RING PROTOCOL (StemTap's, audio/taps.h): a stem block is 128 frames, two render blocks. push() deinterleaves a render block into the slot being produced, and after the second half
// publishes a usb::StemBlock of pointers (ring().write, one wait-free write) and moves to the next slot. LIFETIME CONTRACT (usb/stream.h): a block stays valid until StemRing::capacity() + 2
// further blocks have been written after it, so the pool has capacity() + 2 slots per plane (ring content, the block the reader works on, the one being produced). The channels the
// tap does not feed point at one shared block of zeros that nothing writes.
// COST (estimate from the instruction count, to be measured on the unit, skill rt-optimize): disabled, one relaxed load a block; enabled, about 64 frames x 6 cycles (the two planes) = 400
// cycles a block; the return drain while the OUT stream runs about 1,100 (ReturnReader::pull of 64 frames): under 0.6 % of the 261,000 cycles of a block with both on.
// No heap, no blocking, every loop bounded by the frame count.

#include <o3sdk/hal/audio.h>
#include <o3sdk/usb/stream.h>
#include <o3sdk/usb/usb_types.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::platform::mk66::usb_audio {

// The profiles the tap can feed: every profile but P4S, which track3r-os cannot feed either (StemMap::supported). usb0::start() and request_layout() refuse the others.
constexpr bool supported(o3sdk::usb::Profile profile) noexcept { return profile != o3sdk::usb::Profile::P4S; }

class UsbAudioTap {
 public:
  static constexpr unsigned kPlanes = 2;                                    // master left, master right
  static constexpr unsigned kPool = o3sdk::usb::StemRing::capacity() + 2u;  // blocks per plane: ring + the reader's + the writer's (the lifetime contract)
  static constexpr std::uint32_t kDefaultChannels = 8;                      // the boot profile P8: the master on channels 7 and 8
  static_assert(o3sdk::usb::kStemBlockFrames % hal::audio::kBlock == 0u && o3sdk::usb::kStemBlockFrames >= hal::audio::kBlock,
                "a stem block is a whole number of render blocks");

  UsbAudioTap() noexcept = default;
  UsbAudioTap(const UsbAudioTap&) = delete;
  UsbAudioTap& operator=(const UsbAudioTap&) = delete;

  // The ring usb::IsoAudio's InStream reads (its single consumer is the USB interrupt).
  o3sdk::usb::StemRing& ring() noexcept { return ring_; }

  // ---- USB interrupt or main loop (single atomic stores; a change lands at a block boundary and may mix old and new for one block, as StemTap's) ----------------------------------
  // Off by default: the tap costs one load a block until a stream wants it. Turning it on restarts the block phase at the next render block.
  void set_enabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
  // The channels of the stream (the IN stream's count, usb::profile_channels): the master goes on the last two (channels - 2 and channels - 1, 0-based). Below 2 the master is on 0 and 1;
  // above usb::kMaxChannels it is cut to it.
  void set_channels(std::uint32_t channels) noexcept {
    channels_xor_.store((channels < o3sdk::usb::kMaxChannels ? channels : o3sdk::usb::kMaxChannels) ^ kDefaultChannels, std::memory_order_relaxed);
  }
  std::uint32_t channels() const noexcept { return channels_xor_.load(std::memory_order_relaxed) ^ kDefaultChannels; }
  // The render's end of the return ring (usb::ReturnReader) and whether the OUT stream runs. usb0::start() attaches the reader once, with the audio already running (the USB port starts
  // after it), so the pointer is published with release and read with acquire; the render drains only while both are set.
  void attach_return(o3sdk::usb::ReturnReader* reader) noexcept { reader_.store(reader, std::memory_order_release); }
  void set_return_enabled(bool on) noexcept { return_on_.store(on, std::memory_order_relaxed); }
  bool return_enabled() const noexcept { return return_on_.load(std::memory_order_relaxed); }

  // ---- Render ---------------------------------------------------------------------------------------------------------------------
  // One render block of the master: `lr` is `frames` interleaved stereo frames (the block the main output plays), `frames` hal::audio::kBlock. Nothing happens unless the tap is enabled.
  // A stem block is published after every kStemBlockFrames / kBlock blocks. Other frame counts are accumulated the same way (up to one stem block a call).
  void push(const std::int16_t* lr, std::uint32_t frames) noexcept;
  // Pulls one render block of the return and drops it (see the header). Does nothing unless the OUT stream runs and the reader is attached.
  void drain_return() noexcept;

  // ---- Getters (any context; plain words) -----------------------------------------------------------------------------------------
  std::uint32_t published() const noexcept { return published_.load(std::memory_order_relaxed); }  // stem blocks written
  std::uint32_t drained() const noexcept { return drained_.load(std::memory_order_relaxed); }      // return blocks pulled and dropped

  // Neither side running (tests, before start): the state of a new tap. Never on a tap in use.
  void reset() noexcept;

 private:
  // Zeros for the channels beyond the master pair; nothing writes it, the USB side only reads through it.
  alignas(4) static constexpr std::int16_t kSilence[o3sdk::usb::kStemBlockFrames] = {};

  o3sdk::usb::StemRing ring_;
  alignas(4) std::int16_t pool_[kPlanes][kPool][o3sdk::usb::kStemBlockFrames] = {};
  alignas(4) std::int16_t drop_[kPlanes][hal::audio::kBlock] = {};  // where a pulled return block lands before it is dropped
  std::atomic<bool> enabled_{false};
  std::atomic<bool> return_on_{false};
  // The channel count, stored XOR kDefaultChannels: a tap that is all zeros (static storage before anything ran) has the boot profile's count, so the one tap of the image is `.bss`, not
  // `.data` (a non-zero initialiser would cost the tap's 3 KB of flash and a copy at boot).
  std::atomic<std::uint32_t> channels_xor_{0};
  std::atomic<o3sdk::usb::ReturnReader*> reader_{nullptr};
  // render only
  bool was_enabled_ = false;
  std::uint32_t filled_ = 0;  // frames of the slot being produced already written
  unsigned slot_ = 0;
  std::atomic<std::uint32_t> published_{0};
  std::atomic<std::uint32_t> drained_{0};
};
static_assert(alignof(UsbAudioTap) >= 4u, "the pool is word aligned (the interleave kernel loads words)");
static_assert(o3sdk::usb::kStemBlockFrames * sizeof(std::int16_t) % 4u == 0u && hal::audio::kBlock * sizeof(std::int16_t) % 4u == 0u,
              "every slot of every plane starts on a word boundary, so the packet builder's word loads are aligned");

// The one tap of the image (SRAM_U, .bss): usb0.cpp binds its ring to IsoAudio, main wraps the render with it.
UsbAudioTap& tap() noexcept;

// The render callback of the image with the tap in it: calls `inner` (the app's), then pushes the block the main output is about to play (`io.out`, left untouched) and drains the return.
// Call once, before audio_device().start(); the returned callback points at static state. The tone images do not wrap.
hal::audio::BlockCallback wrap(hal::audio::BlockCallback inner) noexcept;

}  // namespace o3sdk::platform::mk66::usb_audio
