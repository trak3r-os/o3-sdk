// SPDX-License-Identifier: GPL-3.0-or-later
// The console's table: the kit's rows, an app's rows and the rules for them, dispatch, the unknown-command reply, the help layout and
// `state` (the kit's pairs, then the app's).
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/diag.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/platform/mk66/console/commands.h>
#include <o3sdk/platform/mk66/console/core.h>
#include <o3sdk/platform/mk66/console/mem_report.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3sdk;
namespace console = o3sdk::platform::mk66::console;
using console::CommandTable;
using console::Reject;

namespace {

struct Capture {
  std::string text;
};
void capture_write(void* context, const char* data, std::size_t n) { static_cast<Capture*>(context)->text.append(data, n); }

std::vector<std::string> g_log;
void log_line(const char* text) { g_log.emplace_back(text); }

std::string g_ran;  // what the last handler saw
void handler_a(const app::DiagArgs& args, app::DiagOut& out) noexcept {
  g_ran = "a";
  out.text("A:").dec(args.argc);
  for (unsigned i = 1; i < args.argc; ++i) {
    out.chr(' ').text(args.arg(i));
  }
  out.nl();
}
void handler_b(const app::DiagArgs&, app::DiagOut& out) noexcept {
  g_ran = "b";
  out.text("B\n");
}

constexpr app::DiagCommand row(const char* name, const char* help = "does something", app::DiagHandler run = &handler_a) {
  return app::DiagCommand{name, help, run};
}

const char* const kKitNames[] = {"help", "version", "stage", "cpu", "mem",  "crash", "usb",  "log",  "input", "peek",  "poke",  "rot",   "sd",
                                 "ls",   "reset",   "fw",    "key", "pad",  "enc",   "state", "shot", "leds",  "cache", "power", "guard", "midi",  "files", "radio", "mon",   "rec"};

std::string help_of(const CommandTable& t) {
  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  t.write_help(out);
  return c.text;
}

// Table of `rows`, with the log captured.
struct Built {
  CommandTable table;
  explicit Built(const std::vector<app::DiagCommand>& rows) {
    g_log.clear();
    table.assemble(rows.data(), rows.size(), &log_line);
  }
};

class Blank final : public app::Screen {
 public:
  const char* title() const noexcept override { return "MAIN"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

constexpr app::DiagCommand kAppRows[] = {{"ping", "ping: answer", &handler_a}, {"zap", "zap: another", &handler_b}};

class FakeApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo i{"FAKE", "1"};
    return i;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen_); }
  std::size_t diag_commands(const app::DiagCommand*& rows) const noexcept override {
    rows = kAppRows;
    return sizeof kAppRows / sizeof kAppRows[0];
  }
  std::size_t describe_state(const app::Context&, char* out, std::size_t cap) const noexcept override {
    app::StateWriter w(out, cap);
    w.num("steps", 16).str("pattern", "A 1");
    return w.size();
  }

 private:
  Blank screen_;
};

class BareApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo i{"BARE", "1"};
    return i;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen_); }

 private:
  Blank screen_;
};

}  // namespace

TEST_CASE("kit commands: the names are the ones in scope, in table order, and each row obeys the app rules") {
  static_assert(console::kKitCommandCount == 30, "the kit's commands");
  REQUIRE(console::kKitCommandCount == sizeof kKitNames / sizeof kKitNames[0]);
  for (std::size_t i = 0; i < console::kKitCommandCount; ++i) {
    const console::CommandInfo& c = console::kCommandInfo[i];
    CAPTURE(c.name);
    CHECK(std::string(c.name) == kKitNames[i]);
    CHECK(std::strlen(c.name) >= console::kNameMin);
    CHECK(std::strlen(c.name) <= console::kNameMax);
    for (const char* p = c.name; *p != '\0'; ++p) {
      CHECK((*p >= 'a' && *p <= 'z'));
    }
    CHECK(std::strlen(c.help) > 0u);
    CHECK(std::strlen(c.help) <= console::kHelpMax);
    for (std::size_t j = 0; j < i; ++j) {
      CHECK(std::string(console::kCommandInfo[j].name) != c.name);
    }
    // `fw` is the only writer: no other kit command is named like the update path.
    for (const char* word : console::kForbiddenWords) {
      CHECK(std::string(c.name).find(word) == std::string::npos);
    }
  }
  CHECK(std::string(console::info(console::Cmd::State).name) == "state");
  CHECK(std::string(console::info(console::Cmd::Fw).name) == "fw");
  CHECK(std::string(console::info(console::Cmd::Guard).name) == "guard");
  CHECK(std::string(console::info(console::Cmd::Power).name) == "power");
  CHECK(std::string(console::info(console::Cmd::Power).help) == "power [press|release|pin|off [dry]]: the Power switch and the shutdown");
  CHECK(static_cast<unsigned>(console::Cmd::Power) == static_cast<unsigned>(console::Cmd::Cache) + 1u);  // track3r-os's relative order: Cache, Power, then Guard
}

