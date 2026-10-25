// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The components every screen is made of (ADR 0010 section 8, docs/ui/style-guide.md): Strip, Badge, SoftKeys,
// FieldRow, Bar and Meter. Each draws through the Renderer from a rectangle or a position the screen's layout hands it,
// with colour tokens and the theme's faces and metrics; none holds state, and none knows what the data means.
//
// Vertical placement is by capital height, not by cell: caps_y() gives the top of a face's cell such that its capitals
// are centred in a box, which is what makes a Label and a Data string look level in the same row.

#include <o3sdk/ui/renderer.h>

namespace o3sdk::ui {

// The y of a cell of `face` whose capitals are centred in the box [y, y + h) (rounded towards the top).
int caps_y(const Theme& theme, Face face, int y, int h) noexcept;
// The y of a cell of `face` whose baseline is `baseline`.
int baseline_y(const Theme& theme, Face face, int baseline) noexcept;

// A chrome band: the whole rectangle in Chrome, with a one-pixel ChromeDim rule on the edge that meets the data pane.
enum class Edge : std::uint8_t { None, Top, Bottom };
void strip(Renderer& r, const Rect& area, Edge edge) noexcept;

// A badge is a filled rectangle with its text centred in it. Returns the width.
int badge(Renderer& r, const Rect& area, const char* text, Color fill, Color ink, Face face) noexcept;
// The transport badge: an icon and "PLAY" (green) or "STOP" (the grey of ChromeDim), in a one-pixel ChromeDim frame so
// that the green lamp does not melt into the chrome it sits on. `x`, `y` is the top-left corner, `h` the height. Returns
// the width (frame included).
// `label_over` (OS-WAVEXP: "EXPORTING") replaces the word, with the look of `playing`.
int transport_badge(Renderer& r, int x, int y, int h, bool playing, const char* label_over = nullptr) noexcept;
// The track-type badge: a square with the type letter, 'A' audio, 'M' MIDI, 'E' external audio (ADR 0003), inverted.
void type_badge(Renderer& r, int x, int y, int size, char type) noexcept;

// F-key key-caps: `count` equal caps across `area`, each an inverted "Fn" box and the label beside it (the labels
// belong to the caller, ADR 0009). The row is drawn in three colour passes, so it costs three COLOR_RGB words. `dim` (bit n = key n) draws that label in the dim chrome colour (a
// reserved key: one more colour pass when set), `hot` marks the key that is in force: its label sits on an inverted chip joined to the F-box (the same passes as the boxes).
void soft_keys(Renderer& r, const Rect& area, const char* const* labels, unsigned count, std::uint32_t dim = 0, std::uint32_t hot = 0) noexcept;

// A row of labelled values on one baseline: a dim caption in the Label face, then its value, then a gap. Mixed faces
// share the baseline, so the Display, Data and Label glyphs of a status strip sit on one line.
class FieldRow {
 public:
  FieldRow(Renderer& r, int x, int baseline) noexcept : r_(r), x_(x), baseline_(baseline) {}

  int x() const noexcept { return x_; }
  void move_to(int x) noexcept { x_ = x; }
  void skip(int px) noexcept { x_ += px; }
  // `s` in `face`, then no gap.
  void put(Face face, const char* s, Color c) noexcept;
  // A caption (Label face, `label_c`), the theme's gap, the value (`value_face`, `value_c`), then a group gap.
  void field(const char* label, Face value_face, const char* value, Color label_c, Color value_c) noexcept;
  // OS-BUG-LEGIB: a value on a chip. The word in `face` sits in `ink` on a `fill` panel (a theme gap each side, two borders over and under the capitals, as the strip's CPU chip), then no gap.
  // The strip's mode word (EDIT, REC, STOPPED) is the Accent orange, which no theme holds against Chrome (1.2:1 in Amiga): the chip keeps the orange and reads in AccentText.
  void chip(Face face, const char* s, Color fill, Color ink) noexcept;
  // `field` with the value on a chip of `fill`.
  void field(const char* label, Face value_face, const char* value, Color label_c, Color fill, Color ink) noexcept;

 private:
  Renderer& r_;
  int x_;
  int baseline_;
};

// A horizontal bar: `area` in `track`, filled from the left to value / max of its width in `fill`.
void bar(Renderer& r, const Rect& area, unsigned value, unsigned max, Color track, Color fill) noexcept;
// The track meter (ADR 0010 section 4). `area` is the whole meter: a one-pixel frame in `frame` (so the slot is seen
// against any chrome), inside it the slot in `slot`, the level in `fill` from the left and a two-pixel peak tick. The tick
// never lies on the fill (a tick on the green would be 1.8:1): it sits at the peak, or just past the end of the fill when
// the peak is at or below the level, and is left out for a peak of 0 or when the bar is full. level and peak are 0..max.
void meter(Renderer& r, const Rect& area, unsigned level, unsigned peak, unsigned max, Color frame, Color slot,
           Color fill, Color tick) noexcept;

}  // namespace o3sdk::ui
