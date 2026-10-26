// SPDX-License-Identifier: GPL-3.0-or-later
//
// @APP_NAME@: the app. An app is a whole firmware image (o3-sdk docs/FRAMEWORK.md, "Apps"): it owns its units and screens as plain
// objects (no heap), wires them into the engine in setup() and shows the first screen. The one entry point at the end is all the
// platform's main and the simulator know of it.
//
//   pads          a chromatic keyboard (app/include/@APP_ID@/pad_map.h); LEFT / RIGHT move it an octave
//   F1..F3        point the encoder at a value (tap: latched; hold + turn: for a moment); the encoder changes it (SHIFT: coarse)
//
// To grow it: more screens (a class like PlayScreen, ctx.show() to switch), a sequencer (a dsp::Clocked hook added to the engine;
// apps/hello/src/hello_app.cpp in o3-sdk has a riff), more units (attach, route to a slot, a send or an insert), console commands
// (App::diag_commands). The Config button is the kit's (the system Config screen is planned): this app does not use it.

#include "play_screen.h"

#include <@APP_ID@/units/voice/voice.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/dsp/engine.h>

namespace {

using namespace o3sdk;

class MyApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"@APP_NAME@", "0.1.0"};  // upper case: it is shown in the header strip and on the boot screen
    return kInfo;
  }

  // Main loop, once, with the engine in its setup phase: attach and route the units, add hooks, open the first screen.
  void setup(app::Context& ctx) noexcept override {
    dsp::Engine& e = ctx.engine();
    const dsp::UnitId voice = e.attach(voice_);
    // Slot 0 plays the voice; the mixer's level, pan and sends are the slot's. A refused call is counted (Engine::setup_errors) and the
    // simulator and app_test fail on it, so the result need not be checked here.
    (void)e.set_source(0, voice);
    play_.init(voice);
    ctx.show(play_);
  }

  // The console's `state` after the kit's pairs (`o3console.py state` over USB; the same line is in the simulator's tests): the keyboard
  // octave, the key the encoder drives and its value as the knob strip shows it.
  std::size_t describe_state(const app::Context& ctx, char* out, std::size_t cap) const noexcept override {
    app::StateWriter w(out, cap);
    const app::ParamKeys& keys = play_.keys();
    const app::ParamPage* page = keys.page();
    const unsigned focus = keys.focus();
    char value[16] = {};
    if (page != nullptr && focus < page->count) {
      (void)ctx.engine().format_param(page->unit, page->ids[focus], value, sizeof value);
    }
    w.sint("octave", play_.octave());
    w.num("focus", focus);
    w.str("value", value);
    return w.size();
  }

 private:
  @APP_ID@::Voice voice_;
  @APP_ID@::PlayScreen play_;
};

}  // namespace

o3sdk::app::App& o3sdk_app() noexcept {
  static MyApp app;
  return app;
}
