// SPDX-License-Identifier: GPL-3.0-or-later
//
// MidiService (midi/midi_service.h, docs/M2B2-PLAN.md slice MD): the hal::midi::Device of the MK66 over the timing core, tested against the engine it serves. The rig is the
// image's wiring without the registers: a real dsp::Engine and a clocked hook shaped like o3-303's sequencer, the service called the way Runtime::render_block calls it
// (begin_block, Engine::follow, render, end_block), the rings the interrupts would fill, and a virtual DIN wire that the emitter is polled on. The master is an external
// clock whose edges happen at cycle-counter times the rig derives from the audio clock (a sample is HEARD at its cycle count plus the TX ring's render-ahead), with the
// stamps the interrupts would give them, so what the service computes can be compared with the master's own grid: the steps of the hook must land on it, the position must
// never jump (core::ClockContinuity over every frame), and no tick may be skipped or owned twice (TickAudit). What only hardware can show (the interrupt latency, the wire,
// the CPU of a block) is the unit's: docs/M2B2-PLAN.md section 6, MD row.
#include <doctest/doctest.h>

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/core/clock_continuity.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/platform/mk66/midi/midi_service.h>
#include <o3sdk/platform/mk66/midi/usb_bind.h>
#include <o3sdk/platform/mk66/midi_config.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "support/need.h"
#include "support/rng.h"

namespace midi = o3sdk::platform::mk66::midi;
namespace core = o3sdk::core;
namespace dsp = o3sdk::dsp;
namespace hmidi = o3sdk::hal::midi;
using o3sdk::test::Rng;

namespace {

constexpr std::uint32_t kFrames = 64;
constexpr std::uint32_t kStep = dsp::kTicksPerSixteenth;  // 240 ticks, a MIDI beat
constexpr std::uint32_t kClockTicks = dsp::kTicksPerMidiClock;
constexpr std::uint32_t kLead = o3sdk::platform::mk66::midi_cfg::kAudioLeadCycles;
constexpr std::uint64_t kByte = midi::din::kByteCycles;
constexpr double kCps = static_cast<double>(core::kRefCyclesPerSample.num) / static_cast<double>(core::kRefCyclesPerSample.den);  // cycles per sample

std::uint32_t u32(double v) { return static_cast<std::uint32_t>(static_cast<std::uint64_t>(v)); }

// ---- the hook: a sequencer in the shape of o3-303's ----------------------------------------------------------------------------------------------
// A run begins at the first whole tick the transport has not passed, ends on a stop or a restart and walks its own grid with frame_at(); every step is recorded with the
// timeline sample it landed on. It also sends the notes of an optional MIDI test (send_midi).
class RunHook final : public dsp::Clocked {
 public:
  struct Hit {
    std::uint64_t sample;  // first_sample + block * 64 + frame
    std::uint64_t tick;
  };
  void block(dsp::BlockContext& ctx) noexcept override {
    const dsp::Transport& t = ctx.transport();
    const std::uint64_t base = first + blocks_++ * kFrames;
    if (t.stopped) {
      ++stopped_;
      running_ = false;
    }
    if (t.started) {
      ++started_;
    }
    if (send_note && t.started) {
      (void)ctx.send_midi(core::midi_note_on(1, 60, 100, 10));
    }
    if (send_note && t.stopped) {
      (void)ctx.send_midi(core::midi_note_off(1, 60, 3));
    }
    last_following_ = t.following;
    if (!t.playing) {
      running_ = false;
      return;
    }
    if (t.started && running_) {
      running_ = false;  // Play while playing: the pattern starts again
    }
    if (!running_) {
      running_ = true;
      ++runs_;
      step_tick_ = (t.pos + 0xFFFFFFFFull) >> 32;  // the first whole tick the transport has not passed
    }
    for (unsigned n = 0; n < 16; ++n) {
      int f = dsp::frame_at(t, step_tick_);
      if (f < 0 && ((step_tick_ << 32) + t.inc) <= t.pos) {
        f = 0;  // a tick that already went by: late
      }
      if (f < 0) {
        break;
      }
      hits.push_back({base + static_cast<std::uint64_t>(f), step_tick_});
      step_tick_ += kStep;
    }
    last_tempo_ = t.tempo_mbpm;
  }
  std::vector<Hit> hits;
  std::uint64_t first = 0;
  std::uint64_t blocks_ = 0;
  unsigned runs_ = 0;
  unsigned started_ = 0;
  unsigned stopped_ = 0;
  bool running_ = false;
  bool send_note = false;
  bool last_following_ = false;
  std::uint32_t last_tempo_ = 0;
  std::uint64_t step_tick_ = 0;
};

// Every whole tick must be owned by exactly one block of a run (dsp::frame_at's rule): a tick missed or counted twice across a block boundary, a rebase, a tempo
// change is a skipped or a doubled step.
class TickAudit {
 public:
  void feed(const core::ClockFrame& f, std::uint32_t frames) {
    if (!f.follow || !f.playing || f.started) {
      have_ = false;
    }
    if (!f.follow || !f.playing) {
      return;
    }
    dsp::Transport t;
    t.playing = true;
    t.pos = f.pos;
    t.inc = f.inc;
    t.frames = frames;
    std::uint64_t first = f.pos >> 32;
    first = first > 0u ? first - 1u : 0u;
    for (std::uint64_t tick = first; tick <= (t.last() >> 32); ++tick) {
      if (dsp::frame_at(t, tick) < 0) {
        continue;
      }
      if (have_ && tick != last_ + 1u) {
        ++bad_;
      }
      last_ = tick;
      have_ = true;
      ++owned_;
    }
  }
  std::uint32_t bad() const { return bad_; }
  std::uint64_t owned() const { return owned_; }

 private:
  bool have_ = false;
  std::uint64_t last_ = 0;
  std::uint32_t bad_ = 0;
  std::uint64_t owned_ = 0;
};

// ---- the wire -------------------------------------------------------------------------------------------------------------------------------------------
struct WireByte {
  std::uint64_t start;  // timeline cycles
  std::uint8_t byte;
};

// The UART as the PIT interrupt drives it: polled at the times the emitter asks for, up to `until`.
void pump_wire(midi::DinEmitter& em, std::uint64_t& now, std::uint64_t until, std::vector<WireByte>& out) {
  for (unsigned guard = 0; guard < 100000; ++guard) {
    const std::uint64_t wake = em.next_wake(now);
    if (wake == midi::kNever || wake > until) {
      break;
    }
    now = wake;
    const midi::DinEmitter::Tx tx = em.poll(now);
    if (tx.valid) {
      out.push_back({tx.start, tx.byte});
    } else {
      ++now;
    }
  }
  now = std::max(now, until);
}

// ---- the master -----------------------------------------------------------------------------------------------------------------------------------------
// An external clock. Edge k of the clock happens at the cycle count edge[k]; the interrupt stamps it when it is complete (a DIN byte: one byte time later; a USB one: half
// a millisecond, the average of the frame it arrives in), plus the jitter of the line. The tempo is a function of the time since the first edge in seconds.
struct Master {
  int port = 0;                                // 0 DIN, 1 USB
  std::function<double(double)> bpm = [](double) { return 120.0; };
  double jitter = 0.0;                         // cycles, uniform +-
  std::uint64_t quiet_from = ~0ull;            // the clock index from which nothing is sent ...
  std::uint64_t quiet_to = ~0ull;              // ... up to this one (the master comes back)
  Rng rng{1};
  std::vector<double> edge;                    // edge[k]: the cycle count of clock k
  std::uint64_t pushed = 0;                    // clocks whose bytes went to the ring
  struct Extra {
    double stamp;
    std::uint8_t byte;
  };
  std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> before;  // {clock index, bytes}: sent just before that clock
  std::vector<Extra> pend;

