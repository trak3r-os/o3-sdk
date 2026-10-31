// SPDX-License-Identifier: GPL-3.0-or-later
#include "play_screen.h"

#include <@APP_ID@/pad_map.h>
#include <@APP_ID@/units/voice/voice.h>

#include <o3sdk/app/context.h>
#include <o3sdk/app/widgets.h>

#include <cstddef>

namespace @APP_ID@ {

using namespace o3sdk;
using app::Button;
using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

// "C#4" for a MIDI note (60 is C4), into `out`; a rest is "-".
void note_name(int note, char (&out)[5]) noexcept {
  static constexpr char kLetters[12] = {'C', 'C', 'D', 'D', 'E', 'F', 'F', 'G', 'G', 'A', 'A', 'B'};
  static constexpr bool kSharp[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
  std::size_t n = 0;
  if (note <= 0) {
    out[n++] = '-';
  } else {
    out[n++] = kLetters[note % 12];
    if (kSharp[note % 12]) {
      out[n++] = '#';
    }
    out[n++] = static_cast<char>('0' + (note / 12 > 0 ? note / 12 - 1 : 0) % 10);  // the notes below 12 (octave -1) show as 0
  }
  out[n] = '\0';
}

// The card as the files service sees it (Files::card()), in the words the screen uses.
const char* card_text(const app::Files& files) noexcept {
  if (!files.available()) {
    return "NO CARD";
  }
  switch (files.card()) {
    case app::Files::Card::None: return "NO CARD";
    case app::Files::Card::Ready: return "READY";
    case app::Files::Card::ReadOnly: return "WRITE PROTECTED";
    case app::Files::Card::Unsupported: return "NOT SUPPORTED";
    case app::Files::Card::OnComputer: return "ON COMPUTER";
    case app::Files::Card::InUse: return "UPDATE RUNNING";
  }
  return "";
}

}  // namespace

void PlayScreen::init(dsp::UnitId voice) noexcept {
  // setup() can run again on the same app object (every test starts a fresh simulator on the one o3sdk_app() instance): start from scratch.
  voice_ = voice;
  octave_ = 0;
  pattern_ = Pattern{};
  note_ = nullptr;
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
  if (in.press(Button::Delete)) {
    pattern_.clear();
    note_ = nullptr;
    ctx.redraw();
  } else if (in.press(Button::Left) || in.press(Button::Right)) {
    octave_ = clamp_octave(octave_ + (in.press(Button::Left) ? -1 : 1));
    ctx.redraw();
  } else if (in.pad_press()) {
    const int note = key_note(in.pad(), octave_);
    if (note >= 0) {
      held_[in.pad()] = note;
      pattern_.record(note);  // the pattern is what the pads played; FILE saves it
      ctx.redraw();
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
  // The card and the last file job (Context::files(), polled: the screen never waits for the card). The kit's strip shows the same result
  // for a few seconds; this line stays.
  const app::Files& files = ctx.files();
  const app::Files::Status fs = files.status();
  r.text(rx, area.y + 48, "CARD", Color::TextDim, Face::Label);
  r.text(rx + 48, area.y + 48, card_text(files), files.available() && files.card() == app::Files::Card::Ready ? Color::Play : Color::Text, Face::Label);
  r.text(rx, area.y + 68, "FILE", Color::TextDim, Face::Label);
  r.text(rx + 48, area.y + 68, note_ != nullptr ? note_ : (fs.phase == app::Files::Status::Idle ? "-" : fs.text),
         note_ != nullptr || fs.phase == app::Files::Status::Failed ? Color::Accent : Color::Text, Face::Label);
  const int hy = map_y + static_cast<int>(app::kPadRows) * cell_h + 18;
  r.text(16, hy, "PADS play   F1-F3 pick a value   ENCODER turn it (SHIFT coarse)   LEFT/RIGHT octave", Color::TextDim, Face::Label);

  // The pattern: 16 steps, the cursor framed.
  r.text(16, hy + 20, "PATTERN: pads record   DELETE clear   FILE save   SAMPLE LOAD load", Color::TextDim, Face::Label);
  const int step_w = 40;
  for (unsigned i = 0; i < kPatternSteps; ++i) {
    const Rect c{16 + static_cast<int>(i) * step_w, hy + 38, step_w - 4, 26};
    if (pattern_.note[i] != 0) {
      r.panel(c, Color::Empty);
    } else {
      r.frame(c, Color::Empty);
    }
    if (i == pattern_.cursor) {
      r.frame(c, Color::Accent);
    }
    char name[5];
    note_name(pattern_.note[i], name);
    r.text(c.x + 4, c.y + 6, name, Color::Text, Face::Micro);
  }

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

// HELP (Shift + Config, o3-sdk app/help.h): the kit draws it from this table, in the screen's theme. Uncomment it and its declaration in play_screen.h, then say what your controls do (upper case, static
// tables; the limits are in docs/FRAMEWORK.md, "Help"). A button with no entry is shown dimmed. Look at it in the simulator: a scenario with `press shift`, `tap config`, `release shift`, `snap help`.
//
// void PlayScreen::help(const app::Context&, app::Help& h) const noexcept {
//   static constexpr app::HelpButton kButtons[] = {
//       {app::Button::Left, "KEYBOARD AN OCTAVE DOWN", nullptr},
//       {app::Button::Right, "KEYBOARD AN OCTAVE UP", nullptr},
//       {app::Button::Delete, "CLEAR THE PATTERN", nullptr},
//   };
//   static constexpr app::HelpPadGroup kGroups[] = {{"KEYS", "PLAY THE VOICE AND RECORD THE PATTERN", ui::Color::Sel}};
//   h.summary = "THE PADS PLAY THE VOICE. F1-F3 AND THE ENCODER EDIT ITS VALUES.";
//   h.buttons = kButtons;
//   h.button_count = 3;
//   h.encoder = "CHANGE THE VALUE OF THE F KEY";
//   h.groups = kGroups;
//   h.group_count = 1;
//   app::help_pads(h, 0, 0, app::kPadRows - 1, app::kPadCols - 1, 1);  // every pad is a key; give a pad_label[] to name them
// }

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
