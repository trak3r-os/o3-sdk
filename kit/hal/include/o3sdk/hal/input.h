// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/core/input_event.h>

#include <cstdint>

namespace o3sdk::hal {

// OS-BUG-PADS: what one chip on the pad bus (I2C2) answered, as the driver below the scan saw it. The result codes are the platform's I2cResult numbers (0 ack, 1 timeout, 2 NAK, 3 arbitration lost,
// 4 bus stuck); kNone: no transaction yet. Read only: nothing here changes how the bus is driven.
struct BusProbe {
  static constexpr std::uint8_t kNone = 0xFF;
  std::uint8_t first = kNone;  // the result of the first transaction with the chip
  std::uint8_t last = kNone;   // and of the latest one
  std::uint32_t count = 0;     // transactions tried (attempts)
};

// The pad bus as a whole: the two chips on it (the pad key scanner at 0x34, the pad LED driver at 0x74), the module initialisations (bus clears) and the pin levels.
struct PadBusStats {
  static constexpr std::uint8_t kLineScl = 1u << 0;  // PTA12 high
  static constexpr std::uint8_t kLineSda = 1u << 1;  // PTA13 high
  static constexpr std::uint8_t kLineInt = 1u << 2;  // PTC11 (the pad scanner's INT) high
  static constexpr std::uint8_t kClearReleased = 0;  // the bus clear ended with SDA high
  static constexpr std::uint8_t kClearHeld = 1;      // SDA still low after the nine pulses
  BusProbe scanner;                                  // 0x34
  BusProbe leds;                                     // 0x74
  std::uint8_t clear = BusProbe::kNone;              // the last bus clear (module initialisation): kClearReleased / kClearHeld
  std::uint8_t clears = 0;                           // how many, saturating
  std::uint8_t lines_first = BusProbe::kNone;        // the levels (kLine* bits) read just before the first transaction
  std::uint8_t lines_now = BusProbe::kNone;          // and when these stats were taken
};

// The 3-letter name of a probe result code, "--" for none.
constexpr const char* bus_result_name(std::uint8_t code) noexcept {
  switch (code) {
    case 0:
      return "ACK";
    case 1:
      return "TMO";
    case 2:
      return "NAK";
    case 3:
      return "ARB";
    case 4:
      return "STK";
    default:
      return "--";
  }
}

// Health counters of the input path, monotonic since boot (OS-007d): what the INPUT TEST screen shows and `diag` will report.
// A source is one chip or counter that produces events.
struct InputStats {
  static constexpr std::uint8_t kPadsOnline = 1u << 0;     // the pad key scanner answers
  static constexpr std::uint8_t kButtonsOnline = 1u << 1;  // the control-button key scanner answers
  std::uint32_t dropped = 0;    // events refused by a full queue (the newest is dropped and counted, never blocks)
  std::uint32_t errors = 0;     // failed bus transactions
  std::uint32_t filtered = 0;   // chip edges the debounce swallowed (bounce) or the consistency check refused
  std::uint32_t resyncs = 0;    // times a chip FIFO overflow or a lost chip made the driver forget what was held
  std::uint8_t online = 0;      // kPadsOnline | kButtonsOnline
  PadBusStats pad_bus;          // OS-BUG-PADS: the pad bus's own record (a source without such a bus leaves it empty)
};

// Source of user-input events (buttons, pads, encoder), oldest first.
// poll() runs in the main loop only and never blocks: on hardware the scan pushes into a core::InputQueue (a single-producer /
// single-consumer ring) and poll() is the consumer side; on the host a fake replays a script. The MK66's scan runs in the main loop
// as well (platform/mk66/input/scanner.h says why), so both ends of its ring are the main loop; the ring would take an ISR as the
// producer unchanged.
class Input {
 public:
  // Moves the oldest pending event into `out` and returns true; returns false (out untouched) if none.
  virtual bool poll(core::InputEvent& out) noexcept = 0;

  // Health counters; a source without any (the host fake) reports zeros and nothing online.
  virtual InputStats stats() const noexcept { return InputStats{}; }

 protected:
  Input() = default;
  ~Input() = default;
  Input(const Input&) = delete;
  Input& operator=(const Input&) = delete;
};

}  // namespace o3sdk::hal
