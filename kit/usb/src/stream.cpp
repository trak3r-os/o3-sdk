// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/usb/stream.h>

#include <cstring>

namespace o3sdk::usb {
namespace {

constexpr std::int32_t kFillAvgDiv = 1024;  // return fill: about 1 s at one update per SOF
constexpr std::int32_t kFillAvgPackets = 8;  // stem fill: 8 packets
constexpr std::int32_t kTrimDivQ16 = 4096;        // err_q16 / 4096 = 16 Q16 units per frame = 4 Q10.14 LSB per frame

}  // namespace

std::uint32_t InPacketiser::next(Speed s, std::uint32_t rate_q16, std::uint32_t smoothed_fill, FillBand band) noexcept {
  const std::uint32_t lo = frames_lo(s);
  const std::uint32_t hi = frames_hi(s);
  const std::uint32_t rate = rate_q16 < (lo << 16) ? (lo << 16) : (rate_q16 > (hi << 16) ? (hi << 16) : rate_q16);
  acc_ += rate;                 // < 65536 + hi * 65536 by the invariant, so no overflow and n is lo or hi
  std::uint32_t n = acc_ >> 16;
  acc_ &= 0xFFFFu;
  if (smoothed_fill < band.low) {
    n = lo;
  } else if (smoothed_fill > band.high) {
    n = hi;
  }
  return n;
}

void FeedbackControl::reset() noexcept {
  armed_ = false;
  avg_q16_ = static_cast<std::int32_t>(kReturnTargetFrames << 16);
  trim_q16_ = 0;
}

std::uint32_t FeedbackControl::update(std::uint32_t rate_q16, std::uint32_t return_fill) noexcept {
  if (!armed_) {
    if (return_fill < kReturnPrimeFrames) {
      return rate_q16;
    }
    armed_ = true;
    avg_q16_ = static_cast<std::int32_t>(kReturnTargetFrames << 16);
  }
  const std::int32_t fill_q16 = static_cast<std::int32_t>(return_fill << 16);
  avg_q16_ += (fill_q16 - avg_q16_) / kFillAvgDiv;
  const std::int32_t err_q16 = avg_q16_ - static_cast<std::int32_t>(kReturnTargetFrames << 16);
  const std::int32_t dead_q16 = static_cast<std::int32_t>(kReturnDeadFrames << 16);
  const std::int32_t beyond = err_q16 > dead_q16 ? err_q16 - dead_q16 : (err_q16 < -dead_q16 ? err_q16 + dead_q16 : 0);
  std::int32_t trim = -(beyond / kTrimDivQ16);
  trim = trim > kTrimLimitQ16 ? kTrimLimitQ16 : (trim < -kTrimLimitQ16 ? -kTrimLimitQ16 : trim);
  trim_q16_ = trim;
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(rate_q16) + trim);
}

void StemReader::restart() noexcept {
  ring_.flush();
  off_ = kStemBlockFrames;
}

std::uint32_t StemReader::fill_frames() const noexcept {
  return ring_.size() * kStemBlockFrames + (kStemBlockFrames - off_);
}

std::uint32_t StemReader::read_interleaved(std::int16_t* out, std::uint32_t frames, std::uint32_t channels) noexcept {
  std::uint32_t done = 0;
  while (done < frames) {
    if (off_ >= kStemBlockFrames) {
      if (ring_.read(&cur_, 1) == 0) {
        break;
      }
      off_ = 0;
    }
    const std::uint32_t room = kStemBlockFrames - off_;
    const std::uint32_t chunk = (frames - done) < room ? (frames - done) : room;
    interleave(cur_.ch, channels, off_, chunk, out + done * channels);
    off_ += chunk;
    done += chunk;
  }
  return done;
}

void InStream::start(Speed speed, std::uint32_t channels) noexcept {
  speed_ = speed;
  channels_ = channels < kMaxChannels ? channels : kMaxChannels;
  reader_.restart();
  packetiser_.reset();
  primed_ = false;
  avg_q8_ = 0;
  active_ = true;
}

