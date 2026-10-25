// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/input.h>
#include <o3sdk/app/panel.h>

namespace o3sdk::app {

namespace {

constexpr const char* kNames[kButtonCount] = {
    "f1",     "f2",          "f3",         "f4",         "f5",        "f6",     "f7",    "f8",
    "params", "performance", "file",       "config",     "master",    "sampleload", "sampleplay", "sampleedit",
    "samplerec", "song",     "note",       "instr",      "vol",       "fx",     "pattern", "play",
    "copy",   "insert",      "up",         "delete",     "rec",       "shift",  "left",  "down",
    "right",
};

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool same(const char* a, const char* b) noexcept {
  for (; *a != '\0' && *b != '\0'; ++a, ++b) {
    if (lower(*a) != lower(*b)) {
      return false;
    }
  }
  return *a == '\0' && *b == '\0';
}

}  // namespace

const char* button_name(Button b) noexcept {
  const auto i = static_cast<unsigned>(b);
  return i < kButtonCount ? kNames[i] : "?";
}

bool find_button(const char* name, Button& out) noexcept {
  if (name == nullptr) {
    return false;
  }
  for (unsigned i = 0; i < kButtonCount; ++i) {
    if (same(name, kNames[i])) {
      out = static_cast<Button>(i);
      return true;
    }
  }
  return false;
}

unsigned Panel::pads_held() const noexcept {
  unsigned n = 0;
  for (std::uint64_t m = pads_; m != 0u; m &= m - 1u) {
    ++n;
  }
  return n;
}

void Panel::apply(const core::InputEvent& e) noexcept {
  switch (e.kind) {
    case core::InputKind::Button:
      if (e.id < kButtonCount) {
        const std::uint64_t bit = 1ull << e.id;
        buttons_ = e.value != 0 ? (buttons_ | bit) : (buttons_ & ~bit);
      }
      break;
    case core::InputKind::Pad:
      if (e.id < kPadCount) {
        const std::uint64_t bit = 1ull << e.id;
        pads_ = e.value != 0 ? (pads_ | bit) : (pads_ & ~bit);
      }
      break;
    default:
      break;
  }
}

Input make_input(const core::InputEvent& e, bool shift) noexcept {
  Input in{};
  in.kind = e.kind;
  in.id = e.id;
  in.value = e.value;
  in.time = e.timestamp;
  in.shift = shift && !(e.kind == core::InputKind::Button && e.id == static_cast<std::uint8_t>(Button::Shift));
  if (e.kind == core::InputKind::Encoder) {
    in.type = Input::Type::Turn;
  } else {
    in.type = e.value != 0 ? Input::Type::Press : Input::Type::Release;
  }
  return in;
}

}  // namespace o3sdk::app
