// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/counters.h>
#include <o3sdk/usb/kernels.h>
#include <o3sdk/usb/rate.h>
#include <o3sdk/usb/ring.h>
#include <o3sdk/usb/usb_types.h>

#include <atomic>
#include <cstdint>

// The isochronous data path, host-testable: the IN stems packetiser (device to host), the OUT return path
// with explicit feedback (host to device), and the two rings between them and the engine. Nothing here
// touches hardware: the USB0 driver (slice S4, 006b) passes in CYCCNT stamps, buffers and fill levels, and
// the engine (OS-005) writes stem blocks and pulls return blocks. Real-time rules: no allocation, no blocking,
// no virtual calls, every loop bounded by a frame or channel count; the per-packet work is a few dozen
// integer operations plus the interleave.
//
//   engine block --StemBlock--> StemRing --StemReader--> interleave --> IN packet buffer --> host
//   host --> OUT packet buffer --> OutStream --> ReturnRing --ReturnReader--> deinterleave --> engine
//
// Context map (one writer per object; see the class comments):
//   audio ISR / engine:  StemRing::write, ReturnReader (pull)
//   USB ISR:             RateEstimator, InStream, OutStream, FeedbackControl, StemReader (inside InStream)
namespace o3sdk::usb {

// ---- packet buffers: alignment is declared, never placed (ADR 0011 lesson 1) ---------------------------------

// IN packet: up to 45 frames of kMaxChannels int16 samples, interleaved. The driver owns one (or two, for
// ping-pong) per BDT entry, in SRAM_U.
struct InPacketBuffer {
  alignas(4) std::int16_t samples[kMaxChannels * 45u];
};
// OUT packet: up to 48 stereo frames, one uint32_t per frame (left in the low half, right in the high half).
inline constexpr std::uint32_t kReturnMaxChunk = 48;
struct OutPacketBuffer {
  alignas(4) std::uint32_t frames[kReturnMaxChunk];
};
// Feedback payload, 3 or 4 bytes (pack_feedback()).
struct FeedbackPacketBuffer {
  alignas(4) std::uint8_t bytes[4];
};
static_assert(alignof(InPacketBuffer) == 4 && sizeof(InPacketBuffer) % 4u == 0u, "IN packet buffer is word aligned");
static_assert(alignof(OutPacketBuffer) == 4 && sizeof(OutPacketBuffer) % 4u == 0u, "OUT packet buffer is word aligned");
static_assert(alignof(FeedbackPacketBuffer) == 4 && sizeof(FeedbackPacketBuffer) == 4u, "feedback buffer is a word");
static_assert(sizeof(InPacketBuffer) >= max_packet_bytes(kMaxChannels, Speed::Full), "IN buffer holds the largest FS packet");

// ---- IN packetiser ------------------------------------------------------------------------------------------

// The ring-fill band the packetiser keeps the stem ring in. The unit is the SMOOTHED fill (frames buffered at
// packet time, averaged over 8 packets): a block arrives every 128 frames, so the instantaneous fill is a
// sawtooth of that amplitude and no threshold on it can both stay clear of the baseline and protect the ring.
// Numbers (ring of 3 usable blocks, start-up fill of 3 blocks, baseline mean 276..364 depending on the phase
// at which the stream started): an underrun needs the sawtooth trough to reach 45 frames, i.e. a mean near
// 110; a block is lost only when the trough reaches 3 blocks, i.e. a mean near 448. `low` and `high` sit
// well inside those and outside the baseline, so the guard is silent in normal running and acts only when
// something has drifted (a frozen or wrong estimator, a host pause).
struct FillBand {
  std::uint16_t low;
  std::uint16_t high;
};
inline constexpr FillBand kStemFillBand{200, 400};

// Chooses the frame count of each IN packet: a phase accumulator fed by the estimated rate (frames per SOF,
// Q16.16) gives 44 or 45 frames (5 or 6 at high speed) in the proportion that matches the device's true
// rate; outside the ring-fill band the choice is forced to the low or high count (smoothed fill below `low`
// -> take fewer, above `high` -> take more). The guard has 0.12 frames per ms of authority downwards and 0.88
// upwards (against 0.022 frames per ms of drift at 500 ppm), so it always wins. The result is ALWAYS frames_lo or frames_hi, whatever the inputs: that
// is the +/-1 slot rule hosts impose on asynchronous IN endpoints. The guard never touches the accumulator,
// so the nominal phase stays correct once the fill is back in band.
class InPacketiser {
 public:
  void reset() noexcept { acc_ = 0; }
  std::uint32_t next(Speed s, std::uint32_t rate_q16, std::uint32_t smoothed_fill, FillBand band) noexcept;

