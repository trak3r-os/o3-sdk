// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/binding.h>
#include <o3sdk/core/keymap.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3sdk::core;

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: check, and leave the test case if the
// size is wrong (the checks after it index into the container).
#define NEED_SIZE(container, n)       \
  do {                                \
    CHECK((container).size() == (n)); \
    if ((container).size() != (n)) {  \
      return;                         \
    }                                 \
  } while (0)

namespace {

constexpr InputRef btn(std::uint8_t id) { return InputRef{InputKind::Button, id}; }
constexpr InputRef pad(std::uint8_t id) { return InputRef{InputKind::Pad, id}; }
constexpr InputRef enc(std::uint8_t id) { return InputRef{InputKind::Encoder, id}; }

constexpr ContextId kGlobal = 0;
constexpr ContextId kScreen = 1;

// The application's names, as far as the loader needs them: lower-case words in, ids out.
bool find_context(const void*, const char* name, ContextId& out) noexcept {
  if (std::strcmp(name, "global") == 0) {
    out = kGlobal;
    return true;
  }
  if (std::strcmp(name, "screen") == 0) {
    out = kScreen;
    return true;
  }
  return false;
}

bool find_button(const void*, const char* name, std::uint8_t& out) noexcept {
  static const struct {
    const char* name;
    std::uint8_t id;
  } kButtons[] = {{"a", 1}, {"b", 2}, {"c", 3}, {"vol", 20}, {"fx", 21}, {"shift", 29}, {"left", 30}};
  for (const auto& b : kButtons) {
    if (std::strcmp(name, b.name) == 0) {
      out = b.id;
      return true;
    }
  }
  return false;
}

bool find_action(const void*, const char* name, ActionId& out) noexcept {
  static const char* const kActions[] = {"nav.left", "nav.scroll", "instrument.select", "learn.toggle", "x.y"};
  for (const char* a : kActions) {
    if (std::strcmp(name, a) == 0) {
      out = action_id(a);
      return true;
    }
  }
  return false;
}

constexpr std::uint64_t kShiftOnly = std::uint64_t{1} << 29;  // "shift" is the one declared modifier

const KeymapNames kNames{nullptr, find_context, find_button, find_action, kShiftOnly};

// A string literal with its embedded NULs kept.
template <std::size_t N>
std::string lit(const char (&text)[N]) {
  return std::string(text, N - 1);
}

struct Parsed {
  KeymapResult result;
  std::vector<Binding> bindings;
};

Parsed parse(const std::string& text, std::size_t capacity = 64) {
  Parsed p;
  p.bindings.resize(capacity);
  // A buffer without a terminator and without spare bytes after it: an over-read is caught by the sanitiser.
  std::vector<char> exact(text.begin(), text.end());
  p.result = parse_keymap(exact.data(), exact.size(), kNames, p.bindings.data(), p.bindings.size());
  p.bindings.resize(p.result.count);
  return p;
}

}  // namespace

