// SPDX-License-Identifier: GPL-3.0-or-later
//
// The list and keyboard widgets (ui/list_view.h, ui/keyboard.h): the list's cursor, window and pad rules over a caller-owned source, what it
// draws (the Sel bar, the scroll mark, values, cut names, the empty state) and its display-list cost; the keyboard drawn from an app::TextEntry
// driven by the encoder's moves and the pads. Pixels are checked for the Amiga theme (Data 10x20, Label 8x16; pad 4, gap 4, row pitch 24).
// O3SDK_UI_PNG_DIR=<dir> also writes each drawn scene as a PNG there, to be looked at.
#include <doctest/doctest.h>

#include <o3sdk/app/text_entry.h>
#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/platform/host/png.h>
#include <o3sdk/sim/eve_raster.h>
#include <o3sdk/sim/raster_display.h>
#include <o3sdk/ui/keyboard.h>
#include <o3sdk/ui/list_view.h>
#include <o3sdk/ui/theme.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace o3sdk;
using ui::Color;
using ui::Face;

namespace {

const ui::Theme& amiga() { return ui::theme(ui::ThemeId::Amiga); }

struct Names final : ui::ListSource {
  std::vector<std::string> items;
  std::vector<std::string> values;
  unsigned count() const noexcept override { return static_cast<unsigned>(items.size()); }
  void name(unsigned index, char* out, unsigned cap) const noexcept override {
    std::snprintf(out, cap, "%s", items[index].c_str());
  }
  void value(unsigned index, char* out, unsigned cap) const noexcept override {
    std::snprintf(out, cap, "%s", index < values.size() ? values[index].c_str() : "");
  }
};

Names numbered(unsigned n) {
  Names s;
  for (unsigned i = 0; i < n; ++i) {
    s.items.push_back("item " + std::to_string(i));
  }
  return s;
}

// A frame drawn into the raster, for pixels, and (under a FakeDisplay) for its word count.
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
  void save(const char* name) const {
    const char* dir = std::getenv("O3SDK_UI_PNG_DIR");
    if (dir != nullptr) {
      const std::string path = std::string(dir) + "/" + name + ".png";
      CHECK(platform::host::write_png_rgba(path.c_str(), raster.rgba(), 800, 480));
    }
  }
};

std::uint32_t rgb_of(Color c) {
  const ui::Rgb& v = amiga().rgb(c);
  return (std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b;
}

// Words one frame takes: the list or keyboard alone, to compare with the 2,048-word display list.
template <typename Fn>
std::size_t words_of(Fn draw) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
  eve::DisplayListBuilder builder(sink, storage.data(), eve::kMaxDisplayListWords);
  ui::Renderer r(builder, amiga());
  CHECK(r.begin(Color::Pane));
  draw(r);
  const std::size_t n = r.words();
  CHECK(r.end());
  return n;
}

// How many pixels of `area` are `c`.
unsigned count_pixels(const Panel& p, const ui::Rect& area, Color c) {
  unsigned n = 0;
  for (int y = area.y; y < area.y + area.h; ++y) {
    for (int x = area.x; x < area.x + area.w; ++x) {
      n += p.pixel(x, y) == rgb_of(c) ? 1u : 0u;
    }
  }
  return n;
}

}  // namespace

TEST_CASE("list view: the cursor clamps at the ends and the window follows it the least it can") {
  const Names s = numbered(20);
  ui::ListView v(5);
  CHECK(v.cursor() == 0);
  CHECK_FALSE(v.move(s, -1));  // no wrap
  CHECK(v.move(s, 3));
  CHECK((v.cursor() == 3 && v.first() == 0));
  CHECK(v.move(s, 2));  // item 5 is below the window of 0..4: the window scrolls by one
  CHECK((v.cursor() == 5 && v.first() == 1));
  CHECK(v.move(s, -4));  // back to item 1, the window's first row: it does not move
  CHECK((v.cursor() == 1 && v.first() == 1));
  CHECK(v.move(s, -1));  // item 0 is above the window
  CHECK((v.cursor() == 0 && v.first() == 0));
  CHECK(v.move(s, 1000));  // clamped to the last item, the window ends on it
  CHECK((v.cursor() == 19 && v.first() == 15));
  CHECK_FALSE(v.move(s, 1));
  CHECK(v.move(s, -2000));
  CHECK((v.cursor() == 0 && v.first() == 0));
  CHECK_FALSE(v.move(s, -2000000000));
  CHECK(v.move(s, 2000000000));  // far past an int sum
  CHECK(v.cursor() == 19);
}

