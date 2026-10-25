// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/counters.h>
#include <o3sdk/usb/rate.h>
#include <o3sdk/usb/stream.h>
#include <o3sdk/usb/usb_types.h>

#include <cstdint>

// The AUDIO mode's isochronous streams as one object (OS-006d, ADR 0012 Decisions 2-4): the SOF rate estimator, the IN packet builder over the stem
// ring, the OUT (return) writer over the return ring, the explicit-feedback loop, and the one hook that tells the engine when a stream starts and stops.
// It owns no hardware and no buffers: usb::UsbDevice (device.h) owns the buffer descriptors and the packet buffers and calls in here, and the platform
// builds one next to the rings. All the maths is stream.h's and rate.h's (host-tested at millions of simulated SOFs); this class is the sequencing.
//
// CONTEXTS (one writer per object, as everywhere in the usb library):
//   USB ISR (priority 64)   every method below except `engine side`: start/stop, on_sof, build_in, on_out_packet, feedback_packet, reset_session.
//   engine (render, 128)    the ring ends: StemRing::write (the StemTap) and ReturnReader::pull. IsoAudio only holds the ReturnReader to ask it, through an
//                           atomic flag, to restart at a stream start (ReturnReader::request_restart), and reads nothing of the engine.
//   main loop               the getters, for `diag usb audio`: plain words, torn at most across fields.
// RING CONTRACTS:
//   stem ring  (StemRing)    engine writes whole 128-frame blocks (never waits, drops the oldest when the USB side lags: counted in in_dropped_blocks);
//                            the ISR is the single consumer (InStream/StemReader), at most one packet (45 frames) per SOF.
//   return ring (ReturnRing) the ISR writes at most 48 frames per OUT packet (never waits, drops the oldest: out_dropped_frames); the engine's pull is
//                            the single consumer, 64 frames per render block.
// No allocation, no blocking, no virtual calls; every loop is bounded by a frame or channel count.
namespace o3sdk::usb {

class IsoAudio {
 public:
  // Told, in the ISR, whenever the set of running streams changes: the IN stream (with its channel count) and the OUT stream. The engine's side
  // enables its stem tap and its return pull from it. Must be short and non-blocking; called with the new state already in place.
  using StreamHook = void (*)(void* context, bool in_active, std::uint32_t in_channels, bool out_active);

  IsoAudio(StemRing& stems, ReturnRing& returns, ReturnReader& reader, Counters& counters) noexcept
      : rate_(Speed::Full), in_(stems, counters), out_(returns, counters), reader_(reader), counters_(counters) {}

  void set_hook(StreamHook hook, void* context) noexcept {
    hook_ = hook;
    hook_context_ = context;
  }

  // ---- session and streams (ISR) --------------------------------------------------------------------------------------------------
  // A new configuration: the rate estimator starts over (the first SOF after it sets the phase) and any running stream is stopped.
  void reset_session(Speed speed) noexcept;
  // SET_INTERFACE alt 1 on the stems interface (start_in) / the return interface (start_out, which also starts the feedback loop); alt 0, a
  // configuration event, a bus reset: stop. Starting an already running stream restarts it (hosts do that to flush).
  void start_in(std::uint32_t channels) noexcept;
  void stop_in() noexcept;
  void start_out() noexcept;
  void stop_out() noexcept;
  bool in_active() const noexcept { return in_.active(); }
  bool out_active() const noexcept { return out_active_; }
  std::uint32_t in_channels() const noexcept { return in_channels_; }

  // ---- per frame (ISR) -------------------------------------------------------------------------------------------------------------
  // One start of frame: `cyccnt` the cycle counter at the interrupt, `frame` the hardware frame number. Updates the rate estimate and, while the OUT
  // stream runs, the feedback value (one FeedbackControl step per SOF: it averages over about 1 s of them).
  void on_sof(std::uint32_t cyccnt, std::uint32_t frame) noexcept;
  // Builds the next IN packet into `out` (4-byte aligned, room for 45 frames x kMaxChannels samples); returns its length in BYTES
  // (frames x channels x 2; the frame count is always frames_lo or frames_hi).
  std::uint32_t build_in(std::int16_t* out) noexcept;
  // An OUT packet of `bytes` bytes at `frames` (4-byte aligned).
  void on_out_packet(const std::uint32_t* frames, std::uint32_t bytes) noexcept { out_.on_packet(frames, bytes); }
  // The feedback payload for the next feedback IN packet (the value computed at the last SOF), packed for the speed; returns the byte count.
  std::uint32_t feedback_packet(std::uint8_t* out) noexcept;

  // ---- diagnostics (any context; plain words) --------------------------------------------------------------------------------------
  const RateEstimator& rate() const noexcept { return rate_; }
  const FeedbackControl& feedback() const noexcept { return fb_; }
  std::uint32_t stem_fill_frames() const noexcept { return in_.smoothed_fill(); }
  std::uint32_t return_fill_frames() const noexcept { return out_.fill(); }
  std::uint32_t feedback_rate_q16() const noexcept { return fb_rate_q16_; }

 private:
  void notify() noexcept {
    if (hook_ != nullptr) {
      hook_(hook_context_, in_.active(), in_channels_, out_active_);
    }
  }

  RateEstimator rate_;
  InStream in_;
  OutStream out_;
  FeedbackControl fb_;
  ReturnReader& reader_;
  Counters& counters_;
  Speed speed_ = Speed::Full;
  std::uint32_t in_channels_ = 0;
  bool out_active_ = false;
  std::uint32_t fb_rate_q16_ = nominal_frames_q16(Speed::Full);  // frames per SOF to advertise, Q16.16
  StreamHook hook_ = nullptr;
  void* hook_context_ = nullptr;
};

}  // namespace o3sdk::usb
