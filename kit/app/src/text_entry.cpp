// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/text_entry.h>

namespace o3sdk::app {

namespace {

unsigned row_of(unsigned key) noexcept {
  unsigned r = 0;
  while (r + 1 < kTextRows && key >= kTextRowStart[r + 1]) {
    ++r;
  }
  return r;
}

}  // namespace

unsigned text_file_name(const char* title, char* out, unsigned cap) noexcept {
  unsigned n = 0;
  for (; title != nullptr && *title != '\0' && n + 1u < cap; ++title) {
    const char c = *title;
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (alnum || ((c == ' ' || c == '-') && n != 0u)) {
      out[n++] = c;
    }
  }
  while (n != 0u && out[n - 1u] == ' ') {
    --n;
  }
  out[n] = '\0';
  return n;
}

void TextEntry::begin(const char* initial, unsigned cap, Filter filter) noexcept {
  filter_ = filter;
  cap_ = static_cast<std::uint8_t>(cap > kTextCapacity ? kTextCapacity : cap);
  shift_ = false;
  frame_ = false;
  key_ = kTextKeyBackspace;
  set(initial);
}

void TextEntry::set(const char* text) noexcept {
  unsigned n = 0;
  if (text != nullptr) {
    for (; text[n] != '\0' && n < cap_; ++n) {
      text_[n] = text[n];
    }
  }
  text_[n] = '\0';
  len_ = static_cast<std::uint8_t>(n);
  edit_ = len_;
  all_ = len_ != 0u;
}

bool TextEntry::key_allowed(unsigned key) const noexcept {
  if (key >= kTextKeys) {
    return false;
  }
  const char c = kTextKeysSmall[key];
  if (c == 0 || c == 1) {
    return true;  // backspace and caps
  }
  return filter_ == Filter::Any || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == ' ' || c == '-';  // ('_' and '.' are not on the keys)
}

// A character typed over the selection replaces the whole name.
void TextEntry::replace_all(char c) noexcept {
  text_[0] = c;
  text_[1] = '\0';
  len_ = 1;
  edit_ = 1;
  all_ = false;
}

bool TextEntry::type(unsigned key) noexcept {
  if (key >= kTextKeys) {
    return false;
  }
  frame_ = false;
  key_ = static_cast<std::uint8_t>(key);
  const char c = key_char(key);
  if (c == 1) {  // caps
    shift_ = !shift_;
    return true;
  }
  if (!key_allowed(key)) {
    return false;
  }
  if (c == 0) {  // backspace
    return backspace();
  }
  if (all_) {
    replace_all(c);
    return true;
  }
  if (len_ >= cap_) {
    return false;
  }
  for (unsigned i = len_; i > edit_; --i) {
    text_[i] = text_[i - 1u];
  }
  text_[edit_] = c;
  ++edit_;
  ++len_;
  text_[len_] = '\0';
  return true;
}

bool TextEntry::backspace() noexcept {
  if (all_) {  // the selection goes: the text is empty
    text_[0] = '\0';
    len_ = 0;
    edit_ = 0;
    all_ = false;
    return true;
  }
  if (edit_ == 0u) {
    return false;
  }
  for (unsigned i = edit_; i <= len_; ++i) {
    text_[i - 1u] = text_[i];
  }
  --edit_;
  --len_;
  return true;
}

bool TextEntry::move(int dx, int dy) noexcept {
  if (frame_) {
    if (dy > 0) {
      frame_ = false;
      key_ = 0;
      return true;
    }
    if (dx != 0) {
      all_ = false;
      if (dx < 0 && edit_ > 0u) {
        --edit_;
        return true;
      }
      if (dx > 0 && edit_ < len_) {
        ++edit_;
        return true;
      }
      return true;  // (the selection ended)
    }
    return false;
  }
  const unsigned row = row_of(key_);
  const unsigned width = kTextRowStart[row + 1] - kTextRowStart[row];
  const unsigned col = key_ - kTextRowStart[row];
  if (dx != 0) {
    const unsigned next = dx > 0 ? (col + 1u) % width : (col + width - 1u) % width;
    key_ = static_cast<std::uint8_t>(kTextRowStart[row] + next);
    return true;
  }
  if (dy != 0) {
    const bool up = dy < 0;
    if ((up && row == 0u) || (!up && row + 1u == kTextRows)) {
      frame_ = true;  // off the top or the bottom: into the name
      return true;
    }
    const unsigned to = up ? row - 1u : row + 1u;
    const unsigned to_width = kTextRowStart[to + 1] - kTextRowStart[to];
    key_ = static_cast<std::uint8_t>(kTextRowStart[to] + (col < to_width ? col : to_width - 1u));
    return true;
  }
  return false;
}

}  // namespace o3sdk::app
