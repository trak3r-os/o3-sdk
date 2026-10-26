// SPDX-License-Identifier: GPL-3.0-or-later
//
// An app's MIDI (kit/app midi.h, runtime.h, docs/M2B2-PLAN.md section 3) in the simulator's harness on a probe app, with the host fake for the
// device (platform/host/fake_midi.h): routes put a note on or off on a unit at the frame it came in, everything else reaches App::midi() in
// the main loop, the MIDI section of the system Config edits the settings, the clock and the transport go out, the master's clock is followed,
// and a platform without a device is the runtime it was. The engine's side (the frames, the hooks) is dsp_follow_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/platform/host/fake_firmware.h>
#include <o3sdk/platform/host/fake_midi.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/units/synth/synth.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace o3sdk;
using app::Button;
using platform::host::FakeMidi;
using Port = hal::midi::Port;

namespace {

// An instrument that makes no sound and writes down the events it gets: in which block (counted from the first real one) and at which frame.
class RecUnit final : public dsp::Unit {
 public:
  struct Got {
    unsigned block;
    dsp::Event e;
  };
  RecUnit() : info_{"REC", dsp::UnitKind::Instrument, nullptr, 0, nullptr, 0} {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override {}
  void process(const dsp::ProcessArgs& a) noexcept override {
    if (a.frames == 0u) {
      return;  // (start()'s delivery of the stored values)
    }
    ++block;
    for (std::uint32_t i = 0; i < a.event_count; ++i) {
      got.push_back({block, a.events[i]});
    }
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      a.out_l[i] = 0.0f;
      a.out_r[i] = 0.0f;
    }
  }
  dsp::UnitInfo info_;
  unsigned block = 0;
  std::vector<Got> got;
};

// A hook that sends `want` notes a block (frame i * 2).
class Sender final : public dsp::Clocked {
 public:
  void block(dsp::BlockContext& ctx) noexcept override {
    ok = 0;
    for (unsigned i = 0; i < want; ++i) {
      ok += ctx.send_midi(core::midi_note_on(2, static_cast<std::uint8_t>(i), 100, static_cast<std::uint16_t>(i * 2u), ports)) ? 1u : 0u;
    }
  }
  unsigned want = 0;
  unsigned ok = 0;
  std::uint8_t ports = 0;
};

// A hook that ends a note of unit 0 at the last frame of every block it is on for (what a sequencer's note-off is).
class Offer final : public dsp::Clocked {
 public:
  void block(dsp::BlockContext& ctx) noexcept override {
    if (on) {
      (void)ctx.emit(unit, dsp::note_off(99, 63));
    }
  }
  bool on = false;
  dsp::UnitId unit = 0;
};

class ProbeScreen final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PROBE"; }
  void input(app::Context&, const app::Input&) noexcept override { ++inputs; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
  unsigned inputs = 0;
};

class ProbeApp final : public app::App {
 public:
  static constexpr unsigned kUnits = 5;
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"PROBE", "1.0"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override {
    for (unsigned i = 0; i < kUnits; ++i) {
      ids[i] = ctx.engine().attach(unit[i]);
      (void)ctx.engine().set_source(i, ids[i]);
    }
    (void)ctx.engine().add_hook(sender);
    offer.unit = ids[0];
    (void)ctx.engine().add_hook(offer);
    ctx.show(screen);
  }
  bool midi(app::Context&, const core::MidiMessage& m) noexcept override {
    heard.push_back(m);
    return true;
  }
  RecUnit unit[kUnits];
  dsp::UnitId ids[kUnits] = {};
  Sender sender;
  Offer offer;
  ProbeScreen screen;
  std::vector<core::MidiMessage> heard;
};

// A real sound: one synth, for the comparison of a platform with and without a MIDI device.
class SynthApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"SYNTH", "1.0"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override {
    id = ctx.engine().attach(synth);
    (void)ctx.engine().set_source(0, id);
    (void)ctx.midi().route(id, 0);
    ctx.show(screen);
  }
  units::Synth synth;
  dsp::UnitId id = dsp::kNoUnit;
  ProbeScreen screen;
};

// An app that does not override any hook.
class BareApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"BARE", "0"};
    return kInfo;
  }
  void setup(app::Context&) noexcept override {}
};

struct Rig {
  ProbeApp app;
  FakeMidi midi;
  platform::host::FakeFirmwareSink firmware;
  std::unique_ptr<sim::Harness> h;

  explicit Rig(bool with_midi = true, bool with_firmware = false) {
    firmware.add("O3SDK_HELLO.PTF", 612384);
    h = std::make_unique<sim::Harness>(app, with_firmware ? &firmware : nullptr, with_midi ? &midi : nullptr);
    REQUIRE(h->start());
    h->run_ms(20);
  }
  app::Context& ctx() { return h->runtime().context(); }
  app::Midi& api() { return ctx().midi(); }
  dsp::Engine& engine() { return h->engine(); }
  void tap(Button b) {
    h->press(b);
    h->run_ms(1);
    h->release(b);
    h->run_ms(1);
  }
  std::string state() {
    char buf[400];
    const std::size_t n = h->runtime().state_line(buf, sizeof buf);
    return std::string(buf, n);
  }
  bool has(const char* text) { return state().find(text) != std::string::npos; }
  const hal::midi::Config& config() { return ctx().midi().config(); }
  void edit(const hal::midi::Config& c) { ctx().midi().set_config(c); }
};

