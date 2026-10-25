// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The text keymap (ADR 0009 rev 1): how user and preset binding layers reach the binding tables. It is the
// format of the SD data packs of later slices; this file only parses a buffer into a fixed pool. No file I/O, no
// heap, bounded by the buffer and the pool.
//
//   o3keymap 1                             first line (after blanks and comments): format name and version
//   # comment                              from # to the end of the line
//   <context> <gesture> <trigger> <action> [<arg>]       one binding per line; words are case-insensitive
//
//   context   a name the application gives (global, pattern, ...)
//   gesture   tap | hold | holdend | double | holdenc | chord      (holdend: the release of a button whose hold fired)
//   trigger   [<modifier>+]<input>         tap, hold, holdend, double: input is a button, or pad<N> / pad* / enc<N> for tap;
//                                          the modifier must be one of the application's declared modifier buttons
//             <button>+enc<N>              holdenc: the held button and the encoder it turns
//             <button>+<button>            chord: two different buttons, in either order
//             button names are the application's; pad<N> N = 0..254, pad* every pad; enc<N> N = 0..254
//   action    a registered action name, or `none` (explicitly nothing: shadows lower layers)
//   arg       optional: an integer (default 0); @id (the input's number, e.g. the pad) or @value (encoder detents,
//             pad velocity), each optionally followed by +N or -N: @id+1
//
// A line binds in its own context and nowhere else: the walk of the context chain (core/binding.h) stops at the
// first context that has a binding, so a `global` line does not override what a screen's built-in layer binds for
// the same trigger; to change a screen's key, write the line in that screen's context.
//
// Version 1 is the first version, so there is no older loader yet; a changed meaning of any word needs version 2
// and a loader for 1.
//
// A buffer may start with a UTF-8 byte order mark; a NUL inside a word is a syntax error.
//
// The loader is all-or-nothing: on the first error it stops and reports (error, line); nothing of the buffer takes
// effect (count 0). A conflict (two bindings of one context with overlapping triggers) is an error naming both
// lines; it is never resolved silently.

#include <o3sdk/core/binding.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::core {

// Names the application owns, looked up with a lower-case, NUL-terminated copy of the word.
struct KeymapNames {
  const void* ctx;
  bool (*context)(const void* ctx, const char* name, ContextId& out) noexcept;
  bool (*button)(const void* ctx, const char* name, std::uint8_t& out) noexcept;
  bool (*action)(const void* ctx, const char* name, ActionId& out) noexcept;
  std::uint64_t modifiers;  // buttons that may modify another input (bit n = button n): the gesture layer's set
};

enum class KeymapError : std::uint8_t {
  None,
  MissingHeader,    // the first line is not `o3keymap <version>`
  BadVersion,       // a version this loader does not know
  Syntax,           // wrong number of words, or a word too long
  UnknownContext,
  UnknownGesture,
  UnknownInput,
  UnknownAction,
  BadArgument,
  BadCombination,   // e.g. a hold on a pad, a chord with the encoder, a modifier that is not a declared one
  Conflict,         // `line` and `other_line` are the two bindings
  PoolFull,
};

struct KeymapResult {
  KeymapError error = KeymapError::None;
  std::size_t count = 0;  // bindings stored (0 on error)
  int line = 0;           // 1-based line of the error
  int other_line = 0;     // Conflict: the earlier of the two lines
  constexpr bool ok() const noexcept { return error == KeymapError::None; }
};

const char* keymap_error_text(KeymapError error) noexcept;

inline constexpr std::size_t kKeymapMaxWord = 32;  // longest word (name, trigger or argument) in a keymap line

KeymapResult parse_keymap(const char* text, std::size_t length, const KeymapNames& names, Binding* out,
                          std::size_t capacity) noexcept;

// A fixed pool for one binding layer (the app's user and preset layers).
template <std::size_t N>
class BindingPool {
 public:
  static constexpr std::size_t capacity() noexcept { return N; }
  std::size_t size() const noexcept { return count_; }
  BindingView view() const noexcept { return BindingView{items_, count_}; }
  void clear() noexcept { count_ = 0; }

  // Replaces the pool's content with the keymap, or empties it if the keymap is rejected (a half-parsed table
  // never takes effect).
  KeymapResult load(const char* text, std::size_t length, const KeymapNames& names) noexcept {
    const KeymapResult r = parse_keymap(text, length, names, items_, N);
    count_ = r.count;
    return r;
  }

 private:
  Binding items_[N] = {};
  std::size_t count_ = 0;
};

}  // namespace o3sdk::core
