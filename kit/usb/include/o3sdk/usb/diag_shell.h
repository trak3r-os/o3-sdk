// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/core/action.h>

#include <cstddef>
#include <cstdint>

// The `diag` console's line shell (ADR 0011 item 1, OS-006b): a line editor and a dispatcher. Pure and host tested: bytes in, text
// out through a sink, no hardware and no allocation. The CDC endpoint feeds it and carries its output (usb/cdc.h); the commands
// themselves (cpu, mem, crash, usb, log, input, ...) are written where their data lives, in platform/mk66/usb, and registered here
// as a table.
//
// Commands are actions with string names, not a second registry (ADR 0009 rev 1, ADR 0011): the command `cpu` IS the action
// `diag.cpu`, its identity is core::action_id("diag.cpu") and the table is keyed by that id, so a keymap or a MIDI-learn binding can
// name it like any other action (the app layer's registry can adopt the same table without a rename). The table below is plain
// `static const` data.
//
// Everything runs in the main loop (one context). Nothing blocks: output goes to a ring that drops its oldest bytes when the host
// does not read, a command that has more to say than one pass should becomes a stream (Shell::start_stream) and says a bounded
// amount per tick.
namespace o3sdk::usb::diag {

// The action id of the command `name`: the id of "diag.<name>" (core::action_id), computed at compile time.
constexpr core::ActionId command_id(const char* name) noexcept {
  std::uint32_t h = 2166136261u;  // FNV-1a, core/action.h
  for (const char* p = "diag."; *p != '\0'; ++p) {
    h ^= static_cast<std::uint8_t>(*p);
    h *= 16777619u;
  }
  for (const char* p = name; *p != '\0'; ++p) {
    h ^= static_cast<std::uint8_t>(*p);
    h *= 16777619u;
  }
  return static_cast<core::ActionId>(h);
}
static_assert(command_id("cpu") == core::action_id("diag.cpu"), "a command's id is the action id of its name");

// Where output goes: `write` takes bytes (never blocks, may drop), `flush` says "that is all for now" (the CDC writer publishes
// its partial chunk), `room` (optional) says how many bytes can be written now without losing any, a lower bound. The shell uses it for
// back-pressure: it does not start a command it has no room for the reply of (Shell::feed), and a stream says only what fits.
struct Sink {
  void (*write)(void* context, const char* data, std::size_t n) = nullptr;
  void (*flush)(void* context) = nullptr;
  void* context = nullptr;
  std::size_t (*room)(void* context) = nullptr;
};

// Text out. A terminal wants CR LF: every '\n' is sent as "\r\n", so commands write plain '\n'.
class Output {
 public:
  Output() = default;
  explicit Output(Sink sink) noexcept : sink_(sink) {}

  Output& text(const char* s) noexcept;
  Output& text(const char* s, std::size_t n) noexcept;
  Output& chr(char c) noexcept { return text(&c, 1); }
  Output& dec(std::uint32_t v) noexcept;
  Output& hex(std::uint32_t v, unsigned digits = 8) noexcept;  // "0x" and `digits` upper-case hex digits
  Output& nl() noexcept { return chr('\n'); }
  void flush() noexcept {
    if (sink_.flush != nullptr) {
      sink_.flush(sink_.context);
    }
  }
  std::uint32_t bytes_out() const noexcept { return bytes_; }
  // Bytes that can be written now without loss (a lower bound); unbounded for a sink that does not say.
  std::size_t room() const noexcept { return sink_.room != nullptr ? sink_.room(sink_.context) : static_cast<std::size_t>(-1); }

 private:
  void raw(const char* data, std::size_t n) noexcept {
    if (sink_.write != nullptr && n != 0u) {
      sink_.write(sink_.context, data, n);
      bytes_ += static_cast<std::uint32_t>(n);
    }
  }
  Sink sink_{};
  std::uint32_t bytes_ = 0;
};

// ---- Line editor ------------------------------------------------------------------------------------------------------
// A terminal program sends each key as it is pressed and expects the device to echo it. Handled: printable ASCII (appended while
// the line has room), backspace and delete (erase the last character), Ctrl-U (erase the line), Ctrl-C (cancel the line), CR and
// LF (end the line; CR LF counts once), and escape sequences (the arrow and function keys send ESC [ ... and ESC O ..., which are
// swallowed whole so they never end up in a command). Everything else is ignored. A line longer than kMaxLine is not executed:
// the characters past the limit are dropped with a bell, and at the end of the line the editor reports Overflow.
inline constexpr std::size_t kMaxLine = 79;

class LineEditor {
 public:
  enum class Event : std::uint8_t {
    None,      // nothing to act on
    Line,      // a line is ready: line() until the next feed()
    Cancel,    // Ctrl-C: the line was thrown away
    Overflow,  // the line was longer than kMaxLine and is discarded
  };

  // Feeds one byte from the host; `echo` (may be null) gets what a terminal should show.
  Event feed(std::uint8_t byte, Output* echo) noexcept;
  const char* line() const noexcept { return buf_; }
  std::size_t length() const noexcept { return len_; }
  void reset() noexcept;