TEST_CASE("keymap: every form of a binding line gives exactly the binding written") {
  const Parsed p = parse(
      "o3keymap 1\n"
      "screen tap left nav.left\n"
      "screen tap shift+left nav.left 4\n"
      "SCREEN TAP A X.Y -7\r\n"
      "screen tap pad3 instrument.select\n"
      "global tap pad* instrument.select @id+1\n"
      "screen tap enc0 nav.scroll @value\n"
      "screen tap shift+enc1 nav.scroll @value-3\n"
      "screen holdenc fx+enc0 nav.scroll @value\n"
      "screen hold b x.y 12\n"
      "screen double shift+b x.y\n"
      "global chord fx+vol learn.toggle\n"
      "global tap left none\n"
      "global tap shift+pad7 instrument.select @id\n"
      "screen holdend a x.y\n"
      "screen HoldEnd shift+b x.y 3\n");
  CHECK_MESSAGE(p.result.ok(), keymap_error_text(p.result.error) << " at line " << p.result.line);
  NEED_SIZE(p.bindings, 15u);
  const Binding expected[] = {
      bind(kScreen, tap(btn(30)), action_id("nav.left")),
      bind(kScreen, tap(btn(30), btn(29)), action_id("nav.left"), ArgFrom::Fixed, 4),
      bind(kScreen, tap(btn(1)), action_id("x.y"), ArgFrom::Fixed, -7),
      bind(kScreen, tap(pad(3)), action_id("instrument.select")),
      bind(kGlobal, tap(pad(kAnyId)), action_id("instrument.select"), ArgFrom::InputId, 1),
      bind(kScreen, tap(enc(0)), action_id("nav.scroll"), ArgFrom::Value),
      bind(kScreen, tap(enc(1), btn(29)), action_id("nav.scroll"), ArgFrom::Value, -3),
      bind(kScreen, hold_encoder(enc(0), btn(21)), action_id("nav.scroll"), ArgFrom::Value),
      bind(kScreen, hold(btn(2)), action_id("x.y"), ArgFrom::Fixed, 12),
      bind(kScreen, double_tap(btn(2), btn(29)), action_id("x.y")),
      bind(kGlobal, chord(btn(20), btn(21)), action_id("learn.toggle")),
      bind(kGlobal, tap(btn(30)), ActionId::None),
      bind(kGlobal, tap(pad(7), btn(29)), action_id("instrument.select"), ArgFrom::InputId),
      bind(kScreen, hold_end(btn(1)), action_id("x.y")),
      bind(kScreen, hold_end(btn(2), btn(29)), action_id("x.y"), ArgFrom::Fixed, 3),
  };
  for (std::size_t i = 0; i < 15 && i < p.bindings.size(); ++i) {
    CHECK_MESSAGE(p.bindings[i] == expected[i], "binding " << i);
  }
}

TEST_CASE("keymap: a header alone is a valid, empty keymap; comments and blank lines come first") {
  CHECK(parse("o3keymap 1").result.ok());
  CHECK(parse("o3keymap 1").bindings.empty());
  CHECK(parse("\n\n   # nothing yet\n\t\nO3KEYMAP 1 # x\n").result.ok());
}

TEST_CASE("keymap: errors name the line and the kind, and nothing is kept") {
  struct Case {
    const char* text;
    KeymapError error;
    int line;
  };
  const Case cases[] = {
      {"", KeymapError::MissingHeader, 1},
      {"# only a comment\n", KeymapError::MissingHeader, 1},
      {"screen tap a x.y\n", KeymapError::MissingHeader, 1},
      {"o3keymap\n", KeymapError::MissingHeader, 1},
      {"o3keymap 1 2\n", KeymapError::MissingHeader, 1},
      {"\n\nkeymap 1\n", KeymapError::MissingHeader, 3},
      {"o3keymap 2\n", KeymapError::BadVersion, 1},
      {"o3keymap x\n", KeymapError::BadVersion, 1},
      {"o3keymap 1\nscreen tap a\n", KeymapError::Syntax, 2},
      {"o3keymap 1\nscreen tap a x.y 1 2\n", KeymapError::Syntax, 2},
      {"o3keymap 1\nscreen tap aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa x.y\n", KeymapError::Syntax, 2},
      {"o3keymap 1\n# c\nnowhere tap a x.y\n", KeymapError::UnknownContext, 3},
      {"o3keymap 1\nscreen press a x.y\n", KeymapError::UnknownGesture, 2},
      {"o3keymap 1\nscreen tap zzz x.y\n", KeymapError::UnknownInput, 2},
      {"o3keymap 1\nscreen tap pad255 x.y\n", KeymapError::UnknownInput, 2},   // 255 is the wildcard's own number
      {"o3keymap 1\nscreen tap pad-1 x.y\n", KeymapError::UnknownInput, 2},
      {"o3keymap 1\nscreen tap pad x.y\n", KeymapError::UnknownInput, 2},
      {"o3keymap 1\nscreen tap shift+zzz x.y\n", KeymapError::UnknownInput, 2},
      {"o3keymap 1\nscreen tap a nope.nope\n", KeymapError::UnknownAction, 2},
      {"o3keymap 1\nscreen tap a x.y 1x\n", KeymapError::BadArgument, 2},
      {"o3keymap 1\nscreen tap a x.y 99999999999\n", KeymapError::BadArgument, 2},
      {"o3keymap 1\nscreen tap a x.y @nothing\n", KeymapError::BadArgument, 2},
      {"o3keymap 1\nscreen tap a x.y @id+\n", KeymapError::BadArgument, 2},
      {"o3keymap 1\nscreen tap a x.y @value\n", KeymapError::BadArgument, 2},  // a button has no value
      {"o3keymap 1\nscreen hold pad3 x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen double enc0 x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen hold pad*+a x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen tap enc0+a x.y\n", KeymapError::BadCombination, 2},  // the modifier must be a button
      {"o3keymap 1\nscreen holdenc enc0 x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen holdenc a+b x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen holdenc enc0+a x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen tap vol+left x.y\n", KeymapError::BadCombination, 2},    // vol is not a declared modifier
      {"o3keymap 1\nscreen tap a+b x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen hold a+b x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen double a+b x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen chord a x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen chord a+a x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen chord a+pad1 x.y\n", KeymapError::BadCombination, 2},
      {"o3keymap 1\nscreen chord a+enc0 x.y\n", KeymapError::BadCombination, 2},
  };
  for (const Case& c : cases) {
    const Parsed p = parse(c.text);
    CHECK_MESSAGE(p.result.error == c.error, "text: " << c.text << " got: " << keymap_error_text(p.result.error));
    CHECK_MESSAGE(p.result.line == c.line, "text: " << c.text);
    CHECK_MESSAGE(p.result.count == 0u, "text: " << c.text);
    CHECK_FALSE(p.result.ok());
  }
  // The error comes from the first bad line, not the last.
  const Parsed two = parse("o3keymap 1\nscreen tap a x.y\nscreen tap zzz x.y\nscreen press a x.y\n");
  CHECK(two.result.error == KeymapError::UnknownInput);
  CHECK(two.result.line == 3);
  CHECK(two.bindings.empty());
}

