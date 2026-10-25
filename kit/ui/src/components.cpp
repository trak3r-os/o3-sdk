// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/components.h>

namespace o3sdk::ui {

int caps_y(const Theme& theme, Face face, int y, int h) noexcept {
  const FontGeometry g = theme.geometry(face);
  // The capitals occupy rows [ascent - cap_h, ascent) of the cell.
  return y + (h - g.cap_h) / 2 - (g.ascent - g.cap_h);
}

int baseline_y(const Theme& theme, Face face, int baseline) noexcept { return baseline - theme.geometry(face).ascent; }

void strip(Renderer& r, const Rect& area, Edge edge) noexcept {
  r.panel(area, Color::Chrome);
  const int line = r.theme().metrics.border;
  if (edge == Edge::Bottom) {
    r.panel({area.x, area.y + area.h - line, area.w, line}, Color::ChromeDim);
  } else if (edge == Edge::Top) {
    r.panel({area.x, area.y, area.w, line}, Color::ChromeDim);
  }
}

int badge(Renderer& r, const Rect& area, const char* text, Color fill, Color ink, Face face) noexcept {
  r.panel(area, fill);
  const int w = r.text_width(text, face);
  r.text(area.x + (area.w - w) / 2, caps_y(r.theme(), face, area.y, area.h), text, ink, face);
  return area.w;
}

int transport_badge(Renderer& r, int x, int y, int h, bool playing, const char* label_over) noexcept {
  const Metrics& m = r.theme().metrics;
  const char* label = label_over != nullptr ? label_over : (playing ? "PLAY" : "STOP");
  const int w = m.pad + static_cast<int>(kIconSize) + m.pad + r.text_width(label, Face::Label) + m.pad;
  const Color ink = playing ? Color::PlayText : Color::Chrome;
  r.panel({x, y, w, h}, Color::ChromeDim);  // the frame; for STOP it is the whole badge
  if (playing) {
    r.panel({x + m.border, y + m.border, w - 2 * m.border, h - 2 * m.border}, Color::Play);
  }
  r.icon(x + m.pad, y + (h - static_cast<int>(kIconSize)) / 2, playing ? Icon::Play : Icon::Stop, ink);
  r.text(x + m.pad + static_cast<int>(kIconSize) + m.pad, caps_y(r.theme(), Face::Label, y, h), label, ink,
         Face::Label);
  return w;
}

void type_badge(Renderer& r, int x, int y, int size, char type) noexcept {
  const char letter[2] = {type, '\0'};
  badge(r, {x, y, size, size}, letter, Color::ChromeText, Color::Chrome, Face::Label);
}

void soft_keys(Renderer& r, const Rect& area, const char* const* labels, unsigned count, std::uint32_t dim, std::uint32_t hot) noexcept {
  if (count == 0 || labels == nullptr) {
    return;
  }
  const Theme& t = r.theme();
  const int key_w = area.w / static_cast<int>(count);
  const FontGeometry g = t.geometry(Face::Label);
  const int box_w = 2 * g.cell_w + 2 * t.metrics.pad;       // "F1" and a pad each side
  const int box_h = g.cap_h + 2 * t.metrics.pad;
  const int box_y = area.y + (area.h - box_h) / 2;
  const int text_y = caps_y(t, Face::Label, area.y, area.h);
  char numbers[9][3];  // "F1".."F9": more caps than that have no F-number to show
  const unsigned n = count < 9u ? count : 9u;
  for (unsigned i = 0; i < n; ++i) {
    numbers[i][0] = 'F';
    numbers[i][1] = static_cast<char>('1' + i);
    numbers[i][2] = '\0';
  }
  const auto label_x = [&](unsigned i) { return area.x + static_cast<int>(i) * key_w + 2 * t.metrics.pad + box_w; };
  const auto is_hot = [&](unsigned i) { return ((hot >> i) & 1u) != 0u; };
  for (unsigned i = 0; i < n; ++i) {  // pass 1: the inverted boxes, and the chip under the label of the key in force
    r.panel({area.x + static_cast<int>(i) * key_w + t.metrics.pad, box_y, box_w, box_h}, Color::ChromeText);
    if (is_hot(i)) {
      r.panel({label_x(i) - t.metrics.pad, box_y, r.text_width(labels[i], Face::Label) + 2 * t.metrics.pad, box_h}, Color::ChromeText);
    }
  }
  for (unsigned i = 0; i < n; ++i) {  // pass 2: the F-numbers, knocked out of the boxes (and the label on its chip)
    r.text(area.x + static_cast<int>(i) * key_w + t.metrics.pad + t.metrics.pad, text_y, numbers[i], Color::Chrome,
           Face::Label);
    if (is_hot(i)) {
      r.text(label_x(i), text_y, labels[i], Color::Chrome, Face::Label);
    }
  }
  for (unsigned i = 0; i < n; ++i) {  // pass 3: the labels
    if (!is_hot(i) && ((dim >> i) & 1u) == 0u) {
      r.text(label_x(i), text_y, labels[i], Color::ChromeText, Face::Label);
    }
  }
  for (unsigned i = 0; i < n && dim != 0u; ++i) {  // pass 4 (only with reserved keys): the dim labels
    if (!is_hot(i) && ((dim >> i) & 1u) != 0u) {
      r.text(label_x(i), text_y, labels[i], Color::ChromeDim, Face::Label);
    }
  }
}

void FieldRow::put(Face face, const char* s, Color c) noexcept {
  r_.text(x_, baseline_y(r_.theme(), face, baseline_), s, c, face);
  x_ += r_.text_width(s, face);
}

void FieldRow::field(const char* label, Face value_face, const char* value, Color label_c, Color value_c) noexcept {
  const int gap = r_.theme().metrics.gap;
  put(Face::Label, label, label_c);
  x_ += gap;
  put(value_face, value, value_c);
  x_ += 4 * gap;
}

void FieldRow::chip(Face face, const char* s, Color fill, Color ink) noexcept {
  const Metrics& m = r_.theme().metrics;
  const int cap_h = r_.theme().geometry(face).cap_h;
  r_.panel({x_, baseline_ - cap_h - 2 * m.border, r_.text_width(s, face) + 2 * m.gap, cap_h + 4 * m.border}, fill);
  x_ += m.gap;
  put(face, s, ink);
  x_ += m.gap;
}

void FieldRow::field(const char* label, Face value_face, const char* value, Color label_c, Color fill, Color ink) noexcept {
  const int gap = r_.theme().metrics.gap;
  put(Face::Label, label, label_c);
  x_ += gap;
  chip(value_face, value, fill, ink);
  x_ += 4 * gap;
}

void bar(Renderer& r, const Rect& area, unsigned value, unsigned max, Color track, Color fill) noexcept {
  r.panel(area, track);
  if (max == 0 || area.w <= 0) {
    return;
  }
  const unsigned v = value < max ? value : max;
  r.panel({area.x, area.y, static_cast<int>(static_cast<unsigned>(area.w) * v / max), area.h}, fill);
}

void meter(Renderer& r, const Rect& area, unsigned level, unsigned peak, unsigned max, Color frame, Color slot,
           Color fill, Color tick) noexcept {
  const int b = r.theme().metrics.border;
  r.panel(area, frame);
  const Rect inner{area.x + b, area.y + b, area.w - 2 * b, area.h - 2 * b};
  if (inner.w <= 0 || inner.h <= 0) {
    return;
  }
  bar(r, inner, level, max, slot, fill);
  if (max == 0 || peak == 0 || inner.w <= 2) {
    return;  // a silent track has no peak to hold
  }
  const unsigned l = level < max ? level : max;
  const unsigned p = peak < max ? peak : max;
  const int fill_end = inner.x + static_cast<int>(static_cast<unsigned>(inner.w) * l / max);
  int x = inner.x + static_cast<int>(static_cast<unsigned>(inner.w) * p / max) - 2;
  x = x < fill_end ? fill_end : x;  // never on the fill
  if (x + 2 > inner.x + inner.w) {
    return;  // the bar is full: no room for a tick that is not on it
  }
  r.panel({x, inner.y, 2, inner.h}, tick);
}

}  // namespace o3sdk::ui