  explicit Master(double c0 = 40000000.0) { edge.push_back(c0); }

  static double cycles_per_clock(double bpm_now) { return 450e6 / bpm_now; }  // 180e6 * 60 / (24 * bpm)
  void extend(double until) {
    while (edge.back() < until) {
      const double t = (edge.back() - edge.front()) / 180e6;
      edge.push_back(edge.back() + cycles_per_clock(bpm(t)));
    }
  }
  double latency() const { return port == 0 ? static_cast<double>(kByte) : 90000.0; }
  double jit() { return jitter == 0.0 ? 0.0 : (static_cast<double>(rng.below(2001)) / 1000.0 - 1.0) * jitter; }

  // Pushes, in stamp order, everything the interrupts would have stamped by the cycle count `wall` into the rings.
  void feed(midi::MidiService& svc, double wall) {
    extend(wall + 2e6);
    struct Item {
      double stamp;
      std::uint8_t byte;
    };
    std::vector<Item> items;
    for (const Extra& e : pend) {
      items.push_back({e.stamp, e.byte});
    }
    pend.clear();
    for (std::uint64_t k = pushed; k < edge.size() && edge[k] < wall + 1e6; ++k, ++pushed) {
      for (auto& b : before) {
        if (b.first == k) {
          const double n = static_cast<double>(b.second.size());
          for (std::size_t i = 0; i < b.second.size(); ++i) {  // (a byte time apart, the last one a byte and a half before the clock)
            items.push_back({edge[k] - (n - static_cast<double>(i) + 0.5) * static_cast<double>(kByte) + latency(), b.second[i]});
          }
        }
      }
      if (k < quiet_from || k >= quiet_to) {
        items.push_back({edge[k] + latency() + jit(), 0xF8});
      }
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.stamp < b.stamp; });
    for (const Item& it : items) {
      if (it.stamp > wall) {
        pend.push_back({it.stamp, it.byte});
        continue;
      }
      midi::RtInRing& ring = port == 0 ? svc.din_in() : svc.usb_in();
      (void)ring.push(midi::RtItem{u32(it.stamp), it.byte, static_cast<std::uint8_t>(port), 0});
    }
  }
  // The timeline sample (a real number, absolute) at which clock k is heard.
  double heard_sample(std::uint64_t k) const { return (edge[k] - static_cast<double>(kLead)) / kCps; }
  std::uint64_t next_index() const { return pushed; }
  void send_before(std::uint64_t k, std::vector<std::uint8_t> bytes) { before.push_back({k, std::move(bytes)}); }
};

// ---- the rig --------------------------------------------------------------------------------------------------------------------------------------------
struct Note {
  std::uint64_t block;
  core::MidiMessage msg;
};

struct Rig {
  midi::MidiService svc;
  dsp::Engine engine;
  RunHook hook;
  core::ClockContinuity check;
  TickAudit audit;
  std::uint64_t first;
  std::uint64_t block = 0;
  std::vector<core::ClockFrame> frames;
  std::vector<Note> notes;
  std::vector<WireByte> wire;
  std::uint64_t wire_now = 0;
  struct UsbEvent {
    std::uint64_t frame_start;
    std::uint32_t word;
  };
  std::vector<UsbEvent> usb;  // what the SOF interrupt put in the packets: the host listens
  std::uint64_t usb_now = 0;
  std::vector<Master*> masters;
  unsigned kicks = 0;
  unsigned mismatch = 0;  // blocks in which the engine ran another position or increment than the service's frame
  bool usb_on = true;  // the USB port is alive (configured, a host sending start-of-frame): false runs no SOF and the service is told so
  unsigned note_cap = 32;
  std::uint32_t isr_lost = 0;
  float l[kFrames] = {};
  float r[kFrames] = {};

  static std::uint32_t heard(void* ctx) noexcept {
    auto* rig = static_cast<Rig*>(ctx);
    return static_cast<std::uint32_t>(core::cycles_from_samples(rig->first + rig->block * kFrames) + kLead);
  }
  static void kick(void* ctx) noexcept { ++static_cast<Rig*>(ctx)->kicks; }
  static bool usb_alive(void* ctx) noexcept { return static_cast<Rig*>(ctx)->usb_on; }
  static std::uint32_t dropped(void* ctx) noexcept { return static_cast<Rig*>(ctx)->isr_lost; }

  explicit Rig(std::uint64_t first_sample = 0)
      : svc(midi::MidiServiceHooks{&Rig::heard, &Rig::kick, &Rig::dropped, &Rig::usb_alive, this, kLead}, first_sample), first(first_sample) {
    hook.first = first_sample;
    NEED(engine.add_hook(hook));
    engine.start();
  }

  // The cycle count at which the render of the block about to run starts: the boundary stamp (its sound is heard kLead later).
  double wall() const { return static_cast<double>(core::cycles_from_samples(first + block * kFrames)); }

  void config(const hmidi::Config& c) { svc.set_config(c); }
  void follow(bool on, hmidi::Port port = hmidi::Port::Din) {
    hmidi::Config c;
    c.follow = on;
    c.follow_port = port;
    svc.set_config(c);
  }

  // One block as Runtime::render_block does it.
  void step() {
    for (Master* m : masters) {
      m->feed(svc, wall());
    }
    core::MidiMessage in[64];
    core::ClockFrame clock;
    const unsigned n = svc.begin_block(kFrames, in, note_cap, clock);
    for (unsigned i = 0; i < n; ++i) {
      notes.push_back({block, in[i]});
    }
    frames.push_back(clock);
    check.feed(clock, kFrames);
    engine.follow(clock);
    engine.render(nullptr, nullptr, l, r, kFrames);
    // What the engine rendered is what the tick ownership has to hold for (it holds the increment between its own tempo limits), and what the service assumed it would.
    const core::ClockFrame ran = engine.block_frame();
    audit.feed(ran, kFrames);
    if (clock.follow && clock.playing && (ran.pos != clock.pos || ran.inc != clock.inc)) {
      ++mismatch;
    }
    unsigned sent = 0;
    const core::MidiMessage* out = engine.midi_out(sent);
    svc.end_block(engine.block_frame(), out, sent);
    // The interrupts run at wall time: the emitter's clock is the timeline as it is heard now, kLead behind the block just rendered, and this render took half a block.
    const std::uint64_t render_at = core::cycles_from_samples(first + block * kFrames) + core::cycles_from_samples(kFrames) / 2u;
    const std::uint64_t timeline_now = render_at > kLead ? render_at - kLead : 0u;
    pump_wire(svc.din(), wire_now, timeline_now, wire);
    if (!usb_on) {
      usb_now = timeline_now;  // (no SOF while the bus is not alive; the first one after it comes is "now")
    }
    while (usb_on && usb_now + 180000u <= timeline_now) {  // one packet a millisecond
      usb_now += 180000u;
      std::uint32_t buf[midi::usb_out::kEventsPerFrame];
      const std::uint32_t got = svc.usb_out().frame(usb_now, buf, midi::usb_out::kEventsPerFrame);
      for (std::uint32_t i = 0; i < got; ++i) {
        usb.push_back({usb_now, buf[i]});
      }
    }
    ++block;
  }
  void run(unsigned blocks) {
    for (unsigned i = 0; i < blocks; ++i) {
      step();
    }
  }
  void seconds(double s) { run(static_cast<unsigned>(s * 44117.647 / kFrames)); }

  bool following() const { return frames.back().follow; }
  std::size_t count(std::uint8_t byte) const {
    return static_cast<std::size_t>(std::count_if(wire.begin(), wire.end(), [&](const WireByte& w) { return w.byte == byte; }));
  }
};

