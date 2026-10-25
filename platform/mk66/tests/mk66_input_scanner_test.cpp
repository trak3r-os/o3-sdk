// SPDX-License-Identifier: GPL-3.0-or-later
//
// The TCA8418 protocol (tca8418.h) and the input scan (scanner.h) against modelled chips: the configuration order, the order of
// a drain, events with their timestamps, the debounce seen through the scan, the safety poll when INT is not wired, the ring-full
// policy and its convergence, FIFO overflow, a chip that stops answering (backoff, bus recovery, resync), a chip that loses its
// power without a bus error, the encoder through the queue, and a randomised run that holds the invariants (timestamps never go
// back, the consumer's picture converges to the keys' real state). Runs on the host.
#include <doctest/doctest.h>

#include "mk66_input_fakes.h"

#include <cstdint>

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: MUST checks and leaves the test case on failure.
#define MUST(cond)               \
  do {                           \
    const bool must_ok_ = (cond); \
    CHECK(must_ok_);             \
    if (!must_ok_) {             \
      return;                    \
    }                            \
  } while (0)

namespace input = o3sdk::platform::mk66::input;
namespace fake = o3sdk::platform::mk66::input::fake;
namespace core = o3sdk::core;
namespace tca = o3sdk::platform::mk66::input::tca8418;

namespace {

constexpr std::uint8_t kAddress = 0x34;

struct Rig {
  fake::ManualClock clock;
  fake::Bus pad_bus;
  fake::Bus control_bus;
  fake::KeyChip pad_chip;
  fake::KeyChip button_chip;
  fake::Hardware hw{pad_chip, button_chip};
  input::Scanner scanner{pad_bus, control_bus, hw, clock};

  Rig() {
    pad_bus.attach(kAddress, pad_chip);
    control_bus.attach(kAddress, button_chip);
  }
  void pass(std::uint32_t us = 1000) {
    clock.advance_us(us);
    scanner.service();
  }
  void passes(unsigned n, std::uint32_t us = 1000) {
    for (unsigned i = 0; i < n; ++i) {
      pass(us);
    }
  }
  // Everything the scanner has queued, oldest first.
  unsigned pop(core::InputEvent* out, unsigned max) {
    unsigned n = 0;
    while (n < max && scanner.poll(out[n])) {
      ++n;
    }
    return n;
  }
};

}  // namespace

// ---- the chip protocol ----------------------------------------------------------------------------------------------------

TEST_CASE("tca8418: the configuration is in its order") {
  fake::Bus bus;
  fake::KeyChip chip;
  bus.attach(kAddress, chip);
  input::Tca8418 driver(bus, kAddress);
  MUST(driver.configure() == input::I2cResult::Ok);
  MUST(bus.log_count == 4);
  // keypad rows, keypad columns (low, high), CFG; each a one-register write. Clearing the flags is left to the
  // first drain (an overflow flag must survive a reconfiguration).
  const std::uint8_t expect[4][2] = {{0x1D, 0xFF}, {0x1E, 0xFF}, {0x1F, 0x03}, {0x01, 0xB9}};
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(bus.log[i].address == kAddress);
    CHECK(bus.log[i].tx_len == 2);
    CHECK(bus.log[i].rx_len == 0);
    CHECK(bus.log[i].reg == expect[i][0]);
    CHECK(bus.log[i].value == expect[i][1]);
  }
  CHECK(chip.configured());
}

TEST_CASE("tca8418: a configuration stops at the first failed transfer") {
  fake::Bus bus;
  fake::KeyChip chip;
  bus.attach(kAddress, chip);
  bus.fail_next = 1;
  bus.fail_with = input::I2cResult::Timeout;
  input::Tca8418 driver(bus, kAddress);
  CHECK(driver.configure() == input::I2cResult::Timeout);
  CHECK(bus.transfers == 1u);
  CHECK_FALSE(chip.configured());
}

