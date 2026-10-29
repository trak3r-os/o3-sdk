// SPDX-License-Identifier: GPL-3.0-or-later
// The o3-sdk console end to end on the host: the console's pure half (the table with an app's rows, the shell binding, `key` / `pad` / `enc`), the real shell, the real console pump and
// the whole device side of the USB driver against the fake controller, with a host that types lines and reads the replies. The image's console.cpp composes the same objects over the
// board; here a fake app, a fake scanner (the injector's queue) and a few fake handlers stand in for it. `help`, `version`, `state`, an app command that is accepted and one that is
// refused, and `key f1` reaching the app through the injector.
#include <doctest/doctest.h>

#include "usb_rig.h"
#include "usb_test_util.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/diag.h>
#include <o3sdk/app/panel.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/platform/mk66/console/commands.h>
#include <o3sdk/platform/mk66/console/core.h>
#include <o3sdk/platform/mk66/console/inject_commands.h>
#include <o3sdk/platform/mk66/console/shell_bind.h>
#include <o3sdk/platform/mk66/usb/diag_text.h>
#include <o3sdk/usb/diag_pump.h>
#include <o3sdk/usb/diag_shell.h>
#include <o3sdk/usb/identity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3sdk;
using namespace usb_rig;
namespace console = o3sdk::platform::mk66::console;
namespace shell = o3sdk::usb::diag;

namespace {

// ---- The app ------------------------------------------------------------------------------------------------------------------

void handler_ping(const app::DiagArgs& args, app::DiagOut& out) noexcept { out.text("pong ").dec(args.argc).nl(); }
void handler_nop(const app::DiagArgs&, app::DiagOut&) noexcept {}

constexpr app::DiagCommand kRows[] = {
    {"ping", "ping: answer pong", &handler_ping},
    {"flash", "writes the flash", &handler_nop},  // refused: a word of the update path
    {"info", "info: nothing", &handler_nop},
};

class Blank final : public app::Screen {
 public:
  const char* title() const noexcept override { return "MAIN"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

// The rows the app offers: kRows, or a test's own (set before the console is built).
const app::DiagCommand* g_app_rows = kRows;
std::size_t g_app_row_count = sizeof kRows / sizeof kRows[0];

class E2eApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo i{"E2E", "1"};
    return i;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen_); }
  bool input(app::Context&, const app::Input& in) noexcept override {
    if (in.press(app::Button::F1)) {
      ++f1_presses;
    }
    if (in.release(app::Button::F1)) {
      ++f1_releases;
    }
    if (in.pad_press()) {
      last_pad = static_cast<int>(in.pad());
    }
    return false;
  }
  std::size_t diag_commands(const app::DiagCommand*& rows) const noexcept override {
    rows = g_app_rows;
    return g_app_row_count;
  }
  std::size_t describe_state(const app::Context&, char* out, std::size_t cap) const noexcept override {
    app::StateWriter w(out, cap);
    w.num("f1", f1_presses).sint("pad", last_pad);
    return w.size();
  }
  unsigned f1_presses = 0;
  unsigned f1_releases = 0;
  int last_pad = -1;

 private:
  Blank screen_;
};

// ---- The image's side, faked -----------------------------------------------------------------------------------------------------------

std::vector<std::string> g_log;
void log_line(const char* text) { g_log.emplace_back(text); }

class FakePort final : public console::InjectPort {
 public:
  explicit FakePort(platform::host::FakeInput& scanner, const platform::host::FakeClock& clock) : scanner_(scanner), clock_(clock) {}
  bool input_up() noexcept override { return up; }
  bool allowed() noexcept override { return open; }
  bool push(core::InputKind kind, std::uint8_t id, std::int16_t value) noexcept override {
    scanner_.push(core::InputEvent{static_cast<core::Tick>(clock_.uptime_us()), kind, id, value});
    ++pushed;
    return true;
  }
  std::uint32_t now_ms() noexcept override { return static_cast<std::uint32_t>(clock_.uptime_ms()); }
  void log(const char* text) noexcept override { g_log.emplace_back(text); }
  bool up = true;
  bool open = true;
  unsigned pushed = 0;

