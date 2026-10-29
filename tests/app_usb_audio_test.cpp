// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB audio for an app (docs/M2C-PLAN.md section 7, slice U): Context::usb_audio() over the unit's own service on a fake port and a fake computer (platform/host/fake_usb_audio.h) in the
// simulator's harness. Proved here: nothing of it exists on a platform without the device; the boot layout and the state line; the Config's USB rows ask F1 twice, drop a proposal at any other
// key or after kAskMs, and the port re-enumerates once, with the factory map of the new profile; the layout, the level and an edited map are kept with the settings and come back at the next
// boot; CARD AS DISK gives the card to the computer through the same two presses, a save is refused until the eject, the kit's settings stay dirty and are written after it, and a card in use
// is refused; with the audio in's source Usb an instrument hears the computer's return. The unit's USB driver, the card's hand-over and the costs are the unit's.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/usb_audio.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fake_capture.h>
#include <o3sdk/platform/host/fake_usb_audio.h>
#include <o3sdk/sim/harness.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace o3sdk;
namespace host = o3sdk::platform::host;
namespace hu = o3sdk::hal::usb_audio;
using app::Button;

namespace {

// An instrument that passes its input through (in_l, in_r to out_l, out_r): what the codec, or the USB return, delivers is audible at the master through a unit.
class Thru final : public dsp::Unit {
 public:
  Thru() : info_{"THRU", dsp::UnitKind::Instrument, nullptr, 0, nullptr, 0} {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override {}
  void process(const dsp::ProcessArgs& a) noexcept override {
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      a.out_l[i] = a.in_l[i];
      a.out_r[i] = a.in_r[i];
    }
  }

 private:
  dsp::UnitInfo info_;
};

class BlankScreen final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PROBE"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

class ProbeApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"USBPROBE", "1.0.0"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override {
    const dsp::UnitId id = ctx.engine().attach(thru_);
    (void)ctx.engine().set_source(0, id);
    ctx.show(screen_);
  }

 private:
  Thru thru_;
  BlankScreen screen_;
};

struct Rig {
  explicit Rig(bool with_usb = true, bool with_card = true, bool with_capture = false, const char* tag = "app-usb", const std::string& dir = std::string(), bool fall_back = false)
      : own_dir(std::make_unique<host::ScratchDir>(tag)),
        card(dir.empty() ? own_dir->path() : dir),
        h(app, nullptr, nullptr, with_card ? &card : nullptr, nullptr, with_capture ? &cap : nullptr, with_usb ? &usb : nullptr) {
    usb.fall_back(fall_back);
    started = h.start();
  }
  std::unique_ptr<host::ScratchDir> own_dir;
  host::DirCard card;
  host::FakeCapture cap;
  host::FakeUsbAudio usb;
  ProbeApp app;
  sim::Harness h;
  bool started = false;
  app::Context& ctx() { return h.runtime().context(); }
  app::UsbAudio& ua() { return ctx().usb_audio(); }
  std::string state() {
    char line[512];
    (void)h.runtime().state_line(line, sizeof line);
    return line;
  }
  bool has(const char* text) { return state().find(text) != std::string::npos; }
  void tap(Button b) {
    h.press(b);
    h.run_ms(1);
    h.release(b);
    h.run_ms(1);
  }
  // The USB rows are the AUDIO section's when the platform has no capture device: USB AUDIO, PROFILE, RETURN, RETURN LEVEL. Opens the Config on it and moves to `row`.
  void config_row(unsigned row) {
    if (!h.runtime().system_open()) {
      tap(Button::Config);
    }
    for (unsigned i = 0; i < 8u; ++i) {
      tap(Button::Up);
    }
    for (unsigned i = 0; i < row; ++i) {
      tap(Button::Down);
    }
  }
};

}  // namespace

TEST_CASE("usb audio app: a platform without the device has none of it - every call is refused and the Config has no USB rows, no CARD AS DISK and no pairs in the state line") {
  Rig r(false, true);
  REQUIRE(r.started);
  CHECK_FALSE(r.ua().available());
  CHECK_FALSE(r.ua().request(true, hu::Profile::P10, true));
  CHECK_FALSE(r.ua().set_stem(0, 1u));
  CHECK(r.ua().share_disk() == hu::DiskRefusal::Unavailable);
  CHECK_FALSE(r.ua().eject_disk());
  CHECK_FALSE(r.ua().streaming());
  CHECK_FALSE(r.has("usb="));
  // No capture and no USB device: no AUDIO section; the card is there, so the CARD section is, with its three rows and no disk.
  r.tap(Button::Config);
  CHECK(r.has("section=card"));
  r.h.run_ms(5);
}

