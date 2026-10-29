// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The taps of the fixed graph (OS-005b; ADR 0013 D4: "every tap is a block copy into an SPSC ring or, for REC, a sequential write straight
// into the sample arena"; ADR 0010: the UI never sees an audio buffer). A tap copies a finished block out of the render and goes no further:
// nothing downstream waits for a reader, and a reader that falls behind loses the oldest data, never the render's time.
//
//   SampleRing<N>   N int16 samples written a block at a time by the render (the one writer), read in windows by one lower context: the
//                   scope (the master's L/R, interleaved) and the spectrum (the master's mono sum; the rFFT runs in a main-loop job).
//                   Overrun policy: the writer overwrites the oldest samples and never checks; the reader copies the newest `count`
//                   samples, then re-checks that the writer did not come round to the oldest of them (counting the block it is writing
//                   now), retries a bounded number of times and says so if it could not get a whole window.
//   StemTap         the mono post-fader int16 block of each track, plus the master pair and the two sends, into the OS-006a stem ring
//                   (usb::StemRing) for the USB stems (ADR 0012): one channel of the profile is the saturating sum (usb::sum_slots) of the
//                   sources its mask names. The stem ring carries kStemBlockFrames = 128 frames and the render makes kBlock = 64 at a time,
//                   so a stem block is two render blocks and is published after the second. A published block must stay valid until
//                   StemRing::capacity() + 2 further blocks have been written (usb/stream.h), which is what kPool = 5 blocks per channel
//                   round-robin gives.
//   RecTap          the REC bus into a region of the sample arena: a sequential write of interleaved stereo or a mono mix, from the block
//                   after arm() until the region is full (no wrap; the rest is counted). Recording UI and files are later slices (OS-005f).
//
// Contexts. The render is the only writer of every ring and pool and the only reader of the configuration it was handed; the main loop
// configures between blocks through atomics (set_enabled, set_slot) or, for RecTap, through the handshake documented there.

#include <o3sdk/platform/mk66/audio/hot.h>
#include <o3sdk/platform/mk66/audio/kernels_impl.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/usb/kernels.h>
#include <o3sdk/usb/ring.h>
#include <o3sdk/usb/stream.h>
#include <o3sdk/usb/usb_types.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::platform::mk66::audio {

// ---- SampleRing ------------------------------------------------------------------------------------------------------------
// `MaxWrite` is the largest single write in samples (the block the writer may be in the middle of when a reader looks).
template <std::uint32_t N, std::uint32_t MaxWrite>
class SampleRing {
  static_assert(N >= 8u && (N & (N - 1u)) == 0u, "the ring is a power of two number of samples");
  static_assert(MaxWrite % 2u == 0u && MaxWrite < N, "writes are whole words and smaller than the ring");

 public:
  static constexpr std::uint32_t capacity() noexcept { return N; }
  // The largest window a reader can rely on (the rest is the writer's slack).
  static constexpr std::uint32_t max_window() noexcept { return N - MaxWrite; }

