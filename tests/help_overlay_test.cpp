// SPDX-License-Identifier: GPL-3.0-or-later
//
// HELP, Shift + Config (kit/app help.h, help_overlay.h, runtime.cpp route() and draw(); docs/FRAMEWORK.md, "Help"): the route (Shift + Config opens it, Config alone is still the system Config, Config
// or F8 closes it, the pages change, the app hears nothing while it is open but still gets the release of what it saw pressed), the drawing (48 pads in a 12 x 4 grid in the colours of their groups,
// in the screen's theme; a label cut at five characters; a screen with no help()) and the frame's word count, which the kit holds under HelpOverlay::kWordBudget whatever the texts say.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/help.h>
#include <o3sdk/app/help_overlay.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/ui/theme.h>

#include <cstdint>
#include <cstring>
#include <string>

using namespace o3sdk;
using app::Button;
using app::Help;
using app::HelpOverlay;

namespace {

// What the screen saw: its inputs, and the state HELP asked it in.
class Probe final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PROBE"; }
  void input(app::Context&, const app::Input& in) noexcept override {
    if (in.type == app::Input::Type::Press) {
      ++presses;
    } else if (in.type == app::Input::Type::Release) {
      ++releases;
      last_release_kind = in.kind;
      last_release_id = in.id;
    } else {
      ++turns;
    }
  }
  void draw(app::Context&, ui::Renderer& r, const ui::Rect& area) noexcept override {
    ++draws;
    r.panel(area, ui::Color::Pane);
  }
  void soft_keys(app::Context& ctx, app::SoftKeys& keys) noexcept override {
    keys.label[0] = ctx.panel().shift() ? "SHIFTED" : "PLAIN";
    keys.label[1] = "TWO";
    keys.dim = 0x02;
    if (ctx.panel().shift()) {
      ++soft_with_shift;
    }
  }
  void help(const app::Context& ctx, Help& h) const noexcept override {
    ++help_calls;
    if (ctx.panel().shift()) {
      ++help_with_shift;
    }
    if (has_help) {
      h = data;
    }
  }

  bool has_help = false;
  Help data;
  unsigned presses = 0;
  unsigned releases = 0;
  unsigned turns = 0;
  unsigned draws = 0;
  core::InputKind last_release_kind = core::InputKind::None;
  unsigned last_release_id = 99;
  unsigned soft_with_shift = 0;
  mutable unsigned help_calls = 0;
  mutable unsigned help_with_shift = 0;
};

class Other final : public app::Screen {
 public:
  const char* title() const noexcept override { return "OTHER"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

class ProbeApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"HELPTEST", "1"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen); }
  Probe screen;
  Other other;
};

struct Frame {
  explicit Frame(const sim::Harness& h) : px(h.raster().rgba()) {}
  std::uint32_t at(int x, int y) const {
    const std::uint8_t* p = px + (static_cast<std::size_t>(y) * 800u + static_cast<std::size_t>(x)) * 4u;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | std::uint32_t{p[2]};
  }
  const std::uint8_t* px;
};
constexpr std::uint32_t pack(ui::Rgb c) { return (std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | std::uint32_t{c.b}; }

struct Rig {
  ProbeApp app;
  sim::Harness h{app};
  Rig() {
    REQUIRE(h.start());
    h.run_ms(80);
  }
  Probe& screen() { return app.screen; }
  app::Runtime& rt() { return h.runtime(); }
  void tap(Button b) {
    h.press(b);
    h.run_ms(2);
    h.release(b);
    h.run_ms(40);
  }
  void shift_config() {
    h.press(Button::Shift);
    h.run_ms(2);
    h.press(Button::Config);
    h.run_ms(2);
    h.release(Button::Config);
    h.run_ms(2);
    h.release(Button::Shift);
    h.run_ms(60);
  }
  const ui::Theme& theme() { return ui::theme(rt().context().theme()); }
  // The top-left pad of the grid, and the pitch (help_overlay.cpp: the grid is centred, 58 x 50 pads).
  static constexpr int kX0 = 19;
  static constexpr int kY0 = 40;
  std::uint32_t pad_pixel(unsigned row, unsigned col) {  // inside the pad, left of its label
    return Frame(h).at(kX0 + static_cast<int>(col) * 64 + 4, kY0 + static_cast<int>(row) * 56 + 25);
  }
};

constexpr app::HelpPadGroup kGroups[] = {
    {"KEYS", "THE PLAYING PADS", ui::Color::Sel},
    {"MODS", "HOLD FOR A MODIFIER", ui::Color::Play},
};

}  // namespace

