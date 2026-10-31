// SPDX-License-Identifier: GPL-3.0-or-later
// EveRaster::last_frame_digest(): the FNV-1a of the display-list words of the last frame drawn (sim/include/o3sdk/sim/eve_raster.h). The frames of
// the apps are pinned with it (tools/digest_check.py, tests/digests/); this is the digest's own test.
#include <doctest/doctest.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/sim/eve_raster.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dl = o3sdk::eve::dl;
using o3sdk::eve::Primitive;
using o3sdk::sim::EveRaster;

namespace {

using Words = std::vector<std::uint32_t>;

// FNV-1a 32 bit, written out byte by byte: each word as four bytes, least significant first (the order they go to the chip).
std::uint32_t reference(const Words& words) {
  std::uint32_t hash = 2166136261u;
  for (const std::uint32_t w : words) {
    const std::uint8_t ordered[4] = {static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8), static_cast<std::uint8_t>(w >> 16),
                                     static_cast<std::uint8_t>(w >> 24)};
    for (const std::uint8_t b : ordered) {
      hash = (hash ^ b) * 16777619u;
    }
  }
  return hash;
}

bool render(EveRaster& r, const Words& w) { return r.render(w.data(), w.size()); }

}  // namespace

TEST_CASE("digest: the FNV-1a of a known list, from an independent computation") {
  CHECK(EveRaster::digest_words(nullptr, 0) == 0x811C9DC5u);  // the offset basis: the digest of nothing
  CHECK(EveRaster::kDigestBasis == 0x811C9DC5u);
  // CLEAR_COLOR_RGB(10, 20, 30), CLEAR(1, 1, 1), DISPLAY: 0x020A141E, 0x26000007, 0x00000000 (computed with a script, not with this code).
  const Words known = {dl::clear_color_rgb(10, 20, 30), dl::clear(true, true, true), dl::display()};
  REQUIRE(known[0] == 0x020A141Eu);
  REQUIRE(known[1] == 0x26000007u);
  REQUIRE(known[2] == 0u);
  CHECK(EveRaster::digest_words(known.data(), known.size()) == 0xA39B1152u);
  CHECK(EveRaster::digest_words(known.data(), known.size()) == reference(known));
  const std::uint32_t one = 0x00000061u;  // the bytes 61 00 00 00, least significant first
  CHECK(EveRaster::digest_words(&one, 1) == 0xF5E1D3E4u);
}

TEST_CASE("digest: a list one word different, or in another order, has another digest") {
  const Words base = {dl::clear_color_rgb(10, 20, 30), dl::clear(true, true, true), dl::display()};
  const std::uint32_t d = EveRaster::digest_words(base.data(), base.size());
  Words one_bit = base;
  one_bit[1] ^= 1u;
  CHECK(EveRaster::digest_words(one_bit.data(), one_bit.size()) == 0xE65F5167u);
  CHECK(EveRaster::digest_words(one_bit.data(), one_bit.size()) != d);
  const Words swapped = {base[1], base[0], base[2]};
  CHECK(EveRaster::digest_words(swapped.data(), swapped.size()) == 0xD6D60FF6u);
  Words longer = base;
  longer.push_back(0);
  CHECK(EveRaster::digest_words(longer.data(), longer.size()) != d);
  CHECK(EveRaster::digest_words(base.data(), 2) != d);
}

TEST_CASE("digest: the raster digests the list it drew, and a list it refused leaves it alone") {
  EveRaster r;
  CHECK(r.last_frame_digest() == EveRaster::kDigestBasis);  // before the first frame
  const Words a = {dl::clear_color_rgb(10, 20, 30), dl::clear(true, true, true), dl::display()};
  REQUIRE(render(r, a));
  CHECK(r.last_frame_digest() == 0xA39B1152u);
  // The same words give the same digest, whatever the picture under them or the frames before.
  const Words b = {dl::color_rgb(255, 0, 0), dl::begin(Primitive::Rects), dl::vertex2f(0, 0), dl::vertex2f(160, 160), dl::end(), dl::display()};
  REQUIRE(render(r, b));
  CHECK(r.last_frame_digest() == reference(b));
  CHECK(r.last_frame_digest() != 0xA39B1152u);
  REQUIRE(render(r, a));
  CHECK(r.last_frame_digest() == 0xA39B1152u);
  // A list without DISPLAY, or with a word outside the subset, is refused: the visible frame and its digest stay.
  const Words no_display = {dl::color_rgb(1, 2, 3)};
  CHECK_FALSE(render(r, no_display));
  CHECK(r.last_frame_digest() == 0xA39B1152u);
  const Words bad = {0xC0000000u, dl::display()};
  CHECK_FALSE(render(r, bad));
  CHECK(r.last_frame_digest() == 0xA39B1152u);
  CHECK(r.frames() == 3u);
}