  void reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < N / 2u; ++i) {
      words_[i].store(0, std::memory_order_relaxed);
    }
  }

  // Producer (the render). `n` even and at most MaxWrite, `src` 4-byte aligned. Wait-free.
  O3SDK_AUDIO_INLINE void write(const std::int16_t* src, std::uint32_t n) noexcept {
    const std::uint32_t head = head_.load(std::memory_order_relaxed);
#if !defined(__SANITIZE_THREAD__)
    std::atomic_thread_fence(std::memory_order_release);  // the previous publish of head_ is ordered before the stores below
#endif
    for (std::uint32_t i = 0; i < n; i += 2u) {
      words_[((head + i) >> 1) & (N / 2u - 1u)].store(kernel::detail::load_pair(src + i), kSlot);
    }
    head_.store(head + n, std::memory_order_release);
  }
  // Producer (the render). The sequence counter moves on by `n` samples and no sample is stored (OS-BUG-TAPWRITE: a tap whose readers want only its statistics does not pay for the
  // stores): the words keep what they held, so a position written by skip() is STALE and the consumer must not read it (AppTap: a reader starts at a position the writer wrote).
  O3SDK_AUDIO_INLINE void skip(std::uint32_t n) noexcept { head_.store(head_.load(std::memory_order_relaxed) + n, std::memory_order_release); }

  struct Window {
    std::uint32_t head;  // samples ever written when the window was taken: a new value means new data
    bool whole;          // false: the writer lapped the window on every try, `dst` holds the last attempt
  };
  // Consumer (one context). Copies the newest `count` samples (even, at most max_window()) into `dst` (4-byte aligned); before `count`
  // samples exist the front of the window is zero.
  Window read_latest(std::int16_t* dst, std::uint32_t count) const noexcept {
    constexpr unsigned kTries = 3;
    Window w{0, false};
    for (unsigned attempt = 0; attempt < kTries && !w.whole; ++attempt) {
      const std::uint32_t h1 = head_.load(std::memory_order_acquire);
      const std::uint32_t have = h1 < count ? h1 : count;
      const std::uint32_t start = h1 - have;
      const std::uint32_t lead = count - have;
      for (std::uint32_t i = 0; i < lead; ++i) {
        dst[i] = 0;
      }
      for (std::uint32_t i = 0; i < have; i += 2u) {
        kernel::detail::store_pair(dst + lead + i, words_[((start + i) >> 1) & (N / 2u - 1u)].load(kSlot));
      }
#if !defined(__SANITIZE_THREAD__)
      std::atomic_thread_fence(std::memory_order_acquire);  // the copy is ordered before the re-check
#endif
      const std::uint32_t h2 = head_.load(std::memory_order_relaxed);
      w.head = h1;
      // The writer has overwritten samples before h2 - N + MaxWrite at most (the block in progress included).
      w.whole = (h2 - start) + MaxWrite <= N;
    }
    return w;
  }
  // Consumer (one context). Copies the `count` samples (even, at most max_window()) that start at the free-running position `start` into `dst` (4-byte aligned). The range is decided by the
  // caller, so a block the writer publishes meanwhile cannot move it. False when the range is not all written yet, or the writer lapped it on every try (OS-H2a: the apps' tap stream).
  bool read_range(std::uint32_t start, std::int16_t* dst, std::uint32_t count) const noexcept {
    constexpr unsigned kTries = 3;
    for (unsigned attempt = 0; attempt < kTries; ++attempt) {
      const std::uint32_t h1 = head_.load(std::memory_order_acquire);
      if (static_cast<std::int32_t>(h1 - start) < static_cast<std::int32_t>(count)) {
        return false;
      }
      for (std::uint32_t i = 0; i < count; i += 2u) {
        kernel::detail::store_pair(dst + i, words_[((start + i) >> 1) & (N / 2u - 1u)].load(kSlot));
      }
#if !defined(__SANITIZE_THREAD__)
      std::atomic_thread_fence(std::memory_order_acquire);
#endif
      if ((head_.load(std::memory_order_relaxed) - start) + MaxWrite <= N) {
        return true;
      }
    }
    return false;
  }
  std::uint32_t head() const noexcept { return head_.load(std::memory_order_acquire); }

 private:
#if defined(__SANITIZE_THREAD__)
  static constexpr std::memory_order kSlot = std::memory_order_seq_cst;
#else
  static constexpr std::memory_order kSlot = std::memory_order_relaxed;
#endif
  std::atomic<std::uint32_t> head_{0};  // samples written, free-running; written by the producer only
  std::atomic<std::uint32_t> words_[N / 2u]{};
};

// The scope: master L/R interleaved, window of 256 frames, a ring of 512 frames. The spectrum: the mono sum, window of 512 frames (the
// OS-003d analyser's FFT size), a ring of 1,024.
inline constexpr std::uint32_t kScopeWindowFrames = 256;
inline constexpr std::uint32_t kSpectrumWindowFrames = 512;
using ScopeRing = SampleRing<1024u, hal::audio::kBlockSamples>;
using SpectrumRing = SampleRing<1024u, hal::audio::kBlock>;
static_assert(kScopeWindowFrames * 2u <= ScopeRing::max_window(), "the scope window fits the ring with the writer's slack");
static_assert(kSpectrumWindowFrames <= SpectrumRing::max_window(), "the spectrum window fits the ring with the writer's slack");

// ---- StemTap ---------------------------------------------------------------------------------------------------------------
// The sources of a stem slot, in the order usb::kSlotSources and ADR 0012 fix: T1..T12, Master L, Master R, Send A, Send B.
namespace stem_src {
inline constexpr unsigned kTracks = 12;
inline constexpr unsigned kMasterL = 12;
inline constexpr unsigned kMasterR = 13;
inline constexpr unsigned kSendA = 14;
inline constexpr unsigned kSendB = 15;
}  // namespace stem_src
static_assert(usb::kSlotSources == 16u, "the stem sources are the usb slot sources");

