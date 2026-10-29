// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/system_config.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/brightness.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/firmware_flow.h>
#include <o3sdk/ui/components.h>

#include "sys_text.h"
#include "system_pages.h"

namespace o3sdk::app {

namespace {

using ui::Color;
using ui::Face;
using ui::Rect;

using Section = SystemConfig::Section;
using Phase = FirmwareView::Phase;

// The order of the tabs.
constexpr Section kOrder[SystemConfig::kSectionCount] = {Section::Firmware, Section::Audio, Section::Midi, Section::Card, Section::Display, Section::Info};

const char* tab_label(Section s) noexcept {
  switch (s) {
    case Section::Firmware: return "FIRMWARE";
    case Section::Audio: return "AUDIO";
    case Section::Midi: return "MIDI";
    case Section::Card: return "CARD";
    case Section::Display: return "DISPLAY";
    case Section::Info: return "INFO";
    case Section::App: return "";
  }
  return "";
}

// What stops a firmware update now: the files service's job (the update path and a save exclude each other on the card, docs/M2C-PLAN.md section 3; the strip says SAVING first), then
// whatever the app says (App::update_blocker).
const char* update_blocker(const Context& ctx) noexcept { return ctx.files().busy() ? "SAVING" : ctx.app().update_blocker(ctx); }

constexpr int kMargin = 16;
constexpr int kTabH = 28;
constexpr int kTabGap = 8;

// "h:mm:ss" from seconds.
void format_uptime(std::uint64_t seconds, char* out, std::size_t cap) noexcept {
  core::FixedString<24> s;
  append_uint(s, static_cast<unsigned>(seconds / 3600u));
  s.append(":");
  append_dec(s, static_cast<unsigned>(seconds / 60u % 60u), 2);
  s.append(":");
  append_dec(s, static_cast<unsigned>(seconds % 60u), 2);
  upper_copy(out, cap, s.c_str());
}

// The files service's view of the card for the CARD section.
const char* card_state_text(Files::Card c) noexcept {
  switch (c) {
    case Files::Card::None: return "NO CARD";
    case Files::Card::Ready: return "READY";
    case Files::Card::ReadOnly: return "WRITE PROTECTED";
    case Files::Card::Unsupported: return "EXFAT: NOT SUPPORTED";
    case Files::Card::OnComputer: return "ON THE COMPUTER - EJECT FIRST";
    case Files::Card::InUse: return "IN USE (FIRMWARE UPDATE)";
  }
  return "";
}

// The three levels of DISPLAY and PAD LIGHTS.
constexpr const char* kLevelNames[kBrightnessLevels] = {"LOW", "MEDIUM", "HIGH"};

// One row of an app's page: the caption, and the value (or what an action does) at `value_x`.
void draw_app_rows(ui::Renderer& r, const PageGrid& grid, const Context& ctx, const ConfigView::Page& page, unsigned row, bool asking, std::uint16_t ask_key) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int value_x = grid.x + 260;
  const unsigned rows = page.valid();
  for (unsigned i = 0; i < rows; ++i) {
    const ConfigItem* it = page.nth(i);
    if (it == nullptr) {
      continue;
    }
    const int y = grid.y0 + static_cast<int>(i) * grid.pitch;
    const bool on = i == row;
    if (on) {
      r.highlight_row({grid.x - m.pad, y, grid.w + 2 * m.pad, grid.pitch});
    }
    char label[kConfigLabelChars + 1];
    upper_copy(label, sizeof label, it->label);
    r.text(grid.x + m.pad, ui::caps_y(t, Face::Label, y, grid.pitch), label, on ? Color::SelText : Color::TextDim, Face::Label);
    core::FixedString<40> text;
    if (it->value != nullptr) {
      char num[core::kParamTextChars + 1];
      const float v = core::param_clamp(*it->value, ctx.app().config_get(ctx, it->key));
      core::param_format(*it->value, v, num, sizeof num);
      text.assign(num);
      if (it->value->unit_text != nullptr && it->value->unit_text[0] != '\0') {
        text.append(" ");
        text.append(it->value->unit_text);
      }
    } else {
      text.assign(asking && ask_key == it->key ? "F1 AGAIN TO RUN" : "RUN");
    }
    r.text(value_x, ui::caps_y(t, Face::Data, y, grid.pitch), text.c_str(), on ? Color::SelText : Color::Text, Face::Data);
  }
  r.text(grid.x, ui::caps_y(t, Face::Label, grid.y0 + static_cast<int>(rows + 1u) * grid.pitch, grid.pitch),
         "UP / DOWN choose a row.  The encoder and F1 change a value.  An action: F1, then F1 again.", Color::TextDim, Face::Label);
}

}  // namespace

SystemConfig::SystemConfig(hal::firmware::FirmwareSink* firmware, const char* build, bool midi, bool audio, bool usb, bool card, bool backlight, bool pads, const char* crash) noexcept
    : sink_(firmware),
      build_(build),
      midi_(midi),
      audio_(audio),
      usb_(usb),
      card_(card),
      backlight_(backlight),
      pads_(pads),
      crash_(crash),
      section_(firmware != nullptr && firmware->available() ? Section::Firmware : (audio || usb ? Section::Audio : (midi ? Section::Midi : (card ? Section::Card : Section::Display)))) {}

