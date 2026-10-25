// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The image's firmware update path (OS-006c): the FatFs-backed FirmwareStore on the image's one SD volume, the FwHost that stops the sequencer, mutes the
// outputs and drives the bootloader request pin, and the FwService that the `fw` command of the diag console runs. Declared everywhere, defined only in the image
// (update/src/fw_host_mk66.cpp). Main loop only.

#include <o3sdk/hal/firmware.h>
#include <o3sdk/platform/mk66/update/fw_service.h>

namespace o3sdk::platform::mk66::update::device {

// The one service (created on first use: it reaches the SD objects of storage/device.h).
FwService& service() noexcept;

// The App's firmware port (hal/firmware.h, OS-FWCONFIG) over that same service: one check and one hand-over for the console and the screen.
hal::firmware::FirmwareSink& sink() noexcept;

}  // namespace o3sdk::platform::mk66::update::device