// The StemTap's memory: the stem ring, the pool its blocks live in, and the staging blocks of the tracks. Plain storage like MeterStore, so that the
// platform places it (SRAM_L on the MK66, tap_mem.h): the render writes it a block at a time, the USB interrupt reads it a packet at a time, and the
// strips' own working blocks stay in SRAM_U.
struct StemPool {
  static constexpr unsigned kChannels = usb::kMaxChannels;           // the widest profile (P11)
  static constexpr unsigned kPool = usb::StemRing::capacity() + 2u;  // blocks per channel: ring + the reader's + the writer's
  static constexpr unsigned kStaged = stem_src::kTracks;             // the tracks a stem slot can name (T1..T12)

  usb::StemRing ring;
  alignas(4) std::int16_t pool[kChannels][kPool][usb::kStemBlockFrames];
  alignas(4) std::int16_t stage[kStaged][hal::audio::kBlock];  // a track's post-fader block, copied here while its USB bit is set and the tap is on
  void reset() noexcept;
};

class StemTap {
 public:
  static constexpr unsigned kChannels = StemPool::kChannels;
  static constexpr unsigned kPool = StemPool::kPool;
  static constexpr unsigned kHalves = usb::kStemBlockFrames / hal::audio::kBlock;
  static_assert(usb::kStemBlockFrames % hal::audio::kBlock == 0u && kHalves >= 1u, "a stem block is a whole number of render blocks");
  static constexpr std::uint32_t kDefaultChannels = 10;                       // profile P10: T1..T8 and the master pair

  using SumFn = void (*)(const std::int16_t* const*, std::uint32_t, std::uint32_t, std::int16_t*) noexcept;

  explicit StemTap(StemPool& pool) noexcept;
  void reset() noexcept;

  // The ring the USB stack's InStream reads (usb::InStream takes a StemRing&). Its single consumer is the USB ISR.
  usb::StemRing& ring() noexcept { return pool_.ring; }
  // Where a track's post-fader block is staged for the next push (the graph copies it there; kStaged tracks).
  std::int16_t* stage(unsigned track) noexcept { return pool_.stage[track < StemPool::kStaged ? track : 0u]; }

  // ---- Main loop (atomics; a change lands at a block boundary, a profile switch may mix old and new masks for one block) ---------
  // Off by default: the tap costs nothing until a stream wants it. Enabling restarts the block phase.
  void set_enabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
  // The profile's channels: how many (at most kChannels) and, for each, the bit mask over the 16 sources (stem_src).
  void set_channels(std::uint32_t n) noexcept { channels_.store(n < kChannels ? n : kChannels, std::memory_order_relaxed); }
  void set_slot(unsigned channel, std::uint32_t mask) noexcept {
    if (channel < kChannels) {
      mask_[channel].store(mask & 0xFFFFu, std::memory_order_relaxed);
    }
  }
  std::uint32_t slot(unsigned channel) const noexcept { return channel < kChannels ? mask_[channel].load(std::memory_order_relaxed) : 0u; }
  std::uint32_t channels() const noexcept { return channels_.load(std::memory_order_relaxed); }

  // ---- Render ----------------------------------------------------------------------------------------------------------------
  // One render block: `sources` is the 16-pointer array (an entry null = silence, else a kBlock-frame mono block, 4-byte aligned, valid for the
  // call; the array itself null = skip this block). After the last half-block of a stem block it is published.
  O3SDK_AUDIO_HOT void push(const std::int16_t* const* sources) noexcept;

  // Blocks published since reset (for tests and diag): the render writes it.
  std::uint32_t published() const noexcept { return published_.load(std::memory_order_relaxed); }

 private:
  StemPool& pool_;
  SumFn sum_;
  std::atomic<bool> enabled_{false};
  std::atomic<std::uint32_t> channels_{kDefaultChannels};
  std::atomic<std::uint32_t> mask_[kChannels];
  // render only
  bool was_enabled_ = false;
  unsigned half_ = 0;
  unsigned slot_ = 0;
  std::atomic<std::uint32_t> published_{0};
};

