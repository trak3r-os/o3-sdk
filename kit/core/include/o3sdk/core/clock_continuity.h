// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A check of the ClockFrame stream a MIDI device gives the engine (hal/midi.h, "The follow contract"): feed it each block's frame, in order, and it
// counts the frames that break the contract. It is a pure function of the stream: no heap, nothing blocks, a few integer operations a block, so a
// host test can run it over a scripted clock (FakeMidi does), a unit test of a real device over its DLL's output (MidiService), and a diagnostic
// over the live stream, with the same rules. It lives in core, which every layer and every platform test links.
//
// The rules, for a frame with `follow` set (a frame without it ends the run of checks: the next one starts afresh):
//   jump      a block that continues a run (playing, not started, the block before playing too) must begin where the block before ended:
//             pos == pos' + inc' * frames'. A `locate` (the caller knows a Song Position was acted on in this block) is exempt: it is counted.
//   idle      a block that is not playing has pos 0.
//   inc       a block that is playing has an increment.
//   start     `started` only with `playing`, and with pos 0 (a Start begins a run at the top). A run that begins without it (a Continue at a
//             song position, a master joined while it plays) is a locate: counted, its position is whatever it is.
// ok() is true while none of the four has been seen.

#include <o3sdk/core/midi_types.h>

#include <cstdint>

namespace o3sdk::core {

class ClockContinuity {
 public:
  // The frame `f` for a block of `frames` samples. `locate`: a Song Position was acted on in this block, so its position may jump. Returns
  // true when the frame keeps the rules.
  bool feed(const ClockFrame& f, std::uint32_t frames, bool locate = false) noexcept {
    if (!f.follow) {
      have_ = false;
      return true;
    }
    ++blocks_;
    bool good = true;
    if (f.playing) {
      if (f.inc == 0u) {
        ++no_inc_;
        good = false;
      }
      if (f.started) {
        ++starts_;
        if (f.pos != 0u) {
          ++bad_start_;
          good = false;
        }
      } else if (have_ && prev_playing_) {
        if (locate) {
          ++locates_;
        } else if (f.pos != prev_end_) {
          ++jumps_;
          good = false;
        }
      } else {
        ++locates_;  // a run that did not begin with a Start: from the song position, or from where the master is
      }
    } else {
      if (f.pos != 0u) {
        ++idle_pos_;
        good = false;
      }
      if (f.started) {
        ++bad_start_;
        good = false;
      }
    }
    prev_playing_ = f.playing;
    prev_end_ = f.pos + f.inc * frames;
    have_ = true;
    return good;
  }

  void reset() noexcept { *this = ClockContinuity{}; }

  bool ok() const noexcept { return jumps_ == 0u && idle_pos_ == 0u && no_inc_ == 0u && bad_start_ == 0u; }
  std::uint32_t blocks() const noexcept { return blocks_; }      // frames with `follow` set that were checked
  std::uint32_t jumps() const noexcept { return jumps_; }
  std::uint32_t idle_pos() const noexcept { return idle_pos_; }
  std::uint32_t no_inc() const noexcept { return no_inc_; }
  std::uint32_t bad_starts() const noexcept { return bad_start_; }
  std::uint32_t starts() const noexcept { return starts_; }      // Starts seen (not violations)
  std::uint32_t locates() const noexcept { return locates_; }    // runs that began at a song position, and Song Position jumps (not violations)

 private:
  bool have_ = false;          // a followed block came before this one
  bool prev_playing_ = false;
  std::uint64_t prev_end_ = 0;  // where the block before ends: its pos + inc * frames
  std::uint32_t blocks_ = 0;
  std::uint32_t jumps_ = 0;
  std::uint32_t idle_pos_ = 0;
  std::uint32_t no_inc_ = 0;
  std::uint32_t bad_start_ = 0;
  std::uint32_t starts_ = 0;
  std::uint32_t locates_ = 0;
};

}  // namespace o3sdk::core