 private:
  platform::host::FakeInput& scanner_;
  const platform::host::FakeClock& clock_;
};

void version_command(shell::Shell& sh, const shell::Args&) {
  char buf[256];
  const std::size_t n = console::format_version("e2e-test", usb::kVendorId, usb::kProductId, usb::kDeviceBcdStandard, "0123456789ABCDEF", buf, sizeof buf);
  sh.out().text(buf, n);
}

// The whole console over one rig. A new one each test case (the shell holds pointers into the rows).
struct Console {
  explicit Console(Rig& r)
      : rig(r), port(scanner, clock), inject(port) {
    g_log.clear();
    table.assemble(app, &log_line);
    shell::CommandFn kit[console::kKitCommandCount] = {};
    kit[static_cast<unsigned>(console::Cmd::Help)] = &console::help_command;
    kit[static_cast<unsigned>(console::Cmd::State)] = &console::state_command;
    kit[static_cast<unsigned>(console::Cmd::Version)] = &version_command;
    kit[static_cast<unsigned>(console::Cmd::Key)] = &console::key_command;
    kit[static_cast<unsigned>(console::Cmd::Pad)] = &console::pad_command;
    kit[static_cast<unsigned>(console::Cmd::Enc)] = &console::enc_command;
    shell_table.build(table, runtime, kit);
    console::bind_inject(&inject);
    sh = new (sh_storage) shell::Shell(shell_table.rows(), shell_table.size(), shell::Output(shell::Sink{&write, &flush, this, &room}));
    pump = new (pump_storage) shell::ConsolePump(*sh, rig.rx_ring);
    REQUIRE(runtime.start());
  }
  ~Console() {
    console::bind_inject(nullptr);
    pump->~ConsolePump();
    sh->~Shell();
  }

  static void write(void* self, const char* d, std::size_t n) { static_cast<Console*>(self)->rig.writer.write(d, n); }
  static void flush(void* self) { static_cast<Console*>(self)->rig.writer.flush(); }
  static std::size_t room(void* self) { return static_cast<Console*>(self)->rig.writer.room_bytes(); }

  Rig& rig;
  platform::host::FakeDisplay display;
  platform::host::FakeInput scanner;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  E2eApp app;
  app::Platform plat{display, scanner, nullptr, clock, false};
  app::Runtime runtime{app, engine, plat};
  FakePort port;
  console::InjectCommands inject;
  console::CommandTable table;
  console::ShellTable shell_table;
  alignas(shell::Shell) unsigned char sh_storage[sizeof(shell::Shell)];
  alignas(shell::ConsolePump) unsigned char pump_storage[sizeof(shell::ConsolePump)];
  shell::Shell* sh = nullptr;
  shell::ConsolePump* pump = nullptr;
  std::string captured;
  unsigned reads = 4;  // packets the host reads a millisecond (0: it is not reading)
  const std::uint8_t* paste = nullptr;
  std::size_t paste_len = 0;
  std::size_t paste_off = 0;

