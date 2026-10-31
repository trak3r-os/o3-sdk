// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/sim/eve_raster.h>
#include <o3sdk/ui/boot_screen.h>
#include <o3sdk/ui/renderer.h>
#include <o3sdk/ui/style.h>
#include <o3sdk/ui/theme.h>

#include <o3sdk/ui/boot_logo.h>
#include <o3sdk/ui/plot/ram_g_map.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
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
      const int text_y = ui::boot_layout(ui::boot_logo()).text_y;
      r.text(x, text_y, name, ui::Color::Text, ui::Face::Micro);
      r.text(x + name_w, text_y, rest, ui::Color::TextDim, ui::Face::Micro);
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

TEST_CASE("boot screen: the frames of the kit's default logo are the pinned ones (tests/digests/boot_screen.txt)") {
  // The words of a boot frame, digested like the simulator's frames (EveRaster::last_frame_digest, FNV-1a): a change of the renderer or the screen
  // that moves a word moves a digest. The pins are `<label> <digest>` lines; a moved one is printed below, for the review that accepts it.
  std::map<std::string, std::string> pinned;
  {
    std::ifstream in(std::string(O3SDK_DIGESTS_DIR) + "/boot_screen.txt");
    REQUIRE(in.good());
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line[0] != '#') {
        const std::size_t space = line.find(' ');
        REQUIRE(space != std::string::npos);
        pinned[line.substr(0, space)] = line.substr(space + 1);
      }
    }
  }
  struct Form {
    const char* label;
    const char* name;
    std::string build;
  };
  const Form forms[] = {
      {"hello_kit", "HELLO", std::string("o3sdk ") + kKitLabel},
      {"o3_303_two_ids", "O3-303", std::string("ba9876543210 / o3sdk ") + kKitLabel},
      {"o3_303_tagged_dirty", "O3-303", std::string(kTaggedLong) + " / o3sdk " + kTaggedLong},
      {"no_build_line", "HELLO", ""},
  };
  for (const bool translate : {true, false}) {
    for (const Form& f : forms) {
      const std::string label = std::string(f.label) + (translate ? "_translate" : "_plain");
      CAPTURE(label);
      const Drawn d = boot_frame(translate, f.name, f.build);
      REQUIRE(d.complete);
      char text[16];
      std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(sim::EveRaster::digest_words(d.frame.data(), d.frame.size())));
      INFO("the pin line is: " << label << " " << text);
      REQUIRE(pinned.count(label) == 1);
      CHECK(pinned[label] == text);
    }
  }
  CHECK(pinned.size() == 2 * std::size(forms));  // no stale pin
}

// ---- The logo is data: any valid logo draws, and the list bound holds for the default and for the largest one -------------------------

namespace {

namespace op = eve::opcode;

// The words of the logo's own raw section (from binding its bitmap handle to END), whose opcode is `opcode` (the top byte), in order: the
// renderer's begin() binds its own bitmaps before, and the line's glyphs draw after.
Words with_opcode(const Words& frame, std::uint32_t opcode) {
  Words found;
  bool inside = false;
  for (const std::uint32_t w : frame) {
    if (w == eve::dl::bitmap_handle(ui::plot::kHandleBootLogo)) {
      inside = true;
    }
    if (inside && (w >> 24) == opcode) {
      found.push_back(w);
    }
    if (inside && w == eve::dl::end()) {
      break;
    }
  }
  return found;
}

// Backing store of the made-up logos: only the address matters to the draw code, and boot_logo_valid() wants a non-null one.
alignas(4) const std::uint8_t kBlob[4] = {0, 0, 0, 0};

ui::BootLogo made(unsigned w, unsigned h, unsigned scale, eve::BitmapFormat format, std::uint32_t color = ui::kBootLogoWhite) {
  ui::BootLogo l{};
  l.bytes = kBlob;
  l.width = static_cast<std::uint16_t>(w);
  l.height = static_cast<std::uint16_t>(h);
  l.scale = static_cast<std::uint8_t>(scale);
  l.format = format;
  l.color = color;
  const unsigned bits = ui::boot_logo_bits(format);
  l.stride = static_cast<std::uint16_t>((w * bits + 7) / 8);
  const std::uint32_t bitmap = (static_cast<std::uint32_t>(l.stride) * h + 3) / 4 * 4;
  const bool paletted = format == eve::BitmapFormat::Paletted565;
  l.palette_offset = paletted ? bitmap : 0;
  l.size = bitmap + (paletted ? ui::kBootLogoPaletteBytes : 0);
  return l;
}

Drawn logo_frame(bool translate, const ui::BootLogo& logo, const std::string& name, const std::string& build) {
  return drawn(translate, ui::kBootScreenMaxWords, [&](ui::Renderer& r) { ui::draw_boot_screen(r, logo, name.c_str(), build.c_str()); });
}

}  // namespace

