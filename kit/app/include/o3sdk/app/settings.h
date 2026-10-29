// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The kit's own settings, as they are kept on the card for an app (docs/M2C-PLAN.md section 4, decision D5, docs/formats/o3sdk-settings.md): what the system Config
// screen lets the user change and the app did not ask for. They live in `Apps/<app>/system.o3s`, a chunk file (formats/chunk_file.h) of type `O3SK`:
//
//   SETS   u16 version (0x0100), u16 count, then `count` entries of u16 key and f32 value (6 bytes each)
//
// A key is never reused and a new one is appended (the table below), like a unit's parameter id: an older firmware that meets a key it does not know skips it and counts it,
// a newer one that finds a key missing keeps the default. Chunks this version does not know (APPV, the app's page values, and LRNB, the MIDI learn bindings, are reserved for
// the Config pages and learn that come later) are skipped and counted too; saving over such a file drops them. A newer major version of SETS is refused whole and the defaults
// are kept.
//
// This header is the codec only (pure, no card): app/files.h reads and writes the file, the runtime hands it the values.

#include <o3sdk/hal/capture.h>
#include <o3sdk/hal/midi.h>
#include <o3sdk/hal/usb_audio.h>
#include <o3sdk/ui/theme.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

// What the kit keeps. A plain value: the runtime builds it from the live state and compares it with the one last saved.
struct KitValues {
  ui::ThemeId theme = ui::ThemeId::Amiga;
  hal::midi::Config midi;
  // Audio in and the radio (audio_in.h, radio.h): hal::capture::Source and Region as numbers, per cent, and the frequency in 10 kHz units. The defaults are an untouched input.
  std::uint8_t in_source = 0;     // hal::capture::Source, 0 .. kSourceCount-1
  std::uint8_t in_gain = 0;       // 0 .. 100
  std::uint8_t in_monitor = 0;    // 0 .. 100 (0: off)
  std::uint8_t radio_region = 0;  // hal::capture::Region, 0 .. kRegionCount-1
  std::uint16_t radio_freq = 8750;
  // USB audio (usb_audio.h): the port's layout (the boot one: AUDIO, P8, duplex), the return's level in per cent and the stem slots when the user edited them. CARD AS DISK is never kept (D14).
  bool usb_audio = true;
  std::uint8_t usb_profile = static_cast<std::uint8_t>(hal::usb_audio::Profile::P8);  // hal::usb_audio::Profile, 0 .. kProfileCount-1
  bool usb_duplex = true;
  std::uint8_t usb_level = 100;  // 0 .. 100
  bool usb_map_set = false;      // usb_stem holds the edited slots of the profile
  std::uint16_t usb_stem[hal::usb_audio::kMaxChannels] = {};

  friend bool operator==(const KitValues& a, const KitValues& b) noexcept {
    return a.in_source == b.in_source && a.in_gain == b.in_gain && a.in_monitor == b.in_monitor && a.radio_region == b.radio_region && a.radio_freq == b.radio_freq && a.usb_audio == b.usb_audio && a.usb_profile == b.usb_profile &&
           a.usb_duplex == b.usb_duplex && a.usb_level == b.usb_level && a.usb_map_set == b.usb_map_set && same_stems(a, b) && a.theme == b.theme && a.midi.channel == b.midi.channel && a.midi.out_channel == b.midi.out_channel && a.midi.out_ports == b.midi.out_ports &&
           a.midi.clock_out == b.midi.clock_out && a.midi.follow == b.midi.follow && a.midi.follow_port == b.midi.follow_port &&
           a.midi.transport_in == b.midi.transport_in && a.midi.transport_out == b.midi.transport_out;
  }
  friend bool operator!=(const KitValues& a, const KitValues& b) noexcept { return !(a == b); }