TEST_CASE("list view: page, home, end, select and the pad grid") {
  const Names s = numbered(20);
  ui::ListView v(5);
  CHECK(v.page(s, 1));  // a window less one row
  CHECK(v.cursor() == 4);
  CHECK(v.page(s, 1));
  CHECK(v.cursor() == 8);
  CHECK(v.page(s, -1));
  CHECK(v.cursor() == 4);
  CHECK(v.end(s));
  CHECK((v.cursor() == 19 && v.first() == 15));
  CHECK_FALSE(v.end(s));
  CHECK(v.home(s));
  CHECK((v.cursor() == 0 && v.first() == 0));
  CHECK(v.select(s, 9));
  CHECK((v.cursor() == 9 && v.first() == 5));
  CHECK(v.select(s, 500));  // clamped
  CHECK(v.cursor() == 19);
  // Pads: pad p is the p-th visible row; past the window nothing changes.
  CHECK(v.home(s));
  CHECK(v.select(s, 9));  // scrolling forward: the window is 5..9
  CHECK(v.first() == 5);
  CHECK(v.pad(s, 0));
  CHECK(v.cursor() == 5);
  CHECK(v.pad(s, 4));
  CHECK(v.cursor() == 9);
  CHECK_FALSE(v.pad(s, 4));  // already there
  CHECK_FALSE(v.pad(s, 5));  // past the window
  CHECK_FALSE(v.pad(s, 47));
  CHECK(v.cursor() == 9);
  // A short list: pads past the items are refused too.
  const Names three = numbered(3);
  ui::ListView w(5);
  CHECK(w.pad(three, 2));
  CHECK_FALSE(w.pad(three, 3));
  CHECK(w.cursor() == 2);
}

TEST_CASE("list view: fit() clamps to a source that shrank, and an empty source leaves nothing to move") {
  Names s = numbered(20);
  ui::ListView v(5);
  CHECK(v.end(s));
  s.items.resize(7);
  v.fit(s);
  CHECK((v.cursor() == 6 && v.first() == 2));  // the window ends on the last item
  s.items.resize(2);
  v.fit(s);
  CHECK((v.cursor() == 1 && v.first() == 0));
  s.items.clear();
  v.fit(s);
  CHECK((v.cursor() == 0 && v.first() == 0));
  CHECK_FALSE(v.move(s, 1));
  CHECK_FALSE(v.page(s, 1));
  CHECK_FALSE(v.end(s));
  CHECK_FALSE(v.home(s));
  CHECK_FALSE(v.pad(s, 0));
  CHECK_FALSE(v.select(s, 3));
  ui::ListView one(0);  // a window of zero rows is one row
  CHECK(one.rows() == 1);
  CHECK(ui::ListView::rows_in(amiga(), {0, 0, 100, 100}) == 4);  // 100 / 24
  CHECK(ui::ListView::rows_in(amiga(), {0, 0, 100, 23}) == 0);
}

TEST_CASE("list view: the cursor row is the Sel bar, the rows around it are Pane, names are Text and the cursor's SelText") {
  Panel p;
  const Names s = numbered(5);
  ui::ListView v(5);
  v.move(s, 2);
  const ui::Rect area{20, 40, 300, 5 * 24};
  p.draw([&](ui::Renderer& r) { v.draw(r, area, s, {"NONE", ""}); });
  CHECK(p.pixel(20, 40 + 2 * 24) == rgb_of(Color::Sel));
  CHECK(p.pixel(319, 40 + 3 * 24 - 1) == rgb_of(Color::Sel));
  CHECK(p.pixel(20, 40 + 24 + 23) == rgb_of(Color::Pane));       // the row above ends before the bar
  CHECK(p.pixel(20, 40 + 3 * 24) == rgb_of(Color::Pane));        // and the one below starts after it
  CHECK(p.pixel(320, 40 + 2 * 24) == rgb_of(Color::Pane));       // nothing beyond the area
  // Ink: SelText on the bar, Text on the other rows, no mark (the list fits).
  CHECK(count_pixels(p, {20, 40 + 2 * 24, 300, 24}, Color::SelText) > 20);
  CHECK(count_pixels(p, {20, 40, 300, 24}, Color::Text) > 20);
  CHECK(count_pixels(p, {20, 40, 300, 24}, Color::SelText) == 0);
  CHECK(count_pixels(p, {20, 40 + 2 * 24, 300, 24}, Color::Text) == 0);
  CHECK(count_pixels(p, area, Color::Empty) == 0);
  p.save("list_cursor");
}

