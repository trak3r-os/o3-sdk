// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The diagnostics hooks of an app (decision D2 of docs/M2B-PLAN.md): extra commands for the platform's console and the app's part of
// its `state` line. On the Tracker the console is the USB CDC shell (platform/mk66/console); here is all an app sees of it, so kit/app
// depends on no USB code, and the console binds an app's rows into its own table.
//
//   - A command is a name, a help line and a handler `void run(const DiagArgs&, DiagOut&)`. It runs in the main loop, one at a time,
//     never in the render context; it says what it has to in one go (no streams) and must not block. A handler reaches its app through
//     o3sdk_app(). The rules a row must meet (name 2-7 lowercase letters, help up to 70 characters, no kit command's name, none of the
//     update path's words) are the console's, platform/mk66/console/core.h; a row that breaks one is skipped and logged.
//   - A reply is bounded. The console starts a command only when its output has room for 1,600 bytes (usb::diag::kConsoleFeedRoomBytes), counting each '\n' as the two bytes of CR LF
//     it becomes on the wire; that is all it promises. A longer reply is not paged for the app (a handler cannot stream): if the host reads slowly, what does not fit the transmit
//     ring (about 3.9 KB) pushes out the head of what is still unread. So a handler says well under 1,600 bytes: a few lines, not a dump (the console's own `help` and `log` page
//     themselves and are not bound by this).
//   - The state is `key=value` pairs, space separated, the order fixed, a value without a space: `StateWriter` writes them.
//
// Text goes out through `DiagOut`; a '\n' is a line end (the console turns it into what its terminal wants).

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

namespace detail {
// `v` in decimal at the end of `buf`; returns the offset of the first digit (the digits are buf[offset..10)).
inline std::size_t format_dec(std::uint32_t v, char (&buf)[10]) noexcept {
  std::size_t at = sizeof buf;
  do {
    buf[--at] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  return at;
}
}  // namespace detail

// Where a command's text goes: the console gives `write` (never blocks, may drop). Nothing is written without one (a test, a host).
struct DiagSink {
  void (*write)(void* context, const char* data, std::size_t n);
  void* context;
};

class DiagOut {
 public:
  DiagOut() noexcept : sink_{nullptr, nullptr} {}
  explicit DiagOut(DiagSink sink) noexcept : sink_(sink) {}

  DiagOut& text(const char* s) noexcept {
    std::size_t n = 0;
    while (s != nullptr && s[n] != '\0') {
      ++n;
    }
    return text(s, n);
  }
  DiagOut& text(const char* s, std::size_t n) noexcept {
    if (sink_.write != nullptr && s != nullptr && n != 0u) {
      sink_.write(sink_.context, s, n);
      bytes_ += n;
    }
    return *this;
  }
  DiagOut& chr(char c) noexcept { return text(&c, 1); }
  DiagOut& nl() noexcept { return chr('\n'); }
  DiagOut& dec(std::uint32_t v) noexcept {
    char buf[10];
    const std::size_t at = detail::format_dec(v, buf);
    return text(buf + at, sizeof buf - at);
  }
  DiagOut& sdec(std::int32_t v) noexcept {
    if (v < 0) {
      chr('-');
      return dec(0u - static_cast<std::uint32_t>(v));
    }
    return dec(static_cast<std::uint32_t>(v));
  }
  // "0x" and `digits` (1..8) upper-case hex digits.
  DiagOut& hex(std::uint32_t v, unsigned digits = 8) noexcept {
    char out[10] = {'0', 'x'};
    const unsigned n = digits < 1u ? 1u : (digits > 8u ? 8u : digits);
    for (unsigned i = 0; i < n; ++i) {
      const unsigned nibble = (v >> (4u * (n - 1u - i))) & 0xFu;
      out[2u + i] = static_cast<char>(nibble < 10u ? '0' + nibble : 'A' + (nibble - 10u));
    }
    return text(out, 2u + n);
  }
  // Bytes handed to the sink so far (a test or a pager counts replies with it).
  std::size_t bytes() const noexcept { return bytes_; }

