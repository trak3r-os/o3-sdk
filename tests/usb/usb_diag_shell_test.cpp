// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: the `diag` line shell (usb/diag_shell.h): the line editor, the tokenizer, the dispatcher keyed by action id, streams,
// the CR LF translation of the output. Pure: bytes in, text out. Runs on the host and on QEMU (qemu_usb_device).
#include <doctest/doctest.h>

#include "usb_test_util.h"

#include <o3sdk/core/action.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstdint>
#include <cstring>

using namespace o3sdk::usb::diag;

namespace {

// Collects what the shell writes.
struct Capture {
  char text[2048];
  std::size_t len = 0;
  unsigned flushes = 0;
  std::size_t room_bytes = static_cast<std::size_t>(-1);  // what the sink says it can take: every byte written uses some up
  static void write(void* self, const char* data, std::size_t n) {
    Capture* c = static_cast<Capture*>(self);
    for (std::size_t i = 0; i < n && c->len + 1 < sizeof c->text; ++i) {
      c->text[c->len++] = data[i];
    }
    c->text[c->len] = '\0';
    if (c->room_bytes != static_cast<std::size_t>(-1)) {
      c->room_bytes = n < c->room_bytes ? c->room_bytes - n : 0u;
    }
  }
  static void flush(void* self) { ++static_cast<Capture*>(self)->flushes; }
  static std::size_t room(void* self) { return static_cast<Capture*>(self)->room_bytes; }
  Sink sink() { return Sink{&Capture::write, &Capture::flush, this, &Capture::room}; }
  void clear() { len = 0; text[0] = '\0'; }
  bool has(const char* s) const { return std::strstr(text, s) != nullptr; }
};

struct Seen {
  unsigned calls = 0;
  unsigned argc = 0;
  char arg1[32] = {};
  char arg2[32] = {};
};
Seen g_seen;

void cmd_echo(Shell& sh, const Args& a) {
  ++g_seen.calls;
  g_seen.argc = a.argc;
  g_seen.arg1[0] = g_seen.arg2[0] = '\0';
  if (a.argc > 1) {
    std::strncpy(g_seen.arg1, a.argv[1], sizeof g_seen.arg1 - 1);
  }
  if (a.argc > 2) {
    std::strncpy(g_seen.arg2, a.argv[2], sizeof g_seen.arg2 - 1);
  }
  sh.out().text("echo ok\n");
}
void cmd_help(Shell& sh, const Args&) {
  for (std::size_t i = 0; i < sh.command_count(); ++i) {
    sh.out().text(sh.commands()[i].name).text(" - ").text(sh.commands()[i].help).nl();
  }
}
struct StreamState {
  unsigned ticks = 0;
  unsigned stop_after = 3;
};
StreamState g_stream;
bool stream_fn(Shell& sh, void* ctx) {
  StreamState* s = static_cast<StreamState*>(ctx);
  ++s->ticks;
  sh.out().text("tick ").dec(s->ticks).nl();
  return s->ticks < s->stop_after;
}
void cmd_stream(Shell& sh, const Args&) {
  g_stream = StreamState{};
  sh.start_stream(&stream_fn, &g_stream);
}

const Command kTable[] = {
    command("echo", "echo a b: records its arguments", &cmd_echo),
    command("help", "list the commands", &cmd_help),
    command("stream", "say something every tick", &cmd_stream),
};

void type(Shell& sh, const char* s) { sh.feed(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)); }

}  // namespace

TEST_CASE("usb diag: a command is the action named diag.<name>") {
  static_assert(command_id("cpu") == o3sdk::core::action_id("diag.cpu"), "");
  static_assert(command_id("usb") == o3sdk::core::action_id("diag.usb"), "");
  for (const Command& c : kTable) {
    char name[32] = "diag.";
    std::strcat(name, c.name);
    CHECK(c.id == o3sdk::core::action_id(name));
  }
  // Distinct commands have distinct ids (a collision would make find() depend on the name compare alone).
  for (std::size_t i = 0; i < 3; ++i) {
    for (std::size_t j = i + 1; j < 3; ++j) {
      CHECK(kTable[i].id != kTable[j].id);
    }
  }
}