std::uint8_t status_on(unsigned channel) { return core::midi_status(core::kMidiNoteOn, static_cast<std::uint8_t>(channel)); }
std::uint8_t status_off(unsigned channel) { return core::midi_status(core::kMidiNoteOff, static_cast<std::uint8_t>(channel)); }

}  // namespace

TEST_CASE("midi: a note on a route's channel reaches its unit at the frame it came in, off and on with velocity 0 alike") {
  Rig r;
  NEED(r.api().route(r.app.ids[0], 3));
  const unsigned before = r.app.unit[0].block;
  r.midi.in_frame(17, Port::Din, status_on(3), 60, 100);
  r.midi.in_frame(40, Port::Din, status_off(3), 60, 64);
  r.midi.in_frame(41, Port::Din, status_on(3), 62, 90);
  r.midi.in_frame(55, Port::Usb, status_on(3), 62, 0);  // a note on with velocity 0 is a note off
  r.h->run_ms(10);
  const auto& got = r.app.unit[0].got;
  NEED(got.size() == 4u);
  CHECK(got[0].e.kind == dsp::EventKind::NoteOn);
  CHECK(got[0].e.note == 60u);
  CHECK(got[0].e.velocity == 100u);
  CHECK(got[0].e.frame == 17u);
  CHECK(got[1].e.kind == dsp::EventKind::NoteOff);
  CHECK(got[1].e.note == 60u);
  CHECK(got[1].e.frame == 40u);
  CHECK(got[2].e.kind == dsp::EventKind::NoteOn);
  CHECK(got[2].e.frame == 41u);
  CHECK(got[3].e.kind == dsp::EventKind::NoteOff);
  CHECK(got[3].e.note == 62u);
  CHECK(got[3].e.frame == 55u);
  for (const auto& g : got) {
    CHECK(g.block == got[0].block);  // all four in the one block that came next
    CHECK(g.block > before);
  }
  // The routed notes are the render's: they never reach the app.
  CHECK(r.app.heard.empty());
  // The other units heard nothing.
  CHECK(r.app.unit[1].got.empty());
}

TEST_CASE("midi: the controllers that end every note end a routed unit's, on its channel only") {
  Rig r;
  NEED(r.api().route(r.app.ids[0], 2));
  r.midi.in_frame(3, Port::Din, core::midi_status(core::kMidiControl, 2), core::kCcAllNotesOff, 0);
  r.midi.in_frame(9, Port::Din, core::midi_status(core::kMidiControl, 2), core::kCcAllSoundOff, 0);
  r.midi.in_frame(12, Port::Din, core::midi_status(core::kMidiControl, 5), core::kCcAllNotesOff, 0);
  r.h->run_ms(10);
  const auto& got = r.app.unit[0].got;
  NEED(got.size() == 2u);
  CHECK(got[0].e.kind == dsp::EventKind::AllNotesOff);
  CHECK(got[0].e.frame == 3u);
  CHECK(got[1].e.kind == dsp::EventKind::AllNotesOff);
  CHECK(got[1].e.frame == 9u);
  // The other channel's reached the app: it is a controller no route took.
  NEED(r.app.heard.size() == 1u);
  CHECK(r.app.heard[0].status == core::midi_status(core::kMidiControl, 5));
}

TEST_CASE("midi: a route on channel 0 follows the user's channel, omni until the user picks one; a fixed channel does not") {
  Rig r;
  NEED(r.api().route(r.app.ids[0], 0));
  NEED(r.api().route(r.app.ids[1], 1));
  r.midi.in(Port::Din, status_on(4), 60, 100);
  r.midi.in(Port::Din, status_on(1), 61, 100);
  r.h->run_ms(10);
  CHECK(r.app.unit[0].got.size() == 2u);  // omni: both
  CHECK(r.app.unit[1].got.size() == 1u);  // channel 1 only
  CHECK(r.app.unit[1].got[0].e.note == 61u);

  hal::midi::Config c = r.config();
  c.channel = 4;
  r.edit(c);
  r.app.unit[0].got.clear();
  r.app.unit[1].got.clear();
  r.midi.in(Port::Din, status_on(4), 62, 100);
  r.midi.in(Port::Din, status_on(1), 63, 100);
  r.midi.in(Port::Din, status_on(2), 64, 100);
  r.h->run_ms(10);
  NEED(r.app.unit[0].got.size() == 1u);  // the user's channel 4
  CHECK(r.app.unit[0].got[0].e.note == 62u);
  NEED(r.app.unit[1].got.size() == 1u);  // its own channel 1, whatever the user chose
  CHECK(r.app.unit[1].got[0].e.note == 63u);
  NEED(r.app.heard.size() == 1u);  // channel 2 nothing listens to
  CHECK(r.app.heard[0].data1 == 64u);
}

