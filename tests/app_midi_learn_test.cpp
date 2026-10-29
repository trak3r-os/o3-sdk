// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI learn (kit/app midi.h, docs/M2C-PLAN.md section 4, slice CP): Midi::learn() arms the next control change, which is bound to a parameter of a unit and from then on moves it
// (core/learn.h: PICKUP, JUMP, RELATIVE). A bound controller no longer reaches App::midi(); one parameter has one controller and a controller one parameter; the bindings are kept in
// `LRNB` of the kit's settings file and come back at boot. The settings codec's own rules for the chunk are in app_settings_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fake_midi.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/units/synth/synth.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace o3sdk;
using app::Button;
using platform::host::FakeMidi;
using Port = hal::midi::Port;
using units::Synth;

namespace {

class Blank final : public app::Screen {
 public:
  const char* title() const noexcept override { return "LEARN"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

class ProbeApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"LEARN", "1"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override {
    synth_id = ctx.engine().attach(synth);
    ctx.show(screen);
  }
  bool midi(app::Context&, const core::MidiMessage& m) noexcept override {
    heard.push_back(m);
    return true;
  }
  Synth synth;
  Blank screen;
  dsp::UnitId synth_id = dsp::kNoUnit;
  std::vector<core::MidiMessage> heard;
};

constexpr std::uint8_t cc_status(unsigned channel) { return core::midi_status(core::kMidiControl, static_cast<std::uint8_t>(channel)); }

struct Rig {
  ProbeApp app;
  FakeMidi midi;
  std::unique_ptr<sim::Harness> h;
  explicit Rig(platform::host::HostCard* card = nullptr) {
    h = std::make_unique<sim::Harness>(app, nullptr, &midi, card);
    REQUIRE(h->start());
    h->run_ms(20);
  }
  app::Midi& api() { return h->runtime().context().midi(); }
  dsp::Engine& engine() { return h->engine(); }
  float value(Synth::Param p) { return engine().param(app.synth_id, p); }
  void cc(unsigned channel, unsigned number, unsigned v, Port port = Port::Usb) {
    midi.in(port, cc_status(channel), static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(v));
    h->run_ms(6);
  }
  float norm(Synth::Param p) {
    const core::ParamDesc* d = engine().param_desc(app.synth_id, p);
    return d != nullptr ? core::param_norm(*d, value(p)) : -1.0f;
  }
  std::string state() {
    char buf[400];
    const std::size_t n = h->runtime().state_line(buf, sizeof buf);
    return std::string(buf, n);
  }
};

}  // namespace

TEST_CASE("learn: arming is refused for a unit or a parameter that does not exist, and the next control change binds, on its channel") {
  Rig r;
  CHECK_FALSE(r.api().learn(dsp::kNoUnit, Synth::Cutoff));
  CHECK_FALSE(r.api().learn(r.app.synth_id, 200));
  CHECK_FALSE(r.api().learn(7, Synth::Cutoff));
  CHECK_FALSE(r.api().learning());
  NEED(r.api().learn(r.app.synth_id, Synth::Cutoff));
  CHECK(r.api().learning());
  // A note, a program change and the channel-mode controllers (120 and up) are not what learn waits for.
  r.midi.in(Port::Usb, core::midi_status(core::kMidiNoteOn, 0), 60, 100);
  r.midi.in(Port::Usb, cc_status(0), 123, 0);
  r.h->run_ms(6);
  CHECK(r.api().learning());
  CHECK(r.api().bindings() == 0u);
  r.cc(3, 74, 40);  // channel 3
  CHECK_FALSE(r.api().learning());
  CHECK(r.api().bindings() == 1u);
  std::uint8_t cc = 0;
  std::uint8_t ch = 0;
  NEED(r.api().bound(r.app.synth_id, Synth::Cutoff, cc, ch));
  CHECK(cc == 74u);
  CHECK(ch == 3u);
  CHECK(r.api().learned_cc() == 74u);
  CHECK(r.api().learned_channel() == 3u);
  CHECK(r.h->runtime().system().strip_active());  // "LEARNED CC 74 CHANNEL 3"
  // The controller that was taken did not reach the app; the other messages did.
  for (const core::MidiMessage& m : r.app.heard) {
    CHECK(m.data1 != 74u);
  }
  CHECK(r.app.heard.size() == 2u);  // the note (no route took it) and controller 123
}

TEST_CASE("learn: a bound controller moves its parameter, PICKUP first (the knob must reach the value) and then the whole way") {
  Rig r;
  NEED(r.api().learn(r.app.synth_id, Synth::Cutoff));
  r.cc(1, 74, 10);  // binds; the first message is the binding's, it does not move anything
  const float start = r.norm(Synth::Cutoff);
  NEED(start > 0.1f && start < 0.9f);
  r.app.heard.clear();
  r.cc(1, 74, 0);  // far below the value: nothing moves
  r.cc(1, 74, 5);
  CHECK(r.norm(Synth::Cutoff) == doctest::Approx(start));
  r.cc(1, 74, 127);  // the sweep crosses the value: it is picked up and then follows the knob
  CHECK(r.norm(Synth::Cutoff) == doctest::Approx(1.0f).epsilon(0.001));
  r.cc(1, 74, 64);
  CHECK(r.norm(Synth::Cutoff) == doctest::Approx(64.0f / 127.0f).epsilon(0.001));
  CHECK(r.app.heard.empty());  // every one of them was the kit's
  // Another channel's controller 74 is not this binding's (it was bound on channel 1); it goes to the app.
  r.cc(4, 74, 100);
  CHECK(r.norm(Synth::Cutoff) == doctest::Approx(64.0f / 127.0f).epsilon(0.001));
  CHECK(r.app.heard.size() == 1u);
  // The value changed by someone else (here the app) has to be caught again.
  (void)r.engine().set_param(r.app.synth_id, Synth::Cutoff, 0.0f);
  const float low = r.norm(Synth::Cutoff);
  r.cc(1, 74, 90);
  CHECK(r.norm(Synth::Cutoff) == doctest::Approx(low));
}

TEST_CASE("learn: JUMP takes the knob's position at once, RELATIVE steps by the increment") {
  Rig r;
  NEED(r.api().learn(r.app.synth_id, Synth::Reso, core::LearnMode::Jump));
  r.cc(1, 20, 0);
  r.cc(1, 20, 127);
  CHECK(r.norm(Synth::Reso) == doctest::Approx(1.0f).epsilon(0.001));
  r.cc(1, 20, 0);
  CHECK(r.norm(Synth::Reso) < 0.001f);
  NEED(r.api().learn(r.app.synth_id, Synth::Sustain, core::LearnMode::Relative));
  r.cc(1, 21, 0);  // (the binding message: 0 is no increment)
  const float before = r.value(Synth::Sustain);
  r.cc(1, 21, 1);  // up one detent
  const float up = r.value(Synth::Sustain);
  CHECK(up > before);
  r.cc(1, 21, 127);  // down one detent (two's complement)
  CHECK(r.value(Synth::Sustain) == doctest::Approx(before).epsilon(0.01));
}

TEST_CASE("learn: one controller moves one parameter and a parameter has one controller, unlearn takes a binding off, the store has a limit") {
  Rig r;
  NEED(r.api().learn(r.app.synth_id, Synth::Cutoff));
  r.cc(1, 74, 1);
  NEED(r.api().learn(r.app.synth_id, Synth::Reso));
  r.cc(1, 74, 1);  // the same controller: Reso takes it from Cutoff
  CHECK(r.api().bindings() == 1u);
  std::uint8_t cc = 0;
  std::uint8_t ch = 0;
  CHECK_FALSE(r.api().bound(r.app.synth_id, Synth::Cutoff, cc, ch));
  CHECK(r.api().bound(r.app.synth_id, Synth::Reso, cc, ch));
  NEED(r.api().learn(r.app.synth_id, Synth::Reso));
  r.cc(1, 75, 1);  // the same parameter on another controller: it moves
  CHECK(r.api().bindings() == 1u);
  NEED(r.api().bound(r.app.synth_id, Synth::Reso, cc, ch));
  CHECK(cc == 75u);
  CHECK(r.api().unlearn(r.app.synth_id, Synth::Reso));
  CHECK_FALSE(r.api().unlearn(r.app.synth_id, Synth::Reso));
  CHECK(r.api().bindings() == 0u);
  // Every parameter of the synth, one controller each: eight bindings of the store's sixteen.
  unsigned bound = 0;
  for (unsigned p = 0; p < Synth::kParamCount; ++p) {
    if (r.api().learn(r.app.synth_id, static_cast<std::uint16_t>(p))) {
      r.cc(1, 30 + p, 1);
      bound += r.api().bindings() == bound + 1u ? 1u : 0u;
    }
  }
  CHECK(bound == Synth::kParamCount);
  r.api().unlearn_all();
  CHECK(r.api().bindings() == 0u);
  // Learn cancelled: the next controller is the app's.
  NEED(r.api().learn(r.app.synth_id, Synth::Cutoff));
  r.api().learn_cancel();
  r.app.heard.clear();
  r.cc(1, 74, 9);
  CHECK(r.api().bindings() == 0u);
  CHECK(r.app.heard.size() == 1u);
}

TEST_CASE("learn: the bindings are kept in the settings and come back at boot, and a binding of a unit the build does not have goes to the app") {
  platform::host::ScratchDir dir("learn-keep");
  NEED(dir.ok());
  platform::host::DirCard card(dir.path());
  Rig r(&card);
  NEED(r.api().learn(r.app.synth_id, Synth::Cutoff));
  r.cc(1, 74, 1);
  NEED(r.api().learn(r.app.synth_id, Synth::Sustain, core::LearnMode::Jump));
  r.cc(5, 11, 1);
  CHECK(r.api().bindings() == 2u);
  r.h->run_ms(2500);  // 1.5 s after the last change the settings are written
  NEED(r.h->reboot());
  r.h->run_ms(20);
  CHECK(r.api().bindings() == 2u);
  std::uint8_t cc = 0;
  std::uint8_t ch = 0;
  NEED(r.api().bound(r.app.synth_id, Synth::Sustain, cc, ch));
  CHECK(cc == 11u);
  CHECK(ch == 5u);
  r.cc(5, 11, 127);  // JUMP: it works without being picked up first
  CHECK(r.norm(Synth::Sustain) == doctest::Approx(1.0f).epsilon(0.001));
  CHECK(r.state().find("setup_errors=0") != std::string::npos);
}
