// SPDX-License-Identifier: GPL-3.0-or-later
// OS-USBDISK U2a, Q11: the MSC load throttle (usb/msc_throttle.h): on above 85 %, off below 80 % (hysteresis), once per 2 ms while on, smoothed, a wrapping clock.
#include <o3sdk/usb/msc_throttle.h>

#include <doctest/doctest.h>

using o3sdk::usb::MscThrottle;

namespace {
void feed_n(MscThrottle& t, unsigned load, int n) {
  for (int i = 0; i < n; ++i) {
    t.feed(load);
  }
}
}  // namespace

TEST_CASE("msc throttle: idle and a light render never hold the pump") {
  MscThrottle t;
  feed_n(t, 40, 100);
  CHECK_FALSE(t.throttled());
  for (std::uint32_t ms = 0; ms < 10; ++ms) {
    CHECK(t.allow(ms));
  }
}

TEST_CASE("msc throttle: above 85 % the pump runs once per 2 ms, and a long block alone does not trip it") {
  MscThrottle t;
  feed_n(t, 60, 50);
  t.feed(190);  // one block over budget
  CHECK_FALSE(t.throttled());
  feed_n(t, 95, 100);
  CHECK(t.throttled());
  std::uint32_t ran = 0;
  for (std::uint32_t ms = 1000; ms < 1100; ++ms) {
    ran += t.allow(ms) ? 1u : 0u;
  }
  CHECK(ran == 50u);  // every other millisecond
  CHECK_FALSE(t.allow(1100 - 1));
}

TEST_CASE("msc throttle: hysteresis between 80 and 85 %") {
  MscThrottle t;
  feed_n(t, 95, 100);
  CHECK(t.throttled());
  feed_n(t, 83, 200);  // in the band: stays on
  CHECK(t.throttled());
  feed_n(t, 70, 200);  // below 80: released
  CHECK_FALSE(t.throttled());
  feed_n(t, 83, 200);  // in the band from below: stays off
  CHECK_FALSE(t.throttled());
}

TEST_CASE("msc throttle: a wrapping millisecond clock") {
  MscThrottle t;
  feed_n(t, 100, 100);
  CHECK(t.throttled());
  CHECK(t.allow(0xFFFFFFFEu));
  CHECK_FALSE(t.allow(0xFFFFFFFFu));
  CHECK(t.allow(0u));  // two ms later across the wrap
  CHECK_FALSE(t.allow(1u));
  CHECK(t.allow(2u));
}