TEST_CASE("list view: a list longer than its window has a scroll mark whose thumb follows the window") {
  Panel top, bottom;
  const Names s = numbered(40);
  ui::ListView v(8);
  const ui::Rect area{20, 40, 300, 8 * 24};
  const int mark_x = area.x + area.w - 4;
  top.draw([&](ui::Renderer& r) { v.draw(r, area, s, {"NONE", ""}); });
  CHECK(top.pixel(mark_x, area.y) == rgb_of(Color::TextDim));                     // the thumb starts at the top...
  CHECK(top.pixel(mark_x, area.y + area.h - 1) == rgb_of(Color::Empty));          // ...over a track that runs the window's height
  CHECK(top.pixel(mark_x - 1, area.y) == rgb_of(Color::Pane));                    // the bar is cut short of the mark
  v.end(s);
  bottom.draw([&](ui::Renderer& r) { v.draw(r, area, s, {"NONE", ""}); });
  CHECK(bottom.pixel(mark_x, area.y) == rgb_of(Color::Empty));
  CHECK(bottom.pixel(mark_x, area.y + area.h - 1) == rgb_of(Color::TextDim));     // the thumb ends at the bottom
  // The thumb is a fifth of the track (8 of 40 rows), and the same size in both.
  const ui::Rect mark{mark_x, area.y, 4, area.h};
  CHECK(count_pixels(top, mark, Color::TextDim) == count_pixels(bottom, mark, Color::TextDim));
  CHECK(count_pixels(top, mark, Color::TextDim) == 4u * 38u);  // 192 * 8 / 40 = 38 px
  // The last row of the window is the cursor's and has the Sel bar, to the mark's edge.
  CHECK(bottom.pixel(area.x, area.y + 7 * 24) == rgb_of(Color::Sel));
  CHECK(bottom.pixel(mark_x - 2, area.y + 7 * 24) == rgb_of(Color::Sel));
  top.save("list_scroll_top");
  bottom.save("list_scroll_end");
}

TEST_CASE("list view: values sit at the right, in TextDim, and a long name is cut to the room left with its tail kept") {
  Panel p;
  Names s;
  s.items = {"kick.wav", "a_very_long_sample_name_that_does_not_fit_the_row.wav", "snare.wav"};
  s.values = {"12 KB", "1.2 MB", "9 KB"};
  ui::ListView v(3);
  v.move(s, 1);
  const ui::Rect area{0, 0, 300, 72};
  p.draw([&](ui::Renderer& r) { v.draw(r, area, s, {"NONE", ""}); });
  // Row 0: the value is TextDim and ends 8 px (two pads) from the right edge.
  CHECK(count_pixels(p, {250, 0, 50, 24}, Color::TextDim) > 15);
  CHECK(count_pixels(p, {293, 0, 7, 24}, Color::TextDim) == 0);
  // Row 1 (the cursor): its value is SelText at the right and its name stops before the value column (6 chars of 10 px + gaps).
  CHECK(count_pixels(p, {244, 24, 56, 24}, Color::SelText) > 15);
  const std::size_t full = words_of([&](ui::Renderer& r) {
    Names plain;
    plain.items = {s.items[1]};
    ui::ListView(1).draw(r, {0, 0, 800, 24}, plain, {"", ""});
  });
  const std::size_t cut = words_of([&](ui::Renderer& r) {
    Names narrow;
    narrow.items = {s.items[1]};
    narrow.values = {"1.2 MB"};
    ui::ListView(1).draw(r, {0, 0, 300, 24}, narrow, {"", ""});
  });
  CHECK(cut < full);  // fewer glyphs: the name was cut
  p.save("list_values");
}