TEST_CASE("midi: at most four routes, a unit has one, and a route to nothing is refused") {
  Rig r;
  app::Midi& m = r.api();
  CHECK(m.routes() == 0u);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(m.route(r.app.ids[i], static_cast<std::uint8_t>(i + 1)));
  }
  CHECK(m.routes() == 4u);
  CHECK_FALSE(m.route(r.app.ids[4], 9));  // the fifth
  CHECK(m.routes() == 4u);
  CHECK(m.route(r.app.ids[0], 7));        // a unit already routed takes the new channel
  CHECK(m.routes() == 4u);
  r.midi.in(Port::Din, status_on(7), 60, 100);
  r.midi.in(Port::Din, status_on(1), 61, 100);
  r.h->run_ms(10);
  NEED(r.app.unit[0].got.size() == 1u);
  CHECK(r.app.unit[0].got[0].e.note == 60u);
  CHECK(r.app.heard.size() == 1u);  // channel 1 has no route any more
  CHECK(m.unroute(r.app.ids[1]));
  CHECK_FALSE(m.unroute(r.app.ids[1]));
  CHECK(m.routes() == 3u);
  CHECK(m.route(r.app.ids[4], 9));  // there is room again
  CHECK_FALSE(m.route(dsp::kNoUnit, 1));
  CHECK_FALSE(m.route(200, 1));  // never attached
  CHECK_FALSE(m.route(r.app.ids[0], 17));
  CHECK(m.routes() == 4u);
}

TEST_CASE("midi: everything no route took reaches App::midi() in the main loop, in order, with its port") {
  Rig r;
  NEED(r.api().route(r.app.ids[0], 1));
  r.midi.in(Port::Din, core::midi_status(core::kMidiControl, 1), 7, 100);
  r.midi.in(Port::Usb, core::midi_status(core::kMidiProgram, 2), 5, 0);
  r.midi.in(Port::Din, core::midi_status(core::kMidiPitchBend, 1), 0, 64);
  r.midi.in(Port::Usb, status_on(9), 36, 120);  // a note of a channel no route listens to
  r.midi.in(Port::Din, status_on(1), 60, 100);  // routed: not here
  r.midi.in(Port::Din, core::midi_status(core::kMidiPolyPressure, 1), 60, 50);
  r.h->run_ms(10);
  NEED(r.app.heard.size() == 5u);
  CHECK(r.app.heard[0].status == core::midi_status(core::kMidiControl, 1));
  CHECK(r.app.heard[0].data1 == 7u);
  CHECK(r.app.heard[0].data2 == 100u);
  CHECK(r.app.heard[0].ports == core::kMidiDin);
  CHECK(r.app.heard[1].status == core::midi_status(core::kMidiProgram, 2));
  CHECK(r.app.heard[1].data1 == 5u);
  CHECK(r.app.heard[1].ports == core::kMidiUsb);
  CHECK(r.app.heard[2].status == core::midi_status(core::kMidiPitchBend, 1));
  CHECK(r.app.heard[3].status == status_on(9));
  CHECK(r.app.heard[3].ports == core::kMidiUsb);
  CHECK(r.app.heard[4].status == core::midi_status(core::kMidiPolyPressure, 1));
  CHECK(r.app.unit[0].got.size() == 1u);

  // The raw bytes of a port, with running status: two note ons in one go; real-time bytes in the middle are skipped.
  r.app.heard.clear();
  const std::uint8_t bytes[] = {0x95, 40, 100, 0xF8, 41, 100, 0xC5, 3};
  CHECK(r.midi.in_bytes(Port::Din, bytes, sizeof bytes) == 3u);
  r.h->run_ms(10);
  CHECK(r.app.heard.size() == 3u);

  // A bare app has the default hook: it takes the same traffic and does nothing with it.
  BareApp bare;
  FakeMidi fake;
  sim::Harness h(bare, nullptr, &fake);
  REQUIRE(h.start());
  fake.in(Port::Din, status_on(1), 60, 100);
  h.run_ms(10);
  CHECK(fake.status().rx == 1u);
  CHECK_FALSE(bare.midi(h.runtime().context(), core::midi_note_on(1, 60, 100)));
}

TEST_CASE("midi: a flood past the device's ring is lost and counted, and the app still gets what fit") {
  Rig r;
  for (unsigned i = 0; i < 100; ++i) {
    r.midi.in(Port::Din, core::midi_status(core::kMidiControl, 1), static_cast<std::uint8_t>(i), 1);
  }
  r.h->run_ms(20);
  CHECK(r.app.heard.size() == FakeMidi::kRing);
  CHECK(r.api().status().dropped == 100u - FakeMidi::kRing);
  CHECK(r.api().status().rx == 100u);
}

