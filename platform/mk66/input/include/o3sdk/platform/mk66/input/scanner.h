// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The input scan of the MK66 (OS-007d): `hal::Input` over two TCA8418 key scanners and the FTM2 encoder counter. Pure logic over
// three seams (I2cBus, ScanHardware, hal::Clock), so every path is tested on the host against fakes.
//
// Where it runs. In the MAIN LOOP, once a pass (the loop is woken by SysTick every millisecond, so a scan pass is at least that
// often): `service()` produces events, `poll()` (the app's pass) consumes them. There is no scan interrupt and no timer: the chips
// scan the matrices and debounce, and keep a ten-deep FIFO, the FTM2 counts the encoder in hardware, so there is nothing to sample
// at a fixed rate; the one periodic job is draining two FIFOs over I2C (about 115 us a register), which must not run in an ISR:
// I2C0 is shared with the codec (i2c_bus.h) and a wait on the bus at interrupt level would sit on top of the audio render and USB.
// The queue is the single-producer / single-consumer ring of core (input_event.h), so moving the scan to an interrupt later
// (a PIT in the ADR 0013 table's pin-change row) changes nothing above this class; today both ends are the main loop.
//
// Work in one pass is bounded: per chip at most INT_STAT, a clear, the counter and kMaxEventsPerDrain events (about 0.6 ms at
// 375 kHz), every transfer bounded in time by the I2cBus; a chip that fails is left alone for a growing backoff.
//
// Triggers, per chip: the INT line asserted (the level, read from the pin), more events known to be waiting, or the safety poll
// (kSafetyPollUs). The chips' events pass the lockout debounce (key_filter.h), whose accepted changes are published to the queue
// by comparison with what the queue has been told, so a full queue loses no state (stats().dropped counts the refusals).

#include <o3sdk/core/input_event.h>
#include <o3sdk/hal/clock.h>
#include <o3sdk/hal/input.h>
#include <o3sdk/platform/mk66/input/detent.h>
#include <o3sdk/platform/mk66/input/i2c_bus.h>
#include <o3sdk/platform/mk66/input/input_map.h>
#include <o3sdk/platform/mk66/input/key_filter.h>
#include <o3sdk/platform/mk66/input/scan_config.h>
#include <o3sdk/platform/mk66/input/tca8418.h>

#include <cstdint>

namespace o3sdk::platform::mk66::input {

// What the scan needs from the board besides the two buses. Main loop only.
class ScanHardware {
 public:
  virtual bool pads_int_asserted() noexcept = 0;     // the pad scanner's INT pin is low
  virtual bool buttons_int_asserted() noexcept = 0;  // the control scanner's INT pin is low
  virtual std::uint16_t encoder_count() noexcept = 0;  // the FTM2 counter, 16 bits, wrapping

 protected:
  ScanHardware() = default;
  ~ScanHardware() = default;
  ScanHardware(const ScanHardware&) = delete;
  ScanHardware& operator=(const ScanHardware&) = delete;
};

inline constexpr std::int16_t kPadVelocity = 127;  // the pads have no velocity or pressure (product-ux.md section 3): the full value

class Scanner final : public hal::Input {
 public:
  Scanner(I2cBus& pad_bus, I2cBus& control_bus, ScanHardware& hardware, const hal::Clock& clock) noexcept;

  // Configures both scanners (a few attempts each, with a bus recovery in between) and takes the encoder's first count. A chip that
  // does not answer is retried by service() with a backoff; the other one is not held up. Call once, before the first service().
  void begin() noexcept;
  // One pass: drain what is due, debounce, publish to the queue. Bounded (see above). Main loop only.
  void service() noexcept;

  bool poll(core::InputEvent& out) noexcept override { return queue_.pop(out); }
  hal::InputStats stats() const noexcept override;

