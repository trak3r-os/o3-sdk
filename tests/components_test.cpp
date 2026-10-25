// SPDX-License-Identifier: GPL-3.0-or-later
//
// The components (ui/components.h): placement by capital height, the chrome strip, badges, the F-key key-caps, FieldRow
// and the bar and meter. Word counts and pixels are checked by hand-worked numbers for the Amiga theme (Data 10x20,
// Label 8x16, Display 16x32; pad 4, gap 4).
#include <doctest/doctest.h>

#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/eve/dl.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/sim/eve_raster.h>
#include <o3sdk/sim/raster_display.h>
#include <o3sdk/ui/components.h>
#include <o3sdk/ui/renderer.h>
#include <o3sdk/ui/theme.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace dl = o3sdk::eve::dl;
using namespace o3sdk;
using ui::Color;
using ui::Face;

namespace {

const ui::Theme& amiga() { return ui::theme(ui::ThemeId::Amiga); }

template <typename Fn>
std::vector<std::uint32_t> frame_of(Fn draw) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
  eve::DisplayListBuilder builder(sink, storage.data(), eve::kMaxDisplayListWords);
  ui::Renderer r(builder, amiga());
  CHECK(r.begin(Color::Pane));
  draw(r);
  CHECK(r.end());
  return sink.last_frame();
}

unsigned count_op(const std::vector<std::uint32_t>& words, std::uint32_t opcode) {
  unsigned n = 0;
  for (const std::uint32_t w : words) {
    n += (w >> 24) == opcode ? 1u : 0u;
  }
  return n;
}

struct Panel {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  Panel() { CHECK(sim::load_assets(display) == hal::UploadResult::Ok); }
  template <typename Fn>
  void draw(Fn fn) {
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(display, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, amiga());
    CHECK(r.begin(Color::Pane));
    fn(r);
    CHECK(r.end());
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
};
std::uint32_t rgb_of(Color c) {
  const ui::Rgb& v = amiga().rgb(c);
  return (std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b;
}

}  // namespace

TEST_CASE("components: capitals are centred by their height, baselines by the ascent") {
  const ui::Theme& t = amiga();
  // Data (cap 13, ascent 16) in a 24 px row: the 13 capital rows start at (24 - 13) / 2 = 5, so the cell sits 5 - 3 = 2 down.
  CHECK(ui::caps_y(t, Face::Data, 0, 24) == 2);
  CHECK(ui::caps_y(t, Face::Data, 100, 24) == 102);
  // Label (cap 10, ascent 12) in a 14 px badge: capitals at 2, cell at 0.
  CHECK(ui::caps_y(t, Face::Label, 50, 14) == 50);
  // Display (cap 20, ascent 26) in the 32 px strip: capitals at rows 6..25, the cell at 0.
  CHECK(ui::caps_y(t, Face::Display, 0, 32) == 0);
  CHECK(ui::baseline_y(t, Face::Display, 26) == 0);
  CHECK(ui::baseline_y(t, Face::Data, 26) == 10);
  CHECK(ui::baseline_y(t, Face::Label, 26) == 14);
}

TEST_CASE("components: a strip is the chrome fill with a ChromeDim rule on the edge facing the pane") {
  Panel p;
  p.draw([](ui::Renderer& r) {
    ui::strip(r, {0, 0, 100, 20}, ui::Edge::Bottom);
    ui::strip(r, {0, 100, 100, 20}, ui::Edge::Top);
    ui::strip(r, {0, 200, 100, 20}, ui::Edge::None);
  });
  CHECK(p.pixel(50, 5) == rgb_of(Color::Chrome));
  CHECK(p.pixel(50, 19) == rgb_of(Color::ChromeDim));  // bottom row
  CHECK(p.pixel(50, 18) == rgb_of(Color::Chrome));
  CHECK(p.pixel(50, 100) == rgb_of(Color::ChromeDim));  // top row
  CHECK(p.pixel(50, 101) == rgb_of(Color::Chrome));
  CHECK((p.pixel(50, 200) == rgb_of(Color::Chrome) && p.pixel(50, 219) == rgb_of(Color::Chrome)));
  CHECK(p.pixel(100, 5) == rgb_of(Color::Pane));  // and nothing beyond the rectangle
}

TEST_CASE("components: the transport badge is green for PLAY and the grey of ChromeDim for STOP, with its icon") {
  Panel p;
  int play_w = 0, stop_w = 0;
  p.draw([&](ui::Renderer& r) {
    play_w = ui::transport_badge(r, 10, 10, 22, true);
    stop_w = ui::transport_badge(r, 200, 10, 22, false);
  });
  // pad + 16 px icon + pad + 4 characters of 8 px + pad.
  CHECK(play_w == 4 + 16 + 4 + 32 + 4);
  CHECK(stop_w == play_w);
  CHECK(p.pixel(10, 10) == rgb_of(Color::ChromeDim));          // the PLAY lamp has a one-pixel frame...
  CHECK(p.pixel(11, 11) == rgb_of(Color::Play));               // ...and the green inside it
  CHECK(p.pixel(10 + play_w - 2, 30) == rgb_of(Color::Play));
  CHECK(p.pixel(10 + play_w - 1, 31) == rgb_of(Color::ChromeDim));  // its last pixel is the frame's
  CHECK(p.pixel(10 + play_w, 31) == rgb_of(Color::Pane));           // and none beyond
  CHECK(p.pixel(200, 10) == rgb_of(Color::ChromeDim));
  CHECK(p.pixel(201, 11) == rgb_of(Color::ChromeDim));
  // The icon is ink in the badge's text colour: some pixels of the 16 x 16 cell are PlayText, not all.
  int ink = 0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      ink += p.pixel(10 + 4 + x, 10 + 3 + y) == rgb_of(Color::PlayText) ? 1 : 0;
    }
  }
  CHECK((ink > 40 && ink < 200));
}

