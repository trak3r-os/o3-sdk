// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine following a MIDI master (docs/M2B2-PLAN.md section 3): a scripted ClockFrame stream, as a correct device (hal/midi.h) makes it,
// goes through Engine::follow and render, and a clocked hook that walks ctx.transport() the way o3-303's sequencer does lands its steps on the
// master's grid. Start, Continue at a song position, Stop, a master that goes quiet, a tempo that slews, a hook that sends MIDI, a note that is
// injected at its frame. The device side (the DLL itself) is the unit's; the fake that stands for it in the simulator is checked against the
// engine's own arithmetic at the end.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/core/clock_continuity.h>
#include <o3sdk/core/midi_types.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/platform/host/fake_midi.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace o3sdk;

namespace {

constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kStep = dsp::kTicksPerSixteenth;  // 240 ticks: the grid of a sixteenth, and of one MIDI beat

// A sequencer in the shape of o3-303's (app/seq sequencer.cpp, Sequencer::sequence): a run begins at the first whole tick the transport has not
// passed, ends on a stop, a pause or a restart, and walks its own grid with frame_at(); it records where every step landed on the timeline.
class RunHook final : public dsp::Clocked {
 public:
  struct Hit {
    std::uint64_t sample;  // block * 64 + frame
    std::uint64_t tick;
  };
  void block(dsp::BlockContext& ctx) noexcept override {
    const dsp::Transport& t = ctx.transport();
    const std::uint64_t base = blocks_++ * kBlock;
    saw_following_ = t.following;
    if (t.stopped) {
      ++stopped_;
    }
    if (t.started) {
      ++started_;
    }
    playing_ = t.playing;
    if (t.stopped && running_) {
      running_ = false;
    }
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
    last_inc_ = t.inc;
    last_tempo_ = t.tempo_mbpm;
  }
  std::vector<Hit> hits;
  std::uint64_t blocks_ = 0;
  unsigned runs_ = 0;
  unsigned started_ = 0;
  unsigned stopped_ = 0;
  bool running_ = false;
  bool playing_ = false;
  bool saw_following_ = false;
  std::uint64_t step_tick_ = 0;
  std::uint64_t last_inc_ = 0;
  std::uint32_t last_tempo_ = 0;
};

// A scripted master: what a device that keeps the contract of hal/midi.h reports, block after block. The position is continuous (pos + inc * 64);
// only a Start or a Continue moves it.
struct Master {
  std::uint32_t mbpm = 120'000;
  bool playing = false;
  std::uint64_t pos = 0;
  bool locate = false;  // the last frame acted on a song position

  core::ClockFrame next(bool start = false, bool cont = false, unsigned beats = 0, bool stop = false) {
    locate = cont;
    core::ClockFrame f;
    f.follow = true;
    f.tempo_mbpm = mbpm;
    f.inc = dsp::ticks_per_sample_q32(mbpm);
    if (start) {
      playing = true;
      pos = 0;
      f.started = true;
    }
    if (cont) {
      playing = true;
      pos = (static_cast<std::uint64_t>(beats) * kStep) << 32;
    }
    if (stop) {
      f.stopped = playing;
      playing = false;
      pos = 0;
    }
    f.playing = playing;
    f.pos = playing ? pos : 0u;
    if (playing) {
      pos += f.inc * kBlock;
    }
    return f;
  }
};

struct Rig {
  dsp::Engine engine;
  RunHook hook;
  core::ClockContinuity check;  // over every frame a test hands the engine
  float l[kBlock] = {};
  float r[kBlock] = {};
  Rig() {
    NEED(engine.add_hook(hook));
    engine.start();
  }
  void block(const core::ClockFrame& f, bool locate = false) {
    check.feed(f, kBlock, locate);
    engine.follow(f);
    engine.render(nullptr, nullptr, l, r, kBlock);
  }
  void blocks(Master& m, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      block(m.next());
    }
  }
  void block() {  // no follow(): the engine's own transport
    check.feed(core::ClockFrame{}, kBlock);
    engine.render(nullptr, nullptr, l, r, kBlock);
  }
};