TEST_CASE("usb audio app: the image boots in AUDIO, P8, duplex with the master only, the state line says so, and the Config opens on the USB rows without a capture device") {
  Rig r;
  REQUIRE(r.started);
  CHECK(r.ua().available());
  CHECK(r.ua().audio());
  CHECK(r.ua().profile() == hu::Profile::P8);
  CHECK(r.ua().duplex());
  CHECK(r.ua().return_level() == 0u);  // the return is drained, not heard, until the user raises it
  CHECK(r.usb.device().return_level() == 0.0f);
  for (unsigned c = 0; c < 6u; ++c) {
    CHECK(r.ua().stem(c) == 0u);
  }
  CHECK(r.ua().stem(6) == hu::kSourceMasterL);
  CHECK(r.ua().stem(7) == hu::kSourceMasterR);
  CHECK(r.has("usb=P8 dup=1 ret=0 strm=0 disk=idle"));
  r.tap(Button::Config);
  CHECK(r.has("section=audio"));
}

TEST_CASE("usb audio app: a change of the USB set asks F1 twice, any other key takes it back, and the port re-enumerates once with the factory map of the new profile") {
  Rig r;
  REQUIRE(r.started);
  r.config_row(1);  // PROFILE
  r.tap(Button::F1);
  CHECK(r.has("usb=P8"));
  CHECK(r.usb.reconnects() == 0u);
  CHECK_FALSE(r.ua().changing());
  r.tap(Button::Up);  // another control: the proposal is dropped
  r.tap(Button::Down);
  r.tap(Button::F1);  // so this is a new first F1, not a second one
  r.h.run_ms(100);
  CHECK_FALSE(r.ua().changing());
  r.h.run_ms(6000);  // and an F1 that is not followed is dropped after kAskMs
  r.tap(Button::F1);  // a first one again (P10 is proposed from P8: the next profile)
  CHECK_FALSE(r.ua().changing());
  r.tap(Button::F1);  // the second applies it
  CHECK(r.ua().changing());
  CHECK(r.ua().profile() == hu::Profile::P10);  // (the target, while the port re-enumerates)
  r.h.run_ms(host::FakeUsbAudio::kReenumerateMs + 100u);
  CHECK_FALSE(r.ua().changing());
  CHECK(r.usb.reconnects() == 1u);
  CHECK(r.has("usb=P10"));
  CHECK(r.ua().stem(0) == hu::track_source(1));  // P10's factory map: T1..T8, then the master
  CHECK(r.ua().stem(8) == hu::kSourceMasterL);
  // The encoder proposes too; turning back to the layout in use drops the proposal, so the F1 after it is a first one again.
  r.config_row(1);
  r.h.turn(-1);
  r.h.run_ms(2);
  r.h.turn(1);
  r.h.run_ms(2);
  r.tap(Button::F1);
  CHECK_FALSE(r.ua().changing());
  // USB AUDIO off: STANDARD.
  r.config_row(0);
  r.tap(Button::F1);
  r.tap(Button::F1);
  CHECK(r.ua().changing());
  r.h.run_ms(1200);
  CHECK(r.has("usb=STD"));
  CHECK_FALSE(r.ua().audio());
  // A press while the port re-enumerates is refused, not queued.
  r.config_row(0);
  r.tap(Button::F1);
  r.tap(Button::F1);
  CHECK(r.ua().changing());
  r.config_row(1);
  r.tap(Button::F1);
  r.tap(Button::F1);
  r.h.run_ms(1200);
  CHECK(r.usb.reconnects() == 3u);
}

