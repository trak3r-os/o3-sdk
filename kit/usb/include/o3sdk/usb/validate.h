// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/descriptors.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::usb {

// Structural validation of a built descriptor set. It parses the bytes, it does not trust the builder:
//   device:  18 bytes, bcdUSB 2.00, IAD device class, EP0 64, VID/PID and the bcdDevice of the mode, one configuration.
//   config:  every bLength fits, wTotalLength equals the buffer, interface numbers 0..N-1 in order, alternate
//            settings consecutive, bNumInterfaces matches, every endpoint belongs to an interface and is
//            declared (bNumEndpoints), endpoint numbers 1..15 and unique, at most 15 numbers, interface
//            association descriptors tile all interfaces in order, class-specific wTotalLengths (UAC2 AC,
//            MIDI MS) match, entity IDs unique and referenced entities exist, string indices exist, AudioStreaming
//            alt 0 has no endpoints (no bandwidth), isochronous wMaxPacketSize >= channels x 2 B x frames_hi and
//            <= the speed's limit (1,023 FS), feedback endpoint 3 B (FS) / 4 B (HS) and present exactly when the
//            data endpoint is OUT, and the per-mode endpoint counts (STANDARD 5 endpoints on 3 numbers,
//            AUDIO 8 on 5; AUDIO without the return stream, Config::usb_return false, 6 on 4 with 6 interfaces and
//            3 entities instead of 5; DISK 7 on 5 with 5 interfaces: the STANDARD set plus one Mass Storage interface
//            (class 08h/06h/50h, outside any IAD) with exactly the bulk OUT 0x04 and bulk IN 0x85 of the bulk packet size), with the AudioStreaming channel counts matching the profile, and the AudioControl
//            layout (AUDIO: the UAC2 interface comes before the MIDI one, see usb_types.h layout).
enum class ValidateError : std::uint8_t {
  Ok = 0,
  Truncated,           // a descriptor runs past the buffer, or the buffer is shorter than a header
  BadLength,           // bLength is zero, too small, or wrong for its type
  BadType,             // unknown descriptor type
  BadHeader,           // device or configuration header field wrong
  BadTotalLength,      // wTotalLength (configuration, UAC2 AC or MIDI MS) does not match
  BadNumInterfaces,    // bNumInterfaces does not match the interfaces found
  InterfaceNumbering,  // interface numbers not 0..N-1 in order
  AltNumbering,        // alternate settings not consecutive
  EndpointOutsideInterface,
  EndpointNumber,      // number 0 or above 15
  DuplicateEndpoint,
  TooManyEndpointNumbers,
  EndpointCount,       // bNumEndpoints differs from the endpoint descriptors that follow
  Alt0HasBandwidth,    // AudioStreaming alt 0 declares endpoints
  BadMaxPacket,
  BadFeedback,
  IadMismatch,         // IADs do not tile the interfaces, or do not precede their first interface
  BadClassSpecific,    // class-specific descriptor in the wrong place, wrong length or wrong field
  BadEntity,           // duplicate or dangling entity / jack ID, channel-count mismatch
  BadString,           // string index out of range
  ModeCounts,          // endpoint or interface counts do not match the mode
};

struct ValidateResult {
  ValidateError error = ValidateError::Ok;
  std::uint16_t offset = 0;  // byte offset of the offending descriptor
  constexpr bool ok() const noexcept { return error == ValidateError::Ok; }
};

ValidateResult validate_device_descriptor(const std::uint8_t* d, std::size_t n, const Config& cfg) noexcept;
ValidateResult validate_config_descriptor(const std::uint8_t* d, std::size_t n, const Config& cfg) noexcept;

}  // namespace o3sdk::usb
