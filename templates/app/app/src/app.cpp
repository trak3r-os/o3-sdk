// SPDX-License-Identifier: GPL-3.0-or-later
//
// @APP_NAME@: the app. An app is a whole firmware image (o3-sdk docs/FRAMEWORK.md, "Apps"): it owns its units and screens as plain
// objects (no heap), wires them into the engine in setup() and shows the first screen. The one entry point at the end is all the
// platform's main and the simulator know of it.
//
//   pads          a chromatic keyboard (app/include/@APP_ID@/pad_map.h); LEFT / RIGHT move it an octave
//   F1..F3        point the encoder at a value (tap: latched; hold + turn: for a moment); the encoder changes it (SHIFT: coarse)
//   pads, DELETE  the pads also record into a 16-step pattern; DELETE clears it
//   FILE          saves the pattern to the SD card, Apps/@APP_ID@/pattern.o3a;  SAMPLE LOAD  loads it back
//
// The card (Context::files(), o3-sdk kit/app/include/o3sdk/app/files.h: read its header comment, it is short). Every call is a job the
// runtime advances in the main loop, so the sound and the screen never wait for it: a call only records the request and returns, and update()
// polls status() for Done or Failed. Rules this file follows: one job at a time (a call made while busy() is refused, so check it first),
// the buffer a save reads stays untouched until the job ends (a buffer of its own for each file), a load goes into a buffer you give and is
// checked before it is applied (pattern.h), a failed call says why in status().text and on the kit's strip (SAVED, NO CARD, FILE TOO BIG,
// ...), and a power cut never leaves half a file (the kit writes a temporary one and renames it; the old one stays as pattern.o3a.bak).
// What the app keeps: the pattern (on the keys above) and the keyboard octave, saved by itself a moment after it changes and loaded at
// start as soon as the card is ready. Two files, pattern.o3a and config.o3a, in the app's folder on the card (the CMake id names it).
//
// To grow it: more screens (a class like PlayScreen, ctx.show() to switch), a sequencer (a dsp::Clocked hook added to the engine;
// apps/hello/src/hello_app.cpp in o3-sdk has a riff), more units (attach, route to a slot, a send or an insert), console commands
// (App::diag_commands). The Config button is the kit's system Config screen (the theme, MIDI, audio in): this app does not use it.
//
// What else an app can reach through its Context (docs/FRAMEWORK.md has the pages): midi() (notes in, clock, the user's MIDI settings),
// audio_in() (line and microphone input, the monitor), radio() (the FM radio) and, once slice RC of M2c lands, the recorder (audio to the
// card). The template uses none of them; each is a short call like files() and a line in the app's state.

#include "play_screen.h"

#include <@APP_ID@/pattern.h>
#include <@APP_ID@/units/voice/voice.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/dsp/engine.h>

