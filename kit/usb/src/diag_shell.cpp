// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/usb/diag_shell.h>

namespace o3sdk::usb::diag {
namespace {

constexpr char kPrompt[] = "o3> ";
constexpr std::uint8_t kBackspace = 0x08;
constexpr std::uint8_t kDelete = 0x7F;
constexpr std::uint8_t kCtrlC = 0x03;
constexpr std::uint8_t kCtrlU = 0x15;
constexpr std::uint8_t kEscape = 0x1B;
constexpr std::uint8_t kBell = 0x07;

bool is_space(char c) { return c == ' ' || c == '\t'; }

bool same(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

}  // namespace

// ---- Output ---------------------------------------------------------------------------------------------------------------

Output& Output::text(const char* s, std::size_t n) noexcept {
  std::size_t start = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (s[i] == '\n') {
      raw(s + start, i - start);
      raw("\r\n", 2);
      start = i + 1u;
    }
  }
  raw(s + start, n - start);
  return *this;
}

Output& Output::text(const char* s) noexcept {
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return text(s, n);
}

Output& Output::dec(std::uint32_t v) noexcept {
  char tmp[10];
  std::size_t n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  char out[10];
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = tmp[n - 1u - i];
  }
  raw(out, n);
  return *this;
}

Output& Output::hex(std::uint32_t v, unsigned digits) noexcept {
  char out[10] = {'0', 'x'};
  if (digits > 8u) {
    digits = 8u;
  }
  for (unsigned i = 0; i < digits; ++i) {
    const std::uint32_t nibble = (v >> (4u * (digits - 1u - i))) & 0xFu;
    out[2u + i] = static_cast<char>(nibble < 10u ? '0' + nibble : 'A' + (nibble - 10u));
  }
  raw(out, 2u + digits);
  return *this;
}

// ---- Line editor ------------------------------------------------------------------------------------------------------

void LineEditor::reset() noexcept {
  len_ = 0;
  buf_[0] = '\0';
  state_ = State::Normal;
  overflowed_ = false;
  last_was_cr_ = false;
  consumed_ = false;
}

LineEditor::Event LineEditor::finish(Output* echo, bool carriage_return) noexcept {
  last_was_cr_ = carriage_return;
  if (echo != nullptr) {
    echo->nl();
  }
  consumed_ = true;
  buf_[len_] = '\0';
  if (overflowed_) {
    overflowed_ = false;
    len_ = 0;
    buf_[0] = '\0';
    return Event::Overflow;
  }
  return Event::Line;
}

LineEditor::Event LineEditor::feed(std::uint8_t byte, Output* echo) noexcept {
  if (consumed_) {  // the previous call handed out a finished line: start a new one
    len_ = 0;
    buf_[0] = '\0';
    consumed_ = false;
  }
  // An escape sequence is swallowed whole: ESC [ parameters... final (0x40..0x7E), or ESC O final, or ESC and one more byte.
  switch (state_) {
    case State::Escape:
      state_ = byte == '[' ? State::Csi : (byte == 'O' ? State::Ss3 : State::Normal);
      return Event::None;
    case State::Csi:
      if (byte >= 0x40u && byte <= 0x7Eu) {
        state_ = State::Normal;
      }
      return Event::None;
    case State::Ss3:
      state_ = State::Normal;
      return Event::None;
    case State::Normal:
      break;
  }

  const bool after_cr = last_was_cr_;
  last_was_cr_ = false;
  if (byte == '\r') {
    return finish(echo, true);
  }
  if (byte == '\n') {
    if (after_cr) {
      return Event::None;  // the LF of a CR LF pair: the line was already ended
    }
    return finish(echo, false);
  }
  if (byte == kEscape) {
    state_ = State::Escape;
    return Event::None;
  }
  if (byte == kBackspace || byte == kDelete) {
    if (len_ != 0u) {
      --len_;
      buf_[len_] = '\0';
      if (echo != nullptr) {
        echo->text("\b \b");
      }
    }
    return Event::None;
  }
  if (byte == kCtrlU) {
    if (echo != nullptr) {
      for (std::size_t i = 0; i < len_; ++i) {
        echo->text("\b \b");
      }
    }
    len_ = 0;
    buf_[0] = '\0';
    overflowed_ = false;
    return Event::None;
  }
  if (byte == kCtrlC) {
    len_ = 0;
    buf_[0] = '\0';
    overflowed_ = false;
    if (echo != nullptr) {
      echo->text("^C\n");
    }
    return Event::Cancel;
  }
  if (byte >= 0x20u && byte <= 0x7Eu) {
    if (len_ < kMaxLine) {
      buf_[len_++] = static_cast<char>(byte);
      buf_[len_] = '\0';
      if (echo != nullptr) {
        echo->chr(static_cast<char>(byte));
      }
    } else {
      overflowed_ = true;
      if (echo != nullptr) {
        echo->chr(static_cast<char>(kBell));
      }
    }
  }
  return Event::None;  // everything else (tab, other controls, bytes of a multi-byte character) is ignored
}