TEST_CASE("tca8418: a drain clears the flags BEFORE it reads the FIFO, and decodes presses, releases and junk") {
  fake::Bus bus;
  fake::KeyChip chip;
  bus.attach(kAddress, chip);
  input::Tca8418 driver(bus, kAddress);
  MUST(driver.configure() == input::I2cResult::Ok);
  bus.log_count = 0;
  chip.press(0);
  chip.press(79);
  chip.release(0);
  chip.push(97 | 0x80);  // a GPIO event: no key of the matrix
  input::RawKey keys[8];
  input::Tca8418::Drain d;
  MUST(driver.drain(keys, 8, d) == input::I2cResult::Ok);
  MUST(d.stored == 3);
  CHECK(d.ignored == 1);
  CHECK_FALSE(d.overflow);
  CHECK_FALSE(d.more);
  CHECK((keys[0].key == 0 && keys[0].down));
  CHECK((keys[1].key == 79 && keys[1].down));
  CHECK((keys[2].key == 0 && !keys[2].down));
  // INT_STAT read, INT_STAT write (the flag that was seen), the counter, then one read of KEY_EVENT_A each
  MUST(bus.log_count == 3u + 4u);
  CHECK((bus.log[0].reg == 0x02 && bus.log[0].rx_len == 1));
  CHECK((bus.log[1].reg == 0x02 && bus.log[1].tx_len == 2 && bus.log[1].value == 0x01));
  CHECK((bus.log[2].reg == 0x03 && bus.log[2].rx_len == 1));
  for (unsigned i = 3; i < 7; ++i) {
    CHECK((bus.log[i].reg == 0x04 && bus.log[i].rx_len == 1));
  }
  CHECK_FALSE(chip.int_asserted());
}

TEST_CASE("tca8418: more than the limit leaves the rest and says so, an empty chip costs two reads") {
  fake::Bus bus;
  fake::KeyChip chip;
  bus.attach(kAddress, chip);
  input::Tca8418 driver(bus, kAddress);
  MUST(driver.configure() == input::I2cResult::Ok);
  for (unsigned k = 0; k < 6; ++k) {
    chip.press(k);
  }
  input::RawKey keys[4];
  input::Tca8418::Drain d;
  MUST(driver.drain(keys, 4, d) == input::I2cResult::Ok);
  CHECK(d.stored == 4);
  CHECK(d.more);
  CHECK(chip.fifo_count() == 2u);
  MUST(driver.drain(keys, 4, d) == input::I2cResult::Ok);
  CHECK(d.stored == 2);
  CHECK_FALSE(d.more);
  bus.log_count = 0;
  MUST(driver.drain(keys, 4, d) == input::I2cResult::Ok);
  CHECK(d.stored == 0);
  CHECK(bus.log_count == 2u);  // INT_STAT is 0 (nothing to clear), then the counter
}

TEST_CASE("tca8418: an overflow of the FIFO is reported once, with the newest events the chip kept (the oldest is pushed out)") {
  fake::Bus bus;
  fake::KeyChip chip;
  bus.attach(kAddress, chip);
  input::Tca8418 driver(bus, kAddress);
  MUST(driver.configure() == input::I2cResult::Ok);
  for (unsigned k = 0; k < 11; ++k) {
    chip.press(k);  // the eleventh does not fit: with OVR_FLOW_M set (the CFG value) it pushes the OLDEST event out
  }
  CHECK(chip.lost_events == 1u);
  input::RawKey keys[10];
  input::Tca8418::Drain d;
  MUST(driver.drain(keys, 10, d) == input::I2cResult::Ok);
  CHECK(d.overflow);
  CHECK(d.stored == 10);
  CHECK(keys[0].key == 1);   // key 0's press, the oldest, is the one that was lost
  CHECK(keys[9].key == 10);  // and the newest is there
  MUST(driver.drain(keys, 10, d) == input::I2cResult::Ok);
  CHECK_FALSE(d.overflow);
}