TEST_CASE("usb audio app: RETURN LEVEL is a plain row (the encoder), kept per cent, and the return stream's pull follows the device") {
  Rig r;
  REQUIRE(r.started);
  r.config_row(3);
  r.h.turn(30);
  r.h.run_ms(2);
  CHECK(r.ua().return_level() == 30u);
  CHECK(r.usb.device().return_level() == doctest::Approx(0.3f));
  r.ua().set_return_level(250);
  CHECK(r.ua().return_level() == 100u);
  r.ua().set_return_level(0);
  CHECK(r.usb.device().return_level() == 0.0f);
  CHECK(r.has("ret=0"));
}

TEST_CASE("usb audio app: the layout, the level and an edited map are kept with the settings and come back at the next boot; a profile change drops the edited map") {
  host::ScratchDir dir("app-usb-keep");
  {
    Rig r(true, true, false, "unused", dir.path());
    REQUIRE(r.started);
    r.h.run_ms(10);
    REQUIRE(r.ua().request(true, hu::Profile::P10, false));
    r.h.run_ms(1200);
    CHECK(r.has("usb=P10 dup=0"));
    REQUIRE(r.ua().set_stem(1, hu::track_source(1) | hu::track_source(2)));
    r.ua().set_return_level(40);
    r.h.run_ms(2000);  // the settings are written 1.5 s after the last change
    CHECK_FALSE(r.ctx().files().settings_dirty());
  }
  {
    Rig r(true, true, false, "unused", dir.path());
    REQUIRE(r.started);
    CHECK(r.ua().profile() == hu::Profile::P10);  // the port starts with it: before any pass
    CHECK_FALSE(r.ua().duplex());
    CHECK(r.usb.device().profile() == hu::Profile::P10);
    r.h.run_ms(20);  // the first passes apply the level and the slots
    CHECK(r.ua().return_level() == 40u);
    CHECK(r.usb.device().return_level() == doctest::Approx(0.4f));
    CHECK(r.ua().stem(1) == (hu::track_source(1) | hu::track_source(2)));
    CHECK(r.ua().stem(0) == hu::track_source(1));  // an edited map keeps the slots as they were
    // Another profile gives that profile's factory slots and the edit is gone.
    REQUIRE(r.ua().request(true, hu::Profile::P11, false));
    r.h.run_ms(1200);
    CHECK(r.ua().stem(1) == hu::track_source(2));
    r.h.run_ms(2000);
  }
  {
    Rig r(true, true, false, "unused", dir.path());
    REQUIRE(r.started);
    r.h.run_ms(20);
    CHECK(r.ua().profile() == hu::Profile::P11);
    CHECK(r.ua().stem(1) == hu::track_source(2));
  }
}

TEST_CASE("usb audio app: CARD AS DISK gives the card to the computer through two presses, a save is refused until the eject, the settings stay dirty and are written after it") {
  Rig r;
  REQUIRE(r.started);
  r.h.run_ms(20);
  CHECK(r.has("card=ready"));
  r.tap(Button::Config);
  r.tap(Button::Right);  // AUDIO -> CARD
  CHECK(r.has("section=card"));
  r.tap(Button::Down);
  r.tap(Button::Down);
  r.tap(Button::Down);  // CARD AS DISK
  r.tap(Button::F1);
  CHECK(r.has("disk=idle"));
  r.tap(Button::F1);
  CHECK(r.has("disk=working"));
  r.h.run_ms(host::FakeUsbAudio::kShareMs + 100u);
  CHECK(r.has("disk=out"));
  CHECK(r.has("card=computer"));
  CHECK(r.ctx().files().card() == app::Files::Card::OnComputer);
  // Every save is refused with the words of the strip; a theme change is the kit's own write, which waits.
  const char kData[] = "x";
  CHECK_FALSE(r.ctx().files().save("", "a.o3a", kData, 1));
  CHECK(std::string(r.ctx().files().status().text) == "CARD ON COMPUTER - EJECT FIRST");
  r.ctx().set_theme(ui::ThemeId::White);
  r.h.run_ms(3000);
  CHECK(r.ctx().files().settings_dirty());
  // READ ONLY is the policy of the share: fixed while the card is out.
  r.tap(Button::Down);  // READ ONLY
  r.tap(Button::F1);
  CHECK(r.ua().read_only());
  // The eject: F1 twice on CARD AS DISK again.
  r.tap(Button::Up);
  r.tap(Button::F1);
  r.tap(Button::F1);
  r.h.run_ms(host::FakeUsbAudio::kEjectMs + 100u);
  CHECK(r.has("card=ready"));
  CHECK(r.has("disk=idle"));
  r.h.run_ms(2500);
  CHECK(r.ctx().theme() == ui::ThemeId::White);  // what was changed while the card was out is kept, not read over from the card's older file
  CHECK_FALSE(r.ctx().files().settings_dirty());  // written after the eject
  CHECK(r.ctx().files().save("", "a.o3a", kData, 1));
  r.h.run_ms(100);
  CHECK(std::string(r.ctx().files().status().text) == "SAVED");
  // READ ONLY switches while the disk is not out.
  r.tap(Button::Down);
  r.tap(Button::F1);
  CHECK_FALSE(r.ua().read_only());
  CHECK(r.card.violations() == 0u);
}

