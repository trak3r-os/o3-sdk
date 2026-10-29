// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/diag.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/host/fakes.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

using namespace o3sdk;

namespace {

// ---- DiagOut ---------------------------------------------------------------------------------------------------------------

struct Capture {
  std::string text;
  unsigned writes = 0;
};

void capture_write(void* context, const char* data, std::size_t n) {
  auto* c = static_cast<Capture*>(context);
  c->text.append(data, n);
  ++c->writes;
}

std::string out_of(void (*fn)(app::DiagOut&)) {
  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  fn(out);
  return c.text;
}

// ---- Apps -------------------------------------------------------------------------------------------------------------------

class Blank final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PLAY SCREEN"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

void handler_ping(const app::DiagArgs& args, app::DiagOut& out) noexcept { out.text("pong ").dec(args.argc).nl(); }
void handler_nop(const app::DiagArgs&, app::DiagOut&) noexcept {}

constexpr app::DiagCommand kRows[] = {
    {"ping", "ping: answer pong", &handler_ping},
    {"nop", "nop: nothing", &handler_nop},
};

// What an app that overrides nothing looks like.
class PlainApp : public app::App {
 public:
  explicit PlainApp(const char* name = "TEST") : info_{name, "0"} {}
  const app::AppInfo& info() const noexcept override { return info_; }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen_); }

 private:
  app::AppInfo info_;
  Blank screen_;
};

// An app with commands and state; `mode` is what describe_state reports.
class RichApp final : public PlainApp {
 public:
  std::size_t diag_commands(const app::DiagCommand*& rows) const noexcept override {
    rows = kRows;
    return sizeof kRows / sizeof kRows[0];
  }
  std::size_t describe_state(const app::Context& ctx, char* out, std::size_t cap) const noexcept override {
    ++calls;
    last_cap = cap;
    app::StateWriter w(out, cap);
    w.num("mode", mode).str("label", "A B").num("seen_tempo", ctx.engine().tempo());
    return w.size();
  }
  std::uint32_t mode = 3;
  mutable unsigned calls = 0;
  mutable std::size_t last_cap = 0;
};

// An app that returns more than it was given and writes junk past what it reports.
class GreedyApp final : public PlainApp {
 public:
  std::size_t describe_state(const app::Context&, char* out, std::size_t cap) const noexcept override {
    for (std::size_t i = 0; i < cap; ++i) {
      out[i] = 'x';
    }
    return cap + 40;
  }
};

class SilentApp final : public PlainApp {
 public:
  std::size_t describe_state(const app::Context&, char* out, std::size_t cap) const noexcept override {
    if (cap != 0u) {
      out[0] = '\0';
    }
    return 0;
  }
};

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept {
  g_cycles += hal::audio::kCyclesPerBlock / 4u;
  return g_cycles;
}

template <typename AppT>
struct Rig {
  platform::host::FakeDisplay display;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  AppT app;
  app::Platform platform{display, input, nullptr, clock, false};
  app::Runtime runtime{app, engine, platform};

  std::string line(std::size_t cap = 400) {
    std::string buf(cap, '#');
    const std::size_t n = runtime.state_line(buf.data(), cap);
    CHECK(n < cap);
    CHECK(buf[n] == '\0');
    CHECK(std::strlen(buf.c_str()) == n);
    return std::string(buf.c_str(), n);
  }
};

}  // namespace

TEST_CASE("diag out: text, numbers and hex go to the sink; no sink, no writes") {
  CHECK(out_of([](app::DiagOut& o) { o.text("ab").chr('c').nl(); }) == "abc\n");
  CHECK(out_of([](app::DiagOut& o) { o.dec(0).chr(' ').dec(7).chr(' ').dec(4294967295u); }) == "0 7 4294967295");
  CHECK(out_of([](app::DiagOut& o) { o.sdec(-12).chr(' ').sdec(34).chr(' ').sdec(INT32_MIN); }) == "-12 34 -2147483648");
  CHECK(out_of([](app::DiagOut& o) { o.hex(0x1A2Bu).chr(' ').hex(0xBEEFu, 4).chr(' ').hex(0xF, 1).chr(' ').hex(0xDEADBEEFu, 20); }) ==
        "0x00001A2B 0xBEEF 0xF 0xDEADBEEF");
  CHECK(out_of([](app::DiagOut& o) { o.text(nullptr).text("", 0).text("xyz", 2); }) == "xy");

  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  out.text("hello").nl();
  CHECK(out.bytes() == 6u);
  CHECK(c.writes == 2u);

  app::DiagOut none;  // no sink: nothing happens, nothing is counted
  none.text("lost").dec(5).nl();
  CHECK(none.bytes() == 0u);
}