// ---- RecTap ----------------------------------------------------------------------------------------------------------------
// The handshake. The main loop calls arm() while the tap is Idle: it writes the destination, then publishes state Armed (release), or Waiting when the take is to start on the
// timeline (below). The render, seeing Armed (acquire), writes from its next block on, and publishes Done when the region is full or `disarm()` was asked for. The main loop reads
// frames() any time, and calls release() once it has the take (Done to Idle). Each transition is made by one side only and only from a state the other side leaves alone, so the
// single word has one writer at a time (a state machine, no lock):
//
//     Idle    --arm(Free)------>  Armed     main loop       (the take starts at the render's next block)
//     Idle    --arm(Step ...)-->  Waiting   main loop
//     Waiting --on_step()------>  Armed     render          (a step the alignment accepts: the take starts AT THAT STEP'S SAMPLE, see below)
//     Waiting --disarm()------->  Done      render          (0 frames: nothing was recorded)
//     Armed   --full / disarm-->  Done      render
//     Armed   --stop_at() + a boundary, auto_steps, or the transport's stop--> Done   render   (OS-REC2, below)
//     Done    --release()------>  Idle      main loop
//     Idle    --arm_level()---->  Listening main loop       (OS-SREC-LEVEL, below: nothing is recorded yet, the render keeps the last pre-roll in a ring and scans for the level)
//     Listening --the level---->  Armed     render          (the ring is frozen from here on; the take goes on linearly behind it)
//     Listening --disarm()----->  Done      render          (0 frames: nothing was recorded)
//
// OS-011c: the take is aligned to the timeline. The sequencer says where a step begins as a Step event with the sample offset inside the block (audio/events.h); the engine passes
// it to on_step() before the graph renders that block, and the first push() of the take skips the frames before that offset, so the take's first frame IS the first sample of the step
// (the REC bus is what the render produces for the same block: the same sample clock as the voices). Alignments: Free (the next block, as before), Step (the next step), Bar (a step
// whose index is a multiple of 16: four steps a beat, four beats a bar, the sequencer's grid) and Pattern (step 0). The input a take records arrives one RX slot after it was
// played (the codec's own latency), which nothing here compensates: the take of an external source lags the timeline by the loop's round trip (see app/recorder.h).
//
// What the take records is the REC bus (what the strips route to REC, the input strip first) or, with set_source(Master), the MAIN bus after the master level (a resample of
// what is heard, before OUT MODE); the stereo pair is written interleaved or mixed to mono, or one channel of it (set_channel): a microphone or a line's left only. The settings
// are main-loop atomics read once a block; changing them while Armed lands at a block boundary.
//
// OS-REC2 (NEW COMMUNITY OS BEHAVIOUR): the END of a take can be on the timeline too, to the sample. stop_at(Step | Bar | Pattern)
// asks for the take to end at the next Step event the alignment accepts (the same rule as the start), and arm()'s `auto_steps` makes it end by itself at the auto_steps-th Step event after it
// began (N bars are 16 N steps). The Step event says where the step begins inside the block about to be rendered (`offset`), on_step() remembers it and the push of that block writes only
// the frames before it, so the take's last frame is the one before the boundary's sample: the take is exactly [start sample, boundary sample) of the REC bus's own clock, whatever the swing
// or the tempo does between (the Step events carry the swung times). A stop request that is pending when the transport stops (on_stopped: the Stopped event's offset) ends there, the timeline
// having ended; an auto_steps count that the stop interrupts is dropped and the take goes on until disarm(). Written by the main loop before the take is published (auto_steps) or as one
// byte (stop_at), read by the render; the counters are the render's alone.
//
// OS-SREC-LEVEL (E-SREC-11, NEW COMMUNITY OS BEHAVIOUR): the take can start on a LEVEL. arm_level() leaves the tap Listening: the region's first R = pre-roll + kBlock frames are a ring that
// each block is written into (the same kernels as a take), and the block is scanned for the first frame whose |sample| reaches the threshold (only the selected channel counts for Left and
// Right; no hysteresis, no hold: one sample at or above starts it). The scan is O3SDK_AUDIO_COLD (flash, not SRAM_L): it runs only while Listening, one call a block. On the trigger the render
// counts the frames to keep (the pre-roll that was heard, the trigger frame and the rest of its block: all inside the ring), publishes frames() = R and the state Armed; later blocks write
// linearly from frame R on, so the ring is never touched again and the main loop puts it in time order at its leisure: settle_ring() moves the kept frames to END at frame R, zeroes the frames
// before them and says how many leading frames of the take are no audio (the floor, even). The trigger is on the TAPPED bus (the REC bus or MAIN), not on the codec input. A Step event of the
// trigger block comes before its push (the state is still Listening) and is not counted. n of a Listening push is at most kBlock.
class RecTap {
 public:
  enum class State : std::uint32_t { Idle = 0, Armed = 1, Done = 2, Waiting = 3, Listening = 4 };
  enum class Align : std::uint8_t { Free = 0, Step, Bar, Pattern, Level };
  enum class Channel : std::uint8_t { Mix = 0, Left, Right };
  enum class Source : std::uint8_t { Bus = 0, Master };

