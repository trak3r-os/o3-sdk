// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MIDI HAL contract (docs/M2B2-PLAN.md section 3): what the runtime asks of the platform's MIDI in and out, the DIN jack and the USB
// interfaces. The kit sees this contract only. On the MK66 it is platform/mk66's MidiService (the timing core of platform/mk66/midi: the
// UART and USB interrupts stamp what arrives, the clock-follow loop turns the master's clock into a ClockFrame, the emitters put bytes on
// the wire at their sample); the host and the simulator get platform/host/fake_midi.h.
//
// Contexts. Two sides, and the rule is the engine's: the render side runs inside the audio callback (Runtime::render_block), once a block,
// begin_block() then end_block(), never waiting; the main side runs in the main loop. The two meet through the device's own rings and a
// single-writer box for the configuration, so neither blocks the other.
//
//   render   begin_block(frames, in, cap, clock) -> n
//              `in` gets up to `cap` messages the render turns into notes, oldest first: note on (velocity > 0), note off (a 0x8n, or a 0x9n
//              with velocity 0) and the controllers 120 and 123 (all sound / all notes off) of any channel and port, each with `frame` the
//              frame of this block its arrival maps to (0 .. frames - 1, through the audio clock: a constant latency, no jitter). Messages
//              past `cap` wait for the next block. `clock` is the transport of this block: clock.follow false leaves the engine its own.
//            end_block(transport, out, n)
//              after the engine rendered: `transport` is the block's transport as the engine ran it (Engine::block_frame), from which the
//              device sends the clock (24 per quarter note, one at every frame_at(t, k * 40) of the block), Start at `started`, Stop at
//              `stopped` and, for a run that begins elsewhere, the song position and Continue; `out` the n messages the hooks asked to send
//              (BlockContext::send_midi), each at its `frame`. Which ports: Config::out_ports for the clock and the transport, the message's
//              `ports` for a message (0: Config::out_ports). Clock out needs Config::clock_out, Start / Stop / SPP / Continue
//              Config::transport_out.
//   main     set_config(c)   the latest call wins; the render takes it at the start of its next block and never waits for it.
//            status()        a snapshot, a block old at worst.
//            poll(m)         the next channel message received, in order, notes included: the app's (Runtime hands it to App::midi unless a
//                            route took it). false: none. A full ring drops the newest and counts it (Status::dropped).
//            send(m)         one message from the main loop (a pad that sends its note), at frame 0 of the next block. false: refused.
//            request(r)      Engine::play() / stop() while the device is following: Play starts the run on the master's next clock, Stop
//                            stops the transport (until the master starts it again).
//
// The follow contract (what the engine relies on; every clocked hook follows with no change):
//   * ClockFrame::pos at frame 0 is the master's tick at that sample (40 ticks a clock), inc the DLL's ticks a sample, tempo_mbpm its tempo.
//   * The loop slews and never jumps: pos(next block) == pos + inc * frames, whatever the correction does to inc.
//   * Start: `started` with pos 0, on the block the run begins at. A position cannot be negative, so a Start whose first clock falls
//     inside a block begins the run on that block's boundary (the run is then up to one block, 1.45 ms, early or late for good, the same
//     for every step); a hook that begins at the first whole tick not passed (o3-303) takes a small pos as well. Continue after an SPP:
//     `playing` from the song position (the SPP's sixteenths x 240 ticks), `started` false. Stop: `stopped` once. Transport bytes are
//     acted on only with Config::transport_in; the clock is followed without it.
//   * When the master takes over from a run of the engine's own, the engine ends that run (a hook sees `stopped` once, and `playing` again
//     if the master plays); when it is let go the engine stops and its own tempo is back. play() and stop() while `follow` is set reach the
//     device as request().
//   * A master quiet for 48 clocks stops the transport and is let go (the next frames have follow false).
//   * Config::follow off, or a clock that is not there: follow is false.

#include <o3sdk/core/midi_types.h>

#include <cstdint>

namespace o3sdk::hal::midi {

enum class Port : std::uint8_t { Din = 0, Usb = 1 };
inline constexpr unsigned kPortCount = 2;
constexpr std::uint8_t port_bit(Port p) noexcept { return p == Port::Din ? core::kMidiDin : core::kMidiUsb; }

// The user's settings (the MIDI section of the system Config edits them), defaults as docs/M2B2-PLAN.md D3: follow off; clock and
// transport out on both ports; notes in on any channel; out on channel 1. A plain value, copied by the main loop.
struct Config {
  std::uint8_t channel = 0;                  // notes in: 0 omni, 1 .. 16 (the routes that follow it; app/midi.h)
  std::uint8_t out_channel = 1;              // 1 .. 16: the channel an app puts its notes on (the device does not rewrite a message)
  std::uint8_t out_ports = core::kMidiAllPorts;  // core::kMidiDin | kMidiUsb: where the clock and the transport go; 0 for none
  bool clock_out = true;                     // 24 a quarter note while the transport plays
  bool follow = false;                       // the engine's transport follows the clock of follow_port
  Port follow_port = Port::Din;
  bool transport_in = true;                  // false: Start, Continue, Stop and SPP of follow_port are ignored (the clock is still followed)
  bool transport_out = true;                 // false: no Start, Continue, Stop or SPP goes out (the clock still does)
};

// The value with every field forced into its range, which is what a device acts on: out of range is clamped, never refused.
constexpr Config sanitized(Config c) noexcept {
  c.channel = c.channel > 16u ? std::uint8_t{0} : c.channel;
  c.out_channel = c.out_channel < 1u ? std::uint8_t{1} : (c.out_channel > 16u ? std::uint8_t{16} : c.out_channel);
  c.out_ports = static_cast<std::uint8_t>(c.out_ports & core::kMidiAllPorts);
  c.follow_port = static_cast<unsigned>(c.follow_port) >= kPortCount ? Port::Din : c.follow_port;
  return c;
}

// What the Config screen shows, published by the render a block at a time.
struct Status {
  bool following = false;          // the engine's transport is the master's (ClockFrame::follow of the last block)
  bool locked = false;             // the clock-follow loop's own verdict: the period is steady
  std::uint32_t tempo_mbpm = 0;    // the master's tempo, 0 without a clock
  std::uint32_t rx = 0;            // messages received since start, both ports
  std::uint32_t tx = 0;            // messages sent (the clock and the transport count)
  std::uint32_t dropped = 0;       // messages lost to a full ring, in either direction
};

class Device {
 public:
  // ---- Render side ----------------------------------------------------------------------------------------------------------------
  virtual unsigned begin_block(std::uint32_t frames, core::MidiMessage* in, unsigned cap, core::ClockFrame& clock) noexcept = 0;
  virtual void end_block(const core::ClockFrame& transport, const core::MidiMessage* out, unsigned n) noexcept = 0;

  // ---- Main loop ------------------------------------------------------------------------------------------------------------------
  virtual void set_config(const Config& c) noexcept = 0;
  virtual Status status() const noexcept = 0;
  virtual bool poll(core::MidiMessage& m) noexcept = 0;
  virtual bool send(const core::MidiMessage& m) noexcept = 0;
  virtual void request(core::TransportRequest r) noexcept = 0;

 protected:
  Device() = default;
  ~Device() = default;  // never deleted through the interface: no heap, so no virtual destructor slot
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
};

}  // namespace o3sdk::hal::midi
