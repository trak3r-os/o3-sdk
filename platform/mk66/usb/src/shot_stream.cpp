// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/usb/shot_stream.h>

#include <o3sdk/platform/mk66/display/ft812_regs.h>

namespace o3sdk::platform::mk66::console {
namespace {

constexpr std::size_t kHeadroom = 120;  // the room BEGIN and END need

std::uint32_t bytes_per_pixel(ShotFormat f) noexcept { return f == ShotFormat::Argb8 ? 4u : 2u; }

// Appends to a fixed line buffer; the line is built whole and sent in one call, so the output never holds half a line.
struct Line {
  char text[ShotStream::kLineTextMax];
  std::size_t len = 0;
  void chr(char c) noexcept {
    if (len < sizeof text) {
      text[len++] = c;
    }
  }
  void str(const char* s) noexcept {
    for (; *s != '\0'; ++s) {
      chr(*s);
    }
  }
  void dec(std::uint32_t v) noexcept {
    char digits[10];
    unsigned n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10u);
      v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
      chr(digits[--n]);
    }
  }
  void hex(std::uint32_t v, unsigned digits) noexcept {  // plain upper-case hex digits, no prefix
    for (unsigned i = digits; i != 0u; --i) {
      chr("0123456789ABCDEF"[(v >> (4u * (i - 1u))) & 0xFu]);
    }
  }
};

}  // namespace

std::size_t ShotStream::base64(const std::uint8_t* in, std::size_t n, char* out) noexcept {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::size_t o = 0;
  for (std::size_t i = 0; i < n; i += 3) {
    const std::uint32_t b0 = in[i];
    const std::uint32_t b1 = i + 1 < n ? in[i + 1] : 0u;
    const std::uint32_t b2 = i + 2 < n ? in[i + 2] : 0u;
    const std::uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out[o++] = kAlphabet[(v >> 18) & 63u];
    out[o++] = kAlphabet[(v >> 12) & 63u];
    out[o++] = i + 1 < n ? kAlphabet[(v >> 6) & 63u] : '=';
    out[o++] = i + 2 < n ? kAlphabet[v & 63u] : '=';
  }
  return o;
}

bool BandReader::start(const ShotParams& params, std::uint32_t region, std::uint32_t region_bytes) noexcept {
  state_ = State::Idle;
  error_ = "";
  if (region_bytes > kMaxRegionBytes) {
    region_bytes = kMaxRegionBytes;
  }
  constexpr std::uint32_t kRamG = display::ft812::kRamGBytes;
  if (params.width_param == 0u || params.width_param > 4095u || region % 4u != 0u || params.height == 0u || params.height > kHeight || region > kRamG || region_bytes > kRamG - region) {
    return false;  // (the last two: the region must lie inside RAM_G)
  }
  const std::uint32_t bpp = bytes_per_pixel(params.format);
  const std::uint32_t row_bytes = kWidth * bpp;
  // What the chip may WRITE a row is `width_param` pixels, whatever the screen's width is: the bands are sized by the larger of the two, so that no `w` (the console takes
  // 1..4095 for a hardware session's experiment) can make a band run past the region, past RAM_G, over the faces and tiles. What is READ is the screen's own row_bytes.
  const std::uint32_t stride = params.width_param * bpp > row_bytes ? params.width_param * bpp : row_bytes;
  if (region_bytes < stride) {
    return false;
  }
  const std::uint32_t height = params.height;
  const std::uint32_t max_rows = region_bytes / stride;  // >= 1
  const std::uint32_t bands = (height + max_rows - 1u) / max_rows;
  const std::uint32_t rows = (height + bands - 1u) / bands;
  if (rows * stride > region_bytes) {  // cannot happen by the arithmetic above; refused all the same, never trusted
    return false;
  }
  params_ = params;
  region_ = region;
  row_bytes_ = row_bytes;
  height_ = height;
  band_rows_ = rows;
  bands_ = (height + rows - 1u) / rows;
  band_ = 0;
  band_row0_ = 0;
  band_bytes_ = 0;
  band_done_ = 0;
  fresh_ = true;
  state_ = State::StartBand;
  return true;
}

void BandReader::begin(ShotPort& port, std::uint32_t now) noexcept {
  if (fresh_) {
    progress_ms_ = now;  // the stall clock starts with the first pass, not at zero
    reinits_at_start_ = port.reinits();
    fresh_ = false;
  }
}

void BandReader::cancel(ShotPort& port) noexcept {
  if (state_ == State::WaitBand) {
    port.abort_capture();  // a command is queued or running in the coprocessor: leave it clean
  }
  state_ = State::Idle;
}

BandReader::State BandReader::step(ShotPort& port, std::uint32_t now) noexcept {
  if (state_ == State::Idle || state_ == State::Failed) {
    return state_;
  }
  // The region and the bands are RAM_G contents: a chip that was set up again meanwhile (the driver does it after a stalled swap) has lost them, and a display that gave up
  // answers nothing. Say so, leave the coprocessor clean, and stop.
  if (port.reinits() != reinits_at_start_) {
    if (state_ == State::WaitBand) {
      port.abort_capture();
    }
    return fail("display re-initialised: the picture is lost");
  }
  if (!port.ready()) {
    if (state_ == State::WaitBand) {
      port.abort_capture();
    }
    return fail("display stopped");
  }
  switch (state_) {
    case State::StartBand: {
      if (!port.bus_free()) {
        break;
      }
      band_row0_ = band_ * band_rows_;
      const std::uint32_t rows = height_ - band_row0_ < band_rows_ ? height_ - band_row0_ : band_rows_;
      band_bytes_ = rows * row_bytes_;
      band_done_ = 0;
      const std::uint32_t format = params_.format == ShotFormat::Argb8 ? display::ft812::kFmtArgb8 : display::ft812::kFmtRgb565;
      if (!port.start_capture(format, region_, band_row0_, params_.width_param, rows)) {
        return fail("the coprocessor took no command");
      }
      band_started_ms_ = now;
      progress_ms_ = now;
      state_ = State::WaitBand;
      break;
    }
    case State::WaitBand: {
      if (!port.bus_free()) {
        break;
      }
      const ShotPort::Capture c = port.poll_capture();
      if (c == ShotPort::Capture::Fault) {
        return fail("coprocessor fault");
      }
      if (c == ShotPort::Capture::Done) {
        band_done_ = 0;
        progress_ms_ = now;
        state_ = State::Reading;
      } else if (now - band_started_ms_ > kCaptureTimeoutMs) {
        port.abort_capture();  // the stuck command must not stay queued: the next frame's or shot's would wait behind it
        return fail("snapshot timeout");
      }
      break;
    }
    case State::Idle:
    case State::Reading:
    case State::Done:
    case State::Failed:
      break;
  }
  return state_;
}

