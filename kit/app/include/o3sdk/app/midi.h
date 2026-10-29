// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An app's MIDI (docs/M2B2-PLAN.md section 3): what Context::midi() hands the app, on top of the platform's hal::midi::Device
// (Platform::midi; null on a platform or a test rig without one: then nothing arrives and nothing leaves, and the rest of the runtime is
// as it was).
//
//   Notes. route(unit, channel) makes a unit play from MIDI: a note on or off on that channel (0: the channel the user chose in the
//   system Config, any when it is "omni") becomes a note on or off for that unit at the frame its arrival maps to, one block after it came in: a
//   constant latency of about 1.5 ms and no jitter. All sound off and all notes off (controllers 120, 123) on the channel end the unit's
//   notes. At most kMaxRoutes routes; a unit has one. The routes work in the render context; set them in App::setup() or any time after.
//
//   Everything else. Every channel message that no route took (control changes, program changes, pitch bend, notes of another channel) reaches
//   App::midi() in the main loop, in order.
//
//   Clock. Nothing to do: with the user's FOLLOW on, the engine's transport is the master's clock (Engine::follow) and every clocked hook
//   follows with no change; Play and Stop then ask the master's device instead (Engine::play). With the engine playing, the clock and the
//   transport go out on the ports the user chose, and a hook may send its own messages with BlockContext::send_midi.
//
//   Learn. learn(unit, param) arms the next control change (controllers 0 .. 119, any port): it is bound to that parameter of the unit, and from then on moves it (core/learn.h; PICKUP by
//   default: a knob does nothing until it has reached the parameter, so a value never jumps). A bound controller no longer reaches App::midi(). One controller moves one parameter and a
//   parameter has one controller: a new binding takes the old one's place. At most kMaxBindings bindings, kept in the kit's settings (`LRNB` of Apps/<app>/system.o3s: they are about the
//   user's controller, not about the app's patterns, so they follow the unit and not a pattern file) and given back at boot. Main loop only.
//
//   Settings. config() and set_config() are the user's (hal::midi::Config); the MIDI section of the system Config edits them, so an app that
//   sets them in setup() sets the defaults. Main loop only, except the route table, which the render reads (single writer, one word an entry).

#include <o3sdk/app/settings.h>
#include <o3sdk/core/learn.h>
#include <o3sdk/core/midi_types.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/midi.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::app {

class Runtime;

class Midi {
 public:
  static constexpr unsigned kMaxRoutes = 4;
  static constexpr unsigned kMaxBindings = kLearnStoreSlots;

  Midi() noexcept;

  // The platform has a MIDI device. false: route() and set_config() keep their values, status() is empty, send() is refused.
  bool available() const noexcept { return dev_ != nullptr; }

  // ---- Routes -----------------------------------------------------------------------------------------------------------------------
  // `unit` plays the notes of `channel` (1 .. 16; 0: the user's channel, Config::channel, omni when that is 0). A unit that is routed already
  // takes the new channel. false, nothing changed: `unit` is not attached, `channel` past 16, or kMaxRoutes other units are routed.
  bool route(dsp::UnitId unit, std::uint8_t channel = 0) noexcept;
  // Takes `unit` off the routes. false: it was not routed.
  bool unroute(dsp::UnitId unit) noexcept;
  unsigned routes() const noexcept;

  // ---- Learn ----------------------------------------------------------------------------------------------------------------------------
  // Arms learn for parameter `param` (the unit's parameter id) of `unit`: the next control change binds. false, nothing armed: no engine, `unit` or `param` unknown, or the parameter is one
  // that applies on a confirmation (kParamApply: never learnable). Arming again re-aims it; an unbound parameter that is armed and then bound takes a free slot (false at the store's
  // limit, reported when the controller arrives: Learned::Full).
  bool learn(dsp::UnitId unit, std::uint16_t param, core::LearnMode mode = core::LearnMode::Pickup) noexcept;
  void learn_cancel() noexcept { armed_ = false; }
  bool learning() const noexcept { return armed_; }
  // Takes the binding of `param` off. false: it had none.
  bool unlearn(dsp::UnitId unit, std::uint16_t param) noexcept;
  void unlearn_all() noexcept;
  unsigned bindings() const noexcept;
  // The controller (0 .. 119) and channel (0: every) bound to `param`, or false.
  bool bound(dsp::UnitId unit, std::uint16_t param, std::uint8_t& cc, std::uint8_t& channel) const noexcept;
  // What the last controller that learn took was (for the screen's feedback); valid after Learned::Bound.
  std::uint8_t learned_cc() const noexcept { return learned_cc_; }
  std::uint8_t learned_channel() const noexcept { return learned_channel_; }

  // ---- Settings and traffic ------------------------------------------------------------------------------------------------------------
  const hal::midi::Config& config() const noexcept { return cfg_; }
  // Forced into range (hal::midi::sanitized), kept and handed to the device.
  void set_config(const hal::midi::Config& c) noexcept;
  hal::midi::Status status() const noexcept { return dev_ != nullptr ? dev_->status() : hal::midi::Status{}; }
  // One message from the main loop, at frame 0 of the next block; `ports` 0 means Config::out_ports. false: refused (no device, or its queue is full).
  bool send(const core::MidiMessage& m) noexcept { return dev_ != nullptr && dev_->send(m); }

  // The units whose routes take `m` (a note on or off, or controller 120 or 123, on a channel the route listens to), written to `units`;
  // returns how many. 0 for every other message. The runtime uses it in the render (to place the note) and in the main loop (to keep a routed
  // note from App::midi()).
  unsigned match(const core::MidiMessage& m, dsp::UnitId (&units)[kMaxRoutes]) const noexcept;

 private:
  friend class Runtime;
  // What the runtime does with a control change before the app sees it.
  enum class Learned : std::uint8_t { No, Bound, Full, Moved };  // No: not for learn; Bound: the armed parameter now has it; Full: armed and the store is full (nothing bound); Moved: a binding took it
  Learned take_cc(const core::MidiMessage& m) noexcept;
  void fill(KitValues& v) const noexcept;
  void restore(const KitValues& v) noexcept;

  void bind(hal::midi::Device* dev, dsp::Engine* engine) noexcept {
    dev_ = dev;
    engine_ = engine;
  }
  void push_config() noexcept {
    if (dev_ != nullptr) {
      dev_->set_config(cfg_);
    }
  }

  static constexpr std::uint16_t kFree = 0xFFFFu;  // unit kNoUnit: no route has it

  hal::midi::Device* dev_ = nullptr;
  dsp::Engine* engine_ = nullptr;
  hal::midi::Config cfg_;  // main loop
  // The render reads these: the route entries are (unit << 8 | channel) words, written one at a time by the main loop; the user's channel is a byte.
  std::atomic<std::uint16_t> route_[kMaxRoutes];
  std::atomic<std::uint8_t> user_channel_{0};
  // Learn: main loop only (the render never reads it; a CC moves a parameter through the engine's own main-side calls).
  core::LearnBinding slot_[kMaxBindings] = {};
  core::LearnRuntime state_[kMaxBindings] = {};
  bool armed_ = false;
  dsp::UnitId armed_unit_ = dsp::kNoUnit;
  std::uint16_t armed_param_ = 0;
  core::LearnMode armed_mode_ = core::LearnMode::Pickup;
  std::uint8_t learned_cc_ = 0;
  std::uint8_t learned_channel_ = 0;
};

}  // namespace o3sdk::app