 private:
  std::uint32_t acc_ = 0;  // fractional frame, Q16, always below 65536 between calls
};

// ---- return (OUT) feedback trim -----------------------------------------------------------------------------

inline constexpr std::uint32_t kReturnRingFrames = 512;
inline constexpr std::uint32_t kReturnPrimeFrames = 256;   // fill at which the engine starts pulling
// Mean fill the trim steers to. A pull is 128 frames, so the fill is a sawtooth of that amplitude between
// 128 and 256 frames once the reader has trimmed the ring to exactly kReturnPrimeFrames at its start: a mean
// of 192..214 at the start (it then wanders by a few tens of frames as the pull and SOF phases beat), so no
// start-up trim is needed.
inline constexpr std::uint32_t kReturnTargetFrames = 200;
inline constexpr std::uint32_t kReturnDeadFrames = 64;      // the trim does nothing while the mean is this close

// Adds a small correction to the estimated rate so the return ring's mean fill cannot wander out of its safe
// range: too full -> advertise less (the host sends less), too empty -> more. The fill is averaged over about
// 1 s (the instantaneous fill is the pull sawtooth). Inside +/-kReturnDeadFrames of the target nothing
// happens: the estimator alone sets the value, which is the steadiest thing a host's feedback loop can see
// (its residual error is under 25 ppm, about 1 frame per second of fill drift). Beyond the dead zone the trim
// pushes back at 4 Q10.14 LSB per frame (time constant about 4 s), clamped to +/-128 LSB (about 177 ppm). It is
// the safety net for what the estimator cannot see: a wrong reference-clock constant, a host that pauses.
class FeedbackControl {
 public:
  void reset() noexcept;
  // Per SOF: `rate_q16` from the estimator, `return_fill` frames in the return ring. Returns the rate to
  // advertise, Q16.16. Until the ring has first reached kReturnPrimeFrames (the engine is not pulling yet)
  // the trim is zero.
  std::uint32_t update(std::uint32_t rate_q16, std::uint32_t return_fill) noexcept;
  std::int32_t trim_q16() const noexcept { return trim_q16_; }

  static constexpr std::int32_t kTrimLimitQ16 = 512;  // 128 Q10.14 LSB, in Q16.16 units

 private:
  bool armed_ = false;
  std::int32_t avg_q16_ = 0;  // averaged fill, Q16 frames
  std::int32_t trim_q16_ = 0;
};

// ---- stem ring (engine -> USB) ------------------------------------------------------------------------------

// One engine block: per channel a pointer to kStemBlockFrames int16 samples (mono, one slot of the profile),
// each 4-byte aligned. The USB side only reads through them. LIFETIME CONTRACT (the engine's pool must obey
// it): a block stays valid until StemRing::capacity() + 2 further blocks have been written after it (ring
// content, the block the reader is working on, and the one being produced). With the ring below that is
// 5 blocks per slot; ADR 0012's estimate of 2-3 did not count the reader's block and the one in production.
struct StemBlock {
  const std::int16_t* ch[kMaxChannels];
};
inline constexpr std::uint32_t kStemRingSlots = 4;  // 3 usable (OverwriteRing keeps one chunk of slack)
using StemRing = OverwriteRing<StemBlock, kStemRingSlots, 1>;
inline constexpr std::uint32_t kStemPrimeFrames = 3u * kStemBlockFrames;  // the ring full (3 blocks) before packets carry audio

// USB-side reader of the stem ring: hands out frames across block boundaries.
class StemReader {
 public:
  explicit StemReader(StemRing& ring) noexcept : ring_(ring) {}
  // Forget the held block and everything queued (a stream start).
  void restart() noexcept;
  // Frames available now: queued blocks plus what is left of the held one.
  std::uint32_t fill_frames() const noexcept;
  // Interleaves up to `frames` frames of `channels` channels into `out`; returns the frames delivered
  // (fewer only when the ring ran dry). `out` must be 4-byte aligned.
  std::uint32_t read_interleaved(std::int16_t* out, std::uint32_t frames, std::uint32_t channels) noexcept;
  std::uint32_t dropped_blocks() const noexcept { return ring_.dropped(); }

