// SPDX-License-Identifier: GPL-3.0-or-later
//
// hal::Display on the FT812. The scheme and the buffers are described in display.h.
#include <o3sdk/platform/mk66/display/display.h>

#include <cstring>

namespace o3sdk::platform::mk66::display {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "display-list words are copied as they are: the FT812 reads them little-endian, like this core");

namespace {
using namespace ft812;

std::uint32_t load_word(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
void store_word(std::uint8_t* p, std::uint32_t v) noexcept { std::memcpy(p, &v, sizeof v); }

// The 3-byte memory-write header of `addr` into a staging buffer at its header offset.
void put_header(std::uint8_t* buffer, std::uint32_t addr) noexcept {
  std::uint8_t h[kWriteHeaderBytes];
  write_header(addr, h);
  std::memcpy(buffer + Ft812Display::kHeaderOffset, h, sizeof h);
}
}  // namespace

Ft812Display::Ft812Display(Chip& chip, BulkEngine& bulk) noexcept : chip_(chip), bulk_(bulk) {}

void Ft812Display::start(bool use_dma, std::uint8_t backlight_target, const InitConfig& config) noexcept {
  config_ = config;
  // ADR 0011 lesson 1: alignment is declared (alignas on the buffers, a static_assert on the class) and, since a placement
  // that broke it would fault the DMA's word reads or the payload copies, checked once more where the objects exist.
  if (reinterpret_cast<std::uintptr_t>(frame_) % 8u != 0u || reinterpret_cast<std::uintptr_t>(upload_) % 8u != 0u) {
    ready_ = false;
    return;
  }
  ready_ = true;
  failed_ = false;
  assets_stale_ = false;
  stalls_in_a_row_ = 0;
  use_polled_ = !use_dma;
  target_ = backlight_target > kPwmDutyMax ? kPwmDutyMax : backlight_target;
  duty_ = 0;
  light_ = target_ == 0 ? Light::On : Light::Waiting;
  ready_ms_ = chip_.link().now_ms();
  last_swap_poll_ms_ = ready_ms_;
}

// ---- Transfers --------------------------------------------------------------------------------------------------------

void Ft812Display::send_polled() noexcept {
  Link& link = chip_.link();
  link.select();
  for (std::size_t i = 0; i < len_; ++i) {
    link.xfer(src_[kHeaderOffset + i]);
  }
  link.deselect();
  ++stats_.polled_sends;
}

void Ft812Display::finish_send() noexcept {
  if (kind_ == Kind::Frame) {
    chip_.swap_frame();  // the list is in RAM_DL: swap it in at the next frame start
    swap_pending_ = true;
    swap_started_ms_ = chip_.link().now_ms();
  }
  kind_ = Kind::None;
  state_ = State::Idle;
  src_ = nullptr;
}

void Ft812Display::begin_send(Kind kind, std::uint8_t* buffer, std::size_t len) noexcept {
  kind_ = kind;
  src_ = buffer;
  len_ = len;
  started_ms_ = chip_.link().now_ms();
  // Bounded to its own buffer: the engine is told how much of it is left behind the header (the frame buffer and the upload
  // buffer differ), so a transfer can never read past the buffer it comes from.
  const std::size_t capacity = (buffer == frame_ ? kFrameStageBytes : kUploadStageBytes) - kHeaderOffset;
  if (!use_polled_ && bulk_.start(buffer + kHeaderOffset, len, capacity)) {
    state_ = State::Sending;
    ++stats_.dma_sends;
    return;
  }
  send_polled();
  finish_send();
}

// The engine failed or timed out: the transfer goes again by the polled path, and every later one does too.
void Ft812Display::fall_back() noexcept {
  use_polled_ = true;
  ++stats_.bulk_failures;
  send_polled();
  finish_send();
}

void Ft812Display::pump_transfer() noexcept {
  if (state_ != State::Sending) {
    return;
  }
  switch (bulk_.poll()) {
    case BulkStatus::Busy:
      if (chip_.link().now_ms() - started_ms_ >= kBulkTimeoutMs) {
        bulk_.abort();
        ++stats_.bulk_timeouts;
        fall_back();
      }
      return;
    case BulkStatus::Done:
      finish_send();
      return;
    case BulkStatus::Failed:
    case BulkStatus::Idle:  // Sending, but the engine knows nothing of it: lost
      fall_back();
      return;
  }
}

// A committed frame waits for the wire; start it when the transfer in front of it is over.
void Ft812Display::kick() noexcept {
  if (frame_queued_ && state_ == State::Idle) {
    frame_queued_ = false;
    begin_send(Kind::Frame, frame_, kWriteHeaderBytes + 4u * words_);
  }
}

bool Ft812Display::wait_quiescent(std::uint32_t timeout_ms) noexcept {
  const std::uint32_t start = chip_.link().now_ms();
  for (;;) {
    pump_transfer();
    kick();
    if (state_ == State::Idle && !frame_queued_) {
      return true;
    }
    if (chip_.link().now_ms() - start >= timeout_ms) {
      return false;
    }
  }
}

// ---- Swap and backlight -----------------------------------------------------------------------------------------------

void Ft812Display::poll_swap() noexcept {
  if (!swap_pending_ || state_ == State::Sending || failed_) {
    return;
  }
  const std::uint32_t now = chip_.link().now_ms();
  if (now == last_swap_poll_ms_) {
    return;  // at most one register read a millisecond
  }
  last_swap_poll_ms_ = now;
  if (!chip_.dl_swap_pending()) {
    swap_pending_ = false;
    stalls_in_a_row_ = 0;
    ++stats_.swaps_done;
    if (light_ == Light::Waiting) {  // the first list the app drew is on screen: the backlight may come up
      light_ = Light::Ramping;
      light_step_ms_ = now - kBacklightStepMs;
    }
    return;
  }
  if (now - swap_started_ms_ >= kSwapTimeoutMs) {
    recover_stall();
  }
}

// The swap never happened: the scan-out is dead. Once: reset and bring the chip up again (its backlight is off after that, and
// its RAM_G is presumed lost: the owner of the bitmaps is told through the generation). A second stall in a row, or a re-init that
// fails: the display is given up.
void Ft812Display::recover_stall() noexcept {
  ++stats_.swap_stalls;
  swap_pending_ = false;
  frame_queued_ = false;
  open_ = false;
  if (stalls_in_a_row_ >= 1) {
    give_up();
    return;
  }
  ++stalls_in_a_row_;
  const InitReport report = chip_.init(config_);
  ++stats_.reinits;
  if (report.status != InitStatus::Ok) {
    give_up();
    return;
  }
  duty_ = 0;
  light_ = target_ == 0 ? Light::On : Light::Waiting;
  ready_ms_ = chip_.link().now_ms();
  last_swap_poll_ms_ = ready_ms_;
  assets_stale_ = true;
}

void Ft812Display::give_up() noexcept {
  failed_ = true;
  swap_pending_ = false;
  frame_queued_ = false;
  open_ = false;
  hold_dark();
}

void Ft812Display::hold_dark() noexcept {
  light_ = Light::Held;
  duty_ = 0;
  if (ready_ && state_ != State::Sending) {
    chip_.set_backlight(0);
  }
}

void Ft812Display::step_backlight() noexcept {
  if (light_ == Light::On || light_ == Light::Held || state_ == State::Sending) {
    return;  // (a polled register write must never meet a DMA transfer on the bus: the step waits for the next pass)
  }
  const std::uint32_t now = chip_.link().now_ms();
  if (light_ == Light::Waiting) {
    if (now - ready_ms_ < kBacklightFallbackMs) {
      return;
    }
    light_ = Light::Ramping;
    light_step_ms_ = now - kBacklightStepMs;
  }
  if (now - light_step_ms_ < kBacklightStepMs) {
    return;
  }
  light_step_ms_ = now;
  if (duty_ > target_) {  // a lower target (set_backlight_target): the same steps down
    duty_ = static_cast<std::uint8_t>(duty_ - target_ > kBacklightStep ? duty_ - kBacklightStep : target_);
  } else {
    const unsigned next = static_cast<unsigned>(duty_) + kBacklightStep;
    duty_ = static_cast<std::uint8_t>(next > target_ ? target_ : next);
  }
  chip_.set_backlight(duty_);
  if (duty_ == target_) {
    light_ = Light::On;
  }
}

void Ft812Display::set_backlight_target(std::uint8_t target) noexcept {
  const std::uint8_t t = target > kPwmDutyMax ? kPwmDutyMax : target;
  if (t == target_) {
    return;
  }
  target_ = t;
  if (light_ == Light::On && ready_) {  // up and shown: it goes to the new level in the ramp's steps (a Waiting or Ramping light is already on its way to target_)
    light_ = Light::Ramping;
    light_step_ms_ = chip_.link().now_ms() - kBacklightStepMs;
  }
}

void Ft812Display::service() noexcept {
  if (!ready_ || failed_) {
    return;
  }
  pump_transfer();
  kick();
  poll_swap();
  step_backlight();
}

// ---- The self-test of the DMA path ------------------------------------------------------------------------------------

bool Ft812Display::self_test() noexcept {
  if (!ready_ || use_polled_) {
    return false;
  }
  if (!wait_quiescent(kUploadWaitMs)) {
    return false;
  }
  // 64 bytes with every bit pattern that matters to a PUSHR byte write: all-zero, all-one, a lone top bit, 0x7F, then a mixed
  // ramp. A command half of PUSHR picking up junk (the open question of the byte-wide DMA write, README) would show here.
  std::uint8_t pattern[kSelfTestBytes];
  for (std::size_t i = 0; i < kSelfTestBytes; ++i) {
    pattern[i] = i < 4 ? static_cast<std::uint8_t>(i == 0 ? 0x00 : i == 1 ? 0xFF : i == 2 ? 0x80 : 0x7F)
                       : static_cast<std::uint8_t>((i * 37u + 0xA5u) ^ (i << 3));
  }
  put_header(upload_, kSelfTestAddr);
  std::memcpy(upload_ + kPayloadOffset, pattern, kSelfTestBytes);
  const std::uint32_t dma_before = stats_.dma_sends;
  begin_send(Kind::Upload, upload_, kWriteHeaderBytes + kSelfTestBytes);
  bool moved = wait_quiescent(kUploadWaitMs);
  if (state_ == State::Sending) {  // timed out with the engine still busy: stop it, send again the slow way
    bulk_.abort();
    ++stats_.bulk_timeouts;
    fall_back();
    moved = false;
  }
  const bool by_dma = stats_.dma_sends != dma_before;  // the engine refused to start: nothing was proven
  std::uint8_t back[kSelfTestBytes] = {};
  chip_.read_block(kSelfTestAddr, back, kSelfTestBytes);
  const bool same = std::memcmp(back, pattern, kSelfTestBytes) == 0;
  const bool bus_ok = !chip_.link().take_error();
  if (moved && by_dma && same && bus_ok && !use_polled_) {
    return true;
  }
  use_polled_ = true;
  return false;
}

// ---- hal::Display -----------------------------------------------------------------------------------------------------

bool Ft812Display::begin_frame() noexcept {
  open_ = false;  // a frame begun earlier and not committed is abandoned
  if (!ready_ || failed_) {
    ++stats_.begin_refused;
    return false;
  }
  service();
  if (failed_ || assets_stale_) {  // (service() may have just given the display up, or re-initialised it: its bitmaps are gone)
    ++stats_.begin_refused;
    return false;
  }
  // The frame buffer is free once its last list has gone out and the swap behind it has happened (the chip's list memory
  // is safe to write only then).
  if (frame_queued_ || (state_ == State::Sending && kind_ == Kind::Frame) || swap_pending_) {
    ++stats_.begin_refused;
    return false;
  }
  open_ = true;
  words_ = 0;
  return true;
}

bool Ft812Display::write(const std::uint32_t* words, std::size_t count) noexcept {
  if (!open_ || (words == nullptr && count != 0)) {
    return false;
  }
  if (count > kMaxFrameWords - words_) {
    return false;  // nothing of this call is kept (hal/display.h)
  }
  if (count != 0) {
    std::memcpy(frame_payload() + 4u * words_, words, 4u * count);
    words_ += count;
  }
  return true;
}

void Ft812Display::end_frame() noexcept {
  if (!open_) {
    return;
  }
  open_ = false;
  // A list the chip would run off the end of is never sent: it has to end in DISPLAY. The builder appends it; a list that
  // does not is terminated here if there is room, else dropped.
  bool terminated = false;
  if (words_ != 0) {
    terminated = load_word(frame_payload() + 4u * (words_ - 1u)) == kDlDisplay;
    if (!terminated && words_ < kMaxFrameWords) {
      store_word(frame_payload() + 4u * words_, kDlDisplay);
      ++words_;
      terminated = true;
    }
  }
  if (!terminated) {
    ++stats_.frames_aborted;
    return;
  }
  put_header(frame_, kRamDl);
  frame_queued_ = true;
  ++stats_.frames;
  kick();
}

void Ft812Display::abort_frame() noexcept {
  if (open_) {
    open_ = false;
    ++stats_.frames_aborted;
  }
}

hal::UploadResult Ft812Display::upload(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept {
  using hal::UploadResult;
  // The contract's order of refusals (hal/display.h): TooLarge, NoData, Misaligned, BadRange, Busy.
  if (count > hal::kMaxUploadBytes) {
    return UploadResult::TooLarge;
  }
  if (bytes == nullptr && count != 0) {
    return UploadResult::NoData;
  }
  if (addr % hal::kUploadAlign != 0 || reinterpret_cast<std::uintptr_t>(bytes) % hal::kUploadAlign != 0) {
    return UploadResult::Misaligned;
  }
  if (addr > hal::kRamGBytes || count > hal::kRamGBytes - addr) {
    return UploadResult::BadRange;
  }
  if (count == 0) {
    return UploadResult::Ok;
  }
  if (!ready_ || failed_ || !wait_quiescent(kUploadWaitMs)) {
    ++stats_.upload_busy;
    return UploadResult::Busy;
  }
  put_header(upload_, kRamG + addr);
  std::memcpy(upload_ + kPayloadOffset, bytes, count);
  begin_send(Kind::Upload, upload_, kWriteHeaderBytes + count);
  ++stats_.uploads;
  stats_.upload_bytes += static_cast<std::uint32_t>(count);
  return UploadResult::Ok;
}

}  // namespace o3sdk::platform::mk66::display
