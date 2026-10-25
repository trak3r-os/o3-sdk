// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/input_event.h>

#include <cstdint>

using namespace o3sdk::core;

static_assert(sizeof(InputEvent) == 8, "two words per event");
static_assert(InputQueue::capacity() == kInputQueueCapacity, "the shared queue alias");
static_assert(make_button_event(7u, 3, true).value == 1, "usable in constant expressions");

TEST_CASE("input_event: constructors fill every field") {
  const InputEvent b = make_button_event(100, 12, true);
  CHECK(b.timestamp == 100u);
  CHECK(b.kind == InputKind::Button);
  CHECK(b.id == 12);
  CHECK(b.value == 1);
  CHECK(make_button_event(0, 0, false).value == 0);

  const InputEvent p = make_pad_event(200, 47, 90);
  CHECK(p.kind == InputKind::Pad);
  CHECK(p.id == 47);
  CHECK(p.value == 90);

  const InputEvent e = make_encoder_event(300, 0, -3);
  CHECK(e.kind == InputKind::Encoder);
  CHECK(e.value == -3);
}

TEST_CASE("input_event: equality compares all fields") {
  const InputEvent a = make_button_event(5, 1, true);
  CHECK(a == make_button_event(5, 1, true));
  CHECK(a != make_button_event(6, 1, true));
  CHECK(a != make_button_event(5, 2, true));
  CHECK(a != make_button_event(5, 1, false));
  CHECK(a != make_pad_event(5, 1, 1));  // same id/value, different kind
}

TEST_CASE("input_event: the shared queue carries events in order") {
  InputQueue queue;
  CHECK(queue.push(make_button_event(1, 10, true)));
  CHECK(queue.push(make_encoder_event(2, 0, 5)));
  CHECK(queue.push(make_pad_event(3, 20, 127)));
  InputEvent out{};
  CHECK(queue.pop(out));
  CHECK(out == make_button_event(1, 10, true));
  CHECK(queue.pop(out));
  CHECK(out == make_encoder_event(2, 0, 5));
  CHECK(queue.pop(out));
  CHECK(out == make_pad_event(3, 20, 127));
  CHECK_FALSE(queue.pop(out));
}

TEST_CASE("input_event: the queue holds exactly kInputQueueCapacity events") {
  InputQueue queue;
  for (std::uint32_t i = 0; i < kInputQueueCapacity; ++i) {
    CHECK(queue.push(make_encoder_event(i, 0, 1)));
  }
  CHECK_FALSE(queue.push(make_encoder_event(999, 0, 1)));
}