// OS-POWER P1 (D2): after the one-way commit the shell runs only the reads that diagnose the shutdown (commands.h kPowerReads); the pure part of the gate is core.h's power_gate_text.
TEST_CASE("power: the reads that stay open during the shutdown are these nine, and nothing that could start anything is among them") {
  const console::Cmd reads[] = {console::Cmd::Help, console::Cmd::Version, console::Cmd::Stage, console::Cmd::Log,  console::Cmd::Crash,
                                console::Cmd::Power, console::Cmd::State,   console::Cmd::Cpu,   console::Cmd::Mem};
  std::uint64_t expected = 0;
  for (const console::Cmd c : reads) {
    expected |= std::uint64_t{1} << static_cast<unsigned>(c);
  }
  CHECK(console::kPowerReads == expected);
  CHECK(__builtin_popcountll(console::kPowerReads) == 9);
  unsigned open = 0;
  for (std::size_t i = 0; i < console::kKitCommandCount; ++i) {
    const auto c = static_cast<console::Cmd>(i);
    CAPTURE(console::info(c).name);
    bool listed = false;
    for (const console::Cmd r : reads) {
      listed = listed || r == c;
    }
    CHECK(console::power_read(c) == listed);
    open += console::power_read(c) ? 1u : 0u;
  }
  CHECK(open == 9u);
  // The ones that write something, move the keys, restart or reach the card or the bootloader are refused.
  for (const console::Cmd c : {console::Cmd::Fw, console::Cmd::Reset, console::Cmd::Key, console::Cmd::Pad, console::Cmd::Enc, console::Cmd::Poke, console::Cmd::Rot, console::Cmd::Sd,
                               console::Cmd::Ls, console::Cmd::Usb, console::Cmd::Cache, console::Cmd::Guard, console::Cmd::Shot, console::Cmd::Input, console::Cmd::Peek, console::Cmd::Leds, console::Cmd::Midi}) {
    CAPTURE(console::info(c).name);
    CHECK_FALSE(console::power_read(c));
  }
}

TEST_CASE("power: the gate lets everything run until the shutdown begins, then only the reads, and refuses an app row") {
  // Not shutting down: no name is refused, whatever it is.
  for (std::size_t i = 0; i < console::kKitCommandCount; ++i) {
    CHECK(console::power_gate_text(false, console::kCommandInfo[i].name) == nullptr);
  }
  CHECK(console::power_gate_text(false, "ping") == nullptr);
  CHECK(console::power_gate_text(false, nullptr) == nullptr);
  // Shutting down: the nine reads run, every other kit command gets the line.
  for (std::size_t i = 0; i < console::kKitCommandCount; ++i) {
    const auto c = static_cast<console::Cmd>(i);
    CAPTURE(console::info(c).name);
    const char* answer = console::power_gate_text(true, console::info(c).name);
    if (console::power_read(c)) {
      CHECK(answer == nullptr);
    } else {
      REQUIRE(answer != nullptr);
      CHECK(std::string(answer) == "ERR powering off\n");
    }
  }
  // An app row (a name no kit command carries: the table refuses those), an empty name, a null one and a different case are refused: the shell matches names exactly.
  for (const char* name : {"ping", "", "HELP", "helpx", "hel"}) {
    CAPTURE(name);
    const char* answer = console::power_gate_text(true, name);
    REQUIRE(answer != nullptr);
    CHECK(std::string(answer) == "ERR powering off\n");
  }
  CHECK(console::power_gate_text(true, nullptr) == console::kPoweringOff);
}

