// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/host/fakes.h>

#include <cstdint>
#include <utility>

namespace o3sdk::platform::host {

bool FakeDisplay::begin_frame() noexcept {
  in_frame_ = false;  // abandons an uncommitted frame, as the contract says
  if (busy_) {
    return false;
  }
  pending_.clear();
  in_frame_ = true;
  ++begun_;
  return true;
}

bool FakeDisplay::write(const std::uint32_t* words, std::size_t count) noexcept {
  if (!in_frame_ || count > capacity_ - pending_.size()) {
    return false;
  }
  pending_.insert(pending_.end(), words, words + count);
  return true;
}

void FakeDisplay::end_frame() noexcept {
  if (!in_frame_) {
    return;
  }
  committed_ = std::move(pending_);
  pending_.clear();
  in_frame_ = false;
  ++frames_;
}

void FakeDisplay::abort_frame() noexcept {
  if (!in_frame_) {
    return;
  }
  pending_.clear();
  in_frame_ = false;
  ++aborted_;
}

hal::UploadResult FakeDisplay::upload(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept {
  using hal::UploadResult;
  UploadResult result = UploadResult::Ok;
  if (count > hal::kMaxUploadBytes) {
    result = UploadResult::TooLarge;
  } else if (bytes == nullptr && count != 0) {
    result = UploadResult::NoData;
  } else if (addr % hal::kUploadAlign != 0 || reinterpret_cast<std::uintptr_t>(bytes) % hal::kUploadAlign != 0) {
    result = UploadResult::Misaligned;
  } else if (addr > hal::kRamGBytes || count > hal::kRamGBytes - addr) {
    result = UploadResult::BadRange;
  } else if (busy_) {
    result = UploadResult::Busy;
  }
  last_upload_ = result;
  if (result != UploadResult::Ok) {
    ++rejected_uploads_;
    return result;
  }
  ++accepted_uploads_;
  uploaded_bytes_ += count;
  if (count != 0 && record_cap_ != 0) {
    if (recorded_bytes_ <= record_cap_ && count <= record_cap_ - recorded_bytes_) {
      uploads_.push_back(Upload{addr, std::vector<std::uint8_t>(bytes, bytes + count), frames_});
      recorded_bytes_ += count;
    } else {
      ++unrecorded_uploads_;
    }
  }
  return result;
}

bool FakeInput::poll(core::InputEvent& out) noexcept {
  if (queue_.empty()) {
    return false;
  }
  out = queue_.front();
  queue_.pop_front();
  return true;
}

}  // namespace o3sdk::platform::host
