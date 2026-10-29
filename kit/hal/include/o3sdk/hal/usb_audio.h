// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB audio HAL (docs/M2C-PLAN.md section 7): what an app asks of the platform's USB port beyond the console and MIDI it always has: the audio the unit sends to the computer (the
// master and, per slot, stems), the stereo stream the computer sends back, the USB profile, and the card shared to the computer as a disk. Control side only, main loop only; the audio itself
// moves in the render and in the USB interrupt (platform/mk66/audio usb_audio_service.h, the same pure code in the simulator). The app layer's face is Context::usb_audio() (app/usb_audio.h).
//
//   THE LAYOUT. The USB port presents one of two sets: STANDARD (the console and MIDI only) or AUDIO, which adds the audio streams of a profile (track3r-os's, byte for byte, so a computer's
//   cached layout is the proven one): P2 the master alone; P8 eight channels, P10 ten, P11 eleven. The master pair is always the LAST TWO channels, so a DAW's routing of channels 7 and 8 in
//   the boot profile (P8) is the same as track3r-os's. `duplex` adds the stereo stream the computer plays to the unit. A change re-enumerates the port: the computer drops it and finds it again
//   about a second later, and the console with it. The image boots in AUDIO, P8, duplex.
//
//   THE STEMS. The channels before the master pair carry one slot each: a slot is the saturating sum of the stem sources its mask names, bit i of the sources in the order of dsp::kStemSources
//   (T1..T12, Master L, Master R, Send A, Send B; the engine's slots 1..8 are T1..T8, post-fader, pre-pan, mono). The platform sets the profile's factory slots when the profile changes
//   (P10: S1..S8 = T1..T8, S9 and S10 the master; P11: T1..T9; P8: o3-sdk's own, none: S1..S6 empty, so the boot profile streams the master and silence on 1..6, as it always did); set_stem()
//   changes one. The engine stages a source only while the computer has the capture stream open and a slot names it: an app that never streams stems pays nothing.
//
//   THE RETURN. What the computer plays is added to the output after the render at return_level() (0..1, linear), outside the master inserts and never captured by a take or a stem; with the
//   audio in's source Usb it is also the engine's input.
//
//   THE DISK. disk_share() hands the card to the computer: the port re-enumerates with a mass-storage function added, the card is the computer's until the eject (disk_eject() or the
//   computer's), the unit reads and writes nothing meanwhile (Files::card() is OnComputer, the kit's settings stay dirty) and the previous layout comes back. The platform refuses while
//   the card is in use. Never at boot.
//
// Main loop only unless a line says otherwise. The implementation is a static object: never deleted through the interface.

#include <cstdint>

namespace o3sdk::hal::usb_audio {

enum class Profile : std::uint8_t { P2, P8, P10, P11 };
inline constexpr unsigned kProfileCount = 4;
inline constexpr unsigned kMaxChannels = 11;  // P11

constexpr unsigned profile_channels(Profile p) noexcept {
  switch (p) {
    case Profile::P2: return 2;
    case Profile::P8: return 8;
    case Profile::P10: return 10;
    case Profile::P11: return 11;
  }
  return 2;
}
constexpr const char* profile_name(Profile p) noexcept {
  switch (p) {
    case Profile::P2: return "P2";
    case Profile::P8: return "P8";
    case Profile::P10: return "P10";
    case Profile::P11: return "P11";
  }
  return "";
}

// The stem sources, as bits of a slot's mask: the engine's slots 1..8 are T1..T8 (bits 0..7), then T9..T12 (no slot behind them: silence), the master pair and the two sends.
inline constexpr unsigned kStemSources = 16;
constexpr std::uint32_t track_source(unsigned slot) noexcept { return slot >= 1u && slot <= 12u ? 1u << (slot - 1u) : 0u; }  // slot 1..12
inline constexpr std::uint32_t kSourceMasterL = 1u << 12;
inline constexpr std::uint32_t kSourceMasterR = 1u << 13;
inline constexpr std::uint32_t kSourceSendA = 1u << 14;
inline constexpr std::uint32_t kSourceSendB = 1u << 15;

// ---- The card as a disk ---------------------------------------------------------------------------------------------------------------
enum class DiskPhase : std::uint8_t { Idle, Working, Out };  // Working: the hand-over or the eject is running; Out: the computer has the card
enum class DiskRefusal : std::uint8_t {
  None,         // the request went through (a hand-over runs from the next passes)
  Unavailable,  // no such function on this platform, or the driver is down
  NotIdle,      // a hand-over is already running, or the disk is out
  NoCard,       // no card for the unit to give (out, or already the computer's)
  Busy,         // the card is in use (a load, a save, a listing, the update path)
  SaveBusy,     // a save is running
  Layout,       // the USB port cannot change its set now (a change is on its way)
};
struct DiskStatus {
  bool available = false;  // the platform can share the card
  DiskPhase phase = DiskPhase::Idle;
  bool card_present = false;
  bool busy = false;      // a claim says the card is in use now: the request would be refused
  bool writable = false;  // while Out: the computer was given a writable disk
  bool audio = false;     // the unit presents USB audio now: the share pauses it
};

class Device {
 public:
  // The driver runs and presents a set. Until it does nothing below is applied (the layout is only remembered by the app layer).
  virtual bool ready() const noexcept = 0;