TEST_CASE("kit commands: find_kit") {
  console::Cmd which = console::Cmd::Help;
  CHECK(console::find_kit("enc", which));
  CHECK(which == console::Cmd::Enc);
  CHECK(console::find_kit("help", which));
  CHECK(which == console::Cmd::Help);
  CHECK_FALSE(console::find_kit("encx", which));
  CHECK_FALSE(console::find_kit("", which));
  CHECK_FALSE(console::find_kit(nullptr, which));
  CHECK_FALSE(console::find_kit("ENC", which));
}

TEST_CASE("table: with no app rows it is the kit's, in Cmd order") {
  CommandTable t;
  t.assemble(nullptr, 0, &log_line);
  CHECK(t.size() == console::kKitCommandCount);
  CHECK(t.app_count() == 0u);
  CHECK(t.skipped() == 0u);
  for (std::size_t i = 0; i < t.size(); ++i) {
    const CommandTable::Row r = t.row(i);
    CHECK(std::string(r.name) == kKitNames[i]);
    CHECK_FALSE(r.app);
  }
  const CommandTable::Row past = t.row(t.size());
  CHECK(std::string(past.name).empty());
  CHECK(std::string(past.help).empty());
}

TEST_CASE("table: the app's rows follow the kit's, in the app's order") {
  Built b({row("alpha"), row("beta"), row("gamma")});
  CHECK(b.table.app_count() == 3u);
  CHECK(b.table.size() == console::kKitCommandCount + 3u);
  CHECK(std::string(b.table.row(console::kKitCommandCount).name) == "alpha");
  CHECK(b.table.row(console::kKitCommandCount).app);
  CHECK(std::string(b.table.row(console::kKitCommandCount + 2u).name) == "gamma");
  CHECK(std::string(b.table.row(console::kKitCommandCount + 1u).help) == "does something");
  CHECK_FALSE(b.table.row(0).app);
  CHECK(g_log.empty());
  CHECK(b.table.find_app("beta") != nullptr);
  CHECK(b.table.find_app("delta") == nullptr);
  CHECK(b.table.find_app("help") == nullptr);  // kit rows are not app rows
  CHECK(b.table.find_app(nullptr) == nullptr);
}

TEST_CASE("rules: the name is 2 to 7 lowercase letters") {
  CHECK(console::check_app_row(row("ab")) == Reject::None);
  CHECK(console::check_app_row(row("abcdefg")) == Reject::None);
  CHECK(console::check_app_row(row("z")) == Reject::Name);
  CHECK(console::check_app_row(row("abcdefgh")) == Reject::Name);
  CHECK(console::check_app_row(row("")) == Reject::Name);
  CHECK(console::check_app_row(row("Ping")) == Reject::Name);
  CHECK(console::check_app_row(row("pin g")) == Reject::Name);
  CHECK(console::check_app_row(row("p1ng")) == Reject::Name);
  CHECK(console::check_app_row(row("ping-x")) == Reject::Name);
  CHECK(console::check_app_row(row("pi_ng")) == Reject::Name);
  CHECK(console::check_app_row(row("ping\n")) == Reject::Name);
}

TEST_CASE("rules: the help is a line of 1 to 70 characters, and a row needs all three parts") {
  const std::string h70(70, 'h');
  const std::string h71(71, 'h');
  CHECK(console::check_app_row(row("ping", h70.c_str())) == Reject::None);
  CHECK(console::check_app_row(row("ping", h71.c_str())) == Reject::Help);
  CHECK(console::check_app_row(row("ping", "")) == Reject::Help);
  CHECK(console::check_app_row(app::DiagCommand{nullptr, "x", &handler_a}) == Reject::Null);
  CHECK(console::check_app_row(app::DiagCommand{"ping", nullptr, &handler_a}) == Reject::Null);
  CHECK(console::check_app_row(app::DiagCommand{"ping", "x", nullptr}) == Reject::Null);
}

TEST_CASE("rules: a kit command's name is taken, every one of them") {
  for (const char* name : kKitNames) {
    CAPTURE(name);
    // (a one-letter-short variant is free)
    CHECK(console::check_app_row(row(name)) == Reject::Kit);
  }
  CHECK(console::check_app_row(row("helpx")) == Reject::None);
  CHECK(console::check_app_row(row("tate")) == Reject::None);
}

