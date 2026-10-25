// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/wave_bitmap.h>

#include <cstring>

namespace o3sdk::ui::plot {

namespace {

constexpr unsigned kEdgeMin = 6;  // the least alpha (of 15) an edge pixel gets: a peak that grazes a row still shows
constexpr unsigned kFull = 15;

// The alpha of an edge pixel the peak covers `cover16` sixteenths of (1..16).
constexpr std::uint8_t edge_alpha(unsigned cover16) noexcept {
  const unsigned a = (cover16 * kFull + 8u) / 16u;
  return static_cast<std::uint8_t>(a < kEdgeMin ? kEdgeMin : a);
}

struct Rows {
  unsigned top;
  unsigned bot;
  std::int32_t yt;
  std::int32_t yb;
};

Rows rows_of(const PeakPair& p, unsigned height) noexcept {
  const std::int32_t yt = wave_y_q4(p.max, height);
  const std::int32_t yb = wave_y_q4(p.min, height);
  return Rows{static_cast<unsigned>(yt >> 4), static_cast<unsigned>(yb >> 4), yt, yb};
}

}  // namespace

void WaveBitmap::reset() noexcept {
  for (Lane& l : lanes_) {
    l = Lane{};
  }
  active_ = -1;
  failed_ = 0;
  completed_ = 0;
  chunk_ready_ = false;
  gen_pos_ = 0;
  total_ = 0;
}

unsigned WaveBitmap::shown_stride(unsigned lane) const noexcept {
  const Lane& l = lanes_[lane < kWaveLanes ? lane : 0];
  return stride_of(l.shown.width, l.shown.depth);
}

void WaveBitmap::request(unsigned lane, const WaveKey& key) noexcept {
  if (lane >= kWaveLanes) {
    return;
  }
  Lane& l = lanes_[lane];
  const bool shows_it = key.valid() && l.shown_valid && l.shown == key;
  if (!key.valid() || shows_it) {  // nothing to render: drop what was queued
    l.want_pending = false;
    if (active_ == static_cast<int>(lane)) {
      active_ = -1;
      chunk_ready_ = false;
    }
    return;
  }
  if (l.want_pending && l.want == key) {
    return;  // already queued, or being rendered
  }
  l.want = key;
  l.want_pending = true;
  if (active_ == static_cast<int>(lane)) {  // a different picture was in progress: start over
    active_ = -1;
    chunk_ready_ = false;
  }
}

// The runs of full-scale columns, at most kMaxClipRuns: past that the last run is stretched over the rest.
unsigned WaveBitmap::find_clips(const PeakPair* peaks, unsigned width, ClipRun* out) noexcept {
  unsigned n = 0;
  bool in_run = false;
  for (unsigned c = 0; c < width; ++c) {
    const bool clipped = !peaks[c].empty() && (peaks[c].max == 32767 || peaks[c].min == -32768);
    if (clipped && !in_run) {
      if (n < kMaxClipRuns) {
        out[n++] = ClipRun{static_cast<std::uint16_t>(c), static_cast<std::uint16_t>(c)};
      }
      in_run = true;
    }
    if (clipped && n > 0) {
      out[n - 1].x1 = static_cast<std::uint16_t>(c);
    }
    if (!clipped) {
      in_run = false;
    }
  }
  return n;
}

// Puts every column's events in the list of the row where it happens. The picture is made a row at a time from the one
// above, so a column needs attention only where its run of lit rows begins and ends:
//   L1  list 0: set the bit at row `top`            list 1: clear it at row `bot + 1`
//   L4  list 0: the interior (full alpha) starts at row top + 1      list 1: it ends at row `bot`
//       list 2: the first pixel is drawn at row `top` with alpha_top_[c]    list 3: the last one at row `bot`, alpha_bot_[c]
//   (a column that lies in one row is a single full pixel, list 2; two rows have no interior)
void WaveBitmap::build_lists(unsigned width, unsigned height, WaveDepth depth) noexcept {
  for (unsigned l = 0; l < kLists; ++l) {
    for (unsigned r = 0; r < height + 2u; ++r) {
      head_[l][r] = kNil;
    }
  }
  const auto push = [this](unsigned list, unsigned row, unsigned col) {
    next_[list][col] = head_[list][row];
    head_[list][row] = static_cast<std::uint16_t>(col);
  };
  for (unsigned c = 0; c < width; ++c) {
    alpha_top_[c] = 0;
    alpha_bot_[c] = 0;
    if (peaks_[c].empty()) {
      continue;
    }
    const Rows r = rows_of(peaks_[c], height);
    if (depth == WaveDepth::L1) {
      push(0, r.top, c);
      push(1, r.bot + 1u, c);
    } else if (r.top == r.bot) {
      alpha_top_[c] = static_cast<std::uint8_t>(kFull);
      push(2, r.top, c);
    } else {
      alpha_top_[c] = edge_alpha(16u - static_cast<unsigned>(r.yt & 15));
      alpha_bot_[c] = edge_alpha(static_cast<unsigned>(r.yb & 15) + 1u);
      push(2, r.top, c);
      push(3, r.bot, c);
      if (r.bot - r.top >= 2u) {
        push(0, r.top + 1u, c);
        push(1, r.bot, c);
      }
    }
  }
}

bool WaveBitmap::start(unsigned lane) noexcept {
  Lane& l = lanes_[lane];
  const WaveKey& k = l.want;
  if (!k.valid()) {
    return false;
  }
  width_ = k.width;
  height_ = k.height;
  depth_ = k.depth;
  stride_ = stride_of(width_, depth_);
  total_ = static_cast<std::size_t>(stride_) * height_;
  if (total_ > kWaveSlotBytes || stride_ > sizeof(state_)) {
    return false;
  }
  k.cache->columns(k.channel, k.first, k.span, width_, peaks_);
  job_clip_count_ = find_clips(peaks_, width_, job_clips_);
  build_lists(width_, height_, depth_);
  slot_ = l.shown_valid ? 1u - l.shown_slot : 0u;
  gen_pos_ = 0;
  cur_row_ = -1;
  chunk_ready_ = false;
  std::memset(state_, 0, stride_);
  return true;
}

void WaveBitmap::gen_row(unsigned y) noexcept {
  const auto clear = [this](unsigned c) {
    if (depth_ == WaveDepth::L1) {
      state_[c >> 3] = static_cast<std::uint8_t>(state_[c >> 3] & ~(0x80u >> (c & 7u)));
    } else {
      state_[c >> 1] = static_cast<std::uint8_t>(state_[c >> 1] & ((c & 1u) != 0 ? 0xF0u : 0x0Fu));
    }
  };
  for (std::uint16_t c = head_[1][y]; c != kNil; c = next_[1][c]) {
    clear(c);
  }
  if (depth_ == WaveDepth::L1) {
    for (std::uint16_t c = head_[0][y]; c != kNil; c = next_[0][c]) {
      state_[c >> 3] = static_cast<std::uint8_t>(state_[c >> 3] | (0x80u >> (c & 7u)));
    }
  } else {
    for (std::uint16_t c = head_[0][y]; c != kNil; c = next_[0][c]) {
      state_[c >> 1] = static_cast<std::uint8_t>(state_[c >> 1] | ((c & 1u) != 0 ? 0x0Fu : 0xF0u));
    }
    std::memcpy(out_, state_, stride_);
    const auto put = [this](unsigned c, unsigned alpha) {
      out_[c >> 1] = static_cast<std::uint8_t>(out_[c >> 1] | ((c & 1u) != 0 ? alpha : alpha << 4));
    };
    for (std::uint16_t c = head_[2][y]; c != kNil; c = next_[2][c]) {
      put(c, alpha_top_[c]);
    }
    for (std::uint16_t c = head_[3][y]; c != kNil; c = next_[3][c]) {
      put(c, alpha_bot_[c]);
    }
  }
  cur_row_ = static_cast<int>(y);
}

// Renders up to `max` bytes of the picture from byte gen_pos_ on. A chunk is a fixed number of bytes, not of rows: it may
// begin and end inside a row, which `cur_row_` and the row buffers carry over from one chunk to the next.
std::size_t WaveBitmap::fill(std::uint8_t* dst, std::size_t max) noexcept {
  const std::uint8_t* row = depth_ == WaveDepth::L1 ? state_ : out_;
  std::size_t n = 0;
  while (n < max && gen_pos_ + n < total_) {
    const std::size_t pos = gen_pos_ + n;
    const unsigned y = static_cast<unsigned>(pos / stride_);
    const unsigned x0 = static_cast<unsigned>(pos % stride_);
    if (static_cast<int>(y) != cur_row_) {
      gen_row(y);
    }
    std::size_t take = stride_ - x0;
    take = take < max - n ? take : max - n;
    take = take < total_ - pos ? take : total_ - pos;
    std::memcpy(dst + n, row + x0, take);
    n += take;
  }
  return n;
}

void WaveBitmap::finish() noexcept {
  Lane& l = lanes_[active_];
  l.shown = l.want;
  l.shown_valid = true;
  l.shown_slot = slot_;
  l.want_pending = false;
  l.clip_count = job_clip_count_;
  for (unsigned i = 0; i < job_clip_count_; ++i) {
    l.clips[i] = job_clips_[i];
  }
  active_ = -1;
  chunk_ready_ = false;
  ++completed_;
}

bool WaveBitmap::service(hal::Display& display) noexcept {
  if (active_ < 0) {
    const int pick = lanes_[0].want_pending ? 0 : (lanes_[1].want_pending ? 1 : -1);
    if (pick < 0) {
      return false;
    }
    if (!lanes_[pick].want.valid()) {  // the cache was rebuilt since the request: nothing to render from it now
      lanes_[pick].want_pending = false;
      return busy();
    }
    if (!start(static_cast<unsigned>(pick))) {
      lanes_[pick].want_pending = false;
      ++failed_;
      return busy();
    }
    active_ = pick;
  }
  if (!chunk_ready_) {
    const std::size_t want = total_ - gen_pos_;
    chunk_addr_ = wave_slot_addr(static_cast<unsigned>(active_), slot_) + static_cast<std::uint32_t>(gen_pos_);
    chunk_n_ = fill(stage_, want < kChunkBytes ? want : kChunkBytes);
    gen_pos_ += chunk_n_;
    chunk_ready_ = true;
  }
  const hal::UploadResult r = display.upload(chunk_addr_, stage_, chunk_n_);
  if (r == hal::UploadResult::Busy) {
    return true;  // the chunk stays in the stage: the next pass sends it
  }
  if (r != hal::UploadResult::Ok) {
    lanes_[active_].want_pending = false;
    active_ = -1;
    chunk_ready_ = false;
    ++failed_;
    return busy();
  }
  chunk_ready_ = false;
  if (gen_pos_ >= total_) {
    finish();
  }
  return busy();
}

bool WaveBitmap::service_all(hal::Display& display) noexcept {
  const unsigned failed_before = failed_;
  // Every pass moves a chunk or ends a job; the bound is the biggest picture's chunks times the lanes, with room for Busy.
  for (unsigned guard = 0; guard < 4096 && service(display); ++guard) {
  }
  return failed_ == failed_before;
}

void WaveBitmap::render_reference(const PeakPair* peaks, unsigned width, unsigned height, WaveDepth depth,
                                  std::uint8_t* out) noexcept {
  const unsigned stride = stride_of(width, depth);
  std::memset(out, 0, static_cast<std::size_t>(stride) * height);
  for (unsigned c = 0; c < width; ++c) {
    if (peaks[c].empty()) {
      continue;
    }
    const Rows r = rows_of(peaks[c], height);
    for (unsigned y = r.top; y <= r.bot; ++y) {
      unsigned alpha = kFull;
      if (r.top != r.bot && y == r.top) {
        alpha = edge_alpha(16u - static_cast<unsigned>(r.yt & 15));
      } else if (r.top != r.bot && y == r.bot) {
        alpha = edge_alpha(static_cast<unsigned>(r.yb & 15) + 1u);
      }
      std::uint8_t& b = out[static_cast<std::size_t>(y) * stride + (depth == WaveDepth::L1 ? c >> 3 : c >> 1)];
      if (depth == WaveDepth::L1) {
        b = static_cast<std::uint8_t>(b | (0x80u >> (c & 7u)));
      } else {
        b = static_cast<std::uint8_t>(b | ((c & 1u) != 0 ? alpha : alpha << 4));
      }
    }
  }
}

}  // namespace o3sdk::ui::plot