TEST_CASE("tca8418: a transfer that fails in the middle keeps what was read before it") {
  fake::Bus bus;
  fake::KeyChip chip;
  bus.attach(kAddress, chip);
  input::Tca8418 driver(bus, kAddress);
  MUST(driver.configure() == input::I2cResult::Ok);
  chip.press(1);
  chip.press(2);
  bus.fail_next = 0;
  // INT_STAT read, clear and counter pass; the first event read fails
  struct FailAt final : fake::Device {
    fake::KeyChip* inner;
    unsigned count = 0;
    input::I2cResult handle(const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
      return ++count == 5 ? input::I2cResult::Nak : inner->handle(tx, tx_len, rx, rx_len);
    }
  } shim;
  shim.inner = &chip;
  fake::Bus bus2;
  bus2.attach(kAddress, shim);
  input::Tca8418 driver2(bus2, kAddress);
  input::RawKey keys[4];
  input::Tca8418::Drain d;
  CHECK(driver2.drain(keys, 4, d) == input::I2cResult::Nak);
  CHECK(d.stored == 1);  // the first event was read, the second failed
  CHECK(keys[0].key == 1);
}

// ---- the scan -----------------------------------------------------------------------------------------------------------------

TEST_CASE("scanner: begin configures both chips and reports them online") {
  Rig r;
  r.scanner.begin();
  CHECK(r.pad_chip.configured());
  CHECK(r.button_chip.configured());
  const o3sdk::hal::InputStats s = r.scanner.stats();
  CHECK(s.online == (o3sdk::hal::InputStats::kPadsOnline | o3sdk::hal::InputStats::kButtonsOnline));
  CHECK(s.errors == 0u);
}

TEST_CASE("scanner: a pad press and release are events with the pad index, full velocity and the time of the pass") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.pad_chip.press(input::key_of_pad(5));
  r.pass();
  const core::Tick pressed_at = r.clock.now();
  core::InputEvent e[4];
  MUST(r.pop(e, 4) == 1);
  CHECK(e[0].kind == core::InputKind::Pad);
  CHECK(e[0].id == 5);
  CHECK(e[0].value == 127);
  CHECK(e[0].timestamp == pressed_at);
  CHECK(r.scanner.pad_held(5));
  r.passes(50);
  r.pad_chip.release(input::key_of_pad(5));
  r.pass();
  MUST(r.pop(e, 4) == 1);
  CHECK(e[0].id == 5);
  CHECK(e[0].value == 0);
  CHECK(e[0].timestamp == r.clock.now());
  CHECK_FALSE(r.scanner.pad_held(5));
}

TEST_CASE("scanner: a button press and release carry the button code") {
  Rig r;
  r.scanner.begin();
  r.pass();
  const unsigned shift = 29;  // Shift in the unit's button numbering and in app::Button
  r.button_chip.press(input::key_of_button(shift));
  r.pass();
  core::InputEvent e[4];
  MUST(r.pop(e, 4) == 1);
  CHECK(e[0].kind == core::InputKind::Button);
  CHECK(e[0].id == shift);
  CHECK(e[0].value == 1);
  CHECK(r.scanner.button_held(shift));
  r.passes(30);
  r.button_chip.release(input::key_of_button(shift));
  r.pass();
  MUST(r.pop(e, 4) == 1);
  CHECK(e[0].value == 0);
}

TEST_CASE("scanner: a bounce is one press, and a quick tap is delayed by the lockout, not lost") {
  Rig r;
  r.scanner.begin();
  r.pass();
  const unsigned pad = 20;
  r.pad_chip.press(input::key_of_pad(pad));
  r.pass();
  r.pad_chip.release(input::key_of_pad(pad));
  r.pass();
  r.pad_chip.press(input::key_of_pad(pad));
  r.pass();
  core::InputEvent e[8];
  MUST(r.pop(e, 8) == 1);  // one press, the bounce is invisible
  CHECK(e[0].value == 127);
  CHECK(r.scanner.stats().filtered >= 1u);
  // a tap that ends 2 ms after it began: the release waits for the lockout
  r.pad_chip.release(input::key_of_pad(pad));
  r.pass();
  CHECK(r.pop(e, 8) == 0);
  r.passes(10);
  MUST(r.pop(e, 8) == 1);
  CHECK(e[0].value == 0);
}