TEST_CASE("list view: the empty state draws a headline and a hint, centred, and no bar") {
  Panel p;
  const Names none;
  const ui::ListView v(5);
  const ui::Rect area{100, 100, 400, 120};
  p.draw([&](ui::Renderer& r) { v.draw(r, area, none, {"NO FILES", "put .wav files in Samples/"}); });
  CHECK(count_pixels(p, area, Color::Sel) == 0);
  CHECK(count_pixels(p, {area.x, area.y, area.w, area.h / 2}, Color::Text) > 40);      // the headline, above the middle
  CHECK(count_pixels(p, {area.x, area.y + area.h / 2, area.w, area.h / 2}, Color::TextDim) > 40);  // the hint, below it
  // Centred: ink on both sides of the middle column, none at the area's edges.
  CHECK(count_pixels(p, {area.x, area.y, area.w / 2, area.h}, Color::Text) > 15);
  CHECK(count_pixels(p, {area.x + area.w / 2, area.y, area.w / 2, area.h}, Color::Text) > 15);
  CHECK(count_pixels(p, {area.x, area.y, 40, area.h}, Color::Text) == 0);
  p.save("list_empty");
  // A hint wider than the area is cut and an empty hint draws nothing: both stay inside the display list.
  const std::size_t two = words_of([&](ui::Renderer& r) { v.draw(r, area, none, {"NO FILES", "x"}); });
  const std::size_t one = words_of([&](ui::Renderer& r) { v.draw(r, area, none, {"NO FILES", ""}); });
  CHECK(one < two);
  CHECK(words_of([&](ui::Renderer& r) { v.draw(r, {0, 0, 40, 5}, none, {"NO FILES", "hint"}); }) == words_of([](ui::Renderer&) {}));  // no row fits: nothing
}

TEST_CASE("list view: an area shorter than the window shows the rows that fit, with the cursor in view") {
  Panel p;
  const Names s = numbered(30);
  ui::ListView v(12);
  v.end(s);  // cursor 29, first 18, but only 3 rows fit
  const ui::Rect area{0, 0, 300, 3 * 24 + 10};
  p.draw([&](ui::Renderer& r) { v.draw(r, area, s, {"NONE", ""}); });
  CHECK(p.pixel(0, 2 * 24) == rgb_of(Color::Sel));  // the cursor is the last visible row
  CHECK(p.pixel(0, 3 * 24 + 1) == rgb_of(Color::Pane));
  // A cursor row past the drawn rows never happens, and a list shorter than the window leaves the rest of the area empty.
  const Names two = numbered(2);
  ui::ListView w(12);
  Panel q;
  q.draw([&](ui::Renderer& r) { w.draw(r, {0, 0, 300, 12 * 24}, two, {"NONE", ""}); });
  CHECK(q.pixel(0, 0) == rgb_of(Color::Sel));
  CHECK(q.pixel(0, 2 * 24) == rgb_of(Color::Pane));
  CHECK(count_pixels(q, {290, 0, 10, 12 * 24}, Color::Empty) == 0);  // no mark for a list that fits
}

TEST_CASE("list view: a full window of long names with values fits the display list with room to spare") {
  Names s;
  for (unsigned i = 0; i < 40; ++i) {
    s.items.push_back(std::string(40, static_cast<char>('a' + static_cast<int>(i % 26u))));
    s.values.push_back("123.4 MB");
  }
  ui::ListView v(18);  // 18 rows of 24 px is 432 px of the 480
  v.move(s, 9);
  const std::size_t words = words_of([&](ui::Renderer& r) { v.draw(r, {0, 40, 800, 18 * 24}, s, {"NONE", ""}); });
  CHECK(words < 1100);  // 18 x (40 + 8) glyphs, the bar, the mark and the colour passes
  CHECK(words > 800);
}

// ---- the keyboard ----------------------------------------------------------------------------------------------------------------------