TEST_CASE("usb diag: output turns every newline into CR LF") {
  Capture cap;
  Output out(cap.sink());
  out.text("a\nb\n\nc");
  CHECK(std::strcmp(cap.text, "a\r\nb\r\n\r\nc") == 0);
  const std::uint32_t before = out.bytes_out();
  cap.clear();
  out.dec(0).chr(' ').dec(4294967295u).chr(' ').hex(0xBEEF, 4).chr(' ').hex(0x1234ABCDu).chr(' ').hex(0xF, 1);
  CHECK(std::strcmp(cap.text, "0 4294967295 0xBEEF 0x1234ABCD 0xF") == 0);
  CHECK(out.bytes_out() - before == cap.len);
}

TEST_CASE("usb diag: line editor, typing, backspace, kill, cancel") {
  LineEditor ed;
  Capture cap;
  Output echo(cap.sink());
  const auto feed = [&](const char* s) {
    LineEditor::Event e = LineEditor::Event::None;
    for (const char* p = s; *p != '\0'; ++p) {
      e = ed.feed(static_cast<std::uint8_t>(*p), &echo);
    }
    return e;
  };
  CHECK(feed("help") == LineEditor::Event::None);
  CHECK(std::strcmp(cap.text, "help") == 0);  // echoed as typed
  CHECK(feed("\r") == LineEditor::Event::Line);
  CHECK(std::strcmp(ed.line(), "help") == 0);
  CHECK(feed("\n") == LineEditor::Event::None);  // the LF of a CR LF pair
  // Backspace and delete erase; on an empty line they do nothing.
  cap.clear();
  feed("abx");
  ed.feed(0x08, &echo);
  ed.feed('c', &echo);
  ed.feed(0x7F, &echo);
  ed.feed(0x7F, &echo);
  ed.feed(0x7F, &echo);  // the line is empty now
  ed.feed(0x08, &echo);  // and this one has nothing to erase
  CHECK(std::strcmp(cap.text, "abx\b \bc\b \b\b \b\b \b") == 0);
  CHECK(feed("ls") == LineEditor::Event::None);
  CHECK(feed("\n") == LineEditor::Event::Line);  // a bare LF ends a line too
  CHECK(std::strcmp(ed.line(), "ls") == 0);
  // Ctrl-U erases the whole line, Ctrl-C cancels it.
  feed("garbage");
  cap.clear();
  ed.feed(0x15, &echo);
  CHECK(std::strcmp(cap.text, "\b \b\b \b\b \b\b \b\b \b\b \b\b \b") == 0);
  CHECK(feed("ok\r") == LineEditor::Event::Line);
  CHECK(std::strcmp(ed.line(), "ok") == 0);
  feed("abc");
  CHECK(ed.feed(0x03, &echo) == LineEditor::Event::Cancel);
  CHECK(feed("x\r") == LineEditor::Event::Line);
  CHECK(std::strcmp(ed.line(), "x") == 0);
  // An empty line is a line.
  CHECK(feed("\r") == LineEditor::Event::Line);
  CHECK(ed.length() == 0u);
}

TEST_CASE("usb diag: escape sequences and control bytes never reach a command") {
  LineEditor ed;
  const auto feed = [&](const char* s) {
    LineEditor::Event e = LineEditor::Event::None;
    for (const char* p = s; *p != '\0'; ++p) {
      e = ed.feed(static_cast<std::uint8_t>(*p), nullptr);
    }
    return e;
  };
  feed("a");
  feed("\x1b[A");        // up arrow
  feed("b");
  feed("\x1b[1;5C");     // ctrl-right: parameters, then the final byte
  feed("c");
  feed("\x1bOP");        // F1
  feed("d");
  feed("\x1bz");         // ESC and one byte
  feed("e");
  feed("\t\x01\x02\x04\x1f");
  feed("\xc3\xa9");      // a UTF-8 character
  CHECK(feed("\r") == LineEditor::Event::Line);
  CHECK(std::strcmp(ed.line(), "abcde") == 0);
}