TEST_CASE("scanner: Shift pressed before F8 in one drain reaches the queue first, though F8 is the lower button") {
  Rig r;
  r.scanner.begin();
  r.pass();
  const unsigned shift = 29;
  const unsigned f8 = 7;
  r.button_chip.press(input::key_of_button(shift));  // both are in the FIFO when the scan reads it
  r.button_chip.press(input::key_of_button(f8));
  r.pass();
  core::InputEvent e[4];
  MUST(r.pop(e, 4) == 2);
  CHECK(e[0].id == shift);
  CHECK(e[1].id == f8);
  CHECK(core::tick_reached(e[1].timestamp, e[0].timestamp));
}

TEST_CASE("scanner: a modifier and a pad reported in the same pass: the control buttons come first") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.pad_chip.press(input::key_of_pad(3));
  r.button_chip.press(input::key_of_button(29));  // Shift
  r.pass();
  core::InputEvent e[4];
  MUST(r.pop(e, 4) == 2);
  CHECK(e[0].kind == core::InputKind::Button);  // there is no telling which came first: the reading that keeps Shift + pad
  CHECK(e[1].kind == core::InputKind::Pad);
}

TEST_CASE("scanner: an INT line that is not wired costs the safety poll's latency, not the key") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.hw.int_dead = true;
  r.pad_chip.press(input::key_of_pad(1));
  unsigned passes = 0;
  core::InputEvent e;
  while (!r.scanner.poll(e) && passes < 30) {
    r.pass();
    ++passes;
  }
  CHECK(passes < 13);  // within the 10 ms safety poll and a pass or two
  CHECK(passes > 1);   // and it did take a poll: INT was blind
  CHECK(e.id == 1);
}

TEST_CASE("scanner: unwired matrix positions and out-of-range codes are counted, never events") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.pad_chip.push(0x80 | (40 + 1));  // row 4, column 0: no pad
  r.pad_chip.push(0x80 | 99);        // a code beyond the matrix
  r.pass();
  core::InputEvent e;
  CHECK_FALSE(r.scanner.poll(e));
  CHECK(r.scanner.stats().filtered >= 1u);
}

TEST_CASE("scanner: keys held at power-up are in the FIFO and are reported") {
  Rig r;
  r.pad_chip.press(0);
  r.button_chip.press(input::key_of_button(0));  // F1
  r.button_chip.press(input::key_of_button(7));  // F8
  r.button_chip.press(input::key_of_button(29)); // Shift
  r.scanner.begin();
  r.passes(5);
  CHECK(r.scanner.button_held(0));
  CHECK(r.scanner.button_held(7));
  CHECK(r.scanner.button_held(29));
  CHECK_FALSE(r.scanner.button_held(1));
  CHECK(r.scanner.pad_held(0));
}

TEST_CASE("scanner: after a FIFO overflow the keys the chip named keep their state and every other key believed down is released") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.pad_chip.press(input::key_of_pad(40));  // held before the burst, never named again
  r.passes(3);
  core::InputEvent e[40];
  MUST(r.pop(e, 40) == 1);
  for (unsigned p = 0; p < 11; ++p) {
    r.pad_chip.press(input::key_of_pad(p));  // eleven presses into ten places: pad 0's, the oldest, is pushed out
  }
  CHECK(r.pad_chip.lost_events == 1u);
  r.passes(30);
  const unsigned n = r.pop(e, 40);
  unsigned presses = 0;
  unsigned releases = 0;
  bool pad40_released = false;
  for (unsigned i = 0; i < n; ++i) {
    (e[i].value != 0 ? presses : releases) += 1u;
    pad40_released = pad40_released || (e[i].id == 40 && e[i].value == 0);
  }
  CHECK(presses == 10);  // pads 1..10: the ten the chip kept (pad 0's press is gone)
  CHECK(pad40_released);  // the key the chip never mentioned may have lost its release: let go
  CHECK(releases == 1);   // and nothing else: the ten keys the chip named are down, as its latest events say
  CHECK(r.scanner.stats().resyncs == 1u);
  CHECK_FALSE(r.scanner.pad_held(40));
  for (unsigned p = 1; p <= 10; ++p) {
    CHECK_MESSAGE(r.scanner.pad_held(p), "pad " << p);
  }
  CHECK_FALSE(r.scanner.pad_held(0));
}