TEST_CASE("rules: no word of the update path, alone or inside a name, in either case") {
  for (const char* word : console::kForbiddenWords) {
    CAPTURE(word);
    CHECK(console::check_app_row(row(word)) == Reject::Forbidden);
    const std::string embedded = std::string("x") + word;
    CHECK(console::check_app_row(row(embedded.c_str())) == Reject::Forbidden);
    std::string upper = word;
    for (char& c : upper) {
      c = static_cast<char>(c - 'a' + 'A');
    }
    CHECK(console::check_app_row(row(upper.c_str())) == Reject::Forbidden);
  }
  CHECK(console::check_app_row(row("rewrite")) == Reject::Forbidden);
  CHECK(console::check_app_row(row("reboot")) == Reject::Forbidden);
  CHECK(console::check_app_row(row("mdfu")) == Reject::Forbidden);
  CHECK(console::check_app_row(row("fwflash")) == Reject::Forbidden);
  CHECK(console::check_app_row(row("fw")) == Reject::Kit);  // (a kit command, not a forbidden word)
  CHECK(console::check_app_row(row("tempo")) == Reject::None);
  CHECK(console::check_app_row(row("steps")) == Reject::None);
}

TEST_CASE("table: a bad row is skipped and logged, the others stay in order") {
  const std::string long_help(71, 'x');
  Built b({row("alpha"), row("flash"), row("Bad"), row("beta", long_help.c_str()), row("state"), row("gamma"), row("up", "x", nullptr),
           row("alpha"), row("omega")});
  CHECK(b.table.app_count() == 3u);
  CHECK(b.table.skipped() == 6u);
  CHECK(std::string(b.table.row(console::kKitCommandCount).name) == "alpha");
  CHECK(std::string(b.table.row(console::kKitCommandCount + 1u).name) == "gamma");
  CHECK(std::string(b.table.row(console::kKitCommandCount + 2u).name) == "omega");
  REQUIRE(g_log.size() == 6u);
  CHECK(g_log[0] == "app cmd flash skipped: forbidden word");
  CHECK(g_log[1] == "app cmd Bad skipped: bad name");
  CHECK(g_log[2] == "app cmd beta skipped: bad help");
  CHECK(g_log[3] == "app cmd state skipped: a kit command");
  CHECK(g_log[4] == "app cmd up skipped: incomplete row");
  CHECK(g_log[5] == "app cmd alpha skipped: duplicate");
}

TEST_CASE("table: the first of two rows with one name is kept") {
  Built b({row("same", "first", &handler_a), row("same", "second", &handler_b)});
  CHECK(b.table.app_count() == 1u);
  CHECK(std::string(b.table.row(console::kKitCommandCount).help) == "first");
  g_ran.clear();
  const char* const words[] = {"same"};
  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  CHECK(b.table.run_app(app::DiagArgs{1, words}, out));
  CHECK(g_ran == "a");
}

TEST_CASE("table: at most 16 app rows; the rest are skipped and logged") {
  std::vector<std::string> names;
  for (char a = 'a'; a < 'a' + 20; ++a) {
    names.push_back(std::string("x") + a);
  }
  std::vector<app::DiagCommand> rows;
  for (const std::string& n : names) {
    rows.push_back(row(n.c_str()));
  }
  Built b(rows);
  CHECK(b.table.app_count() == console::kMaxAppCommands);
  CHECK(b.table.skipped() == 4u);
  CHECK(std::string(b.table.row(console::kKitCommandCount + 15u).name) == "xp");
  CHECK(std::string(b.table.row(console::kKitCommandCount + 16u).name).empty());
  REQUIRE(g_log.size() == 4u);
  CHECK(g_log[0] == "app cmd xq skipped: more than 16");
  CHECK(g_log[3] == "app cmd xt skipped: more than 16");
}