std::size_t BandReader::read(ShotPort& port, std::uint8_t* out, std::size_t cap, std::uint32_t now, std::uint32_t& offset) noexcept {
  if (state_ != State::Reading) {
    return 0;
  }
  const std::size_t left = band_bytes_ - band_done_;
  const std::size_t n = left < cap ? left : cap;
  port.read(region_ + band_done_, out, n);
  offset = band_row0_ * row_bytes_ + band_done_;
  band_done_ += static_cast<std::uint32_t>(n);
  progress_ms_ = now;
  if (band_done_ >= band_bytes_) {
    ++band_;
    state_ = band_ < bands_ ? State::StartBand : State::Done;
  }
  return n;
}

bool ShotStream::start(const ShotParams& params, std::uint32_t region, std::uint32_t region_bytes) noexcept {
  phase_ = Phase::Idle;
  if (!reader_.start(params, region, region_bytes)) {
    return false;
  }
  lines_ = 0;
  crc_.reset();
  phase_ = Phase::Begin;
  return true;
}

void ShotStream::cancel(ShotPort& port) noexcept {
  reader_.cancel(port);
  phase_ = Phase::Idle;
}

void ShotStream::fail(usb::diag::Output& out, const char* reason) noexcept {
  Line l;
  l.str("SHOT ERR ");
  l.str(reason);
  l.chr('\n');
  out.text(l.text, l.len);
  phase_ = Phase::Idle;
}

bool ShotStream::step(usb::diag::Output& out, ShotPort& port) noexcept {
  if (phase_ == Phase::Idle) {
    return false;
  }
  const std::uint32_t now = port.now_ms();
  reader_.begin(port, now);
  const bool was_reading = reader_.state() == BandReader::State::Reading;  // (a band that became readable this pass is read from the next one)
  BandReader::State st = BandReader::State::Idle;
  if (phase_ != Phase::Begin) {
    st = reader_.step(port, now);
    if (st == BandReader::State::Failed) {
      fail(out, reader_.error());
      return false;
    }
  }
  switch (phase_) {
    case Phase::Begin: {
      if (!port.ready()) {
        fail(out, "display not running");
        return false;
      }
      if (out.room() < kHeadroom) {
        break;
      }
      Line l;
      l.str("SHOT BEGIN fmt=");
      l.str(reader_.row_bytes() == kWidth * 4u ? "8888" : "565");
      l.str(" w=");
      l.dec(kWidth);
      l.str(" h=");
      l.dec(reader_.rows());
      l.str(" bytes=");
      l.dec(reader_.row_bytes() * reader_.rows());
      l.str(" line=");
      l.dec(static_cast<std::uint32_t>(kLineBytes));
      l.chr('\n');
      out.text(l.text, l.len);
      frames_at_start_ = port.frames();
      reader_.touch(now);
      phase_ = Phase::Run;
      break;
    }
    case Phase::Run: {
      for (unsigned i = 0; was_reading && st == BandReader::State::Reading && i < kLinesPerPass && reader_.left_in_band() != 0u; ++i) {
        if (!port.bus_free() || out.room() < kLineTextMax) {
          break;
        }
        std::uint8_t raw[kLineBytes];
        std::uint32_t offset = 0;
        const std::size_t n = reader_.read(port, raw, kLineBytes, now, offset);
        update::Crc32 line_crc;
        line_crc.add(raw, n);
        crc_.add(raw, n);
        Line l;
        l.str("SHOT D ");
        l.hex(offset, 6);
        l.chr(' ');
        char b64[4 * ((kLineBytes + 2) / 3)];
        const std::size_t chars = base64(raw, n, b64);
        for (std::size_t k = 0; k < chars; ++k) {
          l.chr(b64[k]);
        }
        l.chr(' ');
        l.hex(line_crc.value(), 8);
        l.chr('\n');
        out.text(l.text, l.len);
        ++lines_;
        st = reader_.state();
      }
      if (reader_.state() == BandReader::State::Done) {
        phase_ = Phase::End;
      }
      break;
    }
    case Phase::End: {
      if (out.room() < kHeadroom) {
        break;
      }
      Line l;
      l.str("SHOT END lines=");
      l.dec(lines_);
      l.str(" crc=");
      l.hex(crc_.value(), 8);
      l.str(" moved=");
      l.chr(port.frames() != frames_at_start_ ? '1' : '0');
      l.chr('\n');
      out.text(l.text, l.len);
      phase_ = Phase::Idle;
      return false;
    }
    case Phase::Idle:
      return false;
  }
  if (phase_ != Phase::Idle && reader_.stalled(now)) {
    fail(out, "stalled: the host does not read or the bus is never free");
    return false;
  }
  return true;
}

}  // namespace o3sdk::platform::mk66::console