TEST_CASE("scanner: an overflow flag cleared by the drain survives a failure of the next transfer") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.button_chip.press(input::key_of_button(29));  // Shift, held
  r.passes(3);
  core::InputEvent e[40];
  MUST(r.pop(e, 40) == 1);
  for (unsigned k = 0; k < 11; ++k) {
    r.button_chip.press(input::key_of_button(10 + k));  // eleven presses: the FIFO overflows
  }
  // The drain reads INT_STAT (overflow set), clears it, and the next transfer, the counter read, fails.
  r.control_bus.fail_skip = 2;
  r.control_bus.fail_next = 1;
  r.control_bus.fail_with = input::I2cResult::Timeout;
  r.passes(60);
  CHECK(r.scanner.stats().errors == 1u);
  CHECK(r.scanner.stats().resyncs == 1u);  // the overflow was not forgotten with the failure
  const unsigned n = r.pop(e, 40);
  bool shift_released = false;
  for (unsigned i = 0; i < n; ++i) {
    shift_released = shift_released || (e[i].id == 29 && e[i].value == 0);
  }
  CHECK(shift_released);  // Shift, never named again, lost its release in the overflow: it is let go, not left down
  CHECK_FALSE(r.scanner.button_held(29));
}

TEST_CASE("scanner: an overflow flag the failed INT_STAT read never saw survives the reconfiguration") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.button_chip.press(input::key_of_button(29));
  r.passes(3);
  core::InputEvent e[40];
  MUST(r.pop(e, 40) == 1);
  for (unsigned k = 0; k < 11; ++k) {
    r.button_chip.press(input::key_of_button(10 + k));
  }
  r.control_bus.fail_next = 1;  // the first transfer of the drain, the INT_STAT read, fails: the flag is still in the chip
  r.control_bus.fail_with = input::I2cResult::Nak;
  r.passes(60);
  CHECK(r.button_chip.configured());
  CHECK(r.scanner.stats().resyncs == 1u);  // the reconfiguration did not clear it: the next drain saw it
  const unsigned n = r.pop(e, 40);
  bool shift_released = false;
  for (unsigned i = 0; i < n; ++i) {
    shift_released = shift_released || (e[i].id == 29 && e[i].value == 0);
  }
  CHECK(shift_released);
}

TEST_CASE("scanner: an event read that failed may have popped its event: the keys the chip did not name are let go") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.button_chip.press(input::key_of_button(29));  // Shift held
  r.passes(3);
  core::InputEvent e[8];
  MUST(r.pop(e, 8) == 1);
  r.button_chip.press(input::key_of_button(3));
  r.button_chip.release(input::key_of_button(3));
  // INT_STAT read, its clear, the counter: then the first event read fails (the chip may have popped it)
  r.control_bus.fail_skip = 3;
  r.control_bus.fail_next = 1;
  r.control_bus.fail_with = input::I2cResult::Timeout;
  r.passes(60);
  CHECK(r.scanner.stats().resyncs == 1u);
  CHECK_FALSE(r.scanner.button_held(29));
}

// ---- the ring ---------------------------------------------------------------------------------------------------------------

