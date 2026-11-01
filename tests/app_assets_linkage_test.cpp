// SPDX-License-Identifier: GPL-3.0-or-later
// An app's asset table as an app generates it (o3-303's skin_assets.h): `inline constexpr` in a named namespace, so the objects have vague
// (weak) linkage, unlike an anonymous-namespace table. GCC does not fold the address of such an object to non-null once
// -fdelete-null-pointer-checks is off (-fsanitize=null in the O3SDK_SANITIZE build; this file is compiled with -fno-delete-null-pointer-checks in
// every build, tests/CMakeLists.txt), and a `rows == nullptr` inside app_assets_valid() made the app's static_assert "not a constant expression".
// The checks are the static_asserts: the file has to compile.
#include <doctest/doctest.h>

#include <o3sdk/app/app_assets.h>

#include <cstdint>

namespace app_assets_shared_linkage {

using o3sdk::app::AppAsset;

alignas(4) inline constexpr std::uint8_t kBytes[16] = {};
inline constexpr AppAsset kRows[2] = {{0, kBytes, 16}, {16, kBytes, 16}};
static_assert(o3sdk::app::app_assets_valid(kRows, 2), "an app's table with vague linkage is constant-evaluable, with and without the sanitizer");
inline constexpr AppAsset kBad[1] = {{2, kBytes, 16}};
static_assert(!o3sdk::app::app_assets_valid(kBad, 1), "and an invalid one is still refused at compile time");
inline constexpr AppAsset kNullBytes[1] = {{0, nullptr, 16}};
static_assert(!o3sdk::app::app_assets_valid(kNullBytes, 1), "a row without bytes");
static_assert(!o3sdk::app::app_assets_valid(nullptr, 1), "a count without a table");
static_assert(o3sdk::app::app_assets_valid(nullptr, 0), "an empty table, whatever the pointer");

}  // namespace app_assets_shared_linkage

TEST_CASE("app assets linkage: an app's inline constexpr table is accepted by the constant checks") {
  CHECK(o3sdk::app::app_assets_valid(app_assets_shared_linkage::kRows, 2));
}
