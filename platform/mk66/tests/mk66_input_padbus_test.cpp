// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-BUG-PADS: the pad bus (pad_bus.h). The gate (no access before a second after the rail and the audio start, then open for good), the
// module initialisation at the first access, the recovery policy (the module is re-initialised after a timeout, an arbitration loss, a stuck bus or a
// line read low, never after a NAK), the record (per chip address the first and latest result and the attempts, the bus clears, the pin levels),
// and the scan and the LED driver over it: the pads are not touched before the gate, a pad chip that never answers is asked once a second for ever
// without a single re-initialisation of the module, one that answers late is brought online, and the verdict that blink code 13 shows.
#include <doctest/doctest.h>

#include "mk66_input_fakes.h"

#include <o3sdk/platform/mk66/input/pad_bus.h>
#include <o3sdk/platform/mk66/input/pad_bus_log.h>
#include <o3sdk/platform/mk66/input/pins.h>
#include <o3sdk/platform/mk66/input/scan_config.h>

#include <string>

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: MUST checks and leaves the test case on failure.
#define MUST(cond)                \
  do {                            \
    const bool must_ok_ = (cond); \
    CHECK(must_ok_);              \
    if (!must_ok_) {              \
      return;                     \
    }                             \
  } while (0)

namespace in = o3sdk::platform::mk66::input;
namespace fake = o3sdk::platform::mk66::input::fake;
namespace hal = o3sdk::hal;
using o3sdk::platform::mk66::I2cResult;

namespace {

class FakeLines final : public in::LineReader {
 public:
  std::uint8_t read() noexcept override {
    ++reads;
    return levels;
  }
  std::uint8_t read_before_init() noexcept override {
    ++reads_before_init;
    return levels;
  }
  std::uint8_t levels = 0x07;
  unsigned reads = 0;              // read(): the plain register reads
  unsigned reads_before_init = 0;  // read_before_init(): the one that maps the pins
};

constexpr std::uint8_t kScan = in::pins::kKeyScannerAddress;
constexpr std::uint8_t kLed = in::pins::kLedDriverAddress;
constexpr std::uint8_t kBoth = hal::PadBusStats::kLineScl | hal::PadBusStats::kLineSda;

// A pad bus over a modelled master, its gate not yet open.
struct Rig {
  fake::ManualClock clock;  // starts at an uptime of 1000 ms
  fake::Bus master;
  FakeLines lines;
  in::PadBus bus{master, lines, clock};

  // The gate's two conditions, then the time: the rail came up now, the audio start is over, a second has passed.
  void open() {
    bus.arm(static_cast<std::uint32_t>(clock.uptime_ms()));
    bus.audio_started();
    clock.advance_ms(in::PadBus::kSettleMs);
  }
  I2cResult ask(std::uint8_t address) {
    const std::uint8_t reg = 0x01;
    std::uint8_t rx = 0;
    return bus.transfer(address, &reg, 1, &rx, 1);
  }
};

// The control scanner, the pad scanner and the pad LED driver around one pad bus, as the image wires them.
struct Board {
  fake::ManualClock clock;
  fake::Bus master;   // I2C2, behind the pad bus
  fake::Bus control;  // I2C0
  FakeLines lines;
  in::PadBus bus{master, lines, clock};
  fake::KeyChip pad_chip;
  fake::KeyChip button_chip;
  fake::Hardware hw{pad_chip, button_chip};
  in::Scanner scanner{bus, control, hw, clock};

  Board() {
    control.attach(kScan, button_chip);
    scanner.attach_pad_bus(bus);
    bus.arm(static_cast<std::uint32_t>(clock.uptime_ms()));  // the rail is up at the start of the run
  }
  void pass(std::uint32_t ms = 1) {
    clock.advance_ms(ms);
    scanner.service();
  }
  // Passes a millisecond apart for `ms` milliseconds.
  void run_ms(std::uint32_t ms) {
    for (std::uint32_t i = 0; i < ms; ++i) {
      pass();
    }
  }
  bool pads_online() { return (scanner.stats().online & hal::InputStats::kPadsOnline) != 0u; }
  bool buttons_online() { return (scanner.stats().online & hal::InputStats::kButtonsOnline) != 0u; }
};

}  // namespace

