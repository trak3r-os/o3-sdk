// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI with no wire behind it: hal::midi::Device for the host tests and the simulator (docs/M2B2-PLAN.md section 3). Scripted input, captured
// output and a clock generator, so that notes, controllers, a master's clock and the clock and transport the app sends can all be driven and
// looked at without a port. It is also the reference model of what a real device (platform/mk66's MidiService) promises the kit (hal/midi.h).
//
//   input      in(port, status, d1, d2, at) / in_bytes(): a channel message arrives at a timeline sample (kNow: frame 0 of the next block).
//              Notes and the all-notes-off controllers are handed to begin_block() with their frame; every channel message is also queued for
//              poll(). Bytes of the system messages (the clock, Start ...) are not input: the clock is the generator's.
//   generator  clock_start(bpm), clock_continue(bpm, beats), clock_stop(bpm), clock_tempo(bpm), clock_off(): an ideal master. While
//              Config::follow is set and the master is there, begin_block() gives the engine the master's transport: Start begins a run with
//              pos 0 at the next block, Continue at the song position, Stop stops it, and the position and increment are exact
//              (Q32.32 ticks, the engine's own arithmetic), so a hook's steps land on the master's grid. Transport events need
//              Config::transport_in. A master that goes away (clock_off) is flywheeled for 48 clocks, then the transport stops and
//              the master is let go, as the real loop does. A Play or Stop request while following is taken at the next block.
//              Every frame the fake gives the engine goes through a core::ClockContinuity (`continuity`), so a test can end with
//              CHECK(midi.continuity.ok()): the stream kept the contract (no jump in the position, nothing playing at a position but 0 ...).
//   output     end_block() records, as `Sent`, the messages the hooks sent and the clock and transport the transport implies: Start at
//              `started`, Stop at `stopped`, SPP + Continue where a run begins elsewhere, a clock at every frame_at(t, k * 40) of a playing
//              block, on Config::out_ports as Config::clock_out and Config::transport_out say. send() records a message of the main loop.
//
// Time. The fake counts the timeline in samples: block_start() is the first sample of the next block, advanced by the frames of each
// begin_block(). The simulator's harness stamps scripted input from its own count of rendered samples (the same timeline).
//
// Not for the target: standard library allowed here (platform/host); nothing below the app layer includes this header.

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/core/clock_continuity.h>
#include <o3sdk/hal/midi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace o3sdk::platform::host {

class FakeMidi final : public hal::midi::Device {
 public:
  using Port = hal::midi::Port;

  static constexpr std::uint64_t kNow = ~0ull;           // `at`: frame 0 of the next block
  static constexpr std::size_t kRing = 64;               // messages the poll() side keeps, as a real ring does; the newest past it is dropped
  static constexpr std::uint32_t kStaleClocks = 48;      // a master quieter than this many clocks is gone
  static constexpr std::uint32_t kTicksPerClock = 40;    // 960 ppqn / 24
  static constexpr std::uint32_t kTicksPerBeat = 240;    // an SPP beat is six clocks

  struct Sent {
    std::uint64_t sample;  // the timeline sample it leaves at: the block's first sample plus its frame
    std::uint8_t ports;    // resolved: the bits it goes on (never 0)
    std::uint8_t status;
    std::uint8_t data1;
    std::uint8_t data2;
  };