TEST_CASE("scanner: a full ring drops the newest and counts, and the consumer converges to the real state") {
  Rig r;
  r.scanner.begin();
  r.pass();
  // 48 pads and 33 buttons pressed, in batches the chips' FIFOs hold, without the consumer polling: 81 events for a ring of 64
  for (unsigned base = 0; base < 48; base += 8) {
    for (unsigned p = base; p < base + 8; ++p) {
      r.pad_chip.press(input::key_of_pad(p));
    }
    r.passes(4);
  }
  for (unsigned base = 0; base < 33; base += 8) {
    for (unsigned b = base; b < base + 8 && b < 33; ++b) {
      r.button_chip.press(input::key_of_button(b));
    }
    r.passes(4);
  }
  CHECK(r.scanner.stats().dropped > 0u);
  // the consumer wakes up: it takes what the ring holds, and the rest follows on the next passes
  std::uint64_t pads = 0;
  std::uint64_t buttons = 0;
  unsigned total = 0;
  for (unsigned round = 0; round < 6; ++round) {
    core::InputEvent e;
    while (r.scanner.poll(e)) {
      ++total;
      MUST(e.value == (e.kind == core::InputKind::Pad ? 127 : 1));
      (e.kind == core::InputKind::Pad ? pads : buttons) |= std::uint64_t{1} << e.id;
    }
    r.passes(2);
  }
  CHECK(total == 81);  // every press arrived exactly once, late but whole
  CHECK(pads == ((std::uint64_t{1} << 48) - 1u));
  CHECK(buttons == ((std::uint64_t{1} << 33) - 1u));
}

TEST_CASE("scanner: a press and release that both found the ring full are never seen, and nothing is left stuck") {
  Rig r;
  r.scanner.begin();
  r.pass();
  for (unsigned i = 0; i < 40; ++i) {  // fill the ring with encoder events, 64 of them
    r.hw.count = static_cast<std::uint16_t>(r.hw.count + 4u);
    r.pass();
  }
  for (unsigned i = 0; i < 30; ++i) {
    r.hw.count = static_cast<std::uint16_t>(r.hw.count + 4u);
    r.pass();
  }
  CHECK(r.scanner.stats().dropped > 0u);
  r.pad_chip.press(input::key_of_pad(9));
  r.passes(3);
  r.pad_chip.release(input::key_of_pad(9));
  r.passes(10);
  core::InputEvent e;
  int turns = 0;
  bool pad_seen = false;
  while (r.scanner.poll(e)) {
    if (e.kind == core::InputKind::Encoder) {
      turns += e.value;
    } else {
      pad_seen = true;
    }
  }
  r.passes(3);
  while (r.scanner.poll(e)) {
    if (e.kind == core::InputKind::Encoder) {
      turns += e.value;
    } else {
      pad_seen = true;
    }
  }
  CHECK_FALSE(pad_seen);  // the press and the release both met a full ring and cancelled out
  CHECK(turns == 70);     // the turns were kept and offered again: none were lost
  CHECK_FALSE(r.scanner.pad_held(9));
}

// ---- the encoder through the queue ---------------------------------------------------------------------------------------------

TEST_CASE("scanner: the encoder reports whole detents with their sign, and keeps the remainder") {
  Rig r;
  r.hw.count = 100;
  r.scanner.begin();
  r.pass();
  core::InputEvent e;
  CHECK_FALSE(r.scanner.poll(e));  // the first count is the origin
  r.hw.count = 103;
  r.pass();
  CHECK_FALSE(r.scanner.poll(e));
  r.hw.count = 104;
  r.pass();
  MUST(r.scanner.poll(e));
  CHECK(e.kind == core::InputKind::Encoder);
  CHECK(e.id == 0);
  CHECK(e.value == 1);
  r.hw.count = static_cast<std::uint16_t>(104 - 12);  // three detents the other way in one pass
  r.pass();
  MUST(r.scanner.poll(e));
  CHECK(e.value == -3);
  r.hw.count = static_cast<std::uint16_t>(r.hw.count + 4000u);  // a fast spin: a thousand detents in one pass
  r.pass();
  MUST(r.scanner.poll(e));
  CHECK(e.value == 1000);
}

// ---- chips that stop answering ------------------------------------------------------------------------------------------------