double rate() { return static_cast<double>(core::kRefRateHz); }

// The sample at which a master at a steady `mbpm` whose run began at sample `origin` reaches `tick`, as a real number.
double exact_sample(double origin, std::uint32_t mbpm, std::uint64_t tick) {
  return origin + static_cast<double>(tick) * 60.0 * rate() / (static_cast<double>(mbpm) * 0.001 * dsp::kPpqn);
}

}  // namespace

TEST_CASE("follow: a hook's steps land on the master's grid within a sample, at several tempi") {
  for (const std::uint32_t mbpm : {90'000u, 120'000u, 133'333u, 174'500u}) {
    Rig rig;
    Master m;
    m.mbpm = mbpm;
    rig.block(m.next());  // a master that is there, not playing yet
    CHECK(rig.engine.following());
    CHECK(rig.hook.saw_following_);
    const std::uint64_t origin = rig.hook.blocks_ * kBlock;  // the run begins at the start of the next block
    rig.block(m.next(true));
    CHECK(rig.hook.started_ == 1u);
    rig.blocks(m, 7 * 44100 / kBlock);  // seven seconds
    NEED(rig.hook.hits.size() > 20u);
    CHECK(rig.hook.runs_ == 1u);
    for (std::size_t i = 0; i < rig.hook.hits.size(); ++i) {
      const auto& h = rig.hook.hits[i];
      CHECK(h.tick == i * kStep);  // every step of the grid, once, in order
      const double want = exact_sample(static_cast<double>(origin), mbpm, h.tick);
      const double got = static_cast<double>(h.sample);
      // The first sample at or after the master's tick: within one sample (the engine's increment is exact to 1e-8).
      CHECK(got >= want - 1e-3);
      CHECK(got < want + 1.0 + 1e-3);
    }
    CHECK(rig.check.ok());
    CHECK(rig.check.starts() == 1u);
  }
}