TEST_CASE("components: a type badge is an inverted square with its letter") {
  Panel p;
  p.draw([](ui::Renderer& r) { ui::type_badge(r, 100, 100, 14, 'M'); });
  CHECK(p.pixel(100, 100) == rgb_of(Color::ChromeText));
  CHECK(p.pixel(113, 113) == rgb_of(Color::ChromeText));
  CHECK(p.pixel(114, 100) == rgb_of(Color::Pane));
  int letter = 0;
  for (int y = 100; y < 114; ++y) {
    for (int x = 100; x < 114; ++x) {
      letter += p.pixel(x, y) == rgb_of(Color::Chrome) ? 1 : 0;
    }
  }
  CHECK((letter > 15 && letter < 60));
}

TEST_CASE("components: eight key-caps are three colour passes: the boxes, the F-numbers, the labels") {
  static const char* const labels[8] = {"PATTERN", "LENGTH", "JUMP", "FILL", "PREVIEW", "INVERT", "EXPORT", "UNDO"};
  const auto f = frame_of([](ui::Renderer& r) { ui::soft_keys(r, {0, 458, 800, 22}, labels, 8); });
  // Boxes: ChromeText; "F1".."F8": Chrome; labels: ChromeText again. Three COLOR_RGB words in all.
  CHECK(count_op(f, 0x04) == 3u);
  // One word for each of the 8 boxes' two corners, and a glyph word per character: 8 x 2 + the label letters.
  unsigned letters = 0;
  for (const char* l : labels) {
    for (const char* c = l; *c != '\0'; ++c) {
      ++letters;
    }
  }
  CHECK(letters == 7u + 6u + 4u + 4u + 7u + 6u + 6u + 4u);
  // The preamble (35), then: pass 1 = COLOR + BEGIN(RECTS) + 16 corners; pass 2 = COLOR + END + BEGIN(BITMAPS) + 16 glyphs;
  // pass 3 = COLOR + the label letters; END, DISPLAY (that is 40 + letters), and three VERTEX_TRANSLATE_X words: one when
  // the F-numbers pass the 9-bit window (F7), two in the labels (back to 0 at the start, then past it at F6).
  CHECK(f.size() == 35u + 40u + letters + 3u);
  CHECK(count_op(f, 0x2B) == 3u);
}