TEST_CASE("diag args: arg() is safe past the last word") {
  const char* const words[] = {"cmd", "one", "two"};
  const app::DiagArgs a{3, words};
  CHECK(std::string(a.arg(0)) == "cmd");
  CHECK(std::string(a.arg(2)) == "two");
  CHECK(std::string(a.arg(3)) == "");
  CHECK(std::string(a.arg(99)) == "");
  const app::DiagArgs empty;
  CHECK(std::string(empty.arg(0)) == "");
}

TEST_CASE("state writer: pairs in order, one space between, numbers signed and unsigned") {
  char buf[96];
  app::StateWriter w(buf, sizeof buf);
  w.num("a", 0).num("b", 4294967295u).sint("c", -5).sint("d", 7).sint("e", INT32_MIN).flag("f", true).flag("g", false);
  CHECK(std::string(buf) == "a=0 b=4294967295 c=-5 d=7 e=-2147483648 f=1 g=0");
  CHECK(w.size() == std::strlen(buf));
  CHECK(w.complete());
}

TEST_CASE("state writer: a value has no space; null and empty are a dash; non-ASCII is a question mark; the cut is `max`") {
  char buf[96];
  app::StateWriter w(buf, sizeof buf);
  w.str("a", "SAMPLE LOAD").str("b", nullptr).str("c", "").str("d", "t\tab").str("e", "caf\xC3\xA9").str("f", "abcdefghij", 4);
  CHECK(std::string(buf) == "a=SAMPLE_LOAD b=- c=- d=t_ab e=caf?? f=abcd");
}

TEST_CASE("state writer: a pair is written whole or not at all, and after one is dropped so are all the later ones") {
  char buf[16];
  app::StateWriter w(buf, sizeof buf);
  w.num("ab", 12).num("cd", 345);  // "ab=12 cd=345" is 12 characters
  CHECK(std::string(buf) == "ab=12 cd=345");
  CHECK(w.complete());
  w.num("e", 1);  // " e=1" brings it to 16, and the terminator needs one more
  CHECK(std::string(buf) == "ab=12 cd=345");
  CHECK_FALSE(w.complete());
  w.num("f", 1);  // would fit alone, but the order is fixed: nothing after a drop
  CHECK(std::string(buf) == "ab=12 cd=345");

  char exact[8];  // "k=12345" is 7 characters + the terminator
  app::StateWriter e(exact, sizeof exact);
  e.num("k", 12345);
  CHECK(std::string(exact) == "k=12345");
  CHECK(e.complete());
  char shy[7];
  app::StateWriter s(shy, sizeof shy);
  s.num("k", 12345);
  CHECK(std::string(shy).empty());
  CHECK_FALSE(s.complete());
}

TEST_CASE("state writer: no buffer, no capacity, a missing key") {
  app::StateWriter none(nullptr, 40);
  none.num("a", 1);
  CHECK(none.size() == 0u);
  CHECK_FALSE(none.complete());
  char buf[4] = {'#', '#', '#', '#'};
  app::StateWriter zero(buf, 0);
  zero.num("a", 1);
  CHECK(buf[0] == '#');
  app::StateWriter nokey(buf, sizeof buf);
  nokey.num("", 1).num(nullptr, 2);
  CHECK(std::string(buf).empty());
  CHECK_FALSE(nokey.complete());
}

TEST_CASE("app: the hooks default to nothing") {
  PlainApp a;
  const app::DiagCommand* rows = kRows;  // not null: the hook must set it
  CHECK(a.diag_commands(rows) == 0u);
  CHECK(rows == nullptr);
  Rig<PlainApp> r;
  char buf[8] = {'#'};
  CHECK(r.app.describe_state(r.runtime.context(), buf, sizeof buf) == 0u);
  CHECK(buf[0] == '#');
}

TEST_CASE("app: an override gives its rows") {
  RichApp a;
  const app::DiagCommand* rows = nullptr;
  REQUIRE(a.diag_commands(rows) == 2u);
  REQUIRE(rows != nullptr);
  CHECK(std::string(rows[0].name) == "ping");
  CHECK(std::string(rows[1].name) == "nop");
  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  const char* const words[] = {"ping", "x"};
  rows[0].run(app::DiagArgs{2, words}, out);
  CHECK(c.text == "pong 2\n");
}

TEST_CASE("state line: the kit's pairs, in the documented order, before the first frame") {
  Rig<PlainApp> r;
  // Not started: no screen yet.
  CHECK(r.line() == "app=TEST screen=- playing=0 tempo=120000 cpu=0 frames=0 sys=0 section=display fw=closed card=none files=idle");
  REQUIRE(r.runtime.start());
  CHECK(r.line() == "app=TEST screen=PLAY_SCREEN playing=0 tempo=120000 cpu=0 frames=0 sys=0 section=display fw=closed card=none files=idle");
}

