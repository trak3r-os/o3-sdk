// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/app_assets.h>

namespace o3sdk::app {

bool app_assets_aligned(const AppAsset* rows, std::size_t count) noexcept {
  for (std::size_t i = 0; rows != nullptr && i < count; ++i) {
    if (reinterpret_cast<std::uintptr_t>(rows[i].bytes) % hal::kUploadAlign != 0) {
      return false;
    }
  }
  return true;
}

AppAssetsResult upload_app_assets(hal::Display& display, const AppAsset* rows, std::size_t count) noexcept {
  static_assert(hal::kMaxUploadBytes % hal::kUploadAlign == 0, "a chunk keeps the word alignment");
  if (count == 0) {
    return AppAssetsResult::None;
  }
  if (!app_assets_valid(rows, count) || !app_assets_aligned(rows, count)) {
    return AppAssetsResult::Refused;
  }
  for (std::size_t i = 0; i < count; ++i) {
    const AppAsset& a = rows[i];
    for (std::uint32_t done = 0; done < a.size; done += static_cast<std::uint32_t>(hal::kMaxUploadBytes)) {
      const std::uint32_t left = a.size - done;
      const std::size_t n = left < hal::kMaxUploadBytes ? left : hal::kMaxUploadBytes;
      if (display.upload(ui::plot::kRamGApp + a.offset + done, a.bytes + done, n) != hal::UploadResult::Ok) {
        return AppAssetsResult::Failed;
      }
    }
  }
  return AppAssetsResult::Ok;
}

}  // namespace o3sdk::app
