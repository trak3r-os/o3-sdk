// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/ui/boot_screen.h>
#include <o3sdk/ui/renderer.h>
#include <o3sdk/ui/style.h>
#include <o3sdk/ui/theme.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace o3sdk;
using Words = std::vector<std::uint32_t>;

namespace {

struct Drawn {
  bool complete = false;
  Words frame;
};

// The boot frame in a list of `capacity` words, in either translate mode, drawn by `draw` (begin() and end() are the test's).
template <typename Fn>
Drawn drawn(bool translate, std::size_t capacity, Fn draw) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(capacity);
  eve::DisplayListBuilder builder(sink, storage.data(), capacity);
  ui::Renderer r(builder, ui::boot_theme());
  r.use_translate(translate);
  REQUIRE(r.begin(ui::Color::Pane));
  draw(r);
  Drawn d;
  d.complete = r.end();
  d.frame = sink.last_frame();
  return d;
}

Drawn boot_frame(bool translate, const std::string& name, const std::string& build,
                 std::size_t capacity = ui::kBootScreenMaxWords) {
  return drawn(translate, capacity, [&](ui::Renderer& r) { ui::draw_boot_screen(r, name.c_str(), build.c_str()); });
}

int line_width(const std::string& name, const std::string& build) {
  platform::host::FakeDisplay sink;
  Words storage(ui::kBootScreenMaxWords);
  eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
  ui::Renderer r(builder, ui::boot_theme());
  const std::string rest = "  " + build.substr(0, ui::kBootBuildChars);
  return r.text_width(name.c_str(), ui::Face::Micro) + r.text_width(rest.c_str(), ui::Face::Micro);
}

// A label of every kind a build writes (gen_build_info.cmake): the id, "-dirty", and the version tag with its describe suffix.
const char* const kKitLabel = "d8c4ede2c609";
const char* const kKitDirty = "d8c4ede2c609-dirty";
const char* const kTagged = "v0.5.0 (0271ab60bea6)";
const char* const kTaggedLong = "v0.5.0-3-g0271ab6-dirty (0271ab60bea6-dirty)";

}  // namespace

TEST_CASE("boot screen: the frames of every build line fit the list, in both translate modes") {
  const std::string name = "O3-303";
  const std::string forms[] = {
      std::string("o3sdk ") + kKitLabel,
      std::string("o3sdk ") + kKitDirty,
      std::string("ba9876543210-dirty / o3sdk ") + kKitDirty,
      std::string(kTagged) + " / o3sdk " + kTagged,
      std::string(kTaggedLong) + " / o3sdk " + kTaggedLong,  // the longest form a build writes
      std::string(300, 'W'),                                 // far longer, and no space to skip: every glyph a word
  };
  for (const bool translate : {true, false}) {
    for (const std::string& build : forms) {
      CAPTURE(translate);
      CAPTURE(build);
      const Drawn d = boot_frame(translate, name, build);
      CHECK(d.complete);
      CHECK(d.frame.size() <= ui::kBootScreenMaxWords);
    }
  }
}

TEST_CASE("boot screen: the worst line, a long name and kBootBuildChars glyphs, leaves headroom in the list") {
  const std::string name(16, 'W');
  const std::string build(ui::kBootBuildChars, 'W');
  for (const bool translate : {true, false}) {
    CAPTURE(translate);
    const Drawn d = boot_frame(translate, name, build);
    REQUIRE(d.complete);
    // A margin of 16 words under the buffer show_boot_screen() builds in; a bigger line means a bigger buffer, on purpose.
    CHECK(d.frame.size() + 16 <= ui::kBootScreenMaxWords);
  }
}

TEST_CASE("boot screen: a build line longer than kBootBuildChars is cut there") {
  const std::string head(ui::kBootBuildChars, 'x');
  for (const bool translate : {true, false}) {
    const Drawn cut = boot_frame(translate, "HELLO", head);
    const Drawn longer = boot_frame(translate, "HELLO", head + "yyyyyyyyyyyyyyyyyyyy");
    CHECK(cut.frame == longer.frame);
    CHECK(boot_frame(translate, "HELLO", head.substr(1)).frame != cut.frame);
  }
}

