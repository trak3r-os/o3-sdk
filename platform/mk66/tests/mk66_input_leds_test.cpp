// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pad LED driver (pad_leds.h) against a model of the IS31FL3731: the matrix position of each pad, the brightness curve and its
// limit, the setup sequence (one bounded transfer a flush, the 10 ms pause between shutdown and normal operation done by watching the
// clock), the frame diff (only what changed, a row at a time, nothing when nothing changed), the health read-back and the whole
// setup again when the chip has reset itself, a failing bus (backoff, recovery, restoring the frame). Runs on the host.
#include <doctest/doctest.h>

#include "mk66_input_fakes.h"

#include <cstdint>

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: MUST checks and leaves the test case on failure.
#define MUST(cond)                \
  do {                            \
    const bool must_ok_ = (cond); \
    CHECK(must_ok_);              \
    if (!must_ok_) {              \
      return;                     \
    }                             \
  } while (0)

namespace input = o3sdk::platform::mk66::input;
namespace fake = o3sdk::platform::mk66::input::fake;
namespace chip = o3sdk::platform::mk66::input::is31fl3731;

namespace {

constexpr std::uint8_t kAddress = 0x74;

struct Rig {
  fake::ManualClock clock;
  fake::Bus bus;
  fake::LedChip chip;
  input::PadLeds leds{bus, clock};
  Rig() { bus.attach(kAddress, chip); }
  // Calls flush() a millisecond apart until the driver is online with nothing left to send (a pause or a backoff says "nothing to send"
  // too, but the driver is not online then); returns the calls it took.
  unsigned settle(unsigned limit = 400) {
    unsigned calls = 0;
    while (calls < limit) {
      clock.advance_ms(1);
      ++calls;
      if (leds.flush() && leds.online()) {
        break;
      }
    }
    return calls;
  }
  // The setup, to Ready.
  void ready() { settle(); }
};

}  // namespace

TEST_CASE("pad leds: a pad's PWM register and on bit follow the matrix position (n + 4 * (n / 12))") {
  CHECK(input::pad_pwm_register(0) == 0x24);
  CHECK(input::pad_pwm_register(11) == 0x24 + 11);
  CHECK(input::pad_pwm_register(12) == 0x24 + 16);  // the second pad row starts a matrix row down
  CHECK(input::pad_pwm_register(47) == 0x24 + 3 * 16 + 11);
  for (unsigned pad = 0; pad < o3sdk::hal::kPadLedCount; ++pad) {
    const unsigned num = pad + 4u * (pad / 12u);  // four registers are skipped a row
    CHECK(input::pad_pwm_register(pad) == chip::kFramePwm + num);
  }
}

TEST_CASE("pad leds: the brightness curve is monotonic, gamma 2, never above 255, and only 0 is dark") {
  CHECK(input::pad_pwm_of_level(0) == 0);
  CHECK(input::pad_pwm_of_level(255) == 255);
  CHECK(input::pad_pwm_of_level(128) == 64);
  unsigned last = 0;
  for (unsigned level = 0; level <= 255; ++level) {
    const unsigned pwm = input::pad_pwm_of_level(static_cast<std::uint8_t>(level));
    CHECK(pwm >= last);
    CHECK(pwm <= 255u);
    CHECK((pwm == 0u) == (level == 0u));
    last = pwm;
  }
}

