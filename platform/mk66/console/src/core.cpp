// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/console/core.h>

#include <o3sdk/app/panel.h>
#include <o3sdk/app/runtime.h>

namespace o3sdk::platform::mk66::console {

namespace {

constexpr std::size_t kLogMax = 55;  // the log ring's line (usb::diag::kLogTextMax)
constexpr std::size_t kLogNameMax = 12;
constexpr std::size_t kHelpColumn = 8;  // track3r-os's `help`: the names pad to this

bool same(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// `text` has `word` in it, in either case. The forbidden-word filter (core.h) is a lint on a command's NAME: a substring test, so `reboot` and `rewrite` are refused as
// well as `write`, and a synonym (`burn`, `wipe`, `save`) is not. It says nothing about what a handler does.
bool contains_word(const char* text, const char* word) noexcept {
  for (const char* start = text; *start != '\0'; ++start) {
    const char* t = start;
    const char* w = word;
    while (*w != '\0' && *t != '\0' && lower(*t) == *w) {
      ++t;
      ++w;
    }
    if (*w == '\0') {
      return true;
    }
  }
  return false;
}

bool valid_name(const char* name) noexcept {
  std::size_t n = 0;
  for (; name[n] != '\0'; ++n) {
    if (name[n] < 'a' || name[n] > 'z') {
      return false;
    }
  }
  return n >= kNameMin && n <= kNameMax;
}

std::size_t length(const char* s) noexcept {
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

// "app cmd <name> skipped: <why>", the name cut and made printable.
void log_skip(CommandTable::LogFn log, const char* name, Reject why) noexcept {
  if (log == nullptr) {
    return;
  }
  char line[kLogMax + 1];
  std::size_t n = 0;
  const auto put = [&](char c) {
    if (n < kLogMax) {
      line[n++] = c;
    }
  };
  for (const char* p = "app cmd "; *p != '\0'; ++p) {
    put(*p);
  }
  if (name == nullptr) {
    put('?');
  }
  for (std::size_t i = 0; name != nullptr && name[i] != '\0' && i < kLogNameMax; ++i) {
    put(name[i] > ' ' && name[i] < 0x7F ? name[i] : '?');
  }
  for (const char* p = " skipped: "; *p != '\0'; ++p) {
    put(*p);
  }
  for (const char* p = reject_text(why); *p != '\0'; ++p) {
    put(*p);
  }
  line[n] = '\0';
  log(line);
}

}  // namespace

const char* reject_text(Reject why) noexcept {
  switch (why) {
    case Reject::None: return "ok";
    case Reject::Null: return "incomplete row";
    case Reject::Forbidden: return "forbidden word";
    case Reject::Name: return "bad name";
    case Reject::Help: return "bad help";
    case Reject::Kit: return "a kit command";
    case Reject::Duplicate: return "duplicate";
    case Reject::Full: return "more than 16";
  }
  return "?";
}

bool find_kit(const char* name, Cmd& which) noexcept {
  if (name == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < kKitCommandCount; ++i) {
    if (same(kCommandInfo[i].name, name)) {
      which = static_cast<Cmd>(i);
      return true;
    }
  }
  return false;
}

Reject check_app_row(const app::DiagCommand& row) noexcept {
  if (row.name == nullptr || row.help == nullptr || row.run == nullptr) {
    return Reject::Null;
  }
  for (const char* word : kForbiddenWords) {
    if (contains_word(row.name, word)) {
      return Reject::Forbidden;
    }
  }
  if (!valid_name(row.name)) {
    return Reject::Name;
  }
  const std::size_t help = length(row.help);
  if (help == 0u || help > kHelpMax) {
    return Reject::Help;
  }
  Cmd which = Cmd::Help;
  if (find_kit(row.name, which)) {
    return Reject::Kit;
  }
  return Reject::None;
}

void CommandTable::assemble(const app::DiagCommand* rows, std::size_t count, LogFn log) noexcept {
  app_count_ = 0;
  skipped_ = 0;
  for (std::size_t i = 0; rows != nullptr && i < count; ++i) {
    Reject why = check_app_row(rows[i]);
    if (why == Reject::None && find_app(rows[i].name) != nullptr) {
      why = Reject::Duplicate;
    }
    if (why == Reject::None && app_count_ == kMaxAppCommands) {
      why = Reject::Full;
    }
    if (why != Reject::None) {
      ++skipped_;
      log_skip(log, rows[i].name, why);
      continue;
    }
    app_[app_count_++] = rows[i];
  }
}

void CommandTable::assemble(const app::App& app, LogFn log) noexcept {
  const app::DiagCommand* rows = nullptr;
  const std::size_t count = app.diag_commands(rows);
  assemble(rows, count, log);
}

CommandTable::Row CommandTable::row(std::size_t i) const noexcept {
  if (i < kKitCommandCount) {
    return Row{kCommandInfo[i].name, kCommandInfo[i].help, false};
  }
  if (i - kKitCommandCount < app_count_) {
    const app::DiagCommand& r = app_[i - kKitCommandCount];
    return Row{r.name, r.help, true};
  }
  return Row{"", "", false};
}

const app::DiagCommand* CommandTable::find_app(const char* name) const noexcept {
  for (std::size_t i = 0; name != nullptr && i < app_count_; ++i) {
    if (same(app_[i].name, name)) {
      return &app_[i];
    }
  }
  return nullptr;
}

bool CommandTable::run_app(const app::DiagArgs& args, app::DiagOut& out) const noexcept {
  const char* name = args.arg(0);
  const app::DiagCommand* row = find_app(name);
  if (row == nullptr) {
    write_unknown(name, out);
    return false;
  }
  row->run(args, out);
  return true;
}

void CommandTable::write_help_row(std::size_t i, app::DiagOut& out) const noexcept {
  const Row r = row(i);
  out.text(r.name);
  for (std::size_t len = length(r.name); len < kHelpColumn; ++len) {
    out.chr(' ');  // the help texts line up
  }
  out.text(r.help).nl();
}

void CommandTable::write_help(app::DiagOut& out) const noexcept {
  for (std::size_t i = 0; i < size(); ++i) {
    write_help_row(i, out);
  }
}

void CommandTable::write_unknown(const char* name, app::DiagOut& out) noexcept {
  out.text("unknown command: ").text(name).text(" (help lists the commands)\n");
}

const char* power_gate_text(bool shutting_down, const char* name) noexcept {
  if (!shutting_down) {
    return nullptr;
  }
  Cmd which = Cmd::Help;
  return find_kit(name, which) && power_read(which) ? nullptr : kPoweringOff;
}

void write_state(const app::Runtime& runtime, app::DiagOut& out) noexcept {
  char buf[kStateBuffer];
  const std::size_t n = runtime.state_line(buf, sizeof buf);
  out.text(buf, n).nl();
}

int button_code(const char* name) noexcept {
  if (name == nullptr || name[0] == '\0') {
    return -1;
  }
  if (name[0] >= '0' && name[0] <= '9') {
    unsigned v = 0;
    for (const char* p = name; *p != '\0'; ++p) {
      if (*p < '0' || *p > '9' || v > 99u) {
        return -1;
      }
      v = v * 10u + static_cast<unsigned>(*p - '0');
    }
    return v < app::kButtonCount ? static_cast<int>(v) : -1;
  }
  app::Button b = app::Button::F1;
  return app::find_button(name, b) ? static_cast<int>(b) : -1;
}

}  // namespace o3sdk::platform::mk66::console