TEST_CASE("route: Shift + Config opens HELP, Config alone is the system Config, and the Config button never reaches the app") {
  Rig r;
  r.tap(Button::Config);
  CHECK(r.rt().system_open());
  CHECK_FALSE(r.rt().help_open());
  r.tap(Button::Config);
  CHECK_FALSE(r.rt().system_open());

  r.shift_config();
  CHECK(r.rt().help_open());
  CHECK_FALSE(r.rt().system_open());
  CHECK(r.rt().help_overlay().page() == HelpOverlay::Page::Buttons);
  r.tap(Button::Config);  // closes it, with or without Shift
  CHECK_FALSE(r.rt().help_open());
  CHECK_FALSE(r.rt().system_open());
  r.shift_config();
  REQUIRE(r.rt().help_open());
  r.h.press(Button::Shift);
  r.tap(Button::Config);
  r.h.release(Button::Shift);
  r.h.run_ms(40);
  CHECK_FALSE(r.rt().help_open());
  CHECK(r.screen().presses == 2);  // the two Shift presses of the closing: the app saw Shift, never Config
}

TEST_CASE("route: F8 closes, F1 F2 Left and Right change the page, and the screen is as it was") {
  Rig r;
  const unsigned draws = r.screen().draws;
  r.shift_config();
  REQUIRE(r.rt().help_open());
  r.tap(Button::F2);
  CHECK(r.rt().help_overlay().page() == HelpOverlay::Page::Pads);
  r.tap(Button::F1);
  CHECK(r.rt().help_overlay().page() == HelpOverlay::Page::Buttons);
  r.tap(Button::Right);
  CHECK(r.rt().help_overlay().page() == HelpOverlay::Page::Pads);
  r.tap(Button::Right);
  CHECK(r.rt().help_overlay().page() == HelpOverlay::Page::Pads);
  r.tap(Button::Left);
  CHECK(r.rt().help_overlay().page() == HelpOverlay::Page::Buttons);
  r.tap(Button::F8);
  CHECK_FALSE(r.rt().help_open());
  CHECK_FALSE(r.rt().system_open());
  r.shift_config();  // and it opens again on BUTTONS
  CHECK(r.rt().help_overlay().page() == HelpOverlay::Page::Buttons);
  r.tap(Button::Config);
  r.h.run_ms(80);
  CHECK(r.screen().draws > draws);  // the screen is drawn again, and was not while HELP was up
  CHECK(r.screen().presses == 2);   // the two Shift presses and nothing else: the app's state is what it was
  CHECK(r.screen().turns == 0);
}

TEST_CASE("route: the app hears no press or turn while HELP is open, but gets the release of what it saw pressed") {
  Rig r;
  r.h.pad_down(5);  // held when HELP opens
  r.h.run_ms(10);
  REQUIRE(r.screen().presses == 1);
  r.h.press(Button::Shift);
  r.h.run_ms(2);
  r.h.press(Button::Config);
  r.h.run_ms(2);
  r.h.release(Button::Config);
  r.h.run_ms(40);
  REQUIRE(r.rt().help_open());
  const unsigned presses = r.screen().presses;
  CHECK(presses == 2);  // the pad and Shift

  r.h.pad_down(9);  // HELP's: no press, and its release is not the app's either
  r.h.run_ms(10);
  r.h.press(Button::Play);
  r.h.run_ms(10);
  r.h.turn(3);
  r.h.run_ms(10);
  r.h.release(Button::Play);
  r.h.pad_up(9);
  r.h.run_ms(10);
  CHECK(r.screen().presses == presses);
  CHECK(r.screen().turns == 0);
  CHECK(r.screen().releases == 0);

  r.h.pad_up(5);  // seen pressed: released to the app
  r.h.run_ms(10);
  CHECK(r.screen().releases == 1);
  CHECK(r.screen().last_release_kind == core::InputKind::Pad);
  CHECK(r.screen().last_release_id == 5);
  r.h.release(Button::Shift);  // and so is the Shift that opened it
  r.h.run_ms(10);
  CHECK(r.screen().releases == 2);
  CHECK(r.screen().last_release_kind == core::InputKind::Button);
  CHECK(r.screen().last_release_id == static_cast<unsigned>(Button::Shift));
  CHECK(r.rt().help_open());
}