TEST_CASE("components: key-caps are laid out one per eighth of the row, label after the F-number box") {
  static const char* const labels[8] = {"PATTERN", "LENGTH", "JUMP", "FILL", "PREVIEW", "INVERT", "EXPORT", "UNDO"};
  Panel p;
  p.draw([](ui::Renderer& r) { ui::soft_keys(r, {0, 458, 800, 22}, labels, 8); });
  // Box 3 (index 2): x = 2 * 100 + pad, 24 wide (two 8 px cells and a pad each side), 18 high, centred in 22: y = 460.
  CHECK(p.pixel(204, 460) == rgb_of(Color::ChromeText));
  CHECK(p.pixel(227, 477) == rgb_of(Color::ChromeText));
  CHECK(p.pixel(203, 460) == rgb_of(Color::Pane));
  CHECK(p.pixel(228, 460) == rgb_of(Color::Pane));
  CHECK(p.pixel(204, 459) == rgb_of(Color::Pane));
  CHECK(p.pixel(204, 478) == rgb_of(Color::Pane));
  // Nothing at all for no labels.
  const auto none = frame_of([](ui::Renderer& r) { ui::soft_keys(r, {0, 458, 800, 22}, nullptr, 8); });
  CHECK(none.size() == 36u);
}

TEST_CASE("components: a field row puts captions and values of different faces on one baseline") {
  Panel p;
  int end = 0;
  p.draw([&](ui::Renderer& r) {
    ui::FieldRow row(r, 100, 100);
    row.field("LEN", Face::Data, "064", Color::TextDim, Color::Text);
    row.put(Face::Display, "9", Color::Text);
    end = row.x();
  });
  // "LEN" is 24 px, a gap of 4, "064" is 30 px, then a group gap of 4 gaps = 16, then one Display glyph of 16.
  CHECK(end == 100 + 24 + 4 + 30 + 16 + 16);
  // The bottom of the capitals of all three sits on the baseline: the last inked row is 99.
  const auto lowest = [&](int x0, int x1, std::uint32_t colour) {
    int low = -1;
    for (int y = 40; y < 140; ++y) {
      for (int x = x0; x < x1; ++x) {
        low = p.pixel(x, y) == colour ? y : low;
      }
    }
    return low;
  };
  CHECK(lowest(100, 124, rgb_of(Color::TextDim)) == 99);          // Label caption
  CHECK(lowest(128, 158, rgb_of(Color::Text)) == 99);             // Data value ("064": digits stand on the baseline)
  CHECK(lowest(174, 190, rgb_of(Color::Text)) == 99);             // Display glyph
}

TEST_CASE("components: a field row's chip puts the word in its ink on a fill, a gap each side and two borders over and under the capitals") {
  Panel p;
  int after_chip = 0;
  int after_field = 0;
  p.draw([&](ui::Renderer& r) {
    ui::FieldRow row(r, 100, 100);
    row.chip(Face::Data, "EDIT", Color::Accent, Color::AccentText);
    after_chip = row.x();
    ui::FieldRow labelled(r, 100, 200);
    labelled.field("STATE", Face::Data, "REC", Color::TextDim, Color::Accent, Color::AccentText);
    after_field = labelled.x();
  });
  // "EDIT" is 40 px: a gap, the word and a gap, and no group gap after it.
  CHECK(after_chip == 100 + 4 + 40 + 4);
  // The fill: x 100..147, from two rows above the capitals (cap 13: rows 87..99 of baseline 100) to two below the baseline: rows 85..101.
  CHECK(p.pixel(100, 85) == rgb_of(Color::Accent));
  CHECK(p.pixel(147, 101) == rgb_of(Color::Accent));
  CHECK(p.pixel(99, 93) == rgb_of(Color::Pane));
  CHECK(p.pixel(148, 93) == rgb_of(Color::Pane));
  CHECK(p.pixel(100, 84) == rgb_of(Color::Pane));
  CHECK(p.pixel(100, 102) == rgb_of(Color::Pane));
  // The word is AccentText, on the baseline (the last inked row is 99), and never lies on the pane.
  int low = -1;
  unsigned ink = 0;
  for (int y = 80; y < 110; ++y) {
    for (int x = 100; x < 148; ++x) {
      ink += p.pixel(x, y) == rgb_of(Color::AccentText) ? 1u : 0u;
      low = p.pixel(x, y) == rgb_of(Color::AccentText) ? y : low;
    }
  }
  CHECK(ink > 40u);
  CHECK(low == 99);
  // `field` with a fill: "STATE" (40 px), a gap, the chip ("REC" 30 px + 8), then the group gap of 4 gaps.
  CHECK(after_field == 100 + 40 + 4 + (30 + 8) + 16);
  CHECK(p.pixel(144, 185) == rgb_of(Color::Accent));
}

