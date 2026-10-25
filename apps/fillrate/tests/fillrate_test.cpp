// SPDX-License-Identifier: GPL-3.0-or-later
// FILLRATE: the band's arithmetic, its bitmaps, and the app in the simulator (every kind, K, cell width and ballast builds a frame that
// fits the display list). The simulator does not model the scan-line limit; that is what the unit is for (apps/fillrate/README.md).
#include <doctest/doctest.h>

#include "band.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/ui/components.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace o3sdk;
using namespace fillrate;

namespace {

// A renderer on a fake display, a frame begun: what the band's words cost in a real frame.
struct Frame {
  platform::host::FakeDisplay display;
  eve::DisplayListBuffer buffer;
  eve::DisplayListBuilder builder{display, buffer.words, eve::kMaxDisplayListWords};
  ui::Renderer r{builder, ui::theme(ui::ThemeId::Amiga)};
  Frame() { REQUIRE(r.begin(ui::Color::Pane)); }
};

struct RawSink {
  ui::Renderer& r;
  void operator()(std::uint32_t word) noexcept { r.raw(word); }
};

void tap(sim::Harness& h, app::Button b) {
  h.press(b);
  h.run_ms(1);
  h.release(b);
  h.run_ms(1);
}

}  // namespace

TEST_CASE("band: the layers of each kind and the pixels a line carries") {
  for (unsigned k = 0; k <= kMaxLayers; ++k) {
    CHECK(layers_for(Kind::L8Nearest, k).l8n == k);
    CHECK(layers_for(Kind::L8Bilinear, k).l8b == k);
    CHECK(layers_for(Kind::L4, k).l4 == k);
    CHECK(layers_for(Kind::Rect, k).rect == k);
    for (unsigned i = 0; i < kKindCount; ++i) {
      CHECK(layers_for(static_cast<Kind>(i), k).total() == k);
      CHECK(px_per_line(layers_for(static_cast<Kind>(i), k)) == k * 800u);
    }
    const Layers m = layers_for(Kind::Mix, k);  // bitmaps and rectangles alternate, the bitmaps rotate through the three kinds
    CHECK(m.rect == k / 2u);
    CHECK(m.bitmaps() == (k + 1u) / 2u);
    CHECK(m.l8n >= m.l4);
    CHECK(m.l4 >= m.l8b);
    CHECK(m.l8n - m.l8b <= 1u);
  }
  // The skin's busiest line is 2,034 bitmap and 2,174 primitive pixels: K = 5 and K = 6 of the mix bracket it.
  const Layers five = layers_for(Kind::Mix, 5);
  CHECK(bitmap_px(five) == 2400u);
  CHECK(fill_px(five) == 1600u);
  CHECK(bitmap_px(five) + fill_px(five) == px_per_line(five));
  CHECK(bitmap_px(layers_for(Kind::Mix, 6)) == 2400u);
  CHECK(fill_px(layers_for(Kind::Mix, 6)) == 2400u);
  CHECK(sprites_per_line(five, 800) == 5u);
  CHECK(sprites_per_line(five, 50) == 80u);
}

TEST_CASE("band: the words counted are the words drawn") {
  for (unsigned i = 0; i < kKindCount; ++i) {
    for (const unsigned cell : kCellWidths) {
      unsigned last = 0;
      for (unsigned k = 0; k <= kMaxLayers; k += (k < 4 ? 1u : 7u)) {
        const Layers n = layers_for(static_cast<Kind>(i), k);
        const unsigned words = band_words(n, cell);
        CHECK(words >= last);  // more layers, never fewer words
        last = words;
        Frame f;
        const std::size_t before = f.r.words();
        if (n.total() != 0u) {
          f.r.begin_raw();
          RawSink sink{f.r};
          compose(sink, n, cell);
          f.r.end_raw();
        }
        CHECK(f.r.ok());
        const std::size_t drawn = f.r.words() - before;
        CHECK(drawn <= words);       // band_words is the section's words plus the worst overhead, kRawOverheadWords
        CHECK(drawn + 2u >= words);  // SAVE_CONTEXT and RESTORE_CONTEXT are the overhead here: two of the four
      }
    }
  }
}

TEST_CASE("band: fit_layers clamps to the words it is given") {
  for (unsigned i = 0; i < kKindCount; ++i) {
    const Kind kind = static_cast<Kind>(i);
    for (const unsigned cell : kCellWidths) {
      for (unsigned k = 0; k <= kMaxLayers; ++k) {
        const unsigned need = band_words(layers_for(kind, k), cell);
        CHECK(fit_layers(kind, k, cell, need) == k);
        CHECK(fit_layers(kind, k, cell, 100000) == k);
        if (k > 0u) {
          const unsigned fewer = fit_layers(kind, k, cell, need - 1u);
          CHECK(fewer < k);
          CHECK(band_words(layers_for(kind, fewer), cell) <= need - 1u);
        }
      }
      CHECK(fit_layers(kind, kMaxLayers, cell, 0) == 0u);
    }
  }
  // Even the largest band (RECT, 32 layers of 16 cells) leaves room for the reserves below it in an empty frame.
  CHECK(band_words(layers_for(Kind::Rect, kMaxLayers), 50) + kKeysRowWords + kTailWords < kWordsLimit);
}