TEST_CASE("midi: send_midi takes 32 a block, on the ports the message names or the user's, and the main loop can send too") {
  Rig r;
  r.app.sender.want = 40;
  r.h->run_ms(3);
  CHECK(r.app.sender.ok == 32u);
  r.app.sender.want = 0;
  r.h->run_ms(3);
  const std::size_t sent = r.midi.sent.size();
  NEED(sent >= 32u);
  CHECK(sent % 32u == 0u);  // 32 in each block that ran with 40
  CHECK(r.engine().stats().dropped_midi == 8u * (sent / 32u));
  // Frames: the hook's frame i * 2 inside the block it was sent in; ports 0 became the user's, both by default.
  for (std::size_t i = 0; i < 32; ++i) {
    CHECK((r.midi.sent[i].sample - r.midi.sent[0].sample) == i * 2u);
    CHECK(r.midi.sent[i].ports == core::kMidiAllPorts);
    CHECK(r.midi.sent[i].status == status_on(2));
  }
  CHECK(r.midi.sent[0].sample % 64u == 0u);  // the block's first sample

  r.midi.sent.clear();
  hal::midi::Config c = r.config();
  c.out_ports = core::kMidiDin;
  r.edit(c);
  r.app.sender.want = 2;
  r.h->run_ms(3);
  NEED(!r.midi.sent.empty());
  CHECK(r.midi.sent[0].ports == core::kMidiDin);
  r.app.sender.ports = core::kMidiUsb;  // a message that names its port
  r.midi.sent.clear();
  r.h->run_ms(3);
  NEED(!r.midi.sent.empty());
  CHECK(r.midi.sent[0].ports == core::kMidiUsb);
  r.app.sender.want = 0;
  r.h->run_ms(3);

  // The main loop sends: one message, on the user's ports when it names none; none at all when the user has no ports.
  r.midi.sent.clear();
  CHECK(r.api().send(core::midi_control(1, 74, 99)));
  NEED(r.midi.sent.size() == 1u);
  CHECK(r.midi.sent[0].ports == core::kMidiDin);
  CHECK(r.midi.sent[0].data1 == 74u);
  CHECK(r.midi.sent[0].data2 == 99u);
  c.out_ports = 0;
  r.edit(c);
  CHECK_FALSE(r.api().send(core::midi_control(1, 74, 99)));
  CHECK(r.api().send(core::midi_control(1, 74, 99, 0, core::kMidiUsb)));  // (a message that names its port still goes)
  NEED(r.midi.sent.size() == 2u);
  CHECK(r.midi.sent[1].ports == core::kMidiUsb);
}

