// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/usb_types.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::usb {

// Control-transfer SETUP packet (USB 2.0 9.3). The class-request handler works on this struct and never sees
// a bus, so the whole control path can be unit tested.
struct SetupPacket {
  std::uint8_t request_type = 0;  // bmRequestType: bit 7 direction (1 = device to host), bits 6..5 type, bits 4..0 recipient
  std::uint8_t request = 0;       // bRequest
  std::uint16_t value = 0;        // wValue
  std::uint16_t index = 0;        // wIndex
  std::uint16_t length = 0;       // wLength

  constexpr bool device_to_host() const noexcept { return (request_type & 0x80u) != 0; }
  constexpr std::uint8_t type() const noexcept { return static_cast<std::uint8_t>((request_type >> 5) & 0x03u); }
  constexpr std::uint8_t recipient() const noexcept { return static_cast<std::uint8_t>(request_type & 0x1Fu); }
};

// Decodes the 8 little-endian bytes of a SETUP packet.
constexpr SetupPacket parse_setup(const std::uint8_t* raw) noexcept {
  SetupPacket s;
  s.request_type = raw[0];
  s.request = raw[1];
  s.value = static_cast<std::uint16_t>(raw[2] | (raw[3] << 8));
  s.index = static_cast<std::uint16_t>(raw[4] | (raw[5] << 8));
  s.length = static_cast<std::uint16_t>(raw[6] | (raw[7] << 8));
  return s;
}

enum class ControlStatus : std::uint8_t {
  Stall,  // request not supported: the driver stalls the control endpoint
  Data,   // device-to-host reply in ControlReply::data (already truncated to wLength)
};

// A reply is at most 14 bytes (GET RANGE with one sub-range).
struct ControlReply {
  ControlStatus status = ControlStatus::Stall;
  std::uint16_t length = 0;
  std::uint8_t data[16] = {};
};

// UAC2 class-specific requests addressed to the AudioControl interface (bmRequestType 0xA1 for GET,
// 0x21 for SET; wIndex = entity << 8 | interface).
//   Clock Source (kEntClock), sampling frequency control (selector 1):
//     GET CUR   -> 4 bytes, 44,100 (little-endian dCUR)
//     GET RANGE -> 14 bytes: wNumSubRanges = 1, then dMIN = dMAX = 44,100, dRES = 0 (one discrete rate)
//     SET CUR   -> stall: the control is read-only (bmControls 0x01), the host never changes the clock
//   Everything else (other entities, selectors, clock validity, feature units, MEM requests, wrong direction
//   or interface) -> Stall. There is no Feature Unit in the descriptors on purpose: the IN stream is a raw
//   digital stem tap and the OUT stream is an engine source with its own level in the mixer, so a host
//   mute/volume would have no defined meaning.
// A reply is cut to wLength, as the host asked.
ControlReply handle_audio_control_request(const SetupPacket& setup) noexcept;

// AudioStreaming alternate settings (alt 0 = idle with no bandwidth, alt 1 = streaming). The state the USB
// driver keeps for SET_INTERFACE / GET_INTERFACE on the two AS interfaces; the stem tap and the return
// ring are enabled only while the matching alt is 1 (ADR 0012 Decision 3: an idle device pays nothing).
struct AudioStreamState {
  std::uint8_t in_alt = 0;   // interface kIfAudioIn
  std::uint8_t out_alt = 0;  // interface kIfAudioOut

  constexpr bool in_active() const noexcept { return in_alt == 1u; }
  constexpr bool out_active() const noexcept { return out_alt == 1u; }
};

// SET_INTERFACE for an AS interface: false (stall) for an interface that is not AudioStreaming or an alt
// above 1; state untouched in that case. Setting the alt that is already active is accepted (hosts do it to
// flush a stream); the caller restarts the stream when the result is true and the alt is 1.
bool set_audio_alt(AudioStreamState& state, std::uint8_t interface, std::uint8_t alt) noexcept;
// GET_INTERFACE: false for an interface that is not AudioStreaming.
bool get_audio_alt(const AudioStreamState& state, std::uint8_t interface, std::uint8_t& alt) noexcept;

}  // namespace o3sdk::usb
