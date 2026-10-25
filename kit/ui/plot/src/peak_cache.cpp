// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/peak_cache.h>

#include <cstdint>

namespace o3sdk::ui::plot {

namespace {

constexpr unsigned kFan = 1u << PeakCache::kFanShift;  // entries of a level under one entry of the next

}  // namespace

bool PeakCache::init(std::int16_t* storage, std::size_t storage_elems, const std::int16_t* samples, Frame frames,
                     unsigned channels) noexcept {
  valid_ = false;
  built_ = false;
  ++epoch_;  // whatever was cached is gone
  storage_ = nullptr;
  samples_ = nullptr;
  frames_ = 0;
  channels_ = 0;
  levels_ = 0;
  if (channels < 1 || channels > kMaxChannels || frames > kMaxFrames || (frames > 0 && samples == nullptr)) {
    return false;
  }
  const std::size_t need = elements_required(frames, channels);
  if ((need > 0 && storage == nullptr) || storage_elems < need) {
    return false;
  }
  // The samples are read a word at a time (peak_kernel.h) and the arrays of entries start on words: both are
  // declared at their owners with alignas, and a cast from a stray pointer is refused here rather than faulting later.
  if (reinterpret_cast<std::uintptr_t>(samples) % alignof(std::uint32_t) != 0 ||
      reinterpret_cast<std::uintptr_t>(storage) % alignof(std::uint32_t) != 0) {
    return false;
  }
  frames_ = frames;
  channels_ = channels;
  levels_ = levels_for(frames);
  storage_ = storage;
  samples_ = samples;
  std::uint32_t per_channel = 0;
  for (unsigned l = 1; l <= levels_; ++l) {
    pad_[l] = static_cast<std::uint32_t>((entries_at(frames, l) + 1u) & ~Frame{1});
    for (unsigned ch = 0; ch < channels; ++ch) {
      off_[ch][l] = per_channel + ch * static_cast<std::uint32_t>(need / channels);
    }
    per_channel += 2u * pad_[l];
  }
  valid_ = true;
  built_ = levels_ == 0;
  return true;
}

void PeakCache::make_level1(unsigned ch, Frame entry) noexcept {
  const Frame a = entry << kBaseShift;
  const Frame end = a + (Frame{1} << kBaseShift);
  const Frame b = end < frames_ ? end : frames_;
  int mn = 32767, mx = -32768;
  detail::scan_raw(samples_, channels_, ch, a, b, mn, mx);
  mins(ch, 1)[entry] = static_cast<std::int16_t>(mn);
  maxs(ch, 1)[entry] = static_cast<std::int16_t>(mx);
}

void PeakCache::make_upper(unsigned ch, unsigned level, Frame entry) noexcept {
  const Frame first = entry * kFan;
  const Frame below = entries_at(frames_, level - 1);
  const Frame last = first + kFan < below ? first + kFan : below;
  const std::int16_t* lo = mins(ch, level - 1);
  const std::int16_t* hi = maxs(ch, level - 1);
  int mn = 32767, mx = -32768;
  for (Frame i = first; i < last; ++i) {
    mn = lo[i] < mn ? lo[i] : mn;
    mx = hi[i] > mx ? hi[i] : mx;
  }
  mins(ch, level)[entry] = static_cast<std::int16_t>(mn);
  maxs(ch, level)[entry] = static_cast<std::int16_t>(mx);
}

void PeakCache::build() noexcept {
  build_begin();
  while (!build_step(~std::size_t{0})) {
  }
}

void PeakCache::build_begin() noexcept {
  step_level_ = 1;
  step_entry_ = 0;
  built_ = !valid_ || levels_ == 0;
  ++epoch_;
}

bool PeakCache::build_step(std::size_t budget) noexcept {
  std::size_t used = 0;
  while (!built_ && used < budget) {
    if (step_entry_ >= entries_at(frames_, step_level_)) {
      ++step_level_;
      step_entry_ = 0;
      built_ = step_level_ > levels_;
      epoch_ += built_ ? 1u : 0u;  // the picture of a cache that was queried while it was building is not this cache's
      continue;
    }
    for (unsigned ch = 0; ch < channels_; ++ch) {
      if (step_level_ == 1) {
        make_level1(ch, step_entry_);
      } else {
        make_upper(ch, step_level_, step_entry_);
      }
    }
    used += (step_level_ == 1 ? (std::size_t{1} << kBaseShift) : std::size_t{kFan}) * channels_;
    ++step_entry_;
  }
  return built_;
}

void PeakCache::update(Frame first, Frame count) noexcept {
  if (!valid_ || count == 0 || first >= frames_) {
    return;
  }
  if (!built_) {  // nothing to redo yet, and entries already made may be of the old samples: start the build again
    build_begin();
    return;
  }
  const std::uint64_t end64 = std::uint64_t{first} + count;
  const Frame end = end64 < frames_ ? static_cast<Frame>(end64) : frames_;
  Frame e0 = first >> kBaseShift;
  Frame e1 = (end - 1u) >> kBaseShift;
  for (unsigned level = 1; level <= levels_; ++level) {
    for (Frame e = e0; e <= e1; ++e) {
      for (unsigned ch = 0; ch < channels_; ++ch) {
        if (level == 1) {
          make_level1(ch, e);
        } else {
          make_upper(ch, level, e);
        }
      }
    }
    e0 >>= kFanShift;
    e1 >>= kFanShift;
  }
}

// Folds what lies in [x, y) at `tier` into mn / mx: raw frames at tier 0, the entries of level `tier` above it. x and y
// are multiples of the tier's block (range() arranges it), so the entries are [x >> shift, y >> shift).
void PeakCache::scan_tier(unsigned ch, unsigned tier, Frame x, Frame y, int& mn, int& mx) const noexcept {
  if (x >= y) {
    return;
  }
  if (tier == 0) {
    detail::scan_raw(samples_, channels_, ch, x, y, mn, mx);
    return;
  }
  const unsigned shift = level_shift(tier);
  const std::int16_t* lo = mins(ch, tier);
  const std::int16_t* hi = maxs(ch, tier);
  for (Frame i = x >> shift; i < (y >> shift); ++i) {
    mn = lo[i] < mn ? lo[i] : mn;
    mx = hi[i] > mx ? hi[i] : mx;
  }
}

PeakPair PeakCache::range(unsigned channel, Frame begin, Frame end) const noexcept {
  if (end > frames_) {
    end = frames_;
  }
  if (!valid_ || !built_ || channel >= channels_ || begin >= end) {
    return kNoPeak;
  }
  int mn = 32767, mx = -32768;
  Frame lo = begin, hi = end;
  for (unsigned tier = 0;; ++tier) {
    if (tier >= levels_) {  // nothing above this tier: the rest is read here
      scan_tier(channel, tier, lo, hi, mn, mx);
      break;
    }
    // Whole blocks of the next level up that lie inside [lo, hi): [lo_up, hi_up). The two ragged ends are read at this
    // tier, at most 63 frames or 3 entries each; the middle goes up a tier.
    const Frame mask = (Frame{1} << level_shift(tier + 1)) - 1u;
    const Frame lo_up = (lo + mask) & ~mask;
    const Frame hi_up = hi & ~mask;
    if (lo_up >= hi_up) {
      scan_tier(channel, tier, lo, hi, mn, mx);
      break;
    }
    scan_tier(channel, tier, lo, lo_up, mn, mx);
    scan_tier(channel, tier, hi_up, hi, mn, mx);
    lo = lo_up;
    hi = hi_up;
  }
  return PeakPair{static_cast<std::int16_t>(mn), static_cast<std::int16_t>(mx)};
}

void PeakCache::columns(unsigned channel, Frame first, Frame span, unsigned count, PeakPair* out) const noexcept {
  if (count == 0 || out == nullptr) {
    return;
  }
  if (span == 0) {
    span = 1;
  }
  const Frame q = span / count;
  const Frame r = span % count;
  Frame acc = 0;
  std::uint64_t start = first;
  for (unsigned c = 0; c < count; ++c) {
    Frame step = q;
    acc += r;
    if (acc >= count) {
      acc -= count;
      ++step;
    }
    const std::uint64_t next = start + step;
    if (!valid_ || !built_ || channel >= channels_ || start >= frames_) {
      out[c] = kNoPeak;
    } else if (step == 0) {  // zoomed in past a frame a column: the column holds the frame it falls in
      const std::int16_t v = samples_[static_cast<std::size_t>(start) * channels_ + channel];
      out[c] = PeakPair{v, v};
    } else {
      out[c] = range(channel, static_cast<Frame>(start), next < frames_ ? static_cast<Frame>(next) : frames_);
    }
    start = next;
  }
}

Frame PeakCache::column_start(Frame first, Frame span, unsigned count, unsigned c) noexcept {
  if (count == 0) {
    return first;
  }
  return static_cast<Frame>(first + (std::uint64_t{c} * span) / count);
}

unsigned PeakCache::column_of(Frame first, Frame span, unsigned count, Frame frame) noexcept {
  if (count == 0 || frame < first) {
    return 0;
  }
  const std::uint64_t d = std::uint64_t{frame} - first;
  if (span == 0 || d >= span) {
    return count;
  }
  const std::uint64_t c = ((d + 1u) * count - 1u) / span;
  return c < count ? static_cast<unsigned>(c) : count - 1u;
}

}  // namespace o3sdk::ui::plot