TEST_CASE("midi: the clock and the transport go out while the engine plays, on the user's ports, as the settings say") {
  Rig r;
  CHECK(r.engine().set_tempo(120'000));
  CHECK(r.engine().play());
  r.h->run_ms(2000);
  // 120 BPM: 48 clocks a second; the Start is the first thing out, once.
  CHECK(r.midi.count(core::kMidiStart) == 1u);
  CHECK(r.midi.count(core::kMidiClock) >= 95u);
  CHECK(r.midi.count(core::kMidiClock) <= 97u);
  NEED(!r.midi.sent.empty());
  CHECK(r.midi.sent[0].status == core::kMidiStart);
  CHECK(r.midi.sent[0].ports == core::kMidiAllPorts);
  // The clocks are 918.75 samples apart, each on the first sample at or after its tick: between 918 and 919 apart.
  std::vector<std::uint64_t> at;
  for (const auto& s : r.midi.sent) {
    if (s.status == core::kMidiClock) {
      at.push_back(s.sample);
    }
  }
  NEED(at.size() > 10u);
  CHECK(at[0] % 64u == 0u);  // tick 0 is the Start's first sample
  for (std::size_t i = 1; i < at.size(); ++i) {
    const std::uint64_t d = at[i] - at[i - 1];
    CHECK(d >= 918u);
    CHECK(d <= 920u);
  }
  CHECK(at[at.size() - 1] - at[0] >= (at.size() - 1) * 918u);
  CHECK(r.engine().stop());
  r.h->run_ms(20);
  CHECK(r.midi.count(core::kMidiStop) == 1u);
  const unsigned clocks = r.midi.count(core::kMidiClock);
  r.h->run_ms(300);
  CHECK(r.midi.count(core::kMidiClock) == clocks);  // no clock while stopped

  // CLOCK OUT off: the transport still goes; TRANSPORT OUT off: the clock still goes.
  hal::midi::Config c = r.config();
  c.clock_out = false;
  r.edit(c);
  r.midi.sent.clear();
  CHECK(r.engine().play());
  r.h->run_ms(500);
  CHECK(r.midi.count(core::kMidiClock) == 0u);
  CHECK(r.midi.count(core::kMidiStart) == 1u);
  c.clock_out = true;
  c.transport_out = false;
  r.edit(c);
  r.midi.sent.clear();
  CHECK(r.engine().stop());
  r.h->run_ms(20);
  CHECK(r.engine().play());
  r.h->run_ms(500);
  CHECK(r.midi.count(core::kMidiClock) > 10u);
  CHECK(r.midi.count(core::kMidiStart) == 0u);
  CHECK(r.midi.count(core::kMidiStop) == 0u);
  // No ports: nothing goes out at all.
  c.out_ports = 0;
  c.transport_out = true;
  r.edit(c);
  r.midi.sent.clear();
  r.h->run_ms(300);
  CHECK(r.midi.sent.empty());
}

TEST_CASE("midi: FOLLOW on, the engine's transport is the master's: tempo, Start, Stop, and Play waits for the master") {
  Rig r;
  r.engine().set_tempo(100'000);
  r.h->run_ms(5);
  r.midi.clock_start(140'000);  // a master that is there before FOLLOW is on is not followed
  r.h->run_ms(20);
  CHECK_FALSE(r.engine().following());
  CHECK(r.engine().tempo() == 100'000u);
  CHECK_FALSE(r.api().status().following);

  hal::midi::Config c = r.config();
  c.follow = true;
  r.edit(c);
  r.midi.clock_tempo(140'000);
  r.h->run_ms(20);
  CHECK(r.engine().following());
  CHECK(r.engine().tempo() == 140'000u);
  CHECK_FALSE(r.engine().playing());
  CHECK(r.api().status().following);
  CHECK(r.api().status().locked);
  CHECK(r.api().status().tempo_mbpm == 140'000u);
  CHECK(r.has("tempo=140000"));
  CHECK(r.has("playing=0"));

  r.midi.clock_start(140'000);
  r.h->run_ms(100);
  CHECK(r.engine().playing());
  CHECK(r.engine().stats().playing);
  CHECK(r.has("playing=1"));
  r.midi.clock_tempo(150'000);
  r.h->run_ms(20);
  CHECK(r.engine().tempo() == 150'000u);
  CHECK(r.engine().stats().tempo_mbpm == 150'000u);
  r.midi.clock_stop(150'000);
  r.h->run_ms(20);
  CHECK_FALSE(r.engine().playing());
  CHECK(r.engine().tempo() == 150'000u);  // the master is still there

  // Play under the master asks the device, and the run begins on the master's grid at the next block.
  CHECK(r.engine().play());
  r.h->run_ms(20);
  NEED(r.midi.requests.size() == 1u);
  CHECK(r.midi.requests[0] == core::TransportRequest::Play);
  CHECK(r.engine().playing());
  CHECK(r.engine().stop());
  r.h->run_ms(20);
  NEED(r.midi.requests.size() == 2u);
  CHECK(r.midi.requests[1] == core::TransportRequest::Stop);
  CHECK_FALSE(r.engine().playing());

  // TRANSPORT IN off: the master's Start is ignored; the clock is followed.
  c.transport_in = false;
  r.edit(c);
  r.midi.clock_start(150'000);
  r.h->run_ms(20);
  CHECK_FALSE(r.engine().playing());
  CHECK(r.engine().tempo() == 150'000u);

  // A master on the other port is not the followed one.
  c.transport_in = true;
  c.follow_port = Port::Usb;
  r.edit(c);
  r.midi.clock_port(Port::Din);
  r.midi.clock_tempo(160'000);
  r.h->run_ms(20);
  CHECK_FALSE(r.engine().following());
  CHECK(r.engine().tempo() == 100'000u);
  c.follow_port = Port::Din;
  r.edit(c);
  r.h->run_ms(20);
  CHECK(r.engine().following());
  CHECK(r.engine().tempo() == 160'000u);

  // FOLLOW off lets the master go: the engine's own tempo is back and the transport stopped.
  r.midi.clock_start(160'000);
  r.h->run_ms(50);
  CHECK(r.engine().playing());
  c.follow = false;
  r.edit(c);
  r.h->run_ms(20);
  CHECK_FALSE(r.engine().following());
  CHECK_FALSE(r.engine().playing());
  CHECK(r.engine().tempo() == 100'000u);
  // The fake kept the contract through all of it (hal/midi.h): the stream's position never jumped.
  CHECK(r.midi.continuity.ok());
  CHECK(r.midi.continuity.blocks() > 100u);
  CHECK(r.midi.continuity.starts() >= 2u);
}

TEST_CASE("midi: a master that is switched off is flywheeled for 48 clocks, then the transport stops and the engine's clock is back") {
  Rig r;
  hal::midi::Config c = r.config();
  c.follow = true;
  r.edit(c);
  r.midi.clock_start(120'000);
  r.h->run_ms(100);
  CHECK(r.engine().playing());
  r.midi.clock_off();
  r.h->run_ms(900);  // 24 clocks at 120 BPM go by in half a second: still following at 450 ms
  CHECK(r.engine().playing());
  CHECK(r.engine().following());
  r.h->run_ms(1200);  // past 48 clocks (one second)
  CHECK_FALSE(r.engine().playing());
  CHECK_FALSE(r.engine().following());
  CHECK(r.engine().tempo() == 120'000u);
  CHECK(r.engine().play());  // its own transport
  r.h->run_ms(50);
  CHECK(r.engine().playing());
  CHECK(r.midi.continuity.ok());
}

TEST_CASE("midi: a song position is a locate, not a jump, and the fake's stream keeps the contract through Continue") {
  Rig r;
  hal::midi::Config c = r.config();
  c.follow = true;
  r.edit(c);
  r.midi.clock_continue(120'000, 8);
  r.h->run_ms(300);
  CHECK(r.engine().playing());
  r.midi.clock_stop(120'000);
  r.h->run_ms(20);
  r.midi.clock_continue(120'000, 24);
  r.h->run_ms(300);
  CHECK(r.midi.continuity.ok());
  CHECK(r.midi.continuity.locates() == 2u);
  CHECK(r.midi.continuity.starts() == 0u);
  // The clock the engine sends while it follows is the master's grid: an SPP, then Continue, as it is not a Start.
  CHECK(r.midi.count(core::kMidiContinue) == 2u);
  CHECK(r.midi.count(core::kMidiStart) == 0u);
  CHECK(r.midi.count(core::kMidiSongPosition) == 2u);
  bool spp_24 = false;
  for (const auto& s : r.midi.sent) {
    spp_24 = spp_24 || (s.status == core::kMidiSongPosition && s.data1 == 24u && s.data2 == 0u);
  }
  CHECK(spp_24);
}

TEST_CASE("midi: a flood of routed notes cannot push a hook's note-off out of the unit's list") {
  Rig r;
  NEED(r.api().route(r.app.ids[0], 1));
  r.app.offer.on = true;
  for (unsigned i = 0; i < 32; ++i) {  // 32 note ons in one block, all for one unit: the most a block takes from the device
    r.midi.in_frame(i, Port::Din, status_on(1), static_cast<std::uint8_t>(30 + i), 100);
  }
  r.h->run_ms(10);
  const auto& got = r.app.unit[0].got;
  unsigned first_block = 0;
  for (const auto& g : got) {
    if (g.e.kind == dsp::EventKind::NoteOn) {
      first_block = g.block;
      break;
    }
  }
  NEED(first_block != 0u);
  unsigned ons = 0;
  bool off = false;
  for (const auto& g : got) {
    if (g.block != first_block) {
      continue;
    }
    ons += g.e.kind == dsp::EventKind::NoteOn ? 1u : 0u;
    off = off || (g.e.kind == dsp::EventKind::NoteOff && g.e.note == 99u && g.e.frame == 63u);
  }
  CHECK(ons == dsp::kMaxInjectPerUnit);  // the first 16, in order
  CHECK(off);                            // the hook's note-off was not pushed out
  CHECK(r.engine().stats().dropped_events == dsp::kMaxEventsPerUnit - dsp::kMaxInjectPerUnit);
}

TEST_CASE("midi: while the input is locked or an install has begun the app hears nothing, and what the device has is dropped") {
  {
    Rig r;
    r.h->runtime().lock_input(true);  // (the power-off)
    for (unsigned i = 0; i < 5; ++i) {
      r.midi.in(Port::Din, core::midi_status(core::kMidiControl, 1), static_cast<std::uint8_t>(i), 1);
    }
    r.h->run_ms(10);
    CHECK(r.app.heard.empty());
    core::MidiMessage m;
    CHECK_FALSE(r.midi.poll(m));  // read and dropped, not left to pile up
    r.h->runtime().lock_input(false);
    r.midi.in(Port::Din, core::midi_status(core::kMidiControl, 1), 9, 1);
    r.h->run_ms(10);
    NEED(r.app.heard.size() == 1u);
    CHECK(r.app.heard[0].data1 == 9u);
  }
  {
    Rig r(true, true);
    r.tap(Button::Config);  // FIRMWARE: the list, the question, the check, INSTALL
    r.tap(Button::F1);
    r.tap(Button::F1);
    r.h->run_ms(20);
    r.tap(Button::F1);
    NEED(r.h->runtime().system().locked());
    r.midi.in(Port::Din, core::midi_status(core::kMidiControl, 1), 7, 1);
    r.h->run_ms(10);
    CHECK(r.app.heard.empty());
    core::MidiMessage m;
    CHECK_FALSE(r.midi.poll(m));
  }
}

TEST_CASE("midi: a play() or stop() made while following is not lost when the master is let go before the main loop hands it over") {
  Rig r;
  hal::midi::Config c = r.config();
  c.follow = true;
  r.edit(c);
  r.midi.clock_tempo(130'000);
  r.h->run_ms(20);
  CHECK(r.engine().following());
  CHECK(r.engine().play());  // a request: the harness polls only when time moves
  c.follow = false;
  r.edit(c);
  float l[64];
  float rr[64];
  r.engine().render(nullptr, nullptr, l, rr, 64);  // a block with no frame of a master: it is let go before the request is handed over
  CHECK_FALSE(r.engine().following());
  r.h->runtime().poll();
  CHECK(r.midi.requests.empty());  // not for a device that is not followed any more
  r.h->run_ms(20);
  CHECK(r.engine().playing());  // the engine's own play(), made by the runtime
  CHECK(r.engine().stats().playing);

  // The same for stop().
  c.follow = true;
  r.edit(c);
  r.midi.clock_start(130'000);
  r.h->run_ms(20);
  CHECK(r.engine().following());
  CHECK(r.engine().playing());
  CHECK(r.engine().stop());
  c.follow = false;
  r.edit(c);
  r.engine().render(nullptr, nullptr, l, rr, 64);
  CHECK_FALSE(r.engine().following());
  r.h->runtime().poll();
  CHECK(r.midi.requests.empty());
  r.h->run_ms(20);
  CHECK_FALSE(r.engine().playing());
  CHECK_FALSE(r.engine().stats().playing);
}

TEST_CASE("midi: the MIDI section of the system Config edits the settings, row by row, and the device gets each change") {
  Rig r;
  const unsigned configs = r.midi.configs;
  CHECK(configs >= 1u);  // the runtime handed the device the kit's defaults at start()
  CHECK(r.midi.config().channel == 0u);
  CHECK(r.midi.config().out_channel == 1u);
  CHECK(r.midi.config().out_ports == core::kMidiAllPorts);
  CHECK(r.midi.config().clock_out);
  CHECK(r.midi.config().transport_out);
  CHECK(r.midi.config().transport_in);
  CHECK_FALSE(r.midi.config().follow);

  // No update path in this rig: MIDI is the first section.
  CHECK(r.has("sys=0 section=midi"));
  r.tap(Button::Config);
  CHECK(r.has("sys=1 section=midi"));
  CHECK(r.ctx().screen() != nullptr);
  const unsigned before_inputs = r.app.screen.inputs;

  r.h->turn(3);  // IN CHANNEL: omni, 3 detents up
  r.h->run_ms(2);
  CHECK(r.config().channel == 3u);
  CHECK(r.midi.config().channel == 3u);
  CHECK(r.midi.configs > configs);
  r.h->turn(-10);  // (clamped at omni)
  r.h->run_ms(2);
  CHECK(r.config().channel == 0u);
  r.h->turn(40);   // (and at 16)
  r.h->run_ms(2);
  CHECK(r.config().channel == 16u);
  r.tap(Button::F1);  // F1 steps by one and wraps: 16 -> omni
  CHECK(r.config().channel == 0u);

  r.tap(Button::Down);  // OUT CHANNEL
  r.h->turn(4);
  r.h->run_ms(2);
  CHECK(r.config().out_channel == 5u);
  r.h->turn(-50);
  r.h->run_ms(2);
  CHECK(r.config().out_channel == 1u);  // (never below 1)

  r.tap(Button::Down);  // OUT PORTS: both -> (F1 wraps) none -> DIN -> USB -> both
  r.tap(Button::F1);
  CHECK(r.config().out_ports == 0u);
  r.tap(Button::F1);
  CHECK(r.config().out_ports == core::kMidiDin);
  r.tap(Button::F1);
  CHECK(r.config().out_ports == core::kMidiUsb);
  r.tap(Button::F1);
  CHECK(r.config().out_ports == core::kMidiAllPorts);

  r.tap(Button::Down);  // CLOCK OUT
  r.tap(Button::F1);
  CHECK_FALSE(r.config().clock_out);
  r.tap(Button::Down);  // TRANSPORT OUT: the encoder toggles too
  r.h->turn(1);
  r.h->run_ms(2);
  CHECK_FALSE(r.config().transport_out);
  r.tap(Button::Down);  // FOLLOW CLOCK
  r.tap(Button::F1);
  CHECK(r.config().follow);
  r.tap(Button::Down);  // FOLLOW PORT
  r.tap(Button::F1);
  CHECK(r.config().follow_port == Port::Usb);
  r.tap(Button::Down);  // TRANSPORT IN
  r.tap(Button::F1);
  CHECK_FALSE(r.config().transport_in);
  r.tap(Button::Down);  // (the last row: stays)
  r.tap(Button::F1);
  CHECK(r.config().transport_in);

  // The device has the last value of every field.
  const hal::midi::Config& d = r.midi.config();
  CHECK(d.channel == 0u);
  CHECK(d.out_channel == 1u);
  CHECK(d.out_ports == core::kMidiAllPorts);
  CHECK_FALSE(d.clock_out);
  CHECK_FALSE(d.transport_out);
  CHECK(d.follow);
  CHECK(d.follow_port == Port::Usb);
  CHECK(d.transport_in);

  // Going up the rows: UP moves back, and the encoder edits the row it is on.
  r.tap(Button::Up);  // FOLLOW PORT
  r.h->turn(1);
  r.h->run_ms(2);
  CHECK(r.config().follow_port == Port::Din);

  // Nothing of this reached the app: the screen is the system's.
  CHECK(r.app.screen.inputs == before_inputs);

  // The sections: RIGHT goes to DISPLAY, INFO, then round to MIDI; LEFT goes back.
  r.tap(Button::Right);
  CHECK(r.has("section=display"));
  r.tap(Button::Right);
  CHECK(r.has("section=info"));
  r.tap(Button::Right);
  CHECK(r.has("section=midi"));
  r.tap(Button::Left);
  CHECK(r.has("section=info"));
  r.tap(Button::Left);
  r.tap(Button::Left);
  CHECK(r.has("section=midi"));
  r.h->runtime().draw();  // (the page draws: with the status of a following device, too)
  r.midi.clock_start(133'300);
  r.h->run_ms(50);
  r.h->runtime().draw();
  CHECK(r.h->runtime().frame_failures() == 0u);
  CHECK(r.h->display().render_failures() == 0u);

  r.tap(Button::Config);
  CHECK_FALSE(r.h->runtime().system_open());
}

TEST_CASE("midi: with an update path the sections run FIRMWARE, MIDI, DISPLAY, INFO") {
  Rig r(true, true);
  CHECK(r.has("section=firmware"));
  r.tap(Button::Config);
  CHECK(r.has("sys=1 section=firmware"));
  r.tap(Button::Right);
  CHECK(r.has("section=midi"));
  r.tap(Button::Right);
  CHECK(r.has("section=display"));
  r.tap(Button::Right);
  CHECK(r.has("section=info"));
  r.tap(Button::Right);
  CHECK(r.has("section=firmware"));
  r.tap(Button::Left);
  CHECK(r.has("section=info"));
}

TEST_CASE("midi: out-of-range settings are forced into range, and the user's channel reaches the routes") {
  Rig r;
  hal::midi::Config c;
  c.channel = 99;
  c.out_channel = 0;
  c.out_ports = 0xFF;
  c.follow_port = static_cast<Port>(7);
  r.edit(c);
  CHECK(r.config().channel == 0u);
  CHECK(r.config().out_channel == 1u);
  CHECK(r.config().out_ports == core::kMidiAllPorts);
  CHECK(r.config().follow_port == Port::Din);
  CHECK(r.midi.config().out_ports == core::kMidiAllPorts);
}

TEST_CASE("midi: without a device the runtime is what it was: no MIDI section, no traffic, the engine's own transport, the same sound") {
  Rig none(false);
  CHECK_FALSE(none.api().available());
  CHECK(none.api().route(none.app.ids[0], 1));  // kept, harmlessly
  CHECK_FALSE(none.api().send(core::midi_note_on(1, 60, 100)));
  const hal::midi::Status s = none.api().status();
  CHECK_FALSE(s.following);
  CHECK(s.rx == 0u);
  CHECK(s.tx == 0u);
  none.api().set_config(hal::midi::Config{});  // no crash
  CHECK(none.has("sys=0 section=display"));
  none.tap(Button::Config);
  CHECK(none.has("sys=1 section=display"));
  none.tap(Button::Right);
  CHECK(none.has("section=info"));
  none.tap(Button::Right);
  CHECK(none.has("section=display"));  // MIDI is not one of the sections
  none.tap(Button::Config);
  CHECK(none.engine().play());
  none.engine().set_tempo(90'000);
  none.h->run_ms(100);
  CHECK(none.engine().playing());
  CHECK(none.engine().tempo() == 90'000u);
  CHECK_FALSE(none.engine().following());
  CHECK(none.engine().take_transport_request() == core::TransportRequest::None);

  // A device that is there but idle (no traffic, FOLLOW off) changes nothing either: the same notes give the same samples.
  SynthApp a;
  SynthApp b;
  FakeMidi fake;
  sim::Harness ha(a);
  sim::Harness hb(b, nullptr, &fake);
  REQUIRE(ha.start());
  REQUIRE(hb.start());
  ha.record(true);
  hb.record(true);
  for (sim::Harness* h : {&ha, &hb}) {
    h->engine().set_tempo(110'000);
    h->run_ms(10);
    h->engine().play();
    (void)h->engine().note_on(0, 57, 110);
    h->run_ms(300);
    (void)h->engine().note_off(0, 57);
    h->run_ms(100);
  }
  CHECK(ha.audio().size() == hb.audio().size());
  CHECK(ha.audio() == hb.audio());
  CHECK(ha.engine().stats().pos == hb.engine().stats().pos);
  bool sounded = false;
  for (const std::int16_t v : ha.audio()) {
    sounded = sounded || std::abs(v) > 100;
  }
  CHECK(sounded);  // (and it is not silence that was compared)
}

TEST_CASE("midi: the host fake keeps the contract of hal/midi.h: late input lands on frame 0, notes wait for the block they belong to") {
  FakeMidi f;
  core::MidiMessage in[8];
  core::ClockFrame clock;
  f.in(Port::Din, status_on(1), 60, 100, 70);  // sample 70: the second block (64 .. 127), frame 6
  f.in(Port::Din, status_on(1), 61, 100, 5);   // sample 5: the first block
  CHECK(f.begin_block(64, in, 8, clock) == 1u);
  CHECK(in[0].data1 == 61u);
  CHECK(in[0].frame == 5u);
  CHECK_FALSE(clock.follow);
  f.end_block(clock, nullptr, 0);
  CHECK(f.begin_block(64, in, 8, clock) == 1u);
  CHECK(in[0].data1 == 60u);
  CHECK(in[0].frame == 6u);
  f.end_block(clock, nullptr, 0);
  CHECK(f.block_start() == 128u);
  f.in(Port::Din, status_on(1), 62, 100, 3);  // late: it lands on frame 0
  f.in(Port::Din, status_on(1), 63, 100, 200);
  f.in(Port::Din, status_on(1), 64, 100, 129);
  CHECK(f.begin_block(64, in, 1, clock) == 1u);  // a cap of one: the rest wait
  CHECK(in[0].data1 == 62u);
  CHECK(in[0].frame == 0u);
  f.end_block(clock, nullptr, 0);
  CHECK(f.begin_block(64, in, 8, clock) == 2u);  // block 192 .. 255: the one that waited (now late), then sample 200
  CHECK(in[0].data1 == 64u);
  CHECK(in[0].frame == 0u);
  CHECK(in[1].data1 == 63u);
  CHECK(in[1].frame == 8u);
}