TEST_CASE("pad bus: nothing is recorded before the first transaction") {
  Rig r;
  const hal::PadBusStats s = r.bus.stats();
  CHECK(s.scanner.count == 0u);
  CHECK(s.scanner.first == hal::BusProbe::kNone);
  CHECK(s.leds.last == hal::BusProbe::kNone);
  CHECK(s.clear == hal::BusProbe::kNone);
  CHECK(s.clears == 0u);
  CHECK(s.lines_first == hal::BusProbe::kNone);
  CHECK(s.lines_now == hal::BusProbe::kNone);  // the master has not taken PTA12 / PTA13: their mux is 0, a read would say 0 whatever the line does
  CHECK(r.lines.reads == 0u);
  CHECK(r.lines.reads_before_init == 0u);
}

// ---- the gate ---------------------------------------------------------------------------------------------------------------

TEST_CASE("pad bus gate: closed until armed, until the audio start is over and until a second after the rail") {
  Rig r;
  CHECK_FALSE(r.bus.ready());  // not armed
  r.clock.advance_ms(5000);
  CHECK_FALSE(r.bus.ready());
  r.bus.arm(static_cast<std::uint32_t>(r.clock.uptime_ms()));  // the rail came up now
  r.clock.advance_ms(5000);
  CHECK_FALSE(r.bus.ready());  // armed and old enough, but the audio start is not over
  r.bus.audio_started();
  CHECK(r.bus.ready());
}

TEST_CASE("pad bus gate: not before kSettleMs after the rail, exactly then") {
  Rig r;
  r.bus.arm(static_cast<std::uint32_t>(r.clock.uptime_ms()));
  r.bus.audio_started();
  CHECK_FALSE(r.bus.ready());
  r.clock.advance_ms(in::PadBus::kSettleMs - 1u);
  CHECK_FALSE(r.bus.ready());
  r.clock.advance_ms(1);
  CHECK(r.bus.ready());
  CHECK(in::PadBus::kSettleMs == 1000u);  // a second (the brief's number)
}

TEST_CASE("pad bus gate: the rail's time counts, not the arming's") {
  Rig r;
  const auto rail = static_cast<std::uint32_t>(r.clock.uptime_ms());
  r.clock.advance_ms(600);  // the input bring-up came 600 ms after the rail
  r.bus.arm(rail);
  r.bus.audio_started();
  CHECK_FALSE(r.bus.ready());
  r.clock.advance_ms(399);
  CHECK_FALSE(r.bus.ready());
  r.clock.advance_ms(1);
  CHECK(r.bus.ready());
}

TEST_CASE("pad bus gate: once open it stays open") {
  Rig r;
  r.open();
  CHECK(r.bus.ready());
  r.clock.set(1000);  // (the clock is not expected to go back; the latch does not depend on it)
  CHECK(r.bus.ready());
}

TEST_CASE("pad bus gate: a closed gate refuses every call without touching the master, the lines or the record") {
  Rig r;
  r.bus.arm(static_cast<std::uint32_t>(r.clock.uptime_ms()));  // (audio not started)
  const std::uint8_t reg = 0x01;
  std::uint8_t rx = 0;
  CHECK(r.bus.transfer(kScan, &reg, 1, &rx, 1) == I2cResult::Nak);
  CHECK(r.bus.read(kLed, &rx, 1) == I2cResult::Nak);
  CHECK(r.bus.recover() == I2cResult::Nak);
  CHECK(r.master.transfers == 0u);
  CHECK(r.master.recovers == 0u);
  CHECK(r.lines.reads == 0u);
  CHECK(r.lines.reads_before_init == 0u);
  const hal::PadBusStats s = r.bus.stats();
  CHECK(s.scanner.count == 0u);
  CHECK(s.leds.count == 0u);
  CHECK(s.clears == 0u);
  CHECK(s.lines_first == hal::BusProbe::kNone);
  CHECK(r.lines.reads == 0u);  // (asking for the stats of a bus that is not started reads nothing either)
}

