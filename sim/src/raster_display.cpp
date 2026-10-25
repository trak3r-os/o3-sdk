// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/sim/raster_display.h>

#include <o3sdk/ui/assets.h>

#include <algorithm>

namespace o3sdk::sim {

hal::UploadResult upload_chunked(hal::Display& display, std::uint32_t addr, const std::uint8_t* bytes,
                                 std::size_t count) {
  static_assert(hal::kMaxUploadBytes % hal::kUploadAlign == 0, "a chunk keeps the word alignment");
  hal::UploadResult result = hal::UploadResult::Ok;
  for (std::size_t offset = 0; offset < count && result == hal::UploadResult::Ok; offset += hal::kMaxUploadBytes) {
    result = display.upload(addr + static_cast<std::uint32_t>(offset), bytes + offset,
                            std::min<std::size_t>(hal::kMaxUploadBytes, count - offset));
  }
  return result;
}

hal::UploadResult load_assets(hal::Display& display) { return ui::upload_assets(display); }

void RasterDisplay::end_frame() noexcept {
  const std::size_t before = sink_.frames();
  sink_.end_frame();
  if (sink_.frames() == before) {
    return;  // no frame was open
  }
  const std::vector<std::uint32_t>& words = sink_.last_frame();
  peak_words_ = std::max(peak_words_, words.size());
  if (raster_.render(words.data(), words.size())) {
    ++frames_;
  } else {
    ++failures_;
    if (error_.empty()) {
      error_ = raster_.error();
    }
  }
}

}  // namespace o3sdk::sim