  // One millisecond of everything: the paste (a packet if the device takes it), the main loop's pass (console, then the runtime as main.cpp does), the start of frame, the host's reads.
  void step() {
    if (paste_off < paste_len) {
      const std::size_t n = paste_len - paste_off < 64u ? paste_len - paste_off : 64u;
      if (rig.host.bulk_out(2, paste + paste_off, static_cast<std::uint32_t>(n)) == Reply::Ack) {
        paste_off += n;
      }
    }
    inject.service();  // console::service(): the releases that are due
    pump->service();
    sh->tick();
    rig.writer.flush();
    runtime.poll();
    clock.advance(1000);
    rig.hw.sof();
    for (unsigned i = 0; i < reads; ++i) {
      std::uint8_t packet[64];
      std::uint32_t len = 0;
      if (rig.host.bulk_in(2, packet, sizeof packet, len) != Reply::Ack) {
        break;
      }
      captured.append(reinterpret_cast<const char*>(packet), len);
    }
  }
  bool idle() const { return paste_off >= paste_len && pump->held_bytes() == 0u && rig.rx_ring.size() == 0u && rig.tx_ring.size() == 0u && rig.writer.pending() == 0u && !sh->streaming(); }
  void run(unsigned ms) {
    for (unsigned i = 0; i < ms; ++i) {
      step();
    }
  }
  // Types `line` and a carriage return, runs until the reply has been read, returns everything the host saw (echo and prompt included).
  std::string say(const std::string& line) {
    captured.clear();
    text = line + "\r";
    paste = reinterpret_cast<const std::uint8_t*>(text.data());
    paste_len = text.size();
    paste_off = 0;
    unsigned quiet = 0;
    for (unsigned i = 0; i < 2000u && quiet < 8u; ++i) {
      step();
      quiet = idle() ? quiet + 1u : 0u;
    }
    return captured;
  }
  std::string text;
};

Console& fresh_console(Rig& rig) {
  alignas(Console) static unsigned char storage[sizeof(Console)];
  static Console* current = nullptr;
  if (current != nullptr) {
    current->~Console();
  }
  current = new (storage) Console(rig);
  return *current;
}

bool has(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("console e2e: help lists the kit's commands, then the app's accepted ones, one line each") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  const std::string reply = c.say("help");
  for (std::size_t i = 0; i < console::kKitCommandCount; ++i) {
    const std::string line = std::string(console::kCommandInfo[i].name);
    CAPTURE(line);
    CHECK(has(reply, (line + std::string(line.size() < 8 ? 8 - line.size() : 0, ' ') + console::kCommandInfo[i].help + "\r\n").c_str()));
  }
  CHECK(has(reply, "ping    ping: answer pong\r\n"));
  CHECK(has(reply, "info    info: nothing\r\n"));
  CHECK_FALSE(has(reply, "flash"));  // the app's row with a word of the update path was refused at assembly
  CHECK(c.table.app_count() == 2u);
  CHECK(c.table.skipped() == 1u);
  REQUIRE(g_log.size() == 1u);
  CHECK(g_log[0] == "app cmd flash skipped: forbidden word");
  CHECK(rig.writer.dropped_chunks() == 0u);
}

TEST_CASE("console e2e: help is a pager: three helps with 16 app rows (about 2.8 KB each) and a host that stops reading lose nothing") {
  static char names[16][3];
  static std::string helps[16];
  static app::DiagCommand rows[16];
  for (unsigned i = 0; i < 16; ++i) {
    names[i][0] = 'x';
    names[i][1] = static_cast<char>('a' + i);
    names[i][2] = '\0';
    helps[i] = std::string(names[i]) + ": " + std::string(64, static_cast<char>('A' + i));  // 70 characters
    rows[i] = app::DiagCommand{names[i], helps[i].c_str(), &handler_nop};
  }
  g_app_rows = rows;
  g_app_row_count = 16;
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  CHECK(c.table.app_count() == 16u);

  const std::string burst = "help\rhelp\rhelp\r";
  c.captured.clear();
  c.paste = reinterpret_cast<const std::uint8_t*>(burst.data());
  c.paste_len = burst.size();
  c.paste_off = 0;
  c.reads = 0;
  c.run(300);  // nobody reads for 300 ms: the transmit ring fills and the pager waits for room
  c.reads = 3;
  unsigned quiet = 0;
  for (unsigned i = 0; i < 4000u && quiet < 8u; ++i) {
    c.step();
    quiet = c.idle() ? quiet + 1u : 0u;
  }
  const auto count = [&](const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = c.captured.find(needle); at != std::string::npos; at = c.captured.find(needle, at + needle.size())) {
      ++n;
    }
    return n;
  };
  CHECK(count("help    list the commands\r\n") == 3u);
  CHECK(count("guard   guard [n <1..255>]: the main-loop guard's window (OS-MAINGUARD)\r\n") == 3u);
  for (unsigned i = 0; i < 16; ++i) {
    CAPTURE(i);
    CHECK(count(std::string(names[i]) + "      " + helps[i] + "\r\n") == 3u);
  }
  CHECK(c.captured.size() > 3u * 1600u);  // each reply is longer than the 1,600 bytes the shell promises
  CHECK(rig.writer.dropped_chunks() == 0u);
  CHECK(rig.dev.counters().cdc_rx_dropped_chunks == 0u);
  g_app_rows = kRows;
  g_app_row_count = sizeof kRows / sizeof kRows[0];
}

