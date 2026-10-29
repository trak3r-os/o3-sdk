// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A name entry for an app's screen: the on-screen keyboard (ui::keyboard) over TextEntry's state machine (docs/M2C-PLAN.md section 9), for the name of a file or a project. The app owns
// the entry and drives it from its screen:
//
//   app::NameEntry names;                                          // a member of the screen
//   names.open("SAVE AS", "RIFF 1", app::NameEntry::Kind::FileName);
//   ... Screen::input:  if (names.active()) { if (names.input(in) == Result::Done) ctx.files().save("", file_of(names.text()), ...); return; }
//   ... Screen::draw:   names.draw(r, area);                       // the keyboard over the screen
//   ... Screen::soft_keys: names.soft_keys(keys);
//
// Keys: the encoder and LEFT / RIGHT move the focus along the keys, UP / DOWN between the rows and the name, F1 types the key in focus, a pad types the key under it (the pad grid is
// the keyboard's), DELETE is the backspace, F7 accepts (OK) and F8 cancels. OK is refused, with the keyboard saying why, while the name is empty or (Kind::FileName) has not the shape of a name the files
// service takes (letters, digits, space, '_', '-' and '.' only: the filter; the first a letter or digit, the last neither a space nor a dot, no ".."). The service has the last word (it refuses a
// reserved device name such as CON, and its strip says so).
// `text()` is the name once Done. Main loop only; no heap.

#include <o3sdk/app/input.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/app/text_entry.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::app {

class NameEntry {
 public:
  enum class Result : std::uint8_t { Editing, Done, Cancelled };
  enum class Kind : std::uint8_t { FileName, Any };  // FileName: the filter of TextEntry and the files service's own check on OK

  // Starts editing `initial` (cut to `cap` characters, at most 31 for a FileName), all of it selected. `title` is kept as a pointer (a literal).
  void open(const char* title, const char* initial, Kind kind = Kind::FileName, unsigned cap = kTextCapacity - 1u) noexcept;
  void close() noexcept { active_ = false; }
  bool active() const noexcept { return active_; }

  Result input(const Input& in) noexcept;
  void draw(ui::Renderer& r, const ui::Rect& area) const noexcept;
  void soft_keys(SoftKeys& keys) const noexcept;

  const char* text() const noexcept { return entry_.text(); }
  // What OK would do now.
  bool acceptable() const noexcept;
  const TextEntry& entry() const noexcept { return entry_; }

 private:
  TextEntry entry_;
  const char* title_ = "";
  const char* message_ = "";
  Kind kind_ = Kind::FileName;
  bool active_ = false;
};

}  // namespace o3sdk::app