std::uint32_t InStream::build_packet(std::uint32_t rate_q16, std::int16_t* out) noexcept {
  const std::uint32_t fill = reader_.fill_frames();
  if (!primed_ && fill >= kStemPrimeFrames) {
    primed_ = true;
    avg_q8_ = fill << 8;
  } else if (primed_) {
    const std::int32_t diff = static_cast<std::int32_t>(fill << 8) - static_cast<std::int32_t>(avg_q8_);
    avg_q8_ = static_cast<std::uint32_t>(static_cast<std::int32_t>(avg_q8_) + diff / kFillAvgPackets);
  }
  // While priming there is nothing to guard: run the natural cadence (a mid-band value switches the guard off).
  const std::uint32_t guard = primed_ ? (avg_q8_ >> 8) : (kStemFillBand.low + kStemFillBand.high) / 2u;
  const std::uint32_t n = packetiser_.next(speed_, rate_q16, guard, kStemFillBand);

  if (primed_) {
    if (fill < counters_.in_fill_min) {
      counters_.in_fill_min = fill;
    }
    if (fill > counters_.in_fill_max) {
      counters_.in_fill_max = fill;
    }
    const std::uint32_t got = reader_.read_interleaved(out, n, channels_);
    if (got < n) {  // ran dry: finish with silence and rebuild the start-up fill before sending audio again
      std::memset(out + got * channels_, 0, (n - got) * channels_ * kBytesPerSample);
      ++counters_.in_underruns;
      primed_ = false;
    }
    counters_.in_dropped_blocks = reader_.dropped_blocks();
  } else {
    std::memset(out, 0, n * channels_ * kBytesPerSample);
    ++counters_.in_prime_packets;
  }
  if (n == frames_lo(speed_)) {
    ++counters_.in_size_lo;
  } else if (n == frames_hi(speed_)) {
    ++counters_.in_size_hi;
  } else {
    ++counters_.in_size_other;
  }
  return n;
}

void OutStream::on_packet(const std::uint32_t* frames, std::uint32_t bytes) noexcept {
  std::uint32_t n = bytes / 4u;
  if ((bytes & 3u) != 0u || n > kReturnMaxChunk) {
    ++counters_.out_bad_packets;
    if (n > kReturnMaxChunk) {
      n = kReturnMaxChunk;
    }
  }
  ++counters_.out_packets;
  const std::uint32_t fill = ring_.size_from_producer();
  if (fill < counters_.out_fill_min) {
    counters_.out_fill_min = fill;
  }
  if (fill > counters_.out_fill_max) {
    counters_.out_fill_max = fill;
  }
  ring_.write(frames, n);
}

bool ReturnReader::pull(std::int16_t* left, std::int16_t* right, std::uint32_t frames) noexcept {
  if (frames > kStemBlockFrames) {
    frames = kStemBlockFrames;
  }
  std::uint32_t tmp[kStemBlockFrames];
  if (restart_.exchange(false, std::memory_order_acquire)) {
    ring_.flush();
    primed_ = false;
  }
  if (!primed_) {
    const std::uint32_t fill = ring_.size();
    if (fill >= kReturnPrimeFrames) {
      for (std::uint32_t surplus = fill - kReturnPrimeFrames; surplus != 0;) {  // at most 3 rounds of 128
        const std::uint32_t took = ring_.read(tmp, surplus < kStemBlockFrames ? surplus : kStemBlockFrames);
        if (took == 0) {
          break;
        }
        surplus -= took;
      }
      primed_ = true;
    }
  }
  std::uint32_t got = 0;
  if (primed_) {
    got = ring_.read(tmp, frames);
    counters_.out_dropped_frames = ring_.dropped();
    if (got < frames) {
      ++counters_.out_underruns;
      primed_ = false;
    }
  }
  for (std::uint32_t i = got; i < frames; ++i) {
    tmp[i] = 0;
  }
  deinterleave_stereo(tmp, frames, left, right);
  return got == frames;
}

}  // namespace o3sdk::usb
