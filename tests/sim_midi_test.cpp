// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI on the hello app, end to end in the simulator with the host fake for the device: notes sound in the audio at the frame they came in, the
// user's channel filters them, `clock 140` gives tempo 140 and the riff follows the master's ticks, the scenario language's `midi` and `clock`
// lines, and the scenario apps/hello/scenarios/midi.txt runs as it is. The kit's rules are app_midi_test.cpp and dsp_follow_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/app/app.h>
#include <o3sdk/platform/host/fake_midi.h>
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
using platform::host::FakeMidi;
using Port = hal::midi::Port;

namespace {

// RMS of frames [from, to) of an interleaved stereo recording, 0..1.
double rms(const std::vector<std::int16_t>& lr, std::size_t from, std::size_t to) {
  to = to < lr.size() / 2u ? to : lr.size() / 2u;
  double s = 0.0;
  std::size_t n = 0;
  for (std::size_t i = from; i < to; ++i) {
    for (std::size_t c = 0; c < 2; ++c) {
      const double v = static_cast<double>(lr[2u * i + c]);
      s += v * v;
    }
    ++n;
  }
  return n != 0 ? std::sqrt(s / static_cast<double>(2u * n)) / 32768.0 : 0.0;
}

constexpr std::size_t kMs = 44;  // frames in a millisecond, near enough for windows

struct Rig {
  FakeMidi midi;
  std::unique_ptr<sim::Harness> h;
  Rig() {
    h = std::make_unique<sim::Harness>(o3sdk_app(), nullptr, &midi);
    REQUIRE(h->start());
    h->record(true);
    h->run_ms(200);
  }
  std::size_t frames() const { return h->audio().size() / 2u; }
  std::string state() {
    char buf[400];
    const std::size_t n = h->runtime().state_line(buf, sizeof buf);
    return std::string(buf, n);
  }
};

}  // namespace

TEST_CASE("sim midi: a note on DIN or USB sounds in the audio, ends with its off, and starts at the frame it came in") {
  Rig r;
  CHECK(rms(r.h->audio(), 0, r.frames()) < 1e-6);  // nothing played yet: digital silence

  // On DIN, channel 1, at frame 21 of the next block: nothing before that sample, sound just after it.
  const std::uint64_t block = r.midi.block_start();
  r.midi.in_frame(21, Port::Din, 0x90, 48, 110);
  r.h->run_ms(300);
  const std::size_t at = static_cast<std::size_t>(block) + 21u;
  const auto& pcm = r.h->audio();
  NEED(pcm.size() > 2u * (at + 300u));
  bool silent_before = true;
  for (std::size_t i = 0; i < at; ++i) {
    silent_before = silent_before && pcm[2u * i] == 0 && pcm[2u * i + 1u] == 0;
  }
  CHECK(silent_before);  // not one sample before the note's frame
  double early = 0.0;
  for (std::size_t i = at; i < at + 100u; ++i) {
    early += std::fabs(static_cast<double>(pcm[2u * i]));
  }
  CHECK(early > 0.0);  // the attack has begun within the next 100 samples
  CHECK(rms(pcm, at + 300u, at + 300u + 100u * kMs) > 0.01);  // held

  r.midi.in(Port::Din, 0x80, 48, 0);
  r.h->run_ms(1500);
  CHECK(rms(r.h->audio(), r.frames() - 200u * kMs, r.frames()) < 1e-3);  // released, decayed

  // On USB, channel 3: omni by default, so it plays as well.
  const std::size_t mark = r.frames();
  r.midi.in(Port::Usb, 0x92, 52, 100);
  r.h->run_ms(300);
  CHECK(rms(r.h->audio(), mark + 100u * kMs, mark + 300u * kMs) > 0.01);
  r.midi.in(Port::Usb, 0x82, 52, 0);
  r.h->run_ms(1500);
  CHECK(r.midi.status().rx == 4u);
  CHECK(r.midi.status().dropped == 0u);
}

