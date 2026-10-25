// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-SCREENSHOT, spec Q4: while a screenshot is written (the screen is frozen for a second or two) the keys, the pads and the encoder are not the app's. A decorator over the key scan's
// hal::Input, between it and the app (console.cpp's TeeInput owns one):
//   held        every pass() reads the source dry, so the scan's queue never fills. A press (a button, a pad) and every encoder turn is dropped, and the key is marked "press never seen".
//               The release of a key that was down before the hold is kept, in the order it came, and handed to the app after the hold, so no note and no hold hangs.
//   after       the kept releases first, then the source; the release of a marked key (its press was dropped, the key was still down when the hold ended) is dropped too, so the app
//               never sees a release without its press.
// At most kPending releases are kept; when that many wait, the hold stops reading and the rest stays queued in the source (the scan keeps its state, nothing is lost). Main loop only.

#include <o3sdk/core/input_event.h>
#include <o3sdk/hal/input.h>

#include <cstdint>

namespace o3sdk::platform::mk66::console {

class InputHold final : public hal::Input {
 public:
  static constexpr unsigned kPending = 16;  // releases kept across a hold: more keys than ten fingers hold

  explicit InputHold(hal::Input& source) noexcept : source_(source) {}

  // One call a main-loop pass: `held` says whether a capture holds the app now. While held it takes the source's events as above; the pass that ends the hold only opens the gate.
  void pass(bool held) noexcept {
    held_ = held;
    core::InputEvent e;
    while (held_ && count_ < kPending && source_.poll(e)) {
      absorb(e);
    }
  }
  bool held() const noexcept { return held_; }
  unsigned pending() const noexcept { return count_; }

  bool poll(core::InputEvent& out) noexcept override {
    if (held_) {
      return false;  // (the app is not run while a capture holds it; if it polled, it would be given nothing)
    }
    if (count_ != 0u) {
      out = kept_[head_];
      head_ = (head_ + 1u) % kPending;
      --count_;
      return true;
    }
    while (source_.poll(out)) {
      if (!unseen_release(out)) {
        return true;
      }
    }
    return false;
  }
  hal::InputStats stats() const noexcept override { return source_.stats(); }

 private:
  static bool is_key(const core::InputEvent& e) noexcept { return (e.kind == core::InputKind::Button || e.kind == core::InputKind::Pad) && e.id < 64u; }
  static std::uint64_t bit(const core::InputEvent& e) noexcept { return std::uint64_t{1} << e.id; }
  std::uint64_t& marks(const core::InputEvent& e) noexcept { return e.kind == core::InputKind::Button ? button_marks_ : pad_marks_; }

  // After the hold: true for the release of a key whose press was dropped (the mark ends with it).
  bool unseen_release(const core::InputEvent& e) noexcept {
    if (!is_key(e) || e.value != 0 || (marks(e) & bit(e)) == 0u) {
      return false;
    }
    marks(e) &= ~bit(e);
    return true;
  }
  // Within the hold (count_ < kPending): the event is dropped, except the release of a key the app saw go down.
  void absorb(const core::InputEvent& e) noexcept {
    const bool key = e.kind == core::InputKind::Button || e.kind == core::InputKind::Pad;
    if (!key) {
      return;  // an encoder turn (or a kind nobody produces): dropped
    }
    if (e.value != 0) {
      if (is_key(e)) {
        marks(e) |= bit(e);  // pressed during the hold: the app never sees this press, nor its release
      }
      return;
    }
    if (unseen_release(e)) {
      return;
    }
    kept_[(head_ + count_) % kPending] = e;
    ++count_;
  }

  hal::Input& source_;
  core::InputEvent kept_[kPending] = {};  // a ring: the releases that wait for the hold to end
  unsigned head_ = 0;
  unsigned count_ = 0;
  std::uint64_t button_marks_ = 0;  // keys whose press was dropped and whose release has not come yet (by button id / pad index)
  std::uint64_t pad_marks_ = 0;
  bool held_ = false;
};

}  // namespace o3sdk::platform::mk66::console