 private:
  DiagSink sink_;
  std::size_t bytes_ = 0;
};

// The words of the command line. argv[0] is the command word, as in the shell; the strings live until the handler returns.
struct DiagArgs {
  unsigned argc = 0;
  const char* const* argv = nullptr;

  // Word `i`, or "" past the last one (safe to compare).
  const char* arg(unsigned i) const noexcept { return i < argc && argv != nullptr && argv[i] != nullptr ? argv[i] : ""; }
};

using DiagHandler = void (*)(const DiagArgs& args, DiagOut& out) noexcept;

struct DiagCommand {
  const char* name;  // 2-7 lowercase letters, not a kit command
  const char* help;  // one line, "usage: what it does", at most 70 characters
  DiagHandler run;
};

// `key=value` pairs into a caller's buffer, for App::describe_state. A pair is written whole or not at all: when one does not fit,
// it and every later one are dropped (complete() says so), so the line always parses. Keys are lower case words (`[a-z0-9_]`).
class StateWriter {
 public:
  StateWriter(char* out, std::size_t cap) noexcept : out_(out), cap_(out != nullptr ? cap : 0u) {
    if (cap_ != 0u) {
      out_[0] = '\0';
    }
  }

  StateWriter& num(const char* key, std::uint32_t v) noexcept { return number(key, v, false); }
  StateWriter& sint(const char* key, std::int32_t v) noexcept {
    return v >= 0 ? number(key, static_cast<std::uint32_t>(v), false) : number(key, 0u - static_cast<std::uint32_t>(v), true);
  }
  StateWriter& flag(const char* key, bool v) noexcept {
    if (begin(key, 1u)) {
      out_[len_++] = v ? '1' : '0';
      out_[len_] = '\0';
    }
    return *this;
  }
  // A name or a label: at most `max` characters, a space or other non-printable character becomes '_', anything not ASCII '?'; null
  // or empty is "-".
  StateWriter& str(const char* key, const char* text, std::size_t max = 40) noexcept {
    std::size_t n = 0;
    while (text != nullptr && text[n] != '\0' && n < max) {
      ++n;
    }
    if (n == 0u) {
      text = "-";
      n = 1u;
    }
    if (begin(key, n)) {
      for (std::size_t i = 0; i < n; ++i) {
        const unsigned c = static_cast<unsigned char>(text[i]);
        out_[len_++] = c > 0x20u && c < 0x7Fu ? text[i] : (c <= 0x20u ? '_' : '?');
      }
      out_[len_] = '\0';
    }
    return *this;
  }

  std::size_t size() const noexcept { return len_; }
  bool complete() const noexcept { return !full_; }

 private:
  StateWriter& number(const char* key, std::uint32_t magnitude, bool negative) noexcept {
    char buf[10];
    const std::size_t at = detail::format_dec(magnitude, buf);
    const std::size_t n = sizeof buf - at;
    if (begin(key, n + (negative ? 1u : 0u))) {
      if (negative) {
        out_[len_++] = '-';
      }
      for (std::size_t i = 0; i < n; ++i) {
        out_[len_++] = buf[at + i];
      }
      out_[len_] = '\0';
    }
    return *this;
  }

  // Reserves " key=" and `value_len` characters (and the terminator) and writes the separator and the key; false (and nothing written)
  // when it does not fit.
  bool begin(const char* key, std::size_t value_len) noexcept {
    std::size_t k = 0;
    while (key != nullptr && key[k] != '\0') {
      ++k;
    }
    const std::size_t need = (len_ != 0u ? 1u : 0u) + k + 1u + value_len;
    if (full_ || cap_ == 0u || k == 0u || len_ + need + 1u > cap_) {
      full_ = true;
      return false;
    }
    if (len_ != 0u) {
      out_[len_++] = ' ';
    }
    for (std::size_t i = 0; i < k; ++i) {
      out_[len_++] = key[i];
    }
    out_[len_++] = '=';
    return true;
  }

  char* out_;
  std::size_t cap_;
  std::size_t len_ = 0;
  bool full_ = false;
};

}  // namespace o3sdk::app