// A step's ideal timeline sample on the master's grid: master clock `start_idx + 6 * j` for step j (a step is six clocks).
double ideal(const Master& m, std::uint64_t start_idx, std::uint64_t tick) {
  return m.heard_sample(start_idx + (tick / kClockTicks));
}

}  // namespace

// ---- notes in --------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi service: a note gets the frame that keeps its latency constant, and the app can poll every message") {
  Rig rig;
  rig.run(4);
  // Notes arrive j samples before the render of the block: frame 64 - j, whichever port.
  const double w = rig.wall();
  const std::uint8_t notes[3] = {0x90, 60, 100};
  const int offsets[5] = {1, 17, 40, 63, 64};
  for (unsigned i = 0; i < 5; ++i) {
    const std::uint32_t stamp = u32(w - offsets[i] * kCps + 100.0);
    midi::RtInRing& ring = (i & 1u) != 0u ? rig.svc.usb_in() : rig.svc.din_in();
    for (unsigned b = 0; b < 3; ++b) {
      NEED(ring.push(midi::RtItem{stamp, notes[b], static_cast<std::uint8_t>(i & 1u), 0}));
    }
  }
  // The block about to run is the 5th (index 4): feed() is not used, the stamps are in the rings.
  core::MidiMessage in[8];
  core::ClockFrame clock;
  const unsigned n = rig.svc.begin_block(kFrames, in, 8, clock);
  NEED(n == 5);
  // The two rings drain in port order (DIN first), each in arrival order.
  std::vector<unsigned> frames;
  for (unsigned i = 0; i < n; ++i) {
    frames.push_back(in[i].frame);
    CHECK(in[i].status == 0x90);
    CHECK(in[i].data1 == 60);
  }
  std::sort(frames.begin(), frames.end());
  CHECK(frames == std::vector<unsigned>{0, 1, 24, 47, 63});  // 64 - 64, 64 - 63, 64 - 40, 64 - 17, 64 - 1
  CHECK_FALSE(clock.follow);
  // Every message is also the app's, in order of arrival at the drain.
  core::MidiMessage m;
  unsigned polled = 0;
  while (rig.svc.poll(m)) {
    ++polled;
    CHECK(m.status == 0x90);
  }
  CHECK(polled == 5u);
  CHECK(rig.svc.status().rx == 5u);
}

TEST_CASE("midi service: running status, real-time bytes inside a message, system exclusive and every kind of channel message") {
  Rig rig;
  rig.run(2);
  const std::uint32_t t = u32(rig.wall() - 20.0 * kCps);
  const std::uint8_t bytes[] = {
      0x93, 60, 100, 62, 90,   // two notes on channel 4, the second with running status
      0xF8, 0xFE,              // a real-time byte and active sensing in the middle
      0x83, 60, 0,             // a note off
      0xB0, 7, 64,             // a controller
      0xC1, 5,                 // a program change
      0xD2, 33,                // channel pressure
      0xE0, 0, 0x40,           // pitch bend
      0xA0, 61, 20,            // polyphonic pressure
      0xF0, 1, 2, 3, 0xF7,     // system exclusive: not a message
      0x4A, 0x4B,              // data bytes with no status (the system exclusive cancelled it)
      0xB0, 123, 0,            // all notes off
      0x90, 61, 0,             // a note on with velocity 0
  };
  for (const std::uint8_t b : bytes) {
    NEED(rig.svc.din_in().push(midi::RtItem{t, b, 0, 0}));
  }
  // (A drain takes kPassCap bytes of a port: the 36 of this stream come in two blocks.)
  core::MidiMessage in[16];
  core::ClockFrame clock;
  unsigned n = rig.svc.begin_block(kFrames, in, 16, clock);
  n += rig.svc.begin_block(kFrames, in + n, 16 - n, clock);
  // The engine gets the notes and the all-notes-off only.
  NEED(n == 5);
  CHECK(in[0].status == 0x93);
  CHECK(in[0].data1 == 60);
  CHECK(in[1].status == 0x93);
  CHECK(in[1].data1 == 62);
  CHECK(in[2].status == 0x83);
  CHECK(in[3].status == 0xB0);
  CHECK(in[3].data1 == 123);
  CHECK(in[4].status == 0x90);
  CHECK(in[4].data2 == 0);
  for (unsigned i = 0; i < n; ++i) {
    CHECK(in[i].ports == core::kMidiDin);
    CHECK(in[i].frame < kFrames);
  }
  // The app gets all of them, in order.
  const std::uint8_t want[][3] = {{0x93, 60, 100}, {0x93, 62, 90}, {0x83, 60, 0}, {0xB0, 7, 64}, {0xC1, 5, 0}, {0xD2, 33, 0},
                                  {0xE0, 0, 0x40}, {0xA0, 61, 20}, {0xB0, 123, 0}, {0x90, 61, 0}};
  core::MidiMessage m;
  for (const auto& w : want) {
    NEED(rig.svc.poll(m));
    CHECK(m.status == w[0]);
    CHECK(m.data1 == w[1]);
    CHECK(m.data2 == w[2]);
  }
  CHECK_FALSE(rig.svc.poll(m));
  CHECK(rig.svc.status().rx == 10u);
}

TEST_CASE("midi service: notes past the engine's room wait for the next block, in order, at frame 0") {
  Rig rig;
  rig.run(2);
  rig.note_cap = 3;
  const std::uint32_t t = u32(rig.wall() - 30.0 * kCps);
  for (std::uint8_t n = 0; n < 8; ++n) {
    for (const std::uint8_t b : {std::uint8_t{0x90}, static_cast<std::uint8_t>(40 + n), std::uint8_t{100}}) {
      NEED(rig.svc.din_in().push(midi::RtItem{t, b, 0, 0}));
    }
  }
  rig.step();
  CHECK(rig.notes.size() == 3u);
  rig.step();
  rig.step();
  NEED(rig.notes.size() == 8u);
  for (std::uint8_t n = 0; n < 8; ++n) {
    CHECK(rig.notes[n].msg.data1 == 40 + n);
  }
  CHECK(rig.notes[3].msg.frame == 0u);  // the held ones were late: frame 0
  CHECK(rig.svc.stats().held == 5u);
  CHECK(rig.svc.status().dropped == 0u);
}

TEST_CASE("midi service: what the app does not poll is dropped and counted, and the first block discards what the interrupts stamped before it") {
  Rig rig;
  // Before the first block: 40 bytes from boot, which belong to no timeline.
  for (unsigned i = 0; i < 40; ++i) {
    NEED(rig.svc.din_in().push(midi::RtItem{1234u + i, 0x90, 0, 0}));
  }
  rig.run(1);
  CHECK(rig.svc.stats().rings_discarded == 40u);
  CHECK(rig.notes.empty());
  // 80 control changes in one block: the poll ring holds 64.
  const std::uint32_t t = u32(rig.wall() - 20.0 * kCps);
  for (unsigned i = 0; i < 80; ++i) {
    for (const std::uint8_t b : {std::uint8_t{0xB0}, std::uint8_t{1}, static_cast<std::uint8_t>(i)}) {
      NEED(rig.svc.din_in().push(midi::RtItem{t, b, 0, 0}));
    }
  }
  rig.isr_lost = 3;  // the drivers' own counters join
  rig.run(12);  // (a drain takes kPassCap bytes a block)
  CHECK(rig.svc.stats().poll_dropped == 80u - midi::MidiService::kPollRing);
  CHECK(rig.svc.status().dropped == 80u - midi::MidiService::kPollRing + 3u);
  core::MidiMessage m;
  unsigned n = 0;
  while (rig.svc.poll(m)) {
    CHECK(m.data2 == n);  // the oldest are kept
    ++n;
  }
  CHECK(n == midi::MidiService::kPollRing);
}

