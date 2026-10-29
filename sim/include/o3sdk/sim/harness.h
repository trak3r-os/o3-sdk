// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The simulator's core: one app, run on the host with the HAL fakes (sim/README.md).
//
//   * Time moves only through run_ms(): the main loop is polled once a millisecond and the audio is rendered in lockstep, one block
//     each time a block's worth of samples is due at the engine's rate, so a run is deterministic.
//   * The display is the FT81x interpreter (EveRaster) behind hal::Display; the pads' LEDs are a fake hal::Leds.
//   * panel_image() is the screen above a drawing of the 48 pads: each pad lit to its LED level, a held pad framed in blue.
//   * The audio can be recorded and written as a 16-bit stereo WAV at the engine's rate.

#include <o3sdk/app/app.h>
#include <o3sdk/app/panel.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/firmware.h>
#include <o3sdk/hal/leds.h>
#include <o3sdk/platform/host/fake_capture.h>
#include <o3sdk/platform/host/fake_firmware.h>
#include <o3sdk/platform/host/fake_midi.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/platform/host/host_card.h>
#include <o3sdk/platform/host/protect_card.h>
#include <o3sdk/sim/eve_raster.h>
#include <o3sdk/sim/raster_display.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace o3sdk::sim {

// hal::Leds of the simulator: keeps what was staged; flush() always finishes at once.
class FakeLeds final : public hal::Leds {
 public:
  unsigned count() const noexcept override { return hal::kPadLedCount; }
  void set(unsigned index, std::uint8_t level) noexcept override {
    if (index < hal::kPadLedCount) {
      level_[index] = level;
    }
  }
  void clear() noexcept override {
    for (auto& l : level_) {
      l = 0;
    }
  }
  bool flush() noexcept override { return true; }
  bool online() const noexcept override { return true; }
  std::uint8_t level(unsigned index) const noexcept { return index < hal::kPadLedCount ? level_[index] : 0; }

 private:
  std::uint8_t level_[hal::kPadLedCount] = {};
};

class Harness {
 public:
  // `firmware`: the update path the system Config screen lists (a platform::host::FakeFirmwareSink; null: no FIRMWARE section). `midi`: the
  // MIDI device of the platform (a platform::host::FakeMidi; null: none, no MIDI section, the runtime as it was before MIDI). `card`: the SD
  // card of the platform (a platform::host::DirCard over a directory, or any HostCard; null: none, so Context::files() says "NO CARD" and the
  // kit keeps no settings); the harness puts a write-protect switch in front of it (card_protect). `app_id`: the folder name of Apps/<id>/ when
  // the app's build gave none (a test app; null: o3sdk_app_id()). `capture`: the codec's input and the FM radio of the platform (a platform::host::FakeCapture; null: none, no
  // AUDIO section, Context::audio_in() and radio() are refused, and the input of every block is silence); with one, the harness gives the engine what its read_block() delivers and
  // steps its service() once a millisecond. All of them must outlive the harness.
  explicit Harness(app::App& app, hal::firmware::FirmwareSink* firmware = nullptr, platform::host::FakeMidi* midi = nullptr,
                   platform::host::HostCard* card = nullptr, const char* app_id = nullptr, platform::host::FakeCapture* capture = nullptr);
  ~Harness();
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  // Runtime::start(): the assets, App::setup, the engine; then, with a card, the card is mounted (as the unit's main does after the boot) and the kit's
  // settings are read from it (Runtime::load_settings). false: the display refused the assets, or the app's setup had a call that
  // the engine refused (Engine::setup_errors() != 0: an attach, a route or a hook it did not take). Then setup_report() says which,
  // and start() has also written it to stderr, so a test that REQUIREs start() shows why it failed.
  bool start();
  // One line per refused setup call, e.g. "setup refused: set_source(slot 9, unit 0): slot, bus or index out of range\n"; "" when the
  // setup went through (at most Engine's kMaxSetupErrors lines, then a count of the rest).
  std::string setup_report() const;
  // Moves time on by `ms` milliseconds.
  void run_ms(std::uint32_t ms);