TEST_CASE("state line: playing, tempo and frames follow the engine and the runtime") {
  Rig<PlainApp> r;
  REQUIRE(r.runtime.start());
  CHECK(r.engine.play());
  CHECK(r.engine.set_tempo(133'500));
  CHECK(r.runtime.draw());
  CHECK(r.runtime.draw());
  CHECK(r.line() == "app=TEST screen=PLAY_SCREEN playing=1 tempo=133500 cpu=0 frames=2 sys=0 section=display fw=closed card=none files=idle");
  CHECK(r.engine.stop());
  CHECK(r.line().find("playing=0") != std::string::npos);
}

TEST_CASE("state line: cpu is the render's load in percent, only with a cycle counter on the platform") {
  Rig<PlainApp> r;
  r.engine.set_cycle_counter(&fake_cycles);
  REQUIRE(r.runtime.start());
  float l[hal::audio::kBlock] = {};
  float rr[hal::audio::kBlock] = {};
  r.engine.render(nullptr, nullptr, l, rr, hal::audio::kBlock);
  CHECK(r.engine.stats().cycles == hal::audio::kCyclesPerBlock / 4u);
  CHECK(r.line().find("cpu=0 ") != std::string::npos);  // platform.cpu_meter is false

  r.platform.cpu_meter = true;
  CHECK(r.line().find(" cpu=25 ") != std::string::npos);
}

TEST_CASE("state line: the app's pairs follow the kit's, after one space, and see the context") {
  Rig<RichApp> r;
  REQUIRE(r.runtime.start());
  CHECK(r.engine.set_tempo(99'000));
  r.app.mode = 11;
  CHECK(r.line() == "app=TEST screen=PLAY_SCREEN playing=0 tempo=99000 cpu=0 frames=0 sys=0 section=display fw=closed card=none files=idle mode=11 label=A_B seen_tempo=99000");
  CHECK(r.app.calls == 1u);
  CHECK(r.app.last_cap > 100u);  // handed the room that was left, not the whole buffer
}

TEST_CASE("state line: an app with nothing to say adds no separator") {
  Rig<SilentApp> r;
  REQUIRE(r.runtime.start());
  const std::string line = r.line();
  CHECK(line == "app=TEST screen=PLAY_SCREEN playing=0 tempo=120000 cpu=0 frames=0 sys=0 section=display fw=closed card=none files=idle");
}

TEST_CASE("state line: spaces in the app name become underscores") {
  // (PlainApp's name is a constructor argument: build the rig by hand.)
  platform::host::FakeDisplay display;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  PlainApp named("MY APP");
  app::Platform platform{display, input, nullptr, clock, false};
  app::Runtime runtime{named, engine, platform};
  char buf[100];
  const std::size_t n = runtime.state_line(buf, sizeof buf);
  CHECK(std::string(buf, n).rfind("app=MY_APP ", 0) == 0);
}

TEST_CASE("state line: a short buffer is cut at a pair, never inside one, and always terminated") {
  Rig<RichApp> r;
  REQUIRE(r.runtime.start());
  const std::string full = r.line();
  // Every capacity from 1 up to the whole line: the result is a prefix of the full line that ends at a pair boundary.
  for (std::size_t cap = 1; cap <= full.size() + 1u; ++cap) {
    std::string buf(cap + 4u, '#');
    const std::size_t n = r.runtime.state_line(buf.data(), cap);
    REQUIRE(n < cap);
    REQUIRE(buf[n] == '\0');
    REQUIRE(buf[cap] == '#');  // nothing written past the capacity
    const std::string got(buf.c_str(), n);
    REQUIRE(full.compare(0, n, got) == 0);
    REQUIRE((n == 0u || n == full.size() || full[n] == ' '));  // (0: not even the first pair fits)
  }
  char guard[4] = {'#', '#', '#', '#'};
  CHECK(r.runtime.state_line(guard, 0) == 0u);
  CHECK(guard[0] == '#');
  CHECK(r.runtime.state_line(nullptr, 40) == 0u);
}

TEST_CASE("state line: an app that reports more than it was given is clamped and terminated") {
  Rig<GreedyApp> r;
  REQUIRE(r.runtime.start());
  char buf[140];  // the kit's pairs take about 120 characters now (sys, section, fw, card, files): room for the app's first
  std::memset(buf, '#', sizeof buf);
  const std::size_t n = r.runtime.state_line(buf, sizeof buf);
  CHECK(n == sizeof buf - 1u);
  CHECK(buf[n] == '\0');
  CHECK(std::string(buf).rfind("app=TEST screen=PLAY_SCREEN playing=0 tempo=120000 cpu=0 frames=0 sys=0 section=display fw=closed card=none files=idle x", 0) == 0);
}
