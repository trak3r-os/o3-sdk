// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The on-screen keyboard: a name above a grid of keys, drawn from plain arguments. The key grid is the pads' own, 12 columns by 4 rows, one
// cell a pad, so a wide key (backspace and caps two cells, space five) is as wide as the pads under the fingers that type it.
//
// The widget only draws. What it shows is `app::TextEntry` (kit/app/text_entry.h), the state machine that edits the name: ui sits below app, so
// keyboard_view() below turns any such entry into a KeyboardView by duck typing, and an app drives the entry itself:
//
//   encoder turn      entry.move(+-1, 0)        the focus along the keys, wrapping inside the row
//   encoder press     entry.confirm_key()       types the key in focus (a refusal changes nothing)
//   pad p (0..47)     entry.pad(p)              types the key under the pad
//   up / down         entry.move(0, +-1)        between the rows and the name
//   delete button     entry.backspace()
//
//   app::TextEntry entry;  entry.begin("PATTERN", 15, app::TextEntry::Filter::FileName);
//   ... on input: drive `entry`; on a frame:
//   ui::KeyboardKeys keys;   // storage for the labels, on the stack
//   ui::keyboard(r, area, ui::keyboard_view(entry, app::kTextPadKey, keys, "NAME"));
//
// Drawing is immediate-mode, main loop only, no heap. Display-list cost: 4 words of panels and frames per key (42 keys), one glyph word per
// key label and per character of the name, a few per colour change: about 500 words of the 2,048 for a full 32-character name.
//
// Colours are the chrome's (the keyboard is a dialog over the screen): ChromeText on Chrome, ChromeDim for rules and refused keys, the
// focused key an Accent block with AccentText, the name's selection Sel with SelText, the caps key in force an inverted ChromeText block.

#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::ui {

inline constexpr unsigned kKeyboardKeys = 42;  // key ids 0..41
inline constexpr unsigned kKeyboardCols = 12;
inline constexpr unsigned kKeyboardRows = 4;
inline constexpr unsigned kKeyboardPads = kKeyboardCols * kKeyboardRows;  // 48

// What the keyboard shows. All pointers are the caller's and must outlive the call.
struct KeyboardView {
  const char* title;                // above the name ("PATTERN NAME"); nullptr for none
  const char* text;                 // the name being edited
  unsigned edit;                    // the edit cursor, 0..strlen(text), drawn while the focus is in the name
  bool selected;                    // the whole name is selected (the next key replaces it)
  bool name_focus;                  // the focus is in the name (no key is highlighted)
  unsigned key;                     // the key in focus when !name_focus
  bool shift;                       // the caps key is on
  const char* key_chars;            // kKeyboardKeys characters, what each key shows now: 0 is the backspace key, 1 the caps key
  const std::uint8_t* pad_key;      // kKeyboardPads key ids: the key under each pad, row by row
  std::uint64_t refused;            // bit k: key k is refused by the name's filter (drawn dim)
  const char* message;              // a line under the name (a refusal, a hint); nullptr or "" for none
};

// The size the keyboard takes, for placing it: `keyboard()` draws exactly this rectangle.
Rect keyboard_size(const Theme& theme) noexcept;

// Draws the keyboard centred in `area` (a dialog panel with a one-pixel frame). Returns the rectangle it drew in.
Rect keyboard(Renderer& r, const Rect& area, const KeyboardView& view) noexcept;

// The labels' storage for keyboard_view().
struct KeyboardKeys {
  char chars[kKeyboardKeys];
};

// A KeyboardView of `entry`, any type with TextEntry's reading side (text, edit, selected, frame, key, shift, key_char, key_allowed).
// `pad_key` is the entry's pad table (app::kTextPadKey); `keys` takes the labels and must outlive the view.
template <typename Entry>
KeyboardView keyboard_view(const Entry& entry, const std::uint8_t* pad_key, KeyboardKeys& keys, const char* title = nullptr,
                           const char* message = nullptr) noexcept {
  std::uint64_t refused = 0;
  for (unsigned k = 0; k < kKeyboardKeys; ++k) {
    keys.chars[k] = entry.key_char(k);
    if (!entry.key_allowed(k)) {
      refused |= std::uint64_t{1} << k;
    }
  }
  return KeyboardView{title, entry.text(), entry.edit(), entry.selected(), entry.frame(), entry.key(),
                      entry.shift(), keys.chars, pad_key, refused, message};
}

}  // namespace o3sdk::ui