TEST_CASE("band: the test bitmaps") {
  std::vector<std::uint8_t> all(kBitmapBytes);
  generate(all.data(), all.size(), 0);
  // Chunked as the app uploads them (4 KB, the last one short), the stream is the same.
  std::vector<std::uint8_t> chunked(kBitmapBytes);
  for (std::uint32_t off = 0; off < kBitmapBytes; off += hal::kMaxUploadBytes) {
    const std::size_t n = kBitmapBytes - off < hal::kMaxUploadBytes ? kBitmapBytes - off : hal::kMaxUploadBytes;
    generate(chunked.data() + off, n, off);
  }
  CHECK(all == chunked);
  // The L8 rows are bytes, the L4 rows nibbles, left pixel high.
  unsigned l8_min = 255, l8_max = 0, l4_max = 0;
  for (unsigned y = 0; y < kSourceRows; ++y) {
    for (unsigned x = 0; x < kBandW; ++x) {
      const unsigned v = all[y * kL8Stride + x];
      CHECK(v == l8_pixel(x, y));
      l8_min = v < l8_min ? v : l8_min;
      l8_max = v > l8_max ? v : l8_max;
      const std::uint8_t b = all[kL8Bytes + y * kL4Stride + x / 2];
      const unsigned nib = x % 2u == 0u ? b >> 4 : b & 0xFu;
      CHECK(nib == l4_pixel(x, y));
      l4_max = nib > l4_max ? nib : l4_max;
    }
  }
  CHECK(l8_min == 0u);
  CHECK(l8_max == 255u);
  CHECK(l4_max == 15u);
  // A row differs from the next one: a repeated line would show.
  CHECK(std::memcmp(&all[0], &all[kL8Stride], kL8Stride) != 0);
  CHECK(std::memcmp(&all[kL8Bytes], &all[kL8Bytes + kL4Stride], kL4Stride) != 0);
}

TEST_CASE("band: the soft-key row costs what the screen assumes") {
  // The runtime draws the row after the screen's last text (a bitmap run in TextDim): the same calls, from that state.
  Frame f;
  f.r.text(8, 356, "X", ui::Color::TextDim, ui::Face::Data);
  const std::size_t before = f.r.words();
  const ui::Rect area{0, ui::kScreenH - app::kKeysH, ui::kScreenW, app::kKeysH};
  ui::strip(f.r, area, ui::Edge::Top);
  ui::soft_keys(f.r, area, kSoftKeyLabels, app::kFKeyCount, 0, 1u << 3);
  CHECK(f.r.words() - before == kKeysRowWords);
}

TEST_CASE("sim: FILLRATE uploads its bitmaps and its heaviest states fit the list") {
  auto h = std::make_unique<sim::Harness>(o3sdk_app());
  REQUIRE(h->start());
  h->run_ms(900);  // two chunks a frame: the 29 chunks are in after about 15 frames

  // The test bitmaps are in RAM_G where ram_g_map.h put them.
  std::vector<std::uint8_t> expect(kBitmapBytes);
  generate(expect.data(), expect.size(), 0);
  CHECK(std::memcmp(h->raster().ram_g() + ui::plot::kRamGApp, expect.data(), expect.size()) == 0);
  // ... and nothing else of the map was touched: the region ends below the self-test's scratch and the screenshot's bands.
  CHECK(ui::plot::kRamGApp + kBitmapBytes <= hal::kRamGBytes - ui::plot::kRamGSelfTestBytes);
  CHECK(ui::plot::kRamGShot + ui::plot::kRamGShotBytes == ui::plot::kRamGApp);

  for (int i = 0; i < 5; ++i) {  // ballast to its largest
    tap(*h, app::Button::F7);
  }
  for (unsigned kind = 0; kind < kKindCount; ++kind) {
    tap(*h, app::fkey(kind));
    h->turn(100);  // K = 32 (the encoder clamps)
    h->run_ms(1);
    for (unsigned cell = 0; cell < kCellWidthCount; ++cell) {
      app::Runtime& rt = h->runtime();
      CHECK(rt.draw());  // a frame of the new state, now (the raster is the slow part of this test)
      CHECK(rt.frame_failures() == 0u);
      CHECK(h->display().render_failures() == 0u);
      CHECK(rt.last_frame_words() <= kWordsLimit);
      CHECK(h->display().last_frame_words() == rt.last_frame_words() + kListClosingWords);  // what RAM_DL holds
      CHECK(h->display().last_frame_words() <= eve::kMaxDisplayListWords);
      tap(*h, app::Button::F6);  // the next cell width (the fifth wraps to 800)
    }
    h->turn(-100);  // K = 0 for the next kind's start
    h->run_ms(1);
  }
  CHECK(h->display().peak_frame_words() <= eve::kMaxDisplayListWords);
  CHECK(h->display().first_error().empty());
}
