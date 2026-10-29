// SPDX-License-Identifier: GPL-3.0-or-later
// CRASH TEST on the host: its command is registered (the console's own table accepts the row), the table of faults is what CHECKLIST.md lists, and `fault <kind> confirm`
// arms, waits, fires and reports "no fault" where nothing can fault (the host). The faults themselves are provoked only on the unit (CHECKLIST.md).
#include <doctest/doctest.h>

#include "../src/faults.h"
#include "../src/marker.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/diag.h>
#include <o3sdk/platform/mk66/console/core.h>
#include <o3sdk/sim/harness.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

using namespace o3sdk;
namespace console = o3sdk::platform::mk66::console;

namespace {

struct Capture {
  std::string text;
};
void capture_write(void* context, const char* data, std::size_t n) { static_cast<Capture*>(context)->text.append(data, n); }

// Runs `fault <args...>` through the console's own dispatcher and returns what it said.
std::string fault(const console::CommandTable& table, std::initializer_list<const char*> args) {
  const char* argv[8] = {"fault"};
  unsigned argc = 1;
  for (const char* a : args) {
    argv[argc++] = a;
  }
  Capture c;
  app::DiagOut out(app::DiagSink{&capture_write, &c});
  CHECK(table.run_app(app::DiagArgs{argc, argv}, out));
  return c.text;
}

std::string state_of(sim::Harness& h) {
  char line[400];
  const std::size_t n = h.runtime().state_line(line, sizeof line);
  return std::string(line, n);
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("crashtest: the console's table takes the app's one row") {
  console::CommandTable table;
  table.assemble(o3sdk_app(), nullptr);
  CHECK(table.app_count() == 1u);
  CHECK(table.skipped() == 0u);  // no rule of the table (name, help, kit word, update word) refused it
  const app::DiagCommand* row = table.find_app("fault");
  REQUIRE(row != nullptr);
  CHECK(std::strlen(row->help) <= console::kHelpMax);
  CHECK(has(row->help, "SESSION ONLY"));
}

TEST_CASE("crashtest: the faults are the documented twelve, named in lower case and digits, with their expectations") {
  using namespace crashtest;
  static_assert(sizeof kFaults / sizeof kFaults[0] == kKindCount, "one row per Kind");
  const char* const expected[] = {"nmi", "hard", "mem", "bus", "buswr", "undef", "badfn", "unalign", "div0", "irq", "panic", "render"};
  for (std::size_t i = 0; i < kKindCount; ++i) {
    CHECK(std::string(kFaults[i].name) == expected[i]);
    for (const char* c = kFaults[i].name; *c != '\0'; ++c) {
      CHECK(((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')));
    }
    CHECK(std::strlen(kFaults[i].what) <= 56u);  // the screen's column
    CHECK(std::strlen(kFaults[i].expect) > 10u);
  }
  CHECK(&info(Kind::Panic) == &kFaults[10]);
  CHECK(&info(Kind::Render) == &kFaults[11]);
}

TEST_CASE("crashtest: the list, and a kind alone, say what they do and never arm") {
  console::CommandTable table;
  table.assemble(o3sdk_app(), nullptr);
  auto h = std::make_unique<sim::Harness>(o3sdk_app());
  REQUIRE(h->start());

  const std::string list = fault(table, {});
  for (const crashtest::FaultInfo& f : crashtest::kFaults) {
    CHECK(has(list, f.name));
  }
  CHECK(has(list, "session only"));
  CHECK(list.size() + 14u < 1600u);  // a reply the console promises room for, counting each of the 14 line ends as CR LF

  const std::string bus = fault(table, {"bus"});
  CHECK(has(bus, "0x40008008"));
  CHECK(has(bus, "nothing done"));
  CHECK(has(fault(table, {"nope"}), "unknown fault"));
  // A typo of the confirm word, a wrong case and a missing word arm nothing.
  CHECK(has(fault(table, {"bus", "yes"}), "nothing done"));
  CHECK(has(fault(table, {"bus", "CONFIRM"}), "nothing done"));
  h->run_ms(3000);
  const std::string state = state_of(*h);
  CHECK(has(state, "armed=none"));
  CHECK(has(state, "nofault=none"));
  CHECK(has(state, "prev=none"));
  CHECK(has(state, "seq=0"));
  CHECK(has(state, "app=CRASH_TEST_SESSION_ONLY"));
}

TEST_CASE("crashtest: confirm arms, the fault comes 1.5 s later, and on the host the core takes none") {
  console::CommandTable table;
  table.assemble(o3sdk_app(), nullptr);
  auto h = std::make_unique<sim::Harness>(o3sdk_app());
  REQUIRE(h->start());
  h->run_ms(50);

  CHECK(has(fault(table, {"div0", "confirm"}), "armed: div0"));
  CHECK(has(fault(table, {"bus", "confirm"}), "already armed: div0"));
  h->run_ms(100);
  CHECK(has(state_of(*h), "armed=div0"));
  h->run_ms(1000);
  CHECK(has(state_of(*h), "armed=div0"));  // not yet: 1.5 s
  h->run_ms(700);
  const std::string after = state_of(*h);
  CHECK(has(after, "armed=none"));
  CHECK(has(after, "nofault=div0"));
  CHECK(has(after, "seq=1"));
  CHECK(h->runtime().frame_failures() == 0u);  // the countdown's and the result's drawings fit the display list

  // A fault armed and cancelled never fires.
  CHECK(has(fault(table, {"undef", "confirm"}), "armed: undef"));
  h->run_ms(300);
  CHECK(has(fault(table, {"cancel"}), "cancelled"));
  CHECK(has(fault(table, {"cancel"}), "nothing was armed"));
  h->run_ms(3000);
  CHECK(has(state_of(*h), "nofault=none"));  // (a new arming hides the last result)
  CHECK(has(state_of(*h), "armed=none"));
  CHECK(has(state_of(*h), "seq=1"));
}

TEST_CASE("crashtest: the render fault runs inside the engine's block, and its marker survives a restart of the app") {
  console::CommandTable table;
  table.assemble(o3sdk_app(), nullptr);
  {
    auto h = std::make_unique<sim::Harness>(o3sdk_app());
    REQUIRE(h->start());
    h->run_ms(50);
    CHECK(has(fault(table, {"render", "confirm"}), "armed: render"));
    h->run_ms(2000);
    const std::string state = state_of(*h);
    CHECK(has(state, "nofault=render"));  // the hook ran, provoke() returned: the render side is wired
    CHECK(has(state, "seq=2"));           // (the cases above ran in this process: the marker is the app's, not the harness's)
  }
  // A restart (the simulator's `reboot` is a new Harness over the same app object): the marker said "no fault pending", so prev is none and seq goes on.
  auto h = std::make_unique<sim::Harness>(o3sdk_app());
  REQUIRE(h->start());
  h->run_ms(50);
  const std::string state = state_of(*h);
  CHECK(has(state, "prev=none"));
  CHECK(has(state, "armed=none"));
  CHECK(has(state, "seq=2"));
}

TEST_CASE("crashtest: the marker validates, and noise, zeros and a cut write are no marker") {
  crashtest::Marker m = {};
  std::uint32_t kind = 99;
  std::uint32_t seq = 99;
  CHECK_FALSE(crashtest::marker_read(m, kind, seq));  // zeroed RAM
  CHECK(kind == 99u);
  crashtest::marker_write(m, 4, 7);
  REQUIRE(crashtest::marker_read(m, kind, seq));
  CHECK(kind == 4u);
  CHECK(seq == 7u);
  crashtest::marker_write(m, 0, 8);
  REQUIRE(crashtest::marker_read(m, kind, seq));
  CHECK(kind == 0u);  // valid, and nothing armed
  CHECK(seq == 8u);
  m.seq = 9;  // one word changed
  CHECK_FALSE(crashtest::marker_read(m, kind, seq));
  crashtest::marker_write(m, 4, 7);
  m.magic = 0;  // a write cut before its last word
  CHECK_FALSE(crashtest::marker_read(m, kind, seq));
  for (std::uint32_t noise : {0xFFFFFFFFu, 0xA5A5A5A5u, 0x12345678u}) {
    m.magic = noise;
    m.kind = noise;
    m.seq = noise;
    m.check = noise;
    CHECK_FALSE(crashtest::marker_read(m, kind, seq));
  }
}