bool SystemConfig::has(Section s) const noexcept {
  switch (s) {
    case Section::Firmware: return sink_ != nullptr && sink_->available();
    case Section::Audio: return audio_ || usb_;
    case Section::Midi: return midi_;
    case Section::Card: return card_;
    case Section::Display:
    case Section::Info:
    case Section::App: break;
  }
  return true;
}

const char* SystemConfig::section_name() const noexcept {
  switch (section_) {
    case Section::Firmware: return "firmware";
    case Section::Audio: return "audio";
    case Section::Midi: return "midi";
    case Section::Card: return "card";
    case Section::Display: return "display";
    case Section::Info: return "info";
    case Section::App: return "app";
  }
  return "-";
}

// ---- Open and close -----------------------------------------------------------------------------------------------------------------

void SystemConfig::show(Context& ctx) noexcept {
  if (open_) {
    return;
  }
  open_ = true;
  enter(ctx);
  ctx.redraw();
}

void SystemConfig::close(Context& ctx) noexcept {
  if (!open_ || locked()) {
    return;
  }
  leave_section(ctx);
  open_ = false;
  ctx.redraw();
}

// The section just shown: the FIRMWARE list is read when it comes up.
void SystemConfig::enter(Context& ctx) noexcept {
  if (section_ == Section::Firmware) {
    open_list(ctx);
  } else if (section_ == Section::Audio && audio_) {
    ctx.audio_in().watch(true);  // the level meter: the engine measures the input only while this page is up
  }
}

// The section is being left (or the screen closed): nothing the FIRMWARE section started may be left running or waiting, and the AUDIO section stops asking for the input's peaks.
void SystemConfig::leave_section(Context& ctx) noexcept {
  if (section_ == Section::Firmware) {
    (void)firmware_abandon(view_, sink_);
  } else if (section_ == Section::Audio && audio_) {
    ctx.audio_in().watch(false);
  }
  disarm();  // a change that waited for its second F1 is not asked for from another page
}

void SystemConfig::open_list(Context& ctx) noexcept {
  const char* said = nullptr;
  if (!firmware_open(view_, sink_, update_blocker(ctx), said) && said != nullptr) {
    strip(said, -1, kNoticeMs, ctx.now());
  }
}

// LEFT / RIGHT: the kit's sections in their order, then one tab for each page of the app's, round and round.
void SystemConfig::move_section(Context& ctx, int dir) noexcept {
  const unsigned pages = ctx.config_view().pages;
  const unsigned total = kSectionCount + pages;
  unsigned at = 0;
  if (section_ == Section::App) {
    at = kSectionCount + app_page_;
  } else {
    while (at < kSectionCount && kOrder[at] != section_) {
      ++at;
    }
  }
  Section to = section_;
  unsigned to_page = app_page_;
  for (unsigned n = 0; n < total; ++n) {
    at = (at + static_cast<unsigned>(dir > 0 ? 1 : total - 1u)) % total;
    if (at >= kSectionCount) {
      to = Section::App;
      to_page = at - kSectionCount;
      break;
    }
    if (has(kOrder[at])) {
      to = kOrder[at];
      break;
    }
  }
  if (to == section_ && (to != Section::App || to_page == app_page_)) {
    return;
  }
  leave_section(ctx);
  section_ = to;
  if (to == Section::App) {
    app_page_ = static_cast<std::uint8_t>(to_page);
    app_row_ = 0;
  }
  enter(ctx);
}

// F8: one level up; the last level closes the screen.
void SystemConfig::back(Context& ctx) noexcept {
  if (section_ == Section::Firmware && sink_ != nullptr && firmware_back(view_, sink_)) {
    return;
  }
  close(ctx);
}

void SystemConfig::card_changed(Context& ctx) noexcept {
  if (firmware_abandon(view_, sink_)) {
    strip("CARD CHANGED: THE FIRMWARE LIST WAS CLOSED", -1, kNoticeMs, ctx.now());
    ctx.redraw();
  }
}

// ---- Input ----------------------------------------------------------------------------------------------------------------------------

void SystemConfig::input(Context& ctx, const Input& in) noexcept {
  if (!open_ || in.type == Input::Type::Release || locked()) {
    return;  // (past INSTALL no key acts)
  }
  ctx.redraw();
  if (section_ == Section::Firmware && sink_ != nullptr && firmware_question_taken_back(view_, in)) {
    firmware_question_drop(view_, sink_);  // (the input then acts as usual in the list)
  }
  if (in.press(Button::F8)) {
    back(ctx);
    return;
  }
  bool taken = false;
  // A change that waits for its second F1 is kept only by that F1 (and by a turn of the encoder, which changes the proposal); any other control takes it back.
  if (ask_ != Ask::None && !in.press(Button::F1) && !in.turn()) {
    disarm();
  }
  if (section_ == Section::Firmware) {
    taken = firmware_input(ctx, in);
  } else if (section_ == Section::Audio) {
    taken = audio_input(ctx, in);
  } else if (section_ == Section::Midi) {
    taken = midi_input(ctx, in);
  } else if (section_ == Section::Card) {
    taken = card_input(ctx, in);
  } else if (section_ == Section::Display) {
    taken = display_input(ctx, in);
  } else if (section_ == Section::App) {
    taken = app_input(ctx, in);
  }
  if (taken) {
    return;
  }
  const bool calm = section_ != Section::Firmware || view_.phase == Phase::Closed || view_.phase == Phase::List;
  if (calm && (in.press(Button::Left) || in.press(Button::Right))) {
    move_section(ctx, in.press(Button::Right) ? 1 : -1);
  }
}