// ---- the record and the module initialisation ----------------------------------------------------------------------------------

TEST_CASE("pad bus: the first access after the gate takes the lines and initialises the module, once") {
  Rig r;
  r.lines.levels = 0x04;  // SCL and SDA low, INT high
  r.open();
  CHECK(r.master.recovers == 0u);  // the gate opening is not an access
  CHECK(r.bus.ready());
  CHECK(r.master.recovers == 0u);
  CHECK(r.ask(kScan) == I2cResult::Nak);  // nothing attached
  CHECK(r.master.recovers == 1u);          // the module's initialisation, before the transaction
  CHECK(r.lines.reads_before_init == 1u);  // the levels, taken once, before the pins are the master's
  r.lines.levels = 0x07;
  hal::PadBusStats s = r.bus.stats();
  CHECK(s.lines_first == 0x04);
  CHECK(s.lines_now == 0x07);
  CHECK(s.clears == 1u);
  CHECK(s.clear == hal::PadBusStats::kClearReleased);
  CHECK(r.ask(kLed) == I2cResult::Nak);
  CHECK(r.master.recovers == 1u);  // once
  CHECK(r.bus.stats().lines_first == 0x04);
  CHECK(r.lines.reads_before_init == 1u);  // and never again: every later read is the plain one
}

TEST_CASE("pad bus: the levels are read when asked, record() reads none") {
  Rig r;
  r.open();
  CHECK(r.ask(kScan) == I2cResult::Nak);
  const unsigned reads = r.lines.reads;
  CHECK(reads == 0u);  // (the first access takes the levels before the pins are the master's, with the other read)
  const hal::PadBusStats rec = r.bus.record();
  CHECK(r.lines.reads == reads);                  // no read for the record
  CHECK(rec.lines_now == hal::BusProbe::kNone);   // and so no "now"
  CHECK(rec.lines_first == 0x07);
  CHECK(rec.scanner.count == 1u);
  r.lines.levels = 0x05;
  CHECK(r.bus.stats().lines_now == 0x05);         // asked: read as they are
  CHECK(r.lines.reads == reads + 1u);
  CHECK(r.bus.stats().lines_now == 0x05);
  CHECK(r.lines.reads == reads + 2u);
  CHECK(r.lines.reads_before_init == 1u);
}

TEST_CASE("pad bus: a bus clear that ends with SDA low is recorded as held") {
  Rig r;
  r.open();
  r.master.dead = true;  // SDA stays low
  CHECK(r.ask(kScan) == I2cResult::Timeout);
  const hal::PadBusStats s = r.bus.stats();
  CHECK(s.clears == 1u);
  CHECK(s.clear == hal::PadBusStats::kClearHeld);
}

TEST_CASE("pad bus: a first access that is a recovery is the initialisation") {
  Rig r;
  r.open();
  CHECK(r.bus.recover() == I2cResult::Ok);
  CHECK(r.master.recovers == 1u);
  CHECK(r.bus.stats().clears == 1u);
  CHECK(r.ask(kScan) == I2cResult::Nak);
  CHECK(r.master.recovers == 1u);  // not again
}

