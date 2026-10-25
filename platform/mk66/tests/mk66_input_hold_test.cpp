// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SCREENSHOT, spec Q4 (usb/input_hold.h): while a capture holds the app, the presses of keys and pads and the encoder turns are dropped, and the release of a key that was down before is
// kept and handed over after, so no note and no hold hangs; a key whose press was dropped has its release dropped too (whenever it comes). A scripted source stands for the key scan.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include <o3sdk/hal/input.h>
#include <o3sdk/platform/mk66/usb/input_hold.h>

#include <cstdint>
#include <deque>
#include <vector>

namespace console = o3sdk::platform::mk66::console;
namespace core = o3sdk::core;

namespace {

class Script final : public o3sdk::hal::Input {
 public:
  std::deque<core::InputEvent> queue;
  bool poll(core::InputEvent& out) noexcept override {
    if (queue.empty()) {
      return false;
    }
    out = queue.front();
    queue.pop_front();
    return true;
  }
  o3sdk::hal::InputStats stats() const noexcept override {
    o3sdk::hal::InputStats s;
    s.dropped = 7;
    return s;
  }
  core::Tick t = 100;
  void button(std::uint8_t id, bool down) { queue.push_back(core::make_button_event(t++, id, down)); }
  void pad(std::uint8_t id, bool down) { queue.push_back(core::make_pad_event(t++, id, down ? 127 : 0)); }
  void turn(std::int16_t delta) { queue.push_back(core::make_encoder_event(t++, 0, delta)); }
};

// What the app is given: everything poll() has until it says no.
std::vector<core::InputEvent> drain(console::InputHold& h) {
  std::vector<core::InputEvent> got;
  core::InputEvent e;
  while (h.poll(e)) {
    got.push_back(e);
  }
  return got;
}

}  // namespace

TEST_CASE("input hold: not held, every event passes in order and the counters are the source's") {
  Script src;
  console::InputHold h(src);
  src.button(8, true);
  src.pad(3, true);
  src.turn(-2);
  src.pad(3, false);
  src.button(8, false);
  h.pass(false);
  const auto got = drain(h);
  NEED(got.size() == 5u);
  CHECK(got[0] == core::make_button_event(100, 8, true));
  CHECK(got[1] == core::make_pad_event(101, 3, 127));
  CHECK(got[2] == core::make_encoder_event(102, 0, -2));
  CHECK(got[4] == core::make_button_event(104, 8, false));
  CHECK(h.stats().dropped == 7u);
}

TEST_CASE("input hold: while held the source is read dry, presses and turns are dropped and the app is given nothing") {
  Script src;
  console::InputHold h(src);
  h.pass(true);
  src.button(9, true);
  src.pad(40, true);
  src.turn(3);
  h.pass(true);
  CHECK(src.queue.empty());  // the scan's queue never fills during a long capture
  core::InputEvent e;
  CHECK_FALSE(h.poll(e));
  h.pass(false);
  CHECK(drain(h).empty());  // the presses are not delivered later either
  CHECK(h.pending() == 0u);
}

TEST_CASE("input hold: the release of a key that was down before is kept and delivered after the hold, with its own stamp, before later events") {
  Script src;
  console::InputHold h(src);
  src.pad(12, true);
  src.button(28, true);
  h.pass(false);
  CHECK(drain(h).size() == 2u);  // the app saw both go down
  h.pass(true);
  src.turn(1);
  src.pad(12, false);  // t=103
  src.button(5, true);
  src.button(28, false);  // t=105
  h.pass(true);
  CHECK(h.pending() == 2u);
  h.pass(false);
  src.pad(30, true);  // after the hold
  src.pad(30, false);
  const auto got = drain(h);
  NEED(got.size() == 4u);
  CHECK(got[0] == core::make_pad_event(103, 12, 0));
  CHECK(got[1] == core::make_button_event(105, 28, false));
  CHECK(got[2].kind == core::InputKind::Pad);
  CHECK(got[2].id == 30u);
  CHECK(got[2].value == 127);
  CHECK(got[3].value == 0);
  // button 5 went down during the hold and is still down: its release is not the app's either
  src.button(5, false);
  CHECK(drain(h).empty());
  src.button(5, true);  // a fresh press, now delivered
  src.button(5, false);
  CHECK(drain(h).size() == 2u);
}

TEST_CASE("input hold: a press and release both inside the hold never reach the app, and a key pressed again inside the hold is dropped with its release") {
  Script src;
  console::InputHold h(src);
  src.pad(7, true);
  h.pass(false);
  (void)drain(h);
  h.pass(true);
  src.pad(7, false);  // the release of what the app saw: kept
  src.pad(7, true);   // pressed again: dropped, marked
  src.pad(7, false);  // its release: dropped
  src.button(20, true);
  src.button(20, false);
  h.pass(true);
  CHECK(h.pending() == 1u);
  h.pass(false);
  const auto got = drain(h);
  NEED(got.size() == 1u);
  CHECK(got[0] == core::make_pad_event(101, 7, 0));
}

TEST_CASE("input hold: a button and a pad with the same number are two keys") {
  Script src;
  console::InputHold h(src);
  h.pass(true);
  src.button(5, true);  // marks the button only
  h.pass(true);
  h.pass(false);
  src.pad(5, true);
  src.pad(5, false);
  src.button(5, false);  // the button's unseen release
  const auto got = drain(h);
  NEED(got.size() == 2u);  // the pad's press and release
  CHECK(got[0].kind == core::InputKind::Pad);
  CHECK(got[1].kind == core::InputKind::Pad);
}

TEST_CASE("input hold: more releases than it can keep stay queued in the source and arrive after the hold") {
  Script src;
  console::InputHold h(src);
  constexpr unsigned kKeys = console::InputHold::kPending + 4u;
  for (unsigned i = 0; i < kKeys; ++i) {
    src.pad(static_cast<std::uint8_t>(i), true);
  }
  h.pass(false);
  CHECK(drain(h).size() == kKeys);
  h.pass(true);
  for (unsigned i = 0; i < kKeys; ++i) {
    src.pad(static_cast<std::uint8_t>(i), false);
  }
  h.pass(true);
  CHECK(h.pending() == console::InputHold::kPending);
  CHECK(src.queue.size() == 4u);  // the scan keeps what the hold has no room for
  h.pass(false);
  const auto got = drain(h);
  NEED(got.size() == kKeys);  // every release arrives, in order: no key hangs
  for (unsigned i = 0; i < kKeys; ++i) {
    CHECK(got[i].id == i);
    CHECK(got[i].value == 0);
  }
}

TEST_CASE("input hold: ids beyond the marks (not a key of the unit) are dropped on a press and passed on a release, never out of range") {
  Script src;
  console::InputHold h(src);
  h.pass(true);
  src.button(200, true);
  src.button(200, false);
  h.pass(true);
  h.pass(false);
  const auto got = drain(h);
  NEED(got.size() == 1u);
  CHECK(got[0].id == 200u);
  CHECK(got[0].value == 0);
}