TEST_CASE("boot logo: the kit's default is O3 (logo/o3.png), a valid L1 logo of the finest native size within the limits") {
  const ui::BootLogo d = ui::boot_logo();
  CHECK(ui::boot_logo_valid(d));
  CHECK(d.format == eve::BitmapFormat::L1);
  CHECK(d.scale == 1);  // the bitmap is shown at its own size: round dots, no block edges
  CHECK(d.width == 684);
  CHECK(d.height == 420);
  CHECK(d.size == 36120);  // flash and RAM_G: under the 64 KiB a logo may be
  CHECK(d.palette_offset == 0);
  CHECK(d.color == ui::kBootLogoWhite);
  CHECK(d.size <= ui::kBootLogoMaxBytes);
  CHECK(reinterpret_cast<std::uintptr_t>(d.bytes) % 4 == 0);
}

TEST_CASE("boot logo: boot_logo_valid holds the limits") {
  using eve::BitmapFormat;
  CHECK(ui::boot_logo_valid(made(124, 124, 3, BitmapFormat::L4)));
  CHECK(ui::boot_logo_valid(made(800, 420, 1, BitmapFormat::L1)));  // the largest drawn size
  CHECK_FALSE(ui::boot_logo_valid(made(801, 420, 1, BitmapFormat::L1)));
  CHECK_FALSE(ui::boot_logo_valid(made(800, 421, 1, BitmapFormat::L1)));
  CHECK_FALSE(ui::boot_logo_valid(made(401, 210, 2, BitmapFormat::L1)));  // the drawn size is native x scale
  CHECK_FALSE(ui::boot_logo_valid(made(10, 10, 0, BitmapFormat::L1)));
  CHECK_FALSE(ui::boot_logo_valid(made(10, 10, 17, BitmapFormat::L1)));
  CHECK_FALSE(ui::boot_logo_valid(made(10, 10, 1, BitmapFormat::L8)));  // not a logo format
  // The byte limit: 800 x 80 at L4 is 32 KB; 800 x 163 L4 is 65,200 (fits), 800 x 164 is 65,600 (does not); the drawn height is 420 at most.
  CHECK(ui::boot_logo_valid(made(800, 163, 1, BitmapFormat::L4)));
  CHECK_FALSE(ui::boot_logo_valid(made(800, 164, 1, BitmapFormat::L4)));
  // A colour logo counts its palette inside the limit: 250 x 260 is 65,000 bytes + 512 = 65,512 (fits); 250 x 261 is 65,252 + 512, over.
  CHECK(ui::boot_logo_valid(made(250, 260, 1, BitmapFormat::Paletted565)));
  CHECK_FALSE(ui::boot_logo_valid(made(250, 261, 1, BitmapFormat::Paletted565)));
  // A palette that is not where the bitmap ends, or a size that is not the sum, is not consistent.
  ui::BootLogo bad = made(100, 100, 1, BitmapFormat::Paletted565);
  bad.palette_offset += 4;
  CHECK_FALSE(ui::boot_logo_valid(bad));
  bad = made(100, 100, 1, BitmapFormat::L1);
  bad.size += 4;
  CHECK_FALSE(ui::boot_logo_valid(bad));
  bad = made(100, 100, 1, BitmapFormat::L1);
  bad.stride += 1;
  CHECK_FALSE(ui::boot_logo_valid(bad));
}

TEST_CASE("boot logo: the RAM_G region holds the largest logo and the map stays valid") {
  CHECK(ui::plot::kRamGBootLogoBytes >= ui::kBootLogoMaxBytes);
  CHECK(ui::plot::kRamGBootLogoBytes % 4096 == 0);
  CHECK(ui::plot::kRamGShot >= ui::plot::kRamGBootLogo + ui::kBootLogoMaxBytes);
  CHECK(ui::plot::kRamGShot + ui::plot::kRamGShotBytes == ui::plot::kRamGApp);
  CHECK(ui::plot::ram_g_regions_ok(ui::plot::kRamGRegions, ui::plot::kRamGRegionCount));
}

TEST_CASE("boot logo: the layout centres the logo whatever its size, and the line fits under the largest") {
  const ui::BootLayout dflt = ui::boot_layout(ui::boot_logo());
  CHECK(dflt.w == 684);
  CHECK(dflt.h == 420);
  CHECK(dflt.x == 58);  // (800 - 684) / 2
  CHECK(dflt.y == 30);  // (480 - 420) / 2
  CHECK(dflt.text_y == 30 + 420 + ui::kBootTextGap);
  const ui::BootLayout small = ui::boot_layout(made(124, 124, 3, eve::BitmapFormat::L4));
  CHECK(small.w == 372);
  CHECK(small.h == 372);
  CHECK(small.x == 214);
  CHECK(small.y == 54);
  const ui::BootLayout largest = ui::boot_layout(made(800, 420, 1, eve::BitmapFormat::L1));
  CHECK(largest.x == 0);
  CHECK(largest.text_y + ui::kBootTextH <= ui::kScreenH);
}

