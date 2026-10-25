// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/widgets.h>

#include <o3sdk/core/param.h>
#include <o3sdk/ui/components.h>

namespace o3sdk::app {

namespace {

using ui::Color;
using ui::Face;
using ui::Rect;

Rect inset(const Rect& r, int d) noexcept { return Rect{r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d}; }

// `s` cut to `max` characters into `out` (max < cap).
void cut(const char* s, unsigned max, char* out, unsigned cap) noexcept {
  unsigned n = 0;
  for (; s != nullptr && s[n] != '\0' && n < max && n + 1u < cap; ++n) {
    out[n] = s[n];
  }
  out[n] = '\0';
}

void append(char* out, unsigned cap, const char* s) noexcept {
  unsigned n = 0;
  while (n < cap && out[n] != '\0') {
    ++n;
  }
  for (; s != nullptr && *s != '\0' && n + 1u < cap; ++s) {
    out[n++] = *s;
  }
  if (n < cap) {
    out[n] = '\0';
  }
}

}  // namespace

void knob_strip(ui::Renderer& r, const Rect& area, const dsp::Engine& engine, const ParamKeys& keys) noexcept {
  const ParamPage* page = keys.page();
  const int cell_w = area.w / static_cast<int>(kFKeyCount);
  r.panel(area, Color::Pane);
  r.rule(area.x, area.y, area.w, ui::Axis::Horizontal, Color::Empty);

  if (page != nullptr && keys.page_count() > 1u) {
    char line[24];
    cut(page->name, 12, line, sizeof line);
    char num[8] = {static_cast<char>(' '), static_cast<char>('0' + (keys.page_index() + 1u) % 10u), '/',
                   static_cast<char>('0' + keys.page_count() % 10u), '\0'};
    append(line, sizeof line, num);
    r.text(area.x + 6, area.y + 3, line, Color::TextDim, Face::Micro);
  }

  for (unsigned k = 0; k < kFKeyCount; ++k) {
    const Rect cell{area.x + static_cast<int>(k) * cell_w, area.y + 1, cell_w, area.h - 1};
    if (k != 0u) {
      r.rule(cell.x, cell.y + 14, cell.h - 18, ui::Axis::Vertical, Color::Empty);
    }
    const bool used = page != nullptr && k < page->count;
    if (!used) {
      continue;
    }
    const core::ParamDesc* d = engine.param_desc(page->unit, page->ids[k]);
    if (d == nullptr) {
      continue;
    }
    const bool focus = k == keys.focus();
    const bool held = (keys.held() >> k & 1u) != 0u;
    const Rect box{cell.x + 3, cell.y + 14, cell.w - 6, cell.h - 16};
    if (focus) {
      r.panel(box, Color::Sel);
    } else if (held) {
      r.frame(box, Color::Accent);
    }
    char name[16];
    cut(d->name, static_cast<unsigned>((box.w - 6) / 8), name, sizeof name);
    r.text(box.x + 4, box.y + 2, name, focus ? Color::SelText : Color::TextDim, Face::Label);

    char value[24];
    (void)engine.format_param(page->unit, page->ids[k], value, sizeof value);
    append(value, sizeof value, d->unit_text);
    char shown[16];
    cut(value, static_cast<unsigned>((box.w - 6) / 10), shown, sizeof shown);
    r.text(box.x + 4, box.y + 19, shown, focus ? Color::SelText : Color::Text, Face::Data);

    const float n = core::param_norm(*d, engine.param(page->unit, page->ids[k]));
    const auto filled = static_cast<unsigned>(n * 1000.0f + 0.5f);
    ui::bar(r, Rect{box.x + 4, box.y + box.h - 8, box.w - 8, 4}, filled, 1000u, focus ? Color::SelText : Color::Empty,
            focus ? Color::Accent : Color::Instr);
  }
}

void level_meter(ui::Renderer& r, const Rect& area, float peak) noexcept {
  const float p = peak < 0.0f ? 0.0f : (peak > 1.0f ? 1.0f : peak);
  const auto level = static_cast<unsigned>(p * 1000.0f + 0.5f);
  ui::meter(r, area, level, level, 1000u, Color::ChromeDim, Color::Pane, peak >= 1.0f ? Color::Accent : Color::Play, Color::Text);
}

void step_row(ui::Renderer& r, const Rect& area, unsigned count, std::uint32_t on, int playhead, int cursor) noexcept {
  if (count == 0u || count > 32u) {
    return;
  }
  const int w = area.w / static_cast<int>(count);
  for (unsigned i = 0; i < count; ++i) {
    const Rect cell{area.x + static_cast<int>(i) * w + 1, area.y, w - 2, area.h};
    const bool lit = (on >> i & 1u) != 0u;
    if (static_cast<int>(i) == playhead) {
      r.panel(cell, Color::Play);
    } else if (lit) {
      r.panel(cell, (i % 4u) == 0u ? Color::Text : Color::TextDim);
    } else {
      r.frame(cell, (i % 4u) == 0u ? Color::TextDim : Color::Empty);
    }
    if (static_cast<int>(i) == cursor) {
      r.frame(inset(cell, -1), Color::Accent);
    }
  }
}

}  // namespace o3sdk::app