  // ---- Scripted input -------------------------------------------------------------------------------------------------------------
  void in(Port port, std::uint8_t status, std::uint8_t d1, std::uint8_t d2, std::uint64_t at = kNow) {
    if (!core::midi_is_channel(status)) {
      return;
    }
    ++rx_;
    const std::uint64_t when = at == kNow ? block_start_ : at;
    const core::MidiMessage m{hal::midi::port_bit(port), status, static_cast<std::uint8_t>(d1 & 0x7Fu), static_cast<std::uint8_t>(d2 & 0x7Fu), 0};
    const std::uint8_t kind = core::midi_kind(status);
    if (kind == core::kMidiNoteOn || kind == core::kMidiNoteOff ||
        (kind == core::kMidiControl && (m.data1 == core::kCcAllSoundOff || m.data1 == core::kCcAllNotesOff))) {
      auto it = std::upper_bound(timed_.begin(), timed_.end(), when, [](std::uint64_t t, const Timed& x) { return t < x.at; });
      timed_.insert(it, Timed{m, when});
    }
    if (polled_.size() >= kRing) {
      ++dropped_;
    } else {
      polled_.push_back(m);
    }
  }
  // The message arrives at `frame` of the next block.
  void in_frame(unsigned frame, Port port, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) { in(port, status, d1, d2, block_start_ + frame); }
  // Raw bytes of one port with running status (a real-time byte inside a message is skipped; anything that is not a channel message is skipped).
  // Returns the channel messages found.
  std::size_t in_bytes(Port port, const std::uint8_t* bytes, std::size_t n, std::uint64_t at = kNow) {
    std::size_t found = 0;
    std::uint8_t status = 0;
    std::uint8_t data[2] = {0, 0};
    unsigned have = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint8_t b = bytes[i];
      if (b >= 0xF8u) {
        continue;
      }
      if ((b & 0x80u) != 0u) {
        status = core::midi_is_channel(b) ? b : std::uint8_t{0};
        have = 0;
        continue;
      }
      if (status == 0u) {
        continue;
      }
      const unsigned want = (core::midi_kind(status) == core::kMidiProgram || core::midi_kind(status) == core::kMidiChannelPressure) ? 1u : 2u;
      data[have++] = b;
      if (have == want) {
        in(port, status, data[0], want == 2u ? data[1] : std::uint8_t{0}, at);
        ++found;
        have = 0;
      }
    }
    return found;
  }

  // ---- The clock generator --------------------------------------------------------------------------------------------------------
  // A master on `p` (any port until this is called). Only a master on Config::follow_port is followed.
  void clock_port(Port p) { master_port_ = static_cast<int>(p); }
  void clock_start(std::uint32_t tempo_mbpm) {
    present(tempo_mbpm);
    ev_start_ = true;
  }
  void clock_continue(std::uint32_t tempo_mbpm, unsigned beats) {
    present(tempo_mbpm);
    ev_continue_ = true;
    ev_beats_ = beats;
  }
  void clock_stop(std::uint32_t tempo_mbpm) {
    present(tempo_mbpm);
    ev_stop_ = true;
  }
  void clock_tempo(std::uint32_t tempo_mbpm) { present(tempo_mbpm); }
  void clock_off() { master_present_ = false; }

  // ---- What was sent and asked -----------------------------------------------------------------------------------------------------
  std::vector<Sent> sent;                           // everything end_block() and send() recorded, in order
  std::vector<core::TransportRequest> requests;     // request() calls
  unsigned configs = 0;                             // set_config() calls
  unsigned count(std::uint8_t status) const {       // messages sent with this status byte (a clock: 0xF8)
    return static_cast<unsigned>(std::count_if(sent.begin(), sent.end(), [&](const Sent& s) { return s.status == status; }));
  }

  core::ClockContinuity continuity;                 // the check over every ClockFrame begin_block() has given (core/clock_continuity.h)

  std::uint64_t block_start() const { return block_start_; }
  const hal::midi::Config& config() const { return cfg_; }
  bool following() const { return following_; }

  // The tick -> frame mapping of dsp::frame_at (the tests hold the two equal): the first frame f of the block with pos + f * inc >= tick, or -1.
  static int frame_of(std::uint64_t pos, std::uint64_t inc, std::uint32_t frames, std::uint64_t tick) {
    if (inc == 0u || frames == 0u) {
      return -1;
    }
    const std::uint64_t q = tick << 32;
    const std::uint64_t last = pos + inc * (frames - 1u);
    if (q + inc <= pos || q > last) {
      return -1;
    }
    return q <= pos ? 0 : static_cast<int>((q - pos + inc - 1u) / inc);
  }
  // Q32.32 ticks per sample at `mbpm` (960 ppqn, the engine's rate): dsp::ticks_per_sample_q32 with the same arithmetic.
  static std::uint64_t inc_for(std::uint32_t mbpm) {
    constexpr std::uint64_t rate_mhz =
        (static_cast<std::uint64_t>(core::kCpuHz) * 1000u * core::kRefCyclesPerSample.den + core::kRefCyclesPerSample.num / 2u) / core::kRefCyclesPerSample.num;
    const std::uint32_t clamped = mbpm < 20'000u ? 20'000u : (mbpm > 300'000u ? 300'000u : mbpm);
    return ((static_cast<std::uint64_t>(clamped) * 960u) << 32) / (60u * rate_mhz);
  }

  // ---- hal::midi::Device: render side ---------------------------------------------------------------------------------------------------
  unsigned begin_block(std::uint32_t frames, core::MidiMessage* out_in, unsigned cap, core::ClockFrame& clock) noexcept override {
    frames_ = frames;
    unsigned n = 0;
    while (n < cap && !timed_.empty() && timed_.front().at < block_start_ + frames) {
      core::MidiMessage m = timed_.front().m;
      const std::uint64_t at = timed_.front().at;
      m.frame = static_cast<std::uint16_t>(at > block_start_ ? at - block_start_ : 0u);  // (late input lands on frame 0)
      out_in[n++] = m;
      timed_.pop_front();
    }
    last_locate_ = false;
    clock = make_clock(frames);
    continuity.feed(clock, frames, last_locate_);
    return n;
  }

  void end_block(const core::ClockFrame& t, const core::MidiMessage* out, unsigned n) noexcept override {
    for (unsigned i = 0; i < n; ++i) {
      const std::uint8_t ports = static_cast<std::uint8_t>((out[i].ports != 0u ? out[i].ports : cfg_.out_ports) & core::kMidiAllPorts);
      if (ports != 0u) {
        put(block_start_ + out[i].frame, ports, out[i].status, out[i].data1, out[i].data2);
      }
    }
    if (cfg_.out_ports != 0u && t.inc != 0u) {
      if (cfg_.transport_out) {
        if (t.stopped) {
          put(block_start_, cfg_.out_ports, core::kMidiStop, 0, 0);
        }
        if (t.playing && t.started) {
          put(block_start_, cfg_.out_ports, core::kMidiStart, 0, 0);
        } else if (t.playing && !out_playing_) {  // a run that begins elsewhere: where, then go
          const auto beats = static_cast<std::uint32_t>((t.pos >> 32) / kTicksPerBeat);
          put(block_start_, cfg_.out_ports, core::kMidiSongPosition, static_cast<std::uint8_t>(beats & 0x7Fu), static_cast<std::uint8_t>((beats >> 7) & 0x7Fu));
          put(block_start_, cfg_.out_ports, core::kMidiContinue, 0, 0);
        }
      }
      if (cfg_.clock_out && t.playing) {
        const std::uint64_t last = t.pos + t.inc * (frames_ > 0u ? frames_ - 1u : 0u);
        std::uint64_t k = (t.pos >> 32) / kTicksPerClock;
        k = k > 0u ? k - 1u : 0u;
        for (; k * kTicksPerClock <= (last >> 32); ++k) {
          const int f = frame_of(t.pos, t.inc, frames_, k * kTicksPerClock);
          if (f >= 0) {
            put(block_start_ + static_cast<std::uint32_t>(f), cfg_.out_ports, core::kMidiClock, 0, 0);
          }
        }
      }
    }
    out_playing_ = t.playing;
    block_start_ += frames_;
  }

  // ---- hal::midi::Device: main loop -----------------------------------------------------------------------------------------------------
  void set_config(const hal::midi::Config& c) noexcept override {
    cfg_ = hal::midi::sanitized(c);
    ++configs;
  }
  hal::midi::Status status() const noexcept override {
    hal::midi::Status s;
    s.following = following_;
    s.locked = following_ && master_present_;
    s.tempo_mbpm = following_ ? master_mbpm_ : 0u;
    s.rx = rx_;
    s.tx = tx_;
    s.dropped = dropped_;
    return s;
  }
  bool poll(core::MidiMessage& m) noexcept override {
    if (polled_.empty()) {
      return false;
    }
    m = polled_.front();
    polled_.pop_front();
    return true;
  }
  bool send(const core::MidiMessage& m) noexcept override {
    const std::uint8_t ports = static_cast<std::uint8_t>((m.ports != 0u ? m.ports : cfg_.out_ports) & core::kMidiAllPorts);
    if (ports == 0u) {
      return false;
    }
    put(block_start_, ports, m.status, m.data1, m.data2);
    return true;
  }
  void request(core::TransportRequest r) noexcept override {
    requests.push_back(r);
    if (r == core::TransportRequest::Play) {
      req_play_ = true;
    } else if (r == core::TransportRequest::Stop) {
      req_stop_ = true;
    }
  }

 private:
  struct Timed {
    core::MidiMessage m;
    std::uint64_t at;
  };

  void present(std::uint32_t tempo_mbpm) {
    master_present_ = true;
    master_mbpm_ = tempo_mbpm < 20'000u ? 20'000u : (tempo_mbpm > 300'000u ? 300'000u : tempo_mbpm);
  }
  void put(std::uint64_t sample, std::uint8_t ports, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    sent.push_back(Sent{sample, ports, status, d1, d2});
    ++tx_;
  }

  core::ClockFrame make_clock(std::uint32_t frames) {
    core::ClockFrame c;
    const bool on_port = master_port_ < 0 || master_port_ == static_cast<int>(cfg_.follow_port);
    const bool live = cfg_.follow && master_present_ && on_port;
    if (!following_) {
      if (!live) {  // not following: the master's transport events and the requests go nowhere
        ev_start_ = ev_continue_ = ev_stop_ = false;
        req_play_ = req_stop_ = false;
        return c;
      }
      following_ = true;
      playing_ = false;
      pos_ = 0;
      quiet_ = 0;
    }
    const std::uint64_t inc = inc_for(master_mbpm_);
    if (!cfg_.follow || !on_port) {  // let go by the user: the engine stops its own transport
      following_ = false;
      playing_ = false;
      return c;
    }
    c.follow = true;
    c.tempo_mbpm = master_mbpm_;
    c.inc = inc;
    if (!master_present_) {
      quiet_ += inc * frames;
      if (quiet_ >= (static_cast<std::uint64_t>(kStaleClocks) * kTicksPerClock) << 32) {  // 48 clocks of silence: stop, and let go
        c.stopped = playing_;
        following_ = false;
        playing_ = false;
        return c;
      }
    } else {
      quiet_ = 0;
    }
    if (cfg_.transport_in) {
      if (ev_start_) {
        playing_ = true;
        pos_ = 0;
        c.started = true;
      }
      if (ev_continue_) {
        playing_ = true;
        pos_ = (static_cast<std::uint64_t>(ev_beats_) * kTicksPerBeat) << 32;
        c.started = false;
        last_locate_ = true;
      }
      if (ev_stop_) {
        c.stopped = c.stopped || playing_;
        playing_ = false;
        pos_ = 0;
      }
    }
    ev_start_ = ev_continue_ = ev_stop_ = false;
    if (req_play_ && !playing_) {  // Play under an external clock: the run begins on the master's grid
      playing_ = true;
      pos_ = 0;
      c.started = true;
    }
    if (req_stop_ && playing_) {
      c.stopped = true;
      playing_ = false;
      pos_ = 0;
    }
    req_play_ = req_stop_ = false;
    c.playing = playing_;
    c.started = c.started && playing_;
    c.pos = playing_ ? pos_ : 0u;
    if (playing_) {
      pos_ += inc * frames;
    }
    return c;
  }

  hal::midi::Config cfg_;
  std::deque<Timed> timed_;
  std::deque<core::MidiMessage> polled_;
  std::uint64_t block_start_ = 0;
  std::uint32_t frames_ = 0;
  std::uint32_t rx_ = 0;
  std::uint32_t tx_ = 0;
  std::uint32_t dropped_ = 0;
  // The master.
  bool master_present_ = false;
  std::uint32_t master_mbpm_ = 120'000;
  int master_port_ = -1;
  bool ev_start_ = false;
  bool ev_continue_ = false;
  bool ev_stop_ = false;
  unsigned ev_beats_ = 0;
  bool req_play_ = false;
  bool req_stop_ = false;
  bool following_ = false;
  bool playing_ = false;
  std::uint64_t pos_ = 0;
  std::uint64_t quiet_ = 0;
  bool out_playing_ = false;  // the last block's transport was playing (end_block)
  bool last_locate_ = false;  // the block being made acted on a Song Position
};

}  // namespace o3sdk::platform::host
