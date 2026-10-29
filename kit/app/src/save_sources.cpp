// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/save_sources.h>

#include <cstring>

namespace o3sdk::app {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the WAV source writes the samples as they are in memory: WAV is little-endian");

namespace {

void put16(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) noexcept {
  put16(p, v);
  put16(p + 2, v >> 16);
}

}  // namespace

// ---- The bytes of a buffer (track3r-os BufferSaveSource::step, verbatim) ----------------------------------------------------------------------

formats::Progress BufferSaveSource::step(std::size_t byte_budget) noexcept {
  while (byte_budget != 0u && pos_ < size_) {
    const std::size_t n = size_ - pos_ < byte_budget ? size_ - pos_ : byte_budget;
    const core::IoResult r = sink_->write(data_ + pos_, n);
    if (r.status != core::IoStatus::Ok || r.count == 0u || r.count > n) {
      return formats::Progress::Failed;
    }
    pos_ += r.count;
    byte_budget -= r.count;
  }
  return pos_ >= size_ ? formats::Progress::Done : formats::Progress::Running;
}

// ---- A PCM WAV (track3r-os WavSaveSource: make_header, set, begin and step, verbatim; the held check is not here) ------------------------------

void WavSaveSource::make_header(std::uint8_t* out, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate) noexcept {
  const std::uint32_t data = frames * channels * 2u;
  std::memcpy(out, "RIFF", 4);
  put32(out + 4, 36u + data);
  std::memcpy(out + 8, "WAVEfmt ", 8);
  put32(out + 16, 16);
  put16(out + 20, 1);  // PCM
  put16(out + 22, channels);
  put32(out + 24, rate);
  put32(out + 28, rate * channels * 2u);
  put16(out + 32, channels * 2u);
  put16(out + 34, 16);
  std::memcpy(out + 36, "data", 4);
  put32(out + 40, data);
}

bool WavSaveSource::set(const std::int16_t* pcm, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate, Check check, const void* ctx) noexcept {
  const std::uint64_t data = static_cast<std::uint64_t>(frames) * channels * 2u;
  if (pcm == nullptr || frames == 0u || channels < 1u || channels > 2u || rate == 0u || data + 36u > 0xFFFFFFFFu || static_cast<std::uint64_t>(rate) * channels * 2u > 0xFFFFFFFFu) {
    return false;
  }
  pcm_ = pcm;
  frames_ = frames;
  channels_ = channels;
  rate_ = rate;
  data_bytes_ = data;
  check_ = check;
  ctx_ = ctx;
  failure_ = "";
  return true;
}

void WavSaveSource::begin(core::ByteSink& sink) noexcept {
  sink_ = &sink;
  pos_ = 0;
  make_header(header_, frames_, channels_, rate_);
}

formats::Progress WavSaveSource::step(std::size_t byte_budget) noexcept {
  if (check_ != nullptr && !check_(ctx_)) {
    failure_ = "CHANGED WHILE SAVING";
    return formats::Progress::Failed;
  }
  const std::uint64_t total = kHeaderBytes + data_bytes_;
  while (byte_budget != 0u && pos_ < total) {
    const std::uint8_t* src;
    std::size_t n;
    if (pos_ < kHeaderBytes) {
      src = header_ + pos_;
      n = kHeaderBytes - static_cast<std::size_t>(pos_);
    } else {
      src = reinterpret_cast<const std::uint8_t*>(pcm_) + (pos_ - kHeaderBytes);
      n = static_cast<std::size_t>(total - pos_);
    }
    n = n < byte_budget ? n : byte_budget;
    const core::IoResult r = sink_->write(src, n);
    if (r.status != core::IoStatus::Ok || r.count == 0u || r.count > n) {
      return formats::Progress::Failed;
    }
    pos_ += r.count;
    byte_budget -= r.count;
  }
  return pos_ >= total ? formats::Progress::Done : formats::Progress::Running;
}

}  // namespace o3sdk::app