// ---- messages out ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi service: send() reaches the wire as the bytes of the message, on the ports the config names, and a refused message says so") {
  Rig rig;
  rig.run(2);
  struct Case {
    core::MidiMessage m;
    std::vector<std::uint8_t> bytes;
  };
  const Case cases[] = {
      {core::midi_note_on(3, 60, 100), {0x92, 60, 100}},
      {core::midi_note_off(3, 60), {0x82, 60, 0}},
      {core::midi_control(1, 74, 99), {0xB0, 74, 99}},
      {core::MidiMessage{0, 0xC4, 12, 0, 0}, {0xC4, 12}},
      {core::MidiMessage{0, 0xD0, 55, 0, 0}, {0xD0, 55}},
      {core::MidiMessage{0, 0xE2, 0, 0x50, 0}, {0xE2, 0, 0x50}},
      {core::MidiMessage{0, 0xA0, 61, 20, 0}, {0xA0, 61, 20}},
  };
  for (const Case& c : cases) {
    rig.wire.clear();
    NEED(rig.svc.send(c.m));
    rig.run(3);
    std::vector<std::uint8_t> got;
    for (const WireByte& w : rig.wire) {
      got.push_back(w.byte);
    }
    CHECK(got == c.bytes);
  }
  // The off of a note nobody turned on is not owed: it is not sent (the router's hang book).
  rig.wire.clear();
  NEED(rig.svc.send(core::midi_note_off(9, 99)));
  rig.run(3);
  CHECK(rig.wire.empty());
  CHECK(rig.svc.out().stats().stray_offs == 1u);
  // A note on and its off.
  rig.wire.clear();
  NEED(rig.svc.send(core::midi_note_on(1, 64, 90)));
  rig.run(2);
  NEED(rig.svc.send(core::midi_note_off(1, 64)));
  rig.run(3);
  NEED(rig.wire.size() == 6);
  CHECK(rig.wire[0].byte == 0x90);
  CHECK(rig.wire[3].byte == 0x80);
  CHECK(rig.wire[3].start > rig.wire[0].start);
  CHECK(rig.svc.status().tx >= 6u);

  // Refused: not a message, and no port to send on.
  CHECK_FALSE(rig.svc.send(core::MidiMessage{0, 0xF0, 1, 2, 0}));
  CHECK_FALSE(rig.svc.send(core::MidiMessage{0, 0x40, 1, 2, 0}));
  hmidi::Config c;
  c.out_ports = 0;
  rig.config(c);
  CHECK_FALSE(rig.svc.send(core::midi_note_on(1, 60, 100)));
  CHECK(rig.svc.send(core::midi_note_on(1, 60, 100, 0, core::kMidiDin)));  // the message names its port
  // A full ring says so and counts it.
  unsigned refused = 0;
  for (unsigned i = 0; i < 40; ++i) {
    refused += rig.svc.send(core::midi_control(1, 1, 1, 0, core::kMidiDin)) ? 0u : 1u;
  }
  CHECK(refused > 0u);
  CHECK(rig.svc.status().dropped >= refused);
}

TEST_CASE("midi service: the messages a hook sends leave at their frames, on DIN and on USB") {
  Rig rig;
  rig.hook.send_note = true;
  hmidi::Config c;
  c.out_ports = core::kMidiUsb;
  rig.config(c);
  rig.run(2);
  // Play: the hook sends a note on at frame 10 of the first block and an off on stop.
  CHECK(rig.engine.play());
  rig.run(3);
  CHECK(rig.engine.stop());
  rig.run(12);  // (the router's horizon and the 1 ms packets: the Stop and the note off leave a few blocks after the block that decides them)
  std::vector<std::uint8_t> status;
  for (const Rig::UsbEvent& ue : rig.usb) {
    std::uint8_t b[3];
    const std::uint32_t len = midi::usb_event_bytes(ue.word, b);
    if (len > 0 && b[0] != 0xF8) {
      status.push_back(b[0]);
    }
  }
  // Start, the note on, Stop, and the note off the hook sent on stop (the stop_all also finds nothing owed).
  const bool order = status == std::vector<std::uint8_t>{0xFA, 0x90, 0x80, 0xFC} || status == std::vector<std::uint8_t>{0xFA, 0x90, 0xFC, 0x80};
  CHECK(order);
  CHECK(rig.wire.empty());  // DIN is not an out port
}

