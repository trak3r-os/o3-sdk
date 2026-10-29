// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit's file picker and name entry (kit/app file_picker.h, name_entry.h, docs/M2C-PLAN.md section 9): the widgets of kit/ui over Files::list and TextEntry. The picker lists a folder of
// the card as a job of the files service (never waiting for it), keeps the names that end in the asked extension, moves its cursor by the encoder, the arrows, pages and pads, and
// answers Chosen or Cancelled; the name entry types with F1 and the pads, refuses an OK that the files service would refuse, and answers Done or Cancelled.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/file_picker.h>
#include <o3sdk/app/name_entry.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/sim/harness.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace o3sdk;
using app::Button;
using app::FilePicker;
using app::NameEntry;

namespace {

class Blank final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PICK"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

// An app that gives its controls to the picker while it is open, as HELLO does, and notes the last answer.
class PickApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"PICK", "1"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen); }
  void update(app::Context& ctx) noexcept override { picker.update(ctx); }
  bool input(app::Context& ctx, const app::Input& in) noexcept override {
    if (picker.active()) {
      last = picker.input(ctx, in);
      return true;
    }
    return false;
  }
  Blank screen;
  app::BasicFilePicker<8> picker;
  FilePicker::Result last = FilePicker::Result::Browsing;
};

void touch(const std::string& path, std::size_t bytes = 1) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream f(path, std::ios::binary);
  f << std::string(bytes, 'x');
}

struct Rig {
  platform::host::ScratchDir dir{"picker"};
  std::unique_ptr<platform::host::DirCard> card;
  PickApp app;
  std::unique_ptr<sim::Harness> h;
  explicit Rig(bool with_card = true, unsigned files = 3) {
    NEED(dir.ok());
    for (unsigned i = 0; i < files; ++i) {
      touch(dir.path() + "/Apps/hello/" + static_cast<char>('a' + i) + ".o3a", 100u * (i + 1u));
    }
    touch(dir.path() + "/Apps/hello/notes.txt");
    touch(dir.path() + "/Apps/hello/UPPER.O3A", 20000);
    std::filesystem::create_directories(dir.path() + "/Apps/hello/Song");
    if (with_card) {
      card = std::make_unique<platform::host::DirCard>(dir.path());
    }
    h = std::make_unique<sim::Harness>(app, nullptr, nullptr, card.get(), "hello");
    REQUIRE(h->start());
    h->run_ms(30);
  }
  app::Context& ctx() { return h->runtime().context(); }
  void tap(Button b) {
    h->press(b);
    h->run_ms(1);
    h->release(b);
    h->run_ms(1);
  }
  void wait_ready() {
    for (unsigned i = 0; i < 200 && app.picker.state() != FilePicker::State::Ready && app.picker.state() != FilePicker::State::Failed; ++i) {
      h->run_ms(2);
    }
  }
};

}  // namespace

TEST_CASE("file picker: it lists the folder, keeps the files that end in the extension (any case), leaves the project folders out unless asked, and the cursor moves") {
  Rig r;
  NEED(r.app.picker.open(r.ctx(), "LOAD", "", "o3a"));
  CHECK(r.app.picker.active());
  r.wait_ready();
  NEED(r.app.picker.state() == FilePicker::State::Ready);
  CHECK(r.app.picker.count() == 4u);  // a.o3a, b.o3a, c.o3a and UPPER.O3A; notes.txt and the folder are not wanted
  CHECK_FALSE(r.app.picker.truncated());
  r.tap(Button::Down);
  r.tap(Button::Down);
  r.h->turn(1);
  r.h->run_ms(2);
  r.h->turn(5);  // clamped at the last
  r.h->run_ms(2);
  r.tap(Button::Up);
  r.tap(Button::F1);
  CHECK(r.app.last == FilePicker::Result::Chosen);
  CHECK_FALSE(r.app.picker.active());
  CHECK(std::string(r.app.picker.chosen().name) == "c.o3a");  // the listing is sorted: a, b, c, UPPER (the encoder went to the end, UP came back one)
  CHECK(r.app.picker.chosen().size == 300u);
  CHECK_FALSE(r.app.picker.chosen().folder);
  // The listing has room for eight entries before the filter: a folder of more says so.
  Rig many(true, 8);
  NEED(many.app.picker.open(many.ctx(), "LOAD", "", "o3a"));
  many.wait_ready();
  CHECK(many.app.picker.truncated());
  CHECK(many.app.picker.count() <= 8u);
}

TEST_CASE("file picker: folders are listed on request, F8 cancels, F1 on an empty list chooses nothing, a pad selects a row") {
  Rig r;
  NEED(r.app.picker.open(r.ctx(), "OPEN", "", nullptr, true));
  r.wait_ready();
  NEED(r.app.picker.state() == FilePicker::State::Ready);
  CHECK(r.app.picker.count() == 6u);  // the folder Song and the five files (no extension asked: notes.txt too); the picker has room for eight
  CHECK_FALSE(r.app.picker.truncated());
  r.tap(Button::F1);
  CHECK(r.app.last == FilePicker::Result::Chosen);
  CHECK(r.app.picker.chosen().folder);
  CHECK(std::string(r.app.picker.chosen().name) == "Song");
  NEED(r.app.picker.open(r.ctx(), "OPEN", "", "o3a"));
  r.wait_ready();
  r.h->pad_down(2);  // the third visible row
  r.h->run_ms(2);
  r.h->pad_up(2);
  r.h->run_ms(2);
  r.tap(Button::F1);
  CHECK(std::string(r.app.picker.chosen().name) == "c.o3a");
  NEED(r.app.picker.open(r.ctx(), "OPEN", "", "o3a"));
  r.wait_ready();
  r.tap(Button::F8);
  CHECK(r.app.last == FilePicker::Result::Cancelled);
  CHECK_FALSE(r.app.picker.active());
  // A folder with no file of the extension: an empty list, and F1 has nothing to choose.
  NEED(r.app.picker.open(r.ctx(), "OPEN", "", "wav"));
  r.wait_ready();
  CHECK(r.app.picker.count() == 0u);
  r.tap(Button::F1);
  CHECK(r.app.picker.active());
  r.tap(Button::F8);
  CHECK_FALSE(r.app.picker.active());
}

