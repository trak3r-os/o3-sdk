// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The slot protocol between the DMA slot-boundary ISR and the render (ADR 0013 D1; hazards H9/H10/H16): which TX slot a
// render fills, how an underrun is detected, and what the policy does about it. Pure state machine, no hardware: the
// platform tells it which slot the DMA is playing, the tests drive it with a model.
//
//   P  physical slots of the DMA ring (a power of two; the eDMA wraps an address with a modulo)
//   N  render-ahead depth in blocks, 2 <= N <= P (hal::audio::kTxSlots)
//
// Timeline. A "boundary" is the instant the DMA finishes one slot and starts the next; boundary s (s = 1, 2, ...) is the
// s-th since start, the DMA then plays slot `playing(s) = (s + base) mod P`. The ISR numbers the boundary and pends a
// render for it. That render fills the slot the DMA just finished, `(playing(s) - 1) mod P`, which plays N - 1 boundaries
// later, at boundary s + N - 1. So with N = 2 the block rendered for boundary s plays at s + 1 and the deadline is one
// block; with N = 3 it plays at s + 2 (one block more latency, twice the deadline).
//
// Underrun. At boundary s the slot that starts to play must hold the block rendered for boundary s - (N - 1), i.e. that
// slot's `slot_seq_` must be exactly s - (N - 1) (from s = N on; the first N - 1 boundaries play the zeroed ring). If it is
// not, the render did not get its block in on time: the DMA replays the stale slot, the ISR counts it and never waits.
// Three consecutive underruns mute (ADR 0013 D1, ADR 0011): from then on every underrun zeroes the slot that starts to
// play, so a render that has stopped producing yields silence instead of a looping 1.45 ms buzz, and the render is told to
// write silence until `kUnmuteAfter` boundaries in a row were on time.
//
// Late blocks. A render that finishes after the boundary its slot plays at has passed must not write into a slot that is
// (about to be) playing: `on_time()` is checked immediately before the copy, which takes under a microsecond against a
// playhead that moves one frame per 22 us; a late block is dropped (counted), the slot keeps its stale content.
//
// Contexts and single writers (every shared word has exactly one writer; nothing needs a lock):
//   ISR (kAudioDma)  writes seq_, base_, stamp_, muted_, consecutive_, good_       reads slot_seq_
//   render           writes rendered_, slot_seq_                                    reads seq_, base_, stamp_, muted_
//   Release stores publish; the matching loads acquire. seq_ is stored last in on_boundary, so a render that sees a
//   sequence number sees that boundary's stamp and base (a boundary that lands during begin_render() can make the stamp one
//   boundary newer than the ticket's seq, which only skews one latency figure).

#include <o3sdk/platform/mk66/audio/hot.h>
#include <o3sdk/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::platform::mk66::audio {

