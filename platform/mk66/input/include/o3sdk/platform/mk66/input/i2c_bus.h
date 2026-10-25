// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The input drivers name the I2C seam as their own (`input::I2cBus`); it is the platform's (i2c_bus.h), shared with the codec.

#include <o3sdk/platform/mk66/i2c_bus.h>

namespace o3sdk::platform::mk66::input {
using mk66::I2cBus;
using mk66::I2cResult;
}  // namespace o3sdk::platform::mk66::input