 private:
  // The slots only count while the map is set (an unset map keeps whatever the array held).
  static bool same_stems(const KitValues& a, const KitValues& b) noexcept {
    if (!a.usb_map_set) {
      return true;
    }
    for (unsigned i = 0; i < hal::usb_audio::kMaxChannels; ++i) {
      if (a.usb_stem[i] != b.usb_stem[i]) {
        return false;
      }
    }
    return true;
  }
};

namespace settings {

inline constexpr char kType[5] = "O3SK";
inline constexpr std::size_t kMaxBytes = 2048;       // the largest file written or read
inline constexpr std::uint16_t kSetsVersion = 0x0100;  // major 1, minor 0

// The keys of SETS. Append, never insert or reuse.
enum Key : std::uint16_t {
  kTheme = 1,          // ui::ThemeId, 0 .. kThemeCount-1
  kMidiChannel = 2,    // Config::channel, 0 (omni) .. 16
  kMidiOutChannel = 3,  // Config::out_channel, 1 .. 16
  kMidiOutPorts = 4,   // Config::out_ports, a mask of core::kMidiAllPorts
  kMidiClockOut = 5,   // Config::clock_out, 0 or 1
  kMidiFollow = 6,     // Config::follow, 0 or 1
  kMidiFollowPort = 7,  // Config::follow_port, a hal::midi::Port
  kMidiTransportIn = 8,   // Config::transport_in, 0 or 1
  kMidiTransportOut = 9,  // Config::transport_out, 0 or 1
  kInputSource = 10,   // hal::capture::Source, 0 .. kSourceCount-1 (M2c slice AI)
  kInputGain = 11,     // the source's gain, 0 .. 100 per cent
  kInputMonitor = 12,  // the monitor's level, 0 (off) .. 100 per cent
  kRadioRegion = 13,   // hal::capture::Region, 0 .. kRegionCount-1
  kRadioFreq = 14,     // the station, 10 kHz units: the region's band, 7600 .. 10800
  kUsbAudio = 15,      // 1: the AUDIO set, 0: STANDARD (M2c slice U)
  kUsbProfile = 16,    // hal::usb_audio::Profile, 0 .. kProfileCount-1 (P2, P8, P10, P11)
  kUsbDuplex = 17,     // 1: the return stream is part of the set
  kUsbReturnLevel = 18,  // the return's level, 0 .. 100 per cent
  kUsbMapSet = 19,     // 1: the edited slots below apply (else the profile's factory slots)
  kUsbStem1 = 20,      // slot 1 of the profile as a mask of the 16 stem sources, 0 .. 65535; kUsbStem1 + n is slot n + 1, up to kUsbStem11
  kUsbStem11 = 30,
};
inline constexpr unsigned kKeyCount = 30;  // the keys of this version

enum class Result : std::uint8_t {
  Ok,
  Truncated,    // the container is cut short
  BadMagic,     // not an O3SK file
  BadCrc,
  Malformed,    // the container or a SETS chunk has the wrong shape (a count that does not fit its length, a second SETS)
  NewerMajor,   // SETS of a newer major version: refused, nothing applied
};
const char* result_name(Result r) noexcept;

// What a read found beyond the values.
struct Report {
  std::uint16_t applied = 0;         // entries that set a value
  std::uint16_t skipped_keys = 0;    // entries whose key this version does not know
  std::uint16_t skipped_values = 0;  // entries of a known key whose value is out of range (the setting keeps its value)
  std::uint16_t unknown_chunks = 0;  // chunks other than SETS and CRC
  bool newer_minor = false;          // SETS of a newer minor: read as far as this version knows it
};

// The file for `v` into `out` (at most `cap`, which should be kMaxBytes): its size, or 0 when it does not fit.
std::size_t write(const KitValues& v, std::uint8_t* out, std::size_t cap) noexcept;

// Reads `data` and applies it over `v`: a setting the file does not give keeps the value it has (the app's default), one it gives wins. On any result but Ok `v` is untouched.
Result read(const std::uint8_t* data, std::size_t n, KitValues& v, Report& report) noexcept;

}  // namespace settings
}  // namespace o3sdk::app
