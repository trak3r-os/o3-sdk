// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/console/shell_bind.h>

namespace o3sdk::platform::mk66::console {

namespace shell = o3sdk::usb::diag;

namespace {

const CommandTable* g_table = nullptr;
const app::Runtime* g_runtime = nullptr;

void sink_write(void* shell, const char* data, std::size_t n) { static_cast<shell::Shell*>(shell)->out().text(data, n); }

void not_here(shell::Shell& sh, const shell::Args&) { sh.out().text("ERR not in this image\n"); }

}  // namespace

app::DiagOut diag_out(shell::Shell& sh) noexcept { return app::DiagOut(app::DiagSink{&sink_write, &sh}); }

namespace {

// A row is at most 8 + 70 characters and a line end (CR LF on the wire): 80 bytes. The pager writes a row only while the output has room for twice that.
constexpr std::size_t kHelpRowRoom = 160;
constexpr unsigned kHelpRowsPerTick = 8;  // bounds one pass of the main loop
std::size_t g_help_next = 0;              // the shell has one stream, so there is one pager

bool help_step(shell::Shell& sh, void*) {
  app::DiagOut out = diag_out(sh);
  for (unsigned n = 0; n < kHelpRowsPerTick && g_table != nullptr && g_help_next < g_table->size() && sh.out().room() >= kHelpRowRoom; ++n) {
    g_table->write_help_row(g_help_next++, out);
  }
  return g_table != nullptr && g_help_next < g_table->size();
}

}  // namespace

void help_command(shell::Shell& sh, const shell::Args&) {
  if (g_table == nullptr) {
    return;
  }
  g_help_next = 0;
  sh.start_stream(&help_step, nullptr, false);
}

void state_command(shell::Shell& sh, const shell::Args&) {
  app::DiagOut out = diag_out(sh);
  if (g_runtime != nullptr) {
    write_state(*g_runtime, out);
  } else {
    out.text("app=down\n");  // (track3r-os: no app yet)
  }
}

void app_command(shell::Shell& sh, const shell::Args& args) {
  app::DiagOut out = diag_out(sh);
  const app::DiagArgs words{args.argc, args.argv};
  if (g_table != nullptr) {
    (void)g_table->run_app(words, out);
  } else {
    CommandTable::write_unknown(words.arg(0), out);
  }
}

void ShellTable::build(const CommandTable& table, const app::Runtime& runtime, const KitHandlers& kit) noexcept {
  g_table = &table;
  g_runtime = &runtime;
  count_ = 0;
  for (std::size_t i = 0; i < kKitCommandCount; ++i) {
    rows_[count_++] = shell::command(kCommandInfo[i].name, kCommandInfo[i].help, kit[i] != nullptr ? kit[i] : &not_here);
  }
  for (std::size_t i = kKitCommandCount; i < table.size() && count_ < kCapacity; ++i) {
    const CommandTable::Row r = table.row(i);
    rows_[count_++] = shell::command(r.name, r.help, &app_command);
  }
}

}  // namespace o3sdk::platform::mk66::console
