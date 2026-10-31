// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A screen: what the display, the pads, the soft keys and the encoder mean right now (docs/FRAMEWORK.md, "Apps"). The runtime
// draws the chrome around it (the header strip at the top, the soft-key row at the bottom) and calls the screen for the rest.
//
// Every call is in the main loop: no audio work here. A screen talks to the sound through Context::engine() (notes, parameters,
// transport) and reads the render's state through the engine's statistics or the app's own snapshots.

#include <o3sdk/app/input.h>
#include <o3sdk/app/pad_leds.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::app {

class Context;

// The labels of F1..F8 as the screen wants them drawn now (empty or null: a blank key). `hot`: bit n marks key n as the one in
// force (an inverted chip); `dim`: bit n draws its label dimmed (a key that does nothing here).
struct SoftKeys {
  const char* label[8] = {};
  std::uint8_t hot = 0;
  std::uint8_t dim = 0;
};

// Who draws the edges of the display. Full: the runtime's header strip and soft-key row, and draw() gets kContentArea. None: neither is
// drawn and draw() gets kScreenArea (800 x 480): the screen shows its own title, tempo, transport and F labels.
enum class Chrome : std::uint8_t { Full, None };

class Screen {
 public:
  // Shown in the header strip, upper case ("PLAY", "PATTERN").
  virtual const char* title() const noexcept = 0;
  // Asked every frame; default Full, the chrome as it has always been. With None the runtime draws no header and no soft-key row, never asks
  // soft_keys(), and draws its system strip (the power-off hold, the card and USB notices) last, over the bottom 28 px of the screen. Ignored while
  // the system Config screen is open: it always has the full chrome.
  virtual Chrome chrome(const Context&) const noexcept { return Chrome::Full; }
  // The theme the whole frame is drawn with while this screen is up: the clear, the chrome, the content, the text and the system strip. Null (the
  // default): the context's theme (Context::theme()). The theme must live as long as the app. Ignored while the system Config screen is open (it is
  // drawn with the context's theme). Asked every frame.
  virtual const ui::Theme* theme(const Context&) const noexcept { return nullptr; }
  virtual void enter(Context&) noexcept {}
  virtual void leave(Context&) noexcept {}
  virtual void input(Context&, const Input&) noexcept {}
  // Every pass of the main loop (timers, following the playhead).
  virtual void update(Context&) noexcept {}
  // The content area between the header and the soft keys. The renderer is begun; draw into `area` only.
  virtual void draw(Context&, ui::Renderer&, const ui::Rect& area) noexcept = 0;
  // The soft-key row. With Shift held (ctx.panel().shift()) a screen may give its Shift row.
  virtual void soft_keys(Context&, SoftKeys&) noexcept {}
  // The pad LEDs: `leds` is cleared before the call.
  virtual void leds(Context&, PadLeds&) noexcept {}

 protected:
  Screen() = default;
  ~Screen() = default;
  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;
};

}  // namespace o3sdk::app