bool SystemConfig::firmware_input(Context& ctx, const Input& in) noexcept {
  if (sink_ == nullptr) {
    return false;
  }
  const bool f1 = in.press(Button::F1) && !in.shift;
  switch (view_.phase) {
    case Phase::Closed:
      if (f1) {
        open_list(ctx);
        return true;
      }
      return false;
    case Phase::List: {
      if (f1) {
        return firmware_go(view_, sink_, update_blocker(ctx));
      }
      int dir = 0;
      if (in.turn()) {
        dir = in.delta();
      } else if (in.press(Button::Up)) {
        dir = -1;
      } else if (in.press(Button::Down)) {
        dir = 1;
      }
      if (dir != 0 && view_.n != 0) {
        const int to = static_cast<int>(view_.cursor) + dir;
        view_.cursor = static_cast<std::uint8_t>(to < 0 ? 0 : (to >= static_cast<int>(view_.n) ? static_cast<int>(view_.n) - 1 : to));
        return true;
      }
      return dir != 0;
    }
    case Phase::AskUpdate:
    case Phase::AskInstall:
      return f1 && firmware_go(view_, sink_, update_blocker(ctx));
    case Phase::Checking:
    case Phase::Installing:
      return true;  // (the check has F8 only; the hand-over is not reached here)
  }
  return false;
}

// UP / DOWN move the cursor over the rows; the encoder steps the row's value (a channel by the detents, clamped), F1 steps it by one and wraps.
bool SystemConfig::midi_input(Context& ctx, const Input& in) noexcept {
  const int last = static_cast<int>(kMidiRowCount) - 1;
  int row = midi_row_;
  if (in.press(Button::Up) || in.press(Button::Down)) {
    row += in.press(Button::Down) ? 1 : -1;
    midi_row_ = static_cast<std::uint8_t>(row < 0 ? 0 : (row > last ? last : row));
    return true;
  }
  int steps = 0;
  bool wrap = false;
  if (in.turn()) {
    steps = in.delta();
  } else if (in.press(Button::F1) && !in.shift) {
    steps = 1;
    wrap = true;
  }
  if (steps == 0) {
    return false;
  }
  ctx.midi().set_config(midi_step(ctx.midi().config(), midi_row_, steps, wrap));
  return true;
}

// UP / DOWN move the cursor over the rows; the encoder steps the row's value (SHIFT: coarser), F1 steps it by one and wraps (the monitor's F1 is off / on, the radio's SEEK row
// seeks up). F5 and F6 seek down and up while the radio is the input; on a receiver that did not answer F5 tries it again. The USB set's rows (USB AUDIO, PROFILE, RETURN) only propose: the
// second F1 applies, the port reconnects (usb_row).
bool SystemConfig::audio_input(Context& ctx, const Input& in) noexcept {
  AudioIn& a = ctx.audio_in();
  Radio& radio = ctx.radio();
  const bool on_radio = audio_ && a.source() == AudioIn::Source::Radio;
  if (on_radio && (in.press(Button::F5) || in.press(Button::F6))) {
    const hal::capture::RadioStatus::State s = radio.status().state;
    if (s == hal::capture::RadioStatus::State::Absent || s == hal::capture::RadioStatus::State::Failed) {
      if (in.press(Button::F5)) {
        radio.power(true);  // the one retry: a person asked for it
      }
    } else {
      radio.seek(in.press(Button::F6));
    }
    return true;
  }
  AudioRow kinds[kAudioMaxRows];
  const unsigned rows = audio_rows(kinds, a.source(), audio_, usb_);
  const int last = static_cast<int>(rows) - 1;
  if (last < 0) {
    return false;
  }
  if (audio_row_ > last) {
    audio_row_ = static_cast<std::uint8_t>(last);
  }
  if (in.press(Button::Up) || in.press(Button::Down)) {
    const int row = static_cast<int>(audio_row_) + (in.press(Button::Down) ? 1 : -1);
    audio_row_ = static_cast<std::uint8_t>(row < 0 ? 0 : (row > last ? last : row));
    return true;
  }
  int steps = 0;
  bool wrap = false;
  const bool coarse = in.shift;
  if (in.turn()) {
    steps = in.delta();
  } else if (in.press(Button::F1) && !in.shift) {
    steps = 1;
    wrap = true;
  }
  if (steps == 0) {
    return false;
  }
  switch (kinds[audio_row_]) {
    case kAudioInput: {
      const bool radio_ok = radio.status().state != hal::capture::RadioStatus::State::Absent;
      const AudioIn::Source to = audio_step_source(a.source(), steps, wrap, radio_ok, usb_);
      if (to != a.source()) {
        (void)a.select(to, a.gain());
      }
      break;
    }
    case kAudioGain: {
      const unsigned to = audio_step_percent(a.gain(), coarse ? steps * 5 : steps, wrap);
      if (to != a.gain()) {
        (void)a.set_gain(to);
      }
      break;
    }
    case kAudioMonitor:
      if (wrap) {  // F1: off, or on at the usual level
        a.monitor(a.monitor() == 0u ? AudioIn::kMonitorOn : 0u);
      } else {
        a.monitor(audio_step_percent(a.monitor(), coarse ? steps * 5 : steps, false));
      }
      break;
    case kAudioFreq:
      radio.step(coarse ? steps * 10 : steps);
      break;
    case kAudioRegion: {
      const int count = static_cast<int>(hal::capture::kRegionCount);
      int to = static_cast<int>(radio.region()) + (steps > 0 ? 1 : -1);
      to = wrap ? (to + count) % count : (to < 0 ? 0 : (to >= count ? count - 1 : to));
      radio.set_region(static_cast<Radio::Region>(to));
      break;
    }
    case kAudioSeek:
      radio.seek(steps > 0);
      break;
    case kAudioUsb:
    case kAudioProfile:
    case kAudioReturn:
      return usb_row(ctx, kinds[audio_row_], steps, wrap);
    case kAudioReturnLevel: {
      UsbAudio& u = ctx.usb_audio();
      const unsigned to = audio_step_percent(u.return_level(), coarse ? steps * 5 : steps, wrap);
      if (to != u.return_level()) {
        u.set_return_level(to);
      }
      break;
    }
    default: break;
  }
  return true;
}