TEST_CASE("route: the screen is asked once, without Shift in the panel's state, and the panel has it again after") {
  Rig r;
  r.shift_config();
  CHECK(r.screen().help_calls == 1);
  CHECK(r.screen().help_with_shift == 0);
  CHECK(r.screen().soft_with_shift == 0);  // (its soft keys were asked for the caps without Shift too)
  r.h.press(Button::Shift);
  r.h.run_ms(10);
  CHECK(r.rt().context().panel().shift());
  CHECK(r.screen().help_calls == 1);  // nothing asks again while it is open
  r.tap(Button::F2);
  r.tap(Button::F1);
  CHECK(r.screen().help_calls == 1);
  r.h.release(Button::Shift);
}

TEST_CASE("route: the system Config never opens over HELP, a locked input closes nothing, and a change of screen closes it") {
  Rig r;
  r.shift_config();
  REQUIRE(r.rt().help_open());
  r.rt().lock_input(true);
  r.tap(Button::Config);
  r.tap(Button::F8);
  CHECK(r.rt().help_open());  // locked: it reaches nobody
  r.rt().lock_input(false);
  r.tap(Button::Config);
  CHECK_FALSE(r.rt().help_open());
  CHECK_FALSE(r.rt().system_open());

  r.shift_config();
  REQUIRE(r.rt().help_open());
  r.rt().context().show(r.app.other);
  r.h.run_ms(80);
  CHECK_FALSE(r.rt().help_open());
  CHECK(r.rt().context().screen() == &r.app.other);

  r.tap(Button::Config);  // Config over the system screen: Shift + Config closes the Config, it does not open HELP
  REQUIRE(r.rt().system_open());
  r.shift_config();
  CHECK_FALSE(r.rt().system_open());
  CHECK_FALSE(r.rt().help_open());
}

TEST_CASE("route: the header says HELP and the screen's title, the soft keys are BUTTONS, PADS and CLOSE with the page hot") {
  Rig r;
  r.shift_config();
  REQUIRE(r.rt().help_open());
  app::SoftKeys keys;
  r.rt().help_overlay().soft_keys(keys);
  CHECK(std::strcmp(keys.label[0], "BUTTONS") == 0);
  CHECK(std::strcmp(keys.label[1], "PADS") == 0);
  CHECK(std::strcmp(keys.label[7], "CLOSE") == 0);
  CHECK(keys.hot == 0x01);
  r.tap(Button::F2);
  r.rt().help_overlay().soft_keys(keys);
  CHECK(keys.hot == 0x02);
  // The frame is the kit's chrome over HELP: the header strip and the key row are not the pane.
  const Frame f(r.h);
  const std::uint32_t pane = pack(r.theme().rgb(ui::Color::Pane));
  CHECK(f.at(400, 16) != pane);
  CHECK(f.at(400, 480 - 11) != pane);
}

TEST_CASE("drawing: the pads are a grid of 12 x 4 in the colour of their group, a pad in no group is dark") {
  Rig r;
  Help& d = r.screen().data;
  r.screen().has_help = true;
  d.groups = kGroups;
  d.group_count = 2;
  app::help_pads(d, 1, 0, 3, 11, 1);  // three rows of KEYS, the top row in no group
  app::help_pads(d, 2, 2, 2, 4, 2);   // a block of MODS inside them
  r.shift_config();
  r.tap(Button::F2);
  REQUIRE(r.rt().help_overlay().page() == HelpOverlay::Page::Pads);
  const ui::Theme& t = r.theme();
  const std::uint32_t pane = pack(t.rgb(ui::Color::Pane));
  const std::uint32_t keys = pack(t.rgb(ui::Color::Sel));
  const std::uint32_t mods = pack(t.rgb(ui::Color::Play));
  for (unsigned col = 0; col < app::kPadCols; ++col) {
    CHECK(r.pad_pixel(0, col) == pane);  // dark: the pane inside, not the group's colour
    CHECK(r.pad_pixel(1, col) == keys);
    CHECK(r.pad_pixel(3, col) == keys);
    CHECK(r.pad_pixel(2, col) == (col >= 2 && col <= 4 ? mods : keys));
  }
  // Between the pads is the pane: the grid is 12 across and 4 down, not more.
  const Frame f(r.h);
  CHECK(f.at(Rig::kX0 + 58 + 2, Rig::kY0 + 25) == pane);                 // between two pads of the top row
  CHECK(f.at(Rig::kX0 + 12 * 64 + 4, Rig::kY0 + 56 * 1 + 25) == pane);   // no thirteenth column
  CHECK(f.at(Rig::kX0 + 600, Rig::kY0 + 56 * 4 + 25) == pane);           // no fifth row (below the grid is the legend, at the left)
}