TEST_CASE("keymap: only a declared modifier may modify; hold+encoder and chords take any button") {
  CHECK(parse("o3keymap 1\nscreen tap shift+a x.y\nscreen hold shift+a x.y\nscreen double shift+a x.y\n").result.ok());
  CHECK(parse("o3keymap 1\nscreen holdend shift+a x.y\n").result.ok());
  CHECK(parse("o3keymap 1\nscreen holdend b+a x.y\n").result.error == KeymapError::BadCombination);   // only Shift modifies
  CHECK(parse("o3keymap 1\nscreen holdend pad3 x.y\n").result.error == KeymapError::BadCombination);  // a button's hold
  CHECK(parse("o3keymap 1\nscreen holdend a x.y\nscreen hold a x.y\n").result.ok());                    // distinct gestures
  CHECK(parse("o3keymap 1\nscreen holdenc a+enc0 x.y\nscreen chord a+b x.y\n").result.ok());
  // The application with no modifiers declared accepts no modified trigger at all.
  const KeymapNames none_declared{nullptr, find_context, find_button, find_action, 0};
  Binding out[4];
  const char text[] = "o3keymap 1\nscreen tap shift+a x.y\n";
  const KeymapResult r = parse_keymap(text, sizeof(text) - 1, none_declared, out, 4);
  CHECK(r.error == KeymapError::BadCombination);
  CHECK(r.line == 2);
}

TEST_CASE("keymap: a NUL inside a word is an error, a NUL in a comment is not") {
  const std::string header = "o3keymap 1\n";
  CHECK(parse(header + lit("screen tap a\0 x.y\n")).result.error == KeymapError::Syntax);
  CHECK(parse(header + lit("scr\0een tap a x.y\n")).result.error == KeymapError::Syntax);
  CHECK(parse(header + lit("screen tap a x.y 1\0\n")).result.error == KeymapError::Syntax);
  CHECK(parse(header + lit("screen tap a x.y # a\0b\n")).result.ok());
  const Parsed in_header = parse(lit("o3key\0map 1\n"));
  CHECK(in_header.result.error == KeymapError::MissingHeader);
  CHECK(in_header.result.line == 1);
}