TEST_CASE("scanner: a chip that does not answer at boot is retried with a backoff and the other one is not held up") {
  Rig r;
  r.pad_bus.dead = true;
  r.scanner.begin();
  const o3sdk::hal::InputStats s0 = r.scanner.stats();
  CHECK(s0.online == o3sdk::hal::InputStats::kButtonsOnline);
  CHECK(s0.errors == 3u);                // three configuration attempts
  CHECK(r.pad_bus.recovers >= 3u);       // the bus recovered between them
  // the control chip works meanwhile
  r.pass();
  r.button_chip.press(input::key_of_button(3));
  r.pass();
  core::InputEvent e;
  MUST(r.scanner.poll(e));
  CHECK(e.id == 3);
  // the dead bus is tried at most every backoff, not every pass: one second of passes is a handful of transfers
  const unsigned before = r.pad_bus.transfers;
  r.passes(1000);
  CHECK(r.pad_bus.transfers - before <= 12u);
  CHECK(r.pad_bus.transfers - before >= 3u);
  // it comes back: configured within the longest backoff
  r.pad_bus.dead = false;
  r.passes(400);
  CHECK(r.pad_chip.configured());
  CHECK(r.scanner.stats().online == (o3sdk::hal::InputStats::kPadsOnline | o3sdk::hal::InputStats::kButtonsOnline));
}

TEST_CASE("scanner: a chip lost for a while comes back with the keys that were held released") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.pad_chip.press(input::key_of_pad(3));
  r.passes(3);
  core::InputEvent e;
  MUST(r.scanner.poll(e));
  CHECK(e.value == 127);
  r.pad_bus.dead = true;  // the cable is out
  r.passes(100);
  CHECK(r.scanner.pad_held(3));  // nothing is known: nothing changes
  r.pad_bus.dead = false;
  r.pad_chip.power_cycle();      // and the chip lost its power meanwhile
  r.passes(500);
  CHECK(r.pad_chip.configured());
  MUST(r.scanner.poll(e));
  CHECK(e.id == 3);
  CHECK(e.value == 0);  // released: its real state is unknown
  CHECK(r.scanner.stats().resyncs >= 1u);
}

TEST_CASE("scanner: one failed transfer is a retry after the minimum backoff, with no bus recovery and no resync") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.pad_chip.press(input::key_of_pad(7));
  r.passes(3);
  core::InputEvent e;
  MUST(r.scanner.poll(e));
  r.pad_bus.fail_next = 1;
  r.pad_bus.fail_with = input::I2cResult::Nak;
  r.pad_chip.press(input::key_of_pad(8));
  r.passes(40);
  CHECK(r.scanner.stats().errors == 1u);
  CHECK(r.pad_bus.recovers == 0u);
  CHECK(r.scanner.stats().resyncs == 0u);
  CHECK(r.scanner.pad_held(7));  // still held: a single hiccup forgets nothing
  MUST(r.scanner.poll(e));
  CHECK(e.id == 8);
}

TEST_CASE("scanner: a chip that lost its configuration without a bus error is found and set right, its keys released") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.button_chip.press(input::key_of_button(29));
  r.passes(3);
  core::InputEvent e;
  MUST(r.scanner.poll(e));
  r.button_chip.power_cycle();
  CHECK_FALSE(r.button_chip.configured());
  r.passes(700);  // the health check runs every 500 ms
  CHECK(r.button_chip.configured());
  MUST(r.scanner.poll(e));
  CHECK(e.id == 29);
  CHECK(e.value == 0);
  CHECK(r.scanner.stats().resyncs == 1u);
  CHECK(r.scanner.stats().errors == 0u);  // no bus error: that is the point
}

TEST_CASE("scanner: a chip in an unexpected state is configured again without releasing anything") {
  Rig r;
  r.scanner.begin();
  r.pass();
  r.button_chip.press(input::key_of_button(2));
  r.passes(3);
  core::InputEvent e;
  MUST(r.scanner.poll(e));
  const std::uint8_t odd[2] = {tca::kRegCfg, 0x21};  // CFG is something else, and not zero
  MUST(r.control_bus.transfer(kAddress, odd, 2, nullptr, 0) == input::I2cResult::Ok);
  r.passes(700);
  CHECK(r.button_chip.configured());
  CHECK(r.scanner.button_held(2));
  CHECK(r.scanner.stats().resyncs == 0u);
}

