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
//   Settings. config() and set_config() are the user's (hal::midi::Config); the MIDI section of the system Config edits them, so an app that
//   sets them in setup() sets the defaults. Main loop only, except the route table, which the render reads (single writer, one word an entry).

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
  void bind(hal::midi::Device* dev, const dsp::Engine* engine) noexcept {
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
  const dsp::Engine* engine_ = nullptr;
  hal::midi::Config cfg_;  // main loop
  // The render reads these: the route entries are (unit << 8 | channel) words, written one at a time by the main loop; the user's channel is a byte.
  std::atomic<std::uint16_t> route_[kMaxRoutes];
  std::atomic<std::uint8_t> user_channel_{0};
};

}  // namespace o3sdk::app
