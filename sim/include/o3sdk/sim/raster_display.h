// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/hal/display.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/sim/eve_raster.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace o3sdk::sim {

static_assert(EveRaster::kRamGBytes == hal::kRamGBytes, "the simulated RAM_G is the HAL's RAM_G");

// Uploads `count` bytes in chunks of at most hal::kMaxUploadBytes, at word-aligned offsets, so each chunk keeps the
// alignment the contract asks for. Stops at the first chunk the display does not take and returns its result (the
// chunks before it stay uploaded: a chunked upload is not atomic). Ok for count 0.
hal::UploadResult upload_chunked(hal::Display& display, std::uint32_t addr, const std::uint8_t* bytes,
                                 std::size_t count);

// The platform's one job for the UI's bitmaps: puts the glyph faces, the icons and the generated tiles at their RAM_G
// addresses (ui/assets.h) with ui::upload_assets(), through hal::Display::upload() in chunks of at most 4 KB from
// word-aligned buffers, as a device does once at boot, so the simulator exercises the upload contract (and the
// alignment of the generated bitmaps) every time it starts. The result of the first chunk the display refused, Ok
// when everything is in place.
hal::UploadResult load_assets(hal::Display& display);

// hal::Display of the simulator: a committed frame is handed to the EveRaster, which draws it into the
// framebuffer. Like the FT812's RAM_DL the sink holds 2048 words by default (a smaller capacity is for tests):
// write() returns false for a list that does not fit and the caller (eve::DisplayListBuilder) then aborts the
// frame, so nothing is committed and the previous picture stays visible. A committed frame that the raster
// cannot execute is counted in render_failures() and its first error kept; the previous picture stays then too.
//
// upload() follows hal/display.h (the size cap, bounds and alignment are checked by the FakeDisplay inside, which
// answers with the contract's UploadResult) and writes into the raster's RAM_G at once. The visible picture is
// already rendered, so unlike the chip it does not change until the next committed frame. Uploads are counted, not
// kept, unless record_uploads() asks for them: a long run must not grow without bound.
class RasterDisplay final : public hal::Display {
 public:
  explicit RasterDisplay(EveRaster& raster,
                         std::size_t capacity_words = platform::host::FakeDisplay::kDefaultCapacityWords)
      : raster_(raster), sink_(capacity_words) {}

  bool begin_frame() noexcept override { return sink_.begin_frame(); }
  bool write(const std::uint32_t* words, std::size_t count) noexcept override { return sink_.write(words, count); }
  void end_frame() noexcept override;
  void abort_frame() noexcept override { sink_.abort_frame(); }
  hal::UploadResult upload(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept override {
    const hal::UploadResult result = sink_.upload(addr, bytes, count);
    // The sink has checked the bounds the raster checks again, so a taken upload always lands.
    return (result == hal::UploadResult::Ok && !raster_.write_ram_g(addr, bytes, count)) ? hal::UploadResult::BadRange
                                                                                         : result;
  }
  void record_uploads(std::size_t max_bytes) { sink_.record_uploads(max_bytes); }
  std::size_t accepted_uploads() const { return sink_.accepted_uploads(); }
  std::size_t uploaded_bytes() const { return sink_.uploaded_bytes(); }
  std::size_t rejected_uploads() const { return sink_.rejected_uploads(); }
  hal::UploadResult last_upload_result() const { return sink_.last_upload_result(); }

  std::size_t frames() const { return frames_; }               // frames drawn successfully
  std::size_t aborted() const { return sink_.aborted(); }      // frames abandoned with abort_frame()
  std::size_t render_failures() const { return failures_; }    // committed frames the raster refused
  const std::string& first_error() const { return error_; }
  std::size_t last_frame_words() const { return sink_.last_frame().size(); }  // incl. DISPLAY
  std::size_t peak_frame_words() const { return peak_words_; }
  const std::vector<platform::host::FakeDisplay::Upload>& uploads() const { return sink_.uploads(); }

 private:
  EveRaster& raster_;
  platform::host::FakeDisplay sink_;
  std::size_t frames_ = 0;
  std::size_t failures_ = 0;
  std::size_t peak_words_ = 0;
  std::string error_;
};

}  // namespace o3sdk::sim
