// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HELP, the kit's overlay over any app screen (docs/FRAMEWORK.md, "Help"): Shift + Config opens it, Config or F8 closes it. The runtime owns one; the app supplies only data
// (Screen::help(), help.h) and never sees a key while it is open. While it is open the app's input is suspended as under the system Config (runtime.h): a release of something the app
// saw pressed still reaches it, and a press HELP took is not released to the app either. The screen goes on running and the sound on playing.
//
// Two pages, F1 and F2 (Left and Right also switch):
//   BUTTONS  the F row as eight key caps along the bottom (the screen's label, its longer text in two lines, the Shift row's label), above them the 25 other buttons as key caps in their
//            printed groups, two to a row, each with what it does here and what it does with Shift held, and the encoder as a round cap. UP / DOWN and the encoder scroll by rows when the
//            list does not fit. A button the screen gives no text is dimmed, with its name only.
//   PADS     the 4 x 12 grid as 48 pads (rounded boxes), each filled with its group's colour (a theme token) and labelled; a pad in no group is drawn dark; under it the legend, one line a
//            group, and one line about the pads as a whole. With no group at all the page says NO PAD HELP ON THIS SCREEN.
//
// Drawn with the Renderer and ui::plot::Canvas only (no bitmaps), in the theme of the Renderer (the screen's, else the context's), always with the kit's header and the overlay's own
// soft keys. A frame is held under kWordBudget display-list words whatever the texts: the BUTTONS list shows as many rows as the budget leaves after the summary and the F row (a glyph is one
// word) and scrolls for the rest; every text is cut at its limit (help.h), never wrapped past its box.
//
// Main loop only. No heap: the Help (about 0.5 KB) and the soft-key labels live in the overlay, filled once at show().

#include <o3sdk/app/help.h>
#include <o3sdk/app/input.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::app {

class Context;
class Screen;
struct SoftKeys;

class HelpOverlay {
 public:
  enum class Page : std::uint8_t { Buttons, Pads };

  // The most display-list words a HELP frame may use (the simulator's scenarios bound 1,800, RAM_DL holds 2,048).
  static constexpr unsigned kWordBudget = 1400;
  static constexpr unsigned kEntries = 26;  // the 25 buttons that are not soft keys, and the encoder
  static constexpr unsigned kMaxRows = 16;

  bool open() const noexcept { return open_; }
  Page page() const noexcept { return page_; }
  const Help& help() const noexcept { return help_; }
  // The BUTTONS list: the first row shown, the last it may be scrolled to, the rows there are, and how many of them the first row's window shows (the rest need a scroll).
  unsigned scroll() const noexcept { return first_row_; }
  unsigned scroll_max() const noexcept { return max_first_; }
  unsigned rows() const noexcept { return row_count_; }
  unsigned rows_shown() const noexcept { return window(first_row_); }

  // Opens on BUTTONS, scrolled to the top. Asks `screen` for its Help (with `ctx` as it is: the runtime has let go of Shift for the call, so a screen describes itself as it is without it) and
  // keeps the labels of its soft keys as `keys` has them (the runtime asked them without Shift too).
  void show(const Screen& screen, const Context& ctx, const SoftKeys& keys) noexcept;
  void close() noexcept { open_ = false; }

  // One input while open: F1 BUTTONS, F2 PADS, F8 CLOSE (Config closes it too: the runtime's), Left and Right the page, Up, Down and the encoder scroll the BUTTONS list. The runtime
  // asks for a frame after it.
  void input(const Input& in) noexcept;

  // The content area between the header and the soft keys, and the overlay's soft keys (BUTTONS, PADS, CLOSE; the page in force hot).
  void draw(ui::Renderer& r, const ui::Rect& area) const noexcept;
  void soft_keys(SoftKeys& keys) const noexcept;

 private:
  struct Cell {
    const char* name;
    const char* text;
    const char* shift;
    bool encoder;
  };

  Cell cell(unsigned entry) const noexcept;
  void build_rows() noexcept;
  void scroll_by(int rows) noexcept;
  unsigned row_height(unsigned row) const noexcept;
  unsigned row_cost(unsigned row) const noexcept;
  bool group_start(unsigned row) const noexcept;
  unsigned window(unsigned first) const noexcept;
  unsigned summary_words() const noexcept;
  unsigned fkeys_words() const noexcept;

  void draw_buttons(ui::Renderer& r, const ui::Rect& area) const noexcept;
  void draw_pads(ui::Renderer& r, const ui::Rect& area) const noexcept;

  Help help_;
  const HelpButton* given_[kEntries] = {};  // the app's entry for each of the 25 buttons (by position in the printed order), null: none
  char label_[kFKeyCount][13] = {};         // the soft keys' labels as they were at show()
  std::uint8_t label_dim_ = 0;              // ... and which of them the screen dims
  std::uint8_t rows_[kMaxRows][2] = {};     // the entries of each row of the BUTTONS list (kNone: an empty cell)
  std::uint8_t row_count_ = 0;
  std::uint8_t first_row_ = 0;
  std::uint8_t max_first_ = 0;
  std::uint16_t list_budget_ = 0;           // the words the list may use
  bool no_help_ = false;                    // the screen gave nothing: BUTTONS says so
  Page page_ = Page::Buttons;
  bool open_ = false;
};

}  // namespace o3sdk::app