TEST_CASE("console e2e: version, with the USB identity") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  const std::string reply = c.say("version");
  CHECK(has(reply, "Track3r o3sdk, build e2e-test\r\n"));
  CHECK(has(reply, "usb 16D0:0E9F rev 0001 serial 0123456789ABCDEF\r\n"));
}

TEST_CASE("console e2e: state is the kit's pairs, then the app's, on one line") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  const std::string reply = c.say("state");
  CHECK(has(reply, "app=E2E screen=MAIN playing=0 tempo=120000 cpu=0 frames=0 sys=0 section=display fw=closed card=none files=idle setup_errors=0 f1=0 pad=-1\r\n"));
}

TEST_CASE("console e2e: an app command runs with its words; an unknown command and a refused app row get the shell's reply") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  CHECK(has(c.say("ping a b"), "pong 3\r\n"));
  CHECK(has(c.say("ping"), "pong 1\r\n"));
  CHECK(has(c.say("flash"), "unknown command: flash (help lists the commands)\r\n"));
  CHECK(has(c.say("nosuch 1"), "unknown command: nosuch (help lists the commands)\r\n"));
  CHECK(has(c.say("peek 0"), "ERR not in this image\r\n"));  // a kit row whose handler this fixture does not bind
  CHECK(c.shell_table.size() == console::kKitCommandCount + 2u);
}

TEST_CASE("console e2e: key f1 reaches the app through the injector, as a press and, 40 ms later, a release") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  const std::string reply = c.say("key f1");
  CHECK(has(reply, "ok key 0\r\n"));
  CHECK(c.app.f1_presses == 1u);  // the runtime polled the scanner's queue on the same pass
  CHECK(c.port.pushed >= 1u);
  c.run(60);  // the tap's release is due after kInjectTapMs
  CHECK(c.app.f1_releases == 1u);
  CHECK(c.port.pushed == 2u);
  REQUIRE(!g_log.empty());
  CHECK(g_log.back() == "inject key 0 tap");
  CHECK(has(c.say("state"), "f1=1 pad=-1\r\n"));
}

TEST_CASE("console e2e: pad and enc, hold and down, and the refusals") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  CHECK(has(c.say("pad 12 down"), "ok pad 12\r\n"));
  CHECK(c.app.last_pad == 12);
  CHECK(c.runtime.context().panel().pad_held(12));
  CHECK(has(c.say("pad 12 up"), "ok pad 12\r\n"));
  CHECK_FALSE(c.runtime.context().panel().pad_held(12));
  CHECK(has(c.say("enc -3"), "ok encoder -3\r\n"));
  CHECK(has(c.say("key f2 hold 100"), "ok key 1\r\n"));
  CHECK(c.runtime.context().panel().held(app::Button::F2));
  c.run(150);
  CHECK_FALSE(c.runtime.context().panel().held(app::Button::F2));

  CHECK(has(c.say("key nosuchkey"), "ERR unknown button (a name like f1, play, shift, or 0..32)\r\n"));
  CHECK(has(c.say("key"), "usage: key <button> [tap|down|up|hold <ms>]"));
  CHECK(has(c.say("pad 99"), "ERR pad is 0..47\r\n"));
  CHECK(has(c.say("enc 0"), "ERR n is -64..64, not 0\r\n"));

  c.port.open = false;  // an fw update is armed or running
  const unsigned pushed = c.port.pushed;
  CHECK(has(c.say("key f1"), "ERR refused: an fw update is armed or running\r\n"));
  CHECK(c.port.pushed == pushed);
  c.port.open = true;
  c.port.up = false;  // the app has not started
  CHECK(has(c.say("key f1"), "ERR no input: the app has not started\r\n"));
}

