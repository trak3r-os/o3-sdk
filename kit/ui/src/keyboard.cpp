// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/keyboard.h>

#include <o3sdk/ui/components.h>

#include <cstring>

namespace o3sdk::ui {

namespace {

constexpr int kCols = static_cast<int>(kKeyboardCols);

struct Layout {
  Rect outer;
  int x0;       // the left edge of the key grid and of the name box
  int cell_w;   // one pad's width
  int pitch;    // the name box, the title and the message rows
  int row_h;    // a key row
  int y_title;
  int y_name;
  int y_message;
  int y_keys;
};

int cell_width(const Theme& t) noexcept { return t.geometry(Face::Data).cell_w * 4; }

Layout layout_of(const Theme& t, int x, int y) noexcept {
  const Metrics& m = t.metrics;
  Layout l{};
  l.cell_w = cell_width(t);
  l.pitch = m.row_pitch;
  l.row_h = l.pitch * 5 / 4;
  const int w = kCols * l.cell_w + 6 * m.pad;
  const int h = 2 * m.pad + 3 * l.pitch + static_cast<int>(kKeyboardRows) * l.row_h;
  l.outer = {x, y, w, h};
  l.x0 = x + 3 * m.pad;
  l.y_title = y + m.pad;
  l.y_name = l.y_title + l.pitch;
  l.y_message = l.y_name + l.pitch;
  l.y_keys = l.y_message + l.pitch;
  return l;
}

// Calls fn(key, rect) for every key of the grid: a key spans the run of pads that name it in its row.
template <typename Fn>
void for_each_key(const Layout& l, const std::uint8_t* pad_key, Fn fn) noexcept {
  for (int row = 0; row < static_cast<int>(kKeyboardRows); ++row) {
    int c = 0;
    while (c < kCols) {
      const unsigned key = pad_key[row * kCols + c];
      int span = 1;
      while (c + span < kCols && pad_key[row * kCols + c + span] == key) {
        ++span;
      }
      if (key < kKeyboardKeys) {
        fn(key, Rect{l.x0 + c * l.cell_w, l.y_keys + row * l.row_h, span * l.cell_w - 2, l.row_h - 2});
      }
      c += span;
    }
  }
}

}  // namespace

Rect keyboard_size(const Theme& theme) noexcept { return layout_of(theme, 0, 0).outer; }

Rect keyboard(Renderer& r, const Rect& area, const KeyboardView& v) noexcept {
  const Theme& t = r.theme();
  const Metrics& m = t.metrics;
  const Rect size = keyboard_size(t);
  const Layout l = layout_of(t, area.x + (area.w - size.w) / 2, area.y + (area.h - size.h) / 2);
  const int cell = t.geometry(Face::Data).cell_w;
  const unsigned len = static_cast<unsigned>(std::strlen(v.text));

  const Rect box{l.x0, l.y_name, kCols * l.cell_w, l.pitch};
  const int inner_y = box.y + m.border + 1;
  const int inner_h = l.pitch - 2 * m.border - 2;
  const int tx = box.x + m.pad;
  const bool caps_marked = v.shift;  // the caps key, on and not in focus, is the one inverted key

  // Colour passes. Chrome: the dialog. Sel: the selection of the name. Accent: the edit cursor, or the key in focus.
  r.panel(l.outer, Color::Chrome);
  if (v.selected && len != 0u) {
    r.panel({tx - 1, inner_y, static_cast<int>(len) * cell + 2, inner_h}, Color::Sel);
  }
  if (v.name_focus) {
    r.panel({tx + static_cast<int>(v.edit) * cell, inner_y, 2, inner_h}, Color::Accent);
  } else {
    for_each_key(l, v.pad_key, [&](unsigned key, const Rect& k) {
      if (key == v.key) {
        r.panel(k, Color::Accent);
      }
    });
  }
  // ChromeText: the caps key in force (an inverted block), and the frame of the name while it has the focus.
  if (caps_marked) {
    for_each_key(l, v.pad_key, [&](unsigned key, const Rect& k) {
      if (v.key_chars[key] == 1 && !(!v.name_focus && key == v.key)) {
        r.panel(k, Color::ChromeText);
      }
    });
  }
  if (v.name_focus) {
    r.frame(box, Color::ChromeText);
  }
  // ChromeDim: the dialog's frame, the name's frame without the focus and the frame of every key that is not a block.
  r.frame(l.outer, Color::ChromeDim);
  if (!v.name_focus) {
    r.frame(box, Color::ChromeDim);
  }
  for_each_key(l, v.pad_key, [&](unsigned key, const Rect& k) {
    const bool block = (!v.name_focus && key == v.key) || (caps_marked && v.key_chars[key] == 1);
    if (!block) {
      r.frame(k, Color::ChromeDim);
    }
  });

  // Text. ChromeText: the title, the name unless selected, the message and the labels of the keys that are neither refused nor in focus.
  const int cap_y_name = caps_y(t, Face::Data, box.y, l.pitch);
  if (v.title != nullptr) {
    r.text(l.x0, caps_y(t, Face::Label, l.y_title, l.pitch), v.title, Color::ChromeText, Face::Label);
  }
  if (!(v.selected && len != 0u)) {
    r.text(tx, cap_y_name, v.text, Color::ChromeText, Face::Data);
  }
  if (v.message != nullptr && v.message[0] != '\0') {
    r.text(l.x0, caps_y(t, Face::Label, l.y_message, l.pitch), v.message, Color::ChromeText, Face::Label);
  }
  const auto label_of = [&](unsigned key, char (&one)[2]) -> const char* {
    const char c = v.key_chars[key];
    one[0] = c;
    one[1] = '\0';
    return c == 0 ? "DEL" : (c == 1 ? "CAPS" : (c == ' ' ? "SPACE" : one));
  };
  const auto draw_labels = [&](Color color, auto select) {
    for_each_key(l, v.pad_key, [&](unsigned key, const Rect& k) {
      if (!select(key)) {
        return;
      }
      char one[2];
      const char* s = label_of(key, one);
      const Face face = s == one ? Face::Data : Face::Label;
      r.text(k.x + (k.w - r.text_width(s, face)) / 2, caps_y(t, face, k.y, k.h), s, color, face);
    });
  };
  const auto in_focus = [&](unsigned key) { return !v.name_focus && key == v.key; };
  const auto is_refused = [&](unsigned key) { return ((v.refused >> key) & 1u) != 0u; };
  const auto inverted_caps = [&](unsigned key) { return caps_marked && v.key_chars[key] == 1 && !in_focus(key); };
  draw_labels(Color::ChromeText, [&](unsigned key) { return !in_focus(key) && !is_refused(key) && !inverted_caps(key); });
  // SelText: the selected name. AccentText: the label of the key in focus. Chrome: the caps key's label knocked out of its block.
  if (v.selected && len != 0u) {
    r.text(tx, cap_y_name, v.text, Color::SelText, Face::Data);
  }
  draw_labels(Color::AccentText, in_focus);
  draw_labels(Color::Chrome, inverted_caps);
  draw_labels(Color::ChromeDim, [&](unsigned key) { return !in_focus(key) && is_refused(key) && !inverted_caps(key); });
  return l.outer;
}

}  // namespace o3sdk::ui
