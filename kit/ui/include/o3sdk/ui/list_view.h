// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A scrolling list of names (file lists, project names, anything an app picks one of): a cursor on the Sel bar, an optional value at the
// right of each row, a scroll mark when the list is longer than the window, and an empty state with a headline and a hint.
//
// The items are the caller's: a ListSource answers `count()` and the text of item `i`, so the list holds no copy of a directory (a DirIndex
// or a fixed table sits behind it) and nothing is allocated here. ListView itself is the state, a cursor and the first visible row; it
// is cheap to keep next to the screen that shows it, and the screen drives it:
//
//   encoder turn      view.move(source, delta)            clamps at the ends (no wrap: a long file list should not jump from the last name to the first)
//   encoder press     source index = view.cursor()        the caller opens it
//   pad p (0..47)     view.pad(source, p)                 the pad grid reads like the window: pad p is the p-th visible row (false past the window)
//   buttons           view.page(source, +-1), view.home(source), view.end(source)
//
// Every call that moves the cursor returns true when it changed. After the source's content changes (a rescan, a delete) call fit(): the
// cursor and the window are clamped to the new count, and the cursor stays on the same index when it still exists.
//
// Drawing (immediate mode, no state kept between frames, main loop only):
//   ui::ListView view(rows);  ...  view.draw(r, area, source, {"NO FILES", "put .wav files in Samples/"});
// `rows` is the window's height in rows; rows_in(theme, area) gives what an area holds at the theme's row pitch (area.h / row_pitch), so
// a screen builds its ListView from the area it will draw in. A draw never shows more rows than `area` holds.
//
// Colours: names Text, values TextDim on the Pane; the cursor row is the Sel bar with SelText; the scroll mark is an Empty track with a
// TextDim thumb (every pair is in theme.h's kContrastPairs). Display-list cost of a frame: one word per non-space glyph, a few per colour
// change and one per row for the cursor and the mark, so a window of 18 rows of 40-glyph names with 8-glyph values is 898 words of the 2,048 (tests/ui_list_test.cpp).

#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::ui {

// The longest name or value the list asks for (NUL included): longer text is cut by the source's own `cap`.
inline constexpr unsigned kListTextMax = 64;

// The caller's items. The functions are called in the main loop, while drawing, so they must be cheap and must not block: a lookup in
// memory, never a card read.
class ListSource {
 public:
  // The number of items.
  virtual unsigned count() const noexcept = 0;
  // Item `index` (< count) as text: write at most `cap - 1` characters and a NUL into `out` (cap >= 1).
  virtual void name(unsigned index, char* out, unsigned cap) const noexcept = 0;
  // The value shown at the right of the row (a size, a date, a tag); the default shows none. Same contract as name().
  virtual void value(unsigned index, char* out, unsigned cap) const noexcept {
    (void)index;
    if (cap != 0u) {
      out[0] = '\0';
    }
  }

 protected:
  ~ListSource() = default;  // not deleted through this interface
};

// What the list says when it has no item: a headline in the Data face and a hint under it in the Label face, both centred (the hint may
// be an empty string).
struct ListEmpty {
  const char* headline;
  const char* hint;
};

class ListView {
 public:
  // `rows`: the window's height in rows (at least 1).
  explicit ListView(unsigned rows) noexcept : rows_(rows == 0u ? 1u : rows) {}

  // How many rows `area` holds at the theme's row pitch.
  static unsigned rows_in(const Theme& theme, const Rect& area) noexcept;

  void set_rows(unsigned rows) noexcept;
  unsigned rows() const noexcept { return rows_; }
  unsigned cursor() const noexcept { return cursor_; }  // the selected item (0 when the list is empty)
  unsigned first() const noexcept { return first_; }    // the first visible item

  // Clamps the cursor and the window to the source's current count.
  void fit(const ListSource& source) noexcept;
  // Moves the cursor by `delta` items, clamped; scrolls the window the least that keeps the cursor in it.
  bool move(const ListSource& source, int delta) noexcept;
  // `pages` windows at a time (a window's worth of items less one, so the last row seen stays in view).
  bool page(const ListSource& source, int pages) noexcept;
  bool home(const ListSource& source) noexcept;
  bool end(const ListSource& source) noexcept;
  // Puts the cursor on `index` (clamped).
  bool select(const ListSource& source, unsigned index) noexcept;
  // Pad `pad` (0..47) selects the visible row of that number; false (nothing changed) for a pad past the window or past the items.
  bool pad(const ListSource& source, unsigned pad) noexcept;

  // Draws the window into `area` (rows from its top, the cursor's Sel bar the full width, the scroll mark at its right edge), or the
  // empty state when the source has no item. `face` is the text face of the rows (Data by default).
  void draw(Renderer& r, const Rect& area, const ListSource& source, const ListEmpty& empty, Face face = Face::Data) const noexcept;

 private:
  unsigned rows_;
  unsigned cursor_ = 0;
  unsigned first_ = 0;
};

}  // namespace o3sdk::ui