  RecTap() noexcept { reset(); }
  void reset() noexcept;

  // Main loop. `dst` 4-byte aligned, room for `capacity_frames` frames of `channels` (1: the mono channel selected, 2: interleaved L/R) int16. False
  // if the tap is not Idle or the arguments are not usable. With Align::Free the take starts at the render's next block, otherwise at the step `align` asks for.
  // `auto_steps` (OS-REC2): the take ends by itself at the auto_steps-th Step event after its first frame (0: it does not); an aligned take began AT a Step event, which is not counted.
  bool arm(std::int16_t* dst, std::uint32_t capacity_frames, unsigned channels, Align align = Align::Free, std::uint32_t auto_steps = 0) noexcept;
  // OS-SREC-LEVEL: arm a take that starts on a level. `threshold` is the int16 magnitude (1..32767) a frame must reach, `preroll_frames` the audio before the trigger the take keeps (rounded up to
  // even). `capacity_frames` is the whole region and must exceed ring_frames(): the ring is its first frames. False as arm() does, or when the region cannot hold the ring and one more frame.
  bool arm_level(std::int16_t* dst, std::uint32_t capacity_frames, unsigned channels, std::int32_t threshold, std::uint32_t preroll_frames, std::uint32_t auto_steps = 0) noexcept;
  static constexpr std::uint32_t ring_frames(std::uint32_t preroll_frames) noexcept { return ((preroll_frames + 1u) & ~1u) + hal::audio::kBlock; }
  // Main loop, once the state is Armed or Done after a level take's trigger (else it does nothing and returns 0): puts the ring in time order ending at frame ring_frames(), zeroes the frames
  // before it and returns how many leading frames of the take are not audio (even: the Recorder's trim floor). Idempotent.
  std::uint32_t settle_ring() noexcept;
  void disarm() noexcept { stop_.store(true, std::memory_order_relaxed); }
  // OS-REC2: the take ends at the sample of the next Step event that `boundary` accepts (Step: any, Bar: a step index multiple of 16, Pattern: step 0), or where the transport stops. Free is
  // disarm(). Meaningful while Armed (a Waiting take is cancelled by disarm()); a second call replaces the boundary, disarm() ends it at the next block as before.
  void stop_at(Align boundary) noexcept {
    if (boundary == Align::Free) {
      disarm();
    } else {
      stop_align_.store(static_cast<std::uint8_t>(boundary), std::memory_order_relaxed);
    }
  }
  // Done to Idle (the take has been used).
  void release() noexcept {
    State s = state();
    if (s == State::Done) {
      state_.store(static_cast<std::uint32_t>(State::Idle), std::memory_order_release);
    }
  }
  State state() const noexcept { return static_cast<State>(state_.load(std::memory_order_acquire)); }
  std::uint32_t frames() const noexcept { return frames_.load(std::memory_order_acquire); }   // frames written so far
  // OS-LOOPER, render context after Done (loop_player.h): the take's memory, whether it ended AT a boundary of the timeline (stop_at or auto_steps, every frame before it written: not a disarm, not a full
  // arena), and the frame of the block it ended in at which the boundary's sample lies (valid only then, only in the block that ended the take).
  const std::int16_t* data() const noexcept { return dst_; }
  bool ended_on_boundary() const noexcept { return end_ != kNoEnd && !stop_.load(std::memory_order_relaxed); }  // (a disarm() leaves stop_ set: that end is no boundary's, whatever block it fell in; overrun_frames() says the rest)
  std::uint32_t end_offset() const noexcept { return end_; }
  std::uint32_t overrun_frames() const noexcept { return overrun_.load(std::memory_order_relaxed); }  // frames that did not fit

