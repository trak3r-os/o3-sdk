// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::formats {

// What a resumable reader or writer answers from step(): still going, finished, or ended in an error (the
// report says which). One type for every format, so a job can drive any of them (ADR 0009 rev 1).
enum class Progress : std::uint8_t { Running, Done, Failed };

}  // namespace o3sdk::formats