TEST_CASE("follow: a tempo that slews (a clock-follow loop) loses no step and repeats none, and the position never jumps") {
  Rig rig;
  Master m;
  m.mbpm = 100'000;
  rig.block(m.next());
  rig.block(m.next(true));
  std::vector<std::uint64_t> pos;  // the position at the first frame of each block, as the engine saw it
  std::vector<std::uint64_t> inc;
  for (unsigned b = 0; b < 12000; ++b) {
    // The loop slews: a different increment every block, continuous position. Every 700 blocks it "rebases" (a new origin for its own
    // arithmetic): the position carries on exactly where the last increment left it.
    const int phase = static_cast<int>(b % 1400u);
    m.mbpm = static_cast<std::uint32_t>(100'000 + (phase < 700 ? phase : 1400 - phase) * 60);
    const core::ClockFrame f = m.next();
    pos.push_back(f.pos);
    inc.push_back(f.inc);
    rig.block(f);
  }
  const auto& hits = rig.hook.hits;
  NEED(hits.size() > 100u);
  // None lost: the ticks run 0, 240, 480 ... to the last whole step the master passed.
  for (std::size_t i = 0; i < hits.size(); ++i) {
    CHECK(hits[i].tick == i * kStep);
  }
  const std::uint64_t final_tick = (m.pos >> 32) / kStep;
  CHECK(hits.size() >= final_tick - 1u);
  CHECK(hits.size() <= final_tick + 2u);
  // Each landed on the first sample whose position reached it (the first frame f with pos + f * inc >= tick), from the stream alone.
  for (const auto& h : hits) {
    if (h.sample < 2 * kBlock) {
      continue;  // (the first two blocks were the Start's)
    }
    const std::size_t b = static_cast<std::size_t>(h.sample / kBlock) - 2u;  // two blocks came before the stream
    const std::uint64_t f = h.sample % kBlock;
    const std::uint64_t q = h.tick << 32;
    NEED(b < pos.size());
    CHECK(pos[b] + f * inc[b] >= q);
    if (f > 0u) {
      CHECK(pos[b] + (f - 1u) * inc[b] < q);
    } else if (b > 0u) {
      CHECK(pos[b - 1u] + (kBlock - 1u) * inc[b - 1u] < q);
    }
  }
  CHECK(rig.engine.tempo() == m.mbpm);
  CHECK(rig.check.ok());  // the stream's position never jumped, whatever the increment did
  CHECK(rig.check.jumps() == 0u);
}

TEST_CASE("follow: Start, Stop and Start again: the hook sees each once, and the engine reports the master's state") {
  Rig rig;
  Master m;
  rig.block(m.next());
  CHECK_FALSE(rig.engine.playing());
  rig.block(m.next(true));
  rig.blocks(m, 100);
  CHECK(rig.engine.playing());
  CHECK(rig.engine.stats().playing);
  CHECK(rig.hook.started_ == 1u);
  CHECK(rig.hook.runs_ == 1u);
  const std::size_t before_stop = rig.hook.hits.size();
  NEED(before_stop > 0u);

  rig.block(m.next(false, false, 0, true));  // Stop
  CHECK(rig.hook.stopped_ == 1u);
  CHECK_FALSE(rig.engine.playing());
  CHECK_FALSE(rig.engine.stats().playing);
  CHECK(rig.engine.stats().pos == 0u);
  rig.blocks(m, 50);
  CHECK(rig.hook.stopped_ == 1u);  // once
  CHECK(rig.hook.hits.size() == before_stop);

  rig.block(m.next(true));  // Start: from the top again
  CHECK(rig.hook.started_ == 2u);
  rig.blocks(m, 100);
  CHECK(rig.hook.runs_ == 2u);
  CHECK(rig.hook.hits[before_stop].tick == 0u);
  CHECK(rig.engine.playing());
  CHECK(rig.check.ok());
  CHECK(rig.check.starts() == 2u);
}

TEST_CASE("follow: a restart under the master (Start while playing) is a new run") {
  Rig rig;
  Master m;
  rig.block(m.next(true));
  rig.blocks(m, 50);
  rig.block(m.next(true));
  CHECK(rig.hook.started_ == 2u);
  rig.blocks(m, 10);
  CHECK(rig.hook.runs_ == 2u);
}

TEST_CASE("follow: Continue after a song position resumes on the master's grid, not from step 0's position") {
  Rig rig;
  Master m;
  m.mbpm = 126'000;
  rig.block(m.next());
  rig.block(m.next(false, true, 8), true);  // SPP 8: eight sixteenths in; Continue
  CHECK(rig.hook.started_ == 0u);      // not a Start
  CHECK(rig.engine.playing());
  rig.blocks(m, 2 * 44100 / kBlock);
  NEED(rig.hook.hits.size() > 10u);
  CHECK(rig.hook.runs_ == 1u);
  CHECK(rig.hook.hits[0].tick == 8u * kStep);  // the first step is the one at the song position
  for (std::size_t i = 0; i < rig.hook.hits.size(); ++i) {
    CHECK(rig.hook.hits[i].tick == (8u + i) * kStep);
  }
  // Steady tempo: the interval between two steps is a sixteenth, to the sample.
  const double want = 60.0 * rate() / (126.0 * 4.0);  // 60 / bpm / 4 seconds, in samples
  const double span = static_cast<double>(rig.hook.hits.back().sample - rig.hook.hits.front().sample);
  CHECK(std::fabs(span / static_cast<double>(rig.hook.hits.size() - 1u) - want) < 1.0 / static_cast<double>(rig.hook.hits.size() - 1u) + 1e-3);
  CHECK(rig.check.ok());
  CHECK(rig.check.locates() == 1u);
  // The transport the engine ran: the position is the song position plus what has gone by.
  CHECK(rig.engine.stats().pos > (static_cast<std::uint64_t>(8u * kStep) << 32));
}

TEST_CASE("follow: a master that goes quiet stops the transport once and is let go; the engine's own clock is back") {
  Rig rig;
  Master m;
  m.mbpm = 150'000;
  rig.engine.set_tempo(90'000);
  rig.block();  // the command is applied
  rig.block(m.next(true));
  rig.blocks(m, 200);
  CHECK(rig.engine.tempo() == 150'000u);  // the master's
  CHECK(rig.engine.following());

  // 48 clocks of silence later the device stops the transport (follow still set), then stops following.
  core::ClockFrame gone = m.next(false, false, 0, true);
  rig.block(gone);
  CHECK(rig.hook.stopped_ == 1u);
  CHECK_FALSE(rig.engine.playing());
  core::ClockFrame free_frame;  // follow false
  rig.block(free_frame);
  CHECK_FALSE(rig.engine.following());
  CHECK(rig.engine.tempo() == 90'000u);  // the engine's own tempo is back
  CHECK_FALSE(rig.hook.saw_following_);
  CHECK(rig.hook.stopped_ == 1u);  // no second stop

  // Play now works on the engine's own transport, at its own tempo.
  const std::size_t hits = rig.hook.hits.size();
  CHECK(rig.engine.play());
  for (unsigned i = 0; i < 400; ++i) {
    rig.block();
  }
  CHECK(rig.engine.playing());
  CHECK(rig.hook.started_ == 2u);
  CHECK(rig.hook.last_tempo_ == 90'000u);
  CHECK(rig.hook.hits.size() > hits);
  CHECK(rig.hook.hits[hits].tick == 0u);
}

TEST_CASE("follow: the user turning FOLLOW off under a playing master stops the transport once; playing() is false until the next play()") {
  Rig rig;
  CHECK(rig.engine.play());  // the engine played on its own before the master came
  rig.block();
  rig.block();
  CHECK(rig.engine.playing());
  Master m;
  rig.block(m.next(true));
  CHECK(rig.hook.stopped_ == 1u);  // the engine's own run ended when the master took over
  rig.blocks(m, 50);
  CHECK(rig.engine.playing());
  rig.block();  // no frame with follow: let go
  CHECK(rig.hook.stopped_ == 2u);
  CHECK_FALSE(rig.hook.playing_);
  CHECK_FALSE(rig.engine.following());
  CHECK_FALSE(rig.engine.playing());  // (what the main loop asked for before the master is void: the transport stopped)
  rig.block();
  CHECK(rig.hook.stopped_ == 2u);
  CHECK(rig.engine.play());
  rig.block();
  rig.block();
  CHECK(rig.engine.playing());
  CHECK(rig.hook.playing_);
}

TEST_CASE("follow: while following, play() and stop() are requests for the device, set_tempo waits, and the master's tempo shows") {
  Rig rig;
  Master m;
  m.mbpm = 133'000;
  rig.block(m.next());
  CHECK(rig.engine.following());
  CHECK(rig.engine.take_transport_request() == core::TransportRequest::None);
  CHECK(rig.engine.play());
  CHECK(rig.engine.take_transport_request() == core::TransportRequest::Play);
  CHECK(rig.engine.take_transport_request() == core::TransportRequest::None);  // once
  rig.block(m.next());
  CHECK_FALSE(rig.hook.playing_);  // nothing started by itself
  CHECK_FALSE(rig.engine.playing());
  CHECK(rig.engine.stop());
  CHECK(rig.engine.take_transport_request() == core::TransportRequest::Stop);
  CHECK(rig.engine.set_tempo(77'000));
  CHECK(rig.engine.tempo() == 133'000u);  // still the master's
  CHECK(rig.engine.own_tempo() == 77'000u);  // and the own one is what a "+ 1 BPM" steps from
  rig.block(m.next());
  CHECK(rig.engine.tempo() == 133'000u);
  rig.block();  // let go
  CHECK(rig.engine.tempo() == 77'000u);
  // Without a master a request is not made: play() is a command.
  CHECK(rig.engine.play());
  CHECK(rig.engine.take_transport_request() == core::TransportRequest::None);
}

TEST_CASE("follow: a master joined while the engine plays on its own ends that run once; a stop the frame forgot to flag is flagged") {
  Rig rig;
  CHECK(rig.engine.play());
  for (unsigned i = 0; i < 100; ++i) {
    rig.block();
  }
  CHECK(rig.hook.runs_ == 1u);
  CHECK(rig.hook.stopped_ == 0u);
  Master m;
  rig.block(m.next());  // the master appears, not playing: the engine's own run ends
  CHECK(rig.hook.stopped_ == 1u);
  CHECK_FALSE(rig.hook.playing_);
  // A master that is already running when it is joined: the engine's run is ended and the master's begins (a hook that keeps state starts over).
  Rig other;
  CHECK(other.engine.play());
  for (unsigned i = 0; i < 100; ++i) {
    other.block();
  }
  Master running;
  running.playing = true;
  running.pos = 12'345ull << 32;
  other.block(running.next());
  CHECK(other.hook.stopped_ == 1u);
  CHECK(other.hook.playing_);
  CHECK(other.hook.runs_ == 2u);
  CHECK(other.hook.hits.back().tick >= 12'345u);

  // Playing then not playing with no `stopped` in the frame: the engine says it.
  Rig forgot;
  Master f;
  forgot.block(f.next(true));
  forgot.blocks(f, 10);
  core::ClockFrame no_flag = f.next();
  no_flag.playing = false;
  no_flag.stopped = false;
  forgot.block(no_flag);
  CHECK(forgot.hook.stopped_ == 1u);
}

TEST_CASE("follow: a frame is sanitised: an absurd increment is held to a legal tempo's, a missing one is made from the tempo") {
  Rig rig;
  Master m;
  m.mbpm = 120'000;
  rig.block(m.next(true));
  core::ClockFrame wild = m.next();
  wild.inc = 1ull << 60;
  wild.tempo_mbpm = 5'000'000;  // 5000 BPM
  rig.block(wild);
  CHECK(rig.hook.last_inc_ <= dsp::ticks_per_sample_q32(dsp::kMaxTempoMilliBpm));
  CHECK(rig.hook.last_tempo_ == dsp::kMaxTempoMilliBpm);
  core::ClockFrame slow = m.next();
  slow.inc = 3;
  rig.block(slow);
  CHECK(rig.hook.last_inc_ >= dsp::ticks_per_sample_q32(dsp::kMinTempoMilliBpm));
  core::ClockFrame none = m.next();
  none.inc = 0;
  none.tempo_mbpm = 100'000;
  rig.block(none);
  CHECK(rig.hook.last_inc_ == dsp::ticks_per_sample_q32(100'000));
}

namespace {

class SendHook final : public dsp::Clocked {
 public:
  void block(dsp::BlockContext& ctx) noexcept override {
    results.clear();
    for (unsigned i = 0; i < want; ++i) {
      const core::MidiMessage m = core::midi_note_on(1, static_cast<std::uint8_t>(i), 100, static_cast<std::uint16_t>(i * 7u));
      results.push_back(ctx.send_midi(m));
    }
  }
  unsigned want = 0;
  std::vector<bool> results;
};

class Recorder final : public dsp::Unit {
 public:
  Recorder() : info_{"REC", dsp::UnitKind::Instrument, nullptr, 0, nullptr, 0} {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override {}
  void process(const dsp::ProcessArgs& a) noexcept override {
    ++calls;
    for (std::uint32_t i = 0; i < a.event_count; ++i) {
      events.push_back(a.events[i]);
    }
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      a.out_l[i] = 0.0f;
      a.out_r[i] = 0.0f;
    }
  }
  dsp::UnitInfo info_;
  unsigned calls = 0;
  std::vector<dsp::Event> events;
};

class Placer final : public dsp::Clocked {
 public:
  void block(dsp::BlockContext& ctx) noexcept override { (void)ctx.emit(unit, dsp::note_on(70, 90, 0, 20)); }
  dsp::UnitId unit = 0;
};

}  // namespace

TEST_CASE("follow: send_midi takes 32 a block, clamps the frame, keeps the order and resets every block") {
  dsp::Engine e;
  SendHook hook;
  NEED(e.add_hook(hook));
  e.start();
  float l[kBlock];
  float r[kBlock];
  hook.want = 40;
  e.render(nullptr, nullptr, l, r, kBlock);
  unsigned n = 0;
  const core::MidiMessage* out = e.midi_out(n);
  NEED(n == dsp::kMaxMidiOut);
  for (unsigned i = 0; i < n; ++i) {
    CHECK(out[i].data1 == i);  // the order the hook sent them in
    CHECK(out[i].status == core::midi_status(core::kMidiNoteOn, 1));
    CHECK(out[i].frame < kBlock);  // 31 * 7 = 217 is past the block: clamped to its last frame
  }
  CHECK(out[1].frame == 7u);
  CHECK(out[31].frame == kBlock - 1u);
  for (unsigned i = 0; i < 40; ++i) {
    CHECK(hook.results[i] == (i < 32u));
  }
  CHECK(e.stats().dropped_midi == 8u);
  hook.want = 3;
  e.render(nullptr, nullptr, l, r, kBlock);
  out = e.midi_out(n);
  CHECK(n == 3u);
  CHECK(e.stats().dropped_midi == 8u);  // a total since start()
  hook.want = 0;
  e.render(nullptr, nullptr, l, r, kBlock);
  (void)e.midi_out(n);
  CHECK(n == 0u);
}

TEST_CASE("follow: inject puts a note at its frame, among the hooks' events and in order; an unknown unit is refused") {
  dsp::Engine e;
  Recorder rec;
  Placer placer;
  const dsp::UnitId u = e.attach(rec);
  NEED(u != dsp::kNoUnit);
  NEED(e.set_source(0, u));
  placer.unit = u;
  NEED(e.add_hook(placer));
  e.start();
  float l[kBlock];
  float r[kBlock];
  CHECK(e.inject(u, dsp::note_on(60, 100, 0, 40)));
  CHECK(e.inject(u, dsp::note_on(61, 100, 0, 5)));
  CHECK(e.inject(u, dsp::note_off(60, 63)));
  CHECK_FALSE(e.inject(dsp::kNoUnit, dsp::note_on(60, 100, 0, 0)));
  CHECK_FALSE(e.inject(9, dsp::note_on(60, 100, 0, 0)));
  e.render(nullptr, nullptr, l, r, kBlock);
  // (the first process() call was start()'s, of zero frames; the block's is the second)
  NEED(rec.events.size() >= 4u);
  const std::vector<dsp::Event> got(rec.events.end() - 4, rec.events.end());
  CHECK(got[0].frame == 5u);
  CHECK(got[0].note == 61u);
  CHECK(got[1].frame == 20u);  // the hook's
  CHECK(got[1].note == 70u);
  CHECK(got[2].frame == 40u);
  CHECK(got[2].note == 60u);
  CHECK(got[3].frame == 63u);
  CHECK(got[3].kind == dsp::EventKind::NoteOff);
  rec.events.clear();
  e.render(nullptr, nullptr, l, r, kBlock);  // consumed: the next block starts clean (only the hook's note)
  CHECK(rec.events.size() == 1u);
  // A flood of notes cannot fill the unit's list: at most kMaxInjectPerUnit are taken, so the hook's note-off still has room. The rest are
  // refused and counted.
  static_assert(dsp::kMaxInjectPerUnit < dsp::kMaxEventsPerUnit, "the hooks keep some of a unit's list");
  unsigned taken = 0;
  for (unsigned i = 0; i < dsp::kMaxEventsPerUnit; ++i) {
    taken += e.inject(u, dsp::note_on(static_cast<std::uint8_t>(30 + i), 100, 0, static_cast<std::uint16_t>(i))) ? 1u : 0u;
  }
  CHECK(taken == dsp::kMaxInjectPerUnit);
  CHECK(e.stats().dropped_events == 0u);  // (published at the end of the next render)
  rec.events.clear();
  e.render(nullptr, nullptr, l, r, kBlock);
  CHECK(e.stats().dropped_events == dsp::kMaxEventsPerUnit - dsp::kMaxInjectPerUnit);
  // The hook's note (frame 20) is there with the injected ones: none of them pushed it out.
  CHECK(rec.events.size() == dsp::kMaxInjectPerUnit + 1u);
  bool hooks_note = false;
  for (const dsp::Event& ev : rec.events) {
    hooks_note = hooks_note || (ev.note == 70u && ev.frame == 20u);
  }
  CHECK(hooks_note);
  // The count is per block: the next block takes a full allowance again.
  taken = 0;
  for (unsigned i = 0; i < dsp::kMaxEventsPerUnit; ++i) {
    taken += e.inject(u, dsp::note_off(static_cast<std::uint8_t>(30 + i), 3)) ? 1u : 0u;
  }
  CHECK(taken == dsp::kMaxInjectPerUnit);
}

TEST_CASE("follow: block_frame is the transport as the block ran it") {
  Rig rig;
  Master m;
  m.mbpm = 140'000;
  rig.block(m.next(true));
  const core::ClockFrame& a = rig.engine.block_frame();
  CHECK(a.follow);
  CHECK(a.playing);
  CHECK(a.started);
  CHECK(a.pos == 0u);
  CHECK(a.inc == dsp::ticks_per_sample_q32(140'000));
  CHECK(a.tempo_mbpm == 140'000u);
  const std::uint64_t inc = a.inc;
  rig.block(m.next());
  const core::ClockFrame& b = rig.engine.block_frame();
  CHECK_FALSE(b.started);
  CHECK(b.pos == inc * kBlock);
  rig.block(m.next(false, false, 0, true));
  CHECK(rig.engine.block_frame().stopped);
  CHECK_FALSE(rig.engine.block_frame().playing);
  // The engine's own transport: follow false, and the clock out is the engine's.
  rig.block();
  CHECK_FALSE(rig.engine.block_frame().follow);
  CHECK(rig.engine.play());
  rig.block();
  const core::ClockFrame& own = rig.engine.block_frame();
  CHECK_FALSE(own.follow);
  CHECK(own.playing);
  CHECK(own.started);
  CHECK(own.tempo_mbpm == 120'000u);
}

TEST_CASE("follow: a platform without MIDI never calls follow(): the engine's own transport is exactly what it was") {
  Rig rig;
  CHECK(rig.engine.play());
  CHECK(rig.engine.set_tempo(100'000));
  for (unsigned i = 0; i < 5000; ++i) {
    rig.block();
  }
  CHECK_FALSE(rig.engine.following());
  CHECK(rig.engine.playing());
  CHECK(rig.engine.tempo() == 100'000u);
  CHECK(rig.hook.runs_ == 1u);
  CHECK_FALSE(rig.hook.saw_following_);
  const double want = 60.0 * rate() / (100.0 * 4.0);
  NEED(rig.hook.hits.size() > 10u);
  for (std::size_t i = 0; i < rig.hook.hits.size(); ++i) {
    CHECK(std::fabs(static_cast<double>(rig.hook.hits[i].sample) - want * static_cast<double>(i) - 0.5) <= 0.5 + 1e-3);
  }
}

TEST_CASE("follow: the host fake maps ticks to frames and tempo to increments as the engine does") {
  for (const std::uint32_t mbpm : {20'000u, 90'000u, 120'000u, 133'333u, 300'000u}) {
    CHECK(platform::host::FakeMidi::inc_for(mbpm) == dsp::ticks_per_sample_q32(mbpm));
  }
  dsp::Transport t;
  t.playing = true;
  t.frames = kBlock;
  t.inc = dsp::ticks_per_sample_q32(123'456);
  for (std::uint64_t pos : {0ull, 1ull << 31, 37ull << 32, (5'000ull << 32) + 12'345u}) {
    t.pos = pos;
    for (std::uint64_t tick = (pos >> 32) > 3 ? (pos >> 32) - 3 : 0; tick < (pos >> 32) + 10; ++tick) {
      CHECK(platform::host::FakeMidi::frame_of(t.pos, t.inc, t.frames, tick) == dsp::frame_at(t, tick));
    }
  }
}

TEST_CASE("follow: the continuity checker passes a clean stream and names each way a stream breaks the contract") {
  const std::uint64_t inc = dsp::ticks_per_sample_q32(120'000);
  const auto frame = [&](bool playing, bool started, std::uint64_t pos) {
    core::ClockFrame f;
    f.follow = true;
    f.playing = playing;
    f.started = started;
    f.pos = pos;
    f.inc = inc;
    f.tempo_mbpm = 120'000;
    return f;
  };
  {  // a clean run: idle, Start, continuous, Stop, idle, a Continue at a song position, then a locate while playing
    core::ClockContinuity c;
    CHECK(c.feed(frame(false, false, 0), kBlock));
    CHECK(c.feed(frame(true, true, 0), kBlock));
    CHECK(c.feed(frame(true, false, inc * kBlock), kBlock));
    CHECK(c.feed(frame(true, false, 2 * inc * kBlock), kBlock));
    CHECK(c.feed(frame(false, false, 0), kBlock));
    CHECK(c.feed(frame(true, false, 1920ull << 32), kBlock));  // Continue: from the song position
    CHECK(c.feed(frame(true, false, (1920ull << 32) + inc * kBlock), kBlock));
    CHECK(c.feed(frame(true, false, 77ull << 32), kBlock, true));  // an SPP mid-run, flagged by the caller
    CHECK(c.feed(frame(true, false, (77ull << 32) + inc * kBlock), kBlock));
    CHECK(c.ok());
    CHECK(c.blocks() == 9u);
    CHECK(c.starts() == 1u);
    CHECK(c.locates() == 2u);
  }
  {  // a jump: the position is not where the block before ended
    core::ClockContinuity c;
    CHECK(c.feed(frame(true, true, 0), kBlock));
    CHECK_FALSE(c.feed(frame(true, false, inc * kBlock + 1u), kBlock));
    CHECK(c.jumps() == 1u);
    CHECK_FALSE(c.ok());
    // ... and the next block is judged against the one that jumped, so one jump is one violation.
    CHECK(c.feed(frame(true, false, inc * kBlock + 1u + inc * kBlock), kBlock));
    CHECK(c.jumps() == 1u);
    c.reset();
    CHECK(c.ok());
    CHECK(c.blocks() == 0u);
  }
  {  // a position while idle, a start with no run or not at the top, a run with no increment
    core::ClockContinuity c;
    CHECK_FALSE(c.feed(frame(false, false, 5), kBlock));
    CHECK(c.idle_pos() == 1u);
    CHECK_FALSE(c.feed(frame(false, true, 0), kBlock));
    CHECK_FALSE(c.feed(frame(true, true, 3), kBlock));
    CHECK(c.bad_starts() == 2u);
    core::ClockFrame none = frame(true, false, 0);
    none.inc = 0;
    CHECK_FALSE(c.feed(none, kBlock));
    CHECK(c.no_inc() == 1u);
  }
  {  // a frame that is not a master's ends the run of checks (the engine's own transport has no contract to keep)
    core::ClockContinuity c;
    CHECK(c.feed(frame(true, true, 0), kBlock));
    CHECK(c.feed(core::ClockFrame{}, kBlock));
    CHECK(c.feed(frame(true, false, 999ull << 32), kBlock));  // a master joined while it plays: a locate, not a jump
    CHECK(c.ok());
    CHECK(c.locates() == 1u);
    CHECK(c.blocks() == 2u);
  }
}