TEST_CASE("drawing: the group colours are tokens: another theme draws them with its own values") {
  Rig r;
  r.screen().has_help = true;
  r.screen().data.groups = kGroups;
  r.screen().data.group_count = 2;
  app::help_pads(r.screen().data, 0, 0, 3, 11, 1);
  r.rt().context().set_theme(ui::ThemeId::White);
  r.shift_config();
  r.tap(Button::F2);
  const ui::Theme& white = ui::theme(ui::ThemeId::White);
  CHECK(r.pad_pixel(0, 0) == pack(white.rgb(ui::Color::Sel)));
  CHECK(r.pad_pixel(3, 11) == pack(white.rgb(ui::Color::Sel)));
  r.tap(Button::F8);
  r.rt().context().set_theme(ui::ThemeId::Black);
  r.shift_config();
  r.tap(Button::F2);
  const ui::Theme& black = ui::theme(ui::ThemeId::Black);
  CHECK(r.pad_pixel(0, 0) == pack(black.rgb(ui::Color::Sel)));
  CHECK(pack(black.rgb(ui::Color::Sel)) != pack(white.rgb(ui::Color::Sel)));
}

TEST_CASE("drawing: a pad label is cut at five characters, never wider") {
  const char* five = "ABCDE";
  const char* eight = "ABCDEFGH";
  std::uint32_t digest[2] = {};
  unsigned words[2] = {};
  for (unsigned v = 0; v < 2; ++v) {
    Rig r;
    r.screen().has_help = true;
    r.screen().data.groups = kGroups;
    r.screen().data.group_count = 2;
    app::help_pads(r.screen().data, 0, 0, 3, 11, 1);
    r.screen().data.pad_label[0] = v == 0 ? five : eight;
    r.screen().data.pad_label[47] = v == 0 ? five : eight;
    r.shift_config();
    r.tap(Button::F2);
    r.h.run_ms(80);
    digest[v] = r.h.raster().last_frame_digest();
    words[v] = r.rt().last_frame_words();
  }
  CHECK(words[0] == words[1]);
  CHECK(digest[0] == digest[1]);  // the very same frame: the three extra characters were not drawn
}

TEST_CASE("drawing: a screen with no help() still draws both pages: its soft keys on the caps, no pad help") {
  Rig r;
  r.shift_config();
  REQUIRE(r.rt().help_open());
  r.h.run_ms(80);
  CHECK(r.rt().frame_failures() == 0);
  CHECK(r.rt().help_overlay().help().group_count == 0);
  CHECK(r.rt().help_overlay().rows() > 0);
  CHECK(r.rt().help_overlay().rows_shown() >= 10);  // the buttons are dim and cheap: the height is what limits the list, not the words
  // The F row's caps are drawn in the chrome colour along the bottom, over the F keys.
  const Frame f(r.h);
  const ui::Theme& t = r.theme();
  CHECK(f.at(4 + 46, 458 - 4 - 62 + 58) == pack(t.rgb(ui::Color::Chrome)));
  r.tap(Button::F2);
  CHECK(r.rt().frame_failures() == 0);
  for (unsigned pad = 0; pad < app::kPadCount; ++pad) {
    CHECK(r.pad_pixel(pad / 12, pad % 12) == pack(t.rgb(ui::Color::Pane)));  // every pad dark
  }
}

