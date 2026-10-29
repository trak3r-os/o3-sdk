// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/name_entry.h>

#include <o3sdk/ui/keyboard.h>

namespace o3sdk::app {

void NameEntry::open(const char* title, const char* initial, Kind kind, unsigned cap) noexcept {
  title_ = title != nullptr ? title : "";
  message_ = "";
  kind_ = kind;
  entry_.begin(initial != nullptr ? initial : "", cap, kind == Kind::FileName ? TextEntry::Filter::FileName : TextEntry::Filter::Any);
  active_ = true;
}

namespace {

bool alnum(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// The shape the files service's own check starts from (hal/save_guard.h valid_name, which only the save layer names): a letter or digit first, neither a space nor a dot last, no "..". The
// service still has the last word (a reserved device name such as CON is refused there, and the strip says so).
bool file_name_shape(const char* s, unsigned n) noexcept {
  if (n == 0u || !alnum(s[0]) || s[n - 1u] == ' ' || s[n - 1u] == '.') {
    return false;
  }
  for (unsigned i = 1; i < n; ++i) {
    if (s[i] == '.' && s[i - 1u] == '.') {
      return false;
    }
  }
  return true;
}

}  // namespace

bool NameEntry::acceptable() const noexcept {
  return entry_.length() != 0u && (kind_ != Kind::FileName || file_name_shape(entry_.text(), entry_.length()));
}

NameEntry::Result NameEntry::input(const Input& in) noexcept {
  if (!active_) {
    return Result::Editing;
  }
  message_ = "";
  if (in.press(Button::F8)) {
    active_ = false;
    return Result::Cancelled;
  }
  if (in.press(Button::F7)) {
    if (acceptable()) {
      active_ = false;
      return Result::Done;
    }
    message_ = entry_.length() == 0u ? "THE NAME IS EMPTY" : "NOT A NAME THE CARD TAKES";
    return Result::Editing;
  }
  if (in.turn()) {
    (void)entry_.move(in.delta() > 0 ? 1 : -1, 0);
  } else if (in.press(Button::Left)) {
    (void)entry_.move(-1, 0);
  } else if (in.press(Button::Right)) {
    (void)entry_.move(1, 0);
  } else if (in.press(Button::Up)) {
    (void)entry_.move(0, -1);
  } else if (in.press(Button::Down)) {
    (void)entry_.move(0, 1);
  } else if (in.press(Button::F1) && !in.shift) {
    (void)entry_.confirm_key();
  } else if (in.press(Button::Delete)) {
    (void)entry_.backspace();
  } else if (in.pad_press()) {
    (void)entry_.pad(in.pad());
  }
  return Result::Editing;
}

void NameEntry::draw(ui::Renderer& r, const ui::Rect& area) const noexcept {
  if (!active_) {
    return;
  }
  ui::KeyboardKeys keys;
  (void)ui::keyboard(r, area, ui::keyboard_view(entry_, kTextPadKey, keys, title_, message_));
}

void NameEntry::soft_keys(SoftKeys& keys) const noexcept {
  if (!active_) {
    return;
  }
  keys.label[0] = "TYPE";
  keys.label[6] = "OK";
  keys.label[7] = "CANCEL";
  keys.dim = acceptable() ? 0u : 1u << 6;
}

}  // namespace o3sdk::app
