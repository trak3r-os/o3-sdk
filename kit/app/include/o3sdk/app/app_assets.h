// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The app's bitmaps in the display's RAM_G: a table of rows the app hands to the runtime (App::assets()), which puts them in the app's own
// region (ui/plot/ram_g_map.h, kRamGApp) at boot, in Runtime::start(), straight from flash and in chunks of hal::kMaxUploadBytes. The kit
// never reads the region and bitmap handle 0 draws from it (ui/plot/canvas.h). Context::app_assets_ok() says whether they landed.
//
//   alignas(4) constexpr std::uint8_t kTexture[] = {...};
//   constexpr o3sdk::app::AppAsset kRows[] = {{0, kTexture, sizeof kTexture}};
//   static_assert(o3sdk::app::app_assets_valid(kRows, 1), "the table is inside the app's region, aligned and disjoint");
//
// There is no retry and no re-upload (the kit's own faces have neither): the first chunk the display does not take stops the upload, and a
// display re-initialisation loses the region as it loses the faces.

#include <o3sdk/hal/display.h>
#include <o3sdk/ui/plot/ram_g_map.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

struct AppAsset {
  std::uint32_t offset;       // from ui::plot::kRamGApp; a multiple of hal::kUploadAlign
  const std::uint8_t* bytes;  // alignas(4), in flash
  std::uint32_t size;         // > 0, a multiple of hal::kUploadAlign
};

// The table's offsets and sizes: each row inside [0, kRamGAppBytes), aligned, bytes not null, size above zero, no two rows overlapping. An
// empty table (count 0, any pointer) is valid. Constant-evaluable: an app static_asserts its table with it. (The alignment of a row's
// `bytes` is an address, known at run time: app_assets_aligned().)
constexpr bool app_assets_valid(const AppAsset* rows, std::size_t count) noexcept {
  if (count != 0 && rows == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    const AppAsset& a = rows[i];
    if (a.bytes == nullptr || a.size == 0 || a.offset % hal::kUploadAlign != 0 || a.size % hal::kUploadAlign != 0) {
      return false;
    }
    if (std::uint64_t{a.offset} + a.size > ui::plot::kRamGAppBytes) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      const AppAsset& b = rows[j];
      if (std::uint64_t{a.offset} < std::uint64_t{b.offset} + b.size && std::uint64_t{b.offset} < std::uint64_t{a.offset} + a.size) {
        return false;
      }
    }
  }
  return true;
}

// Every row's `bytes` on a word (the display contract's source alignment, hal/display.h). A run-time check: a misaligned table is refused.
bool app_assets_aligned(const AppAsset* rows, std::size_t count) noexcept;

enum class AppAssetsResult : std::uint8_t {
  None,     // an empty table: nothing to do
  Refused,  // the table fails app_assets_valid() or app_assets_aligned(): nothing was uploaded (Runtime::start() aborts on it but in a MK66 release build)
  Failed,   // a chunk was not taken (the first that was not Ok stops the upload; what went before is in RAM_G)
  Ok,       // every byte of every row is in RAM_G
};

// Puts the table in RAM_G: each row in chunks of hal::kMaxUploadBytes to kRamGApp + offset + k * kMaxUploadBytes. Every chunk starts on a
// word of an aligned array, so the source and the address keep the alignment the contract asks for. Main loop only. Runtime::start() calls it.
AppAssetsResult upload_app_assets(hal::Display& display, const AppAsset* rows, std::size_t count) noexcept;

}  // namespace o3sdk::app