TEST_CASE("usb audio app: a share asked within the debounce of a settings edit waits for the write, and the card is handed over with the file written (no rollback)") {
  Rig r;
  REQUIRE(r.started);
  r.h.run_ms(20);
  r.ctx().set_theme(ui::ThemeId::White);
  r.h.run_ms(5);
  CHECK(r.ctx().files().settings_dirty());
  CHECK(r.ua().share_disk() == hu::DiskRefusal::None);  // taken: it waits
  CHECK(r.ua().disk().phase == hu::DiskPhase::Working);
  CHECK(r.usb.device().disk().phase == hu::DiskPhase::Idle);  // the port is not touched yet
  CHECK(r.ua().share_disk() == hu::DiskRefusal::NotIdle);
  for (unsigned i = 0; i < 300u && r.usb.device().disk().phase == hu::DiskPhase::Idle; ++i) {
    r.h.run_ms(1);
  }
  CHECK(r.usb.device().disk().phase == hu::DiskPhase::Working);  // the hand-over began...
  CHECK_FALSE(r.ctx().files().settings_dirty());                 // ...with the settings on the card
  CHECK(r.ctx().files().app_busy() == false);
  r.h.run_ms(host::FakeUsbAudio::kShareMs + 100u);
  CHECK(r.has("disk=out"));
  CHECK(r.ua().take_share_refusal() == hu::DiskRefusal::None);
  // An app job that is running is still Busy, and a share that waited for the settings is dropped when one starts meanwhile.
  CHECK(r.ua().eject_disk());
  r.h.run_ms(host::FakeUsbAudio::kEjectMs + 100u);
  r.ctx().set_theme(ui::ThemeId::Black);
  r.h.run_ms(5);
  REQUIRE(r.ua().share_disk() == hu::DiskRefusal::None);
  std::vector<std::uint8_t> big(200000, 7);
  REQUIRE(r.ctx().files().save("", "big.o3a", big.data(), big.size()));
  r.h.run_ms(5);
  CHECK(r.ua().disk().phase == hu::DiskPhase::Idle);  // the wait is over (the runtime put the refusal on the strip)
  CHECK(r.usb.device().disk().phase == hu::DiskPhase::Idle);
}

TEST_CASE("usb audio app: slots saved for P10 are not applied to the boot profile a refused start fell back to") {
  host::ScratchDir dir("app-usb-fallback");
  {
    Rig r(true, true, false, "unused", dir.path());
    REQUIRE(r.started);
    REQUIRE(r.ua().request(true, hu::Profile::P10, true));
    r.h.run_ms(1200);
    REQUIRE(r.ua().set_stem(1, hu::track_source(1) | hu::track_source(2)));
    r.h.run_ms(2000);
  }
  Rig r(true, true, false, "unused", dir.path(), /*fall_back=*/true);
  REQUIRE(r.started);
  r.h.run_ms(20);
  CHECK(r.ua().profile() == hu::Profile::P8);  // the port came up in the boot profile
  for (unsigned c = 0; c < 6u; ++c) {
    CHECK(r.ua().stem(c) == 0u);  // P10's slot 2 is not P8's channel 2
  }
  CHECK(r.ua().stem(6) == hu::kSourceMasterL);
}

