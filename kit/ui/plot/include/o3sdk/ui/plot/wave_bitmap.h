// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The wave bitmap (ADR 0010 amendment 2, mode (b) of the wave view): a waveform rendered by the MCU into an L1 or L4
// column bitmap and uploaded to RAM_G, so that it costs about ten display-list words whatever its width. The picture is
// made from the per-column peaks of the cache (PeakCache::columns()) and is redone only when the view changes (a zoom, a
// scroll, an edit), never per frame.
//
// * L1: column c lights the rows from the one holding its maximum to the one holding its minimum, every pixel full. This is
//   exactly the picture the vector mode draws (wave_view.h), pixel for pixel.
// * L4: the same, with the first and last pixel of a column carrying the fraction of it the peak covers (4 bits, at least
//   6 of 15 so that a peak that grazes a row is seen), which is what a waveform looks like when it is not
//   stair-stepped. The interior of a column is identical to L1's.
//
// Rendering is O(columns + rows), not O(pixels): every column lights a run of rows, so the picture is built a row at a time
// from the previous row and the few columns whose run begins or ends there (bucketed by row in O(columns + rows) first).
// It is done in chunks of one upload (kChunkBytes, 4 KB, hal::kMaxUploadBytes): service() renders one chunk into an
// aligned staging buffer and uploads it, once a pass of the job runner, and the lane keeps drawing its old picture from
// its other RAM_G slot until the new one is complete, then swaps (ram_g_map.h): an upload never lands on a bitmap that is
// on screen. The work area is shared by both lanes (stereo), about 18 KB; jobs run one at a time.
//
// Main loop only; no heap.

#include <o3sdk/hal/display.h>
#include <o3sdk/ui/plot/peak_cache.h>
#include <o3sdk/ui/plot/ram_g_map.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::ui::plot {

enum class WaveDepth : std::uint8_t { L1 = 1, L4 = 4 };

// What a wave bitmap shows. Two equal keys are the same picture; the owner bumps `generation` after editing the samples
// (and calling PeakCache::update()), and `epoch` is the cache's own (PeakCache::epoch()), which moves when it is rebuilt: a
// picture made from a cache that has been rebuilt since is never taken for current. A key is valid only for a cache that is
// built(): while it is being built its entries are not there yet, and nothing is rendered from it.
struct WaveKey {
  const PeakCache* cache = nullptr;
  std::uint32_t generation = 0;
  std::uint32_t epoch = 0;
  Frame first = 0;
  Frame span = 1;
  std::uint16_t width = 0;   // columns, 1..kWaveMaxWidth
  std::uint16_t height = 0;  // rows, 1..kWaveMaxHeight
  std::uint8_t channel = 0;
  WaveDepth depth = WaveDepth::L1;

  bool operator==(const WaveKey& o) const noexcept {
    return cache == o.cache && generation == o.generation && epoch == o.epoch && first == o.first && span == o.span && width == o.width &&
           height == o.height && channel == o.channel && depth == o.depth;
  }
  bool operator!=(const WaveKey& o) const noexcept { return !(*this == o); }
  bool valid() const noexcept {
    return cache != nullptr && cache->valid() && cache->built() && span >= 1 && width >= 1 && width <= kWaveMaxWidth && height >= 1 &&
           height <= kWaveMaxHeight && channel < cache->channels();
  }
};

// Columns [x0, x1] where the sound reaches digital full scale (a sample at +32767 or -32768).
struct ClipRun {
  std::uint16_t x0;
  std::uint16_t x1;
};
inline constexpr unsigned kMaxClipRuns = 8;  // more runs than this are merged: the last run extends to the last clip

// The row a sample value falls in on a lane `height` rows high, and the fraction of it as 1/16 rows (Q4): 0 at +32767, the
// last row at -32768. The zero line is wave_zero_row(). Shared by both wave modes so that they agree.
constexpr std::int32_t wave_y_q4(int sample, unsigned height) noexcept {
  return static_cast<std::int32_t>(((32767 - sample) * static_cast<int>(height)) >> 12);
}
constexpr unsigned wave_row(int sample, unsigned height) noexcept {
  return static_cast<unsigned>(wave_y_q4(sample, height) >> 4);
}
constexpr unsigned wave_zero_row(unsigned height) noexcept { return wave_row(0, height); }

class WaveBitmap {
 public:
  static constexpr std::size_t kChunkBytes = hal::kMaxUploadBytes;

  WaveBitmap() noexcept { reset(); }

  // Forgets every picture and job (the RAM_G slots keep whatever bytes they hold; nothing draws from them).
  void reset() noexcept;

  // Wants `lane` (0 or 1) to show `key`. Nothing happens if the lane already shows it or is already rendering it; otherwise
  // a job for it is queued (replacing any other job of the lane) and the lane goes on showing its old picture until the new
  // one is complete. An invalid key (or lane) cancels the lane's request.
  void request(unsigned lane, const WaveKey& key) noexcept;