// ---- Dispatcher -------------------------------------------------------------------------------------------------------

bool tokenize(char* line, Args& out) noexcept {
  out.argc = 0;
  char* p = line;
  while (*p != '\0') {
    while (is_space(*p)) {
      *p++ = '\0';
    }
    if (*p == '\0') {
      break;
    }
    if (out.argc == kMaxArgs) {
      return false;
    }
    out.argv[out.argc++] = p;
    while (*p != '\0' && !is_space(*p)) {
      ++p;
    }
  }
  return true;
}

const Command* Shell::find(const char* name) const noexcept {
  const core::ActionId id = command_id(name);
  for (std::size_t i = 0; i < count_; ++i) {
    if (table_[i].id == id && same(table_[i].name, name)) {  // the id picks, the name confirms (a hash is not a proof)
      return &table_[i];
    }
  }
  return nullptr;
}

void Shell::prompt() noexcept { out_.text(kPrompt); }

void Shell::begin(const char* banner) noexcept {
  editor_.reset();
  end_stream();
  end_raw();
  out_.nl().text(banner).nl();
  prompt();
  out_.flush();
}

void Shell::end_stream() noexcept {
  stream_ = nullptr;
  stream_context_ = nullptr;
  stream_interruptible_ = true;
}

void Shell::run_line(char* line) noexcept {
  Args args;
  if (!tokenize(line, args)) {
    out_.text("too many arguments\n");
  } else if (args.argc != 0u) {
    const Command* cmd = find(args.argv[0]);
    if (cmd == nullptr) {
      out_.text("unknown command: ").text(args.argv[0]).text(" (help lists the commands)\n");
    } else if (const char* refused = gate_ != nullptr ? gate_(*cmd) : nullptr; refused != nullptr) {
      out_.text(refused);  // (OS-POWER P1: the command is not run)
    } else {
      ++commands_run_;
      cmd->run(*this, args);
    }
  }
  if (stream_ == nullptr) {
    prompt();
  }
}

std::size_t Shell::feed(const std::uint8_t* bytes, std::size_t n, std::size_t min_room) noexcept {
  std::size_t taken = 0;
  while (taken < n) {
    if (raw_ != nullptr) {  // a command owns the byte stream (start_raw): no editor, no echo
      const std::size_t used = raw_(*this, bytes + taken, n - taken, raw_context_);
      taken += used;
      if (raw_ != nullptr && taken < n) {
        break;  // it wants the rest later
      }
      continue;
    }
    if (stream_ != nullptr) {
      if (!stream_interruptible_) {
        break;  // a pager owns the terminal: what the user types waits for it
      }
      // Any key stops an interruptible stream (a terminal sends bytes only when a human acts).
      end_stream();
      editor_.reset();
      if (bytes[taken] == '\r') {
        (void)editor_.feed('\r', nullptr);  // so that the LF of a CR LF pair is not taken for an empty line
      }
      out_.text("\n");
      prompt();
      ++taken;
      continue;
    }
    if (min_room != 0u && out_.room() < min_room) {
      break;  // no room for a reply: the byte could end a line, so it waits (the host is NAKed once the receive ring fills)
    }
    const std::uint8_t byte = bytes[taken];
    ++taken;
    switch (editor_.feed(byte, &out_)) {
      case LineEditor::Event::Line: {
        char line[kMaxLine + 1];
        const std::size_t len = editor_.length();
        for (std::size_t k = 0; k < len; ++k) {
          line[k] = editor_.line()[k];
        }
        line[len] = '\0';
        run_line(line);
        break;
      }
      case LineEditor::Event::Overflow:
        out_.text("line too long (limit 79 characters), not run\n");
        prompt();
        break;
      case LineEditor::Event::Cancel:
        prompt();
        break;
      case LineEditor::Event::None:
        break;
    }
  }
  out_.flush();
  return taken;
}

void Shell::tick() noexcept {
  if (stream_ == nullptr) {
    return;
  }
  if (!stream_(*this, stream_context_)) {
    end_stream();
    prompt();
  }
  out_.flush();
}

}  // namespace o3sdk::usb::diag