TEST_CASE("pad leds: the setup is one transfer a call, with the 10 ms pause done by the clock") {
  Rig r;
  CHECK(r.leds.count() == 48);
  CHECK_FALSE(r.leds.online());
  // call by call: bank, shutdown (then the pause), function registers, bank, on bits zeroed, blink, 9 PWM rows, the pads' on bits, bank,
  // normal operation, bank
  unsigned calls = 0;
  unsigned last_transfers = 0;
  while (!r.leds.online() && calls < 100) {
    r.clock.advance_us(100);  // faster than the pause: it must be the clock that holds the setup back
    r.leds.flush();
    CHECK(r.bus.transfers - last_transfers <= 1u);  // one bounded transfer a call
    last_transfers = r.bus.transfers;
    ++calls;
    if (calls == 2) {
      CHECK(r.chip.shutdowns == 1u);  // shutdown written
    }
  }
  CHECK_FALSE(r.leds.online());  // 100 calls of 100 us: still inside the setup, waiting on the 10 ms pause
  CHECK(r.bus.transfers == 2u);  // and nothing was sent while it waited
  r.ready();
  CHECK(r.leds.online());
  CHECK(r.chip.normal_operation());
  CHECK(r.chip.bank() == chip::kBankFrame1);
  const fake::Bus::Entry* log = r.bus.log;
  CHECK((log[0].reg == chip::kRegCommand && log[0].value == chip::kBankFunction));
  CHECK((log[1].reg == chip::kFnShutdown && log[1].value == 0x00));
  CHECK((log[2].reg == chip::kFnConfiguration && log[2].value == 0x00));
  CHECK((log[3].reg == chip::kFnPictureDisplay && log[3].value == 0x00));
  CHECK((log[4].reg == chip::kFnAudioSync && log[4].value == 0x00));
  CHECK((log[5].reg == chip::kRegCommand && log[5].value == chip::kBankFrame1));
  CHECK((log[6].reg == chip::kFrameOnOff && log[6].tx_len == 1 + chip::kOnOffBytes && log[6].value == 0x00));  // on bits all off
  CHECK((log[7].reg == chip::kFrameBlink && log[7].tx_len == 1 + chip::kOnOffBytes));
  for (unsigned row = 0; row < chip::kMatrixRows; ++row) {
    CHECK(log[8 + row].reg == chip::kFramePwm + row * chip::kMatrixColumns);
    CHECK(log[8 + row].tx_len == 1 + chip::kMatrixColumns);
  }
  CHECK((log[17].reg == chip::kFrameOnOff && log[17].tx_len == 1 + chip::kOnOffBytes && log[17].value == 0xFF));  // the pads' on bits
  CHECK((log[18].reg == chip::kRegCommand && log[18].value == chip::kBankFunction));
  CHECK((log[19].reg == chip::kFnShutdown && log[19].value == 0x01));  // normal operation, last
  CHECK((log[20].reg == chip::kRegCommand && log[20].value == chip::kBankFrame1));
  CHECK(r.bus.log_count == 21u);
  for (unsigned pad = 0; pad < 48; ++pad) {
    CHECK_MESSAGE(r.chip.on(pad), "pad " << pad);  // every pad's LED is on, its PWM decides
    CHECK(r.chip.pwm(pad) == 0);
  }
}

TEST_CASE("pad leds: the setup keeps the pads dark: shutdown first, the frame cleared before the on bits are set, normal operation last") {
  Rig r;
  // The chip as an earlier run left it: its registers outlive a reset of the MCU, so two pads are still lit when the driver starts.
  r.chip.stale_run(7, 200);
  r.chip.stale_run(30, 255);
  CHECK(r.chip.lit());
  unsigned calls = 0;
  bool dark_while_setting_up = true;
  while (!r.leds.online() && calls < 400) {
    r.clock.advance_ms(1);
    r.leds.flush();
    ++calls;
    if (r.chip.shutdowns > 0u && r.chip.lit()) {
      dark_while_setting_up = false;  // a pad visibly lit after the driver had put the chip in shutdown
    }
  }
  CHECK(r.leds.online());
  CHECK(dark_while_setting_up);
  CHECK_FALSE(r.chip.lit());  // and when the chip first lights anything it is a frame that is all zero
  // The write order, from the log: shutdown, on bits zeroed, every PWM cleared, the pads' on bits, normal operation.
  int enter_shutdown = -1;
  int zero_on = -1;
  int first_clear = -1;
  int last_clear = -1;
  int set_on = -1;
  int leave_shutdown = -1;
  for (unsigned i = 0; i < r.bus.log_count; ++i) {
    const fake::Bus::Entry& e = r.bus.log[i];
    const int at = static_cast<int>(i);
    if (e.reg == chip::kFnShutdown && e.tx_len == 2) {
      (e.value == 0x00 ? enter_shutdown : leave_shutdown) = at;
    } else if (e.reg == chip::kFrameOnOff && e.tx_len == 1 + chip::kOnOffBytes) {
      (e.value == 0x00 ? zero_on : set_on) = at;
    } else if (e.reg >= chip::kFramePwm && e.tx_len == 1 + chip::kMatrixColumns) {
      first_clear = first_clear < 0 ? at : first_clear;
      last_clear = at;
    }
  }
  CHECK(enter_shutdown >= 0);
  CHECK(enter_shutdown < zero_on);
  CHECK(zero_on < first_clear);
  CHECK(first_clear <= last_clear);
  CHECK(last_clear < set_on);
  CHECK(set_on < leave_shutdown);
  CHECK(leave_shutdown >= 0);
}

