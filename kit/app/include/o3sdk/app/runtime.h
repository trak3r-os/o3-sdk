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

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/pad_leds.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/clock.h>
#include <o3sdk/hal/display.h>
#include <o3sdk/hal/input.h>
#include <o3sdk/hal/leds.h>

#include <cstdint>

namespace o3sdk::app {

static_assert(dsp::kMaxBlockFrames == hal::audio::kBlock, "the engine renders the HAL's block");

struct Platform {
  hal::Display& display;
  hal::Input& input;
  hal::Leds* leds;  // null: no pad LEDs (a test)
  const hal::Clock& clock;
  bool cpu_meter;   // the engine has a real cycle counter (the unit): the header shows the render's load
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

 private:
  static void render_block(void* user, const hal::audio::BlockIo& io) noexcept;
  void switch_screen() noexcept;
  void draw_header(ui::Renderer& r) noexcept;
  void draw_keys(ui::Renderer& r) noexcept;
  void update_leds() noexcept;

  App& app_;
  dsp::Engine& engine_;
  Platform& platform_;
  Context ctx_;
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