// The USB set's rows: a move proposes (the row shows it and says what the second F1 does), the second F1 asks the port for it. `f1` is the F1 press (it wraps round and confirms); a turn of the
// encoder changes the proposal, and a proposal that is the layout in use again is dropped.
bool SystemConfig::usb_row(Context& ctx, unsigned kind, int steps, bool f1) noexcept {
  UsbAudio& u = ctx.usb_audio();
  if (u.changing()) {
    strip("THE USB PORT IS RECONNECTING", -1, kNoticeMs, ctx.now());
    return true;
  }
  if (ask_ == Ask::Layout && f1) {
    const bool ok = u.request(want_audio_, static_cast<hal::usb_audio::Profile>(want_profile_), want_duplex_);
    disarm();
    strip(ok ? "THE USB PORT RECONNECTS: THE COMPUTER FINDS THE UNIT AGAIN" : "THE USB PORT CANNOT CHANGE NOW (THE CARD IS OUT OR A CHANGE RUNS)", -1, kNoticeMs, ctx.now());
    return true;
  }
  if (ask_ != Ask::Layout) {
    want_audio_ = u.audio();
    want_profile_ = static_cast<std::uint8_t>(u.profile());
    want_duplex_ = u.duplex();
  }
  if (kind == kAudioUsb) {
    want_audio_ = !want_audio_;
  } else if (kind == kAudioProfile) {
    want_profile_ = static_cast<std::uint8_t>(audio_step_profile(static_cast<hal::usb_audio::Profile>(want_profile_), steps, f1));
  } else {
    want_duplex_ = !want_duplex_;
  }
  const bool differs = want_audio_ != u.audio() || want_profile_ != static_cast<std::uint8_t>(u.profile()) || want_duplex_ != u.duplex();
  ask_ = differs ? Ask::Layout : Ask::None;
  ask_until_ = ctx.now() + core::ticks_from_ms(kAskMs);
  return true;
}

