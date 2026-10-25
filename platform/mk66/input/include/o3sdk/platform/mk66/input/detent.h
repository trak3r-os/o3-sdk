// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Encoder counts to detents (OS-007d), pure. The FTM2 quadrature decoder counts every edge of both phases in hardware (four a
// cycle, up when the phase pattern runs one way, down when it runs the other, a skipped state shows as +-2: nothing is ever
// "illegal" to the counter and nothing is missed however fast the dial spins). The scan reads the 16-bit counter; this turns the
// difference into whole detents, with the remainder kept.
//
// Net counting, no reset on a change of direction: a contact bounce or the spring pushing the dial back into its detent counts up
// and down by the same amount and is invisible, and a reversal has to come back through a whole detent before it is reported.
// No acceleration: that is a policy of the app, see README.

#include <o3sdk/platform/mk66/input/input_map.h>

#include <cstdint>

namespace o3sdk::platform::mk66::input {

class DetentAccumulator {
 public:
  explicit DetentAccumulator(std::uint16_t first_count = 0) noexcept : last_(first_count) {}

  // `count` is the counter now. Returns the detents completed since the last call (positive: counting up).
  int feed(std::uint16_t count) noexcept {
    const int delta = static_cast<std::int16_t>(static_cast<std::uint16_t>(count - last_));  // the counter wraps at 65536
    last_ = count;
    remainder_ += delta;
    const int detents = remainder_ / kEncoderCountsPerDetent;  // truncates toward zero: the remainder keeps its sign
    remainder_ -= detents * kEncoderCountsPerDetent;
    return detents;
  }
  // Counts left over inside the current detent, (-kEncoderCountsPerDetent, kEncoderCountsPerDetent).
  int remainder() const noexcept { return remainder_; }
  void restart(std::uint16_t count) noexcept {
    last_ = count;
    remainder_ = 0;
  }

 private:
  std::uint16_t last_;
  int remainder_ = 0;
};

}  // namespace o3sdk::platform::mk66::input
