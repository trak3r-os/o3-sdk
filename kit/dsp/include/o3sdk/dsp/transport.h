// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The transport: tempo, play state and the musical position, as the render context sees it at the start of a block.
//
// Position is counted in ticks of kPpqn per quarter note, as unsigned Q32.32 fixed point (whole ticks in the high word), from
// the moment Play started. One sample advances it by `inc`, computed in integers from the tempo and the engine's rate
// (core/audio_clock.h), so a long run does not drift: the error of `inc` is below one part in 10^8.
//
// Exact landing. 32 fractional bits are not enough to put a whole tick on the sample the exact arithmetic gives: a floored `inc` is
// short by up to 2^-32 ticks a sample, so a grid that is whole samples (125 BPM: 5,292 a sixteenth) falls one sample late at every
// step after position 0, and a rounded-up one falls a sample early at some tempos within a bar. The engine's own transport therefore
// carries 32 more fractional bits, `pos_lo` and `inc_lo` (Q32.64 in all), with `inc_lo` rounded UP: its position is never below the
// exact one and above it by less than n * 2^-64 ticks after n samples, so a step lands on the sample the exact rational gives for
// 7 * 10^9 samples (44 hours) from Play. A hook reads it through frame_at() and for_each_step(); a master's transport (follow)
// has both words 0 and keeps its own precision. Code that walks a Transport itself uses advance(), not `pos += inc * frames`.
//
// One owner a tick, whatever the tempo does. A block owns the whole ticks the position reaches in it: after the previous block's last
// frame and up to its own. The engine hands the previous block's real last position in the Transport (`prev_gap`: how far it is behind `pos`), so the
// blocks partition the ticks exactly even when `inc` changes between them (a tempo change, a master's slew): no tick is owned by two
// blocks (a doubled step) or by none (a lost one). Without it (`has_prev()` false: the first block of a run, after a locate, a hand-built
// Transport) the block owns the ticks from one increment before its position, which is right as long as the increment does not change.
//
// kPpqn = 960: a sixteenth is 240 ticks, a triplet sixteenth 160, a MIDI clock pulse (24 a quarter) 40; swing has 240 steps
// across a sixteenth.
//
// A sequencer (a clocked hook, engine.h) asks where a tick falls inside the current block with frame_at(), or walks its grid
// with for_each_step(). Both are pure functions of the Transport. They do not care whose transport it is: with a MIDI master followed
// (Engine::follow, `following`) the position and the increment are the master's, and a hook's steps land on the master's grid.

#include <cstdint>

namespace o3sdk::dsp {

inline constexpr std::uint32_t kPpqn = 960;
inline constexpr std::uint32_t kTicksPerSixteenth = kPpqn / 4u;
inline constexpr std::uint32_t kTicksPerMidiClock = kPpqn / 24u;

inline constexpr std::uint32_t kMinTempoMilliBpm = 20'000;   // 20 BPM
inline constexpr std::uint32_t kMaxTempoMilliBpm = 300'000;  // 300 BPM

struct Transport {
  bool playing = false;
  bool started = false;           // this is the first block since Play: position 0 is at frame 0
  bool stopped = false;           // this is the first block since Stop: a sequencer ends the notes it started
  bool following = false;         // the transport is a MIDI master's (Engine::follow): tempo, inc and pos are its; a hook changes nothing
  std::uint32_t tempo_mbpm = 120'000;  // milli-BPM
  std::uint64_t pos = 0;          // Q32.32 ticks at frame 0 of this block (0 while stopped)
  std::uint64_t inc = 0;          // Q32.32 ticks per sample at this tempo
  std::uint32_t pos_lo = 0;       // 32 more fractional bits of pos and inc (Q32.64 in all): 0 with a master's transport and in a Transport built by hand
  std::uint32_t inc_lo = 0;
  std::uint32_t frames = 0;       // frames in this block
  std::uint32_t prev_gap = 0;     // 0: no previous block in this run. Else the previous block's last frame, continuously: pos - prev_gap, Q32.32 rounded down (below 2^32: an increment and a little);
                                  // this block owns the ticks after it