TEST_CASE("pad leds: a flush sends only what changed, one row a transfer, and nothing when nothing changed") {
  Rig r;
  r.ready();
  const unsigned base = r.bus.transfers;
  CHECK(r.leds.flush());  // nothing staged: nothing sent
  CHECK(r.bus.transfers == base);

  r.leds.set(5, 255);
  CHECK(r.leds.flush());  // one pad: one transfer, and nothing is left
  CHECK(r.bus.transfers == base + 1u);
  CHECK(r.bus.log[base].reg == input::pad_pwm_register(5));
  CHECK(r.bus.log[base].tx_len == 2);  // the register and one byte
  CHECK(r.chip.pwm(5) == 255);
  const unsigned after = r.bus.transfers;
  CHECK(r.leds.flush());
  CHECK(r.bus.transfers == after);  // current: nothing again

  // two pads of one row, a gap between: one transfer from the first to the last
  r.leds.set(0, 100);
  r.leds.set(11, 100);
  const bool done_one = r.leds.flush();
  CHECK(done_one);
  CHECK(r.bus.log[after].tx_len == 1 + 12);
  CHECK(r.chip.pwm(0) == input::pad_pwm_of_level(100));
  CHECK(r.chip.pwm(11) == input::pad_pwm_of_level(100));

  // two rows: two calls
  r.leds.set(1, 50);    // row 0
  r.leds.set(30, 50);   // row 2
  const unsigned t0 = r.bus.transfers;
  CHECK_FALSE(r.leds.flush());  // one row sent, one left
  CHECK(r.bus.transfers == t0 + 1u);
  CHECK(r.leds.flush());        // the other, and then nothing is left
  CHECK(r.bus.transfers == t0 + 2u);
  CHECK(r.chip.pwm(1) == input::pad_pwm_of_level(50));
  CHECK(r.chip.pwm(30) == input::pad_pwm_of_level(50));

  r.leds.clear();
  r.settle(10);
  for (unsigned pad = 0; pad < 48; ++pad) {
    CHECK(r.chip.pwm(pad) == 0);
  }
}

TEST_CASE("pad leds: in_sync (OS-HIL `diag leds`) says whether the chip holds the PWM of every staged level, and level() what is staged") {
  Rig r;
  r.ready();
  CHECK(r.leds.in_sync());
  r.leds.set(5, 255);
  CHECK(r.leds.level(5) == 255);
  CHECK_FALSE(r.leds.in_sync());  // staged, not on the chip yet
  CHECK(r.leds.flush());
  CHECK(r.leds.in_sync());
  r.leds.set(1, 50);   // row 0
  r.leds.set(30, 50);  // row 2: two transfers
  CHECK_FALSE(r.leds.flush());
  CHECK_FALSE(r.leds.in_sync());  // one row sent, one left
  CHECK(r.leds.flush());
  CHECK(r.leds.in_sync());
  r.leds.clear();
  CHECK_FALSE(r.leds.in_sync());  // a cleared frame is staged, not sent
  r.settle(10);
  CHECK(r.leds.in_sync());
  CHECK(r.leds.level(5) == 0);
  CHECK(r.leds.level(48) == 0);  // past the last pad
}

TEST_CASE("pad leds: a frame never writes outside the pads' own registers, and an index past the end is ignored") {
  Rig r;
  r.ready();
  r.leds.set(48, 255);  // not a pad
  r.leds.set(1000, 255);
  for (unsigned pad = 0; pad < 48; ++pad) {
    r.leds.set(pad, 255);
  }
  r.settle(20);
  for (unsigned pad = 0; pad < 48; ++pad) {
    CHECK(r.chip.pwm(pad) == 255);
  }
  // the columns of the matrix the pads do not use stay dark, and so do rows 4..8
  for (unsigned row = 0; row < chip::kMatrixRows; ++row) {
    for (unsigned col = 0; col < chip::kMatrixColumns; ++col) {
      const bool pad_cell = row < 4u && col < 12u;
      if (!pad_cell) {
        CHECK(r.chip.raw_frame(chip::kFramePwm + row * chip::kMatrixColumns + col) == 0);
      }
    }
  }
}

