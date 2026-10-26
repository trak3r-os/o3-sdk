// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The transport: tempo, play state and the musical position, as the render context sees it at the start of a block.
//
// Position is counted in ticks of kPpqn per quarter note, as unsigned Q32.32 fixed point (whole ticks in the high word), from
// the moment Play started. One sample advances it by `inc`, computed in integers from the tempo and the engine's rate
// (core/audio_clock.h), so a long run does not drift: the error of `inc` is below one part in 10^8.
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
  std::uint32_t tempo_mbpm = 120'000;  // milli-BPM
  std::uint64_t pos = 0;          // Q32.32 ticks at frame 0 of this block (0 while stopped)
  std::uint64_t inc = 0;          // Q32.32 ticks per sample at this tempo
  std::uint32_t frames = 0;       // frames in this block
  bool following = false;         // the transport is a MIDI master's (Engine::follow): tempo, inc and pos are its; a hook changes nothing

  float tempo_bpm() const noexcept { return static_cast<float>(tempo_mbpm) * 0.001f; }
  // Q32.32 position at the block's last frame. A frame reaches the positions after the previous frame's, up to its own: the block
  // owns (pos - inc, last()], and the next block starts at end().
  std::uint64_t last() const noexcept { return pos + inc * (frames != 0u ? frames - 1u : 0u); }
  std::uint64_t end() const noexcept { return pos + inc * frames; }
};

// Q32.32 ticks per sample at `tempo_mbpm` (clamped to the legal range), at the engine's rate.
std::uint64_t ticks_per_sample_q32(std::uint32_t tempo_mbpm) noexcept;

// Samples in one sixteenth at `tempo_mbpm`, as a float (for a unit that times something musically: a delay synced to the tempo).
float samples_per_sixteenth(std::uint32_t tempo_mbpm) noexcept;

// The frame of this block at which the transport reaches whole tick `tick`: the first frame f with pos + f * inc >= tick. -1 when
// another block reaches it (it is not in (pos - inc, last()]), or while stopped. Every tick is reached by exactly one block.
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
