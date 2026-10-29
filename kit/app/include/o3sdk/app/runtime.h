// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The runtime: the main loop every platform runs for an app (docs/FRAMEWORK.md, "Runtime").
//
//   platform main / simulator:
//     Runtime rt(o3sdk_app(), engine, platform);
//     rt.start();                                  // assets, App::setup, engine start
//     audio_device.start(rt.block_callback());     // the render context calls the engine once per block
//     for (;;) rt.poll();                          // input -> app -> screen, a frame every kFrameUs, LEDs
//
// poll() never blocks. A frame is a whole display list: the header strip (app name, screen title, tempo, transport, CPU), the
// screen's content and the soft-key row. The pads' LEDs are one frame per UI frame, sent a piece at a time (hal/leds.h).
//
// MIDI (when Platform::midi is set). The render side of every block is: Device::begin_block, the routed notes injected into the engine
// (Engine::inject), the clock frame (Engine::follow), Engine::render, Device::end_block with the transport and the hooks' messages. The main
// loop forwards the app's Engine::play() / stop() while following (Device::request) and hands every message no route took to App::midi().
//
// The runtime owns the Config button: it opens the system Config screen (system_config.h) over the app's, and the app never sees the
// button. While that screen is open, or the input is locked, the app gets no new press and no turn; the release of a button or pad
// pressed before still reaches it, and a press the screen took is not released to the app either.

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/pad_leds.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/app/system_config.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/capture.h>
#include <o3sdk/hal/clock.h>
#include <o3sdk/hal/display.h>
#include <o3sdk/hal/firmware.h>
#include <o3sdk/hal/input.h>
#include <o3sdk/hal/leds.h>
#include <o3sdk/hal/midi.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

static_assert(dsp::kMaxBlockFrames == hal::audio::kBlock, "the engine renders the HAL's block");

struct Platform {
  hal::Display& display;
  hal::Input& input;
  hal::Leds* leds;  // null: no pad LEDs (a test)
  const hal::Clock& clock;
  bool cpu_meter;   // the engine has a real cycle counter (the unit): the header shows the render's load
  // The firmware update path for the system Config screen (docs/M2B2-PLAN.md section 4): the unit's is update::device::sink(), the
  // simulator's a fake folder. Null (or one that is not available()) hides the FIRMWARE section.
  hal::firmware::FirmwareSink* firmware = nullptr;
  const char* build = nullptr;  // the build id INFO shows ("o3sdk 0123456789ab-dirty"); null: none
  // MIDI in and out (hal/midi.h, docs/M2B2-PLAN.md section 3): notes into the engine through the app's routes, the clock followed or sent,
  // the rest to App::midi(), and the MIDI section of the system Config. Null (a test rig, an image without MIDI): none of it, and the runtime
  // behaves as it did before MIDI existed.
  hal::midi::Device* midi = nullptr;
  // The SD card as the app's files (files.h, docs/M2C-PLAN.md section 3): `storage` reads it, `writer` writes it (hal/storage_write.h: only the files service, the writers and the
  // platform's main name that header), `files_work` is the jobs' memory (files_work.h; the unit's image places it in the SDRAM), `app_id` the folder name of Apps/<id>/ (null: o3sdk_app_id(),
  // which the app's build generates), `card_claim` says that something else holds the card (the firmware update path) while it returns true. Any of the first three null: no files, every
  // call of Context::files() is refused with "NO CARD", and the kit's settings are not kept (a test rig, an image without a card).
  hal::storage::Storage* storage = nullptr;
  hal::storage::StorageWriter* writer = nullptr;
  Files::Work* files_work = nullptr;
  const char* app_id = nullptr;
  bool (*card_claim)() noexcept = nullptr;
  // Audio in and the FM radio (audio_in.h, radio.h, docs/M2C-PLAN.md section 5): the codec's source and gain and the receiver, for Context::audio_in() and radio() and the AUDIO section of the
  // system Config. The platform's main loop calls capture->service(uptime_ms) once a pass (the radio's state machine; it does at most one bus transfer). Null (a test rig, an image without
  // it): no AUDIO section, every call of audio_in() and radio() is refused, and the runtime and the render are as they were.
  hal::capture::CaptureHw* capture = nullptr;
};

// Layout of the chrome (the theme's metrics: a 32 px strip, a 22 px key row).
inline constexpr int kHeaderH = 32;
inline constexpr int kKeysH = 22;
inline constexpr ui::Rect kContentArea{0, kHeaderH, ui::kScreenW, ui::kScreenH - kHeaderH - kKeysH};

class Runtime {
 public:
  static constexpr core::Tick kFrameUs = 33'333;  // 30 frames a second

  Runtime(App& app, dsp::Engine& engine, Platform& platform) noexcept;

  // Main loop, once: uploads the UI's bitmaps, runs App::setup and starts the engine. false: the display refused the assets (the app
  // still runs; the screen may show nothing).
  bool start() noexcept;
  // One pass of the main loop.
  void poll() noexcept;
  // Draws a frame now (the simulator's snapshots). false: the display refused it or the list did not fit.
  bool draw() noexcept;

  // The audio device's callback (hal/audio.h): one engine block per call, int16 in and out.
  hal::audio::BlockCallback block_callback() noexcept { return hal::audio::BlockCallback{&Runtime::render_block, this}; }