TEST_CASE("help_pads: a rectangle of the grid gets a group and its labels, row by row; a range past the grid is cut") {
  Help h;
  const char* names[6] = {"A", "B", "C", "D", "E", "F"};
  app::help_pads(h, 1, 2, 2, 4, 3, names);  // rows 1..2, columns 2..4
  CHECK(h.pad_group[app::pad_at(1, 2)] == 3);
  CHECK(h.pad_group[app::pad_at(2, 4)] == 3);
  CHECK(h.pad_group[app::pad_at(1, 1)] == 0);
  CHECK(h.pad_group[app::pad_at(1, 5)] == 0);
  CHECK(h.pad_group[app::pad_at(3, 3)] == 0);
  CHECK(std::strcmp(h.pad_label[app::pad_at(1, 2)], "A") == 0);
  CHECK(std::strcmp(h.pad_label[app::pad_at(1, 4)], "C") == 0);
  CHECK(std::strcmp(h.pad_label[app::pad_at(2, 2)], "D") == 0);
  CHECK(std::strcmp(h.pad_label[app::pad_at(2, 4)], "F") == 0);

  Help g;
  const char* rows[6] = {"1", "2", "3", "4", "5", "6"};
  app::help_pads(g, 3, 10, 9, 20, 1, rows);  // past the bottom and the right: only (3, 10) and (3, 11) are on the grid
  CHECK(g.pad_group[app::pad_at(3, 10)] == 1);
  CHECK(g.pad_group[app::pad_at(3, 11)] == 1);
  CHECK(std::strcmp(g.pad_label[app::pad_at(3, 11)], "2") == 0);
  unsigned set = 0;
  for (unsigned i = 0; i < app::kPadCount; ++i) {
    set += g.pad_group[i] != 0 ? 1u : 0u;
  }
  CHECK(set == 2);
  Help e;
  app::help_pads(e, 2, 0, 1, 0, 1);  // an empty rectangle
  CHECK(e.pad_group[app::pad_at(1, 0)] == 0);
  CHECK(e.pad_group[app::pad_at(2, 0)] == 0);
}

namespace {

// Every text at its limit: the worst a screen can ask the kit to draw. Static, as help() promises.
struct Worst {
  char texts[25][app::kHelpButtonText + 1];
  char shifts[25][app::kHelpButtonText + 1];
  char fkey[8][2 * app::kHelpFKeyLine + 2];
  char fshift[8][app::kHelpFKeyShift + 1];
  char summary[2 * app::kHelpSummaryLine + 1];
  char encoder[app::kHelpButtonText + 1];
  char labels[app::kPadCount][app::kHelpPadLabel + 1];
  char gname[8][app::kHelpGroupName + 1];
  char gtext[8][app::kHelpGroupText + 1];
  char pads[app::kHelpPadsText + 1];
  app::HelpButton buttons[25];
  app::HelpPadGroup groups[8];
};

void fill(char* out, unsigned n, char c) {
  for (unsigned i = 0; i < n; ++i) {
    out[i] = static_cast<char>(c + static_cast<char>(i % 20));  // letters only: no space is a free glyph
  }
  out[n] = '\0';
}

const Worst& worst() {
  static const Worst w = [] {
    Worst x{};
    constexpr Button order[25] = {Button::Params,     Button::Performance, Button::File,    Button::Config, Button::Master, Button::SampleLoad, Button::SamplePlay, Button::SampleEdit, Button::SampleRec,
                                  Button::Song,       Button::Note,        Button::Instr,   Button::Vol,    Button::Fx,     Button::Pattern,    Button::Play,       Button::Rec,        Button::Copy,
                                  Button::Insert,     Button::Delete,      Button::Shift,   Button::Up,     Button::Down,   Button::Left,       Button::Right};
    for (unsigned i = 0; i < 25; ++i) {
      fill(x.texts[i], app::kHelpButtonText, 'A');
      fill(x.shifts[i], app::kHelpButtonText, 'A');
      x.buttons[i] = app::HelpButton{order[i], x.texts[i], x.shifts[i]};
    }
    for (unsigned i = 0; i < 8; ++i) {
      fill(x.fkey[i], 2 * app::kHelpFKeyLine, 'A');  // no space: a hard break at the line's width
      fill(x.fshift[i], app::kHelpFKeyShift, 'A');
      fill(x.gname[i], app::kHelpGroupName, 'A');
      fill(x.gtext[i], app::kHelpGroupText, 'A');
      x.groups[i] = app::HelpPadGroup{x.gname[i], x.gtext[i], static_cast<ui::Color>(static_cast<unsigned>(ui::Color::Sel) + i)};
    }
    fill(x.summary, 2 * app::kHelpSummaryLine, 'A');
    fill(x.encoder, app::kHelpButtonText, 'A');
    fill(x.pads, app::kHelpPadsText, 'A');
    for (unsigned i = 0; i < app::kPadCount; ++i) {
      fill(x.labels[i], app::kHelpPadLabel, 'A');
    }
    return x;
  }();
  return w;
}

void fill_worst(Probe& s) {
  const Worst& w = worst();
  s.has_help = true;
  Help& d = s.data;
  d.summary = w.summary;
  for (unsigned i = 0; i < 8; ++i) {
    d.fkey[i] = w.fkey[i];
    d.fkey_shift[i] = w.fshift[i];
  }
  d.buttons = w.buttons;
  d.button_count = 25;
  d.encoder = w.encoder;
  for (unsigned i = 0; i < app::kPadCount; ++i) {
    d.pad_label[i] = w.labels[i];
    d.pad_group[i] = static_cast<std::uint8_t>(1 + i % 8);
  }
  d.groups = w.groups;
  d.group_count = 8;
  d.pads = w.pads;
}

}  // namespace