  // OS-HIL (`key`, `pad`, `enc` of the diag console): one event from outside the matrices, into the SAME queue the scan fills and stamped by the same rule (never
  // earlier than the last event), so the app cannot tell it from a finger. Button / Pad: value 1.. = pressed (a pad's velocity), 0 = released; Encoder: the signed detent
  // delta. A pad's state joins pads_held() (the LEDs show a pressed pad). false: the queue is full (counted in `dropped`) or the kind / id / value is not one the scan
  // makes. Main loop only: the scan's own context, the queue's one producer (the console and the scan never run at the same time).
  bool inject(core::InputKind kind, std::uint8_t id, std::int16_t value) noexcept;

  // The debounced state as the queue will have it once drained: for the boot chord and the tests.
  bool button_held(unsigned code) const noexcept { return pads_buttons_[kButtons].filter.is_held(code); }
  bool pad_held(unsigned index) const noexcept { return pads_buttons_[kPads].filter.is_held(index); }
  std::uint64_t pads_held() const noexcept { return pads_buttons_[kPads].filter.held() | injected_pads_; }  // bit n = pad n

 private:
  enum Which : unsigned { kPads = 0, kButtons = 1 };

  // One key scanner chip: its protocol, its filter and its health.
  struct Source {
    Source(I2cBus& b, std::uint8_t address, core::InputKind k, std::uint8_t (*m)(unsigned), std::uint8_t bit) noexcept
        : bus(b), chip(b, address), kind(k), map(m), online_bit(bit) {}
    I2cBus& bus;
    Tca8418 chip;
    core::InputKind kind;
    std::uint8_t (*map)(unsigned key);  // matrix key -> pad index / button code, kNoKey if unwired
    std::uint8_t online_bit;
    KeyFilter<64> filter{scan::kKeyLockoutUs};
    bool configured = false;
    bool more = false;            // the FIFO is known to hold more than the last drain took
    bool lost = false;            // a failure has been seen since the chip last answered
    bool resync_pending = false;  // its FIFO overflowed (or an event read failed): once the FIFO is empty, the keys it did not name are forgotten
    std::uint64_t touched = 0;    // while a resync is pending: the keys the chip has named since (their latest state is the truth)
    unsigned failures = 0;        // in a row
    std::uint32_t backoff_us = 0;
    core::Tick retry_at = 0;      // no transfer before this (after a failure)
    core::Tick next_poll = 0;     // the safety poll
    core::Tick health_at = 0;     // the next read-back of the chip's configuration
  };

  void service_source(Source& s, bool int_asserted, core::Tick now) noexcept;
  void configure(Source& s, core::Tick now) noexcept;
  void drain(Source& s, core::Tick now) noexcept;
  void check_config(Source& s, core::Tick now) noexcept;
  void fail(Source& s, core::Tick now) noexcept;
  void resync_lost_events(Source& s) noexcept;
  void resync_all(Source& s) noexcept;
  void publish(Source& s) noexcept;
  void publish_encoder(core::Tick now) noexcept;
  core::Tick stamp(core::Tick time) noexcept;  // never earlier than the last event handed to the queue
  void refresh_stamp(core::Tick now) noexcept;  // drops a last stamp that is too far from `now` to be compared (scan::kStampStaleUs)

  ScanHardware& hw_;
  const hal::Clock& clock_;
  Source pads_buttons_[2];  // [kPads], [kButtons]
  DetentAccumulator detents_;
  int pending_detents_ = 0;  // turns counted and not yet accepted by a full queue
  core::Tick last_stamp_ = 0;
  core::InputQueue queue_;
  std::uint32_t dropped_ = 0;
  std::uint32_t errors_ = 0;
  std::uint32_t resyncs_ = 0;
  std::uint32_t unmapped_ = 0;  // chip events of a matrix position with no switch
  std::uint64_t injected_pads_ = 0;  // bit n: pad n is held by an injected press (OS-HIL), until its injected release
};

}  // namespace o3sdk::platform::mk66::input
