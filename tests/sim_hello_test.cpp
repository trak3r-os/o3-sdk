// SPDX-License-Identifier: GPL-3.0-or-later
// The hello app end to end in the simulator: it starts, draws every frame within the display list, sounds when a pad is played and
// when the riff runs, and its scenario language works.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using namespace o3sdk;

namespace {

double rms(const std::vector<std::int16_t>& lr, std::size_t from) {
  double s = 0.0;
  std::size_t n = 0;
  for (std::size_t i = from; i < lr.size(); i += 2) {
    s += static_cast<double>(lr[i]) * static_cast<double>(lr[i]);
    ++n;
  }
  return n != 0 ? std::sqrt(s / static_cast<double>(n)) / 32768.0 : 0.0;
}

}  // namespace

TEST_CASE("sim: hello starts, plays a pad and the riff, and every frame fits") {
  auto h = std::make_unique<sim::Harness>(o3sdk_app());
  REQUIRE(h->start());
  h->record(true);
  h->run_ms(200);
  CHECK(rms(h->audio(), 0) < 1e-6);  // nothing played yet: digital silence

  h->pad_down(36);  // bottom left: C3
  h->run_ms(300);
  CHECK(h->runtime().context().panel().pad_held(36));
  CHECK(h->leds().level(36) == 255);
  const std::size_t mark = h->audio().size();
  h->pad_up(36);
  h->run_ms(600);
  CHECK(rms(h->audio(), 0) > 0.01);

  h->press(app::Button::Play);
  h->run_ms(1);
  h->release(app::Button::Play);
  h->run_ms(1000);
  CHECK(h->engine().stats().playing);
  CHECK(rms(h->audio(), mark) > 0.01);

  CHECK(h->runtime().frames() > 30u);
  CHECK(h->runtime().frame_failures() == 0u);
  CHECK(h->display().render_failures() == 0u);
  CHECK(h->display().peak_frame_words() < 2048u);
  CHECK(h->engine().dropped_commands() == 0u);
}

TEST_CASE("sim: scenarios report the failing line") {
  auto h = std::make_unique<sim::Harness>(o3sdk_app());
  REQUIRE(h->start());
  const std::string dir = std::filesystem::temp_directory_path().string();
  sim::ScenarioResult r = sim::run_scenario(*h, "wait 10\ntap f1\nturn 3\n# fine so far\nbogus 1\n", dir);
  CHECK_FALSE(r.ok);
  CHECK(r.line == 5);
  r = sim::run_scenario(*h, "tap nosuchbutton\n", dir);
  CHECK_FALSE(r.ok);
  r = sim::run_scenario(*h, "pad 48\n", dir);
  CHECK_FALSE(r.ok);
  r = sim::run_scenario(*h, "pad 47 20\npress shift\ntap up\nrelease shift\n", dir);
  CHECK(r.ok);
  CHECK(h->engine().tempo() == 130'000u);
}