// The CARD section: UP / DOWN choose a row; F1 on CARD AS DISK asks (the row says what the second F1 does), F1 or the encoder on READ ONLY switches it.
bool SystemConfig::card_input(Context& ctx, const Input& in) noexcept {
  const int last = static_cast<int>(kCardBaseRows + (usb_ ? kCardUsbRows : 0u)) - 1;
  if (card_row_ > last) {
    card_row_ = static_cast<std::uint8_t>(last);
  }
  if (in.press(Button::Up) || in.press(Button::Down)) {
    const int row = static_cast<int>(card_row_) + (in.press(Button::Down) ? 1 : -1);
    card_row_ = static_cast<std::uint8_t>(row < 0 ? 0 : (row > last ? last : row));
    disarm();
    return true;
  }
  if (!usb_) {
    return false;
  }
  UsbAudio& u = ctx.usb_audio();
  const bool f1 = in.press(Button::F1) && !in.shift;
  if (card_row_ == kCardDisk && (f1 || in.turn())) {
    if (!f1) {
      disarm();
      return true;
    }
    const hal::usb_audio::DiskStatus d = u.disk();
    if (ask_ == Ask::Share || ask_ == Ask::Eject) {  // the second F1
      const bool share = ask_ == Ask::Share;
      disarm();
      if (share) {
        const hal::usb_audio::DiskRefusal refusal = u.share_disk();
        strip(refusal == hal::usb_audio::DiskRefusal::None ? "GIVING THE CARD TO THE COMPUTER: THE USB PORT RECONNECTS" : hal::usb_audio::disk_refusal_text(refusal), -1, kNoticeMs, ctx.now());
      } else {
        strip(u.eject_disk() ? "EJECTING: THE CARD COMES BACK" : "THE DISK IS NOT OUT", -1, kNoticeMs, ctx.now());
      }
      return true;
    }
    // The first F1: refuse at once what the second would be refused for, so that nobody confirms a share that cannot happen.
    const char* refused = nullptr;
    if (d.phase == hal::usb_audio::DiskPhase::Working) {
      refused = "A HAND-OVER IS RUNNING";
    } else if (d.phase == hal::usb_audio::DiskPhase::Idle) {
      if (!d.available) {
        refused = hal::usb_audio::disk_refusal_text(hal::usb_audio::DiskRefusal::Unavailable);
      } else if (!d.card_present) {
        refused = hal::usb_audio::disk_refusal_text(hal::usb_audio::DiskRefusal::NoCard);
      } else if (d.busy || ctx.files().busy()) {
        refused = hal::usb_audio::disk_refusal_text(hal::usb_audio::DiskRefusal::Busy);
      } else if (u.changing()) {
        refused = hal::usb_audio::disk_refusal_text(hal::usb_audio::DiskRefusal::Layout);
      }
    }
    if (refused != nullptr) {
      strip(refused, -1, kNoticeMs, ctx.now());
      return true;
    }
    ask_ = d.phase == hal::usb_audio::DiskPhase::Out ? Ask::Eject : Ask::Share;
    ask_until_ = ctx.now() + core::ticks_from_ms(kAskMs);
    return true;
  }
  if (card_row_ == kCardReadOnly && (f1 || in.turn())) {
    if (!u.set_read_only(!u.read_only())) {
      strip("READ ONLY IS FIXED WHILE THE CARD IS OUT", -1, kNoticeMs, ctx.now());
    }
    return true;
  }
  return false;
}

// With the theme alone (no backlight, no pad LEDs) the encoder, UP / DOWN and F1 change it, as they always did; with BRIGHTNESS or PAD LIGHTS UP / DOWN choose a row and the encoder and F1 change it.
bool SystemConfig::display_input(Context& ctx, const Input& in) noexcept {
  const unsigned rows = display_rows();
  if (display_row_ >= rows) {
    display_row_ = static_cast<std::uint8_t>(rows - 1u);
  }
  if (rows > 1u && (in.press(Button::Up) || in.press(Button::Down))) {
    const int row = static_cast<int>(display_row_) + (in.press(Button::Down) ? 1 : -1);
    display_row_ = static_cast<std::uint8_t>(row < 0 ? 0 : (row >= static_cast<int>(rows) ? static_cast<int>(rows) - 1 : row));
    return true;
  }
  int steps = 0;
  bool wrap = false;
  if (in.turn()) {
    steps = in.delta();
  } else if (in.press(Button::F1) && !in.shift) {
    steps = 1;
    wrap = true;
  } else if (rows == 1u && in.press(Button::Down)) {
    steps = 1;
  } else if (rows == 1u && in.press(Button::Up)) {
    steps = -1;
  }
  if (steps == 0) {
    return false;
  }
  step_display(ctx, rows == 1u ? 0u : display_row_, steps, wrap);
  return true;
}

// Row 0 is the theme, then BRIGHTNESS (with a backlight), then PAD LIGHTS (with pad LEDs). A level moves by the encoder's detents, clamped, or by one and round with F1.
void SystemConfig::step_display(Context& ctx, unsigned row, int steps, bool wrap) noexcept {
  if (row == 0u) {
    step_theme(ctx, steps > 0 ? 1 : -1);
    return;
  }
  const bool brightness = backlight_ && row == 1u;
  const int now = static_cast<int>(brightness ? ctx.brightness() : ctx.pad_lights());
  int next = now + steps;
  if (wrap) {
    next = (next + static_cast<int>(kBrightnessLevels)) % static_cast<int>(kBrightnessLevels);
  } else {
    next = next < 0 ? 0 : (next >= static_cast<int>(kBrightnessLevels) ? static_cast<int>(kBrightnessLevels) - 1 : next);
  }
  if (brightness) {
    ctx.set_brightness(static_cast<unsigned>(next));
  } else {
    ctx.set_pad_lights(static_cast<unsigned>(next));
  }
}