TEST_CASE("usb audio app: a card in use is refused (a save running), so is no card, a disk already out and a hand-over under way") {
  Rig r;
  REQUIRE(r.started);
  r.h.run_ms(20);
  std::vector<std::uint8_t> big(200000, 7);
  REQUIRE(r.ctx().files().save("", "big.o3a", big.data(), big.size()));
  CHECK(r.ctx().files().busy());
  CHECK(r.ua().share_disk() == hu::DiskRefusal::Busy);
  // The Config says so at the first F1 and asks nothing.
  r.tap(Button::Config);
  r.tap(Button::Right);
  r.tap(Button::Down);
  r.tap(Button::Down);
  r.tap(Button::Down);
  r.tap(Button::F1);
  CHECK(r.has("disk=idle"));
  r.tap(Button::F1);  // (no second press of anything: the first was refused)
  CHECK(r.has("disk=idle"));
  r.tap(Button::Config);
  for (unsigned i = 0; i < 400u && r.ctx().files().busy(); ++i) {
    r.h.run_ms(10);
  }
  CHECK_FALSE(r.ctx().files().busy());
  CHECK(r.ua().share_disk() == hu::DiskRefusal::None);
  CHECK(r.ua().share_disk() == hu::DiskRefusal::NotIdle);  // a hand-over is under way
  r.h.run_ms(host::FakeUsbAudio::kShareMs + 100u);
  CHECK(r.ua().share_disk() == hu::DiskRefusal::NotIdle);  // and the disk is out
  CHECK_FALSE(r.ua().set_read_only(false));
  CHECK(r.ua().eject_disk());
  r.h.run_ms(host::FakeUsbAudio::kEjectMs + 100u);
  CHECK_FALSE(r.ua().eject_disk());  // not out any more
  r.h.card_out();
  CHECK(r.ua().share_disk() == hu::DiskRefusal::NoCard);
}

TEST_CASE("usb audio app: the layout cannot change while the card is out, and a request without a driver is refused") {
  Rig r;
  REQUIRE(r.started);
  r.h.run_ms(20);
  REQUIRE(r.ua().share_disk() == hu::DiskRefusal::None);
  CHECK_FALSE(r.ua().request(true, hu::Profile::P10, true));  // the hand-over has the layout
  r.h.run_ms(host::FakeUsbAudio::kShareMs + 100u);
  CHECK_FALSE(r.ua().request(true, hu::Profile::P10, true));
  CHECK(r.ua().eject_disk());
  r.h.run_ms(host::FakeUsbAudio::kEjectMs + 100u);
  CHECK(r.ua().request(true, hu::Profile::P10, true));
}

TEST_CASE("usb audio app: with the audio in's source Usb an instrument hears the computer's return, and with the line it does not") {
  Rig r(true, false, true);
  REQUIRE(r.started);
  r.h.record(true);
  r.ua().set_return_level(0);  // only the engine hears it
  r.usb.listen(true);
  r.usb.play_tone(1000.0f, -12.0f);
  r.h.run_ms(200);
  const auto peak = [&](std::size_t from_frame) {
    std::int32_t m = 0;
    const std::vector<std::int16_t>& a = r.h.audio();
    for (std::size_t i = 2u * from_frame; i < a.size(); ++i) {
      m = std::max<std::int32_t>(m, std::abs(static_cast<std::int32_t>(a[i])));
    }
    return m;
  };
  CHECK(peak(0) == 0);  // the line is the source: the codec hears nothing, the return is not mixed (level 0)
  const std::size_t mark = r.h.audio().size() / 2u;
  REQUIRE(r.ctx().audio_in().select(hal::capture::Source::Usb, 0));
  r.h.run_ms(600);  // the gate settles for 200 ms, then the return is the input
  const std::int32_t heard = peak(mark);
  CHECK(heard > 2500);  // 1 kHz at -12 dBFS is about 8200
  CHECK(heard < 9000);
  CHECK(r.has("in=usb"));
  const std::size_t back = r.h.audio().size() / 2u;
  REQUIRE(r.ctx().audio_in().select(hal::capture::Source::LineMix, 0));
  r.h.run_ms(600);
  // After the settle the input is the codec's again: nothing.
  std::int32_t tail = 0;
  for (std::size_t i = 2u * (back + 22050u / 2u); i < r.h.audio().size(); ++i) {
    tail = std::max<std::int32_t>(tail, std::abs(static_cast<std::int32_t>(r.h.audio()[i])));
  }
  CHECK(tail == 0);
}