template <std::uint32_t P, std::uint32_t N>
class SlotRing {
  static_assert(hal::audio::is_pow2(P), "the DMA ring is a power-of-two number of slots");
  static_assert(N >= 2u && N <= P, "render-ahead depth: 2..P blocks");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "the slot protocol needs lock-free 32-bit atomics");

 public:
  static constexpr std::uint32_t kMuteAfter = 3;       // consecutive underruns that mute
  static constexpr std::uint32_t kUnmuteAfter = 32;    // on-time boundaries (about 46 ms) before the mute lifts (was 256: under a sustained overload the mute never lifted, FQA-7)
  static constexpr std::uint32_t kMask = P - 1u;

  // What one boundary decided, for the ISR to act on and count.
  struct Boundary {
    std::uint32_t seq;
    std::uint32_t slot;       // the slot that starts to play
    bool underrun;
    bool slipped;             // the hardware slot was not the one the count predicted (a boundary was missed): resynchronised
    bool zero_slot;           // muted and stale: the caller zeroes `slot` (the ISR's one data access)
    bool mute_started;
  };

  // What a render needs to know about the boundary it serves.
  struct Ticket {
    std::uint32_t seq;
    std::uint32_t write_slot;
    std::uint32_t stamp;      // cycle counter at the boundary
  };

  // Back to "no boundary yet", ring all zero. Only while neither the ISR nor the render can run (before start, or between tests).
  void reset() noexcept {
    seq_.store(0, std::memory_order_relaxed);
    base_.store(0, std::memory_order_relaxed);
    stamp_.store(0, std::memory_order_relaxed);
    muted_.store(false, std::memory_order_relaxed);
    consecutive_ = 0;
    good_ = 0;
    for (std::uint32_t i = 0; i < P; ++i) {
      slot_seq_[i].store(0, std::memory_order_relaxed);
    }
    rendered_ = 0;
  }

  // ---- ISR context (priority kAudioDma) ------------------------------------------------------------------------------
  // `hw_slot` is the slot the DMA is playing now (0..P-1), read by the platform from the DMA's source address.
  O3SDK_AUDIO_INLINE Boundary on_boundary(std::uint32_t hw_slot, std::uint32_t stamp) noexcept {
    const std::uint32_t s = seq_.load(std::memory_order_relaxed) + 1u;
    std::uint32_t base = base_.load(std::memory_order_relaxed);
    Boundary b{s, hw_slot & kMask, false, false, false, false};
    if (b.slot != ((s + base) & kMask)) {
      b.slipped = true;
      base = (b.slot - s) & kMask;
      base_.store(base, std::memory_order_relaxed);
    }
    if (s >= N) {
      b.underrun = slot_seq_[b.slot].load(std::memory_order_acquire) != s - (N - 1u);
    }
    const bool was_muted = muted_.load(std::memory_order_relaxed);
    if (b.underrun) {
      good_ = 0;
      if (consecutive_ != 0xFFFFFFFFu) {
        ++consecutive_;
      }
      if (consecutive_ >= kMuteAfter && !was_muted) {
        muted_.store(true, std::memory_order_relaxed);
        b.mute_started = true;
      }
      b.zero_slot = was_muted || b.mute_started;
    } else {
      consecutive_ = 0;
      if (was_muted && ++good_ >= kUnmuteAfter) {
        muted_.store(false, std::memory_order_relaxed);
        good_ = 0;
      }
    }
    stamp_.store(stamp, std::memory_order_relaxed);
    seq_.store(s, std::memory_order_release);
    return b;
  }

  // ---- Render context (priority kRender) -----------------------------------------------------------------------------
  // The latest boundary that has not been rendered yet, or false. A render that was held off across several boundaries
  // serves the newest one (the older blocks are underruns the ISR has already counted).
  O3SDK_AUDIO_INLINE bool begin_render(Ticket& t) noexcept {
    const std::uint32_t s = seq_.load(std::memory_order_acquire);
    if (s == 0u || s == rendered_) {
      return false;
    }
    t.seq = s;
    t.stamp = stamp_.load(std::memory_order_relaxed);
    t.write_slot = (s + base_.load(std::memory_order_relaxed) + N - 1u) & kMask;
    rendered_ = s;
    return true;
  }
  // True while the slot of `t` has not started to play: boundary t.seq + N - 1 has not happened yet.
  O3SDK_AUDIO_INLINE bool on_time(const Ticket& t) const noexcept {
    return seq_.load(std::memory_order_acquire) - t.seq < N - 1u;
  }
  // The block of `t` is in its slot: the ISR will find it there.
  O3SDK_AUDIO_INLINE void published(const Ticket& t) noexcept { slot_seq_[t.write_slot].store(t.seq, std::memory_order_release); }

  // ---- Any context (diagnostics; single words) -----------------------------------------------------------------------
  std::uint32_t seq() const noexcept { return seq_.load(std::memory_order_acquire); }
  std::uint32_t base() const noexcept { return base_.load(std::memory_order_relaxed); }
  bool muted() const noexcept { return muted_.load(std::memory_order_relaxed); }
  std::uint32_t slot_seq(std::uint32_t slot) const noexcept { return slot_seq_[slot & kMask].load(std::memory_order_relaxed); }

  static constexpr std::uint32_t slot_for(std::uint32_t seq, std::uint32_t base) noexcept {
    return (seq + base + N - 1u) & kMask;
  }

 private:
  std::atomic<std::uint32_t> seq_{0};          // ISR: boundaries so far
  std::atomic<std::uint32_t> base_{0};         // ISR: slot playing at "boundary 0" (the ring starts on slot 0)
  std::atomic<std::uint32_t> stamp_{0};        // ISR: cycle counter at the last boundary
  std::atomic<bool> muted_{false};             // ISR: the underrun mute
  std::uint32_t consecutive_ = 0;              // ISR only
  std::uint32_t good_ = 0;                     // ISR only
  std::atomic<std::uint32_t> slot_seq_[P] = {};  // render: the boundary a slot's block was rendered for (0 = none)
  std::uint32_t rendered_ = 0;                 // render only: the last boundary served
};

}  // namespace o3sdk::platform::mk66::audio
