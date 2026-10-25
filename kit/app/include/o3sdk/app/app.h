// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An app: one firmware image (docs/FRAMEWORK.md, "Apps"). It owns its units, its devices and its screens as plain objects (no heap),
// wires them into the engine in setup() and chooses the first screen. Every app defines the one entry point
//
//   o3sdk::app::App& o3sdk_app() noexcept { static HelloApp app; return app; }
//
// which the platform's main (platform/mk66) and the simulator (sim/) call. Nothing else in the image names the app.

#include <o3sdk/app/input.h>

namespace o3sdk::app {

class Context;

struct AppInfo {
  const char* name;     // shown in the header strip and on the boot screen, upper case ("HELLO", "O3-303")
  const char* version;  // "0.1.0"
};

class App {
 public:
  virtual const AppInfo& info() const noexcept = 0;
  // Main loop, once, with the engine in its setup phase: attach and route the units, add the clocked hooks, open the first screen
  // (ctx.show). The runtime starts the engine afterwards.
  virtual void setup(Context& ctx) noexcept = 0;
  // Every input, before the screen sees it: the app's global keys (the screen buttons, Play). true: consumed, the screen does not
  // get it.
  virtual bool input(Context&, const Input&) noexcept { return false; }
  // Every pass of the main loop, before the screen's update.
  virtual void update(Context&) noexcept {}

 protected:
  App() = default;
  ~App() = default;
  App(const App&) = delete;
  App& operator=(const App&) = delete;
};

}  // namespace o3sdk::app

// Defined by the app (exactly one per image).
o3sdk::app::App& o3sdk_app() noexcept;