  void press(app::Button b);
  void release(app::Button b);
  void pad_down(unsigned pad);
  void pad_up(unsigned pad);
  void turn(int detents);

  // The screen above the pads: RGBA8, row-major.
  std::vector<std::uint8_t> panel_image(int& width, int& height) const;
  bool snapshot(const std::string& path) const;

  // Draws the boot frame (ui::show_boot_screen: the logo, this app's name and the line "o3sdk simulator") as the unit's first frame is.
  // The next frame the runtime draws replaces it. false: the display took no frame.
  bool show_boot_screen();

  // Audio capture: while on, every rendered block is kept (interleaved L, R int16).
  void record(bool on) { recording_ = on; }
  const std::vector<std::int16_t>& audio() const { return audio_; }
  bool write_wav(const std::string& path) const;

  // The card: the slot's switch (`card out` and `card in` in a scenario; a card put back is mounted at once, as the platform's watch does after its delay, and the runtime told, as
  // Runtime::card_changed), the write-protect switch (`card protect on|off`), and the one the platform sees (null without a card).
  platform::host::ProtectCard* card() { return protect_.get(); }
  void card_out();
  void card_in();
  void card_protect(bool on);
  // A restart of the unit on the same card (`reboot`): the card is power-cycled (a file being written stays torn, as a power cut leaves it), the engine and the runtime are made again from
  // nothing and started as at boot (App::setup, then the settings read from the card). The app object is the same one (it keeps what it holds in memory), as every test starts a fresh
  // simulator on the one o3sdk_app(). Time goes on. false: as start().
  bool reboot();

  // The fake MIDI device the harness was given (null without one), and the timeline sample "now" is (the samples due at the engine's rate by
  // the simulated time): what a scripted `midi` line is stamped with, so it lands in the block that renders next.
  platform::host::FakeMidi* midi() { return midi_; }
  // The fake capture device the harness was given (null without one).
  platform::host::FakeCapture* capture() { return capture_; }
  std::uint64_t due_samples() const;

  app::Runtime& runtime() { return *runtime_; }
  dsp::Engine& engine() { return *engine_; }
  const EveRaster& raster() const { return *raster_; }
  const RasterDisplay& display() const { return *display_; }
  const FakeLeds& leds() const { return leds_; }
  std::uint64_t uptime_us() const { return clock_.uptime_us(); }
  std::uint64_t samples_rendered() const { return samples_; }

 private:
  void push(core::InputKind kind, std::uint8_t id, std::int16_t value);
  void render_due();
  void make_runtime();

  app::App& app_;
  platform::host::FakeMidi* midi_;
  platform::host::FakeCapture* capture_;
  hal::firmware::FirmwareSink* firmware_;
  platform::host::HostCard* inner_card_;
  const char* app_id_;
  std::unique_ptr<platform::host::ProtectCard> protect_;
  std::unique_ptr<app::Files::Work> files_work_;
  std::unique_ptr<EveRaster> raster_;
  std::unique_ptr<RasterDisplay> display_;
  platform::host::FakeInput input_;
  platform::host::FakeClock clock_;
  FakeLeds leds_;
  std::unique_ptr<dsp::Engine> engine_;
  std::unique_ptr<app::Platform> platform_;
  std::unique_ptr<app::Runtime> runtime_;
  std::uint64_t samples_ = 0;
  bool recording_ = false;
  std::vector<std::int16_t> audio_;
};

// Fills `fake` with the *.ptf files of `dir`, in name order (the first is the reset target), each with its size; a file whose first line
// is one decimal number (a fixture) is listed with that size. Nothing else is read from them. A check takes 300 passes of the main loop (300 ms of simulated time). A name that contains "bad" fails its check at byte
// 1024 ("bad vector table"); every other file checks out. An install the check allowed never ends, as on the unit. false, with
// `error` set: `dir` is not a directory.
bool load_firmware_dir(platform::host::FakeFirmwareSink& fake, const std::string& dir, std::string& error);

// A 16-bit stereo PCM WAV of interleaved samples at `rate` Hz.
bool write_wav_s16(const std::string& path, const std::vector<std::int16_t>& lr, std::uint32_t rate);

}  // namespace o3sdk::sim
