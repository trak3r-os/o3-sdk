// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The console's table: the kit's commands (commands.h) followed by the app's (App::diag_commands), and the rules the app's rows must
// meet. Pure and host tested; it knows nothing of the USB library. The image's glue (console.cpp) builds the shell's table from
// row(): the kit rows bound to their handlers by Cmd, every app row bound to one dispatcher that calls run_app(). Text goes out through
// app::DiagOut, which the glue lays over the shell's output.
//
// The rules of an app's row (docs/M2B-PLAN.md section 4, "App commands"):
//   - the name is 2 to 7 lowercase letters and is not a kit command's name or an earlier app row's;
//   - the help is a non-empty line of at most 70 characters, and the handler exists;
//   - neither the name nor a part of it is one of the words of the update path (`update flash boot bootloader fwinfo upload dfu
//     program erase write`, either case): a LINT on the NAMES, not a guard. `fw` is the only command of the kit that writes the card or reaches the bootloader, and the
//     filter keeps an app's commands from advertising that they do (or from being mistaken for it in `help`). It knows nothing of what a handler does: a handler named
//     `burn` or `wipe` passes, and nothing here stops it (what really keeps the card and the bootloader safe is the writer's path guard and
//     tools/check_writers.py, which allow only the update path to name a FatFs write call or the bootloader pin). Substring matching also refuses innocent names that
//     contain a word: `reboot`, `rewrite`, `programs`, `bootx`. The list is the update path's vocabulary, not a proof of anything; a false positive is fixed by choosing
//     another name (`restart`, `resave`);
//   - at most 16 rows count.
// A row that breaks a rule is skipped, logged as "app cmd <name> skipped: <why>", and the rest are kept in order.

#include <o3sdk/app/app.h>
#include <o3sdk/app/diag.h>
#include <o3sdk/platform/mk66/console/commands.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {
class Runtime;
}

namespace o3sdk::platform::mk66::console {

inline constexpr std::size_t kMaxAppCommands = 16;
inline constexpr std::size_t kNameMin = 2;
inline constexpr std::size_t kNameMax = 7;
inline constexpr std::size_t kHelpMax = 70;
// The longest `state` line the command prints; Runtime::state_line cuts at a pair when it would be longer.
inline constexpr std::size_t kStateBuffer = 384;

// The words of the update path that no app command may carry.
inline constexpr const char* kForbiddenWords[] = {"update", "flash", "boot", "bootloader", "fwinfo", "upload", "dfu", "program", "erase", "write"};

enum class Reject : std::uint8_t {
  None,       // the row is fine
  Null,       // no name, no help or no handler
  Forbidden,  // the name has a word of the update path in it
  Name,       // not 2-7 lowercase letters
  Help,       // empty or longer than 70 characters
  Kit,        // the name of a kit command
  Duplicate,  // the name of an earlier app row
  Full,       // the 17th row and the ones after it
};

// The short reason `log` shows ("forbidden word"), at most 18 characters.
const char* reject_text(Reject why) noexcept;

// A row against the rules that need no other row: Null, Forbidden, Name, Help, Kit.
Reject check_app_row(const app::DiagCommand& row) noexcept;

// The kit command named `name`, or false.
bool find_kit(const char* name, Cmd& which) noexcept;

class CommandTable {
 public:
  // The log (console::log in the image): one short line per call, at most 55 characters. May be null.
  using LogFn = void (*)(const char* text);

  struct Row {
    const char* name;
    const char* help;
    bool app;  // false: a kit row (the glue binds its handler by Cmd), true: an app row (the dispatcher)
  };

  // Builds the table from the app's rows (null with a count of 0 is none). Once, at attach(); a second call starts over.
  void assemble(const app::DiagCommand* rows, std::size_t count, LogFn log) noexcept;
  // The same, asking the app (App::diag_commands).
  void assemble(const app::App& app, LogFn log) noexcept;

  std::size_t kit_count() const noexcept { return kKitCommandCount; }
  std::size_t app_count() const noexcept { return app_count_; }
  std::size_t size() const noexcept { return kKitCommandCount + app_count_; }
  // Rows the app offered that were skipped.
  std::size_t skipped() const noexcept { return skipped_; }

  // Row `i` of size(): the kit's in Cmd order, then the app's that were accepted, in the app's order. Past the end: an empty row.
  Row row(std::size_t i) const noexcept;
  // The accepted app row named `name`, or nullptr.
  const app::DiagCommand* find_app(const char* name) const noexcept;

  // The dispatcher every app row is bound to: runs the app row named by args.argv[0]. A name that is not an app row gets the shell's
  // unknown-command reply and false.
  bool run_app(const app::DiagArgs& args, app::DiagOut& out) const noexcept;

  // `help`: every row, the name padded to 8 columns, then the help text, a line each. Row `i` of size() is write_help_row(); the shell's `help` writes a few rows a pass
  // (shell_bind.h) because the whole table, with 16 app rows of 70 characters, is about 2.8 KB, more than a reply the shell can promise (1,600 bytes with CR LF).
  void write_help(app::DiagOut& out) const noexcept;
  void write_help_row(std::size_t i, app::DiagOut& out) const noexcept;

  // "unknown command: <name> (help lists the commands)", as the shell says it.
  static void write_unknown(const char* name, app::DiagOut& out) noexcept;

 private:
  app::DiagCommand app_[kMaxAppCommands] = {};
  std::size_t app_count_ = 0;
  std::size_t skipped_ = 0;
};

// `state`: Runtime::state_line (the kit's pairs, then the app's) and a newline.
void write_state(const app::Runtime& runtime, app::DiagOut& out) noexcept;

// The code of the front-panel button `name` (app::find_button's lower-case names, in either case) or a decimal 0..32; -1 for neither. What `key` takes (track3r-os
// app::button_code does the same over its own table).
int button_code(const char* name) noexcept;

}  // namespace o3sdk::platform::mk66::console
