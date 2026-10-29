// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/file_picker.h>

#include <o3sdk/ui/components.h>

#include <cstring>

namespace o3sdk::app {

namespace {

constexpr char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Does `name` end in ".<ext>", ignoring case?
bool has_ext(const char* name, const char* ext) noexcept {
  const std::size_t n = std::strlen(name);
  const std::size_t e = std::strlen(ext);
  if (e == 0u || n <= e + 1u || name[n - e - 1u] != '.') {
    return false;
  }
  for (std::size_t i = 0; i < e; ++i) {
    if (lower(name[n - e + i]) != lower(ext[i])) {
      return false;
    }
  }
  return true;
}

void copy_text(char* out, std::size_t cap, const char* in) noexcept {
  std::size_t i = 0;
  for (; in != nullptr && in[i] != '\0' && i + 1u < cap; ++i) {
    out[i] = in[i];
  }
  out[i] = '\0';
}

}  // namespace

bool FilePicker::open(Context& ctx, const char* title, const char* folder, const char* ext, bool folders) noexcept {
  title_ = title != nullptr ? title : "";
  copy_text(folder_, sizeof folder_, folder);
  copy_text(ext_, sizeof ext_, ext);
  folders_ = folders;
  n_ = 0;
  truncated_ = false;
  chosen_ = Files::Entry{};
  failure_ = "";
  view_ = ui::ListView(view_.rows());
  if (entries_ == nullptr || cap_ == 0u) {
    state_ = State::Failed;
    failure_ = "NO MEMORY FOR THE LISTING";
    return false;
  }
  generation_ = ctx.files().generation();
  state_ = State::Waiting;
  update(ctx);  // (a free card starts the listing at once; a busy one is waited for)
  return state_ != State::Failed;
}

void FilePicker::close() noexcept { state_ = State::Closed; }

// Waiting: ask for the listing until the service takes it. Listing: follow its job. A card that changed fails the picker.
void FilePicker::update(Context& ctx) noexcept {
  if (state_ == State::Closed || state_ == State::Failed) {
    return;
  }
  Files& f = ctx.files();
  if (f.generation() != generation_ && state_ != State::Waiting) {
    state_ = State::Failed;
    failure_ = "CARD CHANGED";
    return;
  }
  if (state_ == State::Waiting) {
    if (f.busy()) {
      return;  // the card's own job first
    }
    generation_ = f.generation();
    if (f.list(folder_, entries_, cap_)) {
      state_ = State::Listing;
    } else {
      state_ = State::Failed;  // refused now (no card, a name the guard refuses, ...): the service said why
      failure_ = f.status().text != nullptr && f.status().text[0] != '\0' ? f.status().text : "CANNOT LIST";
    }
    return;
  }
  if (state_ == State::Listing) {
    settle(ctx);
  }
}

// The listing job ended: keep what the filter keeps, in order.
void FilePicker::settle(Context& ctx) noexcept {
  const Files::Status st = ctx.files().status();
  if (st.phase == Files::Status::Running) {
    return;
  }
  if (st.phase == Files::Status::Failed) {
    state_ = State::Failed;
    failure_ = st.text != nullptr && st.text[0] != '\0' ? st.text : "CANNOT LIST";
    return;
  }
  const std::size_t listed = ctx.files().listed() < cap_ ? ctx.files().listed() : cap_;
  truncated_ = ctx.files().list_truncated();
  unsigned kept = 0;
  for (std::size_t i = 0; i < listed; ++i) {
    const Files::Entry& e = entries_[i];
    const bool keep = e.folder ? folders_ : (ext_[0] == '\0' || has_ext(e.name, ext_));
    if (keep) {
      if (kept != i) {
        entries_[kept] = e;
      }
      ++kept;
    }
  }
  n_ = kept;
  view_.fit(*this);
  state_ = State::Ready;
  ctx.redraw();
}

FilePicker::Result FilePicker::input(Context& ctx, const Input& in) noexcept {
  if (state_ == State::Closed) {
    return Result::Browsing;
  }
  ctx.redraw();
  if (in.press(Button::F8)) {
    close();
    return Result::Cancelled;
  }
  if (state_ != State::Ready) {
    return Result::Browsing;
  }
  if (in.turn()) {
    view_.move(*this, in.delta());
  } else if (in.press(Button::Up)) {
    view_.move(*this, -1);
  } else if (in.press(Button::Down)) {
    view_.move(*this, 1);
  } else if (in.press(Button::Left)) {
    view_.page(*this, -1);
  } else if (in.press(Button::Right)) {
    view_.page(*this, 1);
  } else if (in.pad_press()) {
    view_.pad(*this, in.pad());
  } else if (in.press(Button::F1) && !in.shift && n_ != 0u) {
    chosen_ = entries_[view_.cursor()];
    close();
    return Result::Chosen;
  }
  return Result::Browsing;
}

void FilePicker::name(unsigned index, char* out, unsigned cap) const noexcept {
  if (cap == 0u) {
    return;
  }
  if (index >= n_) {
    out[0] = '\0';
    return;
  }
  copy_text(out, cap, entries_[index].name);
}

// A folder says so; a file its size, in bytes up to 9999, then kilobytes and megabytes; a name only the .bak is left of says RECOVER.
void FilePicker::value(unsigned index, char* out, unsigned cap) const noexcept {
  if (cap == 0u) {
    return;
  }
  out[0] = '\0';
  if (index >= n_) {
    return;
  }
  const Files::Entry& e = entries_[index];
  if (e.folder) {
    copy_text(out, cap, "FOLDER");
    return;
  }
  if (e.recover) {
    copy_text(out, cap, "RECOVER");
    return;
  }
  std::uint32_t v = e.size;
  char unit = 'B';
  if (v >= 10000u) {
    v /= 1024u;
    unit = 'K';
    if (v >= 10000u) {
      v /= 1024u;
      unit = 'M';
    }
  }
  char digits[12];
  unsigned d = 0;
  do {
    digits[d++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && d < sizeof digits);
  unsigned o = 0;
  while (d != 0u && o + 2u < cap) {
    out[o++] = digits[--d];
  }
  if (o + 1u < cap) {
    out[o++] = unit;
  }
  out[o] = '\0';
}

void FilePicker::draw(ui::Renderer& r, const ui::Rect& area) noexcept {
  if (state_ == State::Closed) {
    return;
  }
  const ui::Theme& t = r.theme();
  const int pitch = t.metrics.row_pitch;
  r.text(area.x + 8, ui::caps_y(t, ui::Face::Label, area.y, pitch), title_, ui::Color::TextDim, ui::Face::Label);
  const ui::Rect body{area.x, area.y + pitch, area.w, area.h - pitch};
  switch (state_) {
    case State::Waiting:
    case State::Listing: {
      const ui::Rect box{body.x, body.y, body.w, body.h};
      r.text(box.x + 8, ui::caps_y(t, ui::Face::Data, box.y, pitch), state_ == State::Waiting ? "WAITING FOR THE CARD" : "READING THE CARD", ui::Color::Text, ui::Face::Data);
      break;
    }
    case State::Failed:
      r.text(body.x + 8, ui::caps_y(t, ui::Face::Data, body.y, pitch), failure_, ui::Color::Text, ui::Face::Data);
      r.text(body.x + 8, ui::caps_y(t, ui::Face::Label, body.y + pitch, pitch), "F8 goes back.", ui::Color::TextDim, ui::Face::Label);
      break;
    case State::Ready: {
      const unsigned rows = ui::ListView::rows_in(t, body);
      view_.set_rows(rows);
      view_.fit(*this);
      const ui::ListEmpty none = ext_[0] != '\0' ? ui::ListEmpty{"NO FILES", "nothing here ends in the extension this list wants"} : ui::ListEmpty{"NO FILES", "the folder is empty"};
      view_.draw(r, body, *this, none);
      break;
    }
    case State::Closed: break;
  }
}

void FilePicker::soft_keys(SoftKeys& keys) const noexcept {
  if (state_ == State::Closed) {
    return;
  }
  keys.label[0] = state_ == State::Ready && n_ != 0u ? "OPEN" : nullptr;
  keys.label[7] = "BACK";
}

}  // namespace o3sdk::app
