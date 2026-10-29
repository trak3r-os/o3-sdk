// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The record service (docs/M2C-PLAN.md section 6, decision D8): hal::record::Recorder over RecTap (taps.h), and the wrap of the render callback that feeds the tap. Pure: the
// simulator runs this very code over its fake codec, so a take there is the unit's to the sample; only the block it is given differs.
//
//   wrap(inner, engine)   returns a block callback that runs `inner` (the runtime's, which renders the block and fills io.out), and then, ONLY while a take is Waiting,
//                         Listening or Recording, does the recording of that block:
//                           1. the sixteenths of the transport that begin inside the block go to RecTap::on_step with the frame they fall on (the walk is dsp::for_each_step over
//                              the block's own transport, Engine::block_frame(), the grid a straight sequencer hook uses), and a transport that stopped goes to on_stopped;
//                           2. the tapped block (io.in or io.out, interleaved) is split into two planes and pushed.
//                         Idle or Done, the whole cost is one acquire load and a branch, and io, the engine and the callback's result are not touched (the golden of every app is
//                         unchanged); active_blocks() counts the blocks that did more, and a test holds it at 0 for an idle recorder.
//   Steps, Bar and Pattern. RecTap's own rules read one index: a Bar is "index a multiple of 16", a Pattern "index 0", and which rule is asked is the take's start alignment while it is
//   Waiting and the pending stop_at() boundary once it records. That cannot say "a bar that is not a pattern start" (or the other way round) with one number, so the service does not
//   hand RecTap the transport's index: it hands it 0 for a sixteenth that IS a boundary of the rule being asked (every sixteenth for Step, a multiple of 16 for Bar, a multiple of the
//   app's pattern length for Pattern: set_pattern_steps) and 1 for one that is not. Every sixteenth is still reported (auto_steps counts them). The sixteenths count from the transport's
//   position 0 (a Play from stop); a continued transport counts from its position.
//   The Input tap and the line's channel map. The runtime tells the service AudioIn's map every pass (set_line_map); an Input take applies it to the codec's block (the mean of the two
//   channels, or the left or the right one, in both planes) before it is written, as the engine's own input is made.
//
// Contexts. The main loop calls the Recorder interface; the render calls the wrapped block. The handshake is RecTap's (taps.h, "The handshake"): one side writes each transition.
// The tap kind (Input or Master) is latched by arm(), so a take records one bus from its first frame to its last.

#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/record.h>
#include <o3sdk/platform/mk66/audio/taps.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::dsp {
class Engine;
}

namespace o3sdk::platform::mk66::audio {

class RecordService final : public hal::record::Recorder {
 public:
  static constexpr std::uint32_t kDefaultPatternSteps = 16;
  static constexpr std::uint32_t kMaxPatternSteps = 255;

  RecordService() noexcept = default;

  // ---- hal::record::Recorder (main loop) ------------------------------------------------------------------------------------------------
  bool arm(const hal::record::Take& take) noexcept override;
  bool stop_at(hal::record::Align boundary) noexcept override {
    if (boundary == hal::record::Align::Level) {
      return false;  // a level starts a take, it ends none
    }
    stop_rule_.store(static_cast<std::uint8_t>(boundary), std::memory_order_relaxed);  // (Free is disarm(): RecTap ends the take at the next block and the rule is moot)
    tap_.stop_at(rec_align(boundary));
    return true;
  }
  void disarm() noexcept override { tap_.disarm(); }
  hal::record::State state() const noexcept override;
  std::uint32_t frames() const noexcept override { return tap_.frames(); }
  std::uint32_t overrun() const noexcept override { return tap_.overrun_frames(); }
  std::uint32_t settle() noexcept override { return tap_.settle_ring(); }
  void release() noexcept override { tap_.release(); }
  void set_tap(hal::record::Tap tap) noexcept override {
    if (tap_.state() == RecTap::State::Idle) {
      tap_sel_.store(static_cast<std::uint8_t>(tap), std::memory_order_relaxed);
    }
  }
  void set_line_map(hal::record::LineMap map) noexcept override { line_map_.store(static_cast<std::uint8_t>(map), std::memory_order_relaxed); }
  hal::record::Tap tap() const noexcept override { return static_cast<hal::record::Tap>(tap_sel_.load(std::memory_order_relaxed)); }
  void set_pattern_steps(std::uint32_t steps) noexcept override {
    pattern_steps_.store(steps < 1u ? 1u : (steps > kMaxPatternSteps ? kMaxPatternSteps : steps), std::memory_order_relaxed);
  }
  std::uint32_t pattern_steps() const noexcept override { return pattern_steps_.load(std::memory_order_relaxed); }

  // ---- The render ---------------------------------------------------------------------------------------------------------------------------
  // The callback to give the audio device in place of `inner`. One service wraps one callback; `engine` must outlive it (its block_frame() is read after `inner` ran).
  hal::audio::BlockCallback wrap(hal::audio::BlockCallback inner, const dsp::Engine& engine) noexcept;
  // Blocks in which the service did more than look at the state (a take was Waiting, Listening or Recording).
  std::uint32_t active_blocks() const noexcept { return active_.load(std::memory_order_relaxed); }
  // For a test and `diag`: the tap's own view.
  const RecTap& rec_tap() const noexcept { return tap_; }

  // The index handed to RecTap for the sixteenth `k` of the transport when `rule` (Step, Bar or Pattern) is the alignment being asked: 0 at a boundary of the rule, 1 elsewhere (see the
  // header).
  static constexpr std::uint32_t tap_step(std::uint64_t k, std::uint32_t pattern_steps, hal::record::Align rule) noexcept {
    return rule == hal::record::Align::Bar ? (k % 16u == 0u ? 0u : 1u) : (rule == hal::record::Align::Pattern ? (k % pattern_steps == 0u ? 0u : 1u) : 0u);
  }

 private:
  static RecTap::Align rec_align(hal::record::Align a) noexcept { return static_cast<RecTap::Align>(a); }  // the enumerators are the same, in the same order
  static void block(void* user, const hal::audio::BlockIo& io) noexcept;
  void record_block(const hal::audio::BlockIo& io, RecTap::State state) noexcept;

  RecTap tap_;
  std::atomic<std::uint8_t> tap_sel_{0};
  std::atomic<std::uint32_t> pattern_steps_{kDefaultPatternSteps};
  std::atomic<std::uint8_t> line_map_{0};   // hal::record::LineMap: the main loop writes, the render reads
  std::atomic<std::uint8_t> stop_rule_{0};  // the boundary stop_at() asked for (Free: none): main loop writes, the render reads
  std::atomic<std::uint32_t> active_{0};  // render writes
  // Written by arm() before the state that publishes the take; read by the render after it saw that state.
  hal::record::Tap armed_tap_ = hal::record::Tap::Input;
  hal::record::Align armed_align_ = hal::record::Align::Free;
  hal::audio::BlockCallback inner_{nullptr, nullptr};
  const dsp::Engine* engine_ = nullptr;
};

// The unit's one service (the image's main wires it: Platform::recorder = &service(), render = wrap(rt.block_callback(), engine)). The simulator makes its own.
namespace record_service {
RecordService& service() noexcept;
inline hal::audio::BlockCallback wrap(hal::audio::BlockCallback inner, const dsp::Engine& engine) noexcept { return service().wrap(inner, engine); }
}  // namespace record_service

}  // namespace o3sdk::platform::mk66::audio