TEST_CASE("pad bus: the first and the latest result of each chip, and the attempts") {
  Rig r;
  r.open();
  r.master.fail_next = 1;
  r.master.fail_with = I2cResult::Nak;
  CHECK(r.ask(kScan) == I2cResult::Nak);
  r.master.fail_next = 2;
  r.master.fail_with = I2cResult::Timeout;
  CHECK(r.ask(kScan) == I2cResult::Timeout);
  CHECK(r.ask(kScan) == I2cResult::Timeout);
  CHECK(r.ask(kLed) == I2cResult::Nak);  // nothing is attached: the model answers NAK
  hal::PadBusStats s = r.bus.stats();
  CHECK(s.scanner.first == static_cast<std::uint8_t>(I2cResult::Nak));
  CHECK(s.scanner.last == static_cast<std::uint8_t>(I2cResult::Timeout));
  CHECK(s.scanner.count == 3u);
  CHECK(s.leds.first == static_cast<std::uint8_t>(I2cResult::Nak));
  CHECK(s.leds.last == static_cast<std::uint8_t>(I2cResult::Nak));
  CHECK(s.leds.count == 1u);
  CHECK(r.master.transfers == 4u);  // everything went through
  fake::KeyChip chip;
  r.master.attach(kScan, chip);
  CHECK(r.ask(kScan) == I2cResult::Ok);
  s = r.bus.stats();
  CHECK(s.scanner.first == static_cast<std::uint8_t>(I2cResult::Nak));  // the first stays
  CHECK(s.scanner.last == static_cast<std::uint8_t>(I2cResult::Ok));
  CHECK(s.scanner.count == 4u);
}

TEST_CASE("pad bus: an address that is not a pad chip is forwarded and not counted") {
  Rig r;
  r.open();
  CHECK(r.ask(0x10) == I2cResult::Nak);
  CHECK(r.master.transfers == 1u);
  const hal::PadBusStats s = r.bus.stats();
  CHECK(s.scanner.count == 0u);
  CHECK(s.leds.count == 0u);
}

// ---- the recovery policy ---------------------------------------------------------------------------------------------------------

TEST_CASE("pad bus recovery: never after a NAK") {
  Rig r;
  r.open();
  CHECK(r.ask(kScan) == I2cResult::Nak);
  MUST(r.master.recovers == 1u);  // the initialisation
  for (unsigned i = 0; i < 20; ++i) {
    CHECK(r.ask(kScan) == I2cResult::Nak);
    CHECK(r.bus.recover() == I2cResult::Ok);  // released: nothing to clear
    CHECK(r.ask(kLed) == I2cResult::Nak);
    CHECK(r.bus.recover() == I2cResult::Ok);
  }
  CHECK(r.master.recovers == 1u);
  CHECK(r.bus.stats().clears == 1u);  // not counted: nothing was cleared
}

TEST_CASE("pad bus recovery: after a timeout, an arbitration loss or a stuck bus, once per transaction") {
  const I2cResult troubles[] = {I2cResult::Timeout, I2cResult::ArbitrationLost, I2cResult::BusStuck};
  for (const I2cResult trouble : troubles) {
    Rig r;
    r.open();
    CHECK(r.ask(kScan) == I2cResult::Nak);
    r.master.fail_next = 1;
    r.master.fail_with = trouble;
    CHECK(r.ask(kScan) == trouble);
    const unsigned before = r.master.recovers;
    CHECK(r.bus.recover() == I2cResult::Ok);
    CHECK(r.master.recovers == before + 1u);
    CHECK(r.bus.recover() == I2cResult::Ok);  // the bus was just cleared and nothing has gone wrong since
    CHECK(r.master.recovers == before + 1u);
    CHECK(r.bus.stats().clears == 2u);
  }
}

TEST_CASE("pad bus recovery: a NAK after a timeout says the bus works again") {
  Rig r;
  r.open();
  r.master.fail_next = 1;
  r.master.fail_with = I2cResult::Timeout;
  CHECK(r.ask(kScan) == I2cResult::Timeout);
  CHECK(r.ask(kScan) == I2cResult::Nak);
  const unsigned before = r.master.recovers;
  CHECK(r.bus.recover() == I2cResult::Ok);
  CHECK(r.master.recovers == before);
}

