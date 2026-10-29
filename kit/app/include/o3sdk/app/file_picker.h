// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A file picker over the card, for an app's screen: the widgets of kit/ui (ui::ListView) on top of Files::list (docs/M2C-PLAN.md section 9). The app owns the picker (and with it the
// listing's memory: BasicFilePicker<N> holds N entries of Files::Entry, 48 bytes each) and drives it from its screen:
//
//   app::BasicFilePicker<24> picker;                                   // a member of the screen
//   picker.open(ctx, "LOAD A RIFF", "", "o3a");                        // the app's own folder, files ending in .o3a; the listing starts when the card is free
//   ... Screen::input:   if (picker.active()) { switch (picker.input(ctx, in)) { case Result::Chosen: ctx.files().load("", picker.chosen().name, ...); break; ...} }
//   ... Screen::update:  picker.update(ctx);                           // follows the listing job and the card going out
//   ... Screen::draw:    picker.draw(r, area);                         // the title, the list (or why there is none), the hints
//
// Keys: the encoder and UP / DOWN move the cursor, LEFT / RIGHT a page, a pad selects the row it sits under (pad p is the p-th visible row), F1 chooses (OPEN), F8 cancels (BACK). Soft keys:
// soft_keys(). A folder is chosen like a file when `folders` was asked for (chosen().folder says which): the app then opens it with a second open() on "<folder>".
//
// The listing is Files::list's: files and project folders of one folder, folders first, sorted, at most N before the extension is applied (truncated() says there were more, and then the list may lack
// files that match: give the picker room for the folder). `ext` keeps the files whose name ends in
// ".<ext>" (case-insensitive) and drops the rest; a listing with nothing left says so. If the listing cannot start (the card is busy, absent, or the folder is not one the files service
// takes) the picker says why in the list's place and `F8` leaves; a card that goes out or changes while it is open fails it with CARD CHANGED. Main loop only; no heap.

#include <o3sdk/app/context.h>
#include <o3sdk/app/files.h>
#include <o3sdk/app/input.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/ui/list_view.h>
#include <o3sdk/ui/renderer.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

class FilePicker : private ui::ListSource {
 public:
  enum class Result : std::uint8_t { Browsing, Chosen, Cancelled };
  enum class State : std::uint8_t { Closed, Waiting, Listing, Ready, Failed };

  // `entries`: the listing's memory, `cap` of them (at least 1), the caller's and alive as long as the picker is.
  FilePicker(Files::Entry* entries, std::size_t cap) noexcept : entries_(entries), cap_(cap) {}

  // Starts a listing of `folder` ("" is the app's own folder; a project folder; "Samples"). `title` is kept as a pointer (a literal). `ext`: null keeps every file, else only those ending in
  // ".<ext>" (no dot, any case); `folders`: project folders stay in the list (and can be chosen). false: the picker has no memory or `folder` is not a path the files service takes
  // (the picker is then Failed with the words of Files::status()); otherwise true even when the card is not free yet (the picker waits for it).
  bool open(Context& ctx, const char* title, const char* folder, const char* ext = nullptr, bool folders = false) noexcept;
  void close() noexcept;
  bool active() const noexcept { return state_ != State::Closed; }
  State state() const noexcept { return state_; }

  Result input(Context& ctx, const Input& in) noexcept;
  // Every pass: starts the listing when the card is free, follows it to its end, and fails it if the card changed under it.
  void update(Context& ctx) noexcept;
  void draw(ui::Renderer& r, const ui::Rect& area) noexcept;
  void soft_keys(SoftKeys& keys) const noexcept;

  // After Result::Chosen (valid until the next open()): the entry. Its name is what Files::load takes, in the folder given to open().
  const Files::Entry& chosen() const noexcept { return chosen_; }
  const char* folder() const noexcept { return folder_; }
  // The listing as it stands (valid in State::Ready).
  unsigned count() const noexcept override { return n_; }
  const char* failure() const noexcept { return failure_; }
  bool truncated() const noexcept { return truncated_; }  // the folder held more than the picker's memory

 private:
  void name(unsigned index, char* out, unsigned cap) const noexcept override;
  void value(unsigned index, char* out, unsigned cap) const noexcept override;
  void settle(Context& ctx) noexcept;

  Files::Entry* entries_;
  std::size_t cap_;
  unsigned n_ = 0;
  State state_ = State::Closed;
  ui::ListView view_{12};
  Files::Entry chosen_ = {};
  char folder_[Files::kEntryNameBytes] = {};
  char ext_[8] = {};
  bool folders_ = false;
  bool truncated_ = false;
  const char* title_ = "";
  const char* failure_ = "";
  std::uint32_t generation_ = 0;  // the card's, when the listing began
};

// A picker with its own memory for N entries.
template <unsigned N>
class BasicFilePicker final : public FilePicker {
 public:
  BasicFilePicker() noexcept : FilePicker(store_, N) {}

 private:
  Files::Entry store_[N] = {};
};

}  // namespace o3sdk::app