  // ---- The layout ------------------------------------------------------------------------------------------------------------------------
  virtual bool audio() const noexcept = 0;        // the AUDIO set is presented (or being changed to), else STANDARD
  virtual Profile profile() const noexcept = 0;   // the profile of the set presented, or being changed to
  virtual bool duplex() const noexcept = 0;       // the return stream is part of it
  virtual bool changing() const noexcept = 0;     // a re-enumeration is on its way
  // Asks for a layout. true: it is presented already or the change is on its way (the port drops and comes back). false: refused for now (a change is already running, the disk is out, the
  // driver is down). In STANDARD the profile and `duplex` are remembered for the next AUDIO attach.
  virtual bool request(bool audio, Profile profile, bool duplex) noexcept = 0;
  bool request_profile(Profile profile, bool duplex) noexcept { return request(audio(), profile, duplex); }

  // ---- The stems -------------------------------------------------------------------------------------------------------------------------
  // Slot `channel` (0-based, below profile_channels(profile())) sums the sources of `sources` (bits above kStemSources are dropped). false: not a channel of the profile.
  virtual bool set_stem(unsigned channel, std::uint32_t sources) noexcept = 0;
  virtual std::uint32_t stem(unsigned channel) const noexcept = 0;
  // The factory slots of the profile (as a profile change gives them).
  virtual void reset_stems() noexcept = 0;

  // ---- The return ------------------------------------------------------------------------------------------------------------------------
  virtual void set_return_level(float level) noexcept = 0;  // 0..1 linear (clamped, NaN is 0); a change ramps across a block
  virtual float return_level() const noexcept = 0;
  // The audio in's source is Usb: the return is also what the engine hears as its input (the codec's block is replaced by it, silence while the computer plays nothing). The app layer
  // (AudioIn) calls it with its choice.
  virtual void route_return_to_input(bool on) noexcept = 0;
  virtual bool streaming() const noexcept = 0;         // the computer has the capture stream open (the IN alternate setting)
  virtual bool return_streaming() const noexcept = 0;  // the computer has the return stream open

  // ---- The card as a disk ----------------------------------------------------------------------------------------------------------------
  virtual DiskStatus disk() const noexcept = 0;
  // `read_only` is this session's policy: the computer is given a write-protected disk. Refused without touching anything unless None.
  virtual DiskRefusal disk_share(bool read_only) noexcept = 0;
  // The unit's eject (the computer's own arrives by itself). false: the disk is not out.
  virtual bool disk_eject() noexcept = 0;

 protected:
  Device() = default;
  ~Device() = default;  // never deleted through the interface: static instances, no heap
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
};

constexpr const char* disk_refusal_text(DiskRefusal r) noexcept {
  switch (r) {
    case DiskRefusal::None: return "";
    case DiskRefusal::Unavailable: return "NO DISK MODE ON THIS UNIT";
    case DiskRefusal::NotIdle: return "A HAND-OVER IS RUNNING OR THE DISK IS OUT";
    case DiskRefusal::NoCard: return "NO CARD TO GIVE";
    case DiskRefusal::Busy: return "THE CARD IS IN USE";
    case DiskRefusal::SaveBusy: return "A SAVE IS RUNNING";
    case DiskRefusal::Layout: return "THE USB PORT IS CHANGING";
  }
  return "";
}

}  // namespace o3sdk::hal::usb_audio