 private:
  enum class State : std::uint8_t { Normal, Escape, Csi, Ss3 };
  Event finish(Output* echo, bool carriage_return) noexcept;

  char buf_[kMaxLine + 1] = {};
  std::size_t len_ = 0;
  State state_ = State::Normal;
  bool overflowed_ = false;
  bool last_was_cr_ = false;
  bool consumed_ = false;  // the last feed() returned a finished line: the next one starts a new line
};

// ---- Dispatcher -------------------------------------------------------------------------------------------------------
inline constexpr unsigned kMaxArgs = 8;
struct Args {
  unsigned argc = 0;
  const char* argv[kMaxArgs] = {};  // argv[0] is the command word; the strings live in the line buffer
};

// Splits `line` in place on spaces and tabs. False when there are more than kMaxArgs words.
bool tokenize(char* line, Args& out) noexcept;

class Shell;
using CommandFn = void (*)(Shell& shell, const Args& args);
struct Command {
  const char* name;
  const char* help;  // one line, "usage: what it does"
  core::ActionId id;
  CommandFn run;
};
constexpr Command command(const char* name, const char* help, CommandFn run) noexcept {
  return Command{name, help, command_id(name), run};
}

class Shell {
 public:
  Shell(const Command* table, std::size_t count, Output out) noexcept : table_(table), count_(count), out_(out) {}

  // OS-POWER P1: a gate asked before a command runs. It returns nullptr to let the command run, or the line to print instead (the command is not run): once the unit has begun to power off the platform
  // answers `ERR powering off` to everything that could start anything. No gate (the default): every command runs.
  using GateFn = const char* (*)(const Command& command);
  void set_gate(GateFn gate) noexcept { gate_ = gate; }

  // A terminal opened the port (DTR rose): the banner, then the prompt.
  void begin(const char* banner) noexcept;
  // Bytes from the host. Echoes, edits, and runs every complete line. Returns how many bytes it took: all of them, unless
  //   * a raw receiver (start_raw) is installed and took fewer than it was offered: the rest waits for the next pass;
  //   * a stream that owns the terminal (start_stream with interruptible = false) is running: input waits until it has finished;
  //   * `min_room` is not 0 and the output has less room than that (Output::room): a command's reply must not be lost, so the byte that
  //     could end a line, and everything after it, waits. `min_room` is the largest reply a command makes (the console passes 1.6 KB).
  // The caller keeps the rest and offers it again on its next pass (diag_pump.h does).
  std::size_t feed(const std::uint8_t* bytes, std::size_t n, std::size_t min_room = 0) noexcept;
  // Once per pass of the main loop: runs the active stream, if any.
  void tick() noexcept;

  // A command that keeps talking registers itself here: `fn` is called once per tick until it returns false. It must say a bounded amount
  // per call, and no more than the output has room for (Output::room). An interruptible stream (live events: `input`) also ends when the
  // user presses a key; one that is not (a pager: `log`) keeps the terminal until it is done, and the user's input waits in the receive ring.
  using StreamFn = bool (*)(Shell& shell, void* context);
  void start_stream(StreamFn fn, void* context, bool interruptible = true) noexcept {
    stream_ = fn;
    stream_context_ = context;
    stream_interruptible_ = interruptible;
  }
  bool streaming() const noexcept { return stream_ != nullptr; }
  // Ends a stream, and a raw receiver, without a prompt (the terminal went away).
  void stop_stream() noexcept {
    end_stream();
    end_raw();
  }

  // A command that is sent raw bytes after its line (OS-006c: `fw put` takes a file) installs a receiver: from here on the host's bytes go to `fn`
  // untouched, with no line editor, no echo and no command lookup, until the receiver calls end_raw() (or the terminal goes away: begin() ends it).
  // `fn(shell, bytes, n, context)` returns how many of the n bytes it took (0..n); it takes fewer when it cannot use them yet (no room for its reply,
  // a write it wants to space out), and the pump offers the rest again on its next pass. Bytes left after it ended itself are the next commands.
  using RawFn = std::size_t (*)(Shell& shell, const std::uint8_t* bytes, std::size_t n, void* context);
  void start_raw(RawFn fn, void* context) noexcept {
    raw_ = fn;
    raw_context_ = context;
  }
  void end_raw() noexcept {
    raw_ = nullptr;
    raw_context_ = nullptr;
  }
  bool raw_active() const noexcept { return raw_ != nullptr; }

  Output& out() noexcept { return out_; }
  const Command* commands() const noexcept { return table_; }
  std::size_t command_count() const noexcept { return count_; }
  const Command* find(const char* name) const noexcept;
  void prompt() noexcept;
  std::uint32_t commands_run() const noexcept { return commands_run_; }

 private:
  void run_line(char* line) noexcept;
  void end_stream() noexcept;

  const Command* table_;
  std::size_t count_;
  Output out_;
  LineEditor editor_;
  StreamFn stream_ = nullptr;
  void* stream_context_ = nullptr;
  RawFn raw_ = nullptr;
  void* raw_context_ = nullptr;
  bool stream_interruptible_ = true;
  GateFn gate_ = nullptr;
  std::uint32_t commands_run_ = 0;
};

}  // namespace o3sdk::usb::diag