TEST_CASE("pad bus recovery: a line read low clears the bus whatever the latest result was") {
  Rig r;
  r.open();
  CHECK(r.ask(kScan) == I2cResult::Nak);
  const unsigned before = r.master.recovers;
  r.lines.levels = static_cast<std::uint8_t>(0x07 & ~hal::PadBusStats::kLineSda);  // a slave holds SDA
  CHECK(r.bus.recover() == I2cResult::Ok);
  CHECK(r.master.recovers == before + 1u);
  r.lines.levels = static_cast<std::uint8_t>(0x07 & ~hal::PadBusStats::kLineScl);
  CHECK(r.bus.recover() == I2cResult::Ok);
  CHECK(r.master.recovers == before + 2u);
  r.lines.levels = hal::PadBusStats::kLineInt;  // INT alone high is no help: both bus lines are low
  CHECK(r.bus.recover() == I2cResult::Ok);
  CHECK(r.master.recovers == before + 3u);
  r.lines.levels = kBoth;  // released
  CHECK(r.bus.recover() == I2cResult::Ok);
  CHECK(r.master.recovers == before + 3u);
}

TEST_CASE("pad bus recovery: a clear that fails leaves the bus due for another") {
  Rig r;
  r.open();
  CHECK(r.ask(kScan) == I2cResult::Nak);
  r.master.dead = true;  // SDA stays low
  CHECK(r.ask(kScan) == I2cResult::Timeout);
  CHECK(r.bus.recover() == I2cResult::BusStuck);
  CHECK(r.bus.recover() == I2cResult::BusStuck);  // still stuck: tried again
  CHECK(r.bus.stats().clear == hal::PadBusStats::kClearHeld);
  r.master.dead = false;
  CHECK(r.bus.recover() == I2cResult::Ok);
  CHECK(r.bus.stats().clear == hal::PadBusStats::kClearReleased);
  const unsigned before = r.master.recovers;
  CHECK(r.bus.recover() == I2cResult::Ok);
  CHECK(r.master.recovers == before);  // released: done
}

// ---- the scan over the gated bus ---------------------------------------------------------------------------------------------------

TEST_CASE("pad bus scan: before the gate only the control buttons are asked") {
  Board b;
  b.scanner.begin();
  CHECK(b.master.transfers == 0u);  // the pad bus was not touched
  CHECK(b.master.recovers == 0u);
  CHECK(b.lines.reads == 0u);
  CHECK(b.lines.reads_before_init == 0u);
  CHECK(b.buttons_online());
  CHECK_FALSE(b.pads_online());
  b.run_ms(3000);  // the gate stays shut: the audio start is not over
  CHECK(b.master.transfers == 0u);
  CHECK(b.master.recovers == 0u);
  CHECK_FALSE(b.scanner.pads_gave_no_answer());  // not asked, not refused
  CHECK(b.scanner.stats().errors == 0u);
  CHECK(b.buttons_online());
}

TEST_CASE("pad bus scan: the control buttons are not held up by the gate") {
  Board b;
  const unsigned shift = 29;  // Shift in the unit's button numbering
  b.pad_chip.press(3);        // (nothing reads it yet)
  b.button_chip.press(in::key_of_button(shift));
  b.scanner.begin();
  b.run_ms(20);
  o3sdk::core::InputEvent e;
  MUST(b.scanner.poll(e));
  CHECK(e.kind == o3sdk::core::InputKind::Button);
  CHECK(e.id == shift);
  CHECK_FALSE(b.scanner.poll(e));
}