// ---- clock and transport out -----------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi service: the engine's own clock goes out 24 a quarter note on the sample it falls on, Start before the first, Stop at the end") {
  Rig rig;
  rig.run(3);
  CHECK(rig.engine.set_tempo(120'000));
  CHECK(rig.engine.play());
  rig.run(2);
  const std::uint64_t play_block = 3;  // the play() command takes effect at the next block
  rig.seconds(4.0);
  CHECK(rig.engine.stop());
  rig.run(8);
  CHECK(rig.count(0xFA) == 1u);
  CHECK(rig.count(0xFC) == 1u);
  NEED(rig.wire.size() > 100);
  CHECK(rig.wire.front().byte == 0xFA);
  std::vector<std::uint64_t> clocks;
  for (const WireByte& w : rig.wire) {
    if (w.byte == 0xF8) {
      clocks.push_back(w.start);
    }
  }
  NEED(clocks.size() > 150);
  // The nth clock is due at the nth 40 ticks of the run: 3,750,000 cycles apart at 120 BPM, from the first sample of the first block of the run.
  const double t0 = static_cast<double>(core::cycles_from_samples(play_block * kFrames));
  const double period = 450e6 / 120.0;
  for (std::size_t k = 0; k < clocks.size(); ++k) {
    const double due = t0 + static_cast<double>(k) * period;
    const double late = static_cast<double>(clocks[k]) - due;
    // On the wire within one byte time of its sample (the emitter's bound for an idle wire is 50 us; a Start or a note ahead of it costs a byte time).
    CHECK(late >= -2.0 * kCps);
    CHECK(late <= static_cast<double>(kByte) + 2.0 * kCps);
  }
  // The Start leaves a quarter block before the first clock, and the Stop is the last real-time byte.
  const auto start = std::find_if(rig.wire.begin(), rig.wire.end(), [](const WireByte& w) { return w.byte == 0xFA; });
  CHECK(start->start + 3 * kByte >= clocks[0]);
  CHECK(start->start < clocks[0]);
  CHECK(rig.wire.back().byte == 0xFC);
  CHECK(rig.kicks > 0u);
}

TEST_CASE("midi service: CLOCK OUT and TRANSPORT OUT are separate switches, and no out port is silence") {
  for (unsigned variant = 0; variant < 3; ++variant) {
    Rig rig;
    hmidi::Config c;
    c.clock_out = variant != 1;
    c.transport_out = variant != 0;
    if (variant == 2) {
      c.out_ports = 0;
    }
    rig.config(c);
    rig.run(3);
    CHECK(rig.engine.play());
    rig.seconds(1.5);
    CHECK(rig.engine.stop());
    rig.run(6);
    const std::size_t clocks = rig.count(0xF8);
    const std::size_t transport = rig.count(0xFA) + rig.count(0xFC);
    if (variant == 0) {  // clock only
      CHECK(clocks > 24u);
      CHECK(transport == 0u);
    } else if (variant == 1) {  // transport only
      CHECK(clocks == 0u);
      CHECK(transport == 2u);
    } else {  // no port
      CHECK(rig.wire.empty());
    }
  }
}

TEST_CASE("midi service: a run that begins elsewhere is announced with its Song Position and a Continue, in that order, and a stop releases the notes still owed") {
  Rig rig;
  Master m;
  rig.masters.push_back(&m);
  rig.follow(true);
  rig.seconds(1.0);
  // The master locates to the 5th sixteenth and continues; the service relays it from the transport it hands the engine (DIN is the default out port).
  const std::uint64_t k = m.next_index() + 6;
  m.send_before(k, {0xF2, 5, 0});
  m.send_before(k + 1, {0xFB});
  rig.seconds(0.5);
  CHECK(rig.engine.playing());
  CHECK(rig.hook.started_ == 0u);
  // A note the router owes, then the master's Stop.
  NEED(rig.svc.send(core::midi_note_on(1, 50, 100)));
  rig.seconds(0.2);
  m.send_before(m.next_index() + 6, {0xFC});
  rig.seconds(0.5);
  CHECK_FALSE(rig.engine.playing());
  std::vector<std::uint8_t> msgs;
  for (const WireByte& w : rig.wire) {
    if (w.byte != 0xF8) {
      msgs.push_back(w.byte);
    }
  }
  // SPP (F2 lsb msb), Continue, the note (90 32 64), Stop, and the off the router owed.
  NEED(msgs.size() >= 10);
  CHECK(msgs[0] == 0xF2);
  CHECK(msgs[1] == 5);
  CHECK(msgs[2] == 0);
  CHECK(msgs[3] == 0xFB);
  const auto stop_at = std::find(msgs.begin(), msgs.end(), 0xFC);
  NEED(stop_at != msgs.end());
  CHECK(std::find(stop_at, msgs.end(), 0x80) != msgs.end());
  CHECK(rig.svc.out().book().owed() == 0u);
  CHECK(rig.check.ok());
}

// ---- following a master -------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi service: follow attaches once the master's period is measured, reports its tempo, and lets go on a quiet master") {
  Rig rig;
  Master m;
  rig.masters.push_back(&m);
  rig.seconds(0.3);
  CHECK_FALSE(rig.following());  // FOLLOW is off: nothing is followed
  CHECK_FALSE(rig.svc.dll().has_phase());
  rig.follow(true);
  rig.seconds(0.02);
  CHECK_FALSE(rig.following());  // (one clock is not a period)
  rig.seconds(0.6);
  NEED(rig.following());
  const hmidi::Status st = rig.svc.status();
  CHECK(st.following);
  CHECK(st.tempo_mbpm >= 119'900u);
  CHECK(st.tempo_mbpm <= 120'100u);
  rig.seconds(3.0);
  CHECK(rig.svc.status().locked);
  CHECK(rig.engine.following());
  CHECK(rig.hook.last_following_);
  CHECK(rig.engine.tempo() >= 119'900u);
  CHECK(rig.engine.tempo() <= 120'100u);
  CHECK(rig.svc.stats().follow_attached == 1u);

  // The master falls silent: 48 clocks (a second at this tempo) later the next frames have no follow, and the engine has its own tempo back.
  m.quiet_from = m.next_index() + 2;
  rig.seconds(0.5);
  CHECK(rig.following());
  rig.seconds(2.0);
  CHECK_FALSE(rig.following());
  CHECK(rig.svc.stats().stale_releases == 1u);
  CHECK_FALSE(rig.svc.status().following);
  CHECK(rig.svc.status().tempo_mbpm == 0u);
  CHECK_FALSE(rig.engine.following());
  CHECK(rig.check.ok());
  // It comes back: acquired afresh.
  m.quiet_from = ~0ull;
  rig.seconds(1.5);
  CHECK(rig.following());
  CHECK(rig.svc.stats().follow_attached == 2u);
  // FOLLOW off lets go at once.
  rig.follow(false);
  rig.run(2);
  CHECK_FALSE(rig.following());
}

TEST_CASE("midi service: a Start begins the run with pos 0, the hook's steps land on the master's grid, nothing jumps and no tick is lost or owned twice") {
  for (const int port : {0, 1}) {
    Rig rig;
    Master m;
    m.port = port;
    m.bpm = [](double) { return 128.0; };
    rig.masters.push_back(&m);
    rig.follow(true, port == 0 ? hmidi::Port::Din : hmidi::Port::Usb);
    rig.seconds(1.0);
    NEED(rig.following());
    const std::uint64_t start_idx = m.next_index() + 6;
    m.send_before(start_idx, {0xFA});
    rig.seconds(9.0);
    CHECK(rig.hook.started_ == 1u);
    CHECK(rig.hook.runs_ == 1u);
    CHECK(rig.check.starts() == 1u);
    CHECK(rig.check.ok());
    CHECK(rig.check.jumps() == 0u);
    CHECK(rig.audit.bad() == 0u);
    CHECK(rig.audit.owned() > 4000u);
    const auto& hits = rig.hook.hits;
    NEED(hits.size() > 30u);
    for (std::size_t i = 0; i < hits.size(); ++i) {
      CHECK(hits[i].tick == i * kStep);  // every step, once, in order
    }
    // The run begins up to a few blocks behind the master (the clock was stamped where it was HEARD, the render-ahead before the block that learns of it), then the
    // position catches up: later steps are on the master's grid to a sample.
    const double first_late = static_cast<double>(hits[0].sample) - ideal(m, start_idx, 0);
    CHECK(first_late >= 0.0);
    CHECK(first_late <= 6.0 * kFrames);
    for (std::size_t i = 20; i < hits.size(); ++i) {
      const double err = static_cast<double>(hits[i].sample) - ideal(m, start_idx, hits[i].tick);
      CHECK(err >= -0.5);
      CHECK(err <= 1.5);
    }
    // The transport the engine reports is the master's.
    CHECK(rig.engine.playing());
    CHECK(rig.engine.tempo() >= 127'900u);
    CHECK(rig.engine.tempo() <= 128'100u);
    // The clocks go out relayed: 24 a quarter note on the DIN wire, from the transport (DIN is the default out port).
    CHECK(rig.count(0xFA) == 1u);
    CHECK(rig.count(0xF8) > 24u * 16u);
  }
}

TEST_CASE("midi service: a master with jitter (DIN a few microseconds, USB half a millisecond) is followed to a few samples") {
  for (const int port : {0, 1}) {
    Rig rig;
    Master m;
    m.port = port;
    m.jitter = port == 0 ? 600.0 : 90000.0;  // cycles: 3 us, 0.5 ms
    m.rng = Rng(7);
    rig.masters.push_back(&m);
    rig.follow(true, port == 0 ? hmidi::Port::Din : hmidi::Port::Usb);
    rig.seconds(1.0);
    const std::uint64_t start_idx = m.next_index() + 6;
    m.send_before(start_idx, {0xFA});
    rig.seconds(30.0);
    CHECK(rig.check.ok());
    CHECK(rig.audit.bad() == 0u);
    const auto& hits = rig.hook.hits;
    NEED(hits.size() > 100u);
    double worst = 0.0;
    for (std::size_t i = 60; i < hits.size(); ++i) {  // the loop has settled
      worst = std::max(worst, std::abs(static_cast<double>(hits[i].sample) - ideal(m, start_idx, hits[i].tick)));
    }
    // The loop's own residual jitter is about the input's times sqrt(2 B / F): 0.5 ms of USB jitter at 120 BPM gives a few tens of samples at worst.
    CHECK(worst < (port == 0 ? 3.0 : 40.0));
    CHECK(rig.engine.tempo() >= 119'800u);
    CHECK(rig.engine.tempo() <= 120'200u);
  }
}

TEST_CASE("midi service: the master's tempo changes (a step, a ramp) and the loop's rebase leave the position continuous with no step lost") {
  // The limits are what the clock-follow loop (ported from track3r-os, midi/clock_dll.h) does with such a master: a ramp or a wobble leaves it a constant lag behind the
  // master's grid (`worst`), a tempo step is re-acquired after three clocks, and the steps of those clocks are off by a clock or two (`outliers` of them, at most `outlier`
  // samples); once the tempo is steady again the steps are on the grid (`last`). What this test holds is the service's: the position never jumps, no tick is skipped or owned
  // twice, every step comes once in order, and the steps follow the loop to within its own lag.
  struct Profile {
    const char* name;
    std::function<double(double)> bpm;
    std::uint64_t first_sample;
    double worst;
    unsigned outliers;
    double outlier;
    double last;
  };
  const Profile profiles[] = {
      {"a step 100 -> 140 -> 90", [](double t) { return t < 6.0 ? 100.0 : (t < 14.0 ? 140.0 : 90.0); }, 0, 120.0, 6, 3000.0, 40.0},
      {"a ramp 80 -> 160 -> 100", [](double t) { return t < 20.0 ? 80.0 + 4.0 * t : std::max(100.0, 160.0 - 6.0 * (t - 20.0)); }, 0, 250.0, 0, 0.0, 40.0},
      {"a loop that rebases (hours into the timeline)", [](double t) { return 118.0 + 4.0 * std::sin(t * 0.7); }, (std::uint64_t{1} << 30) + 5000u, 130.0, 0, 0.0, 130.0},
  };
  for (const Profile& p : profiles) {
    INFO(p.name);
    Rig rig(p.first_sample);
    Master m(static_cast<double>(core::cycles_from_samples(p.first_sample)) + 40e6);
    m.bpm = p.bpm;
    rig.masters.push_back(&m);
    rig.follow(true);
    rig.seconds(1.5);
    const std::uint64_t start_idx = m.next_index() + 6;
    m.send_before(start_idx, {0xFA});
    rig.seconds(40.0);
    NEED(rig.following());
    CHECK(rig.check.ok());
    CHECK(rig.check.jumps() == 0u);
    CHECK(rig.audit.bad() == 0u);
    CHECK(rig.audit.owned() > 1000u);
    const auto& hits = rig.hook.hits;
    NEED(hits.size() > 100u);
    for (std::size_t i = 0; i < hits.size(); ++i) {
      CHECK(hits[i].tick == i * kStep);
    }
    double worst = 0.0;
    double worst_outlier = 0.0;
    unsigned outliers = 0;
    for (std::size_t i = 40; i < hits.size(); ++i) {
      const double err = std::abs(static_cast<double>(hits[i].sample) - ideal(m, start_idx, hits[i].tick));
      if (err > p.worst) {
        ++outliers;
        worst_outlier = std::max(worst_outlier, err);
      } else {
        worst = std::max(worst, err);
      }
    }
    CHECK(worst <= p.worst);
    CHECK(outliers <= p.outliers);
    CHECK(worst_outlier <= p.outlier);
    const double last = std::abs(static_cast<double>(hits.back().sample) - ideal(m, start_idx, hits.back().tick));
    CHECK(last < p.last);
  }
}

TEST_CASE("midi service: the loop's rebase really happens in the late-timeline profile") {
  Rig rig((std::uint64_t{1} << 30) + 5000u);
  Master m(static_cast<double>(core::cycles_from_samples(rig.first)) + 40e6);
  rig.masters.push_back(&m);
  rig.follow(true);
  const std::uint64_t origin_before = rig.svc.dll().origin();
  rig.seconds(0.2);
  CHECK(rig.svc.dll().origin() == origin_before);  // (still the timeline's zero: a loop 2^30 samples late)
  rig.seconds(2.0);
  CHECK(rig.svc.dll().origin() > origin_before + (std::uint64_t{1} << 29));  // the loop moved its origin: the rebase
  CHECK(rig.following());
  CHECK(rig.svc.status().tempo_mbpm >= 119'900u);
  CHECK(rig.svc.status().tempo_mbpm <= 120'100u);
}

TEST_CASE("midi service: Continue resumes at the song position, Stop then Continue resumes where the master stopped") {
  Rig rig;
  Master m;
  rig.masters.push_back(&m);
  rig.follow(true);
  rig.seconds(1.0);
  // SPP 32 (two bars), then Continue.
  const std::uint64_t k = m.next_index() + 8;
  m.send_before(k, {0xF2, 32, 0});
  m.send_before(k + 2, {0xFB});
  rig.seconds(4.0);
  CHECK(rig.check.ok());
  CHECK(rig.audit.bad() == 0u);
  CHECK(rig.hook.runs_ == 1u);
  CHECK(rig.hook.started_ == 0u);  // a Continue is not a Start
  NEED(rig.hook.hits.size() > 4u);
  for (std::size_t i = 0; i < rig.hook.hits.size(); ++i) {
    CHECK(rig.hook.hits[i].tick == (32u + i) * kStep);
  }
  // The first frame of the run was the song position, exactly.
  std::size_t first = 0;
  while (first < rig.frames.size() && !rig.frames[first].playing) {
    ++first;
  }
  NEED(first < rig.frames.size());
  CHECK(rig.frames[first].pos == (std::uint64_t{32} * kStep) << 32);
  CHECK_FALSE(rig.frames[first].started);

  // Stop, then Continue: from where it stopped (whole ticks).
  m.send_before(m.next_index() + 6, {0xFC});
  rig.seconds(1.0);
  CHECK(rig.hook.stopped_ == 1u);
  m.send_before(m.next_index() + 6, {0xFB});
  rig.seconds(2.0);
  CHECK(rig.hook.runs_ == 2u);
  std::size_t stopped_frame = 0;
  for (std::size_t i = 0; i < rig.frames.size(); ++i) {
    if (rig.frames[i].stopped) {
      stopped_frame = i;
    }
  }
  NEED(stopped_frame > 1);
  const core::ClockFrame& before = rig.frames[stopped_frame - 1];
  std::size_t again = stopped_frame;
  while (again < rig.frames.size() && !rig.frames[again].playing) {
    ++again;
  }
  NEED(again < rig.frames.size());
  CHECK(rig.frames[again].pos == (((before.pos + before.inc * kFrames) >> 32) << 32));
  CHECK_FALSE(rig.frames[again].started);
  CHECK(rig.check.jumps() == 0u);
  CHECK(rig.audit.bad() == 0u);
}

TEST_CASE("midi service: TRANSPORT IN off ignores the master's Start but the Play key starts on its next clock, and Stop stops") {
  Rig rig;
  Master m;
  rig.masters.push_back(&m);
  hmidi::Config c;
  c.follow = true;
  c.transport_in = false;
  rig.config(c);
  rig.seconds(1.0);
  NEED(rig.following());
  m.send_before(m.next_index() + 6, {0xFA});
  rig.seconds(1.0);
  CHECK(rig.svc.stats().transport_ignored == 1u);
  CHECK_FALSE(rig.engine.playing());
  CHECK(rig.hook.runs_ == 0u);
  // The Play key: the engine follows, so play() is a request, and the device starts the run on the master's next clock.
  CHECK(rig.engine.play());
  const core::TransportRequest req = rig.engine.take_transport_request();
  CHECK(req == core::TransportRequest::Play);
  rig.svc.request(req);
  const std::uint64_t asked_at = m.next_index();
  rig.seconds(1.5);
  CHECK(rig.engine.playing());
  CHECK(rig.hook.started_ == 1u);
  CHECK(rig.check.ok());
  CHECK(rig.audit.bad() == 0u);
  CHECK(rig.svc.stats().requests == 1u);
  NEED(rig.hook.hits.size() > 4u);
  // The run began on a clock of the master (within the first few after the request): its steps are on that clock's grid.
  double best = 1e18;
  for (std::uint64_t kk = asked_at; kk < asked_at + 12; ++kk) {
    const auto& h = rig.hook.hits.back();
    best = std::min(best, std::abs(static_cast<double>(h.sample) - ideal(m, kk, h.tick)));
  }
  CHECK(best < 2.0);
  // Stop: a request again.
  CHECK(rig.engine.stop());
  rig.svc.request(rig.engine.take_transport_request());
  rig.run(4);
  CHECK_FALSE(rig.engine.playing());
  CHECK(rig.hook.stopped_ == 1u);
  CHECK(rig.following());
}

TEST_CASE("midi service: only the followed port's clock is followed; a change of source starts over") {
  Rig rig;
  Master din;
  din.port = 0;
  din.bpm = [](double) { return 100.0; };
  Master usb(60e6);
  usb.port = 1;
  usb.bpm = [](double) { return 140.0; };
  rig.masters.push_back(&din);
  rig.masters.push_back(&usb);
  rig.follow(true, hmidi::Port::Usb);
  rig.seconds(2.0);
  NEED(rig.following());
  CHECK(rig.svc.status().tempo_mbpm >= 139'800u);
  CHECK(rig.svc.status().tempo_mbpm <= 140'200u);
  CHECK(rig.svc.in().stats().ignored_port > 20u);
  rig.follow(true, hmidi::Port::Din);
  rig.run(2);
  CHECK_FALSE(rig.following());  // let go for the new source ...
  rig.seconds(2.0);
  NEED(rig.following());         // ... and acquired afresh
  CHECK(rig.svc.status().tempo_mbpm >= 99'800u);
  CHECK(rig.svc.status().tempo_mbpm <= 100'200u);
  CHECK(rig.check.ok());
}

TEST_CASE("midi service: a follow that is released ends the engine's run once, and the engine's own transport is its own again") {
  Rig rig;
  Master m;
  rig.masters.push_back(&m);
  rig.follow(true);
  rig.seconds(1.0);
  m.send_before(m.next_index() + 6, {0xFA});
  rig.seconds(2.0);
  CHECK(rig.engine.playing());
  rig.follow(false);
  rig.run(3);
  CHECK_FALSE(rig.engine.playing());
  CHECK(rig.hook.stopped_ == 1u);
  CHECK(rig.engine.tempo() == 120'000u);  // the engine's own (the same number here, but the master's no longer reported)
  CHECK(rig.engine.play());
  rig.run(3);
  CHECK(rig.engine.playing());
  CHECK(rig.hook.started_ == 2u);
}

// ---- pure functions ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi service: frame_of is dsp::frame_at") {
  Rng rng(99);
  for (unsigned i = 0; i < 20000; ++i) {
    dsp::Transport t;
    t.playing = true;
    t.frames = 1u + rng.below(64);
    t.inc = (std::uint64_t{rng.next()} << 4 | rng.below(16)) % (std::uint64_t{1} << 30) + 1000u;
    t.pos = (std::uint64_t{rng.next() % 5000u} << 32) | rng.next();
    const std::uint64_t tick = (t.pos >> 32) + rng.below(40) - std::min<std::uint64_t>(t.pos >> 32, 3u);
    CHECK(midi::MidiService::frame_of(t.pos, t.inc, t.frames, tick) == dsp::frame_at(t, tick));
  }
  CHECK(midi::MidiService::frame_of(0, 0, 64, 0) == -1);
  CHECK(midi::MidiService::frame_of(0, 1000, 0, 0) == -1);
}

TEST_CASE("midi service: the config the render acts on is the sanitized one, and the status starts at zero") {
  Rig rig;
  hmidi::Status s = rig.svc.status();
  CHECK_FALSE(s.following);
  CHECK(s.rx == 0u);
  CHECK(s.tx == 0u);
  CHECK(s.dropped == 0u);
  hmidi::Config c;
  c.out_channel = 0;
  c.channel = 40;
  c.out_ports = 0xFF;
  rig.config(c);
  rig.run(1);
  midi::MidiService::Diag d;
  rig.svc.diag(d);
  CHECK(d.config.out_channel == 1u);
  CHECK(d.config.channel == 0u);
  CHECK(d.config.out_ports == core::kMidiAllPorts);
  CHECK(rig.svc.stats().configs == 1u);
  // A control change goes out on the channel the message names, not the config's.
  CHECK(rig.svc.out().config().cc_channel == 0u);
}

// ---- a run that begins before the loop is attached -----------------------------------------------------------------------------------------------------

namespace {

// The checks every followed run has to keep, whatever began it.
void check_run(Rig& rig, std::size_t min_hits) {
  CHECK(rig.check.ok());
  CHECK(rig.check.jumps() == 0u);
  CHECK(rig.audit.bad() == 0u);
  CHECK(rig.mismatch == 0u);
  const auto& hits = rig.hook.hits;
  NEED(hits.size() >= min_hits);
  for (std::size_t i = 1; i < hits.size(); ++i) {
    CHECK(hits[i].tick == hits[i - 1].tick + kStep);  // every step once, in order
  }
}

}  // namespace

TEST_CASE("midi service: a Start that arrives before the first clock begins the run when the loop attaches") {
  for (const int port : {0, 1}) {
    Rig rig;
    Master m;
    m.port = port;
    m.send_before(0, {0xFA});  // Start, then the very first clock
    rig.masters.push_back(&m);
    rig.follow(true, port == 0 ? hmidi::Port::Din : hmidi::Port::Usb);
    rig.seconds(6.0);
    NEED(rig.following());
    CHECK(rig.engine.playing());
    CHECK(rig.hook.started_ == 1u);
    CHECK(rig.hook.runs_ == 1u);
    CHECK(rig.check.starts() == 1u);
    CHECK(rig.svc.stats().transport_events == 1u);
    check_run(rig, 8);
    CHECK(rig.hook.hits[0].tick == 0u);
    // Later steps are on the master's grid (the run begins a few blocks behind the master, then the position catches up).
    const auto& hits = rig.hook.hits;
    for (std::size_t i = hits.size() - 4; i < hits.size(); ++i) {
      const double err = static_cast<double>(hits[i].sample) - ideal(m, 0, hits[i].tick);
      CHECK(std::abs(err) <= (port == 0 ? 2.0 : 40.0));
    }
  }
}

TEST_CASE("midi service: a Start right after the master was let go for being quiet begins a run on the first clocks it comes back with") {
  Rig rig;
  Master m;
  rig.masters.push_back(&m);
  rig.follow(true);
  rig.seconds(2.0);
  NEED(rig.following());
  m.quiet_from = m.next_index() + 2;
  m.quiet_to = m.quiet_from + 200;  // 200 clocks (4.2 s at 120 BPM) of silence, past the 48 that let the master go
  m.send_before(m.quiet_to, {0xFA});
  rig.seconds(2.5);
  CHECK_FALSE(rig.following());
  CHECK(rig.svc.stats().stale_releases == 1u);
  CHECK_FALSE(rig.engine.playing());
  rig.seconds(5.0);
  NEED(rig.following());
  CHECK(rig.engine.playing());
  CHECK(rig.hook.started_ == 1u);
  CHECK(rig.svc.stats().follow_attached == 2u);
  check_run(rig, 8);
  CHECK(rig.hook.hits[0].tick == 0u);
}

TEST_CASE("midi service: a Continue at a Song Position before the loop attaches resumes there, and a Stop before it calls the run off") {
  {
    Rig rig;
    Master m;
    m.send_before(0, {0xF2, 8, 0, 0xFB});  // SPP 8 (two beats), Continue, then the first clock
    rig.masters.push_back(&m);
    rig.follow(true);
    rig.seconds(6.0);
    NEED(rig.following());
    CHECK(rig.engine.playing());
    CHECK(rig.hook.started_ == 0u);  // a Continue is not a Start
    CHECK(rig.hook.runs_ == 1u);
    CHECK(rig.check.locates() == 1u);
    check_run(rig, 8);
    CHECK(rig.hook.hits[0].tick >= 8u * kStep);  // the run is at the song position (the first step the position has not passed)
    CHECK(rig.hook.hits[0].tick <= 8u * kStep + 2u * kStep);
  }
  {
    Rig rig;
    Master m;
    m.send_before(0, {0xFA});
    m.send_before(1, {0xFC});  // Stop before the loop could attach: the engine never saw the run
    rig.masters.push_back(&m);
    rig.follow(true);
    rig.seconds(4.0);
    NEED(rig.following());
    CHECK_FALSE(rig.engine.playing());
    CHECK(rig.hook.started_ == 0u);
    CHECK(rig.hook.stopped_ == 0u);
    CHECK(rig.hook.hits.empty());
    CHECK(rig.check.ok());
  }
}

TEST_CASE("midi service: a Start whose master never gets a loop attached is forgotten, FOLLOW off drops it at once") {
  {
    Rig rig;
    Master m;
    m.quiet_from = 1;  // one clock after the Start, then nothing: the loop never has a period
    m.send_before(0, {0xFA});
    rig.masters.push_back(&m);
    rig.follow(true);
    rig.seconds(4.0);
    CHECK_FALSE(rig.following());
    // A master that comes later with clocks only must not find a run waiting.
    m.quiet_from = ~0ull;
    rig.seconds(3.0);
    NEED(rig.following());
    CHECK_FALSE(rig.engine.playing());
    CHECK(rig.hook.started_ == 0u);
  }
  {
    Rig rig;
    Master m;
    m.send_before(0, {0xFA});
    rig.masters.push_back(&m);
    rig.follow(true);
    rig.run(3);
    rig.follow(false);
    rig.seconds(3.0);
    CHECK_FALSE(rig.following());
    rig.follow(true);
    rig.seconds(3.0);
    NEED(rig.following());
    CHECK_FALSE(rig.engine.playing());  // (the Start went with FOLLOW off)
  }
}

// ---- the USB port that nobody drains --------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi service: while the USB port is not alive the DIN port keeps its clock (the default ports, the engine's own clock, no start of frame ever), and USB joins when it comes") {
  Rig rig;
  rig.usb_on = false;
  rig.run(3);
  CHECK(rig.engine.set_tempo(120'000));
  CHECK(rig.engine.play());
  rig.seconds(10.0);
  // 24 a quarter note at 120 BPM: 48 a second. The router would have stopped at its USB ring's 64 and held DIN with it.
  CHECK(rig.count(0xF8) >= 470u);
  CHECK(rig.count(0xF8) <= 490u);
  CHECK(rig.count(0xFA) == 1u);
  CHECK(rig.svc.out().stats().port_full == 0u);
  CHECK(rig.usb.empty());
  // The USB port comes up (a host configured it): what was queued for it before is not played late, new clocks reach both ports.
  const std::size_t din_before = rig.count(0xF8);
  rig.usb_on = true;
  rig.seconds(5.0);
  const std::size_t din_after = rig.count(0xF8) - din_before;
  CHECK(din_after >= 230u);
  CHECK(din_after <= 250u);
  std::size_t usb_clocks = 0;
  for (const Rig::UsbEvent& ue : rig.usb) {
    std::uint8_t b[3];
    if (midi::usb_event_bytes(ue.word, b) > 0 && b[0] == 0xF8) {
      ++usb_clocks;
    }
  }
  CHECK(usb_clocks >= 225u);
  CHECK(usb_clocks <= 250u);
  CHECK(rig.svc.out().stats().port_full == 0u);
  // And it goes again: DIN is not held by a ring that nobody empties.
  rig.usb_on = false;
  const std::size_t usb_mark = rig.usb.size();
  const std::size_t din_mark = rig.count(0xF8);
  rig.seconds(5.0);
  CHECK(rig.count(0xF8) - din_mark >= 230u);
  CHECK(rig.usb.size() == usb_mark);
}

TEST_CASE("midi service: a message addressed to the USB port alone is dropped while it is not alive, one for both goes to DIN, and the notes of the default ports keep to DIN") {
  Rig rig;
  rig.usb_on = false;
  rig.hook.send_note = true;
  rig.run(2);
  CHECK(rig.engine.play());
  rig.run(3);
  CHECK(rig.engine.stop());
  rig.run(12);
  CHECK(rig.usb.empty());
  std::vector<std::uint8_t> status;
  for (const WireByte& w : rig.wire) {
    if ((w.byte & 0x80u) != 0u && w.byte != 0xF8) {
      status.push_back(w.byte);
    }
  }
  CHECK((status == std::vector<std::uint8_t>{0xFA, 0x90, 0xFC, 0x80} || status == std::vector<std::uint8_t>{0xFA, 0x90, 0x80, 0xFC}));
  rig.wire.clear();
  // send() from the main loop with explicit ports.
  NEED(rig.svc.send(core::midi_control(1, 7, 99, 0, core::kMidiUsb)));
  NEED(rig.svc.send(core::midi_control(1, 8, 98, 0, core::kMidiAllPorts)));
  rig.run(12);
  CHECK(rig.svc.stats().usb_skipped == 1u);
  CHECK(rig.usb.empty());
  std::size_t ccs = 0;
  for (const WireByte& w : rig.wire) {
    ccs += w.byte == 0xB0 ? 1u : 0u;
  }
  CHECK(ccs == 1u);
}

// ---- the engine's tempo range ------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi service: a master beyond the engine's tempo range (20 to 300 BPM) is followed at the engine's limit: what it renders is the frame, and no step is lost or doubled") {
  struct Profile {
    const char* name;
    std::function<double(double)> bpm;
    double seconds;
  };
  const Profile profiles[] = {
      {"250 BPM", [](double) { return 250.0; }, 20.0},
      {"300 BPM", [](double) { return 300.0; }, 20.0},
      {"a ramp 240 -> 330 -> 280", [](double t) { return t < 15.0 ? 240.0 + 6.0 * t : std::max(280.0, 330.0 - 6.0 * (t - 15.0)); }, 30.0},
      {"20 BPM", [](double) { return 20.0; }, 80.0},
      {"25 BPM", [](double) { return 25.0; }, 80.0},
      {"a ramp 30 -> 15 -> 28", [](double t) { return t < 30.0 ? 30.0 - 0.5 * t : 15.0 + 0.4 * (t - 30.0); }, 80.0},
  };
  for (const Profile& p : profiles) {
    INFO(p.name);
    Rig rig;
    Master m;
    m.bpm = p.bpm;
    m.send_before(0, {0xFA});  // the run starts at the loop's attach: the position catches up at its slew, the increment moves
    rig.masters.push_back(&m);
    rig.follow(true);
    rig.seconds(p.seconds);
    NEED(rig.following());
    CHECK(rig.engine.playing());
    check_run(rig, 20);
    CHECK(rig.audit.owned() > 1000u);
    // The engine's tempo is the master's, held between the limits.
    CHECK(rig.engine.tempo() >= dsp::kMinTempoMilliBpm);
    CHECK(rig.engine.tempo() <= dsp::kMaxTempoMilliBpm);
  }
}
