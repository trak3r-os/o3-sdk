// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/usb/screenshot_service.h>

#include <o3sdk/hal/save_guard.h>

#include <cstring>

namespace o3sdk::platform::mk66::console {
namespace {

namespace shot = hal::storage::shot;
namespace guard = hal::storage::save;
using hal::storage::Status;
using hal::storage::WriteStatus;

constexpr char kTempPath[] = "Screenshots/_o3save.tmp";

}  // namespace

shot::Refusal ScreenshotService::refusal_now() noexcept {
  if (host_->console_shot_active()) {
    return shot::Refusal::ConsoleShot;
  }
  return host_->card_refusal();
}

void ScreenshotService::refuse(shot::Refusal why) noexcept {
  ++refused_;
  phase_ = Phase::Idle;
  host_->notice(shot::refusal_text(why), true);
}

bool ScreenshotService::request(std::uint32_t now) noexcept {
  if (host_ == nullptr) {
    return false;  // (not attached: the card's write side is not built)
  }
  const shot::Refusal why = phase_ != Phase::Idle ? shot::Refusal::InProgress : refusal_now();
  if (why != shot::Refusal::None) {
    ++refused_;
    host_->notice(shot::refusal_text(why), true);  // (an InProgress refusal leaves the run that is going on as it is)
    return false;
  }
  since_ = now;
  phase_ = Phase::WaitKeys;
  return true;
}

void ScreenshotService::scan_names() noexcept {
  for (unsigned i = 0; i < kNamesPerPass && number_ == 0u && phase_ == Phase::Prepare; ++i) {
    if (scan_tried_ >= shot::kLastNumber) {
      refuse(shot::Refusal::FolderFull);
      return;
    }
    char name[shot::kNameCap];
    char path[guard::kPathCap];
    if (!shot::file_name(name, scan_n_) || !guard::join(path, sizeof path, guard::kScreenshotsDir, name)) {
      refuse(shot::Refusal::FolderFull);  // (cannot happen: scan_n_ stays in 1..9999)
      return;
    }
    const Status s = card_->exists(path, nullptr);
    if (s == Status::NotFound) {
      number_ = scan_n_;
    } else if (s == Status::Ok) {
      scan_n_ = scan_n_ == shot::kLastNumber ? shot::kFirstNumber : scan_n_ + 1u;
      cursor_ = scan_n_;  // (taken: the next search, after a refusal or a fault here, does not look at it again)
      ++scan_tried_;
    } else {
      refuse(s == Status::NoCard || s == Status::NotMounted ? shot::Refusal::NoCard : shot::Refusal::Busy);
    }
  }
}

bool ScreenshotService::step(std::uint32_t now, bool keys_down, ShotPort& port) noexcept {
  switch (phase_) {
    case Phase::Idle:
      return false;
    case Phase::WaitKeys:
      if (keys_down) {
        if (now - since_ > kKeysMaxMs) {
          phase_ = Phase::Idle;  // (the keys never came up: nothing was started, so nothing is said)
        }
        return false;
      }
      since_ = now;  // the release
      frames_ = port.frames();
      frame_seen_ = false;
      scan_n_ = cursor_;
      scan_tried_ = 0;
      number_ = 0;
      phase_ = Phase::Prepare;
      return false;
    case Phase::Prepare:
      if (keys_down) {  // pressed again before the picture: the picture waits for the next release
        phase_ = Phase::WaitKeys;
        since_ = now;
        return false;
      }
      scan_names();
      if (phase_ != Phase::Prepare) {
        return false;
      }
      if (!frame_seen_ && (port.frames() != frames_ || now - since_ >= kFrameWaitMs)) {
        frame_seen_ = true;
        seen_at_ = now;
      }
      if (number_ == 0u || !frame_seen_ || now - seen_at_ < kSettleMs) {
        return false;
      }
      return begin_capture(now, port);
    case Phase::Capture:
      return capture_pass(now, port);
  }
  return false;
}

void ScreenshotService::drop_file() noexcept {
  writer_->abandon();
  writer_->remove(kTempPath);
  writer_->end_save();
}

bool ScreenshotService::begin_capture(std::uint32_t now, ShotPort& port) noexcept {
  const shot::Refusal why = refusal_now();  // the card or a console `shot` may have changed since the press
  if (why != shot::Refusal::None) {
    refuse(why);
    return false;
  }
  writer_->begin_save();
  WriteStatus st = writer_->make_dir(guard::kScreenshotsDir);
  if (st == WriteStatus::Ok) {
    st = writer_->create(kTempPath);
    if (st == WriteStatus::Exists) {  // what an earlier capture left when the card went away
      writer_->remove(kTempPath);
      st = writer_->create(kTempPath);
    }
  }
  ShotParams params;  // RGB565, 800 wide, the whole screen: two bands of 240 rows in the region
  if (st != WriteStatus::Ok || !reader_.start(params, region_, region_bytes_)) {
    const bool busy = st == WriteStatus::Busy;
    drop_file();
    if (busy) {
      refuse(shot::Refusal::Busy);
    } else {
      ++failed_;
      phase_ = Phase::Idle;
      host_->notice(shot::kFailedText, true);
    }
    return false;
  }
  port.stop_clock(false);  // (the console's `shot ... pclk0` leaves its flag set; the screenshot takes the picture with the panel clock running)
  reader_.begin(port, now);
  first_ = true;
  written_ = 0;
  phase_ = Phase::Capture;
  return true;
}

void ScreenshotService::abort(ShotPort& port) noexcept {
  reader_.cancel(port);
  drop_file();
  ++failed_;
  phase_ = Phase::Idle;
  host_->notice(shot::kFailedText, true);
}

void ScreenshotService::cancel(ShotPort& port) noexcept {
  if (host_ == nullptr || phase_ == Phase::Idle) {
    return;
  }
  if (phase_ == Phase::Capture) {
    reader_.cancel(port);
    drop_file();
    ++failed_;
  }
  phase_ = Phase::Idle;
}

bool ScreenshotService::capture_pass(std::uint32_t now, ShotPort& port) noexcept {
  BandReader::State st = reader_.step(port, now);
  if (st == BandReader::State::Failed) {
    abort(port);
    return false;
  }
  if (st == BandReader::State::Reading && port.bus_free()) {
    const std::size_t head = first_ ? shot::kHeaderBytes : 0u;
    std::uint32_t offset = 0;
    const std::size_t n = reader_.read(port, buf_ + head, kBufferBytes - head, now, offset);
    // The pieces arrive in the order of the image, each right after the last: the file is the header and the image, byte for byte.
    if (n == 0u || offset + shot::kHeaderBytes != written_ + head) {
      abort(port);
      return false;
    }
    if (first_) {
      shot::bmp_header(buf_);
    }
    if (writer_->write(buf_, head + n) != WriteStatus::Ok) {
      abort(port);
      return false;
    }
    written_ += static_cast<std::uint32_t>(head + n);
    first_ = false;
    st = reader_.state();
  }
  if (st == BandReader::State::Done) {
    finish();
    return false;
  }
  if (reader_.stalled(now)) {
    abort(port);
    return false;
  }
  return true;
}

void ScreenshotService::finish() noexcept {
  char name[shot::kNameCap];
  char path[guard::kPathCap];
  const bool ok = written_ == shot::kFileBytes && shot::file_name(name, number_) && guard::join(path, sizeof path, guard::kScreenshotsDir, name) && writer_->finish() == WriteStatus::Ok &&
                  writer_->rename(kTempPath, path) == WriteStatus::Ok;
  phase_ = Phase::Idle;
  if (!ok) {
    drop_file();
    ++failed_;
    host_->notice(shot::kFailedText, true);
    return;
  }
  writer_->end_save();
  ++saved_;
  cursor_ = number_ == shot::kLastNumber ? shot::kFirstNumber : number_ + 1u;
  char text[48];
  std::size_t n = 0;
  for (const char* p = shot::kSavedPrefix; *p != '\0' && n + 1u < sizeof text; ++p) {
    text[n++] = *p;
  }
  for (const char* p = name; *p != '\0' && n + 1u < sizeof text; ++p) {
    text[n++] = static_cast<char>(*p >= 'a' && *p <= 'z' ? *p - 'a' + 'A' : *p);  // the strip is upper case: SHOT_0007.BMP
  }
  text[n] = '\0';
  host_->notice(text, false);
}

}  // namespace o3sdk::platform::mk66::console