// An app's page: UP / DOWN choose a row; the encoder and F1 change a value (SHIFT: coarse; F1 goes round at the top); F1 on an action asks, the next F1 runs it. Anything else takes the question back.
bool SystemConfig::app_input(Context& ctx, const Input& in) noexcept {
  const ConfigView& view = ctx.config_view();
  if (app_page_ >= view.pages) {
    return false;
  }
  const ConfigView::Page& page = view.page[app_page_];
  const unsigned rows = page.valid();
  if (rows == 0u) {
    return false;
  }
  if (app_row_ >= rows) {
    app_row_ = static_cast<std::uint8_t>(rows - 1u);
  }
  if (in.press(Button::Up) || in.press(Button::Down)) {
    const int row = static_cast<int>(app_row_) + (in.press(Button::Down) ? 1 : -1);
    app_row_ = static_cast<std::uint8_t>(row < 0 ? 0 : (row >= static_cast<int>(rows) ? static_cast<int>(rows) - 1 : row));
    disarm();
    return true;
  }
  const ConfigItem* item = page.nth(app_row_);
  if (item == nullptr) {
    return false;
  }
  const bool f1 = in.press(Button::F1) && !in.shift;
  if (item->value == nullptr) {  // an action
    if (in.turn()) {
      disarm();
      return true;
    }
    if (!f1) {
      return false;
    }
    if (ask_ == Ask::Action && ask_key_ == item->key) {  // the second F1
      disarm();
      ctx.app().config_run(ctx, item->key);
      strip("DONE", -1, kNoticeMs, ctx.now());
    } else {
      ask_ = Ask::Action;
      ask_key_ = item->key;
      ask_until_ = ctx.now() + core::ticks_from_ms(kAskMs);
    }
    return true;
  }
  int steps = 0;
  if (in.turn()) {
    steps = in.delta();
  } else if (f1) {
    steps = 1;
  }
  if (steps == 0) {
    return false;
  }
  const core::ParamDesc& d = *item->value;
  const float now = core::param_clamp(d, ctx.app().config_get(ctx, item->key));
  const int mult = in.turn() && in.shift && d.coarse > 1u ? static_cast<int>(d.coarse) : 1;
  float next = core::param_step(d, now, steps * mult);
  if (f1 && next == now) {
    next = d.min;  // F1 at the top goes round
  }
  if (next != now) {
    ctx.app().config_set(ctx, item->key, next);
  }
  return true;
}

void SystemConfig::step_theme(Context& ctx, int dir) noexcept {
  const unsigned now = static_cast<unsigned>(ctx.theme());
  const unsigned next = (now + (dir > 0 ? 1u : ui::kThemeCount - 1u)) % ui::kThemeCount;
  ctx.set_theme(ui::theme_id_at(next));
}

// ---- Every pass -----------------------------------------------------------------------------------------------------------------

void SystemConfig::update(Context& ctx) noexcept {
  if (strip_on_ && core::tick_reached(ctx.now(), strip_until_)) {
    strip_on_ = false;
  }
  if (ask_ != Ask::None && core::tick_reached(ctx.now(), ask_until_)) {
    ask_ = Ask::None;  // nobody confirmed
    ctx.redraw();
  }
  if (sink_ == nullptr || !view_.open()) {
    return;
  }
  if (view_.phase == Phase::Installing && !(open_ && section_ == Section::Firmware)) {
    open_ = true;  // past INSTALL the page cannot be left: put it back
    section_ = Section::Firmware;
  }
  const bool shown = open_ && section_ == Section::Firmware;
  if (firmware_poll(view_, *sink_, ctx.now(), shown)) {
    ctx.redraw();
  }
}

// ---- The strip ---------------------------------------------------------------------------------------------------------------------

void SystemConfig::strip(const char* text, int progress, std::uint32_t hold_ms, core::Tick now) noexcept {
  if (text == nullptr || text[0] == '\0') {
    strip_on_ = false;
    return;
  }
  strip_text_.assign(text);
  strip_progress_ = progress >= 0 && progress <= 1000 ? progress : -1;
  strip_until_ = now + core::ticks_from_ms(hold_ms);
  strip_on_ = true;
}

void SystemConfig::draw_strip(ui::Renderer& r, const Rect& area) const noexcept {
  if (!strip_on_) {
    return;
  }
  const ui::Theme& t = r.theme();
  ui::strip(r, area, ui::Edge::Top);
  const int y = ui::caps_y(t, Face::Data, area.y, area.h);
  r.text(kMargin, y, strip_text_.c_str(), Color::ChromeText, Face::Data);
  if (strip_progress_ >= 0) {
    const int text_w = r.text_width(strip_text_.c_str(), Face::Data);
    const int x = kMargin + text_w + 24;
    const int w = area.x + area.w - kMargin - x;
    if (w > 16) {
      const int pad = t.metrics.gap;
      ui::bar(r, {x, area.y + pad + 2, w, area.h - 2 * pad - 4}, static_cast<unsigned>(strip_progress_), 1000u, Color::ChromeDim, Color::ChromeText);
    }
  }
}

// ---- Drawing --------------------------------------------------------------------------------------------------------------------