  Context& context() noexcept { return ctx_; }
  const PadLeds& leds() const noexcept { return leds_; }
  std::uint32_t frames() const noexcept { return frames_; }
  // Frames the builder or the display refused at the end (the list did not fit, the commit failed): a fault.
  std::uint32_t frame_failures() const noexcept { return frame_failures_; }
  // Frames the display was too busy to begin (a swap still pending): normal on the unit, tried again on the next pass.
  std::uint32_t frames_busy() const noexcept { return frames_busy_; }
  std::uint32_t last_frame_words() const noexcept { return last_words_; }

  // The unit's state as one line, for the console's `state` (and a test): the kit's pairs, then the app's (App::describe_state).
  //   app=<name> screen=<title> playing=0|1 tempo=<mbpm> cpu=<pct> frames=<n> [<the app's pairs>]
  // `playing` is the last Play/Stop the main loop asked for, `tempo` is in milli-BPM (120000 = 120.0), `cpu` the render's load of the
  // last block in percent (0 without a cycle counter), `frames` the frames drawn. Spaces in a name become '_'. NUL terminated, cut
  // at a pair when `cap` is short; returns the length. Main loop only.
  //   ... frames=<n> sys=0|1 section=<firmware|audio|midi|display|info> fw=<closed|list|ask_update|checking|ask_install|installing>
  //       card=<none|ready|readonly|unsupported|computer|inuse> files=<idle|running|done|failed> [<the app's pairs>]
  // `sys` is the system Config screen being open, `section` the one it shows (or was left on), `fw` the firmware switcher's phase, `card` the
  // files service's view of the card (files.h Card; `none` also without a card capability) and `files` the phase of its last job (Files::Status).
  // `tempo` and `playing` are the master's while the engine follows a MIDI clock (Engine::tempo).
  //   ... files=<...> [in=<line|lineleft|lineright|miclow|michigh|radio|usb> gain=<0..100> mon=<0..100> gate=0|1 radio=<off|starting|ready|tuning|seeking|absent|failed> freq=<10 kHz units>]
  //       [<the app's pairs>]
  // The audio pairs (the choice of Context::audio_in() and radio(); `gate` is 0 while the source settles) are there only when the platform has a capture device.
  std::size_t state_line(char* out, std::size_t cap) const noexcept;

  // ---- The system layer (system_config.h) -----------------------------------------------------------------------------------------
  // The system Config screen is open (and the app's input is suspended).
  bool system_open() const noexcept { return sys_.open(); }
  const SystemConfig& system() const noexcept { return sys_; }  // the screen's state, for a test or a diagnostic
  // One line over the bottom of whatever is on screen, with a bar at `progress` (0..1000; else none): the platform's messages (the
  // power-off hold). Keep calling it to keep it up: it goes `hold_ms` after the last call. A null `text` takes it down.
  void system_strip(const char* text, int progress = -1, std::uint32_t hold_ms = SystemConfig::kStripMs) noexcept;
  // While locked the input is consumed (the panel still follows it) and reaches neither the app nor the Config screen. The
  // platform locks it for the power-off and the like.
  void lock_input(bool on) noexcept { locked_ = on; }
  bool input_locked() const noexcept { return locked_; }
  // The card was taken out or put in (the platform's card watch): a firmware list that is not past INSTALL is closed and cancelled.
  void card_changed() noexcept {
    sys_.card_changed(ctx_);
    ctx_.files_.card_changed();
  }
  // At boot, once the card is mounted (after start(): App::setup has set the app's defaults): reads the kit's settings from Apps/<app>/system.o3s and applies them over those defaults, the
  // theme and the MIDI settings (docs/formats/o3sdk-settings.md). One bounded read. A missing, damaged or newer-major file leaves the defaults. From then on a change is written 1.5 s after the
  // last one (and at once when the power-off begins); nothing is written for what the app set in setup(). Without a card capability: Unavailable, and nothing is kept.
  Files::SettingsLoad load_settings() noexcept;

 private:
  static void render_block(void* user, const hal::audio::BlockIo& io) noexcept;
  void render_midi_in(std::uint32_t frames) noexcept;
  void poll_midi() noexcept;
  void switch_screen() noexcept;
  void route(const Input& in) noexcept;
  void draw_header(ui::Renderer& r) noexcept;
  void draw_keys(ui::Renderer& r) noexcept;
  void update_leds() noexcept;
  KitValues kit_values() const noexcept;
  void apply_kit_audio(const KitValues& v) noexcept;

  App& app_;
  dsp::Engine& engine_;
  Platform& platform_;
  Context ctx_;
  SystemConfig sys_;
  bool locked_ = false;
  // What the system layer took: a press it swallowed is released to nobody (bit n = button n, pad n).
  std::uint64_t taken_buttons_ = 0;
  std::uint64_t taken_pads_ = 0;
  PadLeds leds_;
  core::Tick next_frame_ = 0;
  std::uint32_t frames_ = 0;
  std::uint32_t frame_failures_ = 0;
  std::uint32_t frames_busy_ = 0;
  bool busy_ = false;  // the last draw() found the display busy
  std::uint32_t last_words_ = 0;
  bool started_ = false;
  eve::DisplayListBuffer list_;
};

}  // namespace o3sdk::app