TEST_CASE("pad bus scan: the pads come up on the first pass after the gate, a key held at power-up included") {
  Board b;
  b.master.attach(kScan, b.pad_chip);
  b.pad_chip.press(2);  // held since power-up: in the FIFO
  b.scanner.begin();
  b.bus.audio_started();
  b.run_ms(in::PadBus::kSettleMs - 2u);
  CHECK(b.master.transfers == 0u);  // the second has not passed
  b.run_ms(5);
  CHECK(b.pads_online());
  CHECK(b.master.recovers == 1u);  // the module initialised once, at the first access
  CHECK(b.scanner.stats().pad_bus.scanner.first == static_cast<std::uint8_t>(I2cResult::Ok));
  CHECK_FALSE(b.scanner.pads_gave_no_answer());
  o3sdk::core::InputEvent e;
  bool pad_seen = false;
  while (b.scanner.poll(e)) {
    pad_seen = pad_seen || e.kind == o3sdk::core::InputKind::Pad;
  }
  CHECK(pad_seen);
}

TEST_CASE("pad bus scan: a pad chip that never answers is asked once a second for ever and the module is never re-initialised") {
  Board b;  // no pad chip attached: every transaction is a NAK
  b.scanner.begin();
  b.bus.audio_started();
  b.run_ms(in::PadBus::kSettleMs + 5u);
  // The first attempt: one transaction (the configuration stops at the first refused register write).
  CHECK(b.scanner.stats().pad_bus.scanner.count == 1u);
  CHECK(b.master.recovers == 1u);
  b.run_ms(900);
  CHECK(b.scanner.stats().pad_bus.scanner.count == 1u);  // not before the second is up
  b.run_ms(110);
  CHECK(b.scanner.stats().pad_bus.scanner.count == 2u);
  CHECK_FALSE(b.scanner.pads_gave_no_answer());  // two refusals
  b.run_ms(1000);
  CHECK(b.scanner.stats().pad_bus.scanner.count == 3u);
  CHECK(b.scanner.pads_gave_no_answer());
  // For two minutes more: one a second, no module re-initialisation, the control buttons untouched.
  const unsigned control_before = b.control.transfers;
  const unsigned pad_before = b.master.transfers;
  b.run_ms(120000);
  CHECK(b.master.transfers - pad_before >= 119u);
  CHECK(b.master.transfers - pad_before <= 121u);
  CHECK(b.master.recovers == 1u);
  CHECK(b.bus.stats().clears == 1u);
  CHECK(b.scanner.pads_gave_no_answer());
  CHECK(b.buttons_online());
  CHECK(b.control.transfers > control_before);  // (the buttons' own safety poll goes on)
}

TEST_CASE("pad bus scan: a pad chip that answers late is brought online, and the verdict goes") {
  Board b;
  b.scanner.begin();
  b.bus.audio_started();
  b.run_ms(5000);
  CHECK(b.scanner.pads_gave_no_answer());
  CHECK_FALSE(b.pads_online());
  b.master.attach(kScan, b.pad_chip);  // it comes up
  b.run_ms(1100);
  CHECK(b.pads_online());
  CHECK_FALSE(b.scanner.pads_gave_no_answer());
  CHECK(b.master.recovers == 1u);
  b.pad_chip.press(7);
  b.run_ms(30);
  o3sdk::core::InputEvent e;
  bool pad_seen = false;
  while (b.scanner.poll(e)) {
    pad_seen = pad_seen || (e.kind == o3sdk::core::InputKind::Pad && e.id == 7);
  }
  CHECK(pad_seen);
}

TEST_CASE("pad bus scan: a pad chip that answered and is lost is asked again at the usual backoff, not once a second") {
  Board b;
  b.master.attach(kScan, b.pad_chip);
  b.scanner.begin();
  b.bus.audio_started();
  b.run_ms(1100);
  MUST(b.pads_online());
  b.master.fail_next = 1000;
  b.master.fail_with = I2cResult::Nak;
  b.run_ms(1100);  // the safety poll fails, then retries at 10, 20, 40, ... ms
  CHECK_FALSE(b.pads_online());
  CHECK(b.master.transfers > 6u);
  CHECK_FALSE(b.scanner.pads_gave_no_answer());  // it did answer once
}