TEST_CASE("usb diag: a line over the limit is dropped with a bell and not run") {
  LineEditor ed;
  Capture cap;
  Output echo(cap.sink());
  for (std::size_t i = 0; i < kMaxLine; ++i) {
    CHECK(ed.feed('a', &echo) == LineEditor::Event::None);
  }
  CHECK(ed.length() == kMaxLine);
  cap.clear();
  CHECK(ed.feed('b', &echo) == LineEditor::Event::None);
  CHECK(std::strcmp(cap.text, "\a") == 0);  // refused, with a bell
  CHECK(ed.length() == kMaxLine);
  CHECK(ed.feed('\r', &echo) == LineEditor::Event::Overflow);
  CHECK(ed.length() == 0u);
  // A line that overflowed is never run, even if it is backspaced back under the limit: no truncated command.
  for (std::size_t i = 0; i < kMaxLine + 3u; ++i) {
    ed.feed('a', nullptr);
  }
  for (std::size_t i = 0; i < 5u; ++i) {
    ed.feed(0x08, nullptr);
  }
  ed.feed('x', nullptr);
  CHECK(ed.feed('\r', nullptr) == LineEditor::Event::Overflow);
  // And a following line is fine.
  ed.feed('o', nullptr);
  CHECK(ed.feed('\r', nullptr) == LineEditor::Event::Line);
  CHECK(std::strcmp(ed.line(), "o") == 0);
}

TEST_CASE("usb diag: tokenizer") {
  char line[] = "  echo   one\ttwo  ";
  Args a;
  CHECK(tokenize(line, a));
  USB_REQUIRE(a.argc == 3u);
  CHECK(std::strcmp(a.argv[0], "echo") == 0);
  CHECK(std::strcmp(a.argv[1], "one") == 0);
  CHECK(std::strcmp(a.argv[2], "two") == 0);
  char empty[] = "   ";
  CHECK(tokenize(empty, a));
  CHECK(a.argc == 0u);
  char many[] = "a b c d e f g h";
  CHECK(tokenize(many, a));
  CHECK(a.argc == 8u);
  char toomany[] = "a b c d e f g h i";
  CHECK_FALSE(tokenize(toomany, a));
}

TEST_CASE("usb diag: dispatch, unknown command, arguments, prompt") {
  Capture cap;
  Shell sh(kTable, 3, Output(cap.sink()));
  g_seen = Seen{};
  sh.begin("Track3r diag");
  CHECK(cap.has("Track3r diag\r\n"));
  CHECK(cap.has("o3> "));
  cap.clear();
  type(sh, "echo alpha beta\r");
  CHECK(g_seen.calls == 1u);
  CHECK(g_seen.argc == 3u);
  CHECK(std::strcmp(g_seen.arg1, "alpha") == 0);
  CHECK(std::strcmp(g_seen.arg2, "beta") == 0);
  CHECK(cap.has("echo alpha beta\r\n"));  // the echo of the typing
  CHECK(cap.has("echo ok\r\n"));
  CHECK(cap.has("o3> "));
  cap.clear();
  type(sh, "nope\r");
  CHECK(cap.has("unknown command: nope"));
  cap.clear();
  type(sh, "\r");  // an empty line: a fresh prompt and nothing else
  CHECK(std::strcmp(cap.text, "\r\no3> ") == 0);
  cap.clear();
  type(sh, "e\r");  // no prefix matching: a command is its whole name
  CHECK(cap.has("unknown command: e"));
  cap.clear();
  type(sh, "echo 1 2 3 4 5 6 7 8 9\r");
  CHECK(cap.has("too many arguments"));
  CHECK(g_seen.calls == 1u);
  cap.clear();
  type(sh, "help\r");
  CHECK(cap.has("echo - echo a b: records its arguments"));
  CHECK(cap.has("stream - say something every tick"));
  CHECK(sh.commands_run() == 2u);
  CHECK(sh.find("echo") == &kTable[0]);
  CHECK(sh.find("ech") == nullptr);
  CHECK(sh.find("echo2") == nullptr);
  // Output is flushed at the end of each batch of input.
  const unsigned before = cap.flushes;
  type(sh, "echo\r");
  CHECK(cap.flushes > before);
  // CR LF from a terminal that sends both: the LF is not an empty command.
  cap.clear();
  type(sh, "echo\r\n");
  CHECK(sh.commands_run() == 4u);
  // An over-long line is refused loudly.
  cap.clear();
  char big[120];
  std::memset(big, 'a', sizeof big - 1);
  big[sizeof big - 1] = '\0';
  type(sh, big);
  type(sh, "\r");
  CHECK(cap.has("line too long"));
  CHECK(sh.commands_run() == 4u);
}

