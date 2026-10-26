// SPDX-License-Identifier: GPL-3.0-or-later
#include "play_screen.h"

#include <@APP_ID@/pad_map.h>
#include <@APP_ID@/units/voice/voice.h>

#include <o3sdk/app/context.h>
#include <o3sdk/app/widgets.h>

namespace @APP_ID@ {

using namespace o3sdk;
using app::Button;
using ui::Color;
using ui::Face;
using ui::Rect;

void PlayScreen::init(dsp::UnitId voice) noexcept {
  // setup() can run again on the same app object (every test starts a fresh simulator on the one o3sdk_app() instance): start from scratch.
  voice_ = voice;
  octave_ = 0;
  page_ = app::ParamPage{"VOICE", voice, {Voice::Wave, Voice::Decay, Voice::Level, 0, 0, 0, 0, 0}, Voice::kParamCount};
  keys_.set_pages(&page_, 1);  // the table must outlive the helper: it is a member
  for (auto& n : held_) {
    n = -1;
  }
}

void PlayScreen::input(app::Context& ctx, const app::Input& in) noexcept {
  if (keys_.input(ctx, in)) {  // F1..F8 and the encoder
    return;
  }
  if (in.press(Button::Left) || in.press(Button::Right)) {
    octave_ = clamp_octave(octave_ + (in.press(Button::Left) ? -1 : 1));
    ctx.redraw();
  } else if (in.pad_press()) {
    const int note = key_note(in.pad(), octave_);
    if (note >= 0) {
      held_[in.pad()] = note;
      (void)ctx.engine().note_on(voice_, static_cast<std::uint8_t>(note), 110);
    }
  } else if (in.pad_release()) {
    if (held_[in.pad()] >= 0) {
      (void)ctx.engine().note_off(voice_, static_cast<std::uint8_t>(held_[in.pad()]));
      held_[in.pad()] = -1;
    }
  }
}

void PlayScreen::draw(app::Context& ctx, ui::Renderer& r, const Rect& area) noexcept {
  // The pads as they are laid out; a held pad is lit in the accent colour, a white key is filled and a black key framed.
  const int cell_w = 30;
  const int cell_h = 26;
  const int map_x = 16;
  const int map_y = area.y + 34;
  r.text(map_x, area.y + 12, "PADS", Color::TextDim, Face::Label);
  char oct[8] = "OCT +0";
  oct[4] = octave_ < 0 ? '-' : '+';
  oct[5] = static_cast<char>('0' + (octave_ < 0 ? -octave_ : octave_));
  r.text(map_x + 12 * cell_w - r.text_width(oct, Face::Label), area.y + 12, oct, Color::Text, Face::Label);
  for (unsigned pad = 0; pad < app::kPadCount; ++pad) {
    const Rect c{map_x + static_cast<int>(app::pad_col(pad)) * cell_w, map_y + static_cast<int>(app::pad_row(pad)) * cell_h,
                 cell_w - 3, cell_h - 3};
    const int note = key_note(pad, octave_);
    if (ctx.panel().pad_held(pad)) {
      r.panel(c, Color::Accent);
    } else if (note >= 0 && is_natural(note)) {
      r.panel(c, Color::Empty);
    } else {
      r.frame(c, Color::Empty);
    }
    if (note >= 0 && note % 12 == 0) {
      char name[4] = {'C', static_cast<char>('0' + (note / 12 - 1) % 10), '\0', '\0'};
      r.text(c.x + 4, c.y + 6, name, ctx.panel().pad_held(pad) ? Color::AccentText : Color::Text, Face::Micro);
    }
  }

  // The output level, and what the keys do.
  const int rx = 420;
  const int rw = ui::kScreenW - rx - 16;
  r.text(rx, area.y + 12, "OUT", Color::TextDim, Face::Label);
  const dsp::EngineStats s = ctx.engine().stats();
  app::level_meter(r, Rect{rx + 40, area.y + 14, rw - 40, 10}, s.peak_l > s.peak_r ? s.peak_l : s.peak_r);
  const int hy = map_y + static_cast<int>(app::kPadRows) * cell_h + 18;
  r.text(16, hy, "PADS play   F1-F3 pick a value   ENCODER turn it (SHIFT coarse)   LEFT/RIGHT octave", Color::TextDim, Face::Label);

  // Eight cells above the soft keys: name, value and a bar (the kit's widget).
  app::knob_strip(r, Rect{0, area.y + area.h - app::kKnobStripH, ui::kScreenW, app::kKnobStripH}, ctx.engine(), keys_);
}

void PlayScreen::soft_keys(app::Context& ctx, app::SoftKeys& out) noexcept {
  const app::ParamPage* p = keys_.page();
  if (p == nullptr) {
    return;
  }
  for (unsigned k = 0; k < p->count; ++k) {
    const core::ParamDesc* d = ctx.engine().param_desc(p->unit, p->ids[k]);
    out.label[k] = d != nullptr ? d->name : "";
  }
  out.hot = static_cast<std::uint8_t>(1u << keys_.focus());  // the key the encoder drives now
}

void PlayScreen::leds(app::Context& ctx, app::PadLeds& leds) noexcept {
  // Monochrome LEDs: a few levels carry the meaning (app/pad_leds.h). A white key is dim, a held pad is full.
  for (unsigned pad = 0; pad < app::kPadCount; ++pad) {
    const int note = key_note(pad, octave_);
    if (note < 0) {
      continue;
    }
    leds.set(pad, ctx.panel().pad_held(pad) ? app::kLedOn : (is_natural(note) ? app::kLedDim : app::kLedOff));
  }
}

}  // namespace @APP_ID@
