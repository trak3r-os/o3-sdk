// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The console's table on the shell (kit/usb diag_shell.h): the pure half of the image's composition (console.cpp is the other, with the hardware). The shell's table is one array
// built at attach(): the kit's rows, each with the handler the image binds to its Cmd, then the app's accepted rows, every one bound to one dispatcher that looks the row up by the
// command word. `help` and `state` need nothing but the table and the runtime, so they are here, and so is the bridge from the shell's output to the app's DiagOut. Host tested
// together with the shell, the pump and the device (tests/mk66_console_e2e_test.cpp).
//
// One console per image: build() remembers the table and the runtime for the handlers below (a shell handler is a plain function with no context).

#include <o3sdk/app/diag.h>
#include <o3sdk/platform/mk66/console/core.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstddef>

namespace o3sdk::platform::mk66::console {

// An app::DiagOut that writes through the shell's output (the shell turns '\n' into CR LF). `shell` must outlive it.
app::DiagOut diag_out(usb::diag::Shell& shell) noexcept;

// `help`: CommandTable::write_help_row, a few rows a pass as a stream that keeps the terminal until it is done (like `log`): the whole table is more than the 1,600 bytes a reply may
// have, and a pager says only what the output has room for, so nothing is dropped however slowly the host reads. `state`: write_state over the runtime build() was given.
void help_command(usb::diag::Shell& shell, const usb::diag::Args& args);
void state_command(usb::diag::Shell& shell, const usb::diag::Args& args);
// The dispatcher of every app row: CommandTable::run_app with the shell's words.
void app_command(usb::diag::Shell& shell, const usb::diag::Args& args);

using KitHandlers = usb::diag::CommandFn[kKitCommandCount];

class ShellTable {
 public:
  static constexpr std::size_t kCapacity = kKitCommandCount + kMaxAppCommands;

  // The kit's rows (names and help from commands.h, handler `kit[Cmd]`; a null handler is a row that answers "ERR not in this image"), then the table's app rows bound to
  // app_command. `table` and `runtime` must outlive the shell.
  void build(const CommandTable& table, const app::Runtime& runtime, const KitHandlers& kit) noexcept;

  const usb::diag::Command* rows() const noexcept { return rows_; }
  std::size_t size() const noexcept { return count_; }

 private:
  usb::diag::Command rows_[kCapacity] = {};
  std::size_t count_ = 0;
};

}  // namespace o3sdk::platform::mk66::console