TEST_CASE("pad bus scan: timeouts do re-initialise the module, from the second in a row") {
  Board b;
  b.master.dead = true;  // every transaction times out, and the clear fails
  b.scanner.begin();
  b.bus.audio_started();
  b.run_ms(in::PadBus::kSettleMs + 5u);  // the first attempt
  CHECK(b.master.recovers == 1u);
  b.run_ms(1000);                          // the second: the bus is recovered after it
  CHECK(b.master.recovers == 2u);
  b.run_ms(1000);
  CHECK(b.master.recovers == 3u);
  CHECK(b.bus.stats().clear == hal::PadBusStats::kClearHeld);
  CHECK(b.scanner.pads_gave_no_answer());
}

TEST_CASE("pad bus scan: the stats carry the bus record") {
  Board b;
  CHECK(b.scanner.stats().pad_bus.scanner.count == 0u);
  b.scanner.begin();
  b.bus.audio_started();
  b.run_ms(in::PadBus::kSettleMs + 5u);
  const hal::InputStats stats = b.scanner.stats();
  CHECK(stats.pad_bus.scanner.count == 1u);
  CHECK(stats.pad_bus.scanner.first == static_cast<std::uint8_t>(I2cResult::Nak));
  CHECK(stats.pad_bus.clears == 1u);
  CHECK(stats.pad_bus.lines_first == 0x07);
  CHECK(stats.pad_bus.lines_now == 0x07);
}

TEST_CASE("pad bus scan: the watch's look reads no pin, stats() does") {
  Board b;
  b.scanner.begin();
  b.bus.audio_started();
  b.run_ms(in::PadBus::kSettleMs + 5u);
  const unsigned reads = b.lines.reads;
  for (unsigned i = 0; i < 1000; ++i) {  // a pass per millisecond, as the main loop's watch looks
    const hal::InputStats look = b.scanner.stats_without_levels();
    CHECK(look.pad_bus.lines_now == hal::BusProbe::kNone);
    CHECK(look.pad_bus.lines_first == 0x07);  // the record is all there
    CHECK(look.pad_bus.scanner.count == 1u);
  }
  CHECK(b.lines.reads == reads);
  CHECK(b.scanner.stats().pad_bus.lines_now == 0x07);  // asked: sampled
  CHECK(b.lines.reads == reads + 1u);
}

// ---- what the log gets -------------------------------------------------------------------------------------------------------------

namespace {
using Entry = in::PadBusLogPolicy::Entry;
}

TEST_CASE("pad bus log: nothing until something happens, then a full entry at the first result of each chip") {
  in::PadBusLogPolicy p;
  for (unsigned i = 0; i < 1000; ++i) {
    CHECK(p.update(false, false, false) == Entry::None);
  }
  CHECK(p.update(false, true, false) == Entry::Full);   // the pad scanner's first refusal
  CHECK(p.update(false, true, false) == Entry::None);
  CHECK(p.update(false, true, true) == Entry::Full);    // the LED driver's first result
  for (unsigned i = 0; i < 1000; ++i) {
    CHECK(p.update(false, true, true) == Entry::None);
  }
  CHECK(p.flips() == 0u);
}

TEST_CASE("pad bus log: the first answer is one full entry, not two") {
  in::PadBusLogPolicy p;
  CHECK(p.update(true, true, true) == Entry::Full);  // answered and counted in the same look
  CHECK(p.flips() == 1u);
  CHECK(p.online());
  CHECK(p.update(true, true, true) == Entry::None);
}

