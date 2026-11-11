// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/extras/tract/halfband.h>

#include <cstring>

namespace o3sdk::extras::tract {

void HalfbandDown::reset() noexcept {
  std::memset(even_, 0, sizeof even_);
  std::memset(odd_, 0, sizeof odd_);
  epos_ = 0;
  opos_ = 0;
}

void HalfbandUp::reset() noexcept {
  std::memset(even_, 0, sizeof even_);
  std::memset(odd_, 0, sizeof odd_);
  epos_ = 0;
  opos_ = 0;
}

}  // namespace o3sdk::extras::tract