  void set_source(Source s) noexcept { source_.store(static_cast<std::uint8_t>(s), std::memory_order_relaxed); }
  Source source() const noexcept { return static_cast<Source>(source_.load(std::memory_order_relaxed)); }
  void set_channel(Channel c) noexcept { channel_.store(static_cast<std::uint8_t>(c), std::memory_order_relaxed); }
  Channel channel() const noexcept { return static_cast<Channel>(channel_.load(std::memory_order_relaxed)); }

  // Render: a step of the sequencer begins `offset` frames into the block about to be rendered, with this index in its pattern (the Step event's value).
  O3SDK_AUDIO_HOT void on_step(std::uint32_t step, std::uint32_t offset) noexcept;
  // Render: the transport stopped `offset` frames into the block about to be rendered (the Stopped event): a take with a stop_at() request pending ends there.
  O3SDK_AUDIO_HOT void on_stopped(std::uint32_t offset) noexcept;
  // Render: the two int16 planes of the bus recorded, `n` frames (4-byte aligned).
  O3SDK_AUDIO_HOT void push(const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept;

 private:
  bool arm_as(std::int16_t* dst, std::uint32_t capacity_frames, unsigned channels, Align align, std::uint32_t auto_steps, State first, std::uint32_t ring, std::int32_t threshold) noexcept;
  // Render, state Listening (OS-SREC-LEVEL): write the block into the ring, scan it, trigger. Flash (hot.h O3SDK_AUDIO_COLD): once a block, only while listening.
  O3SDK_AUDIO_COLD void listen(const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept;
  using InterleaveFn = void (*)(const std::int16_t* const*, std::uint32_t, std::uint32_t, std::uint32_t, std::int16_t*) noexcept;
  InterleaveFn interleave_;  // usb::interleave (the OS-006a kernel), called through a pointer: the render runs from SRAM_L, it from flash
  std::atomic<std::uint32_t> state_{0};
  std::atomic<bool> stop_{false};
  std::atomic<std::uint8_t> source_{0};
  std::atomic<std::uint8_t> channel_{0};
  std::atomic<std::uint8_t> stop_align_{0};  // OS-REC2: the Align stop_at() asked for (Free: none); main loop writes, the render reads
  std::atomic<std::uint32_t> frames_{0};
  std::atomic<std::uint32_t> overrun_{0};
  // Written by arm() before Waiting or Armed is published; read by the render after it saw it.
  std::int16_t* dst_ = nullptr;
  std::uint32_t capacity_ = 0;
  unsigned channels_ = 0;
  Align align_ = Align::Free;
  std::uint32_t auto_steps_ = 0;  // OS-REC2: set by arm() before the state is published
  // render only: the frames of the first block before the step's offset, which the take does not hold. The write position equals frames_.
  std::uint32_t skip_ = 0;
  // OS-REC2, render only: the Step events seen since the take began, and the frame of the block being rendered at which the take ends (kNoEnd: not in this take yet).
  static constexpr std::uint32_t kNoEnd = 0xFFFFFFFFu;
  std::uint32_t steps_ = 0;
  std::uint32_t end_ = kNoEnd;
  // OS-SREC-LEVEL. arm_level() writes thresh_ and ring_ before Listening is published; the render alone writes wpos_, heard_ and kept_ (heard_ and kept_ are final once it publishes Armed, which
  // settle_ring() acquires on the main loop); floor_ is the main loop's (kUnsettled until settle_ring() has run).
  static constexpr std::uint32_t kUnsettled = 0xFFFFFFFFu;
  std::int32_t thresh_ = 0;      // the magnitude that starts the take
  std::uint32_t ring_ = 0;       // pre-roll (even) + kBlock frames at the front of the region; 0 when the take is not a level take
  std::uint32_t wpos_ = 0;       // the ring's write position, in frames
  std::uint32_t heard_ = 0;      // frames written into the ring so far, up to ring_
  std::uint32_t kept_ = 0;       // frames the take keeps of the ring (pre-roll heard, the trigger frame, the rest of its block); 0 until the trigger
  std::uint32_t floor_ = kUnsettled;
};

}  // namespace o3sdk::platform::mk66::audio