TEST_CASE("usb diag: a stream talks once per tick and any key stops it") {
  Capture cap;
  Shell sh(kTable, 3, Output(cap.sink()));
  sh.begin("x");
  cap.clear();
  type(sh, "stream\r");
  CHECK(sh.streaming());
  CHECK_FALSE(cap.has("o3> "));  // no prompt while it runs
  sh.tick();
  sh.tick();
  CHECK(cap.has("tick 1\r\n"));
  CHECK(cap.has("tick 2\r\n"));
  sh.tick();  // the third tick returns false: finished, prompt
  CHECK(cap.has("tick 3\r\n"));
  CHECK_FALSE(sh.streaming());
  CHECK(cap.has("o3> "));
  // A key stops a running stream: the key is consumed, the prompt returns, the rest of the line is not run.
  cap.clear();
  type(sh, "stream\r");
  sh.tick();
  CHECK(sh.streaming());
  type(sh, "q");
  CHECK_FALSE(sh.streaming());
  CHECK(cap.has("o3> "));
  const unsigned calls = sh.commands_run();
  sh.tick();  // nothing running: nothing said
  cap.clear();
  sh.tick();
  CHECK(cap.len == 0u);
  // Enter as the stopping key: its LF companion must not be taken for an empty command.
  type(sh, "stream\r");
  sh.tick();
  cap.clear();
  type(sh, "\r\n");
  CHECK_FALSE(sh.streaming());
  CHECK(std::strcmp(cap.text, "\r\no3> ") == 0);
  CHECK(sh.commands_run() == calls + 1u);
}

// ---- back-pressure (OS-006b review fix) ------------------------------------------------------------------------------------------

namespace {
struct PagerState {
  unsigned lines = 0;
};
PagerState g_pager;
bool pager_fn(Shell& sh, void* ctx) {
  PagerState* p = static_cast<PagerState*>(ctx);
  sh.out().text("page line ").dec(++p->lines).nl();
  return p->lines < 3;
}
void cmd_page(Shell& sh, const Args&) {
  g_pager = PagerState{};
  sh.start_stream(&pager_fn, &g_pager, false);  // not interruptible: it keeps the terminal
}
const Command kPageTable[] = {
    command("echo", "echo", &cmd_echo),
    command("page", "a pager", &cmd_page),
    command("stream", "an interruptible stream", &cmd_stream),
};
}  // namespace

TEST_CASE("usb diag: input waits while the output has no room for a reply, and no byte is lost or run twice") {
  Capture cap;
  Shell sh(kPageTable, 3, Output(cap.sink()));
  g_seen = Seen{};
  const std::uint8_t* line = reinterpret_cast<const std::uint8_t*>("echo a\r");
  const std::size_t n = 7;
  cap.room_bytes = 100;  // less than the 1,600 a reply may need
  CHECK(sh.feed(line, n, 1600) == 0u);  // nothing taken, nothing echoed, nothing run
  CHECK(cap.len == 0u);
  CHECK(g_seen.calls == 0u);
  // Room appears: the whole line goes in.
  cap.room_bytes = 100000;
  CHECK(sh.feed(line, n, 1600) == n);
  CHECK(g_seen.calls == 1u);
  CHECK(cap.has("echo ok"));
  // Room runs out part way (each echoed byte uses one): the shell stops at the byte that would have needed more, the caller offers the rest later.
  cap.clear();
  cap.room_bytes = 1602;
  g_seen = Seen{};
  const std::size_t taken = sh.feed(line, n, 1600);
  CHECK(taken == 3u);  // 'e', 'c', 'h' echoed (room 1602 -> 1599), then the check before 'o' fails
  CHECK(g_seen.calls == 0u);
  cap.room_bytes = 100000;
  CHECK(sh.feed(line + taken, n - taken, 1600) == n - taken);
  CHECK(g_seen.calls == 1u);  // the line ran once, with all its characters
  CHECK(std::strcmp(g_seen.arg1, "a") == 0);
  // min_room 0 never holds back (the unchanged behaviour for callers that do not gate).
  cap.room_bytes = 0;
  CHECK(sh.feed(line, n) == n);
}

