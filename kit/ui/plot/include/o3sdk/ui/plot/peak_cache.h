// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The peak-cache mipmap (ADR 0010 amendment 2, docs/specs/OS-003-ui-toolkit.md 003d): a min/max pyramid over int16 mono
// or stereo frames, so that a waveform of any length can be drawn at any zoom from a few hundred reads.
//
//   level 1  one (min, max) entry per 64 frames    level 3  one per 1024     level 5  one per 16384   ...
//   level 2  one per 256                           level 4  one per 4096     (each level is 4 entries of the one below)
//
// Levels are added until one entry covers the whole sound (at most kMaxLevels: 64 * 4^9 = 16.7 M frames, the SDRAM's size).
//
// * Cost in memory: 4 bytes an entry, per channel, so 4 * (1/64 + 1/256 + 1/1024 + ...) = 0.0833 bytes per sample (a
//   twelfth of a byte; 1.67 % of the int16 it summarises): 87 KB per channel for a 2^20-frame (24 s) sound. The caller
//   provides the storage, fixed in size (PeakStorage; SDRAM on the device); nothing is allocated.
// * Cost in time: building is one pass over the samples (cooperatively, in slices: build_step()). After an edit of `n`
//   frames update() redoes n / 64 + 2 entries of level 1 and a few of each level above: O(n + levels). A query for `c`
//   columns at any zoom and offset is O(c): per column at most 2 x 63 raw frames and 2 x 3 entries on each level.
// * The sound is not copied: the cache keeps a pointer to the samples, which must stay valid and unchanged (but for
//   what update() is told about) and word aligned (declared by alignas at the owner, checked by init()).
//
// Main loop only; no heap, no I/O.

#include <o3sdk/ui/plot/peak_kernel.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::ui::plot {

struct PeakPair {
  std::int16_t min;
  std::int16_t max;
  constexpr bool empty() const noexcept { return min > max; }
};
// "No data here": a column past the end of the sound.
inline constexpr PeakPair kNoPeak = {32767, -32768};

class PeakCache {
 public:
  static constexpr unsigned kBaseShift = 6;   // level 1: 64 frames an entry
  static constexpr unsigned kFanShift = 2;    // each level above: 4 entries of the one below
  static constexpr unsigned kMaxLevels = 10;
  static constexpr unsigned kMaxChannels = 2;
  static constexpr Frame kMaxFrames = Frame{1} << 24;

  // log2 of the frames an entry of `level` (1..kMaxLevels) covers.
  static constexpr unsigned level_shift(unsigned level) noexcept { return kBaseShift + kFanShift * (level - 1u); }
  // Entries of `level` for a sound of `frames`.
  static constexpr Frame entries_at(Frame frames, unsigned level) noexcept {
    return (frames + ((Frame{1} << level_shift(level)) - 1u)) >> level_shift(level);
  }
  // Levels built for `frames`: up to the first with a single entry; 0 for an empty sound.
  static constexpr unsigned levels_for(Frame frames) noexcept {
    unsigned l = 0;
    if (frames == 0) {
      return 0;
    }
    do {
      ++l;
    } while (l < kMaxLevels && entries_at(frames, l) > 1u);
    return l;
  }
  // int16 elements of storage needed: per channel and level, the minima then the maxima, each padded to an even count so
  // that every array starts on a word.
  static constexpr std::size_t elements_required(Frame frames, unsigned channels) noexcept {
    std::size_t n = 0;
    for (unsigned l = 1; l <= levels_for(frames); ++l) {
      n += 2u * ((static_cast<std::size_t>(entries_at(frames, l)) + 1u) & ~std::size_t{1});
    }
    return n * channels;
  }
  static constexpr std::size_t bytes_required(Frame frames, unsigned channels) noexcept {
    return elements_required(frames, channels) * sizeof(std::int16_t);
  }

  // Binds the cache to `storage` (`storage_elems` int16 elements, word aligned) and to the sound `samples` (`frames`
  // frames of `channels` = 1 or 2, word aligned). false, and the cache stays unusable, if the storage is too small or not
  // aligned, the samples are not aligned, `frames` is above kMaxFrames or `channels` is not 1 or 2. Does not read the
  // samples: call build() or build_begin() / build_step().
  bool init(std::int16_t* storage, std::size_t storage_elems, const std::int16_t* samples, Frame frames,
            unsigned channels) noexcept;
  bool valid() const noexcept { return valid_; }
  Frame frames() const noexcept { return frames_; }
  unsigned channels() const noexcept { return channels_; }
  unsigned levels() const noexcept { return levels_; }
  const std::int16_t* samples() const noexcept { return samples_; }
  std::size_t bytes_used() const noexcept { return elements_required(frames_, channels_) * sizeof(std::int16_t); }

