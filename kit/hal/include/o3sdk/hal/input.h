// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/core/input_event.h>

#include <cstdint>

namespace o3sdk::hal {

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