namespace {

// The keyboard of `entry` into a fresh panel.
void draw_keyboard(Panel& p, const app::TextEntry& entry, const char* message = nullptr) {
  ui::KeyboardKeys keys;
  p.draw([&](ui::Renderer& r) { ui::keyboard(r, {0, 0, 800, 480}, ui::keyboard_view(entry, app::kTextPadKey, keys, "PATTERN NAME", message)); });
}

// The rectangle of pad `pad` in the 12 x 4 grid of an Amiga keyboard centred on the 800 x 480 screen.
ui::Rect pad_cell(unsigned pad) {
  const ui::Rect box = ui::keyboard_size(amiga());
  const int x0 = (800 - box.w) / 2 + 12;
  const int y0 = (480 - box.h) / 2 + 4 + 3 * 24;
  const int cw = 40;
  const int rh = 30;
  return {x0 + static_cast<int>(pad % 12u) * cw, y0 + static_cast<int>(pad / 12u) * rh, cw - 2, rh - 2};
}

}  // namespace

TEST_CASE("keyboard: the widget's layout constants are the text entry's") {
  static_assert(ui::kKeyboardKeys == app::kTextKeys, "42 keys");
  static_assert(ui::kKeyboardRows == app::kTextRows, "four rows");
  static_assert(ui::kKeyboardPads == sizeof(app::kTextPadKey), "one key id per pad");
  const ui::Rect size = ui::keyboard_size(amiga());
  CHECK(size.w == 12 * 40 + 24);        // 12 pad cells and 3 pads of margin each side
  CHECK(size.h == 8 + 3 * 24 + 4 * 30);  // margins, title, name and message rows, four key rows
  CHECK(size.w <= 800);
  CHECK(size.h <= 480);
}

TEST_CASE("keyboard: the name is selected at first, the focus on a key is an Accent block, and the encoder and the pads edit the entry") {
  app::TextEntry entry;
  entry.begin("KICK", 15, app::TextEntry::Filter::FileName);
  CHECK(entry.selected());

  Panel p;
  draw_keyboard(p, entry);
  const ui::Rect box = ui::keyboard_size(amiga());
  const int top = (480 - box.h) / 2;
  const int left = (800 - box.w) / 2;
  CHECK(p.pixel(left, top) == rgb_of(Color::ChromeDim));            // the dialog's frame...
  CHECK(p.pixel(left + 1, top + 1) == rgb_of(Color::Chrome));       // ...round the chrome fill
  CHECK(p.pixel(left - 1, top) == rgb_of(Color::Pane));
  // The selected name is on the Sel highlight; the focus starts on the backspace key (an Accent block, two pads wide).
  CHECK(count_pixels(p, {left, top, box.w, 3 * 24 + 8}, Color::Sel) > 100);
  const ui::Rect del = pad_cell(10);
  CHECK(p.pixel(del.x, del.y) == rgb_of(Color::Accent));
  CHECK(p.pixel(del.x + 2 * 40 - 3, del.y + del.h - 1) == rgb_of(Color::Accent));  // the whole of the two cells
  CHECK(p.pixel(pad_cell(9).x, pad_cell(9).y) == rgb_of(Color::ChromeDim));        // its neighbour has a frame only
  p.save("keyboard_start");

  // A pad types the key under it and replaces the selection: pad 12 is 'q' (row 1, column 0).
  CHECK(entry.pad(12));
  CHECK(std::strcmp(entry.text(), "q") == 0);
  CHECK(entry.key() == 11);
  // The encoder turned right moves the focus along the row; its press types the key in focus: 'w', 'e'.
  CHECK(entry.move(1, 0));
  CHECK(entry.confirm_key());
  CHECK(entry.move(1, 0));
  CHECK(entry.confirm_key());
  CHECK(std::strcmp(entry.text(), "qwe") == 0);
  // Up and down between the rows; a key past the name's filter ('@' of the layout) is refused and drawn dim.
  CHECK_FALSE(entry.pad(33));  // '@', row 2, column 9: refused, and the focus goes to it
  CHECK(std::strcmp(entry.text(), "qwe") == 0);
  CHECK(entry.key() == 32);
  CHECK(entry.move(-1, 0));  // the focus leaves the refused key: it is drawn dim
  Panel q;
  draw_keyboard(q, entry, "NAME TAKEN");
  const ui::Rect at = pad_cell(33);
  CHECK(count_pixels(q, at, Color::ChromeDim) > 100);  // its frame and its '@' label...
  CHECK(count_pixels(q, at, Color::ChromeText) == 0);  // ...in no other colour
  CHECK(count_pixels(q, pad_cell(13), Color::ChromeText) > 10);  // 'w' is allowed
  CHECK(count_pixels(q, {left, top + 4 + 2 * 24, 300, 24}, Color::ChromeText) > 20);  // the message line
  q.save("keyboard_typed");
}

