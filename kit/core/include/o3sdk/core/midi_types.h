// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI as the layers of the kit hand it to each other (docs/M2B2-PLAN.md section 3): the two plain values that cross the layering
// `core <- hal` and `core <- dsp` (a message, the clock frame of a block), the port bits and the status bytes. Nothing here knows a
// device, an engine or an app; hal/midi.h is the device contract, dsp/engine.h the render side, app/midi.h the app's side.

#include <cstdint>
#include <type_traits>

namespace o3sdk::core {

// ---- Ports -----------------------------------------------------------------------------------------------------------------------
// As bits of a mask: MidiMessage::ports, hal::midi::Config::out_ports.
inline constexpr std::uint8_t kMidiDin = 1u << 0;
inline constexpr std::uint8_t kMidiUsb = 1u << 1;
inline constexpr std::uint8_t kMidiAllPorts = kMidiDin | kMidiUsb;

// ---- Status bytes ----------------------------------------------------------------------------------------------------------------
inline constexpr std::uint8_t kMidiNoteOff = 0x80;  // the channel kinds: the status byte without its channel nibble
inline constexpr std::uint8_t kMidiNoteOn = 0x90;
inline constexpr std::uint8_t kMidiPolyPressure = 0xA0;
inline constexpr std::uint8_t kMidiControl = 0xB0;
inline constexpr std::uint8_t kMidiProgram = 0xC0;
inline constexpr std::uint8_t kMidiChannelPressure = 0xD0;
inline constexpr std::uint8_t kMidiPitchBend = 0xE0;
inline constexpr std::uint8_t kMidiSongPosition = 0xF2;  // data1 the low 7 bits, data2 the high 7 bits, in MIDI beats (a sixteenth)
inline constexpr std::uint8_t kMidiClock = 0xF8;         // 24 a quarter note
inline constexpr std::uint8_t kMidiStart = 0xFA;
inline constexpr std::uint8_t kMidiContinue = 0xFB;
inline constexpr std::uint8_t kMidiStop = 0xFC;
inline constexpr std::uint8_t kCcAllSoundOff = 120;  // the two controllers that end every note of a channel
inline constexpr std::uint8_t kCcAllNotesOff = 123;

// One MIDI message of at most three bytes, with the place it belongs to in a block.
//   input    `ports` is the port it arrived on (one bit); `frame` the frame of the block its arrival maps to (0 .. frames - 1).
//   output   `ports` the ports to send it on (0: Config::out_ports); `frame` the frame of the block it leaves at. Messages that are not
//            channel messages (the clock, SPP) have `status` 0xF8.., 0xF2 and the data bytes they need, 0 otherwise.
// `data1` and `data2` are 7-bit values.
struct MidiMessage {
  std::uint8_t ports;
  std::uint8_t status;
  std::uint8_t data1;
  std::uint8_t data2;
  std::uint16_t frame;
};
static_assert(sizeof(MidiMessage) == 6 && std::is_trivially_copyable<MidiMessage>::value, "a message is copied through rings");

constexpr bool midi_is_channel(std::uint8_t status) noexcept { return status >= 0x80u && status < 0xF0u; }
// The kind of a channel status byte (0x80 .. 0xE0) and its channel as the user counts it (1 .. 16).
constexpr std::uint8_t midi_kind(std::uint8_t status) noexcept { return static_cast<std::uint8_t>(status & 0xF0u); }
constexpr std::uint8_t midi_channel(std::uint8_t status) noexcept { return static_cast<std::uint8_t>((status & 0x0Fu) + 1u); }
// The status byte of `kind` on `channel` (1 .. 16; 0 counts as 1).
constexpr std::uint8_t midi_status(std::uint8_t kind, std::uint8_t channel) noexcept {
  return static_cast<std::uint8_t>((kind & 0xF0u) | ((channel > 0u ? channel - 1u : 0u) & 0x0Fu));
}

constexpr MidiMessage midi_note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity, std::uint16_t frame = 0,
                                   std::uint8_t ports = 0) noexcept {
  return MidiMessage{ports, midi_status(kMidiNoteOn, channel), static_cast<std::uint8_t>(note & 0x7Fu),
                     static_cast<std::uint8_t>(velocity & 0x7Fu), frame};
}
constexpr MidiMessage midi_note_off(std::uint8_t channel, std::uint8_t note, std::uint16_t frame = 0, std::uint8_t ports = 0) noexcept {
  return MidiMessage{ports, midi_status(kMidiNoteOff, channel), static_cast<std::uint8_t>(note & 0x7Fu), 0u, frame};
}
constexpr MidiMessage midi_control(std::uint8_t channel, std::uint8_t cc, std::uint8_t value, std::uint16_t frame = 0,
                                   std::uint8_t ports = 0) noexcept {
  return MidiMessage{ports, midi_status(kMidiControl, channel), static_cast<std::uint8_t>(cc & 0x7Fu),
                     static_cast<std::uint8_t>(value & 0x7Fu), frame};
}

// The transport of one block, as a device reports it to the engine (Engine::follow) and the engine reports it back for the clock
// and transport it sends (Engine::block_frame). Positions are Q32.32 ticks at 960 per quarter note, as dsp::Transport counts them.
//   follow    the block's transport is the master's: the device is following a clock that is there. Every other field is meaningful
//             only then; false means the engine runs its own transport (and on the way back it stops, once).
//   playing   the master's transport runs. started: this is the first block of a run (Start, or Play while following) and `pos` is
//             0; stopped: the first block after a stop (the engine adds it if the device forgets). A Continue is `playing` with
//             `started` false and `pos` at the song position.
//   pos       ticks at frame 0 of the block (0 while not playing); inc: ticks a sample. Continuous from block to block: the device
//             slews `inc`, it never moves `pos` except at a Start or a Continue.
//   tempo_mbpm  the master's tempo in milli-BPM (0: unknown, the engine keeps its own).
struct ClockFrame {
  bool follow = false;
  bool playing = false;
  bool started = false;
  bool stopped = false;
  std::uint64_t pos = 0;
  std::uint64_t inc = 0;
  std::uint32_t tempo_mbpm = 0;
};
static_assert(std::is_trivially_copyable<ClockFrame>::value, "a clock frame is a plain value");

// What the main loop asks of a device that is following when the app calls Engine::play() or stop(): Play starts the run on the master's
// next clock, Stop stops it (docs/M2B2-PLAN.md section 3).
enum class TransportRequest : std::uint8_t { None, Play, Stop };

}  // namespace o3sdk::core
