// SPDX-License-Identifier: GPL-3.0-or-later
//
// Slice B6: the encoder's acceleration (enc_accel.h, Context::enc_scale and the ENCODER row), a screen's button auto-repeat (Screen::repeat_buttons, Input::repeat), and the console's
// limits for an app's rows (diag.h). The settings key is in app_settings_test.cpp, the learn hook in app_midi_learn_test.cpp.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/diag.h>
#include <o3sdk/app/enc_accel.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/sim/harness.h>

#include <cstdint>

using namespace o3sdk;
using app::Button;
using app::EncAccel;

namespace {

constexpr core::Tick ms(std::uint32_t n) { return core::ticks_from_ms(n); }

class Counting final : public app::Screen {
 public:
  explicit Counting(std::uint64_t mask) : mask_(mask) {}
  const char* title() const noexcept override { return "REPEAT"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
  std::uint64_t repeat_buttons(const app::Context&) const noexcept override { return mask_; }
  void input(app::Context& ctx, const app::Input& in) noexcept override {
    if (in.press(Button::Up)) {
      (in.repeat ? repeats : presses) += 1;
    }
    if (in.press(Button::F1) && other != nullptr) {
      ctx.show(*other);
    }
  }
  std::uint64_t mask_;
  int presses = 0;
  int repeats = 0;
  app::Screen* other = nullptr;
};

class ProbeApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"B6", "1"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(asking); }
  Counting asking{std::uint64_t{1} << static_cast<unsigned>(Button::Up)};
  Counting quiet{0};
};

}  // namespace

TEST_CASE("enc accel: a run speeds up from t1 and t2, a pause, a reversal and OFF start again, a lone report is x1") {
  EncAccel a;
  core::Tick t = 1000000;
  CHECK(a.scale(t, 5) == 5);  // one report of five detents: x1 (the multiplier is the run BEFORE it)
  a.reset();
  int sum = 0;
  int last = 0;
  for (int i = 0; i < 20; ++i) {  // a spin: a detent every 20 ms
    t += ms(20);
    last = a.scale(t, 1);
    sum += last;
    if (i < 6) {
      CHECK(last == 1);
    } else if (i < 16) {
      CHECK(last == EncAccel::kMult1);
    } else {
      CHECK(last == EncAccel::kMult2);
    }
  }
  CHECK(sum == 6 + 10 * EncAccel::kMult1 + 4 * EncAccel::kMult2);
  t += ms(200);  // a pause
  CHECK(a.scale(t, 1) == 1);
  for (int i = 0; i < 10; ++i) {
    t += ms(20);
    (void)a.scale(t, 1);
  }
  t += ms(20);
  CHECK(a.scale(t, -1) == -1);  // a reversal
  a.cfg.on = false;
  for (int i = 0; i < 30; ++i) {
    t += ms(20);
    CHECK(a.scale(t, 1) == 1);
  }
  CHECK(a.scale(t, 0) == 0);
  CHECK(EncAccel::valid(EncAccel::Config{}));
  CHECK_FALSE(EncAccel::valid(EncAccel::Config{true, 9, 3, 60}));
}

TEST_CASE("enc accel: the context's modes are OFF, NORMAL (the defaults) and FAST (t1 3, t2 8), NORMAL at first") {
  ProbeApp app;
  sim::Harness h(app);
  REQUIRE(h.start());
  app::Context& ctx = h.runtime().context();
  CHECK(ctx.enc_mode() == 1u);
  CHECK(ctx.enc_accel().cfg.on);
  CHECK(ctx.enc_accel().cfg.t1 == EncAccel::kDefaultT1);
  ctx.set_enc_mode(2);
  CHECK(ctx.enc_accel().cfg.t1 == 3u);
  CHECK(ctx.enc_accel().cfg.t2 == 8u);
  CHECK(ctx.enc_accel().cfg.gap_ms == EncAccel::kDefaultGapMs);
  ctx.set_enc_mode(0);
  CHECK_FALSE(ctx.enc_accel().cfg.on);
  ctx.set_enc_mode(9);  // out of range: the last mode
  CHECK(ctx.enc_mode() == app::Context::kEncModes - 1u);
}

TEST_CASE("repeat: a screen that asks gets repeats of its button, a screen that does not gets none, leaving stops them") {
  ProbeApp app;
  sim::Harness h(app);
  REQUIRE(h.start());
  h.run_ms(20);
  h.press(Button::Up);
  h.run_ms(1000);
  CHECK(app.asking.presses == 1);
  CHECK(app.asking.repeats == 3);  // 400, 700 and 900 ms
  h.release(Button::Up);
  h.run_ms(1000);
  CHECK(app.asking.repeats == 3);  // the release ends them
  // Leaving the screen with the button held: the next screen does not ask, nothing reaches it, and the old one hears no more.
  app.asking.other = &app.quiet;
  app.asking.presses = app.asking.repeats = 0;
  h.press(Button::Up);
  h.run_ms(450);
  CHECK(app.asking.repeats == 1);
  h.press(Button::F1);
  h.run_ms(5);
  REQUIRE(h.runtime().context().screen() == &app.quiet);
  h.run_ms(2000);
  CHECK(app.asking.repeats == 1);
  CHECK(app.quiet.repeats == 0);
  h.release(Button::Up);
  // A button the screen did not ask for, and a screen that asks for none: a held button is one press, as always.
  h.press(Button::Up);
  h.run_ms(1500);
  CHECK(app.quiet.presses == 1);
  CHECK(app.quiet.repeats == 0);
  h.release(Button::Up);
}

TEST_CASE("console limits: an app's rows meet the constants of diag.h, which the console checks with") {
  static_assert(app::kDiagNameMin == 2 && app::kDiagNameMax == 7 && app::kDiagHelpMax == 70 && app::kDiagMaxCommands == 16, "the limits the console enforces");
  static constexpr char kName[] = "gate";
  static constexpr char kHelp[] = "gate <n>: opens the gate";
  static_assert(sizeof kName - 1 >= app::kDiagNameMin && sizeof kName - 1 <= app::kDiagNameMax, "name length");
  static_assert(sizeof kHelp - 1 <= app::kDiagHelpMax, "help length");
  CHECK(app::kDiagNameMin < app::kDiagNameMax);
}

TEST_CASE("repeat: the mask changing while a button is held does not strand it, the release still stops the repeats") {
  ProbeApp app;
  sim::Harness h(app);
  REQUIRE(h.start());
  h.run_ms(20);
  h.press(Button::Up);
  h.run_ms(450);
  CHECK(app.asking.repeats == 1);
  app.asking.mask_ = 0;  // the screen stops asking while the button is down
  h.release(Button::Up);
  const int seen = app.asking.repeats;
  h.run_ms(3000);
  CHECK(app.asking.repeats == seen);  // no repeats after the release
}