void SystemConfig::draw(const Context& ctx, ui::Renderer& r, const Rect& area) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  // The tabs.
  const int tab_y = area.y + 8;
  int x = kMargin;
  for (const Section s : kOrder) {
    if (!has(s)) {
      continue;
    }
    const char* label = tab_label(s);
    const int w = r.text_width(label, Face::Data) + 4 * m.pad;
    const bool on = s == section_;
    if (on) {
      r.panel({x, tab_y, w, kTabH}, Color::Sel);
    }
    r.text(x + 2 * m.pad, ui::caps_y(t, Face::Data, tab_y, kTabH), label, on ? Color::SelText : Color::TextDim, Face::Data);
    x += w + kTabGap;
  }
  const ConfigView& cv = ctx.config_view();
  for (unsigned p = 0; p < cv.pages; ++p) {  // the app's pages, one tab each
    char label[kConfigLabelChars + 1];
    upper_copy(label, sizeof label, cv.page[p].title);
    const int w = r.text_width(label, Face::Data) + 4 * m.pad;
    if (x + w > area.w - kMargin) {
      break;
    }
    const bool on = section_ == Section::App && app_page_ == p;
    if (on) {
      r.panel({x, tab_y, w, kTabH}, Color::Sel);
    }
    r.text(x + 2 * m.pad, ui::caps_y(t, Face::Data, tab_y, kTabH), label, on ? Color::SelText : Color::TextDim, Face::Data);
    x += w + kTabGap;
  }
  const int rule_y = tab_y + kTabH + 4;
  r.rule(kMargin, rule_y, area.w - 2 * kMargin, ui::Axis::Horizontal, Color::Empty);

  const PageGrid grid{kMargin, area.w - 2 * kMargin, rule_y + 12, m.row_pitch};
  switch (section_) {
    case Section::Firmware:
      draw_firmware_page(r, grid, view_);
      break;
    case Section::Audio: {
      const AudioIn& a = ctx.audio_in();
      const UsbAudio& u = ctx.usb_audio();
      const bool proposed = ask_ == Ask::Layout;
      const UsbView usb{proposed ? want_audio_ : u.audio(),
                        proposed ? static_cast<hal::usb_audio::Profile>(want_profile_) : u.profile(),
                        proposed ? want_duplex_ : u.duplex(),
                        u.return_level(),
                        u.streaming(),
                        u.return_streaming(),
                        u.changing(),
                        proposed};
      AudioRow kinds[kAudioMaxRows];
      const unsigned rows = audio_rows(kinds, a.source(), audio_, usb_);
      const AudioView view{audio_, usb_, usb, a.source(), a.gain(), a.monitor(), ctx.radio().status(), a.meter(), a.settling(), a.failed(), audio_row_ < rows ? audio_row_ : (rows != 0u ? rows - 1u : 0u)};
      draw_audio_page(r, grid, view);
      break;
    }
    case Section::Card: {
      const Files& f = ctx.files();
      const hal::usb_audio::DiskStatus disk = usb_ ? ctx.usb_audio().disk() : hal::usb_audio::DiskStatus{};
      const CardAsk ask = ask_ == Ask::Share ? CardAsk::Share : (ask_ == Ask::Eject ? CardAsk::Eject : CardAsk::None);
      const unsigned rows = kCardBaseRows + (usb_ ? kCardUsbRows : 0u);
      const CardView view{card_state_text(f.card()), f.status().text != nullptr ? f.status().text : "", f.volume(), usb_, disk, ctx.usb_audio().read_only(), ask, card_row_ < rows ? card_row_ : rows - 1u};
      draw_card_page(r, grid, view);
      break;
    }
    case Section::Midi:
      draw_midi_page(r, grid, ctx.midi().config(), ctx.midi().status(), midi_row_);
      break;
    case Section::Display: {
      const unsigned rows = display_rows();
      const unsigned cursor = display_row_ < rows ? display_row_ : rows - 1u;
      const int block = 2 * grid.pitch + 12;  // a row: its caption line, a line of chips and a gap
      // Each row is a caption line and a line of chips: THEME, then BRIGHTNESS and PAD LIGHTS where there is something to set.
      const auto caption = [&](unsigned row, const char* text) {
        const int y = grid.y0 + static_cast<int>(row) * block;
        const bool on = rows > 1u && row == cursor;
        if (on) {
          r.highlight_row({grid.x - m.pad, y, grid.w + 2 * m.pad, grid.pitch});
        }
        r.text(grid.x + (on ? m.pad : 0), ui::caps_y(t, Face::Label, y, grid.pitch), text, on ? Color::SelText : Color::TextDim, Face::Label);
      };
      const auto chip = [&](unsigned row, int& cx, const char* name, bool on) {
        const int w = r.text_width(name, Face::Data) + 4 * m.pad;
        const Rect box{cx, grid.y0 + static_cast<int>(row) * block + grid.pitch, w, kTabH};
        if (on) {
          r.panel(box, Color::Sel);
        } else {
          r.frame(box, Color::Empty);
        }
        r.text(cx + 2 * m.pad, ui::caps_y(t, Face::Data, box.y, box.h), name, on ? Color::SelText : Color::Text, Face::Data);
        cx += w + kTabGap;
      };
      caption(0, "THEME");
      int cx = grid.x;
      for (unsigned i = 0; i < ui::kThemeCount; ++i) {
        char name[16];
        upper_copy(name, sizeof name, ui::theme(ui::theme_id_at(i)).name);
        chip(0, cx, name, i == static_cast<unsigned>(ctx.theme()));
      }
      unsigned row = 1;
      for (unsigned which = 0; which < 2u; ++which) {
        if (which == 0u ? !backlight_ : !pads_) {
          continue;
        }
        caption(row, which == 0u ? "BRIGHTNESS" : "PAD LIGHTS");
        cx = grid.x;
        const unsigned level = which == 0u ? ctx.brightness() : ctx.pad_lights();
        for (unsigned i = 0; i < kBrightnessLevels; ++i) {
          chip(row, cx, kLevelNames[i], i == level);
        }
        ++row;
      }
      r.text(grid.x, ui::caps_y(t, Face::Label, grid.y0 + static_cast<int>(rows) * block + 4, grid.pitch),
             rows > 1u ? "UP / DOWN choose a row. The encoder and F1 change it. It shows at once." : "The encoder, UP / DOWN and F1 change the theme. It shows at once.", Color::TextDim,
             Face::Label);
      break;
    }
    case Section::App:
      if (app_page_ < cv.pages) {
        draw_app_rows(r, grid, ctx, cv.page[app_page_], app_row_, ask_ == Ask::Action, ask_key_);
      }
      break;
    case Section::Info: {
      const int value_x = grid.x + 120;
      int row = 0;
      const auto line = [&](const char* caption, const char* value) {
        r.text(grid.x, ui::caps_y(t, Face::Label, grid.y0 + row * grid.pitch, grid.pitch), caption, Color::TextDim, Face::Label);
        r.text(value_x, ui::caps_y(t, Face::Data, grid.y0 + row * grid.pitch, grid.pitch), value, Color::Text, Face::Data);
        ++row;
      };
      const AppInfo& info = ctx.app().info();
      line("APP", info.name);
      line("VERSION", info.version);
      if (build_ != nullptr && build_[0] != '\0') {
        line("BUILD", build_);
      }
      char up[24];
      format_uptime(ctx.uptime_ms() / 1000u, up, sizeof up);
      line("UPTIME", up);
      line("FIRMWARE", has(Section::Firmware) ? "SWITCH FROM THE CARD" : "NO UPDATE PATH IN THIS IMAGE");
      if (crash_ != nullptr) {
        line("LAST CRASH", crash_);
      }
      break;
    }
  }
  const char* foot = locked() ? "No key acts until the unit has restarted." : "CONFIG closes this screen.   LEFT / RIGHT change section.   F8 goes back.";
  r.text(kMargin, ui::caps_y(t, Face::Label, area.y + area.h - m.row_pitch, m.row_pitch), foot, Color::TextDim, Face::Label);
}