 private:
  StemRing& ring_;
  StemBlock cur_{};
  std::uint32_t off_ = kStemBlockFrames;  // next frame of cur_; kStemBlockFrames = nothing held
};

// The IN endpoint's packet builder. start(), stop() and build_packet() all run in the USB ISR (one context: it
// is the stem ring's single consumer); the engine side only calls StemRing::write.
class InStream {
 public:
  InStream(StemRing& ring, Counters& counters) noexcept : reader_(ring), counters_(counters) {}

  // The host selected alt 1: discard stale blocks, restart the phase, wait for the start-up fill.
  void start(Speed speed, std::uint32_t channels) noexcept;
  void stop() noexcept { active_ = false; }
  bool active() const noexcept { return active_; }

  // Builds the next packet into `out` (4-byte aligned, room for frames_hi * channels samples) and returns its
  // frame count (frames_lo or frames_hi), so the byte length is frames * channels * 2. While priming (and
  // after an underrun, until the ring has refilled) the packet is silence of the same size, so the host
  // never sees a size outside {lo, hi}. `rate_q16`: RateEstimator::frames_per_sof_q16().
  std::uint32_t build_packet(std::uint32_t rate_q16, std::int16_t* out) noexcept;

  // The smoothed fill the guard works on, frames (0 until primed).
  std::uint32_t smoothed_fill() const noexcept { return primed_ ? avg_q8_ >> 8 : 0u; }

 private:
  StemReader reader_;
  Counters& counters_;
  InPacketiser packetiser_;
  Speed speed_ = Speed::Full;
  std::uint32_t channels_ = 0;
  bool active_ = false;
  bool primed_ = false;
  std::uint32_t avg_q8_ = 0;  // fill averaged over 8 packets, Q8 frames
};

// ---- return ring (USB -> engine) ----------------------------------------------------------------------------

// One stereo frame per word (left low half, right high half = the memory layout of interleaved LE int16).
using ReturnRing = OverwriteRing<std::uint32_t, kReturnRingFrames, kReturnMaxChunk>;

// USB-side writer of the return ring (the ring's single producer).
class OutStream {
 public:
  OutStream(ReturnRing& ring, Counters& counters) noexcept : ring_(ring), counters_(counters) {}
  // One OUT packet of `bytes` bytes (a whole number of 4-byte frames; a partial frame is dropped and
  // counted, a packet longer than kReturnMaxChunk frames is cut and counted). `frames`: 4-byte aligned.
  void on_packet(const std::uint32_t* frames, std::uint32_t bytes) noexcept;
  // Frames in the ring (an upper bound, see OverwriteRing::size_from_producer), for FeedbackControl.
  std::uint32_t fill() const noexcept { return ring_.size_from_producer(); }

 private:
  ReturnRing& ring_;
  Counters& counters_;
};

// Engine-side reader of the return ring (the ring's single consumer): the USB RETURN source.
class ReturnReader {
 public:
  ReturnReader(ReturnRing& ring, Counters& counters) noexcept : ring_(ring), counters_(counters) {}
  // Stream start (SET_INTERFACE alt 1): discard stale frames and wait for kReturnPrimeFrames again. Callable from
  // any context, in practice the USB ISR; the consumer (the engine's pull) carries it out at its next call, so
  // the ring's single-consumer rule holds.
  void request_restart() noexcept { restart_.store(true, std::memory_order_release); }
  // Delivers `frames` (at most kStemBlockFrames) frames as two planes. Silence while the ring is priming
  // and after an underrun (until it has refilled), and for the part of a pull the ring could not supply
  // (counted). The pull that ends priming first discards the oldest surplus above kReturnPrimeFrames (the
  // ring grows in whole pull intervals while nothing reads it), so the stream always starts at the same fill.
  // Returns true when the whole block is real audio.
  bool pull(std::int16_t* left, std::int16_t* right, std::uint32_t frames) noexcept;

 private:
  ReturnRing& ring_;
  Counters& counters_;
  std::atomic<bool> restart_{false};
  bool primed_ = false;
};

}  // namespace o3sdk::usb
