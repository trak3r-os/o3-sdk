// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// When the outputs may open (track3r-os OS-005d, OS-007g, OS-OUTON). Every image boots muted (hal::audio::Device::start() never
// unmutes); main.cpp opens them (audio_device().unmute(): the codec's ramps, then the amplifier key) ONCE, after the audio path has
// "run clean" for a number of blocks, and they then stay open until power-off or a reset: the amplifier key's click is the one at
// power-on. This is that rule as pure objects, so that it is tested on the host.
//
// OutputGate: the last `clean_blocks` boundaries had no underrun and no hardware fault. The main loop offers it every status
// reading it takes (observe()), so the gate knows where the last fault was; with none since the start it is ready from boundary
// `clean_blocks` (400 blocks, about 0.6 s, is the proven figure: kCleanBlocks). Counters are the status snapshot's (audio/status.h):
// free-running and cumulative since the audio started, compared for equality only. A reading whose boundary count went down is a
// restart of the audio path: the window starts over.
//
// OpenRetry: what a failed unmute costs the main loop. A codec write that is not acknowledged (the I2C0 bus is shared with the
// button scanners) must not stop the unit: the loop goes on, the next attempt waits kBackoffMs (the unmute blocks for about 0.2 s,
// not every pass), and the attempts stop after kAutoAttempts failures.

#include <cstdint>

namespace o3sdk::platform::mk66 {

class OutputGate {
 public:
  static constexpr std::uint32_t kCleanBlocks = 400;  // about 0.6 s of blocks (track3r-os kToneSettleBlocks)

  explicit constexpr OutputGate(std::uint32_t clean_blocks = kCleanBlocks) noexcept : clean_(clean_blocks) {}

  // One status reading (boundaries, underruns and hardware faults of audio::StatusSnapshot). A change of either counter moves the start of the clean run to this
  // reading's boundary; a reading can be up to one poll period later than the fault it sees, which only makes the gate later, never earlier.
  void observe(std::uint32_t boundaries, std::uint32_t underruns, std::uint32_t hw_faults) noexcept {
    if (boundaries < last_) {
      since_ = 0;  // the counters started again
      underruns_ = underruns;
      faults_ = hw_faults;
    } else if (underruns != underruns_ || hw_faults != faults_) {
      since_ = boundaries;
      underruns_ = underruns;
      faults_ = hw_faults;
    }
    last_ = boundaries;
  }

  // True when the last `clean_blocks` boundaries up to `boundaries` (a fresh reading's count) had no fault.
  constexpr bool ready(std::uint32_t boundaries) const noexcept { return boundaries >= since_ && boundaries - since_ >= clean_; }

  constexpr std::uint32_t clean_blocks() const noexcept { return clean_; }

 private:
  std::uint32_t clean_;
  std::uint32_t since_ = 0;      // the boundary the clean run started at
  std::uint32_t last_ = 0;       // the last reading's boundaries
  std::uint32_t underruns_ = 0;  // the counters at the last change
  std::uint32_t faults_ = 0;
};

// Whether the main loop may make an attempt now (ready()), and what a failed or a successful one does to the next. `asked` is a
// request that keeps its retries after the automatic attempts have given up (in track3r-os a Play that found the outputs closed);
// an image with no such request passes false.
class OpenRetry {
 public:
  static constexpr std::uint32_t kBackoffMs = 2000;
  static constexpr std::uint8_t kAutoAttempts = 3;

  constexpr bool ready(bool asked, std::uint32_t now_ms) const noexcept {
    if (failures_ == 0u) {
      return true;
    }
    if (!asked && failures_ >= kAutoAttempts) {
      return false;
    }
    return static_cast<std::int32_t>(now_ms - retry_at_) >= 0;
  }
  constexpr void failed(std::uint32_t now_ms) noexcept {
    if (failures_ < 255u) {
      ++failures_;
    }
    retry_at_ = now_ms + kBackoffMs;
  }
  constexpr void succeeded() noexcept { failures_ = 0; }
  constexpr std::uint8_t failures() const noexcept { return failures_; }
  constexpr bool gave_up() const noexcept { return failures_ >= kAutoAttempts; }

 private:
  std::uint8_t failures_ = 0;
  std::uint32_t retry_at_ = 0;
};

}  // namespace o3sdk::platform::mk66