TEST_CASE("budget: with every text at its limit a HELP frame stays under the word budget, on every scroll position and with the system strip up") {
  Rig r;
  fill_worst(r.screen());
  r.shift_config();
  REQUIRE(r.rt().help_open());
  const HelpOverlay& o = r.rt().help_overlay();
  const char* strip = "A SYSTEM LINE OF THE SEVENTY TWO CHARACTERS THE STRIP HOLDS AT MOST, ALL OF THEM";
  unsigned widest = 0;
  const unsigned max = o.scroll_max();
  CHECK(max > 0);                           // at the limit the list needs its scroll
  CHECK(o.rows_shown() < o.rows());
  for (unsigned step = 0; step <= max + 1u; ++step) {
    r.rt().system_strip(strip, 500, 60'000);
    r.h.run_ms(80);
    CHECK(r.rt().frame_failures() == 0);
    CHECK(r.rt().last_frame_words() < HelpOverlay::kWordBudget);
    widest = r.rt().last_frame_words() > widest ? r.rt().last_frame_words() : widest;
    CHECK(o.scroll() + o.rows_shown() <= o.rows());
    r.tap(Button::Down);
  }
  CHECK(o.scroll() == max);
  CHECK(o.scroll() + o.rows_shown() == o.rows());  // at the end the last row shows
  MESSAGE("worst BUTTONS frame: " << widest << " words");
  r.tap(Button::F2);
  REQUIRE(o.page() == HelpOverlay::Page::Pads);
  r.rt().system_strip(strip, 500, 60'000);
  r.h.run_ms(80);
  CHECK(r.rt().frame_failures() == 0);
  CHECK(r.rt().last_frame_words() < HelpOverlay::kWordBudget);
  MESSAGE("worst PADS frame: " << r.rt().last_frame_words() << " words");
  // The same under the other two themes (the same words, the colours differ) and in the header's busiest state.
  for (const ui::ThemeId id : {ui::ThemeId::Black, ui::ThemeId::White}) {
    r.rt().context().set_theme(id);
    r.tap(Button::F1);
    r.h.run_ms(80);
    CHECK(r.rt().last_frame_words() < HelpOverlay::kWordBudget);
    r.tap(Button::F2);
    CHECK(r.rt().last_frame_words() < HelpOverlay::kWordBudget);
  }
}

TEST_CASE("budget: Up, Down and the encoder scroll the list by rows within its bounds, and only on BUTTONS") {
  Rig r;
  fill_worst(r.screen());
  r.shift_config();
  const HelpOverlay& o = r.rt().help_overlay();
  REQUIRE(o.scroll_max() > 1);
  CHECK(o.scroll() == 0);
  r.tap(Button::Up);
  CHECK(o.scroll() == 0);
  r.tap(Button::Down);
  CHECK(o.scroll() == 1);
  r.h.turn(2);
  r.h.run_ms(40);
  CHECK(o.scroll() == 3);
  r.h.turn(-1);
  r.h.run_ms(40);
  CHECK(o.scroll() == 2);
  r.h.turn(100);
  r.h.run_ms(40);
  CHECK(o.scroll() == o.scroll_max());
  r.h.turn(-100);
  r.h.run_ms(40);
  CHECK(o.scroll() == 0);
  r.tap(Button::Down);
  r.tap(Button::F2);
  r.tap(Button::Down);  // not on PADS
  r.h.turn(3);
  r.h.run_ms(40);
  CHECK(o.scroll() == 1);
  r.tap(Button::F8);
  r.shift_config();  // opens at the top again
  CHECK(o.scroll() == 0);
}

TEST_CASE("budget: a screen with a few short texts shows more rows than the worst case, and the worst case still shows a useful page") {
  Rig light;
  light.screen().has_help = true;
  static const app::HelpButton kFew[] = {{Button::Play, "START", "STOP"}, {Button::Rec, "RECORD", nullptr}};
  light.screen().data.buttons = kFew;
  light.screen().data.button_count = 2;
  light.screen().data.summary = "A SHORT LINE";
  light.shift_config();
  CHECK(light.rt().help_overlay().rows_shown() >= 10);  // the height limits the list here, not the words

  Rig heavy;
  fill_worst(heavy.screen());
  heavy.shift_config();
  CHECK(heavy.rt().help_overlay().rows_shown() >= 4);  // still a useful page at the limit
  CHECK(heavy.rt().help_overlay().rows_shown() < light.rt().help_overlay().rows_shown());
  MESSAGE("rows shown of " << heavy.rt().help_overlay().rows() << ": " << heavy.rt().help_overlay().rows_shown() << " at the limits, " << light.rt().help_overlay().rows_shown() << " with a few short texts");
}

// help_check() counts the lines of the summary and of the soft keys' texts with help_wrap(), the function the overlay draws with: what the check calls too long is what the overlay does not draw.
namespace {
unsigned words_with(const char* summary, const char* fkey) {
  Rig r;
  r.screen().has_help = true;
  r.screen().data.summary = summary;
  r.screen().data.fkey[3] = fkey;
  r.shift_config();
  REQUIRE(r.rt().help_open());
  r.h.run_ms(80);
  return r.rt().last_frame_words();
}
}  // namespace

TEST_CASE("check: the overlay draws as many lines as help_lines() counts, up to two") {
  const std::string line = "AAAAAAAAAAA AAAAAAAAAAA AAAAAAAAAAA AAAAAAAAAAA AAAAAAAAAAA AAAAAAAAAAA AAAAAAAAAAA AAAAAAAAAAA";  // 8 x 11 + 7 = 95 characters
  const std::string fk = "AAAAAAAAAAAA BBBBBBBBBBBB";                                                                         // two lines of 12 at 14
  REQUIRE(line.size() <= app::kHelpSummaryLine);
  REQUIRE(app::help_lines(line.c_str(), app::kHelpSummaryLine) == 1);
  const std::string two = line + " " + line;
  const std::string three = two + " CCCC";
  REQUIRE(app::help_lines(two.c_str(), app::kHelpSummaryLine) == 2);
  REQUIRE(app::help_lines(three.c_str(), app::kHelpSummaryLine) == 3);
  const std::string fk3 = fk + " CCCC";
  REQUIRE(app::help_lines(fk.c_str(), app::kHelpFKeyLine) == 2);
  REQUIRE(app::help_lines(fk3.c_str(), app::kHelpFKeyLine) == 3);

  const unsigned one_line = words_with(line.c_str(), fk.c_str());
  const unsigned two_lines = words_with(two.c_str(), fk.c_str());
  const unsigned three_lines = words_with(three.c_str(), fk3.c_str());
  CHECK(two_lines > one_line);          // the second line is drawn
  CHECK(three_lines == two_lines);      // the third (of the summary and of the F key) is not
  // And the check calls exactly that Help too long.
  Help h;
  h.summary = three.c_str();
  app::HelpProblem p;
  CHECK_FALSE(app::help_check(h, &p));
  CHECK(p.field == app::HelpField::Summary);
  h.summary = two.c_str();
  h.fkey[3] = fk3.c_str();
  CHECK_FALSE(app::help_check(h, &p));
  CHECK(p.field == app::HelpField::FKey);
  CHECK(p.index == 3);
  h.fkey[3] = fk.c_str();
  CHECK(app::help_check(h));
}