TEST_CASE("boot screen: the longest form a build writes is shown whole") {
  // Two tagged, dirty labels (gen_build_info.cmake): 97 characters, inside kBootBuildChars, so one more glyph still shows.
  const std::string longest = std::string(kTaggedLong) + " / o3sdk " + kTaggedLong;
  REQUIRE(longest.size() == 97);
  REQUIRE(longest.size() < ui::kBootBuildChars);
  for (const bool translate : {true, false}) {
    CHECK(boot_frame(translate, "O3-303", longest + "z").frame != boot_frame(translate, "O3-303", longest).frame);
  }
}

TEST_CASE("boot screen: the longest line is narrower than the panel, with room either side") {
  // Micro is 6 px a glyph. A name of 16 characters (the longest an app is meant to have), two spaces and kBootBuildChars glyphs:
  constexpr int kLongest = 6 * (16 + 2 + static_cast<int>(ui::kBootBuildChars));
  static_assert(kLongest <= ui::kScreenW - 2 * 32, "the longest line leaves 32 px either side");
  CHECK(line_width(std::string(16, 'W'), std::string(ui::kBootBuildChars + 40, 'W')) == kLongest);
  // The forms that are shown, whole: o3sdk's own apps and an app of another repository, dirty or tagged.
  CHECK(line_width("HELLO", std::string("o3sdk ") + kKitLabel) == 6 * (5 + 2 + 6 + 12));
  CHECK(line_width("O3-303", std::string("ba9876543210-dirty / o3sdk ") + kKitDirty) == 6 * (6 + 2 + 18 + 3 + 6 + 18));
  CHECK(line_width("O3-303", std::string(kTagged) + " / o3sdk " + kTagged) == 6 * (6 + 2 + 21 + 3 + 6 + 21));
  CHECK(line_width("O3-303", std::string(kTaggedLong) + " / o3sdk " + kKitDirty) < ui::kScreenW);
}

TEST_CASE("boot screen: in o3sdk's own tree the line is what it always was, \"<name>  o3sdk <id>\" centred") {
  for (const bool translate : {true, false}) {
    CAPTURE(translate);
    // The reference: the logo of a frame with no line, then the name and the rest drawn by hand where the line is centred.
    const Drawn ref = drawn(translate, ui::kBootScreenMaxWords, [](ui::Renderer& r) {
      ui::draw_boot_screen(r, "", "");
      const char* const name = "HELLO";
      const char* const rest = "  o3sdk d8c4ede2c609";
      const int name_w = r.text_width(name, ui::Face::Micro);
      const int x = (ui::kScreenW - name_w - r.text_width(rest, ui::Face::Micro)) / 2;
      CHECK(x == 325);  // (800 - 6 * (5 + 2 + 5 + 1 + 12)) / 2
      r.text(x, ui::kBootTextY, name, ui::Color::Text, ui::Face::Micro);
      r.text(x + name_w, ui::kBootTextY, rest, ui::Color::TextDim, ui::Face::Micro);
    });
    REQUIRE(ref.complete);
    CHECK(boot_frame(translate, "HELLO", "o3sdk d8c4ede2c609").frame == ref.frame);
  }
}

TEST_CASE("boot screen: a missing name or build line draws the logo alone") {
  platform::host::FakeDisplay sink;
  Words storage(ui::kBootScreenMaxWords);
  eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
  ui::Renderer r(builder, ui::boot_theme());
  REQUIRE(r.begin(ui::Color::Pane));
  ui::draw_boot_screen(r, nullptr, nullptr);
  CHECK(r.end());
  CHECK(sink.last_frame() == boot_frame(true, "", "").frame);
}

TEST_CASE("boot screen: show_boot_screen commits the frame") {
  platform::host::FakeDisplay sink;
  CHECK(ui::show_boot_screen(sink, "O3-303", "ba9876543210 / o3sdk d8c4ede2c609"));
  CHECK(sink.frames() == 1);
  CHECK(sink.last_frame() == boot_frame(true, "O3-303", "ba9876543210 / o3sdk d8c4ede2c609").frame);
  sink.set_busy(true);
  CHECK_FALSE(ui::show_boot_screen(sink, "O3-303", "x"));  // a busy display takes nothing; the caller tries again
  CHECK(sink.frames() == 1);
}
