// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/system_config.h>

#include <o3sdk/app/app.h>
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

// The order of the tabs (the plan puts MIDI between the first two when the kit has it).
constexpr Section kOrder[SystemConfig::kSectionCount] = {Section::Firmware, Section::Display, Section::Info};

const char* tab_label(Section s) noexcept {
  switch (s) {
    case Section::Firmware: return "FIRMWARE";
    case Section::Display: return "DISPLAY";
    case Section::Info: return "INFO";
  }
  return "";
}

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

}  // namespace

SystemConfig::SystemConfig(hal::firmware::FirmwareSink* firmware, const char* build) noexcept
    : sink_(firmware), build_(build), section_(firmware != nullptr && firmware->available() ? Section::Firmware : Section::Display) {}

bool SystemConfig::has(Section s) const noexcept { return s != Section::Firmware || (sink_ != nullptr && sink_->available()); }

const char* SystemConfig::section_name() const noexcept {
  switch (section_) {
    case Section::Firmware: return "firmware";
    case Section::Display: return "display";
    case Section::Info: return "info";
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
  leave_section();
  open_ = false;
  ctx.redraw();
}

// The section just shown: the FIRMWARE list is read when it comes up.
void SystemConfig::enter(Context& ctx) noexcept {
  if (section_ == Section::Firmware) {
    open_list(ctx);
  }
}

// The section is being left (or the screen closed): nothing the FIRMWARE section started may be left running or waiting.
void SystemConfig::leave_section() noexcept {
  if (section_ == Section::Firmware) {
    (void)firmware_abandon(view_, sink_);
  }
}

void SystemConfig::open_list(Context& ctx) noexcept {
  const char* said = nullptr;
  if (!firmware_open(view_, sink_, ctx.app().update_blocker(ctx), said) && said != nullptr) {
    strip(said, -1, kNoticeMs, ctx.now());
  }
}

void SystemConfig::move_section(Context& ctx, int dir) noexcept {
  unsigned at = 0;
  while (at < kSectionCount && kOrder[at] != section_) {
    ++at;
  }
  for (unsigned n = 0; n < kSectionCount; ++n) {
    at = (at + static_cast<unsigned>(dir > 0 ? 1 : kSectionCount - 1u)) % kSectionCount;
    if (has(kOrder[at])) {
      break;
    }
  }
  if (kOrder[at] == section_) {
    return;
  }
  leave_section();
  section_ = kOrder[at];
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
  if (section_ == Section::Firmware) {
    taken = firmware_input(ctx, in);
  } else if (section_ == Section::Display) {
    taken = display_input(ctx, in);
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
        return firmware_go(view_, sink_, ctx.app().update_blocker(ctx));
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
      return f1 && firmware_go(view_, sink_, ctx.app().update_blocker(ctx));
    case Phase::Checking:
    case Phase::Installing:
      return true;  // (the check has F8 only; the hand-over is not reached here)
  }
  return false;
}

bool SystemConfig::display_input(Context& ctx, const Input& in) noexcept {
  int dir = 0;
  if (in.turn()) {
    dir = in.delta() > 0 ? 1 : -1;
  } else if (in.press(Button::Down) || (in.press(Button::F1) && !in.shift)) {
    dir = 1;
  } else if (in.press(Button::Up)) {
    dir = -1;
  }
  if (dir == 0) {
    return false;
  }
  step_theme(ctx, dir);
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
  const int rule_y = tab_y + kTabH + 4;
  r.rule(kMargin, rule_y, area.w - 2 * kMargin, ui::Axis::Horizontal, Color::Empty);

  const PageGrid grid{kMargin, area.w - 2 * kMargin, rule_y + 12, m.row_pitch};
  switch (section_) {
    case Section::Firmware:
      draw_firmware_page(r, grid, view_);
      break;
    case Section::Display: {
      r.text(grid.x, ui::caps_y(t, Face::Label, grid.y0, grid.pitch), "THEME", Color::TextDim, Face::Label);
      int cx = grid.x;
      for (unsigned i = 0; i < ui::kThemeCount; ++i) {
        char name[16];
        upper_copy(name, sizeof name, ui::theme(ui::theme_id_at(i)).name);
        const int w = r.text_width(name, Face::Data) + 4 * m.pad;
        const bool on = i == static_cast<unsigned>(ctx.theme());
        const Rect chip{cx, grid.y0 + grid.pitch, w, kTabH};
        if (on) {
          r.panel(chip, Color::Sel);
        } else {
          r.frame(chip, Color::Empty);
        }
        r.text(cx + 2 * m.pad, ui::caps_y(t, Face::Data, chip.y, chip.h), name, on ? Color::SelText : Color::Text, Face::Data);
        cx += w + kTabGap;
      }
      r.text(grid.x, ui::caps_y(t, Face::Label, grid.y0 + 3 * grid.pitch, grid.pitch), "The encoder, UP / DOWN and F1 change the theme. It shows at once.", Color::TextDim, Face::Label);
      break;
    }
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
      break;
    }
  }
  const char* foot = locked() ? "No key acts until the unit has restarted." : "CONFIG closes this screen.   LEFT / RIGHT change section.   F8 goes back.";
  r.text(kMargin, ui::caps_y(t, Face::Label, area.y + area.h - m.row_pitch, m.row_pitch), foot, Color::TextDim, Face::Label);
}

void SystemConfig::soft_keys(const Context&, SoftKeys& keys) const noexcept {
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
    case Section::Display:
      keys.label[0] = "NEXT";
      keys.label[7] = "CLOSE";
      break;
    case Section::Info:
      keys.label[7] = "CLOSE";
      break;
  }
}

}  // namespace o3sdk::app