TEST_CASE("pad bus log: a flapping chip is logged in full a few times, then one line per many changes") {
  in::PadBusLogPolicy p;
  CHECK(p.update(false, true, true) == Entry::Full);
  unsigned full = 0;
  unsigned summary = 0;
  bool online = false;
  constexpr unsigned kFlips = 3600;  // an hour at one change a second
  for (unsigned i = 1; i <= kFlips; ++i) {
    online = !online;
    const Entry e = p.update(online, true, true);
    if (e == Entry::Full) {
      ++full;
      CHECK(i <= in::PadBusLogPolicy::kFullFlips);
    } else if (e == Entry::Summary) {
      ++summary;
      CHECK(i % in::PadBusLogPolicy::kSummaryEvery == 0u);
      CHECK(p.flips() == i);
      CHECK(p.online() == online);
    }
    CHECK(p.update(online, true, true) == Entry::None);  // a pass with no change in between
  }
  static_assert(in::PadBusLogPolicy::kFullFlips < in::PadBusLogPolicy::kSummaryEvery, "the summaries start after the full entries");
  CHECK(full == in::PadBusLogPolicy::kFullFlips);
  CHECK(summary == kFlips / in::PadBusLogPolicy::kSummaryEvery);
  // an hour of flapping writes (3 lines a full entry, 1 a summary) a small part of the log's 1024 records
  CHECK(1u + full * 3u + summary < 100u);
}

TEST_CASE("pad bus log: a chip that answers late is still logged in full, after the flapping") {
  in::PadBusLogPolicy p;
  CHECK(p.update(false, true, false) == Entry::Full);
  for (unsigned i = 0; i < 200; ++i) {
    (void)p.update(i % 2u == 0u, true, false);
  }
  CHECK(p.update(p.online(), true, true) == Entry::Full);  // the LED driver's first result, however late
  CHECK(p.update(p.online(), true, true) == Entry::None);
}

// ---- the pad LED driver over the gated bus ---------------------------------------------------------------------------------------------

TEST_CASE("pad bus leds: the driver waits for the gate, then sets itself up") {
  fake::ManualClock clock;
  fake::Bus master;
  FakeLines lines;
  in::PadBus bus(master, lines, clock);
  fake::LedChip chip;
  master.attach(kLed, chip);
  in::PadLeds leds(bus, clock);
  bus.arm(static_cast<std::uint32_t>(clock.uptime_ms()));
  leds.set(3, 255);
  for (unsigned i = 0; i < 3000; ++i) {
    clock.advance_ms(1);
    (void)leds.flush();
  }
  CHECK(master.transfers == 0u);
  CHECK_FALSE(leds.online());
  bus.audio_started();
  for (unsigned i = 0; i < 1500; ++i) {
    clock.advance_ms(1);
    (void)leds.flush();
  }
  CHECK(leds.online());
  CHECK(chip.pwm(3) == 255);
  CHECK(master.recovers == 1u);
}

TEST_CASE("pad bus leds: a driver whose chip never answers is asked once a second, without a module re-initialisation") {
  fake::ManualClock clock;
  fake::Bus master;
  FakeLines lines;
  in::PadBus bus(master, lines, clock);
  in::PadLeds leds(bus, clock);  // no chip: NAK
  bus.arm(static_cast<std::uint32_t>(clock.uptime_ms()));
  bus.audio_started();
  clock.advance_ms(in::PadBus::kSettleMs);
  for (unsigned i = 0; i < 60000; ++i) {
    clock.advance_ms(1);
    (void)leds.flush();
  }
  CHECK(master.transfers >= 59u);
  CHECK(master.transfers <= 61u);
  CHECK(master.recovers == 1u);
  CHECK_FALSE(leds.online());
}

TEST_CASE("pad bus: result names") {
  CHECK(std::string(hal::bus_result_name(0)) == "ACK");
  CHECK(std::string(hal::bus_result_name(1)) == "TMO");
  CHECK(std::string(hal::bus_result_name(2)) == "NAK");
  CHECK(std::string(hal::bus_result_name(3)) == "ARB");
  CHECK(std::string(hal::bus_result_name(4)) == "STK");
  CHECK(std::string(hal::bus_result_name(hal::BusProbe::kNone)) == "--");
}