// ---- time ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("scanner: events keep their order across the 32-bit tick wrap") {
  Rig r;
  r.clock.set(0xFFFFFF00u);
  r.scanner.begin();
  r.pass();
  r.pad_chip.press(input::key_of_pad(0));
  r.passes(2);
  r.pad_chip.release(input::key_of_pad(0));
  r.passes(20);  // across the wrap
  core::InputEvent e[4];
  MUST(r.pop(e, 4) == 2);
  CHECK(core::tick_elapsed(e[1].timestamp, e[0].timestamp) < 100'000u);
  CHECK(core::tick_reached(e[1].timestamp, e[0].timestamp));
}

TEST_CASE("scanner: a randomised run keeps the invariants: timestamps never go back, the consumer converges, no turn is lost") {
  Rig r;
  r.scanner.begin();
  r.pass();
  std::uint32_t seed = 12345;
  const auto next = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  bool pad_down[input::kPadCount] = {};
  bool button_down[input::kButtonCount] = {};
  std::uint64_t seen_pads = 0;
  std::uint64_t seen_buttons = 0;
  long turns_expected = 0;
  long turns_seen = 0;
  core::Tick last_stamp = 0;
  bool have_last = false;
  const auto drain = [&]() {
    core::InputEvent e;
    while (r.scanner.poll(e)) {
      if (have_last) {
        MUST(core::tick_reached(e.timestamp, last_stamp));  // never earlier than the one before
      }
      last_stamp = e.timestamp;
      have_last = true;
      const std::uint64_t bit = std::uint64_t{1} << e.id;
      if (e.kind == core::InputKind::Pad) {
        seen_pads = e.value != 0 ? (seen_pads | bit) : (seen_pads & ~bit);
      } else if (e.kind == core::InputKind::Button) {
        seen_buttons = e.value != 0 ? (seen_buttons | bit) : (seen_buttons & ~bit);
      } else {
        turns_seen += e.value;
      }
    }
  };
  std::uint16_t count = 0;
  for (unsigned pass = 0; pass < 6000; ++pass) {
    // up to two things happen per pass; the chips' FIFOs hold ten, the scan takes four a pass
    for (unsigned op = next() % 3u; op > 0; --op) {
      const unsigned which = next() % 4u;
      if (which == 0) {
        const unsigned p = next() % input::kPadCount;
        pad_down[p] = !pad_down[p];
        if (pad_down[p]) {
          r.pad_chip.press(input::key_of_pad(p));
        } else {
          r.pad_chip.release(input::key_of_pad(p));
        }
      } else if (which == 1) {
        const unsigned b = next() % input::kButtonCount;
        button_down[b] = !button_down[b];
        if (button_down[b]) {
          r.button_chip.press(input::key_of_button(b));
        } else {
          r.button_chip.release(input::key_of_button(b));
        }
      } else if (which == 2) {
        const int detents = static_cast<int>(next() % 9u) - 4;
        count = static_cast<std::uint16_t>(count + detents * 4);
        turns_expected += detents;
        r.hw.count = count;
      }
    }
    r.pass(500 + next() % 1500u);
    // the consumer is slow now and then: whole stretches pass with no poll, so the ring fills and refuses
    if ((pass / 300u) % 3u != 2u) {
      drain();
    }
  }
  // settle: let every lockout end, every refused edge be offered again
  for (unsigned i = 0; i < 40; ++i) {
    r.pass(2000);
    drain();
  }
  for (unsigned p = 0; p < input::kPadCount; ++p) {
    CHECK_MESSAGE(((seen_pads >> p) & 1u) == (pad_down[p] ? 1u : 0u), "pad " << p);
    CHECK(r.scanner.pad_held(p) == pad_down[p]);
  }
  for (unsigned b = 0; b < input::kButtonCount; ++b) {
    CHECK_MESSAGE(((seen_buttons >> b) & 1u) == (button_down[b] ? 1u : 0u), "button " << b);
  }
  CHECK(turns_seen == turns_expected);
  CHECK(r.scanner.stats().dropped > 0u);  // the run did exercise a full ring
}