  // Builds every level now.
  void build() noexcept;
  // Cooperative build: begin, then call build_step() from the main loop until it returns true. `budget` is a number of
  // reads (a frame or an entry); a step stops at the first entry boundary after it, so one step costs at most about
  // `budget` + 64 reads. **The queries answer kNoPeak until it has returned true** (the storage holds whatever it held before: on
  // the device, SDRAM nobody wrote), and the wave bitmap and the wave view refuse a cache that is not built().
  void build_begin() noexcept;
  bool build_step(std::size_t budget) noexcept;
  bool built() const noexcept { return built_; }
  // A counter that moves whenever the entries may have changed under a reader: init(), build_begin() and the step that
  // finishes a build. A picture made from the cache records it (WaveKey), so a rebuilt cache never leaves a stale picture
  // that still looks current.
  std::uint32_t epoch() const noexcept { return epoch_; }

  // Frames [first, first + count) of the samples changed: redoes the entries they touch, O(count + levels). Before the cache is
  // built there is nothing to redo: the edit restarts the build (build_begin()), which will read the new samples.
  void update(Frame first, Frame count) noexcept;

  // Min and max of channel `channel` over the frames [begin, end) (clamped to the sound); kNoPeak if there are none, or the
  // cache is not built().
  PeakPair range(unsigned channel, Frame begin, Frame end) const noexcept;

  // Per-column peaks for a view that shows `span` frames from frame `first` across `count` columns: column c covers
  // the frames [first + c * span / count, first + (c + 1) * span / count) (column_start() is that boundary). O(count),
  // whatever the zoom (kNoPeak in every column while the cache is not built()). A column whose range is empty because the view
  // is zoomed in past one frame a column holds the frame it falls in (zero-order hold: both min and max are that sample); a
  // column past the end of the sound is kNoPeak. `span` and `count` are at least 1.
  void columns(unsigned channel, Frame first, Frame span, unsigned count, PeakPair* out) const noexcept;

  // The first frame of column `c` of that view, and the column a frame falls in (the inverse; frames before the view
  // give 0, after it `count`). 64-bit arithmetic: markers are few.
  static Frame column_start(Frame first, Frame span, unsigned count, unsigned c) noexcept;
  static unsigned column_of(Frame first, Frame span, unsigned count, Frame frame) noexcept;

 private:
  std::int16_t* mins(unsigned ch, unsigned level) const noexcept { return storage_ + off_[ch][level]; }
  std::int16_t* maxs(unsigned ch, unsigned level) const noexcept {
    return storage_ + off_[ch][level] + pad_[level];
  }
  void make_level1(unsigned ch, Frame entry) noexcept;
  void make_upper(unsigned ch, unsigned level, Frame entry) noexcept;
  void scan_tier(unsigned ch, unsigned tier, Frame x, Frame y, int& mn, int& mx) const noexcept;

  bool valid_ = false;
  std::int16_t* storage_ = nullptr;
  const std::int16_t* samples_ = nullptr;
  Frame frames_ = 0;
  unsigned channels_ = 0;
  unsigned levels_ = 0;
  std::uint32_t off_[kMaxChannels][kMaxLevels + 1] = {};  // element offset of each level's minima, per channel
  std::uint32_t pad_[kMaxLevels + 1] = {};                // entries of a level rounded up to even: offset of its maxima
  // build_step() state
  bool built_ = false;
  std::uint32_t epoch_ = 0;
  unsigned step_level_ = 1;
  Frame step_entry_ = 0;
};

// Fixed storage for a cache, aligned by type (ADR 0011): `PeakStorage<PeakCache::elements_required(kFrames, 2)> s;`.
template <std::size_t Elements>
struct alignas(8) PeakStorage {
  static_assert(Elements > 0 && Elements % 4 == 0, "whole 8-byte units: no padding hides in the storage");
  std::int16_t data[Elements];
};

}  // namespace o3sdk::ui::plot
