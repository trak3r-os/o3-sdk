// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/core/keymap.h>

namespace o3sdk::core {

namespace {

struct Word {
  const char* p;
  std::size_t n;
};

constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

// The next line that has something on it once the comment is cut: [begin, end). `number` counts every line.
bool next_line(const char* text, std::size_t length, std::size_t& pos, int& number, const char*& begin,
               const char*& end) noexcept {
  while (pos < length) {
    const char* line = text + pos;
    std::size_t eol = pos;
    while (eol < length && text[eol] != '\n') {
      ++eol;
    }
    pos = eol + 1;
    ++number;
    const char* stop = line;
    while (stop < text + eol && *stop != '#') {
      ++stop;
    }
    const char* p = line;
    while (p < stop && is_space(*p)) {
      ++p;
    }
    if (p != stop) {
      begin = line;
      end = stop;
      return true;
    }
  }
  return false;
}

bool next_word(const char*& p, const char* end, Word& w) noexcept {
  while (p < end && is_space(*p)) {
    ++p;
  }
  if (p == end) {
    return false;
  }
  w.p = p;
  while (p < end && !is_space(*p)) {
    ++p;
  }
  w.n = static_cast<std::size_t>(p - w.p);
  return true;
}

struct Lower {
  char s[kKeymapMaxWord + 1];
};

// Lower-case NUL-terminated copy; false if the word is longer than kKeymapMaxWord or has a NUL inside (the
// lookups would see only the part before it).
bool lowered(const char* p, std::size_t n, Lower& out) noexcept {
  if (n > kKeymapMaxWord) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const char c = p[i];
    if (c == '\0') {
      return false;
    }
    out.s[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
  }
  out.s[n] = '\0';
  return true;
}

bool equals(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// Whole-string decimal integer with an optional sign, within int32.
bool parse_int(const char* p, std::size_t n, std::int32_t& out) noexcept {
  std::size_t i = 0;
  bool negative = false;
  if (n > 0 && (p[0] == '+' || p[0] == '-')) {
    negative = p[0] == '-';
    i = 1;
  }
  if (i == n) {
    return false;
  }
  std::int64_t v = 0;
  for (; i < n; ++i) {
    if (p[i] < '0' || p[i] > '9') {
      return false;
    }
    v = v * 10 + (p[i] - '0');
    if (v > 2147483648LL) {
      return false;
    }
  }
  v = negative ? -v : v;
  if (v < -2147483647LL - 1 || v > 2147483647LL) {
    return false;
  }
  out = static_cast<std::int32_t>(v);
  return true;
}

// "pad7", "enc0": `prefix` then 1..3 digits naming an id 0..254.
bool numbered(const char* s, const char* prefix, std::uint8_t& id) noexcept {
  while (*prefix != '\0') {
    if (*s++ != *prefix++) {
      return false;
    }
  }
  std::int32_t v = 0;
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  if (n == 0 || n > 3 || !parse_int(s, n, v) || s[0] == '+' || s[0] == '-' || v >= kAnyId) {
    return false;
  }
  id = static_cast<std::uint8_t>(v);
  return true;
}

bool parse_input(const char* s, const KeymapNames& names, InputRef& out) noexcept {
  std::uint8_t id = 0;
  if (equals(s, "pad*")) {
    out = InputRef{InputKind::Pad, kAnyId};
  } else if (numbered(s, "pad", id)) {
    out = InputRef{InputKind::Pad, id};
  } else if (numbered(s, "enc", id)) {
    out = InputRef{InputKind::Encoder, id};
  } else if (names.button != nullptr && names.button(names.ctx, s, id)) {
    out = InputRef{InputKind::Button, id};
  } else {
    return false;
  }
  return true;
}

bool parse_gesture(const char* s, GestureKind& out) noexcept {
  if (equals(s, "tap")) {
    out = GestureKind::Tap;
  } else if (equals(s, "hold")) {
    out = GestureKind::Hold;
  } else if (equals(s, "holdend")) {
    out = GestureKind::HoldEnd;
  } else if (equals(s, "double")) {
    out = GestureKind::DoubleTap;
  } else if (equals(s, "holdenc")) {
    out = GestureKind::HoldEncoder;
  } else if (equals(s, "chord")) {
    out = GestureKind::Chord;
  } else {
    return false;
  }
  return true;
}

constexpr bool is_button(InputRef r) noexcept { return r.kind == InputKind::Button; }

// A button the application declared as a modifier.
constexpr bool is_modifier(InputRef r, std::uint64_t modifiers) noexcept {
  return is_button(r) && r.id < 64 && ((modifiers >> r.id) & 1u) != 0;
}

// One binding line: the words after the header. Returns the error, None if `out` is filled.
KeymapError parse_binding(const char* p, const char* end, const KeymapNames& names, Binding& out) noexcept {
  Word w[5];
  unsigned count = 0;
  Word extra{};
  while (count < 5 && next_word(p, end, w[count])) {
    ++count;
  }
  if (count < 4 || next_word(p, end, extra)) {
    return KeymapError::Syntax;
  }
  Lower lw[5];
  for (unsigned i = 0; i < count; ++i) {
    if (!lowered(w[i].p, w[i].n, lw[i])) {
      return KeymapError::Syntax;
    }
  }

  ContextId context = 0;
  if (names.context == nullptr || !names.context(names.ctx, lw[0].s, context)) {
    return KeymapError::UnknownContext;
  }
  GestureKind gesture = GestureKind::Tap;
  if (!parse_gesture(lw[1].s, gesture)) {
    return KeymapError::UnknownGesture;
  }

  // Trigger: `input` or `first+second`.
  InputRef first = kNoInput;
  InputRef second = kNoInput;
  bool paired = false;
  {
    const char* plus = lw[2].s;
    while (*plus != '\0' && *plus != '+') {
      ++plus;
    }
    if (*plus == '+') {
      Lower head;
      std::size_t n = static_cast<std::size_t>(plus - lw[2].s);
      for (std::size_t i = 0; i < n; ++i) {
        head.s[i] = lw[2].s[i];
      }
      head.s[n] = '\0';
      paired = true;
      if (!parse_input(head.s, names, first) || !parse_input(plus + 1, names, second)) {
        return KeymapError::UnknownInput;
      }
    } else if (!parse_input(lw[2].s, names, second)) {
      return KeymapError::UnknownInput;
    }
  }

  // `second` is the actuated input; `first`, when present, the modifier / held button / other chord member.
  Trigger trigger{};
  switch (gesture) {
    case GestureKind::Tap:
      if (paired && !is_modifier(first, names.modifiers)) {
        return KeymapError::BadCombination;
      }
      trigger = tap(second, paired ? first : kNoInput);
      break;
    case GestureKind::Hold:
    case GestureKind::HoldEnd:
    case GestureKind::DoubleTap:
      if (!is_button(second) || (paired && !is_modifier(first, names.modifiers))) {
        return KeymapError::BadCombination;
      }
      trigger = gesture == GestureKind::Hold      ? hold(second, paired ? first : kNoInput)
                : gesture == GestureKind::HoldEnd ? hold_end(second, paired ? first : kNoInput)
                                                  : double_tap(second, paired ? first : kNoInput);
      break;
    case GestureKind::HoldEncoder:
      if (!paired || !is_button(first) || second.kind != InputKind::Encoder) {
        return KeymapError::BadCombination;
      }
      trigger = hold_encoder(second, first);
      break;
    case GestureKind::Chord:
      if (!paired || !is_button(first) || !is_button(second) || first == second || first.id == kAnyId ||
          second.id == kAnyId) {
        return KeymapError::BadCombination;
      }
      trigger = chord(first, second);
      break;
  }

  ActionId action = ActionId::None;
  if (!equals(lw[3].s, "none") && (names.action == nullptr || !names.action(names.ctx, lw[3].s, action))) {
    return KeymapError::UnknownAction;
  }

  ArgFrom from = ArgFrom::Fixed;
  std::int32_t arg = 0;
  if (count == 5) {
    const char* s = lw[4].s;
    const std::size_t n = w[4].n;
    if (s[0] == '@') {
      std::size_t name_end = 1;
      while (name_end < n && s[name_end] != '+' && s[name_end] != '-') {
        ++name_end;
      }
      Lower source;
      for (std::size_t i = 1; i < name_end; ++i) {
        source.s[i - 1] = s[i];
      }
      source.s[name_end - 1] = '\0';
      if (equals(source.s, "id")) {
        from = ArgFrom::InputId;
      } else if (equals(source.s, "value") && (second.kind == InputKind::Pad || second.kind == InputKind::Encoder)) {
        from = ArgFrom::Value;
      } else {
        return KeymapError::BadArgument;
      }
      if (name_end < n && !parse_int(s + name_end, n - name_end, arg)) {
        return KeymapError::BadArgument;
      }
    } else if (!parse_int(s, n, arg)) {
      return KeymapError::BadArgument;
    }
  }
  out = bind(context, trigger, action, from, arg);
  return KeymapError::None;
}

// Line number of the `index`-th binding (0-based): the header is the first line with content, then one binding each.
int line_of_binding(const char* text, std::size_t length, std::size_t index) noexcept {
  std::size_t pos = 0;
  int number = 0;
  const char* b = nullptr;
  const char* e = nullptr;
  for (std::size_t seen = 0; next_line(text, length, pos, number, b, e); ++seen) {
    if (seen == index + 1) {
      return number;
    }
  }
  return 0;
}

}  // namespace

const char* keymap_error_text(KeymapError error) noexcept {
  switch (error) {
    case KeymapError::None:
      return "ok";
    case KeymapError::MissingHeader:
      return "the first line must be 'o3keymap <version>'";
    case KeymapError::BadVersion:
      return "unsupported keymap version";
    case KeymapError::Syntax:
      return "expected: <context> <gesture> <trigger> <action> [<arg>]";
    case KeymapError::UnknownContext:
      return "unknown context";
    case KeymapError::UnknownGesture:
      return "unknown gesture (tap, hold, holdend, double, holdenc, chord)";
    case KeymapError::UnknownInput:
      return "unknown input";
    case KeymapError::UnknownAction:
      return "unknown action";
    case KeymapError::BadArgument:
      return "bad argument";
    case KeymapError::BadCombination:
      return "this gesture does not take that input";
    case KeymapError::Conflict:
      return "conflicts with an earlier binding of the same context";
    case KeymapError::PoolFull:
      return "too many bindings for this layer";
  }
  return "?";
}

KeymapResult parse_keymap(const char* text, std::size_t length, const KeymapNames& names, Binding* out,
                          std::size_t capacity) noexcept {
  KeymapResult r;
  const auto fail = [&](KeymapError e, int line, int other = 0) {
    r.error = e;
    r.count = 0;
    r.line = line;
    r.other_line = other;
    return r;
  };
  std::size_t pos = 0;
  int number = 0;
  const char* begin = nullptr;
  const char* end = nullptr;

  if (text != nullptr && length >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
    text += 3;  // a UTF-8 byte order mark, as some editors write one: not part of the first word
    length -= 3;
  }
  if (text == nullptr || !next_line(text, length, pos, number, begin, end)) {
    return fail(KeymapError::MissingHeader, 1);
  }
  {
    const char* p = begin;
    Word w[3];
    unsigned count = 0;
    while (count < 3 && next_word(p, end, w[count])) {
      ++count;
    }
    Lower name;
    if (count == 0 || !lowered(w[0].p, w[0].n, name) || !equals(name.s, "o3keymap") || count != 2) {
      return fail(KeymapError::MissingHeader, number);
    }
    std::int32_t version = 0;
    if (!parse_int(w[1].p, w[1].n, version) || version != 1) {
      return fail(KeymapError::BadVersion, number);
    }
  }

  while (next_line(text, length, pos, number, begin, end)) {
    if (r.count == capacity) {
      return fail(KeymapError::PoolFull, number);
    }
    const KeymapError e = parse_binding(begin, end, names, out[r.count]);
    if (e != KeymapError::None) {
      return fail(e, number);
    }
    ++r.count;
  }

  const Conflict c = find_conflict(out, r.count);
  if (c.found) {
    return fail(KeymapError::Conflict, line_of_binding(text, length, c.second),
                line_of_binding(text, length, c.first));
  }
  return r;
}

}  // namespace o3sdk::core