TEST_CASE("table: a long or odd name is cut and made printable in the log, and every line fits the log ring") {
  Built b({row("bootloader"), app::DiagCommand{nullptr, "x", &handler_a}, row("abc\x01\x7f""defghijklmnopqrstuvwxyz"), row("a\xC3\xA9")});
  REQUIRE(g_log.size() == 4u);
  CHECK(g_log[0] == "app cmd bootloader skipped: forbidden word");
  CHECK(g_log[1] == "app cmd ? skipped: incomplete row");
  CHECK(g_log[2] == "app cmd abc??defghij skipped: bad name");
  CHECK(g_log[3] == "app cmd a?? skipped: bad name");
  for (const std::string& line : g_log) {
    CHECK(line.size() <= 55u);
  }
  // The longest line there can be.
  g_log.clear();
  CommandTable t;
  const std::string name(40, 'a');
  const app::DiagCommand rows[] = {row(name.c_str())};
  t.assemble(rows, 1, &log_line);
  REQUIRE(g_log.size() == 1u);
  CHECK(g_log[0].size() <= 55u);
  // No log function: nothing happens.
  t.assemble(rows, 1, nullptr);
  CHECK(t.skipped() == 1u);
}

TEST_CASE("table: assembling again starts over; the app is asked for its rows") {
  CommandTable t;
  FakeApp fake;
  g_log.clear();
  t.assemble(fake, &log_line);
  CHECK(t.app_count() == 2u);
  CHECK(std::string(t.row(console::kKitCommandCount).name) == "ping");
  t.assemble(fake, &log_line);  // not 4
  CHECK(t.app_count() == 2u);
  BareApp bare;
  t.assemble(bare, &log_line);
  CHECK(t.app_count() == 0u);
  CHECK(t.size() == console::kKitCommandCount);
  CHECK(g_log.empty());
}

TEST_CASE("dispatch: an app row runs with its words; an unknown name gets the shell's reply") {
  CommandTable t;
  t.assemble(kAppRows, 2, &log_line);
  const char* const one[] = {"ping", "x", "y"};
  const char* const two[] = {"zap"};
  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  g_ran.clear();
  CHECK(t.run_app(app::DiagArgs{3, one}, out));
  CHECK(g_ran == "a");
  CHECK(c.text == "A:3 x y\n");
  c.text.clear();
  CHECK(t.run_app(app::DiagArgs{1, two}, out));
  CHECK(g_ran == "b");
  CHECK(c.text == "B\n");

  c.text.clear();
  g_ran.clear();
  const char* const nope[] = {"nope", "1"};
  CHECK_FALSE(t.run_app(app::DiagArgs{2, nope}, out));
  CHECK(c.text == "unknown command: nope (help lists the commands)\n");
  CHECK(g_ran.empty());

  c.text.clear();
  const char* const kit[] = {"version"};  // a kit name is not the dispatcher's
  CHECK_FALSE(t.run_app(app::DiagArgs{1, kit}, out));
  CHECK(c.text == "unknown command: version (help lists the commands)\n");

  c.text.clear();
  CHECK_FALSE(t.run_app(app::DiagArgs{}, out));  // no word at all
  CHECK(c.text == "unknown command:  (help lists the commands)\n");
  CHECK(g_ran.empty());
}

TEST_CASE("help: a line a row, the name padded to 8 columns, kit rows first") {
  CommandTable t;
  t.assemble(kAppRows, 2, &log_line);
  const std::string text = help_of(t);
  std::vector<std::string> lines;
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = text.find('\n', at);
    REQUIRE(end != std::string::npos);
    lines.push_back(text.substr(at, end - at));
    at = end + 1u;
  }
  REQUIRE(lines.size() == t.size());
  CHECK(lines[0] == "help    list the commands");
  CHECK(lines[1] == "version build identity and USB identity");
  CHECK(lines[2] == "stage   boot/run stage, uptime, why the chip reset");
  CHECK(lines[console::kKitCommandCount] == "ping    ping: answer");
  CHECK(lines[console::kKitCommandCount + 1u] == "zap     zap: another");
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const CommandTable::Row r = t.row(i);
    CAPTURE(i);
    CHECK(lines[i].compare(0, std::strlen(r.name), r.name) == 0);
    CHECK(lines[i][std::strlen(r.name)] == ' ');
    CHECK(lines[i].compare(8, std::string::npos, r.help) == 0);  // the help starts in column 8
  }
  CHECK(text.find('\r') == std::string::npos);  // plain '\n': the shell's output makes it CR LF
}