TEST_CASE("usb diag: a pager keeps the terminal until it has finished, an interruptible stream does not") {
  Capture cap;
  Shell sh(kPageTable, 3, Output(cap.sink()));
  const char* two = "page\recho x\r";
  const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(two);
  g_seen = Seen{};
  // "page" ends with its Enter, which starts the pager; the rest of the paste waits.
  const std::size_t taken = sh.feed(bytes, std::strlen(two));
  CHECK(taken == 5u);  // "page\r"
  CHECK(sh.streaming());
  CHECK(g_seen.calls == 0u);
  CHECK(sh.feed(bytes + taken, std::strlen(two) - taken) == 0u);  // still held while it runs
  sh.tick();
  sh.tick();
  CHECK_FALSE(cap.has("o3> \r\npage line 3"));
  sh.tick();  // the third line ends it: the prompt returns
  CHECK_FALSE(sh.streaming());
  CHECK(cap.has("page line 1\r\npage line 2\r\npage line 3\r\no3> "));
  CHECK(sh.feed(bytes + taken, std::strlen(two) - taken) == std::strlen(two) - taken);
  CHECK(g_seen.calls == 1u);  // the held line ran once the pager was done
  // An interruptible stream still ends on a key, and that key is consumed.
  cap.clear();
  const std::uint8_t go[] = {'s', 't', 'r', 'e', 'a', 'm', '\r'};
  CHECK(sh.feed(go, sizeof go) == sizeof go);
  CHECK(sh.streaming());
  const std::uint8_t key = 'q';
  CHECK(sh.feed(&key, 1) == 1u);
  CHECK_FALSE(sh.streaming());
  // begin() (a terminal that reopened) drops a pager that nobody is reading.
  CHECK(sh.feed(reinterpret_cast<const std::uint8_t*>("page\r"), 5) == 5u);
  CHECK(sh.streaming());
  sh.begin("again");
  CHECK_FALSE(sh.streaming());
  CHECK(sh.feed(reinterpret_cast<const std::uint8_t*>("echo y\r"), 7) == 7u);  // the terminal is the user's again
}

// OS-POWER P1 (D2): after the one-way commit the platform answers `ERR powering off` to everything that could start anything. The gate is asked before a command runs: nullptr lets it run, a line
// is printed instead and the command is not called; no gate, no change.
namespace {
bool g_gate_closed = false;
const char* gate_for_test(const Command& cmd) noexcept {
  if (!g_gate_closed || std::strcmp(cmd.name, "help") == 0) {
    return nullptr;
  }
  return "ERR powering off\n";
}
}  // namespace

TEST_CASE("usb diag: a gate refuses a command before it runs and prints its line, the others pass, and no gate changes nothing") {
  Capture cap;
  Shell sh(kTable, 3, Output(cap.sink()));
  sh.begin("x");
  cap.clear();
  g_seen = Seen{};
  type(sh, "echo a\r");
  CHECK(g_seen.calls == 1u);  // no gate: it runs
  sh.set_gate(&gate_for_test);
  cap.clear();
  type(sh, "echo b\r");
  CHECK(g_seen.calls == 2u);  // the gate is open
  g_gate_closed = true;
  cap.clear();
  const unsigned run = sh.commands_run();
  type(sh, "echo c\r");
  CHECK(g_seen.calls == 2u);  // refused: not called
  CHECK(cap.has("ERR powering off"));
  CHECK(sh.commands_run() == run);
  cap.clear();
  type(sh, "help\r");  // a read that the gate lets through
  CHECK(cap.has("echo - echo a b"));
  CHECK(sh.commands_run() == run + 1u);
  CHECK_FALSE(sh.streaming());
  g_gate_closed = false;
}