TEST_CASE("components: bar and meter fill by proportion, the meter has a frame and its peak tick is never on the fill") {
  Panel p;
  p.draw([](ui::Renderer& r) {
    ui::bar(r, {10, 10, 100, 6}, 50, 100, Color::Chrome, Color::Sel);
    ui::bar(r, {10, 30, 100, 6}, 500, 100, Color::Chrome, Color::Sel);  // over: clamped
    ui::bar(r, {10, 50, 100, 6}, 5, 0, Color::Chrome, Color::Sel);      // no range: only the track
    // Meters: the whole thing is 100 x 6, a one-pixel frame round a 98 x 4 slot.
    ui::meter(r, {10, 70, 100, 6}, 128, 255, 255, Color::ChromeDim, Color::Pane, Color::Play, Color::Text);
    ui::meter(r, {10, 90, 100, 6}, 255, 64, 255, Color::ChromeDim, Color::Pane, Color::Play, Color::Text);
    ui::meter(r, {10, 110, 100, 6}, 200, 100, 255, Color::ChromeDim, Color::Pane, Color::Play, Color::Text);
    ui::meter(r, {10, 130, 100, 6}, 0, 0, 255, Color::ChromeDim, Color::Pane, Color::Play, Color::Text);
  });
  CHECK((p.pixel(10, 10) == rgb_of(Color::Sel) && p.pixel(59, 10) == rgb_of(Color::Sel)));
  CHECK((p.pixel(60, 10) == rgb_of(Color::Chrome) && p.pixel(109, 10) == rgb_of(Color::Chrome)));
  CHECK((p.pixel(10, 30) == rgb_of(Color::Sel) && p.pixel(109, 30) == rgb_of(Color::Sel)));
  CHECK((p.pixel(10, 50) == rgb_of(Color::Chrome) && p.pixel(109, 50) == rgb_of(Color::Chrome)));
  CHECK(p.pixel(110, 10) == rgb_of(Color::Pane));
  // The frame: the whole outline is ChromeDim, so the slot is seen against any chrome.
  CHECK((p.pixel(10, 70) == rgb_of(Color::ChromeDim) && p.pixel(109, 75) == rgb_of(Color::ChromeDim) &&
         p.pixel(50, 70) == rgb_of(Color::ChromeDim) && p.pixel(50, 75) == rgb_of(Color::ChromeDim)));
  CHECK((p.pixel(110, 70) == rgb_of(Color::Pane) && p.pixel(10, 76) == rgb_of(Color::Pane)));
  // Level 128 of 255 over the 98 px slot is 98 * 128 / 255 = 49 px (x 11..59); the peak at the end is a tick on the
  // slot's last two pixels (107, 108), and the rest of the slot is Pane.
  CHECK((p.pixel(11, 71) == rgb_of(Color::Play) && p.pixel(59, 74) == rgb_of(Color::Play)));
  CHECK(p.pixel(60, 72) == rgb_of(Color::Pane));
  CHECK((p.pixel(107, 71) == rgb_of(Color::Text) && p.pixel(108, 74) == rgb_of(Color::Text)));
  CHECK((p.pixel(106, 72) == rgb_of(Color::Pane) && p.pixel(109, 72) == rgb_of(Color::ChromeDim)));
  // A full level has no room for a tick that is not on the fill: none, whatever the peak.
  CHECK((p.pixel(11, 91) == rgb_of(Color::Play) && p.pixel(108, 94) == rgb_of(Color::Play)));
  CHECK(p.pixel(33, 92) == rgb_of(Color::Play));  // (the peak, 64 of 255, would have been here)
  // A peak at or below the level: the tick sits just past the end of the fill (98 * 200 / 255 = 76 px, x 11..86).
  CHECK(p.pixel(86, 112) == rgb_of(Color::Play));
  CHECK((p.pixel(87, 112) == rgb_of(Color::Text) && p.pixel(88, 112) == rgb_of(Color::Text)));
  CHECK(p.pixel(89, 112) == rgb_of(Color::Pane));
  // A silent track: only the slot, no tick.
  CHECK((p.pixel(11, 131) == rgb_of(Color::Pane) && p.pixel(108, 134) == rgb_of(Color::Pane)));
}