  float tempo_bpm() const noexcept { return static_cast<float>(tempo_mbpm) * 0.001f; }
  bool has_prev() const noexcept { return prev_gap != 0u; }
  std::uint64_t prev_last() const noexcept { return pos - prev_gap; }
  // Q32.32 position (rounded down) at frame `f` of this block. A frame reaches the positions after the previous frame's, up to its
  // own: the block owns (at(-1), last()], and the next block starts at end().
  std::uint64_t at(std::uint32_t f) const noexcept {
    return pos + inc * f + ((static_cast<std::uint64_t>(pos_lo) + static_cast<std::uint64_t>(inc_lo) * f) >> 32);
  }
  std::uint64_t last() const noexcept { return at(frames != 0u ? frames - 1u : 0u); }
  std::uint64_t end() const noexcept { return at(frames); }
  // The transport at the start of the next block (position only; the caller owns the rest).
  void advance(std::uint32_t n) noexcept {
    const std::uint64_t lo = static_cast<std::uint64_t>(pos_lo) + static_cast<std::uint64_t>(inc_lo) * n;
    pos += inc * n + (lo >> 32);
    pos_lo = static_cast<std::uint32_t>(lo);
  }
};

// Q32.32 ticks per sample at `tempo_mbpm` (clamped to the legal range), at the engine's rate, rounded down.
std::uint64_t ticks_per_sample_q32(std::uint32_t tempo_mbpm) noexcept;
// The next 32 fractional bits of the same rate, rounded UP: with ticks_per_sample_q32() the Q32.64 increment that is never below the exact
// one (Transport::inc and inc_lo). Never carries into the high word.
std::uint32_t ticks_per_sample_lo(std::uint32_t tempo_mbpm) noexcept;

// Samples in one sixteenth at `tempo_mbpm`, as a float (for a unit that times something musically: a delay synced to the tempo).
float samples_per_sixteenth(std::uint32_t tempo_mbpm) noexcept;

// The frame of this block at which the transport reaches whole tick `tick`: the first frame f with at(f) >= tick (pos + f * inc, with
// pos_lo and inc_lo). -1 when another block owns it, or while stopped. The block owns (prev_last(), last()] when has_prev(), else
// (pos - inc, last()] (the previous block's last frame, if the increment did not change). Every tick is owned by exactly one block.
int frame_at(const Transport& t, std::uint64_t tick) noexcept;

// Calls fn(frame, step) for every step of a grid of `period` ticks that starts inside this block, in order. Step k starts at tick
// k * period + offset, delayed by `swing` more ticks when k is odd. Typical: period kTicksPerSixteenth, offset 0 (or half a step
// for the note-offs of a 50 % gate), swing 0..119 (50 % to about 75 %). offset + swing < period. Nothing while stopped.
template <typename Fn>
void for_each_step(const Transport& t, std::uint32_t period, std::uint32_t offset, std::uint32_t swing, Fn&& fn) noexcept {
  if (!t.playing || period == 0u || t.frames == 0u) {
    return;
  }
  const std::uint64_t first_tick = t.pos >> 32;
  const std::uint64_t last_tick = t.last() >> 32;
  // The earliest step that can start in the block: one before the grid point at or before first_tick (a step is late by up to
  // offset + swing, and the block owns a fraction of a tick before pos); frame_at() decides.
  std::uint64_t k = first_tick / period;
  k = k > 0u ? k - 1u : 0u;
  for (; k * period <= last_tick; ++k) {
    const std::uint64_t tick = k * period + offset + ((k & 1u) != 0u ? swing : 0u);
    const int frame = frame_at(t, tick);
    if (frame >= 0) {
      fn(static_cast<std::uint32_t>(frame), k);
    }
  }
}

}  // namespace o3sdk::dsp
