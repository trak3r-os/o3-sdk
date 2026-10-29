// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/list_view.h>

#include <o3sdk/ui/components.h>

#include <cstring>

namespace o3sdk::ui {

namespace {

constexpr unsigned kCutTail = 4;  // a name over the width keeps its head and its last kCutTail characters (the extension), a '~' between

// Cursor and window against `count` items shown `rows` at a time: the cursor in range, the window the least that holds it, never past
// the end of the list.
void clamp_window(unsigned count, unsigned rows, unsigned& cursor, unsigned& first) noexcept {
  if (count == 0u) {
    cursor = 0u;
    first = 0u;
    return;
  }
  if (cursor >= count) {
    cursor = count - 1u;
  }
  if (cursor < first) {
    first = cursor;
  } else if (cursor >= first + rows) {
    first = cursor + 1u - rows;
  }
  const unsigned max_first = count > rows ? count - rows : 0u;
  if (first > max_first) {
    first = max_first;
  }
}

// Cuts `s` to `chars` characters: head, '~', tail when there is room for all three, else a plain cut.
void cut_to(char* s, unsigned chars) noexcept {
  const unsigned len = static_cast<unsigned>(std::strlen(s));
  if (len <= chars) {
    return;
  }
  if (chars >= kCutTail + 3u) {
    const unsigned head = chars - 1u - kCutTail;
    s[head] = '~';
    std::memmove(s + head + 1u, s + len - kCutTail, kCutTail + 1u);  // the tail and the NUL
  } else {
    s[chars] = '\0';
  }
}

// `src` into `out` (kListTextMax bytes), cut and NUL ended.
void copy_text(char* out, const char* src) noexcept {
  unsigned n = 0;
  for (; src != nullptr && src[n] != '\0' && n < kListTextMax - 1u; ++n) {
    out[n] = src[n];
  }
  out[n] = '\0';
}

int text_chars(const Renderer& r, int width, Face face) noexcept {
  const int cell = r.theme().geometry(face).cell_w;
  return width > 0 && cell > 0 ? width / cell : 0;
}

}  // namespace

unsigned ListView::rows_in(const Theme& theme, const Rect& area) noexcept {
  const int pitch = theme.metrics.row_pitch;
  return pitch > 0 && area.h > 0 ? static_cast<unsigned>(area.h / pitch) : 0u;
}

void ListView::set_rows(unsigned rows) noexcept { rows_ = rows == 0u ? 1u : rows; }

void ListView::fit(const ListSource& source) noexcept { clamp_window(source.count(), rows_, cursor_, first_); }

bool ListView::select(const ListSource& source, unsigned index) noexcept {
  const unsigned count = source.count();
  const unsigned before = cursor_;
  cursor_ = index;
  clamp_window(count, rows_, cursor_, first_);
  return cursor_ != before;
}

bool ListView::move(const ListSource& source, int delta) noexcept {
  const unsigned count = source.count();
  clamp_window(count, rows_, cursor_, first_);
  if (count == 0u) {
    return false;
  }
  const std::int64_t to = static_cast<std::int64_t>(cursor_) + delta;
  const unsigned want = to < 0 ? 0u : (to >= static_cast<std::int64_t>(count) ? count - 1u : static_cast<unsigned>(to));
  return select(source, want);
}

bool ListView::page(const ListSource& source, int pages) noexcept {
  const int step = rows_ > 1u ? static_cast<int>(rows_ - 1u) : 1;
  return move(source, pages * step);
}

bool ListView::home(const ListSource& source) noexcept { return select(source, 0u); }

bool ListView::end(const ListSource& source) noexcept {
  const unsigned count = source.count();
  return select(source, count == 0u ? 0u : count - 1u);
}

bool ListView::pad(const ListSource& source, unsigned pad) noexcept {
  const unsigned count = source.count();
  clamp_window(count, rows_, cursor_, first_);
  if (pad >= rows_ || first_ + pad >= count) {
    return false;
  }
  return select(source, first_ + pad);
}

void ListView::draw(Renderer& r, const Rect& area, const ListSource& source, const ListEmpty& empty, Face face) const noexcept {
  const Theme& t = r.theme();
  const Metrics& m = t.metrics;
  const unsigned count = source.count();
  const int pitch = m.row_pitch;
  const unsigned fits = rows_in(t, area);
  if (count == 0u) {  // the empty state: a headline and a hint, centred
    if (fits == 0u) {
      return;
    }
    char text[kListTextMax];
    const int mid = area.y + area.h / 2;
    const int cols = text_chars(r, area.w - 2 * m.pad, Face::Data);
    copy_text(text, empty.headline);
    cut_to(text, static_cast<unsigned>(cols));
    r.text(area.x + (area.w - r.text_width(text, Face::Data)) / 2, caps_y(t, Face::Data, mid - pitch, pitch), text, Color::Text, Face::Data);
    if (empty.hint != nullptr && empty.hint[0] != '\0') {
      copy_text(text, empty.hint);
      cut_to(text, static_cast<unsigned>(text_chars(r, area.w - 2 * m.pad, Face::Label)));
      r.text(area.x + (area.w - r.text_width(text, Face::Label)) / 2, caps_y(t, Face::Label, mid, pitch), text, Color::TextDim, Face::Label);
    }
    return;
  }
  const unsigned rows = fits < rows_ ? fits : rows_;
  if (rows == 0u) {
    return;
  }
  unsigned cursor = cursor_;
  unsigned first = first_;
  clamp_window(count, rows, cursor, first);
  const unsigned shown = count - first < rows ? count - first : rows;

  const bool mark = count > rows;
  const int mark_w = m.gap;
  const int body_w = area.w - (mark ? mark_w + m.border : 0);
  const int text_x = area.x + 2 * m.pad;

  // The widest value of the window sets the value column, so the names are cut to what is left of the row.
  int value_w = 0;
  char value[kListTextMax];
  for (unsigned i = 0; i < shown; ++i) {
    source.value(first + i, value, sizeof value);
    value[sizeof value - 1u] = '\0';
    const int w = r.text_width(value, face);
    value_w = w > value_w ? w : value_w;
  }
  const int right = area.x + body_w - 2 * m.pad;  // the values end here
  const unsigned name_chars = static_cast<unsigned>(text_chars(r, right - text_x - (value_w > 0 ? value_w + 2 * m.gap : 0), face));

  // Colour passes: panels (Sel bar, mark track, mark thumb), values (TextDim, as the thumb), names (Text), then the cursor row (SelText).
  const int cursor_row = static_cast<int>(cursor - first);
  r.highlight_row({area.x, area.y + cursor_row * pitch, body_w, pitch});
  if (mark) {
    const int track_h = static_cast<int>(shown) * pitch;
    const int track_x = area.x + area.w - mark_w;
    r.panel({track_x, area.y, mark_w, track_h}, Color::Empty);
    const unsigned max_first = count - rows;
    int thumb_h = static_cast<int>(static_cast<unsigned>(track_h) * rows / count);
    thumb_h = thumb_h < 2 * m.gap ? 2 * m.gap : thumb_h;
    const int thumb_y = area.y + static_cast<int>(static_cast<unsigned>(track_h - thumb_h) * first / max_first);
    r.panel({track_x, thumb_y, mark_w, thumb_h}, Color::TextDim);
  }
  char name[kListTextMax];
  const auto row_text = [&](unsigned i, bool as_value) {
    char* out = as_value ? value : name;
    if (as_value) {
      source.value(first + i, out, sizeof value);
    } else {
      source.name(first + i, out, sizeof name);
    }
    out[kListTextMax - 1u] = '\0';
  };
  const auto y_of = [&](unsigned i) { return caps_y(t, face, area.y + static_cast<int>(i) * pitch, pitch); };
  if (value_w > 0) {
    for (unsigned i = 0; i < shown; ++i) {
      if (static_cast<int>(i) != cursor_row) {
        row_text(i, true);
        r.text(right - r.text_width(value, face), y_of(i), value, Color::TextDim, face);
      }
    }
  }
  for (unsigned i = 0; i < shown; ++i) {
    if (static_cast<int>(i) != cursor_row) {
      row_text(i, false);
      cut_to(name, name_chars);
      r.text(text_x, y_of(i), name, Color::Text, face);
    }
  }
  const unsigned c = static_cast<unsigned>(cursor_row);
  row_text(c, false);
  cut_to(name, name_chars);
  r.text(text_x, y_of(c), name, Color::SelText, face);
  if (value_w > 0) {
    row_text(c, true);
    r.text(right - r.text_width(value, face), y_of(c), value, Color::SelText, face);
  }
}

}  // namespace o3sdk::ui