#include <cstddef>
#include <cstdint>

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
    // setup() can run again on this object (a test starts a fresh simulator on it, the simulator's `reboot`): the file state starts over too.
    op_ = Op::None;
    boot_ = Boot::Pattern;
    saved_octave_ = play_.octave();
    ctx.show(play_);
  }

  // The file jobs end and start here, once per pass of the main loop (a Screen has an update() too, for what only it needs).
  void update(app::Context& ctx) noexcept override {
    app::Files& files = ctx.files();
    if (op_ != Op::None) {
      const app::Files::Status st = files.status();
      if (st.phase != app::Files::Status::Done && st.phase != app::Files::Status::Failed) {
        return;  // still running
      }
      settle(ctx, st.phase == app::Files::Status::Done);
      return;
    }
    if (files.busy() || !files.available() || files.card() != app::Files::Card::Ready) {
      return;
    }
    // At start, as soon as the card is ready (the simulator and the unit mount it after setup()): the pattern, then the octave. A first run
    // has neither file: that is a failed load, which the screen shows as NOT FOUND and nothing more. The jobs are quiet (no strip line):
    // the user did not ask for them.
    if (boot_ == Boot::Pattern) {
      files.set_quiet(true);
      op_ = files.load("", @APP_ID@::kPatternFile, load_buf_, sizeof load_buf_) ? Op::LoadPattern : Op::None;
      boot_ = Boot::Config;
      return;
    }
    if (boot_ == Boot::Config) {
      files.set_quiet(true);
      op_ = files.load("", @APP_ID@::kConfigFile, load_buf_, sizeof load_buf_) ? Op::LoadConfig : Op::None;
      boot_ = Boot::Done;
      return;
    }
    // The setting saves itself when it changes (and the card is there): the file's bytes are made now and stay in cfg_buf_ until the job ends.
    if (play_.octave() != saved_octave_) {
      saved_octave_ = play_.octave();  // (even when refused: it is tried again at the next change, not every pass)
      const std::size_t n = @APP_ID@::write_config(saved_octave_, cfg_buf_, sizeof cfg_buf_);
      files.set_quiet(true);
      op_ = n != 0u && files.save("", @APP_ID@::kConfigFile, cfg_buf_, n) ? Op::SaveConfig : Op::None;
    }
  }

  // The keys the screen does not use: FILE saves the pattern, SAMPLE LOAD loads it. A key pressed while a job runs is ignored (busy()).
  bool input(app::Context& ctx, const app::Input& in) noexcept override {
    const bool save = in.press(app::Button::File);
    if (!save && !in.press(app::Button::SampleLoad)) {
      return false;
    }
    app::Files& files = ctx.files();
    if (op_ == Op::None && !files.busy()) {
      files.set_quiet(false);  // the user asked: the strip says SAVED, NO CARD, ...
      play_.set_note(nullptr);
      if (save) {
        const std::size_t n = @APP_ID@::write_pattern(play_.pattern(), save_buf_, sizeof save_buf_);
        op_ = n != 0u && files.save("", @APP_ID@::kPatternFile, save_buf_, n) ? Op::SavePattern : Op::None;
      } else {
        op_ = files.load("", @APP_ID@::kPatternFile, load_buf_, sizeof load_buf_) ? Op::LoadPattern : Op::None;
      }
      ctx.redraw();
    }
    return true;
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
    w.num("steps", play_.pattern().used());
    w.num("cursor", play_.pattern().cursor);
    w.num("focus", focus);
    w.str("value", value);
    return w.size();
  }

 private:
  enum class Op : std::uint8_t { None, LoadPattern, LoadConfig, SavePattern, SaveConfig };  // the job in flight
  enum class Boot : std::uint8_t { Pattern, Config, Done };                                 // the loads at start, in order

  // A job ended: a load that is Done is checked and applied. A load that is not a pattern changes nothing and says so (BAD FILE); one that
  // failed (no file, no card) has already said why in status().
  void settle(app::Context& ctx, bool done) noexcept {
    const std::size_t got = ctx.files().loaded();
    if (done && op_ == Op::LoadPattern) {
      @APP_ID@::Pattern p;
      if (@APP_ID@::read_pattern(load_buf_, got, p)) {
        play_.set_pattern(p);
      } else {
        play_.set_note("BAD FILE");
      }
    } else if (done && op_ == Op::LoadConfig) {
      int octave = 0;
      if (@APP_ID@::read_config(load_buf_, got, octave)) {
        play_.set_octave(octave);
        saved_octave_ = play_.octave();  // what the card holds: nothing to save
      }
    }
    op_ = Op::None;
    ctx.redraw();
  }

  @APP_ID@::Voice voice_;
  @APP_ID@::PlayScreen play_;

  Op op_ = Op::None;
  Boot boot_ = Boot::Pattern;
  int saved_octave_ = 0;  // the octave the card holds (or was last asked to)
  // The buffers of the file jobs: word aligned, and a save's stays unchanged until its job ends, so each file has its own; one load at a
  // time shares one (the files service runs one job at a time).
  alignas(4) std::uint8_t save_buf_[@APP_ID@::kPatternFileMax] = {};
  alignas(4) std::uint8_t cfg_buf_[@APP_ID@::kConfigFileMax] = {};
  alignas(4) std::uint8_t load_buf_[@APP_ID@::kPatternFileMax] = {};
  static_assert(sizeof save_buf_ % 4 == 0 && sizeof cfg_buf_ % 4 == 0 && sizeof load_buf_ % 4 == 0, "word-aligned buffers are whole words");
};

}  // namespace

o3sdk::app::App& o3sdk_app() noexcept {
  static MyApp app;
  return app;
}