TEST_CASE("file picker: no card, a card that went out under the listing, and a folder the service does not take each fail it with the service's words, and draw and soft keys never fault") {
  Rig none(false);
  CHECK_FALSE(none.app.picker.open(none.ctx(), "LOAD", "", "o3a"));
  CHECK(none.app.picker.state() == FilePicker::State::Failed);
  CHECK(std::string(none.app.picker.failure()) == "NO CARD");
  none.tap(Button::F8);
  CHECK_FALSE(none.app.picker.active());

  Rig r;
  CHECK_FALSE(r.app.picker.open(r.ctx(), "LOAD", "../etc", "o3a"));  // not a folder the guard takes
  CHECK(r.app.picker.state() == FilePicker::State::Failed);
  CHECK(std::string(r.app.picker.failure()) != "");
  r.app.picker.close();
  NEED(r.app.picker.open(r.ctx(), "LOAD", "", "o3a"));
  r.wait_ready();
  r.h->runtime().draw();  // the list, drawn
  app::SoftKeys keys;
  r.app.picker.soft_keys(keys);
  CHECK(std::string(keys.label[0]) == "OPEN");
  CHECK(std::string(keys.label[7]) == "BACK");
  r.h->card_out();
  r.h->run_ms(5);
  r.h->card_in();
  r.h->run_ms(5);
  CHECK(r.app.picker.state() == FilePicker::State::Failed);  // the listing is stale: the card was out
  CHECK(std::string(r.app.picker.failure()) == "CARD CHANGED");
  r.h->runtime().draw();
  r.app.picker.soft_keys(keys);
  CHECK(keys.label[0] == nullptr);
}

TEST_CASE("name entry: F1 types the key in focus, a pad types the key under it, DELETE erases, OK is refused for a name the files service would refuse") {
  NameEntry n;
  CHECK_FALSE(n.active());
  n.open("SAVE AS", "riff");
  CHECK(n.active());
  CHECK(std::string(n.text()) == "riff");
  CHECK(n.entry().selected());
  app::Input in{};
  in.type = app::Input::Type::Press;
  in.kind = core::InputKind::Pad;
  in.id = 24;  // the key A
  CHECK(n.input(in) == NameEntry::Result::Editing);
  CHECK(std::string(n.text()) == "a");  // the selected name was replaced
  in.id = 25;
  (void)n.input(in);
  CHECK(std::string(n.text()) == "as");
  const auto press = [&](Button b, bool shift = false) {
    app::Input k{};
    k.type = app::Input::Type::Press;
    k.kind = core::InputKind::Button;
    k.id = static_cast<std::uint8_t>(b);
    k.shift = shift;
    return n.input(k);
  };
  CHECK(press(Button::Delete) == NameEntry::Result::Editing);
  CHECK(std::string(n.text()) == "a");
  CHECK(press(Button::Delete) == NameEntry::Result::Editing);
  CHECK(std::string(n.text()) == "");
  CHECK_FALSE(n.acceptable());
  CHECK(press(Button::F7) == NameEntry::Result::Editing);  // empty: refused
  CHECK(n.active());
  // A name that is a dot or a space at the end, or does not start with a letter or digit, has not the shape: the keyboard says so and stays.
  for (const char* bad : {"-x", "x ", "x.", "x..y"}) {
    n.open("SAVE AS", bad);
    CHECK_FALSE(n.acceptable());
    CHECK(press(Button::F7) == NameEntry::Result::Editing);
    CHECK(n.active());
  }
  n.open("SAVE AS", "My riff 2");
  CHECK(n.acceptable());
  // The focus moves with the encoder and the arrows; F1 types the key in focus (the backspace key is the start: it erases the selected name first).
  app::Input turn{};
  turn.type = app::Input::Type::Turn;
  turn.kind = core::InputKind::Encoder;
  turn.value = 1;
  (void)n.input(turn);
  (void)press(Button::Down);
  (void)press(Button::Left);
  (void)press(Button::Right);
  (void)press(Button::Up);
  (void)press(Button::F1);
  CHECK(press(Button::F7) == NameEntry::Result::Done);
  CHECK_FALSE(n.active());
  n.open("SAVE AS", "x");
  CHECK(press(Button::F8) == NameEntry::Result::Cancelled);
  CHECK_FALSE(n.active());
  // Any: the characters the file filter refuses are typed, and OK takes whatever is not empty.
  n.open("TEXT", "@+ x.", NameEntry::Kind::Any);
  CHECK(n.acceptable());
  app::SoftKeys keys;
  n.soft_keys(keys);
  CHECK(std::string(keys.label[0]) == "TYPE");
  CHECK(std::string(keys.label[6]) == "OK");
  CHECK(std::string(keys.label[7]) == "CANCEL");
  CHECK(keys.dim == 0u);
  n.open("T", "", NameEntry::Kind::FileName);
  n.soft_keys(keys);
  CHECK(keys.dim != 0u);  // OK is dim while it would be refused
}