TEST_CASE("keymap: a leading UTF-8 byte order mark is skipped, anywhere else it is just a bad word") {
  const std::string bom = "\xEF\xBB\xBF";
  const Parsed ok = parse(bom + "o3keymap 1\nscreen tap a x.y\n");
  CHECK(ok.result.ok());
  CHECK(ok.bindings.size() == 1u);
  CHECK(parse(bom + "# comment first\n\no3keymap 1\n").result.ok());
  // Line numbers do not move.
  const Parsed bad = parse(bom + "o3keymap 1\nscreen tap zzz x.y\n");
  CHECK(bad.result.error == KeymapError::UnknownInput);
  CHECK(bad.result.line == 2);
  const Parsed conflict = parse(bom + "o3keymap 1\nscreen tap a x.y\nscreen tap a nav.left\n");
  CHECK(conflict.result.error == KeymapError::Conflict);
  CHECK(conflict.result.line == 3);
  CHECK(conflict.result.other_line == 2);
  // Not at the start, or not whole: no special treatment.
  CHECK(parse("o3keymap 1\n" + bom + "screen tap a x.y\n").result.error == KeymapError::UnknownContext);
  CHECK(parse(std::string("\xEF\xBB") + "o3keymap 1\n").result.error == KeymapError::MissingHeader);
  CHECK(parse(bom).result.error == KeymapError::MissingHeader);  // nothing after it
}

TEST_CASE("keymap: a conflict names both lines and is never resolved silently") {
  const Parsed p = parse(
      "# header follows\n"
      "o3keymap 1\n"
      "\n"
      "screen tap a x.y            # line 4\n"
      "screen tap b x.y\n"
      "# between\n"
      "global tap a nav.left       # another context: fine\n"
      "screen tap a nav.left       # line 8: the same trigger as line 4\n");
  CHECK(p.result.error == KeymapError::Conflict);
  CHECK(p.result.line == 8);
  CHECK(p.result.other_line == 4);
  CHECK(p.result.count == 0u);

  CHECK(parse("o3keymap 1\nscreen tap pad* x.y\nscreen tap pad4 nav.left\n").result.error == KeymapError::Conflict);
  CHECK(parse("o3keymap 1\nglobal chord vol+fx x.y\nglobal chord fx+vol nav.left\n").result.error ==
        KeymapError::Conflict);
  CHECK(parse("o3keymap 1\nscreen tap a x.y\nscreen tap shift+a nav.left\nscreen hold a x.y\n").result.ok());
}

TEST_CASE("keymap: a keymap larger than the pool is refused whole") {
  const std::string text =
      "o3keymap 1\n"
      "screen tap a x.y\n"
      "screen tap b x.y\n"
      "screen tap c x.y\n";
  CHECK(parse(text, 3).result.ok());
  const Parsed p = parse(text, 2);
  CHECK(p.result.error == KeymapError::PoolFull);
  CHECK(p.result.line == 4);
  CHECK(p.result.count == 0u);
  CHECK(parse(text, 0).result.error == KeymapError::PoolFull);
}

TEST_CASE("keymap: a null buffer is a missing header") {
  Binding out[2];
  const KeymapResult r = parse_keymap(nullptr, 0, kNames, out, 2);
  CHECK(r.error == KeymapError::MissingHeader);
}

TEST_CASE("keymap: a pool loads, replaces its content, and empties itself on a rejected keymap") {
  BindingPool<4> pool;
  CHECK(pool.size() == 0u);
  CHECK(pool.view().count == 0u);
  const std::string one = "o3keymap 1\nscreen tap a x.y\nscreen tap b x.y\n";
  CHECK(pool.load(one.data(), one.size(), kNames).ok());
  CHECK(pool.size() == 2u);
  CHECK(pool.view().count == 2u);
  CHECK(pool.view().items[1].trigger == tap(btn(2)));

  const std::string other = "o3keymap 1\nscreen tap c x.y\n";
  CHECK(pool.load(other.data(), other.size(), kNames).ok());
  CHECK(pool.size() == 1u);
  CHECK(pool.view().items[0].trigger == tap(btn(3)));

  const std::string bad = "o3keymap 1\nscreen tap c x.y\nscreen tap zzz x.y\n";
  const KeymapResult r = pool.load(bad.data(), bad.size(), kNames);
  CHECK(r.error == KeymapError::UnknownInput);
  CHECK(pool.size() == 0u);  // a half-parsed table never takes effect

  CHECK(pool.load(one.data(), one.size(), kNames).ok());
  pool.clear();
  CHECK(pool.size() == 0u);
}

TEST_CASE("keymap: every error has a message") {
  for (unsigned e = 0; e <= static_cast<unsigned>(KeymapError::PoolFull); ++e) {
    const char* text = keymap_error_text(static_cast<KeymapError>(e));
    CHECK((text != nullptr && text[0] != '\0'));
  }
}
