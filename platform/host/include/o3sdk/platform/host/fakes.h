// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Host fakes of the HAL contracts, for unit tests and (OS-001b) the simulator. They may use the standard
// library (heap included); nothing below the app layer includes this header.

#include <o3sdk/core/input_event.h>
#include <o3sdk/core/tick.h>
#include <o3sdk/hal/clock.h>
#include <o3sdk/hal/display.h>
#include <o3sdk/hal/input.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace o3sdk::platform::host {

// Records the words of every committed frame (aborted ones leave no trace); counts every RAM_G upload and, only if
// asked (record_uploads), keeps the accepted ones up to a byte cap; can be told to refuse frames or uploads, or to
// run out of room.
class FakeDisplay final : public hal::Display {
 public:
  // RAM_DL size in words (FT81x: 8 KiB). Kept literal so platform does not depend on eve.
  static constexpr std::size_t kDefaultCapacityWords = 2048;

  // One recorded upload(), in call order. `frames_before` is the number of frames committed before the call, so
  // the upload precedes frame number frames_before + 1 (hal/display.h: uploads and frames take effect in call order).
  struct Upload {
    std::uint32_t addr;
    std::vector<std::uint8_t> bytes;
    std::size_t frames_before;
  };

  explicit FakeDisplay(std::size_t capacity_words = kDefaultCapacityWords) : capacity_(capacity_words) {}

  bool begin_frame() noexcept override;
  bool write(const std::uint32_t* words, std::size_t count) noexcept override;
  void end_frame() noexcept override;
  void abort_frame() noexcept override;
  // The hal/display.h contract: an upload over kMaxUploadBytes, without data, misaligned, outside RAM_G or while
  // busy takes nothing and returns its own UploadResult (also kept in last_upload_result()).
  hal::UploadResult upload(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept override;

  // Test controls and observations.
  void set_busy(bool busy) { busy_ = busy; }                  // while busy, begin_frame() and upload() fail
  std::size_t frames() const { return frames_; }              // committed frames
  std::size_t aborted() const { return aborted_; }            // frames discarded with abort_frame()
  std::size_t begun() const { return begun_; }                // successful begin_frame() calls
  bool in_frame() const { return in_frame_; }
  const std::vector<std::uint32_t>& last_frame() const { return committed_; }

  // Uploads. Nothing is stored by default (a long run must not grow without bound); record_uploads(max_bytes) keeps
  // the accepted uploads, in order, while their bytes total at most `max_bytes` (later ones are counted, not kept;
  // count 0 is never kept). The counters below are always kept and are constant in size.
  void record_uploads(std::size_t max_bytes) { record_cap_ = max_bytes; }
  const std::vector<Upload>& uploads() const { return uploads_; }
  std::size_t accepted_uploads() const { return accepted_uploads_; }    // taken, including count 0
  std::size_t uploaded_bytes() const { return uploaded_bytes_; }        // their bytes
  std::size_t unrecorded_uploads() const { return unrecorded_uploads_; }  // taken but over the recording cap
  std::size_t rejected_uploads() const { return rejected_uploads_; }    // refused, whatever the reason
  hal::UploadResult last_upload_result() const { return last_upload_; }

 private:
  std::size_t capacity_;
  bool busy_ = false;
  bool in_frame_ = false;
  std::size_t begun_ = 0;
  std::size_t frames_ = 0;
  std::size_t aborted_ = 0;
  std::vector<std::uint32_t> pending_;
  std::vector<std::uint32_t> committed_;
  std::vector<Upload> uploads_;
  std::size_t record_cap_ = 0;
  std::size_t recorded_bytes_ = 0;
  std::size_t accepted_uploads_ = 0;
  std::size_t uploaded_bytes_ = 0;
  std::size_t unrecorded_uploads_ = 0;
  std::size_t rejected_uploads_ = 0;
  hal::UploadResult last_upload_ = hal::UploadResult::Ok;
};

// Replays scripted events in order.
class FakeInput final : public hal::Input {
 public:
  bool poll(core::InputEvent& out) noexcept override;
  void push(const core::InputEvent& event) { queue_.push_back(event); }
  std::size_t pending() const { return queue_.size(); }

 private:
  std::deque<core::InputEvent> queue_;
};

// Time only moves when the test says so. It keeps a 64-bit microsecond count: now() is its low 32 bits
// (so it wraps like a real tick) and uptime_ms() is the full count in milliseconds (it does not wrap).
class FakeClock final : public hal::Clock {
 public:
  core::Tick now() const noexcept override { return static_cast<core::Tick>(us_); }
  std::uint64_t uptime_ms() const noexcept override { return us_ / core::kTicksPerMs; }
  std::uint64_t uptime_us() const noexcept { return us_; }
  void set(core::Tick t) { us_ = t; }                          // restart the count at `t` microseconds
  void advance(std::uint32_t ticks) { us_ += ticks; }          // carries past 2^32, so now() wraps

 private:
  std::uint64_t us_ = 0;
};

}  // namespace o3sdk::platform::host