TEST_CASE("keyboard: the focus in the name draws the edit cursor, and the caps key in force is an inverted block with its label knocked out") {
  app::TextEntry entry;
  entry.begin("AB", 15, app::TextEntry::Filter::Any);
  CHECK(entry.move(0, -1));  // up from the keys' row goes to the name
  CHECK(entry.frame());
  Panel p;
  draw_keyboard(p, entry);
  const ui::Rect box = ui::keyboard_size(amiga());
  const int name_x = (800 - box.w) / 2 + 12;
  const int name_y = (480 - box.h) / 2 + 4 + 24;
  CHECK(count_pixels(p, {name_x, name_y, 480, 24}, Color::Accent) >= 2u * 16u);  // the 2 px edit cursor
  CHECK(p.pixel(name_x, name_y) == rgb_of(Color::ChromeText));                 // the name's frame is bright while it has the focus
  for (unsigned pad = 0; pad < 48; ++pad) {
    const ui::Rect k = pad_cell(pad);
    CHECK(count_pixels(p, k, Color::Accent) == 0);  // no key has the focus
  }
  p.save("keyboard_name_focus");

  // Caps on: the caps key (id 33) is under the pads 34 and 35.
  CHECK(app::kTextPadKey[34] == app::kTextKeyCaps);
  CHECK(entry.pad(34));
  CHECK(entry.shift());
  Panel q;
  draw_keyboard(q, entry);
  const ui::Rect caps = pad_cell(34);
  CHECK(q.pixel(caps.x, caps.y) == rgb_of(Color::Accent));  // it was just typed, so it is the key in focus
  CHECK(entry.move(1, 0));                                    // the focus leaves it; caps stays on
  Panel s;
  draw_keyboard(s, entry);
  const ui::Rect caps_cell = pad_cell(34);
  CHECK(s.pixel(caps_cell.x + 1, caps_cell.y + 1) == rgb_of(Color::ChromeText));  // an inverted block
  CHECK(count_pixels(s, {caps_cell.x, caps_cell.y, 78, caps_cell.h}, Color::Chrome) > 20);  // "CAPS" knocked out of it
  // The labels are capitals now: the key 'q' (pad 12) shows a 'Q', which has ink in the same cell.
  CHECK(count_pixels(s, pad_cell(12), Color::ChromeText) > 15);
  s.save("keyboard_caps");
}

TEST_CASE("keyboard: a whole keyboard over a list is far inside the display list, and the dialog is drawn centred in a smaller area") {
  app::TextEntry entry;
  entry.begin("A NAME THAT IS THIRTY-TWO CHARS", 32, app::TextEntry::Filter::Any);
  ui::KeyboardKeys keys;
  const ui::KeyboardView view = ui::keyboard_view(entry, app::kTextPadKey, keys, "PROJECT NAME", "A MESSAGE");
  const std::size_t words = words_of([&](ui::Renderer& r) { (void)ui::keyboard(r, {0, 0, 800, 480}, view); });
  CHECK(words < 600);
  CHECK(words > 150);
  // Centred in an area: the rectangle returned is the size, in the middle.
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
  eve::DisplayListBuilder builder(sink, storage.data(), eve::kMaxDisplayListWords);
  ui::Renderer r(builder, amiga());
  CHECK(r.begin(Color::Pane));
  const ui::Rect drawn = ui::keyboard(r, {100, 50, 600, 300}, view);
  CHECK(r.end());
  const ui::Rect size = ui::keyboard_size(amiga());
  CHECK((drawn.w == size.w && drawn.h == size.h));
  CHECK(drawn.x == 100 + (600 - size.w) / 2);
  CHECK(drawn.y == 50 + (300 - size.h) / 2);
}
