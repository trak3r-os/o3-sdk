// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/identity.h>
#include <o3sdk/usb/usb_types.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::usb {

// What the device presents to the host. The descriptors are a pure function of this struct, built into a
// caller-supplied buffer at boot (one-time, outside any ISR): there is no per-profile table in flash.
//
//   mode     STANDARD: CDC `diag` + MIDI.   AUDIO: CDC + MIDI + UAC2 (stems IN, USB RETURN OUT, feedback).   DISK: STANDARD + Mass Storage (EP 4 OUT, EP 5 IN).
//   speed    Full is the only speed the MK66 USB0 runs at; High only changes packet sizes and intervals.
//   profile  channel layout of the IN stream (AUDIO only; ignored in STANDARD).
//   usb_return  AUDIO only: true (the default, the layout before OS-USBRET) carries the stereo OUT stream (USB RETURN) with its feedback
//            endpoint; false drops the OUT AudioStreaming interface, the return terminals and the feedback endpoint, so the function is
//            capture only and the host cannot open a playback stream that the bus may not have room for (ADR 0012 amendment 5).
//   serial   iSerialNumber, ASCII, 1..kMaxSerialChars characters. Null or empty uses the placeholder.
struct Config {
  Mode mode = Mode::Standard;
  Speed speed = Speed::Full;
  Profile profile = Profile::P10;
  bool usb_return = true;
  const char* serial = kSerialPlaceholder;
};

inline constexpr std::size_t kDeviceDescriptorSize = 18;
// Big enough for every mode, speed and profile (AUDIO is the largest; a test asserts the bound).
inline constexpr std::size_t kMaxConfigDescriptorSize = 400;
inline constexpr std::size_t kMaxStringDescriptorSize = 2u + 2u * 31u;

// String descriptor indices. 0 is the LANGID list (US English). The channel names follow each other:
// the IN stream's names start at kStrChannelNames, the OUT stream's two names right after them, because a
// UAC2 cluster descriptor names its channels with consecutive string indices (iChannelNames = first).
inline constexpr std::uint8_t kStrLangId = 0;
inline constexpr std::uint8_t kStrManufacturer = 1;
inline constexpr std::uint8_t kStrProduct = 2;
inline constexpr std::uint8_t kStrSerial = 3;
inline constexpr std::uint8_t kStrCdc = 4;
inline constexpr std::uint8_t kStrMidi = 5;
inline constexpr std::uint8_t kStrAudio = 6;  // AUDIO mode only from here on
inline constexpr std::uint8_t kStrDisk = 6;   // DISK mode: the same slot (the two modes never coexist); there it is the last string
inline constexpr std::uint8_t kStrClock = 7;
inline constexpr std::uint8_t kStrChannelNames = 8;

// Each builder returns the number of bytes written, or 0 when `cap` is too small (nothing useful written).
std::size_t build_device_descriptor(const Config& cfg, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t build_config_descriptor(const Config& cfg, std::uint8_t* out, std::size_t cap) noexcept;

// bcdDevice of a mode (identity.h): the host re-reads its descriptors when it changes.
constexpr std::uint16_t device_bcd(Mode m) noexcept {
  return m == Mode::Audio ? kDeviceBcdAudio : (m == Mode::Disk ? kDeviceBcdDisk : kDeviceBcdStandard);
}

// Number of valid string indices for this configuration (0 .. string_count()-1). STANDARD 6; DISK 7; AUDIO
// 8 + channels + 2 (the two return names only with usb_return).
std::uint32_t string_count(const Config& cfg) noexcept;
// String descriptor `index`; 0 bytes when the index is out of range. Index 0 is the LANGID list.
std::size_t build_string_descriptor(const Config& cfg, std::uint8_t index, std::uint8_t* out,
                                    std::size_t cap) noexcept;

// Name of channel `index` of the IN stream of a profile ("S1".."S9", "Master L", "S3 R" ...), copied NUL-terminated
// into `out` (at least 9 bytes). Returns its length, 0 when index >= profile_channels().
std::size_t channel_name(Profile profile, std::uint32_t index, char* out) noexcept;

// wMaxPacketSize the builder writes for the IN stream (stems) and the OUT stream (return).
constexpr std::uint16_t in_stream_max_packet(Profile p, Speed s) noexcept {
  return static_cast<std::uint16_t>(max_packet_bytes(profile_channels(p), s));
}
constexpr std::uint16_t out_stream_max_packet(Speed s) noexcept {
  return static_cast<std::uint16_t>(max_packet_bytes(kReturnChannels, s));
}

}  // namespace o3sdk::usb
