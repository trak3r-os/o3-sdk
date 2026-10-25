// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/diag_log.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstdint>

// `log [count]` (OS-006b): the last lines of the diag log ring. A pager, not a dump: the reply is written as a non-interruptible stream that
// says only what the output has room for, so however many lines are asked for (up to kLogMax) none is lost when the host reads slowly, and a
// pasted burst of `log` commands is served one after the other (the shell holds the input while the pager runs). Pure: the ring and the
// shell are arguments, so the host tests run the real thing against a fake controller.
namespace o3sdk::platform::mk66::console {

inline constexpr std::uint32_t kLogDefault = 20;
inline constexpr std::uint32_t kLogMax = 1024;  // the whole ring (64 KiB of 64-byte records): the boot's first lines are past the last 48 once a project's samples load (OS-DIAG1, INP-8)

// The command: parses the optional count (usage text for a bad one), writes the header and starts the pager. One pager exists at a time (it
// is the shell's one stream).
void run_log_command(o3sdk::usb::diag::Shell& shell, const o3sdk::usb::diag::LogRing& ring, const o3sdk::usb::diag::Args& args) noexcept;

}  // namespace o3sdk::platform::mk66::console