  // One pass of the job runner: renders at most one chunk of the queued job and uploads it. Returns true while there is more
  // to do (the next pass continues; so does a display that answered Busy), false when idle. A display that refuses a chunk
  // for another reason drops the job and counts it in failed().
  bool service(hal::Display& display) noexcept;
  // service() until idle (tests, the gallery). false if a job failed.
  bool service_all(hal::Display& display) noexcept;

  bool busy() const noexcept { return active_ >= 0 || pending(0) || pending(1); }
  unsigned failed() const noexcept { return failed_; }
  unsigned completed() const noexcept { return completed_; }  // pictures finished since reset()

  // The lane's complete picture, if it has one.
  bool ready(unsigned lane) const noexcept { return lane < kWaveLanes && lanes_[lane].shown_valid; }
  bool shows(unsigned lane, const WaveKey& key) const noexcept {
    return ready(lane) && lanes_[lane].shown == key;
  }
  const WaveKey& shown_key(unsigned lane) const noexcept { return lanes_[lane < kWaveLanes ? lane : 0].shown; }
  std::uint32_t shown_addr(unsigned lane) const noexcept {
    return wave_slot_addr(lane < kWaveLanes ? lane : 0, lanes_[lane < kWaveLanes ? lane : 0].shown_slot);
  }
  // Bytes a row of the shown picture takes (the BITMAP_LAYOUT stride).
  unsigned shown_stride(unsigned lane) const noexcept;
  // The clipped column runs of the shown picture.
  unsigned shown_clip_count(unsigned lane) const noexcept { return lane < kWaveLanes ? lanes_[lane].clip_count : 0; }
  const ClipRun* shown_clips(unsigned lane) const noexcept { return lanes_[lane < kWaveLanes ? lane : 0].clips; }

  static constexpr unsigned stride_of(unsigned width, WaveDepth depth) noexcept {
    return depth == WaveDepth::L1 ? (width + 7u) / 8u : (width + 1u) / 2u;
  }

  // The same picture rendered a pixel at a time, straight from the peaks, into `out` (stride_of(width, depth) * height
  // bytes): the reference the tests hold the fast row renderer to. Not used by the device code.
  static void render_reference(const PeakPair* peaks, unsigned width, unsigned height, WaveDepth depth,
                               std::uint8_t* out) noexcept;
  // The clip runs of a set of column peaks (at most kMaxClipRuns, merged beyond that).
  static unsigned find_clips(const PeakPair* peaks, unsigned width, ClipRun* out) noexcept;

 private:
  static constexpr std::uint16_t kNil = 0xFFFFu;
  static constexpr unsigned kLists = 4;

  struct Lane {
    WaveKey shown;
    bool shown_valid = false;
    unsigned shown_slot = 0;
    WaveKey want;
    bool want_pending = false;
    ClipRun clips[kMaxClipRuns] = {};
    unsigned clip_count = 0;
  };

  bool pending(unsigned lane) const noexcept { return lanes_[lane].want_pending; }
  bool start(unsigned lane) noexcept;
  void build_lists(unsigned width, unsigned height, WaveDepth depth) noexcept;
  void gen_row(unsigned y) noexcept;
  std::size_t fill(std::uint8_t* dst, std::size_t max) noexcept;
  void finish() noexcept;

  Lane lanes_[kWaveLanes];
  int active_ = -1;         // the lane being rendered, or -1
  unsigned failed_ = 0;
  unsigned completed_ = 0;

  // The job in progress.
  unsigned width_ = 0, height_ = 0, stride_ = 0;
  WaveDepth depth_ = WaveDepth::L1;
  unsigned slot_ = 0;               // the slot being filled
  std::size_t total_ = 0;           // bytes in the picture
  std::size_t gen_pos_ = 0;         // bytes rendered into chunks so far
  int cur_row_ = -1;                // the row `out_` holds
  bool chunk_ready_ = false;        // a rendered chunk waits for the display to take it
  std::uint32_t chunk_addr_ = 0;
  std::size_t chunk_n_ = 0;
  ClipRun job_clips_[kMaxClipRuns] = {};
  unsigned job_clip_count_ = 0;

  // The work area, shared by both lanes (about 18 KB).
  PeakPair peaks_[kWaveMaxWidth];
  std::uint8_t alpha_top_[kWaveMaxWidth];  // L4: alpha of the first pixel of the column
  std::uint8_t alpha_bot_[kWaveMaxWidth];  // L4: alpha of the last pixel
  // Per list, the first column whose event falls at each row (a linked list through `next_`), see build_lists().
  std::uint16_t head_[kLists][kWaveMaxHeight + 2];
  std::uint16_t next_[kLists][kWaveMaxWidth];
  std::uint8_t state_[kWaveMaxWidth / 2];  // the running row: bits (L1) or nibbles (L4) of the columns whose run covers it
  std::uint8_t out_[kWaveMaxWidth / 2];    // L4: the row with its edge pixels
  alignas(4) std::uint8_t stage_[kChunkBytes];
};
static_assert(alignof(WaveBitmap) >= hal::kUploadAlign, "the staging buffer is word aligned");

}  // namespace o3sdk::ui::plot