TEST_CASE("pad leds: the shutdown register is read back, and a chip that reset itself is set up again with the frame restored") {
  Rig r;
  r.ready();
  r.leds.set(7, 200);
  r.settle(10);
  CHECK(r.chip.pwm(7) == input::pad_pwm_of_level(200));
  const unsigned t0 = r.bus.transfers;
  r.clock.advance_ms(600);  // past the health interval
  r.settle(20);
  CHECK(r.bus.transfers - t0 == 3u);  // bank, read, bank back
  CHECK(r.chip.bank() == chip::kBankFrame1);
  CHECK(r.leds.reinits() == 0u);
  CHECK(r.leds.online());

  r.chip.reset();  // a glitch took the chip's state, no bus error
  r.clock.advance_ms(600);
  r.settle(100);
  r.clock.advance_ms(20);
  r.settle(100);
  CHECK(r.leds.reinits() == 1u);
  CHECK(r.leds.online());
  CHECK(r.chip.normal_operation());
  CHECK(r.chip.pwm(7) == input::pad_pwm_of_level(200));  // the frame is back
  CHECK(r.chip.on(7));
}

TEST_CASE("pad leds: park (OS-POWER P1) writes every pad dark, then the chip goes into software shutdown and stays there") {
  Rig r;
  r.ready();
  r.leds.set(3, 255);
  r.leds.set(40, 200);
  r.settle(10);
  CHECK(r.chip.pwm(3) == 255);
  CHECK(r.chip.pwm(40) == input::pad_pwm_of_level(200));
  CHECK(r.chip.normal_operation());
  r.leds.park();
  CHECK_FALSE(r.leds.parked());
  for (unsigned i = 0; i < 40 && !r.leds.parked(); ++i) {
    r.clock.advance_ms(1);
    (void)r.leds.flush();
  }
  MUST(r.leds.parked());
  CHECK(r.chip.pwm(3) == 0);
  CHECK(r.chip.pwm(40) == 0);
  CHECK_FALSE(r.chip.normal_operation());  // the shutdown register reads 0: dark until the chip's own power goes
  // nothing is ever sent again, not even the health read-back, and a pad that is set stays dark
  const unsigned transfers = r.bus.transfers;
  r.leds.set(5, 255);
  for (unsigned i = 0; i < 80; ++i) {
    r.clock.advance_ms(20);
    CHECK(r.leds.flush());
  }
  CHECK(r.bus.transfers == transfers);
  CHECK(r.leds.level(5) == 0);
  CHECK(r.leds.parked());
}

TEST_CASE("pad leds: park during the setup waits for it to end, and no pad lights on the way") {
  Rig r;
  r.leds.set(9, 255);
  r.leds.park();
  for (unsigned i = 0; i < 400 && !r.leds.parked(); ++i) {
    r.clock.advance_ms(1);
    (void)r.leds.flush();
    CHECK(r.chip.pwm(9) == 0);
  }
  CHECK(r.leds.parked());
  CHECK_FALSE(r.chip.normal_operation());
}

TEST_CASE("pad leds: a failing bus is retried with a backoff and recovered, then the setup and the frame are redone") {
  Rig r;
  r.ready();
  r.leds.set(3, 255);
  r.settle(10);
  r.bus.dead = true;
  r.leds.set(4, 255);
  const unsigned t0 = r.bus.transfers;
  for (unsigned i = 0; i < 1000; ++i) {  // a second of calls
    r.clock.advance_ms(1);
    r.leds.flush();
  }
  CHECK(r.bus.transfers - t0 <= 12u);  // a handful of attempts, not a thousand
  CHECK(r.bus.transfers - t0 >= 3u);
  CHECK(r.bus.recovers >= 1u);
  CHECK_FALSE(r.leds.online());
  CHECK(r.leds.errors() >= 3u);
  r.bus.dead = false;
  r.chip.reset();  // it was power-cycled meanwhile
  for (unsigned i = 0; i < 1000; ++i) {
    r.clock.advance_ms(1);
    r.leds.flush();
  }
  CHECK(r.leds.online());
  CHECK(r.chip.pwm(3) == 255);  // both pads are back
  CHECK(r.chip.pwm(4) == 255);
}

TEST_CASE("pad leds: one NAK in the setup starts it again, and the driver is not online until it has run through") {
  Rig r;
  r.bus.fail_next = 1;
  r.settle(5);
  CHECK(r.leds.errors() == 1u);
  CHECK_FALSE(r.leds.online());
  r.clock.advance_ms(30);  // the backoff
  r.settle(400);
  CHECK(r.leds.online());
  CHECK(r.chip.normal_operation());
}

TEST_CASE("pad leds: the staged frame is word aligned") {
  CHECK(alignof(input::PadLeds) >= 4u);
}
