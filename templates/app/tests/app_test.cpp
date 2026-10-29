// SPDX-License-Identifier: GPL-3.0-or-later
// The app end to end in the simulator (o3-sdk docs/FRAMEWORK.md section 7): it starts, draws every frame inside the display list,
// sounds when a pad is played, answers the console's `state`, keeps its pattern and its octave on the card across a reboot, and its
// scenarios run as they are (each on a card of its own: a scratch directory).
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace o3sdk;

namespace {

// The rms of the interleaved int16 samples from index `from`, as a fraction of full scale.
double rms(const std::vector<std::int16_t>& lr, std::size_t from) {
  double s = 0.0;
  std::size_t n = 0;
  for (std::size_t i = from; i < lr.size(); i += 2) {
    s += static_cast<double>(lr[i]) * static_cast<double>(lr[i]);
    ++n;
  }
  return n != 0 ? std::sqrt(s / static_cast<double>(n)) / 32768.0 : 0.0;
}

std::string state(sim::Harness& h) {
  char line[512];
  const std::size_t n = h.runtime().state_line(line, sizeof line);
  return std::string(line, n);
}

void tap(sim::Harness& h, app::Button b) {
  h.press(b);
  h.run_ms(5);
  h.release(b);
  h.run_ms(5);
}

}  // namespace

TEST_CASE("app: it starts, a pad sounds, and every frame fits the display list") {
  auto h = std::make_unique<sim::Harness>(o3sdk_app());  // the app's entry point, linked whole into the test
  REQUIRE(h->start());
  h->record(true);
  h->run_ms(200);
  CHECK(rms(h->audio(), 0) < 1e-6);  // nothing played yet: digital silence

  h->pad_down(36);  // bottom left
  h->run_ms(300);
  CHECK(h->runtime().context().panel().pad_held(36));
  CHECK(h->leds().level(36) == 255);  // a held pad is full
  h->pad_up(36);
  h->run_ms(300);
  CHECK(rms(h->audio(), 0) > 0.01);

  CHECK(h->runtime().frames() > 20u);
  CHECK(h->runtime().frame_failures() == 0u);
  CHECK(h->display().render_failures() == 0u);
  CHECK(h->display().peak_frame_words() < 2048u);
  CHECK(h->engine().dropped_commands() == 0u);
}

TEST_CASE("app: the console's state line carries the app's pairs") {
  auto h = std::make_unique<sim::Harness>(o3sdk_app());
  REQUIRE(h->start());
  h->run_ms(100);
  char line[300];
  const std::size_t n = h->runtime().state_line(line, sizeof line);
  const std::string text(line, n);
  CHECK(text.find("app=@APP_NAME@") == 0);
  CHECK(text.find("screen=PLAY") != std::string::npos);
  CHECK(text.find("octave=0") != std::string::npos);
  CHECK(text.find("focus=0") != std::string::npos);
}

TEST_CASE("app: a pattern and an octave saved on the card come back after a restart, a bad file changes nothing") {
  platform::host::ScratchDir card_dir("@APP_ID@-card");
  REQUIRE(card_dir.ok());
  platform::host::DirCard card(card_dir.path());
  auto h = std::make_unique<sim::Harness>(o3sdk_app(), nullptr, nullptr, &card);
  REQUIRE(h->start());
  h->run_ms(100);
  CHECK(state(*h).find("steps=0") != std::string::npos);
  for (unsigned pad : {36u, 40u, 43u}) {
    h->pad_down(pad);
    h->run_ms(20);
    h->pad_up(pad);
    h->run_ms(20);
  }
  tap(*h, app::Button::Left);  // an octave down: it saves itself
  h->run_ms(100);              // (a key pressed while a file job runs is ignored: let that one end)
  tap(*h, app::Button::File);
  h->run_ms(300);
  CHECK(state(*h).find("files=done") != std::string::npos);

  REQUIRE(h->reboot());
  h->run_ms(300);
  const std::string after = state(*h);
  CHECK(after.find("steps=3") != std::string::npos);
  CHECK(after.find("octave=-1") != std::string::npos);

  // A file that is not a pattern: the load works, the file is refused whole, the pattern stays and the screen says BAD FILE.
  const std::filesystem::path file = std::filesystem::path(card_dir.path()) / "Apps" / "@APP_ID@" / "pattern.o3a";
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "not a pattern file, but longer than the header of one";
  }
  tap(*h, app::Button::SampleLoad);
  h->run_ms(300);
  CHECK(state(*h).find("steps=3") != std::string::npos);
  CHECK(h->runtime().frame_failures() == 0u);
}

TEST_CASE("app: the scenarios run as they are, write their pictures and the audio is not silent") {
  const std::filesystem::path dir = APP_SCENARIO_DIR;
  const std::filesystem::path out = std::filesystem::temp_directory_path() / "@APP_ID@_scenarios";
  std::filesystem::create_directories(out);
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".txt") {
      continue;
    }
    ++count;
    INFO(entry.path().filename().string());
    std::ifstream in(entry.path());
    std::stringstream text;
    text << in.rdbuf();
    platform::host::ScratchDir card_dir("@APP_ID@-card");  // an empty card for each scenario
    REQUIRE(card_dir.ok());
    platform::host::DirCard card(card_dir.path());
    auto h = std::make_unique<sim::Harness>(o3sdk_app(), nullptr, nullptr, &card);
    REQUIRE(h->start());
    const sim::ScenarioResult r = sim::run_scenario(*h, text.str(), out.string());
    CHECK_MESSAGE(r.ok, r.error);
    CHECK(h->runtime().frame_failures() == 0u);
    CHECK(h->display().peak_frame_words() < 2048u);
    CHECK(card.violations() == 0u);
  }
  CHECK(count >= 2u);
  CHECK(std::filesystem::exists(out / "boot.wav"));
  CHECK(std::filesystem::exists(out / "01_boot.png"));
}
