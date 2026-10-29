// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An app: one firmware image (docs/FRAMEWORK.md, "Apps"). It owns its units, its devices and its screens as plain objects (no heap),
// wires them into the engine in setup() and chooses the first screen. Every app defines the one entry point
//
//   o3sdk::app::App& o3sdk_app() noexcept { static HelloApp app; return app; }
//
// which the platform's main (platform/mk66) and the simulator (sim/) call. Nothing else in the image names the app. The Config button
// belongs to the runtime (the system Config screen): an app's input() never sees it.

#include <o3sdk/app/diag.h>
#include <o3sdk/app/input.h>
#include <o3sdk/core/midi_types.h>

#include <cstddef>

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
  // ---- MIDI (midi.h, docs/M2B2-PLAN.md section 3). Main loop, once for each message, in order, before update(). --------------------------
  // A channel message that no route of ctx.midi() took: control changes, program changes, pitch bend, notes of a channel nothing listens to
  // (and notes at all, without routes). `m.ports` is the port it came in on, `m.frame` is not meaningful. true: the app used it (the kit has no
  // other consumer today). The notes the routes took reach their units in the render and never come here. Default: false.
  virtual bool midi(Context&, const core::MidiMessage&) noexcept { return false; }

  // ---- The system Config screen (system_config.h, docs/M2B2-PLAN.md section 4). The runtime owns it; an app cannot hide it. ------------
  // What stops a firmware update right now (a recording, an export, unsaved work), as the strip says it in upper case, or nullptr (the
  // default). Asked when the FIRMWARE list opens and again at INSTALL; the update is refused with that text while it is not null.
  virtual const char* update_blocker(const Context&) const noexcept { return nullptr; }
  // The pages the app adds to the Config screen. Declared for the apps to come: the kit draws none yet, and 0 (the default) is the only
  // value that means something.
  virtual unsigned config_pages() const noexcept { return 0; }

  // ---- Diagnostics (diag.h; the platform's console, docs/FRAMEWORK.md section 5). Both default to nothing. -----------------------
  // Extra console commands: points `rows` at a table that lives as long as the app (a constexpr array) and returns its length. Asked
  // once, when the console attaches (the rules a row must meet are in diag.h; a row that breaks one is skipped and logged).
  virtual std::size_t diag_commands(const DiagCommand*& rows) const noexcept {
    rows = nullptr;
    return 0;
  }
  // The app's part of the console's `state` line: `key=value` pairs (StateWriter) written into `out`, NUL terminated, at most `cap`
  // bytes with the terminator, no leading or trailing space; returns the length (0: nothing to add). Main loop only; read only.
  virtual std::size_t describe_state(const Context&, char* /*out*/, std::size_t /*cap*/) const noexcept { return 0; }

 protected:
  App() = default;
  ~App() = default;
  App(const App&) = delete;
  App& operator=(const App&) = delete;
};

}  // namespace o3sdk::app

// Defined by the app (exactly one per image).
o3sdk::app::App& o3sdk_app() noexcept;
// The app's id: the folder `Apps/<id>/` of its files and the name its settings are kept under (app/files.h). the CMake function o3sdk_app() (cmake/O3sdkTargets.cmake) generates it from the CMake id
// (`o3sdk_app(hello ...)`: "hello"); an app of another build defines it itself, or the files service is off. At most 15 bytes of letters, digits, '_', '-' and ' ', no '.'.
const char* o3sdk_app_id() noexcept;
