// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The on-screen keyboard as a pure state machine (OS-FU2): what the text entry overlay edits. The keyboard has 42 keys in four
// rows of 11, 12, 11 and 8, the pads typing the key under them (the key under each pad), a focus that the arrows move, the name above it with an edit cursor and, at the start, the whole
// name SELECTED (a key typed over a selection replaces all of it, a backspace deletes all of it). Typing is refused at the limit (32). The caps key toggles the big letters.
//
// FOCUS MOVES. The 1.7.0 release notes say the focus key was made to WRAP at the edges (PAR-116: documented in the release notes, not verified; the rules are not in the notes). OS-new here, documented and
// reversible: Left and Right wrap inside the row; Up from the top row goes up to the NAME
// and Down from the bottom row goes to the name too, so the focus goes round; between rows of different widths the column is held to the shorter row.
//
// FILTER. A name for a file or a folder takes the characters FAT and the save layer's guard (hal/save_guard.h valid_name) allow: letters, digits, space, '_', '-' and '.'. The keys '@'
// and '+' of the keyboard layout are refused then (shown dim); the whole name is checked again by the owner of the text before it is used. Any other text takes every key.
//
// Pure: no heap, no globals, no I/O.

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

inline constexpr unsigned kTextKeys = 42;
inline constexpr unsigned kTextCapacity = 32;  // our name limit
inline constexpr unsigned kTextKeyBackspace = 10;
inline constexpr unsigned kTextKeyCaps = 33;
inline constexpr unsigned kTextKeySpace = 41;
inline constexpr unsigned kTextRows = 4;

// The keyboard layout, row by row; 0 is the backspace key, 1 the caps key.
inline constexpr char kTextKeysSmall[kTextKeys] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0,   'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '-', '+',
                                                   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '@', 1, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ' '};
inline constexpr char kTextKeysBig[kTextKeys] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '-', '+',
                                                 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '@', 1, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ' '};
// The first key of each row, and one past the last (the rows have 11, 12, 11 and 8 keys).
inline constexpr std::uint8_t kTextRowStart[kTextRows + 1] = {0, 11, 23, 34, 42};
// The key under each of the 48 pads (the 12 x 4 pad grid over the 4 rows; the backspace, caps and space keys are two, two and five pads wide).
inline constexpr std::uint8_t kTextPadKey[48] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                                 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 33, 34, 35, 36, 37, 38, 39, 40, 41, 41, 41, 41, 41};

// A title as a name for a file or a folder (OS-NAMES, Save As of a project that has no folder): the characters the FileName filter takes (letters, digits, space and '-': key_allowed) kept in order, the rest
// dropped, the first a letter or digit, no space at the end, at most `cap` - 1 characters. Writes `out` (NUL ended, `cap` > 0) and returns the length; 0 when nothing is left.
unsigned text_file_name(const char* title, char* out, unsigned cap) noexcept;

class TextEntry {
 public:
  enum class Filter : std::uint8_t { Any, FileName };

  // The name starts as `initial` (cut to `cap`, at most kTextCapacity), all of it selected, the focus on the backspace key.
  void begin(const char* initial, unsigned cap, Filter filter) noexcept;
  // The whole text replaced (the AUTO name): selected again.
  void set(const char* text) noexcept;

  // A key typed: the character at the edit cursor, a backspace, or the caps toggle; the focus moves to it. False when nothing changed (a refused character, the limit, a backspace at the
  // start).
  bool type(unsigned key) noexcept;
  bool pad(unsigned pad) noexcept { return pad < 48u && type(kTextPadKey[pad]); }
  bool confirm_key() noexcept { return !frame_ && type(key_); }  // the key in focus (nothing while the focus is in the name)
  bool backspace() noexcept;                                       // the Delete button
  // The focus: dx / dy in -1..1 (see FOCUS MOVES).
  bool move(int dx, int dy) noexcept;

  const char* text() const noexcept { return text_; }
  unsigned length() const noexcept { return len_; }
  unsigned edit() const noexcept { return edit_; }       // the edit cursor, 0..length
  unsigned key() const noexcept { return key_; }         // the key in focus (meaningful when !frame())
  bool frame() const noexcept { return frame_; }         // the focus is in the name
  bool selected() const noexcept { return all_; }        // the whole name is selected
  bool shift() const noexcept { return shift_; }
  unsigned capacity() const noexcept { return cap_; }
  // What a key shows now (the big letters while caps is on); 0 for backspace, 1 for caps.
  char key_char(unsigned key) const noexcept { return key < kTextKeys ? (shift_ ? kTextKeysBig[key] : kTextKeysSmall[key]) : '\0'; }
  bool key_allowed(unsigned key) const noexcept;

 private:
  void replace_all(char c) noexcept;

  char text_[kTextCapacity + 1] = {};
  std::uint8_t len_ = 0;
  std::uint8_t edit_ = 0;
  std::uint8_t key_ = kTextKeyBackspace;
  std::uint8_t cap_ = kTextCapacity;
  Filter filter_ = Filter::Any;
  bool shift_ = false;
  bool frame_ = false;
  bool all_ = false;
};

}  // namespace o3sdk::app