void SystemConfig::soft_keys(const Context& ctx, SoftKeys& keys) const noexcept {
  keys.label[7] = "BACK";
  switch (section_) {
    case Section::Firmware:
      switch (view_.phase) {
        case Phase::Closed:
          keys.label[0] = sink_ != nullptr ? "LIST" : nullptr;
          keys.label[7] = "CLOSE";
          break;
        case Phase::List:
          keys.label[0] = "UPDATE";
          keys.dim = view_.n == 0 ? 1u : 0u;
          break;
        case Phase::AskUpdate:
          keys.label[0] = "YES";
          keys.hot = 1u;
          break;
        case Phase::Checking:
          keys.label[7] = "CANCEL";
          break;
        case Phase::AskInstall:
          keys.label[0] = "INSTALL";
          keys.hot = 1u;
          break;
        case Phase::Installing:
          keys.label[7] = nullptr;
          break;
      }
      break;
    case Section::Audio:
      keys.label[0] = ask_ == Ask::Layout ? "APPLY" : "NEXT";
      keys.hot = ask_ == Ask::Layout ? 1u : 0u;
      if (audio_ && ctx.audio_in().source() == AudioIn::Source::Radio) {
        const hal::capture::RadioStatus::State s = ctx.radio().status().state;
        const bool dead = s == hal::capture::RadioStatus::State::Absent || s == hal::capture::RadioStatus::State::Failed;
        keys.label[4] = dead ? "RETRY" : "SEEK -";
        keys.label[5] = dead ? nullptr : "SEEK +";
      }
      keys.label[7] = "CLOSE";
      break;
    case Section::Card:
      keys.label[0] = ask_ != Ask::None ? "CONFIRM" : "CHANGE";
      keys.hot = ask_ != Ask::None ? 1u : 0u;
      keys.label[7] = "CLOSE";
      break;
    case Section::Midi:
    case Section::Display:
      keys.label[0] = "NEXT";
      keys.label[7] = "CLOSE";
      break;
    case Section::App: {
      const ConfigView& view = ctx.config_view();
      const ConfigItem* item = app_page_ < view.pages ? view.page[app_page_].nth(app_row_) : nullptr;
      const bool action = item != nullptr && item->value == nullptr;
      keys.label[0] = action ? (ask_ == Ask::Action ? "CONFIRM" : "RUN") : "NEXT";
      keys.hot = ask_ == Ask::Action ? 1u : 0u;
      keys.label[7] = "CLOSE";
      break;
    }
    case Section::Info:
      keys.label[7] = "CLOSE";
      break;
  }
}

}  // namespace o3sdk::app