// OS-POWER P1 (D2): the shell's gate over the real table. The image's gate is console.cpp's power_gate (power_gate_text asked with power::device::locked()); here the flag is the test's.
namespace {
bool g_shutting_down = false;
const char* shutdown_gate(const shell::Command& cmd) noexcept { return console::power_gate_text(g_shutting_down, cmd.name); }
}  // namespace

TEST_CASE("console e2e: once the shutdown has begun the shell answers ERR powering off to everything but the reads, the app's rows included, and does not run them") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  c.sh->set_gate(&shutdown_gate);
  g_shutting_down = false;
  CHECK(has(c.say("key f1"), "ok key 0\r\n"));  // open: it runs
  CHECK(c.app.f1_presses == 1u);
  CHECK(has(c.say("ping"), "pong 1\r\n"));

  g_shutting_down = true;
  const unsigned pushed = c.port.pushed;
  const unsigned run = c.sh->commands_run();
  const std::string refused_key = c.say("key f1");
  CHECK(has(refused_key, "ERR powering off\r\n"));
  CHECK_FALSE(has(refused_key, "ok key"));
  CHECK(c.port.pushed == pushed);  // not run: nothing reached the injector
  CHECK(c.app.f1_presses == 1u);
  CHECK(has(c.say("ping"), "ERR powering off\r\n"));  // an app row
  CHECK(has(c.say("pad 3"), "ERR powering off\r\n"));
  CHECK(has(c.say("enc 1"), "ERR powering off\r\n"));
  CHECK(has(c.say("peek 0"), "ERR powering off\r\n"));  // a kit row that is not a read (this fixture binds no handler, so the gate's answer is the proof)
  CHECK(c.sh->commands_run() == run);
  // The reads still answer.
  CHECK(has(c.say("version"), "Track3r o3sdk, build e2e-test\r\n"));
  const std::string state = c.say("state");
  CHECK(has(state, "app=E2E screen=MAIN playing=0 tempo=120000 cpu=0 frames="));  // (the frame count has moved on: the runtime polled all the while)
  CHECK(has(state, " sys=0 section=display fw=closed card=none files=idle setup_errors=0 f1=1 pad=-1\r\n"));
  CHECK(has(c.say("power"), "ERR not in this image\r\n"));  // a read: the gate lets it through (to the fixture's unbound row)
  const std::string help = c.say("help");
  CHECK(has(help, "power   power [press|release|pin|off [dry]]: the Power switch and the shutdown\r\n"));
  CHECK(has(help, "ping    ping: answer pong\r\n"));  // help lists the whole table, the app's rows too
  CHECK(c.sh->commands_run() == run + 4u);
  g_shutting_down = false;
  CHECK(has(c.say("key f1"), "ok key 0\r\n"));  // the gate is the platform's flag: open again, it runs again
}

TEST_CASE("console e2e: a key typed by name or by code is the same button, a release waits while injection is refused") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Console& c = fresh_console(rig);
  CHECK(has(c.say("key 0 down"), "ok key 0\r\n"));
  CHECK(c.runtime.context().panel().held(app::Button::F1));
  CHECK(has(c.say("key F1 up"), "ok key 0\r\n"));
  CHECK_FALSE(c.runtime.context().panel().held(app::Button::F1));

  // A tap whose release comes due while injection is refused: the release goes out when the refusal ends, so no key stays held.
  CHECK(has(c.say("key play hold 50"), "ok key 23\r\n"));
  CHECK(c.runtime.context().panel().held(app::Button::Play));
  c.port.open = false;
  c.run(200);
  CHECK(c.runtime.context().panel().held(app::Button::Play));
  c.port.open = true;
  c.run(5);
  CHECK_FALSE(c.runtime.context().panel().held(app::Button::Play));
}