TEST_CASE("boot logo: the default is drawn from its own data (size, format, stride; no transform at scale 1; BITMAP_SIZE_H past 511)") {
  using eve::BitmapFormat;
  for (const bool translate : {true, false}) {
    const Drawn d = boot_frame(translate, "HELLO", "o3sdk d8c4ede2c609");
    REQUIRE(d.complete);
    CHECK(with_opcode(d.frame, op::kBitmapLayout) == Words{eve::dl::bitmap_layout(BitmapFormat::L1, 86, 420)});
    CHECK(with_opcode(d.frame, op::kBitmapSize) == Words{eve::dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, 684, 420)});
    CHECK(with_opcode(d.frame, op::kBitmapSizeH) == Words{eve::dl::bitmap_size_h(1, 0)});  // 684 = 512 + 172
    CHECK(with_opcode(d.frame, op::kBitmapSource) == Words{eve::dl::bitmap_source(ui::plot::kRamGBootLogo)});
    CHECK(with_opcode(d.frame, op::kBitmapTransformA).empty());
    CHECK(with_opcode(d.frame, op::kBitmapTransformE).empty());
    CHECK(with_opcode(d.frame, op::kPaletteSource).empty());
    CHECK(with_opcode(d.frame, op::kColorRgb).front() == eve::dl::color_rgb(255, 255, 255));
  }
}

TEST_CASE("boot logo: a grey logo at scale 3 is transformed by 85 / 256 and tinted; a small one needs no BITMAP_SIZE_H") {
  const ui::BootLogo tinted = made(124, 124, 3, eve::BitmapFormat::L4, 0xFF8000);
  REQUIRE(ui::boot_logo_valid(tinted));
  const Drawn d = logo_frame(true, tinted, "HELLO", "x");
  REQUIRE(d.complete);
  CHECK(with_opcode(d.frame, op::kBitmapTransformA) == Words{eve::dl::bitmap_transform_a(85)});
  CHECK(with_opcode(d.frame, op::kBitmapTransformE) == Words{eve::dl::bitmap_transform_e(85)});
  CHECK(with_opcode(d.frame, op::kBitmapSize) == Words{eve::dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, 372, 372)});
  CHECK(with_opcode(d.frame, op::kBitmapSizeH).empty());
  CHECK(with_opcode(d.frame, op::kColorRgb).front() == eve::dl::color_rgb(0xFF, 0x80, 0x00));
}

TEST_CASE("boot logo: a colour logo is drawn through its palette, in white (unmodulated), in one pass") {
  const ui::BootLogo c = made(300, 100, 2, eve::BitmapFormat::Paletted565);
  REQUIRE(ui::boot_logo_valid(c));
  REQUIRE(c.palette_offset == 30000);
  const Drawn d = logo_frame(true, c, "HELLO", "x");
  REQUIRE(d.complete);
  CHECK(with_opcode(d.frame, op::kPaletteSource) == Words{eve::dl::palette_source(ui::plot::kRamGBootLogo + 30000)});
  CHECK(with_opcode(d.frame, op::kColorRgb).front() == eve::dl::color_rgb(255, 255, 255));
  CHECK(with_opcode(d.frame, op::kBitmapSizeH) == Words{eve::dl::bitmap_size_h(1, 0)});  // 600 px drawn
  CHECK(with_opcode(d.frame, op::kBegin).size() >= 1);
}

TEST_CASE("boot logo: the frame fits kBootScreenMaxWords for the default and for the logo that needs the most words, in both modes") {
  // The most words a valid logo costs: a colour one (PALETTE_SOURCE), transformed (two words), with BITMAP_SIZE_H (drawn 512 or more).
  const ui::BootLogo most = made(300, 100, 2, eve::BitmapFormat::Paletted565);
  REQUIRE(ui::boot_logo_valid(most));
  const std::string name(16, 'W');
  const std::string build(ui::kBootBuildChars, 'W');  // and the longest line: every glyph a word
  for (const bool translate : {true, false}) {
    CAPTURE(translate);
    const Drawn def = boot_frame(translate, name, build);
    const Drawn big = logo_frame(translate, most, name, build);
    REQUIRE(def.complete);
    REQUIRE(big.complete);
    CHECK(def.frame.size() + 16 <= ui::kBootScreenMaxWords);
    CHECK(big.frame.size() + 16 <= ui::kBootScreenMaxWords);
    CHECK(big.frame.size() == def.frame.size() + 3);  // the transform's two words and the palette's one; both have BITMAP_SIZE_H
  }
}