TEST_CASE("state: the kit's pairs, then the app's, on one line") {
  platform::host::FakeDisplay display;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  FakeApp fake;
  app::Platform platform{display, input, nullptr, clock, false};
  app::Runtime runtime{fake, engine, platform};
  REQUIRE(runtime.start());

  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  console::write_state(runtime, out);
  CHECK(c.text == "app=FAKE screen=MAIN playing=0 tempo=120000 cpu=0 frames=0 sys=0 section=display fw=closed card=none files=idle steps=16 pattern=A_1\n");

  // The same text as the runtime gives, one line.
  char buf[console::kStateBuffer];
  const std::size_t n = runtime.state_line(buf, sizeof buf);
  CHECK(c.text == std::string(buf, n) + "\n");
  CHECK(c.text.find('\n') == c.text.size() - 1u);
}

TEST_CASE("state: an app with no pairs adds nothing") {
  platform::host::FakeDisplay display;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  BareApp bare;
  app::Platform platform{display, input, nullptr, clock, false};
  app::Runtime runtime{bare, engine, platform};
  REQUIRE(runtime.start());
  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  console::write_state(runtime, out);
  CHECK(c.text == "app=BARE screen=MAIN playing=0 tempo=120000 cpu=0 frames=0 sys=0 section=display fw=closed card=none files=idle\n");
}

TEST_CASE("key: a button by name in either case, or by decimal code 0..32; anything else is -1") {
  CHECK(console::button_code("f1") == 0);
  CHECK(console::button_code("F8") == 7);
  CHECK(console::button_code("play") == 23);
  CHECK(console::button_code("Shift") == 29);
  CHECK(console::button_code("RIGHT") == 32);
  CHECK(console::button_code("sampleload") == 13);
  CHECK(console::button_code("0") == 0);
  CHECK(console::button_code("7") == 7);
  CHECK(console::button_code("32") == 32);
  CHECK(console::button_code("33") == -1);
  CHECK(console::button_code("99") == -1);
  CHECK(console::button_code("100") == -1);
  CHECK(console::button_code("1000") == -1);
  CHECK(console::button_code("1x") == -1);
  CHECK(console::button_code("-1") == -1);
  CHECK(console::button_code("nope") == -1);
  CHECK(console::button_code("") == -1);
  CHECK(console::button_code(nullptr) == -1);
  // Every kit button name maps to its own code.
  for (unsigned b = 0; b < app::kButtonCount; ++b) {
    CHECK(console::button_code(app::button_name(static_cast<app::Button>(b))) == static_cast<int>(b));
  }
}

TEST_CASE("mem: the SDRAM rows add up to the part, the SRAM rows carry the image's figures, and the text says so") {
  console::MemTables t;
  console::fill_mem_tables(t, console::MemFacts{12345u, 54432u, 700000u});
  CHECK(t.report.sdram_count == 2u);
  CHECK(t.sdram[0].size + t.sdram[1].size == t.report.sdram_total);
  CHECK(t.sdram[0].size == 700000u);
  CHECK(t.sdram[1].base == t.sdram[0].base + 700000u);
  CHECK(t.report.sram_l_hot_used == 12345u);
  CHECK(t.report.sram_u_used == 54432u);
  char buf[1536];
  const std::size_t n = platform::mk66::audio::diag::format_mem(t.report, buf, sizeof buf);
  REQUIRE(n > 0u);
  REQUIRE(n < sizeof buf - 1u);
  const std::string text(buf, n);
  CHECK(text.find("(complete)") != std::string::npos);
  CHECK(text.find(".sdram_bss (O3SDK_BIG objects)") != std::string::npos);
  CHECK(text.find("hot code 12345 B of 32 KiB") != std::string::npos);
  CHECK(text.find("sram_u 192 KiB: image 54432 B") != std::string::npos);
  CHECK(text.find("MISMATCH") == std::string::npos);

  // A figure past the part (a corrupt linker symbol) is clamped, never wrapped.
  console::fill_mem_tables(t, console::MemFacts{0u, 0u, 0xFFFFFFFFu});
  CHECK(t.sdram[0].size == platform::mk66::mem::kSdramSize);
  CHECK(t.sdram[1].size == 0u);
}