TEST_CASE("sim midi: the user's channel decides what plays, and a note on another channel is silence") {
  Rig r;
  hal::midi::Config c = r.h->runtime().context().midi().config();
  c.channel = 3;
  r.h->runtime().context().midi().set_config(c);
  r.h->run_ms(10);
  const std::size_t mark = r.frames();
  r.midi.in(Port::Din, 0x90, 48, 110);  // channel 1: not the user's
  r.h->run_ms(300);
  CHECK(rms(r.h->audio(), mark, r.frames()) < 1e-6);
  r.midi.in(Port::Din, 0x80, 48, 0);
  r.h->run_ms(20);
  const std::size_t mark2 = r.frames();
  r.midi.in(Port::Din, 0x92, 48, 110);  // channel 3
  r.h->run_ms(300);
  CHECK(rms(r.h->audio(), mark2 + 100u * kMs, r.frames()) > 0.01);
}

TEST_CASE("sim midi: a master at 140 gives tempo 140, the riff follows its ticks, Stop stops it, and the master going away gives the clock back") {
  Rig r;
  hal::midi::Config c = r.h->runtime().context().midi().config();
  c.follow = true;
  r.h->runtime().context().midi().set_config(c);
  r.midi.clock_start(140'000);
  r.h->run_ms(2000);
  dsp::Engine& e = r.h->engine();
  CHECK(e.tempo() == 140'000u);
  CHECK(e.playing());
  CHECK(r.state().find("tempo=140000") != std::string::npos);
  CHECK(r.state().find("playing=1") != std::string::npos);
  // The transport runs at the master's rate: 140 BPM is 2240 ticks a second. After about 2 s it is at about 4480 (the start is a block's width).
  const double ticks = static_cast<double>(e.stats().pos >> 32);
  CHECK(ticks > 4400.0);
  CHECK(ticks < 4520.0);
  CHECK(rms(r.h->audio(), r.frames() - 1000u * kMs, r.frames()) > 0.003);  // the riff sounds

  r.midi.clock_stop(140'000);
  r.h->run_ms(50);
  CHECK_FALSE(e.playing());
  CHECK(e.tempo() == 140'000u);
  r.h->run_ms(1500);
  CHECK(rms(r.h->audio(), r.frames() - 300u * kMs, r.frames()) < 0.003);  // (the last notes have died away)

  r.midi.clock_off();
  r.h->run_ms(3000);
  CHECK(e.tempo() == 120'000u);  // 48 clocks at 140 BPM later: HELLO's own
  CHECK_FALSE(e.following());
}

TEST_CASE("sim midi: HELLO's tempo keys step its own tempo while a master is followed, not the master's plus one") {
  Rig r;
  hal::midi::Config c = r.h->runtime().context().midi().config();
  c.follow = true;
  r.h->runtime().context().midi().set_config(c);
  r.midi.clock_tempo(140'000);
  r.h->run_ms(50);
  dsp::Engine& e = r.h->engine();
  CHECK(e.tempo() == 140'000u);
  r.h->press(app::Button::Up);
  r.h->run_ms(1);
  r.h->release(app::Button::Up);
  r.h->run_ms(50);
  CHECK(e.tempo() == 140'000u);      // the master's is what the header shows
  CHECK(e.own_tempo() == 121'000u);  // 120 + 1 BPM, not 141
  c.follow = false;
  r.h->runtime().context().midi().set_config(c);
  r.h->run_ms(50);
  CHECK(e.tempo() == 121'000u);  // and it is what HELLO runs at when the master is let go
}

TEST_CASE("sim midi: the scenario lines `midi` and `clock`, and what they refuse") {
  const std::string dir = std::filesystem::temp_directory_path().string();
  {
    Rig r;
    sim::ScenarioResult s = sim::run_scenario(*r.h, "midi din 90 30 64\nwait 20\nmidi usb 80 30 00 90 32 40\nwait 20\n", dir);
    CHECK(s.ok);
    CHECK(r.midi.status().rx == 3u);  // one message, then two in one line
    s = sim::run_scenario(*r.h, "tap config\ntap f1\ntap f1\n", dir);  // (the MIDI section's first row: IN CHANNEL, F1 steps it)
    CHECK(s.ok);
    CHECK(r.h->runtime().context().midi().config().channel == 2u);
    s = sim::run_scenario(*r.h, "tap config\nclock 140.5 start\nwait 20\nclock 128 stop\nclock 100 continue 8\nclock off\n", dir);
    CHECK(s.ok);

    CHECK_FALSE(sim::run_scenario(*r.h, "midi din zz\n", dir).ok);
    CHECK_FALSE(sim::run_scenario(*r.h, "midi din 90 30\n", dir).ok);  // not a whole message
    CHECK_FALSE(sim::run_scenario(*r.h, "midi serial 90 30 64\n", dir).ok);
    CHECK_FALSE(sim::run_scenario(*r.h, "midi din\n", dir).ok);
    CHECK_FALSE(sim::run_scenario(*r.h, "clock 10 start\n", dir).ok);   // below 20 BPM
    CHECK_FALSE(sim::run_scenario(*r.h, "clock 301 start\n", dir).ok);  // above 300
    CHECK_FALSE(sim::run_scenario(*r.h, "clock 140 jump\n", dir).ok);
    CHECK_FALSE(sim::run_scenario(*r.h, "clock 140 continue\n", dir).ok);
    CHECK_FALSE(sim::run_scenario(*r.h, "clock 140 continue 99999\n", dir).ok);
    CHECK_FALSE(sim::run_scenario(*r.h, "clock\n", dir).ok);
  }
  {
    // 140.5 BPM is 140500 milli-BPM.
    Rig r;
    hal::midi::Config c = r.h->runtime().context().midi().config();
    c.follow = true;
    r.h->runtime().context().midi().set_config(c);
    CHECK(sim::run_scenario(*r.h, "clock 140.5 start\nwait 50\n", dir).ok);
    CHECK(r.h->engine().tempo() == 140'500u);
  }
  {
    // A harness without a device says why, and the rest of the language still works.
    sim::Harness h(o3sdk_app());
    REQUIRE(h.start());
    sim::ScenarioResult s = sim::run_scenario(h, "wait 5\nmidi din 90 30 64\n", dir);
    CHECK_FALSE(s.ok);
    CHECK(s.line == 2);
    CHECK(s.error.find("no MIDI device") != std::string::npos);
    s = sim::run_scenario(h, "clock 140 start\n", dir);
    CHECK_FALSE(s.ok);
    CHECK(s.error.find("no MIDI device") != std::string::npos);
    CHECK(h.midi() == nullptr);
  }
}

#ifdef O3SDK_HELLO_SCENARIOS
TEST_CASE("sim midi: apps/hello/scenarios/midi.txt runs as it is: its pictures are drawn and its audio is not silent") {
  const std::filesystem::path file = std::filesystem::path(O3SDK_HELLO_SCENARIOS) / "midi.txt";
  std::ifstream in(file);
  NEED(static_cast<bool>(in));
  std::stringstream text;
  text << in.rdbuf();
  const std::filesystem::path out = std::filesystem::temp_directory_path() / "hello_midi_scenario";
  std::filesystem::create_directories(out);

  FakeMidi midi;
  sim::Harness h(o3sdk_app(), nullptr, &midi);
  REQUIRE(h.start());
  const sim::ScenarioResult r = sim::run_scenario(h, text.str(), out.string());
  CHECK_MESSAGE(r.ok, "line " << r.line << ": " << r.error);
  CHECK(r.snapshots == 9u);
  for (const char* wav : {"01_notes.wav", "02_channel_filter.wav", "03_follow.wav"}) {
    const std::filesystem::path p = out / wav;
    CHECK(std::filesystem::exists(p));
    CHECK(std::filesystem::file_size(p) > 44u + 1000u);
  }
  CHECK(rms(h.audio(), 0, h.audio().size() / 2u) > 0.003);
  // The notes of the first part are in the recording: the first note (300 ms held) is loud, and the silence between the notes is not.
  CHECK(rms(h.audio(), 100u * kMs, 300u * kMs) > 0.01);
  CHECK(h.runtime().frame_failures() == 0u);
  CHECK(h.display().render_failures() == 0u);
  CHECK(h.engine().dropped_commands() == 0u);
  // The scenario ended with the master gone: HELLO's own clock again.
  CHECK(h.engine().tempo() == 120'000u);
  CHECK_FALSE(h.engine().following());
  CHECK(midi.status().rx > 10u);
}
#endif
